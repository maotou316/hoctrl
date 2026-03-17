# 齁控 (HoCtrl)

ESP32 系列開發板的動物管制遠端繼電器控制系統韌體專案。

## 專案簡介

齁控是一個基於 ESP32 的智慧繼電器控制系統，專為動物管制場景設計。支援透過藍牙配對、WiFi 連線、MQTT 遠端控制和 OTA 韌體更新，提供穩定可靠的遠端控制解決方案。

## 核心功能

- **🔵 藍牙配對** - 透過 BLE 快速配置 WiFi 和 MQTT 設定
- **📡 WiFi 連線** - 自動連線與 AP 模式備援
- **☁️ MQTT 通訊** - 多伺服器策略，自動切換確保連線穩定
- **🔄 OTA 更新** - 遠端韌體更新，無需實體接觸設備
- **🌐 Web 介面** - 內建管理介面，可查看狀態與手動控制
- **💡 狀態指示** - LED 即時反映設備運作狀態

## 硬體型號

### hoRelay1
- **開發板**: uPesy ESP32 WROOM DevKit (Type-C)
- **特色**: 1 路光耦隔離繼電器驅動模塊
- **GPIO**:
  - BOOT 按鈕: GPIO 0
  - 第二按鈕: GPIO 14
  - 繼電器: GPIO 13
  - 內建 LED: GPIO 2

### hoRelay2
- **開發板**: ESP32-C3 Dev Module
- **特色**: 無聲繼電器
- **GPIO**:
  - BOOT 按鈕: GPIO 9
  - 面板 LED: GPIO 0
  - 繼電器按鈕: GPIO 4
- **開發板設定**:
  - USB CDC On Boot: Enabled
  - CPU Frequency: 160MHz (WiFi)
  - Flash Size: 4MB
  - Partition Scheme: Custom (partitions.csv)

### hoRelay3
- **開發板**: ESP32-C3 Dev Module
- **特色**: （規劃中）

## MQTT 架構

### 多伺服器策略

系統內建 5 個預設 MQTT 伺服器 + 自訂伺服器支援：

1. **mqttgo.io** (台灣 MQTT Go)
2. **broker.hoban.tw** (齁斑社企，需認證)
3. **mqtt.eclipseprojects.io** (Eclipse)
4. **broker.emqx.io** (EMQX 公共)
5. **broker.hivemq.com** (HiveMQ 公共)

### 主題架構

```
狀態發布: hoban/{device_id}/status
控制訂閱: hoban/{device_id}/control
```

### 控制指令

| 指令 | 功能 |
|------|------|
| `status` | 請求設備狀態回報 |
| `ON` | 觸發繼電器動作（脈衝 1 秒） |
| `reset` | 重置設備配置 |
| `update:{JSON}` | 韌體更新 |

## 快速開始

### 1. 編譯韌體

```bash
# 安裝 Arduino CLI
# https://arduino.github.io/arduino-cli/

# 編譯 hoRelay2
arduino-cli compile --fqbn esp32:esp32:esp32c3 ho_relay2

# 上傳韌體
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32c3 ho_relay2
```

### 2. 配置設備

**方式一：藍牙配對**

使用 Flutter App 透過 BLE 傳送配置 JSON：

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

**方式二：Web 介面**

1. 長按 BOOT 按鈕 3 秒進入 AP 模式
2. 連接 WiFi SSID: `hoban-{device_id}`
3. 開啟瀏覽器訪問 `192.168.4.1`
4. 輸入 WiFi 設定

### 3. 控制設備

**透過 MQTT 客戶端**

```bash
# 訂閱狀態
mosquitto_sub -h mqttgo.io -t "hoban/+/status"

# 發送控制指令
mosquitto_pub -h mqttgo.io -t "hoban/{device_id}/control" -m "ON"
```

**透過 Web 介面**

訪問設備 IP（可從路由器 DHCP 列表查詢）

## 韌體發布工具 (publish.py)

