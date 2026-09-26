// 齁控 hoBattery1 — ESP32-C3 SuperMini 電量偵測模組
// 醒來量 2S 鋰電電壓 → 發 retained 狀態到 MQTT → 深度睡眠 10 分鐘。
// 設計規格：docs/superpowers/specs/2026-09-27-hobattery1-design.md
//
// 深度睡眠醒來等同重新開機，所以整個流程寫在 setup()，
// 每條路徑都以 goToSleep() 或 ESP.restart() 結束，loop() 永遠不會被執行到。

#include <esp_sleep.h>
#include <esp_system.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"
#include "battery_curve.h"
#include "policy.h"
#include "selftest.h"

const char* firmwareVersion = "1.0.0"; // 當前韌體版本
const char* deviceModel = "hoBattery1";

const int batterySensePin = 3;  // ADC1_CH3。C3 只有 GPIO 0~4 是 ADC1；ADC2 在 WiFi 開啟時讀不到
const int ledPin = 8;           // SuperMini 板載 LED，低電位亮
const int BATTERY_SAMPLES = 16;
const int BATTERY_DISCARD = 2;

// ── 跨深度睡眠保存（斷電或軟體重啟會清除）──
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t consecutiveFailures = 0;  // 連續連不上的次數，決定退避

RTC_DATA_ATTR int lastBrokerIndex = 0;       // 上次連上的 broker，下次優先試
RTC_DATA_ATTR bool wifiCacheValid = false;   // 下面兩個快取是否可用
RTC_DATA_ATTR int32_t wifiChannel = 0;
RTC_DATA_ATTR uint8_t wifiBssid[6] = {0};

// ── 本次醒來的量測結果 ──
int batteryMv = 0;
int batteryPercent = -1;       // -1 代表讀值無效
bool batteryValid = false;
unsigned long measuredAtMs = 0;

unsigned long wakeStartMs = 0;
const char* wakeReason = "power_on";
String deviceIdString;

struct MqttServerConfig {
  const char* server;
  int port;
};

// 與 hoRelay2 的 DEFAULT_SERVERS 同一份清單（皆無帳密）
const MqttServerConfig DEFAULT_SERVERS[] = {
  {"mqttgo.io",               1883},
  {"mqtt.eclipseprojects.io", 1883},
  {"broker.emqx.io",          1883},
  {"broker.hivemq.com",       1883},
};
const int DEFAULT_SERVER_COUNT = sizeof(DEFAULT_SERVERS) / sizeof(DEFAULT_SERVERS[0]);

WiFiClient netClient;
PubSubClient mqttClient(netClient);
const char* activeServer = "";

const char* getDeviceId() {
  if (deviceIdString.length() == 0) {
    uint64_t chipId = ESP.getEfuseMac();
    uint8_t* b = (uint8_t*)&chipId;
    // getEfuseMac() 以小端序存放，b[0] 就是 mac[0]；由 [0] 印到 [5] 才是網路順序（與 hoRelay2 相同）
    char tempId[23];
    snprintf(tempId, sizeof(tempId), "hoban-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    deviceIdString = String(tempId);
  }
  return deviceIdString.c_str();
}

// 本次醒來還剩多少預算（ms），用完回傳 0
uint32_t budgetLeftMs() {
  const unsigned long used = millis() - wakeStartMs;
  return used >= wake::kWakeBudgetMs ? 0 : wake::kWakeBudgetMs - used;
}

const char* detectWakeReason() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) return "timer";
  if (esp_reset_reason() == ESP_RST_SW) return "software";  // OTA 成功後的 ESP.restart()
  // C3 的 RESET 鍵是 EN（晶片致能腳），按下與重新上電在晶片看來是同一件事
  return "power_on";
}

