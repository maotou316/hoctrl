### hoRelay2
- **開發板**: ESP32-C3 Dev Module
- **特色**: 無聲繼電器
- **韌體版本**: 1.7.4
- **GPIO 定義**:
  - BOOT 按鈕: GPIO 9
  - RESET 按鈕: GPIO 1
  - 板載 LED: GPIO 3（`setup()` 必須設成 `OUTPUT`；1.7.2 之前誤設為 `INPUT`，板載燈從未亮過）
  - 面板 LED: GPIO 0
  - 繼電器按鈕: GPIO 4 與 GPIO 7（兩支同時驅動，單一韌體通吃兩版板子）
    - 341305A_P25_250814 → 實際接在 GPIO 7
    - 341305A_Y176_250318 → 實際接在 GPIO 4
    - 未接 MOS 的那支為空接腳，輸出無副作用；不再需要依板號手動改腳位
- **繼電器腳位注意事項**:
  - GPIO 4/7 在 ESP32-C3 上是 JTAG 腳（MTMS/MTDO），reset 後狀態不保證為低電位
  - `initRelayPins()` 必須維持在 `setup()` 第一行，越晚拉低、MOS 誤導通的時間窗越長
- **開發板設定**:
  - USB CDC On Boot: Enabled
  - CPU Frequency: 160MHz (WiFi)
  - Flash Size: 4MB (32Mb)
  - Partition Scheme: Custom (使用 partitions.csv)
  - Upload Speed: 921600
  - Flash Mode: DIO
  - Erase All Flash Before Sketch Upload：Enabled

---

## 編譯與燒錄

```powershell
Set-Location A:\project\hoctrl_arduino
.\flash.ps1 -Model 2            # 只編譯
.\flash.ps1 -Model 2 -Upload    # 編譯並燒錄（自動偵測 COM 埠）
```

`-Model` 的合法值是 `1,2,3,master,master-c3,slave,test`，**hoRelay2 對應 `2`**。
腳本預設 `EraseFlash=all`，燒完 EEPROM 一併被抹掉，**需重新透過 BLE 配網**。

### flash 用量要自己換算

用 `PartitionScheme=custom` 時，arduino-cli 印的百分比是拿**整顆晶片**當分母（顯示為 16MB），
不是實際的 app0 分區。真實分母請看 `partitions.csv` 的 `app0`：**0x1F0000 = 2,031,616 bytes**。

例：1.6.1 編譯出 1,433,347 bytes，arduino-cli 印「8%」，對 app0 實際是 **70.5%**。

---

## 已知硬體限制：開機瞬間繼電器短暫通電

> **這是硬體限制，韌體無法根治。** 不要再嘗試用軟體解決，只能靠改板子。

### 症狀

插著設備上電（或按 RESET）時，繼電器會**短暫通電再斷掉**（聽得到咔一聲），
之後才進入正常的斷電狀態。預期行為應該是全程保持斷電。

### 成因

繼電器接的 GPIO 4 與 GPIO 7 在 ESP32-C3 上是外部 JTAG 腳（MTMS / MTDO），
晶片 reset 後由 ROM 依 JTAG 功能配置，**不保證維持低電位**。
從上電 → bootloader → 跑到第一行使用者程式，這段約 **200~500ms** 的空窗期內，
腳位狀態完全由晶片自己決定，任何韌體都插不了手。
浮空或被拉高的 MOS gate 一旦超過導通門檻（約 1~2V），繼電器就會動作。

### 韌體已做的緩解

`initRelayPins()` 放在 `setup()` **最前面**，早於 `Serial.begin()` 與其後的 `delay(1000)`，
把導通窗口從原本的 1 秒以上壓到僅剩上述 ROM 空窗。
**修改 `setup()` 時務必保持它在第一行**，任何插到它前面的程式都會直接拉長繼電器誤動作時間。

同時 GPIO 4 與 7 兩支都會被初始化拉低，因此**不存在「未使用的腳浮空」的情況**。
這一併消除了舊版依板號手動設定腳位、設錯就導致繼電器恆閉燒毀設備的風險。

