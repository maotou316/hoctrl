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
2. **Flash 空間**：Phase 2a Task 6（BLE 配網）起 WROOM 版也改用
   `PartitionScheme=custom`（與 C3 共用
   `ho_master1/partitions.csv`，app0/app1 各 `0x1F0000` = 2,031,616 bytes），
   兩者 app0 分區大小**相同**，差異只在實際用量（Phase 2b Task 7 收尾實測）：
   - WROOM：**1,698,515 bytes（83.60%）**，餘裕約 325KB
   - C3：**1,318,241 bytes（64.89%）**，餘裕約 696KB
   BLE（Bluedroid）在 WROOM 上占用明顯較多 flash，是兩者差距的主因。
   詳見文末「已知風險」第一項。
3. **C3 版若要接繼電器，有硬體限制**：GPIO 4/7 是 ESP32-C3 的 JTAG 腳（MTMS/MTDO），
   reset 後由 ROM 配置、不保證低電位，開機瞬間繼電器會短暫通電，這是硬體限制、
   韌體無法根治，跟 `ho_relay2` 完全同源。需硬體在 MOS gate 對地加 10kΩ 下拉才能根治。
   WROOM 版的 GPIO 13 沒有這個問題。詳見 `ho_relay2/readme.md` 的「已知硬體限制」章節。

## 角色

Phase 1 階段是純 ESP-NOW 主控，用序列埠指令操作。
Phase 2a 起接上 WiFi + MQTT + BLE 配網，成為 App 與所有 slave 之間的唯一對外窗口。
**Phase 2b 完成後，master 是 20 台 slave 的完整 MQTT 代理**：代每台 slave 發布
`hoban/<slaveId>/status`、代每台 slave 訂閱 `hoban/<slaveId>/control`，
所以 slave 在 App 眼裡就是一台普通設備。

**Phase 2b 沒有做的事**（不要從上面那句推論成「都好了」）：

| 項目 | 狀態 |
|---|---|
| Long Range（`LR:*` 指令、`esp_wifi_set_protocol()`） | **完全未實作**。原 Task 6 已依 Ruling 整個移到 Phase 5，`LR:` 分支目前只印一行「尚未實作」；`long_range` 欄位恆為 `false` |
| ESP-NOW 轉送 OTA（`update:{JSON}`） | 未實作，Phase 4 |
| App 端的樹狀 UI、讀 `grp`／`group.noack` | 未做，Phase 3 |
| 指令歸因欄位（讓「韌體層已執行」可證明） | **Phase 4 Task 1 已做**（協定版本 2）。見 `docs/phase4-flag-day-upgrade.md` |

> **⚠ 整個 Phase 2b（Task 1~5、7）至今零實機回歸。**
> 所有結論——包含本文件的每一段推論、`docs/phase2b-regression-checklist.md`
> 的每一條預期輸出——都是**靜態推演**，唯一真正跑過硬體的只有 Task 2 的
> `fakeslaves 20` + `jsonsize`（實測 2100 bytes）。
> Phase 2b 的 review 過程中出現過一次「靜態推演錯到連『新測試抓不抓得到目標缺陷』
> 都推錯」，所以請不要把本文件的任何一句當成實測結論。

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
| `droppeer <n>` | **測試用**：刪掉第 n 台的 ESP-NOW peer 但**保留名冊條目**，製造「在名冊上卻送不出單播」。回歸清單 8e 專用。不寫 NVS、不動名冊內容，重開機或重新配對即恢復。**傷害面**：App 對那台的個別開／關會**靜默失敗**到重開機為止；群組關門仍有效（主指令走廣播×3），但它會被誠實記成未送達 |
| `fakeslaves <n>` | **測試用**：把名冊灌成 n 台假 slave，量狀態 JSON 容量用。MAC 是 `AA:BB:CC:DD:EE:<i>`，**`<i>` 是迴圈索引 0…n−1（不是 n）**，所以 `fakeslaves 20` 產生的 ID 是 `hoban-aabbccddee00` … `hoban-aabbccddee13`（**十六進位**）。**不寫 NVS**、**不註冊 ESP-NOW peer**。灌入後 `fakeSlavesActive` 會鎖住 `saveSlaves()` 直到重開機，避免假 MAC 經由後續的 pair／unpair 流程寫進 NVS 汙染真實名冊。**⚠ 若當下已連上 broker，master 會在約 0.75 秒後開始把這 n 台以 `retain=true` 發上去，而那些 topic 重開機後不會被覆蓋、永久留在 broker 上** —— 清除步驟見 `docs/phase2b-regression-checklist.md` 第 3 項結尾 |
| `jsonsize` | **測試用**：用 `buildStatusDoc()` + `measureJson()` 印出「實際會發布的那份 JSON」的大小，與 `statusBuf`／mqtt buffer 對照。不需連上 MQTT |
| `help` | 顯示說明 |

**沒有 `lr on｜off` 這條序列埠指令。** Long Range 的原 Task 6 已整個移到 Phase 5，
兩端都還沒有任何 LR 開關（`longRangeEnabled` 全檔只有讀取點、沒有寫入 `true` 的路徑）。

`fakeslaves` 與 `droppeer` 兩條是**破壞性測試工具**，只在序列埠可達（需要實體 USB），
沒有 MQTT 入口。差別是：`fakeslaves` **會覆蓋整份名冊**（真實 slave 這次開機不再被追蹤），
`droppeer` **完全不動名冊內容**（只刪 ESP-NOW peer 表這份 RAM 資料）。

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

**topic 總數 ＝ (1 + slave 台數) × 2。** 名冊滿 20 台時是 **21 組、42 條 topic**：

