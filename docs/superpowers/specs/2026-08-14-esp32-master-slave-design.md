# ESP32 Master／Slave 多機聯動設計

日期：2026-08-14
狀態：設計待審

## 背景與目標

現況：`ho_relay1/2/3` 三代韌體都是 `WIFI_STA` + MQTT 點對點，一台設備＝一顆繼電器，
每台設備各自連 WiFi、各自訂閱 `hoban/{deviceId}/control`。全專案（韌體端與 Flutter App 端）
**沒有任何 ESP-NOW、群組、多通道的程式碼**，多機聯動要從零建立。

要解決的三個問題：

1. **一鍵同時觸發多台** — App 按一次，多台繼電器同時動作
2. **Slave 成本要低** — slave 不需要 WiFi 設定、BLE 配對、HTTPS 憑證，韌體與部署都精簡
3. **距離遠** — 現場設備分散，一般 WiFi 覆蓋不到

## 架構總覽

```
                    MQTT (5 台 broker)              ESP-NOW
Flutter App  ←──────────────────────────→  Master  ←────────→  Slave × N (最多 20)
                                        ESP32 WROOM          現有 ho_relay2 C3 板
                                        連 WiFi、跑 MQTT      不連 WiFi、不跑 MQTT
                                        BLE 配對、OTA         只有繼電器 + 按鈕 + LED
```

新增兩個 sketch，**不動現有 `ho_relay1/2/3`**：

| Sketch | 硬體 | `deviceModel` | 角色 |
|---|---|---|---|
| `ho_master1/` | ESP32 WROOM DevKit | `hoMaster1` | 對外 MQTT 窗口 + ESP-NOW 主控，**自身繼電器選配** |
| `ho_slave1/` | 現有 ESP32-C3 繼電器板 | `hoSlave1` | 純 ESP-NOW 受控端 |

`ho_master1` 從 `ho_relay2.ino` 複製 WiFi/MQTT/BLE/OTA/EEPROM 那一整套（已驗證可用），
加上 ESP-NOW 層。**繼電器驅動邏輯完整保留**：硬體接了 MOS 就能用，沒接就是空跑，
`ON`/`OFF` 控制 master 自己那顆，`ALL:ON` 則連自己一起動作。
腳位沿用 `relayPins[]` 陣列的寫法，但改成 WROOM 的腳位（參考 `ho_relay1` 的 GPIO 13）。
`ho_slave1` 從 `ho_relay2.ino` 只保留繼電器驅動、按鈕長按、LED，砍掉 WiFi/MQTT/BLE/HTTPS。

### 核心設計：Master 幫 slave 代發 MQTT topic

Master 除了自己的 topic，還會**用 slave 的 MAC 代發／代訂閱**：

```
發布：hoban/hoban-<slaveMac>/status      ← master 代發
訂閱：hoban/hoban-<slaveMac>/control     ← master 代收，轉成 ESP-NOW 送給該 slave
```

好處：**slave 在 App 的控制路徑上就是一台普通設備** —
詳情頁與「開保險 → 關門 → 關保險」三段鎖零改動就能個別控制每台 slave，
只有設備列表頁需要改成樹狀顯示從屬關係（見下方 App 章節）。
群組控制則走 master 自己的 topic（新指令 `ALL:ON` / `ALL:OFF`）。

這同時滿足「個別控制」「群組控制」「列表看得到樹狀結構」三項需求。

---

## 三個必須先解決的技術難點

### 難點 1：Channel 同步（最高風險）

ESP-NOW 收發雙方必須在**同一個 WiFi channel**。Master 連上路由器後 channel 由 AP 決定
（可能是 1/6/11 任一），且路由器重開或換點後可能改變。Slave 不連 WiFi，無從得知。

解法（三層）：

1. **Master 廣播心跳** — 每 1 秒對 `FF:FF:FF:FF:FF:FF` 發一次 `PKT_HEARTBEAT`，
   內含自己的 MAC、目前 channel、是否在配對模式
