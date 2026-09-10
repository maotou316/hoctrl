---
paths:
  - "**/*.ino"
  - ".vscode/*.json"
  - "flash.ps1"
  - "publish.py"
---

# VSCode 燒錄環境的已知地雷

## 燒錄一律用 task，不要用擴充的 Upload 按鈕

`Ctrl+Shift+B` → 選型號，或 `Tasks: Run Task` 選燒錄項目。
背後是 `flash.ps1` 呼叫 `A:\server\arduino-cli\arduino-cli.exe`（1.3.1），與 `publish.py` 同一套 FQBN。

## tasks.json 必須用 `type: process`，不可用 `shell`

預設終端機是 Git Bash，`shell` 型別會讓 bash 把路徑裡的反斜線當跳脫字元吃掉：
`A:\project\hoctrl_arduino` → `A:projecthoctrl_arduino`，然後報 exit code 127。

## 板子清單消失 → 檢查 `hardware/esp32/` 有沒有殘骸版本目錄

vscode-arduino-community 0.7.2 挑版本用的是 `allVersion[0]`（目錄列表第一個，**不是最新版**），
接著檢查該目錄下的 `boards.txt`，不存在就整個 esp32 平台跳過，一顆板子都不載入。

core 升級後若舊版目錄殘留（例如只剩 `tools/` 沒有 `boards.txt`），就會挑到殘骸而清單全空。
症狀：`Select Board Type` 只剩 Arduino 官方板，搜 ESP32C3 是 `No results found`；
但 `arduino-cli` 一切正常（它讀 `installed.json` 不掃目錄）。

檢查與修法：
```bash
ls C:/Users/maoto/AppData/Local/Arduino15/packages/esp32/hardware/esp32/
# 每個版本目錄都該有 boards.txt，沒有的就是殘骸，刪掉
```

2026-08-14 曾因 `3.1.1/`（只剩一個 `boot_app0.bin`）踩過一次。

## 擴充自帶的 arduino-cli 是 0.31.0，與 1.3.1 格式不相容

`arduino.path` / `arduino.commandPath` 在使用者全域 settings.json 中**刻意留空**，讓擴充用自帶的 0.31.0。
指到 1.3.1 會讓擴充解析失敗（1.x 的 `core list --format json` 從頂層陣列改成包在物件裡）。

另注意 `arduino.*` 是 window scope 設定，multi-root 工作區下放在資料夾層 `.vscode/settings.json` 不生效。

## Arduino IDE 報 `{build.mcu}` is not one of 'auto', 'esp8266'…

板子被選成 **ESP32 Family Device**（`boards.txt` 裡 `hide=true`、沒有定義 `build.mcu`），
它只是給 USB 自動辨識用的（VID 0x303a / PID 0x1001），不能拿來編譯。
插上板子時埠選單旁會自動建議它，不要點。手動選 esp32 → ESP32C3 Dev Module。

## PlatformIO 若要導入，官方平台不能用

官方 `platformio/espressif32` 到 7.0.1（2026-05）仍綁 `framework-arduinoespressif32 ~3.20017.0`，
即 Arduino core **2.0.17**，編不了本專案的 3.3.7。
要 3.x 必須用社群 fork pioarduino：
`platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip`
（55.03.311 對應 core 3.3.11）。2026-08-14 評估後決定暫不導入。
