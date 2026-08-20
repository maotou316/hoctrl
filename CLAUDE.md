# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案概述

齁控 Arduino 韌體專案，用於 ESP32 系列開發板的動物管制遠端繼電器控制系統。支援藍牙配對、WiFi 連線、MQTT 通訊和 OTA 韌體更新。

## 硬體型號

專案包含兩個硬體版本：

### hoRelay1
- **開發板**: uPesy ESP32 WROOM DevKit (Type-C)
- **硬體**: 1 路光耦隔離繼電器驅動模塊
- **韌體版本**: 1.0.5
- **GPIO 定義**:
  - BOOT 按鈕: GPIO 0
  - 第二按鈕: GPIO 14
  - 繼電器: GPIO 13
  - 內建 LED: GPIO 2

### hoRelay2
- **開發板**: ESP32-C3 Dev Module
- **特色**: 無聲繼電器
- **韌體版本**: 1.2.1
- **GPIO 定義**:
  - BOOT 按鈕: GPIO 9
  - RESET 按鈕: GPIO 1
  - 板載 LED: GPIO 3
  - 面板 LED: GPIO 0
  - 繼電器按鈕: GPIO 4 與 GPIO 7（兩支同時驅動，單一韌體通吃兩版板子）
    - PCB 絲印 341305A_P25_250814 → 實際接 GPIO 7；341305A_Y176_250318 → 實際接 GPIO 4
    - 兩支腳都在 `setup()` 第一行由 `initRelayPins()` 拉低，避免未初始化的腳浮空導致 MOS 誤導通、繼電器恆閉燒毀設備
- **開發板設定**:
  - USB CDC On Boot: Enabled
  - CPU Frequency: 160MHz (WiFi)
  - Flash Size: 4MB (32Mb)
  - Partition Scheme: Custom (使用 partitions.csv)
  - Upload Speed: 921600
  - Flash Mode: DIO

## 核心功能架構

### 1. 藍牙 (BLE) 配對

- **Service UUID**: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- **Characteristic UUID**: `beb5483e-36e1-4688-b7f5-ea07361b26a8`
- **用途**: 初次配置 WiFi 設定
- **JSON 格式**:
```json
{
  "wifi": {
    "ssid": "WiFi名稱",
    "password": "WiFi密碼"
  },
  "mqtt": {
    "server": "自訂伺服器",
    "port": 1883,
    "username": "帳號（選用）",
    "password": "密碼（選用）"
  }
}
```

### 2. WiFi 管理

- **配置儲存**: EEPROM (前 64 bytes)
  - SSID: 0-31 bytes
  - Password: 32-63 bytes
  - MQTT 設定: 64+ bytes (hoRelay2)

- **AP 模式**: 當 WiFi 未連線時自動啟動
  - 提供 Web 介面進行設定
  - 同時啟動 BLE 配對模式
  - ⚠ **這一條只適用 hoRelay1。hoRelay2 沒有 AP 模式**（連 `WebServer.h` 都沒 include）：
    它的 `bleConfigMode` 只在 `setup()` 讀到「EEPROM 沒存 SSID」時才設為 true，
    `loop()` 裡沒有任何 fallback。SSID 被改掉或 AP 永久消失時，唯一的復原手段是實體長按 5 秒。
    詳見 `.claude/rules/wifi-mqtt-reconnect-antipatterns.md` 的「已知未處理」

### 3. MQTT 通訊架構

#### 多伺服器策略

系統支援 **5 個預設 MQTT 伺服器** + 自訂伺服器：

1. **mqttgo.io** (台灣 MQTT Go)
2. **broker.hoban.tw** (齁斑社企，需認證)
3. **mqtt.eclipseprojects.io** (Eclipse)
4. **broker.emqx.io** (EMQX 公共)
5. **broker.hivemq.com** (HiveMQ 公共)

#### 智慧連線機制

1. **優先順序**:
   - 自訂伺服器（如有設定）
   - 上次成功的伺服器
   - 循環測試所有預設伺服器（1 秒快速測試）

2. **重連策略**:
   - 失敗 3 次自動切換到下一個伺服器
   - 持續嘗試直到連線成功

#### MQTT 主題架構

```
狀態發布: hoban/{device_id}/status
控制訂閱: hoban/{device_id}/control
```

#### 控制指令

| 指令 | 功能 |
|------|------|
| `status` | 請求設備狀態回報 |
| `ON` | 觸發繼電器動作（脈衝 1 秒） |
| `reset` | 重置設備配置 |
| `update:{JSON}` | 韌體更新 |

