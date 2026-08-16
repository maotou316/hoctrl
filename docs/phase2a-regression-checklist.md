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
[設定] SSID=(未設定) 自訂伺服器=否 繼電器=無 上次AP channel=0
[名冊] 載入 0 台 slave（上次心跳 channel=0）
ESP-NOW 就緒，channel=1
[BLE] 已啟動，名稱: hoban-a0b1c2d3e4f5
[BLE] 等待 App 配網
設備 ID: hoban-a0b1c2d3e4f5
```
用手機藍牙掃描工具確認廣播中的裝置名稱與序列埠印出的 `hoban-xxxxxxxxxxxx` 一致。
LED 應呈現慢閃（1000ms 半週期），對照 `readme.md`「LED 狀態指示」優先序第 1 項。

---

## 2. 配網期間已配對的 slave 不會失聯（master 與 `ho_relay2` 最大的行為差異）

> **本項的預期結果已於最終審查後修訂。** 舊版只寫「slave 不會失聯」，但那個寫法只
> 涵蓋了「`loop()` 有沒有跳過 `maintainEspNow()`」這一個面向。實際上還有第二個、
> 而且更容易踩到的破口：**心跳有發出去 ≠ slave 收得到**。ESP-NOW 的 peer 用
> `channel = 0`（跟隨當前實體 channel），master 的 STA channel 若與 slave 鎖定的
> channel 不同，心跳就打在錯的頻道上。照舊版清單去測，會把這個破口測成 PASS。

要驗證的其實是兩件事，缺一不可：

- **(A) 心跳有沒有繼續發**：`ho_relay2` 在配網模式下 `loop()` 直接 `return`，
  `ho_master1` 只跳過 WiFi／MQTT 管理區塊，`maintainEspNow()` 照跑。
- **(B) 心跳有沒有打在對的 channel 上**：沒有 WiFi 設定時 `setupEspNow()` 的
  `WiFi.mode(WIFI_STA)` 會把 channel 歸 **1**，而 `onWifiChannelMayHaveChanged()`
  在 BLE 模式下完全不會被呼叫。修正後由
  `restoreEspNowChannelForOfflineBoot()` 在開機時從 NVS 的 `homaster/espch`
  讀回「slave 鎖定的 channel」並 `esp_wifi_set_channel()` 切過去。

**前置條件（重要，不滿足就測不出 B）**：
master 必須在**本次測試之前**至少成功連上過一次 WiFi 且當時名冊已有 slave，
`homaster/espch` 才會被寫入。剛燒錄的空白設備直接測，NVS 沒有這個鍵，
master 會停在 channel 1（此時序列埠會印出 `⚠ [channel] …NVS 沒有 channel 記錄…`），
那是**已知且已標示的行為**，不算本項失敗，但也不算通過 —— 請先完成第 3 項配網、
確認 master 連上一個 **channel 不是 1** 的 AP 之後，再回頭做本項。

**步驟**：
1. master 已連上 WiFi（**AP 的 channel 必須不是 1**，例如 6 或 11，否則測不出差異）、
   已配對至少 1 台 slave（`list` 確認在線，slave 端曾印出 `[鎖定] … channel=6`）
2. 對 master 送 MQTT `reset` 指令（見下方第 7 項）。
   `reset` 只清 `hoban` 命名空間（網路設定，含 `apch`），**不會**清 `homaster`
   命名空間（slave 名冊與 `espch`）—— `espch` 刻意分開存放就是為了撐過 `reset`
3. master 重啟，因為沒有 WiFi 設定，重新進入 BLE 配網模式
4. 在 BLE 配網模式停留至少 **60 秒**（遠超 slave 端 30 秒失聯門檻），
   期間持續看 slave 序列埠

**預期序列埠輸出（master，重啟後）**：
```
[設定] SSID=(未設定) 自訂伺服器=否 繼電器=無 上次AP channel=0
[名冊] 載入 1 台 slave（上次心跳 channel=6）
  1. hoban-xxxxxxxxxxxx
