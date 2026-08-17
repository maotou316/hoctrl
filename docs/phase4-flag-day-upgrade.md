# ESP-NOW 協定版本 2：flag-day 升級程序與回歸清單

> 適用韌體：`ho_master1`（WROOM 與 C3 兩種）、`ho_slave1`。
> 對應改動：Phase 4 Task 1 —— CRC 涵蓋標頭、`HO_ESPNOW_VERSION` 1 → 2、
> 指令歸因欄位、OTA 四種封包型別。

---

## 1. 為什麼這是 flag-day

協定版本 1 與版本 2 **雙向不相容**，成因有兩個，任何一個單獨成立就足以讓兩端不通：

| # | 改動 | 舊韌體收到新封包會怎樣 |
|---|---|---|
| 1 | CRC 由「只算 payload」改為「算標頭前 6 bytes ＋ payload」 | CRC 對不上 → `hoUnpackPacket()` 回 false → 丟棄 |
| 2 | `HO_ESPNOW_VERSION` 1 → 2 | 版本檢查不過 → 丟棄 |

`hoUnpackPacket()` 的版本檢查排在 CRC 檢查**之前**
（`libraries/HoEspNow/src/HoEspNowProtocol.cpp`），所以實際先觸發的是第 2 條。

### 不相容不會靜默

兩端的 `onEspNowRecv()` 在 `hoUnpackPacket()` 失敗後會再呼叫
`hoPeekVersionMismatch()`，magic 對、版本不對就印一行（**每 10 秒最多一行**）：

```
⚠ [協定] 收到版本 1 的封包，本機是版本 2，全部丟棄；master 與所有 slave 必須一起重燒
```

**這道告警擋不住什麼**：它只是**回報**，不做任何補救 —— 封包照樣丟棄、心跳照樣中斷、
第 2 節那個 30 秒保護照樣觸發。它也**偵測不到**「版本號相同但欄位語義改了」的那種
不相容（那只能靠 `HoEspNowProtocol.h` 裡的 `static_assert` 與人工紀律）。
另外，一封 magic 剛好撞上、內容全是雜訊的封包也會被報成版本不符
（它刻意不驗 CRC，理由見函式上方註釋）。

---

## 2. 燒錄順序：**兩種順序都有窗口，沒有安全的那一種**

### 30 秒失聯時實際發生什麼（逐行對照過程式碼）

`ho_slave1.ino` 的 `loop()`：

```c
if (now - lastHeartbeatTime > HEARTBEAT_TIMEOUT) {   // HEARTBEAT_TIMEOUT = 30000
  Serial.println("[失聯] 超過 30 秒沒收到心跳");
  startChannelScan();
}
```

`startChannelScan()` 開頭：

```c
if (relayState) {
  setRelayPins(false);
  pulseActive = false;
  Serial.println("[安全] 失去 master，繼電器已關閉");
}
```

也就是說：**繼電器只有在當下是 ON 的時候才會被強制關掉**，並且會印
`[安全] 失去 master，繼電器已關閉`。在本系統的語義下，那等於**籠門被打開**。

### 兩種順序的比較

| 順序 | 窗口 | 傷害面 |
|---|---|---|
| **先 slave 後 master** | 每燒完一台 slave，那台就開始跑自己的 30 秒 | 被燒的那台本來就剛重置（`initRelayPins()` 讓繼電器是 OFF），**但它在 master 升級完成前完全是聾的** —— 這段期間對它下的任何關門指令都不會被執行，而且 master 端只會看到它離線 |
| **先 master 後 slave** | master 一升級，**所有尚未升級的 slave 同時**失去心跳 | 30 秒後**全部**一起 `setRelayPins(false)` —— 若當下有任何一台靠繼電器維持狀態，會**同時**改變。這是「一次要全部關」的系統最不能接受的同步失敗 |

**結論：不要用「哪個順序比較安全」來做決定，因為沒有安全的那一種。**
唯一安全的做法是把窗口的前提消掉。

### 規定的程序

1. **升級前先讓所有籠門處於不依賴繼電器保持的安全狀態**，並確認**籠內沒有動物**。
   這一步不是建議，是前提 —— 沒有做到就不要開始。
