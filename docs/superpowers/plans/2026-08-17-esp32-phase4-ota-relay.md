# Phase 4 實作計畫：ESP-NOW 轉送 OTA

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 讓沒有 WiFi 的 slave 能經由 master 完成韌體更新 ——
App 送 `update_slave:{...}` → master 用 HTTPS 把 `.bin` 下載進**自己的閒置 OTA 分區暫存**
→ 分包經 ESP-NOW 轉送給指定 slave → slave 寫入自己的閒置 OTA 分區 → MD5 校驗通過才切換開機分區 → 重啟
→ master 等它回線並比對版本，最後把成敗回報到 MQTT。

**全程的硬性約束只有一條：任何時刻 ESP-NOW 心跳的空窗都不得逼近 30 秒。**
OTA 要跑 30~90 秒，這是整個專案第一次有「持續一分鐘以上的忙碌狀態」，
所有設計決定都是為了這條約束服務。

**Architecture:** 三層，彼此以「暫存分區」與「單一狀態機」解耦：

1. **下載層（master，HTTPS）** —— 非阻塞串流：每次 `loop()` 最多讀 4 KB 寫進暫存分區。
   唯一的阻塞點（DNS、TLS 握手）被拆成各自獨立的 `loop()` 步驟並前後補發心跳，
   單次阻塞窗口壓在 15 秒以內。
2. **轉送層（master → slave，ESP-NOW）** —— 固定視窗 16 包的 block-ACK：
   送完一個區塊 → 查詢 → slave 用 16-bit bitmap 回報缺哪幾包 → 只補送缺的。
   送出動作每次 `loop()` 最多 2 包，靠 `ESP_ERR_ESPNOW_NO_MEM` 自然背壓。
3. **寫入層（slave）** —— 區塊先進 3840 bytes 的 RAM 緩衝，湊齊整個區塊才順序寫入
   `Update`。因此「亂序抵達」與「選擇性重傳」對 flash 寫入完全透明。

**Tech Stack:** Arduino ESP32 core 3.3.7、PubSubClient、ArduinoJson 7.4.3、Preferences (NVS)、
EEPROM (slave)、esp_now.h、esp_wifi.h、**新增：`HTTPClient.h`／`WiFiClientSecure.h`／`MD5Builder.h`／
`esp_ota_ops.h`／`esp_partition.h`（master）、`Update.h`（slave）**

**Spec:** `docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md`（「難點 3」與「Phase 4」章節）

**前置（不可略過）：**

- 分支 `feature/esp32-master-slave`
- **Phase 2b 的 Task 4、5、7 必須先完成。** 本計畫的 Task 5 把 `update_slave:` 分支加進
  Phase 2b Task 4 才拆出來的 `handleMasterCommand()`。動工時若在 `ho_master1.ino` 裡
  `grep -n "void handleMasterCommand"` 找不到這個函式，**停下來回報，不要自行改寫
  `mqttCallback()` 湊合**（Phase 2a 因「Task 引用尚未存在的函式」踩過一次）。
- Phase 2b Task 6（Long Range）已被裁定移到 Phase 5，本計畫**不依賴**它。

**執行紀錄（全部必讀）：**
- `.superpowers/sdd/2026-08-14-esp32-master-slave-phase1/progress.md`
- `.superpowers/sdd/2026-08-15-esp32-master-phase2a/progress.md`
- `.superpowers/sdd/2026-08-16-esp32-master-phase2b/progress.md`

---

## Global Constraints

### 安全鐵則（違反即為 Critical）

- **繼電器安全鐵則**：`initRelayPins()` 必須是 `setup()` 第一行，早於 `Serial.begin()`。
- **ESP-NOW 不可中斷鐵則**：master 心跳每 1 秒（WiFi 關聯期間 200ms），slave 超過 **30 秒**
  沒收到就**強制關閉繼電器＝籠子被打開**。本階段的所有新程式碼都必須遵守：
  - 所有等待走 `espNowDelay()`，**不得用裸 `delay()`**
  - 任何「跑很久」的工作一律拆成 `loop()` 驅動的狀態機，**每次 `loop()` 只推進一小步**
  - 無法拆解的阻塞呼叫（DNS、TLS 握手、分區抹除）**單次必須 < 20 秒**，
    且**進入前後各補一次 `sendHeartbeatBurst()`**
- **ESP-NOW callback 不得發 MQTT**：`onEspNowRecv()` 跑在 WiFi task。只能設旗標，
  實際發布交給 `loop()`。本階段新增的 OTA_ACK 處理同樣受此約束。
- **master 絕不呼叫 `esp_ota_set_boot_partition()`**：master 只把 slave 的韌體當**資料**
  寫進自己的閒置 OTA 分區。用 `Update` 類別會在 `Update.end(true)` 裡切換開機分區
  ——那會讓 master 下次開機直接跳進一份 **slave 的韌體**而變磚。
  **master 一律用 `esp_partition_erase_range()` / `esp_partition_write()` / `esp_partition_read()` 原生 API，
  完全不碰 `otadata`。**

### 已實測的硬性輸入（直接用，不要重估）

**flash 預算（2026-08-16 探針編譯實測）**

| 情境 | WROOM | 對 app0 (2,031,616) |
|---|---|---|
| 目前（Phase 2b Task 3 後） | 1,690,003 | 83.18% |
| 加 `Update.h` + `HTTPClient.h` | +126,292 | — |
| 再加 `WiFiClientSecure.h`（HTTPS） | 幾乎不變（−492） | 約 89.05%，**剩 217 KB** |

**兩個已推翻的假設，不得再沿用：**

1. HTTPS 實際成本約 126 KB（原估 170~250 KB），**WROOM 放得下**。
2. **走 HTTP 幾乎省不到 flash**（只差 492 bytes）。`ho_master1.ino` 無條件 include BLE 標頭，
   **BLE 的安全機制已把 mbedTLS 整包連結進去**，TLS 是沉沒成本。真正的大頭是
   `Update.h` + `HTTPClient.h`。**所以本階段不為了省 flash 而走 HTTP，也不評估 NimBLE、
   不放棄 WROOM。** Phase 2a progress.md 記的那三條備案實測後全部作廢。

**本階段的 flash 紅線**：WROOM 任一 Task 結束時若超過 **1,930,035 bytes（95%）**，
在 report 標紅並停下來回報，不要自行砍功能。

**ESP-NOW 封包**：單包上限 250 bytes，`HoPacketHeader` 佔 7，payload 上限 **243**。

**分區**：`ho_master1/partitions.csv` 與 `ho_slave1/partitions.csv` **位元組級相同**，
app0／app1 各 `0x1F0000 = 2,031,616` bytes。master 的閒置槽足以暫存任何合法的 slave 韌體。

**slave 目前用量**：flash 978,273（48.15%）、RAM 10%。加 `Update.h` 與 3840 bytes 區塊緩衝後仍充裕。

### 專案慣例

- 結果變數用 `res`，不用 `result`
- 註釋、序列埠輸出、文件、commit 訊息一律**繁體中文**
- **不得修改** `ho_relay1/`、`ho_relay2/`、`ho_relay3/`、`CLAUDE.md`、`flash.ps1`
  （`ho_relay2/ho_relay2.ino` 是本階段的參考來源，**唯讀**）
- **雙板支援**：`ho_master1` 同時支援 WROOM 與 C3，`CONFIG_IDF_TARGET_ESP32C3` / `_ESP32`
  條件編譯結構不得破壞
- 驗證走 `flash.ps1` + `arduino-cli` 1.3.1，不新增外部工具鏈。
  子代理若遇 `flash.ps1` 在編譯成功時仍中止，照 `.claude/rules/flash-ps1-subagent-stderr.md`
  改用它內部的 `arduino-cli compile` 指令，並在報告寫明用了哪一種
- arduino-cli 對 custom 分區印的百分比是拿整顆 4MB 當分母，**一律自行換算成對 app0 的百分比**

### 前階段留下、本階段必須沿用的認知更正

1. `WiFi.disconnect(bool wifioff, bool eraseap)` 第一個參數是 **wifioff**。
2. `WiFi.setAutoReconnect(true)` 在 core 3.3.7 是**死碼**。
3. `setSocketTimeout(3)` 只管「TCP 已連線後等 CONNACK」，不管 TCP connect，**更不管 DNS**。
4. **`NetworkClient::connect(host, port)` 的 DNS 那一段沒有任何 timeout 參數**，
   由 lwIP 的指數退避決定，最壞約 **15 秒**。本階段的下載流程整個是繞著這個事實設計的。
5. ArduinoJson 7.4.3 的 `StaticJsonDocument<N>` 的 N **完全被忽略**；容量控制只能靠
   `measureJson()` 實測值。所有 MQTT JSON 發布一律走 `publishJsonDoc()`。

---

## 檔案結構

```
libraries/HoEspNow/src/HoEspNowProtocol.h    # Task 1：CRC 涵蓋標頭、版本 2、OTA 四種 payload
libraries/HoEspNow/src/HoEspNowProtocol.cpp  # Task 1
ho_espnow_test/ho_espnow_test.ino            # Task 1：CRC 測試改寫 + 新增標頭竄改測試
ho_slave1/ho_slave1.ino                      # Task 2（約 +230 行）
ho_master1/ho_master1.ino                    # Task 3、4、5（約 +560 行）
ho_master1/readme.md                          # Task 6
ho_slave1/readme.md                           # Task 6（若不存在則新增）
docs/phase4-regression-checklist.md           # Task 6 新增
```

**不拆檔**，理由同 Phase 2a／2b：Arduino sketch 目錄下的 `.ino` 會被自動合併，拆檔無隔離效果。

---

## 本計畫的六個設計決定（先讀完再開工）

實作時若發現任一決定站不住腳，**停下來回報**，不要自行改成別的做法。

---

### 決定 1：OTA 期間的心跳 —— 三道各自獨立的保障，不動 30 秒門檻

這是本階段能不能安全的關鍵，逐一回答三個提問。

#### (a) 目標 slave 會不會因為忙於接收而錯過心跳？

**會，而且後果比「繼電器被關掉」更糟。** 現行 `ho_slave1.ino` 的 `lastHeartbeatTime`
**只在 `HO_PKT_HEARTBEAT` 分支被更新**（`onMasterFound()` 裡那行）。收 OTA_DATA 不會更新它。
所以只要 OTA 期間有 30 秒沒收到「廣播心跳」，slave 就會：

1. `startChannelScan()` → **強制關閉繼電器**（籠子被打開）
2. **而且開始每 1200ms 換一個 channel** —— 它會從 master 的頻道上跑掉，
   **OTA 傳輸當場斷掉**，前面傳的幾十秒全部作廢

而 OTA 期間 ESP-NOW 是滿載的：廣播心跳沒有 ACK、沒有重傳，在密集單播流量下丟包率明顯上升。
「master 有發」不等於「slave 收得到」——這是 Phase 2a reviewer 早就點出的區別。

**解法：slave 端把「liveness」的定義從『收到廣播心跳』放寬成『收到任何一封通過驗證、
來自已配對 master 的封包』。**

```cpp
// onEspNowRecv() 內，通過「只接受已配對 master」那道 guard 之後：
lastHeartbeatTime = millis();
```

這是**收緊而不是放寬**安全性，理由有三：

- **單播比廣播是更強的證據**。收到一封來自 master 的單播，同時證明了「master 活著」
  **和**「我跟它在同一個 channel 上」；廣播心跳只證明前者。
- 偽造成本完全相同：兩者都要算得出含共享密鑰的 CRC，本來就是同一道門檻。
  Task 1 把 CRC 擴大到涵蓋標頭之後，門檻只會更高。
- 這條路徑**不會**讓一台真的失聯的 slave 賴著不關繼電器 —— 沒有封包就是沒有封包，
  30 秒計時照常走完。

**改動只有一行，但它是本階段最重要的一行。** 沒有它，OTA 幾乎必定在中途自我破壞。

#### (b) 其他 19 台沒參與 OTA 的 slave，心跳誰來發？

**照常由 `maintainEspNow()` 發，因為 OTA 的每一個階段都是 `loop()` 驅動的狀態機。**

本階段新增的所有長時工作都遵守同一個形狀：

```cpp
void loop() {
  ...
  maintainEspNow();      // 心跳照發（既有）
  ...
  updateOtaSession(now); // 本階段新增：每次只推進一小步，絕不在裡面等
}
```

具體的「一小步」上限：

| 階段 | 每次 `loop()` 的工作量 | 最壞耗時 |
|---|---|---|
| 下載 | 從 TLS stream 讀 ≤ 4096 bytes 並寫暫存分區 | 數十 ms |
| 轉送 | 送 ≤ 2 包 ESP-NOW（每包 250 bytes） | < 5 ms |
| 等 ACK | 純比對計時器 | 微秒級 |

**所以其他 19 台在 OTA 期間的心跳節奏與平常完全一樣。**
`sendCmdToAll()`（`ALL:OFF`）在 OTA 期間**照常可用且不被阻擋** —— 緊急關門的優先權高於 OTA。

#### (c) master 下載 HTTPS 韌體時心跳怎麼辦？

`ho_relay2` 的 `startFirmwareUpdate()` 是一個**從頭阻塞到尾**的函式，最壞
（3 次重試 × 5 分鐘下載逾時 + 指數退避）可達 **16 分鐘**。**整段不能移植，只能重寫。**

重寫後剩下的阻塞點只有三個，各自被拆成獨立的 `loop()` 步驟：

| 阻塞點 | 為什麼不能拆 | 最壞耗時 | 對策 |
|---|---|---|---|
| DNS 解析 | lwIP 無 timeout 參數（Phase 2a 已查證） | **約 15 秒** | 獨立成 `OTA_RESOLVING` 階段，只做 `WiFi.hostByName()` 一件事 |
| TCP connect + TLS 握手 + 收回應標頭 | HTTPClient 的 `GET()` 是同步的 | `setConnectTimeout(6000)` + `setTimeout(6000)` ≈ **12 秒** | 獨立成 `OTA_DOWNLOADING` 的第一次進入；DNS 已在上一階段預熱進 lwIP 快取，這裡不會再撞一次 15 秒 |
| 暫存分區抹除 | `esp_partition_erase_range()` 是同步的 | 1 MB 約 **1~3 秒** | 只做這一件事的獨立步驟 |

**每個阻塞步驟的前後都呼叫 `sendHeartbeatBurst()`**（連發 4 次、間隔 200ms）。
所以最壞情境是「burst → 15 秒空窗 → burst」，**距離 30 秒門檻有一倍餘裕**。

**為什麼要多一個 `OTA_RESOLVING` 階段去預熱 DNS**：不拆的話，
`http.begin()` + `GET()` 這一個呼叫裡會**同時**吃到 DNS 的 15 秒和 TLS 的 12 秒，
單次阻塞 27 秒，**離 30 秒只剩 3 秒**。先用 `WiFi.hostByName()` 把結果灌進 lwIP 的 DNS 快取，
之後 `GET()` 走快取立即返回，就把一個 27 秒窗口拆成 15 + 12 兩個。
**這一步是純安全性投資，不可為了少寫幾行而省略。**

**重試也必須非阻塞。** `ho_relay2` 用 `delay(baseDelay * (1 << retryCount))`
（最長 20 秒裸 delay）——本階段改成狀態機裡的 `otaRetryAt` 時間戳，
重試間隔固定 **10 秒**（不做指數退避：退避的價值是保護伺服器，
而我們最多重試 3 次、間隔 10 秒，對任何伺服器都無壓力，卻換到「延遲上界可預測」）。

#### 明確拒絕的替代方案：在心跳裡加「寬限期」欄位

曾考慮讓 master 在心跳 payload 加 `otaGraceSec`，OTA 期間通知 slave 把門檻拉長到 60 秒。
**不採用**，理由：

- 它**削弱**核心不變量：master 若在寬限期內真的死掉，繼電器會多維持一段時間不進入已知安全狀態
- 它把安全性押在「一個可被重放的欄位」上
- **一旦每個阻塞步驟都 ≤ 15 秒，它就沒有存在必要** —— 用不到的安全機制只會腐化

#### 附帶必須處理的一項：OTA 一定會讓目標 slave 的繼電器斷一次

slave 校驗通過後會 `ESP.restart()`。重開機期間繼電器必然回到 LOW，
**且 C3 板有「開機瞬間繼電器短暫通電」的硬體限制**（`CLAUDE.md` 已載明，韌體無法根治）。
也就是說：**對一台正在把籠門保持在關閉狀態的 slave 做 OTA，等於在遠端把那扇門打開。**

**對策（Task 5 實作）**：`update_slave` 指令在目標 slave 的 `relay == 1` 時**預設拒絕**，
除非指令 JSON 帶 `"force": true`。序列埠與 MQTT 都要說清楚拒絕原因。

---

### 決定 2：分包協定 —— 固定視窗 16 包的 block-ACK + bitmap 選擇性重傳

#### 2.1 前置：CRC 必須先擴大到涵蓋標頭（Phase 1 就記下的欠款）

Phase 1 最終審查的殘留第 5 項原文：

