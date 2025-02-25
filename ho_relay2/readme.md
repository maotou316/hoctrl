型號：hoRelay2
出廠年份：2025

設定：
- ESP32C3 Dev Module
- USB CDC On Boot: Enabled
- CPU Frequency: 160MHz(WiFi)
- Core Debug Level: Error
- Erase All Flash Before Sketch Upload: Enabled
- Flash Frequency: 80MHz
- Flash Mode: DIO
- Flash Size: 4MB(32Mb)
- JTAG: Disabled
- Partition Scheme: Custom
- Upload Speed: 921600
- Zigbee: Disabled

特色：
- 無聲

板載按鈕：
- BUTTON_PIN 9  //boot鍵IO
- RELAY_PIN 4  //繼電器IO
- LED_PIN 3  //狀態指示燈IO
- IO0  //額外引出IO LED
- IO1  //額外引出IO 按鈕


v1.1.0
- 新增：韌體更新過程燈號閃爍
- 新增：伺服器列表
- 新增：伺服器選擇

v1.0.8 2025-02-16
- 新增：未連接WiFi時，啟動AP模式和BLE配對模式
- 新增：未連接WiFi時，燈號閃爍

v1.0.7 2025-02-16
- 優化：版本顯示
- 優化：燈號顯示
- 新增：韌體更新功能
- 新增：WiFi設定清除功能
- 新增：韌體更新過程燈號閃爍
- 新增：韌體更新過程訊息提示


## MQTT 技術規格

### 1. 連線配置
| 參數 | 值 |
|------|-----|
| 代理伺服器 | 可選擇以下伺服器：<br>- 台灣mqtt (broker.MQTTGO.io)<br>- 齁斑社企 (broker.hoban.tw) |
| 連接埠 | 1883 |
| 認證方式 | 無需認證 |
| 重連機制 | 自動重連，最大重試次數：5 |
| QoS 等級 | 0 |

### 2. 伺服器選擇
| 伺服器名稱 | 伺服器位址 | 說明 |
|------------|------------|------|
| 台灣mqtt | broker.MQTTGO.io | 台灣在地免費公共 MQTT 伺服器 |
| 齁斑社企 | broker.hoban.tw | 齁斑社企專用 MQTT 伺服器 |

### 3. 主題架構
```
狀態發布主題：hoban/{device_id}/status
控制訂閱主題：hoban/{device_id}/control
```

### 4. 控制指令集
| 指令 | 功能描述 |
|------|----------|
| status | 請求設備狀態回報 |
| ON | 觸發繼電器動作 |
| reset | 重置設備配置 |
| update | 觸發韌體更新程序 |

### 5. 狀態回報機制
- **回報頻率**：15秒/次
- **回報內容**：
  - 設備識別碼 (device_id)
  - 韌體版本號
  - WiFi 連線狀態
  - 系統記憶體使用量
- **離線檢測**：
  - 實作 Last Will and Testament (LWT)
  - 支援即時離線通知

### 6. LED 狀態指示
| 狀態 | LED 行為模式 |
|------|-------------|
| 韌體更新中 | 快速閃爍 (200ms 間隔) |
| AP 模式 | 慢速閃爍 (1000ms 間隔) |
| 正常運作 | 恆亮 |