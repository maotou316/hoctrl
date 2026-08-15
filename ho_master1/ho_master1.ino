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

// ── Slave 名冊 ──
struct SlaveEntry {
  uint8_t mac[6];
  bool online;
  int8_t rssi;
  unsigned long lastSeen;
};

SlaveEntry slaves[HO_ESPNOW_MAX_SLAVES];
// slaveCount 會被 ESP-NOW callback（WiFi task）寫入、sendHeartbeat()（主 task）讀取，
// 屬跨 context 存取，加 volatile 避免編譯器快取舊值
volatile int slaveCount = 0;
Preferences prefs;

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

WiFiClient espClient;
PubSubClient mqttClient(espClient);

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
// master 目前只有短按進配對（風險低），但重置功能的位置已預留在 loop()，
// Phase 2 一補上就會繼承 hoRelay2 那個「開機即清設定 → 重啟 → 再清除」的無限迴圈缺陷，
// 所以現在先把防呆一併備妥。
const unsigned long BTN_SELFTEST_DURATION = 500;  // 自檢取樣總長度 (毫秒)
const unsigned long BTN_SELFTEST_INTERVAL = 50;   // 取樣間隔 (毫秒)
bool bootButtonUsable = true;                     // BOOT 按鈕是否可用
bool secondButtonUsable = true;                   // 第二按鈕是否可用

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
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, buffer);

    if (err || !doc.containsKey("wifi")) {
      StaticJsonDocument<200> res;
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
      StaticJsonDocument<200> res;
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

    StaticJsonDocument<350> res;
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
//（限制在單一 channel 掃描仍有短暫的 off-channel 時間；升級成全頻掃描時更是整整一輪），
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
// 目前只有 BOOT 用於短按配對，這支保留給 Phase 2 的長按重置流程，屆時一律走這裡判斷。
bool anyResetButtonPressed() {
  if (bootButtonUsable && digitalRead(bootButton) == LOW) return true;
  if (secondButtonUsable && digitalRead(secondButton) == LOW) return true;
  return false;
}

// ── 名冊管理 ──
int findSlave(const uint8_t mac[6]) {
  for (int i = 0; i < slaveCount; i++) {
    if (memcmp(slaves[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

void saveSlaves() {
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
  slaveCount++;

  registerPeer(mac);
  saveSlaves();
  // 名冊從空的變成有人，或又多一台：把目前 channel 一併記住。
  // 少了這行，「先連上 WiFi、之後才配對 slave」這個最常見的順序下，espch 永遠不會被
  // 寫入（connectToWiFi() 的成功分支在配對發生之前就跑完了），下次 reset 進 BLE 配網
  // 仍然會停在 channel 1。這裡與既有的 saveSlaves() 同樣是在 ESP-NOW callback
  // context 寫 NVS，沒有引入新的風險類別。
  if (WiFi.status() == WL_CONNECTED) saveSlaveLockChannel(currentChannel);
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
void sendCmdToSlave(int idx, HoRelayCmd cmd, uint16_t pulseMs) {
  if (idx < 0 || idx >= slaveCount) {
    Serial.println("[控制] 編號超出範圍");
    return;
  }
  HoCmdPayload payload;
  payload.cmd = (uint8_t)cmd;
  payload.pulseMs = pulseMs;

  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);
  Serial.printf("[控制] 送指令 %u 給 %s\n", (uint8_t)cmd, id);
  espNowSendTo(slaves[idx].mac, HO_PKT_CMD, &payload, sizeof(payload));
}

void sendCmdToAll(HoRelayCmd cmd, uint16_t pulseMs) {
  Serial.printf("[控制] 廣播指令 %u 給 %d 台\n", (uint8_t)cmd, slaveCount);
  for (int i = 0; i < slaveCount; i++) {
    sendCmdToSlave(i, cmd, pulseMs);
    delay(20);   // 錯開送出時間，降低同頻碰撞
  }
  // master 自己的繼電器也跟著動作
  if (cmd == HO_CMD_ON) {
    setRelayPins(true);
  } else if (cmd == HO_CMD_OFF) {
    setRelayPins(false);
  } else if (cmd == HO_CMD_PULSE) {
    pulseRelay(pulseMs > 0 ? pulseMs : 2000);
  }
}

void requestSlaveState(int idx) {
  if (idx < 0 || idx >= slaveCount) return;
  espNowSendTo(slaves[idx].mac, HO_PKT_STATE_REQ, nullptr, 0);
}

// 分散式輪詢：每次 loop() 呼叫最多只問一台，用「15000ms ÷ 台數」的間隔
// 平均分攤，一輪剛好 15 秒問完全部，且完全不阻塞 loop()。
// （舊版用 for 迴圈搭配 delay(20) 一次問完全部，20 台會阻塞 loop() 達
// 400ms，若與 master 自身點動的關閉時機重疊，會讓點動時間被拖長超過設定
// 值，故改為此設計，見 Task 6 review 修正 2）
void pollNextSlave() {
  if (slaveCount == 0) return;

  static unsigned long lastPollAt = 0;
  static int pollIdx = 0;

  // 下限 20ms：台數很多時避免間隔過短、無線封包擠在一起碰撞
  unsigned long interval = 15000UL / (unsigned long)slaveCount;
  if (interval < 20) interval = 20;

  unsigned long now = millis();
  if (now - lastPollAt < interval) return;
  lastPollAt = now;

  // slaveCount 可能因配對／解除配對中途變動，索引越界就重頭開始，
  // 不特別處理「跳過某台」，反正下一輪就會輪到
  if (pollIdx >= slaveCount) pollIdx = 0;
  requestSlaveState(pollIdx);
  pollIdx = (pollIdx + 1) % slaveCount;
}

void updateSlaveOnlineStatus() {
  unsigned long now = millis();
  for (int i = 0; i < slaveCount; i++) {
    bool wasOnline = slaves[i].online;
    bool isOnline = slaves[i].lastSeen > 0 &&
                    (now - slaves[i].lastSeen) < SLAVE_OFFLINE_TIMEOUT;
    if (wasOnline && !isOnline) {
      char id[20];
      hoFormatDeviceId(slaves[i].mac, id);
      Serial.printf("[離線] %s 超過 %lu 秒沒回應\n",
                    id, SLAVE_OFFLINE_TIMEOUT / 1000);
    }
    slaves[i].online = isOnline;
  }
}

void unpairSlave(int idx) {
  if (idx < 0 || idx >= slaveCount) {
    Serial.println("[配對] 編號超出範圍");
    return;
  }
  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);

  espNowSendTo(slaves[idx].mac, HO_PKT_UNPAIR, nullptr, 0);
  delay(100);   // 給對方時間收到再刪 peer

  esp_now_del_peer(slaves[idx].mac);
  for (int i = idx; i < slaveCount - 1; i++) slaves[i] = slaves[i + 1];
  slaveCount--;
  saveSlaves();
  Serial.printf("[配對] 已移除 %s，剩 %d 台\n", id, slaveCount);
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
  Serial.println("  ch <n>        測試用：切換 master 的 channel（1~13）");
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
                   verb == "state" || verb == "unpair" || verb == "ch");
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
    sendCmdToSlave(arg, HO_CMD_ON, 0);
  } else if (verb == "off") {
    sendCmdToSlave(arg, HO_CMD_OFF, 0);
  } else if (verb == "pulse") {
    sendCmdToSlave(arg, HO_CMD_PULSE, 2000);
  } else if (verb == "allon") {
    sendCmdToAll(HO_CMD_ON, 0);
  } else if (verb == "alloff") {
    sendCmdToAll(HO_CMD_OFF, 0);
  } else if (verb == "allpulse") {
    sendCmdToAll(HO_CMD_PULSE, 2000);
  } else if (verb == "state") {
    requestSlaveState(arg);
  } else if (verb == "unpair") {
    unpairSlave(arg);
  } else if (verb == "ch") {
    // 測試用：手動切換 channel，模擬 Phase 2 連上不同路由器的情況
    if (arg >= 1 && arg <= 13) {
      esp_wifi_set_channel((uint8_t)arg, WIFI_SECOND_CHAN_NONE);
      currentChannel = (uint8_t)arg;
      Serial.printf("[channel] master 切換到 %d\n", arg);
      sendHeartbeatBurst();   // 立刻連發數次，讓正在輪掃的 slave 早點命中
    } else {
      Serial.println("channel 需在 1~13 之間");
    }
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

    slaves[idx].online = true;
    slaves[idx].rssi = info->rx_ctrl->rssi;
    slaves[idx].lastSeen = millis();

    Serial.printf("[狀態] %s relay=%u 版本=%u.%u.%u 運行=%lus rssi=%d\n",
                  senderId, st.relay, st.fwMajor, st.fwMinor, st.fwPatch,
                  (unsigned long)st.uptimeSec, info->rx_ctrl->rssi);
    return;
  }
}

void onEspNowSent(const wifi_tx_info_t* txInfo, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[ESP-NOW] 送出失敗");
  }
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
    // 兩處都保證 WiFi 已連線；序列埠的 ch <n> 測試指令走另一條路，不會誤寫 NVS。
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
  //   2. 只有 channel（BSSID 已在前次失敗時清掉）→ 仍把掃描限制在單一 channel，
  //      master 不會跑遍 1~13，停在舊 channel 的 slave 心跳命中率幾乎不受影響
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

// ── MQTT 連線 ──
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
  bool res = mqttClient.connect(deviceId, cfg.username, cfg.password,
                                statusTopic.c_str(), 1, true, willBuf, true);
  if (!res) {
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

// 連線到使用者自訂伺服器。結構與 quickConnectToIndex() 相同，差別：
// 用 mqttServer/mqttPort，帳密有值才傳、否則傳 NULL，且不更新 currentServerIndex
// （currentServerIndex 只追蹤「輪詢到預設清單的第幾台」，自訂伺服器不佔用這個游標，
// 避免自訂伺服器斷線後，預設清單的輪詢起點被誤導到不相關的位置）。
bool quickConnectCustom() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  mqttClient.setBufferSize(1024);
  mqttClient.setSocketTimeout(3);

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  StaticJsonDocument<160> willDoc;
  willDoc["device_id"] = deviceId;
  willDoc["status"] = "offline";
  willDoc["server"] = mqttServer;
  willDoc["timestamp"] = millis() / 1000;
  char willBuf[160];
  serializeJson(willDoc, willBuf);

  const char* user = (strlen(mqttUsername) > 0) ? mqttUsername : NULL;
  const char* pass = (strlen(mqttPassword) > 0) ? mqttPassword : NULL;

  Serial.printf("[MQTT] 嘗試自訂伺服器 %s …\n", mqttServer);
  bool res = mqttClient.connect(deviceId, user, pass,
                                statusTopic.c_str(), 1, true, willBuf, true);
  if (!res) {
    Serial.printf("[MQTT] 自訂伺服器 %s 失敗，state=%d\n", mqttServer, mqttClient.state());
    return false;
  }

  String controlTopic = String("hoban/") + deviceId + "/control";
  mqttClient.subscribe(controlTopic.c_str());
  Serial.printf("[MQTT] 已連線自訂伺服器 %s，訂閱 %s\n", mqttServer, controlTopic.c_str());
  publishStatus();
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

// 把游標歸零，讓下一次 smartConnect() 重新從自訂伺服器開始。
// FIND_BEST_SERVER 的語義是「重新挑一台最好的」，必須從頭挑，不能接著上次的位置。
void resetMqttProbe() {
  mqttCustomTried = false;
  mqttProbeOffset = 0;
}

void smartConnect() {
  if (!WiFi.isConnected()) return;

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

// 發布完整狀態（Phase 2a 先不含 slaves 陣列，那是 Phase 2b 才加）
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

  // 真正的容量瓶頸是 StaticJsonDocument<512> 與下方 char buf[512]，不是 mqttClient
  // 的 1024 buffer。ArduinoJson 在文件放不下時是「截斷」而非溢位，截斷後的長度仍然
  // < 1024 → publish() 回傳 true → 靜默失敗，訂閱端收到殘缺 JSON 卻沒有任何錯誤。
  // 目前 317/512 還有餘裕，但 Phase 2b 要在這裡加最多 20 台 slave 的陣列，
  // 必定會逼近上限，所以先把截斷偵測補上。
  if (doc.overflowed()) {
    Serial.println("⚠ [MQTT] 狀態 JSON 已超出 StaticJsonDocument<512> 容量並被截斷，"
                   "請加大 doc 與 buf 的容量（publish 仍會回報成功，屬靜默失敗）");
  }

  char buf[512];
  size_t n = serializeJson(doc, buf);
  if (n >= sizeof(buf) - 1) {
    Serial.printf("⚠ [MQTT] 序列化結果已填滿 buf[%u]，內容可能被截斷\n",
                  (unsigned)sizeof(buf));
  }
  bool res = mqttClient.publish(topic.c_str(), buf, true);
  if (!res) {
    // 超過 mqttClient buffer 時 publish() 只回傳 false、不會拋例外，
    // 先把長度印出來讓「快超過了」在序列埠上就看得見。
    Serial.printf("[MQTT] 狀態發布失敗（長度 %u，buffer %u）\n",
                  (unsigned)n, (unsigned)mqttClient.getBufferSize());
  }
}

// 指令分派。Phase 2a 只處理 master 自己的指令；ALL:*、PAIR:*、UNPAIR:* 等
// slave 相關指令留給 Phase 2b。
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
  } else if (message == "HASRELAY:ON" || message == "HASRELAY:OFF") {
    hasRelay = (message == "HASRELAY:ON");
    saveNetConfig();
    Serial.printf("[設定] 繼電器宣告為 %s\n", hasRelay ? "有接" : "未接");
    publishStatus();
  } else {
    Serial.printf("[MQTT] 未知指令: %s\n", message.c_str());
  }
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

  // ── 短按 BOOT 進入配對模式 ──
  // 長按 3 秒以上不觸發，保留給之後的重置功能（屆時請一律走 anyResetButtonPressed()）
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

  // ── 配對模式逾時 ──
  if (pairingMode && now - pairingStartTime >= PAIRING_TIMEOUT) {
    Serial.println("[配對] 逾時");
    exitPairingMode();
  }

  // ── LED 狀態指示（Task 7）──
  // 先推進一次性閃爍請求（配對接受／拒絕），播完的同一輪就會被 updateStatusLed()
  // 接手判斷持續式狀態（BLE 配網／配對模式／WiFi／MQTT／正常），兩者分工與
  // 優先權詳見各自函式上方註釋
  updateBlink(now);
  updateStatusLed(now);

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
  static unsigned long lastReconnect = 0;
  static unsigned long lastStatusPub = 0;

  if (!bleConfigMode && WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (now - lastReconnect > 10000) {
        smartConnect();
        // 與上方 WiFi 重連同一個理由：smartConnect() 內的 mqttClient.connect() 最壞
        // 阻塞約 18 秒（DNS 逾時，見 smartConnect() 註釋），若沿用進入本區塊前的 now
        // 記錄 lastReconnect，下一輪 loop() 的 now 已超前 18 秒以上，10 秒節流立刻
        // 成立、變成背靠背重試，兩次阻塞之間只擠得出一則心跳。必須在阻塞呼叫「之後」
        // 用新的 millis() 記錄，才能保證每兩次嘗試之間有完整 10 秒可發約 10 則心跳。
        lastReconnect = millis();
      }
    } else {
      mqttClient.loop();
      if (now - lastStatusPub > 10000) {   // 每 10 秒發一次狀態，master 還要發心跳與輪詢 slave，比 ho_relay2 的 3 秒寬鬆
        lastStatusPub = now;
        publishStatus();
      }
    }
  }

  // ── 分散式輪詢 slave 狀態，每次 loop() 最多問一台（見 pollNextSlave()）──
  pollNextSlave();

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