> **CRC 只涵蓋 payload 不含標頭的 type/seq，Phase 4 做 OTA 前必須改成涵蓋標頭**

現況 `hoPackPacket()` 是 `header.crc = hoPayloadCrc(payload, payloadLen)`。
在 OTA 之前這只是理論缺陷，到 OTA 就變成兩個實際危害：

1. **`type` 欄位的位元翻轉不會被偵測。** 一封 OTA_DATA 的 `type` 從 `0x21` 翻成 `0x10`（`HO_PKT_CMD`）
   之後，CRC **仍然正確**，slave 會把韌體資料的前 3 bytes 當成 `HoCmdPayload` 解讀，
   `cmd` 欄位剛好是 0/1/2 就會**實際動作繼電器**。這是動物管制設備上的誤觸發。
2. **`seq` 翻轉不會被偵測**（雖然本協定把 OTA 的塊號放在 payload 而非 `seq`，
   但去重邏輯仍依賴它）。

**Task 1 一併修掉**：CRC 改為涵蓋「標頭前 6 bytes（magic/version/type/seq）＋ payload ＋ 共享密鑰」。

**這是 flag-day 破壞性改動**：舊韌體與新韌體互相收不到（新的 CRC 算法不同，
且 `HO_ESPNOW_VERSION` 由 1 升到 2 會被對方的版本檢查擋掉）。
**master 與所有 slave 必須一起重燒。** 可接受，因為 Phase 2a progress.md 已裁定
「在補上現場恢復手段之前 master 不得佈到現場」——目前沒有任何現場部署。
**Task 6 的回歸清單第一條就是「兩端都燒了新韌體」，且 readme 必須寫明混版的後果
（slave 收不到心跳 → 30 秒 → 強制關閉繼電器）。**

#### 2.2 四種封包的 payload

```c
#define HO_OTA_CHUNK_SIZE   240   // 243（payload 上限）− 3（HoOtaDataPayload 的前置欄位）
#define HO_OTA_WINDOW       16    // 一個區塊 16 包 = 3840 bytes；bitmap 剛好一個 uint16_t

// 0x20 OTA_BEGIN  master → slave
struct __attribute__((packed)) HoOtaBeginPayload {
  uint32_t totalSize;    // 韌體總位元組數
  uint16_t totalChunks;  // ceil(totalSize / HO_OTA_CHUNK_SIZE)；uint16 上限 65535 包 ≈ 15 MB，夠用
  uint8_t  md5[16];      // 整份韌體的 MD5，16 bytes 原始值（不是 32 字元 hex，省 16 bytes）
  uint8_t  verMajor;
  uint8_t  verMinor;
  uint8_t  verPatch;
  uint8_t  sessionId;    // 1~255（0 保留給「無工作階段」），用來丟棄上一次失敗殘留的封包
};  // 4+2+16+3+1 = 26 bytes

// 0x21 OTA_DATA  master → slave
struct __attribute__((packed)) HoOtaDataPayload {
  uint8_t  sessionId;
  uint16_t chunkIndex;   // 0-based。刻意「不」重用標頭的 seq —— seq 是 espNowSendTo()
                         // 自動遞增的全域傳送計數器，挪作塊號會同時破壞去重語義並
                         // 逼所有呼叫端改簽名。多花 2 bytes 換一個乾淨的邊界。
  // 之後緊接著 1~HO_OTA_CHUNK_SIZE bytes 的韌體資料（長度由封包總長推得）
};  // 3 bytes + 資料

// 0x22 OTA_END  master → slave
struct __attribute__((packed)) HoOtaEndPayload {
  uint8_t  sessionId;
  uint8_t  abort;      // 1 = 中止，slave 應 Update.abort() 並丟棄
  uint32_t totalSize;  // 再帶一次，讓 slave 交叉核對自己實際寫入的位元組數
};  // 6 bytes

// 0x23 OTA_ACK  雙向。方向本身就是語義，不需要額外的 request 欄位：
//   到達 slave 的 OTA_ACK ＝ master 在查詢這個區塊收到了哪幾包
//   到達 master 的 OTA_ACK ＝ slave 的回報
struct __attribute__((packed)) HoOtaAckPayload {
  uint8_t  sessionId;
  uint16_t blockBase;  // 本區塊第一包的 chunkIndex
  uint16_t mask;       // slave→master：bit i = 已收到 blockBase+i；master→slave 查詢時填 0
  uint8_t  status;     // HoOtaStatus，只有 slave→master 有意義
};  // 6 bytes
```

`OTA_DATA` 的封包總長 = 7（標頭）+ 3 + 240 = **250**，剛好貼滿 ESP-NOW 上限。

#### 2.3 ACK 策略：為什麼是「16 包區塊 + bitmap」而不是每包 ACK、也不是滑動視窗

| 方案 | 優點 | 為什麼不選 |
|---|---|---|
| **每包 ACK**（stop-and-wait） | 最簡單 | 4400 次來回；每次來回含兩次無線傳輸與一次 slave 處理，實測 ESP-NOW 單向約 2~4ms → **20~35 秒純協定開銷**，且完全無法利用 ESP-NOW 的批次送出能力 |
| **真滑動視窗** | 吞吐最高 | 需要 per-packet 逾時計時器、序號迴繞處理、視窗前緣／後緣兩套指標。在**單一目標、單一工作階段、一次只跑一台**的場景下，這些複雜度買不到任何東西 |
| **區塊 + bitmap（本案）** | 一次來回搬 3840 bytes；重傳精確到單包；狀態只有 `(blockBase, mask, retry)` 三個變數 | — |

**為什麼視窗是 16 不是規格草稿寫的 32**：

- 16 讓 bitmap 剛好是一個 `uint16_t`，`OTA_ACK` payload 停在 6 bytes
- slave 端的區塊緩衝是 `16 × 240 = 3840` bytes；32 就要 7680 bytes。
  slave 的 RAM 雖然夠，但 3840 已經能讓吞吐吃滿 ESP-NOW 的實際速率，
  加大只是多佔記憶體
- 區塊越大，一次重傳失敗要重來的量越大

**吞吐估算**：1 MB 韌體 = 4370 包 = **274 個區塊**。
每個區塊 = 16 次送出（每次 `loop()` 送 2 包 → 8 個 `loop()` 週期）+ 1 次查詢 + 1 次回報。
保守估每區塊 80~150ms → **22~41 秒**。與規格估的 30~90 秒相符。

#### 2.4 逾時與重傳的具體數字

```cpp
const unsigned long OTA_ACK_TIMEOUT_MS   = 400;   // 送完查詢後等 ACK
const int           OTA_POLL_MAX         = 3;     // 同一個區塊最多重發 3 次查詢
const int           OTA_BLOCK_MAX_RETRY  = 8;     // 同一個區塊最多重送 8 輪
const unsigned long OTA_SESSION_MAX_MS   = 300000; // 整個工作階段 5 分鐘上限
const unsigned long OTA_SLAVE_IDLE_MS    = 30000;  // slave 端：這麼久沒收到 OTA 封包就自行中止
```

- 400ms 的來由：ESP-NOW 單向典型 2~4ms，最壞（重傳＋通道忙）約 50ms。
  400ms 給了近 10 倍餘裕，同時讓「整塊全丟」的偵測不會拖太久。
- **查詢與重送分成兩層**：先重發 3 次查詢（成本 1 包），確認不是「ACK 回程丟了」；
  仍無回應才整塊重送（成本 16 包）。這在弱訊號下能省下大量重複資料。
- 8 輪 × 16 包重送仍失敗 → 判定該區塊無望，**中止整個工作階段**。
- 5 分鐘總上限是最後一道防線，涵蓋「每個區塊都恰好在重試 7 次才過」這類病態情況。

#### 2.5 失敗中止時，slave 為什麼不會變磚 —— 五道保障

**這是雙 OTA 分區存在的全部理由，Task 2 的實作必須逐條對得上。**

1. **寫入目標永遠是閒置分區。** `Update.begin()` 內部用
   `esp_ota_get_next_update_partition(NULL)` 取得**目前沒在跑的**那一槽。
   正在執行的韌體所在的分區在整個過程中一個位元都不會被動到。
2. **開機分區只在最後一刻、且校驗通過後才切換。** `esp_ota_set_boot_partition()`
   只在 `Update.end(true)` 內部被呼叫，而它之前會依序檢查：
   已寫入長度 == 宣告長度 → MD5（由 `Update.setMD5()` 設定）相符。
   **任何一項不過就直接回 `false` 且不切換。**
3. **所有中止路徑都走 `Update.abort()`**：收到 `OTA_END(abort=1)`、
   `OTA_SLAVE_IDLE_MS` 逾時、進入 channel 掃描（失去 master）、收到 `HO_PKT_UNPAIR`。
   `abort()` 只是丟棄狀態，不碰 `otadata`。
4. **傳輸中斷電 = 什麼事都沒發生。** `otadata` 未被修改，下次開機仍然從舊分區啟動。
   這正是 Phase 1 Task 3 就把雙 OTA 分區表準備好的價值兌現。
5. **長度與內容的雙重把關。** master 在 `OTA_BEGIN` 與 `OTA_END` **各帶一次** `totalSize`；
   slave 在 `end()` 前先自己比對 `Update.progress() == totalSize`，
   不符就直接 `abort()` 而不進 `end()`。

**唯一無法用軟體覆蓋的殘留風險**（必須寫進 readme 已知限制）：
新韌體**通過 MD5 校驗、成功切換分區、但本身有 bug 開不起來**（例如在 `setupEspNow()` 前就 crash）。
此時 slave 進入開機當機迴圈，只能拆下來接 USB 重燒。
**具體的補救方向（本階段不實作，列為未來項）**：在 slave 啟用
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，開機成功跑到 ESP-NOW 就緒後才呼叫
`esp_ota_mark_app_valid_cancel_rollback()`，否則 bootloader 自動回滾。
這需要改 sdkconfig，超出 Arduino IDE／arduino-cli 的一般流程，因此獨立評估。

---

### 決定 3：master 端的下載策略 —— 暫存在自己的閒置 OTA 分區，不做邊下載邊轉送

| | **A：暫存到閒置 OTA 分區（本案）** | B：邊下載邊轉送（streaming） |
|---|---|---|
| 重傳 | **可以**。任意 `chunkIndex` 都能 `esp_partition_read()` 回來 | **不行**。HTTP stream 不能倒帶，要重傳就得在 RAM 保留視窗 |
| 兩段流控 | 完全解耦。ESP-NOW 慢不會拖住 HTTP socket | 耦合。ESP-NOW 一慢，TLS 連線就閒置到逾時斷開 |
| 完整性 | **傳給 slave 之前就能算完整份 MD5 並比對** | 只能邊傳邊算，發現不符時 slave 已經寫進去大半 |
| 失敗代價 | 下載失敗 = 一次都沒打擾 slave | 下載中途失敗 = slave 已經寫了一半，必須額外中止流程 |
| 成本 | master 每次 OTA 抹除／寫入約 1 MB flash | 無 |
| 記憶體 | 4096 bytes 對齊緩衝 | 需要 ≥ 一個視窗的 RAM |

**選 A。** 決定性的理由是**重傳**：ESP-NOW 在現場一定會丟包，
沒有隨機存取就只能整份重來，而「整份重來」在 90 秒等級的傳輸上等於不可用。
Flash 寫入成本可忽略（OTA 是低頻事件，NOR flash 的抹寫壽命是 10 萬次量級）。

**必須嚴守的兩件事：**

1. **只用 `esp_partition_*` 原生 API，永遠不呼叫 `esp_ota_set_boot_partition()`。**
   在 master 上用 `Update` 類別是絕對禁止的 —— `Update.end(true)` 會把
   **一份 slave 的韌體**設成 master 的開機分區，master 下次開機直接變磚。
   Task 3 的程式碼與註釋都要把這點寫死。
2. **`update_slave`（轉送）與 master 自身的 OTA 共用同一塊暫存區**，
   必須由單一狀態機仲裁。master 自身 OTA 在本階段**不實作**（不在 Phase 4 範圍），
   但 `otaPhase` 這個仲裁點要先建好，日後補上時只是多一個進入分支。

**移植 `ho_relay2::startFirmwareUpdate()` 時要主動避開的三個已知問題：**

| 問題 | `ho_relay2` 的寫法 | 本階段的做法 |
|---|---|---|
| 最壞阻塞 16 分鐘 | 整支函式同步；3 次重試 × 5 分鐘下載逾時 + 裸 `delay()` 退避 | 全部改成 `loop()` 狀態機；單次阻塞 ≤ 15 秒；重試間隔用時間戳而非 `delay()` |
| 302 重定向的 `continue` 不遞增 `retryCount` → 惡意重定向可無限迴圈 | `finalUrl = newUrl; http.end(); continue;` | **刪除整段手寫重定向**，改用 `http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)` + `http.setRedirectLimit(3)`，把次數上限交給函式庫 |
| `setInsecure()` 不驗證憑證 | 直接 `client.setInsecure()` | **本階段仍用 `setInsecure()`，但明確列為已知限制**，並用「App 可在 `update_slave` 帶 `md5` 欄位做端到端校驗」把風險降到可控（見決定 4） |

> **`setFollowRedirects` / `setRedirectLimit` 的相容性**：這兩個 API 在 ESP32 core 的
> `HTTPClient` 已存在多年，core 3.3.7 應可用。**若編譯失敗**，改用手寫版本但**務必**
> 用一個獨立的 `redirectCount` 計數並在 `>= 3` 時失敗 —— 這正是 `ho_relay2` 的缺陷所在。
> 兩種做法都可接受，**但要在 report 寫明用了哪一種**。

---

### 決定 4：進度回報 —— 單一 `ota` 物件掛在 master 狀態上，並且**重算容量常數**

#### 4.1 絕對不要把 OTA 欄位加進 `slaves[]` 陣列

Phase 2b 的 `static_assert` 長這樣：

```cpp
static_assert(
    (STATUS_BUF_SIZE - 1 - STATUS_BASE_MAX_BYTES - SLAVES_KEY_OVERHEAD)
        / SLAVE_ENTRY_MAX_BYTES >= HO_ESPNOW_MAX_SLAVES, ...);
```

現行數值：`(3072 − 1 − 512 − 11) / 96 = 26 ≥ 20`，悲觀邊界只剩 **6 台**的餘裕。

**危險在於：如果在每筆 slave 條目裡加 OTA 欄位卻沒有同步加大 `SLAVE_ENTRY_MAX_BYTES`，
`static_assert` 會用舊常數繼續通過 —— 保護機制看起來還在、實際上已經失效。**
這正是「靜默截斷」缺陷換一個面貌重新出現。

**所以：`SLAVE_ENTRY_MAX_BYTES` 與 `slaves[]` 的欄位在本階段一個字都不准動。**
OTA 進度改掛在 master 狀態的**單一頂層物件**上 —— 反正一次只跑一台（決定 5），
本來就不需要 per-slave 欄位。

```json
"ota": {
  "target": "hoban-aabbccddeeff",
  "phase": "relaying",
  "progress": 57,
  "size": 1048320,
  "error": ""
}
```

#### 4.2 逐項重算常數（Task 5 Step 1 會照這張表寫進註釋）

`ota` 物件的最壞位元組數：

| 片段 | bytes |
|---|---|
| `"ota":{` | 7 |
| `"target":"hoban-aabbccddeeff",` | 30 |
| `"phase":"<最長 12 字元>",` | 23 |
| `"progress":100,` | 15 |
| `"size":2031616,` | 15 |
| `"error":"<最長 16 字元>"` | 26 |
| `}` 與外層逗號 | 2 |
| **合計** | **118 → 取 128** |

因此常數改成**分項相加**的形式，逼未來新增區塊的人也必須新增一個具名常數：

```cpp
// slaves 陣列與 ota 物件「以外」所有欄位的上界（Phase 2b 的實算值，未變）
const size_t STATUS_BASE_WITHOUT_OTA_MAX_BYTES = 512;
// Phase 4 新增的 ota 物件上界，逐項實算見上表
const size_t STATUS_OTA_MAX_BYTES = 128;
const size_t STATUS_BASE_MAX_BYTES =
    STATUS_BASE_WITHOUT_OTA_MAX_BYTES + STATUS_OTA_MAX_BYTES;   // 640
```

重驗 `static_assert`：`(3072 − 1 − 640 − 11) / 96 = 2420 / 96 = 25 ≥ 20` ✓（餘裕 5 台）

**`phase` 與 `error` 必須是固定字串表，不得是自由格式 `String`** —— 上界只有在字串長度
被程式碼本身限制住時才成立。Task 5 會用 `otaPhaseName()` / `otaErrorName()` 兩個查表函式，
並在函式上方註明「新增字串不得超過 12／16 字元」。

**驗證手段（Task 5 必做，不可省略）**：新增序列埠指令 `fakeota`，
把 `ota` 物件灌成最壞值（最長 target／phase／error、progress=100、size=2031616）**但不啟動任何實際工作階段**，
再配合既有的 `fakeslaves 20` + `jsonsize` 實測整份 JSON 的真實大小。
**這是唯一能在只有 1~2 台實體 slave 的情況下證明「20 台 + OTA 進度放得下」的方法。**

