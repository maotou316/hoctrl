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

## 長按重置（Task 8）

移植自 `ho_relay2.ino` 的 `waitForResetConfirm()`，但改寫成**非阻塞狀態機**
（`updateResetButton()`，由 `loop()` 每輪呼叫一次推進），不是照抄阻塞版：master
每 1 秒要發 ESP-NOW 心跳，slave 超過 30 秒沒收到即判定失聯、開始輪掃、
**強制關閉繼電器**（動物管制設備＝開籠），阻塞版整段流程約 5.7 秒會吃掉
將近 6 秒心跳，不能沿用。

**操作方式**（BOOT 或第二按鈕任一支，`anyResetButtonPressed()` 判斷，已排除
自檢卡死的腳）：

1. 按住滿 3 秒 → LED 以 250ms 週期閃爍（進入確認階段）
2. 確認階段**持續按住**再 2 秒 → LED 長亮 0.7 秒 → 清除網路設定並重啟
3. 中途放開即取消，計時歸零

**⚠ 重置範圍只清網路設定，slave 配對記錄保留**：`clearNetConfig()` 只清 NVS 的
`hoban` 命名空間（WiFi SSID/密碼、MQTT 伺服器設定），**不會**清除 `homaster`
命名空間的 slave 名冊——這是刻意設計，不是漏清：重新配網（例如換路由器、
打錯密碼）不該讓所有已配對的籠子全部解除配對。序列埠會明確印出這個範圍，
避免使用者誤以為是出廠重置。若要清空 slave 名冊，需用序列埠 `unpair <n>`
逐台解除。

**與短按配對的區分**：短按（50~999ms）只認 BOOT 一支腳，用於進出配對模式；
長按重置認兩支腳中任一支，需持續按滿 3 秒才進入確認階段。兩段判斷式各自的
時間窗互斥（短按上限 1000ms、長按下限 3000ms），中途放開兩者都不會觸發，
不需要額外的互斥旗標。

**LED 優先權**（master 的 LED 共三種用途，優先權由高到低）：

1. 長按重置確認階段（`RESET_CONFIRM_BLINK`）——閃爍／長亮，直接接管 LED，
   `loop()` 本輪跳過 `updateBlink()`/`updateStatusLed()`，因為使用者正在操作，
   必須立即看到回饋
2. `updateBlink()`——一次性請求式閃爍（配對接受／拒絕）
3. `updateStatusLed()`——持續式狀態指示（BLE 配網／配對中／WiFi/MQTT 狀態）

**心跳不中斷**：3 秒計時階段與 2 秒閃爍確認階段，`loop()` 本身不阻塞，
`maintainEspNow()` 照常每輪執行、心跳維持 1 秒一次；唯一需要等待的最後 0.7 秒
長亮改用 `espNowDelay()` 而非裸 `delay()`，等待期間心跳同樣照常發出。
整個約 5.7 秒的長按流程，心跳幾乎沒有中斷——真正的空窗只發生在 `ESP.restart()`
之後到韌體重新開機完成之間，這與其他觸發重啟的路徑（例如 BLE 配網完成）同源，
不是本功能特有的風險。這段空窗實測約落在 **2.2~3.0 秒**（`setup()` 內
`delay(1000)` + `delay(50)` + `checkStuckButtons()` 的 500ms 取樣，再加上
ESP-NOW 初始化），並非「數百毫秒」這麼短；把長按流程本身的 5.7 秒也算進去，
總空窗約 3.3~4.0 秒，仍遠低於 slave 的 30 秒失聯門檻，安全結論不變。

**已知限制（MQTT 重連可能吃掉閃爍確認的視覺回饋）**：`loop()` 既有的 MQTT
重連（`smartConnect()`）最壞情況會阻塞約 18 秒，且這段**沒有按鈕逃生口**
（`connectToWiFi()` 的等待迴圈有 `anyResetButtonPressed()` 可提早跳出，
`smartConnect()` 這段沒有）。若使用者恰好在按下重置鈕的同一輪 `loop()` 觸發了
`smartConnect()`，會先卡住 18 秒才回到 `updateResetButton()`；下一次呼叫時
`pressDuration` 已遠超過 3+2 秒門檻，會在**同一次呼叫內**直接跳過閃爍確認、
判定滿 2 秒並執行重置——使用者全程看不到閃爍回饋，也沒有機會在閃爍階段放開取消。
心跳空窗仍受既有的 18 秒上限，低於 30 秒門檻，且誤觸風險有限（仍要連續按住
超過 18 秒），因此不是缺陷，但視覺回饋會消失，列為已知限制。

