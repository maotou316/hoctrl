// 齁控 Master — ESP-NOW 主控端
// 硬體：ESP32 WROOM DevKit 或 ESP32-C3（GPIO 依晶片條件編譯，詳見下方 GPIO 定義區塊）
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HoEspNowProtocol.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ── ArduinoJson 版本注意事項 ──
// 本專案安裝的是 ArduinoJson 7.4.3。這個版本的 StaticJsonDocument<N> 只是
// compatibility.hpp 提供的相容殼（class StaticJsonDocument : public JsonDocument），
// 模板參數 N 完全被忽略、底層一律動態配置，且已標記 ARDUINOJSON_DEPRECATED。
// 也就是說「把 N 從 512 改成 2048」對容量沒有任何作用 ——
// 真正會截斷的地方一直是 serializeJson(doc, buf) 的那個固定大小 char buf。
// 因此全檔改用 JsonDocument，容量控制一律交給 publishJsonDoc() 的 measureJson() 實測。
// CLAUDE.md 記載的「用 StaticJsonDocument 避免記憶體碎片」在 7.x 已不成立。

const char* firmwareVersion = "1.0.0";
const char* deviceModel = "hoMaster1";

// ── GPIO（用條件編譯讓同一份 sketch 同時支援 ESP32 WROOM 與 ESP32-C3）──
// 為什麼不複製第二個 sketch：本專案已有 ho_relay1/2/3 三份高度重複程式碼的教訓
//（ho_relay3.ino 的 deviceModel 至今還誤寫成 hoRelay2，就是複製後沒同步的後果）。
// master 之後還有 Phase 2~5 要改，複製出 master-c3 版只會讓每次改動都要同步兩處。
// ESP32 Arduino core 3.x 會依燒錄目標自動定義 CONFIG_IDF_TARGET_ESP32C3 或
// CONFIG_IDF_TARGET_ESP32（已用 #error 實測確認兩種 FQBN 各自命中對應分支）。
#if defined(CONFIG_IDF_TARGET_ESP32C3)
// ESP32-C3 版：GPIO 對齊 ho_slave1.ino（同一塊硬體），沿用其命名習慣
const int bootButton = 9;
const int secondButton = 1;    // C3 版對應的是實體 RESET 按鈕，非備用腳
const int ledPins[] = { 3, 0 };  // 板載 LED(3) ＋ 面板 LED(0)，兩顆需同步驅動
// C3 的 GPIO 4/7 是 JTAG 腳（MTMS/MTDO），reset 後由 ROM 配置、不保證低電位，
// 上電到第一行使用者程式之間的空窗韌體管不到，會繼承跟 ho_relay2 一樣的
// 「開機瞬間繼電器短暫通電」硬體限制，需硬體在 MOS gate 對地加 10kΩ 下拉才能根治。
// 詳見 ho_relay2/readme.md「已知硬體限制」章節。
const int relayPins[] = { 4, 7 };
#elif defined(CONFIG_IDF_TARGET_ESP32)
// ESP32 WROOM 版：與 ho_relay1 一致
const int bootButton = 0;
const int secondButton = 14;   // 目前未實際接線，僅供按鈕自檢與 Phase 2 預留功能用
const int ledPins[] = { 2 };   // 只有板載 LED，GPIO 13 不是 JTAG 腳，沒有 C3 那個開機通電風險
const int relayPins[] = { 13 };
#else
#error "未知的目標晶片，master 目前只支援 esp32 或 esp32c3 這兩種 FQBN"
#endif
const int ledPinCount = sizeof(ledPins) / sizeof(ledPins[0]);
const int relayPinCount = sizeof(relayPins) / sizeof(relayPins[0]);

// ── 全域狀態 ──
bool relayState = false;
bool pairingMode = false;
uint8_t currentChannel = 1;
bool longRangeEnabled = false;
String deviceIdString;
uint16_t txSeq = 0;

// ── 指令識別碼配發（協定版本 2 的指令歸因）──
//
// 一道**邏輯指令**一個 cmdId。同一道指令的 3 次廣播、每台的單播、以及第 2、3 趟
// 補送全部共用同一個值（見 sendCmdToAll()），slave 執行後原樣回填到 HoStatePayload，
// master 才能把「那台的 relay 值」歸因回「我下的那一道」。
//
// **為什麼開機初值是亂數而不是 0**：master 重開機後 cmdId 會從頭再來一遍。
// 若固定從 1 起算，一則重開機前送出、重開機後才被 master 收到的舊 HO_PKT_STATE
// （或被錄下來重播的），就可能帶著與新指令相同的 cmdId 而被誤認成執行證明 ——
// 誤綠方向。亂數初值把它變成 1/65535 的巧合，再加上 groupExecutedIdx() 的
// 「回報時間必須晚於指令送出時間」，兩個條件要同時成立才會出錯。
// **它擋不住什麼**：擋不住指令送出後才被重播的那則舊回報（時間條件會成立），
// 也擋不住 65535 道指令之後的自然繞回。要真正擋住得上 nonce／簽章，不在本階段範圍。
uint16_t nextCmdId = HO_CMD_ID_NONE;

// 配發下一個 cmdId。**必定跳過 HO_CMD_ID_NONE（0）** ——
// 0 在協定裡的語義是「slave 開機以來沒執行過任何指令」，配發出去就會讓
// 「沒執行過」與「執行過這一道」分不出來。
uint16_t allocCmdId() {
  nextCmdId++;
  if (nextCmdId == HO_CMD_ID_NONE) nextCmdId = 1;
  return nextCmdId;
}

// ── Slave 名冊 ──
struct SlaveEntry {
  uint8_t mac[6];
  bool online;
  int8_t rssi;
  unsigned long lastSeen;
  // ── Phase 2b 新增：代發 slave 狀態需要的欄位 ──
  // 這些值由 onEspNowRecv() 收到 HO_PKT_STATE 時填入（WiFi task context），
  // 由 loop() 讀取來組 JSON。跨 context 但都是單一 byte 的存取，
  // 讀到新舊混合的一組值最多讓某一輪代發的資料稍舊，下一輪就會更正，
  // 不會造成錯誤動作，因此沿用 Phase 1 對 slaves[] 的既有做法不加鎖。
  uint8_t relay;      // 0 / 1
  uint8_t fwMajor;
  uint8_t fwMinor;
  uint8_t fwPatch;
  // ── Phase 4 Task 1 新增：指令歸因（協定版本 2）──
  // 由 onEspNowRecv() 的 HO_PKT_STATE 分支填入（WiFi task），loop() 讀。
  // lastCmdAt 是「master 收到這則回報的本機時刻」，不是 slave 的時刻 ——
  // 兩顆板子的 millis() 沒有共同時基，只有本機時刻能拿來跟 groupJob.startedAt 比。
  uint16_t lastCmdId;         // slave 回報的 HoStatePayload::lastCmdId
  uint8_t  lastCmdKind;       // 那道指令的 HoRelayCmd
  uint8_t  lastCmdCount;      // slave 執行同一個 cmdId 的次數
  unsigned long lastCmdAt;    // 收到「帶著這個 lastCmdId 的第一則回報」的本機時刻
  // 有變化就代發，不必等輪播輪到它（見 Task 3 的排程器）
  bool dirty;
  // ── Task 3 review 修正：dirty 連續發布失敗要退避 ──
  // 少了這兩個欄位，一台持續發布失敗的 slave（例如剛好 mqttClient.publish()
  // 真的失敗，不是名額禮讓）會讓 slaveStatusScheduler() 每次都優先發現它
  // dirty、每次都重試同一台，rotateIdx 永遠推進不到其他台，等於把其他
  // slave 的代發餓死。連續失敗滿 SLAVE_DIRTY_MAX_FAIL 次後，在
  // dirtyBackoffUntil 之前先跳過這台、讓其他台優先，見 publishSlaveStatus()
  // 與 slaveStatusScheduler() 的實作。
  uint8_t dirtyFailCount;
  unsigned long dirtyBackoffUntil;
};

SlaveEntry slaves[HO_ESPNOW_MAX_SLAVES];
// slaveCount 會被 ESP-NOW callback（WiFi task）寫入、sendHeartbeat()（主 task）讀取，
// 屬跨 context 存取，加 volatile 避免編譯器快取舊值
volatile int slaveCount = 0;

// slave 主動解除配對是在 WiFi task 收到的（onEspNowRecv() 的 HO_PKT_UNPAIR 分支），
// 不能在那裡直接發 MQTT ——PubSubClient／同一顆 socket 跨 task 存取是明確的競態，
// 理由與 slaveCount／SlaveEntry.dirty 的跨 context 存取相同。只能記下 MAC，
// 交給 loop() 呼叫的 processPendingUnpairPublish() 補發最後一則 offline。
uint8_t pendingOfflineMac[6];
volatile bool hasPendingOfflinePublish = false;

// ── Task 4：名冊變動後要重新對齊 control topic 訂閱 ──
// addSlave() 由 onEspNowRecv() 呼叫，屬 WiFi task context；mqttClient.subscribe()
// 會動 socket，與 loop() 裡的 mqttClient.loop() 是明確的競態（理由與
// pendingOfflineMac 那組旗標完全相同）。因此配對成功只在這裡插一支旗子，
// 真正的訂閱動作一律回到 loop() context 才做（見 loop() 的
// pendingSubscribeRefresh 區塊）。重複訂閱同一個 topic 對 broker 是冪等的，
// 所以直接重跑 subscribeAllControlTopics() 全量對齊即可，不必記錄是哪一台。
volatile bool pendingSubscribeRefresh = false;

// ── Task 3 review 修正（Critical）：每輪 loop() 只允許一次會阻塞的 MQTT publish ──
// mqttClient.publish() 內部的 NetworkClient::write() 卡住時最壞吃 10 秒（不是
// PubSubClient::setSocketTimeout(3) 的 3 秒——那個值只用在等待 CONNACK 與
// readByte()，publish 完全不經過它，詳見 publishJsonDoc() 上方的完整說明）。
// **注意（Task 3 review N1 更正）**：這個 10 秒是 10 次重試 × 1 秒 select 的
// 名目值，**不是硬上限** —— NetworkClient::write() 在部分寫入成功時會把重試
// 計數器重置回 10（NetworkClient.cpp 的 `retry = WIFI_CLIENT_MAX_WRITE_RETRY;`
// 就在 `res > 0` 但尚未寫完的分支裡），所以「每次只擠得出幾個 byte」的病態
// socket 理論上可以把單次 write 拖得更久。10 秒是典型上界，不是保證。
// 若同一輪 loop() 疊了兩、三次阻塞 publish（例如 publishStatus() 接
// slaveStatusScheduler() 接 processPendingUnpairPublish()），心跳空窗會累加成
//
// **Task 4 review（M1）擴大語義**：這個名額不再只管 publish，而是管「本輪
// loop() 的**阻塞式 socket 寫入**」，涵蓋 mqttClient.publish()／subscribe()／
// unsubscribe() 三者 —— 它們走的都是同一條 NetworkClient::write()，都是同一個
// 10 秒級黑箱。旗標名稱維持 Task 3 取的 mqttPublishBudgetUsed，避免跨文件
// （progress.md、歷次 review）的指涉斷裂。取用點：publishJsonDoc()、
// controlSubscribeScheduler()、unsubscribeSlaveControlTopic()。
//
// 20~30 秒，直接撞上 slave 的 30 秒失聯門檻。這個旗標由 loop() 每輪開頭重置，
// publishJsonDoc() 真正要送出封包前會佔用它；本輪已經用掉的話，其餘想發布的
// 呼叫方（見 slaveStatusScheduler()／processPendingUnpairPublish() 開頭的檢查）
// 一律讓位給下一輪——下一輪開頭就是 maintainEspNow() 補心跳。
bool mqttPublishBudgetUsed = false;

Preferences prefs;

// ── Phase 2b review 修正：fakeslaves 容量測試工具的 NVS 安全網 ──
// fakeSlavesForCapacityTest() 本身只寫記憶體，但灌入假資料後若在同一次開機期間
// 執行真正的配對（onEspNowRecv() 的 HO_PKT_PAIR_REQ → addSlave()）或解除配對
// （unpairSlave()），兩者都會呼叫 saveSlaves()，殘留的假 MAC 會被一併寫進
// NVS，汙染真實名冊。這個旗標在 fakeSlavesForCapacityTest() 設為 true 後，
// saveSlaves() 開頭就會擋下所有寫入，不必逐一修改 addSlave()／unpairSlave()。
// 只能靠重開機清除（與假名冊本身「重開機即消失」的語義一致），故意不提供
// 序列埠指令解除，避免使用者忘記假資料還在就手動解鎖。
bool fakeSlavesActive = false;

// ── slave 目前鎖定的 channel（存 homaster 命名空間，與名冊同生共死）──
// 與下方 lastApChannel 是兩個不同用途的值，刻意分開存在兩個命名空間：
// - lastApChannel（hoban／apch）服務「WiFi 關聯」：下次要用哪個 channel 關聯 AP。
//   clearNetConfig() 會清掉它，這是對的 —— 連 SSID 都沒了，這個提示反而會害下次
//   配了新 AP 之後，還一直在舊 channel 上白試十次才肯全頻掃描。
// - slaveLockChannel（homaster／espch）服務「ESP-NOW 心跳」：名冊上的 slave 目前
//   鎖在哪個 channel。它必須撐過 reset —— Critical 2 的情境正是「送 reset 後重開機
//   進 BLE 配網」，若這個值跟著網路設定一起被清掉，master 仍會停在 channel 1，
//   整段配網過程 slave 照樣失聯關籠，等於沒修。它跟著名冊走，名冊清空時自然失效
//   （restoreEspNowChannelForOfflineBoot() 會檢查 slaveCount）。
uint8_t slaveLockChannel = 0;
uint8_t savedSlaveLockChannel = 0;   // NVS 現值，避免沒變也重複寫入磨損 flash

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

// ── 上次成功關聯的 AP channel／BSSID ──
// （宣告放在這裡而非 WiFi 區塊，因為 loadNetConfig()／saveNetConfig() 要用到）
// 重連時可直接指定 channel／BSSID，跳過 WiFi.begin() 內建的全頻道掃描
//（review 發現：即使不呼叫 WiFi.scanNetworks()，WiFi.begin(ssid, password)
// 不帶 channel/BSSID 時，ESP-IDF 底層關聯流程仍會自己全頻道掃一輪，約 1.5 秒，
// 失敗後會立刻再掃，對 ESP-NOW 心跳命中率殺傷力等同顯式呼叫 scanNetworks()）。
//
// lastApChannel 有寫進 NVS，lastApBssid 沒有。兩者用途不同：
// - BSSID 只服務「WiFi 關聯」，開機第一次本來就沒有歷史資料可用，存了也不保證有效
//   （AP 可能已換機、換 BSSID），所以只存 RAM。
// - channel 還服務「ESP-NOW 心跳」。開機後若不會關聯 WiFi（例如被 reset 清掉設定、
//   進入 BLE 配網模式），setupEspNow() 的 WiFi.mode(WIFI_STA) 會把 channel 歸 1，
//   而名冊上的 slave 仍鎖在舊 channel，整個配網過程（可能數分鐘）都收不到心跳、
//   30 秒後失聯強制關閉繼電器＝籠子被打開。存 NVS 才能在開機時先切回舊 channel。
// clearNetConfig() 會連同 apch 一起清掉，這是刻意的：連 SSID 都沒了，channel 也失去意義。
uint8_t lastApChannel = 0;
uint8_t lastApBssid[6] = { 0 };
bool haveLastApBssid = false;
uint8_t savedApChannel = 0;   // NVS 裡目前的值，用來避免沒變也重複寫入磨損 flash

// ── MQTT 多伺服器設定 ──
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

// 目前連上的是自訂伺服器還是 DEFAULT_SERVERS[currentServerIndex]。
// currentServerIndex 刻意不追蹤自訂伺服器（見 quickConnectCustom() 註釋），
// 所以要另外一個旗標才知道 server 欄位該填哪個名字。
bool usingCustomServer = false;

const char* currentServerName() {
  if (usingCustomServer) return mqttServer;
  if (currentServerIndex >= 0 && currentServerIndex < DEFAULT_SERVER_COUNT) {
    return DEFAULT_SERVERS[currentServerIndex].server;
  }
  return "";
}

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ── 狀態 JSON 的容量預算 ──
// 血淚背景（Phase 2a 最終審查的結論）：
//   serializeJson(doc, buf) 寫進固定大小的 char buf 時會「靜默截斷」——
//   截斷後的長度仍然小於 mqttClient 的 buffer，publish() 照樣回傳 true，
//   於是 broker 收到語法殘缺的 JSON、App 解析失敗，而序列埠上什麼異常都看不到。
//   Phase 2a 的 master 狀態最壞 317 bytes，對 char buf[512] 只剩約 195 bytes 餘裕，
//   而 Phase 2b 要加的 20 台 slaves 陣列需要 1900+ bytes，必定超過。
//
// 所以本階段的容量控制是三層，且第一層在編譯期：
//   1. static_assert：statusBuf 扣掉基礎欄位上界後，必須放得下 20 台的陣列。
//      有人把 STATUS_BUF_SIZE 改小 → 編譯失敗，而不是上線後靜默截斷。
//   2. 執行期 maxEntries 上限 + slaves_truncated 標記（照現在的數值永遠走不到，
//      存在的意義是「萬一走到，App 與序列埠都看得見」）。
//   3. publishJsonDoc() 發布前 measureJson()，放不下就整包不發並印出實際需求。
//      寧可不發，也絕不發半截 JSON。

// 單筆 slave 條目的位元組上界。最壞情況實算：
//   {"id":"hoban-aabbccddeeff","relay":1,"online":false,"rssi":-100,"version":"255.255.255",
//    "grp":0,"exe":0},
//   = 89 ＋ 8（Task 5 review M2 的 "grp"，`,"grp":0`）
//        ＋ 8（Phase 4 Task 1 的 "exe"，`,"exe":0`）＝ 105 bytes（含尾端逗號）。
//   取 112 留餘裕，並讓除法算式仍是整數。
//   （沿革：96（Phase 2b Task 1）→ 104（Task 5 的 "grp"）→ 112（Phase 4 的 "exe"）。
//    每一次都是「加了欄位就把常數重算」，這是下方註釋要求的動作。
//    ⚠ plan 決定 4.1 原本寫「slaves[] 的欄位在本階段一個字都不准動」，
//    本 Task 為了讓執行證明能逐台送到 App 而動了它 —— 因此**同一次把預算整個
//    重排並放大 buf**，而不是把餘裕壓到零留給 Task 5 去撞。）
//
// review 補充（實測數字比對）：實機用 fakeslaves 20 量到的「餘裕 971 bytes」是樂觀值——
// 那台測試板 SSID 短、沒設自訂 MQTT 伺服器，基礎欄位只吃了 310 bytes，遠低於
// STATUS_BASE_MAX_BYTES 的預算。static_assert 真正依賴、且必須成立的是用這個
// 常數乘上 HO_ESPNOW_MAX_SLAVES 算出的悲觀上界。
//
// **Phase 4 Task 1 更新（"exe" 欄位 ＋ ota 預算 ＋ 放大 buf）：**
// 現行常數是 STATUS_BUF_SIZE=3584、STATUS_BASE_MAX_BYTES=728（480+120+128）、
// SLAVE_ENTRY_MAX_BYTES=112，
// 執行期上限 maxEntries = (3584-1-728-11)/112 = 2844/112 = **25**
//（沿革：512／96 → 26；Task 5 的 "grp" → 640／104 → 23；
//  Phase 4 的 "exe" → 640／112 → 21；本次把 ota 的 128 誠實列進預算並放大 buf → 25）。
//   - 規格上限 20 台的悲觀值：728+11+20×112 = 2979，對 statusBuf[3584] 餘裕 604
//   - 25 台（極限）：728+11+25×112 = 3539，餘裕只剩 44
//   - 26 台：3651 > 3583，static_assert 會直接編譯失敗
//
// **日後若在 SlaveEntry／appendSlavesArray() 新增欄位，務必重新實算這個上界並
// 更新這個常數**——static_assert 比較的只是這個常數本身，常數沒跟著新欄位變大，
// 編譯期完全檢查不出「單筆條目其實已經超過 112 bytes」，那點邊際
// 會在不知不覺間被吃光，直到現場真的湊到極限台數才會發布時被 publishJsonDoc()
// 的防線 1 擋下（不是編譯期，是執行期放棄發布 —— 會印一行，不是靜默）。
const size_t SLAVE_ENTRY_MAX_BYTES = 112;

// ── slaves 陣列以外所有欄位的位元組上界（Phase 4 Task 1 改成「分項相加」）──
//
// 改成分項的理由（plan 決定 4.2 明文要求）：舊寫法是一個 640 的魔術數字，
// 註釋裡的分項只是說明、不是程式碼。於是 Task 5 把 "group" 物件塞進去、
// Phase 4 又要塞 "ota" 物件時，**沒有任何機制會發現 640 已經被吃光** ——
// static_assert 照樣通過，保護看起來還在、實際上已經失效。
// 拆成具名常數之後，未來新增區塊的人被迫也新增一個具名常數並相加。
//
// (a) group／ota 以外的既有欄位，逐項實算：
//   Phase 2a 的既有欄位最壞                        317
//   + "server":"<最長 63 字元的自訂伺服器>",         75
//   + "free_heap":123456,                          19
//   + "slaves_truncated":true,"slaves_shown":20,   42
//   + "long_range_pending":true,                   27（Task 6 加）
//   = 480（剛好整數，不再取整）
const size_t STATUS_BASE_WITHOUT_GROUP_OTA_MAX_BYTES = 480;

// (b) "group" 物件的最壞實算（appendGroupResult()），逐欄位相加：
//   `"group":{` 9 ＋ `"cmd":255,` 10 ＋ `"age_s":4294967,` 16 ＋ `"busy":1,` 9
//   ＋ `"n":20,` 7 ＋ `"ack":20,` 9 ＋ `"noack":20,` 11 ＋ `"gone":20,` 10
//   ＋ `"exed":20,` 10 ＋ `"exec":"attributed"` 19 ＋ `},` 2
//   = 112 bytes，取 120。
//
//   （沿革：Task 5 review M2 這段寫「94，取 96」時**漏算了 busy 欄位**，
//    實際當時已是 102 > 96。整包沒爆是因為 640 的總預算本身有餘裕 ——
//    那是運氣不是設計。Phase 4 Task 1 一併算對。
//    Phase 4 Task 1 曾短暫加過一個 `"cid":65535,`（12 bytes），
//    後來因為 review M1 又移除，見 appendGroupResult() 上方的說明。）
const size_t STATUS_GROUP_MAX_BYTES = 120;

// (c) "ota" 物件的上界。**這是預留額度，目前沒有任何程式碼會發出這個物件** ——
// Task 5 才會實作。預留而不是等到那時再算，是因為「等到那時」正是 (a) 的
// 640 被吃光卻沒人發現的成因。逐項實算取自 plan 決定 4.2：
//   `"ota":{` 7 ＋ `"target":"hoban-aabbccddeeff",` 30 ＋ `"phase":"<最長 12 字元>",` 23
//   ＋ `"progress":100,` 15 ＋ `"size":2031616,` 15 ＋ `"error":"<最長 16 字元>"` 26
//   ＋ `},` 2 = 118，取 128。
//
// **它擋不住什麼**：這只是「預算保留」，不是對 Task 5 實作的檢查。
// 若 Task 5 的 phase／error 字串超過 12／16 字元，這個常數不會自己變大，
// static_assert 也抓不到 —— plan 決定 4.2 因此要求那兩個字串必須走查表函式、
// 不得是自由格式 String，並用 `fakeota` + `jsonsize` 實測。
const size_t STATUS_OTA_MAX_BYTES = 128;

const size_t STATUS_BASE_MAX_BYTES =
    STATUS_BASE_WITHOUT_GROUP_OTA_MAX_BYTES + STATUS_GROUP_MAX_BYTES
    + STATUS_OTA_MAX_BYTES;   // 480 + 120 + 128 = 728

// "slaves":[] 這個 key 與中括號本身
const size_t SLAVES_KEY_OVERHEAD = 11;

// ── 序列化緩衝區（Phase 4 Task 1 由 3072 放大到 3584）──
//
// 為什麼非放大不可：把 ota 的 128 bytes 誠實列進預算之後，
// (3072-1-728-11)/112 = 2332/112 = 20，**恰好等於 HO_ESPNOW_MAX_SLAVES 而餘裕歸零**。
// 餘裕歸零代表「下一個欄位、甚至下一次重算發現少估了幾個 byte」就會編不過或截斷，
// 而在這個檔案的歷史上，少估幾個 byte 已經發生過兩次（Task 5 的 96→97、
// 以及同一個 commit 漏算 busy）。**靠壓縮餘裕過關正是靜默截斷缺陷回來的路徑。**
// 3072 → 3584（+512）之後 (3584-1-728-11)/112 = 2844/112 = 25，餘裕回到 5 台，
// 與 plan 決定 4.2 原本預期的「餘裕 5 台」一致。
//
// 代價：statusBuf 是 .bss，+512 bytes；MQTT buffer 是 heap，+512 bytes。
// master 的 RAM 用量約 20%，這點增量不影響任何既有邊界。
const size_t STATUS_BUF_SIZE = 3584;

// PubSubClient 的 buffer 要放得下「固定標頭(最多 5) + topic 長度欄位(2) + topic + payload」。
// topic 最長是 "hoban/hoban-a0b1c2d3e4f5/status" = 31 bytes。
// 3584 + 5 + 2 + 31 = 3622，取 3840 留餘裕。
//
// ⚠ 這個數字是回歸清單的判準（docs/phase2b-regression-checklist.md 第 3 項），
// **而且它有兩個合法值**：本次開機沒嘗試過 MQTT 連線時，setBufferSize() 根本
// 沒被呼叫過，buffer 會停在 PubSubClient 建構子給的 MQTT_MAX_PACKET_SIZE ＝ 256。
// 改這個常數就必須同步改那份清單的兩種變體。
const size_t MQTT_BUFFER_SIZE = 3840;

// 編譯期保證：statusBuf 一定放得下 HO_ESPNOW_MAX_SLAVES 台的完整陣列。
static_assert(
    (STATUS_BUF_SIZE - 1 - STATUS_BASE_MAX_BYTES - SLAVES_KEY_OVERHEAD)
        / SLAVE_ENTRY_MAX_BYTES >= HO_ESPNOW_MAX_SLAVES,
    "STATUS_BUF_SIZE 放不下 HO_ESPNOW_MAX_SLAVES 台 slave 的陣列，"
    "請放大 STATUS_BUF_SIZE，或縮減 STATUS_BASE 的三個分項之一"
    "（WITHOUT_GROUP_OTA／GROUP／OTA）");

// 序列化用的共用緩衝區。刻意放在檔案層級（.bss）而非函式內的區域變數：
// loopTask 的堆疊只有 8192 bytes，在裡面開 3072 bytes 的區域陣列，
// 加上 PubSubClient 與 lwIP 的呼叫深度，堆疊溢位風險太高。
// 只在 loop() context（含 mqttClient.loop() 內被呼叫的 mqttCallback）使用，
// 單一 task、不重入，共用一份是安全的。
static char statusBuf[STATUS_BUF_SIZE];

// 配對模式
unsigned long pairingStartTime = 0;
const unsigned long PAIRING_TIMEOUT = 60000;  // 60 秒
// slave 超過這麼久沒回應就標記離線
const unsigned long SLAVE_OFFLINE_TIMEOUT = 30000;

const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
// 心跳間隔。必須「明顯小於 slave 每個 channel 的停留時間（SCAN_DWELL_MS = 1200ms）」，
// slave 輪掃時才能保證每停留一個 channel 至少涵蓋一次心跳、一輪內必定命中。
// 舊值 5000ms 大於 dwell，命中變成兩個週期的機率問題（約 7.7%），
// 期望要十幾次心跳、約一分鐘才鎖得回來。廣播封包只有 11 bytes，加快到 1 秒的流量代價可忽略。
const unsigned long HEARTBEAT_INTERVAL = 1000;

// WiFi 關聯進行中的加密心跳間隔。關聯期間 master 有機會離開 slave 鎖定的 channel
//（連續關聯失敗達 WIFI_CHANNEL_LOCK_MAX_FAIL 次時會升級成一次全頻掃描），
// 停在舊 channel 的 slave 每則心跳命中率只剩約 1/13。1000ms 間隔下 30 秒只有 30 則，
// 全數落空的機率約 9%，而落空的後果是 slave 強制關閉繼電器＝籠子被打開。
// 加密到 200ms 後 30 秒有 150 則，全數落空機率降到 6×10⁻⁶。
// 廣播封包只有 11 bytes，代價可忽略 —— 與當初把 HEARTBEAT_INTERVAL 由 5000 降到 1000
// 的理由完全同源。
const unsigned long HEARTBEAT_INTERVAL_ASSOC = 200;

