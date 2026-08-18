# Phase 2b 回歸清單（`ho_master1` 代發／代訂閱／群組指令）

> **警告：本清單尚未在任何實體硬體上執行過任何一項。**
>
> 不只本清單 —— **整個 Phase 2b（Task 1~5、7）至今零實機回歸**。
> 所有結論都是**靜態推演**，唯一真正跑過硬體的只有 Task 2 的
> `fakeslaves 20` + `jsonsize`（實測 2100 bytes）。
>
> 這一點必須認真看待：Phase 2b 的 review 過程中出現過一次
> **「靜態推演錯到連『新增的測試抓不抓得到目標缺陷』都推錯」**
> （複審設計了一項專門抓 N1 的測試 8e，事後才發現它抓不到）。
> 所以請把下面每一項都當成「待驗證的假說」，不是「已知會通過的步驟」。

> **本清單已於 Phase 4 Task 1 收尾時移除全部 117 處行號對照（`ho_master1.ino:3218` 這種）。**
>
> 理由不是嫌麻煩，是那些行號**當時已經全錯**：Phase 4 Task 1 動了 `ho_master1.ino`
> 之後，31 處可判定的對照裡**錯了 30 處**（`:3218` 實際已是 3540、`:1892` 已是 2159）。
> 而規則檔 `.claude/rules/claim-what-it-does-not-block.md` 早就寫著
> 「**判準以字串為準，不以行號為準；行號會漂移**」——留著只會週期性地重演。
>
> 現在一律寫成「`<檔名>` 的 `<那一行的字串或函式名>`」。字串可以用 grep 逐字回讀驗證，
> 行號不行。`tools/check_doc_claims.py` 的第 6 個方向會擋住行號被重新加回來。

## 這份清單怎麼寫的（維護時請照做）

本專案有一類缺陷已經出現**四次**：**回歸清單的驗收標準與程式碼矛盾，
導致實測者把正確行為判成 FAIL。**（最近一次是 Task 7 自己抓到的：
`phase1-regression-checklist.md` 的 8e 把 `alloff` 的指令碼寫成 `2`，
實際 `HO_CMD_OFF = 0`。）

因此：

1. 每一條「預期序列埠輸出」都**逐字對照過**程式碼的 `Serial.print*` 格式字串，
   每一項結尾用 `> 對照：` 一行寫出對照位置。

   > **這道做法擋得住什麼、擋不住什麼**（本節自己也要守這條規則）
   >
   > - **擋得住**：判準字串與程式碼不符（含全形 `／`、`（）`、`⚠`）。
   > - **擋不住列舉值與數值**。行號對、字串對，**不代表值抄對了** ——
   >   本清單第一版就把 `alloff` 的指令碼寫成 `2`（實際 `HO_CMD_OFF = 0`），
   >   readme 的 JSON 範例還寫過一個**根本不存在的 `"cmd": 3`**。
   >   **列舉值一律回去讀 enum 定義，不要憑名稱推順序。**
   > - **擋不住順序與時序**。字串比對驗不到「實測時會不會照這個順序、
   >   在這個時間內印出來」。凡涉及時序的都已標成觀察項並註明「靜態推演、無上界保證」。
   > - **`> 對照：` 的行號會隨程式碼漂移，判準一律以「字串」為準、不以行號為準。**
   >   本清單的行號就曾因為「commit 前又多補了一行註釋、補完沒重跑腳本」
   >   而整批差 1 行。**動過 `.ino` 之後務必重跑一次回讀腳本再交付。**
2. **凡是「不應該印出某訊息」這類否定式判準一律禁止**，除非能明確指出程式碼
   中沒有任何路徑會印出它。一律改寫成正面判準。
3. 每一項分成 **【失敗判定】** 與 **【觀察項】**。機制未經實機驗證的
   （`WiFi.begin()` 的掃描行為、廣播命中率、`GROUP_ACK_WAIT_MS` 夠不夠長）
   **一律列為觀察項，不得列為失敗判定**。
4. **每新增一道守衛或驗收項，要把「它擋不住什麼」與「它擋住什麼」寫在一起。**
   （這條準則的由來：本專案曾五次「宣稱一道其實不存在／抓不到目標的防線」。）

## 標記說明

| 標記 | 意思 |
|---|---|
| 🖥 | **桌面即可** —— 板子放在桌上、近距離、不需要拉遠或做訊號屏蔽 |
| 📡 | 需要**訊號邊界**或特殊佈置（拉遠到 rssi −85 dBm 上下、金屬罐體半屏蔽） |
| ⛔ | **今天做不到**，缺什麼寫在該項裡 |

> **想先跑一批的話，就跑 🖥 的那些。** Phase 2b 的所有主要驗收項都是 🖥；
> 唯一需要 📡 的是 `phase1-regression-checklist.md` 的 **8a**（不對稱可達性）。

---

## 前置需求

- 1 台已燒錄 `ho_master1`（`master` 或 `master-c3`，任一即可），序列埠 115200 開著
- **至少 2 台**已燒錄 `ho_slave1`（第 9~14 項要看「一台成功、一台不成功」的區別）
- master 已完成 BLE 配網、已連上 WiFi 與某台 MQTT broker
- MQTT Explorer（或 `mosquitto_sub`）訂閱 `hoban/#`，**要能看到 retain 訊息**
- 一個能對任意 topic 發布純文字的工具（MQTT Explorer 的 publish 面板即可）

**本清單全部用「MQTT 直接送純文字指令」驗收，不依賴 App。**
App 端的行為（讀 `grp`／`group.noack`、樹狀 UI）是 Phase 3，不在本清單範圍。

**指令碼對照表**（下面每一項的 `<cmd>` 都用這組值，**不要記錯**）：

| 名稱 | 值 | 哪些指令會送出 |
|---|---|---|
| `HO_CMD_OFF` | **0** | `ALL:OFF`、序列埠 `alloff`、slave topic 的 `OFF`、序列埠 `off <n>` |
| `HO_CMD_ON` | **1** | 序列埠 `allon`、序列埠 `on <n>`（**只有序列埠**） |
| `HO_CMD_PULSE` | **2** | `ALL:ON`、slave topic 的 `ON`、序列埠 `allpulse`／`pulse <n>` |

> 對照：`libraries/HoEspNow/src/HoEspNowProtocol.h:25-30` 的
> `enum HoRelayCmd : uint8_t { HO_CMD_OFF = 0, HO_CMD_ON = 1, HO_CMD_PULSE = 2, };`

---

## 1. 🖥 master 狀態每 10 秒一則，含 `server`、`free_heap`、`slaves` 陣列

**步驟**：master 連上 broker 後不做任何事，用 MQTT Explorer 看
`hoban/<masterId>/status`。

**預期（MQTT）**：每約 10 秒更新一次，且是 **retain**。欄位至少要有
`device_id`／`status`／`version`／`model`／**`server`**／`timestamp`／
`wifi{connected,ssid,rssi,ip}`／`device{relay,has_relay,pairing,slave_count,channel,long_range,`**`free_heap`**`}`／
**`slaves`**（陣列，每台一筆 `{id,relay,online,rssi,version}`）。

**預期（序列埠）**：連上時印
```
[MQTT] 已連線 <伺服器位址>
[MQTT] 已訂閱 hoban/<masterId>/control
[代理] 已訂閱 hoban/<slave0Id>/control
```
（`[代理] 已訂閱 …` 每台一行，**由排程器每輪 `loop()` 一格逐條印出**，
不是一口氣印完；健康網路下 21 格在數十毫秒內走完，肉眼看不出差別。）

**【失敗判定】**
- 出現 `⚠ [MQTT] 訂閱失敗 hoban/<masterId>/control（master 自己的 control topic，此時 App 的指令收不到）`
  → **一律 FAIL**。**不要因為前一行 `[MQTT] 已連線 …` 成功就判 PASS**
  （這兩件事已刻意拆成兩行，就是為了讓訂閱失敗看得見）。
- `slaves` 陣列缺少任何一台已配對的 slave。
- 出現 `⚠ [MQTT] 放棄發布 hoban/<masterId>/status：JSON 需要 <n> bytes，statusBuf 只有 3584`
  或 `⚠ [MQTT] 放棄發布 …：整包需要 <n> bytes，mqtt buffer 只有 3840`。

**【觀察項，不是失敗判定】**
- 偶爾看到 `[MQTT] hoban/<某個 topic>/status 讓位給下一輪（本輪 publish 名額已用掉）`
  —— 這是**設計行為**（每輪 `loop()` 只做一次阻塞式 socket 寫入），不是缺陷。
- `slaves[i].version` 是 `"0.0.0"` —— 代表那台**還沒被輪詢到過**（`fw*` 初始值 0），
  等一輪（約 15 秒）後應變成 `"1.0.0"`。

> 對照：`ho_master1.ino` 的 `publishStatus()`；`ho_master1.ino` 的 `buildStatusDoc()`
> （`doc["server"]`、`dev["free_heap"]`）；`ho_master1.ino` 的 `if (now - lastStatusPub > 10000)`；
> `ho_master1.ino` `Serial.printf("[MQTT] 已連線 %s\n", cfg.server)`；
> `ho_master1.ino`／`ho_master1.ino` 的 `[MQTT] 已訂閱 %s`／`⚠ [MQTT] 訂閱失敗 %s（master 自己的 control topic，此時 App 的指令收不到）`；
> `ho_master1.ino` 的 `[代理] 已訂閱 %s`；`ho_master1.ino` 的 `[MQTT] %s 讓位給下一輪（本輪 publish 名額已用掉）`；
> `ho_master1.ino`／`ho_master1.ino` 的兩行放棄發布；`ho_master1.ino` 的 `formatSlaveVersion()`。

---

## 2. 🖥 **容量驗證**：`fakeslaves 20` → `jsonsize`（**只要 1 台 master，連 slave 都不用**）