ESP-NOW 就緒，channel=1
[名冊] 已重新註冊 1／1 台為 ESP-NOW peer
[channel] 本次開機不關聯 WiFi，切回 NVS 記住的 channel=6，維持 1 台已配對 slave 的心跳
[心跳] channel 已變更，連發 4 次（間隔 200 ms）
[BLE] 已啟動，名稱: hoban-a0b1c2d3e4f5
[BLE] 等待 App 配網
```
注意 `上次AP channel=0`（`apch` 被 `reset` 清掉了，這是正確的）與
`上次心跳 channel=6`（`espch` 沒被清掉，這也是正確的）**必須同時成立**。

之後每約 10 秒應仍看到一行心跳 log，且 **channel 欄位必須是 6 而不是 1**：
```
[心跳] channel=6 配對模式=否 slave=1
```

**要觀察什麼**：
1. master 是否印出 `[channel] 本次開機不關聯 WiFi，切回 NVS 記住的 channel=6`
2. 之後的 `[心跳]` log 的 `channel=` 是否維持 6（**不是 1**）
3. slave 端全程是否沒有出現 `[失聯]`、也沒有出現重新輪掃／重新鎖定的訊息
4. slave 若原本繼電器是 ON，是否全程保持 ON

**什麼情況算失敗**：
- master 印出 `[心跳] channel=1 …` 而 NVS 明明有 channel=6 → `restoreEspNowChannelForOfflineBoot()`
  沒生效或被呼叫在 `setupEspNow()` 之前（`esp_wifi_set_channel()` 需 WiFi 已初始化）
- 序列埠完全沒有心跳 log → BLE 模式下 `loop()` 意外跳過了 `maintainEspNow()`（舊破口）
- slave 印出 `[失聯] 超過 30 秒沒收到心跳`，或繼電器被強制關閉，或重新輪掃鎖定到
  channel 1 → 上述任一條路徑有回歸
- master 重啟後 `[名冊] 載入 … 上次心跳 channel=0`，但測試前確實有成功連過 WiFi
  → `saveSlaveLockChannel()` 的三個寫入點（`connectToWiFi()` 成功、`addSlave()`、
  `onWifiChannelMayHaveChanged()`）沒被正確觸發

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
[設定] SSID=你的WiFi名稱 自訂伺服器=是 繼電器=無 上次AP channel=0
ESP-NOW 就緒，channel=1
[WiFi] 連線到 你的WiFi名稱 …
[WiFi] 無已知 channel，退回全頻掃描關聯
[WiFi] 取得 IP: 192.168.x.x
[WiFi] 已連線 IP=192.168.x.x RSSI=-XX
[設定] 已記住 AP channel=6（供下次開機在 BLE 配網模式維持心跳用）
```
（`上次AP channel=0` 與「退回全頻掃描」在**第一次**配網後是正常的：`reset` 已把
`hoban` 命名空間清空，包含 `apch`。連上之後才會寫入，下次重連就會改走鎖定 channel。）

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
[MQTT] 已連線 mqttgo.io
[MQTT] 已訂閱 hoban/hoban-a0b1c2d3e4f5/control
```
> **Phase 2b Task 4 更新**：訂閱動作已從連線函式抽到
> `subscribeAllControlTopics()`，所以「已連線」與「已訂閱」變成**兩行**
> （原本是同一行的 `[MQTT] 已連線 X，訂閱 Y`）。
> 若名冊上有 slave，後面還會逐台印 `[代理] 已訂閱 hoban/<slaveId>/control`，
> 每台一行 —— 這是正常輸出，不是失敗。
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

## 8. WiFi 拔線 60 秒，確認 slave 全程不失聯

> **本項的預期結果已於最終審查後修訂。** 舊版只寫「心跳有沒有繼續發」，
> 但 WiFi 重連真正的風險不是心跳停發，而是**心跳被發到錯的 channel**：
> `WiFi.begin()` 不帶 channel 時，ESP-IDF 底層仍會全頻掃描（一輪約 20 秒），
> master 會跑遍 channel 1~13，停在舊 channel 的 slave 每則心跳命中率只剩約 1/13。
> 修正前，`connectToWiFi()` 失敗一次就把 channel 記錄清掉，**第 2 次重試起就
> 全部退回全頻掃描**，30 秒 30 則心跳全數落空的機率約 9%。照舊版清單去測，
> 只看「心跳 log 有沒有繼續印」會把這個 9% 的破口測成 PASS。

修正後的行為有兩層：
- **channel 提示**：失敗時只清 BSSID、保留 `lastApChannel`，重試改用
  `WiFi.begin(ssid, password, lastApChannel, nullptr)`。依 ESP-IDF 文件
  （`wifi_sta_config_t.channel` 註解：「Set to 1~13 to scan **starting from**
  the specified channel before connecting to AP」），這是「以指定 channel
  **起始**掃描」，並非「鎖定在該 channel」。配合 Arduino core 預設的
  `WIFI_FAST_SCAN`（找到 SSID 即停）：AP **剛好在**該 channel 時會一擊命中、
  完全跳過後續掃描；但 AP **不在**該 channel 時（本項路由器斷電情境正好符合），
  依文件字面意思仍可能從該 channel 續掃其餘頻道 —— **這個機制細節只有文件
  推導、未經實機驗證**，是本項要實測確認的重點之一。連續失敗達
  `WIFI_CHANNEL_LOCK_MAX_FAIL`（10）次才升級成一次全頻掃描。
- **加密心跳**：關聯期間 `wifiAssociating` 為 true，心跳間隔由 1000ms 縮到
  `HEARTBEAT_INTERVAL_ASSOC`（200ms），30 秒內約 150 則。加上兩次關聯嘗試之間
  `connectToWiFi()` 失敗分支會把射頻主動 park 回 `slaveLockChannel`
  （`esp_wifi_set_channel()` 為即時呼叫，必中），兩層疊加下，即使續掃真的
  發生，30 秒空窗理論上也不會出現。

**步驟**：
1. master 已連上 WiFi（記下 AP 的 channel，例如 6）、已配對至少 1 台 slave，
   slave 端已印出 `[鎖定] … channel=6`
2. 直接拔掉 WiFi 路由器電源，或讓路由器離線，持續至少 60 秒
3. 全程監看 master 與 slave 兩邊序列埠
4. 60 秒後恢復路由器供電，繼續觀察到 master 重新連上為止

**預期序列埠輸出（master）**：
```
[WiFi] 斷線原因碼: XX
[WiFi] 重連嘗試 #1
[WiFi] 連線到 你的WiFi名稱 …
[WiFi] 使用已知 channel=6 的 BSSID 直接關聯，跳過掃描
[WiFi] 連線失敗，狀態=X 原因碼=XX
[WiFi] 指定 BSSID 關聯失敗，清除 BSSID 記錄，下次改為只鎖定 channel 掃描
[WiFi] 重連嘗試 #2
[WiFi] 連線到 你的WiFi名稱 …
[WiFi] 不指定 BSSID，但把掃描限制在已知 channel=6
[WiFi] 連線失敗，狀態=X 原因碼=XX
```
第 2 次起每一次都必須是 `[WiFi] 不指定 BSSID，但把掃描限制在已知 channel=6`。

**要觀察什麼**：
1. **第 2 次以後的每一次重連，是否都印出「把掃描限制在已知 channel=6」**。
   這是本項的核心，60 秒內大約會看到 3~4 次重連嘗試。
2. 心跳 log 是否持續。注意關聯期間心跳加密到 200ms，log 每 10 次印一行，
   所以關聯中會看到**約每 2 秒一行**、非關聯期間回到約每 10 秒一行 ——
   兩種節奏交替出現是正常的，不是異常。
3. 心跳 log 的 `channel=` 欄位變化模式（全程維持 6，或短暫跳動後回穩）——
   這是機制待確認的觀察項，不是失敗判定，理由見下方「待確認的觀察項」。
   （附註：序列埠訊息本身的措辭「把掃描限制在已知 channel=6」是歷史遺留、
   不夠精確——依 ESP-IDF 文件實際機制是「以該 channel 為起點掃描」，AP 不在
   該 channel 時可能續掃其餘頻道，見 `ho_master1.ino` `connectToWiFi()` 內
   `WiFi.begin(ssid, password, lastApChannel, nullptr)` 上方註釋。這裡沿用
   舊字串只是為了與程式碼實際印出的內容一致，判準仍以字串是否出現為準，
   不代表字面「限制」是準確描述。）
4. slave 端全程是否沒有 `[失聯]`、沒有重新輪掃、繼電器（原本是 ON 的話）沒被關閉。

**什麼情況算失敗**：
- 出現 `[WiFi] 無已知 channel，退回全頻掃描關聯`，而重連次數還沒到 10 次
  → 保留 `lastApChannel` 的修正有回歸
- 心跳 log 出現超過 **30 秒**的空窗 → 某段等待沒有走 `maintainEspNow()`
- slave 印出 `[失聯] 超過 30 秒沒收到心跳`，或繼電器被強制關閉

**待確認的觀察項（不是失敗判定）**：
- 心跳 log 的 `channel=` 欄位在 60 秒內是否跳動（例如 6 → 3 → 11 …）。
  **這不是失敗判定** —— `WiFi.begin(ssid, pass, ch, nullptr)` 依 ESP-IDF 文件
  是「以該 channel 起始掃描」，AP 不在該 channel 時，文件字面意思上仍可能從
  該 channel 續掃其餘頻道，因此看到跳動未必代表修正失效；完全不跳也不能單靠
  這一項就證明機制如預期運作，兩種結果都值得記錄。**請記錄實際觀察到的
  channel 變化模式**（完全不跳／跳動但很快回到 6／持續跳動），作為機制驗證
  的第一手資料，用來校正上方「channel 提示」的敘述，而不要單憑跳動與否
  判斷本項 PASS/FAIL。真正保護 slave 不失聯的是「200ms 加密心跳＋兩次嘗試
  之間的 channel 復位」這兩層機制，只要沒出現 30 秒空窗與 slave 失聯，
  這一項本身跳不跳都不影響本項「修正有效」的結論。

> **注意這一項有機率性**：修正前的失敗機率約 9%，代表**單次測試通過不足以證明
> 修正有效**。請以「第 2 次以後的重連是否印出鎖定 channel 的那一行」作為主要判準
> （這是決定性的、非機率性的證據），slave 沒失聯只是必要條件而非充分條件。

---

## 9. MQTT 伺服器切換與重連期間 slave 不失聯

> **本項的預期結果已於最終審查後修訂。** 舊版寫「`mqttClient.disconnect()` 與
> `espNowDelay(500)` 都會維持心跳，所以不會失聯」—— 那個推論漏掉了真正的阻塞來源：
> **`mqttClient.connect()` 本身是不可中斷的阻塞呼叫，期間 `maintainEspNow()`
> 完全不會被呼叫**。單次呼叫對不可達目標最壞約 18 秒（`NetworkClient::connect()`
> 會先做 `getaddrinfo()` DNS 解析，這段沒有 timeout 參數，由 lwIP 的
> `DNS_MAX_RETRIES` 指數退避決定，約 15 秒；`WIFI_CLIENT_DEF_CONN_TIMEOUT_MS=3000`
> 只管 TCP、`setSocketTimeout(3)` 只管 CONNACK）。
> 修正前 `smartConnect()` 在自訂伺服器失敗後**立刻**接第一台預設伺服器，
> 背靠背兩次 = **36 秒 > 30 秒門檻**，slave 必定失聯關籠。
> 照舊版清單只在 broker 一切正常的情況下測，永遠測不到這條路徑。

修正後 `smartConnect()` **每次呼叫只嘗試一台 broker**，用檔案層級的游標
（`mqttCustomTried` / `mqttProbeOffset`）推進，其餘交給 `loop()` 既有的 10 秒
重連節奏，且 `lastReconnect` 改在 `smartConnect()` **之後**用新的 `millis()` 記錄。

### 9a. 正常情況：broker 可連（快速驗證）

**步驟**：master 已連上某個 MQTT broker、已配對至少 1 台 slave，
對 `hoban/<masterId>/control` 送 `FIND_BEST_SERVER`。

**預期序列埠輸出（master）**：
```
[MQTT] 收到指令: FIND_BEST_SERVER
[MQTT] 嘗試自訂伺服器 mqttgo.io …
[MQTT] 已連線自訂伺服器 mqttgo.io
[MQTT] 已訂閱 hoban/hoban-a0b1c2d3e4f5/control
```
（可能又連回同一台，這是正常行為，不是失敗）
> **Phase 2b Task 4 更新**：同第 5 項的說明，「已連線」與「已訂閱」現在是兩行；
> 名冊上有 slave 時後面會再逐台印 `[代理] 已訂閱 …`。

### 9b. 真正要測的情況：broker 不可達（本項的重點）

**步驟**：
1. master 已連上 WiFi、已配對至少 1 台 slave（`list` 確認在線）
2. **讓 DNS 解析不通但 WiFi 仍連著** —— 這是最寫實的觸發條件。做法擇一：
   - 拔掉路由器的 WAN 線（或關掉上網），保持 WiFi AP 正常運作
   - 或在路由器上把 DNS 指向一個不回應的位址
   - 或先透過 BLE 把自訂伺服器設成一個不存在的網域（例如 `no-such-broker.invalid`）
3. 此時 master 的 `WiFi.status()` 仍是 `WL_CONNECTED`，`loop()` 會每 10 秒
   進一次 `smartConnect()`
4. 持續觀察至少 **3 分鐘**，同時監看 slave 序列埠

**預期序列埠輸出（master）**：每一輪只會有**一行**「嘗試」，「嘗試」與緊接著的
「失敗」之間是 `mqttClient.connect()` 的阻塞區間，**不可能**印出心跳 log；
心跳 log 只會出現在**這一輪的「失敗」與下一輪的「嘗試」之間**（`loop()` 的
10 秒重連節奏，`maintainEspNow()` 在這段區間正常運作）：
```
[MQTT] 嘗試自訂伺服器 no-such-broker.invalid …
[MQTT] 自訂伺服器 no-such-broker.invalid 失敗，state=-2
（約 10 秒，期間約 10 則心跳，例如 [心跳] channel=6 配對模式=否 slave=1 × 10）
[MQTT] 嘗試 mqttgo.io …
[MQTT] mqttgo.io 失敗，state=-2
…（走完 5 台預設伺服器後）
[MQTT] 本輪所有伺服器都連不上，下次改從下一台開始
```

**要觀察什麼**：
1. **兩行「嘗試」之間是否一定隔著心跳 log**。這是本項的決定性判準。
2. 心跳 log 的時間戳（或以碼錶計）空窗最長多久。修正後單次阻塞最壞約 18 秒，
   **空窗約 18 秒是預期內的正常現象，不是失敗**。
3. slave 端全程是否沒有 `[失聯]`、繼電器沒被強制關閉。
4. 每一輪 5 台走完後，是否出現「本輪所有伺服器都連不上，下次改從下一台開始」，
   且下一輪的起點確實往後推了一台。

**什麼情況算失敗**：
- 序列埠出現**連續兩行以上的「嘗試」中間沒有任何心跳 log** → 一次呼叫試了多台，
  一次一台的修正有回歸，這是最嚴重的失敗
- 心跳空窗超過 **30 秒** → slave 必然失聯，修正無效
- slave 印出 `[失聯] 超過 30 秒沒收到心跳` 或繼電器被強制關閉
- `FIND_BEST_SERVER` 之後第一行「嘗試」不是自訂伺服器（有設定自訂伺服器時）
  → `resetMqttProbe()` 沒被呼叫，游標沒歸零

---

## 10. 清除設定回到 BLE 配網模式

> **注意（更新）**：本節原本記錄「按鈕長按重置尚未實作」，這個落差已補上——
> 見下方第 13 項驗證按鈕長按重置。本項驗證的是另一條清除路徑：**MQTT
> `reset` 指令**，兩條路徑都會呼叫 `clearNetConfig()`、行為一致（只清
> `hoban` 網路設定，`homaster` slave 名冊保留），以下步驟測 MQTT 這條路徑。

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
[MQTT] 已連線自訂伺服器 自訂broker位址
[MQTT] 已訂閱 hoban/hoban-a0b1c2d3e4f5/control
```
> **Phase 2b Task 4 更新**：同第 5 項的說明，「已連線」與「已訂閱」現在是兩行。