| 誰 | status（發布，retain） | control（訂閱） | 由誰負責 |
|---|---|---|---|
| master 自己 | `hoban/hoban-<masterMac>/status` | `hoban/hoban-<masterMac>/control` | 自己；**LWT 名額也在這一條 status 上** |
| slave #0 | `hoban/hoban-<slave0Mac>/status` | `hoban/hoban-<slave0Mac>/control` | master 代發／代訂閱 |
| … | … | … | … |
| slave #19 | `hoban/hoban-<slave19Mac>/status` | `hoban/hoban-<slave19Mac>/control` | master 代發／代訂閱 |

- **topic 用的是 slave 自己的 MAC，不是 master 的**，所以 App 眼裡它就是一台獨立設備。
- 代發的 payload 多一個 `via` 欄位指回 master（一般設備沒有這個欄位）。
- **代訂閱不用萬用字元 `hoban/+/control`**：預設清單裡有三台公用 broker
  （emqx.io、hivemq.com、eclipseprojects.io），萬用字元會收到全世界所有 hoban 設備的
  控制訊息。逐台訂閱。
- 訂閱是**游標式、每輪 `loop()` 最多一格**（`controlSubscribeScheduler()`），
  不是一口氣訂完 21 條（理由見「代訂閱與指令轉發」）。

### 完整 MQTT 指令表（Phase 2a + 2b，含「送到哪個 topic」）

**只有兩種 control topic**，指令送錯 topic 一律不會有作用：

- `hoban/<masterId>/control` —— master 自己的。`<masterId>` ＝ master 的
  `hoban-<自己的 MAC>`。**下表 A 區的所有指令都送這裡**。
- `hoban/<slaveId>/control` —— **每台 slave 各一條**，由 master 代訂閱。
  `<slaveId>` ＝ 該 slave 的 `hoban-<它自己的 MAC>`。**下表 B 區只有 3 條指令**。

判斷路徑是 `mqttCallback()`：先 `parseControlTopic()` 解析出 MAC，
與自己的 MAC 相同 → `handleMasterCommand()`；否則 `findSlave()` 查名冊，
在名冊上 → `handleSlaveCommand()`，不在 → 印
`[MQTT] 指令的目標不在名冊上，忽略: <topic>` 並丟棄。

#### A 區：送到 `hoban/<masterId>/control`

| 指令 | 送到的 topic | 行為 |
|---|---|---|
| `status` | master | 立即 `publishStatus()` 回報一次（含 `slaves` 陣列與 `group` 摘要） |
| `ON` | master | `pulseRelay(2000)` 點動 **master 自己**的繼電器 2 秒，並回報狀態 |
| `OFF` | master | `setRelayPins(false)` 關閉 **master 自己**的繼電器，並回報狀態 |
| `reset` | master | `clearNetConfig()` 清 NVS 的 **`hoban` 命名空間**（WiFi／MQTT 設定）後 `ESP.restart()`，回到 BLE 配網模式。**不清 `homaster` 命名空間**（slave 名冊與 `espch`） |
| `FIND_BEST_SERVER` | master | 主動斷開目前 MQTT 連線，把輪詢游標歸零（`resetMqttProbe()`）後重新走 `smartConnect()`。注意：`smartConnect()` 一次只試一台，第一台連不上時會由 `loop()` 每 10 秒推進一台，不會在單次呼叫裡試完全部 |
| `HASRELAY:ON` / `HASRELAY:OFF` | master | 設定「本機是否有接繼電器」並存 NVS，回報狀態 |
| `ALL:ON` | master | `sendCmdToAll(HO_CMD_PULSE, 2000)` —— **廣播點動 2 秒，不是持續 ON**，語義與單台 `ON` 一致；含 master 自己的繼電器。**這就是 App「全部關門」按鈕實際送的指令** |
| `ALL:OFF` | master | `sendCmdToAll(HO_CMD_OFF, 0)` —— 把所有繼電器**持續斷開**，含 master 自己的。**App 不送這條**（全 repo 的 `lib/`／`test/` 沒有任何一處），目前只有序列埠 `alloff` 會走到 |
| `SLAVES` | master | `markAllSlavesDirty()` ＋ 一次 `publishStatus()`。名冊會在接下來一輪內逐台重壓保留訊息，不在這裡連發 20 則 |
| `PAIR:START` | master | `enterPairingMode()`，開一個 60 秒配對視窗（`PAIRING_TIMEOUT`，與 App 的 60 秒倒數對齊）。**期間可連續配對多台，配對成功不會自動退出** |
| `PAIR:STOP` | master | `exitPairingMode()` |
| `UNPAIR:<deviceId>` | master | 解除指定 slave（`hoban-xxxxxxxxxxxx`）。**注意是送到 master 的 topic，不是那台 slave 的 topic**。ID 格式錯誤或不在名冊上都只印一行，不動作 |
| `UNPAIRALL` | master | 清空整份名冊。**只插旗，實際拆除由 `loop()` 的 `processUnpairAll()` 每輪拆一台**（理由見下方「UNPAIRALL 為什麼必須分批」） |
| `LR:*` | master | **尚未實作。** 分支只印一行 `[LR] 指令尚未實作（Task 6）`，讓它不掉進「未知指令」。字串裡的「Task 6」是**寫於移轉之前的舊編號**——該 Task 已依 Phase 2b 的 Ruling 整個移到 **Phase 5**。`LR:` 開頭的任何字串（`LR:ON`／`LR:OFF`／`LR:whatever`）都走這一條，**沒有任何實際效果** |
| 其他任何字串 | master | 印 `[MQTT] 未知指令: <字串>`（App 的驗證程序依賴這行探針字串） |

#### B 區：送到 `hoban/<slaveId>/control`（master 代訂閱後轉成 ESP-NOW）