> ## ⚠⚠ 動手前先讀完這一段：`fakeslaves` 會在公用 broker 上留下永久垃圾
>
> **只要 master 當下已連上 broker，`fakeslaves 20` 之後約 0.75 秒就會開始
> 把 20 台假 slave 的狀態以 `retain=true` 發上去**（`slaveStatusScheduler()`
> 的例行輪播不看 `dirty`，一輪 15 秒把 20 台全發完）。
> **這不需要你做第 3 項，光下 `fakeslaves` 就會發生。**
>
> 後果分兩種，請分開理解（別把兩者混為一談）：
>
> | 受影響的 topic | 會不會自己復原 |
> |---|---|
> | `hoban/<masterId>/status`（會宣告 `slave_count: 20` ＋ 20 筆假 `slaves`） | **會**。重開機後下一則真實 status 就覆蓋掉了 |
> | `hoban/hoban-aabbccddee00/status` … `…ee13/status`（20 條） | **不會，永久留著**。重開機後名冊沒有它們，master 再也不會發那些 topic，retained 訊息就一直掛在 broker 上 |
>
> 而**預設伺服器清單五台裡有四台是公用的**
> （`mqttgo.io`／`mqtt.eclipseprojects.io`／`broker.emqx.io`／`broker.hivemq.com`），
> 留下的垃圾別人也看得到、也清不掉（除了你自己去清）。
>
> **這會不會直接讓使用者的 App 冒出 20 台幽靈設備？照實說：不會自動變成
> 「我的設備」清單上的一列，但有一條路徑**今天就通、不是 Phase 3 才有**。**
> App 是**逐台訂閱** `hoban/<device.mqttTopicId>/status`
> （`lib/services/multi_mqtt_service.dart`），不是萬用字元，所以那 20 條
> retained **不會自己**變成設備清單上的一列——這句擋得住的就只有這件事。
>
> **它擋不住的**：`device_detail_page.dart` **現行版本**只要打開任何一台
> master 的詳情頁，就會直接渲染出「子設備（N 台）」清單（收到 master status 後
> `SlaveStatus.listFromMqtt()` 存進 `_slaveStatuses`，**不等 Phase 3 樹狀 UI**；
> 行號會隨 App 端開發持續漂移，判準以函式名稱為準，不要照抄行號）；
> 每一列還帶一顆「加入」鈕（`_buildSlaveTile()` → `_handleAddSlave()`），
> 按下去會把假設備**寫進 Firestore**。之後 App 就會真的去訂
> `hoban/hoban-aabbccddee0x/status`，讀到那 20 則永久 retained，
> 變成一台永遠在線、怎麼控制都沒反應的**持久幽靈設備**。
> **所以：`fakeslaves` 期間請不要打開該 master 的詳情頁，更不要按「加入」。**
>
> ### 三個做法，挑一個（**按安全性排序**）
>
> 1. **【最安全】在還沒連上 MQTT 時做第 2 項。**
>    `jsonsize` 走 `buildStatusDoc()` ＋ `measureJson()`，**完全不碰 socket**，
>    不需要連線。代價是 `mqtt buffer` 那個數字會是 `256`（見下方預期輸出的兩種變體）。
>    **怎麼進入未連線狀態**：用一台**還沒配網**的 master 最乾淨；
>    若板子已配網，**光靠「重開機」做不到**——已配網的板子幾秒內就會自動連上 broker，
>    要在它連上之前搶下 `fakeslaves`／`jsonsize`，或乾脆先把 AP／WiFi 關掉再開機。
>    **第 3 項需要 broker，做不到——請改走做法 2 或 3。**
> 2. **【推薦】把 master 配網到一台私有 broker**（筆電上的 mosquitto、或自架的
>    `broker.hoban.tw` 以外的任何私有位址），第 2、3 項都在那台上做。
>    垃圾只留在自己的 broker 上，清不清都無所謂。
> 3. **在公用 broker 上做，但做完必須自己清乾淨。** 清除步驟見第 3 項結尾，
>    **不要略過**。

**步驟**：
1. **先重新開機**（確保這次開機還沒下過 `fakeslaves`）。
2. 序列埠輸入 `fakeslaves 20`。
3. 序列埠輸入 `jsonsize`。

**預期序列埠輸出（第 2 步）**：
```
[測試] 名冊已灌成 20 台假 slave（未寫入 NVS，重開機即消失）
⚠ [測試] 重開機前請勿執行 pair／unpair：名冊混有假 MAC，一旦觸發存檔就會寫進 NVS 汙染真實名冊（已由 saveSlaves() 擋下）
```

**預期序列埠輸出（第 3 步）—— `mqtt buffer` 有兩種合法值，看你本次開機有沒有嘗試過 MQTT 連線**：

```
[測試] 狀態 JSON 實際 <N> bytes／statusBuf 3584／mqtt buffer 3840（名冊 20 台）
```
或
```
[測試] 狀態 JSON 實際 <N> bytes／statusBuf 3584／mqtt buffer 256（名冊 20 台）
```

**`256` 不是失敗**：`setBufferSize(MQTT_BUFFER_SIZE)` **只在
`quickConnectToIndex()`／`quickConnectCustom()` 內、真的要連線時才被呼叫**，
本次開機從未嘗試連線（還在 BLE 配網模式、或 WiFi 連不上）時，
buffer 會停在 `PubSubClient` 建構子給的 `MQTT_MAX_PACKET_SIZE` ＝ **256**。

**【失敗判定】**
- `<N>` **不小於 3584** → 容量防線已被破壞。
- `statusBuf` 不是 `3584`（Phase 4 Task 1 由 3072 放大）。
- 本次開機**已經連上過** broker（序列埠有 `[MQTT] 已連線 …`），
  但 `mqtt buffer` 仍不是 `3840`（Phase 4 Task 1 由 3328 放大）
  —— 此時序列埠上方應同時有
  `⚠ [MQTT] setBufferSize(3840) 失敗，buffer 仍為 <舊值>…`，代表 realloc 失敗。
- 出現 `⚠ [MQTT] slaves 陣列被截斷：名冊 20 台，只放得下 <n> 台`
  —— 執行期上限 `maxEntries` 算出來是 **25**，20 台不該被截斷。

**【觀察項】**
- Task 2 在另一台板子上實測到的是 **2100 bytes**。你這台可能不同：
  `<N>` 受 **SSID 長度**與**有沒有設自訂 MQTT 伺服器**影響（那台測試板基礎欄位
  只吃 310 bytes，遠低於 728 的預算）。**只要 < 3584 就是 PASS，不必等於 2100。**
  **而且 2100 是 Phase 2b 韌體的數字** —— Phase 4 Task 1 每筆 slave 多了 `"exe"`
  （20 台約 +160 bytes），實測值會比 2100 大，這是預期的。
- `[群組]` 相關欄位若之前下過群組指令會多出 `"group"` 物件，`<N>` 會再大幾十 bytes，
  正常。

> **本項擋得住什麼、擋不住什麼**
>
> - **擋得住**：20 台的 JSON 在**你這台的實際設定下**放不下 `statusBuf`。
> - **擋不住悲觀情境。** `<N>` 量的是**當下的**基礎欄位大小，而
>   `STATUS_BASE_MAX_BYTES = 728`（480＋120＋128）是留給**最壞情況**的：
>   63 字元的自訂 MQTT 伺服器位址（`"server"` 欄位最壞 75 bytes）＋ 長 SSID。
>   **短 SSID、沒設自訂伺服器的板子量出來會樂觀好幾百 bytes** ——
>   PASS 只代表「這台這個設定放得下」，**不代表悲觀上界成立**。
> - 悲觀上界是靠 `static_assert` 在**編譯期**保證的，不是靠本項。
>   要用實測逼近悲觀值，得配一個接近 63 字元的自訂伺服器位址與長 SSID 再量一次
>   —— **本清單沒有要求做這件事，所以那條路徑至今零覆蓋。**

**收尾**：`fakeslaves` 期間不要下 `pair`／`unpair`（韌體會擋，但別去試）。
**先不要重開機** —— 第 3 項要接著在同一個狀態下做（**重開機的時機在第 3 項結尾**）。
（**走做法 1 的人**：第 3 項需要 broker、做法 1 做不到，所以第 2 項做完**直接重開機即可**，
沒有殘留需要清，不用管上面這句「先不要重開機」。）

> 對照：`ho_master1.ino` 的
> `Serial.printf("[測試] 名冊已灌成 %d 台假 slave（未寫入 NVS，重開機即消失）\n", n)`；
> `ho_master1.ino` 的 `⚠ [測試] 重開機前請勿執行 pair／unpair：…`；
> `ho_master1.ino` 的 `Serial.printf("[測試] 狀態 JSON 實際 %u bytes／statusBuf %u／mqtt buffer %u（名冊 %d 台）\n", …)`；
> 常數宣告區的 `STATUS_BUF_SIZE = 3584`／`MQTT_BUFFER_SIZE = 3840`／
> `STATUS_BASE_MAX_BYTES`（＝ `WITHOUT_GROUP_OTA` 480 ＋ `GROUP` 120 ＋ `OTA` 128）；
> `ho_master1.ino` 的 `⚠ [MQTT] slaves 陣列被截斷：名冊 %d 台，只放得下 %d 台`。

---

## 3. 🖥 20 台假資料下實際發布一次 `status`，MQTT 收到的 JSON **語法完整、20 筆齊全**

> 這是「靜默截斷」的**正面驗證** —— 第 2 項量的是 `measureJson()`（發布前的預估），
> 這一項看的是 broker 上真的收到什麼。
>
> **前置：本項一定要有 broker，所以第 2 項的「做法 1（不連線）」在這裡不適用。**
> 請走**做法 2（私有 broker，推薦）**或**做法 3（公用 broker ＋ 做完自己清）**。
> 走做法 3 的話，**做完務必執行本項結尾的清除步驟**。

**步驟**：接續第 2 項（名冊仍是 20 台假 slave，**第 2 項結束後不要重開機**），
對 `hoban/<masterId>/control` 送 `status`。

**預期（序列埠）**：`[MQTT] 收到指令: status`