#### 狀態回報

- **頻率**: 每 15 秒
- **格式**: JSON
- **內容**:
  - `device_id`: 設備識別碼
  - `status`: online/offline/updating
  - `firmware_version`: 韌體版本
  - `wifi`: WiFi 資訊（SSID, IP, RSSI）
  - `mqtt`: MQTT 連線資訊
  - `device`: 設備資訊（記憶體、運行時間）

### 4. OTA 韌體更新

- **觸發方式**: MQTT 指令或 Web 介面
- **更新指令格式**:
```json
{
  "version": "1.0.6",
  "url": "https://example.com/firmware.bin"
}
```
- **更新過程**: LED 快速閃爍（200ms 間隔）
- **更新狀態**: 透過 MQTT 發布更新進度

### 5. LED 狀態指示

hoRelay2 的實際行為（`blinkLED()`）：

| 狀態 | 判斷條件 | LED 行為 |
|------|----------|----------|
| BLE 配對模式（含長按清除設定後） | `bleConfigMode` | 快閃 200ms（`PAIRING_BLINK`），**不熄燈** |
| WiFi 未連接 | `WiFi.status() != WL_CONNECTED` | 快閃 300ms（`QUICK_BLINK`），30 秒後熄燈省電 |
| WiFi 已連、MQTT 未連 | — | 一長二短 |
| WiFi 與 MQTT 都已連上 | — | 熄燈 |
| 長按重置確認中 | `isBlinking`（在 `loop()` 直接控腳，繞過 `blinkLED()`） | 閃爍 250ms（`BLINK_INTERVAL`），確認後長亮 0.7 秒 |

配對模式 200ms 與 WiFi 未連接 300ms 刻意取不同值，肉眼可分辨兩種狀態。

hoRelay2 的 `isUpdating` **不會**影響 LED（韌體更新中沿用當下的連線狀態閃法）；
「韌體更新中 200ms 快閃」是 hoRelay1 的行為。

### 6. Web 管理介面

- **啟動條件**: AP 模式或已連接 WiFi
- **網址**: 設備 IP (預設 192.168.4.1 於 AP 模式)
- **功能**:
  - 顯示設備資訊（Device ID、韌體版本）
  - 顯示網路狀態（SSID、IP）
  - 顯示 MQTT 狀態（伺服器、主題、連線狀態）
  - WiFi 設定
  - WiFi 設定清除
  - 繼電器控制（開門按鈕）
- **UI**: Bootstrap 5.3.3

## 開發環境設定

### Arduino IDE 設定

1. **安裝 ESP32 開發板支援**:
   - 在偏好設定中新增 Board Manager URL
   - 安裝 ESP32 by Espressif Systems

2. **必要函式庫**:
   - `WiFi.h` (內建)
   - `WebServer.h` (ESP32 WROOM) / 無需 (ESP32-C3)
   - `EEPROM.h` (內建)
   - `PubSubClient` (MQTT)
   - `BLEDevice.h` (內建)
   - `ArduinoJson` (第三方)
   - `Update.h` (內建)
   - `HTTPClient.h` (內建)
   - `WiFiClientSecure.h` (內建)

### 編譯與上傳

```bash
# 使用 Arduino IDE 或 Arduino CLI

# 選擇正確的開發板
# hoRelay1: ESP32 Dev Module
# hoRelay2: ESP32C3 Dev Module

# 設定上傳速度: 921600 (hoRelay2) / 115200 (hoRelay1)

# 上傳韌體
arduino-cli compile --fqbn esp32:esp32:esp32c3 ho_relay2
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32c3 ho_relay2
```

### hoRelay2 特殊設定

由於使用自訂分區表，需要在 Arduino IDE 中：
1. 將 `partitions.csv` 放在專案目錄
2. 選擇 "Custom" Partition Scheme
3. 或在 `arduino-cli` 中指定分區表

## 設備 ID 生成

設備 ID 格式: `hoban-{MAC_ADDRESS}`

範例: `hoban-a0b1c2d3e4f5`

使用 ESP32 的 eFuse MAC 地址生成唯一識別碼。

## 重要注意事項

### 變數命名慣例
- 結果變數使用 `res` 而非 `result`

### MQTT 認證
- **預設伺服器**: 帳密寫死在韌體中（`DEFAULT_SERVERS` 陣列）
- **自訂伺服器**: 透過 BLE 或 Web 介面配置，儲存在 EEPROM