| 指令 | 送到的 topic | 行為 |
|---|---|---|
| `ON` | 該 slave | `sendCmdToSlaveMac(mac, HO_CMD_PULSE, 2000)` —— **點動 2 秒，不是持續開啟** |
| `OFF` | 該 slave | `sendCmdToSlaveMac(mac, HO_CMD_OFF, 0)` |
| `status` | 該 slave | 先用名冊上目前已知的狀態立刻代發一則（`publishSlaveStatus()`），再 `requestSlaveStateIndex()` 向 slave 要一次最新狀態，回來時 `dirty` 會觸發第二則代發。**群組指令進行中時第二步會讓開**（印 `[代理] <id> 群組指令進行中，稍後再向它要最新狀態`），第一步照做，App 不空等 |
| 其他 | 該 slave | 印 `[代理] <slaveId> 不支援的指令: …`，不做任何動作 |

**B 區沒有 `reset`／`ALL:*`／`PAIR:*`／`HASRELAY:*`／`FIND_BEST_SERVER`。**
往 slave 的 topic 送這些只會得到「不支援的指令」，不會被轉去 master 執行。

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

**當時的根因是結構性的：協定版本 1 的 `HoStatePayload` 沒有任何指令歸因欄位**
（沒有「我剛執行了哪一則指令」的 seq／echo），所以在那一版協定下
「指令有被執行」**原理上無法證明**。

**Phase 4 Task 1 已把那筆技術債還掉**（協定版本 2：`HoCmdPayload` 帶 `cmdId`、
`HoStatePayload` 帶 `lastCmdId`／`lastCmdKind`／`lastCmdCount`），現在的分類是：

| 事實 | 可否證明 | 依據 |
|---|---|---|
| **已送達** | ✅ 可證明 | 單播有 MAC 層 ACK。`esp_now_send()` 的送出回呼 `onEspNowSent()` 逐幀回報成功／失敗，`wifi_tx_info_t::des_addr` 帶目的 MAC，可逐台歸因 |
| **韌體層已執行** | ✅ 可證明（版本 2 起）**但擋不住偽造** | slave 回報的 `lastCmdId` 等於本次指令的 `cmdId`、種類相符、且回報晚於指令送出（`groupExecutedIdx()`）。證據由 slave 產生，master 造不出來 —— **但射頻範圍內的第三方造得出來**（CRC-8 ＋ 原始碼裡的共享密鑰、來源 MAC 可任意填）。而且靜止狀態是繼電器 OFF ＝ 門開，所以**干擾本身就能造成失敗、偽造只需負責掩蓋**；偽造 CMD 是暫態、**偽造 STATE 卻是持續的**，還會抑制「去現場確認」這個唯一的真防線。見 `docs/phase4-flag-day-upgrade.md` 第 3.2.1 節 |
| **繼電器硬體動作** | ❌ 不可證明 | `setRelayPins()` 只寫 GPIO。MOS 燒毀、線路脫落、觸點黏死都照樣回報「已執行」 |
| **籠門關上了** | ❌ 不可證明 | 同上，再加上機構本身。**現場確認是唯一方法** |
| 廣播是否被收到 | ❌ 不可證明 | 廣播沒有 ACK，`esp_now_send()` 永遠回報成功 |

> **「沒有執行證明」不等於「沒執行」**：回報可能還在路上、可能掉了。
> 所以它只能**維持紅色**，不能宣稱「已確認未執行」。誤紅可接受、誤綠不可接受。
>
> **執行證明刻意不進入控制決策** —— 它只進入回報（序列埠收工訊息、MQTT 的
> `exed`／`exe`）。廣播 ＋ 全台單播 ＋ 補送的流程一行都沒改，理由見
> `sendCmdToAll()` 上方的註釋與 `docs/phase4-flag-day-upgrade.md` 第 3.2 節。

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
| **`HO_PKT_PAIR_ACK`**（`onEspNowRecv()` 的 `HO_PKT_PAIR_REQ` 分支） | **第 5 輪已守住** —— 送 `PAIR_ACK` 之前先 `groupAckArmed = false`（見下） |

**`PAIR_ACK` 那條原本被判為「擋不掉也不該擋」，第 5 輪 review 推翻了那個結論。**
擋不掉的是「回覆配對請求」（配對必須回、且它跑在 WiFi task），
但**歸因閂鎖是關得掉的** —— 送 `PAIR_ACK` 之前加一行 `groupAckArmed = false;` 即可：

- **不影響配對**：`PAIR_ACK` 照送，語義零改變。
- **方向安全**：最壞是把一台其實已送達的誤判成未送達 → 多補送一趟（誤紅）。
- **context 安全**：該分支跑在 WiFi task，而 `groupAckArmed` 本來就是
  `volatile bool`、本來就由 WiFi task 的 `groupNoteUnicastAck()` 寫，不新增競態面。

為什麼非守不可（嚴重性不因「不是自製證據」而降級）：系統對 `grp: 1` 的定義是
「**這次群組指令**的單播對這台拿到了 MAC 層 ACK」。拿 `PAIR_ACK` 的 ACK 去填它，
產出的仍然是一個**與事實不符的綠燈** —— 那台可能三趟 CMD 單播全掉卻顯示已送達，
而「CMD 掉、其他單播通」正是訊號邊界的典型情境，不是憑空假設。
（觸發需要人為動作：`ho_slave1` 的 `requestPairing()` 有 `masterInPairingMode`
前置條件，所以 master 必須正在配對模式、且有人短按該台按鈕。這讓量級小得多，
但不改變方向。）

歸因失敗一律往**誤紅**方向掉（回呼太晚到 → 這台被當成未送達 → 多補送一次），
不會往誤綠掉。

