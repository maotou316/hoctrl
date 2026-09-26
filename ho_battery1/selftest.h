#pragma once
// 編譯期自我測試：被 ho_battery1.ino include，任何一條不成立編譯就失敗。
// 本 repo 沒有主機端 C++ 編譯器，純計算只能這樣驗證；所以受測函式都必須是 constexpr。

#include "battery_curve.h"
#include "policy.h"

// ── 分壓換算：5 倍分壓模組（30k/7.5k），比例 5.0 ──
static_assert(battery::adcToBatteryMv(1680) == 8400, "8.4V 滿電的 ADC 讀值要換算回 8400mV");
static_assert(battery::adcToBatteryMv(0) == 0, "0mV 換算後仍為 0");

// ── 讀值有效區間 900～2400mV（電池約 5.0～13.3V），邊界含等號 ──
static_assert(!battery::isAdcReadingValid(899), "低於 900mV 視為分壓脫落");
static_assert(battery::isAdcReadingValid(900), "900mV 是有效下界");
static_assert(battery::isAdcReadingValid(2400), "2400mV 是有效上界");
static_assert(!battery::isAdcReadingValid(2401), "高於 2400mV 超出 11dB 線性區");

// ── 2S 放電曲線（與 hoRelay2 batteryPercentFromMilliVolts() 同一張表）──
static_assert(battery::percentFromMv(9000) == 100, "超過滿電夾到 100");
static_assert(battery::percentFromMv(8400) == 100, "滿電 100");
static_assert(battery::percentFromMv(8260) == 95, "8400~8120 之間內插");
static_assert(battery::percentFromMv(7690) == 55, "7740~7640 之間內插");
static_assert(battery::percentFromMv(7640) == 50, "表上的點精確命中");
static_assert(battery::percentFromMv(6450) == 2, "6900~6000 之間內插（2.5 無條件捨去）");
static_assert(battery::percentFromMv(6000) == 0, "空電 0");
static_assert(battery::percentFromMv(5000) == 0, "低於空電夾到 0");

// ── broker 連線的最小預算門檻：pin 死數值，改動要同步檢查 connectMqtt() 的註解與 readme ──
static_assert(wake::kMinBrokerAttemptMs == 9000, "單台最壞約 8 秒，9000 是設計值，改動需同步文件");

// ── 退避：連續失敗第 n 次後睡多久。第一次失敗仍睡 600，之後倍增，上限 3600 ──
static_assert(wake::backoffSleepSeconds(0) == 600, "沒有失敗睡 600");
static_assert(wake::backoffSleepSeconds(1) == 600, "第 1 次失敗仍睡 600");
static_assert(wake::backoffSleepSeconds(2) == 1200, "第 2 次失敗 1200");
static_assert(wake::backoffSleepSeconds(3) == 2400, "第 3 次失敗 2400");
static_assert(wake::backoffSleepSeconds(4) == 3600, "第 4 次夾到上限 3600");
static_assert(wake::backoffSleepSeconds(1000) == 3600, "失敗次數很大也不溢位");

// ── 電池保護：只在讀值有效且低於 6200mV 時跳過 WiFi ──
static_assert(wake::shouldSkipForLowBattery(true, 6199), "有效且過低 → 跳過");
static_assert(!wake::shouldSkipForLowBattery(true, 6200), "6200 剛好不跳過");
static_assert(!wake::shouldSkipForLowBattery(false, 0), "讀值無效時不可據此停止回報");

// ── OTA 門檻：讀值有效且 ≥7000mV ──
static_assert(wake::otaBatteryOk(true, 7000), "7000 剛好可以");
static_assert(!wake::otaBatteryOk(true, 6999), "低於 7000 拒絕");
static_assert(!wake::otaBatteryOk(false, 8400), "讀值無效時不冒險刷機");

// ── broker 嘗試順序：從上次成功的那台開始輪 ──
static_assert(broker::attemptIndex(2, 4, 0) == 2, "先試上次成功的");
static_assert(broker::attemptIndex(2, 4, 3) == 1, "繞一圈");
static_assert(broker::attemptIndex(-1, 4, 0) == 0, "RTC 殘值為負 → 從 0 開始");
static_assert(broker::attemptIndex(9, 4, 1) == 1, "RTC 殘值越界 → 從 0 開始");

// ── MD5 格式：剛好 32 個十六進位字元 ──
static_assert(ota::isValidMd5("0123456789abcdefABCDEF0123456789"), "32 個十六進位字元");
static_assert(!ota::isValidMd5("0123456789abcdef0123456789abcde"), "31 個字元");
static_assert(!ota::isValidMd5("0123456789abcdef0123456789abcdef0"), "33 個字元");
static_assert(!ota::isValidMd5("g123456789abcdef0123456789abcdef"), "含非十六進位字元");
static_assert(!ota::isValidMd5(""), "空字串");
static_assert(!ota::isValidMd5(nullptr), "缺欄位（ArduinoJson 回 nullptr）");