**預期（MQTT Explorer）**：`hoban/<masterId>/status` 更新，且
- JSON **能被解析**（MQTT Explorer 會用樹狀顯示；解析失敗它會顯示成純文字）
- `device.slave_count` 是 `20`
- `slaves` 陣列**剛好 20 筆**，`id` 依序是
  `hoban-aabbccddee00`、`hoban-aabbccddee01`、…、`hoban-aabbccddee13`
  （**十六進位，第 20 台是 `13` 不是 `19`**；`fakeslaves` 把 MAC 尾碼設成迴圈索引 `i`，
  `hoFormatDeviceId()` 用 `%02x` 格式化）
- 每筆的 `relay` 是 `1`、`online` 是 `false`、`rssi` 是 `-100`、`version` 是 `"255.255.255"`
  （`fakeslaves` 刻意灌最壞值）
- **Phase 4 Task 1 起**：若開機以來下過群組指令，每筆還會多 `"grp"` 與 `"exe"`；
  `fakeslaves` 的假 MAC 不在任何群組快照裡，所以**通常兩個都不帶** —— 那是正常的
- **沒有** `slaves_truncated` 這個 key。
  （**這是本清單兩條否定式判準之一，而它有明確依據**：
  `slaves_truncated` 只在 `shown < slaveCount` 時才被寫入，而
  `shown = min(slaveCount, maxEntries) = min(20, 25) = 20 = slaveCount`，
  程式上沒有任何路徑會在 20 台時寫入它。另一條見下方「收尾」第 3 步。）

**【失敗判定】**
- JSON 語法不完整（尾端被切掉、少一個 `]` 或 `}`）。
- `slaves` 不足 20 筆，或出現 `slaves_truncated`。

**【觀察項】**
- master 自己的繼電器狀態、`wifi.rssi` 等欄位不影響本項。
- 假 slave 沒有註冊 ESP-NOW peer，所以**序列埠會一直刷
  `[ESP-NOW] esp_now_send 失敗: <碼>`**（`pollNextSlave()` 每輪對假 MAC 送
  `STATE_REQ`）。**這是預期的，不是失敗。**

> **本項擋得住什麼、擋不住什麼**
>
> - **擋得住**：發布路徑上的靜默截斷（broker 上收到的是完整 20 筆、不是半截）。
> - **擋不住悲觀情境**（理由與第 2 項完全相同：`<N>` 是這台這個設定的實測值，
>   不是 `STATUS_BASE_MAX_BYTES = 728` 對應的最壞情況）。
> - **擋不住 21 台以上**：`maxEntries` 是 25，21~25 台仍不會截斷；
>   而名冊硬上限 `HO_ESPNOW_MAX_SLAVES` 是 20，所以
>   **`slaves_truncated` 這條執行期路徑至今零覆蓋，本項也不覆蓋它。**

### ★ 收尾（走做法 3 ＝ 公用 broker 的人**一定要做**）

1. **先重新開機 master**（恢復真實名冊；同時它會用真實名冊重壓一則
   `hoban/<masterId>/status`，把「20 台假 slave」那則覆蓋掉）。
2. **手動刪掉那 20 則永遠不會被覆蓋的 retained 訊息**：
   `hoban/hoban-aabbccddee00/status` … `hoban/hoban-aabbccddee13/status`
   （**十六進位，00~13 共 20 條**）。
   刪法是**對該 topic 發布「零長度 payload ＋ retain=true」**（MQTT 規範裡這代表
   清除該 topic 的保留訊息）：
   - MQTT Explorer：選中該 topic → `Delete retained message`
   - CLI：`mosquitto_pub -h <broker> -t hoban/hoban-aabbccddee00/status -r -n`
     （20 條各跑一次）
3. **確認清乾淨**：重新連上 broker、訂閱 `hoban/#`，
   應該只剩真實設備的 topic，沒有任何 `hoban-aabbccddee??`
   （**否定式判準，依據見下方「韌體幫不上忙」**：重開機後名冊裡沒有那些假 MAC，
   `publishSlaveStatus()` 再也不會碰那 20 條 topic）。

> **韌體幫不上忙**：重開機後名冊裡沒有那些假 MAC，`publishSlaveStatus()`
> 再也不會碰那 20 條 topic，所以**沒有任何韌體路徑能自動清掉它們**。
> 這就是為什麼要人工清。

> 對照：`ho_master1.ino` 的 `fakeSlavesForCapacityTest()`
> （`mac[5] = (uint8_t)i`、`online = false`、`rssi = -100`、`relay = 1`、`fw* = 255`）；
> `libraries/HoEspNow/src/HoEspNowProtocol.cpp:69-72` 的
> `snprintf(out, 20, "hoban-%02x%02x%02x%02x%02x%02x", …)`；
> `ho_master1.ino` 的 `appendSlavesArray()`；`ho_master1.ino` 的 `[MQTT] 收到指令: %s`；
> `ho_master1.ino` 的 `[ESP-NOW] esp_now_send 失敗: %d`。

---

## 4. 🖥 對 `hoban/<slaveId>/control` 送 `ON` → 那台 slave 點動 2 秒

> **這是 Phase 2 的主要驗收條件**：slave 在 App 眼裡是一台普通設備。

**步驟**：配對 2 台真 slave（`list` 確認在線），對
`hoban/<slave0Id>/control` 送純文字 `ON`。**注意 topic 用的是 slave 自己的 MAC。**

**預期（master 序列埠）**：
```
[代理] hoban-<slave0Mac> 收到指令: ON
[控制] 送指令 2 給 hoban-<slave0Mac>
```
（`2` ＝ `HO_CMD_PULSE`。**代理的 `ON` 刻意送 PULSE 不送 `HO_CMD_ON`**，
語義與 App 對一般 hoRelay 設備的「開門」一致。）

**預期（slave0 序列埠）**：
```
[繼電器] 點動 2000 ms
[狀態] 已回報 relay=1
```
繼電器立即動作，約 2 秒後：
```
[繼電器] 點動結束，已關閉
[狀態] 已回報 relay=0
```

**【失敗判定】**
- **slave1（另一台）也動作** → 指令送錯台，這是「開錯門」，最嚴重的失敗型態。
- master 印 `[代理] hoban-… 不支援的指令: ON`（大小寫必須完全相符，指令是 `ON` 不是 `on`）。
- master 印 `[MQTT] 指令的目標不在名冊上，忽略: <topic>` 而那台明明在 `list` 上。
- slave 繼電器持續通電不自動關閉（代理的 `ON` 必須是點動不是持續開啟）。

**【觀察項】**
- 送 `hoban/<slave0Id>/control` 的 `OFF` 會印 `[控制] 送指令 0 給 …`（`HO_CMD_OFF = 0`），
  slave 印 `[繼電器] 關閉`。
- 送任何其他字串（例如 `reset`、`ALL:ON`）會印
  `[代理] hoban-… 不支援的指令: <字串>` 且**不做任何動作** ——
  **B 區不支援 A 區的指令，這是設計，不是缺陷**。

> **本項擋得住什麼、擋不住什麼**
>
> - **擋得住**：轉發路徑整條通（topic 解析 → 查名冊 → ESP-NOW 單播 → slave 執行），
>   以及「2 台的情況下沒有送錯台」。
> - **擋不住 20 台規模下的索引錯位。** 本階段最嚴重的缺陷型態是
>   「`slaves[]` 被 WiFi task 前移 → 索引指到別台 → **開錯門**」，
>   而那需要「**指令轉發與另一台的解除配對同時發生**」才會觸發。
>   2 台、手動一次送一條指令的測法**碰不到那個窗口**。
> - 現行程式是靠**結構**擋掉它的（`handleSlaveCommand()` 收 MAC 不收索引、
>   `sendCmdToSlaveMac()` 用呼叫方自己那份 MAC 副本），
>   **不是靠這一項擋的**。本項 PASS 不等於那道結構防線被驗證過。

> 對照：`ho_master1.ino` 的 `Serial.printf("[代理] %s 收到指令: %s\n", id, message.c_str())`；
> `ho_master1.ino` 的 `ON` → `sendCmdToSlaveMac(mac, HO_CMD_PULSE, 2000)`；
> `ho_master1.ino` 的 `Serial.printf("[控制] 送指令 %u 給 %s\n", (uint8_t)cmd, id)`；
> `ho_master1.ino` 的 `[代理] %s 不支援的指令: %s`；`ho_master1.ino` 的 `[MQTT] 指令的目標不在名冊上，忽略: %s`；
> `ho_slave1.ino` 的 `[繼電器] 點動 %u ms`、`ho_slave1.ino` 的 `[狀態] 已回報 relay=%u`、
> `ho_slave1.ino` 的 `[繼電器] 點動結束，已關閉`、`ho_slave1.ino` 的 `[繼電器] 關閉`。

---

## 5. 🖥 承上，`hoban/<slaveId>/status` 幾乎立刻出現 relay 翻轉（dirty 插隊代發）

**步驟**：在 MQTT Explorer 盯著 `hoban/<slave0Id>/status`，重複第 4 項的 `ON`。

**預期**：`device.relay` 先變 `1`，約 2 秒後變回 `0`，**兩次都不必等一整輪
（約 15 秒）的例行輪播**。

機制：slave 執行完指令會 `sendState()` → master 的 `HO_PKT_STATE` 分支發現
`relay` 變了就設 `dirty` → `slaveStatusScheduler()` 優先處理 dirty 的那台。

**【失敗判定】只有一條**
- **`hoban/<slave0Id>/status` 從頭到尾完全沒有更新**（`timestamp` 也沒動），
  而 slave 序列埠明明印過 `[狀態] 已回報 relay=1` ——
  代表 dirty 插隊代發整條路壞了。

（**注意這裡刻意只留一條。** 上一版把「沒看到 `relay:1`」也列成 FAIL、
括號裡又寫「漏掉中間狀態不算 FAIL」——**立了判準又當場推翻，正是
「把正確行為判成 FAIL」的形狀**。漏掉 `relay:1` 是合法的取樣落差，
已整條移到觀察項。）

