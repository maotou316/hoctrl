# hoMaster1 — ESP-NOW 主控端

## 硬體

同一份 `ho_master1.ino` 用 `CONFIG_IDF_TARGET_ESP32C3` / `CONFIG_IDF_TARGET_ESP32`
條件編譯支援兩種板子，**不是兩份 sketch**，燒錄時用 `flash.ps1` 的型號區分：

| 型號 | 開發板 | BOOT | 第二按鈕 | LED | 繼電器 |
|---|---|---|---|---|---|
| `master`（custom 分區，與 C3 共用 `partitions.csv`） | ESP32 WROOM DevKit | GPIO 0 | GPIO 14（未接線，僅供自檢） | 板載 GPIO 2 | GPIO 13 |
| `master-c3`（custom 分區） | ESP32-C3 Dev Module | GPIO 9 | GPIO 1（RESET） | 板載 GPIO 3 ＋ 面板 GPIO 0 | GPIO 4 與 7（同時驅動） |

C3 版 GPIO 對齊 `ho_slave1.ino`（同一塊硬體）。

- 繼電器為**選配**：只有 `allon` / `alloff` / `allpulse` 會驅動 master 自己的繼電器，
  沒接則這段空跑。`on <n>` / `off <n>` / `pulse <n>` 控制的是**第 n 台 slave**，與 master 自己的繼電器無關

## WROOM 版 vs C3 版怎麼選

1. **CPU**：C3 是單核 RISC-V，WROOM 是雙核。Phase 1（純 ESP-NOW 序列埠操作）沒差，
   但 Phase 2 的 master 要同時跑 WiFi + MQTT（5 台 broker）+ BLE + ESP-NOW，
   單核的 C3 壓力明顯較大，目前尚未實測，屆時若效能不足應優先選 WROOM。
2. **Flash 空間**：Task 6 起 WROOM 版也改用 `PartitionScheme=custom`（與 C3 共用
   `ho_master1/partitions.csv`，app0/app1 各 `0x1F0000` = 2,031,616 bytes），
   兩者 app0 分區大小**相同**，差異只在實際用量：
   - WROOM：實測約 1,680,595 bytes（**82.7%**），餘裕僅約 343KB
   - C3：實測約 1,296,709 bytes（約 63.8%），餘裕約 718KB
   BLE（Bluedroid）在 WROOM 上占用明顯較多 flash，是兩者差距的主因。
   詳見文末「已知風險」第一項。
3. **C3 版若要接繼電器，有硬體限制**：GPIO 4/7 是 ESP32-C3 的 JTAG 腳（MTMS/MTDO），
   reset 後由 ROM 配置、不保證低電位，開機瞬間繼電器會短暫通電，這是硬體限制、
   韌體無法根治，跟 `ho_relay2` 完全同源。需硬體在 MOS gate 對地加 10kΩ 下拉才能根治。
   WROOM 版的 GPIO 13 沒有這個問題。詳見 `ho_relay2/readme.md` 的「已知硬體限制」章節。

## 角色

Phase 1 階段是純 ESP-NOW 主控，用序列埠指令操作。
Phase 2a 起接上 WiFi + MQTT + BLE 配網，成為 App 與所有 slave 之間的唯一對外窗口
（但**尚未**代發代訂閱 slave 的訊息，那是 Phase 2b）。

## 序列埠指令

| 指令 | 說明 |
|---|---|
| `list` | 列出所有 slave 與在線狀態 |
| `pair` | 進入／離開配對模式（60 秒） |
| `on <n>` / `off <n>` | 開啟／關閉第 n 台 **slave** 的繼電器 |
| `pulse <n>` | 點動第 n 台 slave 2 秒 |
| `allon` / `alloff` / `allpulse` | 群組控制，含 master 自己的繼電器 |
| `state <n>` | 要求第 n 台回報狀態 |
| `unpair <n>` | 解除第 n 台配對 |
| `ch <n>` | 測試用：切換 channel，驗證 slave 重掃 |
| `help` | 顯示說明 |

## 配對

短按 BOOT 進入配對模式 60 秒，LED 慢閃（詳見下方「LED 狀態指示」）。
此時短按 slave 的按鈕即可加入。上限 20 台。

master 接受或拒絕一筆配對請求時，LED 會額外閃 3 下作為單次事件回饋
（接受：快閃 3 下／100ms；拒絕或已滿：慢閃 3 下／400ms），播完立刻交回持續式狀態指示，
詳見下方「LED 狀態指示」的分工說明。

