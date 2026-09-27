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
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

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
unsigned long budgetStartMs = 0;  // 預算從 WiFi 連上才開始算（WiFi 本身另有 kWifiTimeoutMs 上限）
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
String pendingCommand;  // callback 只負責收下來，實際處理在 handlePendingCommand()

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

// WiFi 連上之後還剩多少預算（ms），用完回傳 0
uint32_t budgetLeftMs() {
  const unsigned long used = millis() - budgetStartMs;
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
  delay(20);  // 讓 ADC 腳切換衰減設定後、分壓中點的 100nF 電容重新穩定，避免讀值偏低且抖動

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
  while (WiFi.status() != WL_CONNECTED && (long)(deadlineMs - millis()) > 0) {
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

// 診斷用：記下最後一次斷線原因碼（201 找不到基地台、15 四向交握逾時多半是密碼錯、2/4 認證或關聯被拒）
volatile int lastWifiDisconnectReason = 0;

bool connectWiFi() {
  WiFi.persistent(false);  // 帳密寫死在韌體，不需要每次寫 NVS
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
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
    Serial.printf("WiFi 快取連線失敗（最後斷線原因 %d），改一般連線\n", lastWifiDisconnectReason);
    wifiCacheValid = false;
    WiFi.disconnect();
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (!waitWiFi(deadline)) {
    Serial.printf("WiFi 連線逾時（status %d，最後斷線原因 %d）\n", (int)WiFi.status(), lastWifiDisconnectReason);
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

  int attemptsMade = 0;  // 這次醒來實際試過幾台，全部失敗時用來推進 lastBrokerIndex
  for (int attempt = 0; attempt < DEFAULT_SERVER_COUNT; attempt++) {
    // 單台最壞情況約 8 秒（TCP 3 秒＋CONNACK 逾時 5 秒），剩不到這個預算就不值得再試，早點睡
    if (budgetLeftMs() < wake::kMinBrokerAttemptMs) break;
    const int idx = broker::attemptIndex(lastBrokerIndex, DEFAULT_SERVER_COUNT, attempt);
    attemptsMade++;
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
  // 全部失敗：把 lastBrokerIndex 往後推「這次實際試過的台數」，讓下次醒來從沒試過的那台開始，
  // 避免永遠卡在同兩台慢失敗的 broker 上（例如預算只夠試 2 台時，下次該從第 3 台試起）
  lastBrokerIndex = broker::attemptIndex(lastBrokerIndex, DEFAULT_SERVER_COUNT, attemptsMade);
  return false;
}

// 若 RTC 時鐘先前已經對時成功過，深度睡眠期間它會繼續走，time(nullptr) 在呼叫當下
// 就已經有效，下面的等待迴圈幾乎立刻結束——本次 measured_at 用的其實是這個 RTC 時鐘推算出來的
// 舊時間，不是這次剛拿到的 SNTP 結果。configTime() 觸發的 SNTP 對時是背景非同步進行，
// 真正的新時間會在稍後（receiveCommands() 的收指令窗內）才寫回系統時鐘，校正的是「下一次」
// 醒來讀到的時間。每次醒來都重新呼叫是因為 RTC 慢速時鐘的誤差約 5%，10 分鐘就可能漂 30 秒，
// 需要持續追上。
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

  char buf[474];  // 留 38 bytes 給 PubSubClient 512 緩衝區（5 bytes 固定標頭 + 2 bytes topic 長度 + 31 bytes topic）
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

// PubSubClient 的 callback 是在 mqttClient.loop() 裡面被呼叫的，
// 在這裡直接跑 OTA 會卡住 loop() 幾十秒，所以只把內容存下來。
// 注意：mqttClient.setBufferSize(512) 的緩衝區收發共用，指令超過約 470 bytes 會被
// PubSubClient 靜默丟棄（連這個 callback 都不會被呼叫），所以 OTA 網址要盡量短。
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length == 0) return;  // 我們自己清除 retained 時發的空訊息會繞回來，忽略
  pendingCommand = "";
  pendingCommand.reserve(length);
  for (unsigned int i = 0; i < length; i++) pendingCommand += (char)payload[i];
  Serial.printf("收到指令（%s）：%s\n", topic, pendingCommand.c_str());
}

void receiveCommands() {
  mqttClient.setCallback(mqttCallback);
  if (!mqttClient.subscribe(controlTopic().c_str())) {
    Serial.println("訂閱控制主題失敗");
    return;
  }
  // 等 broker 重播 retained 指令；同時讓前面 publish 的封包送出
  const unsigned long start = millis();
  while (millis() - start < wake::kCommandWindowMs && budgetLeftMs() > 0) {
    mqttClient.loop();
    delay(10);
  }
}

// OTA 結果字串與 hoRelay2 相同，但不 retained：
// 休眠設備的 retained 狀態是 App 平常唯一看得到的資料，被字串蓋掉要等 10 分鐘才恢復。
void publishOtaResult(const char* res) {
  mqttClient.publish(statusTopic().c_str(), res, false);
  mqttClient.loop();
  Serial.printf("OTA：%s\n", res);
}

void runOta(const char* url, const char* md5) {
  publishOtaResult("updating");
  WiFi.setSleep(false);  // 下載期間射頻常開，modem-sleep 會讓下載慢數倍

  WiFiClientSecure client;
  client.setInsecure();  // 不驗憑證，完整性完全靠 MD5（呼叫端已確認格式）
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub Release 會轉址
  http.setTimeout(15000);

  if (!http.begin(client, url)) {
    publishOtaResult("update_failed");
    return;
  }
  const int code = http.GET();
  const int contentLength = http.getSize();
  if (code != HTTP_CODE_OK || contentLength <= 0 || contentLength > (int)ESP.getFreeSketchSpace()) {
    Serial.printf("OTA 下載失敗：HTTP %d，大小 %d\n", code, contentLength);
    http.end();
    publishOtaResult("update_failed");
    return;
  }

  // setMD5() 必須在 begin() 之後：begin() 會重置內部的 MD5 狀態
  if (!Update.begin(contentLength) || !Update.setMD5(md5)) {
    Serial.printf("Update 初始化失敗，錯誤碼 %d\n", Update.getError());
    Update.abort();
    http.end();
    publishOtaResult("update_failed");
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buff[1024];
  int written = 0;
  const unsigned long start = millis();
  bool ledOn = false;
  unsigned long lastBlink = start;
  unsigned long lastMqttLoop = start;  // OTA 最久可跑 120 秒，遠超過 keepAlive 15 秒，
                                       // 下載中若完全不呼叫 loop()，broker 會判定斷線把連線收掉，
                                       // 之後的 update_success/update_failed 會發不出去（spec §8 要求回報失敗原因）

  while (http.connected() && written < contentLength && millis() - start < wake::kOtaTimeoutMs) {
    const size_t avail = stream->available();
    if (avail) {
      const size_t n = stream->readBytes(buff, min(avail, sizeof(buff)));
      written += Update.write(buff, n);
    }
    if (millis() - lastBlink >= 200) {  // 更新中 LED 快閃
      ledOn = !ledOn;
      digitalWrite(ledPin, ledOn ? LOW : HIGH);
      lastBlink = millis();
    }
    if (millis() - lastMqttLoop >= 1000) {  // 頻率拉低：太頻繁呼叫會排擠下載吞吐量
      mqttClient.loop();
      lastMqttLoop = millis();
    }
    delay(1);
  }
  http.end();
  digitalWrite(ledPin, HIGH);

  // 不可用 end(true)：那會跳過「寫滿了沒」的檢查，截斷的映像檔也會被接受（hoRelay2 的教訓）
  if (written == contentLength && Update.end()) {
    publishOtaResult("update_success");
    mqttClient.disconnect();
    delay(200);
    ESP.restart();  // 重啟後 wake_reason = software，會立刻發一則新狀態
  }

  Serial.printf("OTA 失敗：寫入 %d/%d bytes，錯誤碼 %d%s\n", written, contentLength, Update.getError(),
                Update.getError() == UPDATE_ERROR_MD5 ? "（MD5 不符）" : "");
  Update.abort();  // otadata 不會切換，設備維持現有韌體
  publishOtaResult("update_failed");
}

void handlePendingCommand() {
  if (pendingCommand.length() == 0) return;

  if (!pendingCommand.startsWith("update:")) {
    // status 本來就不需要處理（醒來已經發過）；其他指令這個型號不支援
    Serial.printf("忽略指令：%s\n", pendingCommand.c_str());
    return;
  }

  // 先清掉 retained 指令，再做任何判斷。順序不能反：
  // 若下載或檢查失敗後才清，或拒絕路徑忘了清，每次醒來都會重新執行，OTA 失敗時會耗盡電池。
  // 下面兩個防護（清除失敗中止、同版本不刷）合起來確保：即使 retained 指令一時清不掉
  // 或被重複送達，最多也只會多刷一次機，不會每次醒來都重跑。
  const bool cleared = mqttClient.publish(controlTopic().c_str(), (const uint8_t*)"", 0, true);
  mqttClient.loop();
  if (!cleared) {
    // 清除失敗（例如 1.5 秒等待窗結束前連線剛好斷了）：這次放棄處理，
    // 下次醒來 retained 指令還在，會重新走一次清除，而不是在沒清乾淨的狀態下先刷機。
    Serial.println("清除 retained 指令失敗，本次醒來放棄處理，下次醒來重試清除");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, pendingCommand.substring(7))) {
    Serial.println("更新指令 JSON 解析失敗");
    publishOtaResult("update_failed");
    return;
  }
  const char* url = doc["url"];
  const char* md5 = doc["md5"];
  const char* version = doc["version"];
  Serial.printf("更新指令：版本 %s，網址 %s\n", version ? version : "(無)", url ? url : "(無)");

  if (version != nullptr && strcmp(version, firmwareVersion) == 0) {
    // 已經是這個版本：多半是同一個 retained 指令被重複送達，不需要再刷一次。
    Serial.println("指令版本與目前韌體相同，略過 OTA");
    return;
  }

  if (url == nullptr) {
    publishOtaResult("update_failed");
    return;
  }
  if (!ota::isValidMd5(md5)) {
    publishOtaResult("update_rejected_no_md5");
    return;
  }
  if (!wake::otaBatteryOk(batteryValid, batteryMv)) {
    publishOtaResult("update_rejected_low_battery");
    return;
  }
  runOta(url, md5);
}

void setup() {
  wakeStartMs = millis();
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH);  // 熄燈
  Serial.begin(115200);

  bootCount++;
  wakeReason = detectWakeReason();

  // 只在非定時喚醒時才檢查：深度睡眠的正常喚醒固定走 ESP_SLEEP_WAKEUP_TIMER，
  // reset_reason 是 ESP_RST_DEEPSLEEP，不會誤觸下面的分支。
  // brownout／panic／看門狗重置不是深度睡眠喚醒：RTC_DATA_ATTR 變數（consecutiveFailures 等）
  // 會被重新初始化成 0，若照常往下跑到 connectWiFi() 拉大電流，電壓撐不住會再次 brownout，
  // 形成「不退避、持續耗電、完全不回報任何狀態」的無限重開機迴圈。
  if (strcmp(wakeReason, "timer") != 0) {
    const esp_reset_reason_t resetReason = esp_reset_reason();
    if (resetReason == ESP_RST_BROWNOUT) {
      Serial.println("偵測到 brownout 重置，睡 1 小時避免無限重開機耗電");
      goToSleep(wake::kLowBatterySleepS);
    }
    if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
        resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT) {
      Serial.println("偵測到例外／看門狗重置，睡眠後重試");
      goToSleep(wake::kNormalSleepS);
    }
  }

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

  // WiFi 連不上不走退避：只睡 1 分鐘就重試，讓設備盡快回到線上（使用者要求）。
  // 代價：分享器長時間故障時，每分鐘醒來等 60 秒，耗電約為一直醒著的一半。
  if (!connectWiFi()) {
    Serial.printf("WiFi %lu 秒內連不上，%lu 秒後重試\n",
                  (unsigned long)(wake::kWifiTimeoutMs / 1000), (unsigned long)wake::kWifiRetrySleepS);
    goToSleep(wake::kWifiRetrySleepS);
  }
  budgetStartMs = millis();
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

  receiveCommands();       // 1.5 秒等待窗同時讓 publish 的封包送出
  handlePendingCommand();  // OTA 成功會直接重啟，不會回到這裡
  mqttClient.disconnect();

  goToSleep(wake::kNormalSleepS);
}

void loop() {
  // 不會執行到：setup() 的每條路徑都以深度睡眠或重啟結束
}
