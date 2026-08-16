# Phase 1 完整回歸測試清單（hoMaster1 / hoSlave1）

> **狀態：尚未在實體硬體上執行過任何一項。**
> 本清單由 Task 7 整理自 `.superpowers/sdd/2026-08-14-esp32-master-slave-phase1/task-7-brief.md`
> 的 Step 6，因執行環境沒有實體板子，燒錄與人工操作全數跳過，只把驗收依據完整寫下來。
> 請維護者依序實際跑過一遍，逐項打勾，全部通過才算 Phase 1 完成。

## 前置：燒錄

需要兩塊板子（master 一塊、slave 一塊）同時插著，分別找出各自的 COM 埠
（裝置管理員或 `flash.ps1` 執行時會列出可用埠）。以下埠號僅為範例，請替換成實際埠號。

```powershell
# 1. 協定測試（任一塊板子皆可，測完再燒回正式韌體）
.\flash.ps1 -Model test -Upload -Port COM13
.\tools\serial_expect.ps1 -Port COM13 -Expect "ALL TESTS PASSED" -Seconds 10 -Reset

# 2. 燒錄 slave 韌體
.\flash.ps1 -Model slave -Upload -Port COM13

# 3. 燒錄 master 韌體
.\flash.ps1 -Model master -Upload -Port COM16
```

兩支序列埠都用 115200 baud 開起來（VSCode 內建終端機或 Arduino IDE 的 Serial Monitor 皆可），
下面每一項都要對照序列埠輸出。

---

## 人工驗證清單（14 項）

### 1. Master 每 1 秒發一次心跳，序列埠每 10 次印一行

- 操作：master 開機後不做任何事，觀察 master 序列埠。
- 預期：開機後**立即**印一行，之後每隔約 **10 秒**印一行
  ```
  [心跳] channel=<目前 channel> 配對模式=否 slave=<目前台數>
  ```
- 說明：**發送**頻率是每 1 秒（`HEARTBEAT_INTERVAL` = 1000ms，配對模式與否都一樣），
  這是 slave 輪掃一輪必定命中的前提，不可更動；
  **印出**頻率則刻意降到每 10 次一行（`HEARTBEAT_LOG_EVERY`），
  否則每秒一行會把序列埠洗版，下面十幾項都沒辦法比對。
  channel／配對模式／slave 台數任一項變化時會立即印一行，不必等滿 10 次。
- 若要確認「確實每秒都有發」而非每 10 秒才發一次：執行第 11 項的 `ch <n>`，
  slave 端最壞 46 秒內會重新鎖定；若心跳真的只有 10 秒一次，該項必然失敗。
- [ ] 通過

### 2. Slave 首次開機進入輪掃，找到 master 後鎖定

- 操作：slave 用 `-Upload`（不帶 `-KeepConfig`）燒錄後開機，此時 EEPROM 是乾淨的。
- 預期：slave 序列埠依序印
  ```
  按鈕自檢: 正常
  EEPROM 無配對記錄
  ...
  [掃描] 開始輪掃 channel 1~13 尋找 master
  ```
  找到 master 心跳後印：
  ```
  [心跳] 來自 hoban-xxxxxxxxxxxx channel=<N> 配對模式=否 rssi=<負數>
  [鎖定] master=hoban-xxxxxxxxxxxx channel=<N>
  ```
- 說明：slave 端的心跳 log 同樣降頻到每 10 次一行（`HEARTBEAT_LOG_EVERY`），
  但**第一次**收到、以及 master MAC／channel／配對模式有變化時都會立即印，
  所以上面兩行一定看得到；之後穩定運行時約每 10 秒才會再出現一行 `[心跳] 來自 …`。
- [ ] 通過

### 3. 短按 master → 短按 slave → 配對成功，slave LED 快閃 3 下

- 操作：master 序列埠輸入 `pair`，master 印 `[配對] 進入配對模式，60 秒內請短按 slave 的按鈕`；
  60 秒內短按 slave 的 BOOT 或 RESET 按鈕（< 1 秒）。
- 預期：
  - slave 印 `[配對] 已送出配對請求，等待回覆`，隨後印
    `[配對] 成功，master=hoban-xxxxxxxxxxxx channel=<N>`，
    面板 LED（GPIO 0）與板載 LED（GPIO 3）快閃 3 下（100ms 間隔）
  - master 印 `[配對] 接受 hoban-xxxxxxxxxxxx，目前共 <N> 台`
- [ ] 通過

### 4. 兩邊斷電再上電，配對記錄都還在，slave 不需重新掃描

