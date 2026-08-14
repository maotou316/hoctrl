# hoMaster1 — ESP-NOW 主控端

## 硬體

- 開發板：ESP32 WROOM DevKit
- GPIO：BOOT 按鈕 0、第二按鈕 14、板載 LED 2、繼電器 13
- 繼電器為**選配**：接了就能用 `on`/`off` 控制，沒接則指令空跑

## 角色

Phase 1 階段是純 ESP-NOW 主控，用序列埠指令操作。
Phase 2 會接上 WiFi + MQTT，成為 App 與所有 slave 之間的唯一對外窗口。

## 序列埠指令

| 指令 | 說明 |
|---|---|
| `list` | 列出所有 slave 與在線狀態 |
| `pair` | 進入／離開配對模式（60 秒） |
| `on <n>` / `off <n>` | 開啟／關閉第 n 台 |
| `pulse <n>` | 點動第 n 台 2 秒 |
| `allon` / `alloff` / `allpulse` | 群組控制，含 master 自己 |
| `state <n>` | 要求第 n 台回報狀態 |
| `unpair <n>` | 解除第 n 台配對 |
| `ch <n>` | 測試用：切換 channel，驗證 slave 重掃 |
| `help` | 顯示說明 |

## 配對

短按 BOOT 進入配對模式 60 秒，LED 慢閃，心跳加快到 1 秒。
此時短按 slave 的按鈕即可加入。上限 20 台。

名冊存在 NVS（`Preferences`，命名空間 `homaster`），斷電不遺失。

## 編譯與燒錄

```powershell
.\flash.ps1 -Model master           # 只編譯
.\flash.ps1 -Model master -Upload   # 編譯並燒錄
```