**失敗判定**：若看到 `[MQTT] 自訂伺服器 X 失敗，state=X` 反覆出現，且已確認
broker 端帳密設定無誤，代表密碼在某處被截斷，NVS 欄位長度或寫入邏輯有回歸。

---

## 12. 點動進行中再下持續性指令，不會被點動逾時撤銷（最終審查新增）

`setRelayPins()` 原本沒有清除 `pulseActive`，導致「點動中途改下持續性指令」時，
先前的點動計時仍在跑，時間到就把繼電器關掉，把剛下的指令無聲撤銷。
對籠門機構就是「命令保持開啟，1 秒後自己關上」。

**步驟**：
1. 對 `hoban/<masterId>/control` 送 `ON`（`pulseRelay(2000)`，點動 2 秒）
2. **在 1 秒內**從序列埠輸入 `allon`
3. 觀察 master 自己的繼電器（或 status 訊息的 `device.relay`）在接下來 5 秒內的變化

**預期結果**：繼電器在 `allon` 之後**持續保持 ON**，不會在原本的 2 秒點動到期時關閉。

**什麼情況算失敗**：繼電器在 `allon` 之後約 1 秒（即原點動的第 2 秒）自己關閉
→ `setRelayPins()` 沒有清除 `pulseActive`，回歸。

