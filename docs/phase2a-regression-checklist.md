# Phase 2a 回歸清單（`ho_master1`）

> **警告：本清單截至撰寫當下，尚未在任何實體硬體上執行過任何一項。**
> 以下每項的「預期序列埠輸出」是依程式碼實際的 `Serial.printf`／`Serial.println`
> 字串與流程邏輯推導而來，不是實測記錄。上機測試前請先完整看過一遍，
> 燒錄用的指令見 `ho_master1/readme.md`「編譯與燒錄」章節。

## 前置需求

- 至少 1 台已燒錄 `ho_master1`（`master` 或 `master-c3`，任一即可）
- 至少 1 台已燒錄 `ho_slave1`
- 一支手機／電腦可用 BLE 連上 master 做配網（或用支援 BLE 的 App／nRF Connect 之類工具手動送 JSON）
- 一台可連上外部 MQTT broker 的裝置，能訂閱／發布 `hoban/<deviceId>/status`、
  `hoban/<deviceId>/control`（例如 MQTT Explorer 或 `mosquitto_pub`/`mosquitto_sub`）
- 兩台裝置的序列埠（115200 baud）同時開著監看，才能對照 master／slave 兩邊的輸出

---

## 1. 未配網時開機，BLE 廣播出現，名稱為 `hoban-xxxxxxxxxxxx`

**步驟**：master 未曾存過 WiFi 設定（新燒錄或已用 `reset` 指令清過）時直接開機。

**預期序列埠輸出（master）**：
```
齁控 Master v1.0.0
================
按鈕自檢: 正常
[設定] SSID=(未設定) 自訂伺服器=否 繼電器=無
[名冊] 載入 0 台 slave
ESP-NOW 就緒，channel=1
[BLE] 已啟動，名稱: hoban-a0b1c2d3e4f5
[BLE] 等待 App 配網
設備 ID: hoban-a0b1c2d3e4f5
```
用手機藍牙掃描工具確認廣播中的裝置名稱與序列埠印出的 `hoban-xxxxxxxxxxxx` 一致。
LED 應呈現慢閃（1000ms 半週期），對照 `readme.md`「LED 狀態指示」優先序第 1 項。

---

## 2. 配網期間已配對的 slave 不會失聯（master 與 `ho_relay2` 最大的行為差異）

這是本 Phase 最重要的驗證項目：`ho_relay2` 在 AP／BLE 配網模式下 `loop()` 直接
`return`，`ho_master1` **不能**這樣做，否則配網期間 ESP-NOW 心跳會停止，
已配對的 slave 會在 30 秒後判定失聯並強制關閉繼電器。

**步驟**：
1. master 已連上 WiFi、已配對至少 1 台 slave（`list` 確認在線）
2. 對 master 送 MQTT `reset` 指令（見下方第 7 項），或直接在序列埠確認清除後的行為 ——
   `reset` 只清 `hoban` 命名空間（網路設定），**不會**清 `homaster` 命名空間（slave 名冊）
3. master 重啟，因為沒有 WiFi 設定，重新進入 BLE 配網模式
4. 在 BLE 配網模式停留至少 40 秒（超過 slave 端 30 秒失聯門檻），期間持續看 slave 序列埠

**預期序列埠輸出（master，重啟後）**：
```
[設定] SSID=(未設定) 自訂伺服器=否 繼電器=無
[名冊] 載入 1 台 slave
  1. hoban-xxxxxxxxxxxx
ESP-NOW 就緒，channel=1
[名冊] 已重新註冊 1／1 台為 ESP-NOW peer
[BLE] 已啟動，名稱: hoban-a0b1c2d3e4f5
[BLE] 等待 App 配網
```
之後每約 10 秒應仍看到一行心跳 log（`[心跳] channel=1 配對模式=否 slave=1`），
代表 BLE 配網模式下 `maintainEspNow()` 仍在跑。

**預期序列埠輸出（slave，全程）**：**不應**出現 `[失聯] 超過 30 秒沒收到心跳`，
`list`（在 slave 若有對應指令）或觀察 slave 的繼電器/LED 應維持配對前的在線狀態，
不應被強制關閉。