**已知限制（現場恢復手段）**：master 之前完全沒有現場恢復手段——長按重置
未實作、WiFi 密碼打錯時 MQTT 依定義連不上、也沒有 AP 模式 Web UI。本項補上
長按重置後，使用者配錯 WiFi 至少可以現場清除設定重新走 BLE 配網，不必拆下來
接 USB 重燒。AP 模式 Web UI 仍未實作，留待後續評估是否需要。

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
最壞阻塞 15 秒。

關聯方式是三段式，優先序由高到低：

1. 有 channel ＋ BSSID（上次成功關聯的記錄）→ 直接定向關聯，完全不掃描
2. 只有 channel（BSSID 已在前一次失敗時清掉）→ `WiFi.begin(ssid, password, ch, nullptr)`。
   依 ESP-IDF 文件（`wifi_sta_config_t.channel` 註解：「Set to 1~13 to scan
   **starting from** the specified channel before connecting to AP」），這是
   「以指定 channel **起始**掃描」，並非「鎖定在該 channel」。配合 Arduino core
   預設的 `WIFI_FAST_SCAN`（找到 SSID 即停）：AP **剛好在**該 channel 時會一擊命中、
   完全跳過後續掃描；但 AP **不在**該 channel 時，依文件字面意思仍可能從該 channel
   續掃其餘頻道 —— **這個機制細節只有文件推導、未經實機驗證，待確認**。
3. 兩者都沒有 → 才退回全頻掃描（該掃描即使不呼叫 `scanNetworks()` 仍會發生，
   全頻一輪約 20 秒）

即使情境 2 的「續掃」真的發生，也不代表 30 秒心跳空窗會出現：關聯窗口本身有
`HEARTBEAT_INTERVAL_ASSOC`（200ms）的加密心跳（15 秒約 75 則，全數落空機率約
0.25%），加上兩次關聯嘗試之間 `connectToWiFi()` 失敗分支會把射頻主動 park 回
`slaveLockChannel`（`esp_wifi_set_channel()` 為即時呼叫，必中），兩層疊加下
30 秒空窗理論上不會發生。

失敗時**只清 BSSID、保留 channel**，連續失敗達 `WIFI_CHANNEL_LOCK_MAX_FAIL`（10）次
才判定 AP 真的換頻、升級成一次全頻掃描並重新學習 channel。
關聯期間 `wifiAssociating` 為 true，心跳間隔由 1000ms 臨時縮到
`HEARTBEAT_INTERVAL_ASSOC`（200ms）。

## ESP-NOW 心跳的實際保證

slave 的失聯門檻是 30 秒，超過就強制關閉繼電器（動物管制設備＝籠子被打開），
所以「心跳空窗必須 < 30 秒」是本韌體的核心安全性質。但**心跳送出 ≠ 送達** ——
ESP-NOW peer 用 `channel = 0`（跟隨當前實體 channel），master 的 STA channel
與 slave 鎖定的 channel 不同時，心跳就打在錯的頻道上。因此保證分成兩個面向：

| 情境 | 心跳有沒有發 | 有沒有打在對的 channel | 最壞空窗 |
|---|---|---|---|
| BLE 配網中 | 有（`loop()` 只跳過 WiFi/MQTT 區塊） | 有，開機時由 `restoreEspNowChannelForOfflineBoot()` 從 NVS `homaster/espch` 切回（前提：NVS 已有該鍵） | < 1 秒 |
| WiFi 關聯中 | 有（等待迴圈走 `maintainEspNow()`） | 以已知 channel 起始掃描（AP 在該 channel 時一擊命中，不在時可能續掃，見上節）；連續失敗 10 次後的那一次全頻掃描例外 | < 1 秒（間隔已加密到 200ms） |
| MQTT 連線中 | **沒有** —— `mqttClient.connect()` 是不可中斷的阻塞呼叫 | 不影響 channel | **約 18 秒**（DNS `getaddrinfo()` 無 timeout 參數，由 lwIP `DNS_MAX_RETRIES` 決定約 15 秒＋TCP 3 秒） |
| MQTT 已連線但 socket 寫入卡住（Task 3 review 發現） | 有——`publishJsonDoc()` 在阻塞呼叫前後各補一次 `maintainEspNow()` | 不影響 channel | **約 10 秒**／每輪 loop()。`PubSubClient::setSocketTimeout(3)` 只管等 CONNACK 與 `readByte()`，對 `publish()` 完全無效；`publish()` 實際走 `NetworkClient::write()` 的 10 次重試 × 1 秒 `select()`，卡住時最壞吃滿 10 秒。典型觸發：AP 正常但 WAN 斷線，本地 socket 仍是 `ESTABLISHED`、`mqttClient.connected()` 仍回 true，代發照發，直到 TCP 送出緩衝塞滿。已用 `mqttPublishBudgetUsed` 名額守衛把每輪 loop() 限制在最多一次這種阻塞，見「代發 slave 狀態」一節 |
| `espNowDelay()` 各處等待 | 有 | 不影響 channel | < 1 秒 |

