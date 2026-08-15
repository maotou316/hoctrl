# Master Phase 2b 實作計畫：MQTT 代理、slaves 陣列、群組指令、LR 同步

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 讓 `ho_master1` 成為所有已配對 slave 的 MQTT 代理 —— 用 slave 的 MAC 代發 `hoban/hoban-<slaveMac>/status`、代訂閱 `hoban/hoban-<slaveMac>/control`，master 自身狀態帶上 `slaves` 陣列，並新增 `ALL:*` / `SLAVES` / `PAIR:*` / `UNPAIR:*` / `LR:*` 指令。**全程不讓 ESP-NOW 心跳空窗超過 30 秒**，且**狀態 JSON 在 20 台 slave 下不得靜默截斷**。

**Architecture:** 在 Phase 2a 已就緒的 WiFi/MQTT/BLE 之上加一層代理。三個結構性改動：
1. **唯一發布出口** —— 所有 MQTT JSON 發布都走 `publishJsonDoc()`，先 `measureJson()` 再決定要不要發，把 Phase 2a 的「靜默截斷」變成「明確拒發＋序列埠告警」。容量以 `static_assert` 在編譯期保證放得下 20 台。
2. **Topic 解析取代字串相等** —— `mqttCallback()` 不再拿 topic 跟固定字串比，改為解析出 device id → MAC → 查名冊，一套邏輯同時處理 master 自己與 20 台 slave。
3. **一切代發都排程化** —— 代發 20 台 slave 的狀態走「每次 loop() 最多發一台」的錯開排程（與 Phase 2a 的 `pollNextSlave()` 同一套設計），永遠不會出現一次連發 21 個 topic 的突波。

**Tech Stack:** Arduino ESP32 core 3.3.7、PubSubClient、**ArduinoJson 7.4.3**、Preferences (NVS)、EEPROM (slave)、esp_now.h、esp_wifi.h

**Spec:** `docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md`（「MQTT 協定擴充」章節逐字照用）

**前置：** Phase 2a 已完成（分支 `feature/esp32-master-slave`）。master 已有 ESP-NOW 心跳／配對／名冊（NVS `homaster`）、NVS 網路設定（`hoban`）、WiFi 連線、MQTT 多伺服器、BLE 配網、LED 狀態機、長按重置。

**執行紀錄（必讀）：** `.superpowers/sdd/2026-08-15-esp32-master-phase2a/progress.md`

---

## Global Constraints

### 安全鐵則（違反即為 Critical）

- **繼電器安全鐵則**：`initRelayPins()` 必須是 `setup()` 第一行，早於 `Serial.begin()`。
- **ESP-NOW 不可中斷鐵則**：master 心跳每 1 秒（WiFi 關聯期間 200ms），slave 超過 **30 秒**沒收到心跳就**強制關閉繼電器**。這是動物管制設備 —— 繼電器被強制關閉等同**籠子被打開**。因此：
  - 所有等待一律用 `espNowDelay()`，**不得用裸 `delay()`**（`sendHeartbeatBurst()` 內部那個 `delay(200)` 是唯一例外，它本身就在發心跳）
  - 新增任何迴圈式的批次操作（例如代發 N 台、對 N 台送指令）都必須**錯開成每次 loop() 一台**，或在迴圈內呼叫 `maintainEspNow()`
  - `mqttClient.publish()` 在連線正常時很快，但 socket 卡住時會拖到 `setSocketTimeout(3)`。**連續 21 次 publish 最壞可達數秒**，這正是本階段不做「一次全發」的原因
- **ESP-NOW callback 不得發 MQTT**：`onEspNowRecv()` 跑在 WiFi task，不是 `loop()` context。只能設旗標（`dirty`），實際發布交給 `loop()`。

### 已知的既有阻塞點（本階段不修，但不得惡化）

| 位置 | 最壞阻塞 | 備註 |
|---|---|---|
| `mqttClient.connect()` 單次 | 約 **18 秒**（DNS 15 + TCP 3） | 不可中斷；`smartConnect()` 每次只試一台正是為此 |
| `connectToWiFi()` | 15 秒 | 有 `anyResetButtonPressed()` 逃生口 |
| `sendCmdToAll()` | 20 × `delay(20)` = 400ms | Phase 2a 裁定可接受；本階段改走 `espNowDelay()` 順手消除 |

### 容量與資源

- **狀態 JSON 真正的邊界是 `char buf[N]`，不是 `mqttClient.setBufferSize()`**。超過 `buf` 時 `serializeJson()` **截斷**，截斷後長度仍 < mqtt buffer → `publish()` 回 `true` → **靜默失敗**。
- **`StaticJsonDocument<N>` 的 N 在 ArduinoJson 7.4.3 完全被忽略**（只是 `compatibility.hpp` 的相容殼，底層動態配置）。放大 N 沒有任何作用，`doc.overflowed()` 量的是「記憶體配置失敗」不是「容量超限」。**本階段全面改用 `JsonDocument`，並且不依賴任何宣告容量，只依賴 `measureJson()` 的實測值。**
- **Flash 餘裕**：WROOM 目前 **82.7%**（1,683,315 / 2,031,616），只剩約 **348 KB**。**每個 Task 的編譯驗證都必須記錄 flash 位元組數與百分比**，並與前一個 Task 比較。累計增量若超過 **60 KB** 就要在 report 中標紅（Phase 4 的轉送 OTA 估需 170~250 KB）。
- **RAM**：目前 WROOM 19%、C3 12%。本階段預估 +5.5 KB（`statusBuf` 3072 靜態 + mqtt buffer 由 1024 擴到 3328）。

### 專案慣例

- 結果變數用 `res`，不用 `result`
- 註釋、序列埠輸出、文件、commit 訊息一律**繁體中文**
- MQTT topic：`hoban/{deviceId}/status`（發布）、`hoban/{deviceId}/control`（訂閱），deviceId 格式 `hoban-<mac12>`
- **不得修改** `ho_relay1/`、`ho_relay2/`、`ho_relay3/`、`CLAUDE.md`、`flash.ps1`
  （progress.md 已裁定 `flash.ps1` 沒有問題，若編譯報錯先檢查 cwd）
- **雙板支援**：`ho_master1` 同時支援 WROOM 與 C3，GPIO 用 `CONFIG_IDF_TARGET_ESP32C3` / `_ESP32` 條件編譯，新增程式碼不得破壞這個結構
- 驗證走 `flash.ps1` + `arduino-cli` 1.3.1，**不新增外部工具鏈**

### Phase 2a 留下、本階段必須沿用的認知更正（不得沿用錯誤前提）

1. `WiFi.disconnect(bool wifioff, bool eraseap)` 第一個參數是 **wifioff**，不是 eraseap。
2. `WiFi.setAutoReconnect(true)` 在 core 3.3.7 是**死碼**，Arduino 3.x 不會自己背景重連。
3. `setSocketTimeout(3)` 只管「TCP 已連線後等 CONNACK」，不管 TCP connect 本身，更不管 DNS。
4. `WiFi.begin(ssid, pass, ch, nullptr)` 依 IDF 文件是「**從**該 channel **開始**掃描」而非「鎖定該 channel」，機制未經實機驗證。**不要在本階段的文件或回歸清單裡把它寫成已證實的事實。**

---

## 檔案結構

```
ho_master1/ho_master1.ino          # 主要改動，約 +550 行（1947 → 約 2500）
ho_slave1/ho_slave1.ino            # 只有 Task 6 動到（LR 接收與套用）
libraries/HoEspNow/HoEspNowProtocol.h   # 只有 Task 6 動到（+2 個封包型別、+1 個 payload struct）
libraries/HoEspNow/HoEspNowProtocol.cpp # 不動
ho_master1/readme.md               # Task 7
docs/phase2b-regression-checklist.md    # Task 7 新增
```

**不拆檔**，理由同 Phase 2a：Arduino sketch 目錄下的 `.ino` 會被自動合併，拆檔無隔離效果只增加理解成本。

---

## 本計畫的五個設計決定（先讀完再開工）

實作時若發現任一決定站不住腳，**停下來回報**，不要自行改成別的做法。

### 決定 1：Topic 比對改成「解析 → 查名冊」，不維護 topic 字串表

**做法**：`parseControlTopic()` 純字串檢查 + hex 解析取出 MAC（O(1)），再用既有的 `findSlave()` 線性掃描名冊（最多 20 次 6 bytes 的 `memcmp`）。

**為什麼不預先產生 21 條 topic 字串再逐條 `strcmp`**：那要 21 × 33 = 693 bytes RAM，而且配對／解除配對時得同步維護，多一份可能與名冊不一致的狀態 —— 名冊已經是唯一真相，再複製一份就是在製造 bug。

**線性掃描可以嗎？可以，而且差距不重要**：`mqttCallback()` 只在收到 MQTT 訊息時被呼叫（App 按按鈕才發生，頻率以「次／分鐘」計），最多 120 bytes 的 `memcmp` 是微秒級。跟同一條路徑上動輒毫秒的 `publish()` 相比完全可忽略。**上 hash map 是在解一個不存在的問題。**

**取捨**：topic 格式從此被寫死在解析函式裡（長度必須剛好 `6 + 18 + 8 = 32`）。若日後 topic 格式改變，`parseControlTopic()` 是唯一要改的地方 —— 這反而比散落 21 條字串好。

### 決定 2：代發頻率 —— 錯開輪播 15 秒一輪，加上「有變化就立刻補發」

**做法**：
- master 自己的狀態：維持 10 秒一次（不動）
- slave 代發：每 `15000 / slaveCount` ms 發**一台**，一輪 15 秒剛好把全部發完。20 台 → 每 750ms 發一台。**每次 `loop()` 最多發一台。**
- 任一台有變化（收到 `PKT_STATE` 且 relay 或版本有變、或 online↔offline 翻轉）→ 設 `dirty`，排程器優先發 dirty 的那台，不必等輪到它

**為什麼不全部 10 秒同頻**：21 個 topic 同一個時間點連發，最壞情況（socket 卡住）每次 `publish()` 吃到 3 秒 socket timeout，21 次就是 63 秒 —— 直接撞破 30 秒門檻。錯開後每次 `loop()` 只有一次 `publish()`，最壞單次 3 秒，中間都有心跳。

**為什麼是 15 秒而不是 10 秒**：slave 的資料本身就由 `pollNextSlave()` 以 15 秒一輪的節奏更新，代發比資料更新還快是純浪費頻寬。兩者對齊後，每台 slave 的代發都能帶到剛更新過的資料。

**為什麼還要 dirty 立刻補發**：App 按下「開門」後若要等最多 15 秒才看到 relay 狀態翻轉，體感是壞掉的。dirty 路徑讓「送指令 → slave 動作 → 回 `PKT_STATE` → 立刻代發」在 1 秒內完成。

**取捨**：極端情況（20 台同時翻轉）會連續 20 次 loop 各發一台，但每次之間仍有完整的 `loop()` 週期與心跳，不構成突波。

### 決定 3：離線代發由既有的 30 秒偵測驅動，並在解除配對時補一次

**觸發點三個**：
1. `updateSlaveOnlineStatus()`（每 15 秒跑一次）偵測到 online → offline 翻轉時設 `dirty`，排程器下一輪就代發 `"status":"offline"`
2. 之後的每一輪輪播**繼續代發 offline**（retain=true，broker 上的保留訊息永遠是最新的真相）
3. `UNPAIR` 時先代發一次 offline，再取消訂閱 —— 否則 broker 上會永遠留著一則 `"status":"online"` 的保留訊息，App 上出現一台永遠在線的幽靈設備

