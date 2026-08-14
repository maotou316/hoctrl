# hoSlave1 — ESP-NOW 受控端

## 硬體

沿用 hoRelay2 的 ESP32-C3 繼電器板，GPIO 定義完全相同：
BOOT 9、RESET 1、板載 LED 3、面板 LED 0、繼電器 4 與 7（兩支同時驅動）。

**開機瞬間繼電器短暫通電是硬體限制**，與 hoRelay2 相同，
詳見 `ho_relay2/readme.md`。`initRelayPins()` 必須維持在 `setup()` 第一行。

## 角色

不連 WiFi、不跑 MQTT、不跑 BLE，只透過 ESP-NOW 接受 master 控制。
因為沒有網路，韌體更新只能靠 USB 燒錄（Phase 4 會加上經 master 轉送的 OTA）。

## 按鈕

| 操作 | 行為 |
|---|---|
| 短按（< 1 秒） | 送出配對請求（需 master 先進入配對模式） |
| 長按 3 秒 → LED 閃爍 → 再按住 2 秒 | 清除配對記錄並重啟 |

兩顆按鈕（BOOT／RESET）任一顆都可以。

## Channel 同步

Slave 不連 WiFi，無從得知 master 在哪個 channel，靠三層機制解決：

1. 配對時把 master 的 channel 存進 EEPROM，開機直接切過去
2. Master 每 5 秒廣播心跳（配對模式時 1 秒），帶出目前 channel
3. 超過 30 秒沒收到心跳，輪掃 channel 1~13（每個停 600ms，一輪約 8 秒）

Master 換路由器導致 channel 改變時，恢復時間約 40 秒。

## 安全預設

失去 master 連線時會強制關閉繼電器，避免長時間通電。

## EEPROM 佈局（32 bytes）

| 位址 | 長度 | 內容 |
|---|---|---|
| 0 | 1 | 魔術數 0x5A，表示已配對 |
| 1 | 6 | Master MAC |
| 7 | 1 | 鎖定的 channel |
| 8 | 1 | Long Range 旗標（Phase 5 使用） |

## 編譯與燒錄

```powershell
.\flash.ps1 -Model slave                 # 只編譯
.\flash.ps1 -Model slave -Upload         # 編譯並燒錄（會抹掉配對記錄）
.\flash.ps1 -Model slave -Upload -KeepConfig  # 保留配對記錄
```
