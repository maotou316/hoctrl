# hoBattery1：ESP32-C3 SuperMini 電量偵測模組設計

日期：2026-09-27
狀態：待審

## 1. 目標與範圍

新型號 `hoBattery1`（新專案目錄 `ho_battery1/`），量測一組 **2S 鋰電（6.0～8.4V）** 的電壓與電量，
透過 MQTT 回報。模組**由被測電池本身供電**，以深度睡眠（deep sleep）定時喚醒，
**每 10 分鐘**回報一次。

### 本次範圍
- 韌體：量測、WiFi（寫死）、MQTT 多 broker、OTA、深度睡眠排程
- 硬體接線規格（降壓、分壓、腳位）
- 給 App 的對接規格（第 9 節），**App 本次不改**

### 不做（YAGNI）
- 低電量告警訊息（只做「過低不開 WiFi」的電池保護，見 5.2）
- 遠端調整喚醒間隔（固定 600 秒）
- BLE 配網、EEPROM 設定、Web 管理介面、AP 模式（WiFi 寫死，見第 4 節）
- 同時連線多台 broker 發布（只發到一台，App 本來就監聽全部 4 台）

### 成功標準
1. 可調電源 6.0／7.4／8.4V 下，回報的 `battery.mv` 誤差 ≤ ±2%
2. 深度睡眠時整機電流（含降壓與分壓）≤ 300µA（5 倍分壓模組本身就漏約 220µA；改自焊 1MΩ/220kΩ 分壓時目標為 ≤ 60µA）
3. WiFi 連上後到入睡 ≤ 20 秒；WiFi 本身最多等 60 秒（正常狀況整次約 3～6 秒）
4. MQTT Explorer 能在任一預設 broker 看到 retained 狀態
5. retained `update:` 指令能在下一次醒來完成 OTA，完成後 retained 指令被清除

## 2. 硬體

| 項目 | 規格 | 理由 |
|---|---|---|
| 開發板 | ESP32-C3 SuperMini | 使用者指定 |
| 供電 | 2S 電池 → **低 Iq 降壓模組 → 3.3V → SuperMini 的 3V3 腳** | 板上 LDO（ME6211）輸入上限約 6V，**8.4V 接 5V 腳會燒板**。降壓晶片建議 Iq ≤ 30µA 且可供 ≥ 500mA（WiFi 發射峰值約 350mA），例如 AP63203 |
| 分壓 | 與 hoRelay2 相同的 5 倍分壓模組（30kΩ/7.5kΩ，比例 5.0），S 腳對 GND 加 **100nF** | 使用者手上現成的模組（2026-09-27 決定）。8.4V → 1.68V，落在 ADC 11dB 線性區；常時漏電約 0.22mA，是睡眠電流的大宗。日後要更省電可改自焊 1MΩ/220kΩ（漏電約 7µA，比例 5.545） |
| ADC 腳 | **GPIO 3**（ADC1_CH3） | C3 只有 GPIO 0～4 是 ADC1；ADC2 在 WiFi 開啟時讀不到；GPIO 2 是 strapping pin |
| LED | GPIO 8（板載，低電位亮） | SuperMini 板載 |
| 按鈕 | BOOT（GPIO 9）、RESET（EN） | 板載 |
| 電源 LED | **必須拆掉**（或拆限流電阻） | SuperMini 板上另有一顆接 3V3 的常亮電源指示 LED，只要有電就亮，常時耗電約 1～3mA，遠超過睡眠電流目標（≤ 300µA），不拆掉這個目標必然達不到 |

S 腳的 100nF：模組輸出阻抗約 6kΩ，擋不住 ADC 取樣電容造成的抖動。上電後量測前要等電容穩定（見 5.1，`analogSetPinAttenuation()` 之後 `delay(20)`）。

電阻誤差造成的比例偏差以 `battery::kScalePermille` 常數校正，首台實機用電表量一次後寫死。

## 3. 架構與檔案

依本 repo 慣例為單一 `.ino` 專案，但把純計算抽成可獨立驗證的標頭檔：

```
ho_battery1/
├── ho_battery1.ino     # setup() 內跑完整個喚醒流程；loop() 為空
├── battery_curve.h     # 純函式：mV → 百分比查表、ADC mV → 電池 mV（無硬體相依）
├── config.h            # WiFi SSID／密碼（寫死）
├── partitions.csv      # 與 hoRelay2 相同（OTA 需要雙 app 分區）
└── readme.md           # 接線圖、操作方式、版本記錄
```