**已知限制（必須寫進 readme 與回歸清單，本階段不解）**：PubSubClient 一條連線只能設**一個** LWT，那個名額已經給了 master 自己。所以 **master 斷電時，20 台 slave 的 status topic 會停在最後一則 `"online"` 保留訊息**。App 端要靠 `via` 欄位 —— 看到自己的 master 離線就把底下所有 slave 一併視為離線（這是 Phase 3 的 App 工作，本階段只負責把 `via` 欄位確實發出去並記錄這個限制）。

### 決定 4：`slaves` 陣列 —— 編譯期保證放得下 20 台，執行期截斷只當「絕不靜默」的安全網

**這是本階段最關鍵的決定，展開在 Task 1 與 Task 2。**

**不分頁**。理由：規格定義的 App 契約裡沒有分頁合併語義，硬加分頁會讓 Phase 3 的 App 得處理「收到第 2 頁但第 1 頁掉了」這類狀態；而且每台 slave 本來就有自己的代發 topic，完整資訊不會因為 master 狀態裡的陣列不完整而遺失 —— `slaves` 陣列的用途是「在 master 詳情頁一眼看到全貌」，不是唯一資料來源。

**做法是三層**：
1. **編譯期**：`static_assert` 保證 `statusBuf` 扣掉基礎欄位上界之後，還能裝下 `HO_ESPNOW_MAX_SLAVES × SLAVE_ENTRY_MAX_BYTES`。有人日後把 `statusBuf` 改小，**編譯直接失敗**，而不是上線後靜默截斷。
2. **執行期上限**：用同一組常數算出 `maxEntries`，超出的部分不加進陣列，改成 `"slaves_truncated": true` + `"slaves_shown": N`。**照選定的數值這條路永遠不會走到**，它存在的意義是「萬一真的走到，App 與序列埠都看得見」。
3. **發布前量測**：`publishJsonDoc()` 先 `measureJson()`，放不下就**整包不發**並印出實際需要幾 bytes。**寧可不發，也絕不發半截 JSON。**

**數字**（Task 1 會逐項驗算）：`STATUS_BUF_SIZE = 3072`，`MQTT_BUFFER_SIZE = 3328`，基礎欄位上界 512 bytes，單筆 slave 上界 96 bytes。`(3072 - 1 - 512 - 11) / 96 = 26 ≥ 20`。實測預估 20 台約 1950 bytes，餘裕約 1100 bytes。

### 決定 5：LR 兩端同步 —— 非阻塞三段握手，且安全性不依賴握手成功

**先講最重要的一點**：規格說「兩端不同步時會完全失聯」，那是**純 LR**（`WIFI_PROTOCOL_LR` 單獨使用）的情況。本階段**兩端都採用混合 bitmap** `11b|11g|11n|(LR?)`：
- master 一定得混合（規格「難點 2」：純 LR 連不上一般 AP，MQTT 整個斷）
- **slave 也跟著混合**（規格沒明說，本計畫決定如此）

混合之下，LR 只是**多一種可用速率**，不是換一套不相容的調變。因此切換過程中兩端短暫不同步，代價是「這段期間沒有 LR 的距離增益」，**不是「互相收不到」**。這讓「逾時仍然照樣切換」變成安全的行為，不必為了等一台離線的 slave 而把整個系統卡住。

**那為什麼還要握手**：
1. master 需要**知道**每台 slave 是否已套用，才能在狀態 JSON 與序列埠上如實回報（Phase 5 實測時要靠這個判斷「這次測距是不是真的兩端都開了 LR」）
2. 若 Phase 5 實測後決定改用**純 LR** 或 `esp_now_set_peer_rate_config()` 強制 `WIFI_PHY_RATE_LORA_250K`，安全性就會真的依賴握手。**現在把握手做好，Phase 5 才有東西可以收緊。**

**流程（全程非阻塞，跑在 `loop()` 的狀態機裡）**：
```
LR:ON / LR:OFF 進來
  ├─ 目標值與現值相同 → 直接回報狀態，不進握手
  ├─ slaveCount == 0   → 直接套用（沒有人要同步）
  └─ 否則進 LR_ANNOUNCING：
       每 100ms 對「還沒 ACK 且 online」的 slave 送一封 HO_PKT_LR_SET（一次一台，錯開）
       slave 收到 → 存 EEPROM → 套用 esp_wifi_set_protocol() → 回 HO_PKT_LR_ACK
       master 收 ACK → lrAckMask 對應 bit 設起來（在 ESP-NOW callback 裡只做這件事）
       ├─ 全部 online 的都 ACK 了 → 套用自己 → LR_IDLE
       └─ 逾時 10 秒 → 印出哪幾台沒 ACK → 仍然套用自己 → LR_IDLE
```
**離線 slave 的自癒路徑**：心跳 payload 本來就有 `longRange` 欄位。slave 回線後收到心跳，發現 `hb.longRange` 與自己存的不同就直接套用並存檔。所以「逾時仍套用」不會留下永久不一致。

**時間預算**：10 秒逾時 + 每 100ms 一台，遠低於 30 秒門檻，且整段期間 `loop()` 照常跑、心跳照常發。

**取捨與風險**：`esp_wifi_set_protocol()` 在已關聯 AP 的狀態下呼叫，**可能導致一次 WiFi 斷線重連**（IDF 行為未在本專案驗證過）。緩解：套用後立刻 `onWifiChannelMayHaveChanged()`，並讓 `loop()` 既有的 WiFi 重連機制接手；回歸清單要明確把「LR 切換後 WiFi 是否斷線、幾秒內回來」列為觀察項而非失敗判定。

---

## Task 依賴圖（不得有循環）

```
Task 1（發布地基）
   ├→ Task 2（slaves 陣列）
   ├→ Task 3（代發 slave 狀態）
   │      └→ Task 4（代訂閱與指令轉發，需要 publishSlaveStatus）
   │             └→ Task 5（群組／配對指令，需要 handleMasterCommand 已拆出來）
   └→ Task 6（LR 指令與同步，需要 publishStatus 已能帶 lr 欄位）
Task 7（文件與回歸清單）← 全部
```

**Phase 2a 踩過的坑**：Task 4 引用了 Task 5 才定義的函式，導致無法單獨編譯。本計畫的做法是**先寫最小可運作版本、後續 Task 擴充**，且已刻意把「代發」排在「代訂閱」之前（代訂閱要呼叫代發，反過來不成立）。**每個 Task 結束時都必須能單獨編譯通過。**

---

## Task 1：MQTT 發布地基與 Phase 2a 殘留修正

把「靜默截斷」這個類別的缺陷從根上消掉，並順手清掉 Phase 2a 交付時列出的殘留項。**本 Task 還不加 `slaves` 陣列。**

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Phase 2a 的 `mqttClient`、`publishStatus()`、`quickConnectToIndex()`、`quickConnectCustom()`、`smartConnect()`、`anyResetButtonPressed()`
- Produces（Task 2~6 全部依賴）：
  - `bool publishJsonDoc(const char* topic, JsonDocument& doc, bool retain)`
  - `const char* currentServerName()`
  - 常數：`STATUS_BUF_SIZE`、`MQTT_BUFFER_SIZE`、`STATUS_BASE_MAX_BYTES`、`SLAVE_ENTRY_MAX_BYTES`
  - 全域：`static char statusBuf[STATUS_BUF_SIZE]`、`bool usingCustomServer`

---

- [ ] **Step 1: 全面把 `StaticJsonDocument<N>` 換成 `JsonDocument`**

檔案裡目前有四處（`publishStatus()`、`quickConnectToIndex()` 的 will、`quickConnectCustom()` 的 will、BLE `onWrite()` 的解析）。

理由寫進註釋（放在檔案上方 include 區塊之後）：

```cpp
// ── ArduinoJson 版本注意事項 ──
// 本專案安裝的是 ArduinoJson 7.4.3。這個版本的 StaticJsonDocument<N> 只是
// compatibility.hpp 提供的相容殼（class StaticJsonDocument : public JsonDocument），
// 模板參數 N 完全被忽略、底層一律動態配置，且已標記 ARDUINOJSON_DEPRECATED。
// 也就是說「把 N 從 512 改成 2048」對容量沒有任何作用 ——
// 真正會截斷的地方一直是 serializeJson(doc, buf) 的那個固定大小 char buf。
// 因此全檔改用 JsonDocument，容量控制一律交給 publishJsonDoc() 的 measureJson() 實測。
// CLAUDE.md 記載的「用 StaticJsonDocument 避免記憶體碎片」在 7.x 已不成立。
```

同時把 `createNestedObject("x")` 改成 7.x 的寫法 `doc["x"].to<JsonObject>()`（`createNestedObject` 在 7.x 也是 deprecated 殼）。

`publishStatus()` 裡那段講 `doc.overflowed()` 的長註釋與該檢查**整段刪除** —— 它量的是記憶體配置失敗而非容量，留著只會誤導；真正的檢查由 Step 3 的 `publishJsonDoc()` 負責。

- [ ] **Step 2: 加入容量常數與靜態緩衝區**

放在 MQTT 全域區塊（`WiFiClient espClient;` 附近）：

```cpp
// ── 狀態 JSON 的容量預算 ──
// 血淚背景（Phase 2a 最終審查的結論）：
//   serializeJson(doc, buf) 寫進固定大小的 char buf 時會「靜默截斷」——
//   截斷後的長度仍然小於 mqttClient 的 buffer，publish() 照樣回傳 true，
//   於是 broker 收到語法殘缺的 JSON、App 解析失敗，而序列埠上什麼異常都看不到。
//   Phase 2a 的 master 狀態最壞 317 bytes，對 char buf[512] 只剩約 195 bytes 餘裕，
//   而 Phase 2b 要加的 20 台 slaves 陣列需要 1900+ bytes，必定超過。
//
// 所以本階段的容量控制是三層，且第一層在編譯期：
//   1. static_assert：statusBuf 扣掉基礎欄位上界後，必須放得下 20 台的陣列。
//      有人把 STATUS_BUF_SIZE 改小 → 編譯失敗，而不是上線後靜默截斷。
//   2. 執行期 maxEntries 上限 + slaves_truncated 標記（照現在的數值永遠走不到，
//      存在的意義是「萬一走到，App 與序列埠都看得見」）。
//   3. publishJsonDoc() 發布前 measureJson()，放不下就整包不發並印出實際需求。
//      寧可不發，也絕不發半截 JSON。

// 單筆 slave 條目的位元組上界。最壞情況實算：
//   {"id":"hoban-aabbccddeeff","relay":1,"online":false,"rssi":-100,"version":"255.255.255"},
//   = 89 bytes（含尾端逗號）。取 96 留餘裕，並讓除法算式是整數。
const size_t SLAVE_ENTRY_MAX_BYTES = 96;

// slaves 陣列以外所有欄位的位元組上界。實算：
//   Phase 2a 的既有欄位最壞 317
//   + "server":"<最長 63 字元的自訂伺服器>",  = 75
//   + "free_heap":123456,                     = 19
//   + "slaves_truncated":true,"slaves_shown":20, = 42
//   + "long_range_pending":true,               = 27（Task 6 加）
//   ≈ 480，取 512。
const size_t STATUS_BASE_MAX_BYTES = 512;

// "slaves":[] 這個 key 與中括號本身
const size_t SLAVES_KEY_OVERHEAD = 11;

const size_t STATUS_BUF_SIZE = 3072;

// PubSubClient 的 buffer 要放得下「固定標頭(最多 5) + topic 長度欄位(2) + topic + payload」。
// topic 最長是 "hoban/hoban-a0b1c2d3e4f5/status" = 32 bytes。
// 3072 + 5 + 2 + 32 = 3111，取 3328 留餘裕。
const size_t MQTT_BUFFER_SIZE = 3328;

// 編譯期保證：statusBuf 一定放得下 HO_ESPNOW_MAX_SLAVES 台的完整陣列。
static_assert(
    (STATUS_BUF_SIZE - 1 - STATUS_BASE_MAX_BYTES - SLAVES_KEY_OVERHEAD)
        / SLAVE_ENTRY_MAX_BYTES >= HO_ESPNOW_MAX_SLAVES,
    "STATUS_BUF_SIZE 放不下 HO_ESPNOW_MAX_SLAVES 台 slave 的陣列，"
    "請放大 STATUS_BUF_SIZE 或縮減 STATUS_BASE_MAX_BYTES");

// 序列化用的共用緩衝區。刻意放在檔案層級（.bss）而非函式內的區域變數：
// loopTask 的堆疊只有 8192 bytes，在裡面開 3072 bytes 的區域陣列，
// 加上 PubSubClient 與 lwIP 的呼叫深度，堆疊溢位風險太高。
// 只在 loop() context（含 mqttClient.loop() 內被呼叫的 mqttCallback）使用，
// 單一 task、不重入，共用一份是安全的。
static char statusBuf[STATUS_BUF_SIZE];
```