#### 4.3 發布頻率

- **不另開 topic、不新增發布路徑。** 只是在 OTA 期間把 master 狀態的發布週期
  由 10 秒**縮短為 5 秒**（`masterStatusIntervalMs()`），OTA 結束就自動回到 10 秒。
- **階段轉換**（開始／下載完成／轉送完成／成功／失敗）**立刻補發一次**
  （把 `lastStatusPub` 設成 0）。
- **進度變化不觸發額外發布** —— 274 個區塊若每塊發一次，最壞每次 `publish()` 吃 3 秒
  socket timeout，直接撞破 30 秒門檻。這是 Phase 2b 決定 2 已經算過的帳。
- 目標 slave 的**代發狀態**在 OTA 期間把 `status` 填成 `"updating"` 並多帶
  `"ota_progress"`（沿用 `ho_relay2` 對 App 既有的 `updating` 語義）。
  那份 doc 只有約 250 bytes，不觸及任何容量常數。

---

### 決定 5：併發限制 —— 全域只有一個工作階段，第二個指令直接拒絕

- **同時只能對一台 slave 做 OTA。** `otaPhase != OTA_IDLE` 時收到第二個 `update_slave`
  → **拒絕**，序列埠印出目前目標與階段，並立刻發一次狀態讓 App 看得到。
- **不排隊。** 排隊需要持久化與 App 端可見的佇列狀態，兩者本階段都沒有；
  而「明確拒絕」讓 App 可以直接重試，語義誠實。
- **`update_slave` 與 master 自身 OTA 互斥** —— 兩者共用同一塊暫存分區。
  master 自身 OTA 本階段不實作，但仲裁點先建好。
- **slave 端也要防重入**：已在工作階段中時，收到 `sessionId` 不同的 `OTA_BEGIN`
  一律回 `HO_OTA_ERR_BUSY` 並忽略 —— 除非目前這個階段已經超過 `OTA_SLAVE_IDLE_MS`
  沒有動靜（視為殘留，先 `abort()` 再接受新的）。
  收到 `sessionId` **相同**的 `OTA_BEGIN` 則視為「master 沒收到我的 READY，重發了」，
  **回一次 READY 但不重置進度**。
- **`ALL:OFF` / 單台 `OFF` 在 OTA 期間永遠可用且不被延後** —— 安全指令的優先權高於 OTA。

---

### 決定 6：OTA 期間的輪詢與 LED

- `pollNextSlave()` 在轉送期間**跳過目標 slave**（它正在忙，且重開機後自然會回報）。
  其餘 19 台照常輪詢，代發照常。
- LED：`updateStatusLed()` 的優先序最前面插入一條
  「**OTA 工作階段進行中 → 快閃 200ms**」，語義與 `ho_relay2` 的更新中指示一致。
  優先序變成：OTA(200ms) > BLE 配網(1000ms) > 配對模式(500ms) > WiFi 未連(300ms) > MQTT 未連(一長二短) > 熄滅。
  **一次性閃爍 `updateBlink()` 仍然最優先**（既有行為不變）。

---

## Task 依賴圖（不得有循環）

```
Task 1（協定：CRC 涵蓋標頭 + 版本 2 + OTA payload + 測試）
   ├→ Task 2（slave 接收端：可單獨編譯、單獨燒錄，master 還不會送）
   └→ Task 3（master 暫存下載：HTTPS → 閒置分區，序列埠 otadl 可單獨驗證）
          └→ Task 4（master 轉送引擎：區塊／ACK／重傳，序列埠 otarelay 可單獨驗證）
                 └→ Task 5（MQTT update_slave + 進度回報 + 容量常數重算 + LED）
Task 6（文件與回歸清單）← 全部
```

**每個 Task 結束時三種型號都必須能單獨編譯通過。**
Task 3 引入 `OtaPhase` 列舉時就把**全部**列舉成員定義齊全（含 Task 4 才會用到的），
Task 4 只補實作不動列舉 —— 避免 Phase 2a 那種「後面的 Task 回頭改前面的宣告」的來回。

---

## Task 1：協定擴充 —— CRC 涵蓋標頭、版本升到 2、OTA 四種 payload

**Files:**
- Modify: `libraries/HoEspNow/src/HoEspNowProtocol.h`
- Modify: `libraries/HoEspNow/src/HoEspNowProtocol.cpp`
- Modify: `ho_espnow_test/ho_espnow_test.ino`

**Interfaces:**
- Consumes：既有的 `hoCrc8()` / `hoPackPacket()` / `hoUnpackPacket()`
- Produces（Task 2~5 全部依賴）：
  - `uint8_t hoFrameCrc(const uint8_t* headerFirst6, const uint8_t* payload, size_t payloadLen)`
  - 封包型別 `HO_PKT_OTA_BEGIN/DATA/END/ACK`
  - `HoOtaBeginPayload` / `HoOtaDataPayload` / `HoOtaEndPayload` / `HoOtaAckPayload`
  - `HoOtaStatus` 列舉
  - 常數 `HO_OTA_CHUNK_SIZE`(240) / `HO_OTA_WINDOW`(16) / `HO_OTA_MAX_CHUNKS`
  - `HO_ESPNOW_VERSION` 由 1 → **2**

---

- [ ] **Step 1: CRC 改為涵蓋標頭**

`HoEspNowProtocol.h`：把 `hoPayloadCrc()` **移除**（不留 deprecated 殼，
它是本函式庫的內部細節，留著只會有人誤用），改成：

```c
// 封包的 CRC。涵蓋範圍：標頭的前 6 bytes（magic/version/type/seq）＋ payload ＋ 共享密鑰。
//
// 為什麼一定要涵蓋標頭（Phase 1 最終審查殘留第 5 項，指定「Phase 4 做 OTA 前必須改」）：
// 舊版只算 payload，於是 type 欄位的位元翻轉「不會」被偵測。一封 OTA_DATA（0x21）的
// type 翻成 HO_PKT_CMD（0x10）之後 CRC 仍然正確，slave 會把韌體資料的前 3 bytes 當成
// HoCmdPayload 解讀 —— cmd 欄位剛好落在 0/1/2 就會實際動作繼電器。
// 這是動物管制設備上的誤觸發，OTA 要送出數千封封包，遇上的機率不再是理論值。
//
// 不含 crc 欄位自己（它就是本函式的輸出），所以只算前 6 bytes。
uint8_t hoFrameCrc(const uint8_t* headerFirst6, const uint8_t* payload, size_t payloadLen);
```

`HoEspNowProtocol.cpp`：

```c
// 把共享密鑰接在資料後面一起跑進 CRC，讓沒有密鑰的封包算不出正確值
static uint8_t hoCrcAppendKey(uint8_t crc) {
  const char* key = HO_ESPNOW_SHARED_KEY;
  for (size_t i = 0; key[i] != '\0'; i++) {
    crc ^= (uint8_t)key[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// 以既有 crc 為初值繼續跑一段資料（hoCrc8 的初值固定為 0，不能直接串接）
static uint8_t hoCrcContinue(uint8_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

uint8_t hoFrameCrc(const uint8_t* headerFirst6, const uint8_t* payload, size_t payloadLen) {
  uint8_t crc = hoCrcContinue(0x00, headerFirst6, 6);
  if (payloadLen > 0 && payload != nullptr) {
    crc = hoCrcContinue(crc, payload, payloadLen);
  }
  return hoCrcAppendKey(crc);
}
```

`hoPackPacket()` 改成先組好標頭（crc 先填 0）再算：

```c
  HoPacketHeader header;
  header.magic   = HO_ESPNOW_MAGIC;
  header.version = HO_ESPNOW_VERSION;
  header.type    = (uint8_t)type;
  header.seq     = seq;
  header.crc     = 0;   // 先填 0，下一行才算得出真值（crc 欄位本身不列入計算）
  header.crc     = hoFrameCrc((const uint8_t*)&header, (const uint8_t*)payload, payloadLen);
```

`hoUnpackPacket()` 對應改成：

```c
  if (header.crc != hoFrameCrc(data, payload, payloadLen)) return false;
```
（`data` 的前 6 bytes 就是標頭的前 6 bytes，直接傳原始緩衝區即可，不必複製。）

- [ ] **Step 2: 版本升到 2，並在標頭寫明這是 flag-day**

```c
// 協定版本。1 → 2 的改動：CRC 由「只涵蓋 payload」改為「涵蓋標頭 + payload」。
//
// ⚠ 這是不相容的破壞性改動，master 與所有 slave 必須「一起」重燒：
//    - 舊 slave 收到新 master 的封包 → version 檢查不過 → 直接丟棄
//    - 於是舊 slave 收不到心跳 → 30 秒後判定失聯 → 強制關閉繼電器（籠子被打開）
// 目前沒有任何現場部署（Phase 2a 已裁定 master 在補上現場恢復手段前不得佈到現場），
// 所以現在改的代價最低。日後若真的需要不停機升級，必須改成「master 同時用新舊兩種
// CRC 各發一次心跳」的過渡期做法 —— 那是另一個階段的工作。
#define HO_ESPNOW_VERSION     2
```

- [ ] **Step 3: 加入 OTA 封包型別、payload 與常數**

在 `HoPacketType` 列舉補上（值照設計規格）：

```c
  HO_PKT_OTA_BEGIN = 0x20,  // master → slave：總長度、包數、MD5、目標版本
  HO_PKT_OTA_DATA  = 0x21,  // master → slave：chunkIndex + 資料塊
  HO_PKT_OTA_END   = 0x22,  // master → slave：結束或中止
  HO_PKT_OTA_ACK   = 0x23,  // 雙向：master 查詢區塊 / slave 回報 bitmap 與狀態
```

常數與 payload 照「決定 2.2」逐字寫入，另加：

```c
// 一包資料的上限。243（HO_ESPNOW_MAX_PAYLOAD）− 3（HoOtaDataPayload 的前置欄位）= 240。
// 因此 OTA_DATA 的封包總長 = 7 + 3 + 240 = 250，剛好貼滿 ESP-NOW 單包上限。
#define HO_OTA_CHUNK_SIZE  240

// 一個區塊的包數。16 讓 ACK 的 bitmap 剛好是一個 uint16_t，
// slave 端的區塊緩衝也剛好 16 × 240 = 3840 bytes。
#define HO_OTA_WINDOW      16

// totalChunks 是 uint16_t，理論上限 65535 包 = 15.7 MB，遠大於 app 分區的 1.94 MB。
// 這個常數只是給兩端做合理性檢查用。
#define HO_OTA_MAX_CHUNKS  ((uint16_t)((2031616 + HO_OTA_CHUNK_SIZE - 1) / HO_OTA_CHUNK_SIZE))
```

`HoOtaStatus`：

```c
enum HoOtaStatus : uint8_t {
  HO_OTA_OK          = 0,  // 校驗通過，即將重啟
  HO_OTA_READY       = 1,  // OTA_BEGIN 已接受，可以開始送資料
  HO_OTA_ERR_BEGIN   = 2,  // Update.begin() 失敗（分區空間不足等）
  HO_OTA_ERR_WRITE   = 3,  // Update.write() 寫入失敗
  HO_OTA_ERR_MD5     = 4,  // MD5 或長度校驗不符，已中止，開機分區未切換
  HO_OTA_ERR_SIZE    = 5,  // 宣告的長度不合理
  HO_OTA_ERR_SESSION = 6,  // sessionId 不符
  HO_OTA_ERR_BUSY    = 7,  // 已有其他工作階段進行中
  HO_OTA_ABORTED     = 8,  // 已依 master 指示或逾時中止
};
```

- [ ] **Step 4: 更新協定測試 sketch**

`ho_espnow_test/ho_espnow_test.ino` 目前有 32 項測試，其中 `testCrc()` 直接呼叫
`hoPayloadCrc()`（本 Task 已移除）。改寫該函式，並**新增三項針對本次改動的測試**：

```cpp
void testCrc() {
  Serial.println("── CRC8 ──");
  uint8_t data[3] = { 0x01, 0x02, 0x03 };
  uint8_t a = hoCrc8(data, 3);
  uint8_t b = hoCrc8(data, 3);
  check(a == b, "同樣輸入產生同樣 CRC");

  uint8_t other[3] = { 0x01, 0x02, 0x04 };
  check(hoCrc8(data, 3) != hoCrc8(other, 3), "不同輸入產生不同 CRC");
  check(hoCrc8(nullptr, 0) == 0x00, "空資料 CRC 為 0");

  // hoFrameCrc 涵蓋標頭前 6 bytes + payload + 共享密鑰
  uint8_t hdr[6] = { 0x48, 0x4F, 0x02, 0x10, 0x01, 0x00 };
  check(hoFrameCrc(hdr, data, 3) != hoCrc8(data, 3), "含標頭與密鑰的 CRC 與純 CRC 不同");
  uint8_t hdr2[6] = { 0x48, 0x4F, 0x02, 0x21, 0x01, 0x00 };   // 只有 type 不同
  check(hoFrameCrc(hdr, data, 3) != hoFrameCrc(hdr2, data, 3), "type 不同會產生不同 CRC");
}

// 新增：標頭被竄改時必須拒收（本 Task 的核心目的）
void testHeaderTamper() {
  Serial.println("── 標頭竄改偵測 ──");
  HoCmdPayload cmd = { HO_CMD_PULSE, 2000 };
  uint8_t buf[250];
  size_t len = hoPackPacket(buf, sizeof(buf), HO_PKT_CMD, 7, &cmd, sizeof(cmd));
  check(len > 0, "打包成功");
  check(hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "未竄改時可解包");

  buf[3] = HO_PKT_OTA_DATA;   // 竄改 type
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "type 被竄改時拒收");

  buf[3] = HO_PKT_CMD;        // 還原 type，改竄改 seq
  buf[4] ^= 0x01;
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "seq 被竄改時拒收");
}

// 新增：OTA payload 的大小必須與協定文件一致（跨端一致性的編譯期保險）
void testOtaStructSizes() {
  Serial.println("── OTA 結構大小 ──");
  check(sizeof(HoOtaBeginPayload) == 26, "HoOtaBeginPayload 為 26 bytes");
  check(sizeof(HoOtaDataPayload) == 3,  "HoOtaDataPayload 為 3 bytes");
  check(sizeof(HoOtaEndPayload) == 6,   "HoOtaEndPayload 為 6 bytes");
  check(sizeof(HoOtaAckPayload) == 6,   "HoOtaAckPayload 為 6 bytes");
  check(sizeof(HoPacketHeader) + sizeof(HoOtaDataPayload) + HO_OTA_CHUNK_SIZE == 250,
        "OTA_DATA 封包總長剛好 250 bytes");
}
```

在 `setup()` 的測試清單加上 `testHeaderTamper();` 與 `testOtaStructSizes();`。
**測試總數會由 32 變成 41**，`printHelp` 或註釋裡若有寫死「32 項」要一併更新
（Phase 1 曾因 commit 訊息寫死項數而需要事後補救）。

- [ ] **Step 5: 編譯驗證**

```powershell
.\flash.ps1 -Model test
.\flash.ps1 -Model slave
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
```
四者皆 exit code 0（本 Task 動到共用函式庫，**四種型號都要編**）。

**report 必須記錄**：四種型號的 flash 位元組數與對 app0 的百分比、RAM 百分比，
並與 Phase 2b Task 3 的基準比較（WROOM 1,690,003 / 83.18%；slave 978,273 / 48.15%）。

- [ ] **Step 6: Commit**

