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
| `state <n>` | 要求第 n 台回報狀態（走 `requestSlaveStateIndex()`，先取 MAC 值再送） |
| `unpair <n>` | 解除第 n 台配對 |
| `unpairall` | 清空整份名冊（分批執行，每輪 `loop()` 拆一台，心跳不中斷） |
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
| MQTT 已連線但 socket 寫入卡住（Task 3 review 發現） | 有——`publishJsonDoc()` 在阻塞呼叫前後各補一次 `maintainEspNow()` | 不影響 channel | **約 10 秒**／每輪 loop()。`PubSubClient::setSocketTimeout(3)` 只管等 CONNACK 與 `readByte()`，對 `publish()` 完全無效；`publish()` 實際走 `NetworkClient::write()`：**10 次重試 × 1 秒 `select()`，但部分寫入會重置計數，故非硬上限**（見下方殘存風險）。典型觸發：AP 正常但 WAN 斷線，本地 socket 仍是 `ESTABLISHED`、`mqttClient.connected()` 仍回 true，代發照發，直到 TCP 送出緩衝塞滿。已用 `mqttPublishBudgetUsed` 名額守衛把每輪 loop() 限制在最多一次這種阻塞，見「代發 slave 狀態」一節 |
| `mqttClient.loop()` 內部的 keepalive `PINGREQ`（Task 4 起明確夾住） | 有——`loop()` 在 `mqttClient.loop()` 前後各補一次 `maintainEspNow()` | 不影響 channel | **約 10 秒**（同一套 `NetworkClient::write()`，同樣非硬上限）。`setKeepAlive(30)` 下**每 30 秒觸發一次，是例行事件不是偶發**：`PubSubClient::loop()` 的條件是 `(t - lastInActivity > keepAlive*1000) \|\| (t - lastOutActivity > ...)`，是 OR，而 `lastInActivity` 只在真的**收到**封包時更新，broker 平時不主動送東西，所以即使我們每 10 秒 publish 一次也擋不住它 |
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
- **socket 寫入卡住的 10 秒不是硬上限，病態情況下可以超過 30 秒門檻，
  而且目前沒有任何防護。** 這是本韌體已知的**最嚴重殘存風險**，
  請不要被下一段的「約 11 秒」誤導 —— 那個數字的前提正是「單段黑箱 ≤ 10 秒」，
  前提不成立時結論就不成立。
  - **機制**（Task 3 review N1）：`NetworkClient::write()` 的迴圈在
    「`send()` 回傳 > 0 但還沒寫完」的分支裡會執行
    `retry = WIFI_CLIENT_MAX_WRITE_RETRY;`，把重試計數器**重置回 10**。
    「10 次重試 × 1 秒 `select()` ＝ 10 秒」只涵蓋「**完全**寫不進去」的情況；
    每輪只擠得出幾個 byte 的病態 socket（例如對端 TCP 視窗趨近於零、
    WAN 極度壅塞）可以無限期地反覆重置計數器。
  - **後果**：單段心跳空窗超過 slave 的 30 秒失聯門檻 ＝ slave 強制關閉繼電器
    ＝ **籠門被打開**。`maintainEspNow()` 的前後括號在這種情況下幫不上忙，
    因為函式庫內部的 `write()` **不可中斷**，我們根本拿不回控制權。
  - **目前的處置**：無。要封死只能 fork `PubSubClient`（在它的 `write()`
    內部插心跳）或換成非阻塞的 MQTT 客戶端，兩者都超出 Phase 2b 範圍。
    現行的三／四層防護（見「代發 slave 狀態」）只能保證
    **「每輪 loop() 最多發生一次這種黑箱」**，不能保證**單次黑箱有多長**。
  - 另外重試次數與 `select()` 逾時皆為函式庫寫死的常數，函式庫版本更新導致
    這兩個數字改變時需要重新推算
- **在「單段黑箱 ≤ 10 秒」成立的前提下**，單輪 `loop()` 的最壞心跳空窗約
  **11 秒**（10 秒黑箱 ＋ 最多 1 秒的心跳間隔餘裕——`maintainEspNow()` 只有在
  距上次心跳滿 `HEARTBEAT_INTERVAL` 時才真的送）。Task 3 的報告與 commit
  message 寫「約 20 秒」是**過度悲觀且錯誤**的推演——它把 `PINGREQ` 黑箱與
  `publish()` 黑箱視為連續的一段空窗，但兩者之間必定隔著一次
  `maintainEspNow()`，所以是兩段各約 10 秒的獨立視窗，不是疊加。
  Task 4 進一步把 `mqttClient.loop()` 前後也明確夾上 `maintainEspNow()`，
  讓這個切分從「呼叫順序的巧合」變成結構性保證（**不會讓 11 秒變小**）。
  **再次強調：11 秒是有前提的推演值，不是保證；前提本身見上一條。**