心跳固定每 1 秒廣播一次（不分是否在配對模式）。這個值與 slave 每個 channel 停留
1200ms 是一組的：dwell 大於心跳間隔，slave 輪掃時一輪內必定命中正確 channel。

**序列埠上的心跳只有每 10 次印一行**（`HEARTBEAT_LOG_EVERY`），約 10 秒一行 ——
發送頻率不受影響，純粹是避免每秒一行把序列埠洗版、蓋掉其他訊息。
channel／配對模式／slave 台數任一項變化時會立即印一行，狀態變化不會被吃掉。
`ch <n>` 切換 channel 時另外印一行 `[心跳] channel 已變更，連發 4 次（間隔 200 ms）`。

名冊存在 NVS（`Preferences`，命名空間 `homaster`），斷電不遺失。
開機時 `setup()` 會在 `setupEspNow()` 之後呼叫 `registerAllPeers()`，
把名冊上每一台重新註冊成 ESP-NOW peer —— ESP-NOW 的 peer 表只存在 RAM，
少了這步，master 重開機後對所有 slave 的指令都會失敗且不會自我修復。

## 按鈕自檢

開機時取樣 BOOT／第二按鈕 500ms，整段都是 LOW 的腳判定為短路／未接，本次開機停用其功能
（`checkStuckButtons()`）。WROOM 版的第二按鈕（GPIO 14）目前沒接線，自檢與
`anyResetButtonPressed()` 對它只是空跑；C3 版的第二按鈕是真正接線的 RESET（GPIO 1），
兩者共用同一套自檢／判斷函式，行為由 GPIO 陣列決定，不需要另外分支。
詳見 `.claude/rules/button-pin-stuck-low.md`。

## LED 狀態指示（Task 7）

LED 用 `ledPins[]` / `ledPinCount` 陣列驅動（`setLeds()`），WROOM 只有一顆板載 LED、
C3 有板載＋面板兩顆同步驅動，行為完全由陣列內容決定，不寫死單顆。

master 的 LED 由兩套機制分工，**刻意不合併成一個函式**：

- **`updateBlink()`（一次性請求式閃爍）**：只在配對接受／拒絕這種單次事件觸發，
  閃完固定次數就結束，行為與 `ho_slave1.ino` 的 `updateBlink()` 同源設計
  （`onEspNowRecv()` 在 WiFi task context 呼叫 `requestBlink()` 只寫 volatile 旗標，
  實際推進在 `loop()` 用 millis 非阻塞完成）。
- **`updateStatusLed()`（持續式狀態指示）**：只要條件成立就一直閃，直到狀態改變。

兩者的優先權：`updateBlink()` 進行中（`blinkActive == true`）時暫時接管 LED；
一次性閃爍播完的**同一輪** `loop()` 就會呼叫 `updateStatusLed()` 接手，兩者不會互相覆蓋
（若硬合併成一個函式，「配對結果閃 3 下」與「WiFi 未連快閃」會互相打斷，行為難以預測）。

`updateStatusLed()` 依優先序（互斥判斷，由高到低）：

| 優先序 | 條件 | 閃法 |
|---|---|---|
| 1 | `bleConfigMode`（BLE 配網中） | 慢閃 1000ms |
| 2 | `pairingMode`（配對模式中） | 慢閃 500ms |
| 3 | WiFi 未連線 | 快閃 300ms，滿 30 秒後熄滅省電 |
| 4 | WiFi 已連但 MQTT 未連 | 一長二短（週期 2000ms：600ms 亮／200ms 滅／150ms 亮／200ms 滅／150ms 亮／700ms 滅） |
| 5 | 全部正常 | 熄滅 |

「WiFi 未連線 30 秒後熄滅省電」的計時只在真的處於「WiFi 未連線且不在 BLE 配網／配對模式」
時累積；被更高優先序借用 LED 的期間計時不會歸零（因為量的是 WiFi 實際離線的時長，
不是 LED 有沒有被拿去做別的事）。

## BLE 配網

僅在**開機時沒有 WiFi 設定**（`!hasWifiConfig()`）才啟動，設定成功後 `ESP.restart()`，
之後不再開 BLE（BLE stack 約佔 50~70KB heap，且與 WiFi 共用 2.4G 射頻，
配網完成沒有必要繼續占用）。

