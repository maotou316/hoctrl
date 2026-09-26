// 齁控 hoBattery1 — ESP32-C3 SuperMini 電量偵測模組
// 醒來量 2S 鋰電電壓 → 發 retained 狀態到 MQTT → 深度睡眠 10 分鐘。
// 設計規格：docs/superpowers/specs/2026-09-27-hobattery1-design.md
//
// 深度睡眠醒來等同重新開機，所以整個流程寫在 setup()，
// 每條路徑都以 goToSleep() 或 ESP.restart() 結束，loop() 永遠不會被執行到。

#include <esp_sleep.h>
#include <esp_system.h>

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

// ── 本次醒來的量測結果 ──
int batteryMv = 0;
int batteryPercent = -1;       // -1 代表讀值無效
bool batteryValid = false;
unsigned long measuredAtMs = 0;

unsigned long wakeStartMs = 0;
const char* wakeReason = "power_on";
String deviceIdString;

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
  Serial.printf("本次醒著 %lu ms，睡眠 %u 秒\n", millis() - wakeStartMs, seconds);
  Serial.flush();
  digitalWrite(ledPin, HIGH);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
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
  Serial.printf("\n齁控 %s v%s｜%s｜第 %u 次醒來（%s）\n",
                deviceModel, firmwareVersion, getDeviceId(), bootCount, wakeReason);

  measureBattery();

  if (wake::shouldSkipForLowBattery(batteryValid, batteryMv)) {
    Serial.printf("電池低於 %d mV，不開 WiFi 以免過放\n", wake::kLowBatteryMv);
    goToSleep(wake::kLowBatterySleepS);
  }

  goToSleep(wake::kNormalSleepS);
}

void loop() {
  // 不會執行到：setup() 的每條路徑都以深度睡眠或重啟結束
}