- 操作：master、slave 都拔電重插（master 名冊存 NVS、slave 存 EEPROM，兩者都不會因斷電遺失）。
- 預期：slave 開機印 `已配對 master: hoban-xxxxxxxxxxxx，上次 channel=<N>`，
  接著直接鎖定該 channel（不會印「開始輪掃」），很快印出
  `[心跳] 來自 hoban-xxxxxxxxxxxx channel=<N> …`（開機後第一次收到必定印，不受降頻影響）。
  注意此時**不會**印 `[鎖定]` —— master 與 channel 都沒變、也沒經過輪掃，屬正常；
  `[鎖定]` 只在「換 master／換 channel／從輪掃中恢復」三種情況才印。
  master 開機後應印 `[名冊] 已重新註冊 <N>／<N> 台為 ESP-NOW peer`，
  `list` 應直接看到剛才配對的 slave，且**重開機後 `on 0` / `off 0` / `pulse 0` 仍能正常控制**
  （ESP-NOW peer 表只存在 RAM，若沒有重新註冊，指令會全部失敗且永不自我修復）。
- [ ] 通過

### 5. `list` 顯示 slave 在線

- 操作：master 序列埠輸入 `list`。
- 預期：
  ```
  ── Slave 名冊（1／20）──
    0. hoban-xxxxxxxxxxxx  在線  rssi=<負數>
  ```
- 注意：編號從 **0** 開始（`printSlaveList()` 的 `i` 由 0 起算），
  所以第一台要用 `on 0` / `off 0` / `pulse 0` 控制，不是 `on 1`。
- [ ] 通過

### 6. `pulse 0` 讓 slave 繼電器動作 2 秒後自動關閉

- 操作：master 輸入 `pulse 0`。
- 預期：master 印 `[控制] 送指令 <N> 給 hoban-xxxxxxxxxxxx`；
  slave 印 `[繼電器] 點動 2000 ms`，繼電器立即動作，約 2 秒後 slave 印
  `[繼電器] 點動結束，已關閉` 並自動回報狀態。
- [ ] 通過

### 7. `on 0` / `off 0` 正確開關

- 操作：依序輸入 `on 0`、`off 0`。
- 預期：`on 0` 後 slave 印 `[繼電器] 開啟`，繼電器保持通電直到手動關閉；
  `off 0` 後 slave 印 `[繼電器] 關閉`。
- [ ] 通過

### 8. `allpulse` 讓所有 slave 與 master 自己同時動作

- 操作：至少配對 2 台 slave 後，master 輸入 `allpulse`。
- 預期：master 印 `[控制] 廣播指令 <N> 給 <台數> 台`，master 自己的繼電器（若有接）
  與所有 slave 的繼電器都動作約 2 秒後自動關閉；每台 slave 各自印
  `[繼電器] 點動 2000 ms` 與 `[繼電器] 點動結束，已關閉`。
- **Phase 2b Task 5 起的行為變更（不是失敗）**：`sendCmdToAll()` 已從「逐台單播、
  每台間隔 20ms」改成 **ESP-NOW 廣播連送 3 次**，所以所有 slave 是同一瞬間動作，
  不再有「一台接一台」的可見落差（20 台原本最後一台會晚 400ms）。
  序列埠會**多出**下列幾行，全部屬正常輸出：
  - `[群組] 已廣播 3 次（廣播無 ack，送出成功不代表任何一台收到），開始逐台查證 <台數> 台`
  - 全部確認：`[群組] 指令 <N> 已逐台確認：<台數> 台全部回報預期狀態`
  - 有台沒回報：`[群組] 第 <n> 趟：對 <台數> 台未確認的補送單播`／`…逐台查狀態`，
    最後 `⚠ [群組] 指令 <N> 未能全部確認：…` 並逐台列出未確認的 ID
- **判準補充**：`⚠ [群組] … 未能全部確認` 出現在「該台 slave 確實沒回應」時是
  **正確行為**（韌體誠實回報而不是假裝成功），只有在「slave 明明有動作卻被判未確認」
  時才算 FAIL。
- [ ] 通過

### 9. Slave 斷電 40 秒後 master 標記離線，復電後自動恢復在線

- 操作：拔掉 slave 電源，等 40 秒（master 判離線門檻 `SLAVE_OFFLINE_TIMEOUT` 是 30 秒，
  40 秒留餘裕確保跨過門檻），觀察 master 序列埠；再重新供電。
- 預期：master 印 `[離線] hoban-xxxxxxxxxxxx 超過 30 秒沒回應`；
  slave 復電並重新鎖定 master 後，master 下一次輪詢（`pollNextSlave()` 發出 `HO_PKT_STATE_REQ`）
  收到該 slave 的狀態回報即自動恢復在線，`list` 顯示回 `在線`。
  （注意：slave 端**不會主動送心跳**，只在收到 `HO_PKT_STATE_REQ` 或執行完指令時才回報狀態，
  所以恢復在線是靠 master 主動輪詢，不是靠 slave 自己上報。）
- [ ] 通過

### 10. Master 斷電 40 秒後 slave 開始輪掃，復電後自動找回