## MQTT

### Topic

| Topic | 方向 | 說明 |
|---|---|---|
| `hoban/<deviceId>/status` | 發布 | master 自身狀態回報，QoS1、retain，每 10 秒一次；LWT 也發到這個 topic（`status:"offline"`） |
| `hoban/<deviceId>/control` | 訂閱 | App／使用者送控制指令 |
| `hoban/<slaveDeviceId>/status` | 發布（代發） | master 用每台 slave 的 MAC 代發它的狀態，retain。見下方「代發 slave 狀態」 |
| `hoban/<slaveDeviceId>/control` | 訂閱（代訂） | master 代名冊上每一台 slave 訂閱，收到的指令轉成 ESP-NOW 送給對應的 slave。見下方「代訂閱與指令轉發」 |

`<deviceId>` 格式為 `hoban-<MAC位址>`，與 BLE 裝置名稱相同；`<slaveDeviceId>` 同格式，用 slave 自己的 MAC。

### 控制指令表（master 自己的 `hoban/<masterId>/control`）

| 指令 | 行為 |
|---|---|
| `status` | 立即 `publishStatus()` 回報一次 |
| `ON` | `pulseRelay(2000)` 點動 master 自己的繼電器 2 秒，並回報狀態 |
| `OFF` | `setRelayPins(false)` 關閉 master 自己的繼電器，並回報狀態 |
| `reset` | 清除 NVS 網路設定並重啟，回到 BLE 配網模式 |
| `FIND_BEST_SERVER` | 主動斷開目前 MQTT 連線，把輪詢游標歸零（`resetMqttProbe()`）後重新走 `smartConnect()`。注意：`smartConnect()` 一次只試一台，第一台連不上時會由 `loop()` 每 10 秒推進一台，不會在單次呼叫裡試完全部 |
| `HASRELAY:ON` / `HASRELAY:OFF` | 設定「本機是否有接繼電器」並存 NVS，回報狀態 |
| `ALL:ON` | `sendCmdToAll(HO_CMD_PULSE, 2000)` —— **廣播點動 2 秒，不是持續 ON**，語義與單台 `ON` 一致；含 master 自己的繼電器。**這就是 App「全部關門」按鈕實際送的指令** |
| `ALL:OFF` | `sendCmdToAll(HO_CMD_OFF, 0)` —— 把所有繼電器**持續斷開**，含 master 自己的。**App 不送這條**（全 repo 的 `lib/`／`test/` 沒有任何一處），目前只有序列埠 `alloff` 會走到 |
| `SLAVES` | `markAllSlavesDirty()` ＋ 一次 `publishStatus()`。名冊會在接下來一輪內逐台重壓保留訊息，不在這裡連發 20 則 |
| `PAIR:START` | `enterPairingMode()`，開一個 60 秒配對視窗（`PAIRING_TIMEOUT`，與 App 的 60 秒倒數對齊）。**期間可連續配對多台，配對成功不會自動退出** |
| `PAIR:STOP` | `exitPairingMode()` |
| `UNPAIR:<deviceId>` | 解除指定 slave（`hoban-xxxxxxxxxxxx`）。ID 格式錯誤或不在名冊上都只印一行，不動作 |
| `UNPAIRALL` | 清空整份名冊。**只插旗，實際拆除由 `loop()` 的 `processUnpairAll()` 每輪拆一台**（理由見下方「UNPAIRALL 為什麼必須分批」） |
| `LR:*` | 尚未實作（Phase 2b Task 6），印一行明確回應而不是掉進「未知指令」 |

目前**尚未支援** `update:{JSON}` OTA 指令 —— 那是 Phase 4 才加。

### 群組指令：廣播（同時）＋ 逐台單播（唯一可證明的送達）

本系統是**多開門的捕捉系統，核心需求是「一次要全部關」**，關門失敗＝動物逃脫或
未被捕捉；**「全部關」的可靠性優先於「全部開」**，且**誤紅可接受、誤綠不可接受**。

> **實際的關門路徑是 `ALL:ON`，不是 `ALL:OFF`。**
> App 的「全部關門」按鈕送的是 `ALL:ON`（見 hoctrl 的
> `lib/pages/device_detail_page.dart` 的 `_sendGroupCloseCommand()`）——
> 因為韌體規格裡 `ALL:ON` ＝「廣播 pulse」＝觸發一次關門動作，與單機 `ON` 一致；
> `ALL:OFF` 是「把所有繼電器持續斷開」，不是關門。全 App 沒有任何一處送 `ALL:OFF`。
> 韌體的關門警語因此掛在 `HO_CMD_PULSE`（與 `HO_CMD_OFF`）上。