- **Service UUID**：`4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- **Characteristic UUID**：`beb5483e-36e1-4688-b7f5-ea07361b26a8`
- **裝置名稱**：`hoban-<MAC位址>`（例：`hoban-a0b1c2d3e4f5`）

UUID 與欄位路徑必須與 `ho_relay2` 完全一致，否則現有 Flutter App 找不到設備／
送出的設定會被忽略（`server`/`port`/帳密全部在 `wifi` 物件底下，非頂層 `mqtt` 物件）。

**App 送出的設定 JSON**（以實際程式碼 `MyCallbacks::onWrite` 為準）：

```json
{
  "wifi": {
    "ssid": "WiFi名稱",
    "password": "WiFi密碼",
    "server": "MQTT伺服器位址",
    "mqtt_username": "帳號（選用）",
    "mqtt_password": "密碼（選用）",
    "mqtt_port": 1883
  }
}
```

`ssid` / `password` / `server` 為必填，缺一則回覆錯誤且不儲存。`mqtt_port` 選用，
未帶時預設 `1883`。

**成功回覆**：

```json
{
  "status": "success",
  "message": "WiFi 和 MQTT 設定已儲存",
  "data": {
    "device_id": "hoban-a0b1c2d3e4f5",
    "ssid": "WiFi名稱",
    "mqttServer": "MQTT伺服器位址",
    "mqttPort": 1883,
    "hasAuth": true
  }
}
```

**失敗回覆**（JSON 格式錯誤，或 `wifi` 欄位缺失）：

```json
{ "status": "error", "message": "無效的JSON格式" }
```

**失敗回覆**（`ssid` / `password` / `server` 任一缺漏）：

```json
{ "status": "error", "message": "SSID、密碼或伺服器格式錯誤" }
```

設定儲存成功後，`espNowDelay(2000)` 等待 2 秒（期間心跳照常發出，已配對的 slave
不會因此失聯）才 `ESP.restart()`。

## WiFi 連線（ESP-NOW 友善版）

`connectToWiFi()` 與 `ho_relay2` 的差異、以及 loop() 重連策略，詳見程式碼中
`connectToWiFi()` 與 `loop()` WiFi 管理區塊的註釋。重點：不呼叫 `WiFi.scanNetworks()`、
不呼叫 `WiFi.mode(WIFI_OFF)`、所有等待都走 `maintainEspNow()` 讓心跳照常發出，
最壞阻塞 15 秒。重連時若有上次成功關聯的 channel／BSSID 記錄，會直接指定跳過
`WiFi.begin()` 內建的全頻道掃描（該掃描即使不呼叫 `scanNetworks()` 仍會發生）。

## MQTT

### Topic

| Topic | 方向 | 說明 |
|---|---|---|
| `hoban/<deviceId>/status` | 發布 | 狀態回報，QoS1、retain，每 10 秒一次；LWT 也發到這個 topic（`status:"offline"`） |
| `hoban/<deviceId>/control` | 訂閱 | App／使用者送控制指令 |

`<deviceId>` 格式為 `hoban-<MAC位址>`，與 BLE 裝置名稱相同。

### 控制指令表

| 指令 | 行為 |
|---|---|
| `status` | 立即 `publishStatus()` 回報一次 |
| `ON` | `pulseRelay(2000)` 點動 master 自己的繼電器 2 秒，並回報狀態 |
| `OFF` | `setRelayPins(false)` 關閉 master 自己的繼電器，並回報狀態 |
| `reset` | 清除 NVS 網路設定並重啟，回到 BLE 配網模式 |
| `FIND_BEST_SERVER` | 主動斷開目前 MQTT 連線並重新走 `smartConnect()` 挑選伺服器 |
| `HASRELAY:ON` / `HASRELAY:OFF` | 設定「本機是否有接繼電器」並存 NVS，回報狀態 |

Phase 2a **尚未支援** `ALL:*` 群組指令、`PAIR:*` / `UNPAIR:*` 針對 slave 的指令、
`update:{JSON}` OTA 指令 —— 這些是 Phase 2b／Phase 4 才加。

### 狀態 JSON 範例

```json
{
  "device_id": "hoban-a0b1c2d3e4f5",
  "status": "online",
  "version": "1.0.0",
  "model": "hoMaster1",
  "timestamp": 123456,
  "wifi": {
    "connected": true,
    "ssid": "MyWiFi",
    "rssi": -55,
    "ip": "192.168.1.100"
  },
  "device": {
    "relay": 0,
    "has_relay": false,
    "pairing": false,
    "slave_count": 2,
    "channel": 6,
    "long_range": false
  }
}
```

Phase 2a 尚未包含 `slaves` 陣列（各 slave 的個別狀態），那是 Phase 2b 才加。
容量瓶頸細節見文末「已知風險」第三項。

## 與 ho_relay2 的差異

| 面向 | `ho_relay2` | `ho_master1` |
|---|---|---|
| ESP-NOW | 無 | 全程維持，配網／WiFi 重連／MQTT 切換期間都不能失聯 |
| BLE 模式下的 `loop()` | 直接 `return`，其餘邏輯全部跳過 | 只跳過 WiFi／MQTT 管理區塊，按鈕、`maintainEspNow()`、LED 仍照跑 |
| BLE `onDisconnect` | 沒有重新開始廣播，App 斷線後需重開機才能再次配對 | 補上 `BLEDevice::startAdvertising()`，斷線可直接重連 |
| 網路設定儲存 | EEPROM 128 bytes，`mqttPassword` 與 `mqttPort` 位址重疊，MQTT 密碼實際只能 12 字元 | NVS（`Preferences`），各欄位獨立，密碼上限 64 字元 |
| WiFi 連線 | 掃描＋多段 auth mode 退避重試 | 不掃描、記住 channel/BSSID 直接關聯、無 auth mode 退避（見已知風險） |
| WiFi 中斷 WiFi 驅動 | `WiFi.disconnect(true)`／關閉驅動 | `WiFi.disconnect(false)`，驅動保持存活供 ESP-NOW 使用 |
| MQTT buffer / timeout | 未設定（256 bytes／15 秒逾時） | `setBufferSize(1024)` / `setSocketTimeout(3)` |
| LED | 恆亮／閃爍幾種簡單狀態 | 一次性事件閃爍 ＋ 持續式狀態機（見上方「LED 狀態指示」） |
| 韌體更新（OTA） | MQTT `update:{JSON}` 指令 | 尚未支援，留給 Phase 4 |

## 已知風險

### 1. WROOM flash 用量偏緊（82.7%）

實測 1,680,595 / 2,031,616 bytes（app0 分區），餘裕僅約 343KB。
Phase 4 要加轉送 OTA 的程式碼前，務必先跑 `.\flash.ps1 -Model master` 確認還編得過；
若餘裕不足，需考慮用 NimBLE 取代目前的 Bluedroid（BLE stack 是兩板差距的主因，
C3 同一份 `partitions.csv` 下用量僅約 63.8%）。

### 2. WiFi 連線沒有 auth mode 退避

`ho_relay2` 的 `connectToWiFi()` 針對連線失敗會依序嘗試多種 auth mode 組合退避重試，
`ho_master1` **刻意不補這段**。若目標 AP 需要特殊的認證退避流程才能連上，
master 只會用預設方式嘗試一次、失敗就等下一輪重連（見 `loop()` 的
`wifiFailCount` / `wifiPauseUntil` 邏輯），不會自動切換 auth mode。

裁決理由：本輪修正的方向是壓縮 WiFi 連線流程的阻塞時間（`maxWaitMs` 已由 30 秒
降到 15 秒），把 `ho_relay2` 那套多段式 auth mode 退避搬回來，會讓單次連線嘗試的
阻塞時間再拉長好幾倍，與這個方向直接衝突。目前部署對象的 AP 已知可用預設方式連線；
若之後遇到真的連不上的路由器，屆時再針對該款 AP 補特定的重試模式，
比現在盲目加五段退避更務實。

### 3. 狀態 JSON 的真正容量瓶頸是 `StaticJsonDocument<512>` 與 `char buf[512]`

不是 `mqttClient` 的 1024 buffer。`publishStatus()` 目前實測最壞情況約 317 bytes，
餘裕僅約 195 bytes。一旦超過 512 bytes，`ArduinoJson` 會**截斷**輸出而非溢位或報錯，
且截斷後的長度仍小於 `mqttClient` 的 1024 buffer，`publish()` 因此仍會回傳 `true`——
現有的診斷（見 `publishStatus()` 裡 `if (!ok)` 那段）完全抓不到這種情況，只會抓到
「連 1024 都塞不下」的極端案例。

Phase 2b 加入 `slaves` 陣列（每台 slave 的個別狀態）時，**必須同時放大三處**：
`StaticJsonDocument<512>`、`char buf[512]`、以及 `mqttClient.setBufferSize(1024)`，
三者任一沒跟著放大都會讓新加的欄位被靜默截斷或整包發布失敗。

## 編譯與燒錄

```powershell
.\flash.ps1 -Model master              # WROOM 版，只編譯
.\flash.ps1 -Model master -Upload      # WROOM 版，編譯並燒錄
.\flash.ps1 -Model master-c3           # C3 版，只編譯
.\flash.ps1 -Model master-c3 -Upload   # C3 版，編譯並燒錄
```