深度睡眠醒來等同重新開機，所以**主流程寫在 `setup()`**，結尾呼叫 `esp_deep_sleep_start()`；
`loop()` 為空。

跨醒來保存的狀態放在 `RTC_DATA_ATTR`（深度睡眠保留、斷電清除）：

| 變數 | 用途 |
|---|---|
| `bootCount` | 醒來次數，放進狀態 JSON 便於除錯 |
| `lastBrokerIndex` | 上次連線成功的 broker，下次優先試 |
| `wifiChannel`、`wifiBssid[6]`、`wifiCacheValid` | WiFi 快速連線快取 |
| `consecutiveFailures` | 連續連不上的次數（決定退避，見 5.4） |

## 4. WiFi 設定（寫死）

使用者決定不做配網，WiFi 寫死在 `config.h`：

```cpp
#define WIFI_SSID     "HBTech"
#define WIFI_PASSWORD "94051311"
```

- 不使用 EEPROM、不開 BLE（BLE 堆疊也不編入，韌體更小、開機更快）
- 不支援自訂 MQTT 伺服器，只用 4 台預設 broker
- 設備 ID 同 hoRelay2：`hoban-{MAC}`
- 代價：換 WiFi 只能改 `config.h` 後重新燒錄或 OTA；若 WiFi 已連不上，OTA 也收不到，只能 USB 燒錄
- `config.h` 會進版控（與 hoRelay2 的 `DEFAULT_SERVERS` 寫死帳密同一做法）

## 5. 喚醒流程

```
醒來 ─► 量電池 ─► 過低？──是──► 睡 1 小時
                  │否
                  ▼
          連 WiFi（≤60s）─失敗─► 睡 60 秒後重試（不退避）
                  ▼
          連 MQTT（依序試）─全失敗─► 睡（退避）
                  ▼
          NTP 對時（≤2s，失敗不中止）
                  ▼
          發 retained 狀態 ─► 訂閱 control，等 1.5s 收 retained 指令
                  ▼
          處理 OTA 指令 ─► 睡 600 秒
```

WiFi 連上之後設**20 秒預算**（`wake::kWakeBudgetMs`，從 WiFi 連上起算）；WiFi 本身另有 60 秒上限。任何步驟超過預算就直接進睡眠，
避免 WiFi 或 broker 異常時設備一直醒著把電池吃光。OTA 下載除外（見 5.6）。

### 5.1 量測
- **在開 WiFi 之前量**：WiFi 發射時電流大，電池內阻造成的壓降會讓讀值偏低
- `analogReadResolution(12)`、第一次讀之後 `analogSetPinAttenuation(pin, ADC_11db)`
- `analogSetPinAttenuation()` 之後 `delay(20)`：讓 ADC 腳切換到新的衰減設定後、分壓中點的
  100nF 電容重新穩定，不然讀值會偏低且抖動
- 用 `analogReadMilliVolts()`（有 eFuse 出廠校準），不用 `analogRead()` 自己乘係數
- 先丟棄 2 次、再取 16 次平均
- 電池 mV = ADC mV × `BATTERY_SCALE`
- 百分比沿用 hoRelay2 的 2S 放電曲線查表（線性內插）：

  | mV | 8400 | 8120 | 7960 | 7840 | 7740 | 7640 | 7580 | 7540 | 7480 | 7360 | 6900 | 6000 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | % | 100 | 90 | 80 | 70 | 60 | 50 | 40 | 30 | 20 | 10 | 5 | 0 |

- `valid` 判定：ADC mV 在 900～2400 之間（電池約 5.0～13.3V）才為 true；
  超出代表分壓脫落或接錯，`percent` 回報 -1

### 5.2 電池保護
- 電池 < **6200mV**（每顆 3.1V）→ 不開 WiFi，睡 **1 小時**
- 理由：WiFi 連線一次約耗 0.1～0.3mAh，且大電流會讓低電量電池瞬間掉到保護板斷電點；
  鋰電過放會永久損傷
- 代價：電池過低期間 App 收不到更新（retained 會停在最後一筆）。
  若 `valid == false` 則**不套用**此保護（讀值不可信時不能據此停止回報）

### 5.3 WiFi 與 MQTT 連線
- WiFi：若 RTC 快取有效，用 `WiFi.begin(ssid, pass, channel, bssid)` 快速連線；
  失敗一次就清快取改一般連線。總上限 **60 秒**（2026-09-27 使用者要求「盡量連上再睡」，原為 10 秒）