**反向確認（順序不能寫反）**：單獨送 `ON`（不下 `allon`），繼電器仍必須在 2 秒後
自動關閉。若也不關了，代表 `pulseRelay()` 裡 `setRelayPins(true)` 與
`pulseActive = true` 的先後順序被寫反。

---

## 13. 按鈕長按重置（Task 8 新增，`updateResetButton()` 非阻塞狀態機）

> **尚未在實機驗證**，以下步驟與預期序列埠輸出皆為程式碼推導。

驗證兩件事：(a) 長按流程本身能正確走完三階段並清除設定；(b) 過程中 ESP-NOW
心跳不中斷、已配對的 slave 不失聯、不被強制關閉繼電器。

**前置**：master 已配對至少 1 台 slave（開機時 `[名冊] 載入 N 台 slave` 的 N ≥ 1），
且已完成 WiFi/MQTT 設定（`hasWifiConfig()` 為 true）。

**步驟**：
1. 按住 BOOT（或 C3 版的第二按鈕／RESET）不放，同時用碼表或手機計時，
   另開一個序列埠視窗監看 slave
2. 持續按住，觀察 master LED：3 秒內應無特殊反應（沿用原本的持續式狀態指示）；
   滿 3 秒後應開始以約 250ms 週期閃爍
3. **持續按住**，閃爍需再維持 2 秒；滿 2 秒後 LED 應長亮約 0.7 秒
4. 長亮結束後 master 應自動重啟。**長亮結束後請立即鬆手**；若按著不放跨過
   重啟，重開機的 `checkStuckButtons()` 會取樣到整段 LOW，判定該按鈕「恆為
   LOW」並印出 `⚠ 按鈕自檢: ... 恆為 LOW，本次開機停用其重置功能`，本次開機
   停用該按鈕（連短按配對也會失效），這是既有的防呆機制（防止壞按鈕造成無限
   重置迴圈），不是缺陷，重新上電即恢復——實測時若看到這個訊息不要誤判成
   硬體故障