// 必須在開 WiFi 之前呼叫：WiFi 發射電流大，電池內阻壓降會讓讀值偏低。
void measureBattery() {
  analogReadResolution(12);
  // 先讀一次把腳附掛成 ANALOG，之後 analogSetPinAttenuation() 才有效
  // （順序反了會印 "Pin is not configured as analog channel"，hoRelay2 踩過）
  analogReadMilliVolts(batterySensePin);
  analogSetPinAttenuation(batterySensePin, ADC_11db);  // 線性區約 0~2.5V，涵蓋 1.08~1.51V

  for (int i = 0; i < BATTERY_DISCARD; i++) {
    analogReadMilliVolts(batterySensePin);
  }
  long total = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    // 用 analogReadMilliVolts() 而非 analogRead()：前者套用 eFuse 出廠校準
    total += analogReadMilliVolts(batterySensePin);
  }
  const int adcMv = total / BATTERY_SAMPLES;

  batteryValid = battery::isAdcReadingValid(adcMv);
  batteryMv = battery::adcToBatteryMv(adcMv);
  batteryPercent = batteryValid ? battery::percentFromMv(batteryMv) : -1;
  measuredAtMs = millis();

  Serial.printf("電量：ADC %d mV → 電池 %d mV，%d%%，%s\n",
                adcMv, batteryMv, batteryPercent, batteryValid ? "有效" : "無效（分壓脫落或超出量程）");
}

// 不會返回（深度睡眠）。不加 [[noreturn]]：Arduino 自動產生函式原型時可能處理不了屬性
void goToSleep(uint32_t seconds) {
  Serial.printf("本次醒著 %lu ms，睡眠 %lu 秒\n", millis() - wakeStartMs, (unsigned long)seconds);
  Serial.flush();
  digitalWrite(ledPin, HIGH);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

String statusTopic() { return String("hoban/") + getDeviceId() + "/status"; }
String controlTopic() { return String("hoban/") + getDeviceId() + "/control"; }

bool waitWiFi(unsigned long deadlineMs) {
  while (WiFi.status() != WL_CONNECTED && (long)(deadlineMs - millis()) > 0 && budgetLeftMs() > 0) {
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool connectWiFi() {
  WiFi.persistent(false);  // 帳密寫死在韌體，不需要每次寫 NVS
  WiFi.mode(WIFI_STA);
  const unsigned long deadline = millis() + wake::kWifiTimeoutMs;

  if (wifiCacheValid) {
    // 指定 channel/BSSID 可以跳過掃描，連線從約 3 秒降到 1 秒內
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, wifiChannel, wifiBssid, true);
    if (waitWiFi(min(deadline, millis() + wake::kWifiCacheTimeoutMs))) {
      Serial.printf("WiFi 已連線（快取），RSSI %d\n", WiFi.RSSI());
      return true;
    }
    // 分享器換了頻道或換了一台 AP，快取過期
    Serial.println("WiFi 快取連線失敗，改一般連線");
    wifiCacheValid = false;
    WiFi.disconnect();
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (!waitWiFi(deadline)) {
    Serial.println("WiFi 連線逾時");
    return false;
  }
  wifiChannel = WiFi.channel();
  memcpy(wifiBssid, WiFi.BSSID(), 6);
  wifiCacheValid = true;
  Serial.printf("WiFi 已連線，channel %d，RSSI %d\n", (int)wifiChannel, WiFi.RSSI());
  return true;
}

bool connectMqtt() {
  mqttClient.setBufferSize(512);  // 預設 256 放不下狀態 JSON，publish() 會靜默失敗（hoRelay2 踩過）
  mqttClient.setSocketTimeout(5);
  mqttClient.setKeepAlive(15);

  // clientId 帶 bootCount：上一次醒來的連線若還沒被 broker 清掉，同一個 ID 會互踢
  const String clientId = String(getDeviceId()) + "-" + bootCount;

  for (int attempt = 0; attempt < DEFAULT_SERVER_COUNT; attempt++) {
    if (budgetLeftMs() < 2000) break;  // 剩不到一次連線的時間，不如早點睡
    const int idx = broker::attemptIndex(lastBrokerIndex, DEFAULT_SERVER_COUNT, attempt);
    const MqttServerConfig& cfg = DEFAULT_SERVERS[idx];
    Serial.printf("MQTT 連線 [%d] %s ... ", idx, cfg.server);
    mqttClient.setServer(cfg.server, cfg.port);
    // 不設 LWT：休眠設備每次都會斷線，異常斷線時 LWT 會用 offline 蓋掉最新的 retained 狀態
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("成功");
      lastBrokerIndex = idx;
      activeServer = cfg.server;
      return true;
    }
    Serial.printf("失敗（state %d）\n", mqttClient.state());
  }
  return false;
}

// 深度睡眠期間 RTC 時鐘會繼續走，所以之前對過時的話，這次對時失敗仍有可用的時間。
// 每次醒來都重對：RTC 慢速時鐘的誤差約 5%，10 分鐘就可能漂 30 秒。
void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC，measured_at 用 Unix 秒
  const unsigned long start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < wake::kNtpTimeoutMs && budgetLeftMs() > 0) {
    delay(50);
  }
}