**「job 之外一律拒絕寫入」是結構而不是約定（第 4 輪 review）。**
`groupNoteUnicastAck()` 開頭多一道 `if (!groupCmdActive()) return;`，
把「只有群組單播才會開閂」從每個開閂點的自律變成**結構保證**。

> **這道守衛今天不擋任何回呼，純屬未來防護（第 5 輪 review N-a 更正）。**
> 上一版寫它「順帶殺掉跨 job 的過期回呼」**是錯的**：
> 跨 job 過期回呼的情境是「job A 的回呼延遲到 **job B 已經開閂之後**才到、且 MAC
> 同一台」，此時 `phase` 是 `ARMED`／`WAIT`（非 `IDLE`），守衛**放行** —— 照樣被
> 記進 job B，**完全沒被擋掉**。而「`phase == IDLE` 且閂還開著」在現行程式
> **觀察不到**，但理由不是「每個出口都關閂」（第 5 輪 review 指出那句字面不精確）：
> `groupFinishJob()` 的關閂在**函式第一行**（先行關閂）；`groupCmdSnapshot()` 是設
> `IDLE` 的**下一行**才關；`sendCmdToAll()` 的空名冊早退**根本沒關閂**（靠
> `groupCmdSnapshot()` 剛關過）。結論成立，但成立的原因是**這道守衛自己**。
> 守衛的價值是「把約定變成結構、擋住未來新增的路徑」，**不是**修掉任何現存缺陷。
> 在這個專案裡，「宣稱一道其實不存在的防線」與那些假綠燈是同一個形狀，不能留。

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

序列埠一定看得見，且**任何情況都會印**最後那三行說明：

```
[群組] 指令 <N> 收工：單播 MAC 層已送達 X／Y 台
⚠ [群組] Z 台連「送達」都沒有（單播沒拿到 MAC 層 ACK）
⚠ [群組]   未送達：hoban-xxxxxxxxxxxx
⚠ [群組] W 台在執行期間離開名冊，同樣未送達
[群組] 韌體層已執行 X／Y 台（slave 回報 cmdId=<數字>）
⚠ [群組]   無執行證明：hoban-xxxxxxxxxxxx
[群組] 注意：MAC 層 ACK 只證明「封包已送達」，不能證明繼電器真的動作
[群組] 執行證明只到韌體層：證明 slave 走完了繼電器動作那段程式，不證明繼電器硬體動作，更不證明籠門關上
[群組] 沒有執行證明不等於沒執行 —— 回報可能還在路上；它只能維持紅色，不能宣稱已確認未執行
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
"group": {"cmd":2,"age_s":2,"busy":0,"n":20,"ack":18,"noack":2,"gone":0,
          "exed":18,"exec":"attributed"}
```

**`cmd` 的合法值只有 `0`／`1`／`2`**（`HO_CMD_OFF = 0`、`HO_CMD_ON = 1`、
`HO_CMD_PULSE = 2`，見 `libraries/HoEspNow/src/HoEspNowProtocol.h`）。
上面舉的 `2` 就是 `ALL:ON`（＝廣播 `HO_CMD_PULSE`）的實際值。
**這個範例原本寫 `"cmd":3`，那是一個不存在的值**，Task 7 review 抓到後更正。

`noack` 就是 App 該顯示紅色的依據。`exec` 自協定版本 2 起是 `"attributed"`
（Phase 4 Task 1 之前固定 `"unprovable"`），並多了 `exed`（有執行證明的台數）
與逐台的 `slaves[].exe`。**`"attributed"` 仍然不等於「門關了」** ——
它只到韌體層，完整的「擋不住什麼」見 `docs/phase4-flag-day-upgrade.md` 第 3.2 節。

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

指令清單見上方「完整 MQTT 指令表」的 **B 區**，這裡只記語義選擇的理由。

`ON` 送 `HO_CMD_PULSE` 而不是 `HO_CMD_ON` 是刻意的：App 對一般 hoRelay 設備送的
`ON` 語義是「開門」＝點動一次，master 自己的 `ON` 分支也是 `pulseRelay(2000)`。
slave 要在 App 眼裡是一台普通設備，語義就必須完全一致，否則
「開保險 → 關門 → 關保險」三段鎖流程對 slave 的行為會與其他設備不同。
**持續開啟只保留給序列埠的 `on <n>`**（現場除錯用）。

### master 自身狀態 JSON（`hoban/<masterId>/status`，retain）

**以下欄位名稱逐一取自 `buildStatusDoc()` ＋ `appendGroupResult()` ＋
`appendSlavesArray()` 的實際程式碼，不是從規格抄的。**

> **這句宣稱擋得住什麼、擋不住什麼（Task 7 review C1 的教訓）**：
> 它擋的是「**欄位名稱**與程式碼不符」——每個 key 都能在上述三支函式裡 grep 到。
> **它擋不住「範例值」寫錯**：底下那些數字是為了讓範例好讀而編的，
> 只有 `"exec": "attributed"` 是程式寫死的常數。
> **這不是假設性的**：本範例的 `"cmd"` 原本寫成 `3`，而 `3` 是一個
> **不存在的 `HoRelayCmd` 值**，review 才抓到。
> **值的合法範圍一律以本節下方的欄位表為準，不要照抄範例的數字。**

```json
{
  "device_id": "hoban-a0b1c2d3e4f5",
  "status": "online",
  "version": "1.0.0",
  "model": "hoMaster1",
  "server": "mqttgo.io",
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
    "long_range": false,
    "free_heap": 123456
  },
  "group": {
    "cmd": 2, "age_s": 2, "busy": 0,
    "n": 2, "ack": 1, "noack": 1, "gone": 0,
    "exed": 1, "exec": "attributed"
  },
  "slaves": [
    {"id":"hoban-aabbccddeeff","relay":0,"online":true,"rssi":-72,"version":"1.0.0","grp":1,"exe":1},
    {"id":"hoban-112233445566","relay":0,"online":false,"rssi":-100,"version":"0.0.0","grp":0,"exe":0}
  ]
}
```

