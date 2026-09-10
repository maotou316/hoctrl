# Master Phase 2a 實作計畫：WiFi、BLE 配網、MQTT 基礎

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 讓 `ho_master1` 能透過 App 的 BLE 配網取得 WiFi 設定、連上網路與 MQTT broker、發布自己的狀態並接受控制指令，且**全程不中斷 ESP-NOW 心跳**。

**Architecture:** 從 `ho_relay2.ino`（v1.6.0，已驗證）移植 WiFi/BLE/MQTT，但三處必須重寫才能與 ESP-NOW 共存：拿掉同步 `scanNetworks()`、不使用 `WIFI_OFF`、所有阻塞等待迴圈改為穿插心跳維持。設定改存 NVS，順帶解掉 EEPROM 位址重疊導致 MQTT 密碼只能 12 字元的缺陷。

**Tech Stack:** Arduino ESP32 core 3.3.7、PubSubClient、ArduinoJson 6.x、Preferences (NVS)、BLEDevice、esp_now.h、esp_wifi.h

**Spec:** `docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md`

**前置:** Phase 1 已完成（分支 `feature/esp32-master-slave`），`ho_master1` 已有 ESP-NOW 心跳、配對、名冊（NVS 命名空間 `homaster`）、序列埠指令、按鈕自檢。

## Global Constraints

- **繼電器安全鐵則**：`initRelayPins()` 必須是 `setup()` 第一行，早於 `Serial.begin()`。C3 的 GPIO 4/7 是 JTAG 腳，晚一行拉低就多一分 MOS 誤導通燒毀設備的風險
- **ESP-NOW 不可中斷鐵則**（Phase 2a 新增，同等重要）：master 心跳間隔 1 秒、slave 失聯門檻 30 秒。**任何超過 20 秒的阻塞都會讓所有 slave 判定失聯、開始輪掃、並強制關閉繼電器**。所以：
  - 禁用 `WiFi.scanNetworks()` 的同步形式（會逐一跳過所有 channel，2~4 秒 ESP-NOW 全滅）
  - 禁用 `WiFi.mode(WIFI_OFF)`（ESP-NOW 需要 WiFi 驅動存活）
  - 所有等待迴圈必須呼叫 `maintainEspNow()` 而非裸 `delay()`
- **變數命名**：結果變數用 `res`，不用 `result`
- **語言**：註釋、序列埠輸出、文件、commit 訊息一律繁體中文
- **JSON**：用 `StaticJsonDocument`，不用 `DynamicJsonDocument`
- **MQTT topic**：`hoban/{deviceId}/status`（發布）、`hoban/{deviceId}/control`（訂閱），deviceId 格式 `hoban-<mac12>`
- **不得修改** `ho_relay1/`、`ho_relay2/`、`ho_relay3/`、`CLAUDE.md`（有其他人的未提交修改，`ho_relay2/` 只能讀）
- **不新增外部工具鏈**：驗證走 `flash.ps1` + `arduino-cli` 1.3.1
- **雙板支援**：`ho_master1` 同時支援 WROOM 與 C3，GPIO 用 `CONFIG_IDF_TARGET_ESP32C3` / `_ESP32` 條件編譯。新增程式碼不得破壞這個結構

## 從 ho_relay2 移植時必須改掉的既有缺陷

盤點 `ho_relay2.ino` v1.6.0 時確認的問題，移植時一併解決（**不要照抄**）：

| 缺陷 | 位置 | 本計畫的處置 |
|---|---|---|
| `mqttPassword`(114-129) 與 `mqttPort`(126-127) EEPROM 位址重疊，密碼實際只能 12 字元 | `saveWiFiConfig()` | 改用 NVS，欄位獨立 |
| `mqttPassword` 尾端溢出 EEPROM 128 邊界 | 同上 | 同上 |
| 從未 `setBufferSize()`，PubSubClient 預設 256，`publishStatusWithServer()` 必定超過而靜默失敗 | 全檔 | `setBufferSize(1024)` |
| 從未 `setSocketTimeout()`，預設 15 秒，`smartConnect()` 最壞阻塞 77 秒 | 同上 | `setSocketTimeout(3)` |
| `MyServerCallbacks::onDisconnect` 沒有重新 `startAdvertising()`，App 第二次連不上 | L144-146 | 補上 |
| `onWrite` 的 `free(buffer)` 出現兩次（成功分支靠 restart 才沒炸） | L508, L534 | 統一單一釋放路徑 |
| `lastWiFiCheck = now + 25000` unsigned 下溢，「暫停 30 秒」實際不生效 | L988 | 改用獨立的暫停旗標與時間戳 |
| 掃描已取得 `authMode` 卻沒用來裁剪嘗試清單，白等 30 秒 | L1087 | 不掃描，改用漸進式重試 |
| BLE 從不 deinit，配網後仍佔 50~70KB heap | 全檔 | 配網完成即 restart（沿用），並在計畫中確認 heap |

## File Structure

```
ho_master1/
├── ho_master1.ino      # 唯一 sketch，Phase 2a 大幅擴充
└── partitions.csv      # 已存在（雙 OTA 分區）
```

**不拆檔**：Arduino sketch 目錄下的 `.ino` 會被自動合併，拆成多個 `.ino` 只會增加理解成本而無隔離效果；拆成 `.h`/`.cpp` 則需處理跨檔的全域狀態宣告。本專案既有風格是單檔，Phase 2a 沿用。檔案會成長到約 1800 行，與 `ho_relay2.ino`（1742 行）相當，在可維護範圍內。

---

## Task 1：NVS 設定儲存

把 WiFi/MQTT 設定改存 NVS，解掉 EEPROM 位址重疊缺陷。

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Phase 1 已有的 `Preferences prefs`（命名空間 `homaster`，存 slave 名冊）
- Produces（Task 2~7 依賴）：
  - 全域：`char ssid[33]`、`char password[65]`、`char mqttServer[64]`、`char mqttUsername[33]`、`char mqttPassword[65]`、`int mqttPort`、`bool useCustomServer`、`bool hasRelay`
  - `void loadNetConfig()` / `void saveNetConfig()` / `void clearNetConfig()`
  - `bool hasWifiConfig()` — `strlen(ssid) > 0`

- [ ] **Step 1: 加入全域設定變數**

在 `ho_master1.ino` 的全域區塊（Phase 1 的名冊宣告之後）加入。
**注意欄位長度都比 `ho_relay2` 大**——NVS 沒有位址重疊問題，直接給足夠空間：

```cpp
// ── 網路設定（存 NVS 命名空間 hoban，與 slave 名冊的 homaster 分開）──
// ho_relay2 用 EEPROM 128 bytes 且 mqttPassword 與 mqttPort 位址重疊，
// 導致 MQTT 密碼實際只能 12 字元。改用 NVS 後各欄位獨立，長度依 MQTT 規格給足。
char ssid[33] = "";           // WiFi SSID 上限 32 字元
char password[65] = "";       // WPA2 PSK 上限 63 字元
char mqttServer[64] = "";
char mqttUsername[33] = "";
char mqttPassword[65] = "";
int  mqttPort = 1883;
bool useCustomServer = false;
bool hasRelay = false;        // master 硬體有沒有接繼電器（韌體無法自動偵測）

Preferences netPrefs;         // 與 Phase 1 名冊用的 prefs 分開，避免鍵名衝突
```

- [ ] **Step 2: 寫存取函式**

加在全域變數之後、`getDeviceId()` 之前：