2. 燒 **master**（`.\flash.ps1 -Model master` 或 `-Model master-c3`，加 `-Upload`）。
3. 立刻逐台燒 **slave**（`.\flash.ps1 -Model slave -Upload`）。
4. 每台燒完後確認序列埠不再出現 `⚠ [協定] 收到版本 …`，且 master 的 `list`
   看得到它在線。
5. 全部完成後，跑第 4 節的回歸項。

> **為什麼可以接受 master 先燒**：`flash.ps1` 預設 `EraseFlash=all`，slave 的
> EEPROM 配對記錄會被抹掉，本來就必須重新配對；既然全體都要重新配對，
> 就沒有「維持既有連線」這個目標可保。此時要控制的只有「籠門在窗口內的物理狀態」，
> 而那由第 1 步負責。

> **目前沒有現場部署**（Phase 2a 已裁定 master 在補上現場恢復手段前不得佈到現場），
> 所以現在改的代價最低。日後若真的需要不停機升級，必須改成
> 「master 同時用新舊兩種 CRC 各發一次心跳」的過渡期做法 —— 那是另一個階段的工作。

---

## 3. 協定版本 2 的新增內容摘要

### 3.1 CRC 涵蓋標頭

`hoPayloadCrc()` **已移除**（不留相容殼），改為：

```c
uint8_t hoFrameCrc(const uint8_t* headerFirst6, const uint8_t* payload, size_t payloadLen);
```

涵蓋標頭前 6 bytes（magic/version/type/seq）＋ payload ＋ 共享密鑰。
不含 `crc` 欄位自己。

**擋住什麼**：`type` 欄位的位元翻轉。舊版一封 `OTA_DATA`（0x21）的 type 翻成
`HO_PKT_CMD`（0x10）之後 CRC 仍然正確，slave 會把韌體資料的前 5 bytes 當成
`HoCmdPayload` 解讀 —— `cmd` 欄位剛好落在 0/1/2 就會實際動作繼電器。

**擋不住什麼**：
- CRC-8 只有 256 種值，**隨機竄改平均每 256 次就有一次撞上正確值**。
- 擋不住**重放**：原封不動重送一封合法封包，CRC 完全正確。
- 擋不住知道共享密鑰的人**偽造**。共享密鑰只讓不知道它的人算不出值，不是簽章。

### 3.2 指令歸因（還 Phase 2b Task 5 登記的技術債）

| 位置 | 欄位 | 語義 |
|---|---|---|
| `HoCmdPayload` | `uint16_t cmdId` | **一道邏輯指令**的識別碼。同一道指令的 3 次廣播、每台單播、2 趟補送共用同一個值 |
| `HoStatePayload` | `uint16_t lastCmdId` | slave 最後一次**實際執行**的 `cmdId`；`HO_CMD_ID_NONE`（0）＝ 開機以來沒執行過 |
| `HoStatePayload` | `uint8_t lastCmdKind` | 那道指令的 `HoRelayCmd`（**OFF=0 / ON=1 / PULSE=2**，回去讀 enum，不要憑名稱推） |
| `HoStatePayload` | `uint8_t lastCmdCount` | 同一個 `cmdId` 被執行的次數，飽和於 255 |

**為什麼是 payload 裡的 `cmdId` 而不是標頭的 `seq`**：`seq` 是每幀遞增的傳輸序號，
一道群組指令會產生幾十幀、每幀 `seq` 都不同，拿它歸因必定對不上。

**為什麼要 `lastCmdCount`**：重送與補送會讓同一道指令被執行多次（Task 5 的補送就是
這樣設計的）。與其假裝只執行一次，不如把次數如實帶回來 —— master 只比對 `cmdId`，
不看次數，`lastCmdCount > 1` **不是異常**。

master 端據此算出「執行證明」（`groupExecutedIdx()`）：

```
slaves[s].lastCmdId   == groupJob.cmdId
&& slaves[s].lastCmdKind == groupJob.cmd
&& slaves[s].lastCmdAt   >= groupJob.startedAt
```

