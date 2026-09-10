---
paths:
  - "**/*.ino"
---

# BLE 回調裡不准重啟、不准長時間阻塞

## 症狀

App 新增設備，填完 WiFi 密碼與設備名稱按「下一步」，跳「與設備的藍牙連線已中斷」。
但設備那頭其實已經配網成功：序列埠印了「[BLE] 收到設定」「設定已儲存」，
NVS 也寫進去了，重開機後正常連上 WiFi 與 MQTT。只有 App 認為失敗、不建設備文件。

## 原因

esp32 core 3.x 的 `BLECharacteristic::handleGATTServerEvent()`（`ESP_GATTS_WRITE_EVT`）
順序是：

```cpp
m_pCallbacks->onWrite(this, param);            // ← 先跑我們的回調
if (param->write.need_rsp) {
  esp_ble_gatts_send_response(...);            // ← 回來之後才送 ATT 寫入回應
}
```

回調裡直接 `ESP.restart()`，那個回應就永遠送不出去。App 端的
`write(withoutResponse: false)` 在等 ACK，等到的是設備重啟造成的斷線例外。

**舊版 core 是先送回應才呼叫 onWrite，所以這個寫法以前不會出事** ——
升級 core 版本後才會突然壞掉，而且四支 sketch 是互相抄的，一壞就一起壞。

## 規則

`onWrite()` / `onRead()` 這類 BLE 回調裡：

- **不准** `ESP.restart()`
- **不准** `delay()` / `espNowDelay()` 等超過幾十毫秒的阻塞（回調跑在 BLE stack 的
  task 上，卡住它等於卡住整個 GATT 交握）

要重啟就排程，讓回調先返回：

```cpp
volatile unsigned long bleRestartAt = 0;   // 0 = 沒有待處理的重啟

// onWrite() 結尾
bleRestartAt = millis() + 2000;
if (bleRestartAt == 0) bleRestartAt = 1;   // 0 是哨兵值
return;                                     // 直接 return，別讓後面的清理再跑一次

// loop() 最前面
if (bleRestartAt != 0 && (long)(millis() - bleRestartAt) >= 0) {
  ESP.restart();
}
```

master 原本靠 `espNowDelay(2000)` 在等待期間維持 slave 心跳；改成排程後這 2 秒
由 `loop()` 消化，`maintainEspNow()` 照樣每輪都跑，效果相同。

## 順帶

原本 relay 系列的 `onWrite()` 成功分支 `free(buffer)` 之後，函式結尾還有第二個
`free(buffer)` —— 只因為 `ESP.restart()` 沒讓它執行到才沒炸。改成排程重啟時
**必須加 `return;`**，否則就變成真的 double free。

## 擋不住什麼

- 擋不住 ATT 回應在空中掉包。App 端另有一層補救：`configSent` 在 `await write()`
  **之前**設，寫入拋斷線例外時仍視為已送達（見 hoctrl 的
  `test/utils/ble_config_delivery_guard_test.dart`）。
- 擋不住現場還沒升級韌體的設備。那些設備靠上面那層 App 補救才配得起來。