因此 `smartConnect()` 刻意設計成**一次呼叫只嘗試一台 broker**，由 `loop()` 的
10 秒重連節奏推進，確保兩次 18 秒阻塞之間必定隔著約 10 秒、約 10 則心跳。
絕對不要把它改回「一個 for 迴圈試完全部」—— 兩次背靠背就是 36 秒，直接跨過門檻。

**已知殘存風險**（不宣稱零風險）：

- 韌體剛燒錄／`homaster/espch` 尚未寫入時，開機進 BLE 配網模式只能停在 channel 1，
  鎖在其他 channel 的 slave 會先失聯再輪掃回來（序列埠會印 `⚠ [channel] …` 警告）
- 連續 10 次關聯失敗後的那一次全頻掃描，期間心跳命中率降到約 1/13
  （已用 200ms 加密心跳把 30 秒內全數落空的機率壓到 6×10⁻⁶ 量級）
- MQTT 阻塞的 18 秒是估算值上界，若某個 broker 的 DNS 行為更慢仍有超出的可能
- MQTT 已連線但 socket 寫入卡住的 10 秒同樣是估算值上界（`NetworkClient::write()`
  的重試次數與 `select()` 逾時皆為函式庫寫死的常數，實測若函式庫版本更新導致
  這兩個數字改變，需要重新推算）

## MQTT

### Topic

| Topic | 方向 | 說明 |
|---|---|---|
| `hoban/<deviceId>/status` | 發布 | master 自身狀態回報，QoS1、retain，每 10 秒一次；LWT 也發到這個 topic（`status:"offline"`） |
| `hoban/<deviceId>/control` | 訂閱 | App／使用者送控制指令 |
| `hoban/<slaveDeviceId>/status` | 發布（代發） | master 用每台 slave 的 MAC 代發它的狀態，retain。見下方「代發 slave 狀態」 |

`<deviceId>` 格式為 `hoban-<MAC位址>`，與 BLE 裝置名稱相同；`<slaveDeviceId>` 同格式，用 slave 自己的 MAC。

### 控制指令表

| 指令 | 行為 |
|---|---|
| `status` | 立即 `publishStatus()` 回報一次 |
| `ON` | `pulseRelay(2000)` 點動 master 自己的繼電器 2 秒，並回報狀態 |
| `OFF` | `setRelayPins(false)` 關閉 master 自己的繼電器，並回報狀態 |
| `reset` | 清除 NVS 網路設定並重啟，回到 BLE 配網模式 |
| `FIND_BEST_SERVER` | 主動斷開目前 MQTT 連線，把輪詢游標歸零（`resetMqttProbe()`）後重新走 `smartConnect()`。注意：`smartConnect()` 一次只試一台，第一台連不上時會由 `loop()` 每 10 秒推進一台，不會在單次呼叫裡試完全部 |
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

Phase 2a 尚未包含 `slaves` 陣列（各 slave 的個別狀態）；Phase 2b Task 1/2 已補上
（`appendSlavesArray()`），容量瓶頸細節見文末「已知風險」第三項。

### 代發 slave 狀態（Phase 2b Task 3）

master 除了自己的 `hoban/<deviceId>/status`，還會用**每台 slave 的 MAC** 代發一份
`hoban/hoban-<slaveMac>/status`，讓 slave 在 App 眼裡就是一台普通設備 —— App 現有
的設備卡片、詳情頁、「開保險→關門→關保險」三段鎖幾乎零改動就能個別控制每台 slave。

**payload 格式**（由 `publishSlaveStatus()` 組出）：

```json
{
  "device_id": "hoban-aabbccddeeff",
  "status": "online",
  "version": "1.0.0",
  "model": "hoSlave1",
  "via": "hoban-<masterMac>",
  "timestamp": 12345,
  "wifi": { "connected": true, "ssid": "ESP-NOW", "rssi": -72, "ip": "N/A" },
  "device": { "relay": 0 }
}
```