### 根治方式（需改硬體）

在 MOS gate 對地加 **10kΩ 下拉電阻**。
上電瞬間即把 gate 釘在 0V，ROM 空窗完全消失，這也是 MOS 驅動電路的標準做法。
目前 341305A_P25_250814 與 341305A_Y176_250318 兩版板子皆未配置，
**下一版 layout 應補上**。已出貨的板子只能維持現狀的緩解程度。

---

## LED 狀態指示

| 狀態 | LED 行為 |
|------|----------|
| BLE 配對模式（含長按清除設定後）| 快閃 200ms（`PAIRING_BLINK`），持續不熄燈 |
| WiFi 未連接 | 快閃 300ms（`QUICK_BLINK`），滿 30 秒（`LED_TIMEOUT`）後轉為心跳閃：每 3 秒亮 100ms（`HEARTBEAT_PERIOD` / `HEARTBEAT_ON`），連不上就一直閃不熄燈 |
| WiFi 已連、MQTT 未連 | 一長二短 |
| WiFi 與 MQTT 都已連上 | 熄燈 |
| 長按重置確認中 | 閃爍 250ms（`BLINK_INTERVAL`），確認後長亮 0.7 秒 |

長按清除設定後，EEPROM 全 0 會被 `loadWiFiConfig()` 判定為「有效但空白」，
覆蓋掉編譯期預設的 `HBTech`，因此設備會進入 BLE 配對模式持續快閃，**不會自己去連預設 WiFi**。
硬編碼的預設 WiFi 只在晶片出廠、EEPROM 從未寫過時生效一次。

---

## 長按重置設定

BOOT(GPIO 9) 或 RESET(GPIO 1) **任一顆**，總共按住 5 秒：

1. 按住滿 3 秒（`LONG_PRESS_TIME`）→ LED 開始以 250ms 週期閃爍
2. 閃爍期間**持續按住**再 2 秒（`BLINK_CONFIRM_TIME`）→ LED 長亮 0.7 秒（`CONFIRM_SOLID_TIME`）
3. 清除 EEPROM 全部 160 bytes（`EEPROM_SIZE`）、還原 WiFi driver 的 NVS 設定，
   並重啟 → 進入 BLE 配對模式（200ms 持續快閃）

中途放開即取消、計時歸零。WiFi 連線等待期間共用同一支 `waitForResetConfirm()`，
行為與正常運作時一致。

（1.7.0 之前另有一支 `interruptibleDelay()` 也共用它，該函式的呼叫點全在
1.7.0 重寫掉的 WiFi 重連區段裡，已一併移除。）

### 開機按鈕自檢

`checkStuckButtons()` 在 `setup()` 中 `pinMode(..., INPUT_PULLUP)` 之後、任何重置流程之前，
取樣兩支按鈕腳 500ms。整段都是 LOW 的腳判定為短路／未接，**本次開機停用其重置功能**：

```
⚠ 按鈕自檢: RESET(GPIO 1) 恆為 LOW，本次開機停用其重置功能
```

這是為了擋掉「開機即清除設定 → 重啟 → 再清除」的無限迴圈（2026-08-14 實際發生過，
RESET 按鈕 GPIO 1 內部短路）。副作用：「按住按鈕再上電」會被擋掉，放開後重新上電即恢復。
詳見 `.claude/rules/button-pin-stuck-low.md`。

---

## 設備 ID 與 MQTT 主題

設備 ID 格式 `hoban-{MAC}`，MAC 為**網路正序**（與 `WiFi.macAddress()` 一致）。

```
狀態發布: hoban/{device_id}/status
控制訂閱: hoban/{device_id}/control
```

1.6.0 修正了 `getDeviceId()` 把 `ESP.getEfuseMac()` 的位元組**反序**輸出的缺陷。
為避免 OTA 後尚未更新的 App 失聯，韌體會**同時訂閱舊版反序主題**
（`getLegacyDeviceId()`），收到時序列埠會印：

```
⚠ 收到舊版主題指令（App 尚未更新設備 ID）
```