// channel 改變時立刻連發數次心跳，讓正在輪掃的 slave 更快命中
const int HEARTBEAT_BURST_COUNT = 4;
const int HEARTBEAT_BURST_GAP = 200;

// 心跳「印出序列埠」的降頻倍數（發送頻率不受影響，仍是每 1 秒一次）。
// 每 10 次印一行 ≈ 每 10 秒一行，狀態有變化時仍會立即印，詳見 sendHeartbeat()
const int HEARTBEAT_LOG_EVERY = 10;

// ── 開機按鈕自檢（移植自 ho_relay2.ino）──
// 按鈕接法是 GPIO ──[按鈕]── GND，靠 INPUT_PULLUP 拉高；某支腳一旦短路或走線接地
// 就恆為 LOW，會被按鈕狀態機誤判成「使用者一直按著」。
// master 除了短按進配對（風險低），現已補上長按重置（見 updateResetButton()），
// 同樣會繼承 hoRelay2 那個「開機即清設定 → 重啟 → 再清除」的無限迴圈缺陷，
// 這裡的防呆正是為它而設。
const unsigned long BTN_SELFTEST_DURATION = 500;  // 自檢取樣總長度 (毫秒)
const unsigned long BTN_SELFTEST_INTERVAL = 50;   // 取樣間隔 (毫秒)
bool bootButtonUsable = true;                     // BOOT 按鈕是否可用
bool secondButtonUsable = true;                   // 第二按鈕是否可用

// ── 長按重置參數（移植自 ho_relay2.ino，數值完全一致）──
const unsigned long LONG_PRESS_TIME = 3000;    // 長按 3 秒進入閃爍確認階段
const unsigned long BLINK_CONFIRM_TIME = 2000; // 閃爍確認階段再按住 2 秒才清除設定
const unsigned long BLINK_INTERVAL = 250;      // 確認階段 LED 閃爍週期 (毫秒，亮/滅各半)
const unsigned long CONFIRM_SOLID_TIME = 700;  // 快閃結束後長亮 0.7 秒表示確認重置

// ── LED（WROOM 只有板載 LED 一顆；C3 另有面板 LED，兩顆須同步驅動）──
// 用 ledPins[]／ledPinCount 迴圈寫，而非寫死單一顆，理由與繼電器的 relayPins[] 一致：
// 條件編譯只需改陣列內容，這兩個函式在兩種板子配置下都不用另外分支。
void initLeds() {
  for (int i = 0; i < ledPinCount; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }
}

void setLeds(bool on) {
  for (int i = 0; i < ledPinCount; i++) {
    digitalWrite(ledPins[i], on ? HIGH : LOW);
  }
}

// ── 繼電器 ──
// 與 ho_relay2 相同的鐵則：initRelayPins() 必須是 setup() 第一行
void initRelayPins() {
  for (int i = 0; i < relayPinCount; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }
  relayState = false;
}

// 用「起始時間＋持續時間」搭配無號數減法比較，而非「結束時間」搭配絕對值比較，
// 避免 millis() 約 49.7 天溢位時，迴繞後的極小值被誤判為「時間已到」而提前關閉繼電器
// （宣告必須早於 setRelayPins()，因為後者要清除 pulseActive）
unsigned long pulseStartTime = 0;
uint16_t pulseDuration = 0;
bool pulseActive = false;

void setRelayPins(bool on) {
  for (int i = 0; i < relayPinCount; i++) {
    digitalWrite(relayPins[i], on ? HIGH : LOW);
  }
  relayState = on;
  // 明確的持續性 ON／OFF 指令必須撤銷進行中的點動計時。
  // 少了這行的實際後果：MQTT 送 ON（點動 2 秒）後 1 秒內從序列埠下 allon，
  // 繼電器先被 allon 設成持續開啟，但點動計時仍在跑，1 秒後逾時把它關掉，
  // 等於使用者的 allon 被無聲撤銷 ——「命令保持開啟，卻自己關上」對籠門機構是危險行為。
  // pulseRelay() 呼叫本函式之後才設 pulseActive = true，順序上不會被這行清掉。
  pulseActive = false;
}

void pulseRelay(uint16_t ms) {
  setRelayPins(true);
  pulseStartTime = millis();
  pulseDuration = ms;
  pulseActive = true;
}

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
  lastApChannel = netPrefs.getUChar("apch", 0);
  netPrefs.end();

  if (mqttPort <= 0 || mqttPort > 65535) mqttPort = 1883;
  if (lastApChannel > 13) lastApChannel = 0;   // 髒資料防呆
  savedApChannel = lastApChannel;

  Serial.printf("[設定] SSID=%s 自訂伺服器=%s 繼電器=%s 上次AP channel=%u\n",
                strlen(ssid) > 0 ? ssid : "(未設定)",
                useCustomServer ? "是" : "否",
                hasRelay ? "有" : "無",
                lastApChannel);
}

// 只寫 channel 這一個鍵：連線成功時呼叫，值沒變就不寫，避免每次重連都磨損 flash
void saveApChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return;
  if (ch == savedApChannel) return;
  netPrefs.begin("hoban", false);
  netPrefs.putUChar("apch", ch);
  netPrefs.end();
  savedApChannel = ch;
  Serial.printf("[設定] 已記住 AP channel=%u（供下次開機在 BLE 配網模式維持心跳用）\n", ch);
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
  netPrefs.putUChar("apch", lastApChannel);
  netPrefs.end();
  savedApChannel = lastApChannel;
  Serial.println("[設定] 已儲存到 NVS");
}

void clearNetConfig() {
  netPrefs.begin("hoban", false);
  netPrefs.clear();   // 連 apch 一起清掉：連 SSID 都沒了，記住的 AP channel 也失去意義
  netPrefs.end();
  savedApChannel = 0;
  Serial.println("[設定] NVS 網路設定已清除");
}

bool hasWifiConfig() {
  return strlen(ssid) > 0;
}

// ── BLE 配網 ──
// UUID 必須與 ho_relay2 完全一致，否則現有 Flutter App 找不到設備
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool bleConfigMode = false;   // 是否處於 BLE 配網模式（開機時沒有 WiFi 設定才會開啟）

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* srv) override {
    deviceConnected = true;
    Serial.println("[BLE] App 已連線");
  }
  void onDisconnect(BLEServer* srv) override {
    deviceConnected = false;
    Serial.println("[BLE] App 已斷線，重新開始廣播");
    // ho_relay2 的 MyServerCallbacks::onDisconnect 沒有這行，導致 App 斷線後
    // 第二次連不上，必須重開機才能再次配對，master 補上這個缺口。
    BLEDevice::startAdvertising();
  }
};

// BLE 收到設定資料時的回調。
// JSON 欄位路徑必須與 ho_relay2 MyCallbacks::onWrite 的實際程式碼一致
//（server/port/帳密全部在 wifi 物件底下，非頂層 mqtt 物件），否則現有 App 送的設定會被忽略。
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* chr) override {
    uint8_t* data = chr->getData();
    size_t len = chr->getLength();
    if (len == 0) return;

    char* buffer = (char*)malloc(len + 1);
    if (!buffer) return;
    memcpy(buffer, data, len);
    buffer[len] = '\0';
    Serial.printf("[BLE] 收到設定：%s\n", buffer);

    // 記憶體釋放只能有一條路徑：ho_relay2 的 onWrite 在成功分支呼叫 free(buffer) 後
    // 又在函式結尾再呼叫一次，靠 ESP.restart() 沒真的執行到才沒炸。這裡改成
    // 所有分支共用同一個結尾，只在函式唯一的出口 free 一次。
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buffer);

    if (err || !doc.containsKey("wifi")) {
      JsonDocument res;
      res["status"] = "error";
      res["message"] = "無效的JSON格式";
      char resBuf[200];
      serializeJson(res, resBuf);
      chr->setValue((uint8_t*)resBuf, strlen(resBuf));
      chr->notify();
      free(buffer);
      return;
    }

    const char* newSsid = doc["wifi"]["ssid"];
    const char* newPassword = doc["wifi"]["password"];
    const char* newMqttServer = doc["wifi"]["server"];
    const char* newMqttUsername = doc["wifi"]["mqtt_username"];  // 選用
    const char* newMqttPassword = doc["wifi"]["mqtt_password"];  // 選用
    int newMqttPort = doc["wifi"]["mqtt_port"] | 1883;            // 選用，預設 1883

    if (!newSsid || !newPassword || !newMqttServer) {
      JsonDocument res;
      res["status"] = "error";
      res["message"] = "SSID、密碼或伺服器格式錯誤";
      char resBuf[200];
      serializeJson(res, resBuf);
      chr->setValue((uint8_t*)resBuf, strlen(resBuf));
      chr->notify();
      free(buffer);
      return;
    }

    strncpy(ssid, newSsid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(password, newPassword, sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    strncpy(mqttServer, newMqttServer, sizeof(mqttServer) - 1);
    mqttServer[sizeof(mqttServer) - 1] = '\0';

    if (newMqttUsername) {
      strncpy(mqttUsername, newMqttUsername, sizeof(mqttUsername) - 1);
      mqttUsername[sizeof(mqttUsername) - 1] = '\0';
    } else {
      mqttUsername[0] = '\0';
    }
    if (newMqttPassword) {
      strncpy(mqttPassword, newMqttPassword, sizeof(mqttPassword) - 1);
      mqttPassword[sizeof(mqttPassword) - 1] = '\0';
    } else {
      mqttPassword[0] = '\0';
    }
    mqttPort = newMqttPort;
    useCustomServer = true;

    saveNetConfig();

    JsonDocument res;
    res["status"] = "success";
    res["message"] = "WiFi 和 MQTT 設定已儲存";
    res["data"]["device_id"] = getDeviceId();
    res["data"]["ssid"] = ssid;
    res["data"]["mqttServer"] = mqttServer;
    res["data"]["mqttPort"] = mqttPort;
    res["data"]["hasAuth"] = (strlen(mqttUsername) > 0);
    char resBuf[350];
    serializeJson(res, resBuf);
    chr->setValue((uint8_t*)resBuf, strlen(resBuf));
    chr->notify();

    free(buffer);

    Serial.println("[BLE] 設定已儲存，2 秒後重新啟動");
    espNowDelay(2000);   // 維持心跳，避免已配對的 slave 在重啟前失聯
    ESP.restart();
  }
};

void setupBLE() {
  const char* deviceId = getDeviceId();
  BLEDevice::init(deviceId);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());

  pService->start();
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf("[BLE] 已啟動，名稱: %s\n", deviceId);
}

// ── LED：一次性閃爍請求（Task 7）──
// 這套機制沿用 ho_slave1.ino 的 updateBlink() 設計，用於配對接受／拒絕等
// 「單次事件」的視覺回饋（閃 N 下就結束），與下方 updateStatusLed() 的
// 「只要條件成立就一直閃」持續式狀態指示是兩種不同用途，刻意不合併成一個函式：
// 硬合併會讓「配對結果閃 3 下」與「WiFi 未連快閃」互相覆蓋，行為難以預測。
//
// 分工與優先權：updateBlink() 進行中（blinkActive）時暫時接管 LED；
// 播完（blinkActive 轉回 false）的同一輪 loop() 就會呼叫 updateStatusLed()，
// 該函式一開頭就檢查 blinkActive，是的話直接讓出 LED，兩者不會互相覆蓋。
// 呼叫順序見 loop() 尾端：每輪先 updateBlink() 推進一次性閃爍，再呼叫 updateStatusLed()。
//
// onEspNowRecv() 依 ESP-IDF 規定在 WiFi task context 執行、不可做冗長操作，
// 這裡呼叫 requestBlink() 只寫入 volatile 旗標、立即返回，與 slaveCount 的
// 跨 context 存取是同一個理由。
volatile uint8_t blinkRequestTimes = 0;      // 待執行的閃爍次數（由 callback 登記）
volatile uint16_t blinkRequestInterval = 0;  // 亮／滅各自的毫秒數

bool blinkActive = false;        // loop() 是否正在執行一次性閃爍
uint8_t blinkRemaining = 0;      // 還剩幾次
uint16_t blinkInterval = 0;
bool blinkPhaseOn = false;       // 目前是「亮」的半週期
unsigned long blinkPhaseStart = 0;

// 供 ESP-NOW callback 呼叫：只寫旗標，立即返回，不在 WiFi task 裡做耗時操作
void requestBlink(uint8_t times, uint16_t intervalMs) {
  blinkRequestInterval = intervalMs;
  blinkRequestTimes = times;   // 次數最後寫，確保 loop() 讀到時間隔已就緒
}

void updateBlink(unsigned long now) {
  // 有新的登記且目前沒在閃 → 啟動
  if (!blinkActive && blinkRequestTimes > 0) {
    blinkRemaining = blinkRequestTimes;
    blinkInterval = blinkRequestInterval;
    blinkRequestTimes = 0;
    blinkActive = true;
    blinkPhaseOn = true;
    blinkPhaseStart = now;
    setLeds(true);
    return;
  }
  if (!blinkActive) return;
  if (now - blinkPhaseStart < blinkInterval) return;   // 無號數減法，不怕 millis() 溢位

  blinkPhaseStart = now;
  if (blinkPhaseOn) {
    blinkPhaseOn = false;   // 亮的半週期結束 → 轉滅
    setLeds(false);
    return;
  }

  // 滅的半週期結束 → 完成一次閃爍
  blinkRemaining--;
  if (blinkRemaining == 0) {
    blinkActive = false;
    // 這裡刻意不設定 LED 最終狀態：loop() 同一輪緊接著會呼叫 updateStatusLed()，
    // LED 該亮或滅交由持續式狀態指示決定（master 沒有 slave 那種「還原成繼電器
    // 狀態」的 LED 語義，master 的 LED 代表的是連線狀態，不是繼電器狀態）
    return;
  }
  blinkPhaseOn = true;
  setLeds(true);
}

// ── LED：持續式狀態指示（Task 7）──
// 互斥判斷，優先序由高到低：
//   1. bleConfigMode（BLE 配網中）      → 慢閃 1000ms
//   2. pairingMode（配對模式中）        → 慢閃 500ms
//   3. WiFi 未連線                      → 快閃 300ms，滿 30 秒後熄滅省電
//   4. WiFi 已連但 MQTT 未連            → 一長二短
//   5. 全部正常                          → 熄滅
// 與 updateBlink() 的分工見上方註釋；這裡只需在開頭檢查 blinkActive 即可。
unsigned long wifiDownSince = 0;   // 0 表示目前是連線狀態，尚未開始計時省電熄燈

void updateStatusLed(unsigned long now) {
  if (blinkActive) return;   // 一次性閃爍進行中，暫時讓出 LED

  if (bleConfigMode) {
    setLeds((now / 1000) % 2 == 0);
    return;
  }

  if (pairingMode) {
    setLeds((now / 500) % 2 == 0);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSince == 0) wifiDownSince = now;
    if (now - wifiDownSince >= 30000) {
      setLeds(false);   // 滿 30 秒後熄滅省電
    } else {
      setLeds((now / 300) % 2 == 0);   // 快閃 300ms
    }
    return;
  }
  wifiDownSince = 0;   // 已連線：重置計時，下次斷線才重新從 0 開始算 30 秒

  if (!mqttClient.connected()) {
    // 一長二短，週期 2000ms：600ms 亮 → 200ms 滅 → 150ms 亮 → 200ms 滅 → 150ms 亮 → 700ms 滅
    unsigned long phase = now % 2000;
    bool on = (phase < 600) ||
              (phase >= 800 && phase < 950) ||
              (phase >= 1150 && phase < 1300);
    setLeds(on);
    return;
  }

  setLeds(false);   // 全部正常：熄滅
}

// ── WiFi 狀態 ──
volatile uint8_t lastWifiDisconnectReason = 0;
unsigned long wifiConnectedTime = 0;
uint8_t lastKnownChannel = 0;     // 用於偵測 channel 變化

// lastApChannel／lastApBssid／haveLastApBssid 的宣告與說明已上移到 netPrefs 附近，
// 因為 loadNetConfig()／saveNetConfig() 要存取 lastApChannel。

// 「WiFi 關聯進行中」旗標。關聯期間 master 可能離開 slave 鎖定的 channel
//（以指定 channel 起始掃描仍有短暫的 off-channel 時間；升級成全頻掃描時更是整整一輪），
// 此時把心跳間隔臨時加密到 HEARTBEAT_INTERVAL_ASSOC，用發送次數換命中率。
// 由 connectToWiFi() 在 WiFi.begin() 前後設定，maintainEspNow() 讀取。
bool wifiAssociating = false;

// 連續幾次「鎖定 channel 關聯」失敗後，才判定 AP 真的換頻、升級成一次全頻掃描。
// 取 10 而非 1~2：全頻掃描是心跳落空的主要來源，寧可多在舊 channel 上試幾次；
// loop() 的重連節奏下 10 次約 1 分鐘，AP 真換頻時的恢復延遲仍在可接受範圍。
const int WIFI_CHANNEL_LOCK_MAX_FAIL = 10;
int wifiChannelLockFailCount = 0;

// WiFi 事件回調：取得底層斷線原因碼，並在取得 IP 時通知 slave 新 channel
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[WiFi] 斷線原因碼: %d\n", lastWifiDisconnectReason);
    // 常見：2=AUTH_EXPIRE 15=4WAY_HANDSHAKE_TIMEOUT 201=NO_AP_FOUND 202=AUTH_FAIL
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    // 設在事件回調而非 connectToWiFi() 的成功分支：GOT_IP 是「取得 IP」這件事本身的
    // 唯一權威來源，不論它由哪條路徑觸發（connectToWiFi()、或底層自行重新關聯後
    // 重新 DHCP）都會進來，值不會漏更新。
    // 註：舊註釋用「setAutoReconnect(true) 背景重連不會經過 connectToWiFi()」來論證，
    // 那個理由是錯的 —— core 3.3.7 的 _autoReconnect 只存在於建構子／setter／getter，
    // STA_DISCONNECTED 事件處理完全沒有讀它，setAutoReconnect() 實際上是死碼。
    // 結論（設在 GOT_IP）仍然正確，而且比設在 connectToWiFi() 更精確。
    wifiConnectedTime = millis();
    Serial.printf("[WiFi] 取得 IP: %s\n", WiFi.localIP().toString().c_str());
  }
}

// ── 設備 ID ──
const char* getDeviceId() {
  if (deviceIdString.length() == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[20];
    hoFormatDeviceId(mac, buf);
    deviceIdString = String(buf);
  }
  return deviceIdString.c_str();
}

// ── 開機按鈕自檢 ──
// 短暫取樣兩支按鈕腳，整段都是 LOW 即判定卡住並停用其按鈕功能。
// 必須在 pinMode(..., INPUT_PULLUP) 之後呼叫，但絕不能早於 initRelayPins()（繼電器安全鐵則）。
// 注意：這也會擋掉「按住按鈕再上電」的操作，放開後重新上電即恢復。
void checkStuckButtons() {
  const int totalSamples = BTN_SELFTEST_DURATION / BTN_SELFTEST_INTERVAL;
  int bootLowCount = 0;
  int secondLowCount = 0;

  for (int i = 0; i < totalSamples; i++) {
    if (digitalRead(bootButton) == LOW) bootLowCount++;
    if (digitalRead(secondButton) == LOW) secondLowCount++;
    delay(BTN_SELFTEST_INTERVAL);
  }

  bootButtonUsable = (bootLowCount < totalSamples);
  secondButtonUsable = (secondLowCount < totalSamples);

  if (bootButtonUsable && secondButtonUsable) {
    Serial.println("按鈕自檢: 正常");
    return;
  }

  if (!bootButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: BOOT(GPIO %d) 恆為 LOW，本次開機停用其按鈕功能\n", bootButton);
  }
  if (!secondButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: 第二按鈕(GPIO %d) 恆為 LOW，本次開機停用其按鈕功能\n", secondButton);
  }
  Serial.println("  若非按住按鈕開機，代表該腳短路或未接，請檢查硬體");
}

// 是否有「可用的」按鈕正被按下；自檢判定卡住的腳一律視為未按下。
// BOOT 用於短按配對（見 loop() 短按判斷區塊，只認 BOOT 單一支腳）；
// 長按重置（見 updateResetButton()）認兩支腳中任一支，一律走這裡判斷。
bool anyResetButtonPressed() {
  if (bootButtonUsable && digitalRead(bootButton) == LOW) return true;
  if (secondButtonUsable && digitalRead(secondButton) == LOW) return true;
  return false;
}

// ── 長按重置：非阻塞狀態機（Task 8）──
// 移植自 ho_relay2.ino 的 waitForResetConfirm()，但那是阻塞版（while 迴圈裡用
// delay()），不能直接搬過來：master 每 1 秒要發 ESP-NOW 心跳，slave 超過 30 秒
// 沒收到即判定失聯、開始輪掃、強制關閉繼電器（動物管制設備＝開籠），阻塞版整段
// 流程約 5.7 秒（3 秒計時＋2 秒閃爍確認＋0.7 秒長亮）會吃掉將近 6 秒心跳，太危險。
// 改用 millis() 推進的狀態機，每次由 loop() 呼叫一次，推進一小步就返回，
// loop() 每輪仍會呼叫 maintainEspNow()；唯一真的需要等待的一段（長亮 0.7 秒）
// 改用 espNowDelay() 而非裸 delay()，等待期間心跳照常發出。
//
// 與短按配對的區分（見 loop() 短按判斷區塊）：短按只認 BOOT 一支腳，放開時
// pressDuration 落在 [50, 1000)ms 才觸發配對；這裡認 anyResetButtonPressed()
// 涵蓋的任一支腳，需持續按滿 3 秒（LONG_PRESS_TIME）才進入確認階段。兩者互不
// 打架：同一次按壓若在 3 秒內放開，這裡的階段還停在 RESET_WAITING、不會有任何
// 重置動作；若按壓時間落在 1~3 秒之間，長度已超出短按配對的 1000ms 上限，
// 短按判斷式本身就不會觸發配對——「長按中途放開兩者都不觸發」在兩段判斷式各自
// 的條件下自然成立，不需要額外的互斥旗標。
//
// LED 優先權（master 的 LED 共有三種用途，優先權由高到低）：
//   1. 本狀態機的確認階段（RESET_CONFIRM_BLINK）：閃爍／長亮，優先權最高——
//      使用者正在操作重置，必須立即看到回饋。做法是直接呼叫 setLeds()，並回傳
//      true，loop() 收到 true 時會跳過 updateBlink()/updateStatusLed()，兩者
//      本輪完全不會被呼叫、不會覆蓋這裡畫的燈號。
//   2. updateBlink()：一次性請求式閃爍（配對接受／拒絕），播完交還。
//   3. updateStatusLed()：持續式狀態指示（BLE 配網／配對中／WiFi/MQTT 狀態）。
// 三者刻意分成三個獨立機制而非合併：合併會讓「使用者正在長按重置」與「配對結果
// 閃 3 下」或「WiFi 未連快閃」互相覆蓋，行為難以預測。
enum ResetPhase { RESET_IDLE, RESET_WAITING, RESET_CONFIRM_BLINK };
ResetPhase resetPhase = RESET_IDLE;
unsigned long resetPressStart = 0;

// 回傳 true 代表本輪已接管 LED（確認階段），loop() 應跳過 updateBlink()/updateStatusLed()
bool updateResetButton(unsigned long now) {
  bool pressed = anyResetButtonPressed();

  if (!pressed) {
    // 只在已進入閃爍確認階段才印「取消」訊息：3 秒內放開很可能只是短按配對的
    // 正常操作，若也印出「取消重置」字樣會讓使用者誤以為自己觸發了重置流程。
    if (resetPhase == RESET_CONFIRM_BLINK) {
      Serial.println("[重置] 按鈕放開，取消重置");
    }
    resetPhase = RESET_IDLE;
    return false;
  }

  if (resetPhase == RESET_IDLE) {
    // 刻意不在這裡印訊息：0~3 秒是「刻意靜默」區間（可能只是短按配對），
    // 見本函式最上方註釋。訊息延後到真正確認為長按（滿 3 秒）才一併印出。
    resetPhase = RESET_WAITING;
    resetPressStart = now;
    return false;
  }

  unsigned long pressDuration = now - resetPressStart;   // 無號數減法，不怕 millis() 溢位

  if (resetPhase == RESET_WAITING) {
    if (pressDuration < LONG_PRESS_TIME) return false;   // 未滿 3 秒，LED 交還上層，全程靜默
    resetPhase = RESET_CONFIRM_BLINK;
    Serial.println("[重置] 偵測到按鈕按下，開始計時...");
    Serial.println("[重置] 長按 3 秒達成，開始 LED 閃爍確認...");
  }

  // RESET_CONFIRM_BLINK：閃爍確認階段，再按住 BLINK_CONFIRM_TIME(2 秒) 才會執行重置
  unsigned long blinkDuration = pressDuration - LONG_PRESS_TIME;
  if (blinkDuration < BLINK_CONFIRM_TIME) {
    bool ledOn = (blinkDuration % BLINK_INTERVAL) < (BLINK_INTERVAL / 2);
    setLeds(ledOn);
    return true;
  }

  // 閃爍滿 2 秒且按鈕仍按著：確認執行重置。長亮 0.7 秒用 espNowDelay() 而非
  // delay()，等待期間心跳照常發出（見本函式最上方註釋）。
  Serial.println("[重置] 確認重置，LED 長亮 0.7 秒後清除網路設定...");
  setLeds(true);
  espNowDelay(CONFIRM_SOLID_TIME);
  setLeds(false);

  // 只清 hoban（WiFi/MQTT 網路設定），homaster（slave 名冊）刻意保留——
  // 重新配網不該讓所有已配對的籠子全部解除配對。這是刻意行為，不是漏清。
  clearNetConfig();
  Serial.println("[重置] 長按重置只清除網路設定（WiFi/MQTT），"
                 "slave 配對記錄（homaster 名冊）保留，不會解除任何已配對的籠子");
  ESP.restart();
  return true;   // 理論上不會執行到這裡（ESP.restart() 不返回），保留使函式簽名完整
}