2. **Slave 記住 channel** — 配對時把 master MAC + channel 寫入 EEPROM，
   開機直接 `esp_wifi_set_channel()` 切過去
3. **失聯自動輪掃** — Slave 超過 30 秒沒收到心跳，開始輪掃 channel 1~13
   （每個 channel 停 1200ms），收到心跳就鎖定並更新 EEPROM

**心跳週期與掃描停留時間的比例是有意設計的**：dwell（1200ms）必須大於心跳週期（1000ms），
才能保證 slave 掃過正確 channel 時**必定**涵蓋到至少一次心跳。若兩者比例失衡
（例如心跳 5 秒、dwell 600ms），命中就變成機率事件——停留時間佔比僅 600/7800 ≈ 7.7%，
期望要約 13 次心跳才鎖得回來，恢復時間會從十幾秒惡化到 60 秒以上且無上界。
**調整任一個常數時務必一併檢查這個關係。**

恢復時間：最壞 30 秒失聯門檻 + 一輪掃描 13 × 1200ms = **45.6 秒**；典型約 38 秒。

Master 也要在 `WiFi.channel()` 改變時立刻連發數次心跳，縮短 slave 的失聯窗口。

### 難點 2：Long Range 模式可切換

使用者選擇「先做可切換，現場實測再決定」。

```c
// 兩端都要設一樣的 protocol bitmap
esp_wifi_set_protocol(WIFI_IF_STA,
    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
    (longRangeEnabled ? WIFI_PROTOCOL_LR : 0));
```

**關鍵限制：master 不能用純 `WIFI_PROTOCOL_LR`**，那樣就連不上一般 AP，MQTT 會斷。
所以一律用「11b/g/n + LR」混合 bitmap，讓 AP 連線走 11n、ESP-NOW 對 slave 走 LR。

混合模式下實際 rate 由底層協商，不保證用到 LR。若實測距離沒改善，
再用 IDF 5.x 的 `esp_now_set_peer_rate_config()` 對 peer 強制指定
`WIFI_PHY_RATE_LORA_250K`（此 API 在 Arduino core 3.x 應可用，**實作時需先驗證**）。

切換方式：master 端 EEPROM flag，可由 MQTT 指令 `LR:ON` / `LR:OFF` 改；
master 改變後透過心跳把 LR 狀態帶給 slave，slave 跟著切並存 EEPROM。
兩端不同步時會完全失聯，所以**切換流程要先讓所有 slave 確認收到再切自己**。

### 難點 3：Slave 沒有網路，OTA 要靠 master 轉送

使用者選擇要做。流程：

```
App → MQTT update_slave:{...} → Master 用 HTTPS 下載 .bin 到自己的 OTA 分區暫存
                              → 分包經 ESP-NOW 送給 slave → slave 寫入 Update 分區 → 重啟
```

ESP-NOW 單包上限 250 bytes，扣掉封包標頭剩約 240 bytes/包。
1.5MB 韌體約 6500 包，實測吞吐約 100~500 包/秒，估計 30~90 秒可傳完。

必須有：序號、CRC、滑動視窗 ACK（每 32 包 ACK 一次）、逾時重傳、
整體 MD5 校驗、失敗中止不寫入。這是整個專案最複雜的部分，排在最後階段做。

---

## ESP-NOW 通訊協定

不用 JSON（250 bytes 上限太緊），用固定長度 struct。

