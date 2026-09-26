#pragma once
// 醒來流程的決策純函式與常數。不含 Arduino 相依、全部 constexpr，由 selftest.h 在編譯期驗證。

#include <stdint.h>

namespace wake {

constexpr uint32_t kNormalSleepS = 600;       // 正常回報間隔 10 分鐘
constexpr uint32_t kMaxSleepS = 3600;         // 退避上限 1 小時
constexpr uint32_t kLowBatterySleepS = 3600;  // 電池過低時的睡眠時間

constexpr int kLowBatteryMv = 6200;     // 每顆 3.1V：低於此值不開 WiFi，避免鋰電過放
constexpr int kOtaMinBatteryMv = 7000;  // 約 10%：OTA 下載要醒著一兩分鐘，電量不夠會刷到一半斷電

constexpr uint32_t kWakeBudgetMs = 20000;       // 單次醒來的硬上限（OTA 除外）
constexpr uint32_t kWifiTimeoutMs = 10000;      // WiFi 連線總上限
constexpr uint32_t kWifiCacheTimeoutMs = 4000;  // 用快取的 channel/BSSID 快速連線的上限
constexpr uint32_t kCommandWindowMs = 1500;     // 等 broker 重播 retained 指令的時間
constexpr uint32_t kNtpTimeoutMs = 2000;        // NTP 對時上限
constexpr uint32_t kOtaTimeoutMs = 120000;      // OTA 下載上限

// 開始下一台 broker 連線前，剩餘預算至少要有這麼多：單台最壞情況約 8 秒
// （TCP 連線逾時 3 秒＋CONNACK 逾時 5 秒），留一點餘裕避免試到一半被 20 秒硬上限腰斬
constexpr uint32_t kMinBrokerAttemptMs = 9000;

// 連續失敗 failures 次之後這次要睡多久：0、1 → 600；2 → 1200；3 → 2400；≥4 → 3600。
// 第一次失敗不退避：偶發一次連不上（分享器重開、broker 抖動）很常見，不值得晚 10 分鐘回報。
constexpr uint32_t backoffSleepSeconds(uint32_t failures) {
  uint32_t s = kNormalSleepS;
  for (uint32_t i = 1; i < failures && s < kMaxSleepS; i++) {
    s *= 2;
  }
  return s > kMaxSleepS ? kMaxSleepS : s;
}

// 讀值無效時不套用：讀值不可信就不能據此停止回報，否則分壓脫落的設備會永遠沉默。
constexpr bool shouldSkipForLowBattery(bool valid, int mv) {
  return valid && mv < kLowBatteryMv;
}

// 讀值無效時拒絕：不知道電量就不冒險刷機。
constexpr bool otaBatteryOk(bool valid, int mv) {
  return valid && mv >= kOtaMinBatteryMv;
}

}  // namespace wake

namespace broker {

// 第 attempt 次（從 0 起）要試哪一台。lastIndex 來自 RTC 記憶體，
// 可能是任意殘值（例如改過 broker 數量的舊韌體留下的），越界一律從 0 開始。
constexpr int attemptIndex(int lastIndex, int count, int attempt) {
  const int start = (lastIndex >= 0 && lastIndex < count) ? lastIndex : 0;
  return (start + attempt) % count;
}

}  // namespace broker

namespace ota {

constexpr bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 下載走 setInsecure() 不驗 TLS 憑證，MD5 是唯一能確認映像檔沒壞、沒被掉包的依據，缺了就拒絕。
constexpr bool isValidMd5(const char* s) {
  if (s == nullptr) return false;
  int n = 0;
  for (; s[n] != '\0'; n++) {
    if (n >= 32 || !isHexChar(s[n])) return false;
  }
  return n == 32;
}

}  // namespace ota
