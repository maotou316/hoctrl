#pragma once

// WiFi 寫死（使用者決定不做配網，見設計規格第 4 節）。
// 換 WiFi 只能改這裡重新燒錄或 OTA；若舊 WiFi 已連不上，OTA 也收不到，只能接 USB 燒錄。
#define WIFI_SSID     "HBTech"
#define WIFI_PASSWORD "94051311"