```bash
git add libraries/HoEspNow/src/HoEspNowProtocol.h libraries/HoEspNow/src/HoEspNowProtocol.cpp ho_espnow_test/ho_espnow_test.ino
git commit -m "$(cat <<'EOF'
ESP-NOW 協定：CRC 改為涵蓋標頭，版本升到 2，加入 OTA 四種封包

Phase 1 最終審查的殘留第 5 項指定「Phase 4 做 OTA 前必須把 CRC 改成涵蓋標頭」，
這次兌現。舊版只算 payload，於是 type 欄位的位元翻轉不會被偵測 —— 一封 OTA_DATA
的 type 翻成 HO_PKT_CMD 之後 CRC 仍然正確，slave 會把韌體資料的前 3 bytes 當成
HoCmdPayload 解讀，cmd 欄位剛好落在 0/1/2 就會實際動作繼電器。OTA 一次要送數千封，
這不再是理論風險。

改動：
- 新增 hoFrameCrc()，涵蓋標頭前 6 bytes（magic/version/type/seq）+ payload + 共享密鑰
- 移除 hoPayloadCrc()（不留相容殼，避免有人誤用）
- HO_ESPNOW_VERSION 1 → 2

⚠ 這是不相容的 flag-day 改動：舊 slave 會因版本檢查丟棄新 master 的封包，
30 秒後判定失聯並強制關閉繼電器。master 與所有 slave 必須一起重燒。
目前沒有現場部署，現在改的代價最低。

同時加入 Phase 4 需要的協定內容：
- HO_PKT_OTA_BEGIN/DATA/END/ACK 四種型別與對應 payload
- HO_OTA_CHUNK_SIZE=240、HO_OTA_WINDOW=16（bitmap 剛好一個 uint16_t，
  OTA_DATA 封包總長 7+3+240=250 貼滿 ESP-NOW 上限）
- HoOtaStatus 錯誤碼

協定測試由 32 項增為 41 項，新增「type 被竄改時拒收」「seq 被竄改時拒收」
與 OTA 結構大小的編譯期一致性檢查。

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2：slave 端 OTA 接收

**Files:**
- Modify: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes：Task 1 的全部協定內容；既有的 `masterKnown`/`masterMac`/`txSeq`/
  `lastHeartbeatTime`/`startChannelScan()`/`setRelayPins()`
- Produces（Task 4 在實機上依賴，但編譯上互不依賴）：完整的 slave 端接收行為

> **本 Task 結束後 slave 可以單獨燒錄。** master 還不會送 OTA 封包，
> 所以行為與現在完全一樣 —— 這是刻意的：先把接收端做穩，再讓 master 開始送。

---

- [ ] **Step 1: 把「收到任何 master 封包」都當成 liveness（決定 1a，最重要的一行）**

`onEspNowRecv()` 的「只接受已配對 master 的控制指令」那道 guard **之後**，
在 `HO_PKT_CMD` 分支**之前**插入：

```cpp
  // ── OTA 期間的失聯保護（Phase 4 決定 1a）──
  // 走到這裡代表：封包通過了 magic/version/CRC 驗證（CRC 含共享密鑰與標頭），
  // 而且來源正是本機已配對的 master。這比廣播心跳是更強的存活證據 ——
  // 單播同時證明了「master 活著」和「我跟它在同一個 channel 上」，廣播只證明前者。
  //
  // 沒有這一行的話：OTA 轉送要跑 30~90 秒，期間 ESP-NOW 通道被單播塞滿，
  // 沒有 ACK、沒有重傳的廣播心跳丟包率明顯上升。一旦累積 30 秒沒收到心跳，
  // startChannelScan() 會 (1) 強制關閉繼電器＝籠子被打開，
  // (2) 開始每 1200ms 換一個 channel，當場把正在進行的 OTA 打斷。
  //
  // 這是收緊而非放寬：沒有封包就是沒有封包，30 秒計時照常走完。
  lastHeartbeatTime = millis();
```

> **注意**：這一行必須在 guard **之後**，否則任何持有共享密鑰的第三方都能無限延長
> 本機的失聯計時。放在 guard 之後就只有自己的 master 能刷新它。

- [ ] **Step 2: 加入 include 與 OTA 全域狀態**

檔案上方：

```cpp
#include <Update.h>
```

全域區（放在 `pulseActive` 那組之後）：

```cpp
// ── OTA 接收狀態（Phase 4）──
// 區塊緩衝：先把一個區塊的 16 包收齊在 RAM，再一次順序寫進 Update。
// 這讓「亂序抵達」與「選擇性重傳」對 flash 寫入完全透明 ——
// Update 只看得到嚴格遞增的位元組流，不需要任何 seek 能力。
// 3840 bytes 放在檔案層級（.bss）而非函式區域變數：loopTask 堆疊只有 8192 bytes，
// 而 ESP-NOW recv callback 又跑在另一個 task 上，區域變數在這裡是明確的溢位風險。
static uint8_t otaBlockBuf[HO_OTA_WINDOW * HO_OTA_CHUNK_SIZE];

uint8_t  otaSession     = 0;      // 0 = 目前沒有工作階段
bool     otaActive      = false;  // Update.begin() 已成功、尚未 end/abort
uint32_t otaTotalSize   = 0;
uint16_t otaTotalChunks = 0;
uint16_t otaBlockBase   = 0;      // 目前區塊第一包的 chunkIndex
uint16_t otaBlockMask   = 0;      // bit i = 已收到 otaBlockBase + i
uint32_t otaWritten     = 0;      // 已寫進 Update 的位元組數
unsigned long otaLastPacketAt = 0;

// 這麼久沒收到任何 OTA 封包就自行中止並釋放 Update。
// 與失聯門檻同樣是 30 秒，語義一致：master 不見了就回到已知安全狀態。
const unsigned long OTA_SLAVE_IDLE_MS = 30000;
```

- [ ] **Step 3: 中止與回覆的共用函式**

放在 `sendState()` 之後：

```cpp
// 中止目前的 OTA 工作階段。
// Update.abort() 只是丟棄內部狀態，「不會」碰 otadata ——
// 開機分區維持指向目前正在跑的這一份韌體，所以中止永遠不會變磚。
void otaAbort(const char* why) {
  if (!otaActive && otaSession == 0) return;
  if (otaActive) Update.abort();
  Serial.printf("[OTA] 已中止：%s（已寫入 %u/%u bytes，開機分區未變動）\n",
                why, (unsigned)otaWritten, (unsigned)otaTotalSize);
  otaActive = false;
  otaSession = 0;
  otaWritten = 0;
  otaBlockMask = 0;
  otaBlockBase = 0;
}

// 回一封 OTA_ACK 給 master。mask 只在回報區塊進度時有意義。
void otaSendAck(uint16_t blockBase, uint16_t mask, uint8_t status) {
  if (!masterKnown) return;
  HoOtaAckPayload ack;
  ack.sessionId = otaSession;
  ack.blockBase = blockBase;
  ack.mask      = mask;
  ack.status    = status;

  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), HO_PKT_OTA_ACK, txSeq++, &ack, sizeof(ack));
  esp_now_send(masterMac, buf, total);
}
```

- [ ] **Step 4: `HO_PKT_OTA_BEGIN` 分支**

插在 `HO_PKT_STATE_REQ` 分支之後：

```cpp
  if (header.type == HO_PKT_OTA_BEGIN && payloadLen >= sizeof(HoOtaBeginPayload)) {
    HoOtaBeginPayload bg;
    memcpy(&bg, payload, sizeof(bg));
    otaLastPacketAt = millis();

    // 同一個 sessionId 再次收到 BEGIN：多半是我的 READY 回程丟了，master 重發。
    // 重回一次 READY 就好，「絕不」重置已經收到的進度。
    if (otaActive && bg.sessionId == otaSession) {
      Serial.println("[OTA] 重複收到 OTA_BEGIN，重新回覆 READY（進度保留）");
      otaSendAck(otaBlockBase, otaBlockMask, HO_OTA_READY);
      return;
    }

    // 已有其他工作階段：只有在它已經很久沒動靜（殘留）時才讓位
    if (otaActive && bg.sessionId != otaSession) {
      if (millis() - otaLastPacketAt < OTA_SLAVE_IDLE_MS) {
        Serial.printf("[OTA] 已有工作階段 %u 進行中，拒絕 %u\n", otaSession, bg.sessionId);
        otaSession = bg.sessionId;      // 讓 master 認得出這封 ACK 是回給誰的
        otaSendAck(0, 0, HO_OTA_ERR_BUSY);
        otaSession = 0;
        return;
      }
      otaAbort("上一個工作階段殘留");
    }

    otaSession = bg.sessionId;

    // 合理性檢查：擋掉明顯錯誤的長度，避免白白抹掉整個分區
    if (bg.totalSize < 65536 || bg.totalSize > 2031616 ||
        bg.totalChunks == 0 || bg.totalChunks > HO_OTA_MAX_CHUNKS) {
      Serial.printf("[OTA] 拒絕：長度不合理 size=%u chunks=%u\n",
                    (unsigned)bg.totalSize, bg.totalChunks);
      otaSendAck(0, 0, HO_OTA_ERR_SIZE);
      otaSession = 0;
      return;
    }

    if (!Update.begin(bg.totalSize, U_FLASH)) {
      Serial.printf("[OTA] Update.begin 失敗，錯誤碼 %u（可用空間 %u）\n",
                    Update.getError(), (unsigned)ESP.getFreeSketchSpace());
      otaSendAck(0, 0, HO_OTA_ERR_BEGIN);
      otaSession = 0;
      return;
    }

    // 把期望的 MD5 交給 Update，Update.end(true) 會在切換開機分區「之前」比對。
    // 不符就直接回 false 且不切換 —— 這是「失敗不變磚」的核心那一道。
    char md5hex[33];
    for (int i = 0; i < 16; i++) snprintf(md5hex + i * 2, 3, "%02x", bg.md5[i]);
    Update.setMD5(md5hex);

    otaActive      = true;
    otaTotalSize   = bg.totalSize;
    otaTotalChunks = bg.totalChunks;
    otaBlockBase   = 0;
    otaBlockMask   = 0;
    otaWritten     = 0;

    Serial.printf("[OTA] 開始接收：%u bytes／%u 包，目標版本 %u.%u.%u，MD5 %s\n",
                  (unsigned)bg.totalSize, bg.totalChunks,
                  bg.verMajor, bg.verMinor, bg.verPatch, md5hex);
    otaSendAck(0, 0, HO_OTA_READY);
    return;
  }
```

- [ ] **Step 5: `HO_PKT_OTA_DATA` 分支 —— 收進區塊緩衝，湊齊才寫**

```cpp
  if (header.type == HO_PKT_OTA_DATA && payloadLen > sizeof(HoOtaDataPayload)) {
    HoOtaDataPayload dh;
    memcpy(&dh, payload, sizeof(dh));
    if (!otaActive || dh.sessionId != otaSession) return;   // 殘留封包，靜默丟棄

    otaLastPacketAt = millis();

    size_t dataLen = payloadLen - sizeof(HoOtaDataPayload);
    if (dataLen == 0 || dataLen > HO_OTA_CHUNK_SIZE) return;

    // 只收目前這個區塊內的包。前一個區塊的重複包（master 沒收到 ACK 而重送）
    // 落在區間外，直接丟棄即可 —— 資料已經寫進 Update 了。
    if (dh.chunkIndex < otaBlockBase || dh.chunkIndex >= otaBlockBase + HO_OTA_WINDOW) return;

    uint16_t slot = dh.chunkIndex - otaBlockBase;
    memcpy(otaBlockBuf + (size_t)slot * HO_OTA_CHUNK_SIZE,
           payload + sizeof(HoOtaDataPayload), dataLen);
    otaBlockMask |= (uint16_t)(1u << slot);

    // 這個區塊要幾包才算滿？最後一個區塊可能不足 16 包。
    uint16_t need = HO_OTA_WINDOW;
    if ((uint32_t)otaBlockBase + HO_OTA_WINDOW > otaTotalChunks) {
      need = (uint16_t)(otaTotalChunks - otaBlockBase);
    }
    uint16_t fullMask = (need >= 16) ? 0xFFFF : (uint16_t)((1u << need) - 1u);
    if ((otaBlockMask & fullMask) != fullMask) return;   // 還沒收齊，等 master 查詢

    // 收齊了 → 一次順序寫入。最後一個區塊的長度要用總長度回推，不能假設是滿的。
    uint32_t remain = otaTotalSize - otaWritten;
    size_t writeLen = (size_t)need * HO_OTA_CHUNK_SIZE;
    if (writeLen > remain) writeLen = remain;

    size_t wrote = Update.write(otaBlockBuf, writeLen);
    if (wrote != writeLen) {
      Serial.printf("[OTA] 寫入失敗：預期 %u 實際 %u，錯誤碼 %u\n",
                    (unsigned)writeLen, (unsigned)wrote, Update.getError());
      otaSendAck(otaBlockBase, otaBlockMask, HO_OTA_ERR_WRITE);
      otaAbort("寫入失敗");
      return;
    }
    otaWritten += wrote;

    // 主動回報「這一塊收齊了」，master 就不必等查詢逾時，直接推進下一塊
    otaSendAck(otaBlockBase, fullMask, HO_OTA_OK);

    otaBlockBase += need;
    otaBlockMask = 0;

    // 每 40 個區塊（約 150 KB）印一行，避免序列埠被 270 行進度洗版
    if ((otaBlockBase / HO_OTA_WINDOW) % 40 == 0) {
      Serial.printf("[OTA] 進度 %u%%（%u/%u bytes）\n",
                    (unsigned)(otaWritten * 100 / otaTotalSize),
                    (unsigned)otaWritten, (unsigned)otaTotalSize);
    }
    return;
  }
```

- [ ] **Step 6: `HO_PKT_OTA_ACK`（master 的查詢）與 `HO_PKT_OTA_END`**

```cpp
  // 到達 slave 的 OTA_ACK 一律是 master 的查詢：「這個區塊你收到哪幾包了？」
  // 方向本身就是語義，不需要額外的 request 欄位。
  if (header.type == HO_PKT_OTA_ACK && payloadLen >= sizeof(HoOtaAckPayload)) {
    HoOtaAckPayload q;
    memcpy(&q, payload, sizeof(q));
    if (!otaActive || q.sessionId != otaSession) return;
    otaLastPacketAt = millis();

    // 查詢的區塊已經被我寫完並推進了 → 回一個「全滿」讓 master 直接往前走
    if (q.blockBase < otaBlockBase) {
      otaSendAck(q.blockBase, 0xFFFF, HO_OTA_OK);
      return;
    }
    otaSendAck(otaBlockBase, otaBlockMask, HO_OTA_OK);
    return;
  }

  if (header.type == HO_PKT_OTA_END && payloadLen >= sizeof(HoOtaEndPayload)) {
    HoOtaEndPayload en;
    memcpy(&en, payload, sizeof(en));
    if (en.sessionId != otaSession) return;
    otaLastPacketAt = millis();

    if (en.abort) {
      otaSendAck(0, 0, HO_OTA_ABORTED);
      otaAbort("master 指示中止");
      return;
    }

    // 進 Update.end() 之前先自己比一次長度。長度不符時直接 abort，
    // 連 end() 都不進 —— 少一條可能切換開機分區的路徑。
    if (!otaActive || otaWritten != en.totalSize || otaWritten != otaTotalSize) {
      Serial.printf("[OTA] 長度不符：已寫 %u，master 宣告 %u\n",
                    (unsigned)otaWritten, (unsigned)en.totalSize);
      otaSendAck(0, 0, HO_OTA_ERR_MD5);
      otaAbort("長度不符");
      return;
    }

    // end(true) 會依序檢查長度與 setMD5() 設定的 MD5，
    // 兩者都過才呼叫 esp_ota_set_boot_partition()。不過就回 false 且不切換。
    if (!Update.end(true)) {
      Serial.printf("[OTA] 校驗失敗，錯誤碼 %u —— 開機分區未變動，重啟後仍是舊韌體\n",
                    Update.getError());
      otaSendAck(0, 0, HO_OTA_ERR_MD5);
      otaAbort("校驗失敗");
      return;
    }

    Serial.println("[OTA] 校驗通過，1 秒後重新啟動");
    otaSendAck(0, 0, HO_OTA_OK);
    otaActive = false;
    // 給 ACK 送出的時間再重啟。這裡用裸 delay() 是可以的：
    // 本機即將重開機，繼電器與心跳計時都會被重置，沒有「維持心跳」可言。
    delay(1000);
    ESP.restart();
    return;
  }
```

`HO_PKT_UNPAIR` 分支開頭補一行 `otaAbort("收到解除配對");`。

- [ ] **Step 7: `loop()` 接上逾時，並讓失聯時中止 OTA**

`loop()` 的「已鎖定：超時沒收到心跳就重新掃描」之前插入：

```cpp
  // OTA 工作階段逾時：master 不見了就回到已知安全狀態，釋放 Update
  if (otaActive && now - otaLastPacketAt >= OTA_SLAVE_IDLE_MS) {
    otaAbort("超過 30 秒沒收到 OTA 封包");
  }
```

`startChannelScan()` 開頭（關閉繼電器那段之前）補：

```cpp
  // 失去 master 就不可能繼續接收，先把 Update 釋放掉。
  // 不釋放的話，flash 分區會被一個永遠不會完成的工作階段占著，
  // 下一次 OTA 的 Update.begin() 會失敗。
  otaAbort("失去 master，開始輪掃");
