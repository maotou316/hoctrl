#pragma once
// 電池換算的純函式。不含任何 Arduino 相依、全部 constexpr，
// 才能在 selftest.h 用 static_assert 於編譯期驗證。

namespace battery {

// 分壓比：沿用 hoRelay2 的 5 倍分壓模組（上臂 30kΩ、下臂 7.5kΩ → (30 + 7.5) / 7.5 = 5.0）。
// 模組常時漏電約 0.22mA（8.4V ÷ 37.5kΩ），是睡眠電流的大宗；日後要更省電可改自焊 1MΩ/220kΩ，比例改 5545。
// 用千分比整數而不是 float：constexpr 內的浮點運算在不同編譯器的捨入不一定一致，
// 測試值會因此飄動。電阻有誤差，首台實機用電表校正後改這個數字（見 readme 的校正步驟）。
constexpr int kScalePermille = 5000;

// ADC 讀值的有效區間（mV）。對應電池約 5.0～13.3V。
// 低於下界：分壓脫落或電池沒接；高於上界：超出 11dB 衰減的線性區（約 2.5V 起飽和）。
constexpr int kValidMinAdcMv = 900;
constexpr int kValidMaxAdcMv = 2400;

constexpr int adcToBatteryMv(int adcMv) {
  return adcMv * kScalePermille / 1000;
}

constexpr bool isAdcReadingValid(int adcMv) {
  return adcMv >= kValidMinAdcMv && adcMv <= kValidMaxAdcMv;
}

// 2S 鋰電放電曲線（單顆 SOC 曲線 ×2），與 hoRelay2 的 batteryPercentFromMilliVolts() 同一張表。
// 不用線性換算：鋰電中段極平坦，7.74V 到 7.58V 就跨掉 20% 電量。
constexpr int kCurve[][2] = {
  {8400, 100}, {8120, 90}, {7960, 80}, {7840, 70}, {7740, 60}, {7640, 50},
  {7580, 40}, {7540, 30}, {7480, 20}, {7360, 10}, {6900, 5}, {6000, 0}
};
constexpr int kCurvePoints = sizeof(kCurve) / sizeof(kCurve[0]);

constexpr int percentFromMv(int mv) {
  if (mv >= kCurve[0][0]) return 100;
  if (mv <= kCurve[kCurvePoints - 1][0]) return 0;
  for (int i = 0; i < kCurvePoints - 1; i++) {
    if (mv <= kCurve[i][0] && mv > kCurve[i + 1][0]) {
      const int mvSpan = kCurve[i][0] - kCurve[i + 1][0];
      const int pctSpan = kCurve[i][1] - kCurve[i + 1][1];
      return kCurve[i + 1][1] + (mv - kCurve[i + 1][0]) * pctSpan / mvSpan;
    }
  }
  return 0;
}

}  // namespace battery