**證明什麼**：那台 slave 的韌體確實走完了 `HO_PKT_CMD` 分支的
`setRelayPins()`／`pulseRelay()` 呼叫，而且那一次帶的就是這道指令的 `cmdId`。
證據由 slave 產生，master 造不出來 —— 這正是 Task 5 的 C1 缺的那一半。

**擋不住什麼（三項，不准拆開引用）**：
1. **不證明繼電器硬體動作，更不證明籠門關上了。** `setRelayPins()` 只寫 GPIO；
   MOS 燒毀、線路脫落、觸點黏死一律照樣回報「已執行」。`HO_CMD_PULSE` 只證明
   點動計時器被啟動。**現場確認仍然是唯一能證明門關上的方法。**
2. **不擋重放。** 時間條件擋得住**跨指令**的舊回報，擋不住「指令送出後才被重播」
   的那則。`cmdId` 只有 16 bits，開機初值取 `esp_random()`，65535 道之後自然繞回。
3. **沒有執行證明 ≠ 沒執行。** 回報可能還在路上、可能掉了。所以它只能**維持紅色**，
   不能宣稱「已確認未執行」。

**執行證明刻意不進入控制決策**：它只進入回報（序列埠收工訊息、MQTT 的
`exed`／`exe`）。廣播 ＋ 全台單播 ＋ 補送的流程一行都沒改，理由見 `sendCmdToAll()`
上方的註釋。

### 3.3 MQTT 欄位變更

| 欄位 | 位置 | 變更 |
|---|---|---|
| `group.cid` | master status | **新增**。這道指令的 `cmdId`。每次開機從亂數起算，只在同一次開機內可比較 |
| `group.exed` | master status | **新增**。有執行證明的台數。**可能在收工之後才變大** |
| `group.exec` | master status | `"unprovable"` → **`"attributed"`** |
| `slaves[].exe` | master status | **新增**。`1` = 有執行證明；`0` = 沒有證據（**不是**「已確認沒執行」）；欄位不存在 = 不在最近一次群組指令的快照裡 |

**舊 App 的相容性是安全的**：`hoctrl` 的 `GroupExecEvidence.fromWire()` 把任何非
`"unprovable"` 的值歸成 `unrecognized`，而那個列舉**刻意沒有「已證明」那一態**，
所以舊 App 收到 `"attributed"` 只會繼續維持「無法證明已執行」—— 誤紅方向，
**不會憑空長出綠燈路徑**。`SlaveStatus.fromJson()` 也只挑它認得的 key，
多出來的 `exe` 會被忽略。

### 3.4 JSON 容量重算（三層防線都跟著更新）

| 常數 | 舊值 | 新值 | 依據 |
|---|---|---|---|
| `SLAVE_ENTRY_MAX_BYTES` | 104 | **112** | 單筆最壞實算 97 → **105**（`,"exe":0` ＝ 8 bytes） |
| `STATUS_BASE_MAX_BYTES` | 640 | 640（不變） | `group` 子預算 96 → 128，總計 ≈ 608 ≤ 640 |
| `STATUS_BUF_SIZE` | 3072 | 3072（不變） | — |
| `MQTT_BUFFER_SIZE` | 3328 | 3328（不變） | — |

`static_assert` 的實際值：`(3072-1-640-11)/112 = 21 ≥ 20`。
**餘裕只剩 1 台**（Task 7 時是 3 台）—— 下一個想在 slave 條目加欄位的人必須先放大
`STATUS_BUF_SIZE`（連帶 `MQTT_BUFFER_SIZE`），不能再靠壓縮餘裕。

順帶更正一個既有的算錯：Task 5 review M2 把 `group` 物件寫成「94 bytes，取 96」，
但**漏算了 `busy` 欄位**，實際當時已經是 102，早就超過自己寫的 96。整包沒爆是因為
640 的總預算本身有餘裕 —— 那是運氣不是設計。本次一併算對（124，取 128）。

### 3.5 OTA 封包（本 Task 只定義，Task 2~5 才實作）

