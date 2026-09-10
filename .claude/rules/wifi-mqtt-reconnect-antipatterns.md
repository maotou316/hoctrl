---
paths:
  - "**/*.ino"
---

# WiFi／MQTT 重連的四個反模式（ho_relay2 2026-08 實例）

2026-08-20 診斷「斷線後不會自動連線」時，在 `ho_relay2.ino` 一次挖出 13 項缺陷。
根因不是單一 bug，是四個彼此加乘的反模式。**其他型號（relay1／relay3）尚未套用這些修正，
出現同樣症狀時請比照處理。**

---

## 反模式 1：把「連上了但太慢」當成失敗丟掉

最致命、也最難從外部看出來的一個。舊碼：

```c
if (mqttClient.connect(...)) {          // ← 已經連上了
  unsigned long connectTime = millis() - startTime;
  if (connectTime < 1000) {
    ...訂閱、發布、return true
  } else {
    Serial.printf("太慢 (%lu ms) ✗\n", connectTime);
    mqttClient.disconnect();            // ← 把成功的連線丟掉
    return false;
  }
}
```

註釋自稱「1秒超時」，但它**不是超時**——DNS、TCP 握手、等 CONNACK 的成本全部已經付完、
連線也真的建立了，才因為碼錶超過 1 秒而主動斷掉。

台灣連海外公共 broker（emqx.io／eclipseprojects.io／hivemq.com）光 RTT 就 150~300ms，
DNS＋握手＋CONNACK 幾個往返輕易破 1 秒。而 `smartConnect()` 對五台預設伺服器套用**同一個
門檻、沒有任何 fallback**（沒有「都太慢就挑最快的那台」）。

**淨效果：每一台 broker 都連得上，設備卻永遠離線。** App 端看到的就是「不知道怎麼就斷了，
之後再也不上線」。

### 規則

> **凡是「成功之後才判定要不要接受」的程式碼，都必須有 fallback 路徑。**
> 沒有 fallback 的品質門檻，在品質普遍下降時會變成 100% 拒絕。

診斷時的決定性證據是序列埠上的 `太慢 (xxxx ms) ✗`。看到它就不必再查別的。

---

## 反模式 2：跟 core 的自動重連打架

`WiFi.setAutoReconnect(true)` 的重連跑在 **WiFi 事件任務裡，完全不佔 `loop()`**，
AP 一回來就會自己接上。舊碼卻在每次重連時先做：

```c
WiFi.disconnect(true);   // 註釋寫「true = 清除之前的 AP 配置」——寫錯了
delay(200);
WiFi.mode(WIFI_OFF);
delay(200);
WiFi.mode(WIFI_STA);
```

**`disconnect()` 的第一個參數是 `wifioff`，不是 `eraseap`**：

```cpp
// core 3.3.7 libraries/WiFi/src/WiFiSTA.h:166
bool disconnect(bool wifioff = false, bool eraseap = false, unsigned long timeoutLength = 100);
```

`wifioff=true` 會走到 `STAClass::onDisable()`（`STA.cpp:286-298`），那裡做兩件致命的事：

```cpp
Network.removeEvent(_wifi_sta_event_handle);   // 事件處理器整個移除
_esp_netif = NULL;                             // 之後 connect() 直接 return false
```

而 core 的 auto-reconnect 正是掛在那個事件處理器上（`STA.cpp:150-165`）。
**等於每次「重連」都先親手把正在運作的自動重連拆掉**，再改用阻塞 50 秒以上的方式自己重試。

### 規則

> **運行期重連不要呼叫 `WiFi.disconnect(true)` 或 `WiFi.mode(WIFI_OFF)`。**
> 讓 core 自己跑；韌體只在久久沒接回來時補送一次**非阻塞**的 `esp_wifi_connect()`。

**它擋不住什麼**：以下原因碼**不在** core 的重連白名單
`_is_staReconnectableReason()`（`STA.cpp:58-85`）裡，core 一次都不會重試，
仍然需要韌體自己跑完整探測換一種 auth 設定：

| 碼 | 名稱 |
|---|---|
| 202 | `AUTH_FAIL`（密碼錯／加密模式被換掉） |
| 210 | `NO_AP_FOUND_W_COMPATIBLE_SECURITY` |
| 211 | `NO_AP_FOUND_IN_AUTHMODE_THRESHOLD`（被自己設的 `threshold.authmode` 擋掉） |
| 212 | `NO_AP_FOUND_IN_RSSI_THRESHOLD` |

**15（`4WAY_HANDSHAKE_TIMEOUT`）、200（`BEACON_TIMEOUT`）、201（`NO_AP_FOUND`）都在白名單裡**，
core 會自己重試，不可列入。這份名單第一版就寫顛倒過一次，詳見本文末的 A-17。