- [ ] **Step 3: 寫唯一的發布出口 `publishJsonDoc()`**

放在 `publishStatus()` 之前：

```cpp
// 全專案唯一的 MQTT JSON 發布出口。Task 2~6 新增的每一種發布都必須走這裡，
// 不得自己另外開 char buf 呼叫 serializeJson()。
bool publishJsonDoc(const char* topic, JsonDocument& doc, bool retain) {
  if (!mqttClient.connected()) return false;

  // 防線 1：發布前先量。放不下就整包放棄，絕不送出被截斷的半截 JSON。
  size_t needed = measureJson(doc);
  if (needed + 1 > sizeof(statusBuf)) {
    Serial.printf("⚠ [MQTT] 放棄發布 %s：JSON 需要 %u bytes，statusBuf 只有 %u\n",
                  topic, (unsigned)needed, (unsigned)sizeof(statusBuf));
    return false;
  }

  // 防線 2：mqttClient 的 buffer 要放得下整個 PUBLISH 封包
  size_t frameNeeded = 5 + 2 + strlen(topic) + needed;
  if (frameNeeded > mqttClient.getBufferSize()) {
    Serial.printf("⚠ [MQTT] 放棄發布 %s：整包需要 %u bytes，mqtt buffer 只有 %u\n",
                  topic, (unsigned)frameNeeded, (unsigned)mqttClient.getBufferSize());
    return false;
  }

  size_t n = serializeJson(doc, statusBuf, sizeof(statusBuf));

  // 防線 3：Phase 2a 就有的截斷偵測，保留當最後一道保險。
  // 理論上防線 1 已經擋掉，但這道成本是零，而它擋的是「靜默送出壞資料」。
  if (n >= sizeof(statusBuf) - 1) {
    Serial.printf("⚠ [MQTT] 序列化填滿 statusBuf[%u]，放棄發布 %s\n",
                  (unsigned)sizeof(statusBuf), topic);
    return false;
  }

  bool res = mqttClient.publish(topic, (const uint8_t*)statusBuf, n, retain);
  if (!res) {
    Serial.printf("[MQTT] 發布失敗 %s（長度 %u，buffer %u）\n",
                  topic, (unsigned)n, (unsigned)mqttClient.getBufferSize());
  }
  return res;
}
```

- [ ] **Step 4: `setBufferSize()` 改用新常數並檢查回傳值**

`quickConnectToIndex()` 與 `quickConnectCustom()` 兩處都要改。**`setBufferSize()` 會 realloc，heap 不足時回傳 false**，Phase 2a 完全沒檢查 —— 失敗時 buffer 停在舊大小，之後所有大狀態發布都會失敗。

```cpp
  // Phase 2b：buffer 由 1024 擴到 MQTT_BUFFER_SIZE，才放得下 20 台的 slaves 陣列。
  // setBufferSize() 內部是 realloc，heap 不足會回 false 而 buffer 停在舊大小，
  // 之後每一次大狀態發布都會靜默失敗，所以一定要檢查。
  if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
    Serial.printf("⚠ [MQTT] setBufferSize(%u) 失敗，buffer 仍為 %u，"
                  "帶 slaves 陣列的狀態將無法發布\n",
                  (unsigned)MQTT_BUFFER_SIZE, (unsigned)mqttClient.getBufferSize());
  }
  mqttClient.setSocketTimeout(3);
```

同時把兩處的 will 從 `StaticJsonDocument<160>` + `char willBuf[160]` 改成 `JsonDocument`，但 will **不能**走 `publishJsonDoc()`（它是 connect 參數不是 publish），保留獨立的 `char willBuf[192]` 並加上長度檢查：

```cpp
  JsonDocument willDoc;
  willDoc["device_id"] = deviceId;
  willDoc["status"] = "offline";
  willDoc["server"] = cfg.server;
  willDoc["timestamp"] = millis() / 1000;
  char willBuf[192];
  size_t willLen = serializeJson(willDoc, willBuf, sizeof(willBuf));
  if (willLen >= sizeof(willBuf) - 1) {
    Serial.println("⚠ [MQTT] LWT JSON 被截斷，改用最小 will");
    snprintf(willBuf, sizeof(willBuf), "{\"device_id\":\"%s\",\"status\":\"offline\"}", deviceId);
  }
```

- [ ] **Step 5: 記錄目前連上哪一台伺服器（狀態 JSON 的 `server` 欄位）**

規格的 master 狀態範例有 `"server": "mqttgo.io"`，Phase 2a 漏掉了。加全域：

```cpp
// 目前連上的是自訂伺服器還是 DEFAULT_SERVERS[currentServerIndex]。
// currentServerIndex 刻意不追蹤自訂伺服器（見 quickConnectCustom() 註釋），
// 所以要另外一個旗標才知道 server 欄位該填哪個名字。
bool usingCustomServer = false;

const char* currentServerName() {
  if (usingCustomServer) return mqttServer;
  if (currentServerIndex >= 0 && currentServerIndex < DEFAULT_SERVER_COUNT) {
    return DEFAULT_SERVERS[currentServerIndex].server;
  }
  return "";
}
```

`quickConnectToIndex()` 成功時 `usingCustomServer = false;`，`quickConnectCustom()` 成功時 `usingCustomServer = true;`。

- [ ] **Step 6: 重寫 `publishStatus()` 走新出口，並補上 `server` / `free_heap`**

```cpp
// 發布 master 自身狀態。Task 2 會在這裡加上 slaves 陣列。
void publishStatus() {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String topic = String("hoban/") + deviceId + "/status";

  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["status"] = "online";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["server"] = currentServerName();     // Phase 2a 漏掉，規格範例有這個欄位
  doc["timestamp"] = millis() / 1000;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = WiFi.isConnected();
  wifi["ssid"] = ssid;
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["relay"] = relayState ? 1 : 0;
  dev["has_relay"] = hasRelay;
  dev["pairing"] = pairingMode;
  dev["slave_count"] = (int)slaveCount;
  dev["channel"] = currentChannel;
  dev["long_range"] = longRangeEnabled;
  dev["free_heap"] = (uint32_t)ESP.getFreeHeap();

  publishJsonDoc(topic.c_str(), doc, true);
}
```

- [ ] **Step 7: 清掉 Phase 2a 交付的兩項 MQTT 殘留**

**(a) `FIND_BEST_SERVER` 沒被 10 秒節流覆蓋**（progress.md 列為 Phase 2b 待辦，Critical 1 的角落）。
`lastReconnect` 目前是 `loop()` 內的 `static`，`mqttCallback()` 改不到它。改成檔案層級變數：

```cpp
// 由 loop() 與 mqttCallback() 的 FIND_BEST_SERVER 共用。
// 原本是 loop() 內的 static，callback 無法更新它，導致 FIND_BEST_SERVER 觸發的
// smartConnect()（最壞 18 秒阻塞）之後，loop() 仍以為距離上次重連已滿 10 秒，
// 立刻再阻塞一次 —— 兩次背靠背 36 秒 > 30 秒門檻。
unsigned long lastMqttReconnectAt = 0;
```
`loop()` 的 `lastReconnect` 全部改用它；`mqttCallback()` 的 `FIND_BEST_SERVER` 分支在 `smartConnect()` 之後補 `lastMqttReconnectAt = millis();`。

**(b) `smartConnect()` 補上按鈕逃生口**（progress.md：「長按流程可能被 smartConnect 凍結，Phase 2b 可補對稱的 `anyResetButtonPressed()` 早退」）。
`mqttClient.connect()` 本身不可中斷，能做的只有**進去之前先看一眼**：

```cpp
void smartConnect() {
  if (!WiFi.isConnected()) return;

  // 按鈕正被按住時不要開始一次最壞 18 秒的不可中斷連線。
  // connectToWiFi() 的等待迴圈有同樣的逃生口，這裡補上讓兩條路徑對稱。
  // 這只縮小視窗、不能完全消除：已經進到 connect() 裡面就叫不回來了。
  if (anyResetButtonPressed()) {
    Serial.println("[MQTT] 偵測到按鈕按住，本輪跳過連線嘗試");
    return;
  }
  ...（其餘不變）
}
```

- [ ] **Step 8: 編譯驗證**

Run：
```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
```
Expected：三者皆 exit code 0。

**必須在 report 記錄**：三種型號的 flash 位元組數與百分比、RAM 百分比，並與 Phase 2a 收尾的基準比較（WROOM 1,683,315 / 82.7%）。

- [ ] **Step 9: Commit**