// ── 名冊管理 ──
int findSlave(const uint8_t mac[6]) {
  for (int i = 0; i < slaveCount; i++) {
    if (memcmp(slaves[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

void saveSlaves() {
  // 安全網：名冊含 fakeslaves 灌入的測試假資料時拒絕寫入，避免 unpairSlave()／
  // addSlave() 把假 MAC 一併存進 NVS 汙染真實名冊。理由與解法見 fakeSlavesActive
  // 宣告處的註釋；恢復方式只有重開機（假資料本身也是重開機即消失，兩者一致）。
  if (fakeSlavesActive) {
    Serial.println("⚠ [名冊] 目前名冊含 fakeslaves 測試假資料，拒絕寫入 NVS："
                   "真實名冊未被更動。請重新開機清除假資料後，再執行配對／解除配對。");
    return;
  }
  prefs.begin("homaster", false);
  prefs.putInt("count", slaveCount);
  // 只存 MAC，online/rssi/lastSeen 是執行期狀態不需持久化
  uint8_t macs[HO_ESPNOW_MAX_SLAVES * 6];
  for (int i = 0; i < slaveCount; i++) {
    memcpy(macs + i * 6, slaves[i].mac, 6);
  }
  prefs.putBytes("macs", macs, slaveCount * 6);
  prefs.end();
  Serial.printf("[名冊] 已儲存 %d 台\n", slaveCount);
}

// 只寫 channel 這一個鍵，值沒變就不寫（理由同 saveApChannel()）
void saveSlaveLockChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return;
  if (ch == savedSlaveLockChannel) return;
  prefs.begin("homaster", false);
  prefs.putUChar("espch", ch);
  prefs.end();
  slaveLockChannel = ch;
  savedSlaveLockChannel = ch;
  Serial.printf("[名冊] 已記住 slave 鎖定的 channel=%u（供下次開機不關聯 WiFi 時維持心跳）\n", ch);
}

void loadSlaves() {
  prefs.begin("homaster", true);
  slaveCount = prefs.getInt("count", 0);
  if (slaveCount < 0 || slaveCount > HO_ESPNOW_MAX_SLAVES) slaveCount = 0;

  if (slaveCount > 0) {
    uint8_t macs[HO_ESPNOW_MAX_SLAVES * 6];
    size_t got = prefs.getBytes("macs", macs, sizeof(macs));
    if (got != (size_t)slaveCount * 6) {
      Serial.println("[名冊] 資料長度不符，重置為空");
      slaveCount = 0;
    } else {
      for (int i = 0; i < slaveCount; i++) {
        memcpy(slaves[i].mac, macs + i * 6, 6);
        slaves[i].online = false;
        slaves[i].rssi = 0;
        slaves[i].lastSeen = 0;
        slaves[i].relay = 0;
        slaves[i].fwMajor = slaves[i].fwMinor = slaves[i].fwPatch = 0;
        slaves[i].lastCmdId = HO_CMD_ID_NONE;
        slaves[i].lastCmdKind = 0;
        slaves[i].lastCmdCount = 0;
        slaves[i].lastCmdAt = 0;
        slaves[i].dirty = false;
        slaves[i].dirtyFailCount = 0;
        slaves[i].dirtyBackoffUntil = 0;
      }
    }
  }
  slaveLockChannel = prefs.getUChar("espch", 0);
  prefs.end();

  if (slaveLockChannel > 13) slaveLockChannel = 0;   // 髒資料防呆
  savedSlaveLockChannel = slaveLockChannel;

  Serial.printf("[名冊] 載入 %d 台 slave（上次心跳 channel=%u）\n",
                slaveCount, slaveLockChannel);
  for (int i = 0; i < slaveCount; i++) {
    char id[20];
    hoFormatDeviceId(slaves[i].mac, id);
    Serial.printf("  %d. %s\n", i + 1, id);
  }
}

// 把 slave 註冊成 ESP-NOW peer，才能對它單播
bool registerPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  esp_err_t res = esp_now_add_peer(&peer);
  if (res != ESP_OK) {
    Serial.printf("[名冊] 註冊 peer 失敗: %d\n", res);
    return false;
  }
  return true;
}

// 把 NVS 名冊裡的每一台都重新註冊成 ESP-NOW peer。
// 這步不能省，也必須在 esp_now_init() 之後才叫得動：
// peer 表只存在 RAM，重開機後全空，而 loadSlaves() 只從 NVS 還原 MAC 而已。
// 少了它，master 拔電重插這種例行操作就會讓 esp_now_send() 回 ESP_ERR_ESPNOW_NOT_FOUND，
// 所有輪詢與控制指令全部失敗、list 顯示全部離線，而且不會自我修復
//（slave 不會主動送封包來觸發重新註冊），只能一台一台重新配對。
// 更嚴重的是：slave 仍持續收到廣播心跳，lastHeartbeatTime 一直更新，
// slave 端「30 秒失聯就強制關閉繼電器」的安全保護永遠不會觸發 ——
// master 重開機前若某台繼電器是 ON，它會無限期保持通電，等於籠子無限期停在錯誤狀態。
void registerAllPeers() {
  int okCount = 0;
  for (int i = 0; i < slaveCount; i++) {
    if (registerPeer(slaves[i].mac)) okCount++;
  }
  Serial.printf("[名冊] 已重新註冊 %d／%d 台為 ESP-NOW peer\n", okCount, slaveCount);
}

bool addSlave(const uint8_t mac[6]) {
  if (findSlave(mac) >= 0) return true;   // 已存在視為成功
  if (slaveCount >= HO_ESPNOW_MAX_SLAVES) return false;

  memcpy(slaves[slaveCount].mac, mac, 6);
  slaves[slaveCount].online = true;
  slaves[slaveCount].rssi = 0;
  slaves[slaveCount].lastSeen = millis();
  slaves[slaveCount].relay = 0;
  slaves[slaveCount].fwMajor = slaves[slaveCount].fwMinor = slaves[slaveCount].fwPatch = 0;
  slaves[slaveCount].lastCmdId = HO_CMD_ID_NONE;
  slaves[slaveCount].lastCmdKind = 0;
  slaves[slaveCount].lastCmdCount = 0;
  slaves[slaveCount].lastCmdAt = 0;
  slaves[slaveCount].dirty = true;   // 新加入的直接代發一次，不必等輪播輪到它
  slaves[slaveCount].dirtyFailCount = 0;
  slaves[slaveCount].dirtyBackoffUntil = 0;
  slaveCount++;

  registerPeer(mac);
  saveSlaves();
  // 名冊從空的變成有人，或又多一台：把目前 channel 一併記住。
  // 少了這行，「先連上 WiFi、之後才配對 slave」這個最常見的順序下，espch 永遠不會被
  // 寫入（connectToWiFi() 的成功分支在配對發生之前就跑完了），下次 reset 進 BLE 配網
  // 仍然會停在 channel 1。這裡與既有的 saveSlaves() 同樣是在 ESP-NOW callback
  // context 寫 NVS，沒有引入新的風險類別。
  if (WiFi.status() == WL_CONNECTED) saveSlaveLockChannel(currentChannel);

  // Task 4：新配對的這一台也要代訂 hoban/<slaveId>/control，否則 App 送給它的
  // 指令沒有人收。但**不能在這裡直接 subscribe** —— 本函式由 onEspNowRecv()
  // 呼叫，跑在 WiFi task，動 socket 會與 loop() 的 mqttClient.loop() 競態
  // （與 pendingOfflineMac 那組旗標同一個理由）。只插旗，實際訂閱交給 loop()
  // 的 pendingSubscribeRefresh 區塊。
  pendingSubscribeRefresh = true;
  return true;
}

void enterPairingMode() {
  pairingMode = true;
  pairingStartTime = millis();
  Serial.println("[配對] 進入配對模式，60 秒內請短按 slave 的按鈕");
  sendHeartbeat();  // 立刻發一次，讓 slave 早點知道
}

void exitPairingMode() {
  if (!pairingMode) return;
  pairingMode = false;
  Serial.println("[配對] 離開配對模式");
}

// ── 控制 slave ──
// 用 MAC 直接送，不經過名冊索引（Task 4 review M2 修正）。
// 給「索引隨時可能失效」的呼叫路徑用 —— 目前是 MQTT 代理指令：
// topic 解析出 MAC 之後，中間可能隔著阻塞的 publish，而 WiFi task 的
// HO_PKT_UNPAIR 會前移 slaves[]，任何先前取得的索引都可能改指到別台。
// 對捕捉籠而言那不是「更新到舊資料」，是**開錯門**。
//
// 這個寫法是無鎖且真正安全的：findSlave() 只用來確認「這個 MAC 目前確實在
// 名冊上」（純值比較），實際送出時用的是呼叫方自己那份 MAC 副本，不是
// slaves[idx].mac。即使檢查與送出之間陣列剛好搬移，送出的目標仍然是被驗證
// 過的那一台；最壞情況只是對一台剛剛解除配對的設備多送一則指令，而它的
// peer 已被 esp_now_del_peer() 移除，esp_now_send() 會直接失敗，無副作用。
// 回傳值（review N1 新增）：封包**有沒有真的被交給 esp_now_send()**。
// 群組指令的 ACK 歸因閂鎖靠它判斷「這次會不會有送出回呼」——不會有回呼就必須
// 立刻關閂，否則閂會一直開著，把後續任何一則送往同一 MAC 的 ACK 誤記成
// 「群組指令已送達」。既有呼叫端忽略回傳值不受影響。
//
// cmdId（Phase 4 Task 1 新增）：**一道邏輯指令**的識別碼，不是一幀的識別碼。
// 群組指令的所有廣播、單播與補送都要傳同一個值進來，slave 才會把它們算成同一道；
// 單台控制則每次呼叫 allocCmdId() 取一個新的。傳 HO_CMD_ID_NONE 進來是明確的
// 程式錯誤（slave 會照做但不記歸因），所以這裡沒有預設值，強迫呼叫端表態。
bool sendCmdToSlaveMac(const uint8_t mac[6], HoRelayCmd cmd, uint16_t pulseMs,
                       uint16_t cmdId) {
  char id[20];
  hoFormatDeviceId(mac, id);
  if (findSlave(mac) < 0) {
    Serial.printf("[控制] %s 已不在名冊上，放棄送出指令 %u\n", id, (uint8_t)cmd);
    return false;
  }
  HoCmdPayload payload;
  payload.cmd = (uint8_t)cmd;
  payload.pulseMs = pulseMs;
  payload.cmdId = cmdId;
  Serial.printf("[控制] 送指令 %u 給 %s\n", (uint8_t)cmd, id);
  return espNowSendTo(mac, HO_PKT_CMD, &payload, sizeof(payload));
}

// 序列埠專用：把 `list` 顯示的編號轉成 MAC，之後一律走 sendCmdToSlaveMac()。
//
// 為什麼不保留原本那個「拿索引直接 espNowSendTo(slaves[idx].mac, ...)」的版本：
// 檔案裡曾同時存在 MAC 與索引兩套送指令慣例，而同一類缺陷（索引在阻塞或被
// 搶佔之後失效）已經出現過兩次（Task 3 review 的 M4、Task 4 review 的 M2），
// 後果是**開錯門**。這裡只留一個「編號 → MAC」的薄轉換層。
//
// 精確的敘述（review M4 更正）：**索引式的送出路徑已經消失**，而**單播的
// 繼電器指令送出點只剩 sendCmdToSlaveMac() 一個**。全檔的 HO_PKT_CMD 送出點
// 其實有兩個 —— 另一個是 sendCmdToAll() 裡送往 BROADCAST_MAC 的那次廣播，
// 它天生就不經過名冊、也沒有索引可以指錯，屬於另一類。
// 原本寫成「送出點只剩一個」是錯的，這裡改對。
void sendCmdToSlaveIndex(int idx, HoRelayCmd cmd, uint16_t pulseMs) {
  if (idx < 0 || idx >= slaveCount) {
    Serial.println("[控制] 編號超出範圍");
    return;
  }
  uint8_t mac[6];
  memcpy(mac, slaves[idx].mac, 6);   // 先取值，之後不再依賴 idx
  // 單台控制：每次都是一道新的邏輯指令，配一個新的 cmdId
  sendCmdToSlaveMac(mac, cmd, pulseMs, allocCmdId());
}

// 用 MAC 要求回報狀態（理由同 sendCmdToSlaveMac()：索引隨時可能失效）。
void requestSlaveStateMac(const uint8_t mac[6]) {
  if (findSlave(mac) < 0) return;
  espNowSendTo(mac, HO_PKT_STATE_REQ, nullptr, 0);
}

// 序列埠／輪詢用的編號版本，同樣先取 MAC 值再送（review Mi2）。
//
// 第一版把它留成索引式，理由是「唯讀查詢，指錯台不驅動繼電器」。**那個判斷
// 被本 Task 自己推翻了**：狀態回報會刷新 slaves[idx].lastSeen 與 online，
// 而 online 正是 App 判定「這台有沒有回報」的依據（hoctrl 的
// master_slave_logic.dart `evaluateGroupCloseProgress()` 讀的是 slave.online）。
// 指錯台 ＝ **替沒人問的 slave 偽造存活證據**，在「一次要全部關」的系統裡
// 那是誤綠方向的錯誤。因此檔案裡不再保留任何索引式的 ESP-NOW 送出路徑。
void requestSlaveStateIndex(int idx) {
  if (idx < 0 || idx >= slaveCount) return;
  uint8_t mac[6];
  memcpy(mac, slaves[idx].mac, 6);   // 先取值，之後不再依賴 idx
  requestSlaveStateMac(mac);
}

// ── 群組指令：廣播（真正同時）→ 逐台單播（唯一可證明的送達）──
//
// **為什麼不是純逐台單播。** 本系統是多開門的捕捉系統，核心需求是
// 「一次要全部關」，關門失敗＝動物逃脫或未被捕捉。純逐台單播每台間隔 20ms，
// 20 台就是最後一台晚 400ms 收到 —— 那不叫「同時」，規格已明文否決這個落差。
// 所以主指令走 ESP-NOW 廣播（FF:FF:FF:FF:FF:FF），一次送出、所有已配對 slave
// 同一瞬間收到。
//
// 廣播零新基礎設施也不會誤觸發別人的設備：廣播 peer 在 setupEspNow() 就註冊好、
// 心跳本來就走廣播；slave 端
// `if (!masterKnown || memcmp(masterMac, info->src_addr, 6) != 0) return;`
// 擋在 HO_PKT_CMD 分支之前，而 **ESP-NOW 廣播幀的 src_addr 仍是發送端的真實
// MAC**（廣播只換目的位址），這道檢查對廣播與單播一體適用。
//
// ── review C1 之後的重寫：只宣稱可證明的事 ──
//
// 第一版試圖證明「指令已被執行」：靜置後看 slaves[i].relay 是不是預期值，
// 並用 master 自己送的 HO_PKT_STATE_REQ 去「製造」一則新回報當作證據。
// **那是假綠燈。** 對 HO_CMD_OFF 而言，「新回報」是我們自己那則查詢造成的，
// 而 relay == 0 是 slave 的**靜止預設值**（開機、點動結束、loadSlaves()、
// addSlave() 全都初始化成 0）。兩半都不是證據，AND 起來仍然不是證據 ——
// 一台從未收到 OFF 的 slave 會被判成「已確認」。更糟的是確認是 sticky 的，
// 假確認一旦成立就再也不會補送，而「收不到廣播、卻收得到單播」正是邊界訊號
// 那台的典型表現（廣播沒有 MAC 層 ACK 與重傳，單播有）。
//
// **當時的根因是結構性的**：協定版本 1 的 `HoStatePayload` 沒有任何指令歸因欄位
//（沒有「我剛執行了哪一則指令」的 seq／echo），所以在那一版協定下「指令有被執行」
// **原理上無法證明**。Task 5 的裁決是不試圖證明「已執行」，只誠實區分兩件事 ——
//
//   1. **「已送達」是可證明的**：單播有 MAC 層 ACK，esp_now_send() 的送出回呼
//      （onEspNowSent()）會逐幀回報成功／失敗，且 wifi_tx_info_t::des_addr
//      帶著目的 MAC，可以逐台歸因。**廣播沒有 ACK**，永遠回報成功，不算證據。
//   2. **「已執行」不可證明**：不准用任何自製證據去宣稱它。
//
// 因此流程是：**廣播 3 次（同時性）＋ 對每一台都送一次單播（可證明的送達）**，
// 未取得 ACK 的再補送。**誤紅可接受、誤綠不可接受** —— 寧可每次都補送，
// 也不要用假證據省下那幾個封包。20 台的單播不貴。
//
// ── Phase 4 Task 1：那筆技術債已還，但流程刻意**不改** ──
//
// 協定版本 2 的 HoCmdPayload 帶 cmdId、HoStatePayload 帶 lastCmdId，
// master 因此第一次有了**由 slave 產生、master 造不出來**的執行證據
//（見 groupExecutedIdx()）。上面第 2 點的「不可證明」到此為止。
//
// **但廣播 ＋ 全台單播 ＋ 補送的流程一行都不動**，理由有三：
//   - 執行證明是**非同步**的：它可能在收工之後才到，不能拿來當「可以不用補送了」
//     的即時依據。拿它提前結束補送就是用一個晚到的證據去省掉一次真實的重送。
//   - 沒有執行證明**不等於沒執行**（回報可能掉了），所以它也不能當成「要多補幾趟」
//     的依據 —— 兩個方向都不該讓它介入送出決策。
//   - 送達證明與執行證明**互補**：只收到廣播的那台會是「送達紅、執行綠」。
//     兩者都保留，才看得出是哪一段出問題。
// 於是執行證明只進入**回報**（序列埠收工訊息、MQTT 的 exed／exe），不進入**控制**。
const int GROUP_BROADCAST_REPEAT = 3;             // 廣播連送次數（無 ACK，靠重送提高命中）
const unsigned long GROUP_BROADCAST_GAP = 20;     // 每次廣播之間的間隔（ms）
const unsigned long GROUP_STEP_GAP = 20;          // 逐台單播時每台之間的間隔（ms）
const unsigned long GROUP_ACK_WAIT_MS = 300;      // 一趟掃完後，等 MAC 層 ACK 回來的時間
const int GROUP_MAX_SWEEPS = 3;                   // 第一趟（全部）＋ 最多 2 趟補送

// ── review M3：整個 job 的 wall-clock 硬上限 ──
// 沒有這道上限，job 的長度＝趟數 ×（台數 × 每步間隔 ＋ 等待），而「每步」實際上是
// 一次 loop() 迭代 —— 只要 loop() 被任何一個 10 秒級的阻塞 socket 寫入拖慢，
// 整個 job 就會被拉長到不可預期。而 job 進行期間 pollNextSlave() 是讓開的
// （理由見 groupNoteUnicastAck()），輪詢被餓死超過 30 秒就會讓
// updateSlaveOnlineStatus() 把全部 slave 誤判離線。
// 6 秒是刻意選的：輪詢週期 15 秒 ＋ 最多 6 秒停擺 ＝ 21 秒 < 30 秒門檻，留 9 秒餘裕。
// 時間到就立刻收工並據實回報，不再補送。
const unsigned long GROUP_JOB_MAX_MS = 6000;

enum : uint8_t {
  GROUP_JOB_IDLE = 0,
  // review 第 4 輪：inline 第一趟單播進行中。
  // 這個階段的存在理由見 groupCmdSnapshot()／groupNoteUnicastAck()：
  // 「job 已啟動」的時點必須**早於** sendCmdToAll() 裡那趟 inline 單播，
  // 否則所有以 groupCmdActive() 為條件的守衛在那 400ms 內全部是假的。
  GROUP_JOB_ARMED,
  GROUP_JOB_WAIT,     // 等 MAC 層 ACK 回來，時間到就結算
  GROUP_JOB_SWEEP,    // 逐台補送單播（每輪 loop() 最多一台）
};

struct GroupCmdJob {
  // volatile：groupNoteUnicastAck() 在 **WiFi task** 讀它（透過 groupCmdActive()），
  // loop() 寫它。與 slaveCount 同一個理由。
  volatile uint8_t phase;
  uint8_t cmd;              // HoRelayCmd
  uint16_t pulseMs;
  // 本次群組指令的邏輯識別碼。3 次廣播、每台的單播、以及第 2、3 趟補送全部共用它。
  // 收工後**不清掉**：執行證明可能在收工之後才到（slave 的回報要經過 master 的
  // WiFi task），留著才能讓 groupExecutedIdx() 繼續把紅的翻成綠的。
  uint16_t cmdId;
  int count;                // 快照下來的台數
  uint8_t macs[HO_ESPNOW_MAX_SLAVES][6];
  bool gone[HO_ESPNOW_MAX_SLAVES];   // 執行期間離開名冊（**不是**「已送達」）
  unsigned long startedAt;
  unsigned long waitUntil;
  unsigned long nextStepAt;
  int cursor;
  int sweep;
  bool everRan;             // 開機以來至少跑過一次群組指令（決定 MQTT 要不要帶 group 欄位）
};
GroupCmdJob groupJob;   // 全域，開機零初始化 ＝ GROUP_JOB_IDLE

// ── 單播的 MAC 層送達旗標 ──
// 由 onEspNowSent()（**WiFi task**）寫入、由 loop() 讀取，所以是 volatile。
// 語義嚴格限定為「這一台在本次群組指令中，至少有一次單播 HO_PKT_CMD 拿到了
// MAC 層 ACK」。**不代表繼電器動作了**（見上方 C1 的說明）。
volatile bool groupDelivered[HO_ESPNOW_MAX_SLAVES];

// ── ACK 歸因閂鎖 ──
// onEspNowSent() 會被**每一次** espNowSendTo() 觸發，包含心跳（廣播）、
// HO_PKT_STATE_REQ（單播！）、HO_PKT_UNPAIR（單播）。若不加限制，一則
// STATE_REQ 的 ACK 會被誤記成「群組指令已送達」—— 那正是 C1 的同一個錯誤
// （拿不相干的證據去宣稱另一件事）。
// 所以只在「剛送出群組單播、還沒收到它的回呼」這個窗口內開閂，並且比對目的 MAC。
//
// ── 殘留窗口的完整清單（review 第 4 輪更正）──
// 若在閂開著的那 1~2ms 內，剛好有另一則送往**同一個 MAC** 的單播先拿到 ACK，
// 會被誤記。上一版寫「剩下的同 MAC 送出只有 HO_PKT_UNPAIR」是**錯的**，
// 實際上有三條，而且後兩條那台**仍在名冊上**（gone 論證不適用）：
//
//   1. HO_PKT_UNPAIR（unpairSlave()）—— 那台會被 groupRefreshRoster() 標成
//      gone，而 gone 在 groupCountAll() 與 groupDeliveryFor() 都優先於
//      delivered，不會被算成已送達。**這條有守住。**
//   2. 序列埠 state <n>（requestSlaveStateIndex()）—— 上一版**沒有**守衛，
//      而 8a 的校準步驟正好教操作者用它，等於驗收程序自己製造危害。
//      **本輪已補上 groupCmdActive() 守衛**，與 pollNextSlave()／
//      handleSlaveCommand() 一致。
//   3. HO_PKT_PAIR_ACK（onEspNowRecv() 的 HO_PKT_PAIR_REQ 分支）——
//      **這條擋不掉也不該擋**：配對請求必須回覆，而且它跑在 WiFi task。
//      ho_slave1 的 requestPairing() 沒有「已配對就不送」守衛，所以一台已配對的
//      slave 理論上可以在那 1~2ms 內送 PAIR_REQ 進來。
//      風險評估（誠實版）：這會讓證據的**指向性**變差（拿 PAIR_ACK 的 ACK 去
//      認 CMD 的送達），但它**不是 C1 那種自製證據** —— MAC 層 ACK 仍然是由
//      對方射頻產生的，master 造不出來，所以那台當下確實可達。
//      量級：需要同一台、在同一個 1~2ms、剛好送出 PAIR_REQ。**沒有實機驗證過。**
volatile bool groupAckArmed = false;
volatile int groupAckIdx = -1;
// ── review N-e：groupAckMac 也要 volatile ──
// loop() 先寫值再開閂、WiFi task 只在閂開著時讀，這個順序本身是對的，但
// groupAckMac 原本是**非** volatile，而 groupAckArmed／groupAckIdx 是 volatile，
// 兩者之間沒有任何記憶體屏障。Arduino-ESP32 的 WiFi task 通常釘在 core 0、
// loopTask 在 core 1，是真併發；嚴格依 C++ 記憶體模型，編譯器可以把非 volatile
// 的存取搬到 volatile 存取之前。實務上 LX6 對內部 SRAM 有序、C3 是單核，踩不到，
// 但那是**約定而非結構** —— 與本輪要修的毛病同一形狀，所以一併改掉。
// 代價：memcpy／memcmp 不能用在 volatile 上，改成 6 次的逐 byte 迴圈。
volatile uint8_t groupAckMac[6];

void groupAckMacSet(const uint8_t* mac) {
  for (int k = 0; k < 6; k++) groupAckMac[k] = mac[k];
}

bool groupAckMacMatches(const uint8_t* mac) {
  for (int k = 0; k < 6; k++) {
    if (groupAckMac[k] != mac[k]) return false;
  }
  return true;
}

bool groupCmdActive() { return groupJob.phase != GROUP_JOB_IDLE; }

// 由 onEspNowSent() 呼叫（WiFi task）。desAddr 可能為 nullptr（防禦性檢查）。
void groupNoteUnicastAck(const uint8_t* desAddr, bool ok) {
  // ── review 第 4 輪：結構性防線 —— job 之外一律拒絕寫入 ──
  // 「只有群組單播才會開閂」原本只是一個**約定**（靠每個開閂點自律）。
  // 這一行把它變成**結構**：不在 job 內就不可能寫到 groupDelivered[]。
  //
  // ── review N-a：這道守衛今天不擋任何回呼，純屬未來防護。誠實敘述 ──
  // 上一版寫它「順帶殺掉跨 job 的過期回呼」**是錯的**，那句已刪除：
  //   - 跨 job 過期回呼的情境是「job A 的回呼延遲到 **job B 已經開閂之後**才到、
  //     且 MAC 同一台」。此時 phase 是 ARMED／WAIT（非 IDLE），本守衛**放行**，
  //     groupAckArmed 為 true、MAC 也對得上 → 照樣被記進 job B。**沒有被擋掉。**
  //   - 反過來，「phase == IDLE 且閂還開著」這個狀態在現行程式**觀察不到**。
  //     但理由不是「每個出口都關閂」（上一版那句字面不精確，這裡改對）——
  //     三個進 IDLE 的出口實際上是：
  //       * groupFinishJob()：關閂在**函式第一行**、設 IDLE 在最後 → 先行關閂 ✔
  //       * groupCmdSnapshot()：設 IDLE 在第一行、關閂在**下一行** → 後關；
  //         但兩行之間不讓出 CPU，且此刻 phase 已是 IDLE，本守衛照樣擋下
  //       * sendCmdToAll() 的空名冊早退：**根本沒關閂** ——
  //         它靠的是「groupCmdSnapshot() 剛剛才關過」這個前置條件
  //     結論仍然成立（IDLE 時寫不進 groupDelivered[]），但成立的原因是
  //     **本守衛自己**，不是那三個出口都做了關閂。
  // 所以本守衛的價值是「把約定變成結構、擋住未來新增的路徑」，**不是**修掉任何
  // 現存缺陷。在這個專案裡「宣稱一道其實不存在的防線」與那些假綠燈是同一個形狀，
  // 不能留。
  //
  // **這一行的位置有陷阱**：groupCmdSnapshot() 曾經把 phase 設成 IDLE，而
  // sendCmdToAll() 要到 inline 第一趟單播跑完才設 WAIT —— 若沿用那個順序，
  // 第一趟全程 groupCmdActive() 都是 false，這行會把第一趟的 ACK 全部丟掉，
  // 變成**大規模誤紅**。所以 groupCmdSnapshot() 已改成在快照完成時就設
  // GROUP_JOB_ARMED（見該函式），「已啟動」的時點前移到 inline 段之前。
  if (!groupCmdActive()) return;
  if (!groupAckArmed) return;
  if (desAddr == nullptr) return;
  // 廣播的送出回呼永遠回報成功，明確排除 —— 把它當送達就是假綠燈
  if (memcmp(desAddr, BROADCAST_MAC, 6) == 0) return;
  if (!groupAckMacMatches(desAddr)) return;

  int i = groupAckIdx;
  groupAckArmed = false;    // 先關閂，避免同一則回呼被重複歸因
  if (!ok) return;
  if (i < 0 || i >= HO_ESPNOW_MAX_SLAVES) return;
  groupDelivered[i] = true;
}

// 送一則群組單播給快照第 i 台，並開啟 ACK 歸因閂鎖。
// 先寫 MAC 再開閂：WiFi task 只在閂開著時讀 groupAckMac，順序反過來會讀到舊值。
void groupSendUnicast(int i) {
  if (i < 0 || i >= groupJob.count) return;
  groupAckMacSet(groupJob.macs[i]);
  groupAckIdx = i;
  groupAckArmed = true;
  // review N1 的根因修正：sendCmdToSlaveMac() 在「已不在名冊上」或
  // esp_now_send() 回錯時**根本不送**，那次就不會有送出回呼來關閂。
  // 閂若留著開，下一則送往同一 MAC 的封包（收工後恢復的 pollNextSlave()
  // 所送的 HO_PKT_STATE_REQ 最典型）會被誤記成「群組指令已送達」，
  // 讓序列埠已經印過的「未送達」在 MQTT 上被翻成綠的。送不出去就當場關閂。
  if (!sendCmdToSlaveMac(groupJob.macs[i], (HoRelayCmd)groupJob.cmd, groupJob.pulseMs,
                         groupJob.cmdId)) {
    groupAckArmed = false;
  }
}

// ── 執行證明（Phase 4 Task 1：還 Phase 2b Task 5 登記的技術債）──
//
// 回傳 true 的**唯一**含意：快照第 i 台回報過「我執行的是 groupJob.cmdId 這道指令」，
// 而且那則回報是在本次指令送出**之後**才收到的。
//
// **這證明什麼**：那台的韌體確實走完了 HO_PKT_CMD 分支的
// setRelayPins()／pulseRelay() 呼叫，而且那一次帶的就是本次的 cmdId。
// 證據是 slave 產生的，master 造不出來 —— 這正是 Task 5 的 C1 缺的那一半
//（當時 master 用自己送的 STATE_REQ 去「製造」新回報，那是自製證據）。
//
// **它擋不住什麼（三項，必須連著讀）**：
//   1. **不證明繼電器硬體動作**，更不證明籠門關上了。setRelayPins() 只寫 GPIO；
//      MOS 燒毀、線路脫落、觸點黏死一律照樣回報「已執行」。
//      HO_CMD_PULSE 只證明點動計時器被啟動，不證明門真的落下。
//      **現場確認仍然是唯一能證明門關上的方法。**
//   2. **不擋重放**。協定沒有加密也沒有 nonce。時間條件（回報晚於指令送出）擋得住
//      **跨指令**的舊回報，擋不住「指令送出後才被重播」的那則。
//   3. **回 false 不等於「沒執行」**。回報可能還在路上、可能掉了、那台可能剛好離線。
//      所以 false 只能拿來**維持紅色**，不能拿來宣稱「已確認未執行」。
//      誤紅可接受、誤綠不可接受 —— 這個不對稱是刻意的。
//   4. **完全擋不住偽造，而且這是四項裡最嚴重的一項。**
//      認證強度只有「CRC-8 ＋ 原始碼裡的共享密鑰字面常數」，ESP-NOW 來源 MAC 可任意填。
//      射頻範圍內的第三方只要有「目標 MAC ＋ cmdId ＋ 種類」就能組出一封合法的
//      HO_PKT_STATE，讓一台**沒動作**的 slave 在這裡回 true、在 MQTT 上顯示 "exe":1、
//      在序列埠印出 [歸因] —— 那是**由攻擊者遞送的假綠燈**：「門關好了」而門是開的。
//      三個輸入全部可從空中觀測（MAC 與 cmdId 就在 master 廣播的 HO_PKT_CMD 明文裡）。
//      **能偽造 STATE 的人同樣能偽造 CMD 直接驅動繼電器**，所以本函式新增的不是
//      「控制面」而是「說謊面」—— 但這個限定比想像中弱：
//        (i)  靜止狀態是繼電器 OFF ＝ 門開，**干擾本身就能造成關門失敗**，
//             攻擊者只需要「掩蓋」，不需要「製造」。
//        (ii) 偽造 CMD 的效果是暫態（撐不過 slave 的 30 秒失聯保護），
//             **一封偽造的 STATE 卻是持續的**，而且會抑制「去現場確認」——
//             那是這個系統唯一的真防線。
//      謊的方向正是誤綠，仍須明列。
//      **唯一的結構性修法是訊息鑑別（HMAC／nonce），CRC-8 承載不了。**
//      緩解評估與具名技術債見 docs/phase4-flag-day-upgrade.md 第 3.2 節。
//
// 只在 loop() context 呼叫（appendGroupResult()／groupFinishJob()／
// appendSlavesArray()），會讀 slaves[] 與 slaveCount，不從 WiFi task 呼叫。
bool groupExecutedIdx(int i) {
  if (!groupJob.everRan) return false;
  if (i < 0 || i >= groupJob.count) return false;
  if (groupJob.cmdId == HO_CMD_ID_NONE) return false;
  int s = findSlave(groupJob.macs[i]);
  if (s < 0) return false;   // 已離開名冊：沒有證據，維持紅色
  if (slaves[s].lastCmdId != groupJob.cmdId) return false;
  // 種類也要對。cmdId 本身已足以識別，這一項是縱深防禦：萬一 cmdId 撞號，
  // 種類不同就不會被誤認。多擋一種誤綠，代價是零。
  if (slaves[s].lastCmdKind != groupJob.cmd) return false;
  // 回報必須晚於指令送出。用無號數減法轉有號比較，millis() 迴繞時仍正確。
  if ((long)(slaves[s].lastCmdAt - groupJob.startedAt) < 0) return false;
  return true;
}

int groupCountExecuted() {
  int n = 0;
  for (int i = 0; i < groupJob.count; i++) {
    if (groupExecutedIdx(i)) n++;
  }
  return n;
}

// 把「執行期間離開名冊」的標記更新一次。這些台**不計為已送達**。
void groupRefreshRoster() {
  for (int i = 0; i < groupJob.count; i++) {
    if (groupJob.gone[i]) continue;
    if (findSlave(groupJob.macs[i]) < 0) {
      char id[20];
      hoFormatDeviceId(groupJob.macs[i], id);
      Serial.printf("[群組] %s 已不在名冊上，停止補送（不計為已送達）\n", id);
      groupJob.gone[i] = true;
    }
  }
}

// 仍需補送的台數：沒拿到 ACK、且還在名冊上的
int groupCountPending() {
  int n = 0;
  for (int i = 0; i < groupJob.count; i++) {
    if (!groupDelivered[i] && !groupJob.gone[i]) n++;
  }
  return n;
}

void groupCountAll(int& ack, int& noack, int& gone) {
  ack = 0;
  noack = 0;
  gone = 0;
  for (int i = 0; i < groupJob.count; i++) {
    if (groupJob.gone[i]) gone++;
    else if (groupDelivered[i]) ack++;
    else noack++;
  }
}

// 收工。**只講可證明的事**：MAC 層送達與否。執行與否一律明講無法證明。
void groupFinishJob(bool hitTimeCap) {
  // ── review N1 ＋ N-f：關閂，而且必須是本函式的**第一件事** ──
  //
  // N1（為什麼收工一定要關閂）：groupSendUnicast() 是「先開閂再送」，但
  // sendCmdToSlaveMac() 在 findSlave() 失敗或 esp_now_send() 回錯時**根本不送**，
  // 那次開的閂沒有回呼來關它，會跨過收工繼續開著。而收工當輪 pollNextSlave()
  // 立刻恢復，它送的 HO_PKT_STATE_REQ 是單播，命中同一個 MAC 就會把
  // groupDelivered[] 設成 true —— 而 groupDelivered[] 在 job 結束後**仍持續被
  // MQTT 讀取**（groupDeliveryFor()／appendGroupResult()）。根因已在
  // groupSendUnicast() 一併堵掉，這裡是第二道：無論閂為什麼還開著，收工一律關。
  //
  // N-f（為什麼要排在最前面）：
  // 原本這行排在所有 Serial.printf 之後。收工走 wall-clock 上限那條時，最後一次
  // 單播可能只在 20ms 前送出，而 115200 baud 下那 5~8 行輸出本身就要 10~20ms ——
  // 那則**遲到的真實 ACK** 會落在「計數已取、序列埠已印」與「關閂」之間的窗口裡，
  // 把一台剛被印成「未送達」的 slave 在 groupDelivered[] 裡翻成 true。
  // 這則 ACK 是真的（誤紅修正，不是假綠燈），但它的**外顯症狀與 N1 一模一樣**：
  // 序列埠說紅、MQTT 說綠。而回歸清單 8a／8d 第 4 步正是拿這個症狀當 FAIL 判準，
  // 於是會產生假 FAIL；更糟的是一旦被當成「已知雜訊」，真正的 N1 復發就會被
  // 一起吃掉。移到最前面，序列埠與 MQTT 在收工那一刻就一致。
  groupAckArmed = false;

  int ack, noack, gone;
  groupCountAll(ack, noack, gone);

  Serial.printf("[群組] 指令 %u 收工%s：單播 MAC 層已送達 %d／%d 台\n",
                groupJob.cmd, hitTimeCap ? "（達 wall-clock 上限）" : "",
                ack, groupJob.count);
  if (noack > 0) {
    Serial.printf("⚠ [群組] %d 台連「送達」都沒有（單播沒拿到 MAC 層 ACK）\n", noack);
    for (int i = 0; i < groupJob.count; i++) {
      if (groupDelivered[i] || groupJob.gone[i]) continue;
      char id[20];
      hoFormatDeviceId(groupJob.macs[i], id);
      Serial.printf("⚠ [群組]   未送達：%s\n", id);
    }
  }
  if (gone > 0) {
    Serial.printf("⚠ [群組] %d 台在執行期間離開名冊，同樣未送達\n", gone);
  }

  // ── Phase 4 Task 1：執行證明（Task 5 登記的技術債已還）──
  // 協定版本 2 起，slave 在 HoStatePayload 帶回 lastCmdId，master 才第一次
  // 有辦法把「那台的 relay 值」歸因到「我下的這一道指令」。
  int executed = groupCountExecuted();
  Serial.printf("[群組] 韌體層已執行 %d／%d 台（slave 回報 cmdId=%u）\n",
                executed, groupJob.count, groupJob.cmdId);
  if (executed < groupJob.count) {
    for (int i = 0; i < groupJob.count; i++) {
      if (groupExecutedIdx(i)) continue;
      char id[20];
      hoFormatDeviceId(groupJob.macs[i], id);
      Serial.printf("⚠ [群組]   無執行證明：%s\n", id);
    }
  }

  // 這三行是本 Task 最重要的輸出，**任何情況都要印**（包含全部送達時）。
  Serial.println("[群組] 注意：MAC 層 ACK 只證明「封包已送達」，"
                 "不能證明繼電器真的動作");
  Serial.println("[群組] 執行證明只到韌體層：證明 slave 走完了繼電器動作那段程式，"
                 "不證明繼電器硬體動作，更不證明籠門關上");
  Serial.println("[群組] 沒有執行證明不等於沒執行 —— 回報可能還在路上；"
                 "它只能維持紅色，不能宣稱已確認未執行");

  // 關門路徑要額外點名。App 的「全部關門」按鈕送的是 **ALL:ON**（＝廣播 PULSE，
  // 見 hoctrl 的 device_detail_page.dart `_sendGroupCloseCommand()`），
  // 不是 ALL:OFF —— 所以警語掛在 PULSE 上才有意義。OFF 也一併掛（序列埠 alloff）。
  if (groupJob.cmd == HO_CMD_PULSE || groupJob.cmd == HO_CMD_OFF) {
    Serial.println("⚠ [群組] 這是關門路徑：未送達的籠門必然沒關；已送達的也只代表"
                   "封包到了，一律以現場確認為準，不要當成已關閉");
  }

  // 讓 App 收到每一台的真實狀態與本次的送達結果（見 appendSlavesArray()／
  // buildStatusDoc() 的 "grp" 與 "group" 欄位，review M2）
  markAllSlavesDirty();
  groupJob.phase = GROUP_JOB_IDLE;
}

// 群組指令的補送段。由 loop() 每輪呼叫，每次最多送一台，完全不阻塞。
void processGroupCmd() {
  if (groupJob.phase == GROUP_JOB_IDLE) return;

  unsigned long now = millis();

  // review M3：wall-clock 硬上限優先於一切，時間到就收工
  if ((long)(now - (groupJob.startedAt + GROUP_JOB_MAX_MS)) >= 0) {
    groupRefreshRoster();
    groupFinishJob(true);
    return;
  }

  // inline 第一趟單播還在跑（sendCmdToAll() 內），loop() 不該半途接手一個
  // 還沒送完的 job。這個分支**進不來**：espNowDelay() 只跑 maintainEspNow()、
  // 不跑 loop()，而 GroupArmScope 的解構子保證 sendCmdToAll() 一離開就不可能
  // 留下 ARMED。留著是第三道防線。
  //
  // 若真的看到 ARMED，代表 inline 段已經失控 —— 此時繼續等 6 秒毫無價值，
  // 直接升級成 WAIT 讓補送機制接手（未送出的那幾台會在第 2、3 趟補回來），
  // 比「乾等 6 秒然後宣告收工」誠實也安全。
  if (groupJob.phase == GROUP_JOB_ARMED) {
    groupJob.phase = GROUP_JOB_WAIT;
    groupJob.waitUntil = now + GROUP_ACK_WAIT_MS;
    return;
  }

  groupRefreshRoster();

  if (groupJob.phase == GROUP_JOB_WAIT) {
    if ((long)(now - groupJob.waitUntil) < 0) return;

    int pending = groupCountPending();
    if (pending == 0 || groupJob.sweep >= GROUP_MAX_SWEEPS) {
      groupFinishJob(false);
      return;
    }
    groupJob.sweep++;
    groupJob.cursor = 0;
    groupJob.nextStepAt = now;
    groupJob.phase = GROUP_JOB_SWEEP;
    Serial.printf("[群組] 第 %d 趟補送：對 %d 台未取得 MAC 層 ACK 的重送單播\n",
                  groupJob.sweep, pending);
    return;
  }

  // ── GROUP_JOB_SWEEP：每次 loop() 最多送一台 ──
  if ((long)(now - groupJob.nextStepAt) < 0) return;

  // 掃描只讀快照，完全不碰 slaves[]／slaveCount（裁決二）
  while (groupJob.cursor < groupJob.count &&
         (groupDelivered[groupJob.cursor] || groupJob.gone[groupJob.cursor])) {
    groupJob.cursor++;
  }
  if (groupJob.cursor >= groupJob.count) {
    groupJob.phase = GROUP_JOB_WAIT;
    groupJob.waitUntil = now + GROUP_ACK_WAIT_MS;
    return;
  }

  groupSendUnicast(groupJob.cursor);
  groupJob.cursor++;
  groupJob.nextStepAt = now + GROUP_STEP_GAP;
}

// 送出「之前」先把 MAC 清單快照到區域陣列。
//
// 快照的必要性（複審的關鍵警告）：整個群組流程橫跨多輪 loop()，中間 WiFi task
// 的 HO_PKT_UNPAIR 分支隨時可能前移 slaves[]。全程只用這份快照，就不存在
// 「索引跑掉→補送給錯的那台」的可能。快照本身是一段沒有讓出 CPU 的緊湊迴圈。
void groupCmdSnapshot(uint8_t cmd, uint16_t pulseMs) {
  groupJob.phase = GROUP_JOB_IDLE;   // 舊的未完成工作直接作廢，最新的意圖才是對的
  groupAckArmed = false;

  int n = slaveCount;
  if (n < 0) n = 0;
  if (n > HO_ESPNOW_MAX_SLAVES) n = HO_ESPNOW_MAX_SLAVES;

  groupJob.count = n;
  for (int i = 0; i < n; i++) {
    memcpy(groupJob.macs[i], slaves[i].mac, 6);
    groupJob.gone[i] = false;
    groupDelivered[i] = false;
  }
  groupJob.cmd = cmd;
  groupJob.pulseMs = pulseMs;
  // 一道邏輯指令一個 cmdId。**必須在這裡配、不能在每次送出時配**，
  // 否則 3 次廣播 ＋ 每台單播 ＋ 兩趟補送會變成幾十道不同的指令，
  // slave 回報的 lastCmdId 永遠對不上最後一次送出的那個 —— 歸因整個失效。
  groupJob.cmdId = allocCmdId();
  groupJob.sweep = 1;          // 第 1 趟就是下面那段 inline 的「對每一台都送」
  groupJob.cursor = 0;
  groupJob.startedAt = millis();
  groupJob.everRan = true;

  // ── review 第 4 輪：「已啟動」的時點必須前移到 inline 第一趟單播之前 ──
  // 舊順序是「snapshot 設 IDLE → 廣播 → inline 單播 400ms → 才設 WAIT」，
  // 於是那 400ms 內 groupCmdActive() 全程是 false，導致：
  //   (a) groupNoteUnicastAck() 的新守衛會把第一趟 ACK 全丟掉（大規模誤紅）；
  //   (b) pollNextSlave() 與 handleSlaveCommand() 的兩道讓路守衛**其實是失效的**
  //       —— 它們之所以沒出事，只是因為 espNowDelay() 內部只跑 maintainEspNow()、
  //       不跑 loop() 也不跑 mqttClient.loop()，所以那兩條路徑實務上進不來。
  //       **那是巧合，不是設計。** 現在時點前移，守衛在 400ms 內是真的成立。
  // 必須排在所有欄位寫完之後：WiFi task 一看到非 IDLE 就會開始信任這份 job。
  // n == 0 時不進入 job（沒有任何一台要追），phase 維持 IDLE。
  if (n > 0) groupJob.phase = GROUP_JOB_ARMED;
}

void sendCmdToAll(HoRelayCmd cmd, uint16_t pulseMs) {
  // 先快照再印（review Mi3 更正）。原本這行排在 groupCmdSnapshot() **之前**、
  // 直接讀 volatile slaveCount，於是序列埠印的台數與這次實際處理的台數可能
  // 對不起來（WiFi task 在兩者之間動了名冊）。沒有安全後果 —— 送出迴圈本來
  // 就只讀快照 —— 但既然整個函式的原則是「先快照、之後只信快照」，
  // 這行也該遵守，否則現場除錯會拿到一個對不上的數字。
  // 輸出格式一字未動：docs/phase1-regression-checklist.md 第 8 項拿它當判準。
  groupCmdSnapshot((uint8_t)cmd, pulseMs);

  // ── review N-c：讓「不會卡在 ARMED」變成結構保證，而不是靠作者記得 ──
  // GROUP_JOB_ARMED 只在本函式內存在，而本函式**目前**只有一個 return
  //（空名冊早退，且它明確設 IDLE），所以卡住是不可達的。
  // 但那是「今天的程式碼剛好如此」，不是保證 —— 任何未來新增的 return
  //（例如在 inline 單播迴圈中間加一個錯誤處理）都會讓 job 永遠停在 ARMED，
  // 而 ARMED 期間 pollNextSlave() 是讓開的，只能等 6 秒 wall-clock 上限收拾。
  // 解構子必然執行，把出口責任從「作者記得寫」改成「離開作用域就會做」。
  // 正常路徑進到這裡時 phase 已是 WAIT 或 IDLE，解構子什麼都不做（零行為改變）。
  struct GroupArmScope {
    ~GroupArmScope() {
      if (groupJob.phase == GROUP_JOB_ARMED) {
        // 有人中途 return 才會成立：不留半成品，直接交給 loop() 的補送段。
        // 沒送出去的那幾台會在第 2、3 趟被補回來，比乾等 6 秒誠實也安全。
        Serial.println("⚠ [群組] inline 段未正常結束，直接交給補送段接手");
        groupJob.phase = GROUP_JOB_WAIT;
        groupJob.waitUntil = millis() + GROUP_ACK_WAIT_MS;
      }
    }
  } groupArmScope;

  Serial.printf("[控制] 廣播指令 %u 給 %d 台\n", (uint8_t)cmd, groupJob.count);

  HoCmdPayload payload;
  payload.cmd = (uint8_t)cmd;
  payload.pulseMs = pulseMs;
  // 廣播與後面每一次單播、補送都用同一個 cmdId（見 groupCmdSnapshot()）。
  // 因此一台 slave 就算「只收到廣播、單播全丟」，它回報的 lastCmdId 仍然對得上 ——
  // 這正是本次歸因最有價值的一種情形：**送達證明是紅的、執行證明卻是綠的**，
  // 代表廣播有效而單播的 ACK 掉了。兩種證據互補，不能互相取代。
  payload.cmdId = groupJob.cmdId;

  // ── 第 1 段：廣播（同時性）──
  // 連送 GROUP_BROADCAST_REPEAT 次、間隔 GROUP_BROADCAST_GAP：廣播沒有 ACK
  // 也沒有重傳，只能靠重送提高命中率。HO_CMD_OFF（關繼電器）與 HO_CMD_PULSE
  // （重觸發 2000ms 計時器）都是冪等的，重送安全。
  // 等待走 espNowDelay() 而非裸 delay()：整段最多 40ms，期間心跳照發。
  for (int i = 0; i < GROUP_BROADCAST_REPEAT; i++) {
    espNowSendTo(BROADCAST_MAC, HO_PKT_CMD, &payload, sizeof(payload));
    if (i < GROUP_BROADCAST_REPEAT - 1) espNowDelay(GROUP_BROADCAST_GAP);
  }

  // master 自己的繼電器也跟著動作（本機 GPIO，不需要無線送達證明）
  if (cmd == HO_CMD_ON) {
    setRelayPins(true);
  } else if (cmd == HO_CMD_OFF) {
    setRelayPins(false);
  } else if (cmd == HO_CMD_PULSE) {
    pulseRelay(pulseMs > 0 ? pulseMs : 2000);
  }

  if (groupJob.count <= 0) {
    // groupCmdSnapshot() 在 n == 0 時本來就沒有進入 job（phase 維持 IDLE），
    // 這裡再寫一次是明示不變式，不是修補。
    groupJob.phase = GROUP_JOB_IDLE;   // 名冊是空的，沒有東西要送
    Serial.println("[群組] 名冊是空的，只有 master 自己動作");
    return;
  }

  // ── 第 2 段：對**每一台**都送一次單播（唯一可證明的送達）──
  //
  // 這一趟刻意寫成 inline 而不是丟給 loop()（review Mi3）：
  // handleMasterCommand() 的 ALL:* 分支結尾就是 publishStatus()，而單次阻塞
  // publish 最壞是 10 秒級黑箱（App 端依賴那則 publishStatus()，不能拿掉）。
  // 若第一趟單播排在它後面，「一次要全部關」的補強會被整整延後 10 秒 ——
  // 那是這套系統最不能延後的動作。
  // 代價是最多 20 × 20ms ＝ 400ms 的 inline 阻塞，全程走 espNowDelay()，
  // 心跳與點動結束檢查都照跑，遠低於 30 秒門檻。
  //
  // 只讀快照，不讀 slaves[]／slaveCount（裁決二：舊寫法的迴圈上界每次迭代重讀
  // volatile slaveCount，中途陣列搬移會同時造成「跳過一台」與「提前結束」
  // ＝ alloff 少關一扇門）。
  for (int i = 0; i < groupJob.count; i++) {
    groupSendUnicast(i);
    if (i < groupJob.count - 1) espNowDelay(GROUP_STEP_GAP);
  }

  // 補送段交給 loop() 的 processGroupCmd()。這裡刻意**不印任何「成功」字樣**：
  // esp_now_send() 對廣播位址永遠回報成功，把它當成「已送達」就是假綠燈。
  groupJob.phase = GROUP_JOB_WAIT;
  groupJob.waitUntil = millis() + GROUP_ACK_WAIT_MS;
  Serial.printf("[群組] 已廣播 %d 次（廣播無 ACK，送出成功不代表任何一台收到），"
                "並對 %d 台各送一次單播，等 MAC 層 ACK\n",
                GROUP_BROADCAST_REPEAT, groupJob.count);
}

// 分散式輪詢：每次 loop() 呼叫最多只問一台，用「15000ms ÷ 台數」的間隔
// 平均分攤，一輪剛好 15 秒問完全部，且完全不阻塞 loop()。
// （舊版用 for 迴圈搭配 delay(20) 一次問完全部，20 台會阻塞 loop() 達
// 400ms，若與 master 自身點動的關閉時機重疊，會讓點動時間被拖長超過設定
// 值，故改為此設計，見 Task 6 review 修正 2）
void pollNextSlave() {
  // review 修正（M1，除零競態）：slaveCount 是 volatile，若在本函式往下讀取
  // 之間被 WiFi task 的 HO_PKT_UNPAIR 分支減到 0，下面的除法／取模會整數除零
  // 導致 panic 重開機。一開頭就快照成區域變數 n，函式全程只用 n，不再重新
  // 讀取 slaveCount（與 slaveStatusScheduler() 同一模式的修正）。
  int n = slaveCount;
  if (n <= 0) return;

  static unsigned long lastPollAt = 0;
  static int pollIdx = 0;

  // 下限 20ms：台數很多時避免間隔過短、無線封包擠在一起碰撞
  unsigned long interval = 15000UL / (unsigned long)n;
  if (interval < 20) interval = 20;

  unsigned long now = millis();
  if (now - lastPollAt < interval) return;
  lastPollAt = now;

  // slaveCount 可能因配對／解除配對中途變動，索引越界就重頭開始，
  // 不特別處理「跳過某台」，反正下一輪就會輪到
  if (pollIdx >= n) pollIdx = 0;
  requestSlaveStateIndex(pollIdx);
  pollIdx = (pollIdx + 1) % n;
}

void updateSlaveOnlineStatus() {
  unsigned long now = millis();
  for (int i = 0; i < slaveCount; i++) {
    bool wasOnline = slaves[i].online;
    bool isOnline = slaves[i].lastSeen > 0 &&
                    (now - slaves[i].lastSeen) < SLAVE_OFFLINE_TIMEOUT;
    if (wasOnline != isOnline) {
      char id[20];
      hoFormatDeviceId(slaves[i].mac, id);
      Serial.printf("[%s] %s（超過 %lu 秒沒回應即判離線）\n",
                    isOnline ? "上線" : "離線", id, SLAVE_OFFLINE_TIMEOUT / 1000);
      // 上下線翻轉一定要立刻代發，否則 App 最壞要等一整輪輪播才看到
      slaves[i].dirty = true;
    }
    slaves[i].online = isOnline;
  }
}

void unpairSlave(int idx) {
  if (idx < 0 || idx >= slaveCount) {
    Serial.println("[配對] 編號超出範圍");
    return;
  }
  // review 修正（M4）：先把 MAC 複製到區域變數，不要繼續依賴 idx。
  // publishSlaveOffline() 內部最壞會阻塞 10 秒（見 publishJsonDoc() 的更正
  // 說明），這段期間 WiFi task 若收到另一台 slave 主動送來的 HO_PKT_UNPAIR，
  // onEspNowRecv() 會並行搬移 slaves[] 陣列（loop() 與 WiFi task 是不同 task，
  // 會真的並行執行，不是誤會），讓這裡原本記住的 idx 失效。之後所有動作
  // 一律用這份 MAC，陣列搬移完成後再用 MAC 重新查一次 idx。
  uint8_t mac[6];
  memcpy(mac, slaves[idx].mac, 6);
  char id[20];
  hoFormatDeviceId(mac, id);

  // 解除配對前，先補發一則 status=offline 的保留訊息。少了這步，broker 上
  // 會永遠留著最後那則 "online" 保留訊息，App 上就多出一台永遠在線、
  // 卻怎麼控制都沒反應的幽靈設備。必須在陣列搬移之前呼叫，此時 idx
  // 還指向正確的這一台。
  publishSlaveOffline(idx);

  // Task 4：不再代理這一台，訂閱要一起收掉，否則 broker 會繼續把它的 control
  // 訊息推過來（mqttCallback() 雖然會用 findSlave() 擋掉，但那是防禦不是設計）。
  // 用本函式開頭複製的 mac 而不是 slaves[idx].mac：publishSlaveOffline() 可能
  // 阻塞數秒，期間 slaves[] 可能已被 WiFi task 搬移過，idx 不再可信（M4 修正）。
  unsubscribeSlaveControlTopic(mac);

  espNowSendTo(mac, HO_PKT_UNPAIR, nullptr, 0);
  // 給對方時間收到再刪 peer。改用 espNowDelay() 而非裸 delay(100)：
  // UNPAIRALL 會連續呼叫本函式，裸 delay() 期間一則心跳都發不出去，
  // 而 slave 超過 30 秒收不到心跳就強制關閉繼電器＝籠門被打開。
  espNowDelay(100);

  esp_now_del_peer(mac);

  // 用 MAC 重新查一次：上面 publishSlaveOffline() 阻塞期間，slaves[] 可能已經
  // 因為別台的並行解除配對而搬移過，不能再信任進入本函式時的 idx。
  int freshIdx = findSlave(mac);
  if (freshIdx < 0) {
    Serial.printf("[配對] %s 在等待期間已不在名冊上，視為已移除\n", id);
    return;
  }
  for (int i = freshIdx; i < slaveCount - 1; i++) slaves[i] = slaves[i + 1];
  slaveCount--;
  // review 修正（M3）：與 onEspNowRecv() 的 HO_PKT_UNPAIR 分支同一個理由 ——
  // 陣列前移會讓正在推進的訂閱對齊游標漏掉某一台，插旗讓 loop() 重跑一次。
  pendingSubscribeRefresh = true;
  saveSlaves();
  Serial.printf("[配對] 已移除 %s，剩 %d 台\n", id, slaveCount);
}

// ── UNPAIRALL：清空整份名冊，但必須分批 ──
// 一口氣跑完的版本是：`while (slaveCount > 0) unpairSlave(slaveCount - 1);`
// 20 台 × (espNowDelay(100) ＋ 一次 publishSlaveOffline())，而單次阻塞 publish
// 最壞是 10 秒級的黑箱，合計最壞 **超過 60 秒沒有心跳** —— 直接撞破 slave 的
// 30 秒失聯門檻，等於把所有已配對的籠門一次打開。這是「為了清名冊而開全部的門」，
// 完全不可接受。
// 改成每次 loop() 只拆一台：espNowDelay() 期間心跳照發，publish 又受
// mqttPublishBudgetUsed 名額管轄（每輪最多一次），全程心跳不中斷。
bool unpairAllPending = false;

void processUnpairAll() {
  if (!unpairAllPending) return;
  if (slaveCount <= 0) {
    unpairAllPending = false;
    Serial.println("[配對] 名冊已清空");
    publishStatus();
    return;
  }
  // 從最後一台往前拆：unpairSlave() 內部的陣列前移只會影響被刪那台之後的元素，
  // 拆最後一台就沒有任何元素需要搬移，剩下的內容全部保持原位。
  unpairSlave(slaveCount - 1);
}

void printSlaveList() {
  Serial.printf("── Slave 名冊（%d／%d）──\n", slaveCount, HO_ESPNOW_MAX_SLAVES);
  if (slaveCount == 0) {
    Serial.println("  （空）");
    return;
  }
  unsigned long now = millis();
  for (int i = 0; i < slaveCount; i++) {
    char id[20];
    hoFormatDeviceId(slaves[i].mac, id);
    bool online = slaves[i].lastSeen > 0 &&
                  (now - slaves[i].lastSeen) < SLAVE_OFFLINE_TIMEOUT;
    Serial.printf("  %d. %s  %s  rssi=%d\n",
                  i, id, online ? "在線" : "離線", slaves[i].rssi);
  }
}

// slave 版本號 → "1.0.0"。out 至少 16 bytes。
// 尚未回報過狀態的 slave（fwMajor/Minor/Patch 都是 0）填 "0.0.0"，
// 不留空字串 —— App 端的解析比較單純，寧可給一個明確的無效值。
void formatSlaveVersion(int idx, char* out, size_t outSize) {
  snprintf(out, outSize, "%u.%u.%u",
           slaves[idx].fwMajor, slaves[idx].fwMinor, slaves[idx].fwPatch);
}

// 把 slaves 陣列加進 master 的狀態 doc。
// 條目數量以 Task 1 的容量常數推算的上界為準；照**現行**數值
// （STATUS_BUF_SIZE=3584／STATUS_BASE_MAX_BYTES=728／SLAVES_KEY_OVERHEAD=11／
// SLAVE_ENTRY_MAX_BYTES=112）算出 maxEntries = (3584-1-728-11)/112 = 25
// （沿革見 SLAVE_ENTRY_MAX_BYTES 宣告處）。
// 25 ≥ HO_ESPNOW_MAX_SLAVES = 20，這條截斷路徑永遠走不到，
// 且已有 static_assert 在編譯期擋住「有人把 statusBuf 改小」。
// 保留執行期截斷的意義是：萬一真的走到，App 看得到 slaves_truncated、
// 序列埠也會告警，而不是靜默給出一份不完整的清單。
// 最近一次群組指令的摘要（review M2）。開機以來沒下過群組指令就整個不帶。
//
// ── 欄位語義的分界（Phase 4 Task 1 更新）──
// ack／noack／gone **嚴格限定在「送達」**；exed／exec 講的是「**韌體層**已執行」。
// 沒有任何一個欄位宣稱「門關了」—— 那在本協定下仍然無法證明。
// （這段原本寫「每一個欄位的語義都嚴格限定在『送達』」，加了 exed／exec 之後那句
//   已經不成立，一併改對。宣稱與事實不符是本專案的 A 族病灶，不能留。）
//   （**沒有 cid 欄位**：Phase 4 Task 1 一度把 cmdId 發成 "cid"，review M1 移除。
//     理由是 cmdId 正是偽造一封 HO_PKT_STATE 所需的 nonce，主動發到無認證的公開
//     broker 上是**沒有必要的洩漏**。誠實說清楚：**這不是有效的緩解** ——
//     偽造本來就需要射頻近距離，而近距離的攻擊者直接嗅探 master 廣播的
//     HO_PKT_CMD 明文就拿得到 cmdId。移除它只是不做白送，成本為零；
//     真正的緩解只有訊息鑑別。cmdId 仍會印在序列埠收工訊息裡供現場除錯。）
//   exed   有執行證明的台數（見 groupExecutedIdx()）。**這個數字可能在收工之後
//          才變大** —— slave 的回報是非同步的，晚到的證據會把紅的翻成綠的。
//   cmd    最近一次群組指令的 HoRelayCmd。**實際數值是 0=OFF 1=ON 2=PULSE**
//          （見 libraries/HoEspNow/src/HoEspNowProtocol.h 的 enum HoRelayCmd）。
//          這行原本寫「1=ON 2=OFF 3=PULSE」，三個都錯，Task 7 逐一對照 enum 後更正。
//          序列埠的 `[控制] 廣播指令 %u …`／`[群組] 指令 %u 收工…`／
//          `[控制] 送指令 %u 給 …` 印的都是同一組數值，所以 `alloff` 印的是
//          **0** 不是 2 —— 回歸清單引用這些字串時務必用對數字。
//   age_s  距離指令送出的秒數
//   busy   1 = 補送還在進行中，0 = 已收工（收工的數字才是定案）
//   n      快照台數
//   ack    單播拿到 MAC 層 ACK 的台數（**只是送達**）
//   noack  沒拿到的台數 —— 這就是 App 該顯示紅色的依據
//   gone   指令期間離開名冊的台數（同樣未送達）
//   exec   證據等級。Task 5 時固定 "unprovable"（協定版本 1 沒有歸因欄位，
//          「已執行」原理上無法證明）。**協定版本 2 起改成 "attributed"**：
//          exed 那幾台有 slave 自己產生的執行證明。
//
//          ⚠ **"attributed" 仍然不是「門關了」。** 它只到韌體層：
//          證明 slave 走完了 setRelayPins()／pulseRelay()，
//          不證明繼電器硬體動作、不證明籠門落下。
//
//          **舊 App 的相容性是安全的**：hoctrl 的 GroupExecEvidence.fromWire()
//          把任何非 "unprovable" 的值歸成 unrecognized，而那個列舉刻意沒有
//          「已證明」那一態 —— 所以舊 App 收到 "attributed" 只會繼續維持
//          「無法證明已執行」，是誤紅方向。**不會憑空長出綠燈路徑。**
void appendGroupResult(JsonDocument& doc) {
  if (!groupJob.everRan) return;
  int ack, noack, gone;
  groupCountAll(ack, noack, gone);

  JsonObject g = doc["group"].to<JsonObject>();
  g["cmd"] = groupJob.cmd;
  g["age_s"] = (uint32_t)((millis() - groupJob.startedAt) / 1000);
  g["busy"] = groupCmdActive() ? 1 : 0;
  g["n"] = groupJob.count;
  g["ack"] = ack;
  g["noack"] = noack;
  g["gone"] = gone;
  g["exed"] = groupCountExecuted();
  g["exec"] = "attributed";
}

// 查出某台 slave 在**最近一次**群組指令中的送達結果。
// 回傳 -1 = 這台不在最近一次群組指令的快照裡（例如指令之後才配對進來），
//        0 = 單播沒拿到 MAC 層 ACK（未送達），
//        1 = 單播拿到了 MAC 層 ACK（已送達，**但不代表繼電器動作了**）。
int groupDeliveryFor(const uint8_t mac[6]) {
  if (!groupJob.everRan) return -1;
  for (int i = 0; i < groupJob.count; i++) {
    if (memcmp(groupJob.macs[i], mac, 6) != 0) continue;
    if (groupJob.gone[i]) return 0;      // 離開名冊＝未送達，不是「不知道」
    return groupDelivered[i] ? 1 : 0;
  }
  return -1;
}

// 查出某台 slave 在**最近一次**群組指令中有沒有執行證明。
// 回傳 -1 = 這台不在最近一次群組指令的快照裡（欄位就不帶），
//        0 = **沒有證據**（不是「已確認沒執行」！回報可能還在路上），
//        1 = 有執行證明（slave 回報的 cmdId／種類對得上，且回報晚於指令送出）。
//
// 0 與 1 的不對稱是刻意的，語義寫在 groupExecutedIdx() 上方的「擋不住什麼」四項。
//
// ⚠ **與 "grp" 的關鍵差異：這個值會由 1 變回 0，"grp" 不會。**
// groupDelivered[] 是本次 job 的一次性快照旗標，設了就不再改；而執行證明是
// **即時比對 slaves[] 的目前值**，所以下列兩種情況會讓已經綠的那台翻回紅：
//   (a) 那台執行了**下一道**指令 → lastCmdId 換成新值 → 與本次 cmdId 不再相符
//   (b) 那台離開名冊（unpair／UNPAIRALL）→ findSlave() 回 −1
// 兩者都是**誤紅方向**（把綠翻紅），符合本專案的不對稱原則，但 App 端若拿它
// 做「一次關門是否成功」的定案判斷，必須以**收工當下**那一則 status 為準，
// 不能假設它單調遞增 —— exed 同理。
int groupExecutedFor(const uint8_t mac[6]) {
  if (!groupJob.everRan) return -1;
  for (int i = 0; i < groupJob.count; i++) {
    if (memcmp(groupJob.macs[i], mac, 6) != 0) continue;
    return groupExecutedIdx(i) ? 1 : 0;
  }
  return -1;
}

// ── review M2：把「未送達」帶進 MQTT ──
// 收工判定原本只存在於序列埠，App 沒有任何依據可以顯示紅色。
// 這裡在 master 狀態的 slaves 陣列每一筆加上 "grp"，並在 buildStatusDoc()
// 加上 "group" 摘要物件。
//
// **語義只有一個，且刻意取一個不會被誤讀成「已關門」的名字**：
//   "grp": 1 → 最近一次群組指令的單播對這台拿到了 MAC 層 ACK（封包已送達）
//   "grp": 0 → 沒拿到（未送達；或它在指令期間離開了名冊）
//   欄位不存在 → 這台不在最近一次群組指令的快照裡，或開機以來還沒下過群組指令
//
// **"grp": 1 不等於「門關了」。** 它只講封包送達。
//
// ── Phase 4 Task 1 新增 "exe"：執行證明（協定版本 2）──
//   "exe": 1 → 這台回報過「我執行的是本次群組指令的 cmdId」（見 groupExecutedIdx()）
//   "exe": 0 → **沒有證據**。不是「已確認沒執行」—— 回報可能還在路上、可能掉了
//   欄位不存在 → 這台不在最近一次群組指令的快照裡，或開機以來沒下過群組指令
//
// ⚠ **"exe" 會由 1 翻回 0，"grp" 不會**（那台執行了下一道指令、或離開名冊）。
// 兩者都是誤紅方向，但代表 "exe" **不是單調的** —— 定案要看收工當下那一則 status。
// 完整說明見 groupExecutedFor() 上方。
//
// **"exe": 1 也不等於「門關了」。** 它證明的是「slave 的韌體走完了繼電器動作那段
// 程式」，不證明繼電器硬體動作，更不證明籠門落下。完整的「擋不住什麼」清單寫在
// groupExecutedIdx() 上方，三項，請連著讀。
//
// **兩個欄位互補、不能互相取代**：一台只收到廣播、單播 ACK 全掉的 slave 會是
// `"grp":0,"exe":1`（送達沒證明、執行有證明）；反過來 `"grp":1,"exe":0` 代表
// 封包到了它的射頻但沒回報執行 —— 那才是最該現場去看的一台。
//
// 舊版 App 的相容性：`SlaveStatus.fromJson()` 只挑它認得的 key，多出來的
// 欄位會被忽略，不會解析失敗（hoctrl 的 lib/models/slave_status.dart）。
void appendSlavesArray(JsonDocument& doc) {
  const int maxEntries =
      (int)((STATUS_BUF_SIZE - 1 - STATUS_BASE_MAX_BYTES - SLAVES_KEY_OVERHEAD)
            / SLAVE_ENTRY_MAX_BYTES);

  int shown = slaveCount;
  if (shown > maxEntries) shown = maxEntries;

  JsonArray arr = doc["slaves"].to<JsonArray>();
  for (int i = 0; i < shown; i++) {
    char id[20];
    hoFormatDeviceId(slaves[i].mac, id);
    char ver[16];
    formatSlaveVersion(i, ver, sizeof(ver));

    JsonObject o = arr.add<JsonObject>();
    // id 與 ver 都是區域 char[]（非 const char*），ArduinoJson 會複製一份進 doc，
    // 離開本次迴圈後仍然有效。若改成 const char* 會只存指標而變成懸空指標。
    o["id"] = id;
    o["relay"] = slaves[i].relay ? 1 : 0;
    o["online"] = slaves[i].online;
    o["rssi"] = slaves[i].rssi;
    o["version"] = ver;
    int grp = groupDeliveryFor(slaves[i].mac);
    if (grp >= 0) o["grp"] = grp;
    int exe = groupExecutedFor(slaves[i].mac);
    if (exe >= 0) o["exe"] = exe;
  }

  if (shown < slaveCount) {
    doc["slaves_truncated"] = true;
    doc["slaves_shown"] = shown;
    Serial.printf("⚠ [MQTT] slaves 陣列被截斷：名冊 %d 台，只放得下 %d 台\n",
                  slaveCount, shown);
  }
}

// ── 序列埠測試指令（Phase 2 接上 MQTT 後仍保留，方便現場除錯）──
void printHelp() {
  Serial.println("── 可用指令 ──");
  Serial.println("  list          列出所有 slave");
  Serial.println("  pair          進入／離開配對模式");
  Serial.println("  on <n>        開啟第 n 台（n 為 list 的編號）");
  Serial.println("  off <n>       關閉第 n 台");
  Serial.println("  pulse <n>     點動第 n 台 2 秒");
  Serial.println("  allon         全部開啟（含 master 自己）");
  Serial.println("  alloff        全部關閉（含 master 自己）");
  Serial.println("  allpulse      全部點動 2 秒");
  Serial.println("  state <n>     要求第 n 台回報狀態");
  Serial.println("  unpair <n>    解除第 n 台配對");
  Serial.println("  unpairall     清空整份名冊（分批執行，每輪 loop 拆一台，心跳不中斷）");
  Serial.println("  ch <n>        測試用：切換 master 的 channel（1~13）");
  Serial.println("  droppeer <n>  測試用：刪掉第 n 台的 ESP-NOW peer 但保留名冊條目，");
  Serial.println("                 讓它「在名冊上卻送不出單播」（回歸清單 8e 用）；");
  Serial.println("                 不寫 NVS、不動名冊內容，重開機或重新配對即恢復");
  Serial.println("  fakeslaves <n> 測試用：把名冊灌成 n 台假 slave，實測容量（不寫 NVS；");
  Serial.println("                 灌入後到重開機前，pair／unpair 會被擋下，避免假 MAC 寫進 NVS）");
  Serial.println("  jsonsize      測試用：印出目前狀態 JSON 的實際大小");
  Serial.println("  help          顯示這份說明");
}

// 驗證序列埠指令的編號參數是否為合法非負整數。
// String::toInt() 對非數字輸入會靜默回傳 0，若不驗證，打錯字（如 on a）
// 會被誤當成「編號 0」直接執行，動物管制設備誤觸發繼電器等於誤關籠子，故視為安全性驗證。
bool parseIndexArg(const String& argStr, int& outIdx) {
  if (argStr.length() == 0) return false;
  for (unsigned int i = 0; i < argStr.length(); i++) {
    char c = argStr.charAt(i);
    if (c < '0' || c > '9') return false;  // 只接受純數字，不支援負號
  }
  outIdx = argStr.toInt();
  return true;
}

void handleSerialCommand(const String& line) {
  String cmd = line;
  cmd.trim();
  if (cmd.length() == 0) return;

  int spacePos = cmd.indexOf(' ');
  String verb = (spacePos < 0) ? cmd : cmd.substring(0, spacePos);
  String argStr = (spacePos < 0) ? "" : cmd.substring(spacePos + 1);
  argStr.trim();

  // 這些指令需要數字參數，其餘指令（list/pair/allon/alloff/allpulse/help）不受影響
  // ch 雖然不是編號而是 channel，但同樣要求純數字，沿用同一套驗證避免 String::toInt()
  // 對非數字輸入靜默回傳 0（例如 ch abc 誤觸發切到 channel 0）
  bool needsArg = (verb == "on" || verb == "off" || verb == "pulse" ||
                   verb == "state" || verb == "unpair" || verb == "ch" ||
                   verb == "fakeslaves" || verb == "droppeer");
  int arg = -1;
  if (needsArg && !parseIndexArg(argStr, arg)) {
    Serial.println("[指令] 參數必須是數字，例如：on 0（輸入 help 看說明）");
    return;
  }

  if (verb == "list") {
    printSlaveList();
  } else if (verb == "pair") {
    if (pairingMode) exitPairingMode(); else enterPairingMode();
  } else if (verb == "on") {
    sendCmdToSlaveIndex(arg, HO_CMD_ON, 0);
  } else if (verb == "off") {
    sendCmdToSlaveIndex(arg, HO_CMD_OFF, 0);
  } else if (verb == "pulse") {
    sendCmdToSlaveIndex(arg, HO_CMD_PULSE, 2000);
  } else if (verb == "allon") {
    sendCmdToAll(HO_CMD_ON, 0);
  } else if (verb == "alloff") {
    sendCmdToAll(HO_CMD_OFF, 0);
  } else if (verb == "allpulse") {
    sendCmdToAll(HO_CMD_PULSE, 2000);
  } else if (verb == "state") {
    // review 第 4 輪：這條缺了與 pollNextSlave()／handleSlaveCommand() 一致的
    // 守衛。它送的是**單播** HO_PKT_STATE_REQ，其 MAC 層 ACK 會落進
    // onEspNowSent()；若與群組單播的 ACK 歸因閂鎖撞在同一個 MAC 上，
    // 會把「查詢已送達」誤記成「群組指令已送達」（與 C1 同一類的錯誤歸因）。
    // 而這條特別要守：回歸清單 8a 的校準步驟正好教操作者用 state <n> 來判斷
    // 訊號有沒有調到不對稱可達的位置 —— **驗收程序本身會製造這個危害**。
    // 群組指令有 6 秒 wall-clock 硬上限，最多只擋這麼久。
    if (groupCmdActive()) {
      Serial.println("[指令] 群組指令進行中，稍後再試 state（避免污染送達判定）");
    } else {
      requestSlaveStateIndex(arg);
    }
  } else if (verb == "unpair") {
    unpairSlave(arg);
  } else if (verb == "unpairall") {
    // 與 MQTT 的 UNPAIRALL 同一條路徑：只插旗，實際拆除由 loop() 分批做
    if (slaveCount <= 0) {
      Serial.println("[配對] 名冊本來就是空的");
    } else {
      unpairAllPending = true;
      Serial.printf("[配對] 開始清空名冊，共 %d 台（每輪 loop 拆一台）\n", slaveCount);
    }
  } else if (verb == "ch") {
    // 測試用：手動切換 channel，模擬 Phase 2 連上不同路由器的情況
    if (arg >= 1 && arg <= 13) {
      esp_wifi_set_channel((uint8_t)arg, WIFI_SECOND_CHAN_NONE);
      currentChannel = (uint8_t)arg;
      // 同步 lastKnownChannel，避免 WiFi 仍是 WL_CONNECTED 時，下一次 loop()
      // 呼叫 onWifiChannelMayHaveChanged() 偵測到 primary != lastKnownChannel
      // 而誤判「AP 換頻」、意外把這個測試用的 channel 寫入 NVS（saveSlaveLockChannel()）。
      lastKnownChannel = (uint8_t)arg;
      Serial.printf("[channel] master 切換到 %d\n", arg);
      sendHeartbeatBurst();   // 立刻連發數次，讓正在輪掃的 slave 早點命中
    } else {
      Serial.println("channel 需在 1~13 之間");
    }
  } else if (verb == "droppeer") {
    // ── 測試專用（回歸清單 8e）：刪掉 ESP-NOW peer，但**保留名冊條目** ──
    // 這是唯一能用人手製造 N1 觸發前提的方法：該台仍在名冊上（findSlave() 成功、
    // 不會被標成 gone），但 esp_now_send() 會回 ESP_ERR_ESPNOW_NOT_FOUND，
    // 於是 sendCmdToSlaveMac() 回 false —— 正是 N1 需要的「送不出去卻開了閂」。
    // 之後在配對模式下短按該台按鈕，master 的 HO_PKT_PAIR_REQ 分支會無條件
    // registerPeer() 把 peer 補回來，隨後的單播就通了，N1 的症狀（grp 由 0 翻 1）
    // 才有機會現形。完整步驟見 docs/phase1-regression-checklist.md 的 8e。
    //
    // 與 fakeslaves 同一類：危險、只給實測用。但兩者有一個重要差別 ——
    // **droppeer 不動名冊內容**（不寫 slaves[]、不改 slaveCount、不碰 NVS，
    // 也不設 fakeSlavesActive），所以名冊的真相沒有被汙染，pair／unpair 照常可用。
    // 恢復方式：重新開機（setup() 的 registerAllPeers() 會從 NVS 名冊重建 peer 表），
    // 或在配對模式下重新短按該台的按鈕。
    if (arg < 0 || arg >= slaveCount) {
      Serial.println("[控制] 編號超出範圍");
    } else {
      // 先把 MAC 取值再動作：檢查與使用之間 WiFi task 的 HO_PKT_UNPAIR 可能前移
      // slaves[]，沿用 arg 會刪到別台的 peer（與全檔「索引隨時可能失效」同一慣例）。
      uint8_t mac[6];
      memcpy(mac, slaves[arg].mac, 6);
      char id[20];
      hoFormatDeviceId(mac, id);
      esp_err_t res = esp_now_del_peer(mac);
      Serial.printf("[測試] 已刪除 %s 的 ESP-NOW peer（名冊條目保留，未寫 NVS），"
                    "回傳 %d\n", id, res);
      Serial.println("⚠ [測試] 該台在重新配對或重開機之前收不到任何單播"
                     "（廣播仍收得到）；名冊真相未被更動，pair／unpair 照常可用");
      // 講清楚真正的傷害面：不是「全部關門失效」，而是**單台控制靜默失敗**。
      // 群組指令的主指令走廣播×3，所以那台的門仍然關得起來（只是不會有
      // MAC 層送達證明，會被誠實記成未送達）；真正壞掉的是 App 對它的個別
      // 開／關（handleSlaveCommand() 的單播），而且**不會有任何錯誤回饋**，
      // 一路靜默到重開機或重新配對為止。
      Serial.println("⚠ [測試] 傷害面：App 對這台的**個別開／關會靜默失敗**直到重開機；"
                     "群組關門仍有效（主指令走廣播），但它會被記成未送達");
    }
  } else if (verb == "fakeslaves") {
    fakeSlavesForCapacityTest(arg);
  } else if (verb == "jsonsize") {
    printStatusJsonSize();
  } else if (verb == "help") {
    printHelp();
  } else {
    Serial.printf("未知指令：%s（輸入 help 看說明）\n", verb.c_str());
  }
}

// ── ESP-NOW callbacks ──
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  HoPacketHeader header;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  if (!hoUnpackPacket(data, (size_t)len, &header, &payload, &payloadLen)) {
    // ── flag-day 告警：讓「協定版本不相容」看得見 ──
    // 不是這一段的話，v2 master 配 v1 slave 的現場症狀是「slave 全部離線、
    // 而且完全不知道為什麼」。節流成每 10 秒最多一行，避免洗版。
    //
    // **它擋不住什麼**：只是回報，不做任何補救 —— 那些 slave 照樣收不到心跳、
    // 30 秒後照樣觸發 setRelayPins(false)。也偵測不到「版本相同但欄位語義改了」。
    uint8_t theirVersion = 0;
    if (hoPeekVersionMismatch(data, (size_t)len, &theirVersion)) {
      static unsigned long lastVerWarn = 0;
      unsigned long nowWarn = millis();
      if (lastVerWarn == 0 || (nowWarn - lastVerWarn) >= 10000) {
        lastVerWarn = nowWarn;
        Serial.printf("⚠ [協定] 收到版本 %u 的封包，本機是版本 %u，全部丟棄；"
                      "master 與所有 slave 必須一起重燒\n",
                      theirVersion, (unsigned)HO_ESPNOW_VERSION);
      }
    }
    return;  // 非本系統封包或 CRC 不符，靜默丟棄
  }

  char senderId[20];
  hoFormatDeviceId(info->src_addr, senderId);
  Serial.printf("[ESP-NOW] 收到 type=0x%02x seq=%u 來自 %s rssi=%d\n",
                header.type, header.seq, senderId, info->rx_ctrl->rssi);

  if (header.type == HO_PKT_PAIR_REQ) {
    HoPairAckPayload ack;
    ack.channel = currentChannel;
    ack.longRange = longRangeEnabled ? 1 : 0;

    if (!pairingMode) {
      ack.accepted = 0;
      ack.reason = HO_PAIR_NOT_PAIRING;
      Serial.printf("[配對] 拒絕 %s：不在配對模式\n", senderId);
    } else if (!addSlave(info->src_addr)) {
      ack.accepted = 0;
      ack.reason = HO_PAIR_FULL;
      requestBlink(3, 400);   // 慢閃 3 下：拒絕（語義沿用 ho_slave1.ino 的「失敗閃 3 下」）
      Serial.printf("[配對] 拒絕 %s：已達 %d 台上限\n", senderId, HO_ESPNOW_MAX_SLAVES);
    } else {
      ack.accepted = 1;
      ack.reason = HO_PAIR_OK;
      requestBlink(3, 100);   // 快閃 3 下：接受（語義沿用 ho_slave1.ino 的「成功閃 3 下」）
      Serial.printf("[配對] 接受 %s，目前共 %d 台\n", senderId, slaveCount);
    }

    // 回覆前必須先註冊 peer，否則單播送不出去
    // 注意：一定要在 registerPeer() 之前記錄 wasPeer，且只在「原本就不是 peer」
    // 又被拒絕時才刪除。若沒有這個判斷，已配對成功的 slave 若因故重送
    // PAIR_REQ（例如剛好 master 不在配對模式），會被誤刪 peer 而收不到後續指令。
    bool wasPeer = esp_now_is_peer_exist(info->src_addr);
    registerPeer(info->src_addr);
    // ── review N-b：PAIR_ACK 是同 MAC 單播，送之前先關 ACK 歸因閂鎖 ──
    // 上一版把這條列為「擋不掉也不該擋」，那個結論**前提就錯了**：擋不掉的是
    // 「回覆配對請求」，但**歸因閂鎖是關得掉的**。少了這行，一台已配對的 slave
    // 在群組單播的 1~2ms 窗口內送 PAIR_REQ（ho_slave1 的 requestPairing() 沒有
    // 「已配對就不送」守衛），PAIR_ACK 的 MAC 層 ACK 就會被記成「群組指令已送達」。
    // 那台可能三趟 CMD 單播全掉卻顯示 "grp":1 —— 而「CMD 掉、其他單播通」正是
    // 訊號邊界的典型情境，不是憑空假設。
    // 方向安全：最壞只是把一台其實已送達的誤判成未送達（誤紅），補送會再試一次。
    // context 安全：本分支跑在 WiFi task，而 groupAckArmed 本來就是 volatile bool、
    // 本來就由 WiFi task 的 groupNoteUnicastAck() 寫，不新增競態面。
    groupAckArmed = false;
    espNowSendTo(info->src_addr, HO_PKT_PAIR_ACK, &ack, sizeof(ack));
    if (!ack.accepted && !wasPeer) {
      // 被拒絕又不是原本就存在的 peer：這只是為了送出 ACK 而暫時註冊，
      // 用完就刪，避免陌生設備灌爆 20 台的 peer 表上限。
      esp_now_del_peer(info->src_addr);
    }
    return;
  }

  if (header.type == HO_PKT_UNPAIR) {
    int idx = findSlave(info->src_addr);
    if (idx >= 0) {
      // slave 主動解除配對：記下 MAC，交給 loop() 補發最後一則 offline
      // （不能在這裡直接發 MQTT，見 pendingOfflineMac 宣告處的註釋）。
      // 必須在陣列搬移之前記錄，否則 processPendingUnpairPublish() 拿到的
      // 會是錯誤或已不存在的 MAC。
      memcpy(pendingOfflineMac, info->src_addr, 6);
      hasPendingOfflinePublish = true;
      // review 修正（M3）：陣列前移會讓「正在跑的訂閱對齊」漏掉某一台
      //（被移到已經走過的游標位置）。這裡順手插旗，讓 loop() 對齊完後
      // 再全量重跑一次。因為對齊已改成每輪一格的游標式（見
      // controlSubscribeScheduler()），重跑的成本只是多幾輪 loop()，
      // 不再是 21 次背靠背的阻塞寫入。
      pendingSubscribeRefresh = true;
      for (int i = idx; i < slaveCount - 1; i++) slaves[i] = slaves[i + 1];
      slaveCount--;
      esp_now_del_peer(info->src_addr);
      saveSlaves();
      Serial.printf("[配對] %s 已解除配對，剩 %d 台\n", senderId, slaveCount);
    }
    return;
  }

  if (header.type == HO_PKT_STATE && payloadLen >= sizeof(HoStatePayload)) {
    int idx = findSlave(info->src_addr);
    if (idx < 0) {
      Serial.printf("[狀態] 收到未配對設備 %s 的回報，忽略\n", senderId);
      return;
    }
    HoStatePayload st;
    memcpy(&st, payload, sizeof(st));

    // 只有「內容真的變了」才設 dirty，避免每 15 秒的例行輪詢回報都觸發一次
    // 額外代發（例行輪播本來就會發，重複發是浪費頻寬）。
    // 歸因換了一道指令也算變化：那是 App 端「這扇門動了沒」的依據，
    // 不設 dirty 的話最壞要等下一輪輪播（最長 15 秒）才會出現在 MQTT 上。
    bool cmdIdChanged = (slaves[idx].lastCmdId != st.lastCmdId);

    bool changed = (!slaves[idx].online) ||
                   (slaves[idx].relay != st.relay) ||
                   (slaves[idx].fwMajor != st.fwMajor) ||
                   (slaves[idx].fwMinor != st.fwMinor) ||
                   (slaves[idx].fwPatch != st.fwPatch) ||
                   cmdIdChanged;

    // lastCmdAt 只在 lastCmdId **換值**時更新，記的是「帶著這個 cmdId 的第一則回報
    // 何時到」。若每則回報都更新，一台在群組指令之前就執行過同一個 cmdId 的 slave
    // （只有 cmdId 撞號才可能）會被之後任何一則例行輪詢回報把時間推到指令之後，
    // 讓 groupExecutedIdx() 的時間條件失效 —— 那是誤綠方向。
    //
    // ── 下面四行的**寫入順序是刻意的**，不要重排 ──
    // 這裡是 WiFi task，groupExecutedIdx() 在 loop() 讀，兩者之間沒有鎖也沒有屏障
    //（沿用 slaves[] 既有的無鎖慣例）。所以必須讓**每一種撕裂讀都落在誤紅方向**：
    //
    //   lastCmdAt 先寫 → 等到 lastCmdId 讀起來已經等於本次 cmdId 時，時間戳一定
    //     已經是新的。反過來寫的話，會出現「id 已經對上、時間戳還是舊的」那一瞬間，
    //     時間條件反而擋掉一則真實證據（誤紅，安全但會產生假 FAIL 雜訊）。
    //   lastCmdId 次之 → 讀到舊 id ＝ 不匹配 ＝ 回 false（誤紅）。
    //   lastCmdKind 最後 → 讀到「新 id ＋ 舊 kind」時：若舊 kind 與本次不同就回
    //     false（誤紅）；若剛好相同則回 true，而此時 id 已經確實匹配、證據本來就
    //     成立，所以 true 是對的，不是誤綠。
    //   lastCmdCount 只是顯示用，不參與任何判斷，排最後。
    //
    // **它擋不住什麼**：這個順序只保證「撕裂讀不會誤綠」，**不保證原子性**，
    // 也完全不處理偽造 —— 一封偽造的 HO_PKT_STATE 走的是正常路徑，順序幫不上忙。
    if (cmdIdChanged) {
      slaves[idx].lastCmdAt = millis();
    }
    slaves[idx].lastCmdId = st.lastCmdId;
    slaves[idx].lastCmdKind = st.lastCmdKind;
    slaves[idx].lastCmdCount = st.lastCmdCount;

    slaves[idx].online = true;
    slaves[idx].rssi = info->rx_ctrl->rssi;
    slaves[idx].lastSeen = millis();
    slaves[idx].relay = st.relay;
    slaves[idx].fwMajor = st.fwMajor;
    slaves[idx].fwMinor = st.fwMinor;
    slaves[idx].fwPatch = st.fwPatch;
    // 這裡只設旗標，絕不在此發 MQTT ——
    // 本函式跑在 WiFi task，不是 loop() context，在這裡呼叫 PubSubClient
    // 等於從另一個 task 動同一個 socket，是明確的競態。
    if (changed) slaves[idx].dirty = true;

    Serial.printf("[狀態] %s relay=%u 版本=%u.%u.%u 運行=%lus rssi=%d\n",
                  senderId, st.relay, st.fwMajor, st.fwMinor, st.fwPatch,
                  (unsigned long)st.uptimeSec, info->rx_ctrl->rssi);
    // 指令歸因（協定版本 2）。只在 cmdId **換值**時印，例行輪詢的回報不會洗版。
    // 這行證明的是「那台的韌體走完了繼電器動作那段程式」，**不是**「門關了」。
    if (st.lastCmdId != HO_CMD_ID_NONE && cmdIdChanged) {
      Serial.printf("[歸因] %s 回報已執行 cmdId=%u 種類=%u 次數=%u\n",
                    senderId, st.lastCmdId, st.lastCmdKind, st.lastCmdCount);
    }
    return;
  }
}

// 送出回呼（**WiFi task context**，不是 loop()）。
// Task 5 review C1 起，這裡是「已送達」的**唯一**證據來源：單播有 MAC 層 ACK，
// 這個回呼會逐幀回報成功／失敗，且 wifi_tx_info_t::des_addr 帶著目的 MAC，
// 可以逐台歸因。廣播沒有 ACK、永遠回報成功，groupNoteUnicastAck() 內部會明確
// 把廣播位址排除掉。
void onEspNowSent(const wifi_tx_info_t* txInfo, esp_now_send_status_t status) {
  bool ok = (status == ESP_NOW_SEND_SUCCESS);
  if (!ok) {
    Serial.println("[ESP-NOW] 送出失敗");
  }
  groupNoteUnicastAck(txInfo != nullptr ? txInfo->des_addr : nullptr, ok);
}

// ── 送出封包 ──
bool espNowSendTo(const uint8_t mac[6], HoPacketType type,
                  const void* payload, size_t len) {
  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), type, txSeq++, payload, len);
  if (total == 0) {
    Serial.println("[ESP-NOW] 打包失敗");
    return false;
  }
  esp_err_t res = esp_now_send(mac, buf, total);
  if (res != ESP_OK) {
    Serial.printf("[ESP-NOW] esp_now_send 失敗: %d\n", res);
    return false;
  }
  return true;
}