**失敗判定**：只要 slave 端印出失聯或 30~40 秒內繼電器被強制關閉，即代表
BLE 模式下的 `loop()` 意外跳過了 `maintainEspNow()`，是本 Phase 的核心回歸破口。

---

## 3. App 送設定後設備重啟並連上 WiFi

**步驟**：在第 1 項的 BLE 廣播狀態下，用 App（或手動送 JSON，格式見
`readme.md`「BLE 配網」章節）送出：
```json
{
  "wifi": {
    "ssid": "你的WiFi名稱",
    "password": "你的WiFi密碼",
    "server": "mqttgo.io",
    "mqtt_port": 1883
  }
}
```

**預期序列埠輸出（master）**：
```
[BLE] App 已連線
[BLE] 收到設定：{"wifi":{"ssid":"...","password":"...","server":"mqttgo.io","mqtt_port":1883}}
[設定] 已儲存到 NVS
[BLE] 設定已儲存，2 秒後重新啟動
```
（App 端應收到 `{"status":"success",...}` 的 notify 回覆，欄位見 readme）

2 秒後重啟，接著：
```
[設定] SSID=你的WiFi名稱 自訂伺服器=是 繼電器=無
ESP-NOW 就緒，channel=1
[WiFi] 連線到 你的WiFi名稱 …
[WiFi] 取得 IP: 192.168.x.x
[WiFi] 已連線 IP=192.168.x.x RSSI=-XX
```

---

## 4. 連上 WiFi 後 channel 改變，slave 在 46 秒內重新鎖定

Phase 1 channel 同步機制第一次面對真實情境（AP 決定 channel，非序列埠手動指定）。

**步驟**：
1. master 與至少 1 台 slave 已配對且都已鎖定（slave 序列埠曾印出 `[鎖定] master=... channel=X`）
2. 把 WiFi 路由器的頻道手動改成與目前不同的頻道（路由器管理介面設定），
   或直接換一個 channel 不同的 AP 讓 master 連
3. 讓 master 重新連線（拔插電源，或等待既有斷線重連邏輯觸發）
4. 觀察 master 連線成功後、以及 slave 端重新鎖定所花的時間

**預期序列埠輸出（master）**：
```
[WiFi] 已連線 IP=192.168.x.x RSSI=-XX
[channel] 由 1 變為 Y，連發心跳通知 slave
[心跳] channel 已變更，連發 4 次（間隔 200 ms）
```

**預期序列埠輸出（slave，46 秒內）**：
```
[鎖定] master=hoban-a0b1c2d3e4f5 channel=Y
```

**補充（快速模擬，不需真的換路由器頻道）**：韌體本身保留了 `ch <n>`
序列埠測試指令（`ho_master1.ino` 的 `handleSerialCommand()`），可在序列埠對 master
輸入 `ch 6` 之類指令手動觸發 channel 切換與心跳連發，用來快速驗證 slave 重掃邏輯，
不必依賴實際更換路由器頻道；但這屬於模擬，仍應至少做一次真實換頻道的完整驗證。

---

## 5. MQTT 連上，`hoban/<masterId>/status` 每 10 秒收到一次

**步驟**：master 已連上 WiFi 與 MQTT 後，用 MQTT Explorer 或
`mosquitto_sub -h mqttgo.io -t "hoban/+/status"` 訂閱並計時。

**預期序列埠輸出（master）**：
```
[MQTT] 嘗試 mqttgo.io …
[MQTT] 已連線 mqttgo.io，訂閱 hoban/hoban-a0b1c2d3e4f5/control
```
之後每 10 秒應收到一則 retain 的 status 訊息（`lastStatusPub` 間隔），
JSON 內容見第 6 項。

---

## 6. 狀態 JSON 欄位完整（含 `has_relay`、`slave_count`、`channel`）

**步驟**：訂閱 `hoban/<masterId>/status`，取得任一則訊息，比對欄位。