### 記憶體管理
- 使用 `StaticJsonDocument` 而非 `DynamicJsonDocument` 以避免記憶體碎片
- JSON 文件大小通常設為 200 bytes

### 長按重置功能

hoRelay2（BOOT GPIO 9 或 RESET GPIO 1 任一顆，總共按住 5 秒）：
- 按住滿 3 秒（`LONG_PRESS_TIME`）→ LED 以 250ms 週期閃爍（`BLINK_INTERVAL`）
- 閃爍期間**持續按住**再 2 秒（`BLINK_CONFIRM_TIME`）→ LED 長亮 0.7 秒（`CONFIRM_SOLID_TIME`）→ 清除 EEPROM 並重啟
- 中途放開即取消，計時歸零
- WiFi 連線等待期間共用 `waitForResetConfirm()`，行為與正常運作時一致
  （1.7.0 之前另有 `interruptibleDelay()` 也共用它，該函式已隨 WiFi 重連重寫一併移除）

hoRelay1 / hoRelay3 仍為舊行為（長按 3 秒 → 閃爍 3 秒 → 確認重置）。

hoRelay2 另有**開機按鈕自檢**（`checkStuckButtons()`）：開機取樣 500ms，整段都是 LOW 的腳
判定為短路／未接，本次開機停用其重置功能。防止一顆壞按鈕造成「開機即清除設定 → 重啟 →
再清除」的無限迴圈（2026-08-14 實際發生過，RESET 按鈕 GPIO 1 內部短路）。
副作用：「按住按鈕再上電」會被擋掉，放開後重新上電即恢復。
詳見 `.claude/rules/button-pin-stuck-low.md`。

### hoRelay2 開機瞬間繼電器短暫通電（硬體限制）

上電時繼電器會短暫通電再斷掉，**這是硬體限制、韌體無法根治**，不要再嘗試用軟體解決。

- **成因**: GPIO 4/7 是 ESP32-C3 的 JTAG 腳（MTMS/MTDO），reset 後由 ROM 配置、不保證為低電位；上電到第一行使用者程式之間約 200~500ms 的空窗，韌體管不到
- **已做的緩解**: `initRelayPins()` 放在 `setup()` 第一行（早於 `Serial.begin()`），把窗口壓到只剩 ROM 空窗。**修改 `setup()` 時務必保持它在第一行**
- **根治方式**: 需硬體在 MOS gate 對地加 10kΩ 下拉電阻，下一版 layout 補上
- 詳見 `ho_relay2/readme.md` 的「已知硬體限制」章節

### Web 介面開發
- 使用 Bootstrap CDN (5.3.3)
- 所有 HTML 在程式碼中生成（避免使用 SPIFFS）
- 支援繁體中文（使用 UTF-8）

## Flutter App 整合

此韌體與 Flutter App (`hoctrl`) 配合使用：

- **BLE 配對**: App 透過 BLE 傳送 WiFi 設定
- **MQTT 控制**: App 訂閱相同的 MQTT 主題進行控制
- **多伺服器**: 韌體和 App 使用相同的 5 個伺服器列表
- **狀態同步**: 透過 MQTT 訊息實時同步設備狀態

## 版本發佈流程

1. 更新 `firmwareVersion` 常數
2. 更新 `readme.md` 版本記錄
3. 編譯韌體
4. 測試功能
5. 上傳 `.bin` 檔案到伺服器（供 OTA 使用）
6. 透過 MQTT 或 App 觸發更新

## 除錯技巧

- 使用 Serial Monitor (115200 baud)
- 查看 MQTT 連線狀態訊息
- 檢查 LED 閃爍模式判斷設備狀態
- 使用 MQTT 客戶端工具（如 MQTT Explorer）監聽主題
- 檢查 Web 介面的設備資訊頁面

## 常見問題

1. **WiFi 無法連線**:
   - 長按 BOOT 重置設定
   - 使用 AP 模式重新配置

2. **MQTT 無法連線**:
   - 檢查伺服器列表
   - 確認網路防火牆設定
   - 系統會自動切換伺服器

3. **BLE 無法配對**:
   - 確認 `CONFIG_BT_ENABLED` 已啟用
   - 重啟設備
   - 檢查 BLE Service UUID

4. **韌體更新失敗**:
   - 確認下載 URL 可訪問
   - 檢查記憶體空間
   - 使用正確的分區表 (hoRelay2)