`sendCmdToAll()` 因此**不是**純逐台單播（20 台會有 400ms 落差，不叫「同時」），
而是兩段：

| 段 | 動作 | 位置 |
|---|---|---|
| 1 | **廣播** `HO_PKT_CMD` 到 `FF:FF:FF:FF:FF:FF` 連送 3 次、間隔 20ms（同時性），master 自己的繼電器同步動作 | `sendCmdToAll()`（inline） |
| 2 | **對每一台都送一次單播**，間隔 20ms（可證明的送達）。刻意 inline，理由見下 | `sendCmdToAll()`（inline） |
| 3 | 未取得 MAC 層 ACK 的**補送單播**，每輪 `loop()` 一台，最多 2 趟 | `processGroupCmd()` |

廣播零新基礎設施也不會誤觸發別人的設備：廣播 peer 在 `setupEspNow()` 就註冊好、
心跳本來就走廣播；slave 的
`if (!masterKnown || memcmp(masterMac, info->src_addr, 6) != 0) return;`
擋在 `HO_PKT_CMD` 分支之前，而 **ESP-NOW 廣播幀的 `src_addr` 仍是發送端的真實
MAC**（廣播只換目的位址），這道檢查對廣播與單播一體適用。

#### 只宣稱可證明的事：「已送達」可證明，「已執行」不可證明

這是本設計最重要的一條，也是第一版被 review 判為 **C1 假綠燈**之後重寫的結果。

第一版試圖證明「指令已被執行」：靜置後看 `slaves[i].relay` 是不是預期值，並用
master 自己送的 `HO_PKT_STATE_REQ` 去「製造」一則新回報當證據。**那是假綠燈。**
對 `HO_CMD_OFF` 而言，「新回報」是我們自己那則查詢造成的，而 `relay == 0` 是
slave 的**靜止預設值**（開機、點動結束、`loadSlaves()`、`addSlave()` 全都初始化
成 0）。兩半都不是證據，AND 起來仍然不是證據 —— 一台從未收到 OFF 的 slave 會被
判成「已確認」，而且確認是 sticky 的，假確認一旦成立就再也不會補送。
「收不到廣播、卻收得到單播」正是邊界訊號那台的典型表現。

**根因是結構性的：`HoStatePayload` 沒有任何指令歸因欄位**（沒有「我剛執行了哪一則
指令」的 seq／echo），所以現行協定下「指令有被執行」**原理上無法證明**。

因此現在只區分兩件事：

| 事實 | 可否證明 | 依據 |
|---|---|---|
| **已送達** | ✅ 可證明 | 單播有 MAC 層 ACK。`esp_now_send()` 的送出回呼 `onEspNowSent()` 逐幀回報成功／失敗，`wifi_tx_info_t::des_addr` 帶目的 MAC，可逐台歸因 |
| **已執行** | ❌ 不可證明 | 協定沒有指令歸因欄位。**不准用任何自製證據去宣稱它** |
| 廣播是否被收到 | ❌ 不可證明 | 廣播沒有 ACK，`esp_now_send()` 永遠回報成功 |

> **技術債：指令歸因欄位交 Phase 4 Task 1** 一併處理。那個 Task 本來就要動協定
> （CRC 涵蓋標頭），是已排定的 flag-day、兩端同時重燒。在那之前不為此單獨改協定。

#### ACK 歸因閂鎖

`onEspNowSent()` 會被**每一次** `espNowSendTo()` 觸發，包含心跳（廣播）、
`HO_PKT_STATE_REQ`（單播！）、`HO_PKT_UNPAIR`（單播）。若不加限制，一則
`STATE_REQ` 的 ACK 會被誤記成「群組指令已送達」—— 與 C1 同一類的錯誤歸因。
所以只在「剛送出群組單播、還沒收到它的回呼」的窗口內開閂，並比對目的 MAC。

**同 MAC 單播的完整清單（第 4 輪 review 更正）。** 上一版寫「剩下的同 MAC 送出
只有 `HO_PKT_UNPAIR`」**是錯的**，實際上有三條，而且後兩條那台**仍在名冊上**
（`gone` 論證不適用）：