// ── 心跳廣播 ──
void sendHeartbeat() {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);
  currentChannel = primary;

  HoHeartbeatPayload hb;
  hb.channel = currentChannel;
  hb.pairingMode = pairingMode ? 1 : 0;
  hb.longRange = longRangeEnabled ? 1 : 0;
  hb.slaveCount = (uint8_t)slaveCount;

  espNowSendTo(BROADCAST_MAC, HO_PKT_HEARTBEAT, &hb, sizeof(hb));

  // ── 心跳 log 降頻 ──
  // 發送頻率必須維持 1 秒（保證 slave 掃描一輪必定命中），但若每次都印，
  // 序列埠會被每秒一行的心跳洗版，人工照回歸清單逐項比對時根本看不到其他訊息。
  // 因此只降低「印出」頻率：平常每 HEARTBEAT_LOG_EVERY 次印一行；
  // 但 channel／配對模式／slave 台數任一項與上次印出時不同就立即印，狀態變化不會被吃掉。
  static int hbLogCounter = 0;
  static uint8_t lastLoggedChannel = 0xFF;   // 0xFF 為不可能值，確保開機第一次必定印
  static uint8_t lastLoggedPairing = 0xFF;
  static uint8_t lastLoggedSlaveCount = 0xFF;

  hbLogCounter++;
  bool changed = (hb.channel != lastLoggedChannel) ||
                 (hb.pairingMode != lastLoggedPairing) ||
                 (hb.slaveCount != lastLoggedSlaveCount);

  if (changed || hbLogCounter >= HEARTBEAT_LOG_EVERY) {
    hbLogCounter = 0;
    lastLoggedChannel = hb.channel;
    lastLoggedPairing = hb.pairingMode;
    lastLoggedSlaveCount = hb.slaveCount;
    Serial.printf("[心跳] channel=%u 配對模式=%s slave=%u\n",
                  hb.channel, hb.pairingMode ? "是" : "否", hb.slaveCount);
  }
}