```

> **注意**：`otaAbort()` 必須定義在 `startChannelScan()` **之前**（Arduino 會自動產生
> 函式原型，但把定義順序排對能讓閱讀順序與呼叫順序一致，也避免自動原型產生器
> 在 `static` 陣列宣告上出錯）。

- [ ] **Step 8: 編譯驗證**

```powershell
.\flash.ps1 -Model slave
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
```
三者皆 exit 0。**slave 這次是實質驗證**（新增 `Update.h` 與 3840 bytes 靜態緩衝）。

**report 必須記錄**：slave 的 flash 與 RAM 變化，並確認 flash 仍 < 70%（有大量餘裕）。

- [ ] **Step 9: Commit**

commit 訊息要包含：
- 「收到任何 master 封包都刷新 `lastHeartbeatTime`」為什麼是收緊而非放寬安全性，
  以及沒有它 OTA 會自我打斷（channel 掃描）
- 區塊緩衝的設計（亂序與選擇性重傳對 Update 透明）
- 失敗不變磚的五道保障，特別是「`Update.abort()` 不碰 otadata」

---

## Task 3：master 端暫存下載（HTTPS → 閒置 OTA 分區）

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes：Task 1 的協定；既有的 `maintainEspNow()`/`espNowDelay()`/`sendHeartbeatBurst()`/
  `publishJsonDoc()`/`handleSerialCommand()`/`printHelp()`
- Produces（Task 4、5 依賴）：
  - `enum OtaPhase`（**全部成員一次定義齊**）與 `OtaPhase otaPhase`
  - `bool otaStart(const char* slaveId, const char* url, const char* version, const char* expectMd5, bool force)`
  - `void otaFail(const char* errCode)` / `void otaFinish()`
  - `void updateOtaSession(unsigned long now)`（Task 4 擴充轉送階段）
  - `const char* otaPhaseName()` / `const char* otaErrorName()`
  - `bool otaStageRead(uint32_t offset, uint8_t* out, size_t len)`

---

- [ ] **Step 1: include 與狀態宣告**

檔案上方 include 區：

```cpp
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <MD5Builder.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
```

> **刻意「不」include `Update.h`。** master 把 slave 的韌體當**資料**暫存，
> 用 `Update` 類別的話 `Update.end(true)` 會呼叫 `esp_ota_set_boot_partition()`，
> 把**一份 slave 的韌體**設成 master 的開機分區 —— master 下次開機直接變磚。
> master 一律用 `esp_partition_*` 原生 API，完全不碰 otadata。

全域區（放在 MQTT 區塊之後、`pairingStartTime` 之前）：

```cpp
// ── 轉送 OTA 狀態機（Phase 4）──
// 全部階段一次定義齊，Task 4 只補實作、不再回頭改這個列舉。
enum OtaPhase : uint8_t {
  OTA_IDLE = 0,
  OTA_RESOLVING,       // 預熱 DNS（唯一會吃到 lwIP 15 秒的地方，獨立成一步）
  OTA_ERASING,         // 抹除暫存分區
  OTA_DOWNLOADING,     // HTTPS 串流寫入暫存分區
  OTA_STAGED,          // 下載完成、MD5 已核對
  OTA_BEGIN_SENT,      // 已送 OTA_BEGIN，等 slave 回 READY（Task 4）
  OTA_RELAYING,        // 區塊傳送中（Task 4）
  OTA_WAIT_BLOCK_ACK,  // 等區塊 ACK（Task 4）
  OTA_END_SENT,        // 已送 OTA_END，等 slave 校驗結果（Task 4）
  OTA_VERIFYING,       // 等 slave 重開機後回報新版本（Task 4）
  OTA_SUCCESS,
  OTA_FAILED,
};

OtaPhase otaPhase = OTA_IDLE;
char     otaTargetId[20] = "";       // "hoban-aabbccddeeff"
uint8_t  otaTargetMac[6] = { 0 };
int      otaTargetIdx = -1;
char     otaUrl[160] = "";
char     otaHost[80] = "";
uint8_t  otaVerMajor = 0, otaVerMinor = 0, otaVerPatch = 0;
char     otaErrCode[20] = "";        // 見 otaErrorName() 的長度約束
uint8_t  otaSessionId = 0;
uint32_t otaTotalSize = 0;
uint16_t otaTotalChunks = 0;
uint32_t otaDownloaded = 0;
uint8_t  otaExpectMd5[16] = { 0 };   // App 指定的期望值（全 0 = 未指定）
bool     otaHasExpectMd5 = false;
uint8_t  otaImageMd5[16] = { 0 };    // master 實算的值，會塞進 OTA_BEGIN
int      otaAttempt = 0;
unsigned long otaPhaseStart = 0;
unsigned long otaRetryAt = 0;
unsigned long otaSessionStart = 0;

const int           OTA_MAX_ATTEMPTS   = 3;
const unsigned long OTA_RETRY_GAP_MS   = 10000;   // 固定 10 秒，不做指數退避
const unsigned long OTA_SESSION_MAX_MS = 300000;  // 整段 5 分鐘上限
const size_t        OTA_DL_BYTES_PER_LOOP = 4096; // 每次 loop() 最多從 TLS stream 讀這麼多

// TLS 與 HTTP 物件只在工作階段期間存在（WiFiClientSecure 的 TLS 緩衝約 40~50 KB heap），
// 所以用指標動態配置，結束立刻釋放。
WiFiClientSecure* otaTls  = nullptr;
HTTPClient*       otaHttp = nullptr;
MD5Builder        otaMd5;

// 暫存用的閒置 OTA 分區。master「只寫不切換」，永遠不呼叫 esp_ota_set_boot_partition()。
const esp_partition_t* otaStagePart = nullptr;
static uint8_t otaStageBuf[4096];    // esp_partition_write 需要 4-byte 對齊，統一以 4096 為單位寫
size_t   otaStageFill = 0;
uint32_t otaStageOffset = 0;
```

- [ ] **Step 2: 階段與錯誤字串（長度受約束，容量常數靠它成立）**

```cpp
// ⚠ 這兩張表的字串長度是 STATUS_OTA_MAX_BYTES 這個容量常數的前提：
//    phase 最長 12 字元、error 最長 16 字元。新增字串時不得超過，
//    否則 master 狀態 JSON 的上界推算會失真（見 Task 5 Step 1 的逐項實算）。
const char* otaPhaseName() {
  switch (otaPhase) {
    case OTA_IDLE:           return "idle";
    case OTA_RESOLVING:      return "resolving";
    case OTA_ERASING:        return "erasing";
    case OTA_DOWNLOADING:    return "downloading";   // 11 字元，目前最長
    case OTA_STAGED:         return "staged";
    case OTA_BEGIN_SENT:     return "begin_sent";
    case OTA_RELAYING:       return "relaying";
    case OTA_WAIT_BLOCK_ACK: return "relaying";      // 對 App 而言與 relaying 同義
    case OTA_END_SENT:       return "finishing";
    case OTA_VERIFYING:      return "verifying";
    case OTA_SUCCESS:        return "success";
    case OTA_FAILED:         return "failed";
  }
  return "idle";
}

const char* otaErrorName() { return otaErrCode; }
```

錯誤碼一律用這組固定字串（**最長 16 字元**）：
`busy` / `bad_json` / `no_target` / `offline` / `relay_on` / `bad_url` /
`dns_fail` / `http_fail` / `too_big` / `bad_image` / `md5_mismatch` / `low_heap` /
`flash_fail` / `espnow_fail` / `slave_reject` / `slave_timeout` / `no_return`

- [ ] **Step 3: 暫存分區的存取（只寫不切換）**

```cpp
// 取得閒置的 OTA 分區當暫存區。
// ⚠ 只用 esp_partition_* 原生 API 讀寫，「永遠不呼叫 esp_ota_set_boot_partition()」。
// master 的開機分區在整個轉送過程中一個位元都不會被動到。
bool otaStageOpen(uint32_t needBytes) {
  otaStagePart = esp_ota_get_next_update_partition(NULL);
  if (otaStagePart == nullptr) {
    Serial.println("⚠ [OTA] 找不到閒置的 OTA 分區（分區表沒有雙槽？）");
    return false;
  }
  if (needBytes > otaStagePart->size) {
    Serial.printf("⚠ [OTA] 韌體 %u bytes 超過暫存分區 %u bytes\n",
                  (unsigned)needBytes, (unsigned)otaStagePart->size);
    return false;
  }
  // 只抹除實際會用到的範圍，並對齊到 4096。1 MB 約 1~3 秒，是本階段第三個
  // 不可拆的阻塞點，呼叫端負責前後補心跳。
  uint32_t eraseLen = (needBytes + 4095u) & ~4095u;
  esp_err_t res = esp_partition_erase_range(otaStagePart, 0, eraseLen);
  if (res != ESP_OK) {
    Serial.printf("⚠ [OTA] 抹除暫存分區失敗: %d\n", res);
    return false;
  }
  otaStageFill = 0;
  otaStageOffset = 0;
  Serial.printf("[OTA] 暫存分區 %s 已抹除 %u bytes\n",
                otaStagePart->label, (unsigned)eraseLen);
  return true;
}

// 把 4096 緩衝區沖進分區。esp_partition_write 要求位址與長度 4-byte 對齊，
// 最後一段不足 4 的倍數時補 0xFF（flash 抹除後的值，補進去不改變任何語義）。
bool otaStageFlush() {
  if (otaStageFill == 0) return true;
  size_t len = (otaStageFill + 3u) & ~3u;
  while (otaStageFill < len) otaStageBuf[otaStageFill++] = 0xFF;
  esp_err_t res = esp_partition_write(otaStagePart, otaStageOffset, otaStageBuf, len);
  if (res != ESP_OK) {
    Serial.printf("⚠ [OTA] 寫入暫存分區失敗 offset=%u: %d\n", (unsigned)otaStageOffset, res);
    return false;
  }
  otaStageOffset += len;
  otaStageFill = 0;
  return true;
}

bool otaStageWrite(const uint8_t* data, size_t len) {
  while (len > 0) {
    size_t room = sizeof(otaStageBuf) - otaStageFill;
    size_t n = (len < room) ? len : room;
    memcpy(otaStageBuf + otaStageFill, data, n);
    otaStageFill += n;
    data += n;
    len  -= n;
    if (otaStageFill == sizeof(otaStageBuf) && !otaStageFlush()) return false;
  }
  return true;
}

// 供 Task 4 的轉送引擎隨機存取任一包。
// 這正是選 A（暫存）而非 B（邊下載邊轉送）的決定性理由：HTTP stream 不能倒帶。
bool otaStageRead(uint32_t offset, uint8_t* out, size_t len) {
  if (otaStagePart == nullptr) return false;
  return esp_partition_read(otaStagePart, offset, out, len) == ESP_OK;
}
```

- [ ] **Step 4: 收尾與失敗處理**

```cpp
void otaReleaseHttp() {
  if (otaHttp) { otaHttp->end(); delete otaHttp; otaHttp = nullptr; }
  if (otaTls)  { delete otaTls; otaTls = nullptr; }
}

// 失敗收尾。errCode 必須來自 Step 2 列出的固定字串表（最長 16 字元）。
void otaFail(const char* errCode) {
  snprintf(otaErrCode, sizeof(otaErrCode), "%s", errCode);
  otaReleaseHttp();
  otaPhase = OTA_FAILED;
  otaPhaseStart = millis();
  Serial.printf("[OTA] 失敗：%s（目標 %s）\n", otaErrCode, otaTargetId);
  // 實際的 MQTT 發布由 loop() 的狀態排程處理，這裡不直接 publish
  // （本函式也可能由 onEspNowRecv() 的 WiFi task 路徑間接觸發，見 Task 4）
}