**條件出現的欄位**（不是每一則都有）：

| 欄位 | 何時出現 |
|---|---|
| `group` | **開機以來下過至少一次群組指令**才有（`if (!groupJob.everRan) return;`）。冷開機後第一則 status 沒有這個物件 |
| `slaves[i].grp` | 該台**在最近一次群組指令的快照裡**才有（`groupDeliveryFor()` 回 −1 就整個不帶）。指令之後才配對進來的 slave 沒有 |
| `slaves[i].exe` | 同 `grp` 的出現條件（`groupExecutedFor()` 回 −1 就整個不帶）。`1` ＝ 有執行證明；`0` ＝ **沒有證據**，不是「已確認沒執行」。⚠ **與 `grp` 不同，它會由 1 翻回 0**（那台執行了下一道指令，或離開名冊）—— 誤紅方向，但不可假設單調 |
| `slaves_truncated` / `slaves_shown` | 名冊台數超過執行期上限才有。**照現行常數（3584／728／11／112）算出的 `maxEntries` 是 25，而名冊上限 `HO_ESPNOW_MAX_SLAVES` 是 20，所以這兩個欄位在正常情況下永遠不會出現**。它們存在的意義是：萬一有人改小 buf 又繞過 `static_assert`，App 與序列埠都看得見，而不是靜默給出一份不完整的清單 |
| `long_range` | **一定有，而且恆為 `false`** —— `longRangeEnabled` 全檔沒有任何寫入 `true` 的路徑（Long Range 在 Phase 5 才做） |

`group` 摘要每個欄位的語義。**`ack`／`noack`／`gone` 嚴格限定在「送達」；
`exed`／`exec` 講的是「韌體層已執行」，仍然沒有任何一個宣稱「門關了」**：

| 欄位 | 語義 |
|---|---|
| `cmd` | 最近一次群組指令的 `HoRelayCmd`。**實際數值是 `0 = OFF`、`1 = ON`、`2 = PULSE`**（`libraries/HoEspNow/src/HoEspNowProtocol.h`）。`appendGroupResult()` 上方的註釋原本寫成「1=ON 2=OFF 3=PULSE」，三個都錯，Task 7 已更正。**`ALL:ON` 送的是 `HO_CMD_PULSE`，所以它的 `cmd` 是 `2` 不是 `1`** |
| `age_s` | 距離該次指令送出的秒數 |
| `busy` | `1` ＝ 補送還在進行中，`0` ＝ 已收工。**只有 `busy: 0` 的數字才是定案** |
| `n` | 快照台數 |
| `ack` | 單播拿到 MAC 層 ACK 的台數（**只是送達**） |
| `noack` | 沒拿到的台數 —— **這就是 App 該顯示紅色的依據** |
| `gone` | 指令期間離開名冊的台數（同樣未送達） |
| `exed` | 有執行證明的台數（`groupExecutedIdx()`）。**可能在收工之後才變大**（回報是非同步的），**也可能之後變小**（那幾台執行了下一道指令，或離開名冊）。**不是單調的** —— 定案看收工當下那一則 |
| `exec` | **固定寫死 `"attributed"`**（版本 2 起；之前是 `"unprovable"`）。它宣稱的是「有 `exed` 台拿到韌體層的執行證明」，**不是**「門關了」。舊 App 把任何非 `"unprovable"` 的值歸成 `unrecognized`，仍然不會轉綠 —— 誤紅方向，安全 |

### 代發的 slave 狀態 JSON（`hoban/<slaveId>/status`，retain）

由 `publishSlaveStatus()` 組出，**每台 slave 各一個 topic**：

```json
{
  "device_id": "hoban-aabbccddeeff",
  "status": "online",
  "version": "1.0.0",
  "model": "hoSlave1",
  "via": "hoban-a0b1c2d3e4f5",
  "timestamp": 12345,
  "wifi": { "connected": true, "ssid": "ESP-NOW", "rssi": -72, "ip": "N/A" },
  "device": { "relay": 0, "grp": 1 }
}
```

- `status` 與 `wifi.connected` 都跟著 `slaves[i].online`（規格範例把 `wifi.connected`
  寫死 `true`，這裡**刻意偏離**：寫死 true 會讓離線的 slave 在 App 上永遠顯示在線）。
- `version` 是 slave 回報過的韌體版本；**還沒回報過任何狀態時 `formatSlaveVersion()`
  給的是 `"0.0.0"`**（`addSlave()`／`loadSlaves()` 把 `fwMajor/Minor/Patch` 都初始化成 0），
  不是空字串也不是 `null` —— 剛從 NVS 載入、還沒被輪詢到的 slave 就是這個值。
- `via` 標示這台是被哪一台 master 代發的（**Phase 3 的 App 要靠它處理「master 離線」**，
  見「代發 slave 狀態」一節的 LWT 缺口）。
- `device.grp` 與 master 狀態的 `slaves[].grp` 是同一個值、同一個語義，
  **不在最近一次群組指令的快照裡就整個不帶**。
- `wifi.ssid` 固定字串 `"ESP-NOW"`、`wifi.ip` 固定字串 `"N/A"`、`rssi` 借來顯示
  ESP-NOW 訊號強度 —— 這個形狀是刻意跟一般設備對齊的，App 兩個頁面各自的
  `_handleMqttMessage` 手動解析不用改就吃得下。