---

## 反模式 3：時間戳設在阻塞呼叫「之前」

```c
unsigned long now = millis();          // ← 只取樣一次
if (now - lastWiFiCheck > 5000) {
  lastWiFiCheck = now;                 // ← 在阻塞之前就寫
  ...
  connectToWiFi();                     // ← 這裡阻塞 50~113 秒
}
```

`connectToWiFi()` 回來時 `millis() - lastWiFiCheck` 早已遠超過 5000，
**下一次 `loop()` 迭代立刻再試一次**。註釋寫的「每 5 秒檢查」在失敗時等於「不間斷連續重試」。

同一份程式碼裡 `lastReconnectAttempt`（宣稱每 10 秒）、`lastKeepAlive`（宣稱每 3 秒）
全部有同樣的問題。

### 規則

> **時間戳一律在阻塞呼叫「之後」用新的 `millis()` 取。**
> 一個函式只要可能阻塞超過節流間隔，用它前面取的 `now` 寫時間戳，那個節流就不存在。

---

## 反模式 4：把「未來時刻」存進一個被當成「過去時刻」用的變數

```c
lastWiFiCheck = now + 25000;           // 想表達「延遲到 30 秒後再檢查」
...
if (now - lastWiFiCheck > 5000) {      // 但這裡把它當「上次檢查的時刻」
```

兩邊都是 `unsigned long`（ESP32 上 32-bit）。設下一次迭代的 `now' = now + δ`：

```
now' - lastWiFiCheck = (now + δ) - (now + 25000) = (δ - 25000) mod 2^32
                     = 4294967296 - 25000 + δ = 4294942296 + δ
```

δ=0 時是 **4 294 942 296**，遠大於 5000 → **條件恆真**。
而且下一行 `lastWiFiCheck = now;` 立刻把它覆寫，`+25000` 連第二次比較的機會都沒有。

**淨效果**：序列埠印出「⚠ 重連失敗次數過多，暫停重試 30 秒」之後，設備**零間隔**地立刻
重跑整套重連。那行程式碼唯一的作用是印出一句與事實相反的話。

### 規則

> **「上次做了 X 的時刻」與「下次才允許做 X 的時刻」是兩種不同的變數，不可共用。**
> 要表達「暫停到某個時刻」就另外開一個 `nextXxxAt`，並用 wrap-safe 的有號數比較：
> ```c
> bool allowed = (nextXxxAt == 0) || ((long)(now - nextXxxAt) >= 0);
> ```
> `== 0` 那個哨兵不可省略——少了它，開機超過 **24.8 天**（`millis()` 超過 2^31）後
> `(long)(now - 0)` 會是負數，條件永遠不成立，功能直接消失。
> 設定時也要避開 0：`if (nextXxxAt == 0) nextXxxAt = 1;`

---

## 附帶：這次一併修掉的其他項目

| 項目 | 問題 |
|---|---|
| 三種「升級策略」 | 三者走同一套動作（`connectToWiFi()` 開頭就把前置的 disconnect/OFF/STA 全部重做），差別只有多墊 3 秒與 5 秒的延遲。序列埠三行不同訊息對應完全相同的行為 |
| `smartConnect()` 一次試完五台 | 單次呼叫最壞 90 秒以上（每台：不受 caller timeout 管的 DNS ＋ 3 秒 TCP ＋ 15 秒等 CONNACK 的 busy-wait）。改成 `smartConnectStep()` 每次只試一台 |
| `currentServerIndex` 只在成功時更新 | 連續重連一直打同一台剛失敗的 broker，白燒 18 秒以上才輪到下一台 |
| 早退原因碼只認 202/15 | 漏掉 210/211/212（換模式可能有救）與 201（掃描不到，整輪該放棄），導致每種 auth 模式都把 20×500ms 等滿，五種共 52 秒 |
| `scanNetworks()` 無 timeout | core 的 `_scanTimeout` 預設 **60000ms**，專案從未呼叫 `setScanTimeout()`。掃描卡住時 `loop()` 停擺一分鐘 |
| `failedAttempts` | 三處寫入、**零處讀取**。讀程式的人會以為有一套基於它的退避機制 |

---

## 修這段程式碼時，第一版自己犯的四個錯（A 族第 17~20 次）

2026-08-20 的重寫在對抗性複審裡被抓到四個問題，**全部是「照腦中的語義模型寫東西、
沒回去讀程式碼」**——與 `claim-what-it-does-not-block.md` 記錄的 16 次同型。
特別諷刺的是：當時正在**指揮子代理去查證同一份白名單**，自己寫註釋時卻沒查。

### A-17：把 reason 15 說成「不在 core 的重連白名單」

第一版寫下：