- 操作：拔掉 master 電源，等 40 秒（slave 判失聯門檻 `HEARTBEAT_TIMEOUT` 是 30 秒），
  觀察 slave 序列埠；再重新供電。
- 預期：slave 印 `[失聯] 超過 30 秒沒收到心跳`，接著印 `[安全] 失去 master，繼電器已關閉`
  （若當時繼電器是通電狀態）與 `[掃描] 開始輪掃 channel 1~13 尋找 master`；
  master 復電後 slave 應在**一輪掃描內（最多約 15.6 秒＝13 × `SCAN_DWELL_MS` 1200ms）**
  收到心跳並印 `[鎖定] master=hoban-xxxxxxxxxxxx channel=<N>`。
  因 dwell（1200ms）大於心跳間隔（1000ms），掃到正確 channel 時必定涵蓋至少一次心跳，
  一輪內保證命中。
- 說明：master 只是重開機、channel 沒變時，`isNewMaster` 與 `channelChanged` 都是 false，
  而心跳 log 已降頻，本項若只看 `[心跳]` 會有最多 10 秒看不到任何輸出。
  因此 `onMasterFound()` 已改成「從輪掃中恢復」也印 `[鎖定]`，本項一律以這行為判定依據。
- [ ] 通過

### 11. `ch 6` 切換 channel 後，slave 最壞約 46 秒內重新鎖定到 channel 6

- 操作：master 輸入 `ch 6`。
- 預期：master 依序印
  ```
  [channel] master 切換到 6
  [心跳] channel 已變更，連發 4 次（間隔 200 ms）
  [心跳] channel=6 配對模式=否 slave=<台數>
  ```
  （連發 4 次心跳，但 log 已降頻，只有 channel 剛變的第一次會印出 `[心跳] channel=6`；
  `sendHeartbeatBurst()` 另外印的那行讓「連發確實發生」仍可驗證）；
  slave 因超過 30 秒沒收到心跳（頻道已變）
  進入輪掃，印 `[失聯]` 與 `[掃描]`，掃到 channel 6 時收到心跳並印
  `[鎖定] master=hoban-xxxxxxxxxxxx channel=6`。
  整體時間 = 30 秒失聯門檻 + 最多一輪掃描 15.6 秒，**最壞約 46 秒、典型約 38 秒**
  （對應 `ho_slave1/readme.md` 的「Channel 同步」章節）。
- [ ] 通過

### 12. Slave 長按 5 秒清除配對，重啟後回到未配對狀態

- 操作：按住 slave 的 BOOT 或 RESET 按鈕不放，持續 5 秒以上（3 秒觸發閃爍 + 再 2 秒確認）。
- 預期：
  - 第 3 秒：LED 開始以 250ms 週期閃爍，序列埠印 `長按 3 秒達成，繼續按住 2 秒清除配對…`
  - 再過 2 秒（總計約第 5 秒）：LED 長亮 0.7 秒，序列埠依序印
    `確認清除配對`、`[配對] 已通知 master 解除配對`、`配對記錄已清除，重新啟動中…`
  - 重啟後印 `EEPROM 無配對記錄` 並開始輪掃
  - **master 端**應印 `[配對] hoban-xxxxxxxxxxxx 已解除配對，剩 <N-1> 台`，
    `list` 不再顯示該台（slave 主動送出 `HO_PKT_UNPAIR`，避免名冊留下無法自動移除的殘留）
- [ ] 通過

### 13. Slave 長按中途放開會取消，配對記錄保留

- 操作：按住按鈕約 4 秒（已進入閃爍階段但未滿 5 秒）後放開。
- 預期：序列埠印 `按鈕放開，取消重置`；配對記錄不受影響，
  且**不會**出現 `[配對] 已通知 master 解除配對`（沒走到 `clearPairing()`）；
  等下一行 `[心跳] 來自 …`（約 10 秒內）仍是原本配對的 master。
- [ ] 通過

### 14. `unpair 0` 後 slave 重啟並回到未配對狀態，master 名冊剩 0 台

- 操作：master 輸入 `unpair 0`。
- 預期：master 印 `[配對] hoban-xxxxxxxxxxxx 已解除配對，剩 0 台`；
  slave 收到後印 `[配對] master 要求解除配對`，隨後自動重啟，
  重啟後印 `EEPROM 無配對記錄` 並開始輪掃；master 再次 `list` 應顯示 `（空）`。
- [ ] 通過

---

## 驗收判定

以上 14 項全數打勾，且步驟 1~3（協定測試 / 燒錄 slave / 燒錄 master）都無編譯或燒錄錯誤，
才視為 Phase 1 完整回歸測試通過。若有任一項失敗，記錄失敗現象與序列埠實際輸出，
回頭對照 `ho_master1/ho_master1.ino`、`ho_slave1/ho_slave1.ino` 與
`docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md` 排查。