hoRelay 韌體發布自動化腳本，支援 hoRelay1～3。自動完成版本遞增、編譯、上傳至 GitHub Releases / Firebase Storage，並更新 Firestore 記錄。

### 必要工具

| 工具 | 用途 | 安裝方式 |
|------|------|----------|
| arduino-cli | 編譯韌體 | https://arduino.github.io/arduino-cli/ |
| firebase | Firestore 更新 | `npm install -g firebase-tools` |
| gh | GitHub Releases 上傳（優先） | https://cli.github.com/ |
| gsutil | Firebase Storage 上傳（備選） | https://cloud.google.com/storage/docs/gsutil_install |

### 用法

```bash
# 互動式選擇型號
python publish.py

# 指定型號發布
python publish.py 1                       # hoRelay1 (ESP32 WROOM)
python publish.py 2                       # hoRelay2 (ESP32-C3)
python publish.py 3                       # hoRelay3 (ESP32-C3)

# 附帶更新說明
python publish.py 2 -c "修正 WiFi 連接問題"

# 跳過確認直接發布
python publish.py 1 -y

# 指定最低版本要求
python publish.py 2 -m 1.2.0

# 組合使用
python publish.py 3 -c "新增藍牙配對功能" -m 1.0.0 -y
```

### 參數說明

| 參數 | 說明 | 預設值 |
|------|------|--------|
| `relay` | 型號編號 (1/2/3)，不帶則進入互動選單 | 無 |
| `-c`, `--changelog` | 更新說明 | `修正錯誤，優化效能` |
| `-m`, `--min-version` | 最低版本要求 | `1.0.0` |
| `-y`, `--yes` | 跳過確認直接發布 | 否 |

### 發布流程

1. **檢查工具** — 確認 arduino-cli、firebase 等已安裝
2. **讀取韌體資訊** — 從 `.ino` 讀取目前版本號與設備型號
3. **版本遞增** — 自動將末位版本號 +1（例如 1.3.9 → 1.3.10）
4. **編譯韌體** — 使用 arduino-cli 編譯，產出 `.bin` 檔案
5. **上傳韌體** — 依序嘗試 GitHub Releases → Firebase Storage → gsutil → 手動上傳
6. **更新 Firestore** — 寫入 `firmware_updates/{model}` 文件，設備下次連線時收到更新通知

### 上傳優先順序

1. **GitHub Releases**（需安裝 gh CLI）— 上傳至 `maotou316/hoctrl-firmware`
2. **Python Firebase Storage**（需安裝 `google-cloud-storage`）
3. **gsutil**（需安裝 Google Cloud SDK）
4. **手動上傳** — 提示開啟 Firebase Console 手動操作

## LED 狀態指示

| 狀態 | LED 行為 |
|------|----------|
| 韌體更新中 | 快速閃爍 (200ms) |
| AP 模式 | 慢速閃爍 (1000ms) |
| 正常運作 | 恆亮 |
| WiFi 未連接 | 閃爍 |

## 開發環境

### Arduino IDE 函式庫

- WiFi.h (內建)
- WebServer.h / ESP32WebServer.h
- EEPROM.h (內建)
- PubSubClient (MQTT)
- BLEDevice.h (內建)
- ArduinoJson
- Update.h (內建)
- HTTPClient.h (內建)

### 編譯設定

**hoRelay1 (ESP32 WROOM)**
- 開發板: ESP32 Dev Module
- Upload Speed: 115200

**hoRelay2/3 (ESP32-C3)**
- 開發板: ESP32C3 Dev Module
- USB CDC On Boot: Enabled
- CPU Frequency: 160MHz
- Partition Scheme: Custom
- Upload Speed: 921600

## 相關專案

- **hoctrl-firmware** - 韌體發布倉庫
- **hoctrl_arduino** - Arduino 相關工具
- **Flutter App** - 配套行動應用程式

## 授權

此專案為齁斑社會企業開發維護。

## 聯絡方式

- GitHub: [@maotou316](https://github.com/maotou316)
- 專案問題：請開 Issue
