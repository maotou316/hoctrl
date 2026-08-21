# Phase 4 桌面優先清單（只要 **1 台 master** 就跑得完的項目）

> **警告：本清單尚未在任何實體硬體上執行過任何一項。**
>
> 不只本清單 —— **整個 Phase 4（Task 1~5）至今零實機回歸**。
> OTA 轉送的程式全部寫完、四個型號都編得過，但**一次實機測試都沒跑過**，
> 所有結論都是**靜態推演**。
>
> 這一點必須認真看待：本專案的 review 過程中出現過
> **「靜態推演錯到連『新增的測試抓不抓得到目標缺陷』都推錯」**。
> 所以請把下面每一項都當成「**待驗證的假說**」，不是「已知會通過的步驟」。

> **這份清單為什麼獨立成一個檔案**
>
> 使用者手上現在只有 master 板子，而 Phase 4 的完整回歸清單裡真正需要
> 「實體 slave ＋ 20 台規模 ＋ 野外佈置」的佔了大半。混在同一張表裡的話，
> 使用者無從知道該從哪裡開始。**這份只收「今天就跑得完」的**，
> 需要 slave 的一律不收，留給之後的 `docs/phase4-regression-checklist.md`。

---

## 收錄判準（**硬體張數**軸，與既有的距離／佈置標記無關）

> **一台 master ＋ USB 序列埠 ＋ WiFi 就能完成；不需要任何 slave 實體板、
> 不需要第二台裝置、不需要拉遠或做訊號屏蔽。**

- `docs/phase2b-regression-checklist.md` 的 `🖥`／`📡`／`⛔` 是**距離／佈置**軸
  （桌面即可／需要訊號邊界／今天做不到），**語義與本清單不同，本清單不使用、
  也不重新定義它們**。
- 本清單**整份**都符合上面那句判準，所以**不需要任何標記** ——
  每一項都是「一台 master 就夠」。
- **反過來的判準同樣重要**：凡是在程式路徑上會撞到
  「名冊裡必須有一台**真實且在線**的 slave」的項目，**一律不收**，
  即使它「感覺上不接觸 slave」。哪些項目因此被排除、依據是什麼，
  寫在本檔最後的〈**被排除的候選項**〉一節 —— **請務必讀那一節**，
  否則你會以為那些項目今天做得了。

---

## 這份清單怎麼寫的（維護時請照做）

本專案有一類缺陷已經出現**四次**：**回歸清單的驗收標準與程式碼矛盾，
導致實測者把正確行為判成 FAIL。**（最近一次：清單寫「按住 1.5 秒放開不應印出
任何訊息」，而程式碼在按下的瞬間就無條件印出。）

因此：

1. 每一條「預期序列埠輸出」都**逐字對照過**程式碼的 `Serial.print*` 格式字串
   （去掉 `%d`／`%s`／`%u` 之後用 grep 回讀），
   每一項結尾用 `> 對照：` 一行寫出對照位置。
2. **判準以「字串」為準，不以行號為準。** 本專案的行號漂移過
   （`phase2b-regression-checklist.md` 的 117 處對照裡，31 處可判定的錯了 30 處），
   所以本檔**一個行號都不寫**。
3. **凡是「不應該印出某訊息」這類否定式判準一律禁止**，除非能明確指出程式碼中
   沒有任何路徑會印出它（本檔只有兩處，各自附了依據）。一律改寫成正面判準。
4. 每一項分成 **【失敗判定】** 與 **【觀察項】**。機制未經實機驗證的
   （`setFollowRedirects` 的實際行為、`esp_partition_erase_range` 的實際耗時、
   arduino-cli 對 WROOM 的實際抹除行為）**一律列為觀察項，不得列為失敗判定**。
5. **列舉值與數值一律回去讀定義**，不憑名稱推順序
   （本專案曾把 `alloff` 的指令碼寫成 `2`，實際 `HO_CMD_OFF = 0`）。
6. 每一項都要寫「**它擋不住什麼**」，與「它擋住什麼」寫在一起。

---

## 前置需求

- **1 台已燒錄 `ho_master1` 的板子**（`master`＝ESP32 WROOM，或 `master-c3`＝ESP32-C3），
  序列埠 115200 開著
- 一條能燒錄的 USB 線；`A:\server\arduino-cli\arduino-cli.exe` 存在
- 一個可用的 WiFi AP（2.4G）
- MQTT Explorer（或 `mosquitto_sub`／`mosquitto_pub`），**要能看到 retain 訊息、
  也要能刪除 retain 訊息**
- **本清單不需要 App**，全部用序列埠 ＋ MQTT 直接送純文字／JSON 驗收

### ⚠ 用 `master-c3` 執行的人必讀

**`master-c3` 的 OTA 路徑從未在實機上驗證過任何一段。**
Task 3、4、5 三個 Task 全程都沒有在 C3 上跑過 OTA 的任何一步。
因此：**若你用 `master-c3` 執行本清單，所有 OTA 相關項目（D1、D5~D8）
一律視為「首次驗證」——出現的任何異常都不能假定是你操作錯誤。**
（C3 已知的另一件事是「開機瞬間繼電器短暫通電」，那是硬體限制、
與本段講的「OTA 路徑零驗證」是兩件不同的事。）

### ⚠ 公用 broker 的 retain 副作用（D1／D2 一定會踩到）

`fakeslaves` 與 `fakeota` 灌進去的**捏造值會經由正常的 status 發布路徑
帶 `retain=true` 送上 broker**，而預設伺服器清單五台裡有四台是公用的
（`mqttgo.io`／`mqtt.eclipseprojects.io`／`broker.emqx.io`／`broker.hivemq.com`），
**retain 永久保留、韌體端沒有任何自動清除機制**。

兩者的性質**不一樣，別混為一談**：

| 受影響的 topic | 重開機會不會壓掉 |
|---|---|
| `hoban/<masterId>/status`（`fakeota` 捏造的 `ota` 物件、`slave_count: 20`） | **會**。重開機後下一則真實 status 就覆蓋掉了 |
| `hoban/hoban-aabbccddee00/status` … `…ee13/status`（`fakeslaves` 的 20 條） | **不會，永久留著**。假 MAC 不寫 NVS，重開機後名冊裡沒有它們，`publishSlaveStatus()` 再也不會碰那 20 條 topic |

**三個做法，挑一個**（沿用 `docs/phase2b-regression-checklist.md` 第 2 項的同一套，
細節請去讀那一節，本檔不重寫一份）：

1. **【最安全】在還沒連上 MQTT 時做 D1。** `jsonsize` 走 `buildStatusDoc()` ＋
   `measureJson()`，完全不碰 socket。代價是量到的 `mqtt buffer` 會是 `256`。
   **D2 需要 broker，做法 1 做不到。**
2. **【推薦】把 master 配網到一台私有 broker**（筆電上的 mosquitto 等），D1／D2 都在那台做。
3. **在公用 broker 上做，做完自己清。** 清除步驟見 **D2 的「★ 收尾」**，**不要略過**。

### 這份清單與「完整回歸清單」的關係

`docs/phase4-regression-checklist.md`（27 項的總表）**目前尚未建立**。
下面每一項若在 Task 6 brief 的規劃編號裡有對應，會標成「**總表規劃第 N 項**」；
總表寫出來之後，細節應以總表為準，本檔只留一行摘要與指路
（**兩處各寫一份必定腐爛**）。

---

## 項目一覽