| 來源 | 狀態 |
|---|---|
| `HO_PKT_UNPAIR`（`unpairSlave()`） | 守住了 —— 那台會被 `groupRefreshRoster()` 標成 `gone`，而 `gone` 在 `groupCountAll()` 與 `groupDeliveryFor()` **都優先於** `delivered` |
| `pollNextSlave()` 的 `HO_PKT_STATE_REQ` | 守住了 —— `loop()` 用 `groupCmdActive()` 整個讓開 |
| `handleSlaveCommand()` 的 `status` 分支 | 守住了 —— 跳過 `requestSlaveStateIndex()`，`publishSlaveStatus()` 照發，App 不空等 |
| **序列埠 `state <n>`** | 上一版**漏了**。特別諷刺：回歸清單 8a 的校準步驟正好教操作者用它 ——**驗收程序自己製造危害**。**第 4 輪已補上同樣的 `groupCmdActive()` 守衛** |
| **`HO_PKT_PAIR_ACK`**（`onEspNowRecv()` 的 `HO_PKT_PAIR_REQ` 分支） | **擋不掉也不該擋**：配對請求必須回覆，且它跑在 WiFi task。`ho_slave1` 的 `requestPairing()` 沒有「已配對就不送」守衛，所以已配對的 slave 理論上能在那 1~2ms 內送 `PAIR_REQ` 進來 |

最後那條的誠實評估：它會讓證據的**指向性**變差（拿 `PAIR_ACK` 的 ACK 去認 `CMD`
的送達），但**不是 C1 那種自製證據** —— MAC 層 ACK 仍由對方射頻產生、master 造不
出來，所以那台當下確實可達。量級上需要同一台、在同一個 1~2ms、剛好送出
`PAIR_REQ`。**沒有實機驗證過。**

歸因失敗一律往**誤紅**方向掉（回呼太晚到 → 這台被當成未送達 → 多補送一次），
不會往誤綠掉。

**「job 之外一律拒絕寫入」是結構而不是約定（第 4 輪 review）。**
`groupNoteUnicastAck()` 開頭多一道 `if (!groupCmdActive()) return;`，
把「只有群組單播才會開閂」從每個開閂點的自律變成結構保證，順帶殺掉「跨 job 的
過期回呼」那條理論殘留。

> **這一行的位置有陷阱。** `groupCmdSnapshot()` 原本把 phase 設成 `IDLE`，而
> `sendCmdToAll()` 要到 inline 第一趟單播跑完才設 `WAIT` —— 照抄會讓**第一趟
> 全程 `groupCmdActive()` 為 false**，ACK 全被丟掉，變成**大規模誤紅**。
> 所以先新增 `GROUP_JOB_ARMED` 階段、把「已啟動」的時點前移到快照完成的當下，
> 再加守衛。
>
> 附帶收穫：那 400ms 內的兩道讓路守衛（`pollNextSlave()`／`handleSlaveCommand()`）
> **原本其實是失效的**，只因為 `espNowDelay()` 只跑 `maintainEspNow()`、不跑
> `loop()` 也不跑 `mqttClient.loop()`，那兩條路徑實務上進不來 ——
> **那是巧合不是設計**。時點前移之後，它們在那 400ms 內是真的成立。

**閂一定要關得掉（review N1）。** 只防「開閂那一端」是不夠的：
`groupSendUnicast()` 是先開閂再送，而 `sendCmdToSlaveMac()` 在
「已不在名冊上」或 `esp_now_send()` 回錯時**根本不送**，那次就沒有回呼來關閂。
閂若跨過收工繼續開著，收工當輪恢復的 `pollNextSlave()` 所送的 `HO_PKT_STATE_REQ`
（單播）只要命中同一個 MAC，就會把送達旗標翻成 `true` —— 而送達旗標在 job 結束後
**仍持續被 MQTT 讀取**。結果是**序列埠誠實印過「未送達」，下一則 status 卻自己
把 `grp` 由 0 翻成 1、`noack` 減成 0**，誤綠方向。兩道防線：

1. `sendCmdToSlaveMac()` 現在回傳「有沒有真的交給 `esp_now_send()`」，
   `groupSendUnicast()` 送不出去就**當場關閂**（根因）。
2. `groupFinishJob()` 無條件 `groupAckArmed = false`（兜底）。

回歸清單第 8a／8a-2 項的「收工後 40 秒內 `grp` 不得由 0 翻成 1」就是這條的守衛。

#### wall-clock 硬上限 `GROUP_JOB_MAX_MS = 6000`

job 的長度＝趟數 ×（台數 × 每步間隔 ＋ 等待），而「每步」是一次 `loop()` 迭代 ——
只要 `loop()` 被任何一個 10 秒級的阻塞 socket 寫入拖慢，整個 job 就會被拉長到
不可預期。而 job 期間 `pollNextSlave()` 是讓開的，輪詢被餓死超過 30 秒就會讓
`updateSlaveOnlineStatus()` **把全部 slave 誤判離線**。
6 秒是刻意選的：輪詢週期 15 秒 ＋ 最多 6 秒停擺 ＝ 21 秒 < 30 秒門檻，留 9 秒餘裕。
時間到就立刻收工並據實回報，不再補送。

