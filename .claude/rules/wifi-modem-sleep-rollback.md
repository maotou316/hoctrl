---
paths:
  - "ho_relay2/**"
---

# ho_relay2 WiFi Modem-sleep：何時該還原、怎麼還原

1.9.0（2026-09-23）起 `ho_relay2.ino` 預設 `#define WIFI_MODEM_SLEEP 1`（`WIFI_PS_MIN_MODEM`），
為了延長 2S 電池待機。1.8.5 以前一律 `WiFi.setSleep(false)`（`WIFI_PS_NONE`），理由是避免斷線。

## 基準數字

- 1.8.5（射頻常開）：電池端待機實測 **0.09 A**（2026-09-23，繼電器未吸合，三用電錶 A 檔）
- 1.9.0 預期 25～40 mA；刷完若仍 >60 mA，代表睡眠沒生效或被別的耗電吃掉，先查這個再談續航

## 出現以下任一情況就考慮還原

- 現場回報斷線／離線次數比 1.8.5 明顯變多（序列埠看斷線原因碼，常見是 AP 端踢睡眠設備）
- App 下指令後繼電器反應明顯變慢（>0.5 秒）
- 電量讀數異常抖動或 brownout 重開（射頻開關造成電源尖峰，穩壓電容不足）

## 還原步驟

1. `ho_relay2.ino` 把 `#define WIFI_MODEM_SLEEP 1` 改成 `0`——只改這一行，
   `applyWiFiPowerSettings()` 會在開機、`connectToWiFi()` 收尾、OTA 失敗三處套用
2. 版本號遞增（例如 1.9.1）並在 readme 版本記錄寫明「因 X 還原射頻常開」
3. 完整還原到改動前的程式碼：改動前的 main 是 commit `168d1bf`（1.8.5）

**不要**只在其中一處手寫 `WiFi.setSleep(...)`：睡眠模式是驅動層設定，
`esp_wifi_deinit()` 後會被清掉，漏掉任何一處就會在完整探測後靜默換回另一種模式。

## 編譯

`PUBLISH_README.md` 的 `--fqbn esp32:esp32:esp32c3` 缺 `CDCOnBoot=cdc`，照抄會在
`Serial.setTxTimeoutMs` 報錯。用 `ho_relay2/build/vscode/build.options.json` 裡的完整 FQBN。