| 型別 | 值 | payload | 大小 |
|---|---|---|---|
| `HO_PKT_OTA_BEGIN` | 0x20 | `HoOtaBeginPayload` | 26 |
| `HO_PKT_OTA_DATA` | 0x21 | `HoOtaDataPayload` ＋ 最多 240 bytes 資料 | 3 (+240) |
| `HO_PKT_OTA_END` | 0x22 | `HoOtaEndPayload` | 6 |
| `HO_PKT_OTA_ACK` | 0x23 | `HoOtaAckPayload` | 6 |

常數：`HO_OTA_CHUNK_SIZE` = 240、`HO_OTA_WINDOW` = 16、`HO_OTA_MAX_CHUNKS` = 8466、
`HO_OTA_SESSION_NONE` = 0（`sessionId` 合法範圍 1~255）。
`HoOtaStatus` 九個值見 `HoEspNowProtocol.h`（**回去讀定義，不要照抄別處的範例值**）。

四個 struct 的**欄位名稱與順序逐字取自設計文件的「決定 2.2」**
（`docs/superpowers/plans/2026-08-17-esp32-phase4-ota-relay.md`）——
Task 2~5 會照那份文件實作，改名或調換順序就會兩邊對不起來。
本 Task **只定義型別，沒有任何收送邏輯**：現行 master 與 slave 收到
`HO_PKT_OTA_*` 一律走到 `onEspNowRecv()` 的結尾而被忽略。

---

## 4. 回歸清單（本次改動專屬）

> 既有的 `phase1`／`phase2b` 清單仍然全部適用，只有引用到收工訊息與 `exec` 欄位的
> 幾項已就地更新。這一節只列**本次改動新產生**的驗收項。

### 4-1. 🖥 協定測試 sketch 全綠

**步驟**：`.\flash.ps1 -Model test -Upload`，看序列埠。

**預期**：`執行 56 項，失敗 0 項` 之後 `ALL TESTS PASSED`。

**【失敗判定】**
- 出現 `TESTS FAILED`。
- 項數**不是 56** —— 代表有人加減了測試卻沒更新這一行判準。

**【擋不住什麼】** 這 56 項全部是**單機的結構與序列化檢查**。
它**驗不到任何跨板行為**：slave 有沒有在執行後正確填回 `lastCmdId`、
master 有沒有正確比對、版本告警在真實 flag-day 現場印不印得出來 ——
一項都測不到，那些要靠 4-2 ~ 4-4。

### 4-2. 🖥 版本不符時印得出告警（**方向要選對，選錯就永遠等不到**）

> **先讀這一段，否則會白測。** 版本告警只在「**收到**對方的封包」時才觸發，
> 而 v1 與 v2 之間**只有一個方向有穩定的流量**：
>
> - **master → slave 的心跳是每 1 秒無條件廣播的** ⇒ 新 slave 配舊 master，
>   新 slave 每 10 秒必定印一次告警。**這是唯一好驗的方向。**
> - **反方向沒有穩定流量**：slave 只在「收到 CMD／STATE_REQ 之後」或「按鍵配對」
>   才發話。舊 slave 解不開新 master 的封包，所以不會回 STATE；而 `requestPairing()`
>   需要 `masterInPairingMode`，那個旗標是從**心跳**解出來的 —— 舊 slave 同樣解不開。
>   ⇒ **新 master 配舊 slave 時，master 端很可能一行都不會印。**
>   **那不是 FAIL**，是這個方向本來就沒有封包可收。

**步驟**：把 **master** 燒成舊版（`git checkout 16da5fc -- ho_master1 libraries` 後燒，
測完記得還原），slave 維持新版，兩者放在一起 40 秒以上。

**預期（新版 slave 序列埠）**：
```
⚠ [協定] 收到版本 1 的封包，本機是版本 2，全部丟棄；master 與所有 slave 必須一起重燒
```
接著（若它原本已配對）會有 `[失聯] 超過 30 秒沒收到心跳`，
若當下繼電器是 ON 還會有 `[安全] 失去 master，繼電器已關閉`。

**【失敗判定】**
- 新版 slave 完全沒印那一行 —— 代表 `hoPeekVersionMismatch()` 沒被接上，
  現場的 flag-day 事故將完全沒有線索。