- WiFi 60 秒內連不上：**只睡 60 秒就重試，不走退避**。代價：分享器長時間故障時耗電約為一直醒著的一半
- MQTT 伺服器順序：`lastBrokerIndex` → 其餘預設伺服器，每台 5 秒
- 預設伺服器清單與 hoRelay2 的 `DEFAULT_SERVERS` 相同（4 台，皆 1883、無帳密）
- Client ID：`{device_id}-{bootCount}`，避免上一次連線尚未被 broker 清掉時被踢
- PubSubClient 緩衝區 `setBufferSize(512)`
- **退避**（只用於 MQTT 連不上或發布失敗，WiFi 失敗不退避）：連續失敗時睡眠時間 600s → 1200s → 最多 3600s，成功一次即重置

### 5.4 發布狀態
- 主題 `hoban/{device_id}/status`，**retained**，QoS 0
- 發布後呼叫 `mqttClient.loop()` 並等 200ms，確保封包送出後才斷線

### 5.5 接收指令
- 訂閱 `hoban/{device_id}/control`，持續 `loop()` 1.5 秒收 broker 重播的 retained 訊息
- 支援指令：

  | 指令 | 行為 |
  |---|---|
  | `update:{JSON}` | OTA，格式與 hoRelay2 相同；**`md5` 必填**，缺少回報 `update_rejected_no_md5` |
  | `status` | 忽略（醒來本來就會發） |

- 處理 `update:` **之前**先發一則空的 retained 訊息到 control 主題清除它，
  否則每次醒來都會重複執行（OTA 失敗時會無限重試下載，耗盡電池）
- **操作規則：update 指令每次都要以 retained 同時發到全部 4 台 broker**——設備只從上次連上的
  那台收指令，只發部分 broker 會有時收不到，日後換 broker 時還可能收到某台上殘留的舊指令而降版；
  放棄某次更新時，一樣要在全部 4 台都發空的 retained 訊息清除，否則沒清到的那台會在設備連上它時
  把舊指令重新交出來
- 若清除 retained 指令失敗（例如發布當下連線剛好斷了），本次醒來放棄處理該指令，
  不嘗試刷機；retained 指令仍留在 broker 上，下次醒來會重新走一次清除
- 若指令中的 `version` 與目前執行中的 `firmwareVersion` 相同，只清除 retained 指令、
  不進行刷機（多半是同一個 retained 指令被重複送達，不需要也不應該重刷）
- OTA 下載期間不受 20 秒預算限制，但設 **120 秒**上限；下載迴圈中每約 1 秒呼叫一次
  `mqttClient.loop()` 讓 MQTT 連線保持存活（下載耗時遠超過 keepAlive 15 秒，
  完全不呼叫會被 broker 判定斷線，導致下載完成後的結果訊息發不出去）；
  下載前要求電池 ≥ 7000mV（約 10%），不足則回報 `update_rejected_low_battery` 並跳過
- OTA 結果字串（`updating`、`update_success`、`update_failed`、`update_rejected_*`）與 hoRelay2 相同，
  但**不 retained**：休眠設備的 retained 狀態是 App 平常唯一看得到的資料，被字串蓋掉就要等 10 分鐘才恢復成 JSON。
  OTA 成功重啟後會立刻發一則新的 retained 狀態（`wake_reason: software`）
- **不設 LWT（遺囑訊息）**：休眠設備每次都會斷線，異常斷線時 LWT 會用 offline 蓋掉最新的 retained 狀態

## 6. 狀態 JSON

欄位名稱**沿用 hoRelay2**（`version`、`model`、`timestamp`、`wifi`、`battery`），
讓 App 現有的解析直接可用；新增的欄位 App 不認得也不會出錯。

```json
{
  "device_id": "hoban-a0b1c2d3e4f5",
  "status": "sleeping",
  "version": "1.0.0",
  "model": "hoBattery1",
  "timestamp": 4,
  "measured_at": 1790000000,
  "wifi": { "connected": true, "ssid": "MyWiFi", "rssi": -61, "ip": "192.168.1.23" },
  "battery": { "mv": 7820, "percent": 72, "valid": true },
  "sleep": { "interval_s": 600, "next_wake_s": 600, "boot_count": 123, "wake_reason": "timer" }
}
```