#### 第 2 段為什麼 inline

`handleMasterCommand()` 的 `ALL:*` 分支結尾就是 `publishStatus()`，而單次阻塞
publish 最壞是 10 秒級黑箱（**App 端依賴那則 `publishStatus()`，不能拿掉** ——
`device_detail_page.dart` 明講「沒有排 +2 秒那一次，因為韌體 `ALL:ON` 的分支結尾
本來就會 `publishStatus()`」）。若第一趟單播排在它後面，「一次要全部關」的補強會被
整整延後 10 秒。代價是最多 20 × 20ms ＝ 400ms 的 inline 阻塞，全程走
`espNowDelay()`，心跳與點動結束檢查照跑，遠低於 30 秒門檻。

#### 收工輸出

序列埠一定看得見，且**任何情況都會印**「不能證明已執行」那兩行：

```
[群組] 指令 <N> 收工：單播 MAC 層已送達 X／Y 台
⚠ [群組] Z 台連「送達」都沒有（單播沒拿到 MAC 層 ACK）
⚠ [群組]   未送達：hoban-xxxxxxxxxxxx
⚠ [群組] W 台在執行期間離開名冊，同樣未送達
[群組] 注意：MAC 層 ACK 只證明「封包已送達」，不能證明繼電器真的動作
[群組] 現行 HoStatePayload 沒有指令歸因欄位，「已執行」在本版協定下無法證明（技術債：Phase 4 Task 1 補）
⚠ [群組] 這是關門路徑：未送達的籠門必然沒關；已送達的也只代表封包到了，一律以現場確認為準，不要當成已關閉
```

達 wall-clock 上限時第一行會多一段「（達 wall-clock 上限）」。

#### 收工判定必須進 MQTT（review M2）

只印序列埠等於 App 沒有任何依據顯示紅色。所以：

- master 狀態的 `slaves[]` 每一筆多一個 **`"grp"`**：`1` ＝ 單播拿到 MAC 層 ACK
  （**只是送達**），`0` ＝ 沒拿到（或指令期間離開名冊），欄位不存在 ＝ 這台不在
  最近一次群組指令的快照裡。單台的 `hoban/<slaveId>/status` 也在 `device.grp` 帶同一個值。
- master 狀態多一個 **`"group"`** 摘要物件：

```json
"group": {"cmd":3,"age_s":2,"busy":0,"n":20,"ack":18,"noack":2,"gone":0,
          "exec":"unprovable"}
```

`noack` 就是 App 該顯示紅色的依據。`exec` 固定 `"unprovable"`，用來擋掉
「App 把 `ack` 當成關門成功」這條誤讀路徑。

> **舊版 App 相容**：`SlaveStatus.fromJson()` 只挑它認得的 key，多出來的欄位會被
> 忽略、不會解析失敗（hoctrl 的 `lib/models/slave_status.dart`）。
>
> **App 端目前仍讀 `online` 而不是 `grp`**（`master_slave_logic.dart` 的
> `evaluateGroupCloseProgress()` 用 `slave.online`）。韌體這邊先把依據送上去，
> App 端改讀 `grp`／`group.noack` 是另一個 Task。

### UNPAIRALL 為什麼必須分批

一口氣跑完的版本是 `while (slaveCount > 0) unpairSlave(slaveCount - 1);`。
`unpairSlave()` 內含 `espNowDelay(100)` 與一次 `publishSlaveOffline()`，而單次阻塞
publish 最壞是 10 秒級黑箱 —— 20 台合計最壞**超過 60 秒沒有心跳**，直接撞破 slave
的 30 秒失聯門檻，等於為了清名冊而把所有籠門一次打開。

改成 `unpairAllPending` 旗標 ＋ `loop()` 的 `processUnpairAll()` 每輪拆一台：
`espNowDelay()` 期間心跳照發，publish 又受 `mqttPublishBudgetUsed` 名額管轄
（每輪最多一次），全程心跳不中斷。從**最後一台往前拆**，`unpairSlave()` 內部的
陣列前移就不會搬動任何元素。

### 代理指令表（slave 的 `hoban/<slaveId>/control`，Phase 2b Task 4）

| 指令 | 行為 |
|---|---|
| `ON` | `sendCmdToSlaveMac(mac, HO_CMD_PULSE, 2000)` —— **點動 2 秒，不是持續開啟** |
| `OFF` | `sendCmdToSlaveMac(mac, HO_CMD_OFF, 0)` |
| `status` | 先用名冊上目前已知的狀態立刻代發一則（`publishSlaveStatus()`），同時 `requestSlaveState()` 向 slave 要一次最新狀態，回來時 `dirty` 會觸發第二則代發 |
| 其他 | 序列埠印 `[代理] <slaveId> 不支援的指令: …`，不做任何動作 |