5. 全程監看 slave 序列埠，確認**沒有**出現 `[失聯]`、沒有進入輪掃、
   繼電器（若原本是 ON）沒被強制關閉

**預期序列埠輸出（master）**：
```
[重置] 偵測到按鈕按下，開始計時...
[重置] 長按 3 秒達成，開始 LED 閃爍確認...
[重置] 確認重置，LED 長亮 0.7 秒後清除網路設定...
[設定] NVS 網路設定已清除
[重置] 長按重置只清除網路設定（WiFi/MQTT），slave 配對記錄（homaster 名冊）保留，不會解除任何已配對的籠子
```
（緊接著重啟，重啟後應回到第 1 項的 BLE 配網模式輸出，且 `[名冊] 載入 N 台 slave`
的 N 應維持長按前的台數——`clearNetConfig()` 不影響 `homaster` 名冊）

整段過程中，心跳 log（`[心跳] channel=... ...`）應照常出現，**不應出現超過
30 秒的空窗**（正常情況下 3+2+0.7 ≈ 5.7 秒的長按過程中心跳幾乎不中斷，
唯一可能的空窗是 `ESP.restart()` 之後到重開機完成之間，實測約 2.2~3.0 秒
——`setup()` 的 `delay(1000)` + `delay(50)` + `checkStuckButtons()` 500ms
取樣，再加上 ESP-NOW 初始化——連同長按流程本身總空窗約 3.3~4.0 秒，遠低於
30 秒門檻）。