void otaFinish() {
  otaReleaseHttp();
  otaPhase = OTA_SUCCESS;
  otaPhaseStart = millis();
  otaErrCode[0] = '\0';
  Serial.printf("[OTA] 完成：%s 已更新到 %u.%u.%u\n",
                otaTargetId, otaVerMajor, otaVerMinor, otaVerPatch);
}
```

- [ ] **Step 5: 啟動一次工作階段**

```cpp
// 啟動一次轉送 OTA。呼叫端（Task 5 的 MQTT 指令、序列埠 otadl）負責確認 otaPhase == OTA_IDLE。
// expectMd5 可為 nullptr／空字串（App 沒指定）；force 用來略過「繼電器正開著」的保護。
bool otaStart(const char* slaveId, const char* url, const char* version,
              const char* expectMd5, bool force) {
  if (otaPhase != OTA_IDLE && otaPhase != OTA_SUCCESS && otaPhase != OTA_FAILED) {
    Serial.printf("[OTA] 已有工作階段進行中（目標 %s，階段 %s），忽略本次指令\n",
                  otaTargetId, otaPhaseName());
    return false;
  }
  if (!WiFi.isConnected()) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("offline");
    return false;
  }
  // TLS 握手需要約 40~50 KB heap，不夠就不要開始 —— 失敗在握手中途比一開始就拒絕更難查
  if (ESP.getFreeHeap() < 70000) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("low_heap");
    return false;
  }

  uint8_t mac[6];
  if (!hoParseMacFromDeviceId(slaveId, mac)) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("no_target");
    return false;
  }
  int idx = findSlave(mac);
  if (idx < 0) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("no_target");
    return false;
  }
  if (!slaves[idx].online) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("offline");
    return false;
  }
  // ⚠ 安全保護：OTA 一定會讓目標 slave 重開機，繼電器必然回到 LOW，
  //   而 C3 板開機瞬間還會短暫通電（CLAUDE.md 記載的硬體限制，韌體無法根治）。
  //   對一台正把籠門保持在關閉狀態的 slave 做 OTA，等於在遠端把那扇門打開。
  //   所以預設拒絕，要做必須明示 force。
  if (slaves[idx].relay != 0 && !force) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("relay_on");
    Serial.println("[OTA] 目標的繼電器正開著。OTA 會讓它重開機並把繼電器歸零，"
                   "確定要做請在指令加上 \"force\":true");
    return false;
  }

  // 解析 URL 取出 host（只支援 https://），供 OTA_RESOLVING 預熱 DNS 用
  if (strncmp(url, "https://", 8) != 0) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("bad_url");
    Serial.println("[OTA] 只接受 https:// 開頭的網址");
    return false;
  }
  const char* hostStart = url + 8;
  const char* hostEnd = strpbrk(hostStart, "/:");
  size_t hostLen = (hostEnd != nullptr) ? (size_t)(hostEnd - hostStart) : strlen(hostStart);
  if (hostLen == 0 || hostLen >= sizeof(otaHost)) {
    snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
    otaFail("bad_url");
    return false;
  }
  memcpy(otaHost, hostStart, hostLen);
  otaHost[hostLen] = '\0';

  snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);
  memcpy(otaTargetMac, mac, 6);
  otaTargetIdx = idx;
  snprintf(otaUrl, sizeof(otaUrl), "%s", url);
  otaErrCode[0] = '\0';

  // 版本字串 "1.0.1" → 三個 byte
  otaVerMajor = otaVerMinor = otaVerPatch = 0;
  sscanf(version, "%hhu.%hhu.%hhu", &otaVerMajor, &otaVerMinor, &otaVerPatch);

  otaHasExpectMd5 = false;
  memset(otaExpectMd5, 0, sizeof(otaExpectMd5));
  if (expectMd5 != nullptr && strlen(expectMd5) == 32) {
    bool ok = true;
    for (int i = 0; i < 16 && ok; i++) {
      unsigned v = 0;
      if (sscanf(expectMd5 + i * 2, "%2x", &v) != 1) ok = false;
      otaExpectMd5[i] = (uint8_t)v;
    }
    otaHasExpectMd5 = ok;
  }
  if (!otaHasExpectMd5) {
    Serial.println("⚠ [OTA] 指令未附 md5，只能保證 ESP-NOW 這一段的完整性；"
                   "HTTPS 目前用 setInsecure() 不驗證憑證，建議 App 帶上 md5");
  }

  // sessionId 避開 0（0 保留給「無工作階段」）
  otaSessionId = (uint8_t)((millis() % 255) + 1);
  otaAttempt = 0;
  otaDownloaded = 0;
  otaTotalSize = 0;
  otaTotalChunks = 0;
  otaSessionStart = millis();
  otaPhase = OTA_RESOLVING;
  otaPhaseStart = millis();
  otaRetryAt = 0;

  Serial.printf("[OTA] 開始工作階段 %u：目標 %s，版本 %u.%u.%u\n",
                otaSessionId, otaTargetId, otaVerMajor, otaVerMinor, otaVerPatch);
  Serial.printf("[OTA] 網址 %s\n", otaUrl);
  return true;
}
```

- [ ] **Step 6: 狀態機的下載半段**

```cpp
// 轉送 OTA 的狀態機。由 loop() 每輪呼叫一次，「每次只推進一小步」。
// 三個不可拆的阻塞點（DNS 約 15 秒、TLS 握手約 12 秒、抹除分區約 1~3 秒）
// 各自被關在自己的階段裡，且前後補心跳 —— 所以最壞心跳空窗約 15 秒，
// 距離 slave 的 30 秒失聯門檻有一倍餘裕。
void updateOtaSession(unsigned long now) {
  if (otaPhase == OTA_IDLE) return;

  // 整段的總上限。任何階段卡住都由這裡兜底。
  if (otaPhase != OTA_SUCCESS && otaPhase != OTA_FAILED &&
      now - otaSessionStart >= OTA_SESSION_MAX_MS) {
    otaFail("slave_timeout");
    return;
  }

  switch (otaPhase) {

    case OTA_RESOLVING: {
      if (otaRetryAt != 0 && (long)(now - otaRetryAt) < 0) return;   // 非阻塞重試等待
      otaAttempt++;
      if (otaAttempt > OTA_MAX_ATTEMPTS) { otaFail("dns_fail"); return; }

      Serial.printf("[OTA] 解析主機 %s（第 %d 次）\n", otaHost, otaAttempt);
      // ⚠ 這是本階段最長的單一阻塞點。lwIP 的 DNS 沒有 timeout 參數
      //   （Phase 2a 已查證），最壞約 15 秒。前後各補一次心跳連發。
      //   把它獨立成一步的理由：不拆的話 http.begin()+GET() 會「同時」吃到
      //   DNS 的 15 秒與 TLS 的 12 秒，單次阻塞 27 秒，離 30 秒門檻只剩 3 秒。
      //   先在這裡把結果灌進 lwIP 的 DNS 快取，之後 GET() 走快取立即返回。
      sendHeartbeatBurst();
      IPAddress ip;
      bool res = WiFi.hostByName(otaHost, ip);
      sendHeartbeatBurst();

      if (!res) {
        Serial.println("[OTA] DNS 解析失敗，10 秒後重試");
        otaRetryAt = millis() + OTA_RETRY_GAP_MS;
        return;
      }
      Serial.printf("[OTA] %s → %s\n", otaHost, ip.toString().c_str());
      otaAttempt = 0;
      otaRetryAt = 0;
      otaPhase = OTA_DOWNLOADING;
      otaPhaseStart = millis();
      return;
    }

    case OTA_DOWNLOADING: {
      // 第一次進來：建立連線、發 GET、確認長度、抹除分區
      if (otaHttp == nullptr) {
        if (otaRetryAt != 0 && (long)(now - otaRetryAt) < 0) return;
        otaAttempt++;
        if (otaAttempt > OTA_MAX_ATTEMPTS) { otaFail("http_fail"); return; }

        otaTls = new WiFiClientSecure();
        otaHttp = new HTTPClient();
        if (otaTls == nullptr || otaHttp == nullptr) { otaFail("low_heap"); return; }

        // ⚠ 已知限制：不驗證伺服器憑證。沿用 ho_relay2 的做法。
        //   端到端完整性靠 App 在 update_slave 帶 md5（見 otaStart()）。
        otaTls->setInsecure();
        otaHttp->setConnectTimeout(6000);
        otaHttp->setTimeout(6000);
        // ho_relay2 手寫的 302 處理有缺陷：重定向分支用 continue 且「不」遞增
        // retryCount，惡意或設定錯誤的伺服器可讓它無限迴圈。改交給函式庫限制次數。
        otaHttp->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        otaHttp->setRedirectLimit(3);

        sendHeartbeatBurst();
        bool begun = otaHttp->begin(*otaTls, otaUrl);
        int code = begun ? otaHttp->GET() : -1;
        sendHeartbeatBurst();

        if (code != HTTP_CODE_OK) {
          Serial.printf("[OTA] GET 失敗，碼 %d，10 秒後重試\n", code);
          otaReleaseHttp();
          otaRetryAt = millis() + OTA_RETRY_GAP_MS;
          return;
        }

        int len = otaHttp->getSize();
        if (len <= 65536 || len > 2031616) {
          Serial.printf("[OTA] 韌體長度不合理: %d\n", len);
          otaReleaseHttp();
          otaFail("too_big");
          return;
        }
        otaTotalSize = (uint32_t)len;
        otaTotalChunks = (uint16_t)((otaTotalSize + HO_OTA_CHUNK_SIZE - 1) / HO_OTA_CHUNK_SIZE);

        sendHeartbeatBurst();
        bool opened = otaStageOpen(otaTotalSize);
        sendHeartbeatBurst();
        if (!opened) { otaReleaseHttp(); otaFail("flash_fail"); return; }

        otaMd5.begin();
        otaDownloaded = 0;
        Serial.printf("[OTA] 開始下載 %u bytes（%u 包）\n",
                      (unsigned)otaTotalSize, otaTotalChunks);
        return;   // 交還 loop()，下一輪才開始讀
      }

      // 已建立連線：每輪最多讀 OTA_DL_BYTES_PER_LOOP，讀完就交還 loop()
      WiFiClient* stream = otaHttp->getStreamPtr();
      size_t budget = OTA_DL_BYTES_PER_LOOP;
      uint8_t buf[512];
      while (budget > 0 && otaDownloaded < otaTotalSize) {
        int avail = stream->available();
        if (avail <= 0) break;
        size_t want = (size_t)avail;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (want > budget) want = budget;
        int got = stream->readBytes(buf, want);
        if (got <= 0) break;
        if (!otaStageWrite(buf, (size_t)got)) { otaFail("flash_fail"); return; }
        otaMd5.add(buf, (uint16_t)got);
        otaDownloaded += (uint32_t)got;
        budget -= (size_t)got;
      }

      if (otaDownloaded >= otaTotalSize) {
        if (!otaStageFlush()) { otaFail("flash_fail"); return; }
        otaMd5.calculate();
        otaMd5.getBytes(otaImageMd5);
        otaReleaseHttp();

        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", otaImageMd5[i]);

        // 第一個位元組是 ESP32 應用映像的魔術碼。擋掉「下載到一頁 HTML 錯誤頁」
        // 這類最常見的錯誤，成本一次 read。
        uint8_t magic = 0;
        if (!otaStageRead(0, &magic, 1) || magic != 0xE9) {
          Serial.printf("[OTA] 不是合法的 ESP32 映像（首位元組 0x%02x，應為 0xE9）\n", magic);
          otaFail("bad_image");
          return;
        }

        if (otaHasExpectMd5 && memcmp(otaImageMd5, otaExpectMd5, 16) != 0) {
          Serial.printf("[OTA] MD5 與指令指定的不符，實算 %s\n", hex);
          otaFail("md5_mismatch");
          return;
        }

        Serial.printf("[OTA] 下載完成 %u bytes，MD5 %s%s\n",
                      (unsigned)otaDownloaded, hex,
                      otaHasExpectMd5 ? "（與指令指定的相符）" : "（指令未指定，僅供 ESP-NOW 段校驗）");
        otaPhase = OTA_STAGED;
        otaPhaseStart = millis();
        return;
      }

      // 逾時判定：連線還在但長時間沒有新資料
      if (!otaHttp->connected() && stream->available() == 0) {
        Serial.println("[OTA] 下載中連線中斷，10 秒後重試");
        otaReleaseHttp();
        otaRetryAt = millis() + OTA_RETRY_GAP_MS;
        return;
      }
      if (now - otaPhaseStart >= 120000 && otaDownloaded == 0) {
        Serial.println("[OTA] 下載 120 秒沒有任何資料，放棄");
        otaReleaseHttp();
        otaFail("http_fail");
      }
      return;
    }

    case OTA_STAGED:
      // Task 4 會在這裡接上轉送。本 Task 先原地停住並印一行，
      // 讓 otadl 指令可以單獨驗證「下載 + 暫存 + MD5」這一整段。
      Serial.println("[OTA] 已暫存完成，轉送階段尚未實作（Task 4）");
      otaFinish();
      return;

    case OTA_SUCCESS:
    case OTA_FAILED:
      // 停留 30 秒讓 App 讀得到結果，之後回到 idle
      if (now - otaPhaseStart >= 30000) {
        otaPhase = OTA_IDLE;
        otaTargetIdx = -1;
      }
      return;

    default:
      return;
  }
}
```

- [ ] **Step 7: 接進 `loop()` 與序列埠**

`loop()` 在 `slaveStatusScheduler();` 之後加：

```cpp
  // ── 轉送 OTA 狀態機（每次 loop() 只推進一小步，見 updateOtaSession()）──
  updateOtaSession(now);
```

`pollNextSlave()` 開頭加（決定 6）：

```cpp
  // 轉送期間跳過目標 slave：它正忙著收韌體，而且重開機後自然會回報狀態
  if (otaPhase >= OTA_BEGIN_SENT && otaPhase <= OTA_VERIFYING && pollIdx == otaTargetIdx) {
    pollIdx = (pollIdx + 1) % (slaveCount > 0 ? slaveCount : 1);
    return;
  }
```
> 實作時要對照 `pollNextSlave()` 現行的游標變數名稱，**不要憑這段程式碼假設它叫 `pollIdx`**。

`handleSerialCommand()` 加兩個 verb（不走 `parseIndexArg()`，參數不是數字）：

```cpp
  } else if (verb == "otadl") {
    // 測試用：只跑「下載 + 暫存 + MD5」，不轉送。用法：otadl <slave 編號> <url>
    int sp = argStr.indexOf(' ');
    if (sp < 0) {
      Serial.println("用法：otadl <slave 編號> <https 網址>");
    } else {
      int n = argStr.substring(0, sp).toInt();
      String u = argStr.substring(sp + 1);
      u.trim();
      if (n < 0 || n >= slaveCount) {
        Serial.println("[OTA] slave 編號超出範圍");
      } else {
        char id[20];
        hoFormatDeviceId(slaves[n].mac, id);
        otaStart(id, u.c_str(), "0.0.0", nullptr, true);
      }
    }
  } else if (verb == "otastat") {
    Serial.printf("[OTA] 階段=%s 目標=%s 進度=%u/%u bytes 錯誤=%s\n",
                  otaPhaseName(), otaTargetId,
                  (unsigned)otaDownloaded, (unsigned)otaTotalSize,
                  otaErrCode[0] ? otaErrCode : "無");
```

`printHelp()` 補：
```cpp
  Serial.println("  otadl <n> <url>  測試用：只下載並暫存韌體，不轉送（會抹除閒置 OTA 分區）");
  Serial.println("  otastat        印出目前 OTA 工作階段的階段與進度");
```

- [ ] **Step 8: 編譯驗證**

```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
```
三者皆 exit 0。

**report 必須記錄且與探針實測比對**：WROOM 的 flash 位元組數與百分比。
探針預測「加 HTTPClient + WiFiClientSecure 約 +126 KB → 約 89%」，
本 Task 少了 `Update.h`（master 用原生分區 API）、多了實際邏輯碼。
**實測值若超過 1,930,035 bytes（95%）就標紅並停下來回報。**

- [ ] **Step 9: Commit**

commit 訊息必須說明：
- 為什麼 master 不用 `Update` 而用 `esp_partition_*`（`Update.end(true)` 會把 slave 的韌體
  設成 master 的開機分區）
- 為什麼要有獨立的 `OTA_RESOLVING` 階段（27 秒單次阻塞拆成 15 + 12）
- `ho_relay2` 的三個已知問題各自怎麼避開（16 分鐘阻塞、302 無限迴圈、`setInsecure`）
- 選「暫存」而非「邊下載邊轉送」的決定性理由是重傳需要隨機存取

---

## Task 4：master 端轉送引擎（區塊視窗、ACK、重傳、中止）

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes：Task 3 的 `otaPhase` / `otaStageRead()` / `otaFail()` / `otaFinish()` /
  `updateOtaSession()`；Task 1 的協定
- Produces（Task 5 依賴）：完整可用的轉送流程、`otaProgressPercent()`

---

- [ ] **Step 1: `espNowSendTo()` 拆出安靜版**

轉送一份 1 MB 韌體要送 4400 包，其中一部分必然遇到
`ESP_ERR_ESPNOW_NO_MEM`（ESP-NOW 傳送佇列滿）。現行 `espNowSendTo()` 每次失敗都
`Serial.printf`，會把序列埠洗到無法閱讀。

```cpp
// verbose=false 的版本供 OTA 大量送出使用：佇列滿（ESP_ERR_ESPNOW_NO_MEM）是
// 正常的背壓訊號而不是錯誤，不該印。呼叫端看回傳值決定下一輪重試。
bool espNowSendToEx(const uint8_t mac[6], HoPacketType type,
                    const void* payload, size_t len, bool verbose) {
  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), type, txSeq++, payload, len);
  if (total == 0) {
    if (verbose) Serial.println("[ESP-NOW] 打包失敗");
    return false;
  }
  esp_err_t res = esp_now_send(mac, buf, total);
  if (res != ESP_OK) {
    if (verbose) Serial.printf("[ESP-NOW] esp_now_send 失敗: %d\n", res);
    return false;
  }
  return true;
}

// 既有呼叫端一行都不用改
bool espNowSendTo(const uint8_t mac[6], HoPacketType type, const void* payload, size_t len) {
  return espNowSendToEx(mac, type, payload, len, true);
}
```

- [ ] **Step 2: 轉送狀態與流控**

```cpp
// ── 轉送引擎（Phase 4 決定 2）──
uint16_t otaBlockBase = 0;      // 目前區塊第一包的 chunkIndex
uint16_t otaSendMask  = 0;      // bit i = blockBase+i「還要送」
uint16_t otaAckMask   = 0;      // slave 回報已收到的 bitmap
int      otaBlockRetry = 0;
int      otaPollCount  = 0;
unsigned long otaLastSendAt = 0;
unsigned long otaWaitStart  = 0;
volatile bool otaAckPending = false;   // ESP-NOW callback 收到 ACK，交給 loop() 處理
volatile uint16_t otaAckBase = 0;
volatile uint16_t otaAckBits = 0;
volatile uint8_t  otaAckStatus = 0;

const unsigned long OTA_ACK_TIMEOUT_MS  = 400;
const int           OTA_POLL_MAX        = 3;
const int           OTA_BLOCK_MAX_RETRY = 8;
// 每次 loop() 最多送幾包。
//
// 為什麼用「每輪固定包數」而不是「追蹤 in-flight 計數」：
// in-flight 要在 loop() 遞增、在 onEspNowSent()（WiFi task）遞減，那是一組
// 跨 task 的 read-modify-write 競態。改成固定配額之後完全沒有共享可變狀態，
// 而 ESP_ERR_ESPNOW_NO_MEM 本身就是完美的背壓訊號 —— 佇列滿就這輪不送，
// 下一輪 loop() 再試，自然貼著硬體的實際速率跑。
const int OTA_SEND_PER_LOOP = 2;
```

- [ ] **Step 3: 送一包與送查詢**

```cpp
// 從暫存分區讀出第 idx 包並送給目標 slave。
bool otaSendChunk(uint16_t idx) {
  uint32_t off = (uint32_t)idx * HO_OTA_CHUNK_SIZE;
  uint32_t remain = otaTotalSize - off;
  size_t dataLen = (remain < HO_OTA_CHUNK_SIZE) ? (size_t)remain : HO_OTA_CHUNK_SIZE;

  uint8_t pkt[sizeof(HoOtaDataPayload) + HO_OTA_CHUNK_SIZE];
  HoOtaDataPayload dh;
  dh.sessionId  = otaSessionId;
  dh.chunkIndex = idx;
  memcpy(pkt, &dh, sizeof(dh));
  if (!otaStageRead(off, pkt + sizeof(dh), dataLen)) return false;

  return espNowSendToEx(otaTargetMac, HO_PKT_OTA_DATA, pkt, sizeof(dh) + dataLen, false);
}

// 送一封「這個區塊你收到哪幾包了？」。到達 slave 的 OTA_ACK 一律是查詢。
void otaSendPoll() {
  HoOtaAckPayload q;
  q.sessionId = otaSessionId;
  q.blockBase = otaBlockBase;
  q.mask      = 0;
  q.status    = 0;
  espNowSendToEx(otaTargetMac, HO_PKT_OTA_ACK, &q, sizeof(q), false);
}

// 本區塊要幾包（最後一塊可能不足 16）
uint16_t otaBlockNeed() {
  uint16_t need = HO_OTA_WINDOW;
  if ((uint32_t)otaBlockBase + HO_OTA_WINDOW > otaTotalChunks) {
    need = (uint16_t)(otaTotalChunks - otaBlockBase);
  }
  return need;
}

uint16_t otaFullMask() {
  uint16_t need = otaBlockNeed();
  return (need >= 16) ? 0xFFFF : (uint16_t)((1u << need) - 1u);
}

uint8_t otaProgressPercent() {
  if (otaPhase == OTA_DOWNLOADING || otaPhase == OTA_RESOLVING || otaPhase == OTA_ERASING) {
    if (otaTotalSize == 0) return 0;
    return (uint8_t)((uint64_t)otaDownloaded * 100 / otaTotalSize);
  }
  if (otaTotalChunks == 0) return 0;
  if (otaPhase == OTA_SUCCESS) return 100;
  return (uint8_t)((uint32_t)otaBlockBase * 100 / otaTotalChunks);
}
```

- [ ] **Step 4: ESP-NOW callback 只設旗標**

`onEspNowRecv()` 在 `HO_PKT_STATE` 分支之後加：

```cpp
  if (header.type == HO_PKT_OTA_ACK && payloadLen >= sizeof(HoOtaAckPayload)) {
    // 本函式跑在 WiFi task。這裡「只」搬旗標，一切判斷與後續送出都交給 loop()。
    // 在這裡直接送下一個區塊等於從另一個 task 動 ESP-NOW 與暫存分區，是明確的競態。
    if (memcmp(info->src_addr, otaTargetMac, 6) != 0) return;
    HoOtaAckPayload ak;
    memcpy(&ak, payload, sizeof(ak));
    if (ak.sessionId != otaSessionId) return;
    otaAckBase    = ak.blockBase;
    otaAckBits    = ak.mask;
    otaAckStatus  = ak.status;
    otaAckPending = true;
    return;
  }