```cpp
// ── 網路設定的 NVS 存取 ──
void loadNetConfig() {
  netPrefs.begin("hoban", true);   // 唯讀
  netPrefs.getString("ssid", ssid, sizeof(ssid));
  netPrefs.getString("pass", password, sizeof(password));
  netPrefs.getString("mqttsrv", mqttServer, sizeof(mqttServer));
  netPrefs.getString("mqttuser", mqttUsername, sizeof(mqttUsername));
  netPrefs.getString("mqttpass", mqttPassword, sizeof(mqttPassword));
  mqttPort = netPrefs.getInt("mqttport", 1883);
  useCustomServer = netPrefs.getBool("customsrv", false);
  hasRelay = netPrefs.getBool("hasrelay", false);
  netPrefs.end();

  if (mqttPort <= 0 || mqttPort > 65535) mqttPort = 1883;

  Serial.printf("[設定] SSID=%s 自訂伺服器=%s 繼電器=%s\n",
                strlen(ssid) > 0 ? ssid : "(未設定)",
                useCustomServer ? "是" : "否",
                hasRelay ? "有" : "無");
}

void saveNetConfig() {
  netPrefs.begin("hoban", false);
  netPrefs.putString("ssid", ssid);
  netPrefs.putString("pass", password);
  netPrefs.putString("mqttsrv", mqttServer);
  netPrefs.putString("mqttuser", mqttUsername);
  netPrefs.putString("mqttpass", mqttPassword);
  netPrefs.putInt("mqttport", mqttPort);
  netPrefs.putBool("customsrv", useCustomServer);
  netPrefs.putBool("hasrelay", hasRelay);
  netPrefs.end();
  Serial.println("[設定] 已儲存到 NVS");
}

void clearNetConfig() {
  netPrefs.begin("hoban", false);
  netPrefs.clear();
  netPrefs.end();
  Serial.println("[設定] NVS 網路設定已清除");
}

bool hasWifiConfig() {
  return strlen(ssid) > 0;
}
```

- [ ] **Step 3: 在 setup() 載入**

在 `setup()` 的 `loadSlaves();` **之前**插入 `loadNetConfig();`。
確認 `initRelayPins()` 仍是第一行。

- [ ] **Step 4: 編譯驗證**

Run：
```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
```
Expected：兩者皆 exit code 0。

- [ ] **Step 5: Commit**

```bash
git add ho_master1/ho_master1.ino
git commit -m "Master 網路設定改存 NVS

ho_relay2 用 EEPROM 128 bytes，mqttPassword(114-129) 與 mqttPort(126-127)
位址重疊，MQTT 密碼實際只能 12 字元、超過必定認證失敗；
且 password 尾端溢出 128 邊界。改用 NVS 後各欄位獨立、長度依 MQTT 規格給足。

命名空間 hoban 與 Phase 1 名冊用的 homaster 分開，避免鍵名衝突。

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

---

## Task 2：ESP-NOW 維持機制

建立整個 Phase 2a 的地基：讓任何阻塞流程都能持續發心跳。

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Phase 1 的 `sendHeartbeat()`、`HEARTBEAT_INTERVAL`（1000）
- Produces（Task 3~7 全部依賴）：
  - `void maintainEspNow()` — 到時間就發心跳，不阻塞
  - `void espNowDelay(unsigned long ms)` — 取代所有 `delay()`，等待期間維持心跳與按鈕檢查

- [ ] **Step 1: 寫維持函式**

加在 `sendHeartbeat()` 之後：

```cpp
// ── ESP-NOW 維持機制 ──
// Phase 2a 引入 WiFi/MQTT 後，連線流程有大量阻塞等待。
// slave 的失聯門檻是 30 秒，超過就會判定失聯、開始輪掃、並強制關閉繼電器。
// 所以所有等待都必須走這裡，讓心跳在阻塞期間照常發出。
void maintainEspNow() {
  static unsigned long lastBeat = 0;
  unsigned long now = millis();
  if (now - lastBeat >= HEARTBEAT_INTERVAL) {
    lastBeat = now;
    sendHeartbeat();
  }
}

// 取代 delay()：等待期間維持心跳，並保留按鈕重置的可用性
void espNowDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    maintainEspNow();
    delay(10);
  }
}
```

- [ ] **Step 2: 把 loop() 的心跳改走維持函式**

Phase 1 的 `loop()` 有一段自己的心跳計時（`static unsigned long lastHeartbeat`）。
把它整段換成呼叫 `maintainEspNow();`，避免兩套計時器並存導致重複發送。

**注意**：`sendHeartbeatBurst()`（channel 改變時連發）保持原樣，那是另一個用途。
但它內部的 `delay(200)` 要換成 `espNowDelay` 以外的處理——burst 本身就是在發心跳，
不需要再穿插，改成直接 `delay(200)` 即可（總共只有 600ms，遠低於 30 秒門檻）。

- [ ] **Step 3: 編譯驗證**

Run：`.\flash.ps1 -Model master` 與 `.\flash.ps1 -Model master-c3`
Expected：兩者 exit code 0。

- [ ] **Step 4: Commit**

```bash
git add ho_master1/ho_master1.ino
git commit -m "新增 ESP-NOW 維持機制，供 Phase 2a 的阻塞流程使用

slave 失聯門檻 30 秒，而 WiFi 連線與 MQTT 連線流程有大量阻塞等待。
maintainEspNow() 讓心跳在阻塞期間照常發出，espNowDelay() 取代裸 delay()。
loop() 原有的心跳計時併入 maintainEspNow()，避免兩套計時器重複發送。

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

---

## Task 3：WiFi 連線（ESP-NOW 友善版）

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 1 的 `ssid`/`password`/`hasWifiConfig()`、Task 2 的 `espNowDelay()`、Phase 1 的 `sendHeartbeatBurst()`
- Produces（Task 4~7 依賴）：
  - `void connectToWiFi()` — 非破壞性連線，不掃描不關 WiFi
  - `void onWiFiEvent(WiFiEvent_t, WiFiEventInfo_t)`
  - 全域：`volatile uint8_t lastWifiDisconnectReason`、`unsigned long wifiConnectedTime`

> **2026-08-15 review 修正**：以下 Step 1/2/4 的程式碼區塊已更新為 review 後的最終版本
> （原始 brief 版本有 2 個 Critical + 1 個 Important + 1 個 Minor 問題，詳見本節末尾
> 「Review 修正紀錄」）。Step 3（`onWifiChannelMayHaveChanged()`）與 Step 5
> （`setup()` 接上 WiFi）的程式碼未受影響，維持原樣。

- [ ] **Step 1: 加入全域與事件回調**

```cpp
// ── WiFi 狀態 ──
volatile uint8_t lastWifiDisconnectReason = 0;
unsigned long wifiConnectedTime = 0;
uint8_t lastKnownChannel = 0;     // 用於偵測 channel 變化

// lastApChannel／lastApBssid／haveLastApBssid 的宣告已上移到 netPrefs 附近
//（loadNetConfig()／saveNetConfig() 要存取 lastApChannel，見最終審查修正）。
// lastApChannel 寫進 NVS 的 hoban/apch；另有 slaveLockChannel 寫進 homaster/espch。
// 兩者用途不同、命名空間刻意分開，理由見程式碼註釋與下方「最終審查修正」章節。

// 「WiFi 關聯進行中」旗標。關聯期間心跳間隔臨時加密到 HEARTBEAT_INTERVAL_ASSOC(200ms)
bool wifiAssociating = false;

// 連續幾次「鎖定 channel 關聯」失敗後，才升級成一次全頻掃描
const int WIFI_CHANNEL_LOCK_MAX_FAIL = 10;
int wifiChannelLockFailCount = 0;

// WiFi 事件回調：取得底層斷線原因碼，並在取得 IP 時通知 slave 新 channel
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[WiFi] 斷線原因碼: %d\n", lastWifiDisconnectReason);
    // 常見：2=AUTH_EXPIRE 15=4WAY_HANDSHAKE_TIMEOUT 201=NO_AP_FOUND 202=AUTH_FAIL
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    // 設在事件回調而非 connectToWiFi() 的成功分支：GOT_IP 是「取得 IP」這件事的
    // 唯一權威來源，不論由哪條路徑觸發都會進來。
    // 註（最終審查更正）：舊版理由「setAutoReconnect(true) 背景重連不會經過
    // connectToWiFi()」是錯的 —— core 3.3.7 的 _autoReconnect 只存在於建構子／
    // setter／getter，斷線事件處理完全沒讀它，setAutoReconnect() 實際上是死碼。
    // 結論（設在 GOT_IP）仍然正確且更精確。
    wifiConnectedTime = millis();
    Serial.printf("[WiFi] 取得 IP: %s\n", WiFi.localIP().toString().c_str());
  }
}
```