**【觀察項，不是失敗判定】**
- **看不看得到 `relay:1` 這個中間值，不是判準。** 若 MQTT publish 恰好被延後
  超過 2 秒，master 代發時 slave 早已點動結束，發出來的就直接是 `relay:0`。
  這是取樣落差，不是缺陷。
- **「1 秒內」不是保證。** 代發受兩道限制：任兩次代發至少間隔
  `SLAVE_STATUS_MIN_GAP_MS`（**250ms**），且每輪 `loop()` 只有一次阻塞式 socket
  寫入名額（`publishStatus()` 若剛好在同一輪先發了，這台就讓位給下一輪）。
  健康網路下典型是數百毫秒，**但沒有上界保證**。
- 判定一律**以序列埠的 `[狀態] 已回報 relay=1` / `relay=0` 兩行為準**，
  MQTT 那邊看到幾個中間值都可以。
- **本項的時序描述全部是靜態推演**（讀 `loop()` 的呼叫順序與兩個常數推出來的），
  **沒有實機量測過**。若實測差很多，先懷疑推演而不是韌體。

> 對照：`ho_master1.ino` 的 `if (changed) slaves[idx].dirty = true;`
> （`changed` 的條件含 `slaves[idx].relay != st.relay`，見 `ho_master1.ino`）；
> `ho_master1.ino` 的 `slaveStatusScheduler()` dirty 優先迴圈；
> `ho_master1.ino` 的 `const unsigned long SLAVE_STATUS_MIN_GAP_MS = 250;`；
> `ho_slave1.ino` 的 `sendState();`（`HO_PKT_CMD` 分支結尾）。

---

## 6. 🖥 slave 拔電 30~45 秒後，`hoban/<slaveId>/status` 變成 `offline`

**步驟**：拔掉 slave0 電源，等 **45 秒**（判離線門檻 30 秒 ＋ 檢查週期 15 秒，
留餘裕），觀察 master 序列埠與 MQTT。

**預期（master 序列埠）**：
```
[離線] hoban-<slave0Mac>（超過 30 秒沒回應即判離線）
```

**預期（MQTT，`hoban/<slave0Id>/status`）**：
```
"status": "offline",  "wifi": { "connected": false, … }
```

**【失敗判定】**
- 超過 60 秒仍停在 `"status":"online"`。
- 序列埠印了 `[離線]` 但 MQTT 一直沒更新（代發沒被 dirty 觸發）。

**【觀察項】**
- 判離線的檢查是 **每 15 秒**跑一次（`updateSlaveOnlineStatus()`），
  所以實際看到的時間落在 30~45 秒之間，**不是精準的 30 秒**。
- `wifi.rssi` 會停在最後一次收到的值，不會歸零 —— 正常。

> 對照：`ho_master1.ino` 的
> `Serial.printf("[%s] %s（超過 %lu 秒沒回應即判離線）\n", isOnline ? "上線" : "離線", id, SLAVE_OFFLINE_TIMEOUT / 1000)`；
> `ho_master1.ino` 的 `slaves[i].dirty = true;`；`ho_master1.ino` 的 `if (now - lastOnlineCheck >= 15000)`；
> `ho_master1.ino` 的 `SLAVE_OFFLINE_TIMEOUT = 30000`；
> `ho_master1.ino` 的 `doc["status"] = slaves[idx].online ? "online" : "offline"`、
> `ho_master1.ino` 的 `wifi["connected"] = slaves[idx].online`。

---

## 7. 🖥 slave 復電後回到 `online`

**步驟**：把第 6 項的 slave0 重新供電。

**預期（master 序列埠）**：
```
[狀態] hoban-<slave0Mac> relay=0 版本=1.0.0 運行=<秒>s rssi=<負數>
[上線] hoban-<slave0Mac>（超過 30 秒沒回應即判離線）
```
（`[上線]` 與 `[離線]` **共用同一個格式字串**，括號那段在上線時看起來很怪，
但那是程式碼實際的樣子，**不要因此判 FAIL**。）

**預期（MQTT）**：`hoban/<slave0Id>/status` 回到 `"status":"online"`、
`wifi.connected: true`。

**【失敗判定】**
- 超過 **一輪輪詢週期 ＋ 一次檢查週期（約 30 秒）** 仍停在 offline。

**【觀察項】**
- **slave 不會主動送心跳**，恢復在線靠 master 的 `pollNextSlave()` 主動輪詢
  （一輪約 15 秒），所以恢復時間取決於輪到它的時機，不是即時。
- 兩行的先後順序：`[狀態]` 在 WiFi task 印、`[上線]` 在 `loop()` 的 15 秒檢查印，
  中間可能隔十幾秒。
- **Phase 4 Task 1 起可能多一行** `[歸因] hoban-… 回報已執行 cmdId=<數字> 種類=<0/1/2> 次數=<次數>`，
  緊接在 `[狀態]` 之後。它**只在 slave 回報的 cmdId 換值時印**（例行輪詢的回報不印），
  所以本項（復電後第一次回報）**通常不會有這一行** —— slave 剛開機，`lastCmdId` 是
  `HO_CMD_ID_NONE`（0），條件不成立。**沒有這一行不是 FAIL。**

> 對照：`ho_master1.ino` 的
> `Serial.printf("[狀態] %s relay=%u 版本=%u.%u.%u 運行=%lus rssi=%d\n", …)`；
> `ho_master1.ino` 同第 6 項；`ho_master1.ino` 起的 `pollNextSlave()`。

---

## 8. 🖥 `SLAVES` 指令：master status 立即重發，接著每台 slave 的 status 各重壓一次

**步驟**：對 `hoban/<masterId>/control` 送 `SLAVES`，
在 MQTT Explorer 盯著全部 `hoban/#`。

**預期（序列埠）**：`[MQTT] 收到指令: SLAVES`

**預期（MQTT）**：
1. `hoban/<masterId>/status` **立刻**更新一則。
2. 接著**每台** slave 的 `hoban/<slaveId>/status` 各更新一次（retain 被重壓）。

**【失敗判定】**
- 有任何一台已配對的 slave 的 status **完全沒有**被重壓。

**【觀察項，不是失敗判定】**
- **重壓是逐台的，不是一次連發。** `SLAVES` 只做 `markAllSlavesDirty()` ＋
  一次 `publishStatus()`；實際重壓由 `slaveStatusScheduler()` 每輪 `loop()`
  最多一台、且任兩次至少隔 250ms。
  **2 台約 0.5 秒走完、20 台約 5 秒走完**（健康網路下），**遠短於一輪輪播的 15 秒**。
  - 這個「逐台」設計是**刻意的**：21 個 topic 背靠背發布，病態 socket 下最壞
    可達 210 秒沒有心跳，會撞破 slave 的 30 秒失聯門檻。
  - 所以**看到「一台一台慢慢出現」是正確行為，不是卡住**。
- 若同一輪 `loop()` 的 publish 名額已被 `publishStatus()` 用掉，
  會印 `[MQTT] hoban/<slaveId>/status 讓位給下一輪（本輪 publish 名額已用掉）`
  —— 也是設計行為。

> 對照：`ho_master1.ino` 的 `SLAVES` 分支（`markAllSlavesDirty(); publishStatus();`）；
> `ho_master1.ino` 的 `void markAllSlavesDirty()`；`ho_master1.ino` 的 dirty 優先迴圈；
> `ho_master1.ino` 的 `SLAVE_STATUS_MIN_GAP_MS = 250`；`ho_master1.ino` 的讓位訊息。

---

## 9. 🖥 `ALL:ON`：所有 slave **與 master 自己**都點動 2 秒（是 PULSE 不是持續 ON）

> **這是 App「全部關門」按鈕實際送的指令**（`ALL:ON`，不是 `ALL:OFF`）。

**步驟**：2 台 slave 都在線，對 `hoban/<masterId>/control` 送 `ALL:ON`。

**預期（master 序列埠，依序）**：
```
[MQTT] 收到指令: ALL:ON
[控制] 廣播指令 2 給 2 台
[控制] 送指令 2 給 hoban-<slave0Mac>
[控制] 送指令 2 給 hoban-<slave1Mac>
[群組] 已廣播 3 次（廣播無 ACK，送出成功不代表任何一台收到），並對 2 台各送一次單播，等 MAC 層 ACK
```
之後（約 0.3~6 秒內）收工：
```
[群組] 指令 2 收工：單播 MAC 層已送達 2／2 台
[群組] 韌體層已執行 2／2 台（slave 回報 cmdId=<數字>）
[群組] 注意：MAC 層 ACK 只證明「封包已送達」，不能證明繼電器真的動作
[群組] 執行證明只到韌體層：證明 slave 走完了繼電器動作那段程式，不證明繼電器硬體動作，更不證明籠門關上
[群組] 沒有執行證明不等於沒執行 —— 回報可能還在路上；它只能維持紅色，不能宣稱已確認未執行
⚠ [群組] 這是關門路徑：未送達的籠門必然沒關；已送達的也只代表封包到了，一律以現場確認為準，不要當成已關閉
```

> **Phase 4 Task 1 的變更**：`cmdId` 每次開機從亂數起算，所以那個數字**每次都不同**，
> 判準是「有印出這一行」而不是某個特定值。`韌體層已執行` 的分子若小於分母，
> 每台未取得證明的還會多印一行 `⚠ [群組]   無執行證明：hoban-xxxxxxxxxxxx`。

**預期（兩台 slave 序列埠）**：各自 `[繼電器] 點動 2000 ms`，
約 2 秒後 `[繼電器] 點動結束，已關閉`。master 自己的繼電器（若有接）同步點動。

**預期（MQTT，收工後的 `hoban/<masterId>/status`）**：
```json
"group": {"cmd":2,"age_s":<秒>,"busy":0,"n":2,"ack":2,"noack":0,"gone":0,"exed":2,"exec":"attributed"}
```
且 `slaves[]` 每筆有 `"grp":1` 與 `"exe":1`。

