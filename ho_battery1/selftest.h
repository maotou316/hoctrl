#pragma once
// 編譯期自我測試：被 ho_battery1.ino include，任何一條不成立編譯就失敗。
// 本 repo 沒有主機端 C++ 編譯器，純計算只能這樣驗證；所以受測函式都必須是 constexpr。

#include "battery_curve.h"

// ── 分壓換算：1MΩ/220kΩ，比例 5.545 ──
static_assert(battery::adcToBatteryMv(1515) == 8400, "8.4V 滿電的 ADC 讀值要換算回 8400mV");
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