- [ ] **Step 2: 寫 ESP-NOW 友善的連線函式**

**與 `ho_relay2` 的三個關鍵差異，這是本 Task 的核心，不可省略：**

1. **不呼叫 `WiFi.scanNetworks()`** — 同步掃描會逐一跳過所有 channel，2~4 秒內 ESP-NOW 全滅。`ho_relay2` 掃描是為了診斷與取得 authMode，但取得的 authMode 從未被使用，所以整段拿掉沒有損失
2. **不呼叫 `WiFi.mode(WIFI_OFF)`** — ESP-NOW 需要 WiFi 驅動存活
3. **等待迴圈走 `maintainEspNow()`** — 而非裸 `delay(500)`

```cpp
// ESP-NOW 友善的 WiFi 連線
// 與 ho_relay2 的差異：不掃描頻道、不關閉 WiFi 驅動、等待迴圈走 maintainEspNow()。
// 最壞阻塞約 15 秒，期間心跳照常發出（review 修正：maxWaitMs 由 30000 降到 15000）。
//
// 已知限制（裁決不補）：不像 ho_relay2 那樣針對不同 auth mode 重試多種連線方式，
// 遇到需要特殊退避流程的路由器可能連不上，詳見 ho_master1/readme.md「已知限制」章節。
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (!hasWifiConfig()) return;

  Serial.printf("[WiFi] 連線到 %s …\n", ssid);
  lastWifiDisconnectReason = 0;

  // WiFi.disconnect(wifioff=false, eraseap=false)：第一個參數是 wifioff，不是「是否抹除
  // 設定」。傳 false 代表只斷開目前的 AP 連線、WiFi 驅動繼續存活，這正是 ESP-NOW 需要的；
  // ho_relay2 用的 disconnect(true) 其實是把 wifioff 設成 true、等同關閉 WiFi 驅動，
  // 跟它自己註釋寫的「抹除設定」是兩回事（ho_relay2 那處註釋本身寫錯）。
  WiFi.disconnect(false);
  espNowDelay(100);

  // 進入關聯階段：心跳改用加密間隔（最終審查修正 Critical 3）
  wifiAssociating = true;

  // 三段式優先序（最終審查修正 Critical 3）：
  //   1. channel＋BSSID → 定向關聯，完全不掃描
  //   2. 只有 channel  → WiFi.begin(ssid, pass, ch, nullptr)。依 ESP-IDF 文件
  //      是「以該 channel 起始掃描」而非「鎖定在該 channel」：AP 在該 channel 時
  //      一擊命中、完全跳過掃描；AP 不在時依文件字面意思仍可能續掃其餘頻道
  //      （機制未經實機驗證，待確認）。安全網是失敗分支的 channel 復位（必中）
  //      ＋關聯期 200ms 加密心跳，兩層疊加下 30 秒空窗理論上不會發生
  //   3. 都沒有        → 才退回全頻掃描（一輪約 20 秒，心跳命中率剩約 1/13）
  if (haveLastApBssid) {
    Serial.printf("[WiFi] 使用已知 channel=%u 的 BSSID 直接關聯，跳過掃描\n", lastApChannel);
    WiFi.begin(ssid, password, lastApChannel, lastApBssid);
  } else if (lastApChannel >= 1 && lastApChannel <= 13) {
    Serial.printf("[WiFi] 不指定 BSSID，但把掃描限制在已知 channel=%u\n", lastApChannel);
    WiFi.begin(ssid, password, lastApChannel, nullptr);
  } else {
    Serial.println("[WiFi] 無已知 channel，退回全頻掃描關聯");
    WiFi.begin(ssid, password);
  }

  // 等待最多 15 秒，期間持續發心跳（關聯中自動加密到 200ms）
  const int maxWaitMs = 15000;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < maxWaitMs) {
    maintainEspNow();
    if (anyResetButtonPressed()) {
      wifiAssociating = false;
      return;   // 讓長按重置仍可用
    }
    delay(100);

    // 認證失敗類的原因碼不必等滿，直接放棄本次
    if (lastWifiDisconnectReason == 202 || lastWifiDisconnectReason == 15) {
      Serial.println("[WiFi] 認證失敗，放棄本次嘗試");
      break;
    }
  }

  wifiAssociating = false;

  if (WiFi.status() == WL_CONNECTED) {
    // wifiConnectedTime 改在 onWiFiEvent() 的 GOT_IP 分支設定，這裡不重複設
    Serial.printf("[WiFi] 已連線 IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    lastApChannel = WiFi.channel();
    memcpy(lastApBssid, WiFi.BSSID(), 6);
    haveLastApBssid = true;
    wifiChannelLockFailCount = 0;
    saveApChannel(lastApChannel);                          // hoban/apch
    if (slaveCount > 0) saveSlaveLockChannel(lastApChannel); // homaster/espch
    onWifiChannelMayHaveChanged();
  } else {
    Serial.printf("[WiFi] 連線失敗，狀態=%d 原因碼=%d\n",
                  WiFi.status(), lastWifiDisconnectReason);
    // 只清 BSSID，刻意「不」跟著清掉 lastApChannel（最終審查修正 Critical 3）：
    // 舊版兩個一起清，導致第 2 次重試起就全部退回全頻掃描，30 秒 30 則心跳
    // 全數落空的機率約 9%，而落空的後果是 slave 強制打開繼電器。
    if (haveLastApBssid) {
      Serial.println("[WiFi] 指定 BSSID 關聯失敗，清除 BSSID 記錄，下次改為只鎖定 channel 掃描");
      haveLastApBssid = false;
    }
    if (lastApChannel != 0) {
      wifiChannelLockFailCount++;
      if (wifiChannelLockFailCount >= WIFI_CHANNEL_LOCK_MAX_FAIL) {
        wifiChannelLockFailCount = 0;
        Serial.printf("[WiFi] 已連續 %d 次在 channel %u 上關聯失敗，下次改為全頻掃描重新學習\n",
                      WIFI_CHANNEL_LOCK_MAX_FAIL, lastApChannel);
        lastApChannel = 0;
      }
    }
  }
}
```

- [ ] **Step 3: 寫 channel 變化偵測**

這是 Phase 1 channel 同步機制第一次面對真實情境的接點。
master 連上 AP 後 channel 由 AP 決定（1/6/11 都可能），與 Phase 1 固定的 channel 1 不同。

加在 `connectToWiFi()` 之前（因為它被呼叫）：

```cpp
// 偵測 WiFi channel 是否改變，改變就立刻連發心跳通知 slave
// slave 靠心跳得知 master 的 channel，若不通知會等到 30 秒失聯門檻才開始輪掃
void onWifiChannelMayHaveChanged() {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);

  if (primary != lastKnownChannel) {
    Serial.printf("[channel] 由 %u 變為 %u，連發心跳通知 slave\n",
                  lastKnownChannel, primary);
    lastKnownChannel = primary;
    currentChannel = primary;
    sendHeartbeatBurst();
  }
}
```