**【失敗判定】**
- **序列埠或 MQTT 任何一處宣稱「已關門」「門已關閉」「全部回報預期狀態」**
  → FAIL。**「韌體層已執行」不在此列**（Phase 4 Task 1 起它是合法且可證的宣稱），
  但任何把它等同於關門成功的措辭就是 FAIL。
- 少印那三行說明（**無論送達與否都必須印**）：
  `[群組] 注意：MAC 層 ACK …`／`[群組] 執行證明只到韌體層：…`／
  `[群組] 沒有執行證明不等於沒執行 …`。
- `"exec"` 不是 `"attributed"`（Phase 4 Task 1 之前是 `"unprovable"`）。
- `"exed"` 大於 `"n"`，或某台 `"exe":1` 而它的序列埠從未印過對應的繼電器動作
  —— 那是歸因指錯台，誤綠方向。
- 廣播行印的是 `廣播指令 1`（那是 `HO_CMD_ON` ＝持續開啟）而不是 `2`
  —— 代表有人把 `ALL:ON` 改成 `HO_CMD_ON`，那不是關門動作。
- 有 slave 的繼電器**持續通電不自動關閉**。

**【觀察項，不是失敗判定】**
- **每台 slave 會連續收到兩次指令**（廣播一次、單播一次），點動計時器被重觸發，
  **門開啟時間會比 2 秒略長** —— 設計行為。
- 收工訊息可能多一段「（達 wall-clock 上限）」，代表 6 秒硬上限到了。
  這只在有台一直拿不到 ACK 時出現。
- `age_s` 是「距離指令送出的秒數」，會隨每則 status 增加 —— 正常。
- **`ALL:ON` 分支結尾那則 `publishStatus()` 是在收工之前發的**
  （分支順序是 `sendCmdToAll()` → `markAllSlavesDirty()` → `publishStatus()`，
  而收工在之後的 `loop()` 才發生）。所以**那一則的 `busy` 會是 `1`、
  `ack` 可能還是 `0`**，這是**正常的中間狀態，不是 FAIL** ——
  判定一律看 `busy: 0` 的那則。
- **`ack: 2` 只代表 MAC 層送達，不代表門關了。** 這是本階段刻意保留的極限。

> **⚠ Phase 4 加上的一個窗口：OTA 轉送期間，「正在接收 OTA 的那一台」有可能靜默漏掉這道關門指令。**
>
> Phase 4 的設計文件寫的是「`ALL:OFF`／單台 `OFF` 在 OTA 期間永遠可用且**不被延後**」。
> **那句話對 master 端成立**（送出流程完全不被 OTA 阻擋，每個 `loop()` 只推進一小步），
> **對目標 slave 不成立**：`ho_slave1.ino` 的 `Update.write()` 跑在 ESP-NOW 收包的同一個
> task 上，跨 64 KB 邊界時會被 flash 區塊抹除擋住 **60~190 ms**
>（依據：esp32 core 3.3.7 `Updater.cpp` 的 `partitionEraseRange(..., block_erase ? SPI_FLASH_BLOCK_SIZE : SPI_FLASH_SEC_SIZE)`，
> 而 `Update.h` 的 `SPI_SECTORS_PER_BLOCK` 是 16 ⇒ 區塊是 64 KB 不是 4 KB 扇區），
> 約 1 MB 的映像會發生 15 次。
>
> **為什麼補送接不住**：那段期間 **MAC 層 ACK 由硬體回覆**，master 因此判定「已送達」，
> 而 `processGroupCmd()` 的補送判準正是「沒拿到 MAC 層 ACK 的才補送」。
> **會讓它現形的是指令歸因**（該台不會回報對應的 `cmdId`，收工時印
> `⚠ [群組]   無執行證明：%s`）—— 那是**回報，不是補救**。
>
> **判準怎麼寫**：本項的失敗判定**完全不變** —— 沒有 OTA 在跑時這個窗口不存在。
> 只有在「實測當下剛好有 OTA 轉送」時，目標那一台沒動作要記成**已知窗口**而不是本項 FAIL；
> **其餘每一台照舊一台都不能少**（對應 Phase 4 計畫的回歸清單 21a／21b）。
>
> **不要拿「`update_slave` 對 `relay == 1` 預設拒絕」當安慰**：那道拒絕的效果，
> 是讓 **OTA 目標依建構必然是 `relay == 0` 的那一台 —— 門還開著、
> 正是這道 `ALL:ON` 要去關的那一台**。它擋的是另一件事（避免 OTA 重啟把正在通電的
> 繼電器斷開），對「一次要全部關」**一點暴露面都沒有減少**。

> 對照：`ho_master1.ino` 的 `ALL:ON` 分支（`sendCmdToAll(HO_CMD_PULSE, 2000)`）；
> `ho_master1.ino` 的 `Serial.printf("[控制] 廣播指令 %u 給 %d 台\n", (uint8_t)cmd, groupJob.count)`；
> `ho_master1.ino` 的 `[群組] 已廣播 %d 次（廣播無 ACK，送出成功不代表任何一台收到），並對 %d 台各送一次單播，等 MAC 層 ACK`；
> `ho_master1.ino` 的 `[群組] 指令 %u 收工%s：單播 MAC 層已送達 %d／%d 台`；
> `ho_master1.ino`／`ho_master1.ino` 的兩行「不能證明已執行」；`ho_master1.ino` 的 `⚠ [群組] 這是關門路徑：…`；
> `ho_master1.ino`／`ho_master1.ino` 的 `GROUP_BROADCAST_REPEAT = 3`／`GROUP_JOB_MAX_MS = 6000`；
> `appendGroupResult()`（`exec` 固定 `"attributed"`，另有 `exed`；**刻意沒有 `cid`**，理由見該函式上方）；
> `groupExecutedIdx()`／`groupExecutedFor()`（執行證明與它的三項「擋不住什麼」）。

---

## 10. 🖥 `ALL:OFF`：全部持續關閉（指令碼是 **0**）

**步驟**：先用序列埠 `on 0` 讓 slave0 持續通電，再對
`hoban/<masterId>/control` 送 `ALL:OFF`。

**預期（master 序列埠）**：與第 9 項同一組訊息，但**指令碼全部是 `0`**：
```
[控制] 廣播指令 0 給 2 台
[控制] 送指令 0 給 hoban-<slave0Mac>
[群組] 指令 0 收工：單播 MAC 層已送達 2／2 台
[群組] 韌體層已執行 2／2 台（slave 回報 cmdId=<數字>）
```
收工說明（三行說明 ＋ `⚠ [群組] 這是關門路徑：…`）**同樣全部要印**。

**預期（slave 序列埠）**：`[繼電器] 關閉`（**不是**「點動結束」——
`HO_CMD_OFF` 走的是 `setRelayPins(false)`）。

**預期（MQTT）**：`group.cmd` 是 **`0`**。

**【失敗判定】**
- 指令碼印成 `2`（那是 PULSE）。
- 有 slave 的繼電器仍然通電。
- 少印那三行說明。

> **OTA 轉送期間的例外與第 9 項完全相同**（目標那一台可能在 60~190 ms 的 flash 區塊抹除
> 窗口內靜默漏包，而補送以 MAC 層 ACK 為判準、接不住）。判準寫法見第 9 項的警告框，
> **不要在這裡重寫一份**。

**【觀察項】**
- **每台 slave 的 `[繼電器] 關閉` 會印兩次**（廣播一次、單播一次），
  `HO_CMD_OFF` 是冪等的，重複執行無害 —— 設計行為。
- 與第 9 項同理，`ALL:OFF` 分支結尾那則 status 的 `busy` 會是 `1`，
  以 `busy: 0` 的那則為準。
- **App 不會送這條指令**（全 repo 的 `lib/`／`test/` 送 `ALL:OFF` 是 **0 處**）。
  它只有序列埠 `alloff` 與人工 MQTT 測試會走到。
  **不要因為按鈕寫「關門」就以為 App 送的是這條** —— App 送的是 `ALL:ON`。

> 對照：`ho_master1.ino` 的 `ALL:OFF` 分支（`sendCmdToAll(HO_CMD_OFF, 0)`）；
> `libraries/HoEspNow/src/HoEspNowProtocol.h:27` 的 `HO_CMD_OFF = 0`；
> `ho_slave1.ino` 的 `[繼電器] 關閉`；
> `A:\project\hoctrl` 的 `lib/pages/device_detail_page.dart:2877-2881`
> （`_sendGroupCloseCommand()` 內的 `_sendCommand('ALL:ON');`，
> 由 `ho_slave1.ino` 的 `_handleGroupCloseDoor()` 呼叫）
> 與 `ho_slave1.ino` 的具名註釋「不要因為按鈕寫「關門」就改成 ALL:OFF」。

---

## 11. 🖥 `PAIR:START` → slave 短按配對 → `slave_count` +1、新 slave 的 status topic 出現

**步驟**：
1. 先 `unpair` 掉 slave1（或用一台全新的 slave）。
2. 對 `hoban/<masterId>/control` 送 `PAIR:START`。
3. 60 秒內短按該 slave 的 BOOT 按鈕。

**預期（master 序列埠）**：
```
[MQTT] 收到指令: PAIR:START
[配對] 進入配對模式，60 秒內請短按 slave 的按鈕
[配對] 接受 hoban-<slave1Mac>，目前共 2 台
[MQTT] 已訂閱 hoban/<masterId>/control
[代理] 已訂閱 hoban/<slave0Id>/control
[代理] 已訂閱 hoban/<slave1Id>/control
```
（**`[MQTT] 已訂閱 …` 那一行也會重印**：`subscribeAllControlTopics()` 只是把游標
歸零，而**游標 0 格就是 master 自己那條 topic**，之後才逐台走名冊。
所以是「master 自己 ＋ 名冊全部」重訂一輪，不是只訂新加入的那台。）