```

> **注意**：`onEspNowRecv()` 開頭有一行無條件的
> `Serial.printf("[ESP-NOW] 收到 type=0x%02x seq=%u 來自 %s rssi=%d\n", ...)`。
> 轉送期間 slave 每個區塊回一封 ACK，1 MB 會產生 270+ 行。
> **必須在該 printf 加上條件**：`if (header.type != HO_PKT_OTA_ACK)`，
> 並在註釋說明理由。**Task 6 寫回歸清單時要以修改後的行為為準。**

- [ ] **Step 5: 狀態機的轉送半段**

把 Task 3 Step 6 的 `case OTA_STAGED:` 換成完整實作，並補上其餘階段：

```cpp
    case OTA_STAGED: {
      HoOtaBeginPayload bg;
      bg.totalSize   = otaTotalSize;
      bg.totalChunks = otaTotalChunks;
      memcpy(bg.md5, otaImageMd5, 16);
      bg.verMajor  = otaVerMajor;
      bg.verMinor  = otaVerMinor;
      bg.verPatch  = otaVerPatch;
      bg.sessionId = otaSessionId;

      espNowSendToEx(otaTargetMac, HO_PKT_OTA_BEGIN, &bg, sizeof(bg), true);
      otaBlockBase = 0;
      otaBlockRetry = 0;
      otaAckPending = false;
      otaPhase = OTA_BEGIN_SENT;
      otaPhaseStart = now;
      Serial.printf("[OTA] 已送出 OTA_BEGIN 給 %s，等待回應\n", otaTargetId);
      return;
    }

    case OTA_BEGIN_SENT: {
      if (otaAckPending) {
        otaAckPending = false;
        if (otaAckStatus == HO_OTA_READY) {
          Serial.println("[OTA] slave 已就緒，開始轉送");
          otaSendMask = otaFullMask();
          otaAckMask = 0;
          otaBlockRetry = 0;
          otaPollCount = 0;
          otaPhase = OTA_RELAYING;
          otaPhaseStart = now;
        } else {
          Serial.printf("[OTA] slave 拒絕，狀態碼 %u\n", otaAckStatus);
          otaFail("slave_reject");
        }
        return;
      }
      // 3 秒沒回應就重送 BEGIN，最多 5 次
      if (now - otaPhaseStart >= 3000) {
        otaBlockRetry++;
        if (otaBlockRetry > 5) { otaFail("slave_timeout"); return; }
        otaPhase = OTA_STAGED;   // 回上一階段重送
      }
      return;
    }

    case OTA_RELAYING: {
      // 每輪最多送 OTA_SEND_PER_LOOP 包；佇列滿就這輪不送，下一輪再試
      int sent = 0;
      for (uint16_t i = 0; i < HO_OTA_WINDOW && sent < OTA_SEND_PER_LOOP; i++) {
        if ((otaSendMask & (1u << i)) == 0) continue;
        if (!otaSendChunk(otaBlockBase + i)) break;   // NO_MEM：背壓，下一輪再送
        otaSendMask &= (uint16_t)~(1u << i);
        sent++;
      }
      if (otaSendMask == 0) {
        otaSendPoll();
        otaPollCount = 1;
        otaWaitStart = now;
        otaPhase = OTA_WAIT_BLOCK_ACK;
      }
      return;
    }

    case OTA_WAIT_BLOCK_ACK: {
      if (otaAckPending) {
        otaAckPending = false;
        uint16_t full = otaFullMask();

        // slave 已經推進到更後面的區塊 → 代表這塊它早就寫完，直接跟上
        if (otaAckBase > otaBlockBase) {
          otaBlockBase = otaAckBase;
          otaBlockRetry = 0;
          otaSendMask = otaFullMask();
          otaPhase = OTA_RELAYING;
          return;
        }
        if (otaAckBase == otaBlockBase && (otaAckBits & full) == full) {
          otaBlockBase += otaBlockNeed();
          otaBlockRetry = 0;
          if (otaBlockBase >= otaTotalChunks) {
            HoOtaEndPayload en;
            en.sessionId = otaSessionId;
            en.abort = 0;
            en.totalSize = otaTotalSize;
            espNowSendToEx(otaTargetMac, HO_PKT_OTA_END, &en, sizeof(en), true);
            otaPhase = OTA_END_SENT;
            otaPhaseStart = now;
            Serial.println("[OTA] 全部區塊已送達，已送出 OTA_END，等待校驗結果");
            return;
          }
          otaSendMask = otaFullMask();
          otaPhase = OTA_RELAYING;
          // 每 40 塊印一行，避免 270 行洗版
          if ((otaBlockBase / HO_OTA_WINDOW) % 40 == 0) {
            Serial.printf("[OTA] 轉送進度 %u%%（%u/%u 包）\n",
                          otaProgressPercent(), otaBlockBase, otaTotalChunks);
          }
          return;
        }

        // 缺包 → 只補送缺的那幾包
        otaSendMask = (uint16_t)(full & ~otaAckBits);
        otaBlockRetry++;
        if (otaBlockRetry > OTA_BLOCK_MAX_RETRY) {
          Serial.printf("[OTA] 區塊 %u 重試 %d 次仍失敗\n", otaBlockBase, otaBlockRetry);
          HoOtaEndPayload en;
          en.sessionId = otaSessionId;
          en.abort = 1;
          en.totalSize = otaTotalSize;
          espNowSendToEx(otaTargetMac, HO_PKT_OTA_END, &en, sizeof(en), true);
          otaFail("espnow_fail");
          return;
        }
        otaPhase = OTA_RELAYING;
        return;
      }

      if (now - otaWaitStart < OTA_ACK_TIMEOUT_MS) return;

      // 逾時：先重發查詢（成本 1 包），確認不是 ACK 回程丟了；
      // 查詢也試完才整塊重送（成本 16 包）。弱訊號下這一層能省下大量重複資料。
      if (otaPollCount < OTA_POLL_MAX) {
        otaPollCount++;
        otaSendPoll();
        otaWaitStart = now;
        return;
      }
      otaBlockRetry++;
      if (otaBlockRetry > OTA_BLOCK_MAX_RETRY) {
        HoOtaEndPayload en;
        en.sessionId = otaSessionId;
        en.abort = 1;
        en.totalSize = otaTotalSize;
        espNowSendToEx(otaTargetMac, HO_PKT_OTA_END, &en, sizeof(en), true);
        otaFail("espnow_fail");
        return;
      }
      otaSendMask = otaFullMask();
      otaPollCount = 0;
      otaPhase = OTA_RELAYING;
      return;
    }

    case OTA_END_SENT: {
      if (otaAckPending) {
        otaAckPending = false;
        if (otaAckStatus == HO_OTA_OK) {
          Serial.println("[OTA] slave 校驗通過，正在重新啟動，等它回線確認版本");
          otaPhase = OTA_VERIFYING;
          otaPhaseStart = now;
        } else {
          Serial.printf("[OTA] slave 校驗失敗，狀態碼 %u（它的開機分區未變動）\n", otaAckStatus);
          otaFail("md5_mismatch");
        }
        return;
      }
      if (now - otaPhaseStart >= 10000) { otaFail("slave_timeout"); }
      return;
    }

    case OTA_VERIFYING: {
      // slave 重開機後會恢復配對、鎖回 channel，master 的 pollNextSlave()
      // 會問到它的狀態，版本欄位由 HO_PKT_STATE 更新進 slaves[]。
      if (otaTargetIdx >= 0 && otaTargetIdx < slaveCount &&
          slaves[otaTargetIdx].online &&
          slaves[otaTargetIdx].fwMajor == otaVerMajor &&
          slaves[otaTargetIdx].fwMinor == otaVerMinor &&
          slaves[otaTargetIdx].fwPatch == otaVerPatch) {
        otaFinish();
        return;
      }
      if (now - otaPhaseStart >= 90000) {
        Serial.printf("[OTA] 等待 %s 回線 90 秒逾時，它可能沒有開起來\n", otaTargetId);
        otaFail("no_return");
      }
      return;
    }