| 欄位 | 說明 |
|---|---|
| `status` | `sleeping`（發完就睡）／`updating`（OTA 中） |
| `timestamp` | 本次醒來經過的秒數（與 hoRelay2 同義：`millis()/1000`） |
| `measured_at` | 量測時的 Unix 時間（秒）；深度睡眠期間 RTC 時鐘會繼續走，NTP 對時失敗時沿用「上次對時成功的時間＋經過的時間」推算，只有從未對時成功過才為 0 |
| `sleep.next_wake_s` | 這次實際要睡多久（含退避），App 可據此推算「下次應該何時回報」 |
| `sleep.wake_reason` | `timer`（定時喚醒）／`power_on`（上電或按 RESET；C3 的 RESET 是 EN 腳，晶片分不出兩者）／`software`（OTA 後重啟） |

估計約 330 bytes，緩衝區 512 足夠；加欄位前要重算。

## 7. LED

平時完全不亮（省電）。

| 狀態 | 行為 |
|---|---|
| 醒來工作中 | 不亮 |
| 發布成功（僅非 timer 喚醒） | 亮 0.5 秒，讓按 RESET 的人知道有成功回報 |

## 8. 錯誤處理

| 狀況 | 行為 |
|---|---|
| 讀值 `valid == false` | 照常回報（`percent: -1`），不套用電池保護 |
| WiFi 60 秒內連不上 | 睡 60 秒後重試，不退避 |
| MQTT 全失敗 | 退避睡眠，不重試 |
| NTP 失敗 | `measured_at` 沿用 RTC 時鐘推算的時間（深度睡眠期間持續走），照常發布；只有從未對時成功過才是 0 |
| 發布失敗 | 不重試，當作連線失敗計入退避 |
| OTA 失敗 | 回報錯誤原因後睡 600 秒，retained 指令已先清除所以不會重試 |
| 20 秒預算用完 | 直接睡眠 |

## 9. App 對接（另案，本次不做）

以下是 App 端要支援 hoBattery1 時需要處理的事，寫在這裡讓另案有依據：

1. **離線判定**：現行 `DeviceTimeoutPolicy` 約 36 秒判離線，休眠設備會永遠顯示離線。
   `model == "hoBattery1"` 或 `status == "sleeping"` 應改用
   「`measured_at` 或收到時間 + `sleep.next_wake_s` + 寬限」判斷是否逾期
2. **retained 舊快照**：現行 App 不把 retained 的 online 當作新鮮證據。休眠設備平常只會
   以 retained 形式被看到，需要改用 `measured_at` 判斷新舊
3. **下指令要 retained**：OTA 對休眠設備必須以 **retained** 發布，
   否則設備醒來時收不到；UI 要提示「將於下次醒來（最多 N 分鐘）執行」
4. **電量顯示**：`battery` 物件格式與 hoRelay2 相同，`battery_display.dart` 可直接沿用
5. **新增設備**：hoBattery1 沒有 BLE，App 無法用藍牙配對新增，需要另一種新增方式（例如輸入／掃描設備 ID）
6. **Firestore 韌體發佈**：`firmware_updates/hoBattery1` 要登記，`publish.py` 要加新型號

## 10. 驗證

| 項目 | 方法 |
|---|---|
| 分壓校正 | 可調電源輸出 6.0／7.4／8.4V，對照 Serial 印出的 mV 與電表，調整 `BATTERY_SCALE` |
| 百分比查表 | `battery_curve.h` 的純函式用邊界值手算對照（6000→0、8400→100、中間內插） |
| 睡眠電流 | 電表串在電池端量，目標 ≤ 300µA（其中分壓模組約 220µA） |
| 電池保護 | 可調電源調到 6.1V，確認不開 WiFi、Serial 印出睡 3600 秒 |
| 發布 | MQTT Explorer 訂閱 `hoban/+/status`，確認 retained 內容 |
| WiFi 重試 | 關掉分享器，確認每次等 60 秒後睡 60 秒 |
| 退避 | 讓 MQTT 連不上（例如分享器能連但封鎖 1883 port），確認睡眠時間依序 600→1200→2400→3600 |
| 手動喚醒 | 按 RESET，確認立即回報且 LED 亮 0.5 秒 |
| OTA | 以 retained 發 `update:`（含 md5），確認下次醒來更新、指令被清除；再測缺 md5 被拒 |

## 11. 已知限制

- 最多 10 分鐘（MQTT 退避時最多 1 小時）才看得到新數據，指令也要等到下次醒來
- retained 狀態只存在設備當時連上的那台 broker；換 broker 後舊 broker 上會留一筆舊快照，
  App 要以 `measured_at` 取最新的一筆
- 電池保護期間不回報，App 看到的是最後一筆（電量會停在約 6.2V 那筆）
- 公共 broker 不保證永久保存 retained 訊息