| 編號 | 一句話 | 類型 |
|---|---|---|
| **D0** | 燒錄 master 並確認開機序列 | 失敗判定 |
| **D1 ★** | `fakeslaves 20` → `fakeota` → `jsonsize` 的容量實測（**最高優先**） | 失敗判定 |
| **D2** | 同一狀態實際發布一次 `status`，broker 上的 JSON 完整、`ota` 五欄齊全 | 失敗判定 |
| **D3** | `otastat` 在閒置時的輸出 | 失敗判定 |
| **D4** | `help` 指令表含四條 OTA 指令與 retain 警告 | 失敗判定 |
| **D5** | 空名冊下 `otadl 0 <url>` 被範圍守衛擋下 | 失敗判定 |
| **D6** | 假名冊下 `otarelay 0` 被 `fakeSlavesActive` 守衛擋下 | 失敗判定 |
| **D7** | `update_slave` 的三條**純解析**拒絕路徑（`bad_json` ×2、`no_target`） | 失敗判定 |
| **D8 ★** | `otaSetTarget()` 的字元過濾（`STATUS_OTA_MAX_BYTES = 128` 的前提 (3)） | 失敗判定 |
| **D9** | `test` 型號的協定測試：**執行 57 項，失敗 0 項**（⚠ 需 C3 板，會覆蓋韌體） | 失敗判定 |

**共 10 項。** 每一項底下各自另有【觀察項】，那些**不判 FAIL**。

**建議順序**：D0 → **D1** → D2 →（收尾清 retain）→ D3 → D4 → D5 → D6 → D7 → D8 → D9。
**D9 放最後**，因為它會把板子上的韌體覆蓋掉。

---

# D0. 燒錄 master 並確認開機序列

**步驟**：

```powershell
.\flash.ps1 -Model master -Upload          # ESP32 WROOM
# 或
.\flash.ps1 -Model master-c3 -Upload       # ESP32-C3
```

（`-Port COMx` 可手動指定埠；不加時腳本會自動偵測。）
燒完打開序列埠監控（115200），按一次 RESET。

**預期（序列埠，開機時）**：

```
齁控 Master v1.0.0
================
```
接著（**已有 WiFi 設定**時會直接連 WiFi；**沒有 WiFi 設定**時會多印一行
`[BLE] 等待 App 配網`），最後固定是：
```
設備 ID: hoban-xxxxxxxxxxxx
── 可用指令 ──
  list          列出所有 slave
  ...
  help          顯示這份說明
就緒
```

序列埠輸入 `list`，名冊是空的時候印：
```
── Slave 名冊（0／20）──
  （空）
```

**【失敗判定】**
- 腳本以非 0 結束（`編譯失敗` 或 `燒錄失敗`）。
- 序列埠看不到 `齁控 Master v1.0.0` 這一行（版本字串 `firmwareVersion` 目前是 `"1.0.0"`）。
- 看不到 `就緒` 那一行 —— 代表 `setup()` 沒跑完。
- 反覆重開機（開機橫幅每幾秒重印一次）＝ 開機當機迴圈。

**【觀察項，不是失敗判定】**
- **燒完之後 WiFi 設定到底還在不在，兩種結果都不判 FAIL。**
  腳本會印 `模式：全抹 flash（EraseFlash=all，燒完需重新 BLE 配網）`，
  但 **`master`（WROOM）那組 FQBN 的字串裡沒有 `EraseFlash` 這個鍵**
  （只有 `PartitionScheme=custom`），而 `master-c3`／`slave`／`test`
  三組 FQBN 都帶 `EraseFlash={0}`。所以 WROOM 上「腳本的訊息」與
  「FQBN 實際帶的選項」不一致。**arduino-cli 對這顆板子實際會不會抹，
  本專案沒有驗證過，所以列為觀察項。**
  **請照實記錄**：開機後看到的是 `[BLE] 等待 App 配網`（設定被抹掉），
  還是直接開始連 WiFi（設定留著）。
- 若進了 BLE 配網模式，**本清單的 D2／D7／D8 需要 MQTT**，
  請先用 App 完成配網（或改用一台已配網的板子）再繼續。
- `設備 ID:` 後面那串就是本清單各處的 `<masterId>`，**抄下來**。

> **它擋不住什麼**：只驗「燒得進去、開得起來、序列埠活著」。
> 不驗任何 OTA 行為，也不驗 flash 用量（用量在 Task 6 的編譯盤點裡看，不在本清單）。

> 對照：`flash.ps1` 的 `模式：全抹 flash（EraseFlash=all，燒完需重新 BLE 配網）`、
> `編譯失敗`、`燒錄失敗`、`$configs` 裡 `'master'` 的
> `Fqbn = 'esp32:esp32:esp32:PartitionScheme=custom'`；
> `ho_master1.ino` 的 `Serial.println("齁控 Master v" + String(firmwareVersion))`、
> `const char* firmwareVersion = "1.0.0"`、`Serial.printf("設備 ID: %s\n", getDeviceId())`、
> `Serial.println("就緒")`、`Serial.println("[BLE] 等待 App 配網")`；
> `ho_master1.ino` 的 `printSlaveList()` 之 `Serial.printf("── Slave 名冊（%d／%d）──\n", …)`
> 與 `Serial.println("  （空）")`；`ho_master1.ino` 的 `printHelp()` 之 `── 可用指令 ──`。

---

# D1 ★. 容量實測：`fakeslaves 20` → `fakeota` → `jsonsize`

> **總表規劃第 19 項。這是整份清單的最高優先項，請第一個跑。**
>
> **理由**：`STATUS_OTA_MAX_BYTES = 128` 這個上界的**四條前提全部是靜態字面檢查**
> （`tools/check_doc_claims.py` 方向 19 量的是「原始碼裡的字面長度」），
> 而「**ArduinoJson 真的照這個算法序列化**」**從來沒有被實際量過一次**。
> 整條靜態推演鏈上，**這一項是唯一能被一台板子當場證偽的**。
>
> ⚠ **動手前請先讀完前置需求裡的〈公用 broker 的 retain 副作用〉並挑好做法。**

**步驟**：

1. **先重新開機**（確保這次開機還沒下過 `fakeslaves`／`fakeota`，
   也確保 `otaPhase` 是 `idle`）。
2. 序列埠輸入 `fakeslaves 20`
3. 序列埠輸入 `fakeota`
4. 序列埠輸入 `jsonsize`
5. **順手記一下**：序列埠輸入 `otastat`（D3 要用，而且它能告訴你 `otaPhase` 是不是還在 `idle`）

**預期序列埠輸出（第 2 步）**：
```
[測試] 名冊已灌成 20 台假 slave（未寫入 NVS，重開機即消失）
⚠ [測試] 重開機前請勿執行 pair／unpair：名冊混有假 MAC，一旦觸發存檔就會寫進 NVS 汙染真實名冊（已由 saveSlaves() 擋下）
```

**預期序列埠輸出（第 3 步）**（下面五行的 `**` 星號是**程式碼字串裡真的有的**，
不是本文件的粗體標記）：
```
[測試] ota 欄位已灌成最壞值（未啟動工作階段），請接著執行 jsonsize
       正確的驗證順序：fakeslaves 20 → fakeota → jsonsize
⚠ [測試] 若當下已連上 broker，這些**捏造的 ota 欄位**會隨下一次狀態發布以 retain=true 壓上 hoban/<本機 ID>/status，而 retain 是永久保留、重開機也不會被自動清（與 fakeslaves 同一個坑）
       清除方式：重開機後讓 master 重新發一次真實狀態壓過去，或用 MQTT 客戶端對該 topic 發一則空 payload 的 retain 訊息
⚠ [測試] jsonsize 量到的 phase 是 "idle"（4 字元）而非最壞的 12 字元，記錄結果時要把 8 bytes 加回去再與 statusBuf 比較
```

**預期序列埠輸出（第 4 步）—— `mqtt buffer` 有兩種合法值**：
```
[測試] 狀態 JSON 實際 <N> bytes／statusBuf 3584／mqtt buffer 3840（名冊 20 台）
```
或（本次開機從未嘗試過 MQTT 連線時，走做法 1 的人會看到這個）
```
[測試] 狀態 JSON 實際 <N> bytes／statusBuf 3584／mqtt buffer 256（名冊 20 台）
```

### 怎麼算才算過

實測值 `<N>` **不是**可以直接拿去跟 3584 比的數字，要先補兩筆：