**預期（MQTT）**：
- `hoban/<masterId>/status` 的 `device.slave_count` 變成 `2`、`device.pairing` 是 `true`
- **新出現** `hoban/<slave1Id>/status` 這個 topic

**【失敗判定】**
- `slave_count` 沒有增加。
- `hoban/<slave1Id>/status` 一直沒有出現。
- 沒有印 `[代理] 已訂閱 hoban/<slave1Id>/control`（第 12 項會直接受影響）。

**【觀察項】**
- **配對成功不會自動退出配對模式**，`device.pairing` 會維持 `true` 直到
  `PAIR:STOP`／序列埠 `pair`／BOOT 短按／60 秒逾時（印 `[配對] 逾時`）。
  這是刻意的（App 端已依賴），**不是缺陷**。
- 訂閱動作**不在** `addSlave()` 當場做（那是 WiFi task），而是設
  `pendingSubscribeRefresh` 後由 `loop()` 排隊全量重訂 ——
  所以 `[MQTT] 已訂閱 …` ＋ `[代理] 已訂閱 …` 會把
  **master 自己與所有**已配對的 slave 重印一遍，不只新那台。正常。

> 對照：`ho_master1.ino` 的 `PAIR:START`／`PAIR:STOP` 分支；
> `ho_master1.ino` 的 `[配對] 進入配對模式，60 秒內請短按 slave 的按鈕`；
> `ho_master1.ino` 的 `Serial.printf("[配對] 接受 %s，目前共 %d 台\n", senderId, slaveCount)`；
> `ho_master1.ino` 的 `[配對] 逾時`；`ho_master1.ino` 的 `[MQTT] 已訂閱 %s`（游標 0 格＝master 自己）；
> `ho_master1.ino` 的 `[代理] 已訂閱 %s`；`ho_master1.ino` 的 `controlSubscribeScheduler()`
> （`if (subscribeCursor == 0)` 那個分支就是 master 自己那條）；
> `ho_master1.ino` 的 `if (pendingSubscribeRefresh && …) subscribeAllControlTopics();`。

---

## 12. 🖥 新配對的 slave **立刻**能用自己的 control topic 控制

> 驗證 `pendingSubscribeRefresh` → `controlSubscribeScheduler()` 這條路。

**步驟**：接續第 11 項，**不要重開機**，直接對
`hoban/<slave1Id>/control` 送 `ON`。

**預期**：與第 4 項完全相同（`[代理] … 收到指令: ON`、`[控制] 送指令 2 給 …`、
slave1 點動 2 秒）。

**【失敗判定】**
- master 印 `[MQTT] 指令的目標不在名冊上，忽略: hoban/<slave1Id>/control`
  （代表 `findSlave()` 查不到 —— 名冊沒更新）。
- **完全沒有任何反應**（連 `[代理] … 收到指令` 都沒印）→ 代表沒訂到那條 topic。

**【觀察項，不是失敗判定】**
- 訂閱是**游標式、每輪 `loop()` 一格**，所以理論上有一個「剛配對完、還沒訂到」
  的窗口。**窗口長度是「格數 × 每輪 `loop()` 的長度」，而每一格都含一次
  阻塞式 socket 寫入（`subscribe()`），所以健康網路下 21 格的量級是
  「數十毫秒」而不是「<21ms」**（上一版寫 <21ms 偏樂觀，已改）。
  MQTT 指令經 broker 往返本身也要數十到數百毫秒，所以**實務上仍碰不到**。
  若真的碰到（第一次送沒反應、再送一次就好），
  **記錄下來但不判 FAIL** —— 這是已知的設計取捨（避免病態 socket 下單輪凍結 210 秒）。

> **本項擋得住什麼、擋不住什麼**
>
> - **擋得住**：「配對成功 → `pendingSubscribeRefresh` → 排隊 → 逐格訂閱」這條路真的接上了。
> - **擋不住 20 台規模**：2 台時游標只有 3 格，窗口小到不可能觀察到；
>   20 台是 21 格，窗口大一個數量級。**本項在 2 台下 PASS 不代表 20 台下也 PASS。**
> - **擋不住索引錯位**（理由同第 4 項）：這裡驗的是「訂得到」，不是「送對台」。
> - **上面的毫秒級數字全是靜態推演，沒有實機量測，沒有上界保證。**

> 對照：`ho_master1.ino` 的 `controlSubscribeScheduler()`；
> `ho_master1.ino` 的 `subscribeAllControlTopics()`（**只把游標歸零，不當場送 subscribe**）；
> `ho_master1.ino` 的 `[MQTT] 指令的目標不在名冊上，忽略: %s`。

---

## 13. 🖥 `UNPAIR:<deviceId>`：該台最後一則 status 是 `offline`，之後控制它沒有反應

**步驟**：對 `hoban/<masterId>/control` 送
`UNPAIR:hoban-<slave1Mac>`（**送到 master 的 topic，不是 slave 的**），
然後對 `hoban/<slave1Id>/control` 送 `ON`。

**預期（master 序列埠，第一步）**：
```
[MQTT] 收到指令: UNPAIR:hoban-<slave1Mac>
[配對] 已移除 hoban-<slave1Mac>，剩 1 台
```

**預期（master 序列埠，第二步）**：
```
[MQTT] 指令的目標不在名冊上，忽略: hoban/<slave1Id>/control
```

**預期（MQTT）**：`hoban/<slave1Id>/status` 最後一則是 `"status":"offline"`；
`hoban/<masterId>/status` 的 `slave_count` 減 1、`slaves` 陣列不再有那台。

**預期（slave1 序列埠）**：`[配對] master 要求解除配對`，隨後自動重啟、
印 `EEPROM 無配對記錄` 並開始輪掃。

**【失敗判定】**
- slave1 的 status 停在 `"online"`（會在 App 上留下永遠在線、卻控制不了的幽靈設備）。
- 第二步時 slave1 仍然動作。
- ID 格式打錯時應印 `[配對] UNPAIR 的設備 ID 格式錯誤: <字串>`；
  ID 正確但不在名冊上應印 `[配對] UNPAIR 的設備不在名冊上: <字串>`
  —— 兩者都應**只印一行、不做任何動作**。

**【觀察項，不是失敗判定】**
- 序列埠會印 `[代理] 本輪 socket 名額已用掉，略過取消訂閱 hoban/<slave1Id>/control`。
  **這是 100% 會發生的**（呼叫取消訂閱之前一定先做過一次 offline publish，
  名額必然已被佔走），**不是缺陷**：殘留訂閱收到的訊息會被上面那道
  `findSlave()` 擋掉，且下次重連是 `cleanSession=true`，殘留自然消失。
- 因此第二步印的是「目標不在名冊上」而**不是**完全沒收到 —— 兩者都是 PASS。

> 對照：`ho_master1.ino` 的 `UNPAIR:` 分支（含兩行錯誤訊息）；
> `ho_master1.ino` 的 `Serial.printf("[配對] 已移除 %s，剩 %d 台\n", id, slaveCount)`；
> `ho_master1.ino` 的 `[代理] 本輪 socket 名額已用掉，略過取消訂閱 %s`；
> `ho_master1.ino` 的 `[MQTT] 指令的目標不在名冊上，忽略: %s`；
> `ho_slave1.ino` 的 `[配對] master 要求解除配對`、`ho_slave1.ino` 的 `EEPROM 無配對記錄`、
> `ho_slave1.ino` 的 `[掃描] 開始輪掃 channel 1~13 尋找 master`。

---

## 14. 🖥 `UNPAIRALL`：名冊清空，**過程中 slave 全程收得到心跳**

> **這是最容易踩到 30 秒門檻的一條。**

**步驟**：配對 2 台以上（台數愈多愈有意義），對
`hoban/<masterId>/control` 送 `UNPAIRALL`，**兩邊序列埠同時盯著**。

**預期（master 序列埠）**：
```
[MQTT] 收到指令: UNPAIRALL
[配對] 開始清空名冊，共 2 台（每輪 loop 拆一台）
[配對] 已移除 hoban-<某台>，剩 1 台
[配對] 已移除 hoban-<某台>，剩 0 台
[配對] 名冊已清空
```
拆除全程 `[心跳] channel=<n> 配對模式=否 slave=<台數>` **仍持續出現**
（心跳 log 每 10 次才印一行，另外台數變化時會立即印，所以會看到 slave 數遞減）。

**【失敗判定】**
- **任何一台 slave 印出 `[失聯] 超過 30 秒沒收到心跳`** → FAIL。
- master 序列埠出現超過 30 秒完全沒有 `[心跳]` 的空白。
- 名冊沒有清乾淨（`list` 不是 `（空）`）。

**【觀察項】**
- 名冊本來就是空的時候會印 `[配對] 名冊本來就是空的` 而不進入拆除流程 —— 正常。
- 拆除**從最後一台往前**（`unpairSlave(slaveCount - 1)`），所以 `已移除` 的順序
  是名冊的倒序，**不是** 0、1、2。
- 每台 slave 收到 `HO_PKT_UNPAIR` 後會自動重啟，重啟期間它自己不發任何訊息，
  這與「失聯」不同 —— 判準以 **有沒有 `[失聯]` 那一行**為準。

> **本項擋得住什麼、擋不住什麼**
>
> - **擋得住**：「不分批」這個回歸（若有人把 `processUnpairAll()` 改回
>   `while (slaveCount > 0) unpairSlave(...)` 的一口氣版本，
>   2 台雖然只會累積約 0.2 秒，**但序列埠會在同一輪 `loop()` 內把兩台一次拆完**，
>   看得出來）。
> - **擋不住 20 台的累積量**。分批設計要防的是
>   **20 台 × (每台一次 `espNowDelay(100)` ＋ 一次最壞 10 秒級的阻塞 publish)
>   ＝ 最壞超過 60 秒沒有心跳**。
>   **2 台的最壞累積約 20 秒，本來就撐不破 30 秒門檻** ——
>   也就是說**這一項用 2 台跑，就算分批機制整個壞掉也可能 PASS**。
>   要真的驗到，得配滿 20 台（或至少 5 台以上）＋ 一個會讓 publish 卡住的網路。
> - **擋不住「單次 publish 阻塞過長」**（readme 已知風險第 0 項）：
>   分批只保證「不疊加」，不保證「單次多短」。
> - 上面的秒數全是靜態推演（每台 100ms 固定等待 ＋ 10 秒級黑箱的上界推算），
>   **沒有實機量測**。