// 量測當下的 Unix 秒；時間從未對上過就回 0
uint32_t measuredAtEpoch() {
  const time_t now = time(nullptr);
  if (now < 1700000000) return 0;
  return (uint32_t)(now - (millis() - measuredAtMs) / 1000);
}

bool publishStatus(uint32_t nextWakeS) {
  // ArduinoJson 7 的 JsonDocument 走 heap。CLAUDE.md 要求 StaticJsonDocument 是為了避免
  // 長時間運行的碎片化；這裡每次醒來只配置一次、隨即深度睡眠，不存在碎片問題。
  JsonDocument doc;
  doc["device_id"] = getDeviceId();
  doc["status"] = "sleeping";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["timestamp"] = millis() / 1000;
  doc["measured_at"] = measuredAtEpoch();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = true;
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();

  JsonObject bat = doc["battery"].to<JsonObject>();
  bat["mv"] = batteryMv;
  bat["percent"] = batteryPercent;
  bat["valid"] = batteryValid;

  JsonObject sleepInfo = doc["sleep"].to<JsonObject>();
  sleepInfo["interval_s"] = wake::kNormalSleepS;
  sleepInfo["next_wake_s"] = nextWakeS;
  sleepInfo["boot_count"] = bootCount;
  sleepInfo["wake_reason"] = wakeReason;

  char buf[480];  // 留 32 bytes 給 PubSubClient 512 緩衝區裡的 topic 與標頭
  const size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len == 0 || len >= sizeof(buf) - 1) {
    Serial.printf("狀態 JSON 過大（%u bytes），放不進緩衝區\n", (unsigned)measureJson(doc));
    return false;
  }

  const bool res = mqttClient.publish(statusTopic().c_str(), (const uint8_t*)buf, len, true);
  Serial.printf("發布狀態（%u bytes）到 %s：%s\n%s\n", (unsigned)len, activeServer, res ? "成功" : "失敗", buf);
  return res;
}

// 不會返回（深度睡眠）
void failAndSleep(const char* why) {
  consecutiveFailures++;
  Serial.printf("%s，連續失敗 %lu 次\n", why, (unsigned long)consecutiveFailures);
  goToSleep(wake::backoffSleepSeconds(consecutiveFailures));
}

void setup() {
  wakeStartMs = millis();
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH);  // 熄燈
  Serial.begin(115200);

  bootCount++;
  wakeReason = detectWakeReason();
  // 手動上電／按 RESET 時多等一下讓電腦的 USB CDC 接上，才看得到 log；定時喚醒不等，省電
  if (strcmp(wakeReason, "timer") != 0) {
    delay(1500);
  }
  Serial.printf("\n齁控 %s v%s｜%s｜第 %lu 次醒來（%s）\n",
                deviceModel, firmwareVersion, getDeviceId(), (unsigned long)bootCount, wakeReason);

  measureBattery();

  if (wake::shouldSkipForLowBattery(batteryValid, batteryMv)) {
    Serial.printf("電池低於 %d mV，不開 WiFi 以免過放\n", wake::kLowBatteryMv);
    goToSleep(wake::kLowBatterySleepS);
  }

  if (!connectWiFi()) failAndSleep("WiFi 連不上");
  if (!connectMqtt()) failAndSleep("MQTT 全部 broker 都連不上");

  syncTime();

  if (!publishStatus(wake::kNormalSleepS)) failAndSleep("狀態發布失敗");
  consecutiveFailures = 0;

  // 讓按 RESET 的人知道這次有成功回報；定時喚醒不亮，省電
  if (strcmp(wakeReason, "timer") != 0) {
    digitalWrite(ledPin, LOW);
    delay(500);
    digitalWrite(ledPin, HIGH);
  }

  // 給 publish 的 TCP 封包時間送出，否則緊接著斷線可能把它丟掉（Task 5 會換成收指令的等待窗）
  const unsigned long flushStart = millis();
  while (millis() - flushStart < 300) {
    mqttClient.loop();
    delay(10);
  }
  mqttClient.disconnect();

  goToSleep(wake::kNormalSleepS);
}

void loop() {
  // 不會執行到：setup() 的每條路徑都以深度睡眠或重啟結束
}