`ON` 送 `HO_CMD_PULSE` 而不是 `HO_CMD_ON` 是刻意的：App 對一般 hoRelay 設備送的
`ON` 語義是「開門」＝點動一次，master 自己的 `ON` 分支也是 `pulseRelay(2000)`。
slave 要在 App 眼裡是一台普通設備，語義就必須完全一致，否則
「開保險 → 關門 → 關保險」三段鎖流程對 slave 的行為會與其他設備不同。
**持續開啟只保留給序列埠的 `on <n>`**（現場除錯用）。

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
`SO_SNDTIMEO` 對它無效。**單次 `mqttClient.publish()` 的典型上界是 10 次重試
× 1 秒 `select()` ＝ 10 秒，不是 3 秒；而且部分寫入會重置重試計數，所以 10 秒
不是硬上限**（見上方「ESP-NOW 心跳的實際保證」表格與其下方的殘存風險）。觸發情境：AP 正常但 WAN
斷線，`NetworkClient::connected()` 仍回 true（本地 socket 還在 `ESTABLISHED`），
代發照發，直到 lwIP 的 `TCP_SND_BUF`（約 5.7KB）被塞滿後每次 write 都吃滿 10 秒。

master 自己 + 20 台 slave = 21 個 topic，若背靠背發布，21 個連發最壞可達
**210 秒**，遠遠撞破 slave 的 30 秒失聯門檻 ＝ 籠子被打開。因此排下三層防護：

1. **`slaveStatusScheduler()` 每次呼叫最多發一台**，不會一次全發。
2. **`mqttPublishBudgetUsed` 名額守衛**：`loop()` 每輪開頭重置，`publishJsonDoc()`
   真正呼叫 `mqttClient.publish()` 前會佔用這個旗標；本輪已用掉的話，
   `publishStatus()`／`slaveStatusScheduler()`／`processPendingUnpairPublish()`
   之中排在後面的呼叫方一律讓位給下一輪，**保證每輪 loop() 最多只發生一次
   會阻塞的 socket 寫入**，把原本可能疊加成 20~30 秒的空窗壓回單次約 10 秒。
   **Task 4 review（M1）把這個名額的涵蓋範圍從 `publish()` 擴大到
   `subscribe()`／`unsubscribe()`**（三者走同一條 `NetworkClient::write()`），
   取用點是 `publishJsonDoc()`、`controlSubscribeScheduler()`、
   `unsubscribeSlaveControlTopic()`。
   **注意這只保證「一輪最多一次」，不保證「單次有多長」** ——
   單次的長度沒有硬上限，見上方「ESP-NOW 心跳的實際保證」下的殘存風險。
3. **`publishJsonDoc()` 內部在阻塞呼叫前後各補一次 `maintainEspNow()`**，
   成本近乎零，確保進入 10 秒黑箱前剛發過心跳、出來立刻再發一次。
4. **（Task 4 新增）`loop()` 也把 `mqttClient.loop()` 前後夾上 `maintainEspNow()`**，
   把函式庫內部每 30 秒一次的 keepalive `PINGREQ` 黑箱同樣切成獨立視窗。
   **這不會讓最壞空窗變小**（單輪仍約 11 秒），但它讓「`PINGREQ` 黑箱與
   `publish()` 黑箱之間必定隔著一次心跳」這件事從呼叫順序的巧合變成結構性保證。

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
序列埠 `unpair <n>` 這條路徑上，`publishSlaveOffline()` 可能阻塞約 10 秒，
期間若另一台 slave 經 ESP-NOW 主動解除配對造成陣列搬移，`unpairSlave()`
會在阻塞呼叫後改用先前存好的 MAC 重新 `findSlave()`，不會用過期的索引刪錯人。

**但這只涵蓋「master 活著、slave 失聯」的情況** —— PubSubClient 一條連線只有
**一個 LWT（遺囑）名額**，已經給了 master 自己（`hoban/<deviceId>/status`）。
**master 自己斷電或失去網路時，所有 slave 會停在最後一則 `"online"` 保留訊息**，
不會被代發成 offline（沒有 loop() 可以跑，代發機制本身也停了）。這個缺口
由 App 端用 `via` 欄位彌補：master 若本身離線，其底下所有 slave 都應視為
狀態不明，不能單看 slave 自己那則 retain 的 `"online"` 就判斷實際在線。

### 代訂閱與指令轉發（Phase 2b Task 4）