> 對照：`ho_master1.ino` 的 `UNPAIRALL` 分支（`unpairAllPending = true;`，
> **只插旗不跑迴圈**）；`ho_master1.ino` 的 `processUnpairAll()`
> （`unpairSlave(slaveCount - 1)` ＋ `[配對] 名冊已清空`）；
> `ho_master1.ino` 的 `[配對] 已移除 %s，剩 %d 台`；
> `ho_master1.ino` 的 `Serial.printf("[心跳] channel=%u 配對模式=%s slave=%u\n", …)`；
> `ho_slave1.ino` 的 `[失聯] 超過 30 秒沒收到心跳`。

---

## 15~18. ⛔ Long Range（`LR:ON`／`LR:OFF`／逾時／自癒／WiFi 斷線觀察）——**今天做不到**

**缺什麼**：功能**完全沒有實作**。

| 原訂驗收項 | 現況 |
|---|---|
| `LR:ON` 兩端同步流程 | **不存在**。`LR:` 分支只印一行 |
| 逾時 10 秒仍套用並印未確認名單 | **不存在** |
| 拔電那台由心跳自癒 | **不存在** |
| 切換前後其他 slave 不失聯 | **無從測起** |
| 切換後 WiFi 是否斷線 | **無從測起**（`esp_wifi_set_protocol()` 從未被呼叫） |
| `status` 的 `long_range` 變 `true` | **不可能** —— `longRangeEnabled` 全檔沒有任何寫入 `true` 的路徑 |

原 Task 6 已依 Phase 2b 的 Ruling **整個移到 Phase 5，與現場實測綁在一起做**。
在那之前這四項無法執行，**請直接標記為「N/A — 功能未實作」，不要標成 FAIL**。

### 15-a. 🖥 唯一今天做得到的部分：`LR:*` 佔位分支不會掉進「未知指令」

**步驟**：對 `hoban/<masterId>/control` 送 `LR:ON`。

**預期（序列埠）**：
```
[MQTT] 收到指令: LR:ON
[LR] 指令尚未實作（Task 6）
```

**【失敗判定】**
- 印的是 `[MQTT] 未知指令: LR:ON`（代表佔位分支被刪掉了）。

**【觀察項】**
- 字串裡的「Task 6」是**寫於移轉之前的舊編號**，該 Task 現在屬於 **Phase 5**。
  **字串本身沒改，所以請照上面那樣逐字比對，不要因為編號看起來過時而判 FAIL。**
- `LR:` 開頭的**任何**字串（`LR:OFF`、`LR:whatever`）都走同一條，都印同一行。
- `status` 的 `device.long_range` **恆為 `false`**，下完 `LR:ON` 也一樣。

> 對照：`ho_master1.ino` 的
> `} else if (message.startsWith("LR:")) { Serial.println("[LR] 指令尚未實作（Task 6）"); }`；
> `ho_master1.ino` 的 `Serial.printf("[MQTT] 未知指令: %s\n", message.c_str())`；
> `ho_master1.ino` 的 `bool longRangeEnabled = false;`（全檔無寫入 `true` 的點）。

---

## 19. 🖥 master 重開機後：名冊、訂閱、channel 全部復原（**LR 設定不在其中**）

**步驟**：配對 2 台 slave 且已連上 MQTT，把 master 斷電重插。

**預期（master 序列埠，開機依序）**：
```
[設定] SSID=<你的SSID> 自訂伺服器=<是/否> 繼電器=<有/無> 上次AP channel=<n>
[名冊] 載入 2 台 slave（上次心跳 channel=<n>）
  1. hoban-<slave0Mac>
  2. hoban-<slave1Mac>
ESP-NOW 就緒，channel=<n>
[名冊] 已重新註冊 2／2 台為 ESP-NOW peer
```
連上 broker 後：
```
[MQTT] 已連線 <伺服器>
[MQTT] 已訂閱 hoban/<masterId>/control
[代理] 已訂閱 hoban/<slave0Id>/control
[代理] 已訂閱 hoban/<slave1Id>/control
```

**【失敗判定】**
- `[名冊] 載入 0 台 slave …`（名冊沒存住）。
- `[名冊] 已重新註冊 <x>／2 台` 的 `<x>` 不是 2 —— **peer 表只存在 RAM，
  少了這步 master 重開機後對所有 slave 的指令都會失敗且永不自我修復**。
- 重開機後對 `hoban/<slaveId>/control` 送 `ON` 沒反應。

**【觀察項】**
- `[名冊] 載入 …` 印的編號是 **1 起算**，但 `list` 印的編號是 **0 起算**，
  兩者刻意不同（`list` 的編號是序列埠指令要用的索引）。**不是缺陷。**
- **LR 設定不在復原範圍內** —— 它根本不存在，NVS 的 `homaster` 命名空間只有
  `count`／`macs`／`espch` 三個鍵。

> 對照：`ho_master1.ino` 的 `[名冊] 載入 %d 台 slave（上次心跳 channel=%u）`
> 與 `  %d. %s`（`i + 1`）；`ho_master1.ino` 的 `[名冊] 已重新註冊 %d／%d 台為 ESP-NOW peer`；
> `ho_master1.ino` 的 `printSlaveList()`（`  %d. %s  %s  rssi=%d`，`i` 由 0 起算）；
> `ho_master1.ino` 的 `prefs.begin("homaster", …)`（只有 `count`／`macs`／`espch`）；
> `ho_master1.ino` 的 `[設定] SSID=%s 自訂伺服器=%s 繼電器=%s 上次AP channel=%u`；
> `ho_master1.ino` 的 `Serial.printf("ESP-NOW 就緒，channel=%u\n", currentChannel)`；
> `ho_master1.ino` 的 `[MQTT] 已連線 %s`；`ho_master1.ino` 的 `[MQTT] 已訂閱 %s`；`ho_master1.ino` 的 `[代理] 已訂閱 %s`。

---

## 20. 🖥 `reset` 後重開機：**slave 名冊與 `espch` 仍然保留**（`homaster` 不是 `hoban`）

> 原訂是「LR 設定仍然保留」，但 **LR 設定不存在**（見第 15~18 項）。
> 這一項改成驗證同一個命名空間選擇 —— **名冊與 `espch` 撐過 `reset`**，
> 那才是這個設計真正要保護的東西（重新配網不該讓所有籠子解除配對）。

**步驟**：配對至少 1 台 slave、已連上一個 **channel 不是 1** 的 AP，
對 `hoban/<masterId>/control` 送 `reset`。

**預期（序列埠，reset 當下）**：
```
[MQTT] 收到指令: reset
[設定] NVS 網路設定已清除
```
1 秒後重啟，開機印：
```
[設定] SSID=(未設定) 自訂伺服器=否 繼電器=無 上次AP channel=0
[名冊] 載入 1 台 slave（上次心跳 channel=6）
```
（`上次AP channel=0` 與 `上次心跳 channel=6` **必須同時成立**：前者是 `hoban`
命名空間的 `apch`，被清掉了；後者是 `homaster` 命名空間的 `espch`，沒被清掉。）

接著因為沒有 WiFi 設定，進 BLE 配網模式：
```
[channel] 本次開機不關聯 WiFi，切回 NVS 記住的 channel=6，維持 1 台已配對 slave 的心跳
[BLE] 已啟動，名稱: hoban-<masterMac>
[BLE] 等待 App 配網
```

**【失敗判定】**
- `[名冊] 載入 0 台 slave` → 名冊被 `reset` 清掉了，**這是嚴重回歸**
  （重新配網會讓所有籠子解除配對）。
- `上次心跳 channel=0` 而測試前確實成功連過 WiFi。

**【觀察項】**
- 若這台之前從未成功連上 WiFi，NVS 沒有 `espch`，會印
  `⚠ [channel] 名冊有 <n> 台 slave，但 NVS 沒有 channel 記錄，…` 並停在 channel 1。
  **那是已知且已標示的行為，不算本項失敗，但也不算通過** —— 先配網成功一次再回來測。

> 對照：`ho_master1.ino` 的 `reset` 分支（`clearNetConfig(); espNowDelay(1000); ESP.restart();`）；
> `ho_master1.ino` 的 `clearNetConfig()`（`netPrefs.begin("hoban", false); netPrefs.clear();` ＋
> `[設定] NVS 網路設定已清除`）；`ho_master1.ino`／`ho_master1.ino` 的 `prefs.begin("homaster", false)`；
> `ho_master1.ino` 的 `[設定] SSID=%s 自訂伺服器=%s 繼電器=%s 上次AP channel=%u`；
> `ho_master1.ino` 的 `[名冊] 載入 %d 台 slave（上次心跳 channel=%u）`；
> `ho_master1.ino`／`ho_master1.ino` 的兩行 `[channel] …`；
> `ho_master1.ino` 的 `Serial.printf("[BLE] 已啟動，名稱: %s\n", deviceId)`；
> `ho_master1.ino` 的 `Serial.println("[BLE] 等待 App 配網")`。

---

## 21. 🖥 **回歸 Phase 2a**：BLE 配網、長按重置、`FIND_BEST_SERVER`、`HASRELAY:*`

**做法**：直接跑 `docs/phase2a-regression-checklist.md` 的對應項目，
**不要在這裡重寫一份判準**（重寫就是製造第二份可能不一致的真相）。

Phase 2b 只動到其中兩處，實測時要注意：