| 補正 | bytes | 依據 |
|---|---|---|
| `phase` 字串 | **+8** | 量到的是 `"idle"`（4 字元），最壞是 11 字元（`"downloading"`／`"unconfirmed"`／`"staged_only"`）；上界常數按 12 字元算，`+8` 是保守值 |
| `progress` 位數 | **+2**（**只在 `otastat` 顯示 `階段=idle` 時才要加**） | `otaProgressPercent()` 有一條 `if (otaPhase == OTA_IDLE) return 0;`，**排在**「`otaBlockBase / otaTotalChunks` 換算」之前。`fakeota` 只填欄位、**一個字都不動 `otaPhase`**，所以剛開機做這一輪時 `progress` 會序列化成 `0`（1 byte）而不是最壞的 `100`（3 bytes） |

**判準**：`<N> + 8 + 2` **必須 < 3584**。

**【失敗判定】**
- `<N> + 8 + 2` **不小於 3584** → 容量防線已被破壞，**停工回報**。
- `statusBuf` 那個數字不是 `3584`。
- 本次開機**已經連上過** broker（序列埠有出現過 `[MQTT] 已連線 …`），
  但 `mqtt buffer` 不是 `3840`。
- 出現 `⚠ [MQTT] slaves 陣列被截斷：名冊 20 台，只放得下 <n> 台`
  —— 執行期上限 `maxEntries` 算出來是 **25**（`(3584-1-728-11)/112 = 25`），
  20 台不該被截斷。

**【觀察項，不是失敗判定】**
- **`fakeota` 的註釋說它「讓 `otaProgressPercent()` 回 100（3 位數，最壞）」，
  而剛開機時實際會回 `0`** —— 因為它沒動 `otaPhase`，而 `OTA_IDLE` 那條提前 return
  排在換算之前。**這不是缺陷、不判 FAIL**：方向是**低估 2 bytes**（樂觀 2 bytes），
  上表的 `+2` 就是把它補回來。**請照實把 `<N>`、`otastat` 的 `階段=` 值一起記下來。**
  （若你不是剛開機、`otastat` 顯示的階段不是 `idle`，`progress` 已經是 `100`，
  那就**不要**再加那 2 bytes。）
- Phase 2b 在另一台板子上實測到的是 **2100 bytes**（那是 Phase 2b 的韌體、
  而且沒有 `ota` 物件）。**你這台一定不同也應該不同**：`<N>` 受 **SSID 長度**、
  **有沒有設自訂 MQTT 伺服器**、**有沒有下過群組指令**影響。
  **只要補正後 < 3584 就是 PASS，不必等於任何歷史數字。**
- 假 slave 沒有註冊 ESP-NOW peer，所以序列埠會一直刷
  `[ESP-NOW] esp_now_send 失敗: <碼>`（`pollNextSlave()` 每輪對假 MAC 送 `STATE_REQ`）。
  **這是預期的，不是失敗。**

> **本項擋得住什麼、擋不住什麼**
>
> - **擋得住**：ArduinoJson 在「20 台 ＋ 最壞 `ota` 物件」下的**實際**序列化長度
>   超出 `statusBuf`。這是整條容量推演鏈上**第一次**被真正量到。
> - **擋不住悲觀情境。** `<N>` 量的是**你這台當下設定**的基礎欄位，而
>   `STATUS_BASE_MAX_BYTES = 728`（480＋120＋128）留的是最壞情況
>   （63 字元的自訂 MQTT 伺服器位址 ＋ 長 SSID）。
>   **短 SSID、沒設自訂伺服器的板子量出來會樂觀好幾百 bytes** ——
>   PASS 只代表「這台這個設定放得下」，**不代表悲觀上界成立**。
>   悲觀上界是靠 `static_assert` 在**編譯期**保證的，不是靠本項。
> - **擋不住 `ota["size"]` 的位數。** `otaTotalSize` 是 `uint32_t`，十進位最多 10 位；
>   `fakeota` 灌的是 `2031616`（7 位）。真正最壞的 10 位不在本項覆蓋範圍內
>   （原始碼的註釋已把這件事寫明，說 128 這個數字「不是被完整證明過的」）。
> - **擋不住 `target` 的逃逸字元** —— 那是前提 (3)，由 **D8** 覆蓋。
> - **擋不住 21 台以上**：`maxEntries` 是 25，而名冊硬上限 `HO_ESPNOW_MAX_SLAVES` 是 20，
>   所以 `slaves_truncated` 這條執行期路徑至今**零覆蓋**，本項也不覆蓋它。

**收尾**：**先不要重開機** —— D2 要接著在同一個狀態下做。
（走**做法 1**（不連 MQTT）的人：D2 需要 broker、做不到，**D1 做完直接重開機即可**，
沒有殘留需要清。）
`fakeslaves` 期間**不要下 `pair`／`unpair`**（韌體會擋，但別去試）。

> 對照：`ho_master1.ino` 的
> `Serial.printf("[測試] 名冊已灌成 %d 台假 slave（未寫入 NVS，重開機即消失）\n", n)`；
> `ho_master1.ino` 的 `⚠ [測試] 重開機前請勿執行 pair／unpair：`；
> `ho_master1.ino` 的 `fakeOtaForCapacityTest()` 五行輸出，含
> `[測試] ota 欄位已灌成最壞值（未啟動工作階段），請接著執行 jsonsize`、
> `正確的驗證順序：fakeslaves 20 → fakeota → jsonsize`、
> `⚠ [測試] jsonsize 量到的 phase 是 \"idle\"（4 字元）而非最壞的 12 字元，`；
> `ho_master1.ino` 的
> `Serial.printf("[測試] 狀態 JSON 實際 %u bytes／statusBuf %u／mqtt buffer %u（名冊 %d 台）\n", …)`；
> 常數宣告區的 `const size_t STATUS_BUF_SIZE = 3584;`／
> `const size_t MQTT_BUFFER_SIZE = 3840;`／`const size_t STATUS_OTA_MAX_BYTES = 128;`／
> `STATUS_BASE_MAX_BYTES`（＝ 480 + 120 + 128 = 728）；
> `ho_master1.ino` 的 `⚠ [MQTT] slaves 陣列被截斷：名冊 %d 台，只放得下 %d 台`；
> `ho_master1.ino` 的 `otaProgressPercent()` 之 `if (otaPhase == OTA_IDLE) return 0;`；
> `ho_master1.ino` 的 `fakeOtaForCapacityTest()` 之
> `otaBlockBase = otaTotalChunks;`／`otaTotalSize = 2031616;`；
> `ho_master1.ino` 的 `[ESP-NOW] esp_now_send 失敗: %d`。

---

# D2. 同一狀態實際發布一次 `status`，broker 上的 JSON 完整、`ota` 五欄齊全

> **總表規劃第 20 項。** 這是「靜默截斷」的**正面驗證** ——
> D1 量的是 `measureJson()`（發布前的預估），這一項看的是 **broker 上真的收到什麼**。
>
> **前置：本項一定要有 broker**，所以 D1 的做法 1（不連線）在這裡不適用。
> 請走**做法 2（私有 broker，推薦）**或**做法 3（公用 broker ＋ 做完自己清）**。

**步驟**：接續 D1（名冊仍是 20 台假 slave、`ota` 仍是捏造值，**中間不要重開機**），
用 MQTT Explorer 對 `hoban/<masterId>/control` 發布純文字 `status`。

**預期（序列埠）**：
```
[MQTT] 收到指令: status
```

**預期（MQTT Explorer，`hoban/<masterId>/status`）**：JSON 更新，且
- JSON **能被解析**（MQTT Explorer 會用樹狀顯示；解析失敗它會顯示成純文字）
- `device.slave_count` 是 `20`
- `slaves` 陣列**剛好 20 筆**，`id` 依序是
  `hoban-aabbccddee00`、`hoban-aabbccddee01`、…、`hoban-aabbccddee13`
  （**十六進位，第 20 台是 `13` 不是 `19`**）
- 每筆的 `relay` 是 `1`、`online` 是 `false`、`rssi` 是 `-100`、`version` 是 `"255.255.255"`
- **`ota` 物件存在，且剛好五個欄位**：
  | 欄位 | 預期值 |
  |---|---|
  | `target` | `"hoban-aabbccddeeff"` |
  | `phase` | `"idle"`（`fakeota` **刻意不動** `otaPhase`） |
  | `progress` | `0`（見 D1 的補正表；**不是** `100`） |
  | `size` | `2031616` |
  | `error` | `"slave_timeout"` |