// ── ESP-NOW 維持機制 ──
// Phase 2a 引入 WiFi/MQTT 後，連線流程有大量阻塞等待。
// slave 的失聯門檻是 30 秒，超過就會判定失聯、開始輪掃、並強制關閉繼電器。
// 所以所有等待都必須走這裡，讓心跳在阻塞期間照常發出。
void maintainEspNow() {
  static unsigned long lastBeat = 0;
  unsigned long now = millis();
  // WiFi 關聯期間改用加密間隔（理由見 HEARTBEAT_INTERVAL_ASSOC 的註釋）
  unsigned long interval = wifiAssociating ? HEARTBEAT_INTERVAL_ASSOC : HEARTBEAT_INTERVAL;
  if (now - lastBeat >= interval) {
    lastBeat = now;
    sendHeartbeat();
  }

  // 點動結束檢查併入這裡（review 修正）：loop() 原本把這段檢查排在 WiFi 連線管理
  // 之後，但 connectToWiFi() 最壞會阻塞 15 秒，期間 loop() 卡住、這段檢查完全不會跑，
  // 等於點動會被拖長到「阻塞時間＋原訂點動秒數」。maintainEspNow() 是所有阻塞等待
  // （connectToWiFi() 內的等待迴圈、espNowDelay()）都會呼叫到的地方，併入這裡才能
  // 確保無論卡在哪個阻塞點，繼電器都能準時關閉。
  if (pulseActive && (now - pulseStartTime) >= pulseDuration) {
    pulseActive = false;
    setRelayPins(false);
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

// channel 改變的當下連發數次心跳（設計規格要求）。
// 換 channel 後 slave 還停在舊 channel，得等 30 秒失聯門檻才開始輪掃；
// 連發能讓「剛好已在輪掃、正巧停在新 channel」的 slave 立刻命中，不必再等下一輪。
void sendHeartbeatBurst() {
  // 心跳 log 已降頻，連發的 4 次裡只有第一次（channel 剛變）會印，
  // 這裡另外印一行讓「連發確實有發生」在序列埠上仍然可驗證
  Serial.printf("[心跳] channel 已變更，連發 %d 次（間隔 %d ms）\n",
                HEARTBEAT_BURST_COUNT, HEARTBEAT_BURST_GAP);
  for (int i = 0; i < HEARTBEAT_BURST_COUNT; i++) {
    sendHeartbeat();
    if (i < HEARTBEAT_BURST_COUNT - 1) delay(HEARTBEAT_BURST_GAP);
  }
}

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
    // AP 不斷線就換頻（例如 DFS 或自動選台）也要更新 NVS 記錄，
    // 否則下次開機進 BLE 配網模式會切回一個已經過時的 channel。
    // 本函式只由 connectToWiFi() 成功分支與 loop() 的「WiFi 已連線」分支呼叫，
    // 兩處都保證 WiFi 已連線；序列埠的 ch <n> 測試指令走另一條路，但也會同步
    // lastKnownChannel（見 handleSerialCommand()），所以下一輪 loop() 不會因為
    // primary != lastKnownChannel 而誤判、意外把測試用的 channel 寫入 NVS。
    if (slaveCount > 0) saveSlaveLockChannel(primary);
    sendHeartbeatBurst();
  }
}