1. **`[MQTT] 已連線 X，訂閱 Y` 已在 Task 4 拆成兩行**
   （`[MQTT] 已連線 X` ＋ `[MQTT] 已訂閱 Y`），
   `phase2a-regression-checklist.md` 的第 5／9a／11 項已同步更新。
   後面還會逐台印 `[代理] 已訂閱 …`。
2. **看到 `⚠ [MQTT] 訂閱失敗 …` 一律判 FAIL**，不要因為連線那行成功就判 PASS。

**`FIND_BEST_SERVER` 的補充判準（Phase 2b 新增的連帶行為）**：
連上新 broker 後應該看到 `[MQTT] 已訂閱 …` ＋ 逐台 `[代理] 已訂閱 …`，
且**每台 slave 的 status 都被重壓一次**（新 broker 上沒有任何 retain，
`markAllSlavesDirty()` 會讓整份名冊重發）。

**【失敗判定】**
- 換 broker 後 slave 的 `hoban/<slaveId>/status` 在新 broker 上一直不出現。

> 對照：`ho_master1.ino`／`ho_master1.ino` 的 `quickConnectToIndex()`／`quickConnectCustom()`
> 結尾（`subscribeAllControlTopics(); publishStatus(); markAllSlavesDirty();`）；
> `ho_master1.ino` 的 `[MQTT] 已訂閱 %s`；`ho_master1.ino` 的 `⚠ [MQTT] 訂閱失敗 %s（…）`。

---

## 22. 🖥 **回歸 Phase 2a**：WiFi 拔線 60 秒，slave 全程不失聯

> **這一項有兩個變體，斷法不同、走的程式路徑也不同，請分開做、分開記錄。**
> 上一版把兩者混成一句「拔掉 AP 的網路線（或關掉 AP）」，
> 又在觀察項寫「拔線會讓 WiFi 直接斷開」——**那句對變體 B 是錯的**：
> 拔的是 AP 對外的 WAN 線時 **WiFi 關聯根本不會斷**，
> 而那**正好就是製造病態 socket 的方式**。

**共同前置**：master 已連上 WiFi 與 MQTT、已配對至少 1 台 slave 且該台繼電器是 ON。
兩邊序列埠同時盯著，各做 **60 秒**。

### 變體 A：WiFi 真的斷（關掉 AP／把 AP 電源拔掉）

**預期（master 序列埠）**：出現 `[WiFi] 重連嘗試 #<n>`，之後
`[MQTT] 嘗試 <伺服器> …`／`[MQTT] <伺服器> 失敗，state=<n>`；
**`[心跳] …` 全程持續出現**。

### 變體 B：WiFi 不斷、只斷外網（**拔掉 AP 的 WAN／上行網路線**）

WiFi 關聯與 IP 都還在，`WiFi.isConnected()` 仍是 true，
本地 socket 仍停在 `ESTABLISHED`、`mqttClient.connected()` 仍回 true。

**預期（master 序列埠）**：**多半看不到 `[WiFi] 重連嘗試 #<n>`**（WiFi 沒斷），
而是狀態發布開始變慢／卡住，最後才出現 MQTT 斷線與重連訊息；
**`[心跳] …` 全程持續出現**。

**【失敗判定】（兩個變體共用）**
- **slave 印出 `[失聯] 超過 30 秒沒收到心跳`** → FAIL。
- slave 印出 `[安全] 失去 master，繼電器已關閉`（＝籠門被打開）→ FAIL。

**【觀察項，不是失敗判定】**
- 變體 A：單次 `mqttClient.connect()` 最壞阻塞約 **18 秒**（DNS 約 15 ＋ TCP 3），
  期間沒有心跳。`smartConnect()` 刻意設計成**一次呼叫只試一台 broker**，
  由 `loop()` 的 10 秒節奏推進，確保兩次 18 秒之間必定隔著約 10 秒的心跳。
- **變體 B 是目前手邊最接近「病態 socket」的做法**，值得記錄
  兩則心跳之間最長的空白有多久（這是第 0 號風險唯一能拿到實測數字的機會）。

> **本項擋得住什麼、擋不住什麼（上一版的理由對變體 B 是錯的，已更正）**
>
> - **兩個變體都擋得住**：「重連流程吃掉心跳」這條回歸。
> - **變體 A 擋不住第 0 號風險** —— WiFi 一斷，socket 直接失效，
>   根本走不到「寫得進去但寫不完」那條路。
> - **變體 B 走得到 10 秒級黑箱**（TCP 送出緩衝塞滿後每次 `write()` 吃滿
>   10 次重試 × 1 秒 `select()`），**但仍不保證重現「無限期」那一種**：
>   無限期需要「每輪只擠得出幾個 byte」讓重試計數器反覆被重置，
>   對應的是**對端 TCP 視窗趨近於零**，而不是「完全不通」。
>   拔 WAN 線比較容易造成後者。
> - 所以：**變體 B PASS 不代表第 0 號風險不存在**；
>   它頂多證明「這一次沒有踩到」。第 0 號風險目前**沒有任何測試能證明它不發生**，
>   只能靠讀 `NetworkClient::write()` 的原始碼確認機制存在。

> 對照：`ho_master1.ino` 的 `[WiFi] 重連嘗試 #%d`；
> `ho_master1.ino`／`ho_master1.ino` 的 `[MQTT] 嘗試 %s …`／`[MQTT] %s 失敗，state=%d`；
> `ho_master1.ino` 的 `[心跳] channel=%u …`；
> `ho_slave1.ino` 的 `[失聯] 超過 30 秒沒收到心跳`、`ho_slave1.ino` 的
> `[安全] 失去 master，繼電器已關閉`。

---

## 23. 🖥 送錯 topic／送未知指令：探針字串

**步驟**：
1. 對 `hoban/<masterId>/control` 送 `NOSUCHCOMMAND`。
2. 對 `hoban/hoban-000000000000/control` 送 `ON`（一個不存在的設備 ID）。

**預期（序列埠）**：
```
[MQTT] 收到指令: NOSUCHCOMMAND
[MQTT] 未知指令: NOSUCHCOMMAND
```
```
[MQTT] 指令的目標不在名冊上，忽略: hoban/hoban-000000000000/control
```

**【失敗判定】**
- 第 1 步印的不是 `[MQTT] 未知指令: …` —— **App 的驗證程序依賴這行探針字串**。
- 第 2 步觸發了任何 ESP-NOW 送出（序列埠出現 `[控制] 送指令 …`）。

**【觀察項】**
- topic 格式本身解析不了時（例如 `hoban/abc/control`）印的是
  `[MQTT] 無法解析的 topic，忽略: <topic>`，是另一行、另一條路徑。

> 對照：`ho_master1.ino` 的 `[MQTT] 未知指令: %s`；
> `ho_master1.ino` 的 `[MQTT] 指令的目標不在名冊上，忽略: %s`；
> `ho_master1.ino` 的 `[MQTT] 無法解析的 topic，忽略: %s`。

---

## 24. 交叉引用：假綠燈相關的四項在 Phase 1 清單裡

以下四項驗證的是 Task 5 的群組指令假綠燈防線，**寫在
`docs/phase1-regression-checklist.md`**（因為它們用的是序列埠 `alloff`／`allpulse`），
不要在這裡重寫一份：

| 項目 | 標記 | 驗什麼 |
|---|---|---|
| **第 8 項**（`allpulse`） | 🖥 | 群組指令的基本輸出與「不得宣稱已執行」 |
| **8a**（不對稱可達性） | 📡 | 收不到廣播、收得到單播的 slave 必須誠實回報。**這是唯一需要訊號邊界的一項** |
| **8a-2**（斷電） | 🖥 | 完全不可達的 slave 同樣誠實回報 |
| **8b**（空名冊） | 🖥 | 名冊為空時不得宣稱任何成功 |
| **8c**（`unpairall` 心跳） | 🖥 | 與本清單第 14 項互補（8c 走序列埠，第 14 項走 MQTT） |
| **8d**（`fakeslaves` ＋ `alloff`） | 🖥 | N1 的觸發前提會發生，且沒有任何東西轉綠 |
| **8e**（`droppeer` ＋ 重新配對） | 🖥 | 混合成敗（`已送達 1／2`）、逐台 `grp` 正確 |

**複審的建議：進 Phase 3 之前，至少先跑「第 8 項 ＋ 8d ＋ 8e」——
這三項都不需訊號邊界、桌面即可。**

**這三項擋得住什麼、擋不住什麼（照實寫）**：

- **擋得住**：群組指令的輸出格式、「不得宣稱已執行」、混合成敗的逐台歸因、
  `esp_now_send()` 失敗路徑不會被算成送達。
- **擋不住**：**N1（ACK 歸因閂鎖沒關掉）**。
  在 `groupNoteUnicastAck()` 開頭那道 `if (!groupCmdActive()) return;` 存在的前提下，
  N1 的兩道關閂**原理上就無法被黑箱測試單獨驗出來**
  （收工後的窗口被 phase 守衛獨力封死；job 進行中的同 MAC 窗口只剩 `PAIR_ACK`
  那 1~2ms，人手做不出來）。
  **那是「症狀已被結構防線封死」的結果，不是覆蓋缺口** ——
  但也**不要**把 8d／8e 的 PASS 讀成「N1 已驗證」。

---

## 驗收判定

- 第 1~14、19~23 項全數打勾 → Phase 2b 的**可測範圍**通過。
- 第 15~18 項標記 **N/A — 功能未實作（移至 Phase 5）**，不計入通過與否。
- 第 24 項引用的 Phase 1 清單項目建議一併跑，至少跑完 🖥 的那批。

任一項失敗時，記錄失敗現象與**序列埠的實際輸出原文**，
回頭對照 `ho_master1/ho_master1.ino`、`ho_slave1/ho_slave1.ino` 與
`ho_master1/readme.md` 排查。

**若發現本清單的預期輸出與程式碼不符，那是本清單的缺陷，不是韌體的缺陷 ——
請直接修這份文件，並在 `.superpowers/sdd/2026-08-16-esp32-master-phase2b/progress.md`
記一筆。** 這類缺陷在本專案已經出現四次。