```bash
git add ho_master1/ho_master1.ino
git commit -m "$(cat <<'EOF'
Master MQTT 發布改走統一出口，容量在編譯期就保證放得下 20 台

Phase 2a 最終審查的結論：狀態 JSON 真正的邊界是 serializeJson() 寫入的
char buf，不是 mqttClient.setBufferSize()。超過 buf 時 ArduinoJson 是截斷，
截斷後長度仍小於 mqtt buffer，publish() 照樣回 true，於是 broker 收到語法
殘缺的 JSON 而序列埠上完全看不到異常。

本次改為：
- 全檔 StaticJsonDocument<N> 改成 JsonDocument。ArduinoJson 7.4.3 的 N 完全
  被忽略（相容殼、底層動態配置），放大 N 對容量沒有任何作用
- 新增 publishJsonDoc()：發布前先 measureJson()，放不下就整包不發並印出實際
  需求，寧可不發也絕不發半截
- 容量常數搭配 static_assert，日後有人把 statusBuf 改小會直接編譯失敗
- mqtt buffer 由 1024 擴到 3328，且檢查 setBufferSize() 的回傳值
  （realloc 失敗時 buffer 會停在舊大小，之後大狀態全部靜默失敗）

順手清掉 Phase 2a 交付時列出的兩項殘留：
- FIND_BEST_SERVER 未被 10 秒節流覆蓋（lastReconnect 移出 loop() 的 static）
- smartConnect() 補上 anyResetButtonPressed() 早退，與 connectToWiFi() 對稱

另補上規格有、Phase 2a 漏掉的 server 欄位與 free_heap。

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2：master 狀態加上 `slaves` 陣列

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 1 的 `publishJsonDoc()`、容量常數；Phase 1 的 `slaves[]`/`slaveCount`/`findSlave()`
- Produces（Task 3、4 依賴）：
  - `SlaveEntry` 新欄位：`uint8_t relay`、`uint8_t fwMajor/fwMinor/fwPatch`、`bool dirty`
  - `void appendSlavesArray(JsonDocument& doc)`
  - `void formatSlaveVersion(int idx, char* out, size_t outSize)`

---

- [ ] **Step 1: 擴充 `SlaveEntry`**

```cpp
struct SlaveEntry {
  uint8_t mac[6];
  bool online;
  int8_t rssi;
  unsigned long lastSeen;
  // ── Phase 2b 新增：代發 slave 狀態需要的欄位 ──
  // 這些值由 onEspNowRecv() 收到 HO_PKT_STATE 時填入（WiFi task context），
  // 由 loop() 讀取來組 JSON。跨 context 但都是單一 byte 的存取，
  // 讀到新舊混合的一組值最多讓某一輪代發的資料稍舊，下一輪就會更正，
  // 不會造成錯誤動作，因此沿用 Phase 1 對 slaves[] 的既有做法不加鎖。
  uint8_t relay;      // 0 / 1
  uint8_t fwMajor;
  uint8_t fwMinor;
  uint8_t fwPatch;
  // 有變化就代發，不必等輪播輪到它（見 Task 3 的排程器）
  bool dirty;
};
```

`loadSlaves()` 的初始化迴圈補上 `relay = 0; fwMajor = fwMinor = fwPatch = 0; dirty = false;`，
`addSlave()` 同樣補上（新加入的直接 `dirty = true`，讓它立刻被代發一次）。

- [ ] **Step 2: `onEspNowRecv()` 的 `HO_PKT_STATE` 分支記錄新欄位並設 dirty**

取代現有那段：

```cpp
    HoStatePayload st;
    memcpy(&st, payload, sizeof(st));

    // 只有「內容真的變了」才設 dirty，避免每 15 秒的例行輪詢回報都觸發一次
    // 額外代發（例行輪播本來就會發，重複發是浪費頻寬）。
    bool changed = (!slaves[idx].online) ||
                   (slaves[idx].relay != st.relay) ||
                   (slaves[idx].fwMajor != st.fwMajor) ||
                   (slaves[idx].fwMinor != st.fwMinor) ||
                   (slaves[idx].fwPatch != st.fwPatch);

    slaves[idx].online = true;
    slaves[idx].rssi = info->rx_ctrl->rssi;
    slaves[idx].lastSeen = millis();
    slaves[idx].relay = st.relay;
    slaves[idx].fwMajor = st.fwMajor;
    slaves[idx].fwMinor = st.fwMinor;
    slaves[idx].fwPatch = st.fwPatch;
    // 這裡只設旗標，絕不在此發 MQTT ——
    // 本函式跑在 WiFi task，不是 loop() context，在這裡呼叫 PubSubClient
    // 等於從另一個 task 動同一個 socket，是明確的競態。
    if (changed) slaves[idx].dirty = true;
```

- [ ] **Step 3: `updateSlaveOnlineStatus()` 在翻轉時設 dirty**

離線與回線兩個方向都要（回線的那次會由 `HO_PKT_STATE` 一併設，但 `online` 也可能是被這裡改回來的，兩邊都設最保險）：

```cpp
    if (wasOnline != isOnline) {
      char id[20];
      hoFormatDeviceId(slaves[i].mac, id);
      Serial.printf("[%s] %s（超過 %lu 秒沒回應即判離線）\n",
                    isOnline ? "上線" : "離線", id, SLAVE_OFFLINE_TIMEOUT / 1000);
      // 上下線翻轉一定要立刻代發，否則 App 最壞要等一整輪輪播才看到
      slaves[i].dirty = true;
    }
    slaves[i].online = isOnline;
```

> **注意**：這行 `Serial.printf` 的字串改了（原本只有離線方向、格式不同）。
> Task 7 寫回歸清單時**必須以這裡的最終字串為準**，不得沿用舊格式。

- [ ] **Step 4: 版本字串格式化**

```cpp
// slave 版本號 → "1.0.0"。out 至少 16 bytes。
// 尚未回報過狀態的 slave（fwMajor/Minor/Patch 都是 0）填 "0.0.0"，
// 不留空字串 —— App 端的解析比較單純，寧可給一個明確的無效值。
void formatSlaveVersion(int idx, char* out, size_t outSize) {
  snprintf(out, outSize, "%u.%u.%u",
           slaves[idx].fwMajor, slaves[idx].fwMinor, slaves[idx].fwPatch);
}
```

- [ ] **Step 5: 寫 `appendSlavesArray()`**

```cpp
// 把 slaves 陣列加進 master 的狀態 doc。
// 條目數量以 Task 1 的容量常數推算的上界為準；照現行數值（3072/512/96）
// maxEntries = 26 ≥ HO_ESPNOW_MAX_SLAVES = 20，這條截斷路徑永遠走不到，
// 且已有 static_assert 在編譯期擋住「有人把 statusBuf 改小」。
// 保留執行期截斷的意義是：萬一真的走到，App 看得到 slaves_truncated、
// 序列埠也會告警，而不是靜默給出一份不完整的清單。
void appendSlavesArray(JsonDocument& doc) {
  const int maxEntries =
      (int)((STATUS_BUF_SIZE - 1 - STATUS_BASE_MAX_BYTES - SLAVES_KEY_OVERHEAD)
            / SLAVE_ENTRY_MAX_BYTES);

  int shown = slaveCount;
  if (shown > maxEntries) shown = maxEntries;

  JsonArray arr = doc["slaves"].to<JsonArray>();
  for (int i = 0; i < shown; i++) {
    char id[20];
    hoFormatDeviceId(slaves[i].mac, id);
    char ver[16];
    formatSlaveVersion(i, ver, sizeof(ver));

    JsonObject o = arr.add<JsonObject>();
    // id 與 ver 都是區域 char[]（非 const char*），ArduinoJson 會複製一份進 doc，
    // 離開本次迴圈後仍然有效。若改成 const char* 會只存指標而變成懸空指標。
    o["id"] = id;
    o["relay"] = slaves[i].relay ? 1 : 0;
    o["online"] = slaves[i].online;
    o["rssi"] = slaves[i].rssi;
    o["version"] = ver;
  }

  if (shown < slaveCount) {
    doc["slaves_truncated"] = true;
    doc["slaves_shown"] = shown;
    Serial.printf("⚠ [MQTT] slaves 陣列被截斷：名冊 %d 台，只放得下 %d 台\n",
                  slaveCount, shown);
  }
}
```

在 `publishStatus()` 的 `dev` 區塊之後、`publishJsonDoc()` 之前呼叫 `appendSlavesArray(doc);`。

- [ ] **Step 6: 加一個能實測容量的序列埠指令**

**這一步不可省略** —— 沒有它就無法在只有 1~2 台實體 slave 的情況下驗證「20 台放得下」這個核心宣稱。

```cpp
// 測試用：把名冊灌成 n 台假 slave，用來實測 20 台時狀態 JSON 的真實大小。
// 刻意「不」呼叫 saveSlaves()、也不註冊 peer —— 這是純記憶體內的假資料，
// 重開機即消失，不會污染 NVS 名冊、也不會對不存在的 MAC 送出封包。
// 只開放序列埠，不接到 MQTT：這是開發驗證工具，不是產品功能。
void fakeSlavesForCapacityTest(int n) {
  if (n < 0) n = 0;
  if (n > HO_ESPNOW_MAX_SLAVES) n = HO_ESPNOW_MAX_SLAVES;
  for (int i = 0; i < n; i++) {
    slaves[i].mac[0] = 0xAA; slaves[i].mac[1] = 0xBB; slaves[i].mac[2] = 0xCC;
    slaves[i].mac[3] = 0xDD; slaves[i].mac[4] = 0xEE; slaves[i].mac[5] = (uint8_t)i;
    slaves[i].online = false;      // false 比 true 多 1 byte，取最壞
    slaves[i].rssi = -100;         // 3 位數負值，取最壞
    slaves[i].lastSeen = 0;
    slaves[i].relay = 1;
    slaves[i].fwMajor = 255; slaves[i].fwMinor = 255; slaves[i].fwPatch = 255;
    slaves[i].dirty = false;
  }
  slaveCount = n;
  Serial.printf("[測試] 名冊已灌成 %d 台假 slave（未寫入 NVS，重開機即消失）\n", n);
}

// 印出目前狀態 JSON 的實際大小，與各層預算比對
void printStatusJsonSize() {
  JsonDocument doc;
  buildStatusDoc(doc);          // 見下方說明
  size_t n = measureJson(doc);
  Serial.printf("[測試] 狀態 JSON 實際 %u bytes／statusBuf %u／mqtt buffer %u（名冊 %d 台）\n",
                (unsigned)n, (unsigned)sizeof(statusBuf),
                (unsigned)mqttClient.getBufferSize(), slaveCount);
}
```

為此把 `publishStatus()` 拆成兩段，`buildStatusDoc()` 負責組 doc、`publishStatus()` 負責發：

```cpp
void buildStatusDoc(JsonDocument& doc) { /* Task 1 Step 6 的內容 + appendSlavesArray */ }

void publishStatus() {
  if (!mqttClient.connected()) return;
  String topic = String("hoban/") + getDeviceId() + "/status";
  JsonDocument doc;
  buildStatusDoc(doc);
  publishJsonDoc(topic.c_str(), doc, true);
}
```

`handleSerialCommand()` 加兩個 verb（`fakeslaves` 要走 `parseIndexArg()` 驗證）：
```cpp
  } else if (verb == "fakeslaves") {
    fakeSlavesForCapacityTest(arg);
  } else if (verb == "jsonsize") {
    printStatusJsonSize();
```
`needsArg` 的清單要加上 `fakeslaves`；`printHelp()` 加上兩行說明。

- [ ] **Step 7: 編譯驗證**

三種型號皆 exit 0，記錄 flash／RAM 變化。

- [ ] **Step 8: Commit**

commit 訊息要包含：`slaves` 陣列的欄位對照規格、單筆 96 bytes 上界的實算、`fakeslaves`/`jsonsize` 是測試工具且不寫 NVS。

---

## Task 3：代發 slave 狀態（錯開輪播 + dirty 立刻補發）

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 1 的 `publishJsonDoc()`、Task 2 的 `SlaveEntry` 新欄位與 `formatSlaveVersion()`
- Produces（Task 4、5 依賴）：
  - `void publishSlaveStatus(int idx)`
  - `void publishSlaveOffline(int idx)`
  - `void slaveStatusScheduler()`
  - `void markAllSlavesDirty()`

---

- [ ] **Step 1: 寫 `publishSlaveStatus()`**

**欄位逐字照規格「Master 代發的 Slave 狀態」章節**：

```cpp
// 用 slave 的 MAC 代發它的狀態，讓它在 App 眼裡就是一台普通設備。
//
// wifi 區塊的內容是刻意這樣填的（規格明訂）：App 現有的
// Device.updateFromMqttMessage()（hoctrl/lib/models/device.dart:117-142）
// 不用改就能解析，rssi 借來顯示 ESP-NOW 訊號強度。
// via 是新欄位，標示這台是誰代發的。
//
// 與規格範例的唯一刻意差異：規格把 wifi.connected 寫死成 true，
// 這裡改成跟著 slaves[idx].online。理由是 App 可能拿 wifi.connected 判斷上下線，
// 寫死 true 會讓離線的 slave 在 App 上永遠顯示在線 —— 那正是規格自己
// 「Slave 失聯時 master 要代發 offline」想避免的情況。
void publishSlaveStatus(int idx) {
  if (!mqttClient.connected()) return;
  if (idx < 0 || idx >= slaveCount) return;

  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);
  char ver[16];
  formatSlaveVersion(idx, ver, sizeof(ver));

  String topic = String("hoban/") + id + "/status";

  JsonDocument doc;
  doc["device_id"] = id;
  doc["status"] = slaves[idx].online ? "online" : "offline";
  doc["version"] = ver;
  doc["model"] = "hoSlave1";
  doc["via"] = getDeviceId();
  doc["timestamp"] = millis() / 1000;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = slaves[idx].online;
  wifi["ssid"] = "ESP-NOW";
  wifi["rssi"] = slaves[idx].rssi;
  wifi["ip"] = "N/A";

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["relay"] = slaves[idx].relay ? 1 : 0;

  if (publishJsonDoc(topic.c_str(), doc, true)) {
    slaves[idx].dirty = false;
  }
}
```

> `dirty` 只在**發布成功**時才清。發失敗（例如剛好斷線）就留著，下次補發。

- [ ] **Step 2: 寫排程器**

```cpp
// 代發 slave 狀態的排程器。設計原則：每次呼叫最多發一台。
//
// 為什麼不 20 台一次全發：mqttClient.publish() 在連線正常時很快，但 socket
// 卡住時會拖到 setSocketTimeout(3)。21 個 topic 背靠背最壞 63 秒，直接撞破
// slave 的 30 秒失聯門檻＝籠子被打開。錯開之後每次 loop() 只有一次 publish()，
// 最壞單次 3 秒，兩次之間都有完整的 loop() 週期可發心跳。
//
// 為什麼一輪是 15 秒：slave 的資料本身由 pollNextSlave() 以 15 秒一輪更新，
// 代發比資料更新還快是純浪費頻寬。兩者對齊後每次代發都帶到剛更新過的資料。
const unsigned long SLAVE_STATUS_CYCLE_MS = 15000;
const unsigned long SLAVE_STATUS_MIN_GAP_MS = 250;

