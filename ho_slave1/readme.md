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

清除配對時會先送一個 `HO_PKT_UNPAIR` 通知 master，master 收到後會從名冊移除並刪除 peer，
避免名冊留下永遠離線又無法自動移除的殘留項目。

### 按鈕自檢

開機時取樣兩支按鈕腳 500ms，整段都是 LOW 的腳判定為短路／未接，本次開機停用其按鈕功能
（`checkStuckButtons()`）。這是為了擋掉「開機即清除配對 → 重啟 → 再清除」的無限迴圈 ——
2026-08 在同硬體的 hoRelay2 上實際發生過。副作用是「按住按鈕再上電」會被擋掉，
放開後重新上電即恢復。詳見 `.claude/rules/button-pin-stuck-low.md`。

## Channel 同步

Slave 不連 WiFi，無從得知 master 在哪個 channel，靠三層機制解決：

1. 配對時把 master 的 channel 存進 EEPROM，開機直接切過去
2. Master 每 1 秒廣播心跳（`HEARTBEAT_INTERVAL`），帶出目前 channel；
   channel 改變的當下另外連發 4 次（間隔 200ms）
3. 超過 30 秒沒收到心跳（`HEARTBEAT_TIMEOUT`），輪掃 channel 1~13
   （每個停 1200ms＝`SCAN_DWELL_MS`，一輪 15.6 秒）

**dwell（1200ms）必須大於心跳間隔（1000ms）**，這樣只要 master 就在某個 channel，
掃到它時的停留期間必定涵蓋至少一次心跳，一輪之內保證鎖定，不是碰運氣。
（舊值 dwell 600ms 搭配心跳 5000ms 剛好相反，命中率只有約 7.7%，
期望要十幾次心跳、約一分鐘才鎖得回來，且沒有上界。）

Master 換路由器導致 channel 改變時的恢復時間：
30 秒失聯門檻 + 最多一輪掃描 15.6 秒 = **最壞約 46 秒，典型約 38 秒**。

### 序列埠輸出頻率

收到心跳的 `[心跳] 來自 …` 只有每 10 次印一行（`HEARTBEAT_LOG_EVERY`），約 10 秒一行 ——
收訊與鎖定邏輯完全不受影響，純粹避免每秒一行把序列埠洗版。
第一次收到、以及 master MAC／channel／配對模式有變化時都會立即印。

`[鎖定]` 在三種情況會印：換 master、換 channel、**從輪掃中恢復**。
第三種是為了讓「master 重開機但 channel 沒變」也看得出恢復發生過
（此時前兩個條件都不成立，若又碰上心跳降頻，序列埠會完全沒有輸出）。

## 安全預設

失去 master 連線時會強制關閉繼電器，避免長時間通電。

精確一點（`startChannelScan()`）：**只有繼電器當下是 ON 才會被關掉**，
並印 `[安全] 失去 master，繼電器已關閉`；本來就是 OFF 的話什麼都不做、也不印。
觸發點是 `loop()` 裡的 `now - lastHeartbeatTime > HEARTBEAT_TIMEOUT`（30 秒），
它同時也是進入 channel 輪掃的入口。

> **在本系統的語義下，這個「安全預設」等於籠門被打開。**
> 因此協定升級（flag-day）時它是最主要的風險 —— 見
> `docs/phase4-flag-day-upgrade.md` 第 2 節。

## 指令歸因（協定版本 2）

slave 在**實際走完**繼電器動作之後（`switch` 的 `default` 分支不算），
把該指令的 `cmdId` 記進 `lastCmdId`／`lastCmdKind`，並累加 `lastCmdCount`，
之後每一則 `HO_PKT_STATE` 都帶著這三個值回去。master 據此判斷
「這個狀態是哪一道指令造成的」。

**這證明什麼**：slave 的韌體確實走完了 `setRelayPins()`／`pulseRelay()` 那段程式。

**擋不住什麼**：不證明繼電器硬體動作、不證明籠門關上、不擋重放，
**也完全不擋偽造** —— 射頻範圍內的第三方湊得出「MAC ＋ cmdId ＋ 種類」就能
組出一封合法的 `HO_PKT_STATE`，讓一台沒動作的 slave 顯示成已執行。
完整的**四項**清單寫在 `libraries/HoEspNow/src/HoEspNowProtocol.h` 的
`HoStatePayload` 上方，緩解評估見 `docs/phase4-flag-day-upgrade.md` 第 3.2.1 節。

`lastCmdCount` 通常會大於 1（群組指令會廣播 3 次再加至少一次單播），
**這是設計行為不是異常** —— master 只比對 `cmdId`，不看次數。

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
