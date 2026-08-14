# hoMaster1 — ESP-NOW 主控端

## 硬體

同一份 `ho_master1.ino` 用 `CONFIG_IDF_TARGET_ESP32C3` / `CONFIG_IDF_TARGET_ESP32`
條件編譯支援兩種板子，**不是兩份 sketch**，燒錄時用 `flash.ps1` 的型號區分：

| 型號 | 開發板 | BOOT | 第二按鈕 | LED | 繼電器 |
|---|---|---|---|---|---|
| `master`（預設分區） | ESP32 WROOM DevKit | GPIO 0 | GPIO 14（未接線，僅供自檢） | 板載 GPIO 2 | GPIO 13 |
| `master-c3`（custom 分區） | ESP32-C3 Dev Module | GPIO 9 | GPIO 1（RESET） | 板載 GPIO 3 ＋ 面板 GPIO 0 | GPIO 4 與 7（同時驅動） |

C3 版 GPIO 對齊 `ho_slave1.ino`（同一塊硬體）。

- 繼電器為**選配**：只有 `allon` / `alloff` / `allpulse` 會驅動 master 自己的繼電器，
  沒接則這段空跑。`on <n>` / `off <n>` / `pulse <n>` 控制的是**第 n 台 slave**，與 master 自己的繼電器無關

## WROOM 版 vs C3 版怎麼選

1. **CPU**：C3 是單核 RISC-V，WROOM 是雙核。Phase 1（純 ESP-NOW 序列埠操作）沒差，
   但 Phase 2 的 master 要同時跑 WiFi + MQTT（5 台 broker）+ BLE + ESP-NOW，
   單核的 C3 壓力明顯較大，目前尚未實測，屆時若效能不足應優先選 WROOM。
2. **Flash 空間**：反而是 C3 較寬鬆——custom 分區的 app0 約 1.94MB（`partitions.csv`
   的 `0x1F0000`），目前燒錄用量約 5 成；WROOM 用預設分區（app 約 1.31MB），目前已用
   將近 7 成。長期看 C3 的 OTA 升級空間比較不吃緊。
3. **C3 版若要接繼電器，有硬體限制**：GPIO 4/7 是 ESP32-C3 的 JTAG 腳（MTMS/MTDO），
   reset 後由 ROM 配置、不保證低電位，開機瞬間繼電器會短暫通電，這是硬體限制、
   韌體無法根治，跟 `ho_relay2` 完全同源。需硬體在 MOS gate 對地加 10kΩ 下拉才能根治。
   WROOM 版的 GPIO 13 沒有這個問題。詳見 `ho_relay2/readme.md` 的「已知硬體限制」章節。

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
（`checkStuckButtons()`）。WROOM 版的第二按鈕（GPIO 14）目前沒接線，自檢與
`anyResetButtonPressed()` 對它只是空跑；C3 版的第二按鈕是真正接線的 RESET（GPIO 1），
兩者共用同一套自檢／判斷函式，行為由 GPIO 陣列決定，不需要另外分支。
詳見 `.claude/rules/button-pin-stuck-low.md`。

## 編譯與燒錄

```powershell
.\flash.ps1 -Model master              # WROOM 版，只編譯
.\flash.ps1 -Model master -Upload      # WROOM 版，編譯並燒錄
.\flash.ps1 -Model master-c3           # C3 版，只編譯
.\flash.ps1 -Model master-c3 -Upload   # C3 版，編譯並燒錄
```