void slaveStatusScheduler() {
  if (slaveCount == 0) return;
  if (!mqttClient.connected()) return;

  static unsigned long lastPubAt = 0;
  static int rotateIdx = 0;

  unsigned long now = millis();

  // 任何一次發布之間至少隔 SLAVE_STATUS_MIN_GAP_MS，
  // 避免 20 台同時翻轉時連續 20 個 loop() 各發一台把頻寬吃滿
  if (now - lastPubAt < SLAVE_STATUS_MIN_GAP_MS) return;

  // 優先處理有變化的：從 rotateIdx 開始找，確保多台同時 dirty 時能公平輪到
  for (int k = 0; k < slaveCount; k++) {
    int i = (rotateIdx + k) % slaveCount;
    if (slaves[i].dirty) {
      lastPubAt = now;
      publishSlaveStatus(i);
      return;
    }
  }

  // 沒有變化就走例行輪播
  unsigned long interval = SLAVE_STATUS_CYCLE_MS / (unsigned long)slaveCount;
  if (interval < SLAVE_STATUS_MIN_GAP_MS) interval = SLAVE_STATUS_MIN_GAP_MS;
  if (now - lastPubAt < interval) return;

  lastPubAt = now;
  if (rotateIdx >= slaveCount) rotateIdx = 0;
  publishSlaveStatus(rotateIdx);
  rotateIdx = (rotateIdx + 1) % slaveCount;
}

// SLAVES 指令與剛連上 broker 時用：讓整份名冊在接下來一輪內全部重發一次，
// 而不是當場連發 20 個 topic
void markAllSlavesDirty() {
  for (int i = 0; i < slaveCount; i++) slaves[i].dirty = true;
}
```

- [ ] **Step 3: 解除配對時的最後一則 offline**

```cpp
// 解除配對前，先把一則 status=offline 的保留訊息壓上去。
// 少了這步，broker 上會永遠留著最後那則 "online" 的保留訊息，
// App 上就多出一台永遠在線、卻怎麼控制都沒反應的幽靈設備。
// 這裡直接組 doc 而不呼叫 publishSlaveStatus()，因為呼叫時 slave 可能
// 還在名冊上（online=true），要強制發 offline。
void publishSlaveOffline(int idx) {
  if (!mqttClient.connected()) return;
  if (idx < 0 || idx >= slaveCount) return;
  slaves[idx].online = false;
  slaves[idx].dirty = false;
  publishSlaveStatus(idx);
}
```

在 `unpairSlave()` 的開頭（陣列搬移之前）呼叫 `publishSlaveOffline(idx);`。
`onEspNowRecv()` 的 `HO_PKT_UNPAIR` 分支（slave 主動解除）同樣要 —— 但那裡是 WiFi task context，**不可直接發 MQTT**。改為設一個待辦：

```cpp
// slave 主動解除配對是在 WiFi task 收到的，不能在這裡發 MQTT。
// 記下 MAC，交給 loop() 處理（見 processPendingUnpairPublish()）。
uint8_t pendingOfflineMac[6];
volatile bool hasPendingOfflinePublish = false;
```
在 `loop()` 呼叫的 `processPendingUnpairPublish()` 裡直接用 MAC 組 topic 發一則 offline（此時名冊已經沒有這台，不能用 idx）。

- [ ] **Step 4: 接進 `loop()`**

在 `pollNextSlave();` 之後加：
```cpp
  // ── 代發 slave 狀態（每次 loop() 最多一台，見 slaveStatusScheduler()）──
  processPendingUnpairPublish();
  slaveStatusScheduler();
```

- [ ] **Step 5: 編譯驗證**

三種型號皆 exit 0，記錄 flash／RAM。

- [ ] **Step 6: Commit**

commit 訊息要說明「為什麼不 21 個 topic 一次全發」與「為什麼一輪是 15 秒」，以及 LWT 只有一個名額的已知限制。

---

## Task 4：代訂閱與指令轉發

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 3 的 `publishSlaveStatus()`；Phase 1 的 `findSlave()`/`sendCmdToSlave()`/`requestSlaveState()`
- Produces（Task 5、6 依賴）：
  - `bool parseControlTopic(const char* topic, uint8_t outMac[6])`
  - `void handleMasterCommand(const String& message)` —— 由 Task 5、6 擴充
  - `void handleSlaveCommand(int idx, const String& message)`
  - `void subscribeAllControlTopics()` / `void subscribeSlaveControlTopic(int idx)` / `void unsubscribeSlaveControlTopic(const uint8_t mac[6])`

---

- [ ] **Step 1: 寫 topic 解析**

```cpp
// 解析 "hoban/hoban-<12 位 hex>/control"，成功則把 MAC 寫進 outMac。
//
// 為什麼不預先產生 21 條 topic 字串再逐條 strcmp：那要 21 × 33 = 693 bytes RAM，
// 而且配對／解除配對時得同步維護，多出一份可能與名冊不一致的狀態。
// 名冊已經是唯一真相，再複製一份就是在製造 bug。
// 解析是 O(1)（純長度檢查 + hex 解析），之後查名冊最多 20 次 6 bytes 的 memcmp
// ＝ 微秒級，而且只在收到 MQTT 訊息時才跑（App 按按鈕才發生，頻率以次／分鐘計）。
static const char CONTROL_TOPIC_PREFIX[] = "hoban/";
static const char CONTROL_TOPIC_SUFFIX[] = "/control";
static const size_t DEVICE_ID_LEN = 18;   // "hoban-" + 12 位 hex

bool parseControlTopic(const char* topic, uint8_t outMac[6]) {
  if (topic == nullptr) return false;
  const size_t prefixLen = sizeof(CONTROL_TOPIC_PREFIX) - 1;   // 6
  const size_t suffixLen = sizeof(CONTROL_TOPIC_SUFFIX) - 1;   // 8
  if (strlen(topic) != prefixLen + DEVICE_ID_LEN + suffixLen) return false;
  if (strncmp(topic, CONTROL_TOPIC_PREFIX, prefixLen) != 0) return false;
  if (strcmp(topic + prefixLen + DEVICE_ID_LEN, CONTROL_TOPIC_SUFFIX) != 0) return false;

  char id[DEVICE_ID_LEN + 1];
  memcpy(id, topic + prefixLen, DEVICE_ID_LEN);
  id[DEVICE_ID_LEN] = '\0';
  return hoParseMacFromDeviceId(id, outMac);   // 這個函式會驗 "hoban-" 前綴與 hex 合法性
}
```

- [ ] **Step 2: 訂閱管理**

```cpp
// 逐台訂閱，不用萬用字元 hoban/+/control。
// 理由：那些 broker 是公用的（emqx.io、hivemq.com、eclipseprojects.io），
// hoban/+/control 會收到全世界所有 hoban 設備的控制訊息 —— 我們雖然會過濾掉，
// 但流量與被動接收他人指令的風險完全不必要。
void subscribeSlaveControlTopic(int idx) {
  if (idx < 0 || idx >= slaveCount) return;
  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);
  String t = String("hoban/") + id + "/control";
  if (mqttClient.subscribe(t.c_str())) {
    Serial.printf("[代理] 已訂閱 %s\n", t.c_str());
  } else {
    Serial.printf("⚠ [代理] 訂閱失敗 %s\n", t.c_str());
  }
}

void unsubscribeSlaveControlTopic(const uint8_t mac[6]) {
  if (!mqttClient.connected()) return;
  char id[20];
  hoFormatDeviceId(mac, id);
  String t = String("hoban/") + id + "/control";
  mqttClient.unsubscribe(t.c_str());
  Serial.printf("[代理] 已取消訂閱 %s\n", t.c_str());
}

// 剛連上 broker 時把 master 自己與名冊上每一台的 control topic 一次訂完。
// 20 台等於 21 次 subscribe，每次只是一小段 TCP 寫入；為保險起見迴圈內
// 仍呼叫 maintainEspNow()，讓 socket 萬一變慢時心跳照常發出。
void subscribeAllControlTopics() {
  String own = String("hoban/") + getDeviceId() + "/control";
  mqttClient.subscribe(own.c_str());
  Serial.printf("[MQTT] 已訂閱 %s\n", own.c_str());

  for (int i = 0; i < slaveCount; i++) {
    subscribeSlaveControlTopic(i);
    maintainEspNow();
  }
}
```

`quickConnectToIndex()` 與 `quickConnectCustom()` 裡原本那兩行「組 controlTopic + subscribe」都換成 `subscribeAllControlTopics();`，並在其後把 `publishStatus();` 保留、再加 `markAllSlavesDirty();`（讓所有 slave 的代發在接下來一輪內重新壓一次保留訊息）。

`addSlave()` 成功後補：
```cpp
  // 規格：配對完成要立刻代發一次該 slave 的 status，App 才會看到新設備。
  // 這裡在 ESP-NOW callback context，只能設 dirty 讓 loop() 的排程器去發。
  if (mqttClient.connected()) subscribeSlaveControlTopic(slaveCount - 1);