在 `loop()` 的 WiFi 管理區塊也要定期呼叫（AP 可能在不斷線的情況下換頻）。

- [ ] **Step 4: 寫 loop() 的 WiFi 管理區塊**

`ho_relay2` 的三段式策略含 `WIFI_OFF`，不能照抄。改成兩段式且不關 WiFi 驅動：

```cpp
  // ── WiFi 連線管理（每 5 秒檢查）──
  static unsigned long lastWiFiCheck = 0;
  static int wifiFailCount = 0;
  static unsigned long wifiPauseUntil = 0;   // 取代 ho_relay2 會 unsigned 下溢的寫法

  if (hasWifiConfig() && now - lastWiFiCheck > 5000) {
    if (WiFi.status() != WL_CONNECTED) {
      // wrap-safe 寫法（review 修正 Minor）：(long)(now - wifiPauseUntil) < 0
      // 等同「now 還沒到 wifiPauseUntil」，millis() 溢位時仍成立；
      // 原本的 `now < wifiPauseUntil` 在溢位當下不 wrap-safe。
      if (wifiPauseUntil != 0 && (long)(now - wifiPauseUntil) < 0) {
        // 連續失敗過多，暫停重試中：不更新 lastWiFiCheck，暫停一過期下次檢查立即生效
      } else {
        wifiPauseUntil = 0;
        wifiFailCount++;
        Serial.printf("[WiFi] 重連嘗試 #%d\n", wifiFailCount);

        if (wifiFailCount <= 3) {
          connectToWiFi();
        } else {
          // 重設連線狀態但不關閉 WiFi 驅動（ESP-NOW 要靠它）
          Serial.println("[WiFi] 重設連線狀態後重試");
          esp_wifi_disconnect();
          espNowDelay(500);
          connectToWiFi();
        }

        // review 修正 Critical：connectToWiFi() 最壞阻塞 15 秒，若沿用進入本區塊前的
        // now 記錄 lastWiFiCheck，下一輪 loop() 的 now 已經超前 15 秒以上，
        // (now - lastWiFiCheck > 5000) 立刻成立，15 秒的嘗試會背靠背重試。
        // 必須在阻塞呼叫「之後」用新的 millis() 記錄。
        lastWiFiCheck = millis();

        if (WiFi.status() == WL_CONNECTED) {
          wifiFailCount = 0;
        } else if (wifiFailCount > 10) {
          wifiFailCount = 0;
          wifiPauseUntil = lastWiFiCheck + 60000;   // 暫停 60 秒再試
          Serial.println("[WiFi] 連續失敗過多，暫停 60 秒");
        }
      }
    } else {
      lastWiFiCheck = now;
      wifiFailCount = 0;
      onWifiChannelMayHaveChanged();   // AP 可能不斷線就換頻
    }
  }
```

**另一處 review 修正 Critical（與本區塊相鄰、需一併理解）**：`loop()` 原本在這個
WiFi 區塊之後才做點動（`pulseActive`）結束檢查，但 `connectToWiFi()` 最壞阻塞 15 秒，
期間這段檢查完全不會跑，等於點動會被拖長到「阻塞時間＋原訂點動秒數」（`allpulse` 2 秒
可能被拖到 15~30 秒）。修法是把點動結束檢查搬進 Task 2 的 `maintainEspNow()`
（所有阻塞等待都會呼叫到的地方），`loop()` 原本那段檢查移除：

```cpp
void maintainEspNow() {
  static unsigned long lastBeat = 0;
  unsigned long now = millis();
  // WiFi 關聯期間改用加密間隔 200ms（最終審查修正 Critical 3）
  unsigned long interval = wifiAssociating ? HEARTBEAT_INTERVAL_ASSOC : HEARTBEAT_INTERVAL;
  if (now - lastBeat >= interval) {
    lastBeat = now;
    sendHeartbeat();
  }

  // 點動結束檢查併入這裡，確保無論卡在哪個阻塞點，繼電器都能準時關閉
  if (pulseActive && (now - pulseStartTime) >= pulseDuration) {
    pulseActive = false;
    setRelayPins(false);
  }
}
```

這處修改實際落在 Task 2 產出的 `maintainEspNow()` 內，但問題是本 Task（WiFi 連線引入
長阻塞）才觸發的，記在此處而非回改 Task 2 的區塊。

- [ ] **Step 5: setup() 接上 WiFi**

在 `setup()` 的 `setupEspNow()` 與 `registerAllPeers()` **之後**加入：

```cpp
  WiFi.onEvent(onWiFiEvent);
  // 註（最終審查更正）：setAutoReconnect(true) 在 core 3.3.7 是死碼，_autoReconnect
  // 只存在於建構子／setter／getter，斷線事件處理完全沒讀它。真正負責重連的是
  // loop() 的 WiFi 管理區塊。保留這行只為與其他 sketch 寫法一致並向前相容。
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);      // 禁用睡眠，避免 ESP-NOW 收包延遲

  if (hasWifiConfig()) {
    connectToWiFi();
  } else {
    bleConfigMode = true;
    // 最終審查修正 Critical 2：本次開機不關聯 WiFi，channel 會停在 WIFI_STA 預設的 1，
    // 先從 NVS(homaster/espch) 切回 slave 鎖定的 channel，否則整段配網期間
    // 已配對的 slave 全部失聯關籠
    restoreEspNowChannelForOfflineBoot();
    setupBLE();
  }
```

**順序很重要**：ESP-NOW 必須先 init，WiFi 才連線。反過來會讓 `esp_now_init()` 在
STA 已連線的狀態下執行，peer 的 channel 跟隨行為可能不如預期。

### Review 修正紀錄（2026-08-15）

最強模型 review 原始實作後抓到 2 個 Critical + 1 個 Important + 1 個 Minor：

1. **Critical－點動被拖長到 60 秒**：`loop()` 把點動結束檢查排在 WiFi 管理區塊之後，
   而 `connectToWiFi()` 最壞阻塞（原 30 秒），阻塞期間該檢查完全不會跑。
   修法：檢查併入 `maintainEspNow()`（見上方 Step 4 附帶的修正），
   所有阻塞等待都會呼叫到，繼電器準時關閉。
2. **Critical－`WiFi.begin()` 內建掃描 + `lastWiFiCheck` 時序錯誤**：即使不呼叫
   `WiFi.scanNetworks()`，`WiFi.begin(ssid, password)` 不帶 channel/BSSID 時 ESP-IDF
   底層仍會全頻道掃描一輪；且 `lastWiFiCheck = now;` 原本設在阻塞呼叫之前，
   下一輪 `loop()` 的 `now` 已超前，「每 5 秒檢查」在失敗情境下形同虛設、變成背靠背重試。
   三處修法：(a) `lastWiFiCheck` 改在 `connectToWiFi()` 之後用新 `millis()` 記錄；
   (b) 記住上次成功關聯的 channel/BSSID，重連時直接指定跳過掃描，關聯失敗就清除記錄
   退回一般連線；(c) `maxWaitMs` 由 30000 降到 15000。
3. **Important－`wifiConnectedTime` 在背景重連時不會更新**：`setAutoReconnect(true)`
   背景重連成功不經過 `connectToWiFi()`。修法：改在 `onWiFiEvent()` 的
   `ARDUINO_EVENT_WIFI_STA_GOT_IP` 分支設定。
4. **Minor－`wifiPauseUntil` 的 millis 溢位**：`now < wifiPauseUntil` 不是 wrap-safe。
   修法：改成 `(long)(now - wifiPauseUntil) < 0`。

**裁決不修**：reviewer 另外提到拿掉 `ho_relay2` 的 5 段 auth mode 退避會讓需要特殊
退避流程的路由器連不上。這輪不補——補回退避會讓阻塞時間再拉長數倍，與這輪壓縮阻塞
時間的方向直接衝突。已記錄在 `ho_master1/readme.md`「已知限制」章節。