`wifi` 區塊刻意填成與一般設備相同的形狀，讓 App **兩個頁面（設備卡片／詳情頁）
各自的 `_handleMqttMessage` 手動解析**不用改就能吃下同一份 payload；`rssi` 借來
顯示 ESP-NOW 訊號強度；`via` 是新欄位，標示這台是被哪一台 master 代發的。

**review 更正**：這裡原本引用 `Device.updateFromMqttMessage()`，但那支函式在
`lib/` 沒有生產呼叫點，實際解析路徑是上述兩個頁面各自的手動解析，已更正。

**排程：每次 `loop()` 最多代發一台**（`slaveStatusScheduler()`）。

**review 更正（Critical）**：原本以為 `mqttClient.publish()` 卡住時的上界是
`setSocketTimeout(3)` 的 3 秒，逐層查了實際安裝的 `PubSubClient`／`NetworkClient`
原始碼後推翻——`setSocketTimeout()` 只寫入自己的成員變數，從未呼叫
`_client->setTimeout()`，這個值只用在 `connect()` 等 CONNACK 與 `readByte()`
（收包路徑），`publish()` 完全不經過它。`publish()` 實際走
`PubSubClient::write()` → `_client->write(buf, len)`，`NetworkClient::write()`
內部是 `retry = WIFI_CLIENT_MAX_WRITE_RETRY(10)` 迴圈，每輪 `select()` 的
`tv_usec` 硬編碼 1 秒；`send()` 帶 `MSG_DONTWAIT`，`setSocketTimeout()` 設的
`SO_SNDTIMEO` 對它無效。**單次 `mqttClient.publish()` 最壞卡 10 秒，不是 3 秒**
（見上方「ESP-NOW 心跳的實際保證」表格新增的一列）。觸發情境：AP 正常但 WAN
斷線，`NetworkClient::connected()` 仍回 true（本地 socket 還在 `ESTABLISHED`），
代發照發，直到 lwIP 的 `TCP_SND_BUF`（約 5.7KB）被塞滿後每次 write 都吃滿 10 秒。

master 自己 + 20 台 slave = 21 個 topic，若背靠背發布，21 個連發最壞可達
**210 秒**，遠遠撞破 slave 的 30 秒失聯門檻 ＝ 籠子被打開。因此排下三層防護：

1. **`slaveStatusScheduler()` 每次呼叫最多發一台**，不會一次全發。
2. **`mqttPublishBudgetUsed` 名額守衛**：`loop()` 每輪開頭重置，`publishJsonDoc()`
   真正呼叫 `mqttClient.publish()` 前會佔用這個旗標；本輪已用掉的話，
   `publishStatus()`／`slaveStatusScheduler()`／`processPendingUnpairPublish()`
   之中排在後面的呼叫方一律讓位給下一輪，**保證每輪 loop() 最多只發生一次
   會阻塞的 publish**，把原本可能疊加成 20~30 秒的空窗壓回單次最壞 10 秒。
3. **`publishJsonDoc()` 內部在阻塞呼叫前後各補一次 `maintainEspNow()`**，
   成本近乎零，確保進入 10 秒黑箱前剛發過心跳、出來立刻再發一次。

一輪例行輪播「大約」15 秒（`SLAVE_STATUS_CYCLE_MS`），與 `pollNextSlave()`
更新 slave 資料的節奏對齊 —— 代發比資料更新還快是純浪費頻寬。**review 更正**：
這只是兩個各自獨立、各自用 `millis()` 起算的 static 計時器，沒有同步機制保證
相位對齊，代發帶到的資料最舊可能落後一整輪（約 15 秒），不是精確同步。
20 台滿載時，輪播間隔是 `15000 / slaveCount ≈ 750ms`，即**每 750ms 輪到一台**
（不是「每台 750ms 被輪到一次」——每台實際仍是約 15 秒才會輪到自己一次）；
狀態有變化（上下線翻轉、繼電器變化、版本回報）時會設 `dirty` 立刻插隊代發，
不必等輪播輪到。

**dirty 連續失敗退避**：一台持續發布失敗的 slave（區別於「名額被讓位」——
`slaveStatusScheduler()` 一發現本輪名額已用掉就整個跳過，不會計入失敗）
若不處理會讓排程器每次都優先重試同一台，其他台的 `rotateIdx` 永遠推進不到、
被餓死。連續失敗滿 `SLAVE_DIRTY_MAX_FAIL`（3）次後，暫停該台
`SLAVE_DIRTY_BACKOFF_MS`（30 秒），讓其他台優先，見 `SlaveEntry.dirtyFailCount`
／`dirtyBackoffUntil`。