```c
#define ESPNOW_MAGIC     0x4F48   // "HO"
#define ESPNOW_PROTO_VER 1

enum PacketType : uint8_t {
  PKT_HEARTBEAT = 0x01,  // master → 廣播：MAC、channel、配對模式、LR 狀態
  PKT_PAIR_REQ  = 0x02,  // slave  → master：請求配對
  PKT_PAIR_ACK  = 0x03,  // master → slave：配對成功／被拒（已滿 20 台）
  PKT_UNPAIR    = 0x04,  // 雙向：解除配對

  PKT_CMD       = 0x10,  // master → slave：ON / OFF / PULSE
  PKT_STATE_REQ = 0x11,  // master → slave：請回報狀態
  PKT_STATE     = 0x12,  // slave  → master：繼電器狀態、韌體版本、RSSI

  PKT_OTA_BEGIN = 0x20,  // master → slave：總長度、MD5、目標版本
  PKT_OTA_DATA  = 0x21,  // master → slave：seq + 資料塊
  PKT_OTA_END   = 0x22,  // master → slave：結束，請校驗並重啟
  PKT_OTA_ACK   = 0x23,  // slave  → master：已收到 seq N／進度／錯誤碼
};

struct __attribute__((packed)) EspNowHeader {
  uint16_t magic;    // ESPNOW_MAGIC，過濾非本系統封包
  uint8_t  version;  // 協定版本，未來擴充用
  uint8_t  type;     // PacketType
  uint16_t seq;      // 去重與 OTA 分包序號
  uint8_t  crc;      // payload 的 CRC8
};  // 7 bytes
```

**不使用 ESP-NOW 原生 LMK 加密**（peer 上限只有 6 個，使用者要 20 台）。
改在封包層加一組共享密鑰的簡易驗證碼防誤觸發：
`crc` 欄位改為「payload + 共享密鑰」的 CRC8，密鑰寫死在雙方韌體。
這擋得住鄰居的 ESP-NOW 誤觸，擋不住有心人重放攻擊 — 對本場景可接受。

---

## MQTT 協定擴充

### Master 自身狀態（新增 `slaves` 陣列）

```json
{
  "device_id": "hoban-a0b1c2d3e4f5",
  "status": "online",
  "version": "1.0.0",
  "model": "hoMaster1",
  "server": "mqttgo.io",
  "timestamp": 12345,
  "wifi": { "connected": true, "ssid": "HBTech", "rssi": -55, "ip": "192.168.1.20" },
  "device": { "relay": 0, "has_relay": true, "pairing": false, "slave_count": 3, "channel": 6, "long_range": false },
  "slaves": [
    { "id": "hoban-aabbccddeeff", "relay": 0, "online": true, "rssi": -72, "version": "1.0.0" }
  ]
}
```

`slaves` 陣列讓 App 能在 master 詳情頁列出所有 slave、顯示上下線與訊號強度。
20 台 slave 會讓 JSON 超過 `StaticJsonDocument<200>` 很多，
**master 的狀態 buffer 要放大到 2048 bytes**。

### Master 代發的 Slave 狀態

```json
{
  "device_id": "hoban-aabbccddeeff",
  "status": "online",
  "version": "1.0.0",
  "model": "hoSlave1",
  "via": "hoban-a0b1c2d3e4f5",
  "timestamp": 12345,
  "wifi": { "connected": true, "ssid": "ESP-NOW", "rssi": -72, "ip": "N/A" },
  "device": { "relay": 0 }
}
```

`wifi` 欄位刻意填成與一般設備相同的形狀，讓 App 兩個頁面既有的手動解析
（`devices_page.dart` 與 `device_detail_page.dart` 各自的 `_handleMqttMessage`）
不用改就能吃下 slave 的狀態，`rssi` 借來顯示 ESP-NOW 訊號強度。
`via` 是新欄位，標示這台是誰代發的。

**修正（2026-08-16）**：本節原先寫「讓 `Device.updateFromMqttMessage()` 不用改就能解析」，
該宣稱不成立——那支函式在 `lib/` 底下**沒有任何生產呼叫點**（只有
`test/models/device_test.dart:209-295` 的 12 個測試在測它），兩個頁面都是各自手寫
`copyWith` 解析。設計意圖（slave payload 與一般設備同形狀）仍然成立，只是受益的
是兩個頁面的手動解析而非那支函式。

