---
paths:
  - "ho_relay*/**"
  - "ho_slave*/**"
  - "ho_master*/**"
  - "flash.ps1"
---

# USB 接著電腦時的兩種假故障（ESP32-C3，`CDCOnBoot=cdc`）

現場設備從不接電腦，這兩個問題**只在桌面測試出現**，但外觀跟真的硬體／韌體故障一模一樣。
2026-09-09 晚上兩個疊在一起，把一張正常的 P25 板誤判成「MOS 貫通、要換」，繞了兩小時。

## 假故障一：關掉序列埠／燒完韌體後繼電器常開

**機制**：C3 內建 USB-Serial/JTAG 會把 DTR/RTS 當 esptool 的自動下載電路用。開埠、關埠、
esptool 收尾時兩條線切換的先後順序，決定晶片是正常重啟還是**卡在 ROM 下載模式**。卡住時
韌體沒跑、GPIO 4/7 沒人驅動；P25 版的 gate 放手就停在高電位 → 繼電器吸住，**而且是穩定常開**，
不是開機那一下。

**辨認**：開序列埠只看到 `ESP-ROM:esp32c3-api1-20210207` 一行、之後什麼都沒有；MQTT 查不到狀態。
（只看到 ROM banner 也可能是 USB CDC 重新列舉讓 handle 失效，兩者的分辨法：用 MQTT 查有沒有
`online`，或看開機秒數 `timestamp` 有沒有在走。）

**脫困**：`esptool --chip esp32c3 -p COMx --after hard-reset chip-id`，或按板上 RESET。
之後**不要再開序列埠**去確認，用 MQTT 訂閱 `hoban/<id>/status` 看。

**開埠不擾動晶片的參數**：.NET `SerialPort` 的 `DtrEnable=$false; RtsEnable=$false`。
`RtsEnable=$true` 會 assert RTS 把 EN 拉低，晶片全程卡在 reset、什麼都讀不到。

## 假故障二：收到 MQTT 指令就斷線、一分鐘後自己回來

**機制**：core 3.3.x `HWCDC::write()`（`cores/esp32/HWCDC.cpp`）。電腦曾開過埠之後
`connected` 旗標為 true；關掉監視但 USB 線還插著，`isPlugged()` 仍 true，旗標永遠翻不回去。
這種「host backpressure」下每一次 `write()` 等 `20 × tx_timeout_ms(100 ms)` = **2 秒**才放棄。
`mqttCallback → publishStatus` 印十幾行 → 一個指令卡 35～60 秒 → keepalive 30 秒過期、
broker 45 秒後 LWT 踢人。開機秒數連續（只是卡住，沒重啟）。**開著監視就完全正常**——因為
有人在讀。WiFi/MQTT 開機後要連 114 秒也是同一個原因（開機 log 很多行）。

**辨認**：MQTT 訂閱 status，設備每 3～5 秒發一次；一送任何指令（連 `status` 都算）就安靜，
45 秒後收到 `"status":"offline"` 的 LWT，一分鐘後 `server_changed` 重連。

**修法**：`Serial.begin()` 之後 `Serial.setTxTimeoutMs(0)`（ho_relay2 1.8.1 起已加）。
其他 C3 sketch（ho_relay3、ho_slave1、ho_master1 的 C3 版）2026-09-10 時**尚未加**；ho_master1
同一份 sketch 也編成 WROOM，`Serial` 是 HardwareSerial 沒這個方法，要包 `#if ARDUINO_USB_CDC_ON_BOOT`。

## 桌面測試的正確姿勢

1. 燒完韌體 → 按一下 RESET 或 esptool 硬重置 → **用 MQTT 確認 `online`**，不要靠序列埠
2. 要看序列 log 就整段測試都開著，中途不要關；關了要重做第 1 步
3. 修正前的舊韌體，MQTT 指令測試一律開著監視做，否則指令根本沒被處理
4. 手刻的極簡 MQTT 客戶端（`mqtt_pub.py`／`mqtt_sub.py`／`mqtt_seq_when_online.py`，
   只用 Python 標準函式庫）留在 scratchpad，不在就重寫，幾十行