- 那一行每秒都在印（沒有節流）—— 會把序列埠洗掉，其他判準就看不到了。
  正確頻率是**每 10 秒最多一行**。

**【擋不住什麼／覆蓋範圍】**
- 這一項只驗 **slave 端**那一份告警。master 端是**另一段程式碼**
  （各自寫在自己的 `onEspNowRecv()` 裡，只是內容相同），本項**完全沒有覆蓋到它**。
- 要驗 master 端那份，得製造「舊 slave 主動發話」的情境 ——
  最實際的做法是**同時擺一台舊 master 讓舊 slave 跟它配對**，
  舊 slave 才會有 STATE／PAIR_REQ 流量落到新 master 的天線上。
  **調不出來就註明「master 端未覆蓋」，不要用 slave 端的 PASS 代稱兩端都驗過。**

### 4-3. 🖥 群組指令：執行證明會出現，而且與送達證明分開

**步驟**：2 台 slave 都在線，序列埠 `allpulse`。

**預期（master 序列埠，收工那段）**：
```
[群組] 指令 2 收工：單播 MAC 層已送達 2／2 台
[群組] 韌體層已執行 2／2 台（slave 回報 cmdId=<數字>）
[群組] 注意：MAC 層 ACK 只證明「封包已送達」，不能證明繼電器真的動作
[群組] 執行證明只到韌體層：證明 slave 走完了繼電器動作那段程式，不證明繼電器硬體動作，更不證明籠門關上
[群組] 沒有執行證明不等於沒執行 —— 回報可能還在路上；它只能維持紅色，不能宣稱已確認未執行
⚠ [群組] 這是關門路徑：未送達的籠門必然沒關；已送達的也只代表封包到了，一律以現場確認為準，不要當成已關閉
```

**預期（MQTT `hoban/<masterId>/status`，`busy:0` 的那則）**：
`group.exec` 是 `"attributed"`、`group.exed` 是 `2`、每筆 `slaves[]` 有
`"grp":1` 與 `"exe":1`。

**【失敗判定】**
- `exed` **大於** `n`。
- 某台 `"exe":1` 而**它自己的序列埠從未印過**對應的 `[繼電器] 點動 2000 ms`
  —— 那是歸因指錯台，誤綠方向，必須判 FAIL。
- 少印那三行說明中的任何一行。
- `cmdId` 的數字在同一次開機內的**兩道不同指令**之間相同 —— `allocCmdId()` 壞了。

**【觀察項，不是失敗判定】**
- `cmdId` 每次開機都不同（亂數初值），**判準是「有印出這一行」而不是某個值**。
- `韌體層已執行` 的分子**可能小於**分母而其實一切正常 —— 回報還在路上。
  後續的 status 會看到 `exed` 自己變大。
- `[歸因] … 次數=4` 之類大於 1 的次數是**設計行為**（3 次廣播 ＋ 1 次單播）。

### 4-4. 🖥 只收得到廣播的 slave：`grp` 紅、`exe` 綠

> 這一項是本次改動**唯一能證明「兩種證據互補」**的驗收項，也是最不容易湊到的。

**步驟**：照 `phase1` 清單 8a 的方法把一台調到訊號邊界，但方向相反 ——
要讓它**收得到廣播、單播的 ACK 卻回不來**。

**預期**：那台在 MQTT 上是 `"grp":0,"exe":1`，而它自己的序列埠**確實印過**
`[繼電器] 點動 2000 ms`。

**【失敗判定】**
- 那台是 `"exe":0` 而序列埠明明印過繼電器動作，且 master 序列埠也印過
  `[歸因] <那台> 回報已執行 …` —— 代表歸因比對邏輯壞了。

**【擋不住什麼／覆蓋範圍】**
**這個情境在實務上很難刻意製造**：ESP-NOW 單播有 MAC 層重傳，
「廣播通、單播 ACK 全掉」比 8a 的反向情境更罕見。
**本項調不出來時請直接註明「未覆蓋」，不要用其他情境代替後宣稱通過** ——
那正是本專案 A 族病灶（覆蓋宣稱與事實不符）的標準長法。