代發狀態的反方向：master 代名冊上每一台 slave 訂閱 `hoban/<slaveId>/control`，
收到的 MQTT 純文字指令轉成 ESP-NOW 封包送給對應的 slave（指令內容見上方
「代理指令表」）。slave 沒有 WiFi，master 是它與 App 之間唯一的橋。

**topic 比對：解析出 MAC → 查名冊，不維護 topic 字串表。**
Phase 2a 的 `mqttCallback()` 是把 topic 與自己的 control topic 做完整字串相等，
現在同時代理最多 20 台，一台一條字串去比對等於要維護一份 21 條的 topic 表
（21 × 33 ＝ 693 bytes RAM），而且配對／解除配對時得同步維護 ——
那是名冊之外的第二份真相，遲早會不一致。名冊已經是唯一真相。
`parseControlTopic()` 是純長度檢查 + hex 解析（O(1)），之後 `findSlave()`
最多 20 次 6 bytes 的 `memcmp` ＝ 微秒級，而且只在收到 MQTT 訊息時才跑。

**逐台訂閱，不用萬用字元 `hoban/+/control`。**
預設清單裡有三台是公用 broker（emqx.io、hivemq.com、eclipseprojects.io），
`hoban/+/control` 會收到全世界所有 hoban 設備的控制訊息 —— 雖然
`findSlave()` 會全部過濾掉，但流量與被動接收他人指令的風險完全不必要。

**訂閱動作一律回到 `loop()` context 才做。**
`mqttClient.subscribe()`／`unsubscribe()` 會動 socket，與 `loop()` 裡的
`mqttClient.loop()` 是明確的競態。而配對成功（`addSlave()`）發生在
`onEspNowRecv()` 裡，屬 WiFi task —— 理由與代發狀態不能在 callback 裡直接發
完全相同。所以 `addSlave()` 只設 `pendingSubscribeRefresh = true;`，
`loop()` 看到旗標才排隊一次全量對齊
（重複訂閱同一個 topic 對 broker 是冪等的，所以不必記錄是哪一台）。

**訂閱一次只做一格，與 publish 共用同一個名額（review M1 修正）。**
`subscribe()` 與 `publish()` 走的是**同一條** `NetworkClient::write()`，
是同一個 10 秒級黑箱。第一版把 21 條 topic 在單次呼叫裡一口氣訂完 ——
病態 socket 下單輪 `loop()` 最壞凍結約 **210 秒**。心跳因為有
`maintainEspNow()` 括號所以不會斷（籠門不會被誤開），但這 210 秒內 master
**收不到也發不出任何 MQTT 指令**，而本系統的核心需求是「一次要全部關」，
關不了就是動物逃脫。

因此：

- `mqttPublishBudgetUsed` 的語義**擴大為「本輪 `loop()` 的阻塞式 socket 寫入
  名額」**，涵蓋 `publish()`／`subscribe()`／`unsubscribe()` 三者
  （旗標名稱維持 Task 3 取的名字，避免跨文件的指涉斷裂）。
- `subscribeAllControlTopics()` **不再當場送出任何 `subscribe()`**，
  只是把 `subscribeCursor` 歸零排隊。
- `loop()` 的 `controlSubscribeScheduler()` 每輪推進一格
  （`0` ＝ master 自己，`1..N` ＝ 名冊第 `cursor-1` 台），佔用名額後才寫 socket。
  健康網路下每輪 `loop()` 是毫秒級，21 格在數十毫秒內走完，與一口氣訂完
  沒有可感知差別；病態 socket 下自動退化成「每輪一次」，不再凍結整輪。
- `controlSubscribeScheduler()` 排在 `publishStatus()` **之前**：對齊是有限步數
  （最多 `slaveCount + 1` 格）的一次性工作，讓它先走完，代價只是狀態發布晚幾輪。

**訂閱的維護點**：

| 時機 | 動作 |
|---|---|
| 剛連上 broker（`quickConnectToIndex()`／`quickConnectCustom()`） | `subscribeAllControlTopics()` 排隊全量對齊；接著 `publishStatus()` 與 `markAllSlavesDirty()`（新 broker 上沒有任何 retain，整份名冊要重壓一次） |
| 配對成功（`addSlave()`，WiFi task） | 只設 `pendingSubscribeRefresh`，由 `loop()` 排隊全量對齊 |
| slave 主動解除（`onEspNowRecv()` 的 `HO_PKT_UNPAIR`，WiFi task） | 設 `pendingSubscribeRefresh`。**必要**：陣列前移會把某台移到游標已經走過的位置而漏訂（review M3） |
| 序列埠 `unpair <n>`（`unpairSlave()`） | `publishSlaveOffline()` 之後、陣列搬移之前 `unsubscribeSlaveControlTopic()`；搬移後同樣設 `pendingSubscribeRefresh`（理由同上） |
| slave 主動解除的收尾（`processPendingUnpairPublish()`） | 補發最後一則 offline 之後 `unsubscribeSlaveControlTopic()` |