**已知限制（MQTT 重連可能讓閃爍確認「跳過」）**：`loop()` 的 `smartConnect()`
最壞會阻塞約 18 秒，且**沒有按鈕逃生口**（`connectToWiFi()` 的等待迴圈有
`anyResetButtonPressed()` 早退，`smartConnect()` 沒有）。若按下重置鈕當輪恰好
觸發 `smartConnect()`，會先卡住 18 秒，回來後 `pressDuration` 已遠超過 5 秒，
`updateResetButton()` 會在同一次呼叫內直接跳過閃爍確認、判定為「已按住超過
2 秒確認時間」而執行重置——使用者看不到閃爍回饋、也沒機會在閃爍階段放開取消。
心跳空窗仍受 18 秒上限（< 30 秒門檻），且要連續按住超過 18 秒才會觸發，
誤觸風險有限，不是缺陷，但實測時若發現「完全沒閃爍就直接重置」，先確認是否
剛好撞上這個時序，不要直接判定失敗。

**中途放開的取消驗證**：
- 按住 1 秒內放開 → 若原本不在配對模式，應觸發配對模式（`[配對] 進入配對模式...`），
  這是短按行為，不是重置的一部分，屬正常
- 按住 1.5 秒（超過短按上限、未滿 3 秒長按門檻）後放開 → 序列埠**不應**印出
  任何重置或配對相關訊息，LED 也不應有異常閃爍