```c
// 這兩個原因碼不在 core 的重連白名單 _is_staReconnectableReason() 裡，
// core 一次都不會自己重試
bool authFailure = (lastWifiDisconnectReason == 202 || lastWifiDisconnectReason == 15);
```

**事實相反**：`STA.cpp:62` 明列 `case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:` → `return true`。
只有 **202** 不在白名單。而 210/211/212（真的不在白名單）反而**沒被列入**——兩份名單顛倒。

執行路徑上的後果：15 是訊號邊緣最常見的斷線原因之一，把它算成 authFailure
會讓設備每次弱訊號斷線都立刻進入阻塞式完整探測，**正好是這次重寫要消滅的行為**。

### A-18：拿掉了隱性的還原機制，沒補上顯式的

`connectToWiFi()` 用 `esp_wifi_set_config()` 逐一套用五種 auth 設定，返回時不還原。
舊版每 5 秒重跑一次，等於**每 5 秒把模式 1 重新套上**（隱性還原）。
新版把重跑拿掉，卻沒發現那個副作用是有人在依賴的 → 全部失敗後驅動裡留下
`WIFI_AUTH_OPEN + pmf_capable=false`，之後所有非阻塞重連都沿用它，
對 PMF required 的 AP **永遠連不上**。

同理還有 `setSleep(false)` / `setTxPower()`：它們寫的是驅動層
（`esp_wifi_set_ps()` / `esp_wifi_set_max_tx_power()`），`wifiLowLevelInit()` 不會重套。
舊版「第 7 次策略」那兩行是全檔唯一的重套點，被當成「設成本來就已經是的值」刪掉了。

> **規則**：刪掉重複執行的程式碼之前，先問「這個重複本身有沒有在提供某種還原或保活」。
> 冪等的呼叫看起來像冗餘，但在「中間有東西會把狀態打掉」的情況下，它就是唯一的修復機制。

### A-19：宣稱某個東西已經退出執行路徑，但沒 grep

新註釋讓讀者以為 `smartConnect()` 已被 `smartConnectStep()` 取代。
實際上 `mqttCallback()` 的 `FIND_BEST_SERVER` 分支**在執行期**仍然直呼它，
單次 `loop()` 迭代阻塞 111 秒以上——與舊版實質相同。

> **規則**：寫下「X 已經不再被使用」之前，`grep -n "X" ` 一次。這是一個指令的事。

### A-20：自己立的原則漏在自己身上

同一輪重寫的核心論述是「時間戳一律在阻塞呼叫之後取」，
但 `loop()` 的 MQTT 區段仍沿用 WiFi 區段阻塞前取樣的那個 `now`——
而 WiFi 區段剛剛可能跑完一次最壞 61 秒的完整探測。

### 還有一項「防線是裝飾」（同 A 族形狀）

新加的 `abortAllModes` 只在原因碼恰為 201 時生效。最壞路徑（202、211、等滿沒事件）
完全不生效，**把它整段刪掉，最壞值 61 秒一點都不會變**。
真正把 113 秒砍到 61 秒的是 `setScanTimeout()` 那一行。

第一版的註釋暗示 `abortAllModes` 擋住了那數十秒——這正是「加寬方向的突變會活下來」
的實例。現在該處已改為明文寫出「它擋不住什麼」。

---

## 診斷這類症狀的順序

1. **先看序列埠有沒有 `太慢 (xxxx ms) ✗`** —— 有就是反模式 1，不必再查
2. 看「WiFi 重連嘗試 #N」的間隔 —— 若遠小於宣稱的間隔，就是反模式 3 或 4
3. 看 `mqttClient.loop()` 的沉默窗口有沒有超過 `1.5 × keepAlive` —— 超過就會被 broker 踢
4. 盤點阻塞點時**務必回函式庫原始碼確認 timeout 管到哪**，見 `claim-what-it-does-not-block.md`

## 已知未處理

**WiFi 長期連不上時無法回到配對模式**：`bleConfigMode` 只在 `setup()` 的「EEPROM 沒存 SSID」
分支被設為 true，`loop()` 裡沒有任何 fallback。SSID 被改掉或 AP 永久消失時，
唯一的復原手段是實體長按 5 秒。

`CLAUDE.md` 寫的「AP 模式：當 WiFi 未連線時自動啟動」對 hoRelay2 **不成立**——
它連 `WebServer.h` 都沒 include。那是 hoRelay1 的行為，文件宣稱了一條 relay2 裡不存在的路徑。

未處理的理由：讓設備自己進配對模式會停止 WiFi 重連（`loop()` 開頭的 `if (bleConfigMode) return;`），
在「AP 只是長時間停電」的情況下反而更糟。要做必須讓 BLE 與 WiFi 重連並存，
那會動到 ESP32-C3 上兩者的資源競爭，風險超出這次的修正範圍。