### 代發 slave 狀態（Phase 2b Task 3）

master 除了自己的 `hoban/<deviceId>/status`，還會用**每台 slave 的 MAC** 代發一份
`hoban/hoban-<slaveMac>/status`，讓 slave 在 App 眼裡就是一台普通設備 —— App 現有
的設備卡片、詳情頁、「開保險→關門→關保險」三段鎖幾乎零改動就能個別控制每台 slave。

**payload 格式**見上方「代發的 slave 狀態 JSON」一節（逐欄位取自
`publishSlaveStatus()` 的實際程式碼）。

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
| MQTT buffer / timeout | 未設定（256 bytes／15 秒逾時） | `setBufferSize(3840)` / `setSocketTimeout(3)`（Phase 2b Task 1 從 1024 放大到 3328，因為狀態 JSON 要塞 20 台 slave；Phase 4 Task 1 再放大到 3840，因為 `statusBuf` 由 3072 放大到 3584；**`setSocketTimeout()` 對 `publish()` 無效**，見上方殘存風險） |
| LED | 恆亮／閃爍幾種簡單狀態 | 一次性事件閃爍 ＋ 持續式狀態機（見上方「LED 狀態指示」） |
| 韌體更新（OTA） | MQTT `update:{JSON}` 指令 | 尚未支援，留給 Phase 4 |

## 已知風險

> **這一節是索引，不是全部。** 幾條最重要的風險的完整論證寫在各自的章節裡，
> 這裡只給一句話與連結 —— 因為之前有人直接跳來看這一節，第一眼看到的是 flash 用量，
> 而**排在第 0 項的那條才是最嚴重的**。

### 0. 【最嚴重】MQTT socket 寫入的阻塞**沒有硬上限，可以超過 slave 的 30 秒失聯門檻，而且目前無防護**

**後果是籠門被打開**（slave 超過 30 秒沒收到心跳就強制關閉繼電器）。
機制、後果與「為什麼現行的名額守衛擋不住」的完整說明在
**「ESP-NOW 心跳的實際保證」→「已知殘存風險」**那一節，請務必讀過。

一句話版本：`PubSubClient::publish()`／`subscribe()` 走
`NetworkClient::write()`，**部分寫入成功時會把重試計數器重置回 10**，
所以常被引用的「10 秒」是**名目值不是硬上限**；病態 socket（例如對端 TCP 視窗
趨近於零）可以無限期地反覆重置。現行的
`mqttPublishBudgetUsed` 名額守衛只保證**「每輪 `loop()` 最多發生一次」**，
**完全不保證「單次有多長」**。

**處置：無。** 要封死只能 fork `PubSubClient`（在它的 `write()` 內部插心跳）
或換成非阻塞的 MQTT 客戶端，兩者都超出 Phase 2b 範圍。

> **它擋不住什麼，也要一起寫**：本節下方的所有其他防線（三／四層防護、
> 群組指令的 6 秒 wall-clock 上限、`unpairall` 分批）擋的都是**「多次阻塞疊加」**，
> 沒有任何一條擋得住**「單次阻塞過長」**。**別把它們讀成已經解決了第 0 項。**

**目前手邊最接近的做法（也只是逼近，不是重現；起一個不 `read()` 的 TCP sink 或用
netem 壓上行才是真重現，但那超出本節範圍）**：
**拔掉 AP 的 WAN／上行網路線，但讓 WiFi 關聯保持不斷**。
此時 `WiFi.isConnected()` 仍是 true、本地 socket 仍停在 `ESTABLISHED`、
`mqttClient.connected()` 仍回 true，代發照發，直到 lwIP 的 `TCP_SND_BUF`
（約 5.7KB）被塞滿 —— 之後每次 `write()` 吃滿 10 次重試 × 1 秒 `select()`。

- **它能驗到**：10 秒級黑箱確實存在，以及兩則心跳之間最長的空白有多久。
- **它驗不到**：「無限期」那一種。無限期需要**每輪只擠得出幾個 byte**
  才會反覆重置重試計數器，對應的是**對端 TCP 視窗趨近於零**而不是「完全不通」，
  而拔 WAN 線比較容易造成後者。
- **所以這個實驗 PASS 不代表本項風險不存在**，只代表「這一次沒踩到」。
  本項目前**沒有任何測試能證明它不發生**，只能靠讀 `NetworkClient::write()`
  的原始碼確認機制存在。步驟寫在
  `docs/phase2b-regression-checklist.md` 第 22 項的**變體 B**。

### 0-b. 群組廣播沒有 ack，「已送達」不等於「已關門」

- **廣播沒有 ACK**：`esp_now_send()` 對廣播位址**永遠回報成功**，
  不代表任何一台收到了。可靠性全押在第 2、3 段（逐台單播 → 對未取得 MAC 層 ACK 的補送）。
  **廣播命中率至今沒有任何數據支撐**，第一次實測要記錄「第 1 趟就全部確認的比例」。
- **「韌體層已執行」自協定版本 2 起可證明**（Phase 4 Task 1 已還這筆技術債）：
  slave 在 `HoStatePayload` 帶回 `lastCmdId`，master 比對本次指令的 `cmdId`。
  MQTT 的 `group.exec` 因此由 `"unprovable"` 改成 `"attributed"`，並新增
  `group.exed`（有證明的台數）與 `slaves[].exe`（逐台）。
  **但它只到韌體層** —— 不證明繼電器硬體動作、更不證明籠門關上，
  且「沒有證明」不等於「沒執行」。完整論證見「只宣稱可證明的事」那一節與
  `docs/phase4-flag-day-upgrade.md`。