- 按住滿 3 秒、閃爍確認階段中途放開（例如閃了 1 秒後放開）→ 應印出
  `[重置] 按鈕放開，取消重置`，LED 交還原本的持續式狀態指示，**不會**清除設定、
  **不會**重啟

**什麼情況算失敗**：
- 閃爍確認階段或長亮階段，slave 印出 `[失聯]` 或繼電器被強制關閉
  → 心跳被這段流程中斷，`updateResetButton()` 某段意外變成阻塞
- 按住 1.5 秒放開卻觸發了重置或印出取消訊息 → 短按／長按的時間窗判斷有回歸
- 重啟後 slave 名冊台數歸零 → 誤清了 `homaster` 命名空間，是嚴重回歸
  （動物管制設備的配對記錄不該因為重設網路而遺失）

---

## 疑慮與待確認事項

- 全部 13 項截至目前皆**未在實體硬體驗證**，上述輸出全為程式碼推導，實測時
  請對照實際輸出並記錄差異
- 第 2、8、9 項的「預期結果」已於最終審查後改寫。改寫前的版本會把三個 Critical
  缺陷測成 PASS，若手上有舊版列印稿請丟棄
- 第 10 項驗證的是 MQTT `reset` 指令路徑；按鈕長按重置已實作，另見第 13 項
- 第 4 項「46 秒內重新鎖定」的門檻依賴 slave 端掃描週期（`SCAN_DWELL_MS=1200ms`
  × 頻道數）與心跳連發，實測時建議額外記錄實際花費秒數，不只是「有無在時限內鎖定」
- 第 11 項需要一組密碼超過 12 字元的可用 MQTT broker 帳密，若手邊沒有，
  建議自架一個 mosquitto 測試帳號再測，不要用會影響其他人的正式帳密
