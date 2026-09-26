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
2. 深度睡眠時整機電流（含降壓與分壓）≤ 60µA
3. 單次醒來到入睡 ≤ 20 秒（正常狀況約 3～6 秒）
4. MQTT Explorer 能在任一預設 broker 看到 retained 狀態
5. retained `update:` 指令能在下一次醒來完成 OTA，完成後 retained 指令被清除

## 2. 硬體

| 項目 | 規格 | 理由 |
|---|---|---|
| 開發板 | ESP32-C3 SuperMini | 使用者指定 |
| 供電 | 2S 電池 → **低 Iq 降壓模組 → 3.3V → SuperMini 的 3V3 腳** | 板上 LDO（ME6211）輸入上限約 6V，**8.4V 接 5V 腳會燒板**。降壓晶片建議 Iq ≤ 30µA 且可供 ≥ 500mA（WiFi 發射峰值約 350mA），例如 AP63203 |
| 分壓 | 上臂 1MΩ、下臂 220kΩ（比例 5.545），S 腳對 GND 加 **100nF** | 8.4V → 1.515V，落在 ADC 11dB 線性區；常時漏電約 7µA。**不可用 hoRelay2 的 30k/7.5k 模組**（漏 0.22mA，是睡眠電流的數倍） |
| ADC 腳 | **GPIO 3**（ADC1_CH3） | C3 只有 GPIO 0～4 是 ADC1；ADC2 在 WiFi 開啟時讀不到；GPIO 2 是 strapping pin |
| LED | GPIO 8（板載，低電位亮） | SuperMini 板載 |
| 按鈕 | BOOT（GPIO 9）、RESET（EN） | 板載 |

分壓電阻的 100nF 在高阻抗分壓下是**必要**的：ADC 取樣電容從 181kΩ 等效源阻抗充電不夠快，
沒有電容時讀值會偏低且抖動。上電後量測前要等電容穩定（見 5.1 的 `delay`）。

電阻誤差造成的比例偏差以 `BATTERY_SCALE` 常數校正，首台實機用電表量一次後寫死。

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
          連 WiFi（≤10s）─失敗─► 睡（退避）
                  ▼
          連 MQTT（依序試）─全失敗─► 睡（退避）
                  ▼
          NTP 對時（≤2s，失敗不中止）
                  ▼
          發 retained 狀態 ─► 訂閱 control，等 1.5s 收 retained 指令
                  ▼
          處理 OTA 指令 ─► 睡 600 秒
```

整體設**20 秒硬上限**（`WAKE_BUDGET_MS`）：任何步驟超過預算就直接進睡眠，
避免 WiFi 或 broker 異常時設備一直醒著把電池吃光。OTA 下載除外（見 5.6）。

### 5.1 量測
- **在開 WiFi 之前量**：WiFi 發射時電流大，電池內阻造成的壓降會讓讀值偏低
- `analogReadResolution(12)`、第一次讀之後 `analogSetPinAttenuation(pin, ADC_11db)`
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
  失敗一次就清快取改一般連線。總上限 10 秒
- MQTT 伺服器順序：`lastBrokerIndex` → 其餘預設伺服器，每台 5 秒
- 預設伺服器清單與 hoRelay2 的 `DEFAULT_SERVERS` 相同（4 台，皆 1883、無帳密）
- Client ID：`{device_id}-{bootCount}`，避免上一次連線尚未被 broker 清掉時被踢
- PubSubClient 緩衝區 `setBufferSize(512)`
- **退避**：連續失敗時睡眠時間 600s → 1200s → 最多 3600s，成功一次即重置。
  防止分享器斷線期間每 10 分鐘空耗一次 10 秒的連線

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
- OTA 下載期間不受 20 秒預算限制，但設 **120 秒**上限；
  下載前要求電池 ≥ 7000mV（約 10%），不足則回報 `update_rejected_low_battery` 並跳過
- OTA 進度／結果發到 `hoban/{device_id}/status`（`status: "updating"`），格式同 hoRelay2

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
| `measured_at` | 量測時的 Unix 時間（秒）；NTP 失敗時為 0 |
| `sleep.next_wake_s` | 這次實際要睡多久（含退避），App 可據此推算「下次應該何時回報」 |
| `sleep.wake_reason` | `timer`／`reset`／`power_on` |

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
| WiFi／MQTT 全失敗 | 退避睡眠，不重試 |
| NTP 失敗 | `measured_at: 0`，照常發布 |
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
| 睡眠電流 | 電表串在電池端量，目標 ≤ 60µA |
| 電池保護 | 可調電源調到 6.1V，確認不開 WiFi、Serial 印出睡 3600 秒 |
| 發布 | MQTT Explorer 訂閱 `hoban/+/status`，確認 retained 內容 |
| 退避 | 關掉分享器，確認睡眠時間依序 600→1200→2400→3600 |
| 手動喚醒 | 按 RESET，確認立即回報且 LED 亮 0.5 秒 |
| OTA | 以 retained 發 `update:`（含 md5），確認下次醒來更新、指令被清除；再測缺 md5 被拒 |

## 11. 已知限制

- 最多 10 分鐘（退避時最多 1 小時）才看得到新數據，指令也要等到下次醒來
- retained 狀態只存在設備當時連上的那台 broker；換 broker 後舊 broker 上會留一筆舊快照，
  App 要以 `measured_at` 取最新的一筆
- 電池保護期間不回報，App 看到的是最後一筆（電量會停在約 6.2V 那筆）
- 公共 broker 不保證永久保存 retained 訊息