- [ ] **Step 6: 編譯驗證**

Run：`.\flash.ps1 -Model master` 與 `.\flash.ps1 -Model master-c3`
Expected：兩者 exit code 0。

- [ ] **Step 7: Commit**

```bash
git add ho_master1/ho_master1.ino
git commit -m "Master 接上 WiFi，連線流程改寫為不干擾 ESP-NOW

ho_relay2 的 connectToWiFi() 有三處會讓 ESP-NOW 停擺，不可照抄：
- WiFi.scanNetworks() 同步全頻道掃描 2~4 秒，期間 STA 逐一跳過所有 channel
  （該掃描取得的 authMode 從未被使用，整段拿掉沒有損失）
- WiFi.mode(WIFI_OFF) 會讓 ESP-NOW peer 失效
- 等待迴圈的裸 delay(500) 期間發不出心跳

改為：只 disconnect 不關驅動、不掃描直接 begin、等待走 maintainEspNow()。
最壞阻塞由 55 秒降到 30 秒，且期間心跳照常。

另新增 channel 變化偵測：master 連上 AP 後 channel 由 AP 決定，
變化時立刻連發心跳，否則 slave 要等 30 秒失聯門檻才會開始輪掃。

loop() 的重連策略改為兩段式，並修掉 ho_relay2 那個
lastWiFiCheck = now + 25000 會 unsigned 下溢、導致暫停不生效的寫法。

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

---

## Task 4：MQTT 多伺服器連線

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 1 的設定變數、Task 2 的 `espNowDelay()`
- Produces（Task 5~7 依賴）：
  - `void smartConnect()`、`bool quickConnectToIndex(int)`、`bool quickConnectCustom()`
  - 全域：`WiFiClient espClient`、`PubSubClient mqttClient`、`DEFAULT_SERVERS[]`、`currentServerIndex`

- [ ] **Step 1: 加入 MQTT 全域**

```cpp
#include <PubSubClient.h>

struct MqttServerConfig {
  const char* server;
  int port;
  const char* username;
  const char* password;
};

// 與 Flutter App 的 multi_mqtt_service.dart 保持一致
const MqttServerConfig DEFAULT_SERVERS[] = {
  {"mqttgo.io",               1883, NULL,         NULL},
  {"broker.hoban.tw",         1883, "hoban_user", "hoban_pass"},
  {"mqtt.eclipseprojects.io", 1883, NULL,         NULL},
  {"broker.emqx.io",          1883, NULL,         NULL},
  {"broker.hivemq.com",       1883, NULL,         NULL},
};
const int DEFAULT_SERVER_COUNT = sizeof(DEFAULT_SERVERS) / sizeof(DEFAULT_SERVERS[0]);
int currentServerIndex = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
```

- [ ] **Step 2: 寫連線函式**

**兩個必須加的設定，`ho_relay2` 都沒有：**

```cpp
// 連線到指定的預設伺服器
bool quickConnectToIndex(int index) {
  if (index < 0 || index >= DEFAULT_SERVER_COUNT) return false;
  const MqttServerConfig& cfg = DEFAULT_SERVERS[index];

  mqttClient.setServer(cfg.server, cfg.port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  // ho_relay2 沒設這兩個，導致：
  // - buffer 預設 256，帶 server 欄位的狀態 JSON 必定超過而 publish 靜默失敗
  // - socket timeout 預設 15 秒，5 台輪一輪最壞阻塞 77 秒
  mqttClient.setBufferSize(1024);
  mqttClient.setSocketTimeout(3);

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  StaticJsonDocument<160> willDoc;
  willDoc["device_id"] = deviceId;
  willDoc["status"] = "offline";
  willDoc["server"] = cfg.server;
  willDoc["timestamp"] = millis() / 1000;
  char willBuf[160];
  serializeJson(willDoc, willBuf);

  Serial.printf("[MQTT] 嘗試 %s …\n", cfg.server);
  bool ok = mqttClient.connect(deviceId, cfg.username, cfg.password,
                               statusTopic.c_str(), 1, true, willBuf, true);
  if (!ok) {
    Serial.printf("[MQTT] %s 失敗，state=%d\n", cfg.server, mqttClient.state());
    return false;
  }

  String controlTopic = String("hoban/") + deviceId + "/control";
  mqttClient.subscribe(controlTopic.c_str());
  currentServerIndex = index;
  Serial.printf("[MQTT] 已連線 %s，訂閱 %s\n", cfg.server, controlTopic.c_str());
  publishStatus();
  return true;
}
```

`quickConnectCustom()` 結構相同，差別：用 `mqttServer`/`mqttPort`，帳密 `strlen() > 0` 才傳否則 `NULL`，且**不更新** `currentServerIndex`。請照此寫出。

```cpp
// 最終審查修正 Critical 1：改成「每次呼叫只嘗試一台 broker」。
// 語義不變（自訂伺服器優先 → 從上次成功的位置輪詢預設清單），但改由檔案層級的
// 游標推進，其餘交給 loop() 既有的 10 秒重連節奏。
//
// 為什麼非改不可：單次 mqttClient.connect() 對不可達目標最壞約 18 秒
//（NetworkClient::connect() 先做 getaddrinfo()，這段沒有 timeout 參數，
// 由 lwIP 的 DNS_MAX_RETRIES 指數退避決定約 15 秒；
// WIFI_CLIENT_DEF_CONN_TIMEOUT_MS=3000 只管 TCP、setSocketTimeout(3) 只管 CONNACK），
// 而它是不可中斷的阻塞呼叫，期間 maintainEspNow() 完全不會被叫到。
// 舊寫法在自訂伺服器失敗後立刻接第一台預設伺服器 = 背靠背 36 秒 > 30 秒門檻，
// slave 必定失聯關籠。觸發條件：AP 正常但 WAN/DNS 不通（此時 WiFi 仍 WL_CONNECTED，
// loop() 每 10 秒進來一次，每次都製造 36 秒心跳真空）。
bool mqttCustomTried = false;
int  mqttProbeOffset = 0;
bool mqttLastHasCustom = false;

void resetMqttProbe() {
  mqttCustomTried = false;
  mqttProbeOffset = 0;
}

void smartConnect() {
  if (!WiFi.isConnected()) return;

  bool hasCustom = (useCustomServer && strlen(mqttServer) > 0);
  if (hasCustom != mqttLastHasCustom) {   // 設定被改動 → 游標歸零重新開始
    mqttLastHasCustom = hasCustom;
    resetMqttProbe();
  }

  if (hasCustom && !mqttCustomTried) {
    mqttCustomTried = true;
    if (quickConnectCustom()) {
      resetMqttProbe();
      return;
    }
    return;   // 本次呼叫只嘗試這一台
  }

  int index = (currentServerIndex + mqttProbeOffset) % DEFAULT_SERVER_COUNT;
  mqttProbeOffset++;
  if (quickConnectToIndex(index)) {
    resetMqttProbe();   // 成功時 quickConnectToIndex() 已把 currentServerIndex 設為 index
    return;
  }

  if (mqttProbeOffset >= DEFAULT_SERVER_COUNT) {
    currentServerIndex = (currentServerIndex + 1) % DEFAULT_SERVER_COUNT;
    resetMqttProbe();
    Serial.println("[MQTT] 本輪所有伺服器都連不上，下次改從下一台開始");
  }
}
```

`FIND_BEST_SERVER` 指令在呼叫 `smartConnect()` 前必須先 `resetMqttProbe()`，
語義才是「重新挑一台最好的」而非「接著上次的位置繼續」。

- [ ] **Step 3: loop() 的 MQTT 管理**

```cpp
  // ── MQTT 連線管理 ──
  static unsigned long lastReconnect = 0;
  static unsigned long lastStatusPub = 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (now - lastReconnect > 10000) {
        smartConnect();
        // 最終審查修正：lastReconnect 必須在阻塞呼叫「之後」用新的 millis() 記錄。
        // smartConnect() 最壞阻塞約 18 秒，若沿用進入迴圈前的 now，10 秒節流立刻
        // 成立、變成背靠背重試，兩次阻塞之間只擠得出一則心跳。
        lastReconnect = millis();
      }
    } else {
      mqttClient.loop();
      if (now - lastStatusPub > 10000) {   // 每 10 秒發一次狀態
        lastStatusPub = now;
        publishStatus();
      }
    }
  }