- **關門的實際路徑是 `ALL:ON` → `HO_CMD_PULSE`，不是 `ALL:OFF`**。
  App 全 repo 送 `ALL:OFF` 的地方是 **0 處**，而且
  `lib/pages/device_detail_page.dart` 有具名註釋寫著「不要因為按鈕寫『關門』
  就改成 `ALL:OFF`，那是把所有繼電器持續斷開」。

### 0-c. 代發的 slave topic 沒有 LWT：master 斷電時 20 台 slave 會停在最後一則 `online`

PubSubClient **一條連線只有一個 will（遺囑）名額**，已經給了 master 自己的
`hoban/<masterId>/status`。所以代發機制只涵蓋「master 活著、slave 失聯」，
**master 自己斷電或失去網路時做不到**（沒有 `loop()` 可以跑）。

處置：發 `via` 欄位，**由 Phase 3 的 App 用「master 離線 → 其底下所有 slave 一律
視為狀態不明」處理**。韌體端無解，除非改成每台 slave 一條 MQTT 連線（不可能，slave 沒有 WiFi）。

### 0-d. 整個 Phase 2b 零實機回歸

見文件開頭「角色」一節的警告框。`docs/phase2b-regression-checklist.md`
的每一項都還沒有人跑過。

### 1. WROOM flash 用量偏緊（83.60%）

Phase 2b 收尾實測 **1,698,515 / 2,031,616 bytes（app0 分區，83.60%）**，
餘裕約 325KB。**注意 `arduino-cli` 印的百分比（10%）分母是 16MB 的預設值，
不是本 sketch 實際使用的 `partitions.csv` 的 app0（2,031,616 bytes），
要自己換算。**
Phase 4 要加轉送 OTA 的程式碼前，務必先跑 `.\flash.ps1 -Model master` 確認還編得過；
若餘裕不足，需考慮用 NimBLE 取代目前的 Bluedroid（BLE stack 是兩板差距的主因，
C3 同一份 `partitions.csv` 下用量僅約 64.9%）。

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

### 3. 狀態 JSON 的容量：三層防線（Phase 2b Task 1／2）

**Phase 2a 的血淚背景**：`serializeJson(doc, buf)` 寫進固定大小的 `char buf` 時會
**靜默截斷** —— 截斷後的長度仍小於 `mqttClient` 的 buffer，`publish()` 照樣回傳
`true`，於是 broker 收到語法殘缺的 JSON、App 解析失敗，而序列埠上什麼異常都看不到。
Phase 2a 的狀態最壞 317 bytes，對 `char buf[512]` 只剩約 195 bytes 餘裕，
而 Phase 2b 要加的 20 台 `slaves` 陣列需要 1900+ bytes，必定超過。

#### 為什麼放大 `StaticJsonDocument<N>` 完全沒有用

本專案的 ArduinoJson 是 **7.4.3**。這個版本的 `StaticJsonDocument<N>` 只是
`compatibility.hpp` 提供的相容殼（`class StaticJsonDocument : public JsonDocument`），
**`N` 完全被忽略，底層一律動態配置**。連帶的結論有兩個，兩個都很違反直覺：

1. **把 `<512>` 改成 `<3072>` 不會多出任何容量**，那個數字自始至終沒有作用。
   （現行程式已全面改用 `JsonDocument`，只剩 BLE `onWrite()` 三處固定內容的回應
   還留著 `StaticJsonDocument<200>/<350>`，那三處永遠不會變大、功能零風險。）
2. **`doc.overflowed()` 量的是「記憶體配置失敗」，不是「內容超過容量」** ——
   正常情況下不會因為 JSON 變大而觸發，只有 heap 真的配置不出來才回 `true`。

**所以容量控制不能依賴任何宣告容量，只能依賴 `measureJson()` 的實測值。**
這就是三層防線的設計前提。

#### 三層防線

| 層 | 位置 | 擋什麼 |
|---|---|---|
| **1. 編譯期 `static_assert`** | 常數宣告區 | 有人把 `STATUS_BUF_SIZE` 改小、或把 `STATUS_BASE_MAX_BYTES` 改大到放不下 20 台 → **編譯直接失敗**，而不是上線後才靜默截斷 |
| **2. 執行期上限 ＋ `slaves_truncated` 標記** | `appendSlavesArray()` | 名冊台數超過 `maxEntries` 時只放前 N 台，並在 JSON 帶 `slaves_truncated`／`slaves_shown`、序列埠印 `⚠ [MQTT] slaves 陣列被截斷：…`。**照現行常數這條路永遠走不到**（`maxEntries` 25 > 名冊上限 20），留著是為了「萬一走到，看得見」 |
| **3. 發布出口先量再發** | `publishJsonDoc()` | `measureJson()` 量出實際需求，放不下 `statusBuf` 或放不下 mqtt buffer 就**整包不發**並印出實際需求。加上序列化後的 `n >= sizeof(statusBuf) - 1` 兜底。**寧可不發，也絕不發半截 JSON** |

**第 3 層是物理保證，不是紀律**：全檔只有**一處** `mqttClient.publish(`，
就在 `publishJsonDoc()` 內部，三層檢查都在它之前 `return`。

#### `static_assert` 的算式與現行數字

```
(STATUS_BUF_SIZE - 1 - STATUS_BASE_MAX_BYTES - SLAVES_KEY_OVERHEAD)
    / SLAVE_ENTRY_MAX_BYTES  >=  HO_ESPNOW_MAX_SLAVES
(3584 - 1 - 728 - 11) / 112 = 2844 / 112 = 25  >=  20   ✔
```