**預期 JSON**（欄位需全部存在，數值依實際狀態而定，完整範例見 `readme.md`）：
```json
{
  "device_id": "hoban-a0b1c2d3e4f5",
  "status": "online",
  "version": "1.0.0",
  "model": "hoMaster1",
  "timestamp": 123456,
  "wifi": { "connected": true, "ssid": "...", "rssi": -55, "ip": "192.168.1.100" },
  "device": {
    "relay": 0,
    "has_relay": false,
    "pairing": false,
    "slave_count": 1,
    "channel": 6,
    "long_range": false
  }
}
```
重點檢查 `device.has_relay`、`device.slave_count`、`device.channel` 三個欄位是否存在
且數值正確（`slave_count` 應與序列埠 `list` 指令顯示的台數一致）。

---

## 7. 送 `status` / `ON` / `OFF` / `HASRELAY:ON` 各指令的反應

**步驟**：對 `hoban/<masterId>/control` 依序發布以下訊息，各自觀察序列埠與後續
status 訊息的變化。

| 送出 | 預期序列埠輸出（master） | 預期效果 |
|---|---|---|
| `status` | `[MQTT] 收到指令: status` | 立即收到一則新的 status 訊息 |
| `ON` | `[MQTT] 收到指令: ON` | master 自己的繼電器點動 2 秒（`pulseRelay(2000)`），status 的 `device.relay` 短暫變 1 |
| `OFF` | `[MQTT] 收到指令: OFF` | master 自己的繼電器立即關閉，`device.relay` 為 0 |
| `HASRELAY:ON` | `[MQTT] 收到指令: HASRELAY:ON` 接著 `[設定] 繼電器宣告為 有接` | `device.has_relay` 變 `true`，且此設定會存入 NVS，重啟後仍保留 |

---

## 8. WiFi 拔線 60 秒，確認 slave 全程不失聯（驗證 `maintainEspNow()`）

**步驟**：
1. master 已連上 WiFi、已配對至少 1 台 slave
2. 直接拔掉 WiFi 路由器電源，或讓路由器離線，持續至少 60 秒
3. 全程監看 master 與 slave 兩邊序列埠
4. 60 秒後恢復路由器供電

**預期序列埠輸出（master）**：
```
[WiFi] 斷線原因碼: XX
[WiFi] 重連嘗試 #1
[WiFi] 連線失敗，狀態=X 原因碼=XX
```
（`wifiFailCount` 遞增期間會反覆嘗試，重試邏輯與間隔見 `loop()` 的 WiFi 管理區塊）
心跳 log 應**持續**每約 10 秒一行，不因 WiFi 斷線而停止或延遲超過預期。

**預期序列埠輸出（slave，全程 60 秒）**：**不應**出現 `[失聯] 超過 30 秒沒收到心跳`，
在線狀態應全程維持，繼電器（如原本是 ON）不應被強制關閉。

**失敗判定**：只要 slave 在這 60 秒內判定失聯，代表 WiFi 斷線後的重連流程中
有某段等待沒有正確呼叫到 `maintainEspNow()`。

---

## 9. MQTT broker 切換（`FIND_BEST_SERVER`）期間 slave 不失聯

**步驟**：
1. master 已連上某個 MQTT broker（例如 `mqttgo.io`）、已配對至少 1 台 slave
2. 對 `hoban/<masterId>/control` 送 `FIND_BEST_SERVER`
3. 觀察 master 序列埠切換過程，同時監看 slave 是否維持在線

**預期序列埠輸出（master）**：
```
[MQTT] 收到指令: FIND_BEST_SERVER
[MQTT] 嘗試 mqttgo.io …
[MQTT] 已連線 mqttgo.io，訂閱 hoban/hoban-a0b1c2d3e4f5/control
```
（`smartConnect()` 會依序嘗試，若原本連線的伺服器仍是最快回應者，可能又連回同一台，
這是正常行為，不是失敗）