- **沒有** `slaves_truncated` 這個 key
  （**這是本檔兩條否定式判準之一，而它有明確依據**：`slaves_truncated` 只在
  `shown < slaveCount` 時才被寫入，而 `shown = min(slaveCount, maxEntries)
  = min(20, 25) = 20 = slaveCount`，**程式上沒有任何路徑**會在 20 台時寫入它。）

**【失敗判定】**
- JSON 語法不完整（尾端被切掉、少一個 `]` 或 `}`）。
- `slaves` 不足 20 筆，或出現 `slaves_truncated`。
- `ota` 物件不存在，或五個欄位缺任何一個。
- 出現 `⚠ [MQTT] 放棄發布 hoban/<masterId>/status：JSON 需要 <n> bytes，statusBuf 只有 3584`
  或 `⚠ [MQTT] 放棄發布 …：整包需要 <n> bytes，mqtt buffer 只有 3840`。

**【觀察項，不是失敗判定】**
- 每筆 slave 條目上若多出 `"grp"`／`"exe"`：那是本次開機下過群組指令才會有，
  而 `fakeslaves` 的假 MAC 不在任何群組快照裡，所以**通常兩個都不帶** —— 都正常。
- 偶爾看到 `[MQTT] hoban/<某個 topic>/status 讓位給下一輪（本輪 publish 名額已用掉）`
  —— **設計行為**，不是缺陷。
- master 自己的 `wifi.rssi`、`device.relay` 等欄位不影響本項。

> **本項擋得住什麼、擋不住什麼**
>
> - **擋得住**：發布路徑上的靜默截斷（broker 上收到的是完整 20 筆 ＋ 完整 `ota`）。
> - **擋不住悲觀情境**（理由與 D1 完全相同）。
> - **擋不住 `ota` 五個欄位的最壞組合同時出現。** `fakeota` 灌的是
>   「最長 error ＋ 18 字元 target ＋ 7 位 size」，而最壞是
>   「12 字元 phase ＋ 19 字元 target ＋ 10 位 size」——**三者不會同時被量到**。
> - **擋不住 App 端的行為**。App 的 `lib/` 對 `ota` 物件**零讀取點**，
>   所以本項 PASS 不代表 App 會顯示什麼。

### ★ 收尾（走做法 3 ＝ 公用 broker 的人**一定要做**）

**沿用 `docs/phase2b-regression-checklist.md` 第 3 項底下那套做法，不要另創一套。**

1. **先重新開機 master**。這會做掉一件事、做不掉另一件：
   - ✔ **做得掉**：`hoban/<masterId>/status` 那一則 —— 重開機後名冊回到真實狀態、
     `ota` 欄位回到初值，下一則真實 status 會把 `fakeota` 的捏造值與
     `slave_count: 20` **覆蓋掉**。
   - ✘ **做不掉**：`fakeslaves` 那 20 條。**假 MAC 不寫 NVS，重開機後名冊裡沒有它們**，
     `publishSlaveStatus()` 再也不會碰那 20 條 topic，
     **所以沒有任何韌體路徑能自動清掉它們**。這就是為什麼要人工清。
2. **手動刪掉那 20 則永遠不會被覆蓋的 retained 訊息**：
   `hoban/hoban-aabbccddee00/status` … `hoban/hoban-aabbccddee13/status`
   （**十六進位，00~13 共 20 條**）。
   刪法是**對該 topic 發布「零長度 payload ＋ retain=true」**：
   - MQTT Explorer：選中該 topic → `Delete retained message`
   - CLI：`mosquitto_pub -h <broker> -t hoban/hoban-aabbccddee00/status -r -n`（20 條各跑一次）
3. **（保險）連 `hoban/<masterId>/status` 也一起清一次**，若你想確定 `ota` 捏造值
   不會留在 broker 上：同樣發一則空 payload ＋ retain。
   （第 1 步的覆蓋通常就夠了，這一步是保險。）
4. **確認清乾淨**：重新連上 broker、訂閱 `hoban/#`，
   應該只剩真實設備的 topic，沒有任何 `hoban-aabbccddee??`。

> ⚠ **`fakeslaves` 期間請不要打開該 master 的詳情頁，更不要按「加入」。**
> App 的 `device_detail_page.dart` 現行版本只要打開任何一台 master 的詳情頁
> 就會渲染出「子設備（N 台）」清單，每一列還帶一顆「加入」鈕，
> 按下去會把假設備**寫進 Firestore**，之後 App 會真的去訂那 20 條 topic，
> 變成**永遠在線、怎麼控制都沒反應的持久幽靈設備**。

> 對照：`ho_master1.ino` 的 `Serial.printf("[MQTT] 收到指令: %s\n", message.c_str())`；
> `ho_master1.ino` 的 `handleMasterCommand()` 之 `if (message == "status") { publishStatus(); }`；
> `ho_master1.ino` 的 `buildStatusDoc()` 之
> `ota["target"]   = otaTargetId;`／`ota["phase"]    = otaPhaseName();`／
> `ota["progress"] = otaProgressPercent();`／`ota["size"]     = otaTotalSize;`／
> `ota["error"]    = otaErrCode;`；
> `ho_master1.ino` 的 `fakeOtaForCapacityTest()` 之
> `snprintf(otaTargetId, sizeof(otaTargetId), "hoban-aabbccddeeff");` 與
> `snprintf(otaErrCode, sizeof(otaErrCode), "slave_timeout");`；
> `ho_master1.ino` 的 `fakeSlavesForCapacityTest()`
> （`mac[5] = (uint8_t)i`、`online = false`、`rssi = -100`、`relay = 1`、`fw* = 255`）；
> `libraries/HoEspNow/src/HoEspNowProtocol.cpp` 的
> `snprintf(out, 20, "hoban-%02x%02x%02x%02x%02x%02x", …)`；
> `ho_master1.ino` 的 `appendSlavesArray()` 之 `if (shown < slaveCount) { doc["slaves_truncated"] = true; … }`；
> `ho_master1.ino` 的 `[MQTT] %s 讓位給下一輪（本輪 publish 名額已用掉）`。

---

# D3. `otastat` 在閒置時的輸出

**步驟**：重新開機後（名冊真實、沒下過 `fakeota`），序列埠輸入 `otastat`。

**預期（序列埠）**：**一行**，格式如下（`目標=` 後面是空的、`錯誤=無`）：
```
[OTA] 階段=idle 目標= 下載=0/0 bytes 區塊=0/0 包 進度=0% 送出失敗=0 錯誤=無
```

**【失敗判定】**
- 這一行的**欄位名稱或順序**與上面不同（`階段=`／`目標=`／`下載=`／`bytes`／
  `區塊=`／`包`／`進度=`／`%`／`送出失敗=`／`錯誤=`）。
- `階段=` 的值不是 `idle` —— 剛開機、沒下過任何 OTA 指令時，
  `otaPhase` 的宣告初值就是 `OTA_IDLE`，`otaPhaseName()` 的 `case OTA_IDLE` 回 `"idle"`。

**【觀察項，不是失敗判定】**
- **在 D1 之後（沒重開機）下 `otastat`**，預期會變成
  `階段=idle 目標=hoban-aabbccddeeff 下載=0/2031616 bytes 區塊=8466/8466 包 進度=0% 送出失敗=0 錯誤=slave_timeout`。
  `8466` 是 `HO_OTA_MAX_CHUNKS`（`(2031616 + 240 - 1) / 240`）。
  **這一組數字若對得上，等於同時驗到了 D1 的 `progress` 補正依據**（`進度=0%` 而非 100%）。
  請照實記錄，不判 FAIL。
- `階段=idle` 時 `目標=` 與 `錯誤=` 是**上一次工作階段的殘值**（刻意保留，
  讓現場還看得到上一次失敗的原因）。這是設計，不是髒資料。