> **這一節在 Phase 4 Task 1 曾經整節過期一次，值得記下來。**
> 當時同一份文件的第 719 與 1089 行都已改成新數字，**唯獨這一節沒改** ——
> 而這一節正是被指名為權威、且自己寫著「新增欄位時必須手動重算」的那一節。
> 成因是驗證腳本**只跑了「新字串必須出現」一個方向**，
> 從來沒跑「**被取代的舊數字必須消失**」。
> 突變驗證必須跑兩個方向，這是 A 族與 B 族在文件層的同一個形狀。

| 項 | 值 | 來源 |
|---|---|---|
| `STATUS_BUF_SIZE` | 3584 | 序列化用的共用緩衝區，放 `.bss` 不放堆疊（loopTask 只有 8192）。Phase 4 Task 1 由 3072 放大 |
| `-1` | | `serializeJson()` 的字串結尾安全邊界 |
| `STATUS_BASE_MAX_BYTES` | 728 | **分項相加**：`WITHOUT_GROUP_OTA` 480 ＋ `GROUP` 120（實算 112）＋ `OTA` 128（**預留，Task 5 才會真的發出**）。拆成具名常數是 plan 決定 4.2 的要求 —— 舊寫法的 640 是一個魔術數字，被吃光也不會有人發現 |
| `SLAVES_KEY_OVERHEAD` | 11 | `"slaves":[]` 剛好的字元數 |
| `SLAVE_ENTRY_MAX_BYTES` | 112 | 單筆條目的悲觀上界（逐字元實算最壞 **105**，含 Task 5 的 `"grp"` 與 Phase 4 的 `"exe"`） |
| `MQTT_BUFFER_SIZE` | 3840 | 3584 + 固定標頭 5 + 長度欄位 2 + topic 31 = 3622，取 3840 留餘裕。**它有兩個合法值** —— 本次開機沒嘗試過 MQTT 連線時 `setBufferSize()` 根本沒被呼叫，會停在 `MQTT_MAX_PACKET_SIZE` ＝ **256** |

**它保證什麼**：常數被改壞時編譯失敗。
**它擋不住什麼**（同樣重要）：`static_assert` **比較的只是 `SLAVE_ENTRY_MAX_BYTES`
這個常數本身**。若有人在 `SlaveEntry`／`appendSlavesArray()` 新增欄位卻沒把常數
調大，編譯期**完全檢查不出來**「單筆其實已經超過 112 bytes」——
Task 5 加 `"grp"` 時就是這樣把 96 撐到 97 的。新增欄位時必須**手動重算**。

同一個 commit 還把 `group` 物件寫成「94，取 96」卻**漏算了 `busy`**（實際 102）——
`static_assert` 同樣抓不到，因為它只比對常數本身。

**現行餘裕**：20 台的悲觀值 728+11+20×112 = 2979，對 `statusBuf[3584]` 餘裕 604；
25 台（執行期極限）3539，餘裕只剩 44；26 台就編不過。

#### 實測數字（Phase 2b 唯一真正跑過硬體的一項）

序列埠 `fakeslaves 20` ＋ `jsonsize`，實測 **2100 bytes**（那是 Phase 2b 的韌體，
沒有 `"exe"` 欄位、`statusBuf` 還是 3072），對 3072 餘裕 971。
但這是**樂觀值**：那台測試板 SSID 短、沒設自訂 MQTT 伺服器，基礎欄位只吃了
310 bytes，遠低於 728 的預算。**要用來判斷安全與否的是上面那組悲觀值，不是 2100。**
**而且那個 2100 是舊韌體的數字，Phase 4 Task 1 之後尚未重測。**

#### 若日後要放寬 20 台上限

必須**同時**做三件事，少一件就會有靜默失效：
放大 `STATUS_BUF_SIZE`、放大 `mqttClient.setBufferSize()`（且**要檢查回傳值**，
realloc 失敗會停在舊大小，導致之後全部發布靜默失敗）、重算 `SLAVE_ENTRY_MAX_BYTES`。

### 4. Long Range 完全未實作，且距離效益未經任何驗證

`LR:*` 只有一個印一行的佔位分支；`longRangeEnabled` 全檔沒有寫入 `true` 的路徑；
`esp_wifi_set_protocol()` 一次都沒有被呼叫過。整個 Long Range（含
「`esp_wifi_set_protocol()` 在已關聯 AP 的狀態下呼叫會不會造成一次 WiFi 斷線重連」
這個**未驗證的 IDF 行為**、以及「混合 bitmap 下實際有沒有走到 LR 速率」）
已依 Phase 2b 的 Ruling **整個移到 Phase 5，與實測綁在一起做**。

裁決理由（記在這裡免得未來有人以為是漏做）：它是唯一「本階段做完也無法驗收」的
Task，同時動 master + slave + 共用 library 三個編譯目標，而 Phase 2a 已經因為
「未驗證的 IDF 行為被寫成事實」踩過一次（`WiFi.begin()` 帶 channel 的掃描語義，
見「WiFi 連線」一節的情境 2）。

### 5. WiFi 連線的兩項沿用自 Phase 2a 的未決事項

- **沒有 auth mode 退避**（同上方第 2 項）。
- **`WiFi.begin(ssid, pass, ch, nullptr)` 是否真的把掃描限制在該 channel，
  只有 ESP-IDF 文件推導、未經實機驗證**（見「WiFi 連線（ESP-NOW 友善版）」
  的關聯方式情境 2）。回歸清單一律把它列為**觀察項，不是失敗判定**。

## 編譯與燒錄

```powershell
.\flash.ps1 -Model master              # WROOM 版，只編譯
.\flash.ps1 -Model master -Upload      # WROOM 版，編譯並燒錄
.\flash.ps1 -Model master-c3           # C3 版，只編譯
.\flash.ps1 -Model master-c3 -Upload   # C3 版，編譯並燒錄
```
