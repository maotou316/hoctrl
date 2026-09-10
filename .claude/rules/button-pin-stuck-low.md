---
paths:
  - "**/*.ino"
---

# 按鈕腳位卡在 LOW → 開機無限清除設定迴圈

## 症狀

序列埠不斷重複這一段，設備永遠停不下來、也永遠無法上線：

```
找不到 WiFi 設定，啟動 BLE 配對模式...
BLE 已啟動，名稱: hoban-xxxxxxxxxxxx
請使用 App 透過 BLE 配對設定 WiFi
偵測到按鈕按下，開始計時...
長按 3 秒達成，開始 LED 閃爍確認...
確認重置，LED 長亮 0.7 秒後清除 WiFi 設定...
WiFi 設定已清除。重新啟動中...
```

**關鍵特徵：全程沒有印出「按鈕放開，取消重置」**，代表 `digitalRead()` 從頭到尾沒回到 HIGH，
不是使用者手殘按住，是腳位真的卡在低電位。

## 成因

按鈕接法是 `GPIO ──[按鈕]── GND`，靠 `INPUT_PULLUP` 的內部提升電阻（約 45kΩ）拉成 HIGH，
按下才接地變 LOW。一旦按鈕內部短路／焊點短路／走線接地，該腳就恆為 LOW。

而 `lastBootButtonState` / `lastResetButtonState` 初值是 `HIGH`，
**開機時就已是 LOW 的腳會被判定成「使用者剛按下」**，5 秒後清空 EEPROM 並重啟，如此無限循環。

2026-08-14 在 hoRelay2 實際發生過：RESET 按鈕（GPIO 1）內部短路，換掉按鈕就正常。

## 判定方法

不要靠猜，開機時取樣兩支腳。韌體已內建 `checkStuckButtons()`（`ho_relay2.ino`），
自檢異常時會印：

```
⚠ 按鈕自檢: RESET(GPIO 1) 恆為 LOW，本次開機停用其重置功能
```

**排除 BOOT 腳的技巧**：開機訊息若是 `boot:0xc SPI_FAST_FLASH_BOOT`，
代表 reset 當下 GPIO 9 是 HIGH（否則會進 UART 下載模式），可直接把 BOOT 按鈕排除，
剩下的就是另一支。

**排除序列埠工具的技巧**：ESP32 自動下載電路的 DTR 會驅動 GPIO 9(BOOT)、RTS 驅動 EN。
用 PowerShell 的 `System.IO.Ports.SerialPort` 讀取時明確設 `$sp.DtrEnable = $false`，
就能確認不是監控工具把腳位拉低。硬體重啟改用 RTS 短暫拉高再放開。

**硬體驗證**：三用電表量該腳對 GND 的電阻。接近 0Ω 就是短路／走線接地。

## 韌體防呆（已實作，勿移除）

`ho_relay2.ino` 的 `checkStuckButtons()` 在 `setup()` 的 `pinMode(..., INPUT_PULLUP)` 之後、
任何重置流程之前跑 500ms 取樣，整段都是 LOW 的腳會被設為不可用，
四處讀按鈕的地方（`loop()`、`waitForResetConfirm()`、`interruptibleDelay()`、WiFi 連線等待）
都改走 `anyResetButtonPressed()` 統一判斷。

副作用：「按住按鈕再上電」會被擋掉。正常重置流程是設備運作中才長按，放開後重新上電即恢復。

同時修掉的同源缺陷：`loop()` 裡 `millis() - buttonPressTime` 在 `buttonPressTime == 0` 時
等於開機時間，只要漏抓一次 HIGH→LOW 邊緣就會**立刻**觸發重置，不需要按滿 5 秒。
新增按鈕時務必先補上計時起點再算 `pressDuration`。

hoRelay1 / hoRelay3 尚未套用這套防呆，若出現同樣症狀請一併移植。