// 開機時若「這次開機不會關聯 WiFi」（沒有 WiFi 設定 → 進 BLE 配網模式），
// 主動把 ESP-NOW 切回 NVS 記住的 AP channel。
//
// 為什麼需要（review 抓到的 Critical）：master 已連上 channel 6 的 AP 並配對多台
// slave，使用者送 reset → 重開機 → 沒有 WiFi 設定 → setupEspNow() 的
// WiFi.mode(WIFI_STA) 把 channel 歸 1 → 進入 BLE 配網模式。而
// onWifiChannelMayHaveChanged() 只在 connectToWiFi() 成功分支與 loop() 的
// 「WiFi 已連線」分支被呼叫，BLE 模式下兩者都不會執行，master 就永久停在 channel 1。
// 原本鎖在 channel 6 的 slave 從此收不到任何心跳 → 30 秒後全部失聯、強制關閉繼電器、
// 開始輪掃，整個配網過程（可能數分鐘）籠子都是開的。
//
// 呼叫時機限制：必須在 setupEspNow() 之後（esp_wifi_set_channel() 要 WiFi 已初始化
// 才有效），但 lastApChannel 的值來自 loadNetConfig()，而它排在 setupEspNow() 之前，
// 兩者順序在 setup() 裡已經滿足。
void restoreEspNowChannelForOfflineBoot() {
  if (slaveCount <= 0) {
    // 名冊是空的，沒有 slave 在等心跳，切 channel 沒有意義
    return;
  }
  if (slaveLockChannel < 1 || slaveLockChannel > 13) {
    // 這台 master 從沒成功連上過 WiFi（或剛升級韌體、NVS 還沒這個鍵），
    // 沒有歷史 channel 可用，只能停在 WIFI_STA 預設的 channel 1。
    // 此時已配對的 slave 若鎖在別的 channel，仍會失聯並開始輪掃（約 16 秒重鎖到 1）。
    Serial.printf("⚠ [channel] 名冊有 %d 台 slave，但 NVS 沒有 channel 記錄，"
                  "只能停在 channel %u；鎖在其他 channel 的 slave 會先失聯再輪掃回來\n",
                  slaveCount, currentChannel);
    return;
  }
  esp_wifi_set_channel(slaveLockChannel, WIFI_SECOND_CHAN_NONE);
  currentChannel = slaveLockChannel;
  lastKnownChannel = slaveLockChannel;
  Serial.printf("[channel] 本次開機不關聯 WiFi，切回 NVS 記住的 channel=%u，"
                "維持 %d 台已配對 slave 的心跳\n", slaveLockChannel, slaveCount);
  sendHeartbeatBurst();
}

// ESP-NOW 友善的 WiFi 連線
// 與 ho_relay2 的差異：不掃描頻道、不關閉 WiFi 驅動、等待迴圈走 maintainEspNow()。
// 最壞阻塞約 15 秒，期間心跳照常發出（見 review 修正：maxWaitMs 由 30000 降到 15000）。
//
// 已知限制（裁決不補）：不像 ho_relay2 那樣針對不同 auth mode 重試多種連線方式，
// 遇到需要特殊退避流程的路由器可能連不上。理由見 ho_master1/readme.md「已知限制」章節——
// 補回那套退避會讓阻塞時間再拉長數倍，與這裡壓縮阻塞時間的修正方向直接衝突。
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

  // 進入關聯階段：心跳改用加密間隔，直到本次關聯結束（成功或失敗）
  wifiAssociating = true;

  // WiFi.begin(ssid, password) 不指定 channel/BSSID 時，ESP-IDF 底層關聯流程仍會自己
  // 全頻道掃描一輪（約 1.5 秒、一輪約 20 秒），這正是我們想擋掉的行為，只是被包在
  // begin() 裡面。三段式優先序：
  //   1. 有 channel＋BSSID → 直接定向關聯，完全不掃描（最快、對 ESP-NOW 最友善）
  //   2. 只有 channel（BSSID 已在前次失敗時清掉）→ WiFi.begin(ssid, pass, ch, nullptr)。
  //      依 ESP-IDF 文件（wifi_sta_config_t.channel 註解：「Set to 1~13 to scan
  //      starting from the specified channel before connecting to AP」），這是
  //      「以指定 channel 起始掃描」，不是「鎖定在該 channel」。配合 Arduino core
  //      預設的 WIFI_FAST_SCAN（找到 SSID 即停）：AP 剛好在該 channel 時會一擊命中、
  //      完全跳過後續掃描；但 AP 不在該 channel 時，依文件字面意思仍可能從該 channel
  //      續掃其餘頻道 —— 這個機制細節只有文件推導、未經實機驗證，待確認。真正的安全網
  //      是本函式失敗分支的 channel 復位（必中）＋關聯期 200ms 加密心跳，兩層疊加下
  //      即使續掃真的發生，理論上也不會出現 30 秒空窗。
  //   3. 兩者都沒有（開機第一次，或連續失敗到判定 AP 真的換頻了）→ 才退回全頻掃描
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

  // 等待最多 15 秒，期間持續發心跳（關聯中心跳自動加密到 200ms，見 maintainEspNow()）
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
    // 兩個 NVS 記錄都更新：apch 給下次 WiFi 關聯用（reset 會清），
    // espch 給下次開機不關聯 WiFi 時維持 ESP-NOW 心跳用（跟著名冊走，reset 不清）
    saveApChannel(lastApChannel);
    if (slaveCount > 0) saveSlaveLockChannel(lastApChannel);
    onWifiChannelMayHaveChanged();
  } else {
    Serial.printf("[WiFi] 連線失敗，狀態=%d 原因碼=%d\n",
                  WiFi.status(), lastWifiDisconnectReason);
    // 只清 BSSID，刻意「不」跟著清掉 lastApChannel。
    // 舊版兩個一起清，導致路由器斷電 60 秒這種情境下，第 1 次重試還是單 channel 定向
    // 關聯（安全），第 2 次起就全部退回全頻掃描，master 每輪約 20 秒跑遍 1~13，
    // 停在舊 channel 的 slave 每則心跳命中率只剩約 1/13，30 秒 30 則全數落空的機率
    // 約 9% —— 落空的後果是繼電器被強制打開，對動物管制設備不可接受。
    if (haveLastApBssid) {
      Serial.println("[WiFi] 指定 BSSID 關聯失敗，清除 BSSID 記錄，下次改為只鎖定 channel 掃描");
      haveLastApBssid = false;
    }
    // 只有連續失敗到門檻，才判定「AP 真的換頻了」，升級成一次全頻掃描重新學習 channel。
    // NVS 裡的 apch 刻意不同步清除：那份值只在下次開機、且沒有 WiFi 設定時用來維持
    // ESP-NOW 心跳，就算 AP 已換頻，停在舊 channel 也遠優於一律停在 channel 1。
    if (lastApChannel != 0) {
      wifiChannelLockFailCount++;
      if (wifiChannelLockFailCount >= WIFI_CHANNEL_LOCK_MAX_FAIL) {
        wifiChannelLockFailCount = 0;
        Serial.printf("[WiFi] 已連續 %d 次在 channel %u 上關聯失敗，下次改為全頻掃描重新學習\n",
                      WIFI_CHANNEL_LOCK_MAX_FAIL, lastApChannel);
        lastApChannel = 0;
      }
    }

    // 關聯失敗後，射頻停在哪個 channel 是底層掃描流程的殘留狀態，沒有保證。
    // 走過全頻掃描的那一次尤其危險：可能停在 channel 13，而接下來
    // loop() 的重試節流（每 5 秒；連續失敗超過 10 次更會暫停整整 60 秒）期間，
    // 心跳會一直打在錯的頻道上 —— 60 秒的暫停遠大於 slave 的 30 秒失聯門檻。
    // 因此主動把 channel 切回 slave 鎖定的位置，兩次關聯嘗試之間的心跳才有意義。
    if (slaveCount > 0 && slaveLockChannel >= 1 && slaveLockChannel <= 13) {
      uint8_t primary = 0;
      wifi_second_chan_t second;
      esp_wifi_get_channel(&primary, &second);
      if (primary != slaveLockChannel) {
        esp_wifi_set_channel(slaveLockChannel, WIFI_SECOND_CHAN_NONE);
        currentChannel = slaveLockChannel;
        lastKnownChannel = slaveLockChannel;
        Serial.printf("[channel] 關聯失敗後射頻停在 %u，切回 slave 鎖定的 %u 再繼續發心跳\n",
                      primary, slaveLockChannel);
        sendHeartbeatBurst();
      }
    }
  }
}

// ── Task 4：control topic 的解析與訂閱管理 ──
//
// 解析 "hoban/hoban-<12 位 hex>/control"，成功則把 MAC 寫進 outMac。
//
// 為什麼不預先產生 21 條 topic 字串再逐條 strcmp：那要 21 × 33 = 693 bytes RAM，
// 而且配對／解除配對時得同步維護，多出一份可能與名冊不一致的狀態。
// 名冊已經是唯一真相，再複製一份就是在製造 bug。
// 解析是 O(1)（純長度檢查 + hex 解析），之後查名冊最多 20 次 6 bytes 的 memcmp
// ＝ 微秒級，而且只在收到 MQTT 訊息時才跑（App 按按鈕才發生，頻率以次／分鐘計）。
static const char CONTROL_TOPIC_PREFIX[] = "hoban/";
static const char CONTROL_TOPIC_SUFFIX[] = "/control";
static const size_t DEVICE_ID_LEN = 18;   // "hoban-" + 12 位 hex

bool parseControlTopic(const char* topic, uint8_t outMac[6]) {
  if (topic == nullptr) return false;
  const size_t prefixLen = sizeof(CONTROL_TOPIC_PREFIX) - 1;   // 6
  const size_t suffixLen = sizeof(CONTROL_TOPIC_SUFFIX) - 1;   // 8
  if (strlen(topic) != prefixLen + DEVICE_ID_LEN + suffixLen) return false;
  if (strncmp(topic, CONTROL_TOPIC_PREFIX, prefixLen) != 0) return false;
  if (strcmp(topic + prefixLen + DEVICE_ID_LEN, CONTROL_TOPIC_SUFFIX) != 0) return false;

  char id[DEVICE_ID_LEN + 1];
  memcpy(id, topic + prefixLen, DEVICE_ID_LEN);
  id[DEVICE_ID_LEN] = '\0';
  return hoParseMacFromDeviceId(id, outMac);   // 這個函式會驗 "hoban-" 前綴與 hex 合法性
}

// 逐台訂閱，不用萬用字元 hoban/+/control。
// 理由：那些 broker 是公用的（emqx.io、hivemq.com、eclipseprojects.io），
// hoban/+/control 會收到全世界所有 hoban 設備的控制訊息 —— 我們雖然會過濾掉，
// 但流量與被動接收他人指令的風險完全不必要。
//
// ── review 修正（M1）：訂閱同樣受「每輪 loop() 只做一次阻塞 socket 寫入」管轄 ──
// 第一版把 21 條 topic 在單次呼叫裡一口氣訂完。subscribe() 與 publish() 走的是
// 同一條 NetworkClient::write()，卡住時是同一個 10 秒級黑箱，21 次背靠背 ＝
// 單輪 loop() 最壞凍結約 210 秒。心跳因為有 maintainEspNow() 括號所以不會斷
//（籠門不會被誤開），但這 210 秒內 master **收不到也發不出任何 MQTT 指令** ——
// 本系統的核心需求是「一次要全部關」，關不了就是動物逃脫。
//
// 因此 mqttPublishBudgetUsed 的語義自本次起**擴大為「本輪 loop() 的阻塞式
// socket 寫入名額」**，涵蓋 publish／subscribe／unsubscribe 三者（旗標名稱維持
// Task 3 取的名字，避免跨文件的指涉斷裂）。訂閱改成游標式，由 loop() 的
// controlSubscribeScheduler() 每輪推進一格，與代發狀態共用同一個名額。
// 健康的網路下每輪 loop() 是毫秒級，21 格在數十毫秒內走完，與一口氣訂完
// 沒有可感知的差別；病態 socket 下則自動退化成「每輪一次」，不再凍結整輪。

// 訂閱對齊游標：-1 ＝ 沒有待辦；0 ＝ 下一格要訂 master 自己；
// 1..N ＝ 下一格要訂名冊第 (cursor-1) 台。只在 loop() context 讀寫，不需 volatile。
int subscribeCursor = -1;

// 呼叫前後夾 maintainEspNow()，理由與 publishJsonDoc() 相同：
// 進入 10 秒級黑箱前剛發過心跳、出來立刻再發一次。
// 名額由呼叫方（controlSubscribeScheduler()）負責取得，本函式不重複檢查。
void subscribeSlaveControlTopic(int idx) {
  if (idx < 0 || idx >= slaveCount) return;
  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);
  String t = String("hoban/") + id + "/control";
  maintainEspNow();
  bool res = mqttClient.subscribe(t.c_str());
  maintainEspNow();
  if (res) {
    Serial.printf("[代理] 已訂閱 %s\n", t.c_str());
  } else {
    Serial.printf("⚠ [代理] 訂閱失敗 %s\n", t.c_str());
  }
}

// 取消訂閱受「每輪 loop() 只做一次阻塞式 socket 寫入」的名額管轄
// （review M1 的同一個不變式），名額已被用掉就放棄，不硬擠第二次阻塞寫入。
//
// ── 敘述更正（Task 4 複審建議 (a)，Task 5 順手改）──
// 這段原本寫成「取消訂閱是**盡力而為**」，暗示「有時退得成、有時退不成」。
// **實際上必定退不成**：目前兩個呼叫端（unpairSlave()、
// processPendingUnpairPublish()）在呼叫本函式之前都**一定**先做過一次 offline
// publish，那次 publish 必然已經佔走本輪的名額，所以本函式 **100% 走進下面的
// 「略過」分支**，一次都不會真的送出 unsubscribe。
//
// 安全性不受影響（這一點複審已逐環節確認），理由不變：
//   (a) handleSlaveCommand() 進來前 mqttCallback() 會用 findSlave() 擋掉
//       已不在名冊上的目標，殘留訂閱收到的訊息不會造成任何 ESP-NOW 動作；
//   (b) 下次重連時 mqttClient.connect(...) 的 cleanSession 傳 true，
//       broker 不保留舊 session 的訂閱清單，殘留自然消失（殘留有上限）。
// 保留本函式與那行序列埠輸出的意義：未來若有「不先 publish」的呼叫端加進來，
// 這條路徑立刻就會生效；現場除錯也看得出「這次沒退成」。
void unsubscribeSlaveControlTopic(const uint8_t mac[6]) {
  if (!mqttClient.connected()) return;
  char id[20];
  hoFormatDeviceId(mac, id);
  String t = String("hoban/") + id + "/control";

  if (mqttPublishBudgetUsed) {
    Serial.printf("[代理] 本輪 socket 名額已用掉，略過取消訂閱 %s\n", t.c_str());
    return;
  }
  mqttPublishBudgetUsed = true;

  maintainEspNow();
  mqttClient.unsubscribe(t.c_str());
  maintainEspNow();
  Serial.printf("[代理] 已取消訂閱 %s\n", t.c_str());
}

// 請求「把 master 自己與名冊上每一台的 control topic 重新對齊一次」。
// **本函式不會當場送出任何 subscribe**，只是把游標歸零排隊，實際動作由
// loop() 的 controlSubscribeScheduler() 每輪做一格（理由見上方 M1 的說明）。
// 重複訂閱同一個 topic 對 broker 是冪等的，所以「全量重訂」是安全的對齊方式，
// 不必記錄是哪一台變動。
void subscribeAllControlTopics() {
  subscribeCursor = 0;
}

// 訂閱對齊排程器：每輪 loop() 最多做一次 subscribe，佔用與 publish 相同的名額。
//
// 關於「在 mqttClient.loop() 的 callback 裡呼叫 subscribe() 是否會踩壞
// PubSubClient 的收包 buffer」（Task 4 review 已查證，結論是**不會**，
// 記在這裡免得未來又有人重新擔心一次）：
//   - FIND_BEST_SERVER 這條路徑會在 mqttCallback() 內一路走到 quickConnect*()，
//     而 topic／payload 指標是直接指進 PubSubClient 的 this->buffer 的。
//   - 但 PubSubClient::subscribe(topic) 預設 QoS 0（PubSubClient.cpp:605-607），
//     所以 PubSubClient::loop() 裡的 PUBACK 分支（:402-413）根本走不到；
//   - 就算走到，msgId 是在 :403、**呼叫 callback 之前**就存進區域變數的，
//     callback 返回後那段是**寫入** buffer[0..3] 組 PUBACK，不是讀取，
//     不依賴 callback 期間 buffer 的內容。
//   - 另外 mqttCallback() 本身在做任何呼叫之前就已把 payload 複製進 message、
//     也已解析完 topic，之後不再碰那兩個指標。
void controlSubscribeScheduler() {
  if (subscribeCursor < 0) return;
  if (!mqttClient.connected()) {
    // 斷線就放棄整輪對齊：重連是 cleanSession，會由 quickConnect*() 重新排隊。
    subscribeCursor = -1;
    return;
  }
  if (mqttPublishBudgetUsed) return;   // 讓位給下一輪

  // 與 slaveStatusScheduler()／pollNextSlave() 同一個 M1 快照模式：slaveCount 是
  // volatile，函式執行中途可能被 WiFi task 減少，全程只用這份快照。
  int n = slaveCount;

  if (subscribeCursor == 0) {
    String own = String("hoban/") + getDeviceId() + "/control";
    mqttPublishBudgetUsed = true;
    maintainEspNow();
    bool res = mqttClient.subscribe(own.c_str());
    maintainEspNow();
    // review 修正（M4）：以前這裡丟掉回傳值、無條件印「已訂閱」。而這一行正是
    // 回歸清單第 5／9a／11 項的通過判準 —— 訂閱失敗時實測者會把破口判成 PASS。
    // 失敗一律印 ⚠ 並且**不印**「已訂閱」。
    if (res) {
      Serial.printf("[MQTT] 已訂閱 %s\n", own.c_str());
    } else {
      Serial.printf("⚠ [MQTT] 訂閱失敗 %s（master 自己的 control topic，"
                    "此時 App 的指令收不到）\n", own.c_str());
    }
    subscribeCursor = 1;
    return;
  }

  int idx = subscribeCursor - 1;
  if (idx >= n) {
    subscribeCursor = -1;   // 全部走完
    return;
  }
  mqttPublishBudgetUsed = true;
  subscribeSlaveControlTopic(idx);
  subscribeCursor++;
}