```

- [ ] **Step 6: 序列埠指令 `otarelay`**

```cpp
  } else if (verb == "otarelay") {
    // 測試用：把「master 目前正在跑的這一份韌體」當假韌體轉送給指定 slave。
    // ⚠ 這會讓 slave 收到一份「master 的韌體」——校驗一定會過、也一定會切換分區，
    //   slave 重開機後會跑 master 的程式。所以這條指令「只」能在開發板上用，
    //   用完必須重燒 slave。序列埠會印出警告，且需要輸入 confirm 參數才執行。
    // 用法：otarelay <slave 編號> confirm
```
> **裁量點：** 若實作者判斷這條指令的誤用風險高於它的驗證價值，
> **可以改成「要求先跑一次 `otadl` 把真正的 slave 韌體下載進暫存區，
> 再用 `otarelay <n>` 從暫存區轉送」**，完全不提供「拿 master 韌體當假資料」的路徑。
> **後者是建議做法**，前面那段只是說明為什麼不直接那樣做。
> 實作時請採用後者，並在 report 說明。

`printHelp()` 補一行。

- [ ] **Step 7: 編譯驗證**

三種型號皆 exit 0，記錄 flash／RAM 與 Task 3 的差異。

- [ ] **Step 8: Commit**

commit 訊息要點名：
- 區塊 + bitmap 為什麼優於每包 ACK 與真滑動視窗（含吞吐估算）
- 為什麼用「每輪固定配額」而不是跨 task 的 in-flight 計數
- 「先重發查詢再整塊重送」兩層重傳的用意
- `onEspNowRecv()` 開頭的 log 為什麼要排除 OTA_ACK

---

## Task 5：MQTT `update_slave` 指令、進度回報與容量常數重算

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes：Phase 2b Task 4 的 `handleMasterCommand()`；Task 3/4 的 `otaStart()`/
  `otaPhaseName()`/`otaProgressPercent()`；Phase 2b Task 1 的 `publishJsonDoc()` 與容量常數
- Produces：`ota` 狀態欄位、`update_slave` 指令、`fakeota` 測試指令、OTA 期間的 LED

---

- [ ] **Step 1: 重算容量常數（本 Task 最重要的一步）**

把 Phase 2b 的 `STATUS_BASE_MAX_BYTES` 改成**分項相加**：

```cpp
// ⚠ 這裡刻意拆成兩個具名常數再相加，不是把 512 直接改成 640。
//    理由：Phase 4 差一點踩到一個很隱蔽的陷阱 ——
//    如果把 OTA 進度欄位加進「每一筆 slave 條目」卻沒有同步加大
//    SLAVE_ENTRY_MAX_BYTES，下面那個 static_assert 會拿舊常數繼續通過，
//    保護機制看起來還在、實際已經失效，「靜默截斷」會換一個面貌回來。
//    拆成具名分項之後，任何人要新增一個 JSON 區塊都必須先為它宣告一個常數，
//    才有辦法把它加進總和 —— 讓「忘記重算」變成一件做不到的事。
//
// slaves 陣列與 ota 物件「以外」所有欄位的上界（Phase 2b 的實算值，本階段未變）
const size_t STATUS_BASE_WITHOUT_OTA_MAX_BYTES = 512;

// Phase 4 新增的 ota 物件上界。逐項實算（最壞值）：
//   "ota":{                                    =   7
//   "target":"hoban-aabbccddeeff",             =  30
//   "phase":"<最長 12 字元>",                   =  23
//   "progress":100,                            =  15
//   "size":2031616,                            =  15
//   "error":"<最長 16 字元>"                    =  26
//   } 與外層逗號                                =   2
//                                          合計 = 118 → 取 128
// ⚠ 上界成立的前提是 otaPhaseName() 的字串 ≤ 12 字元、錯誤碼 ≤ 16 字元。
//    改動那兩張表時必須回頭重算這個常數。
const size_t STATUS_OTA_MAX_BYTES = 128;

const size_t STATUS_BASE_MAX_BYTES =
    STATUS_BASE_WITHOUT_OTA_MAX_BYTES + STATUS_OTA_MAX_BYTES;   // 640
```

**`SLAVE_ENTRY_MAX_BYTES` 維持 96，`slaves[]` 的欄位一個字都不准動。**
在該常數上方補一行註釋說明「Phase 4 的 OTA 進度刻意不放進 slave 條目，理由見上」。

重驗 `static_assert`（**必須手算一次寫進 report**）：
`(3072 − 1 − 640 − 11) / 96 = 2420 / 96 = 25 ≥ 20` ✓，餘裕 5 台。

- [ ] **Step 2: `buildStatusDoc()` 加上 `ota` 物件**

在 `appendSlavesArray(doc);` **之前**插入：

```cpp
  // OTA 進度。刻意做成單一頂層物件而不是每筆 slave 條目的欄位 ——
  // 一次只跑一台（見 otaStart() 的併發保護），本來就不需要 per-slave 欄位，
  // 而加進 slaves[] 會讓 SLAVE_ENTRY_MAX_BYTES 與 static_assert 失真。
  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["target"]   = otaTargetId;
  ota["phase"]    = otaPhaseName();
  ota["progress"] = otaProgressPercent();
  ota["size"]     = otaTotalSize;
  ota["error"]    = otaErrCode;
```

- [ ] **Step 3: 目標 slave 的代發狀態標成 `updating`**

`publishSlaveStatus()` 內，`doc["status"]` 那行改成：

```cpp
  // 轉送期間對 App 顯示 "updating"，語義沿用 ho_relay2 對一般設備的既有用法。
  bool isOtaTarget = (idx == otaTargetIdx) &&
                     (otaPhase >= OTA_BEGIN_SENT && otaPhase <= OTA_VERIFYING);
  if (isOtaTarget) {
    doc["status"] = "updating";
    doc["ota_progress"] = otaProgressPercent();
  } else {
    doc["status"] = slaves[idx].online ? "online" : "offline";
  }
```

> 這份 doc 約 250 bytes，遠低於 `statusBuf` 3072，不觸及任何容量常數。

- [ ] **Step 4: 發布節奏**

`loop()` 的狀態發布區塊，把寫死的 `10000` 換成：

```cpp
// OTA 期間把 master 狀態的發布週期由 10 秒縮短為 5 秒，結束自動回到 10 秒。
// 刻意「不」在每個區塊完成時額外發布：274 個區塊若每塊發一次，
// 最壞每次 publish() 吃 3 秒 socket timeout，直接撞破 slave 的 30 秒失聯門檻。
unsigned long masterStatusIntervalMs() {
  return (otaPhase != OTA_IDLE) ? 5000UL : 10000UL;
}
```
```cpp
      if (now - lastStatusPub > masterStatusIntervalMs()) {
```

階段轉換要立刻補一次。在 `otaStart()` 成功處、`otaFail()`、`otaFinish()`
以及 `OTA_STAGED` / `OTA_VERIFYING` 的進入點各加一行：

```cpp
  otaStatusDirty = true;   // 階段轉換：讓 loop() 這一輪就補發一次 master 狀態
```
`loop()` 的發布條件改成：
```cpp
      if (otaStatusDirty || now - lastStatusPub > masterStatusIntervalMs()) {
        otaStatusDirty = false;
        lastStatusPub = now;
        publishStatus();
      }
```

- [ ] **Step 5: `update_slave` 指令**

在 `handleMasterCommand()` 的 `LR:` 分支之前插入：

```cpp
  } else if (message.startsWith("update_slave:")) {
    // 規格：update_slave:{"id":"hoban-...","version":"1.0.1","url":"https://..."}
    // 本階段另外接受兩個選用欄位：
    //   "md5"  ：32 字元十六進位。有帶的話 master 會在轉送「之前」核對下載到的映像，
    //            這是目前唯一能涵蓋 HTTPS 那一段的完整性保護（憑證未驗證，見已知限制）。
    //   "force"：true 時略過「目標繼電器正開著」的保護。OTA 一定會讓 slave 重開機，
    //            繼電器必然歸零 —— 對一台正把籠門保持關閉的設備而言等於遠端開門。
    String body = message.substring(13);
    JsonDocument cmd;
    DeserializationError err = deserializeJson(cmd, body);
    if (err) {
      Serial.printf("[OTA] update_slave 的 JSON 解析失敗: %s\n", err.c_str());
      snprintf(otaErrCode, sizeof(otaErrCode), "bad_json");
      otaPhase = OTA_FAILED;
      otaPhaseStart = millis();
      otaStatusDirty = true;
      return;   // 若 handleMasterCommand() 不是 void 或此處不是函式尾，改用對應的流程控制
    }
    const char* id  = cmd["id"]  | "";
    const char* url = cmd["url"] | "";
    const char* ver = cmd["version"] | "0.0.0";
    const char* md5 = cmd["md5"] | "";
    bool force = cmd["force"] | false;

    if (strlen(id) == 0 || strlen(url) == 0) {
      Serial.println("[OTA] update_slave 缺少 id 或 url");
      snprintf(otaErrCode, sizeof(otaErrCode), "bad_json");
      otaPhase = OTA_FAILED;
      otaPhaseStart = millis();
    } else if (otaPhase != OTA_IDLE && otaPhase != OTA_SUCCESS && otaPhase != OTA_FAILED) {
      Serial.printf("[OTA] 已有工作階段進行中（目標 %s，階段 %s），忽略本次指令\n",
                    otaTargetId, otaPhaseName());
      snprintf(otaErrCode, sizeof(otaErrCode), "busy");
    } else {
      otaStart(id, url, ver, md5, force);
    }
    otaStatusDirty = true;
```

> **`busy` 這個分支刻意不覆蓋 `otaTargetId`** —— 那個欄位要留著顯示「正在忙誰」。

- [ ] **Step 6: LED 指示**

`updateStatusLed()` 在 `bleConfigMode` 分支**之前**插入：

```cpp
  // OTA 工作階段進行中：快閃 200ms，語義與 ho_relay2 的更新中指示一致。
  // 優先權在 BLE 配網之上 —— OTA 只會在已連網的狀態下發生，兩者不會同時成立，
  // 放最前面是為了讓「正在做危險動作」這件事一眼可見。
  if (otaPhase != OTA_IDLE && otaPhase != OTA_SUCCESS && otaPhase != OTA_FAILED) {
    setLeds((now / 200) % 2 == 0);
    return;
  }
```

- [ ] **Step 7: `fakeota` 容量實測指令**

**這一步不可省略** —— 沒有它就無法證明「20 台 slave + OTA 進度」放得下。

```cpp
// 測試用：把 ota 物件灌成「最壞情況」的內容，配合 fakeslaves 20 + jsonsize
// 實測整份 master 狀態 JSON 的真實大小。
// ⚠ 刻意「不」啟動任何實際工作階段：otaPhase 只被設成 OTA_DOWNLOADING（字串最長的那個），
//   updateOtaSession() 會在下一輪 loop() 因為 otaHttp == nullptr 而走進重試路徑 ——
//   所以本指令設完之後立刻把 otaPhase 復原成 OTA_IDLE，只留下欄位值供 jsonsize 量測。
//   真正被量到的是 buildStatusDoc() 組出來的字串，不是狀態機的行為。
void fakeOtaForCapacityTest() {
  snprintf(otaTargetId, sizeof(otaTargetId), "hoban-aabbccddeeff");
  snprintf(otaErrCode, sizeof(otaErrCode), "slave_timeout");   // 目前最長的錯誤碼
  otaTotalSize = 2031616;
  otaTotalChunks = HO_OTA_MAX_CHUNKS;
  otaBlockBase = otaTotalChunks;      // 讓 otaProgressPercent() 回 100（3 位數，最壞）
  Serial.println("[測試] ota 欄位已灌成最壞值（未啟動工作階段），請接著執行 jsonsize");
  Serial.println("       正確的驗證順序：fakeslaves 20 → fakeota → jsonsize");
}
```

`handleSerialCommand()` 加 `fakeota` 與 `printHelp()` 說明。

> **量測時 `phase` 會是 `"idle"`（4 字元）而不是最壞的 12 字元**，
> 兩者差 8 bytes。實作者在 report 記錄 `jsonsize` 結果時**必須把這 8 bytes 加回去**
> 再與 3072 比較，並在 `fakeota` 的說明裡寫明這一點。

- [ ] **Step 8: 編譯與容量驗證**

```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
```
三者皆 exit 0。

**report 必須包含**：
1. `static_assert` 算式的手算過程（`(3072−1−640−11)/96 = 25 ≥ 20`）
2. 三種型號的 flash／RAM，以及從 Phase 2b Task 3（WROOM 1,690,003 / 83.18%）到現在的總增量
3. 若 WROOM ≥ 95%（1,930,035 bytes）→ **標紅並停下來回報**

- [ ] **Step 9: Commit**

commit 訊息必須說明：
- 為什麼 OTA 進度是單一頂層物件而不是 per-slave 欄位（`static_assert` 會靜默失效）
- `STATUS_BASE_MAX_BYTES` 拆成具名分項相加的用意
- 為什麼進度不是每個區塊發一次（274 × 3 秒 socket timeout）
- `force` 與 `relay_on` 保護的理由（OTA 必然讓 slave 重開機＝遠端開門）

---

## Task 6：文件、回歸清單與整體驗證

**Files:**
- Modify: `ho_master1/readme.md`
- Modify/Create: `ho_slave1/readme.md`
- Create: `docs/phase4-regression-checklist.md`

---

- [ ] **Step 1: 更新 `ho_master1/readme.md`**

新增或更新這些章節：

1. **`update_slave` 指令的完整格式**（送到 `hoban/<masterId>/control`），
   四個欄位（`id`/`version`/`url` 必填，`md5`/`force` 選填）與各自的語義
2. **`ota` 狀態欄位的完整說明** —— 每個 `phase` 值的意義、每個 `error` 值的意義，
   以及 App 該怎麼呈現（`updating` 狀態、進度條、失敗訊息）
3. **轉送 OTA 的流程圖**（下載 → 暫存 → BEGIN → 區塊/ACK → END → 校驗 → 重啟 → 回線確認版本）
4. **容量預算**（更新版）：`STATUS_BASE_WITHOUT_OTA_MAX_BYTES` + `STATUS_OTA_MAX_BYTES` 的
   分項相加、重算後的 `static_assert`、以及「為什麼 OTA 欄位不能放進 slaves 陣列」
5. **已知限制**（新增）：
   - **協定版本 2 是 flag-day**：master 與所有 slave 必須一起重燒。
     混版的後果是 slave 收不到心跳 → 30 秒 → 強制關閉繼電器
   - **HTTPS 不驗證憑證**（`setInsecure()`）。端到端完整性請靠 `md5` 欄位
   - **一次只能對一台 slave 做 OTA，不排隊**，第二個指令會被拒絕並回 `busy`
   - **OTA 必然讓目標 slave 重開機，繼電器歸零**；C3 板開機瞬間還會短暫通電（硬體限制）。
     預設在繼電器開著時拒絕，需 `"force":true`
   - **DNS 解析最壞阻塞約 15 秒**（lwIP 無 timeout 參數），這是本階段最長的單一心跳空窗
   - **master 暫存用的閒置 OTA 分區在轉送期間會被覆寫**；master 自身 OTA 本階段未實作
   - 沿用 Phase 2a/2b 既有的：代發 topic 沒有 LWT、`smartConnect()` 最壞 18 秒、
     `WiFi.begin(ssid,pass,ch,nullptr)` 的掃描語義未經證實
6. **序列埠指令表** —— 補上 `otadl <n> <url>`、`otarelay <n>`、`otastat`、`fakeota`，
   並標明哪些是測試工具

- [ ] **Step 2: 更新／建立 `ho_slave1/readme.md`**

必須涵蓋：
- OTA 接收流程與區塊緩衝的設計
- **失敗不變磚的五道保障**（決定 2.5 逐條）
- **唯一無法用軟體覆蓋的殘留風險**：新韌體通過校驗但本身開不起來 →
  只能拆下來接 USB 重燒；補救方向是 bootloader rollback（本階段未實作，需改 sdkconfig）
- 「收到任何 master 封包都刷新 `lastHeartbeatTime`」的理由

- [ ] **Step 3: 寫 `docs/phase4-regression-checklist.md`**

> ### 這一步有一條硬性規定，違反即視為 Task 未完成
>
> **Phase 2a 有一類缺陷出現了三次：回歸清單的驗收標準與程式碼矛盾，
> 導致實測者把正確行為判成 FAIL。**（最後一次是清單寫「按住 1.5 秒放開不應印出任何訊息」，
> 但程式碼按下瞬間就無條件印出訊息，100% 觸發。）
>
> 因此本清單的**每一條「預期序列埠輸出」都必須逐字對照實際程式碼的
> `Serial.print` / `Serial.printf` 格式字串確認**，做法是：
>
> 1. 寫下預期輸出後，用 `Grep` 在 `ho_master1/ho_master1.ino` 或 `ho_slave1/ho_slave1.ino`
>    搜尋該字串的**固定部分**（去掉 `%d`/`%s`/`%u` 等格式指示子的那一段）
> 2. **搜不到就是寫錯了**，不准靠記憶或推測補上
> 3. 在清單每一項的結尾用一行註記寫出對照過的位置，格式：
>    `> 對照：ho_slave1.ino:512 的 Serial.printf("[OTA] 開始接收：%u bytes／%u 包…")`
> 4. **凡是「不應該印出某訊息」這類否定式判準一律禁止**，除非能明確指出程式碼中
>    沒有任何路徑會印出它。改寫成正面判準
> 5. 每一項都要區分「**失敗判定**」與「**觀察項**」。機制未經實機驗證的
>    （`setFollowRedirects` 的實際行為、`esp_partition_erase_range` 的實際耗時、
>    ESP-NOW 在現場的實際吞吐）**一律列為觀察項**
>
> **本階段特別容易踩的兩處**（Task 4 都動過輸出）：
> - `onEspNowRecv()` 開頭那行 `[ESP-NOW] 收到 type=...` 已被加上
>   「排除 `HO_PKT_OTA_ACK`」的條件 —— 清單不得再要求看到 OTA_ACK 的收包行
> - master 與 slave 的進度都是**每 40 個區塊**才印一行，不是每塊都印

清單開頭必須有與 Phase 2a／2b 同樣的警告：**本清單尚未在任何實體硬體上執行過任何一項。**

必須涵蓋的項目（至少）：

| # | 項目 | 重點 |
|---|---|---|
| 0 | **前置：master 與所有 slave 都燒了新韌體** | 協定版本 2 是 flag-day；混版時 slave 會在 30 秒後印失聯並關閉繼電器。**失敗判定** |
| 1 | `.\flash.ps1 -Model test -Upload` 後跑協定測試，應顯示 41 項全過 | Task 1 的測試 |
| 2 | 平時（無 OTA）行為與 Phase 2b 完全一致：狀態 10 秒一則、代發輪播、`ALL:*`、配對 | **回歸，失敗判定** |
| 3 | `otadl <n> <url>` 指向一個真實的 slave `.bin`：序列埠依序出現「解析主機」→「開始下載」→「下載完成 … MD5」 | 只驗證下載段 |
| 4 | 同上，**下載全程 slave 端不得出現失聯訊息** | **失敗判定**（決定 1c 的核心） |
| 5 | 故意給一個不存在的網域：10 秒後重試、共 3 次後 `[OTA] 失敗：dns_fail`，**期間 slave 不失聯** | 驗證非阻塞重試 |
| 6 | 故意給一個回傳 HTML 的網址：`[OTA] 不是合法的 ESP32 映像` | 驗證 0xE9 檢查 |
| 7 | 指令帶錯誤的 `md5`：`[OTA] MD5 與指令指定的不符` 且**完全沒有打擾 slave** | 驗證「暫存」相對「串流」的價值 |
| 8 | 完整轉送：MQTT 對 master 送 `update_slave:{...}`，slave 更新成功並重啟 | 主要驗收條件 |
| 9 | 轉送全程（30~90 秒）**其他 slave 一台都不失聯、繼電器不被強制關閉** | **失敗判定，本階段最重要的一條** |
| 10 | 轉送期間 App／MQTT Explorer 看得到 `ota.phase` 由 `downloading` → `relaying` → `verifying` → `success`，`progress` 遞增 | |
| 11 | 轉送期間目標 slave 的代發 status 是 `"updating"` 且帶 `ota_progress` | |
| 12 | slave 重啟後回線，master 印出 `[OTA] 完成：… 已更新到 x.y.z`，`ota.phase` 變 `success` | 驗證版本回檢 |
| 13 | **轉送中途把目標 slave 拔電**：master 重試後 `[OTA] 失敗：espnow_fail` 或 `slave_timeout`；**slave 復電後仍是舊韌體、可正常配對與控制** | **不變磚的正面驗證，失敗判定** |
| 14 | **轉送中途把 master 拔電**：slave 印出 `[OTA] 已中止：超過 30 秒沒收到 OTA 封包`，**開機分區未變動** | **失敗判定** |
| 15 | 故意送一份 MD5 對不上的映像（改一個位元組）：slave 印出校驗失敗、**重啟後仍是舊韌體** | 驗證 `Update.end(true)` 那道 |
| 16 | 轉送進行中再送一次 `update_slave`：master 印出「已有工作階段進行中」，`ota.error` 為 `busy`，**原工作階段不受影響** | 決定 5 |
| 17 | 對繼電器正開著的 slave 送 `update_slave`（不帶 force）：`ota.error` 為 `relay_on` 且**沒有開始下載** | 決定 1 的附帶保護 |
| 18 | 同上但帶 `"force":true`：正常開始 | |
| 19 | **容量驗證**：`fakeslaves 20` → `fakeota` → `jsonsize`，記錄實際 bytes（**加回 phase 字串的 8 bytes**），必須 < 3072 且**沒有** `slaves_truncated` | |
| 20 | 同上狀態實際發布一次 `status`，MQTT Explorer 收到的 JSON **語法完整、20 筆條目齊全、`ota` 物件完整** | 「靜默截斷」的正面驗證 |
| 21 | 轉送期間 `ALL:OFF` 立刻生效 | 安全指令優先權，**失敗判定** |
| 22 | 抹除暫存分區實際耗時幾秒 | **觀察項** |
| 23 | 一份 1 MB 韌體的實際轉送時間、重傳次數 | **觀察項**，用來校正 30~90 秒的估計 |
| 24 | 302 重定向的網址是否正確跟隨 | **觀察項**（`setFollowRedirects` 行為未在本專案驗證） |
| 25 | LED 在 OTA 期間快閃 200ms，結束後回到正常狀態指示 | |
| 26 | **回歸 Phase 2a/2b**：BLE 配網、長按重置、`FIND_BEST_SERVER`、`HASRELAY:*`、`UNPAIRALL` 行為不變 | **失敗判定** |

- [ ] **Step 4: 完整編譯與資源盤點**

```powershell
.\flash.ps1 -Model test
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
```
四者皆 exit code 0。

**在 report 做一張表**：

| 型號 | Phase 2b Task 3 | Phase 4 完成 | 增量 | 對 app0 百分比 | RAM |
|---|---|---|---|---|---|
| master (WROOM) | 1,690,003 (83.18%) | ? | ? | ? | ? |
| master-c3 | ? | ? | ? | ? | ? |
| slave | 978,273 (48.15%) | ? | ? | ? | ? |

與探針實測（+126 KB → 約 89.05%）比對並說明差異來源。
**WROOM ≥ 95% 就標紅。**

- [ ] **Step 5: 更新 SDD ledger**

在 `.superpowers/sdd/2026-08-17-esp32-phase4-ota-relay/progress.md` 記錄：
- 每個 Task 的 commit 範圍與 flash 數字
- 所有 Ruling（含被推翻的）
- 交付使用者判斷的殘留項
- **Phase 5（Long Range）開工前必須知道的事**：協定已升到版本 2、
  `HoHeartbeatPayload.longRange` 欄位仍未生效、`esp_wifi_set_protocol()` 的行為仍未驗證

- [ ] **Step 6: Commit**

---

## 本階段結束後的狀態

- App 可以對任一台已配對且在線的 slave 發起韌體更新，全程有進度可看、有錯誤碼可查
- 轉送全程 ESP-NOW 心跳不中斷，**其他 slave 完全無感**
- 失敗（下載失敗、傳輸中斷、校驗不符、任一端斷電）**都不會讓 slave 變磚**
- ESP-NOW 協定的 CRC 涵蓋標頭，`type` 位元翻轉造成的誤觸發風險消失
- **尚未有**：master 自身的 OTA（仍需 USB 重燒）、bootloader rollback、
  OTA 排隊、HTTPS 憑證驗證、Long Range 實測（Phase 5）

## 已知風險

| 風險 | 影響 | 緩解 |
|---|---|---|
| 協定版本 2 是 flag-day，漏燒一台 slave | 該台 30 秒後強制關閉繼電器＝籠子被打開 | 回歸清單第 0 項列為前置且是失敗判定；readme 已知限制明列 |
| DNS 最壞阻塞約 15 秒 | 單次心跳空窗 15 秒 | 獨立成 `OTA_RESOLVING` 階段、前後補 `sendHeartbeatBurst()`；距門檻一倍餘裕 |
| 新韌體通過校驗但本身開不起來 | slave 進開機當機迴圈，只能 USB 重燒 | 本階段不解；readme 寫明補救方向是 bootloader rollback（需改 sdkconfig） |
| `setInsecure()` 不驗證憑證 | 遭 MITM 可植入任意韌體 | `md5` 欄位提供端到端校驗；列為已知限制，建議 App 一律帶 md5 |
| ESP-NOW 現場吞吐低於估計 | 轉送超過 5 分鐘總上限而失敗 | `OTA_SESSION_MAX_MS` 是可調常數；回歸清單第 23 項把實測時間列為觀察項供校正 |
| master 暫存分區被 slave 韌體占用 | master 自身 OTA 暫時不可用 | master 自身 OTA 本階段未實作；`otaPhase` 已是單一仲裁點，日後補上只是多一個分支 |
| WROOM flash 逼近上限 | 後續階段編不過 | 每個 Task 記錄用量；≥ 95% 標紅停工回報 |
| `otaAckPending` 等旗標的跨 task 讀寫無鎖 | 極端時序下漏收一次 ACK | 全部是單一 byte／word 的 `volatile` 存取；漏一次只會走進 400ms 逾時後重發查詢，行為收斂 |
| `fakeota` 量到的 `phase` 是 `"idle"` 而非最壞的 12 字元 | 容量結論偏樂觀 8 bytes | 指令說明與回歸清單第 19 項都要求把 8 bytes 加回去再比較 |