**預期序列埠輸出（slave，全程）**：不應出現失聯訊息，因為切換過程中的
`mqttClient.disconnect()` 與 `espNowDelay(500)` 都會維持心跳。

---

## 10. 清除設定回到 BLE 配網模式

> **注意（與 brief 的落差）**：brief 原文寫「長按重置清除 NVS」，但目前
> `ho_master1.ino` **尚未實作按鈕長按重置**（程式碼註釋明確寫著「保留給 Phase 2
> 的長按重置流程」，`anyResetButtonPressed()` 目前只用於讓 WiFi 連線等待迴圈
> 期間仍可跳出，尚未接上真正的清除動作）。Phase 2a 目前唯一能清除網路設定並
> 回到 BLE 配網模式的方式是 **MQTT `reset` 指令**，以下步驟改測這條路徑；
> 按鈕長按重置留待之後的 Phase 補上按鈕流程時再驗證。

**步驟**：
1. master 已連上 WiFi 與 MQTT
2. 對 `hoban/<masterId>/control` 送 `reset`
3. 觀察序列埠

**預期序列埠輸出（master）**：
```
[MQTT] 收到指令: reset
[設定] NVS 網路設定已清除
```
（`espNowDelay(1000)` 後 `ESP.restart()`，心跳在這 1 秒內仍照常發送）

重啟後應回到第 1 項的 BLE 配網模式輸出（`[BLE] 已啟動，名稱: ...`），
且 `[名冊] 載入 N 台 slave` 的 N 應維持清除前的台數（`reset` 不影響 `homaster` 名冊，
見第 2 項）。

---

## 11. MQTT 密碼設超過 12 字元能正常認證（驗證 NVS 修掉的 EEPROM 位址重疊缺陷）

`ho_relay2` 用 EEPROM 且 `mqttPassword` 與 `mqttPort` 位址重疊，導致密碼實際上限
只有 12 字元，超過會被截斷。`ho_master1` 改用 NVS、各欄位獨立配置容量
（`mqttPassword[65]`），此項驗證這個修正確實生效。

**步驟**：
1. 準備一個需要帳密驗證、密碼長度**超過 12 字元**（例如 20 字元）的 MQTT broker
   帳號（可自架 mosquitto 設一組測試帳密，或使用 `broker.hoban.tw` 若已知其
   帳密長度符合條件）
2. 透過 BLE 配網送出包含此帳密的設定：
   ```json
   {
     "wifi": {
       "ssid": "...", "password": "...",
       "server": "自訂broker位址",
       "mqtt_username": "testuser",
       "mqtt_password": "超過12字元的測試密碼ABCDEFG",
       "mqtt_port": 1883
     }
   }
   ```
3. master 重啟後連線，觀察是否認證成功

**預期序列埠輸出（master）**：
```
[MQTT] 嘗試自訂伺服器 自訂broker位址 …
[MQTT] 已連線自訂伺服器 自訂broker位址，訂閱 hoban/hoban-a0b1c2d3e4f5/control
```

**失敗判定**：若看到 `[MQTT] 自訂伺服器 X 失敗，state=X` 反覆出現，且已確認
broker 端帳密設定無誤，代表密碼在某處被截斷，NVS 欄位長度或寫入邏輯有回歸。

---

## 疑慮與待確認事項

- 全部 11 項截至目前皆**未在實體硬體驗證**，上述輸出全為程式碼推導，實測時
  請對照實際輸出並記錄差異
- 第 10 項的按鈕長按重置目前在 `ho_master1` 尚未實作，只能測 MQTT `reset` 路徑，
  已在該項標註
- 第 4 項「46 秒內重新鎖定」的門檻依賴 slave 端掃描週期（`SCAN_DWELL_MS=1200ms`
  × 頻道數）與心跳連發，實測時建議額外記錄實際花費秒數，不只是「有無在時限內鎖定」
- 第 11 項需要一組密碼超過 12 字元的可用 MQTT broker 帳密，若手邊沒有，
  建議自架一個 mosquitto 測試帳號再測，不要用會影響其他人的正式帳密