Slave 失聯時 master 要代發 `"status": "offline"`，否則 App 會一直顯示上線。

**架構限制（2026-08-16 於 Phase 2b 規劃時發現）**：PubSubClient 一條連線只有
**一個 LWT（遺囑）名額**，已經給了 master 自己。所以上面這條「代發 offline」只涵蓋
「master 活著但 slave 失聯」的情況；**master 自己斷電時，所有 slave 會停在最後一則
`online`**。App 端必須用 `via` 欄位反查代發者：master 顯示離線時，其底下所有 slave
一律視為狀態不明，不可信任它們最後一則 `online`。

### Master 新增的控制指令

沿用現有的純文字前綴風格（與既有的 `update:{json}` 一致）：

| 指令 | 行為 |
|---|---|
| `ALL:ON` | 廣播 pulse 給所有已配對 slave |
| `ALL:OFF` | 廣播 OFF 給所有已配對 slave |
| `SLAVES` | 立刻回報 slave 清單（發一次 status） |
| `PAIR:START` | 進入配對模式 60 秒 |
| `PAIR:STOP` | 離開配對模式 |
| `UNPAIR:hoban-aabbccddeeff` | 移除指定 slave |
| `LR:ON` / `LR:OFF` | 切換 Long Range 模式 |
| `HASRELAY:ON` / `HASRELAY:OFF` | 宣告 master 硬體有沒有接繼電器（韌體無法自動偵測，存 NVS，預設 OFF） |
| `update_slave:{"id":"hoban-...","version":"1.0.1","url":"https://..."}` | 對指定 slave 做轉送 OTA |

Slave 的 control topic 由 master 代訂閱，收到 `ON` / `OFF` / `status`
就轉成對應的 `PKT_CMD` / `PKT_STATE_REQ` 送出。

---

## 配對流程

使用者選擇「實體按鈕與 App 指令都要支援」。

**Master 進入配對模式**（60 秒，LED 慢閃）：
- 實體：**短按** BOOT 按鈕一下（長按 3 秒仍然是重置，不衝突 — 現有韌體短按沒有功能）
- App：MQTT 送 `PAIR:START`

**Slave 送出配對請求**：
- 實體：短按 BOOT 按鈕一下 → 輪掃 channel 1~13 尋找帶有「配對模式中」旗標的心跳
  → 找到就送 `PKT_PAIR_REQ`

**完成**：
- Master 檢查未滿 20 台 → 存 slave MAC 到 NVS → 回 `PKT_PAIR_ACK`
  → 立刻代發一次該 slave 的 status（App 就會看到新設備）
- Slave 收到 ACK → 存 master MAC + channel + LR 旗標到 EEPROM → LED 快閃 3 下
- 已滿 20 台則回拒絕碼，slave LED 長閃 3 下表示失敗

### 儲存空間

- **Master**：20 台 × 6 bytes MAC ＝ 120 bytes，加上名稱與狀態會超過現有 128 bytes EEPROM
  → master 改用 `Preferences`（NVS），不要沿用 EEPROM
  （現有 EEPROM 佈局本身還有 bug：`mqttPassword` 的 114-129 與 port 的 126-127 重疊，
  新 sketch 用 NVS 順便避開）
- **Slave**：master MAC (6) + channel (1) + LR flag (1) ＝ 8 bytes，EEPROM 綽綽有餘

---

## Flutter App 改動

### 設備列表改成樹狀結構

現況：`devices_page.dart:1004-1081` 是「上線設備」「離線設備」兩段平鋪列表，
各自 `onlineDevices.map(_buildDeviceCard)`，資料來自
`DeviceService.getDevicesByRoomGrouped()`。

改成階層顯示後有個衝突要先解決：**master 上線但某台 slave 離線時，
舊的分組會把 slave 丟到「離線設備」區塊，樹就斷成兩截**。

取捨：**樹狀關係優先於上下線分組**。