> **它擋不住什麼**：只驗這一行的格式與閒置初值。
> 不驗任何真正的 OTA 狀態轉換 —— 那些全部需要一台真實 slave。

> 對照：`ho_master1.ino` 的
> `Serial.printf("[OTA] 階段=%s 目標=%s 下載=%u/%u bytes 區塊=%u/%u 包 進度=%u%% " "送出失敗=%u 錯誤=%s\n", …)`
> （`otaErrCode[0] ? otaErrCode : "無"`）；
> `ho_master1.ino` 的 `OtaPhase otaPhase = OTA_IDLE;`；
> `ho_master1.ino` 的 `otaPhaseName()` 之 `case OTA_IDLE:           return "idle";`；
> `libraries/HoEspNow/src/HoEspNowProtocol.h` 的
> `#define HO_OTA_CHUNK_SIZE  240` 與
> `#define HO_OTA_MAX_CHUNKS  ((uint16_t)((2031616 + HO_OTA_CHUNK_SIZE - 1) / HO_OTA_CHUNK_SIZE))`。

---

# D4. `help` 指令表含四條 OTA 指令與 retain 警告

**步驟**：序列埠輸入 `help`（開機時也會自動印一次）。

**預期（序列埠）**：`── 可用指令 ──` 底下**必須逐字出現**下面這幾段
（`otadl`／`otarelay`／`otastat` 是 Phase 4 新加的，`fakeslaves`／`fakeota`
底下各自帶著 retain 副作用的警告）：

```
  fakeslaves <n> 測試用：把名冊灌成 n 台假 slave，實測容量（不寫 NVS；
                 灌入後到重開機前，pair／unpair 會被擋下，避免假 MAC 寫進 NVS）
                 ⚠ 已連上 broker 時，這 n 台會以 retain 壓上各自的 status topic，
                 而重開機後名冊沒有它們、master 再也不會發那些 topic ——
                 只能手動對每一條 topic 發空 payload 的 retain 訊息才清得掉
  fakeota       測試用：把 ota 欄位灌成最壞值（不啟動工作階段、不寫 NVS），
                 配合 fakeslaves 20 → fakeota → jsonsize 實測容量；
                 量到的 phase 是 idle（4 字元），記錄時要加回 8 bytes
                 ⚠ 已連上 broker 時，捏造的 ota 欄位會以 retain=true 壓上
                 hoban/<本機 ID>/status，retain 永久保留、重開機不會自動清；
                 清除：重開機讓 master 重發真實狀態壓過去，或對該 topic
                 發一則空 payload 的 retain 訊息
  jsonsize      測試用：印出目前狀態 JSON 的實際大小
  otadl <n> <url>  測試用：只下載並暫存韌體，不轉送（會抹除 master 的閒置 OTA 分區，
                 不動開機分區；固定略過『繼電器正開著就拒絕』的保護）
  otarelay <n> [版本] [force]  測試用：把暫存分區裡那份韌體轉送給第 n 台
                 （要先跑過 otadl；轉送成功那台會重開機，繼電器會斷一次，
                  省略版本＝0.0.0，版本回檢必定走到 90 秒 no_return）
  otastat       印出目前 OTA 工作階段的階段與進度
  help          顯示這份說明
```

**【失敗判定】**
- `otadl`／`otarelay`／`otastat`／`fakeota` 這四條**任何一條沒出現在指令表裡**。
- `fakeslaves` 或 `fakeota` 底下的 **retain 警告段落缺席**
  ——「只寫不寫 NVS、不寫 retain」曾經是本專案被抓到的缺陷型態
  （會讓人以為重開機就乾淨了）。

**【觀察項，不是失敗判定】**
- 指令表很長，序列埠終端若有捲動上限可能吃掉開頭幾行。**請用 `help` 手動再印一次**，
  不要用開機時那一份判 FAIL。

> **它擋不住什麼**：只驗「說明文字印得出來」。
> 不驗那些指令實際做不做得到它說的事 —— `otadl`／`otarelay` 的實際行為
> 需要真實 slave，見〈被排除的候選項〉。

> 對照：`ho_master1.ino` 的 `printHelp()`，逐行含
> `  otadl <n> <url>  測試用：只下載並暫存韌體，不轉送（會抹除 master 的閒置 OTA 分區，`、
> `  otarelay <n> [版本] [force]  測試用：把暫存分區裡那份韌體轉送給第 n 台`、
> `  otastat       印出目前 OTA 工作階段的階段與進度`、
> `  fakeota       測試用：把 ota 欄位灌成最壞值（不啟動工作階段、不寫 NVS），`、
> `                 ⚠ 已連上 broker 時，捏造的 ota 欄位會以 retain=true 壓上`、
> `                 ⚠ 已連上 broker 時，這 n 台會以 retain 壓上各自的 status topic，`。

---

# D5. 空名冊下 `otadl 0 <url>` 被範圍守衛擋下

> 這是「編號範圍守衛」的**正面驗證**，也是本清單能碰到 `otadl` 的**唯一一段**
> —— 再往前一步就需要一台真實且在線的 slave（見〈被排除的候選項〉）。

**步驟**：確認 `list` 顯示 `── Slave 名冊（0／20）──` 與 `  （空）`
（**沒有下過 `fakeslaves`**；下過的話先重開機），然後序列埠輸入：

```
otadl 0 https://example.com/whatever.bin
```

**預期（序列埠）**：**一行**
```
[OTA] slave 編號超出範圍
```

**【失敗判定】**
- 印出的不是上面那一行。
- 出現任何 `[OTA] 開始工作階段 …` 或 `[OTA] 網址 …` —— 那代表範圍守衛沒擋住，
  master 會真的去抹除自己的閒置 OTA 分區並連網下載。
- 出現 `用法：otadl <slave 編號> <https 網址>` —— 那代表指令**沒有帶網址**
  （`otadl` 用 `argStr.indexOf(' ')` 找空白，找不到就印用法）。
  **請確認你打的是 `otadl 0 <url>` 兩個參數。**

**【觀察項，不是失敗判定】**
- `otadl` 的編號解析用的是 `String::toInt()`（**不是** `parseIndexArg()`），
  所以 `otadl abc <url>` 會被靜默當成編號 `0`。空名冊下一樣會落到
  `[OTA] slave 編號超出範圍`。**這個差異請照實記錄**（`on`／`off`／`otarelay`
  走的是 `parseIndexArg()`，`otadl` 沒有）。

> **它擋不住什麼**：它只驗「名冊是空的時候不會往下走」。
> 名冊上有真實 slave 時的行為（`offline`／`relay_on`／下載）本項完全不覆蓋。

> 對照：`ho_master1.ino` 的 `handleSerialCommand()` 之 `} else if (verb == "otadl") {`
> 分支，含 `Serial.println("用法：otadl <slave 編號> <https 網址>");`、
> `if (n < 0 || n >= slaveCount) { Serial.println("[OTA] slave 編號超出範圍"); }`、
> `otaStart(id, u.c_str(), "0.0.0", nullptr, true, true);`；
> `ho_master1.ino` 的 `Serial.printf("[OTA] 開始工作階段 %u：目標 %s，版本 %u.%u.%u\n", …)`。

---

# D6. 假名冊下 `otarelay 0` 被 `fakeSlavesActive` 守衛擋下

> 這道守衛存在的理由：`fakeslaves` 灌的 MAC 沒有對應的實體 slave，
> 對它們發起轉送等於把封包送進虛空。**它排在編號檢查與暫存映像檢查之前**，
> 所以本項不需要跑過 `otadl`。

**步驟**：

1. 重新開機
2. 序列埠輸入 `fakeslaves 20`
3. 序列埠輸入 `otarelay 0`

**預期（序列埠，第 3 步）**：**一行**
```
[OTA] 目前掛著 fakeslaves 的假名冊，拒絕轉送（那些 MAC 沒有對應的實體 slave）
```

**【失敗判定】**
- 印出的不是上面那一行。
- 印出 `[OTA] 暫存分區沒有可用的映像，請先跑一次 otadl <n> <url>（重開機後也要重跑，master 不會沿用上次開機留下的暫存內容）`
  —— 那代表 `fakeSlavesActive` 的守衛**排在暫存映像檢查之後**了，順序被改動過。