狀態一律只發布到新的正序主題。待所有 App 完成遷移後，可移除 `getLegacyDeviceId()`
與兩處 `legacyControlTopic` 訂閱。

---

## EEPROM 佈局

`EEPROM.begin(160)`，欄位常數定義在 `ho_relay2.ino` 的 `EE_*`：

| 位址 | 長度 | 欄位 |
|------|------|------|
| 0～31 | 32 | `ssid` |
| 32～63 | 32 | `password` |
| 64～95 | 32 | `mqttServer` |
| 96 | 1 | `useCustomServer` |
| 97 | 1 | 保留（未使用） |
| 98～113 | 16 | `mqttUsername` |
| 126～127 | 2 | `mqttPort`（低位元組在前） |
| 130～145 | 16 | `mqttPassword` |

**新增欄位務必先看這張表。** 1.7.3 之前 `mqttPassword` 排在 114～129，
與 `mqttPort`(126/127) 重疊、且尾端 2 bytes 超出 `begin(128)` 的範圍，
密碼只有前 12 字元是可靠的。

---

## 版本記錄

### 1.7.4

**OTA 加上 MD5 驗證。** 起因是 2026-09-09 實測：同一份原始碼 **USB 燒進去一切正常、
OTA 傳下去就開不起來**，而開不起來在這塊板子上等同**繼電器恆閉合**
（MOS gate 無下拉電阻，見 `.claude/rules/relay-stuck-on-diagnosis.md`）——
捕捉籠上最危險的失效方向。使用者回報舊版本也發生過，是長期問題。

真正讓壞映像檔生效的是收尾那行 `Update.end(true)`：

- `end(evenIfRemaining = true)` 會**跳過「寫滿了沒」的檢查**，把 `_size` 改成已寫入量直接收工
- 而 `_verifyEnd()` 在沒有設定 MD5 時**只認映像檔開頭的 `0xE9` magic byte**
- 兩者相加 → 只要前幾個位元組像個映像檔，後面全錯也會被接受，`otadata` 照樣切過去

修正涵蓋三處（韌體／`publish.py`／App）：

- 韌體 `startFirmwareUpdate()` 多收 `expectedMd5`，**缺合法 MD5（32 個十六進位字元）一律拒絕**
  並回報 `update_rejected_no_md5`；`Update.begin()` 後呼叫 `Update.setMD5()`
  （順序不可對調，`begin()` 會重置 MD5 狀態）
- `Update.end(true)` 改為 `Update.end()`；失敗路徑補上 `Update.abort()`，
  不 abort 的話半套映像檔會留在 OTA 分區、下一輪 `begin()` 也會失敗
- 指令解析的 `StaticJsonDocument` 由 200 放大到 384（version + url + md5 三個欄位）
- `publish.py` 發佈時算出 `.bin` 的 MD5 寫進 Firestore（Python 與 Node.js 兩條路徑都寫）
- App 讀 `md5` 欄位，格式不合直接擋在確認對話框之前並說明原因，合法才送出

**這不是把 OTA 修好，是把失效方向改掉。** 映像檔為何會在 OTA 途中損壞仍然不明；
加了 MD5 之後驗證失敗會整份作廢、**`otadata` 不切換**，設備留在原本跑得動的韌體上，
表現從「變磚 + 門恆開」變成「更新沒成功，繼續運作」。

**相容性**：現場舊韌體用 `StaticJsonDocument<200>`，實測送出 158 bytes 的含 md5 指令
（網址長度與真實發佈相同）解析成功並回報 `updating`，**不需要依版本決定送不送 md5**。

**實測狀態**：三處均通過編譯／語法／`flutter analyze`，並已在實機上驗證舊韌體解析相容性。
MD5 驗證本身的正向路徑（下載成功並通過比對）**尚未實機驗證**。

### 1.7.3

**修正「清除 WiFi 設定後第一次綁定必定失敗、第二次才成功」。**
僅在「設備原本已連上某個 AP」時發生，因為沒連上過就不會有舊 AP 被寫進 NVS。

根因是**清除設定只清了 EEPROM，沒清 WiFi driver 存在 NVS 的舊 AP**，
開機後變成「driver 在背景連舊 AP」對撞「`connectToWiFi()` 在前景連新 AP」：