// ── MQTT 連線 ──
// 連線到指定的預設伺服器
bool quickConnectToIndex(int index) {
  if (index < 0 || index >= DEFAULT_SERVER_COUNT) return false;
  const MqttServerConfig& cfg = DEFAULT_SERVERS[index];

  mqttClient.setServer(cfg.server, cfg.port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  // Phase 2b：buffer 由 1024 擴到 MQTT_BUFFER_SIZE，才放得下 20 台的 slaves 陣列。
  // setBufferSize() 內部是 realloc，heap 不足會回 false 而 buffer 停在舊大小，
  // 之後每一次大狀態發布都會靜默失敗，所以一定要檢查。
  if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
    Serial.printf("⚠ [MQTT] setBufferSize(%u) 失敗，buffer 仍為 %u，"
                  "帶 slaves 陣列的狀態將無法發布\n",
                  (unsigned)MQTT_BUFFER_SIZE, (unsigned)mqttClient.getBufferSize());
  }
  mqttClient.setSocketTimeout(3);

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  JsonDocument willDoc;
  willDoc["device_id"] = deviceId;
  willDoc["status"] = "offline";
  willDoc["server"] = cfg.server;
  willDoc["timestamp"] = millis() / 1000;
  char willBuf[192];
  size_t willLen = serializeJson(willDoc, willBuf, sizeof(willBuf));
  if (willLen >= sizeof(willBuf) - 1) {
    Serial.println("⚠ [MQTT] LWT JSON 被截斷，改用最小 will");
    snprintf(willBuf, sizeof(willBuf), "{\"device_id\":\"%s\",\"status\":\"offline\"}", deviceId);
  }

  Serial.printf("[MQTT] 嘗試 %s …\n", cfg.server);
  bool res = mqttClient.connect(deviceId, cfg.username, cfg.password,
                                statusTopic.c_str(), 1, true, willBuf, true);
  if (!res) {
    Serial.printf("[MQTT] %s 失敗，state=%d\n", cfg.server, mqttClient.state());
    return false;
  }

  currentServerIndex = index;
  usingCustomServer = false;
  Serial.printf("[MQTT] 已連線 %s\n", cfg.server);
  // Task 4：master 自己的 control topic 與名冊上每一台的都要訂（見該函式註釋）
  subscribeAllControlTopics();
  publishStatus();
  // 重新連上 broker（可能是換了一台伺服器）後，新 broker 上完全沒有這些 slave 的
  // 保留訊息，必須把整份名冊重壓一次，否則 App 端要等輪播自然轉到才看得到
  // （最壞一整輪 SLAVE_STATUS_CYCLE_MS）。標記 dirty 而不是當場連發 21 則，
  // 理由見 slaveStatusScheduler() 上方對背靠背發布的分析。
  markAllSlavesDirty();
  return true;
}

// 連線到使用者自訂伺服器。結構與 quickConnectToIndex() 相同，差別：
// 用 mqttServer/mqttPort，帳密有值才傳、否則傳 NULL，且不更新 currentServerIndex
// （currentServerIndex 只追蹤「輪詢到預設清單的第幾台」，自訂伺服器不佔用這個游標，
// 避免自訂伺服器斷線後，預設清單的輪詢起點被誤導到不相關的位置）。
bool quickConnectCustom() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  // Phase 2b：buffer 由 1024 擴到 MQTT_BUFFER_SIZE，才放得下 20 台的 slaves 陣列。
  // setBufferSize() 內部是 realloc，heap 不足會回 false 而 buffer 停在舊大小，
  // 之後每一次大狀態發布都會靜默失敗，所以一定要檢查。
  if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
    Serial.printf("⚠ [MQTT] setBufferSize(%u) 失敗，buffer 仍為 %u，"
                  "帶 slaves 陣列的狀態將無法發布\n",
                  (unsigned)MQTT_BUFFER_SIZE, (unsigned)mqttClient.getBufferSize());
  }
  mqttClient.setSocketTimeout(3);

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  JsonDocument willDoc;
  willDoc["device_id"] = deviceId;
  willDoc["status"] = "offline";
  willDoc["server"] = mqttServer;
  willDoc["timestamp"] = millis() / 1000;
  char willBuf[192];
  size_t willLen = serializeJson(willDoc, willBuf, sizeof(willBuf));
  if (willLen >= sizeof(willBuf) - 1) {
    Serial.println("⚠ [MQTT] LWT JSON 被截斷，改用最小 will");
    snprintf(willBuf, sizeof(willBuf), "{\"device_id\":\"%s\",\"status\":\"offline\"}", deviceId);
  }

  const char* user = (strlen(mqttUsername) > 0) ? mqttUsername : NULL;
  const char* pass = (strlen(mqttPassword) > 0) ? mqttPassword : NULL;

  Serial.printf("[MQTT] 嘗試自訂伺服器 %s …\n", mqttServer);
  bool res = mqttClient.connect(deviceId, user, pass,
                                statusTopic.c_str(), 1, true, willBuf, true);
  if (!res) {
    Serial.printf("[MQTT] 自訂伺服器 %s 失敗，state=%d\n", mqttServer, mqttClient.state());
    return false;
  }

  usingCustomServer = true;
  Serial.printf("[MQTT] 已連線自訂伺服器 %s\n", mqttServer);
  // Task 4：理由與 quickConnectToIndex() 完全相同
  subscribeAllControlTopics();
  publishStatus();
  markAllSlavesDirty();
  return true;
}

// ── smartConnect() 的輪詢游標 ──
// 語義維持不變：自訂伺服器優先，然後從上次成功的位置（currentServerIndex）輪詢預設清單。
// 差別在於「一次呼叫只嘗試一台」，剩下的交給 loop() 既有的 10 秒重連節奏推進。
//
// 為什麼非改不可（review 抓到的 Critical）：單次 mqttClient.connect() 對不可達目標
// 最壞約 18 秒 —— NetworkClient::connect(host, port) 會先做 Network.hostByName()
// → getaddrinfo()，這段完全沒有 timeout 參數，由 lwIP 的 DNS_MAX_RETRIES 指數退避
// 決定，約 15 秒；WIFI_CLIENT_DEF_CONN_TIMEOUT_MS = 3000 只管 TCP select()、
// setSocketTimeout(3) 只管 CONNACK 等待。而 mqttClient.connect() 是不可中斷的阻塞
// 呼叫，期間 maintainEspNow() 完全不會被叫到，心跳整段停擺。
// 舊寫法在自訂伺服器失敗後「立刻」接第一台預設伺服器，兩次背靠背 = 36 秒 > slave 的
// 30 秒失聯門檻，slave 會強制關閉繼電器＝籠子被打開。
// 觸發條件很寫實：AP 正常但 WAN 斷線／DNS 不回應（路由器斷網、ISP 中斷），
// 此時 WiFi 仍是 WL_CONNECTED，loop() 每 10 秒進來一次，每次都製造 36 秒的心跳真空。
// 改成一次一台後，單次呼叫最壞阻塞降到約 18 秒（< 30 秒），且兩次呼叫之間必定隔著
// loop() 的 10 秒節流，足以發出約 10 則心跳。
bool mqttCustomTried = false;   // 本輪是否已試過自訂伺服器
int  mqttProbeOffset = 0;       // 本輪已試過幾台預設伺服器
bool mqttLastHasCustom = false; // 上次看到的「是否有自訂伺服器」，用於偵測設定變更

// 由 loop() 與 mqttCallback() 的 FIND_BEST_SERVER 共用。
// 原本是 loop() 內的 static，callback 無法更新它，導致 FIND_BEST_SERVER 觸發的
// smartConnect()（最壞 18 秒阻塞）之後，loop() 仍以為距離上次重連已滿 10 秒，
// 立刻再阻塞一次 —— 兩次背靠背 36 秒 > 30 秒門檻。
unsigned long lastMqttReconnectAt = 0;

// 把游標歸零，讓下一次 smartConnect() 重新從自訂伺服器開始。
// FIND_BEST_SERVER 的語義是「重新挑一台最好的」，必須從頭挑，不能接著上次的位置。
void resetMqttProbe() {
  mqttCustomTried = false;
  mqttProbeOffset = 0;
}

void smartConnect() {
  if (!WiFi.isConnected()) return;

  // 按鈕正被按住時不要開始一次最壞 18 秒的不可中斷連線。
  // connectToWiFi() 的等待迴圈有同樣的逃生口，這裡補上讓兩條路徑對稱。
  // 這只縮小視窗、不能完全消除：已經進到 connect() 裡面就叫不回來了。
  if (anyResetButtonPressed()) {
    Serial.println("[MQTT] 偵測到按鈕按住，本輪跳過連線嘗試");
    return;
  }

  bool hasCustom = (useCustomServer && strlen(mqttServer) > 0);
  if (hasCustom != mqttLastHasCustom) {
    // useCustomServer／mqttServer 被改動（BLE 配網、之後 Phase 的設定指令）：
    // 游標對應的是舊設定，直接歸零重新開始，避免跳過自訂伺服器或停在無意義的位置
    mqttLastHasCustom = hasCustom;
    resetMqttProbe();
  }

  if (hasCustom && !mqttCustomTried) {
    mqttCustomTried = true;
    if (quickConnectCustom()) {
      resetMqttProbe();
      return;
    }
    return;   // 本次呼叫只嘗試這一台，其餘交給 loop() 的 10 秒節奏
  }

  int index = (currentServerIndex + mqttProbeOffset) % DEFAULT_SERVER_COUNT;
  mqttProbeOffset++;
  if (quickConnectToIndex(index)) {
    // quickConnectToIndex() 成功時會把 currentServerIndex 更新為 index，
    // 下一輪重連自然從這台開始
    resetMqttProbe();
    return;
  }

  if (mqttProbeOffset >= DEFAULT_SERVER_COUNT) {
    // 一輪都試完了：起點往後推一台，避免永遠卡在同一台開頭
    currentServerIndex = (currentServerIndex + 1) % DEFAULT_SERVER_COUNT;
    resetMqttProbe();
    Serial.println("[MQTT] 本輪所有伺服器都連不上，下次改從下一台開始");
  }
}

// ── Task 3 review 修正（Critical）：publish() 真正的阻塞上界是 10 秒，不是 3 秒 ──
// 原本誤以為 mqttClient.setSocketTimeout(3) 對 publish() 有效，逐層查了實際安裝的
// PubSubClient／NetworkClient 原始碼後推翻：
//   - PubSubClient.cpp 的 setSocketTimeout() 只寫入自己的成員變數，從未呼叫
//     _client->setTimeout()；這個值只用在 connect() 等 CONNACK、readByte()
//     （收包路徑），publish() 完全不經過它。
//   - publish() → PubSubClient::write() → _client->write(buf, len)。
//     NetworkClient.cpp 的 write() 是 retry = WIFI_CLIENT_MAX_WRITE_RETRY(10) 迴圈，
//     每輪 select() 的 tv_usec = WIFI_CLIENT_SELECT_TIMEOUT_US 硬編碼 1 秒；
//     send() 帶 MSG_DONTWAIT，所以 setSocketTimeout() 設的 SO_SNDTIMEO 對它無效。
//   - 結論：單次 mqttClient.publish() 的典型上界是 10 次重試 × 1 秒 select ＝ 10 秒，
//     不是 3 秒。**但 10 秒不是硬上限**（Task 3 review N1 更正）：write() 在部分
//     寫入成功時會把重試計數器重置回 10（`res > 0` 但 totalBytesSent < size 的
//     分支裡就是 `retry = WIFI_CLIENT_MAX_WRITE_RETRY;`），所以每輪只擠得出
//     幾個 byte 的病態 socket 理論上可以拖得比 10 秒更久。下面所有以 10 秒為
//     基數的推演都要理解成「典型上界」而不是「保證」。
// 觸發情境：AP 正常但 WAN 斷線——NetworkClient::connected() 仍回 true（本地 socket
// 還在 ESTABLISHED），代發照發，lwIP 的 TCP_SND_BUF（約 5.7KB）被幾十則 payload
// 塞滿後，之後每次 write() 都吃滿 10 秒。
//
// 全專案唯一的 MQTT JSON 發布出口。Task 2~6 新增的每一種發布都必須走這裡，
// 不得自己另外開 char buf 呼叫 serializeJson()。
bool publishJsonDoc(const char* topic, JsonDocument& doc, bool retain) {
  if (!mqttClient.connected()) return false;

  // 每輪 loop() 只允許一次會阻塞的 publish（見 mqttPublishBudgetUsed 宣告處的
  // 註釋）。名額已用掉就直接讓位給下一輪，而不是把多個 10 秒黑箱疊加在同一輪
  // loop() 裡、吃光心跳空窗。
  if (mqttPublishBudgetUsed) {
    Serial.printf("[MQTT] %s 讓位給下一輪（本輪 publish 名額已用掉）\n", topic);
    return false;
  }

  // 防線 1：發布前先量。放不下就整包放棄，絕不送出被截斷的半截 JSON。
  size_t needed = measureJson(doc);
  if (needed + 1 > sizeof(statusBuf)) {
    Serial.printf("⚠ [MQTT] 放棄發布 %s：JSON 需要 %u bytes，statusBuf 只有 %u\n",
                  topic, (unsigned)needed, (unsigned)sizeof(statusBuf));
    return false;
  }

  // 防線 2：mqttClient 的 buffer 要放得下整個 PUBLISH 封包
  size_t frameNeeded = 5 + 2 + strlen(topic) + needed;
  if (frameNeeded > mqttClient.getBufferSize()) {
    Serial.printf("⚠ [MQTT] 放棄發布 %s：整包需要 %u bytes，mqtt buffer 只有 %u\n",
                  topic, (unsigned)frameNeeded, (unsigned)mqttClient.getBufferSize());
    return false;
  }

  size_t n = serializeJson(doc, statusBuf, sizeof(statusBuf));

  // 防線 3：Phase 2a 就有的截斷偵測，保留當最後一道保險。
  // 理論上防線 1 已經擋掉，但這道成本是零，而它擋的是「靜默送出壞資料」。
  if (n >= sizeof(statusBuf) - 1) {
    Serial.printf("⚠ [MQTT] 序列化填滿 statusBuf[%u]，放棄發布 %s\n",
                  (unsigned)sizeof(statusBuf), topic);
    return false;
  }

  // 佔用本輪唯一的 publish 名額，即使接下來真的卡住，其他呼叫方也會提前讓位，
  // 而不是跟著一起卡。
  mqttPublishBudgetUsed = true;

  // 阻塞呼叫前後各補一次心跳：把即將發生的 10 秒級黑箱前後各釘住一次心跳，
  // 需要時能將原本可能連續的空窗切成兩段各自約 10 秒的視窗。
  // 注意這裡的「前置心跳」同時也是 mqttClient.loop() 內部 PINGREQ 黑箱的收尾——
  // Task 4 已把 loop() 裡的 mqttClient.loop() 也用 maintainEspNow() 明確夾住，
  // 讓這件事不再依賴「publishJsonDoc() 剛好排在 mqttClient.loop() 後面」的巧合。
  maintainEspNow();
  bool res = mqttClient.publish(topic, (const uint8_t*)statusBuf, n, retain);
  maintainEspNow();

  if (!res) {
    Serial.printf("[MQTT] 發布失敗 %s（長度 %u，buffer %u）\n",
                  topic, (unsigned)n, (unsigned)mqttClient.getBufferSize());
  }
  return res;
}

// 組出 master 自身狀態的 doc（不發布）。供 publishStatus() 與 Step 6 的
// jsonsize 測試指令（printStatusJsonSize()）共用，讓測試指令不必真的連上 MQTT
// 就能量出「實際會發布的那份 JSON」的真實大小。
void buildStatusDoc(JsonDocument& doc) {
  const char* deviceId = getDeviceId();
  doc["device_id"] = deviceId;
  doc["status"] = "online";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["server"] = currentServerName();     // Phase 2a 漏掉，規格範例有這個欄位
  doc["timestamp"] = millis() / 1000;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = WiFi.isConnected();
  wifi["ssid"] = ssid;
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["relay"] = relayState ? 1 : 0;
  dev["has_relay"] = hasRelay;
  dev["pairing"] = pairingMode;
  dev["slave_count"] = (int)slaveCount;
  dev["channel"] = currentChannel;
  dev["long_range"] = longRangeEnabled;
  dev["free_heap"] = (uint32_t)ESP.getFreeHeap();

  appendGroupResult(doc);
  appendSlavesArray(doc);
}

// 發布 master 自身狀態（含代發的 slaves 陣列）。
void publishStatus() {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String topic = String("hoban/") + deviceId + "/status";

  JsonDocument doc;
  buildStatusDoc(doc);
  publishJsonDoc(topic.c_str(), doc, true);
}

// ── 代發 slave 狀態（Phase 2b Task 3）──
// 用 slave 的 MAC 代發它的狀態，讓它在 App 眼裡就是一台普通設備。
//
// wifi 區塊的內容是刻意這樣填的（規格明訂）：App 兩個頁面（設備卡片／詳情頁）
// 各自的 `_handleMqttMessage` 手動解析都吃這個形狀，不用改就能解析同一份
// payload；rssi 借來顯示 ESP-NOW 訊號強度。via 是新欄位，標示這台是誰代發的。
// （原本這裡引用 `Device.updateFromMqttMessage()`，review 指出規格已明文撤回
// 這個引用——那支函式在 `lib/` 沒有生產呼叫點，實際解析路徑是上述兩個頁面
// 各自的手動解析，已更正。）
//
// 與規格範例的唯一刻意差異：規格把 wifi.connected 寫死成 true，
// 這裡改成跟著 slaves[idx].online。理由是 App 可能拿 wifi.connected 判斷上下線，
// 寫死 true 會讓離線的 slave 在 App 上永遠顯示在線 —— 那正是規格自己
// 「Slave 失聯時 master 要代發 offline」想避免的情況。
//
// 排程常數集中放在這裡（供 slaveStatusScheduler() 使用，但 publishSlaveStatus()
// 的失敗退避也要用到，故聲明提前到函式群組最前面）：
//
// 為什麼不 20 台一次全發：mqttClient.publish() 卡住時典型上界是 10 秒
// （10 次重試 × 1 秒 select，但部分寫入會重置計數，故非硬上限；見 publishJsonDoc()
// 上方對 setSocketTimeout() 與 NetworkClient::write() 的完整更正說明——不是原先
// 誤判的 3 秒）。master 自己 + 20 台 slave = 21 個 topic，背靠背最壞可達 210 秒，
// 遠遠撞破 slave 的 30 秒失聯門檻＝籠子被打開。錯開之後每次 loop() 最多一次
// publish()，且全域 mqttPublishBudgetUsed 名額守衛保證同一輪 loop() 不會疊加
// 第二次阻塞 publish，兩次之間都有完整的 loop() 週期可發心跳（publishJsonDoc()
// 內部也會在阻塞呼叫前後各補一次心跳，見該函式）。
//
// 為什麼一輪「大約」是 15 秒：slave 的資料本身由 pollNextSlave() 以 15 秒一輪
// 更新，代發比資料更新還快是純浪費頻寬。但這只是兩個各自獨立、各自用 millis()
// 起算的 static 計時器，沒有同步機制保證相位對齊，代發帶到的資料最舊可能落後
// 一整輪（約 15 秒），不是精確同步——先前的註釋宣稱「兩者對齊」是過度陳述，
// 已更正。
const unsigned long SLAVE_STATUS_CYCLE_MS = 15000;

// review 修正（I1）：這個下限**對心跳空窗完全沒有保護力**，只在網路正常、
// publish 幾乎瞬間完成時，防止 20 台同時翻轉 dirty 時被壓縮到極短間隔內連發
// （且 mqttPublishBudgetUsed 名額守衛本來就會擋掉同一輪 loop() 內的第二次，
// 這裡防的只是跨多輪、但 loop() 本身跑很快的情境）。publish() 真的卡住時，
// lastPubAt 是在耗時的呼叫「之前」設定，10 秒級的阻塞遠大於這 250ms，下一次
// `now - lastPubAt` 必定早已超過門檻，不會被這裡攔住。真正的心跳保護是
// mqttPublishBudgetUsed 名額守衛，加上 publishJsonDoc() 內部的 maintainEspNow()。
const unsigned long SLAVE_STATUS_MIN_GAP_MS = 250;

// dirty 連續發布失敗的退避參數（review 修正）：避免一台持續失敗的 slave
// 讓 slaveStatusScheduler() 每次都優先重試同一台，餓死其他台的代發
// （見 SlaveEntry.dirtyFailCount／dirtyBackoffUntil 宣告處的註釋）。
const uint8_t SLAVE_DIRTY_MAX_FAIL = 3;
const unsigned long SLAVE_DIRTY_BACKOFF_MS = 30000;

void publishSlaveStatus(int idx) {
  if (!mqttClient.connected()) return;
  if (idx < 0 || idx >= slaveCount) return;

  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);
  char ver[16];
  formatSlaveVersion(idx, ver, sizeof(ver));

  String topic = String("hoban/") + id + "/status";

  JsonDocument doc;
  doc["device_id"] = id;
  doc["status"] = slaves[idx].online ? "online" : "offline";
  doc["version"] = ver;
  doc["model"] = "hoSlave1";
  doc["via"] = getDeviceId();
  doc["timestamp"] = millis() / 1000;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = slaves[idx].online;
  wifi["ssid"] = "ESP-NOW";
  wifi["rssi"] = slaves[idx].rssi;
  wifi["ip"] = "N/A";

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["relay"] = slaves[idx].relay ? 1 : 0;
  // 與 master 狀態的 slaves 陣列同一個語義（review M2）：只講「送達」，
  // 不講「已執行」。這台不在最近一次群組指令的快照裡就整個不帶這個欄位。
  int grp = groupDeliveryFor(slaves[idx].mac);
  if (grp >= 0) dev["grp"] = grp;

  if (publishJsonDoc(topic.c_str(), doc, true)) {
    slaves[idx].dirty = false;
    slaves[idx].dirtyFailCount = 0;
    slaves[idx].dirtyBackoffUntil = 0;
    return;
  }

  // dirty 保持 true：發失敗就留著，下次補發。
  // 失敗計數只在這裡真正呼叫過 mqttClient.publish() 卻沒成功時累加——
  // slaveStatusScheduler() 已經在呼叫本函式之前用 mqttPublishBudgetUsed
  // 擋掉「本輪名額已用掉」的情況，不會走到這裡，因此這裡的失敗都是
  // 真正的網路／buffer 問題，計入退避是合理的。
  slaves[idx].dirtyFailCount++;
  if (slaves[idx].dirtyFailCount >= SLAVE_DIRTY_MAX_FAIL) {
    slaves[idx].dirtyBackoffUntil = millis() + SLAVE_DIRTY_BACKOFF_MS;
    Serial.printf("⚠ [MQTT] %s 連續 %u 次代發失敗，暫停 %lu 秒後再試，避免餓死其他台\n",
                  id, (unsigned)slaves[idx].dirtyFailCount, SLAVE_DIRTY_BACKOFF_MS / 1000);
  }
}

// 解除配對前，先把一則 status=offline 的保留訊息壓上去。
// 少了這步，broker 上會永遠留著最後那則 "online" 的保留訊息，
// App 上就多出一台永遠在線、卻怎麼控制都沒反應的幽靈設備。
// 這裡直接強制把 slaves[idx].online 設 false 再委由 publishSlaveStatus() 組 doc，
// 因為呼叫當下 slave 可能還在名冊上（online=true），要強制發 offline，
// 而不是照它原本的線上狀態發布。
void publishSlaveOffline(int idx) {
  if (!mqttClient.connected()) return;
  if (idx < 0 || idx >= slaveCount) return;
  slaves[idx].online = false;
  slaves[idx].dirty = false;
  publishSlaveStatus(idx);
}

// 代發 slave 狀態的排程器。設計原則：每次呼叫最多發一台。
// 排程間隔、心跳安全性設計見上方 SLAVE_STATUS_CYCLE_MS／SLAVE_STATUS_MIN_GAP_MS
// 宣告處的完整說明。
void slaveStatusScheduler() {
  // review 修正（M1，除零競態）：slaveCount 是 volatile，若在本函式往下讀取
  // 之間被 WiFi task 的 HO_PKT_UNPAIR 分支減到 0，後面的除法／取模會整數除零
  // 導致 panic 重開機。因此一開頭就快照成區域變數 n，函式全程只用 n，
  // 不再重新讀取 slaveCount。pollNextSlave() 是同一模式，已一併修正。
  int n = slaveCount;
  if (n <= 0) return;
  if (!mqttClient.connected()) return;
  // 本輪已經有別的 publish 用掉名額：整個排程器讓位給下一輪，不嘗試也不動
  // lastPubAt／rotateIdx，避免虛耗一次輪播時間片（見 mqttPublishBudgetUsed
  // 宣告處的註釋）。
  if (mqttPublishBudgetUsed) return;

  static unsigned long lastPubAt = 0;
  static int rotateIdx = 0;

  unsigned long now = millis();

  // 任何一次發布之間至少隔 SLAVE_STATUS_MIN_GAP_MS（僅在網路正常時有意義，
  // 見該常數宣告處的更正說明，這裡不重複保護心跳空窗）
  if (now - lastPubAt < SLAVE_STATUS_MIN_GAP_MS) return;

  // 優先處理有變化的：從 rotateIdx 開始找，確保多台同時 dirty 時能公平輪到。
  // 連續失敗仍在退避時間內的台先跳過，讓其他台優先（見 SLAVE_DIRTY_MAX_FAIL）。
  for (int k = 0; k < n; k++) {
    int i = (rotateIdx + k) % n;
    if (!slaves[i].dirty) continue;
    if (slaves[i].dirtyBackoffUntil != 0 && (long)(now - slaves[i].dirtyBackoffUntil) < 0) {
      continue;   // 還在退避中，跳過讓其他台優先
    }
    lastPubAt = now;
    publishSlaveStatus(i);
    return;
  }

  // 沒有（可發布的）變化就走例行輪播
  unsigned long interval = SLAVE_STATUS_CYCLE_MS / (unsigned long)n;
  if (interval < SLAVE_STATUS_MIN_GAP_MS) interval = SLAVE_STATUS_MIN_GAP_MS;
  if (now - lastPubAt < interval) return;

  lastPubAt = now;
  if (rotateIdx >= n) rotateIdx = 0;
  publishSlaveStatus(rotateIdx);
  rotateIdx = (rotateIdx + 1) % n;
}

// SLAVES 指令與剛連上 broker 時用：讓整份名冊在接下來一輪內全部重發一次，
// 而不是當場連發 20 個 topic
void markAllSlavesDirty() {
  for (int i = 0; i < slaveCount; i++) slaves[i].dirty = true;
}

// 處理「slave 主動解除配對」留下的最後一則 offline 代發（見
// pendingOfflineMac／hasPendingOfflinePublish 宣告處的註釋）。
// 由 loop() 呼叫，此時名冊已經沒有這台（onEspNowRecv() 已搬移陣列），
// 不能用 idx 查表，直接用暫存的 MAC 組 topic 與內容。
void processPendingUnpairPublish() {
  if (!hasPendingOfflinePublish) return;
  // 本輪已經有別的 publish 用掉名額：讓位給下一輪，旗標保留、下一輪重試
  // （見 mqttPublishBudgetUsed 宣告處的註釋）。
  if (mqttPublishBudgetUsed) return;
  // 先清旗標再嘗試發布：即使這次發布失敗（例如剛好沒連上 MQTT）也不重試，
  // 避免卡在一台已經不存在的 MAC 上占用代發頻寬。這是一次性的收尾訊息，
  // 不像 slaves[idx].dirty 有名冊條目可以在下一輪自然補發。
  hasPendingOfflinePublish = false;

  // 先把 MAC 複製到區域變數再做任何阻塞呼叫：下面的 publishJsonDoc() 可能卡上
  // 10 秒級，期間 WiFi task 若又收到另一台的 HO_PKT_UNPAIR，會覆寫
  // pendingOfflineMac，之後的取消訂閱就會退錯 topic（與 unpairSlave() 的
  // M4 修正同一類問題）。
  uint8_t mac[6];
  memcpy(mac, pendingOfflineMac, 6);

  if (!mqttClient.connected()) return;

  char id[20];
  hoFormatDeviceId(mac, id);
  String topic = String("hoban/") + id + "/status";

  JsonDocument doc;
  doc["device_id"] = id;
  doc["status"] = "offline";
  doc["version"] = "0.0.0";   // 已解除配對，名冊上沒有版本資料可查
  doc["model"] = "hoSlave1";
  doc["via"] = getDeviceId();
  doc["timestamp"] = millis() / 1000;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = false;
  wifi["ssid"] = "ESP-NOW";
  wifi["rssi"] = 0;
  wifi["ip"] = "N/A";

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["relay"] = 0;

  publishJsonDoc(topic.c_str(), doc, true);

  // Task 4：slave 主動解除配對的路徑同樣要收掉代訂的 control topic
  // （序列埠 unpair 的路徑在 unpairSlave() 裡處理）。
  unsubscribeSlaveControlTopic(mac);
}