**離線代發與已知限制**：slave 超過 30 秒沒回應時，`updateSlaveOnlineStatus()`
會把它標記離線並設 `dirty`，下一輪代發就會送出 `status:"offline"`，避免 App
一直顯示上線。解除配對時（無論是序列埠 `unpair <n>` 還是 slave 主動送
`HO_PKT_UNPAIR`）也會補發一次 offline，否則 broker 上會留下永遠在線的幽靈設備。
序列埠 `unpair <n>` 這條路徑上，`publishSlaveOffline()` 可能阻塞最壞 10 秒，
期間若另一台 slave 經 ESP-NOW 主動解除配對造成陣列搬移，`unpairSlave()`
會在阻塞呼叫後改用先前存好的 MAC 重新 `findSlave()`，不會用過期的索引刪錯人。

**但這只涵蓋「master 活著、slave 失聯」的情況** —— PubSubClient 一條連線只有
**一個 LWT（遺囑）名額**，已經給了 master 自己（`hoban/<deviceId>/status`）。
**master 自己斷電或失去網路時，所有 slave 會停在最後一則 `"online"` 保留訊息**，
不會被代發成 offline（沒有 loop() 可以跑，代發機制本身也停了）。這個缺口
由 App 端用 `via` 欄位彌補：master 若本身離線，其底下所有 slave 都應視為
狀態不明，不能單看 slave 自己那則 retain 的 `"online"` 就判斷實際在線。

## 與 ho_relay2 的差異

| 面向 | `ho_relay2` | `ho_master1` |
|---|---|---|
| ESP-NOW | 無 | 全程維持。配網／WiFi 重連／MQTT 重連期間心跳照常發出，且會盡量停在 slave 鎖定的 channel 上，把心跳空窗壓在 slave 的 30 秒失聯門檻以下（實際保證與殘存風險見下方「ESP-NOW 心跳的實際保證」） |
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

實測 1,682,707 / 2,031,616 bytes（app0 分區，82.8%），餘裕僅約 341KB。
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

### 3. 狀態 JSON 的真正容量瓶頸是 `char buf[512]`

不是 `mqttClient` 的 1024 buffer。`publishStatus()` 目前實測最壞情況約 317 bytes，
餘裕僅約 195 bytes。真正會**截斷**輸出的邊界是 `char buf[512]`：`serializeJson()`
寫入固定大小 buffer 時確實會截斷，且截斷後的長度仍小於 `mqttClient` 的 1024
buffer，`publish()` 因此仍會回傳 `true`。`publishStatus()` 的 `if (!res)` 診斷
只抓得到「連 1024 都塞不下」的極端案例，所以另外補了 `n >= sizeof(buf) - 1`
這道有效的截斷偵測。

**`doc.overflowed()` 在本專案的 ArduinoJson 7.4.3 抓不到「超過 512 bytes」**：
這個版本的 `StaticJsonDocument<512>` 只是 `compatibility.hpp` 提供的相容殼
（`class StaticJsonDocument : public JsonDocument`），`512` 這個 N 完全被忽略、
底層一律動態配置記憶體。所以 `doc.overflowed()` 量的是「記憶體配置失敗」，
不是「內容超過容量」——正常情況下不會因為 JSON 內容大小觸發，只有 heap 真的
配置不出來時才會回 `true`。仍保留這道檢查（配置失敗本身值得知道），但序列埠
警告訊息已改用「記憶體配置失敗」的措辭，不再說「超出 512 容量」。

Phase 2b 加入 `slaves` 陣列（每台 slave 的個別狀態）時，**必須同時放大兩處**：
`char buf[512]`、以及 `mqttClient.setBufferSize(1024)`，兩者任一沒跟著放大都會讓
新加的欄位被靜默截斷或整包發布失敗。`StaticJsonDocument<512>` 的 `512` 在
ArduinoJson 7.x 已無容量作用（見上段說明），要改的話純粹是為了可讀性、
讓數字與 `buf`/`setBufferSize` 對齊，不是為了擴充容量，不改也不影響功能。

## 編譯與燒錄

```powershell
.\flash.ps1 -Model master              # WROOM 版，只編譯
.\flash.ps1 -Model master -Upload      # WROOM 版，編譯並燒錄
.\flash.ps1 -Model master-c3           # C3 版，只編譯
.\flash.ps1 -Model master-c3 -Upload   # C3 版，編譯並燒錄
```