- 只有**根設備**（`parentId == null`，含 master 與一般 hoRelay 單機）參與上線／離線分組
- Slave 一律緊跟在自己的 master 底下顯示，不論自己是否離線
- Slave 卡片自身仍以顏色／圖示標示上下線，離線的 slave 在 master 底下呈灰階

呈現方式用 `ExpansionTile` 包住 master 卡片，展開後是縮排的 slave 卡片，
左側加一條連接線標示從屬關係。Master 卡片標題列顯示「N 台子設備・M 台在線」，
收合狀態下也看得出底下有幾台、有沒有異常。預設展開。

一般單機設備（`hoRelay1/2/3`，沒有 slave）維持現在的平面卡片，不套 `ExpansionTile`，
避免多一層無意義的展開箭頭。

### 逐項改動

| 項目 | 檔案 | 改動 |
|---|---|---|
| Device 模型加 `parentId` | `lib/models/device.dart` | 新欄位（slave 填 master 的 Firestore id，根設備為 null），`fromJson`/`toJson` 同步；`isSlave` 為 `parentId != null` 的 getter |
| 樹狀分組邏輯 | `lib/services/device_service.dart` | 新增 `getDeviceTreeGrouped()`：根設備做上下線分組，每個根節點掛上 `children` |
| 列表改樹狀 | `lib/pages/devices_page.dart:1004-1081` | 改用上述分組；新增 `_buildDeviceTreeNode()`，有子設備時包 `ExpansionTile` |
| Slave 卡片樣式 | `lib/pages/devices_page.dart` 的 `_buildDeviceCard()` | 加 `isChild` 參數控制縮排、連接線、緊湊版面 |
| 解析 `slaves` 陣列與 `via` 欄位 | `lib/pages/device_detail_page.dart:2705` | 現有解析區塊擴充 |
| Master 詳情頁：slave 清單 | `lib/pages/device_detail_page.dart` | 新區塊，顯示每台 slave 狀態＋個別控制入口 |
| 群組控制按鈕 | 同上 | 「全部關門」按鈕，送 `ALL:ON` |
| 配對 UI | 同上 | 「加入新設備」→ 送 `PAIR:START`，倒數 60 秒 |
| 把 slave 加入我的設備 | `lib/services/device_service.dart` | 從 master 的 `slaves` 陣列建立 Firestore 文件，`parentId` 填 master id |
| model 對應 | `firmware_updates/{model}` | 新增 `hoMaster1`、`hoSlave1` 兩份 Firestore 文件 |

**注意**：App 不會自動新增未知設備。Slave 要先在 master 詳情頁點「加入」，
才會寫進 `users/{uid}/devices/{slaveId}`，之後就在列表樹上長出來。

**孤兒處理**：若 slave 的 `parentId` 指向一台已被刪除的 master，
`getDeviceTreeGrouped()` 要把它降級成根節點顯示，不能讓它從列表消失。

---

## 實作階段

分五階段，每階段結束都是可燒錄可驗證的狀態。

### Phase 1：ESP-NOW 骨架與配對
建立 `ho_master1/`、`ho_slave1/` 兩個 sketch，實作心跳、channel 同步、
配對／解除配對、`PKT_CMD` 開關與 `PKT_STATE` 回報。
**先不接 MQTT**，master 用序列埠指令測試。
驗收：Serial 輸入指令能開關 slave 繼電器；slave 拔電重開能自動找回 master。

### Phase 2：Master MQTT 代理
Master 接上 WiFi/MQTT/BLE（從 `ho_relay2.ino` 移植），實作代發／代訂閱、
`slaves` 陣列、`ALL:ON`/`ALL:OFF`/`PAIR:*`/`UNPAIR:*` 指令。
驗收：用 MQTT Explorer 對 `hoban/hoban-<slaveMac>/control` 送 `ON`，slave 動作；
App 現有介面不改就能控制 slave。