**取消訂閱目前 100% 不會真的送出（敘述更正）。** `unsubscribeSlaveControlTopic()`
受「每輪 `loop()` 只做一次阻塞式 socket 寫入」的名額管轄，名額被用掉就放棄
（序列埠會印一行）。這裡原本寫成「盡力而為」，暗示有時退得成 —— **實際上必定
退不成**：兩個呼叫端（`unpairSlave()`、`processPendingUnpairPublish()`）在呼叫它
之前都**一定**先做過一次 offline publish，那次 publish 必然已經佔走本輪名額。
保留這個函式的意義是：未來若有「不先 publish」的呼叫端加進來，這條路徑立刻生效。
放棄是安全的：

1. `mqttCallback()` 會用 `findSlave()` 擋掉已不在名冊上的目標，
   殘留訂閱收到的訊息不會造成任何動作；
2. 下次重連時 `mqttClient.connect(...)` 的最後一個參數 `cleanSession` 傳 `true`，
   broker 不保留上一個 session 的訂閱清單，殘留自然消失。

**指令轉發全程用 MAC，不用索引（review M2 修正）。**
`handleSlaveCommand()` 收的是 `const uint8_t mac[6]` 而不是索引：
`slaves[]` 會被 WiFi task 的 `HO_PKT_UNPAIR` 分支前移，任何先前取得的索引
在任何一個可能被打斷的點上都會失效 —— 而 `status` 分支中間夾著
`publishSlaveStatus()`，那是一次最壞 10 秒級的阻塞 publish，窗口大到不能忽略。
**索引指錯台 ＝ 開錯門**，對動物捕捉設備是最嚴重的失敗型態。
送指令走 `sendCmdToSlaveMac()`：`findSlave()` 只用來確認「這個 MAC 目前確實在
名冊上」（純值比較），實際 `espNowSendTo()` 用的是呼叫方自己那份 MAC 副本，
所以連「檢查與送出之間」的窗口都不存在。

**精確的敘述（Task 5 review M4 更正）**：Task 5 之後**索引式的送出路徑已經消失**
（`sendCmdToSlave(int idx, …)` 已刪除，序列埠改走只做「編號 → MAC 值複製」的
`sendCmdToSlaveIndex()`；`requestSlaveState(int idx)` 同樣改成
`requestSlaveStateIndex()`），而**單播的繼電器指令送出點只剩
`sendCmdToSlaveMac()` 一個**。
但**全檔的 `HO_PKT_CMD` 送出點其實有兩個** —— 另一個是 `sendCmdToAll()` 裡送往
`BROADCAST_MAC` 的那次廣播，它天生不經過名冊、沒有索引可以指錯，屬於另一類。
「送出點只剩一個」是錯的說法，不要再這樣寫。

> 這正是 Task 3 review 抓到的 M4 缺陷（`unpairSlave()` 阻塞後沿用舊 `idx`）。
> 同一類缺陷在新路徑上被重新引入過一次，改動 `slaves[]` 相關程式碼時請
> 一律假設「索引在下一個瞬間就會失效」。

**訂閱失敗一定要看得見（review M4 修正）。**
`[MQTT] 已訂閱 …` 這一行是回歸清單第 5／9a／11 項的**通過判準**，
所以它只在 `mqttClient.subscribe()` 回傳 `true` 時才印；失敗改印
`⚠ [MQTT] 訂閱失敗 …`（slave 的對應行是 `⚠ [代理] 訂閱失敗 …`）。
第一版丟掉回傳值、無條件印「已訂閱」，等於在訂閱失敗時給實測者一個假綠燈。
訂閱失敗不重試（游標照樣前進），避免「topic 過長」這類必定失敗的原因
把排程器卡死；下次重連或下次名冊變動時會重新對齊。

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

### 1. WROOM flash 用量偏緊（83.3%）

Phase 2b Task 4（含 review 修正）後實測 1,692,467 / 2,031,616 bytes
（app0 分區，**83.30%**），餘裕約 331KB。**注意 `arduino-cli` 印的百分比（10%）分母是 16MB 的預設值，
不是本 sketch 實際使用的 `partitions.csv` 的 app0（2,031,616 bytes），
要自己換算。**
Phase 4 要加轉送 OTA 的程式碼前，務必先跑 `.\flash.ps1 -Model master` 確認還編得過；
若餘裕不足，需考慮用 NimBLE 取代目前的 Bluedroid（BLE stack 是兩板差距的主因，
C3 同一份 `partitions.csv` 下用量僅約 64.5%）。

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