- **`WiFi.persistent(false)` 排在 `WiFi.mode()` 之後，等於沒有生效。**
  core 的 `persistent()` 只設一個旗標，真正生效的是 `mode()` 觸發的
  `wifiLowLevelInit()` 裡那句 `if (!_persistent) esp_wifi_set_storage(WIFI_STORAGE_RAM)`。
  順序寫反 → driver 用預設的 `WIFI_STORAGE_FLASH` 起來 → AP 照樣進 NVS。**已對調順序**
- **`clearWiFiConfig()` 只清 EEPROM。** 已補上 `WiFi.disconnect(false, true)`
  與 `esp_wifi_restore()`，把 driver 的持久化設定一併還原
- **探測期間沒有關掉 core 的 auto-reconnect。** `connectToWiFi()` 每次
  `WiFi.disconnect()` 都會觸發 core 的 `STA_DISCONNECTED` 分支去 `disconnect(); connect();`，
  與前景的 `esp_wifi_set_config()` + `esp_wifi_connect()` 對撞，
  4-way handshake 每次都被打斷 → 五種模式**全部** reason 15。
  **已改為進門關閉、收尾打開**（收尾那行不可省，`_autoReconnect` 在 deinit 後仍存活、不會自己恢復）

現場 log 的三個特徵都由此解釋：每個模式開頭的 reason 8（ASSOC_LEAVE，被自己人斷開）、
五種模式一致的 reason 15、以及收尾的 `E wifi:sta is connecting, cannot set config`。
「第二次就成功」是因為第一次探測已把 NVS 覆寫成新 AP，第二次前後景目標一致、不再互搶。

一併修正 **EEPROM 佈局越界重疊**：`mqttPassword` 舊位址 114～129 與 `mqttPort`(126/127)
重疊、尾端 2 bytes 超出 `begin(128)`，實際只有前 12 字元可靠。已搬到 130～145、
`EEPROM.begin(160)`，並改用 `EE_*` 常數定址。**其餘欄位一格未動**，
OTA 升級後既有設定照常運作，只有 MQTT 密碼需要重設。

**實測狀態**：僅通過編譯，**尚未實機驗證**。根因為靜態推論，
需實機重現「連線成功 → 清除 → 綁定新 SSID」確認第一次即可連上。

### 1.7.2

**修正「WiFi 連不上時完全沒有燈號」。** 兩個獨立缺陷疊在一起，
造成最需要指示燈的情境（設備始終連不上）反而一顆燈都不亮：

- **板載 LED 從未被驅動。** `setup()` 裡是 `pinMode(ledOnBoard, INPUT)`，
  註釋還誤標成「初始化第二個按鈕」。GPIO 3 設成輸入模式後，全檔案 14 處
  `digitalWrite(ledOnBoard, ...)` 全部推不動它。**已改為 `OUTPUT`**
- **斷線 30 秒後永久熄燈。** `blinkLED()` 的 `wifiDisconnectStart` 只有
  「WiFi 連上」才會歸零，而開機的 `connectToWiFi()` 每種 auth 模式就要等 10 秒、
  還要輪好幾種，等它跑完進 `loop()`，`LED_TIMEOUT` 早就用光 → 連不上的設備
  從頭到尾是暗的。**已改為滿 30 秒後轉低頻心跳閃**（每 3 秒亮 100ms，
  duty cycle 約 3%，比原本 50% 的快閃更省電，且永遠看得出「我還沒連上」）

沒有改成「每次重試重置計時」，因為補送 `esp_wifi_connect()` 的間隔是 10 秒
（`WIFI_KICK_INTERVAL_MS`）< `LED_TIMEOUT`，那樣等於退回全速快閃、省電完全失效。

**實測狀態**：僅通過編譯，**尚未實機驗證**。

### 1.7.0

**WiFi／MQTT 重連邏輯整段重寫。** 起因是「斷線後不會自動連線」，盤點後在重連路徑上
找出 13 項缺陷，詳見 `.claude/rules/wifi-mqtt-reconnect-antipatterns.md`。

