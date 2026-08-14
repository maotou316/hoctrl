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

- [ ] **Step 1: 加入全域與事件回調**

```cpp
// ── WiFi 狀態 ──
volatile uint8_t lastWifiDisconnectReason = 0;
unsigned long wifiConnectedTime = 0;
uint8_t lastKnownChannel = 0;     // 用於偵測 channel 變化

// WiFi 事件回調：取得底層斷線原因碼，並在取得 IP 時通知 slave 新 channel
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[WiFi] 斷線原因碼: %d\n", lastWifiDisconnectReason);
    // 常見：2=AUTH_EXPIRE 15=4WAY_HANDSHAKE_TIMEOUT 201=NO_AP_FOUND 202=AUTH_FAIL
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
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
// 與 ho_relay2 的差異見上方註釋。最壞阻塞約 30 秒，期間心跳照常發出。
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (!hasWifiConfig()) return;

  Serial.printf("[WiFi] 連線到 %s …\n", ssid);
  lastWifiDisconnectReason = 0;

  // 只斷開連線，不關閉 WiFi 驅動（ESP-NOW 要靠它活著）
  WiFi.disconnect(false);
  espNowDelay(100);

  WiFi.begin(ssid, password);

  // 等待最多 30 秒，期間持續發心跳
  const int maxWaitMs = 30000;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < maxWaitMs) {
    maintainEspNow();
    if (anyResetButtonPressed()) return;   // 讓長按重置仍可用
    delay(100);

    // 認證失敗類的原因碼不必等滿，直接放棄本次
    if (lastWifiDisconnectReason == 202 || lastWifiDisconnectReason == 15) {
      Serial.println("[WiFi] 認證失敗，放棄本次嘗試");
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectedTime = millis();
    Serial.printf("[WiFi] 已連線 IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    onWifiChannelMayHaveChanged();
  } else {
    Serial.printf("[WiFi] 連線失敗，狀態=%d 原因碼=%d\n",
                  WiFi.status(), lastWifiDisconnectReason);
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
    lastWiFiCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
      if (wifiPauseUntil != 0 && now < wifiPauseUntil) {
        // 連續失敗過多，暫停重試中
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

        if (WiFi.status() == WL_CONNECTED) {
          wifiFailCount = 0;
        } else if (wifiFailCount > 10) {
          wifiFailCount = 0;
          wifiPauseUntil = now + 60000;   // 暫停 60 秒再試
          Serial.println("[WiFi] 連續失敗過多，暫停 60 秒");
        }
      }
    } else {
      wifiFailCount = 0;
      onWifiChannelMayHaveChanged();   // AP 可能不斷線就換頻
    }
  }
```

- [ ] **Step 5: setup() 接上 WiFi**

在 `setup()` 的 `setupEspNow()` 與 `registerAllPeers()` **之後**加入：

```cpp
  WiFi.onEvent(onWiFiEvent);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);      // 禁用睡眠，避免 ESP-NOW 收包延遲

  if (hasWifiConfig()) {
    connectToWiFi();
  }
```

**順序很重要**：ESP-NOW 必須先 init，WiFi 才連線。反過來會讓 `esp_now_init()` 在
STA 已連線的狀態下執行，peer 的 channel 跟隨行為可能不如預期。

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
// 依序嘗試：自訂伺服器 → 從上次成功的位置輪詢預設清單
void smartConnect() {
  if (!WiFi.isConnected()) return;

  if (useCustomServer && strlen(mqttServer) > 0) {
    if (quickConnectCustom()) return;
  }

  for (int i = 0; i < DEFAULT_SERVER_COUNT; i++) {
    int index = (currentServerIndex + i) % DEFAULT_SERVER_COUNT;
    if (quickConnectToIndex(index)) return;
    espNowDelay(300);   // 錯開重試，期間維持心跳
  }

  currentServerIndex = (currentServerIndex + 1) % DEFAULT_SERVER_COUNT;
  Serial.println("[MQTT] 所有伺服器都連不上");
}
```

- [ ] **Step 3: loop() 的 MQTT 管理**

```cpp
  // ── MQTT 連線管理 ──
  static unsigned long lastReconnect = 0;
  static unsigned long lastStatusPub = 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (now - lastReconnect > 10000) {
        lastReconnect = now;
        smartConnect();
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

  char buf[512];
  size_t n = serializeJson(doc, buf);
  bool ok = mqttClient.publish(topic.c_str(), buf, true);
  if (!ok) {
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

## Phase 2a 完成後的狀態

- master 能被 App 配網、連上 WiFi 與 MQTT、發布狀態、接受基本控制指令
- **ESP-NOW 全程不中斷**，slave 在配網、WiFi 重連、MQTT 切換期間都不會失聯
- channel 同步機制第一次面對真實情境（AP 決定 channel）
- **尚未有**：slave 的代發代訂閱、`slaves` 陣列、`ALL:*` 群組指令、OTA —— 那是 Phase 2b

## 已知風險

| 風險 | 影響 | 緩解 |
|---|---|---|
| BLE + WiFi + MQTT + ESP-NOW 並存的 heap 壓力 | 可能 OOM 重啟 | Task 6 Step 4 檢查 RAM 用量；BLE 僅在未配網時啟動且配網後 restart |
| C3 單核跑滿四套協定的即時性 | 心跳延遲、封包遺失 | 回歸清單第 8、9 項專測此情境；必要時 master 改用 WROOM |
| `WiFi.begin()` 不掃描直接連，對隱藏 SSID 或特殊 AP 可能失敗 | 配網後連不上 | 保留 30 秒等待與原因碼診斷；若實測有問題再考慮首次連線才掃描（此時 ESP-NOW 尚未有 peer，掃描無害） |
| MQTT 狀態 JSON 加入 `slaves` 陣列後（2b）可能超過 1024 | publish 失敗 | 已 `setBufferSize(1024)`；2b 若 20 台會超過需再擴或分頁 |