// ── 容量實測測試工具（Phase 2b）──
// 測試用：把名冊灌成 n 台假 slave，用來實測 20 台時狀態 JSON 的真實大小。
// 刻意「不」呼叫 saveSlaves()、也不註冊 peer —— 這是純記憶體內的假資料，
// 重開機即消失，不會污染 NVS 名冊、也不會對不存在的 MAC 送出封包。
// 只開放序列埠，不接到 MQTT：這是開發驗證工具，不是產品功能。
//
// review 補強：本函式本身確實不寫 NVS，但灌入後名冊裡混著假 MAC，若在同一次
// 開機期間接著做真正的 pair／unpair，addSlave()／unpairSlave() 一樣會呼叫
// saveSlaves()，把假 MAC 一併存進 NVS 汙染真實名冊。因此這裡另外設
// fakeSlavesActive，由 saveSlaves() 統一擋下（見該旗標宣告處與 saveSlaves()
// 的註釋），並在序列埠明講「重開機前不要 pair／unpair」。
void fakeSlavesForCapacityTest(int n) {
  if (n < 0) n = 0;
  if (n > HO_ESPNOW_MAX_SLAVES) n = HO_ESPNOW_MAX_SLAVES;
  for (int i = 0; i < n; i++) {
    slaves[i].mac[0] = 0xAA; slaves[i].mac[1] = 0xBB; slaves[i].mac[2] = 0xCC;
    slaves[i].mac[3] = 0xDD; slaves[i].mac[4] = 0xEE; slaves[i].mac[5] = (uint8_t)i;
    slaves[i].online = false;      // false 比 true 多 1 byte，取最壞
    slaves[i].rssi = -100;         // 3 位數負值，取最壞
    slaves[i].lastSeen = 0;
    slaves[i].relay = 1;
    slaves[i].fwMajor = 255; slaves[i].fwMinor = 255; slaves[i].fwPatch = 255;
    slaves[i].dirty = false;
    slaves[i].dirtyFailCount = 0;
    slaves[i].dirtyBackoffUntil = 0;
  }
  slaveCount = n;
  fakeSlavesActive = true;   // 鎖住 saveSlaves()，直到重開機為止（見該旗標宣告處的說明）
  Serial.printf("[測試] 名冊已灌成 %d 台假 slave（未寫入 NVS，重開機即消失）\n", n);
  Serial.println("⚠ [測試] 重開機前請勿執行 pair／unpair："
                 "名冊混有假 MAC，一旦觸發存檔就會寫進 NVS 汙染真實名冊（已由 saveSlaves() 擋下）");
}

// 印出目前狀態 JSON 的實際大小，與各層預算比對
void printStatusJsonSize() {
  JsonDocument doc;
  buildStatusDoc(doc);
  size_t n = measureJson(doc);
  Serial.printf("[測試] 狀態 JSON 實際 %u bytes／statusBuf %u／mqtt buffer %u（名冊 %d 台）\n",
                (unsigned)n, (unsigned)sizeof(statusBuf),
                (unsigned)mqttClient.getBufferSize(), slaveCount);
}

// 代收 slave 的 control topic：把 MQTT 純文字指令轉成 ESP-NOW 封包。
//
// ON 為什麼送 HO_CMD_PULSE 而不是 HO_CMD_ON：
// App 對一般 hoRelay 設備送的 ON 語義是「開門」＝點動一次，master 自己的
// ON 分支也是 pulseRelay(2000)。slave 要在 App 眼裡是一台普通設備，
// 語義就必須完全一致，否則「開保險 → 關門 → 關保險」三段鎖流程對 slave
// 的行為會與其他設備不同。持續開啟只保留給序列埠的 on <n> 指令（現場除錯用）。
//
// ── review 修正（M2，門控安全）：參數收 MAC，不收索引 ──
// 第一版沿用 mqttCallback() 查出來的 idx。但 slaves[] 會被 WiFi task 的
// HO_PKT_UNPAIR 分支前移（onEspNowRecv()），索引在任何一個可能被打斷的點上
// 都會失效 —— 而本函式的 status 分支中間夾著 publishSlaveStatus()，那是一次
// 最壞 10 秒級的阻塞 publish，窗口大到不能忽略。索引指錯台 ＝ **開錯門**，
// 對動物捕捉設備而言是最嚴重的失敗型態。
// 這正是 Task 3 review 的 M4 缺陷（unpairSlave() 阻塞後沿用舊 idx），
// 第一版在新路徑上原封不動地重新引入了一次。
//
// 修法與 M4 相同：全程持有 MAC，每一個真正要動作的點都用 findSlave() 重查，
// 送指令則走 sendCmdToSlaveMac()（連重查與送出之間的窗口都不存在）。
void handleSlaveCommand(const uint8_t mac[6], const String& message) {
  char id[20];
  hoFormatDeviceId(mac, id);
  Serial.printf("[代理] %s 收到指令: %s\n", id, message.c_str());

  if (message == "ON") {
    sendCmdToSlaveMac(mac, HO_CMD_PULSE, 2000, allocCmdId());
  } else if (message == "OFF") {
    sendCmdToSlaveMac(mac, HO_CMD_OFF, 0, allocCmdId());
  } else if (message == "status") {
    // 先用目前已知的狀態立刻回一則，App 不必空等；
    // 同時向 slave 要一次最新狀態，回來時 dirty 會觸發第二則代發。
    // 索引每次重查：中間的 publishSlaveStatus() 可能阻塞 10 秒級，
    // 沿用同一個 idx 會問到別台去。
    int idx = findSlave(mac);
    if (idx >= 0) publishSlaveStatus(idx);
    // review Mi2：群組指令進行期間讓開這則 STATE_REQ。
    // 它是送往某一台 slave 的**單播**，其 MAC 層 ACK 會落進 onEspNowSent()；
    // 若剛好與群組單播的 ACK 歸因閂鎖撞在同一個 MAC 上，會把「查詢已送達」
    // 誤記成「群組指令已送達」—— 與 C1 同一類的錯誤歸因。
    // 群組指令有 6 秒 wall-clock 硬上限，最多只讓開這麼久；上面那則
    // publishSlaveStatus() 已經先回了 App 一份目前已知的狀態，不會空等。
    if (groupCmdActive()) {
      Serial.printf("[代理] %s 群組指令進行中，稍後再向它要最新狀態\n", id);
    } else {
      idx = findSlave(mac);
      if (idx >= 0) requestSlaveStateIndex(idx);
    }
  } else {
    Serial.printf("[代理] %s 不支援的指令: %s\n", id, message.c_str());
  }
}

// master 自己的指令分派（Phase 2a 的 mqttCallback() 內容原樣搬過來）。
//
// 分支現況（Task 7 更新，原註釋寫「由 Task 5、6 往這裡加分支」已過時）：
// Task 5 已把 ALL:ON／ALL:OFF／SLAVES／PAIR:START／PAIR:STOP／UNPAIR:<id>／
// UNPAIRALL 全部長出來，就在下面。**唯一還沒長的是 LR:**，它落在最後一個
// else-if，只印一行「尚未實作」而不掉進「未知指令」——因為 Task 6（Long Range）
// 已依 Phase 2b 的 Ruling 整個移到 Phase 5 執行，本階段不做。
//
// 這裡不擋任何未來分支，但**新增時務必同步 ho_master1/readme.md 的「控制指令表」
// 與 docs/phase2b-regression-checklist.md**：本專案已經有**四次**「回歸清單的判準
// 字串與程式碼不符，導致實測者把正確行為判成 FAIL」——第四次就是 Task 7 在
// phase1 清單的 8e 抓到的（`alloff` 的指令碼被寫成 2，實際 HO_CMD_OFF = 0）。
// 最終 else 的探針字串 `[MQTT] 未知指令: %s` 是 App 端驗證程序依賴的，不要改。
void handleMasterCommand(const String& message) {
  if (message == "status") {
    publishStatus();
  } else if (message == "ON") {
    pulseRelay(2000);
    publishStatus();
  } else if (message == "OFF") {
    setRelayPins(false);
    publishStatus();
  } else if (message == "reset") {
    // 這裡刻意「清除後立刻重開機」，不可改成「清除後繼續往下執行」：
    // Preferences::getString() 在 key 不存在時完全不寫入緩衝區（core 3.3.7 的
    // Preferences.cpp 遇到 key 不存在直接 return 0），所以 clearNetConfig() 之後
    // 若不重開機就再呼叫 loadNetConfig()，RAM 裡的 ssid 等變數會殘留清除前的
    // 舊字串，hasWifiConfig() 會誤判成「仍有設定」。用 restart 讓開機流程重新
    // 從乾淨的 RAM 狀態呼叫 loadNetConfig()，才能保證讀到的是清除後的結果。
    clearNetConfig();
    espNowDelay(1000);
    ESP.restart();
  } else if (message == "FIND_BEST_SERVER") {
    mqttClient.disconnect();
    espNowDelay(500);
    // 語義是「重新挑一台最好的」，所以先把輪詢游標歸零，從自訂伺服器重新開始。
    // 注意：smartConnect() 現在一次只試一台（見該函式註釋），若第一台連不上，
    // 後續會由 loop() 每 10 秒推進一台，不會在這裡一口氣試完全部而卡住心跳。
    resetMqttProbe();
    smartConnect();
    // FIND_BEST_SERVER 未被 10 秒節流覆蓋的修正：這裡也要更新，否則 loop() 誤以為
    // 距離上次重連已滿 10 秒，緊接著又觸發一次最壞 18 秒的阻塞連線。
    lastMqttReconnectAt = millis();
  } else if (message == "HASRELAY:ON" || message == "HASRELAY:OFF") {
    hasRelay = (message == "HASRELAY:ON");
    saveNetConfig();
    Serial.printf("[設定] 繼電器宣告為 %s\n", hasRelay ? "有接" : "未接");
    publishStatus();
  } else if (message == "ALL:ON") {
    // 規格：「廣播 pulse 給所有已配對 slave」——是 PULSE 不是持續 ON，
    // 語義與單台的 ON（pulseRelay(2000)）一致。
    // sendCmdToAll() 內部會連 master 自己那顆一起動作（規格：ALL:ON 連自己一起），
    // 並排入「事後查證 → 補送單播」的第 2／3 段。
    sendCmdToAll(HO_CMD_PULSE, 2000);
    markAllSlavesDirty();
    publishStatus();
  } else if (message == "ALL:OFF") {
    // 本系統最關鍵的一條指令：關門失敗＝動物逃脫或未被捕捉。
    // 廣播只是第 1 段，真正保證「全部關」的是 processGroupCmd() 的查證與補送。
    sendCmdToAll(HO_CMD_OFF, 0);
    markAllSlavesDirty();
    publishStatus();
  } else if (message == "SLAVES") {
    // 規格：「立刻回報 slave 清單（發一次 status）」。
    // 另外把所有 slave 標記成 dirty，讓每台的代發 topic 在接下來一輪內
    // 也各自重壓一次保留訊息 —— 但不在這裡連發 20 則（見 slaveStatusScheduler()）。
    markAllSlavesDirty();
    publishStatus();
  } else if (message == "PAIR:START") {
    // 語義與 BOOT 短按／序列埠 pair 完全一致：開一個 60 秒的配對視窗
    // （PAIRING_TIMEOUT，與 App 端的 60 秒倒數對齊），期間可以連續配對多台，
    // **配對成功不會自動退出**（onEspNowRecv() 的 HO_PKT_PAIR_REQ 分支只做
    // addSlave() 與回一則 PAIR_ACK）。退出只有三條路：PAIR:STOP／序列埠 pair
    // 或 BOOT 短按切換／60 秒逾時。App 端已依賴這個行為，不要自創
    // 「配對成功即退出」。重複下 PAIR:START 等於把 60 秒視窗重新計時。
    enterPairingMode();
    publishStatus();
  } else if (message == "PAIR:STOP") {
    exitPairingMode();
    publishStatus();
  } else if (message.startsWith("UNPAIR:")) {
    String idStr = message.substring(7);
    idStr.trim();
    uint8_t mac[6];
    if (!hoParseMacFromDeviceId(idStr.c_str(), mac)) {
      Serial.printf("[配對] UNPAIR 的設備 ID 格式錯誤: %s\n", idStr.c_str());
    } else {
      int idx = findSlave(mac);
      if (idx < 0) {
        Serial.printf("[配對] UNPAIR 的設備不在名冊上: %s\n", idStr.c_str());
      } else {
        unpairSlave(idx);
        publishStatus();
      }
    }
  } else if (message == "UNPAIRALL") {
    // 規格沒有這條，是 Phase 2a 交付時列出的待辦：目前完全沒有辦法清空名冊，
    // master 要重新部署只能整台重燒。
    // **只插旗，不在這裡跑迴圈** —— 一口氣拆 20 台最壞超過 60 秒沒有心跳，
    // 會撞破 slave 的 30 秒失聯門檻（理由詳見 processUnpairAll() 上方註釋）。
    if (slaveCount <= 0) {
      Serial.println("[配對] 名冊本來就是空的");
    } else {
      unpairAllPending = true;
      Serial.printf("[配對] 開始清空名冊，共 %d 台（每輪 loop 拆一台）\n", slaveCount);
    }
  } else if (message.startsWith("LR:")) {
    // Task 6 實作，先給一個明確的回應而不是掉進「未知指令」
    Serial.println("[LR] 指令尚未實作（Task 6）");
  } else {
    Serial.printf("[MQTT] 未知指令: %s\n", message.c_str());
  }
}

// 收到任何 control topic 的訊息。本函式跑在 mqttClient.loop() 內，屬 loop() context，
// 可以安全呼叫阻塞函式，但等待一律走 espNowDelay() 而非裸 delay()。
//
// topic 比對從 Phase 2a 的「跟自己的 control topic 做完整字串相等」改成
// 「解析出 MAC → 查名冊」：master 現在同時代理最多 20 台 slave 的 control topic，
// 一台一條字串去比對等於要維護一份 21 條的 topic 表（693 bytes RAM，且配對／
// 解除配對時要同步維護，是名冊之外的第二份真相）。名冊已經是唯一真相。
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  uint8_t mac[6];
  if (!parseControlTopic(topic, mac)) {
    Serial.printf("[MQTT] 無法解析的 topic，忽略: %s\n", topic);
    return;
  }

  uint8_t ownMac[6];
  WiFi.macAddress(ownMac);
  if (memcmp(mac, ownMac, 6) == 0) {
    Serial.printf("[MQTT] 收到指令: %s\n", message.c_str());
    handleMasterCommand(message);
    return;
  }

  if (findSlave(mac) < 0) {
    // 解除配對後 broker 上可能還有殘留訊息，或訂閱尚未完全取消
    Serial.printf("[MQTT] 指令的目標不在名冊上，忽略: %s\n", topic);
    return;
  }
  // 這裡刻意只傳 MAC 不傳剛查到的索引：索引在下一個瞬間就可能因為 WiFi task
  // 的 HO_PKT_UNPAIR 前移 slaves[] 而指到別台（review M2）。上面的 findSlave()
  // 只是「不在名冊上就別吵」的早期過濾，不是給 handleSlaveCommand() 用的。
  handleSlaveCommand(mac, message);
}

// ── ESP-NOW 初始化 ──
void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // Phase 1 不連 WiFi，Phase 2 才接上

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失敗，重啟");
    delay(2000);
    ESP.restart();
  }

  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);

  // 廣播位址也必須註冊成 peer 才能送出
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;  // 0 = 沿用目前 channel
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("加入廣播 peer 失敗");
  }

  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);
  currentChannel = primary;

  Serial.printf("ESP-NOW 就緒，channel=%u\n", currentChannel);
}

void setup() {
  initRelayPins();  // 必須第一行

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("齁控 Master v" + String(firmwareVersion));
  Serial.println("================");

  initLeds();
  pinMode(bootButton, INPUT_PULLUP);
  pinMode(secondButton, INPUT_PULLUP);
  delay(50);  // 等內部提升電阻把腳位拉穩再取樣

  checkStuckButtons();  // 必須早於任何按鈕流程，卡住的腳會在此被排除

  loadNetConfig();  // 載入網路設定，Task 2~7 會用到
  loadSlaves();     // 只讀 NVS，可以在 ESP-NOW 初始化之前
  setupEspNow();
  registerAllPeers();  // 必須在 esp_now_init() 之後，否則名冊上的 slave 全部送不出指令

  // ── 指令識別碼的開機初值（理由見 nextCmdId 宣告處）──
  // **必須排在 setupEspNow() 之後**：setupEspNow() 裡的 WiFi.mode(WIFI_STA) 才會
  // 啟動射頻，而 ESP-IDF 明文規定「RF 未啟用時 esp_random() 不得視為真隨機」——
  // 排在前面等於每次開機都可能拿到同一串固定值，那正好把「重開機後撞到上一輪
  // 同一個 cmdId」從巧合變成常態，是誤綠方向。
  // 0 是保留值（HO_CMD_ID_NONE），撞到就改成 1。
  //
  // **它擋不住什麼**：即使有真隨機，也只是把撞號機率壓到 1/65535；
  // 真正擋住舊回報的是 groupExecutedIdx() 的「回報時間 ≥ 指令送出時間」那一條，
  // 而那一條擋不住「指令送出後才被重播」的舊回報，更擋不住偽造。
  nextCmdId = (uint16_t)esp_random();
  if (nextCmdId == HO_CMD_ID_NONE) nextCmdId = 1;

  // WiFi 連線必須排在 ESP-NOW 初始化與 peer 註冊之後：
  // 反過來會讓 esp_now_init() 在 STA 已連線的狀態下執行，peer 的 channel 跟隨行為可能不如預期。
  WiFi.onEvent(onWiFiEvent);
  // 註：setAutoReconnect(true) 在 Arduino core 3.3.7 實際上是死碼 —— _autoReconnect
  // 只存在於 STAClass 的建構子／setter／getter，STA_DISCONNECTED 事件處理從頭到尾
  // 沒有讀取它。保留這行只是為了與其他 sketch 的寫法一致並向前相容，
  // 真正負責重連的是 loop() 裡的 WiFi 管理區塊（wifiFailCount／wifiPauseUntil）。
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);      // 禁用睡眠，避免 ESP-NOW 收包延遲

  // BLE 只在「沒有 WiFi 設定」時啟動，與 ho_relay2 一致。
  // 原因：BLE stack 約佔 50~70KB heap，且與 WiFi 共用 2.4G 射頻。
  // 配網是一次性動作，完成後 ESP.restart()，之後不再開 BLE。
  if (hasWifiConfig()) {
    connectToWiFi();
  } else {
    bleConfigMode = true;
    // 本次開機不會關聯 WiFi，channel 會停在 WIFI_STA 預設的 1。
    // 先切回 NVS 記住的 AP channel，避免已配對的 slave 在整個配網過程中失聯關籠
    //（詳見 restoreEspNowChannelForOfflineBoot() 的註釋）
    restoreEspNowChannelForOfflineBoot();
    setupBLE();
    Serial.println("[BLE] 等待 App 配網");
  }

  Serial.printf("設備 ID: %s\n", getDeviceId());
  printHelp();
  Serial.println("就緒");
}

void loop() {
  unsigned long now = millis();

  // review 修正（Critical）：每輪 loop() 開頭重置本輪的「阻塞 publish 名額」，
  // 保證本輪最多只發生一次會阻塞的 mqttClient.publish()（見 mqttPublishBudgetUsed
  // 宣告處與 publishJsonDoc() 的完整說明）。
  mqttPublishBudgetUsed = false;

  // ── 短按 BOOT 進入配對模式 ──
  // 長按 3 秒以上不觸發，交給下面的 updateResetButton() 判斷長按重置
  // （只認 BOOT 單一支腳，與 updateResetButton() 認 anyResetButtonPressed()
  // 涵蓋的任一支腳刻意不同，兩者互不打架的理由詳見 updateResetButton() 上方註釋）
  // 自檢判定卡在 LOW 的腳一律視為未按下，避免壞按鈕不斷觸發按鈕流程
  static bool lastButtonState = HIGH;
  static unsigned long buttonDownTime = 0;
  bool buttonState = (bootButtonUsable && digitalRead(bootButton) == LOW) ? LOW : HIGH;

  if (lastButtonState == HIGH && buttonState == LOW) {
    buttonDownTime = now;
  }
  if (lastButtonState == LOW && buttonState == HIGH) {
    unsigned long pressDuration = now - buttonDownTime;
    if (pressDuration >= 50 && pressDuration < 1000) {   // 50ms 去彈跳，1 秒內算短按
      if (pairingMode) exitPairingMode(); else enterPairingMode();
    }
  }
  lastButtonState = buttonState;

  // ── 長按重置（Task 8）：非阻塞狀態機，詳見 updateResetButton() 上方註釋 ──
  bool resetButtonActive = updateResetButton(now);

  // ── 配對模式逾時 ──
  if (pairingMode && now - pairingStartTime >= PAIRING_TIMEOUT) {
    Serial.println("[配對] 逾時");
    exitPairingMode();
  }

  // ── LED 狀態指示（Task 7）──
  // 長按重置的確認階段優先權最高，接管 LED 期間（resetButtonActive == true）
  // 跳過以下兩者，避免被覆蓋（優先權分工詳見 updateResetButton() 上方註釋）。
  // 未接管時：先推進一次性閃爍請求（配對接受／拒絕），播完的同一輪就會被
  // updateStatusLed() 接手判斷持續式狀態（BLE 配網／配對模式／WiFi／MQTT／正常），
  // 兩者分工與優先權詳見各自函式上方註釋
  if (!resetButtonActive) {
    updateBlink(now);
    updateStatusLed(now);
  }

  // 心跳固定 1 秒一次（HEARTBEAT_INTERVAL），計時併入 maintainEspNow()，
  // 避免與 Phase 2a 阻塞流程內的維持機制形成兩套計時器、重複發送
  maintainEspNow();

  // ── WiFi 連線管理（每 5 秒檢查）──
  static unsigned long lastWiFiCheck = 0;
  static int wifiFailCount = 0;
  static unsigned long wifiPauseUntil = 0;   // 取代 ho_relay2 會 unsigned 下溢的寫法

  // BLE 配網模式下沒有 WiFi 設定也連不上，跳過整段管理；
  // 與 ho_relay2 在 BLE 模式直接 return 不同，master 只跳過這一區塊，
  // 按鈕處理、maintainEspNow()、LED 仍照跑，避免已配對的 slave 在配網期間失聯關籠。
  if (!bleConfigMode && hasWifiConfig() && now - lastWiFiCheck > 5000) {
    if (WiFi.status() != WL_CONNECTED) {
      // wrap-safe 寫法：(long)(now - wifiPauseUntil) < 0 等同「now 還沒到 wifiPauseUntil」，
      // 且在 millis() 溢位時仍成立（與檔案內點動計時用的無號數減法比較同一套邏輯）。
      // review 抓到：原本的 `now < wifiPauseUntil` 在溢位當下不 wrap-safe。
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

        // review 抓到的 Critical：connectToWiFi() 最壞阻塞 15 秒，若沿用進入本區塊前的
        // now 記錄 lastWiFiCheck，下一輪 loop() 的 now 已經超前 15 秒以上，
        // (now - lastWiFiCheck > 5000) 立刻成立，「每 5 秒檢查」在失敗情境下形同虛設、
        // 15 秒的嘗試會背靠背重試。必須在阻塞呼叫「之後」用新的 millis() 記錄。
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

  // ── MQTT 連線管理 ──
  // BLE 配網模式下同樣跳過：沒有 WiFi 連線，WiFi.status() 本就不會是 WL_CONNECTED，
  // 這裡明講 !bleConfigMode 是為了讓「配網期間不碰 MQTT」的意圖在程式碼上明確可見。
  static unsigned long lastStatusPub = 0;

  if (!bleConfigMode && WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (now - lastMqttReconnectAt > 10000) {
        smartConnect();
        // 與上方 WiFi 重連同一個理由：smartConnect() 內的 mqttClient.connect() 最壞
        // 阻塞約 18 秒（DNS 逾時，見 smartConnect() 註釋），若沿用進入本區塊前的 now
        // 記錄 lastMqttReconnectAt，下一輪 loop() 的 now 已超前 18 秒以上，10 秒節流
        // 立刻成立、變成背靠背重試，兩次阻塞之間只擠得出一則心跳。必須在阻塞呼叫
        // 「之後」用新的 millis() 記錄，才能保證每兩次嘗試之間有完整 10 秒可發約 10 則心跳。
        lastMqttReconnectAt = millis();
      }
    } else {
      // ── Task 4（採納 Task 3 review 的結構化提案）：把 mqttClient.loop() 也夾住 ──
      // mqttClient.loop() 內部會在距離上次收發超過 keepAlive（本韌體設 30 秒）
      // 時自動送出 PINGREQ，走的是與 publish() 完全相同的 NetworkClient::write()，
      // 卡住時同樣是 10 秒級的黑箱，而且完全在函式庫內部，既不經過
      // publishJsonDoc()，也不受 mqttPublishBudgetUsed 名額守衛管轄。
      //
      // 更正 Task 3 報告的錯誤推論：PINGREQ **不是罕見事件**。PubSubClient::loop()
      // 的判斷是 `(t - lastInActivity > keepAlive*1000) || (t - lastOutActivity > ...)`，
      // 是 OR 不是 AND；lastInActivity 只有在真的**收到**封包時才更新，而 broker
      // 平時不會主動送東西過來。所以即使我們每 10 秒 publish 一次（只更新
      // lastOutActivity），in 這一側照樣會超時 —— **PINGREQ 是每 30 秒觸發一次的
      // 例行事件**，不是偶發。
      //
      // 夾住之後這個保證變成結構性的：進入 PINGREQ 黑箱前剛發過心跳、出來立刻
      // 再發一次。**這不會讓最壞空窗變小**（仍是約 11 秒 ＝ 10 秒黑箱 ＋ 最多
      // 1 秒的心跳間隔餘裕），它讓這個數字不再依賴「publishJsonDoc() 剛好排在
      // mqttClient.loop() 後面、它的前置心跳剛好落在兩個黑箱之間」的呼叫順序巧合。
      maintainEspNow();
      mqttClient.loop();
      maintainEspNow();

      // 名冊在 ESP-NOW callback 裡變動過（配對成功／slave 主動解除），
      // 回到 loop() context 才動 socket。這裡只是把游標歸零排隊，
      // 真正的 subscribe 由下面的排程器每輪做一格。
      if (pendingSubscribeRefresh && mqttClient.connected()) {
        pendingSubscribeRefresh = false;
        subscribeAllControlTopics();
      }

      // 訂閱對齊：每輪最多一次 subscribe，與 publish 共用同一個名額（review M1）。
      // 排在 publishStatus() 之前：對齊是有限步數的一次性工作（最多 slaveCount+1 格），
      // 讓它先走完，代價只是狀態發布晚幾輪（健康網路下是毫秒級）。
      controlSubscribeScheduler();

      if (now - lastStatusPub > 10000) {   // 每 10 秒發一次狀態，master 還要發心跳與輪詢 slave，比 ho_relay2 的 3 秒寬鬆
        lastStatusPub = now;
        publishStatus();
      }
    }
  }

  // ── 群組指令的補送段（見 processGroupCmd()）──
  // ESP-NOW 廣播沒有 ACK，「廣播完就當成功」是假綠燈；補送若在指令回呼裡
  // 一口氣跑完會是數秒級阻塞，撞破 slave 的 30 秒失聯門檻。
  // 本函式每次 loop() 最多送一台，完全不阻塞，並有 6 秒 wall-clock 硬上限。
  processGroupCmd();

  // ── 分散式輪詢 slave 狀態，每次 loop() 最多問一台（見 pollNextSlave()）──
  // 群組指令進行期間整個讓開：輪詢送的 HO_PKT_STATE_REQ 是**單播**，它的
  // MAC 層 ACK 會落進 onEspNowSent()，若與群組單播的 ACK 歸因閂鎖撞在同一個
  // MAC 上，會把「查詢已送達」誤記成「群組指令已送達」（與 C1 同一類的錯誤歸因）。
  // 讓開的長度由 GROUP_JOB_MAX_MS 硬性封頂在 6 秒（review M3）：
  // 輪詢週期 15 秒 ＋ 6 秒停擺 ＝ 21 秒 < updateSlaveOnlineStatus() 的 30 秒
  // 離線門檻，不可能因此把全台誤判離線。
  if (!groupCmdActive()) pollNextSlave();

  // ── UNPAIRALL 分批拆除，每輪 loop() 最多一台（見 processUnpairAll()）──
  processUnpairAll();

  // ── 代發 slave 狀態（每次 loop() 最多代發一台，見 slaveStatusScheduler()）──
  processPendingUnpairPublish();
  slaveStatusScheduler();

  // ── 每 15 秒檢查一次 slave 是否離線 ──
  static unsigned long lastOnlineCheck = 0;
  if (now - lastOnlineCheck >= 15000) {
    lastOnlineCheck = now;
    updateSlaveOnlineStatus();
  }

  // 點動結束檢查已併入 maintainEspNow()（review 修正，見該函式註釋），
  // 這裡不再重複判斷，避免兩處各自比對造成競態或重複關閉

  // ── 序列埠指令 ──
  static String serialBuffer = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        handleSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 64) serialBuffer = "";  // 防溢位
    }
  }
}