### Phase 3：App 樹狀結構與 UI
Device 模型加 `parentId`、`getDeviceTreeGrouped()` 樹狀分組、設備列表改階層顯示、
master 詳情頁 slave 清單、群組控制、配對流程 UI。
驗收：App 上完成一次完整配對，slave 長在 master 底下成為子節點；
master 離線時整棵樹一起移到離線區塊；slave 單獨離線時仍留在樹上並顯示灰階；
刪掉 master 後 slave 降級成根節點不會消失；群組按鈕能一次觸發多台。

### Phase 4：ESP-NOW 轉送 OTA

**flash 預算已實測（2026-08-16 探針編譯，結論：WROOM 放得下 HTTPS，不需改架構）**

| 情境 | WROOM | 對 app0 (2,031,616) | C3 |
|---|---|---|---|
| 基準（Phase 2a 完成時） | 1,683,311 | 82.86% | 1,299,877（63.98%） |
| 加 `Update.h` + `HTTPClient.h` | 1,809,603 | 89.07% | 1,438,627（70.81%） |
| 再加 `WiFiClientSecure.h`（HTTPS） | 1,809,111 | **89.05%，剩 217 KB** | 1,437,463（70.75%） |

**兩個推翻先前假設的結論**：

1. **HTTPS 的實際成本只有約 126 KB**，不是原先估的 170~250 KB。WROOM 剩 217 KB 餘裕，寫完完整的重試／進度回報邏輯（多是邏輯碼、幾百到一兩千 bytes 等級）仍吃得下
2. **走 HTTP 幾乎省不到 flash**（只差 492 bytes，雜訊範圍內）。原因：`ho_master1.ino` 無條件 include BLE 標頭，**BLE 的安全機制已把 mbedTLS 整包連結進去**，TLS 是沉沒成本。真正的大頭是 `Update.h` + `HTTPClient.h`

**所以：Phase 4 沒有理由為了省 flash 而犧牲 HTTPS**。原先列的三條備案（走 HTTP／換 NimBLE／master 只支援 C3）實測後都不需要。


分包、ACK 視窗、重傳、MD5 校驗、進度回報到 MQTT。
驗收：從 App 對 slave 發起 OTA，成功升版且失敗時不會變磚。

### Phase 5：Long Range 實測與切換
`LR:ON`/`LR:OFF` 指令、兩端同步切換流程、現場測距。
驗收：記錄標準模式與 LR 模式的實際可用距離，決定出貨預設值。

---

## 驗證方式

- **韌體編譯**：`flash.ps1` 加入 master／slave 兩個新型號設定，
  沿用既有 arduino-cli 1.3.1 工具鏈（見 `.claude/rules/vscode-arduino-toolchain.md`）
- **MQTT 觀測**：MQTT Explorer 訂閱 `hoban/#`，確認代發 topic 與 payload 格式
- **失聯測試**：slave 拔電、master 拔電、路由器換 channel 三種情境各測一次恢復時間
- **距離測試**：Phase 5 在空曠地實測，記錄 RSSI 與封包遺失率對距離的曲線
- **App 端**：`flutter analyze` 通過，並在實機完成配對→控制→群組控制全流程

## 已知風險

| 風險 | 影響 | 緩解 |
|---|---|---|
| 混合 protocol bitmap 下 LR 不生效 | 距離沒改善 | Phase 5 實測；備案是 `esp_now_set_peer_rate_config()` |
| Master 換 AP 後 channel 改變 | 所有 slave 短暫失聯 | 30 秒後自動輪掃恢復，可接受 |
| 20 台 slave 的 status JSON 過大 | MQTT 發布失敗 | buffer 放大到 2048；必要時 slave 清單改分頁發送 |
| ESP-NOW OTA 中斷 | slave 變磚 | 雙 OTA 分區＋MD5 校驗，未通過不切換分區 |
| Master 單點故障 | 全部 slave 失控 | 本版不處理；未來可考慮備援 master |