四個根因：

- **連上了卻因為「太慢」被丟掉**（最致命）。`quickConnectToIndex()` / `quickConnectCustom()`
  舊碼在 `mqttClient.connect()` **成功之後**才判斷耗時，超過 1 秒就 `disconnect()` 並回 false。
  台灣連海外公共 broker 光 RTT 就 150~300ms，五台會**全部**被判太慢且沒有 fallback
  → 每台都連得上、設備卻永遠離線。**已改為連上就採用**，耗時只印警告
- **跟 core 的自動重連打架**。`WiFi.disconnect(true)` 的第一個參數是 `wifioff` 不是 `eraseap`，
  它會走到 `STAClass::onDisable()` 移除 WiFi 事件處理器、把 `_esp_netif` 設 NULL，
  而 core 的 auto-reconnect 正是掛在那個處理器上。**運行期不再拆 WiFi 子系統**，
  改為讓 core 自己重連、韌體只補送非阻塞的 `esp_wifi_connect()`
- **時間戳設在阻塞呼叫之前**。`connectToWiFi()` 可阻塞 50 秒以上，回來時 5 秒閘門早已過期
  → 「每 5 秒檢查」在失敗時等於背靠背連續重試。**所有時間戳改到阻塞呼叫之後才取**
- **`lastWiFiCheck = now + 25000` 的無號數比較**。差值恆為 4294942296，
  「暫停重試 30 秒」條件恆真、從未生效。**改用獨立的 `nextXxxAt` 變數與 wrap-safe 比較**

其他：

- `smartConnectStep()` 每次只試一台 broker，單次阻塞從最壞 90 秒壓到單台成本；
  `FIND_BEST_SERVER` 指令也改走這條路（原本在 callback 裡直呼 `smartConnect()`，阻塞 111 秒）
- `currentServerIndex` 改為失敗也輪替，不再連續重試同一台死 broker
- 早退原因碼補上 210/211/212；`setScanTimeout(8000)`（core 預設 60 秒）
- `connectToWiFi()` 收尾會**還原 auth config 並重套 `setSleep(false)` / `setTxPower()`**
  —— 這兩者是驅動層設定，`esp_wifi_deinit()` 後不會自動恢復
- 移除死變數 `failedAttempts`（三處寫入、零處讀取）與死函式 `interruptibleDelay()`

**實測狀態**：僅通過編譯與兩輪對抗性複審，**尚未實機驗證**。
`mqttClient.loop()` 的最大沉默窗口由靜態推演從 113 秒降至約 13 秒（broker 踢人門檻 45 秒），
AP 斷電 30 秒的恢復時間由 60~190 秒降至約 31~39 秒——**兩者都是靜態推演，無上界保證**。

### 1.6.1

- BLE 配對模式（含長按清除設定後）的 LED 由 1000ms 慢閃改為 **200ms 持續快閃**，
  與「WiFi 連不上」的 300ms 快閃刻意錯開以便肉眼分辨
- 序列埠的「已發布狀態」日誌加上 device_id，多台設備同時看 log 時可分辨來源

### 1.6.0

- **繼電器改為 GPIO 4 與 GPIO 7 同時驅動**，單一韌體通吃兩版板子，
  並以 `initRelayPins()` 在 `setup()` 第一行拉低兩支腳，消除「未使用的腳浮空導致 MOS 誤導通」的風險
- **修正 `getDeviceId()` 的 MAC 反序缺陷**，改為網路正序；同時訂閱舊版反序控制主題以相容尚未更新的 App
- **新增開機按鈕自檢 `checkStuckButtons()`**，擋掉按鈕短路造成的無限清除迴圈
- 長按重置改為「3 秒 → 250ms 閃爍 → 再 2 秒 → 長亮 0.7 秒」，並修掉 `buttonPressTime == 0`
  時 `millis() - 0` 會瞬間超過門檻而立刻重置的缺陷
- `device["relay"]` 改讀 `relayState` 變數，不再 `digitalRead()` 單一腳位

### 1.5.1 以前

見 git log。