```
> **注意**：`addSlave()` 由 `onEspNowRecv()` 呼叫，屬 WiFi task context。
> `mqttClient.subscribe()` 會動 socket，**與 loop() 的 `mqttClient.loop()` 競態**。
> 因此**不可在此直接訂閱**。改成設一個待辦旗標：
> ```cpp
> volatile bool pendingSubscribeRefresh = false;   // 名冊變動，loop() 要重新對齊訂閱
> ```
> `addSlave()` 只設 `pendingSubscribeRefresh = true;`，`loop()` 看到就呼叫
> `subscribeAllControlTopics()`（重複訂閱同一個 topic 對 broker 是冪等的）。
> **上面 `subscribeSlaveControlTopic(slaveCount - 1)` 那行不要寫進 `addSlave()`**，
> 它只保留給 `loop()` context 的呼叫者用。

- [ ] **Step 3: 把 `mqttCallback()` 拆成三段**

```cpp
// 收到任何 control topic 的訊息。本函式跑在 mqttClient.loop() 內，屬 loop() context，
// 可以安全呼叫阻塞函式，但等待一律走 espNowDelay() 而非裸 delay()。
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  uint8_t mac[6];
  if (!parseControlTopic(topic, mac)) {
    Serial.printf("[MQTT] 無法解析的 topic，忽略: %s\n", topic);
    return;
  }

  uint8_t ownMac[6];
  WiFi.macAddress(ownMac);
  if (memcmp(mac, ownMac, 6) == 0) {
    Serial.printf("[MQTT] 收到指令: %s\n", message.c_str());
    handleMasterCommand(message);
    return;
  }

  int idx = findSlave(mac);
  if (idx < 0) {
    // 解除配對後 broker 上可能還有殘留訊息，或訂閱尚未完全取消
    Serial.printf("[MQTT] 指令的目標不在名冊上，忽略: %s\n", topic);
    return;
  }
  handleSlaveCommand(idx, message);
}
```

`handleMasterCommand()` 就是把 Phase 2a 的 `mqttCallback()` 內容整段搬過來（`status`/`ON`/`OFF`/`reset`/`FIND_BEST_SERVER`/`HASRELAY:*`/未知指令），一行邏輯都不改。Task 5、6 再往裡面加分支。

- [ ] **Step 4: 寫 `handleSlaveCommand()`**

```cpp
// 代收 slave 的 control topic：把 MQTT 純文字指令轉成 ESP-NOW 封包。
//
// ON 為什麼送 HO_CMD_PULSE 而不是 HO_CMD_ON：
// App 對一般 hoRelay 設備送的 ON 語義是「開門」＝點動一次，master 自己的
// ON 分支也是 pulseRelay(2000)。slave 要在 App 眼裡是一台普通設備，
// 語義就必須完全一致，否則「開保險 → 關門 → 關保險」三段鎖流程對 slave
// 的行為會與其他設備不同。持續開啟只保留給序列埠的 on <n> 指令（現場除錯用）。
void handleSlaveCommand(int idx, const String& message) {
  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);
  Serial.printf("[代理] %s 收到指令: %s\n", id, message.c_str());

  if (message == "ON") {
    sendCmdToSlave(idx, HO_CMD_PULSE, 2000);
  } else if (message == "OFF") {
    sendCmdToSlave(idx, HO_CMD_OFF, 0);
  } else if (message == "status") {
    // 先用目前已知的狀態立刻回一則，App 不必空等；
    // 同時向 slave 要一次最新狀態，回來時 dirty 會觸發第二則代發。
    publishSlaveStatus(idx);
    requestSlaveState(idx);
  } else {
    Serial.printf("[代理] %s 不支援的指令: %s\n", id, message.c_str());
  }
}
```

- [ ] **Step 5: 解除配對時取消訂閱**

`unpairSlave()` 內，在 `publishSlaveOffline(idx)` 之後、陣列搬移之前：
```cpp
  unsubscribeSlaveControlTopic(slaves[idx].mac);
```
`processPendingUnpairPublish()`（slave 主動解除的路徑）也要取消訂閱。

- [ ] **Step 6: `loop()` 接上訂閱對齊**

```cpp
  // 名冊在 ESP-NOW callback 裡變動過（配對成功），回到 loop() context 才動 socket
  if (pendingSubscribeRefresh && mqttClient.connected()) {
    pendingSubscribeRefresh = false;
    subscribeAllControlTopics();
  }
