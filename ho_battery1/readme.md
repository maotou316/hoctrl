# hoBattery1：ESP32-C3 SuperMini 電量偵測模組

## 用途

量測一組 2S 鋰電（6.0～8.4V）的電壓與電量，透過 MQTT 回報；模組**由被測電池本身供電**，
平常深度睡眠，每 10 分鐘醒來量測、回報一次就再睡。設計規格：
`docs/superpowers/specs/2026-09-27-hobattery1-design.md`。

## 接線

| 訊號 | 接法 |
|---|---|
| 供電 | 電池 → 低 Iq 降壓模組（Iq ≤ 30µA、可供電 ≥ 500mA，例如 AP63203）→ SuperMini 的 **3V3 腳** |
| 分壓 | 上臂 **1MΩ**、下臂 **220kΩ**，中點接 **GPIO 3**（ADC1_CH3）；中點對 **GND** 加 **100nF** |
| LED | 板載 GPIO 8（低電位亮），不需外接 |

> **警告：8.4V 絕對不可以接到 SuperMini 的 5V 腳！**
> 板上 LDO（ME6211）輸入上限約 6V，8.4V 直接接 5V 腳會燒板。降壓後的 3.3V 只能接 **3V3** 腳。

其他接線細節：
- 分壓中點的 100nF **必要**：ADC 取樣電容從約 181kΩ 等效源阻抗充電不夠快，沒有這顆電容讀值會偏低且抖動
- 用 GPIO 3 是因為 ESP32-C3 只有 GPIO 0～4 是 ADC1（ADC2 在 WiFi 開啟時讀不到），且 GPIO 2 是 strapping pin 不能用
- **不可沿用 hoRelay2 的 30k/7.5k 分壓模組**：常時漏電約 0.22mA，是這顆模組睡眠電流目標（≤ 60µA）的數倍

## 操作

- WiFi 帳密**寫死**在 `config.h`（目前 SSID：`HBTech`）；換 WiFi 只能改這裡重新燒錄或 OTA，
  若舊 WiFi 已連不上，OTA 也收不到，只能接 USB 燒錄。沒有 BLE、沒有 EEPROM、沒有 AP 模式
- **上電或按 RESET** 會立即量測並回報一次（定時喚醒則不等 USB、LED 不亮，省電）；
  回報成功後 LED 會亮 **0.5 秒**，代表這次有成功送出。平常定時喚醒 LED 全程不亮
- 電池低於 **6200mV**（每顆 3.1V）時不會開 WiFi，直接睡 1 小時以避免鋰電過放；
  此狀態下 App 端只會看到最後一筆 retained 狀態，要等電量回升才會再更新

## MQTT

| 主題 | 用途 |
|---|---|
| `hoban/{device_id}/status` | 狀態，**retained**，QoS 0 |
| `hoban/{device_id}/control` | 接收指令，**指令必須 retained 發送**——設備大部分時間在睡，非 retained 的指令永遠收不到 |

狀態 JSON 範例：

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

OTA 指令（發到 `control` 主題）：

```
update:{"url":"https://...","md5":"<32 碼 hex>","version":"1.0.1"}
```

- **必須用 retained 發送**，否則設備下次醒來收不到
- `md5` **必填**，缺少會回報 `update_rejected_no_md5`（不驗 TLS 憑證，MD5 是唯一確認映像檔沒壞、沒被掉包的依據）
- PubSubClient 收發共用 **512 bytes** 緩衝區，**指令超過約 470 bytes 會被靜默丟棄**（連 callback 都不會被觸發），OTA 網址要盡量短
- 若清除 retained 指令失敗（例如等待窗結束前連線剛好斷了），本次醒來放棄處理，下次醒來會重新試著清除，不會在沒清乾淨前先刷機
- 若指令裡的 `version` 與目前韌體版本相同，只會清除 retained 指令、**不會重新刷機**（多半是同一個 retained 指令被重複送達）
- 下載期間每秒會呼叫一次 `mqttClient.loop()` 讓 MQTT 連線保持存活（下載最長可跑 120 秒，遠超過 keepAlive 15 秒，完全不呼叫會被 broker 判定斷線，導致之後的成功／失敗結果發不出去）
- 電量低於 7000mV（約 10%）會拒絕下載，回報 `update_rejected_low_battery`

## 校正

分壓比例用千分比整數 `battery::kScalePermille`（`battery_curve.h`，目前 5545，對應 1MΩ/220kΩ 的理論比例）換算 ADC 讀值：

```cpp
電池 mV = ADC mV × kScalePermille / 1000
```

電阻本身有誤差，**首台實機要用可調電源＋電表校正**：輸出 6.0／7.4／8.4V，比較 Serial
印出的「電池 mV」與電表實測值，回推正確的 `kScalePermille` 寫回 `battery_curve.h`。

**改完 `kScalePermille` 一定要同步檢查 `selftest.h`**：裡面
`static_assert(battery::adcToBatteryMv(1515) == 8400, ...)` 是照原比例算好的期望值，
比例改了這行也要跟著改，否則編譯會直接失敗（這是刻意的編譯期防呆，不是 bug）。

## 燒錄

```powershell
.\flash.ps1 -Model battery -Upload -Port COMx
```

編譯輸出的容量百分比（例如「7% / Maximum 16777216」）是拿**整顆晶片**當分母，
這是 `PartitionScheme=custom` 下 `boards.txt` 的顯示方式，不是實際可用空間。
真實分母要看 `partitions.csv` 的 `app0`：`0x1F0000` = 2,031,616 bytes（約 2MB）。
目前韌體編譯出約 1,183,815 bytes，對 app0 實際使用約 **58%**。

## 實機驗證紀錄（Task 3～5）

硬體驗證尚未進行，以下項目待補上實測數據：

| 項目 | 狀態 |
|---|---|
| 分壓校正誤差（6.0／7.4／8.4V，目標 ≤ ±2%） | 待實機驗證 |
| 睡眠電流（目標 ≤ 60µA） | 待實機驗證 |
| 醒著時間（目標 ≤ 20 秒，正常約 3～6 秒） | 待實機驗證 |
| MQTT retained 狀態（MQTT Explorer 訂閱 `hoban/+/status`） | 待實機驗證 |
| 退避（斷網後睡眠時間依序 600→1200→2400→3600 秒） | 待實機驗證 |
| OTA 正向（含 md5，下次醒來完成更新並清除 retained 指令） | 待實機驗證 |
| OTA 負向（缺 md5 被拒、電量不足被拒） | 待實機驗證 |

## 版本記錄

- `1.0.0` — 首版