```

**狀態發布間隔用 10 秒**，不是 `ho_relay2` 的 3 秒——master 還要發 ESP-NOW 心跳（每 1 秒）與輪詢 slave，3 秒太密。

- [ ] **Step 4: 處理與 Task 5 的循環依賴**

本 Task 的 `setCallback(mqttCallback)` 與 `quickConnectToIndex()` 結尾的 `publishStatus()`
都引用 Task 5 才定義的函式。**單獨編譯會失敗**（Arduino 的自動原型產生解決宣告順序，
但解決不了「函式根本不存在」）。

處理方式：本 Task 先寫出兩個可運作的最小版本，Task 5 再擴充成完整版。

```cpp
// Task 5 會擴充成完整的狀態 JSON
void publishStatus() {
  if (!mqttClient.connected()) return;
  String topic = String("hoban/") + getDeviceId() + "/status";
  mqttClient.publish(topic.c_str(), "{\"status\":\"online\"}", true);
}

// Task 5 會擴充成完整的指令分派
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] 收到訊息，長度 %u（指令處理待 Task 5 實作）\n", length);
}
```

這樣本 Task 的 commit 是可編譯、可燒錄、能實際連上 broker 的狀態，
Task 5 的 diff 也會清楚呈現「最小版 → 完整版」的擴充。

- [ ] **Step 5: 編譯驗證**

Run：`.\flash.ps1 -Model master` 與 `.\flash.ps1 -Model master-c3`
Expected：兩者 exit code 0。

- [ ] **Step 6: Commit**

訊息要說明 `setBufferSize(1024)` 與 `setSocketTimeout(3)` 修掉的兩個 ho_relay2 缺陷
（buffer 256 導致帶 server 欄位的狀態必定靜默失敗、socket timeout 15 秒導致輪一輪最壞 77 秒）。

---

## Task 5：狀態發布與控制指令

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 4 的 `mqttClient`、Phase 1 的 `slaveCount`/`pairingMode`/`currentChannel`/`longRangeEnabled`/`relayState`
- Produces：`void publishStatus()`、`void mqttCallback(char*, byte*, unsigned int)`

- [ ] **Step 1: 把 Task 4 的最小版 publishStatus() 擴充成完整版**

依規格的 master status 格式（Phase 2a 先不含 `slaves` 陣列，那是 2b）。
直接取代 Task 4 寫的那個兩行版本：

```cpp
void publishStatus() {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String topic = String("hoban/") + deviceId + "/status";

  StaticJsonDocument<512> doc;
  doc["device_id"] = deviceId;
  doc["status"] = "online";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["timestamp"] = millis() / 1000;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["connected"] = WiFi.isConnected();
  wifi["ssid"] = ssid;
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();

  JsonObject dev = doc.createNestedObject("device");
  dev["relay"] = relayState ? 1 : 0;
  dev["has_relay"] = hasRelay;
  dev["pairing"] = pairingMode;
  dev["slave_count"] = (int)slaveCount;
  dev["channel"] = currentChannel;
  dev["long_range"] = longRangeEnabled;

  // 最終審查修正 6：真正的截斷邊界是 char buf[512]，不是 mqttClient 的 1024 buffer。
  // 附加修正（final-fix-report.md）：本專案的 ArduinoJson 7.4.3 中
  // StaticJsonDocument<512> 只是相容殼、N 被忽略、底層動態配置，doc.overflowed()
  // 量的是「記憶體配置失敗」不是「超過 512 bytes」，訊息已改措辭，真正有效的
  // 截斷偵測是下面的 n >= sizeof(buf) - 1。
  if (doc.overflowed()) {
    Serial.println("⚠ [MQTT] 狀態 JSON 序列化時記憶體配置失敗（ArduinoJson 7 的"
                   "doc.overflowed() 語意，非容量超限；heap 壓力大時可能發生）");
  }

  char buf[512];
  size_t n = serializeJson(doc, buf);
  if (n >= sizeof(buf) - 1) {
    Serial.printf("⚠ [MQTT] 序列化結果已填滿 buf[%u]，內容可能被截斷\n",
                  (unsigned)sizeof(buf));
  }
  bool res = mqttClient.publish(topic.c_str(), buf, true);   // 專案慣例用 res 不用 ok
  if (!res) {
    Serial.printf("[MQTT] 狀態發布失敗（長度 %u，buffer %u）\n",
                  (unsigned)n, (unsigned)mqttClient.getBufferSize());
  }
}
```

- [ ] **Step 2: 把 Task 4 的最小版 mqttCallback() 擴充成完整版**

Phase 2a 只做 master 自己的指令；`ALL:*`、`PAIR:*`、`UNPAIR:*` 等 slave 相關的留給 2b。
直接取代 Task 4 寫的那個只印訊息的版本：

```cpp
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  String expected = String("hoban/") + getDeviceId() + "/control";
  if (String(topic) != expected) return;

  Serial.printf("[MQTT] 收到指令: %s\n", message.c_str());

  if (message == "status") {
    publishStatus();
  } else if (message == "ON") {
    pulseRelay(2000);
    publishStatus();
  } else if (message == "OFF") {
    setRelayPins(false);
    publishStatus();
  } else if (message == "reset") {
    clearNetConfig();
    espNowDelay(1000);
    ESP.restart();
  } else if (message == "FIND_BEST_SERVER") {
    mqttClient.disconnect();
    espNowDelay(500);
    smartConnect();
  } else if (message == "HASRELAY:ON" || message == "HASRELAY:OFF") {
    hasRelay = (message == "HASRELAY:ON");
    saveNetConfig();
    Serial.printf("[設定] 繼電器宣告為 %s\n", hasRelay ? "有接" : "未接");
    publishStatus();
  } else {
    Serial.printf("[MQTT] 未知指令: %s\n", message.c_str());
  }
}
```

**注意**：`mqttCallback` 在 `mqttClient.loop()` 內被呼叫，屬 `loop()` context，
所以可以安全呼叫阻塞函式；但仍應用 `espNowDelay()` 而非裸 `delay()`。

- [ ] **Step 3: 編譯與 Commit**

Run 兩種 FQBN 編譯，皆 exit code 0 後 commit。

---

## Task 6：BLE 配網

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 1 的設定變數與 `saveNetConfig()`
- Produces：`void setupBLE()`、`bool bleConfigMode`

- [ ] **Step 1: 加入 BLE 定義與 callback**

**UUID 必須與 `ho_relay2` 完全一致**，否則現有 App 找不到設備：

```cpp
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool bleConfigMode = false;
```

Server callback **必須補上 `ho_relay2` 缺少的重新廣播**：

```cpp
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* srv) override {
    deviceConnected = true;
    Serial.println("[BLE] App 已連線");
  }
  void onDisconnect(BLEServer* srv) override {
    deviceConnected = false;
    Serial.println("[BLE] App 已斷線，重新開始廣播");
    // ho_relay2 沒有這行，導致 App 斷線後第二次連不上
    BLEDevice::startAdvertising();
  }
};
```

- [ ] **Step 2: 寫設定接收 callback**

**JSON 欄位路徑必須與 `ho_relay2` 的實際程式碼一致，不是 CLAUDE.md 寫的格式。**
CLAUDE.md 記載的是頂層 `mqtt` 物件，但實際程式碼把 server/port/帳密全放在 `wifi` 底下，
key 名也不同（`mqtt_username` 而非 `username`）。**照文件寫會讓現有 App 送的設定被忽略。**

實際要讀的路徑：
- `doc["wifi"]["ssid"]`（必要）
- `doc["wifi"]["password"]`（必要）
- `doc["wifi"]["server"]`（必要）
- `doc["wifi"]["mqtt_username"]`（選用）
- `doc["wifi"]["mqtt_password"]`（選用）
- `doc["wifi"]["mqtt_port"]`（選用，預設 1883）

回應格式（成功）：
```json
{"status":"success","message":"WiFi 和 MQTT 設定已儲存",
 "data":{"device_id":"hoban-...","ssid":"...","mqttServer":"...","mqttPort":1883,"hasAuth":false}}
