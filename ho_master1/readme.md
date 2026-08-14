# hoMaster1 — ESP-NOW 主控端

## 硬體

- 開發板：ESP32 WROOM DevKit
- GPIO：BOOT 按鈕 0、第二按鈕 14、板載 LED 2、繼電器 13
- 繼電器為**選配**：只有 `allon` / `alloff` / `allpulse` 會驅動 master 自己的繼電器，
  沒接則這段空跑。`on <n>` / `off <n>` / `pulse <n>` 控制的是**第 n 台 slave**，與 master 自己的繼電器無關

## 角色

Phase 1 階段是純 ESP-NOW 主控，用序列埠指令操作。
Phase 2 會接上 WiFi + MQTT，成為 App 與所有 slave 之間的唯一對外窗口。

## 序列埠指令

| 指令 | 說明 |
|---|---|
| `list` | 列出所有 slave 與在線狀態 |
| `pair` | 進入／離開配對模式（60 秒） |
| `on <n>` / `off <n>` | 開啟／關閉第 n 台 **slave** 的繼電器 |
| `pulse <n>` | 點動第 n 台 slave 2 秒 |
| `allon` / `alloff` / `allpulse` | 群組控制，含 master 自己的繼電器 |
| `state <n>` | 要求第 n 台回報狀態 |
| `unpair <n>` | 解除第 n 台配對 |
| `ch <n>` | 測試用：切換 channel，驗證 slave 重掃 |
| `help` | 顯示說明 |

## 配對

短按 BOOT 進入配對模式 60 秒，LED 慢閃。
此時短按 slave 的按鈕即可加入。上限 20 台。

心跳固定每 1 秒廣播一次（不分是否在配對模式）。這個值與 slave 每個 channel 停留
1200ms 是一組的：dwell 大於心跳間隔，slave 輪掃時一輪內必定命中正確 channel。

**序列埠上的心跳只有每 10 次印一行**（`HEARTBEAT_LOG_EVERY`），約 10 秒一行 ——
發送頻率不受影響，純粹是避免每秒一行把序列埠洗版、蓋掉其他訊息。
channel／配對模式／slave 台數任一項變化時會立即印一行，狀態變化不會被吃掉。
`ch <n>` 切換 channel 時另外印一行 `[心跳] channel 已變更，連發 4 次（間隔 200 ms）`。

名冊存在 NVS（`Preferences`，命名空間 `homaster`），斷電不遺失。
開機時 `setup()` 會在 `setupEspNow()` 之後呼叫 `registerAllPeers()`，
把名冊上每一台重新註冊成 ESP-NOW peer —— ESP-NOW 的 peer 表只存在 RAM，
少了這步，master 重開機後對所有 slave 的指令都會失敗且不會自我修復。

## 按鈕自檢

開機時取樣 BOOT／第二按鈕 500ms，整段都是 LOW 的腳判定為短路／未接，本次開機停用其功能
（`checkStuckButtons()`）。詳見 `.claude/rules/button-pin-stuck-low.md`。

## 編譯與燒錄

```powershell
.\flash.ps1 -Model master           # 只編譯
.\flash.ps1 -Model master -Upload   # 編譯並燒錄
```