- 出現任何 `[OTA] 開始轉送工作階段 …`。

**【觀察項，不是失敗判定】**
- 本項也會踩到 retain 副作用（`fakeslaves` 一下就開始發）。
  **做完請執行 D2 的「★ 收尾」**，或乾脆把 D6 併在 D1／D2 那一輪裡做完再一起清。

> **它擋不住什麼**：只驗這一道守衛的**存在與排序**。
> `otarelay` 對真實 slave 的實際轉送行為完全不覆蓋。

> 對照：`ho_master1.ino` 的 `otaRelayStaged()`，守衛順序為
> `if (otaSessionBusy())` → `if (fakeSlavesActive) { Serial.println("[OTA] 目前掛著 fakeslaves 的假名冊，拒絕轉送（那些 MAC 沒有對應的實體 slave）"); }`
> → `if (n < 0 || n >= slaveCount)` → `if (otaStagePart == nullptr || otaTotalSize < 65536 || otaTotalChunks == 0)`；
> `ho_master1.ino` 的 `Serial.printf("[OTA] 開始轉送工作階段 %u：目標 %s，暫存 %u bytes／%u 包，宣告版本 %u.%u.%u\n", …)`。

---

# D7. `update_slave` 的三條**純解析**拒絕路徑

> 這是 Phase 4 唯一的遠端入口（`hoban/<masterId>/control` 的 `update_slave:`），
> 而它有三條拒絕路徑**在碰到名冊之前**就結束了 —— 那三條今天就驗得到。
> **前置**：master 已連上 WiFi 與某台 broker（序列埠出現過 `[MQTT] 已連線 …`）。

**步驟**：用 MQTT Explorer 對 `hoban/<masterId>/control` 依序發布下面三則**純文字**，
每則之間等 **35 秒以上**（終局階段會停留 30 秒才回 `idle`，等它回 `idle` 再送下一則）。

### D7-a：JSON 壞掉

送：
```
update_slave:{"id":
```

**預期（序列埠）**：
```
[MQTT] 收到指令: update_slave:{"id":
[OTA] update_slave 的 JSON 解析失敗: <ArduinoJson 的錯誤字串>
```
**預期（MQTT `hoban/<masterId>/status`）**：`ota.error` 變成 `"bad_json"`，
`ota.phase` 變成 `"failed"`。

### D7-b：缺 `id` 或 `url`

送：
```
update_slave:{"version":"1.0.1"}
```

**預期（序列埠）**：
```
[MQTT] 收到指令: update_slave:{"version":"1.0.1"}
[OTA] update_slave 缺少 id 或 url
```
**預期（MQTT）**：`ota.error` ＝ `"bad_json"`，`ota.phase` ＝ `"failed"`。

### D7-c：`id` 不在名冊上

送（用一個絕對不存在的 MAC）：
```
update_slave:{"id":"hoban-ffffffffff01","url":"https://example.com/x.bin","version":"1.0.1"}
```

**預期（序列埠）**：
```
[MQTT] 收到指令: update_slave:{"id":"hoban-ffffffffff01","url":"https://example.com/x.bin","version":"1.0.1"}
⚠ [OTA] 指令未附 md5，只能保證 ESP-NOW 這一段的完整性；HTTPS 目前用 setInsecure() 不驗證憑證，建議 App 帶上 md5
[OTA] 失敗：no_target（目標 hoban-ffffffffff01）
```
> ⚠ 上面那行 `⚠ [OTA] 指令未附 md5…` **不一定會出現在這一則**：
> 它印在 `otaStart()` 較後段，而 `no_target` 的 `return` **排在它之前**。
> **判準只看 `[OTA] 失敗：no_target（目標 hoban-ffffffffff01）` 這一行。**

**預期（MQTT）**：`ota.error` ＝ `"no_target"`，`ota.phase` ＝ `"failed"`，
`ota.target` ＝ `"hoban-ffffffffff01"`。

**【失敗判定】**
- 三則裡任何一則沒印出對應的那一行。
- `ota.error` 不是預期的值（`bad_json`／`bad_json`／`no_target`）。
- **三則裡任何一則出現 `[OTA] 開始工作階段 …`** —— 這三則都不該進入下載。

**【觀察項，不是失敗判定】**
- **D7-c 若印的是 `[OTA] 失敗：offline（目標 …）`**：代表 `WiFi.isConnected()`
  在那一瞬間是 false（那道檢查排在 MAC 解析之前）。重試一次即可，不判 FAIL。
- **D7-c 若印的是 `[OTA] 失敗：low_heap（目標 …）`**：代表 `ESP.getFreeHeap()`
  當下 **< 70000**（那道檢查也排在 MAC 解析之前）。
  **請把 `jsonsize` 之外的 heap 數字一起記下來** ——
  C3 的可用 heap 比 WROOM 少，這條路徑在 C3 上被踩到的機率未知、從未實測過。
- **`ota` 相關欄位在失敗後的發布節奏會變快**：`masterStatusIntervalMs()` 在
  `otaPhase != OTA_IDLE` 時回 `5000`（否則 `10000`），而終局階段會停留
  **30 秒**才回 `idle`。所以你會看到「約 5 秒一則、連續約 6 則、然後回到 10 秒一則」。
  **這是設計行為**，請照實記錄，不判 FAIL。
- App 的 `lib/` 對 `ota` 物件**零讀取點**，所以這三則不會讓 App 出錯，
  也不會在 App 上顯示任何東西。**不要拿 App 畫面當本項的判準。**

> **它擋不住什麼**
>
> - 只驗「碰到名冊之前」的三條路徑。`offline`／`relay_on`／`bad_url`／
>   以及所有下載與轉送路徑**全部不覆蓋** —— 它們都排在
>   「名冊上要有一台**在線**的真實 slave」之後（見〈被排除的候選項〉）。
> - **不驗 `busy`**：`busy` 需要當下有一個正在跑的工作階段，而本清單跑不出來。
> - 不驗 App 端如何呈現這些錯誤（App 根本不讀 `ota`）。

> 對照：`ho_master1.ino` 的 `handleMasterCommand()` 之
> `} else if (message.startsWith("update_slave:")) {` 分支，含
> `Serial.printf("[OTA] update_slave 的 JSON 解析失敗: %s\n", err.c_str());`、
> `snprintf(otaErrCode, sizeof(otaErrCode), "bad_json");`、
> `Serial.println("[OTA] update_slave 缺少 id 或 url");`、
> `otaStart(id, url, ver, md5, force, false);`；
> `ho_master1.ino` 的 `otaStart()` 之
> `if (!WiFi.isConnected()) { … otaFail("offline"); }`、
> `if (ESP.getFreeHeap() < 70000) { … otaFail("low_heap"); }`、
> `if (!hoParseMacFromDeviceId(slaveId, mac)) { … otaFail("no_target"); }`、
> `Serial.println("⚠ [OTA] 指令未附 md5，只能保證 ESP-NOW 這一段的完整性；"`；
> `ho_master1.ino` 的 `otaFail()` 之 `Serial.printf("[OTA] 失敗：%s（目標 %s）\n", otaErrCode, otaTargetId)`；
> `ho_master1.ino` 的 `otaPhaseName()` 之 `case OTA_FAILED:         return "failed";`；
> `ho_master1.ino` 的 `masterStatusIntervalMs()` 之
> `return (otaPhase != OTA_IDLE) ? 5000UL : 10000UL;`；
> `ho_master1.ino` 的 `updateOtaSession()` 之
> `case OTA_SUCCESS: case OTA_FAILED: case OTA_STAGED_OK:` … `if (now - otaPhaseStart >= 30000) { otaPhase = OTA_IDLE; … }`；
> `libraries/HoEspNow/src/HoEspNowProtocol.cpp` 的 `hoParseMacFromDeviceId()`
> （`isxdigit()` 逐字元驗證）。

---

# D8 ★. `otaSetTarget()` 的字元過濾（`STATUS_OTA_MAX_BYTES = 128` 的前提 (3)）