```
失敗兩種：`{"status":"error","message":"SSID、密碼或伺服器格式錯誤"}` 與
`{"status":"error","message":"無效的JSON格式"}`。

實作要求：
- 用 `StaticJsonDocument<512>` 解析
- **記憶體釋放只能有一條路徑**——`ho_relay2` 的 `free(buffer)` 出現兩次，成功分支靠 `ESP.restart()` 才沒炸。請用單一出口或 RAII 風格避免
- 三個必要欄位齊全才儲存，儲存後 `useCustomServer = true`、`saveNetConfig()`、notify 回應、`espNowDelay(2000)`、`ESP.restart()`

- [ ] **Step 3: setupBLE() 與啟動條件**

`setupBLE()` 流程照 `ho_relay2` L745-773 移植（`BLEDevice::init(deviceId)` → createServer →
createService → createCharacteristic(READ|WRITE|NOTIFY) → addDescriptor(BLE2902) →
start → advertising）。

**啟動條件與 ESP-NOW 的關係要明確**：

```cpp
  // BLE 只在「沒有 WiFi 設定」時啟動，與 ho_relay2 一致。
  // 原因：BLE stack 約佔 50~70KB heap，且與 WiFi 共用 2.4G 射頻。
  // 配網是一次性動作，完成後 restart，之後不再開 BLE。
  if (!hasWifiConfig()) {
    bleConfigMode = true;
    setupBLE();
    Serial.println("[BLE] 等待 App 配網");
  }