```

- [ ] **Step 7: 編譯驗證**

三種型號皆 exit 0，記錄 flash／RAM。

- [ ] **Step 8: Commit**

commit 訊息要說明：topic 比對由「完整字串相等」改為「解析 + 查名冊」的理由、為什麼不用萬用字元、為什麼訂閱動作一律回到 loop() context 才做。

---

## Task 5：群組、配對與名冊指令

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 3 的 `markAllSlavesDirty()`、Task 4 的 `handleMasterCommand()`
- Produces：`handleMasterCommand()` 新增 `ALL:ON`/`ALL:OFF`/`SLAVES`/`PAIR:START`/`PAIR:STOP`/`UNPAIR:<id>`/`UNPAIRALL`

---

- [ ] **Step 1: 消除 `sendCmdToAll()` 與 `unpairSlave()` 的裸 `delay()`**

```cpp
void sendCmdToAll(HoRelayCmd cmd, uint16_t pulseMs) {
  Serial.printf("[控制] 廣播指令 %u 給 %d 台\n", (uint8_t)cmd, slaveCount);
  for (int i = 0; i < slaveCount; i++) {
    sendCmdToSlave(i, cmd, pulseMs);
    // 錯開送出時間，降低同頻碰撞。改用 espNowDelay() 而非裸 delay(20)：
    // 20 台合計 400ms，雖然遠低於 30 秒門檻，但這 400ms 內原本一則心跳都發不出去，
    // 且點動結束檢查（已併入 maintainEspNow()）也會停擺 400ms。
    espNowDelay(20);
  }
  ...（其餘不變）
}
```
`unpairSlave()` 的 `delay(100)` 同樣改成 `espNowDelay(100)`。

- [ ] **Step 2: 加入指令分派（逐字對照規格的指令表）**

在 `handleMasterCommand()` 的 `HASRELAY:*` 之後、`else` 之前插入：

```cpp
  } else if (message == "ALL:ON") {
    // 規格：「廣播 pulse 給所有已配對 slave」——是 PULSE 不是持續 ON，
    // 語義與單台的 ON（pulseRelay(2000)）一致。
    // sendCmdToAll() 內部會連 master 自己那顆一起動作（規格：ALL:ON 連自己一起）。
    sendCmdToAll(HO_CMD_PULSE, 2000);
    markAllSlavesDirty();
    publishStatus();
  } else if (message == "ALL:OFF") {
    sendCmdToAll(HO_CMD_OFF, 0);
    markAllSlavesDirty();
    publishStatus();
  } else if (message == "SLAVES") {
    // 規格：「立刻回報 slave 清單（發一次 status）」。
    // 另外把所有 slave 標記成 dirty，讓每台的代發 topic 在接下來一輪內
    // 也各自重壓一次保留訊息 —— 但不在這裡連發 20 則（見 slaveStatusScheduler()）。
    markAllSlavesDirty();
    publishStatus();
  } else if (message == "PAIR:START") {
    enterPairingMode();
    publishStatus();
  } else if (message == "PAIR:STOP") {
    exitPairingMode();
    publishStatus();
  } else if (message.startsWith("UNPAIR:")) {
    String idStr = message.substring(7);
    idStr.trim();
    uint8_t mac[6];
    if (!hoParseMacFromDeviceId(idStr.c_str(), mac)) {
      Serial.printf("[配對] UNPAIR 的設備 ID 格式錯誤: %s\n", idStr.c_str());
    } else {
      int idx = findSlave(mac);
      if (idx < 0) {
        Serial.printf("[配對] UNPAIR 的設備不在名冊上: %s\n", idStr.c_str());
      } else {
        unpairSlave(idx);
        publishStatus();
      }
    }
  } else if (message == "UNPAIRALL") {
    // 規格沒有這條，是 Phase 2a 交付時列出的待辦：
    // 目前完全沒有辦法清空名冊，master 要重新部署只能整台重燒。
    // 從後往前刪，unpairSlave() 內部的陣列搬移才不會讓索引錯位。
    Serial.printf("[配對] 開始清空名冊，共 %d 台\n", slaveCount);
    while (slaveCount > 0) {
      unpairSlave(slaveCount - 1);
    }
    publishStatus();
  } else if (message.startsWith("LR:")) {
    // Task 6 實作，先給一個明確的回應而不是掉進「未知指令」
    Serial.println("[LR] 指令尚未實作（Task 6）");
```

> **`UNPAIRALL` 的阻塞估算**：`unpairSlave()` 內含 `espNowDelay(100)` 與一次
> `publishSlaveOffline()`。20 台合計約 20 × (100ms + publish)。`espNowDelay()`
> 期間心跳照發；`publish()` 最壞 3 秒 × 20 = 60 秒**沒有心跳**。
> **因此 `UNPAIRALL` 必須改成非阻塞的分批執行**，見 Step 3。

- [ ] **Step 3: `UNPAIRALL` 改成分批，不可一口氣跑完**

把上面的 `while` 迴圈換成待辦旗標，由 `loop()` 每次拆一台：

```cpp
// UNPAIRALL 若在 callback 裡一口氣跑完，20 台 × (espNowDelay(100) + 一次 publish)
// 最壞會超過 60 秒沒有心跳 —— 直接撞破 slave 的 30 秒失聯門檻。
// 改成每次 loop() 只拆一台，全程心跳不中斷。
bool unpairAllPending = false;

void processUnpairAll() {
  if (!unpairAllPending) return;
  if (slaveCount <= 0) {
    unpairAllPending = false;
    Serial.println("[配對] 名冊已清空");
    publishStatus();
    return;
  }
  unpairSlave(slaveCount - 1);
}
```
`handleMasterCommand()` 的 `UNPAIRALL` 分支只設 `unpairAllPending = true;` 並印一行；
`loop()` 在 `slaveStatusScheduler();` 之前呼叫 `processUnpairAll();`。

- [ ] **Step 4: 序列埠指令對齊**

`handleSerialCommand()` 加 `unpairall`，`printHelp()` 補說明，讓現場除錯不必靠 MQTT。

- [ ] **Step 5: 編譯驗證**

三種型號皆 exit 0，記錄 flash／RAM。

- [ ] **Step 6: Commit**

commit 訊息要點名：`ALL:ON` 是 PULSE 不是持續 ON（規格語義）、`UNPAIRALL` 為何必須分批、`sendCmdToAll` 的 `delay(20)` → `espNowDelay(20)`。

---

## Task 6：Long Range 指令與兩端同步

**Files:**
- Modify: `libraries/HoEspNow/HoEspNowProtocol.h`、`ho_master1/ho_master1.ino`、`ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes: Task 1 的 `publishJsonDoc()`、Task 4 的 `handleMasterCommand()`
- Produces：`HO_PKT_LR_SET`/`HO_PKT_LR_ACK`/`HoLrPayload`、master 的 `applyLongRange()` 與 LR 狀態機、slave 的 LR 套用

> **本 Task 只做「指令與同步流程」。實際的距離效益要到 Phase 5 才實測。**
> 因此驗收標準是「兩端狀態一致、序列埠可觀察、不失聯」，**不是**「距離變遠」。

---

- [ ] **Step 1: 協定擴充**

`libraries/HoEspNow/HoEspNowProtocol.h`：

```c
  HO_PKT_LR_SET    = 0x30,  // master → slave：切換 Long Range，帶目標值
  HO_PKT_LR_ACK    = 0x31,  // slave  → master：已套用並存檔
```

```c
// Long Range 切換。LR_SET 由 master 送出（applied 填 0），
// LR_ACK 由 slave 回覆（applied = 1 表示已 esp_wifi_set_protocol 且已寫 EEPROM）。
struct __attribute__((packed)) HoLrPayload {
  uint8_t longRange;   // 目標值：1 = 開啟
  uint8_t applied;
};
```

`.cpp` 不用改（打包／解包是型別無關的）。

- [ ] **Step 2: 兩端共用的套用函式**

**master 與 slave 各寫一份，內容一致**：

```cpp
// 套用 Long Range 設定。
//
// 一律用「11b/g/n + LR」混合 bitmap，兩端都是。
// master 非混合不可：純 WIFI_PROTOCOL_LR 連不上一般 AP，MQTT 會整個斷掉
//（設計規格「難點 2」）。
// slave 本來可以用純 LR，但本階段刻意也用混合，理由是安全性：
// 混合之下 LR 只是「多一種可用速率」，不是換一套不相容的調變，
// 所以切換過程中兩端短暫不同步，代價是「這段期間沒有 LR 的距離增益」，
// 而不是「互相收不到」。這正是下面的握手可以「逾時仍照樣套用」的前提 ——
// 不必為了等一台離線的 slave 把整個系統卡在中間狀態。
//
// 若 Phase 5 實測後決定改用純 LR 或 esp_now_set_peer_rate_config() 強制
// WIFI_PHY_RATE_LORA_250K，這個安全性前提就不再成立，握手會變成真正必要 ——
// 屆時要回頭把「逾時仍套用」改成「逾時則放棄並回滾」。
bool applyLongRange(bool enable) {
  uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  if (enable) proto |= WIFI_PROTOCOL_LR;
  esp_err_t res = esp_wifi_set_protocol(WIFI_IF_STA, proto);
  if (res != ESP_OK) {
    Serial.printf("⚠ [LR] esp_wifi_set_protocol 失敗: %d\n", res);
    return false;
  }
  Serial.printf("[LR] 已套用 %s（bitmap=0x%02x）\n", enable ? "開啟" : "關閉", proto);
  return true;
}
```

- [ ] **Step 3: master 端 —— NVS 持久化**

`longRangeEnabled` 存 **`homaster/lr`**，不是 `hoban`：

```cpp
// LR 服務的是 ESP-NOW 與 slave，跟名冊同生共死，所以放在 homaster 命名空間。
// 放 hoban 會被 clearNetConfig()（reset 指令、長按重置）清掉，
// 但 slave 那端的 EEPROM 不會跟著清 —— master 重置後兩端就不一致了。
// 這個推理與 Phase 2a 把 slaveLockChannel 放進 homaster/espch 完全同源。
uint8_t savedLongRange = 0xFF;   // NVS 現值，避免沒變也重複寫入磨損 flash

void saveLongRange(bool enable) {
  uint8_t v = enable ? 1 : 0;
  if (v == savedLongRange) return;
  prefs.begin("homaster", false);
  prefs.putUChar("lr", v);
  prefs.end();
  savedLongRange = v;
  Serial.printf("[LR] 已寫入 NVS: %s\n", enable ? "開啟" : "關閉");
}
```
`loadSlaves()` 內補讀 `longRangeEnabled = prefs.getUChar("lr", 0) != 0; savedLongRange = longRangeEnabled ? 1 : 0;`
`setup()` 在 `setupEspNow()` 之後、`connectToWiFi()` 之前呼叫 `applyLongRange(longRangeEnabled);`

- [ ] **Step 4: master 端 —— 握手狀態機**

```cpp
// ── Long Range 兩端同步 ──
// 非阻塞三段流程，全程跑在 loop() 裡，心跳不受影響：
//   LR:ON/OFF → LR_ANNOUNCING（每 100ms 對一台還沒 ACK 的 online slave 送 LR_SET）
//             → 全部 ACK 或逾時 10 秒 → 套用自己 → LR_IDLE
// 離線的 slave 由心跳的 longRange 欄位自癒（回線後發現與自己存的不同就跟著切）。
enum LrPhase { LR_IDLE, LR_ANNOUNCING };
LrPhase lrPhase = LR_IDLE;
bool lrTarget = false;
// bit i = slaves[i] 已回 ACK。ESP-NOW callback（WiFi task）只做「設 bit」，
// loop() 只做「讀」與「整個歸零」。歸零與設 bit 若剛好撞在一起，最多漏記一次 ACK，
// 而那個時間點階段已經要結束了，後果只是序列埠多印一台「未確認」，不影響行為。
volatile uint32_t lrAckMask = 0;
unsigned long lrPhaseStart = 0;
unsigned long lrLastSendAt = 0;
int lrSendIdx = 0;
const unsigned long LR_ANNOUNCE_TIMEOUT = 10000;
const unsigned long LR_SEND_GAP = 100;

void startLongRangeSwitch(bool target) {
  if (lrPhase != LR_IDLE) {
    Serial.println("[LR] 上一次切換尚未結束，忽略本次指令");
    return;
  }
  if (target == longRangeEnabled) {
    Serial.printf("[LR] 已經是%s狀態，不需切換\n", target ? "開啟" : "關閉");
    publishStatus();
    return;
  }
  if (slaveCount == 0) {
    Serial.println("[LR] 名冊是空的，直接套用");
    if (applyLongRange(target)) {
      longRangeEnabled = target;
      saveLongRange(target);
      sendHeartbeatBurst();
    }
    publishStatus();
    return;
  }
  lrPhase = LR_ANNOUNCING;
  lrTarget = target;
  lrAckMask = 0;
  lrPhaseStart = millis();
  lrLastSendAt = 0;
  lrSendIdx = 0;
  Serial.printf("[LR] 開始切換為%s，先等 %d 台 slave 確認（最多 %lu 秒）\n",
                target ? "開啟" : "關閉", slaveCount, LR_ANNOUNCE_TIMEOUT / 1000);
  publishStatus();
}

void finishLongRangeSwitch(bool timedOut) {
  if (timedOut) {
    // 逐台印出誰沒確認，Phase 5 實測時要靠這份名單判斷測距結果是否可信
    for (int i = 0; i < slaveCount; i++) {
      if ((lrAckMask & (1UL << i)) == 0) {
        char id[20];
        hoFormatDeviceId(slaves[i].mac, id);
        Serial.printf("[LR] %s 未確認（%s）\n",
                      id, slaves[i].online ? "在線但沒回 ACK" : "離線");
      }
    }
    Serial.println("[LR] 等待逾時，仍照樣套用 —— 混合 bitmap 下不同步只會失去距離增益，"
                   "不會互相收不到；離線的 slave 回線後會由心跳自癒");
  } else {
    Serial.println("[LR] 所有在線 slave 都已確認");
  }

  if (applyLongRange(lrTarget)) {
    longRangeEnabled = lrTarget;
    saveLongRange(lrTarget);
  }
  lrPhase = LR_IDLE;
  // esp_wifi_set_protocol() 有可能造成一次 AP 斷線重連（IDF 行為未在本專案驗證），
  // 這裡主動檢查一次 channel，並連發心跳把最新的 LR 狀態帶給所有 slave
  onWifiChannelMayHaveChanged();
  sendHeartbeatBurst();
  publishStatus();
}

void updateLongRangeSwitch(unsigned long now) {
  if (lrPhase != LR_ANNOUNCING) return;

  // 只要求「在線」的 slave 確認：離線的等它回線由心跳自癒
  bool allAcked = true;
  for (int i = 0; i < slaveCount; i++) {
    if (slaves[i].online && (lrAckMask & (1UL << i)) == 0) { allAcked = false; break; }
  }
  if (allAcked) { finishLongRangeSwitch(false); return; }

  if (now - lrPhaseStart >= LR_ANNOUNCE_TIMEOUT) { finishLongRangeSwitch(true); return; }

  // 每 LR_SEND_GAP 送一台，錯開避免同頻碰撞，也讓 loop() 不被塞住
  if (now - lrLastSendAt < LR_SEND_GAP) return;
  lrLastSendAt = now;

  for (int k = 0; k < slaveCount; k++) {
    int i = (lrSendIdx + k) % slaveCount;
    if (slaves[i].online && (lrAckMask & (1UL << i)) == 0) {
      HoLrPayload p;
      p.longRange = lrTarget ? 1 : 0;
      p.applied = 0;
      espNowSendTo(slaves[i].mac, HO_PKT_LR_SET, &p, sizeof(p));
      lrSendIdx = (i + 1) % slaveCount;
      return;
    }
  }
}
```

`onEspNowRecv()` 加分支（只設 bit，其他什麼都不做）：
```cpp
  if (header.type == HO_PKT_LR_ACK && payloadLen >= sizeof(HoLrPayload)) {
    int idx = findSlave(info->src_addr);
    if (idx >= 0 && idx < 32) {
      HoLrPayload p;
      memcpy(&p, payload, sizeof(p));
      if (p.longRange == (lrTarget ? 1 : 0)) {
        lrAckMask |= (1UL << idx);
        Serial.printf("[LR] %s 已確認\n", senderId);
      }
    }
    return;
  }
```

`handleMasterCommand()` 的 `LR:` 分支換成：
```cpp
  } else if (message == "LR:ON") {
    startLongRangeSwitch(true);
  } else if (message == "LR:OFF") {
    startLongRangeSwitch(false);
```

`loop()` 在 `maintainEspNow();` 之後呼叫 `updateLongRangeSwitch(now);`

`buildStatusDoc()` 的 `dev` 區塊補：
```cpp
  dev["long_range_pending"] = (lrPhase == LR_ANNOUNCING);
```
（已計入 Task 1 的 `STATUS_BASE_MAX_BYTES` 預算）

序列埠加 `lr on` / `lr off`（`handleSerialCommand()` 需要一個接受非數字參數的分支，不能走 `parseIndexArg()`）。

- [ ] **Step 5: slave 端**

`ho_slave1/ho_slave1.ino`：

1. 加 `bool longRangeEnabled = false;`，`loadPairing()` 從 `EE_ADDR_LONGRANGE` 讀（欄位已存在，Phase 1 只存不用）
2. 加與 master 一模一樣的 `applyLongRange()`
3. `setup()` 在 `setupEspNow()` 之後呼叫 `applyLongRange(longRangeEnabled);`
4. `onEspNowRecv()` 在「只接受已配對 master 的控制指令」那道檢查**之後**加：

```cpp
  if (header.type == HO_PKT_LR_SET && payloadLen >= sizeof(HoLrPayload)) {
    HoLrPayload p;
    memcpy(&p, payload, sizeof(p));
    bool target = (p.longRange == 1);

    // 先存 EEPROM 再套用：萬一套用當下斷電，重開機會用新值，
    // 與 master 一致；反過來則會不一致。
    if (target != longRangeEnabled) {
      longRangeEnabled = target;
      savePairing();                 // EE_ADDR_LONGRANGE 一併寫入
      applyLongRange(target);
    }

    HoLrPayload ack;
    ack.longRange = target ? 1 : 0;
    ack.applied = 1;
    espNowSendTo(info->src_addr, HO_PKT_LR_ACK, &ack, sizeof(ack));
    Serial.printf("[LR] 已依 master 指示切換為%s並回覆確認\n", target ? "開啟" : "關閉");
    return;
  }
```

5. 心跳自癒（在 `HO_PKT_HEARTBEAT` 分支、`onMasterFound()` 之前，且只在**已配對**時）：

```cpp
    // 自癒：切換當下若本機離線，會漏掉 LR_SET。心跳一直帶著 master 的現值，
    // 回線後第一則心跳就能對齊。
    if (masterKnown && (hb.longRange == 1) != longRangeEnabled) {
      longRangeEnabled = (hb.longRange == 1);
      savePairing();
      applyLongRange(longRangeEnabled);
      Serial.printf("[LR] 依心跳自癒為%s\n", longRangeEnabled ? "開啟" : "關閉");
    }
```

6. `savePairing()` 要把 `longRangeEnabled` 寫進 `EE_ADDR_LONGRANGE`（確認目前實作有沒有寫，沒有就補）

- [ ] **Step 6: 編譯驗證**

三種型號皆 exit 0（本 Task 動到 slave 與共用 library，**`-Model slave` 這次是實質驗證不是形式檢查**），記錄 flash／RAM。

- [ ] **Step 7: Commit**

commit 訊息必須包含：為什麼兩端都用混合 bitmap、為什麼逾時仍照樣套用是安全的、Phase 5 若改用純 LR 要回頭改哪裡、LR 存 `homaster` 而非 `hoban` 的理由。

---

## Task 7：文件、回歸清單與整體驗證

**Files:**
- Modify: `ho_master1/readme.md`
- Create: `docs/phase2b-regression-checklist.md`

---

- [ ] **Step 1: 更新 `ho_master1/readme.md`**

新增或更新這些章節：

1. **MQTT 指令表** —— 逐條列出 Phase 2a + 2b 的全部指令（`status`/`ON`/`OFF`/`reset`/`FIND_BEST_SERVER`/`HASRELAY:*`/`ALL:ON`/`ALL:OFF`/`SLAVES`/`PAIR:START`/`PAIR:STOP`/`UNPAIR:<id>`/`UNPAIRALL`/`LR:ON`/`LR:OFF`），**每一條都要註明「送到哪個 topic」**
2. **代發／代訂閱的 topic 對照** —— master 自己 1 組 + 每台 slave 1 組，附完整的 JSON 範例（直接取自實際程式碼組出來的欄位，不是從規格複製）
3. **狀態 JSON 容量** —— 三層防線、實測數字、`static_assert` 的意義、以及「為什麼放大 `StaticJsonDocument<N>` 沒有用」
4. **已知限制**（新增／更新）：
   - **代發 topic 沒有 LWT**：PubSubClient 一條連線只有一個 will 名額，已給 master。master 斷電時 20 台 slave 的 status 會停在最後一則 `online` 保留訊息。App 端須以 `via` 欄位判斷（Phase 3）
   - **LR 尚未實測**：Phase 2b 只做指令與同步流程，混合 bitmap 下實際有沒有走到 LR 速率未經驗證，Phase 5 才測
   - **`esp_wifi_set_protocol()` 可能造成一次 WiFi 斷線重連**：未驗證的 IDF 行為
   - `smartConnect()` 最壞 18 秒且只有「進去前」的按鈕逃生口，進到 `connect()` 裡就叫不回來
   - 沿用 Phase 2a 既有的「不做 auth mode 退避」「`WiFi.begin(ssid,pass,ch,nullptr)` 是否真的限制掃描未經證實」兩項
5. **序列埠指令表** —— 補上 `unpairall`、`fakeslaves <n>`、`jsonsize`、`lr on|off`，並標明 `fakeslaves` 是測試工具、不寫 NVS

- [ ] **Step 2: 寫 `docs/phase2b-regression-checklist.md`**

> ### 這一步有一條硬性規定，違反即視為 Task 未完成
>
> **Phase 2a 有一類缺陷出現了三次：回歸清單的驗收標準與程式碼矛盾，
> 導致實測者把正確行為判成 FAIL。**（最後一次是清單寫「按住 1.5 秒放開不應印出任何訊息」，
> 但程式碼按下瞬間就無條件印出訊息，100% 觸發。）
>
> 因此本清單的**每一條「預期序列埠輸出」都必須逐字對照實際程式碼的
> `Serial.print` / `Serial.printf` 格式字串確認**，做法是：
>
> 1. 寫下預期輸出後，用 `Grep` 在 `ho_master1/ho_master1.ino`（或 `ho_slave1.ino`）
>    搜尋該字串的**固定部分**（去掉 `%d`/`%s` 等格式指示子的那一段）
> 2. **搜不到就是寫錯了**，不准靠記憶或推測補上
> 3. 在清單每一項的結尾，用一行註記寫出對照過的位置，格式：
>    `> 對照：ho_master1.ino:1234 的 Serial.printf("[代理] %s 收到指令: %s\n", ...)`
> 4. **凡是「不應該印出某訊息」這類否定式判準一律禁止**，除非你能明確指出
>    程式碼中沒有任何路徑會印出它。改寫成正面判準（「應該印出 X」）
> 5. 每一項都要區分「**失敗判定**」與「**觀察項**」。凡是機制未經實機驗證的
>    （例如 `WiFi.begin()` 的掃描行為、`esp_wifi_set_protocol()` 會不會斷線），
>    **一律列為觀察項，不得列為失敗判定**

清單開頭必須有與 Phase 2a 同樣的警告：**本清單尚未在任何實體硬體上執行過任何一項。**

必須涵蓋的項目（至少）：

| # | 項目 | 重點 |
|---|---|---|
| 1 | master 上線後 `hoban/<masterId>/status` 每 10 秒一則，含 `server`、`free_heap`、`slaves` 陣列 | 用 MQTT Explorer 對照欄位 |
| 2 | **容量驗證**：序列埠下 `fakeslaves 20` 再 `jsonsize` | 記錄實際 bytes；必須 < 3072；同時確認**沒有**出現 `slaves_truncated` |
| 3 | 同上狀態下實際發布一次 `status`，MQTT Explorer 收到的 JSON **語法完整、20 筆條目齊全** | 這是「靜默截斷」的正面驗證 |
| 4 | 對 `hoban/<slaveId>/control` 送 `ON`，slave 繼電器點動 2 秒 | 規格 Phase 2 的主要驗收條件 |
| 5 | 同上，1 秒內 `hoban/<slaveId>/status` 出現 relay 翻轉（dirty 立刻補發） | 驗證 dirty 路徑 |
| 6 | slave 拔電 30~45 秒後，`hoban/<slaveId>/status` 變成 `"status":"offline"`、`wifi.connected:false` | 驗證離線代發 |
| 7 | slave 復電後回到 `online` | 驗證雙向翻轉 |
| 8 | `SLAVES` 指令：master status 立即重發，且接下來 15 秒內每台 slave 的 status 各重壓一次 | 驗證不是一次連發 |
| 9 | `ALL:ON`：所有 slave **與 master 自己**都點動 2 秒 | 注意是 PULSE 不是持續 ON |
| 10 | `ALL:OFF`：全部關閉 | |
| 11 | `PAIR:START` → slave 短按配對 → master status 的 `slave_count` +1、新 slave 的 status topic 出現 | 驗證配對後自動訂閱與代發 |
| 12 | 新配對的 slave 立刻能用自己的 control topic 控制 | 驗證 `pendingSubscribeRefresh` 這條路 |
| 13 | `UNPAIR:<id>`：該 slave 的 status 最後一則是 `offline`，之後對它的 control topic 送指令**沒有反應**且 master 印出「目標不在名冊上」 | |
| 14 | `UNPAIRALL`：名冊清空，**過程中 slave 端全程沒有印出失聯訊息** | 驗證分批執行；這是最容易踩到 30 秒門檻的一條 |
| 15 | `LR:ON`：master 印出開始切換 → 每台 slave 印出已切換並回覆 → master 印出全部確認 → status 的 `long_range` 變 true | |
| 16 | `LR:ON` 後**拔掉一台 slave 再下 `LR:OFF`**：master 逾時 10 秒後仍套用並印出未確認名單；該 slave 復電後由心跳自癒 | 驗證逾時路徑與自癒 |
| 17 | LR 切換前後，**其他 slave 全程不失聯**（繼電器不會被強制關閉） | **失敗判定** |
| 18 | LR 切換後 WiFi 是否斷線、幾秒內回來 | **觀察項，不是失敗判定** |
| 19 | master 重開機後：名冊、訂閱、LR 設定、channel 全部復原 | |
| 20 | master 送 `reset` 後重開機：LR 設定**仍然保留**（存 `homaster` 不是 `hoban`） | 驗證命名空間的選擇 |
| 21 | **回歸 Phase 2a**：BLE 配網、長按重置、`FIND_BEST_SERVER`、`HASRELAY:*` 行為不變 | |
| 22 | **回歸 Phase 2a**：WiFi 拔線 60 秒，slave 全程不失聯 | **失敗判定** |

- [ ] **Step 3: 完整編譯驗證與資源盤點**

```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
```
三者皆 exit code 0。

**在 report 中做一張表**，列出三種型號從 Phase 2a 收尾到 Phase 2b 完成的 flash／RAM 變化：

| 型號 | Phase 2a flash | Phase 2b flash | 增量 | 對分區百分比 | RAM |
|---|---|---|---|---|---|
| master (WROOM) | 1,683,315 (82.7%) | ? | ? | ? | ? |
| master-c3 | 1,299,881 | ? | ? | ? | ? |
| slave | 978,273 | ? | ? | ? | ? |

**若 WROOM 增量超過 60 KB 或百分比超過 86%，在 report 中標紅**，並明確寫出 Phase 4 的轉送 OTA（估 +170~250 KB）在 WROOM 上已不可行，需要走 progress.md 記錄的三條路之一（HTTP 取代 HTTPS ／ NimBLE 取代 Bluedroid ／ master 只支援 C3）。

- [ ] **Step 4: 更新 SDD ledger**

在 `.superpowers/sdd/2026-08-16-esp32-master-phase2b/progress.md` 記錄：
- 每個 Task 的 commit 範圍與 flash 數字
- 所有 Ruling（含被推翻的）
- 交付給使用者判斷的殘留項
- **Phase 3（App）開工前必須知道的事**：`via` 欄位的用途、代發 topic 沒有 LWT、`slaves_truncated` 欄位的語義

- [ ] **Step 5: Commit**

---

## 本階段結束後的狀態

- master 是 20 台 slave 的完整 MQTT 代理，slave 在 App 眼裡就是普通設備
- master 狀態帶 `slaves` 陣列，容量在**編譯期**就保證放得下 20 台
- 「靜默截斷」這個缺陷類別從根上消失：所有 JSON 發布都會先量、放不下就明確拒發
- `ALL:*` / `SLAVES` / `PAIR:*` / `UNPAIR:*` / `UNPAIRALL` / `LR:*` 全部可用
- LR 的指令與兩端同步流程就緒，**但距離效益未測**（Phase 5）
- **尚未有**：App 的樹狀 UI（Phase 3）、ESP-NOW 轉送 OTA（Phase 4）、LR 實測（Phase 5）

## 已知風險

| 風險 | 影響 | 緩解 |
|---|---|---|
| 代發 topic 沒有 LWT，master 斷電後 slave 全部顯示在線 | App 顯示錯誤 | 發 `via` 欄位，Phase 3 由 App 用「master 離線 → 子節點一律離線」處理；已寫入 readme 已知限制 |
| `esp_wifi_set_protocol()` 造成 WiFi 斷線重連 | LR 切換後短暫失去 MQTT | 切換後主動 `onWifiChannelMayHaveChanged()`，`loop()` 的重連機制接手；回歸清單列為觀察項 |
| 混合 bitmap 下 LR 完全不生效 | Phase 5 測不出差異 | 本階段不處理；備案是 `esp_now_set_peer_rate_config()`（規格「難點 2」） |
| WROOM flash 餘裕（Phase 2a 已 82.7%） | Phase 4 可能編不過 | 每個 Task 記錄用量；Task 7 做總表並在超標時明確標紅 |
| `statusBuf` 3072 + mqtt buffer 3328 的 RAM／heap 壓力 | 極端情況 OOM | `statusBuf` 放 .bss 不佔堆疊；`setBufferSize()` 已檢查回傳值；BLE 只在配網模式存在，與 MQTT 互斥 |
| `slaves[]` 的跨 task 存取無鎖 | 某一輪代發資料稍舊 | 沿用 Phase 1 的既有做法；都是單 byte 存取，下一輪即更正，不造成錯誤動作 |
| `lrAckMask` 的 read-modify-write 競態 | 漏記一次 ACK | 只在階段結束的瞬間可能發生，後果僅為序列埠多印一台「未確認」 |