> **為什麼這一項值得單獨跑**：`STATUS_OTA_MAX_BYTES = 128` 的四條前提裡，
> 第 (3) 條是「**`otaTargetId` 不含任何需要 JSON 逃逸的字元**」。
> 這一條若破，19 個 `"` 會逃逸成 38 bytes，`target` 那一項從 31 漲到 50、
> 整個 `ota` 物件從 119 漲到 **138**，**超過 128**，
> 而 `static_assert` 用的是常數、**抓不到**。
>
> 這條前提由 `otaSetTarget()` 的字元白名單保證，而 `otaTargetId` 的內容
> **Task 5 起可能整段來自遠端的 `update_slave`**。
> 靜態腳本只驗到「白名單那段字面沒被改掉」——**過濾實際會不會發生，沒人量過。**
> D1 量的是長度，**D8 量的是這一條**，兩者合起來才涵蓋到 128 的兩個主要前提。

**步驟**：對 `hoban/<masterId>/control` 發布（注意 `id` 裡那個 `\"` 是 JSON 逃逸的雙引號）：

```
update_slave:{"id":"hoban-aabbccddee\"f","url":"https://example.com/x.bin","version":"1.0.1"}
```

`id` 被 ArduinoJson 解回來是 `hoban-aabbccddee"f`（12 個十六進位位置裡有一個 `"`），
`hoParseMacFromDeviceId()` 的 `isxdigit()` 會拒絕它 → 走 `no_target`。

**預期（序列埠）**：
```
[OTA] 失敗：no_target（目標 hoban-aabbccddee?f）
```
**注意那個 `?`** —— 白名單是 `0-9`／`a-z`／`A-Z`／`-`／`_`／`.`／`:`，
`"` 不在裡面，被就地換成 `?`。

**預期（MQTT `hoban/<masterId>/status`）**：
- `ota.target` ＝ `"hoban-aabbccddee?f"`
- `ota.error` ＝ `"no_target"`，`ota.phase` ＝ `"failed"`
- **整份 JSON 仍然解析得動**（沒有多出來的引號把它切開）

**【失敗判定】**
- 序列埠印出的目標裡**帶著真正的 `"`**（例如 `目標 hoban-aabbccddee"f`）
  → 前提 (3) 在執行期不成立，**停工回報**。
- `hoban/<masterId>/status` 的 JSON **解析失敗**（MQTT Explorer 顯示成純文字）。
- `ota.target` 的長度超過 19 個字元（`otaTargetId` 是 `char[20]`，
  `snprintf` 應該把更長的輸入截斷）。

**【觀察項，不是失敗判定】**
- **順手多送一則超長 `id` 量截斷**：
  `update_slave:{"id":"hoban-0123456789abcdef0123456789","url":"https://example.com/x.bin"}`
  → 預期 `ota.target` 被截成 **19 個字元**。
  這一段對應前提 (4)（`otaTargetId` 的容量是 20 bytes），
  **本專案同樣沒有實機驗過**，請照實記錄實際字元數。
- MQTT Explorer 的 publish 面板對反斜線的處理各版本不同。
  若送出去的 `id` 沒有帶到 `"`，序列埠會直接是 `目標 hoban-aabbccddee` 之類的乾淨字串
  —— **那是工具沒送出你想送的東西，不是韌體 PASS**。請改用
  `mosquitto_pub -h <broker> -t hoban/<masterId>/control -m 'update_slave:{"id":"hoban-aabbccddee\"f","url":"https://example.com/x.bin"}'`
  確認。

> **它擋不住什麼**
>
> - **只驗 `update_slave` 這一條進入路徑。** 靜態腳本（方向 19）用的是
>   「`otaTargetId` 的寫入點必須全部落在 `otaSetTarget()`／`fakeOtaForCapacityTest()` 之內」
>   這張**函式名白名單** —— 原始碼註釋自己寫明「**先把 `otaTargetId`
>   指派給指標再寫**」這一類寫法它原理上驗不到。本項也驗不到。
> - **不驗最壞長度下的實際 bytes**：`?` 與 `"` 同樣是 1 byte 輸入，
>   本項證明的是「不會逃逸」，不是「119 這個數字算對了」。
> - 不驗 `error`（16 字元上界）與 `size`（10 位上界）那兩條前提。

> 對照：`ho_master1.ino` 的 `otaSetTarget()`，含
> `bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == ':';`
> 與 `if (!safe) otaTargetId[i] = '?';`；
> `ho_master1.ino` 的 `char     otaTargetId[20] = "";`；
> `ho_master1.ino` 的 `const size_t STATUS_OTA_MAX_BYTES = 128;` 上方那四條前提的註釋；
> `ho_master1.ino` 的 `otaFail()` 之 `Serial.printf("[OTA] 失敗：%s（目標 %s）\n", …)`；
> `libraries/HoEspNow/src/HoEspNowProtocol.cpp` 的 `hoParseMacFromDeviceId()`。

---

# D9. `test` 型號的協定測試：**執行 57 項，失敗 0 項**

> **總表規劃第 1 項。**
>
> ⚠ **兩個前提，請先確認再動手**：
> 1. **`test` 型號的 FQBN 是 `esp32:esp32:esp32c3`**（ESP32-C3）。
>    若你手上唯一那台 master 是 **WROOM**（`-Model master`），
>    **這一項今天做不到** —— 需要另一片 C3 板。
> 2. **這會覆蓋掉板子上現有的韌體。** 用 `master-c3` 那台板子做的話，
>    做完必須重新 `.\flash.ps1 -Model master-c3 -Upload` 才會變回 master。
>    **所以本項放在整份清單的最後。**

**步驟**：

```powershell
.\flash.ps1 -Model test -Upload
```
燒完打開序列埠（115200），按 RESET。測試在 `setup()` 裡跑完一輪就停。

**預期（序列埠）**：
```
═══ 齁控 ESP-NOW 協定測試 ═══
── struct 大小 ──
  [PASS] ...
（中略，共九組：struct 大小／CRC8／打包／解包／拒收異常封包／標頭竄改偵測／
  OTA 結構大小／指令歸因欄位／版本不符偵測／設備 ID）
──────────────────────
執行 57 項，失敗 0 項
ALL TESTS PASSED
```

**【失敗判定】**
- 最後一行不是 `ALL TESTS PASSED`（出現 `TESTS FAILED` ＝ FAIL）。
- 「失敗 N 項」的 N 不是 0。
- **「執行 N 項」的 N 不是 57** —— 但請先跑一次
  `python tools/check_doc_claims.py` 確認它印的
  `協定測試項數：原始碼 57、文件 ['57']（掃 2 份）` 裡那個「原始碼」數字。
  **那個數字是從 `ho_espnow_test.ino` 的 `check()` 呼叫次數直接數出來的，
  以它為準，不要以本檔寫死的 57 為準。**
  （本專案曾把這個數字手寫成 41 而實際是 57，照舊值驗收會把正確行為判成 FAIL。）

**【觀察項，不是失敗判定】**
- 前幾行被吃掉：測試 sketch 在 `Serial.begin()` 後 `delay(2000)` 等 USB CDC 列舉，
  但不同終端仍可能吃掉開頭。**按 RESET 重印一次即可。**
- 這是一份**純協定單元測試**，跑在單一顆 MCU 上、**不發任何 ESP-NOW 封包**，
  所以它 PASS 不代表現場的收發行為正確。

> **它擋不住什麼**：只驗 `libraries/HoEspNow` 的打包／解包／CRC／結構大小等
> **純函式**行為。射頻、時序、心跳、OTA 轉送**一項都不覆蓋**。

> 對照：`ho_espnow_test.ino` 的 `Serial.println("═══ 齁控 ESP-NOW 協定測試 ═══")`、
> `Serial.printf("執行 %d 項，失敗 %d 項\n", testsRun, testsFailed)`、
> `Serial.println("ALL TESTS PASSED")`、`Serial.println("TESTS FAILED")`、
> `Serial.printf("  [PASS] %s\n", name)`／`Serial.printf("  [FAIL] %s\n", name)`；
> `flash.ps1` 的 `'test' = @{ Dir = 'ho_espnow_test'; Fqbn = 'esp32:esp32:esp32c3:…' }`；
> `tools/check_doc_claims.py` 的方向 4（協定測試項數）。