```

**與 `ho_relay2` 的差異**：`ho_relay2` 在 BLE 模式下 `loop()` 直接 `return`，
完全不跑其他邏輯。master **不能這樣做**——ESP-NOW 心跳仍要發，否則已配對的 slave
會在配網期間全部失聯並強制關閉繼電器。

所以 BLE 模式下 `loop()` 仍要執行：按鈕處理、`maintainEspNow()`、LED；
只跳過 WiFi/MQTT 相關區塊（反正沒有 WiFi 設定也連不上）。

- [ ] **Step 4: 編譯與 heap 檢查**

Run 兩種 FQBN 編譯。**除了 exit code 0，還要在 report 記錄 RAM 用量**——
BLE + WiFi + MQTT + ESP-NOW 同時存在是 `ho_relay2` 從未有過的組合
（它的 BLE 與 WiFi 互斥），heap 預算需要確認。

若編譯後 RAM 用量超過 80%，在 report 中明確標示風險。

> **實作後補記（2026-08-15，計畫當初沒預見）：WROOM 板需要 `PartitionScheme=custom`
> 才放得下 BLE。**
>
> `flash.ps1` 的 `master` 型號原本用 `esp32:esp32:esp32`（板子預設 OTA 雙槽分區，
> 每槽 1.31MB）。加入 BLE 後，Bluedroid stack 本身就要吃掉數百 KB 程式碼段，
> BLE + WiFi + MQTT + ESP-NOW 全部連結進同一顆映像，編譯結果超出這個分區容量
> 28%（1,680,051 / 1,310,720 bytes），直接編譯失敗（連結錯誤：本文區已超出開發板
> 的可用空間）。`master-c3` 沒遇到這問題，因為它的 FQBN 本來就帶
> `PartitionScheme=custom`，讀取 `ho_master1/partitions.csv`（app0/app1 各 1.94MB）。
>
> 修法：把 `flash.ps1` 的 `master` FQBN 也改成
> `esp32:esp32:esp32:PartitionScheme=custom`，讓 WROOM 與 C3 共用同一份
> `ho_master1/partitions.csv`。這份分區表總和剛好等於 WROOM DevKit 的 4MB flash
> （`0x400000`），且**保留了 OTA 雙槽**（app0 + app1 各 0x1F0000），沒有改用
> `huge_app` 之類犧牲雙槽的方案——Phase 4 的轉送 OTA 需要雙槽。
>
> 改完後兩種板子都編譯成功，但 WROOM 的 flash 用量對真實 app0 分區
> （2,031,616 bytes）達到 **82.7%**（1,680,083 bytes），已超過原訂的 80% 風險線，
> 只剩約 351KB 餘裕。**後續 Phase（尤其 Phase 2b 加 slaves 陣列進狀態 JSON、
> Phase 4 加轉送 OTA 邏輯）在 WROOM 板上要留意 flash 餘裕，必要時考慮拿掉
> BLE 改用其他配網方式，或評估 NimBLE 取代 Bluedroid（NimBLE 體積小很多，
> 是獨立的技術評估，未在本輪處理）。**

- [ ] **Step 5: Commit**

---

## Task 7：整合、LED 狀態與文件

**Files:**
- Modify: `ho_master1/ho_master1.ino`、`ho_master1/readme.md`
- Create: `docs/phase2a-regression-checklist.md`

- [ ] **Step 1: LED 狀態指示**

依優先序（互斥判斷），沿用 `ho_relay2` 的語義但加入 ESP-NOW 狀態：

| 優先序 | 條件 | 閃法 |
|---|---|---|
| 1 | `bleConfigMode` | 慢閃 1000ms |
| 2 | `pairingMode` | 慢閃 500ms |
| 3 | WiFi 未連線 | 快閃 300ms，30 秒後熄滅省電 |
| 4 | WiFi 已連但 MQTT 未連 | 一長二短 |
| 5 | 全部正常 | 熄滅 |

**注意**：Phase 1 已有配對模式的 LED 慢閃與非阻塞 `updateBlink()` 機制，
不要另寫一套，整合進同一個狀態機。

- [ ] **Step 2: 更新 readme**

`ho_master1/readme.md` 補上：BLE 配網流程與 JSON 格式（以實際程式碼為準）、
MQTT topic 與指令表、狀態 JSON 範例、與 `ho_relay2` 的差異說明。

- [ ] **Step 3: 寫回歸清單**

`docs/phase2a-regression-checklist.md`，內容須包含：

1. 未配網時開機，BLE 廣播出現、名稱為 `hoban-xxxxxxxxxxxx`
2. **配網期間已配對的 slave 不會失聯**（這是 master 與 `ho_relay2` 最大的行為差異）
3. App 送設定後設備重啟並連上 WiFi
4. **連上 WiFi 後 channel 改變，slave 在 46 秒內重新鎖定**（Phase 1 channel 同步機制的真實驗證）
5. MQTT 連上，`hoban/<masterId>/status` 每 10 秒收到一次
6. 狀態 JSON 欄位完整（含 `has_relay`、`slave_count`、`channel`）
7. 送 `status` / `ON` / `OFF` / `HASRELAY:ON` 各指令的反應
8. **WiFi 拔線 60 秒，確認 slave 全程不失聯**（驗證 `maintainEspNow()`）
9. MQTT broker 切換（`FIND_BEST_SERVER`）期間 slave 不失聯
10. 長按重置清除 NVS 後回到 BLE 配網模式
11. MQTT 密碼設超過 12 字元能正常認證（驗證 NVS 修掉的 EEPROM 重疊缺陷）

每項要有操作步驟與預期的序列埠輸出。清單開頭標明尚未在實體硬體執行過。

- [ ] **Step 4: 完整編譯驗證**

```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
```
三者皆 exit code 0（最後一項確保沒動到 slave）。

- [ ] **Step 5: Commit**

---

## 最終審查修正紀錄（2026-08-15，Phase 2a 收尾）

最終審查在已完成的實作上再抓到 3 個 Critical（都會讓 slave 判定失聯並強制關閉
繼電器＝籠子被打開）＋ 1 個文件缺陷 ＋ 4 個建議項。上方各 Task 的程式碼區塊
已同步更新，此處記錄修正的全貌：

| # | 等級 | 問題 | 修法 |
|---|---|---|---|
| 1 | Critical | `smartConnect()` 在自訂伺服器失敗後立刻接第一台預設伺服器，兩次 `mqttClient.connect()` 背靠背最壞 36 秒無心跳（> 30 秒門檻）。單次 18 秒的主因是 `getaddrinfo()` DNS 解析沒有 timeout 參數 | 改成每次呼叫只嘗試一台，用檔案層級游標推進，交給 `loop()` 的 10 秒節奏；`lastReconnect` 改在阻塞呼叫之後記錄 |
| 2 | Critical | BLE 配網模式永久停在 channel 1：`WiFi.mode(WIFI_STA)` 把 channel 歸 1，而 `onWifiChannelMayHaveChanged()` 在 BLE 模式下不會被呼叫 | 新增 `restoreEspNowChannelForOfflineBoot()`，開機時從 NVS 讀回 channel 並 `esp_wifi_set_channel()` ＋ `sendHeartbeatBurst()` |
| 3 | Critical | `connectToWiFi()` 失敗一次就清掉 channel 記錄，第 2 次重試起全部退回全頻掃描，30 秒心跳全數落空機率約 9% | 只清 BSSID、保留 channel，重試用 `WiFi.begin(ssid, pass, ch, nullptr)`；連續失敗 10 次才升級全頻掃描；關聯期間心跳加密到 200ms |
| 4 | 必修 | 回歸清單第 2、8、9 項的「預期結果」正好是上述三個 Critical 的觸發路徑，照舊清單測會把破口測成 PASS | 三項全部改寫，明確寫出「要觀察什麼／什麼情況算失敗」；`readme.md` 的超額宣稱改為精確敘述並新增「ESP-NOW 心跳的實際保證」章節 |
| 5 | 建議 | `setRelayPins()` 不清 `pulseActive`，點動中途下 `allon` 會在 1 秒後被點動逾時撤銷 | `setRelayPins()` 內清 `pulseActive`（`pulseRelay()` 在其後才設 true，順序安全） |
| 6 | 建議 | `publishStatus()` 無截斷偵測，ArduinoJson 超過 512 是截斷而非溢位，`publish()` 仍回 true | 加 `doc.overflowed()` 與 `n >= sizeof(buf)-1` 兩道警告 |
| 7 | 建議 | `publishStatus()` 用 `bool ok`，違反專案慣例 | 改為 `res` |
| 8 | 建議 | `setAutoReconnect` 相關註釋的論證錯誤（core 3.3.7 中它是死碼） | 更正 `onWiFiEvent()` 與 `setup()` 兩處註釋，結論不變 |

### Critical 2 的修法與原始指示的差異（實作時發現的矛盾）

原始指示要求把 channel 存進 `hoban` 命名空間，並說「`clearNetConfig()` 會把它一起
清掉，這正是我們要的」。但 **Critical 2 的觸發情境正是 `reset`**，而 `reset` 呼叫的
`clearNetConfig()` 用 `netPrefs.clear()` 清空整個 `hoban` 命名空間 —— 若 channel 只存
在那裡，重開機時 `lastApChannel` 必然是 0，`restoreEspNowChannelForOfflineBoot()`
什麼都做不了，等於沒修。

因此實作拆成**兩個值、兩個命名空間**：

- `hoban/apch` → `lastApChannel`：服務「WiFi 關聯」。`reset` 會清掉它，這是對的 ——
  留著會害使用者配了新 AP 之後還在舊 channel 上白試 10 次（約 3 分鐘）才肯全頻掃描。
- `homaster/espch` → `slaveLockChannel`：服務「ESP-NOW 心跳」，跟 slave 名冊同生共死。
  `reset` 不會清掉它，這才真正修好 Critical 2。名冊清空時它自然失效
  （`restoreEspNowChannelForOfflineBoot()` 會先檢查 `slaveCount > 0`）。

`slaveLockChannel` 的三個寫入點：`connectToWiFi()` 成功分支、`addSlave()`
（涵蓋「先連 WiFi 才配對」這個最常見順序）、`onWifiChannelMayHaveChanged()`
（涵蓋 AP 不斷線就換頻）。序列埠的 `ch <n>` 測試指令走另一條路，不經過
`onWifiChannelMayHaveChanged()`；但它只設 `currentChannel`、沒同步
`lastKnownChannel`，若當時 WiFi 仍是 `WL_CONNECTED`，下一次 `loop()` 呼叫
`onWifiChannelMayHaveChanged()` 仍會誤判成「AP 換頻」而寫入 NVS。
附加修正（見 final-fix-report.md）已補上 `lastKnownChannel` 的同步。

## Phase 2a 完成後的狀態

- master 能被 App 配網、連上 WiFi 與 MQTT、發布狀態、接受基本控制指令
- **ESP-NOW 心跳空窗控制在 slave 的 30 秒失聯門檻以下**（不是「完全不中斷」——
  `mqttClient.connect()` 是不可中斷的阻塞呼叫，最壞約 18 秒無心跳，
  精確的保證與殘存風險見 `ho_master1/readme.md`「ESP-NOW 心跳的實際保證」）
- channel 同步機制第一次面對真實情境（AP 決定 channel）
- **尚未有**：slave 的代發代訂閱、`slaves` 陣列、`ALL:*` 群組指令、OTA —— 那是 Phase 2b

## 已知風險

| 風險 | 影響 | 緩解 |
|---|---|---|
| BLE + WiFi + MQTT + ESP-NOW 並存的 heap 壓力 | 可能 OOM 重啟 | Task 6 Step 4 檢查 RAM 用量；BLE 僅在未配網時啟動且配網後 restart |
| WROOM 板 flash 用量偏緊（Task 6 實作後補記，計畫當初沒預見）| 後續 Phase 加程式碼可能編不過 | 已改用 `PartitionScheme=custom`（1.94MB app0，與 C3 共用 `partitions.csv`），但最終審查修正後用量達 82.8%（1,682,707 / 2,031,616），餘裕約 341KB；後續加功能前先跑 `.\flash.ps1 -Model master` 確認 |
| C3 單核跑滿四套協定的即時性 | 心跳延遲、封包遺失 | 回歸清單第 8、9 項專測此情境；必要時 master 改用 WROOM |
| `WiFi.begin()` 不掃描直接連，對隱藏 SSID 或特殊 AP 可能失敗 | 配網後連不上 | 保留 30 秒等待與原因碼診斷；若實測有問題再考慮首次連線才掃描（此時 ESP-NOW 尚未有 peer，掃描無害） |
| MQTT 狀態 JSON 加入 `slaves` 陣列後（2b）可能超過 1024 | publish 失敗 | 已 `setBufferSize(1024)`；2b 若 20 台會超過需再擴或分頁 |