---

# 被排除的候選項（**這些今天做不到，原因寫在這裡**）

> **這一節必須讀。** 下面這些項目「感覺上」只要 master 就能做
> （它們確實**不接觸任何 slave**），但**程式路徑上都要先通過
> 「名冊裡有一台真實且在線的 slave」這道門**。
> 分錯邊的代價是你按著跑、跑到一半才發現缺硬體。

`otaStart()` 的守衛順序（**由前到後**）是：

1. `otaSessionBusy()`
2. `!WiFi.isConnected()` → `offline`
3. `ESP.getFreeHeap() < 70000` → `low_heap`
4. `!hoParseMacFromDeviceId(slaveId, mac)` → `no_target`　← **D7-c／D8 停在這裡**
5. **`findSlave(mac) < 0` → `no_target`**　← **名冊裡必須有這台**
6. **`!slaves[idx].online` → `offline`**　← **而且必須在線**
7. `slaves[idx].relay != 0 && !force` → `relay_on`
8. `!otaParseUrlHost(url)` → `bad_url`
9. …（才進入 `OTA_RESOLVING` → DNS → 下載）

**第 5、6 條就是分界線。**

| 候選項 | 為什麼排除 |
|---|---|
| `otadl <n> <url>` 的**純下載段** | 要先過 `n < slaveCount`（序列埠分支）**以及** 第 5、6 條。名冊空的話停在 `[OTA] slave 編號超出範圍`（＝ **D5**）。用 `fakeslaves 20` 灌名冊也**沒用**：`fakeSlavesForCapacityTest()` 明文把 `online = false`（「false 比 true 多 1 byte，取最壞」），第 6 條會直接 `otaFail("offline")`。**沒有真實且在線的 slave 就下載不了。** |
| 下載失敗路徑：不存在的網域（`dns_fail`） | 在第 9 條之後，同上排除 |
| 下載失敗路徑：回傳 HTML 的網址（`bad_image`，首位元組不是 `0xE9`） | 同上排除 |
| 下載失敗路徑：`md5` 不符（`md5_mismatch`） | 同上排除 |
| `bad_url`（非 `https://` 的網址） | 排在第 8 條，**在**第 5、6 條之後，同上排除 |
| 對繼電器開著的 slave 送 `update_slave` 不帶 force（`relay_on` 早退） | 排在第 7 條，而且**需要一台繼電器正開著的真實 slave** |
| `busy`（轉送中再送一次 `update_slave`） | 需要一個真的跑得起來的工作階段 |
| 302 重定向是否正確跟隨（`setFollowRedirects` 的實際行為） | 在下載段裡，同上排除。**這個機制在本專案從未驗證過**，之後驗時一律列為觀察項 |
| 抹除暫存分區的實際耗時（`esp_partition_erase_range`） | 在下載段裡，同上排除。之後驗時一律列為觀察項 |
| 「轉送期間目標 slave 在 App 上顯示離線」的已知不符 | 需要一台真實 slave 才能進到 `updating` 狀態，本清單範圍外。**簡述見下** |

### 附帶說明：「轉送期間目標 slave 在 App 上顯示離線」是**已知不符**，不是韌體缺陷

本清單跑不到這一項（需要 slave），但先寫在這裡，免得之後判成 FAIL：

- **(a) MQTT 層現在就該對**：用訂閱工具看 `hoban/<目標 ID>/status`，
  `status` 應該是 `"updating"`、且帶 `ota_progress`。**這一段判 PASS／FAIL。**
- **(b) App 畫面會顯示「離線」**：App 的解析是
  `data['status'] == 'online' ? online : offline`，`"updating"` 會落到 **offline**。
  `DeviceStatus.updating` 這個列舉值確實存在，但它的**唯一產生點是舊的
  `updating:` 純字串格式**。**這是已知的 App 端缺口，記成「已知不符」，
  不是韌體回歸失敗。**
- **不要在韌體側改回 `"online"`**：一台正在重開機、繼電器已歸零的設備
  顯示成正常在線，那是**誤綠**，比顯示離線更危險。

---

# 這份清單擋不住什麼（整份的自我限制）

**寫在這裡是因為本專案曾五次「宣稱一道其實不存在／抓不到目標的防線」。**

1. **它不覆蓋任何一段真正的 OTA 資料路徑。**
   下載、暫存、抹除、BEGIN／區塊／ACK／END、校驗、重啟、版本回檢 ——
   **一段都沒有**。全部卡在「需要真實且在線的 slave」那道門後面。
   本清單全 PASS **不代表 OTA 轉送能用**。
2. **它不覆蓋任何時序保證。** 「DNS 最壞阻塞 15 秒 < slave 的 30 秒失聯門檻」、
   「每 40 個區塊印一行」、「轉送 30~90 秒」——這些全部是靜態推演，本清單一項都驗不到。
3. **`> 對照：` 這套做法擋得住字串不符，擋不住三件事**：
   （a）**列舉值與數值抄錯**（本專案曾把 `HO_CMD_OFF` 寫成 2，實際是 0）；
   （b）**順序與時序**（字串對，不代表實測時會照這個順序、在這個時間內印出來）；
   （c）**「這一行真的會被走到」** —— 例如 D7-c 的 `⚠ [OTA] 指令未附 md5…`
   字串存在、但那條路徑走不到它，本檔已特別標註。
4. **本檔沒有任何機械守衛。**
   `tools/check_doc_claims.py` 的 `DOC_FILES` 是一份**寫死的五個檔名的清單**
   （`phase1`／`phase2b`／`phase4-flag-day-upgrade`／兩份 readme），
   **本檔不在裡面**。所以本檔的判準字串、被禁用的舊值（`statusBuf 3072`、
   `mqtt buffer 3328`、`maxEntries 21`…）、行號回歸，**都不會被腳本檢查到**。
   該腳本目前是**凍結狀態**（使用者裁定：三輪長到 615 行而韌體只動 175 行），
   所以**沒有把本檔加進去**。**動過 `.ino` 之後，本檔的對照字串要人工重讀一次。**
5. **D0 的抹除行為、D8 的截斷長度、D7-c 的 `low_heap` 觸發機率**
   都是**純推演、沒有實測來源**，已各自標成觀察項。
6. **用 `master-c3` 執行時，全部 OTA 項目（D1、D5~D8）都是首次驗證** ——
   C3 上的 OTA 路徑至今零實機覆蓋。異常不能假定是操作錯誤。

---

# 記錄表（跑完請填，之後的完整回歸清單會用到）

| 項目 | 結果 | 實測記錄 |
|---|---|---|
| D0 | ☐PASS ☐FAIL | 板子型號＝`master` / `master-c3`；燒完 WiFi 設定＝保留／被抹 |
| **D1 ★** | ☐PASS ☐FAIL | `<N>` ＝ ______ bytes；`otastat` 的 `階段=` ＝ ______；補正後 ＝ ______ ／ 3584；SSID 長度 ＝ ______；自訂 MQTT 伺服器＝有／無 |
| D2 | ☐PASS ☐FAIL | `ota` 五欄實際值；retain 是否已清 ☐ |
| D3 | ☐PASS ☐FAIL | 閒置那一行原文 |
| D4 | ☐PASS ☐FAIL | 四條 OTA 指令 ☐；兩段 retain 警告 ☐ |
| D5 | ☐PASS ☐FAIL | |
| D6 | ☐PASS ☐FAIL | |
| D7 | ☐PASS ☐FAIL | a／b／c 各自的 `ota.error`；free heap ＝ ______ |
| **D8 ★** | ☐PASS ☐FAIL | `ota.target` 實際字串 ＝ ______；超長 id 截斷後字元數 ＝ ______ |
| D9 | ☐PASS ☐FAIL ☐N/A（無 C3 板） | 「執行 ___ 項，失敗 ___ 項」 |
