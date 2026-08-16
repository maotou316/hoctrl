# Phase 5 實作計畫：Long Range 模式切換與現場實測

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 讓 Long Range 成為一個**可切換、可驗證、可自動回滾**的模式，並產出一份**使用者可以拿著兩片板子到空曠地執行**的實測程序，用實測數字決定出貨預設值。

這句話要拆成兩半看，兩半的驗收標準完全不同：

- **韌體那半的驗收不是「距離變遠」，是「切換過程沒有製造它要解決的問題」。**
  LR 的價值是讓遠處的門也收得到「全部關」的廣播；如果切換的那 10 秒讓兩端失聯，
  就是在最需要它的那一刻把它關掉。因此本階段的硬性驗收條件是
  **「LR 切換的任何一條路徑（成功／逾時／失敗／回滾／緊急插隊）都不得讓任何一台
  slave 的心跳空窗達到 30 秒」**。
- **實測那半的驗收不是「ping 得到」，是「一次全部關在極限距離下的實際到達率」。**
  兩台板子在 500 公尺外還能互相聽到心跳，不代表一封沒有 ACK 的廣播關門指令
  一定會到。Task 5 為此做了一個會自動跑 N 輪、自動列表的量測工具，
  因為那個數字才是這套捕捉系統要的。

**Architecture:** 四層，刻意讓「驗證」在最前面、「實測」在最後面：

1. **驗證層（Task 1）** —— 一個獨立的最小探針 sketch `ho_lr_probe`，
   只做一件事：把三個**未驗證的 IDF 行為**變成寫在文件上的事實。
   **它不實作任何功能**，Task 2 以後的每一個設計決定都以它的產出為輸入。
2. **套用層（Task 2）** —— `applyLongRange()` 與協定的 `HO_PKT_LR_SET`／`HO_PKT_LR_ACK`，
   master 與 slave 各一份、行為一致，且**套用後用 `esp_wifi_get_protocol()` 讀回驗證**
   （把「呼叫成功」升級成「確實生效」）。本層只做「開機依儲存值套用」，不做切換。
3. **編排層（Task 3 slave／Task 4 master）** —— 切換的時序、安全指令的插隊權、
   切換後的主動驗證與自動回滾、以及 slave 端「失聯時自我對調 LR」的救援路徑。
   **這一層的每一個決定都是在回答「切換到一半出事時，籠門會不會開」。**
4. **實測層（Task 5 儀器／Task 6 程序）** —— 心跳序號、封包遺失率統計、
   `lrtest` 全關可靠度測試，以及交付使用者執行的現場程序文件。

**Tech Stack:** Arduino ESP32 core 3.3.7、`esp_wifi.h`（`esp_wifi_set_protocol` /
`esp_wifi_get_protocol`）、`esp_now.h`（含 `esp_now_set_peer_rate_config` 的可用性探測）、
PubSubClient、ArduinoJson 7.4.3、Preferences (NVS, master)、EEPROM (slave)

**Spec:** `docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md`
（「系統本質」與「難點 2：Long Range 模式可切換」逐字照用）

**執行紀錄（全部必讀，不得跳過）：**
- `.superpowers/sdd/2026-08-14-esp32-master-slave-phase1/progress.md`
- `.superpowers/sdd/2026-08-15-esp32-master-phase2a/progress.md`
- `.superpowers/sdd/2026-08-16-esp32-master-phase2b/progress.md`

---

## 前置狀態與相依（開工第一件事就是核對，核對不過要停下來回報）

本計畫的前身是 Phase 2b 的 Task 6，被裁定移到這裡（理由見 phase2b 的 progress.md）。
移出來之後它有兩個**尚未成立**的前提，動工前必須逐一 `grep` 確認：

| # | 需要的東西 | 檢查指令 | 找不到時怎麼辦 |
|---|---|---|---|
| 1 | Phase 2b Task 4 拆出來的 `handleMasterCommand()` | `grep -n "void handleMasterCommand" ho_master1/ho_master1.ino` | **不要自行改寫 `mqttCallback()` 的結構。** Task 4 已備妥兩種接法的程式碼（4-A：接在 `handleMasterCommand()`；4-B：接在現行 `mqttCallback()` 的 else-if 鏈尾）。照實際狀況選一種，**並在 report 寫明用了哪一種** |
| 2 | Phase 4 的 `hoFrameCrc()` 與 `HO_ESPNOW_VERSION 2` | `grep -n "hoFrameCrc\|HO_ESPNOW_VERSION" libraries/HoEspNow/src/HoEspNowProtocol.h` | 兩種情況都可以做：Phase 5 只**新增**封包型別與 payload，不動 CRC 演算法也不動版本號。若 Phase 4 尚未執行，`hoPayloadCrc()` 仍在，照樣可用 |

> **2026-08-16 實際狀態**（撰寫本計畫時 `git log` 最新為 `85b6d15`）：
> Phase 2b 只完成到 **Task 3**，`handleMasterCommand()` **不存在**，`mqttCallback()`
> 仍是 Phase 2a 的 else-if 鏈；Phase 4 **尚未開工**，`HO_ESPNOW_VERSION` 仍是 1。
> 這個狀態會變，所以**以 `grep` 的結果為準，不要以本段文字為準。**

**另外三件必須先知道的既有事實：**

- `ho_slave1.ino` 的 `EE_ADDR_LONGRANGE`（EEPROM 位址 8）**已定義但從頭到尾沒被讀也沒被寫** ——
  `loadPairing()` 沒讀、`savePairing()` 沒寫。Phase 1 只留了位址。Task 2 要補上。
- `ho_master1.ino` 的 `longRangeEnabled` **只被讀、從來沒被寫過**：
  `sendHeartbeat()` 與 `HO_PKT_PAIR_ACK` 帶出去、`buildStatusDoc()` 放進 JSON，
  但沒有任何路徑會把它從 `false` 改成 `true`，也沒有持久化。
- `ho_espnow_test/ho_espnow_test.ino` 有一條 `check(sizeof(HoHeartbeatPayload) == 4, ...)`。
  Task 5 會把心跳 payload 加長到 6 bytes，**那條測試必須同步改**，否則 `-Model test` 會紅。
  （目前該檔共 33 個 `check()`，以實際 `grep -c "check("` 為準，不要照抄任何文件寫死的數字 ——
   Phase 1 曾因 commit 訊息寫死項數而需要事後補救。）

---

## Global Constraints

### 安全鐵則（違反即為 Critical）

- **繼電器安全鐵則**：`initRelayPins()` 必須是 `setup()` 第一行，早於 `Serial.begin()`。
- **ESP-NOW 不可中斷鐵則**：master 心跳每 1 秒（WiFi 關聯期間 200ms），slave 超過
  **30 秒**沒收到心跳就 `startChannelScan()` → **強制關閉繼電器 ＝ 籠門被打開、
  動物逃脫或未被捕捉**。本階段所有新程式碼：
  - 所有等待走 `espNowDelay()`，**不得用裸 `delay()`**
    （`sendHeartbeatBurst()` 內部那個 `delay(200)` 是唯一既有例外，它本身就在發心跳）
  - LR 握手、驗證、回滾**全部是 `loop()` 驅動的狀態機**，每次 `loop()` 只推進一小步
  - `esp_wifi_set_protocol()` 是本階段唯一新增的、可能引發射頻中斷的呼叫。
    **它的前後各必須 `sendHeartbeatBurst()` 一次**（做法與 Phase 4 對阻塞步驟的處理一致）
- **ESP-NOW callback 不得發 MQTT、不得做冗長操作**：`onEspNowRecv()` 跑在 WiFi task。
  本階段新增的 `HO_PKT_LR_SET`／`HO_PKT_LR_ACK` 處理**只能設旗標與時間戳**，
  實際的 `esp_wifi_set_protocol()`／`EEPROM.commit()`／`publishStatus()` 一律交給 `loop()`。
  （slave 端這條特別重要，見 Task 3 決定 3。）
- **安全指令的優先權高於 LR**：任何繼電器指令（`ALL:ON`／`ALL:OFF`／單台 `ON`／`OFF`／
  序列埠 `allon`／`alloff`／`allpulse`／`on`／`off`／`pulse`）在 LR 切換進行中
  **不得被延後、不得被排隊、不得被拒絕**。做法見「決定 2」。

### 前階段留下、本階段必須沿用的認知更正（不得沿用錯誤前提）

1. `WiFi.disconnect(bool wifioff, bool eraseap)` 第一個參數是 **wifioff**。
2. `WiFi.setAutoReconnect(true)` 在 core 3.3.7 是**死碼**，Arduino 3.x 不會自己背景重連。
   → **本階段的直接影響**：若 `esp_wifi_set_protocol()` 真的造成 STA 斷線，
   **不會有任何底層機制自己把它接回來**，只有 `loop()` 的 WiFi 管理區塊（每 5 秒檢查）會處理。
3. `setSocketTimeout(3)` 只管「TCP 已連線後等 CONNACK」，不管 TCP connect，**更不管 DNS**。
4. `NetworkClient::connect(host, port)` 的 DNS 那段沒有 timeout 參數，最壞約 **15 秒**；
   `mqttClient.publish()` 卡住時最壞吃 **10 秒**（Phase 2b Task 3 review 的更正值）。
5. ArduinoJson 7.4.3 的 `StaticJsonDocument<N>` 的 N **完全被忽略**；容量控制只能靠
   `measureJson()` 實測值，所有 MQTT JSON 發布一律走 `publishJsonDoc()`。
6. **`WiFi.begin(ssid, pass, ch, nullptr)` 是「從該 channel 開始掃描」而非「鎖定該 channel」，
   機制未經實機驗證。** 不得在本階段的文件或回歸清單裡把它寫成已證實的事實。

### 「未驗證的 IDF 行為不得被寫成事實」（本階段存在的理由）

Phase 2a 已因此踩過一次（`WiFi.begin()` 帶 channel 的掃描語義被誤以為能限制單頻掃描，
且回歸清單把「心跳 channel 跳動」列成**失敗判定**，若 IDF 真的會續掃就會把正常韌體判成 FAIL）。

本階段有**三個**同類的未驗證行為，全部集中在 Task 1 用探針量掉：

| # | 未驗證的宣稱 | 出處 | 若宣稱不成立的後果 |
|---|---|---|---|
| A | `esp_wifi_set_protocol()` 在**已關聯 AP** 的狀態下呼叫不會造成斷線 | Phase 2b Task 6 只寫成「可能造成一次斷線重連（未驗證）」 | LR 切換 = 一次 MQTT 中斷，最壞疊上 DNS 15 秒 |
| B | **混合 bitmap 下兩端不同步只會失去距離增益，不會互相收不到** | Phase 2b「決定 5」的核心前提 | **握手逾時仍照樣套用」從安全行為變成危險行為**；整個切換流程要重新設計 |
| C | ESP32-C3 支援 `WIFI_PROTOCOL_LR` | 從未有人查證。master 支援 WROOM 與 C3 雙板，**slave 全部是 C3** | 若 C3 不支援，slave 端 LR 完全做不了，整個功能只剩 master 單邊，等於無效 |

**宣稱 B 是本計畫最重要的一條。** 它是 Phase 2b「決定 5」的地基：因為「不同步只失去增益」，
所以「等不到就照樣切」才是安全的；一旦這個前提倒了，那句話就變成
「等不到就照樣把那台切成聾子」。**在 Task 1 的探針量出結果之前，任何 Task 都不得
把 B 當成已知事實寫進註釋、readme 或回歸清單。**

> **本計畫對 B 的預先立場**：即使 B 成立，本計畫的編排（決定 1、2、3）**也不依賴它**。
> 這是刻意的 —— 詳見「決定 1」對「master 為什麼最後才切自己」的重新論證。
> B 只決定「失敗的嚴重程度」，不決定「流程長什麼樣」。

#### 文獻查證結果（2026-08-16，規劃階段先做的桌面查證，**不能取代 Task 1 的實測**）

**關於 C（ESP32-C3 支不支援 LR）—— 有明確的官方敘述，風險大幅下降：**

> Since LR is Espressif-unique Wi-Fi mode, **only ESP32 chip series devices (except ESP32-C2)
> can transmit and receive the LR data.**
> —— ESP-IDF Wi-Fi Driver 指南，「LR Compatibility」

例外只有 **ESP32-C2**，而本專案的 slave 與 master-c3 都是 **ESP32-C3**。
所以 C3 應該支援。**但 Task 1 測項 C 仍然要做** —— 「文件說支援」與
「這條 Arduino core 3.3.7 工具鏈上 `esp_wifi_get_protocol()` 真的讀得回 LR 位元」
是兩件事，而後者才是 `applyLongRange()` 的讀回驗證要擋的東西。

**關於 B（混合 bitmap 的互通性）—— 查證結果對 Phase 2b 的宣稱不利：**

1. Wi-Fi 驅動指南確實有一句聽起來支持它的話：
   > For LR-enabled **station** of ESP32 whose mode is NOT LR-only mode,
   > **it is compatible with traditional 802.11 mode.**

   **但那句話講的是 STA 與 AP 的「關聯」**：關聯過程有 capability 協商，
   雙方會談出一個共同支援的速率集合。**ESP-NOW 是無連線的，沒有任何協商過程。**
   把這句話直接套到 ESP-NOW 上是一次語境誤植。

2. **反方向的證據比較直接**：ESP-IDF 官方 espnow 範例的 Kconfig 說明是
   > `ESPNOW_ENABLE_LONG_RANGE` — "When enable long range,
   > **the PHY rate of ESP32 will be 512Kbps or 256Kbps**"

   也就是說，**把 LR 位元加進 bitmap 之後，ESP-NOW 的送出速率就變成 LR 速率**，
   這是單方面的行為、沒有對象可以協商。收端若沒有 LR 位元，就解不出這個調變。
   而且該範例是**兩端都設**同一組 bitmap。

3. `docs.espressif.com` 的 ESP-NOW API 頁面**完全沒有**任何關於 LR 的敘述，
   也沒有說明兩端 bitmap 不一致時會怎樣 —— 這一點本身就值得記下來：
   **沒有文件保證的行為不可以當作設計前提。**

**規劃階段的結論**：宣稱 B **沒有任何文件支持，且現有證據指向它不成立**。
因此本計畫**一律以「B 不成立」為預設前提**來設計（決定 1、2、3、4 全部如此），
並把它交給 Task 1 的探針去實測推翻或確認。

**這也意味著 Phase 2b「決定 5」那句話必須被降級**：
「混合之下，LR 只是多一種可用速率，不是換一套不相容的調變」——
這句話對 **STA↔AP 關聯**大致成立，對 **ESP-NOW** 沒有依據。
Task 6 的 readme 與回歸清單在 Task 1 填出結果之前**不得引用它**。

#### 補充查證（2026-08-16 第二輪，由獨立研究代理完成）—— A 已被文件判定，並新增兩個致命危害

**宣稱 A 已不再是「未驗證」，而是「文件明說會發生」。** ESP-IDF Wi-Fi 驅動指南
（`station-scenarios.rst`，station 情境的「Wi-Fi Configuration Phase」）逐字：

> However, if the configuration does not need to change after the Wi-Fi connection is set up,
> you should configure the Wi-Fi driver at this stage, because the configuration APIs
> (such as `esp_wifi_set_protocol()`) **will cause the Wi-Fi to reconnect**,
> which may not be desirable.

也就是說：**在已關聯 AP 的狀態下切換 LR，一定會斷線重連。**
Task 1 的測項 A 因此**從「測會不會斷」改成「測斷多久、MQTT 多久恢復」**——
要量的是 `WiFi.status()` 從 `WL_CONNECTED` 離開到回來的毫秒數，以及
`mqttClient.connected()` 恢復的毫秒數。這個數字決定「決定 3」的 10 秒驗證窗夠不夠。

**追加：必須在 `esp_wifi_start()` 之後呼叫。** 這一點**不在標頭註釋裡**，是 Espressif
維護者在 esp-idf#9933 明講的（"`esp_wifi_set_protocol` … need to be called after
`esp_wifi_start()`, will update the doc"）。開機路徑的 `applyLongRange()` 位置要照這個排。

##### 危害 D：STA 與 SoftAP **共用同一個 LR 位元** —— 會讓配網模式的 AP 手機看不到

esp-idf#9978，維護者逐字：

> Currently, the STA and the softAP use the same LR bit, so when you set the LR mode,
> **both STA and softAP will take effect.**

而 Wi-Fi 驅動指南同時說：

> For LR-enabled **AP** of ESP32, it is **incompatible with traditional 802.11 mode**,
> because the **beacon is sent in LR mode**.

**合起來就是一條完整的變磚路徑**：使用者開了 LR → 之後 WiFi 換路由器需要重新配網 →
master 進 AP 模式 → **beacon 用 LR 送出，手機完全掃不到這個 AP** → 配網模式等於不存在。

**本階段必須新增的硬性設計約束（寫進 Task 4，並在 Task 2 的 `applyLongRange()` 就留好介面）**：

> **進入配網模式（SoftAP／`startConfigPortal()` 之類）之前，必須無條件先把 protocol
> 還原成 `WIFI_PROTOCOL_11B|11G|11N`（不含 LR），且不因為 NVS 裡存的是 LR 而跳過。**
> 配網結束離開 AP 模式後，再依儲存值重新套用。

這條與「決定 5：現場沒有 App 時的回滾」是同一個目的的兩條路徑，但**不能互相取代**：
決定 5 是使用者主動要求回滾，這一條是系統為了不失去最後的救援管道而強制執行。

##### 危害 E：LR 位元**存在 NVS，重燒韌體與 erase 都清不掉**

同一位維護者（esp-idf#9933）：

> because **the LR state has been saved in NVS**, when wifi start,
> the phy and HW register will be updated.

espressif/esp-now#37 的標題就是使用者的慘叫：
"ESP long range mode persists on flash, erase, and reset!!!"

**後果**：一台被切成 LR 的板子，就算整個重燒 Phase 1 的舊韌體（完全沒有 LR 程式碼），
它**還是 LR**——因為舊韌體從不呼叫 `esp_wifi_set_protocol()`，NVS 裡的值就繼續生效。
現場「重燒回舊版救援」這條所有人都會直覺去用的路，**是無效的**。

**本階段必須新增的硬性設計約束**：

> **開機路徑必須無條件呼叫一次 `esp_wifi_set_protocol()`，把 protocol 明確設成
> 目前應有的值——而不是「NVS 說要 LR 才呼叫、否則不呼叫」。**
> 「不呼叫」不等於「沒有 LR」，它等於「沿用上一次殘留的狀態」。

這條同時是 Task 2 `applyLongRange()` 的正確性條件：它的 else 分支**不可以是空的**，
必須實際送出不含 LR 的 bitmap。

##### 追加：**沒有 `soc_caps.h` 巨集可以判斷 LR 支援**

研究代理實際 grep 了 v5.5 的 `components/soc/{esp32,esp32c3,esp32s3,esp32c6,esp32c2}/include/soc/soc_caps.h`：
**任何晶片都沒有** `SOC_WIFI_LR_SUPPORT` 這類巨集。
所以 **不得寫 `#ifdef SOC_WIFI_LR_SUPPORT`**（會永遠不成立，等於整段 LR 程式碼被靜默編譯掉，
而且編譯會通過——這是最惡劣的失敗型態）。要條件編譯只能沿用既有的
`CONFIG_IDF_TARGET_ESP32C3` / `_ESP32`。

##### 追加：`esp_now_set_peer_rate_config()` 的呼叫順序是硬性的

- 標頭（IDF v5.1~v5.5 一致）：`@attention 1. This API should be called after
  esp_wifi_start() and esp_now_init().`
- 文件補充：「Make sure that the peer is added before configuring the rate.」
- bitmap 沒有 LR 位元就設 LR 速率 → `W (719) wifi: invalid rate, need change phy mode to LR`（esp-idf#11595）
- 在 `esp_now_init()` 之前設速率會 assert 崩潰（esp-idf#11751，已由 commit `554e6880` 修）

**唯一正確順序**：
`esp_wifi_start()` → `esp_wifi_set_protocol(…|LR)` → `esp_now_init()` → `esp_now_add_peer()` → `esp_now_set_peer_rate_config()`

**踩雷點**：`esp_now_rate_config_t` 的第一個欄位 `phymode`，其列舉 `WIFI_PHY_MODE_LR`
的值是 **0**。所以 `memset(&rc, 0, sizeof(rc))` 想「取預設值」**會靜默選到 LR**。

Arduino core 3.3.7 對應 IDF v5.5.x，該版把 `esp_now_rate_config_t` 改成
`wifi_tx_rate_config_t` 的 typedef 別名，**欄位逐一相同**，不影響寫法。

##### 追加：官方實測的距離數字（給 Task 6 的實測程序當對照基準）

Espressif 開發者入口自己做的 ESP-NOW 戶外實測（**ESP32-C6-DevKitM-1，板載 PCB 天線**）：

| 條件 | 一般 ESP-NOW | ESP-NOW + LR |
|---|---|---|
| 空曠、接近 100% 成功 | 約 150 m | **約 450 m** |
| 空曠、劣化區 | 300 m 時 60% | 900 m 時 40% |
| 樹林 | 125 m 已低於 50% | 約 400 m 才掉到 50%，可用約 600 m |

**規劃用的現實數字是「約 2~3 倍」，不是官方行銷的「1 公里」。**
Task 6 的實測程序若把 1 公里寫成期望值，會讓實測者把正常結果判成失敗。
另有多筆現場回報（esp-now#144 的 C6、esp32.com 的 S3）**完全量不到增益**，
所以 Task 1 的探針量出「沒有增益」時，**那是已知可能結果，不是探針寫錯**。

### 容量與資源

- **Flash 紅線**：WROOM 任一 Task 結束時若超過 **1,930,035 bytes（對 app0 的 95%）**，
  在 report 標紅並停下來回報，不要自行砍功能。
  基準（Phase 2b Task 3 後）：WROOM **1,690,003 / 83.18%**；C3 master 與 slave 見各 Task。
  arduino-cli 對 custom 分區印的百分比是拿整顆 flash 當分母，**一律自行換算成對
  app0（`0x1F0000 = 2,031,616`）的百分比**。
- **狀態 JSON 容量常數必須重算**：本階段要在 master 狀態加 `long_range_pending`
  與 `long_range_error`。Phase 4 的「決定 4.1」已經指出這個陷阱 ——
  **加欄位卻不同步加大常數時，`static_assert` 會用舊常數繼續通過，保護機制看起來還在、
  實際上已經失效**。做法見 Task 4 Step 6，一樣採「分項相加的具名常數」。
- **ESP-NOW 封包**：單包 250 bytes，`HoPacketHeader` 佔 7，payload 上限 243。
  本階段新增的 payload 都極小（`HoLrPayload` 2 bytes、心跳由 4 加到 6 bytes）。

### 專案慣例

- 結果變數用 `res`，不用 `result`
- 註釋、序列埠輸出、文件、commit 訊息一律**繁體中文**
- **不得修改** `ho_relay1/`、`ho_relay2/`、`ho_relay3/`、`CLAUDE.md`
- **`flash.ps1` 只允許一種改動：在 `$configs` 新增一個 `lrprobe` 條目**（Task 1）。
  其他任何一行都不得動 —— Phase 2a 已實測裁定「`flash.ps1` 沒有問題，
  編譯報錯先檢查 cwd」，已有兩個子代理提議要改它、都被實測駁回
- **雙板支援**：`ho_master1` 同時支援 WROOM 與 C3，`CONFIG_IDF_TARGET_ESP32C3` / `_ESP32`
  條件編譯結構不得破壞
- 驗證走 `flash.ps1` + `arduino-cli` 1.3.1，不新增外部工具鏈。
  子代理若遇 `flash.ps1` 在編譯成功時仍中止，照 `.claude/rules/flash-ps1-subagent-stderr.md`
  改用它內部的 `arduino-cli compile` 指令，**並在報告寫明用了哪一種**
- **不拆檔**（理由同 Phase 2a/2b/4：Arduino sketch 目錄下的 `.ino` 會被自動合併）

---

## 檔案結構

```
ho_lr_probe/ho_lr_probe.ino              # Task 1 新增（獨立探針，約 320 行，不進產品）
flash.ps1                                 # Task 1：只新增一個 lrprobe 條目
docs/lr-idf-behavior-findings.md          # Task 1 新增（事實表，由使用者填實測結果）
libraries/HoEspNow/src/HoEspNowProtocol.h # Task 2（+2 型別 +1 payload）、Task 5（心跳加 hbSeq）
ho_espnow_test/ho_espnow_test.ino         # Task 5（心跳 payload 大小測試要同步改）
ho_slave1/ho_slave1.ino                   # Task 2、3、5（約 +190 行）
ho_master1/ho_master1.ino                 # Task 2、4、5（約 +330 行）
ho_master1/readme.md                      # Task 6
ho_slave1/readme.md                       # Task 6
docs/phase5-field-test-procedure.md       # Task 6 新增（交付使用者執行）
docs/phase5-regression-checklist.md       # Task 6 新增
```

---

## 本計畫的六個設計決定（先讀完再開工）

實作時若發現任一決定站不住腳，**停下來回報**，不要自行改成別的做法。

---

### 決定 1：切換的時序 —— master 最後才切自己，理由與 Phase 2b 寫的不同

**做法沿用 Phase 2b：非阻塞三段握手，master 最後才套用自己。**
**但論證要換掉，因為 Phase 2b 給的理由如果成立，反而推不出這個做法。**

Phase 2b 的論證是：「混合 bitmap 下不同步只會失去距離增益，不會互相收不到，
所以逾時仍照樣切換是安全的」。這句話若成立，**先切後切根本沒差** ——
既然不同步無害，何必安排順序？所以那個做法其實是在為「B 不成立」的世界做保險，
只是計畫沒有明講。本計畫把話講清楚：

| | master 先切 | **master 最後切（本案）** |
|---|---|---|
| B 成立（混合可互通） | 沒差 | 沒差 |
| **B 不成立** | master 一切，**所有還沒收到 `LR_SET` 的 slave 立刻變聾**，而 `LR_SET` 正是要靠 ESP-NOW 送的 → **握手當場自我毀滅，一台都切不成** | 每台 slave 從「自己 ACK 完」到「master 套用」之間變聾，窗口 ≤ 握手總長 |

所以「master 最後切」不是慣例，是**在 B 不成立時唯一可行的順序**。

**那個「變聾窗口」有多長？必須算出來，因為它直接對上 30 秒門檻。**

```
t=0        使用者下 LR:ON，master 進入 LR_ANNOUNCING（master 仍是舊 bitmap）
t=0.1s     第一台 online slave 收到 LR_SET → 回 ACK → 50ms 後套用新 bitmap
           ↑ 若 B 不成立，這台從此刻起聽不到 master 的心跳
t≤10s      全部 online 都 ACK（正常約 2 秒）或逾時 10 秒
t=10s      master 套用新 bitmap（前後各 sendHeartbeatBurst()）
           ↑ 已 ACK 的 slave 從此刻起又聽得到了
```

**最壞窗口 = `LR_ANNOUNCE_TIMEOUT_MS` = 10 秒，只有 30 秒門檻的三分之一。**
這是把逾時定在 10 秒的真正理由（Phase 2b 只寫了「遠低於 30 秒」，沒說它是**上界**）。

**明確拒絕的替代方案：「排程同步切換」**（`LR_SET` 帶 `applyInMs`，兩端各自倒數、同時切）。
它能把不同步窗口從 10 秒壓到毫秒級，看起來更漂亮。**不採用**，理由：

- 10 秒已經在 30 秒門檻內留了三倍餘裕，這個方案買到的安全性是零
- 它要求兩端時鐘對齊、要處理「倒數期間收到第二個 `LR_SET`」、要處理「倒數到一半
  收到緊急關門」，三個新的競態換零收益
- **它會讓「決定 2 的緊急插隊」變得不可能** —— 一旦兩端都在倒數，master 就不能
  隨時決定不切了

### 決定 2：切換期間按下「全部關門」—— 送兩次，一次舊 bitmap 一次新 bitmap

**這是本計畫最重要的一個決定，也是使用者點名的問題。**

先確立事實：規格「Flutter App 改動」章節寫明 **App 的「全部關門」按鈕送 `ALL:ON`**
（`ALL:ON` = 廣播 pulse）。這是捕捉系統的核心動作，**不能等**。

**第一層答案（不夠，但必要）**：LR 握手全程是 `loop()` 驅動的非阻塞狀態機，
`mqttClient.loop()` 照跑，所以指令進來當下就會被處理，**不排隊、不延後、不拒絕**。
Phase 2b 的設計已經滿足這一層。

**但這一層有一個它沒回答的漏洞**：在 B 不成立的世界裡，指令進來的那一刻，
master 還在**舊 bitmap**（它最後才切），而名冊已經裂成兩半 ——
已 ACK 的在新 bitmap、還沒 ACK 的在舊 bitmap。**master 用任何一個 bitmap 廣播，
都只打得到一半的門。** 這正是「切換過程本身在製造它要解決的問題」。

**本計畫的答案：緊急指令送兩次，中間夾一次立即收尾的切換。**

```
繼電器指令進來（lrPhase == LR_ANNOUNCING）
  ├─ 1. 立刻用「目前的（舊）bitmap」照常送出        → 打到還沒 ACK 的那些台
  ├─ 2. 記下這道指令（cmd / pulseMs / 目標是全體或單台）
  ├─ 3. 立刻結束握手：不等剩下的 ACK、不等逾時，直接
  │       sendHeartbeatBurst() → esp_wifi_set_protocol(新) → sendHeartbeatBurst()
  └─ 4. 套用後 LR_RESEND_GAP_MS（150ms）再送一次同一道指令 → 打到已 ACK 的那些台
```

**兩次送出之間相隔約 150~200ms**，遠小於 Phase 1 `sendCmdToAll()` 逐台單播就會產生的
400ms 落差（規格「系統本質」點名那半秒是動物的逃脫窗口），所以這個做法**沒有把
落差擴大到規格已經明確拒絕的量級**。

**為什麼不是「中止切換、回到舊值」？** 那看起來更保守，其實更糟：
已經 ACK 並套用了新 bitmap 的那些 slave，在 B 不成立時會**永久**聽不到 master，
只能等自己的 30 秒門檻觸發 slave 端救援（決定 3）。
「中止」等於把一半的門丟給 30 秒之後的自救，「切完再補送」則是 150ms 之後全部覆蓋到。

**重複點動的副作用是可接受的**：目標若已在點動中，第二次 `pulseRelay()` 只是重設計時器；
`HO_CMD_OFF` 送兩次是冪等的。對籠門機構而言「多關一次」不會造成傷害，
**這正是「關的可靠性優先於開」的取捨方向**（規格「系統本質」）。

**防遞迴**：補送會再次呼叫 `sendCmdToAll()`／`sendCmdToSlave()`，而那兩支的開頭又會呼叫
`lrNoteRelayCommand()`。**`lrNoteRelayCommand()` 只在 `lrPhase == LR_ANNOUNCING` 時作用**，
補送發生時已經是 `LR_SETTLING`／`LR_VERIFYING`，不會再觸發第二次，迴圈自然封閉。
（Task 4 Step 4 的程式碼與註釋要把這點寫死。）

### 決定 3：切換完不算完 —— 10 秒主動驗證，不合格就自動回滾

Phase 2b 的流程做完「套用自己」就結束了。**那等於在說「我切了，希望大家都還在」。**
在一套「一次要全部關」的系統上，希望不是機制。

**本計畫加一個 `LR_VERIFYING` 階段：**

```
套用新 bitmap
  → LR_SETTLING（2 秒，讓射頻與 WiFi 事件穩定，期間 onWifiChannelMayHaveChanged()）
  → LR_VERIFYING（最多 10 秒）
       每 250ms 對一台「切換前本來就 online」的 slave 送 HO_PKT_STATE_REQ
       收到 HO_PKT_STATE 就把 lrVerifyMask 對應 bit 設起來
       ├─ 回應率 ≥ LR_VERIFY_MIN_PERCENT(60%) → 成功，LR_IDLE，寫 NVS
       └─ 低於門檻或 10 秒到 → 自動回滾：套用回舊值、寫 NVS、發 MQTT 告警、LR_IDLE
```

**為什麼是 10 秒**：整條路徑的時間預算必須在 30 秒門檻內收斂。

```
ANNOUNCING 10.0s（上界）
+ 套用自己（前後各一次心跳連發 0.6s）  1.2s
+ SETTLING                            2.0s
+ VERIFYING                          10.0s
+ 回滾套用（同樣前後各一次連發）        1.2s
                                    ─────────
                                     24.4s   距離 30 秒門檻餘 5.6s
```

**這是「整條路徑」的保守上界，而任何一台 slave 實際的失聯窗口都遠小於它**：
已 ACK 的 slave 最壞 10.6 秒（從自己 ACK 到 master 套用完成），
未 ACK 的 slave 最壞 13.2 秒（從 master 套用完成到回滾套用完成）。
兩者都不到門檻的一半。保守上界只是拿來當編譯期的護欄。

**這個算式就是所有 LR 常數的來源。改動任何一個常數，都必須重算這一行，
並確認總和 < 30000ms。** Task 4 的程式碼會用 `static_assert` 把這件事鎖在編譯期
（做法與 Phase 2b 用 `static_assert` 鎖 `statusBuf` 同源）。

**為什麼用主動 `STATE_REQ` 而不是等 `pollNextSlave()`**：後者一輪 15 秒，
20 台的話每台 750ms 才輪到一次，10 秒內只問得到 13 台。主動探測每 250ms 一台，
10 秒可以問 40 次，20 台每台至少兩次機會。

**為什麼門檻是 60% 而不是 100%**：現場本來就會有一兩台因為訊號邊緣或剛好在點動而漏答。
100% 會讓正常的切換被誤判成失敗而回滾，**而回滾本身也是一次射頻中斷** ——
用一次假警報換一次真中斷是負收益。60% 抓的是「大面積失聯」這種真正的災難，
不是「掉一兩台」。**兩台以下的名冊例外處理**：`lrExpectCount <= 2` 時門檻改成
「至少一台回應」，否則 60% 在 1 台時等於 100%、在 2 台時等於掉一台就回滾。

**回滾之後不自動重試。** `lrPhase` 回到 `LR_IDLE`，`long_range` 停在舊值，
狀態 JSON 帶 `long_range_error`（例如 `"verify_failed"`），序列埠印出哪幾台沒回應。
**刻意不做自動重試**：ON → 壞 → 回滾 → 自動再 ON 是一個會無限震盪的迴圈，
每一次震盪都是一次射頻中斷。要不要再試由人決定。

**必須誠實記錄的一項限制（寫進 readme 與回歸清單）**：
在 B 不成立的世界裡，master 回滾到舊 bitmap 之後，**那些已經套用新 bitmap 的 slave
仍然聽不到它** —— 回滾救得了 master 與「還沒切的那些」，救不了「已經切了的那些」。
**它們的恢復完全依賴 slave 端的自我救援（下面的決定 4）。**
所以：**master 端的回滾是必要的，但不是充分的；恢復保證來自 slave 端。**

### 決定 4：slave 端的自我救援 —— 失聯時先關繼電器（鐵則不打折），再對調 LR 試一次

這是整個 Phase 5 唯一「不需要任何人在現場、不需要 App、不需要 MQTT」的恢復手段，
也是決定 3 那條限制的唯一解。

**現行 slave 的失聯路徑**（`ho_slave1.ino:666-669`）：
```
30 秒沒收到心跳 → Serial.println("[失聯] 超過 30 秒沒收到心跳") → startChannelScan()
                                                                     ↓
                                                        強制關閉繼電器 + 輪掃 1~13
```
輪掃一輪 13 × 1200ms = 15.6 秒。**但如果失聯的原因是「兩端 LR bitmap 不一致」，
掃 13 個 channel 一輪都不會有用 —— master 就在原本那個 channel 上，只是聽不到。**

**加入的救援路徑（順序極重要）：**

```
30 秒沒收到心跳
  → 1. 照鐵則立刻 startChannelScan()（含強制關閉繼電器）      ← 順序不可調換
  → 2. 在掃描的「第一個 channel」停留期間，額外做一次 LR 對調：
         applyLongRange(!longRangeEnabled)，不寫 EEPROM
  → 3. 若在 LR_RESCUE_DWELL_MS(3000) 內收到心跳
         → onMasterFound() 照常鎖定；心跳自癒路徑會用 hb.longRange 校正並寫 EEPROM
  → 4. 沒收到 → 把 LR 切回原值（同樣不寫 EEPROM），繼續正常的 channel 輪掃
```

**為什麼「先關繼電器」不能為了救援而延後**：
把關閉延到救援之後，等於把「錯誤狀態的持續時間」從 30 秒延長到 33 秒。
30 秒門檻的用途是**回到已知安全狀態**，這個門檻不打折扣。
救援成功的收益（門保持在關閉狀態）不能拿「多開三秒」去換 —— 何況救援也可能失敗。
**鐵則不因為有更好的辦法就變成「差不多就好」。**

**為什麼救援期間不寫 EEPROM**：這是一次**試探**不是一次決定。
寫進去之後如果沒救回來、又剛好斷電，下次開機就用了一個沒被驗證過的值。
真正的寫入交給已經存在的自癒路徑 —— 心跳裡本來就帶著 `hb.longRange`，
一旦聽到 master，就以 master 的值為準寫檔（Task 3 Step 5）。

**為什麼是 3 秒**：必須明顯大於心跳間隔 1 秒，才能保證「只要對調是對的，
這 3 秒內必定收得到至少一次心跳」。這與 `SCAN_DWELL_MS = 1200ms` 為什麼要大於
心跳週期 1000ms 是同一個推理（設計規格「難點 1」逐字說明過），
只是這裡把餘裕拉大到 3 倍，因為對調 LR 之後射頻參數剛變、第一則心跳可能吃掉。

**每次失聯只救援一次**：`lrRescueTried` 旗標在收到心跳時清掉。
否則會變成「每輪掃描都對調一次 LR」，把一個穩定的掃描過程變成抖動。

### 決定 5：現場沒有 App 時的回滾 —— 三層，且最後一層是純韌體、不需要人

使用者點名的問題：`LR:ON` 從 App 送出，但 master 連不上 MQTT 時怎麼回去？

| 層 | 手段 | 需要什麼 | 多久生效 |
|---|---|---|---|
| 1 | **切換後 10 秒自動驗證 + 回滾**（決定 3） | 什麼都不需要 | 最壞 24.4 秒 |
| 2 | **開機守衛：LR 開著但 90 秒內一台都沒回報 → 自動關掉 LR 並寫 NVS**（本決定新增） | 什麼都不需要 | 開機後 90 秒 |
| 3 | **長按重置（實體按鈕）額外把 LR 強制關閉**（本決定新增，**修正 Phase 2b 的決定**） | 一個人、一根手指、5 秒 | 立即 |
| 4 | 序列埠 `lr off` | USB 線與電腦 | 立即 |

**第 2 層（開機守衛）的設計**：
```
setup() 套用 LR 之後記下 lrBootGuardStart = millis()
loop()  若 (longRangeEnabled && slaveCount > 0 && 本次開機從未收過任何 HO_PKT_STATE
          && millis() - lrBootGuardStart > LR_BOOT_GUARD_MS(90000))
        → applyLongRange(false); saveLongRange(false);
        → Serial.println / publishStatus，且本次開機只做一次
```
**90 秒的來由**：守衛絕對不能比**正常的恢復路徑**還早開槍。
規格「難點 1」算過最壞恢復時間是「30 秒失聯門檻 + 一輪掃描 15.6 秒 = 45.6 秒」。
90 秒給了將近兩倍餘裕，所以「slave 只是正在輪掃、還沒回來」不會被誤判成災難。

**誤判的代價刻意設計成無害**：如果 slave 只是全部斷電、而 LR 其實是好的，
守衛會把 LR 關掉 —— 而 slave 復電後收到心跳、發現 `hb.longRange` 與自己存的不同，
心跳自癒路徑會直接跟著關。**兩端仍然一致，只是失去距離增益。**
「誤判的後果只是失去增益」正是這個守衛敢做得這麼積極的原因。

**第 3 層是對 Phase 2b 決定的實質修正，必須寫進 commit 訊息：**

Phase 2b 把 LR 存進 `homaster/lr` 而非 `hoban`，理由是
「`reset` 清掉 master 的 LR 但清不掉 slave EEPROM 的，會造成永久不一致」。
**存放位置的決定仍然正確、照舊沿用**（`reset` 是「重設網路設定」，此時人有 App 有其他手段，
不該順手改動一個無關的射頻設定）。

**但「不一致會永久存在」這個前提，在本階段已經被決定 4 推翻了。**
slave 現在會在失聯 30 秒後自己對調 LR 試一次，master 單方面關掉 LR 不再造成永久失聯。
於是：

> **長按重置（實體、5 秒、現場唯一不需要任何工具的手段）在清除設定的同時，
> 一併把 LR 強制關閉並寫入 NVS。**

理由：長按重置的語義本來就是「把設備弄回最保守的已知可用狀態」，
而 LR 正是最可能讓 master 連不上 slave 的設定。**這條路徑必須留在 `homaster` 的寫入，
不能只是 `clearNetConfig()`** —— Task 4 Step 7 會在長按重置的確認分支明確補一行
`saveLongRange(false);`，並在註釋說明它為什麼與 `reset` 指令刻意不同。

### 決定 6：實測要量的是「一次全部關的到達率」，不是「ping 得到」

規格的 Phase 5 驗收是「記錄標準模式與 LR 模式的實際可用距離，決定出貨預設值」。
**「可用距離」必須被定義成一個可量測的東西，否則實測會退化成「走到收不到為止」。**

本計畫定義三個量測項，**優先序由高到低**，全部由韌體自己算、自己印，
**不依賴任何人在幾百公尺外判讀**：

| # | 指標 | 誰量 | 為什麼要它 |
|---|---|---|---|
| **1** | **「全部關」到達率**：master 連跑 N 輪廣播關門，每輪事後逐台查狀態，統計每台的成功輪數 | master（`lrtest <n>` 指令） | **這是這套系統真正要的數字。** 廣播沒有 ACK，只有實際去查才知道到底關上沒有 |
| 2 | **心跳遺失率**：slave 用心跳序號差值算真實遺失率（不依賴任何間隔假設） | slave（每 60 秒自動印一行） | 廣播下行的純粹指標，與「全部關」走完全相同的無線電路徑 |
| 3 | **RSSI**：min／avg／max | 兩端都有 | 只是輔助。**RSSI 好不代表封包會到**，不得單獨用它下結論 |

**為什麼指標 1 必須是 master 端自動列表**：人在 500 公尺外看不到序列埠，
也不可能可靠地數「剛才那 30 次裡繼電器動了幾次」。把統計放在 master 端，
基地台那個人看著表格念數字給遠端的人聽，是唯一能規模化的做法。

**為什麼指標 2 需要在心跳裡加一個序號**：現行心跳沒有序號，slave 只能用
「收到則數 ÷ 經過秒數」推算遺失率 —— 而 master 在 WiFi 關聯期間會把心跳間隔從
1000ms 改成 200ms（`HEARTBEAT_INTERVAL_ASSOC`），推算值會在關聯期間整個失真。
加一個 `uint16_t hbSeq` 之後，遺失率 = `1 - 收到數 / (最後seq - 第一seq + 1)`，
**完全不依賴任何對發送間隔的假設**。這是 2 bytes 換一個誠實的數字。

**心跳 payload 加長的相容性（必須寫進 readme 的燒錄順序）**：
兩端的檢查都是 `payloadLen >= sizeof(HoHeartbeatPayload)`，所以

- **新 master → 舊 slave**：舊 slave 檢查 `6 >= 4` 通過，`memcpy` 只取前 4 bytes → **相容**
- **舊 master → 新 slave**：新 slave 檢查 `4 >= 6` 失敗 → **丟棄心跳 → 30 秒後強制關閉繼電器**

**所以燒錄順序是有方向性的：先燒 master、再燒 slave。** 反過來會在中間那段時間開籠。
（若 Phase 4 已執行過，`HO_ESPNOW_VERSION` 已是 2 且 CRC 涵蓋標頭；本階段
**不再升版本號**，因為 payload 只增不減、接收端一律用 `>=`，屬向前相容的擴充。
這條「payload 只增不減」的規則要寫進 `HoEspNowProtocol.h` 的註釋。）

---

## Task 依賴圖（不得有循環）

```
Task 1（探針：把三個未驗證的 IDF 行為量成事實）
   └→ Task 2（協定 + 兩端 applyLongRange + 持久化；只做「開機依儲存值套用」）
          └→ Task 3（slave：接收 LR_SET、心跳自癒、失聯自我救援）
                 └→ Task 4（master：握手 + 緊急插隊 + 驗證 + 回滾 + 開機守衛 + MQTT/序列埠介面）
                        └→ Task 5（實測儀器：心跳序號、遺失率統計、lrtest 全關可靠度）
                               └→ Task 6（實測程序文件、回歸清單、readme）
```

**Task 1 是硬性關卡**：它的產出 `docs/lr-idf-behavior-findings.md` 需要**使用者在實體硬體上
執行探針**才會有內容。子代理做不到現場執行，所以：

> **Task 1 完成 = 探針 sketch 編譯通過 + findings 文件的「待填」骨架就緒 + 明確的執行說明。
> Task 2 開工前必須先讀 findings 文件；若它仍是空白的骨架，
> 在 report 中明確標示「以下設計以宣稱 B 未經驗證為前提，全部路徑都不依賴 B」，
> 然後照計畫繼續。** 本計畫的每一個設計都刻意不依賴 B，正是為了讓這個關卡
> 不會把整個階段卡死（見「決定 1」的重新論證）。

**每個 Task 結束時三種型號都必須能單獨編譯通過**（Task 1 之後四種，多一個 `lrprobe`）。
Phase 2a 踩過「Task 引用尚未存在的函式」的坑，本計畫的做法是
**先寫最小可運作版本、後續 Task 擴充**，且刻意把 slave 排在 master 之前
（master 要送 `LR_SET`，slave 得先收得到；反過來不成立）。

---

## Task 1：用最小探針把三個未驗證的 IDF 行為量成事實

**本 Task 刻意不實作任何功能。** Phase 2a 的教訓是「未驗證的 IDF 行為被寫成事實」
會一路傳下去（`WiFi.begin()` 帶 channel 的掃描語義那次，錯的不只是註釋，
連回歸清單的失敗判定都跟著錯）。所以 Phase 5 的第一件事是**先量**。

探針刻意**不使用** `HoEspNowProtocol`：它要量的是底層射頻行為，
若共用了會隨 Phase 4／Phase 5 改動的協定函式庫，量出來的東西就分不清是誰造成的。
探針自己帶一個 6 bytes 的極簡封包格式。

**Files:**
- Create: `ho_lr_probe/ho_lr_probe.ino`
- Create: `docs/lr-idf-behavior-findings.md`
- Modify: `flash.ps1`（**只新增 `lrprobe` 一個條目，其他一行都不准動**）

**Interfaces:**
- Consumes：無（完全獨立）
- Produces：`docs/lr-idf-behavior-findings.md` 的實測結論 → Task 2~6 全部以它為輸入

---

- [ ] **Step 1: 建立探針 sketch**

```cpp
// 齁控 Long Range 行為探針 —— Phase 5 Task 1
//
// 這不是產品韌體，是一支只為了回答三個問題而存在的量測工具：
//   A. esp_wifi_set_protocol() 在「已關聯 AP」的狀態下呼叫，會不會造成 STA 斷線？
//   B. 兩端 protocol bitmap 不同時（一端有 LR、一端沒有），ESP-NOW 到底還通不通？
//   C. ESP32-C3 到底支不支援 WIFI_PROTOCOL_LR？
//
// 為什麼要有這支東西：Phase 2a 已經因為「未驗證的 IDF 行為被寫成事實」踩過一次，
// 錯的不只是註釋，連回歸清單的失敗判定都跟著錯，會讓實測者把正確行為判成 FAIL。
// 問題 B 更是 Phase 2b「決定 5」的整個地基 —— 若它不成立，
// 「握手等不到就照樣切」會從安全行為變成「照樣把那台切成聾子」。
//
// 刻意不 include HoEspNowProtocol.h：要量的是底層射頻行為，
// 共用一個會隨 Phase 4／5 改動的協定函式庫，量出來的東西就分不清是誰造成的。
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// esp_now_set_peer_rate_config() 在 Arduino core 3.3.7（IDF 5.x）應該存在，
// 但本專案從未用過。若編譯失敗，把這個巨集改成 0 讓其餘測項照樣可用，
// 並在 report 明確寫出「rate config 在本工具鏈編不過」——那本身就是一項實測結論。
#define PROBE_TEST_PEER_RATE 1

const char* probeVersion = "1.0.0";

// ── 探針自帶的極簡封包（6 bytes）──
struct __attribute__((packed)) ProbePacket {
  uint8_t  magic0;   // 'L'
  uint8_t  magic1;   // 'R'
  uint32_t seq;
};

uint8_t peerMac[6] = { 0 };
bool peerSet = false;
const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

uint32_t txSeq = 0;

// 接收統計
uint32_t rxCount = 0;
uint32_t rxFirstSeq = 0;
uint32_t rxLastSeq  = 0;
bool     rxHaveFirst = false;
long     rssiSum = 0;
int      rssiMin = 0;
int      rssiMax = -200;

// 送出結果統計（單播才有意義，廣播一律回 SUCCESS）
uint32_t txOk = 0, txFail = 0;

// 非阻塞連送
uint32_t burstRemaining = 0;
bool     burstBroadcast = false;
unsigned long burstLastAt = 0;
const unsigned long BURST_GAP_MS = 100;

void printProto(const char* tag) {
  uint8_t p = 0;
  esp_err_t res = esp_wifi_get_protocol(WIFI_IF_STA, &p);
  Serial.printf("[探針] %s protocol 讀回 = 0x%02x（11B=%d 11G=%d 11N=%d LR=%d）res=%d\n",
                tag, p,
                (p & WIFI_PROTOCOL_11B) ? 1 : 0,
                (p & WIFI_PROTOCOL_11G) ? 1 : 0,
                (p & WIFI_PROTOCOL_11N) ? 1 : 0,
                (p & WIFI_PROTOCOL_LR)  ? 1 : 0,
                (int)res);
}

void printLink(const char* tag) {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);
  Serial.printf("[探針] %s wifi=%s ip=%s ch=%u rssi=%d heap=%u t=%lu\n",
                tag,
                WiFi.isConnected() ? "已連線" : "未連線",
                WiFi.localIP().toString().c_str(),
                primary, (int)WiFi.RSSI(),
                (unsigned)ESP.getFreeHeap(), millis());
}

// 三種 bitmap，對應三種測試組態
uint8_t protoFromName(const String& name, bool& okOut) {
  okOut = true;
  if (name == "bgn")   return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  if (name == "bgnlr") return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR;
  if (name == "lr")    return WIFI_PROTOCOL_LR;   // 純 LR：規格說 master 不能用，這裡只是要量它的行為
  okOut = false;
  return 0;
}

void doSetProto(const String& name) {
  bool ok = false;
  uint8_t proto = protoFromName(name, ok);
  if (!ok) {
    Serial.println("[探針] 用法：proto bgn | proto bgnlr | proto lr");
    return;
  }
  printProto("套用前");
  printLink("套用前");
  Serial.printf("[探針] >>> 呼叫 esp_wifi_set_protocol(WIFI_IF_STA, 0x%02x) <<<\n", proto);
  unsigned long t0 = millis();
  esp_err_t res = esp_wifi_set_protocol(WIFI_IF_STA, proto);
  Serial.printf("[探針] esp_wifi_set_protocol 回傳 %d，耗時 %lu ms\n", (int)res, millis() - t0);
  printProto("套用後");
  printLink("套用後");
  if (peerSet) {
    Serial.printf("[探針] 套用後 peer 是否仍存在：%s\n",
                  esp_now_is_peer_exist(peerMac) ? "是" : "否");
  }
  Serial.println("[探針] 接下來 30 秒請觀察有沒有 WiFi 事件被印出（沒有事件＝沒斷線）");
}

void resetStats() {
  rxCount = 0; rxHaveFirst = false; rxFirstSeq = 0; rxLastSeq = 0;
  rssiSum = 0; rssiMin = 0; rssiMax = -200;
  txOk = 0; txFail = 0;
  Serial.println("[探針] 統計已歸零");
}

void printStats() {
  if (!rxHaveFirst) {
    Serial.printf("[探針] 收到 0 封（送出成功 %u／失敗 %u）\n",
                  (unsigned)txOk, (unsigned)txFail);
    return;
  }
  uint32_t span = rxLastSeq - rxFirstSeq + 1;
  float loss = (span > 0) ? (100.0f * (span - rxCount) / (float)span) : 0.0f;
  Serial.printf("[探針] 收到 %u／區間 %u（seq %u~%u）遺失 %.1f%%　rssi 平均 %ld 最好 %d 最差 %d"
                "　送出成功 %u／失敗 %u\n",
                (unsigned)rxCount, (unsigned)span, (unsigned)rxFirstSeq, (unsigned)rxLastSeq,
                loss, rssiSum / (long)rxCount, rssiMax, rssiMin,
                (unsigned)txOk, (unsigned)txFail);
}

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < (int)sizeof(ProbePacket)) return;
  ProbePacket p;
  memcpy(&p, data, sizeof(p));
  if (p.magic0 != 'L' || p.magic1 != 'R') return;

  int rssi = info->rx_ctrl->rssi;
  if (!rxHaveFirst) { rxFirstSeq = p.seq; rxHaveFirst = true; rssiMin = rssi; rssiMax = rssi; }
  rxLastSeq = p.seq;
  rxCount++;
  rssiSum += rssi;
  if (rssi < rssiMin) rssiMin = rssi;
  if (rssi > rssiMax) rssiMax = rssi;

  // 每 10 封印一行，避免序列埠被洗版而看不到 WiFi 事件
  if (rxCount % 10 == 1) {
    Serial.printf("[探針] 收到 seq=%u rssi=%d（累計 %u）\n",
                  (unsigned)p.seq, rssi, (unsigned)rxCount);
  }
}

void onSent(const wifi_tx_info_t* txInfo, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) txOk++; else txFail++;
}

void addPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  esp_err_t res = esp_now_add_peer(&peer);
  Serial.printf("[探針] esp_now_add_peer 回傳 %d\n", (int)res);
}

void sendOne(bool broadcast) {
  ProbePacket p;
  p.magic0 = 'L'; p.magic1 = 'R'; p.seq = txSeq++;
  const uint8_t* dst = broadcast ? BCAST : peerMac;
  esp_err_t res = esp_now_send(dst, (const uint8_t*)&p, sizeof(p));
  if (res != ESP_OK) {
    Serial.printf("[探針] esp_now_send 立即失敗 %d（seq=%u）\n", (int)res, (unsigned)p.seq);
  }
}

#if PROBE_TEST_PEER_RATE
void tryPeerRate() {
  if (!peerSet) { Serial.println("[探針] 先用 peer <mac12> 設定對象"); return; }
  // 這一段的存在意義就是「量它能不能編、能不能跑」。
  // 規格「難點 2」把 esp_now_set_peer_rate_config() + WIFI_PHY_RATE_LORA_250K
  // 列為「混合模式測不出差異時的備案」，但從未驗證過它在本工具鏈是否可用。
  esp_now_rate_config_t cfg = {};
  cfg.phymode = WIFI_PHY_MODE_LR;
  cfg.rate    = WIFI_PHY_RATE_LORA_250K;
  cfg.ersu    = false;
  cfg.dcm     = false;
  esp_err_t res = esp_now_set_peer_rate_config(peerMac, &cfg);
  Serial.printf("[探針] esp_now_set_peer_rate_config(LR, LORA_250K) 回傳 %d\n", (int)res);
}
#endif

void printHelp() {
  Serial.println("── LR 探針指令 ──");
  Serial.println("  mac                 印出本機 MAC");
  Serial.println("  peer <mac12>        設定對象（12 個 hex，不含冒號）");
  Serial.println("  proto bgn|bgnlr|lr  切換 protocol bitmap，並印出前後的連線狀態");
  Serial.println("  getproto            只讀回目前 bitmap");
  Serial.println("  link                印出目前 WiFi／channel／RSSI");
  Serial.println("  wifi <ssid> <pass>  連上 AP（測項 A 用）");
  Serial.println("  ch <n>              手動設定 channel（不連 AP 時用）");
  Serial.println("  send <n>            對 peer 單播 n 封（每 100ms 一封）");
  Serial.println("  bcast <n>           廣播 n 封（每 100ms 一封）");
  Serial.println("  stats               印出收送統計");
  Serial.println("  clear               統計歸零");
#if PROBE_TEST_PEER_RATE
  Serial.println("  rate                嘗試 esp_now_set_peer_rate_config(LR,250K)");
#endif
  Serial.println("  help                顯示這份說明");
}

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;
  int sp = line.indexOf(' ');
  String verb = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? "" : line.substring(sp + 1);
  rest.trim();

  if (verb == "mac") {
    uint8_t m[6]; WiFi.macAddress(m);
    Serial.printf("[探針] 本機 MAC = %02x%02x%02x%02x%02x%02x\n",
                  m[0], m[1], m[2], m[3], m[4], m[5]);
  } else if (verb == "peer") {
    if (rest.length() != 12) { Serial.println("[探針] 需要 12 個 hex 字元"); return; }
    for (int i = 0; i < 6; i++) {
      peerMac[i] = (uint8_t)strtoul(rest.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    }
    peerSet = true;
    addPeer(peerMac);
    Serial.printf("[探針] peer 設定為 %s\n", rest.c_str());
  } else if (verb == "proto") {
    doSetProto(rest);
  } else if (verb == "getproto") {
    printProto("目前");
  } else if (verb == "link") {
    printLink("目前");
  } else if (verb == "wifi") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) { Serial.println("[探針] 用法：wifi <ssid> <pass>"); return; }
    String ss = rest.substring(0, s2), pw = rest.substring(s2 + 1);
    Serial.printf("[探針] 連線到 %s …\n", ss.c_str());
    WiFi.begin(ss.c_str(), pw.c_str());
  } else if (verb == "ch") {
    int n = rest.toInt();
    if (n < 1 || n > 13) { Serial.println("[探針] channel 需 1~13"); return; }
    esp_err_t res = esp_wifi_set_channel((uint8_t)n, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[探針] esp_wifi_set_channel(%d) 回傳 %d\n", n, (int)res);
  } else if (verb == "send" || verb == "bcast") {
    long n = rest.toInt();
    if (n <= 0 || n > 100000) { Serial.println("[探針] 數量需 1~100000"); return; }
    if (verb == "send" && !peerSet) { Serial.println("[探針] 先設 peer"); return; }
    burstBroadcast = (verb == "bcast");
    burstRemaining = (uint32_t)n;
    burstLastAt = 0;
    Serial.printf("[探針] 開始%s %ld 封（每 %lu ms 一封，約 %ld 秒）\n",
                  burstBroadcast ? "廣播" : "單播", n, BURST_GAP_MS, n * (long)BURST_GAP_MS / 1000);
  } else if (verb == "stats") {
    printStats();
  } else if (verb == "clear") {
    resetStats();
#if PROBE_TEST_PEER_RATE
  } else if (verb == "rate") {
    tryPeerRate();
#endif
  } else if (verb == "help") {
    printHelp();
  } else {
    Serial.printf("[探針] 未知指令：%s\n", verb.c_str());
  }
}

// WiFi 事件全部印出來，時間戳是測項 A 的主要證據
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[事件] id=%d t=%lu", (int)event, millis());
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf(" STA_DISCONNECTED reason=%u", info.wifi_sta_disconnected.reason);
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.print(" STA_CONNECTED");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.print(" STA_GOT_IP");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.printf("齁控 LR 探針 v%s\n", probeVersion);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  Serial.println("晶片：ESP32-C3");
#elif defined(CONFIG_IDF_TARGET_ESP32)
  Serial.println("晶片：ESP32 (WROOM)");
#else
  Serial.println("晶片：其他");
#endif

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[探針] esp_now_init 失敗");
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  addPeer(BCAST);

  printProto("開機預設");
  printLink("開機");
  printHelp();
}

void loop() {
  // 非阻塞連送：每 BURST_GAP_MS 一封，讓序列埠與 WiFi 事件全程可觀察
  if (burstRemaining > 0 && millis() - burstLastAt >= BURST_GAP_MS) {
    burstLastAt = millis();
    sendOne(burstBroadcast);
    burstRemaining--;
    if (burstRemaining == 0) {
      Serial.println("[探針] 連送結束");
      printStats();
    }
  }

  static String buf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length() > 0) { handleCommand(buf); buf = ""; }
    } else {
      buf += c;
      if (buf.length() > 96) buf = "";
    }
  }
}
```

- [ ] **Step 2: `flash.ps1` 新增 `lrprobe` 條目**

**只加這一段，其他一行都不准動。** 分區用 `default`（探針不需要 OTA 雙槽），
與既有的 `test` 型號一致。

```powershell
    'lrprobe' = @{
        Dir  = 'ho_lr_probe'
        # 同一份 sketch 兩種晶片都要跑（測項 C 要比對 WROOM 與 C3 對 LR 的支援差異），
        # 這裡放 C3；WROOM 版直接用下面的 arduino-cli 指令另外編一次即可。
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=default,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'Long Range 行為探針（Phase 5 Task 1，非產品韌體）'
    }
```

WROOM 版（測項 C 要兩種晶片對照）另外用這條指令編：

```
A:\server\arduino-cli\arduino-cli.exe compile --fqbn esp32:esp32:esp32 --libraries A:\project\hoctrl_arduino\libraries --output-dir A:\project\hoctrl_arduino\ho_lr_probe\build\wroom A:\project\hoctrl_arduino\ho_lr_probe
```

- [ ] **Step 3: 建立 `docs/lr-idf-behavior-findings.md` 的待填骨架**

**這份文件是本 Task 真正的交付物。** 骨架要把「怎麼測、預期看到什麼、
結果會影響哪個設計決定」三件事寫清楚，讓使用者照著做就能填完。

文件開頭必須有：

```
> 狀態：⬜ 尚未執行任何一項　執行者：　日期：
> 韌體：ho_lr_probe v1.0.0　工具鏈：arduino-cli 1.3.1、Arduino ESP32 core 3.3.7
> 本文件被填寫之前，Phase 5 的所有設計都刻意不依賴測項 B 的結果（見計畫「決定 1」）。
```

**測項 A：`esp_wifi_set_protocol()` 在已關聯 AP 時呼叫會不會斷線**（一片板子即可）
1. `wifi <ssid> <pass>` → 等 `[事件] ... STA_GOT_IP` → `link` 記下 ip 與 ch
2. `proto bgnlr`
3. **接下來 30 秒不要輸入任何指令**，只看序列埠有沒有出現 `[事件]`
4. 30 秒後 `link`、`getproto`
5. 記錄：有沒有 `STA_DISCONNECTED`（有的話 reason 多少）、ip 有沒有變、ch 有沒有變、
   `esp_wifi_set_protocol` 本身耗時幾 ms
6. 用 `proto bgn` 再做一次（切回去也是一次呼叫，行為可能不同）
7. **`proto lr`（純 LR）也做一次** —— 規格「難點 2」宣稱純 LR 連不上一般 AP，
   這一步是驗證那句話，順便量「已連上之後改成純 LR 會怎樣」

**測項 B：兩端 bitmap 不同時 ESP-NOW 還通不通**（兩片板子，都不連 AP）← **最重要**
1. 兩片都 `ch 6`（固定同一個 channel，排除 channel 變因）
2. 各自 `mac` 取得 MAC，互相 `peer <對方 mac>`
3. 依下表逐格執行：每格先兩邊 `clear`，A 執行 `bcast 100`（約 10 秒），
   結束後 B 執行 `stats`；接著 A 執行 `send 100`，B 再 `stats`
4. **兩片板子擺在同一張桌上、相距約 1 公尺**，排除距離變因 ——
   這一格量的是「通不通」，不是「多遠」

| A 的 bitmap | B 的 bitmap | 廣播收到／100 | 單播收到／100 | 備註 |
|---|---|---|---|---|
| bgn | bgn | | | 基準線，必須接近 100 |
| bgnlr | bgnlr | | | 兩端都開 LR |
| **bgnlr** | **bgn** | | | **← 這一格就是宣稱 B** |
| **bgn** | **bgnlr** | | | **← 反方向也要測** |
| lr（純） | bgn | | | 純 LR 對非 LR |
| lr（純） | lr（純） | | | 純 LR 對純 LR |

> **判讀規則（寫進文件，不要靠實測者自己判斷）**：
> 第 3、4 格接近 100 → 宣稱 B **成立**，Phase 2b「決定 5」的
> 「逾時仍照樣套用是安全的」可以繼續沿用。
> 接近 0 → 宣稱 B **不成立**，計畫的決定 1／2／3／4 全部從「保險」升級成「必要」，
> 且 readme 與回歸清單裡任何寫「不同步只會失去距離增益」的句子都必須刪掉。
> **介於中間（例如 30~70）一律視為不成立處理** —— 一個時通時不通的通道，
> 在「一次要全部關」的系統上等同不通。

**測項 C：ESP32-C3 是否支援 `WIFI_PROTOCOL_LR`**（WROOM 與 C3 各一片）
1. `proto bgnlr` → `getproto`
2. 記錄讀回的 bitmap 有沒有 LR 位元、`esp_wifi_set_protocol` 的回傳值
3. **若 C3 讀回沒有 LR 位元或回傳非 0 → slave 端（全部是 C3）根本做不了 LR，
   停下來回報，整個 Phase 5 的價值假設要重新評估**

> **文獻預期**：官方文件寫「only ESP32 chip series devices (except ESP32-C2) can transmit
> and receive the LR data」，例外只有 C2，所以 C3 **應該**讀得回 LR 位元。
> 這一格若與文件不符，那本身就是一項重要發現，務必連 core 版本一起記下來。

**測項 D：`esp_now_set_peer_rate_config()` 可用性**
`peer <mac>` → `rate` → 記錄回傳值。
若必須把 `PROBE_TEST_PEER_RATE` 改成 0 才編得過，**那本身就是結論**，照實記錄。

文件最後留一節「對 Phase 5 設計的影響（實測後回填）」，
明確列出每個測項的結果會改動計畫的哪一個決定。

- [ ] **Step 4: 編譯驗證**

```powershell
.\flash.ps1 -Model lrprobe
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
```
再加上 WROOM 版探針的 `arduino-cli compile`（見 Step 2）。**五者皆 exit code 0。**

master／master-c3／slave 這三個本 Task 沒改動，編它們是為了確立 Task 2 的比較基準
（`flash.ps1` 的新增條目不可能影響它們，但基準數字要記下來）。

**report 必須記錄**：五種編譯的 flash 位元組數與 RAM 百分比。
探針用 `default` 分區，app 槽大小與其他型號不同，**百分比不可與其他型號互相比較**，
只記絕對值即可；master／slave 一律換算成對 app0（2,031,616）的百分比。

- [ ] **Step 5: Commit**

commit 訊息必須包含：為什麼第一個 Task 是量測而不是實作（Phase 2a 的教訓）、
三個測項各自的內容、宣稱 B 為什麼是 Phase 2b「決定 5」的地基、
以及「`flash.ps1` 只新增一個條目、其他一行未動」。

---

## Task 2：協定擴充與兩端的 `applyLongRange()`（只做「開機依儲存值套用」）

本 Task 結束後三個編譯目標都能單獨燒錄，**行為與現在完全相同**（預設值是關閉）。
這是刻意的：先把「套用」這件事做到可驗證，再讓任何人有能力去切換它。

**Files:**
- Modify: `libraries/HoEspNow/src/HoEspNowProtocol.h`
- Modify: `ho_master1/ho_master1.ino`
- Modify: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes：Task 1 的 `docs/lr-idf-behavior-findings.md`（**開工前必讀**）
- Produces（Task 3~5 全部依賴）：
  - `HO_PKT_LR_SET` (0x30) / `HO_PKT_LR_ACK` (0x31) / `HoLrPayload`
  - master 與 slave 各一份 `bool applyLongRange(bool enable)`
  - master：`void saveLongRange(bool)`、全域 `bool lrRadioValue`
  - slave：`bool longRangeEnabled`、`bool lrRadioValue`、EEPROM 的 `EE_ADDR_LONGRANGE` 真的被讀寫

---

- [ ] **Step 0: 讀 Task 1 的實測結果**

`docs/lr-idf-behavior-findings.md`。三種情況：

| findings 的狀態 | 怎麼做 |
|---|---|
| 測項 C 顯示 **C3 不支援 LR** | **停下來回報。** slave 全部是 C3，整個 Phase 5 的價值假設要重新評估，不要繼續往下寫 |
| 測項 B 顯示宣稱 **不成立** | 照計畫繼續（本計畫的每個決定都不依賴 B），但 Task 6 的 readme 與回歸清單**不得出現**「不同步只會失去距離增益」這句話 |
| 文件仍是空白骨架 | 照計畫繼續，並在 report 明確寫出「以下設計以宣稱 B 未經驗證為前提」 |

- [ ] **Step 1: 協定擴充**

`libraries/HoEspNow/src/HoEspNowProtocol.h` 的 `HoPacketType` 補兩個型別
（值刻意選 0x30/0x31，與 Phase 4 的 OTA 群組 0x20~0x23 分開）：

```c
  HO_PKT_LR_SET    = 0x30,  // master → slave：切換 Long Range，帶目標值
  HO_PKT_LR_ACK    = 0x31,  // slave  → master：已受理並承諾套用
```

payload：

```c
// Long Range 切換。
// LR_SET 由 master 送出（applied 填 0），LR_ACK 由 slave 回覆。
//
// ⚠ applied 的語義是「已受理並承諾在 LR_APPLY_DELAY_MS 之內套用」，
//    「不是」「已經套用完成」。這個差別是刻意的：
//    slave 必須「先回 ACK、後套用」——若順序相反，而兩端 bitmap 不同時真的
//    收不到彼此（Task 1 測項 B 若不成立），slave 一套用就再也送不出 ACK，
//    master 必定逾時，於是每一次切換都會走進逾時路徑並漏記所有已切換的 slave。
struct __attribute__((packed)) HoLrPayload {
  uint8_t longRange;   // 目標值：1 = 開啟
  uint8_t applied;
};
```

同時在檔案的協定常數區加一段相容性規則（Task 5 會靠它加長心跳 payload）：

```c
// ── payload 的相容性規則（新增欄位前務必讀）──
// 兩端收包一律用 `payloadLen >= sizeof(XxxPayload)` 檢查，所以
// 「只在既有 payload 的尾端加欄位」是向前相容的：舊的接收端會用 `>=` 通過檢查、
// memcpy 只取它認得的前幾個 bytes。反過來（減欄位、改欄位順序、改欄位型別）
// 一律是破壞性改動，必須升 HO_ESPNOW_VERSION。
//
// 但相容是有方向的：新版發送端 → 舊版接收端 OK；舊版發送端 → 新版接收端會
// 因為長度不足而被丟棄。對心跳而言「被丟棄」＝ slave 30 秒後強制關閉繼電器＝
// 籠門被打開。**所以任何加長 payload 的改版，燒錄順序一律是「先 master、後 slave」。**
```

`.cpp` 不用改（打包／解包是型別無關的）。

- [ ] **Step 2: 兩端共用的 `applyLongRange()`（master 與 slave 各寫一份，內容一致）**

**與 Phase 2b Task 6 的版本的關鍵差異：加上 `esp_wifi_get_protocol()` 讀回驗證。**
`esp_wifi_set_protocol()` 回 `ESP_OK` 只代表「呼叫被接受」，不代表「這顆晶片真的支援 LR」。
Task 1 測項 C 要量的就是這件事，而讀回驗證讓**產品韌體自己也看得見**。

```cpp
// 目前射頻裡實際生效的 LR 值。刻意與「儲存／相信的值」分成兩個變數：
// slave 端的失聯救援會在「不改儲存值」的前提下暫時對調射頻設定（見 Task 3 決定 4），
// 那段期間兩者必然不同；只有一個變數就寫不出那條路徑。
bool lrRadioValue = false;

// 套用 Long Range 設定。
//
// 一律用「11b/g/n + LR」混合 bitmap，兩端都是。
// master 非混合不可：純 WIFI_PROTOCOL_LR 連不上一般 AP，MQTT 會整個斷掉
//（設計規格「難點 2」）。slave 本來可以用純 LR，但刻意也用混合，
// 讓兩端的設定完全對稱 —— 不對稱會多出一整類「只有某一端會發生」的行為，
// 而這類行為在只有兩片板子的現場最難診斷。
//
// 呼叫成功之後一定要用 esp_wifi_get_protocol() 讀回來對一次：
// set 回 ESP_OK 只代表「呼叫被接受」，不代表這顆晶片真的支援 LR。
// 讀回值與寫入值不同時本函式回傳 false，呼叫端據此不更新 longRangeEnabled，
// 於是「以為開了其實沒開」這種狀態在本專案不可能存在。
bool applyLongRange(bool enable) {
  uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  if (enable) proto |= WIFI_PROTOCOL_LR;

  esp_err_t res = esp_wifi_set_protocol(WIFI_IF_STA, proto);
  if (res != ESP_OK) {
    Serial.printf("⚠ [LR] esp_wifi_set_protocol(0x%02x) 失敗，錯誤碼 %d\n", proto, (int)res);
    return false;
  }

  uint8_t readBack = 0;
  esp_err_t res2 = esp_wifi_get_protocol(WIFI_IF_STA, &readBack);
  if (res2 != ESP_OK) {
    Serial.printf("⚠ [LR] esp_wifi_get_protocol 失敗，錯誤碼 %d，無法確認是否生效\n", (int)res2);
    return false;
  }
  if (readBack != proto) {
    Serial.printf("⚠ [LR] 讀回的 bitmap 0x%02x 與寫入的 0x%02x 不同，本晶片可能不支援 LR，"
                  "維持原設定\n", readBack, proto);
    return false;
  }

  lrRadioValue = enable;
  Serial.printf("[LR] 已套用%s（bitmap=0x%02x，讀回一致）\n", enable ? "開啟" : "關閉", proto);
  return true;
}
```

> **實作注意**：`ho_slave1.ino` 已經 `#include <esp_wifi.h>`，`ho_master1.ino` 也有，
> 兩邊都不需要新的 include。

- [ ] **Step 3: master 端持久化（NVS `homaster/lr`）**

```cpp
// LR 存 homaster 而非 hoban，理由與 Phase 2a 把 espch 放進 homaster 完全同源：
// 它服務的是 ESP-NOW 與 slave，跟名冊同生共死。放 hoban 會被 clearNetConfig()
// （MQTT 的 reset 指令）清掉，而 reset 的語義是「重設網路設定」——
// 此時操作者手上有 App、有其他手段，不該順手改動一個無關的射頻設定。
//
// 注意這條理由「不」涵蓋實體長按重置：那是現場唯一不需要任何工具的手段，
// 它的語義是「把設備弄回最保守的已知可用狀態」，所以它反而必須把 LR 關掉。
// 見 Task 4 Step 7（那是對 Phase 2b 決定的實質修正）。
uint8_t savedLongRange = 0xFF;   // NVS 現值，避免沒變也重複寫入磨損 flash

void saveLongRange(bool enable) {
  uint8_t v = enable ? 1 : 0;
  if (v == savedLongRange) return;
  prefs.begin("homaster", false);
  prefs.putUChar("lr", v);
  prefs.end();
  savedLongRange = v;
  Serial.printf("[LR] 已寫入 NVS: %s\n", enable ? "開啟" : "關閉");
}
```

`loadSlaves()` 在 `slaveLockChannel = prefs.getUChar("espch", 0);` 那一行旁邊補：
```cpp
  longRangeEnabled = prefs.getUChar("lr", 0) != 0;
```
`prefs.end()` 之後補：
```cpp
  savedLongRange = longRangeEnabled ? 1 : 0;
  Serial.printf("[LR] 開機載入設定：%s\n", longRangeEnabled ? "開啟" : "關閉");
```

`setup()` 在 `registerAllPeers();` **之後**、`WiFi.onEvent(onWiFiEvent);` **之前**插入：
```cpp
  // 套用 LR 必須排在 setupEspNow() 之後（esp_wifi_set_protocol() 需要 WiFi 已 start，
  // 而 setupEspNow() 第一行的 WiFi.mode(WIFI_STA) 才會 start 它），
  // 也必須排在 connectToWiFi() 之前 —— 讓關聯 AP 這件事一開始就用最終的 bitmap 進行，
  // 避免「先關聯、再改 protocol」這條 Task 1 測項 A 正在驗證的可疑路徑在開機時就走一次。
  if (longRangeEnabled && !applyLongRange(true)) {
    Serial.println("⚠ [LR] 開機套用失敗，強制視為關閉（NVS 一併更正，避免每次開機都失敗一次）");
    longRangeEnabled = false;
    saveLongRange(false);
  } else if (!longRangeEnabled) {
    applyLongRange(false);   // 明確寫一次，不倚賴晶片預設值剛好等於我們要的
  }
```

- [ ] **Step 4: slave 端持久化（把 Phase 1 欠的 `EE_ADDR_LONGRANGE` 補完）**

**`EE_ADDR_LONGRANGE`（位址 8）目前已定義但從頭到尾沒被讀也沒被寫。** 補上：

`loadPairing()` 在 `lockedChannel = EEPROM.read(EE_ADDR_CHANNEL);` 之後補：
```cpp
  // Phase 1 只保留了位址，從來沒有讀寫過。Phase 5 開始它是真的有意義的欄位。
  longRangeEnabled = (EEPROM.read(EE_ADDR_LONGRANGE) == 1);
```
並把該函式最後那行 `Serial.printf` 的格式字串加上 LR：
```cpp
  Serial.printf("已配對 master: %s，上次 channel=%u，LR=%s\n",
                id, lockedChannel, longRangeEnabled ? "開啟" : "關閉");
```
> **這一行改了字串。Task 6 寫回歸清單時必須以這裡的最終字串為準。**

`savePairing()` 補一行（`EEPROM.commit()` 之前）：
```cpp
  EEPROM.write(EE_ADDR_LONGRANGE, longRangeEnabled ? 1 : 0);
```

全域補 `bool longRangeEnabled = false;`（放在 `lockedChannel` 旁邊）。

`setup()` 在 `setupEspNow();` **之後**、`Serial.printf("設備 ID: ...")` 之前插入：
```cpp
  // 與 master 同樣的順序理由：esp_wifi_set_protocol() 需要 WiFi 已 start，
  // 而 setupEspNow() 的 WiFi.mode(WIFI_STA) 才會 start 它。
  // 也必須早於下面的 setChannel()／startChannelScan()，否則第一輪掃描會用
  // 一個還沒套用目標設定的射頻去找 master。
  if (longRangeEnabled && !applyLongRange(true)) {
    Serial.println("⚠ [LR] 開機套用失敗，強制視為關閉（EEPROM 一併更正）");
    longRangeEnabled = false;
    savePairing();
  } else if (!longRangeEnabled) {
    applyLongRange(false);
  }
```

- [ ] **Step 5: 編譯驗證**

```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
.\flash.ps1 -Model test
```
**四者皆 exit code 0**（本 Task 動到共用函式庫，`-Model test` 也要編）。
記錄 flash／RAM，與 Task 1 的基準比較。

- [ ] **Step 6: Commit**

commit 訊息必須包含：為什麼 `applyLongRange()` 一定要讀回驗證（set 回 ESP_OK ≠ 晶片支援 LR）、
為什麼 `lrRadioValue` 要與 `longRangeEnabled` 分成兩個變數、
`HoLrPayload.applied` 的語義是「已受理並承諾套用」而不是「已套用完成」及其原因、
以及 slave 的 `EE_ADDR_LONGRANGE` 是 Phase 1 留下的欠款、本次補完。

---

## Task 3：slave 端 —— 接收切換、心跳自癒、失聯自我救援

**本 Task 結束後 slave 可以單獨燒錄。** master 還不會送 `LR_SET`，
所以在真實系統裡行為與現在相同 —— 這是刻意的：先把接收端做穩，再讓 master 開始送。

**Files:**
- Modify: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes：Task 2 的 `applyLongRange()`、`lrRadioValue`、`longRangeEnabled`、`HoLrPayload`
- Produces（Task 4 在實機上依賴，編譯上互不依賴）：完整的 slave 端切換與救援行為

---

- [ ] **Step 1: 全域狀態**

```cpp
// ── Long Range 切換與救援（Phase 5 Task 3）──
// ESP-NOW recv callback 跑在 WiFi task，不可在裡面做 esp_wifi_set_protocol()
// 或 EEPROM.commit()（前者動射頻、後者會擦寫 flash 並可能阻塞數十毫秒），
// 所以 callback 只登記「要套用什麼」，實際動作全部交給 loop()。
// 這與檔案裡既有的 requestBlink()／updateBlink() 是同一個模式。
volatile bool     lrPendingApply = false;
volatile uint8_t  lrPendingValue = 0;
volatile bool     lrPendingNeedSave = false;   // 是否連 EEPROM 一起更新
volatile unsigned long lrPendingAt = 0;

// 回 ACK 與實際套用之間刻意隔一小段，讓 ACK 有時間離開天線。
// 若兩端 bitmap 不同時真的收不到彼此（Task 1 測項 B），套用的那一瞬間就再也
// 送不出東西給 master 了，ACK 必須在那之前送出去。
const unsigned long LR_APPLY_DELAY_MS = 50;

// ── 失聯自我救援（計畫「決定 4」）──
// 失聯最可能的原因之一就是「兩端 LR bitmap 不一致」，而那種情況下
// 掃 13 個 channel 一輪都不會有用 —— master 就在原本那個 channel 上，只是聽不到。
bool lrRescueTried  = false;   // 本次失聯是否已經試過對調（收到心跳就清掉）
bool lrRescueActive = false;   // 目前射頻是「對調後」的狀態
unsigned long lrRescueStart = 0;
// 必須明顯大於 master 的心跳間隔（1 秒），才能保證「只要對調是對的，
// 這段期間必定收得到至少一次心跳」。這與 SCAN_DWELL_MS(1200ms) 為什麼要大於
// 心跳週期是同一個推理（設計規格「難點 1」逐字說明過），
// 這裡把餘裕拉到 3 倍，因為剛改完射頻參數，第一則心跳有可能被吃掉。
const unsigned long LR_RESCUE_DWELL_MS = 3000;
```

- [ ] **Step 2: `HO_PKT_LR_SET` 分支（先回 ACK、後套用）**

插在 `onEspNowRecv()` 的「只接受已配對 master 的控制指令」那道 guard **之後**、
`HO_PKT_CMD` 分支**之前**：

```cpp
  if (header.type == HO_PKT_LR_SET && payloadLen >= sizeof(HoLrPayload)) {
    HoLrPayload p;
    memcpy(&p, payload, sizeof(p));
    bool target = (p.longRange == 1);

    // ── 順序極重要：先回 ACK，後套用 ──
    // 若兩端 bitmap 不同時真的收不到彼此（Task 1 測項 B 不成立的世界），
    // 一旦先套用，這封 ACK 就永遠送不出去 —— master 會逾時，
    // 而且會「以為沒有人切換成功」，於是逾時路徑印出來的未確認名單全部是假的。
    // Phase 5 實測時要靠那份名單判斷「這次測距是不是真的兩端都開了 LR」，
    // 名單不能是假的。
    HoLrPayload ack;
    ack.longRange = target ? 1 : 0;
    ack.applied   = 1;      // 語義是「已受理並承諾套用」，見協定標頭的註釋
    uint8_t buf[250];
    size_t total = hoPackPacket(buf, sizeof(buf), HO_PKT_LR_ACK, txSeq++, &ack, sizeof(ack));
    esp_now_send(info->src_addr, buf, total);

    // 只登記，不在 callback 裡動射頻或寫 EEPROM
    if (target != longRangeEnabled || target != lrRadioValue) {
      lrPendingValue    = target ? 1 : 0;
      lrPendingNeedSave = (target != longRangeEnabled);
      lrPendingAt       = millis();
      lrPendingApply    = true;
    }
    return;
  }
```

- [ ] **Step 3: 心跳是唯一權威（自癒 + 救援收尾，合成同一條規則）**

在 `HO_PKT_HEARTBEAT` 分支裡、`onMasterFound(...)` 呼叫**之前**插入：

```cpp
    // ── 心跳裡 master 說什麼就是什麼（Phase 5 Task 3）──
    // 這一條同時處理三件事，刻意合成一條規則而不是三段判斷：
    //   (a) 自癒：切換當下本機剛好離線、漏掉了 LR_SET
    //   (b) 救援收尾：下方的失聯救援把射頻對調了，收到心跳就該以 master 為準收斂
    //   (c) master 回滾：master 驗證失敗回到舊值，本機要跟著回去
    // 判斷式同時比對「儲存值」與「射頻現值」，因為救援期間兩者必然不同。
    if (masterKnown) {
      bool hbLr = (hb.longRange == 1);
      if (lrRescueActive) {
        lrRescueActive = false;   // 對調後聽到心跳＝救援成功，交給下面的收斂
        Serial.println("[LR] 失聯救援成功：對調 LR 之後收到心跳");
      }
      lrRescueTried = false;      // 已經聽得到 master，下次失聯可以再救一次
      if (hbLr != longRangeEnabled || hbLr != lrRadioValue) {
        lrPendingValue    = hbLr ? 1 : 0;
        lrPendingNeedSave = (hbLr != longRangeEnabled);
        lrPendingAt       = millis();
        lrPendingApply    = true;
        Serial.printf("[LR] 依心跳對齊為%s（儲存值=%s 射頻現值=%s）\n",
                      hbLr ? "開啟" : "關閉",
                      longRangeEnabled ? "開啟" : "關閉",
                      lrRadioValue ? "開啟" : "關閉");
      }
    }
```

- [ ] **Step 4: `loop()` 消化待辦的套用**

放在 `updateBlink(now);` 那一段之後：

```cpp
  // ── 消化 ESP-NOW callback 登記的 LR 套用（Task 3 Step 1 的理由）──
  if (lrPendingApply && (now - lrPendingAt) >= LR_APPLY_DELAY_MS) {
    lrPendingApply = false;
    bool target = (lrPendingValue == 1);
    bool needSave = lrPendingNeedSave;

    // 先寫 EEPROM 再套用：萬一套用的當下斷電，重開機會用新值，
    // 與 master（它最後才切自己）一致；反過來則會不一致。
    if (needSave) {
      longRangeEnabled = target;
      savePairing();
    }
    if (!applyLongRange(target)) {
      Serial.println("⚠ [LR] 套用失敗，射頻維持原狀（EEPROM 已是新值，"
                     "下次開機會再試一次；心跳自癒也會再送一次）");
    }
  }
```

- [ ] **Step 5: 失聯自我救援（順序不可調換）**

改寫 `loop()` 裡的 `if (scanning) { ... return; }` 區塊：

```cpp
  // 掃描中：先處理 LR 對調救援，再走既有的 channel 輪掃
  if (scanning) {
    // ── LR 對調救援（計畫「決定 4」）──
    // 前提：走到這裡代表 startChannelScan() 已經跑過，繼電器「已經」被強制關閉了。
    // 鐵則不打折：救援絕不延後關閉繼電器的時機。把關閉延到救援之後，
    // 等於把錯誤狀態的持續時間從 30 秒拉到 33 秒，而 30 秒門檻的用途就是
    // 「回到已知安全狀態」——它不會因為有一個更好的辦法就變成「差不多就好」。
    if (lrRescueActive) {
      if (now - lrRescueStart < LR_RESCUE_DWELL_MS) return;   // 還在等，先不換 channel
      lrRescueActive = false;
      applyLongRange(longRangeEnabled);   // 切回儲存值，同樣不寫 EEPROM
      Serial.printf("[LR] 對調後 %lu 秒仍無心跳，切回%s，繼續 channel 輪掃\n",
                    LR_RESCUE_DWELL_MS / 1000, longRangeEnabled ? "開啟" : "關閉");
      scanChannelStart = now;   // 本 channel 重新給一輪 dwell
      return;
    }
    if (masterKnown && !lrRescueTried) {
      lrRescueTried  = true;
      lrRescueActive = true;
      lrRescueStart  = now;
      // 只動射頻，「不」動 longRangeEnabled、「不」寫 EEPROM ——
      // 這是一次試探不是一次決定。寫進去之後若沒救回來又剛好斷電，
      // 下次開機就用了一個從來沒被驗證過的值。真正的寫入交給心跳自癒
      //（Step 3），它以 master 說的為準，那才是權威。
      applyLongRange(!lrRadioValue);
      Serial.printf("[LR] 失聯救援：把射頻對調成%s試 %lu 秒（不寫 EEPROM）—— "
                    "失聯最可能的原因是兩端 LR 設定不一致，那種情況掃 13 個 channel 沒有用\n",
                    lrRadioValue ? "開啟" : "關閉", LR_RESCUE_DWELL_MS / 1000);
      return;
    }
    if (now - scanChannelStart >= SCAN_DWELL_MS) {
      scanChannel = (scanChannel % 13) + 1;
      scanChannelStart = now;
      setChannel(scanChannel);
    }
    return;
  }
```

> **注意 `applyLongRange(!lrRadioValue)` 的參數**：對調的是**射頻現值**，不是儲存值。
> 兩者在正常情況下相同，但在「上一次救援失敗、剛切回去」的路徑上可能不同，
> 用射頻現值才是「把現在這個試過的相反面拿來試」的正確語義。

- [ ] **Step 6: 編譯驗證**

三種型號（master／master-c3／slave）皆 exit 0，記錄 flash／RAM。
**`-Model slave` 這次是實質驗證不是形式檢查。**

- [ ] **Step 7: Commit**

commit 訊息必須包含：為什麼 ACK 一定要在套用之前送、為什麼救援期間不寫 EEPROM、
為什麼救援絕不能延後關閉繼電器（鐵則不打折）、以及「心跳是唯一權威」這條規則
如何同時解掉自癒／救援收尾／master 回滾三件事。

---

## Task 4：master 端 —— 握手、緊急插隊、切換後驗證、自動回滾、開機守衛

本階段最複雜的一個 Task。**它的每一段都是在回答同一個問題：切換到一半出事時，籠門會不會開。**

**Files:**
- Modify: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes：Task 2 的 `applyLongRange()`／`saveLongRange()`／`lrRadioValue`／`HoLrPayload`；
  既有的 `espNowSendTo()`／`sendHeartbeat()`／`espNowDelay()`／`findSlave()`／
  `requestSlaveState()`／`sendCmdToSlave()`／`sendCmdToAll()`／`onWifiChannelMayHaveChanged()`／
  `publishStatus()`／`buildStatusDoc()`
- Produces（Task 5 依賴）：`lrPhase`／`lrHeartbeatBurst()`／`lrNoteRelayCommand()`／
  狀態 JSON 的 `long_range_pending`／`long_range_error`

---

- [ ] **Step 0: 核對 `handleMasterCommand()` 是否存在**

```
grep -n "void handleMasterCommand" ho_master1/ho_master1.ino
```
找到 → 走 **4-A**（Step 8 的第一種接法）。找不到 → 走 **4-B**。
**不要為了配合本計畫去重構 `mqttCallback()`** —— 那是 Phase 2b Task 4 的工作範圍。
**report 必須寫明用了哪一種。**

- [ ] **Step 1: 常數與時間預算（含把預算鎖在編譯期的 `static_assert`）**

放在 `SLAVE_OFFLINE_TIMEOUT` 那組常數附近：

```cpp
// ── Long Range 切換的時間預算 ──
// 整條路徑的每一段都必須在「slave 30 秒失聯就強制關閉繼電器＝籠門被打開」之內收斂。
// 逐段實算（HEARTBEAT_BURST_COUNT=4、HEARTBEAT_BURST_GAP=200 → 一次連發 600ms）：
//
//   LR_ANNOUNCE_TIMEOUT_MS   10000   握手上界
//   套用前後各一次連發          1200   （2 × 600）
//   LR_SETTLE_MS              2000   讓射頻與 WiFi 事件穩定
//   LR_VERIFY_WINDOW_MS      10000   主動探測窗
//   回滾時再一次套用（含連發）  1200
//   ─────────────────────────────
//   合計                      24400  ms　距離 30000 還有 5600ms
//
// 這是「整條路徑」的保守上界。**任何一台 slave 實際的失聯窗口都遠小於它**：
//   - 已 ACK 的 slave：從自己 ACK 到 master 套用完成，最壞 10000 + 600 = 10600ms
//   - 未 ACK 的 slave：從 master 套用完成到回滾套用完成，最壞 600+2000+10000+600 = 13200ms
// 兩者都不到門檻的一半。保守上界只是拿來當編譯期的護欄。
//
// **改動下面任何一個常數，都必須重算這一段並確認 static_assert 仍成立。**
const unsigned long LR_ANNOUNCE_TIMEOUT_MS = 10000;
const unsigned long LR_SEND_GAP_MS         = 100;    // 握手期間每 100ms 對一台送 LR_SET
const unsigned long LR_SETTLE_MS           = 2000;
const unsigned long LR_VERIFY_WINDOW_MS    = 10000;
const unsigned long LR_VERIFY_PROBE_GAP_MS = 250;    // 驗證期間每 250ms 探測一台
const int           LR_VERIFY_MIN_PERCENT  = 60;
const unsigned long LR_RESEND_GAP_MS       = 150;    // 緊急指令補送的間隔（決定 2）
// 開機守衛：LR 開著但這麼久都沒有任何一台回報過狀態，就自動關掉 LR。
// 90 秒的來由：守衛絕對不能比「正常的恢復路徑」還早開槍。設計規格「難點 1」算過
// 最壞恢復是「30 秒失聯門檻 + 一輪掃描 13×1200ms = 45.6 秒」，90 秒給了近兩倍餘裕，
// 所以「slave 只是正在輪掃、還沒回來」不會被誤判成災難。
const unsigned long LR_BOOT_GUARD_MS       = 90000;

// 一次心跳連發的耗時（供上面的預算與下面的 static_assert 使用）
const unsigned long LR_BURST_COST_MS =
    (unsigned long)(HEARTBEAT_BURST_COUNT - 1) * HEARTBEAT_BURST_GAP;

static_assert(LR_ANNOUNCE_TIMEOUT_MS + LR_SETTLE_MS + LR_VERIFY_WINDOW_MS
                  + 4 * LR_BURST_COST_MS < 30000,
              "LR 切換的時間預算總和已逼近 slave 的 30 秒失聯門檻（＝籠門被打開），"
              "請縮小 LR_ANNOUNCE_TIMEOUT_MS 或 LR_VERIFY_WINDOW_MS");
```

- [ ] **Step 2: 全域狀態**

```cpp
// ── Long Range 切換狀態機（全程非阻塞，跑在 loop()）──
enum LrPhase {
  LR_IDLE = 0,
  LR_ANNOUNCING,   // 對「切換開始時在線」的 slave 送 LR_SET，等 ACK
  LR_SETTLING,     // 自己剛套用，等射頻與 WiFi 事件穩定
  LR_VERIFYING,    // 主動探測，確認 slave 還在；不合格就自動回滾
};
LrPhase lrPhase = LR_IDLE;

bool lrTarget    = false;   // 這次要切到什麼
bool lrPrevValue = false;   // 切換前是什麼（回滾用）

// bit i = slaves[i]。ESP-NOW callback（WiFi task）只做「設 bit」，
// loop() 只做「讀」與「整個歸零」。歸零與設 bit 若剛好撞在一起，最多漏記一次，
// 而那個時間點階段已經要結束了，後果只是序列埠多印一台「未確認」，不影響行為。
// 上限 32 台（uint32_t），HO_ESPNOW_MAX_SLAVES 是 20，所有迴圈仍會顯式檢查 i < 32。
volatile uint32_t lrAckMask    = 0;
volatile uint32_t lrVerifyMask = 0;
uint32_t lrExpectMask  = 0;   // 切換開始的那一刻「本來就在線」的那些台
int      lrExpectCount = 0;

unsigned long lrPhaseStart = 0;
unsigned long lrLastSendAt = 0;
int lrSendIdx = 0;

// 狀態 JSON 的 long_range_error。
// ⚠ 這裡放進去的字串長度上界被 STATUS_LR_ERROR_MAX_BYTES 綁住，
//    新增字串「不得超過 16 個字元」，否則要同步加大該常數並重驗 static_assert。
//    目前使用的字串：no_online_slave(15)／verify_failed(13)／boot_guard(10)／apply_failed(12)
char lrLastError[20] = "";

// ── 緊急插隊（計畫「決定 2」）──
bool          lrResendPending = false;
unsigned long lrResendAt      = 0;
uint8_t       lrResendCmd     = 0;
uint16_t      lrResendPulseMs = 0;
int           lrResendIdx     = -1;   // -1 = 全體

// ── 開機守衛（計畫「決定 5」第 2 層）──
unsigned long lrBootGuardStart = 0;
bool lrBootGuardDone     = false;
bool lrAnySlaveReported  = false;   // 本次開機是否收過任何一台的 HO_PKT_STATE
```

- [ ] **Step 3: 心跳連發與套用的包裝**

```cpp
// 與既有的 sendHeartbeatBurst() 同樣是連發，但改用 espNowDelay() 而非裸 delay()。
//
// 為什麼不能沿用 sendHeartbeatBurst()：它內部的 delay(200) 是全專案唯一被允許的裸
// delay()（它本身就在發心跳），但那 600ms 期間 maintainEspNow() 不會被呼叫，
// 而點動結束檢查正是掛在 maintainEspNow() 裡（Phase 2a Task 3 的 review 修正）。
// LR 切換前後各連發一次 = 1.2 秒，而這段時間剛好就落在「使用者按下全部關門、
// slave 正在點動」的尺度上 —— 這是本階段唯一不能沿用那個既有例外的地方。
void lrHeartbeatBurst() {
  Serial.printf("[LR] 連發 %d 次心跳（間隔 %d ms）\n",
                HEARTBEAT_BURST_COUNT, HEARTBEAT_BURST_GAP);
  for (int i = 0; i < HEARTBEAT_BURST_COUNT; i++) {
    sendHeartbeat();
    if (i < HEARTBEAT_BURST_COUNT - 1) espNowDelay(HEARTBEAT_BURST_GAP);
  }
}

// 套用 LR，前後各連發一次心跳。
// esp_wifi_set_protocol() 是本階段唯一新增的、可能中斷射頻的呼叫（Task 1 測項 A
// 就是在量它會不會造成 STA 斷線）。做法與 Phase 4 對「無法拆解的阻塞步驟」一致：
// 進去之前先讓所有 slave 拿到一批心跳，出來之後立刻再補一批。
bool applyLongRangeWithBurst(bool enable) {
  lrHeartbeatBurst();
  bool res = applyLongRange(enable);
  lrHeartbeatBurst();
  // 若 set_protocol 真的造成一次斷線重連，channel 可能已經變了。
  // 主動檢查一次，讓 slave 在 channel 真的變動時立刻收到通知，
  // 而不是等 30 秒失聯門檻。
  onWifiChannelMayHaveChanged();
  return res;
}
```

- [ ] **Step 4: 握手的開始、結束與緊急插隊**

```cpp
void startLongRangeSwitch(bool target) {
  if (lrPhase != LR_IDLE) {
    Serial.println("[LR] 上一次切換尚未結束，忽略本次指令");
    return;
  }
  if (target == longRangeEnabled) {
    Serial.printf("[LR] 已經是%s狀態，不需切換\n", target ? "開啟" : "關閉");
    publishStatus();
    return;
  }

  int n = slaveCount;
  if (n == 0) {
    Serial.println("[LR] 名冊是空的，直接套用");
    if (applyLongRangeWithBurst(target)) {
      longRangeEnabled = target;
      saveLongRange(target);
      lrLastError[0] = '\0';
    } else {
      snprintf(lrLastError, sizeof(lrLastError), "apply_failed");
    }
    publishStatus();
    return;
  }

  lrExpectMask = 0;
  lrExpectCount = 0;
  for (int i = 0; i < n && i < 32; i++) {
    if (slaves[i].online) { lrExpectMask |= (1UL << i); lrExpectCount++; }
  }

  // 守門：名冊上有 slave 但一台在線的都沒有時，拒絕切換。
  // 這種狀態下既握不了手、也驗證不了結果，等於閉著眼睛動射頻設定；
  // 而唯一的恢復手段會落到 slave 端 30 秒失聯救援 —— 那條路徑一定會先關繼電器。
  if (lrExpectCount == 0) {
    Serial.printf("[LR] 名冊上 %d 台全部離線，拒絕切換（無法握手也無法驗證）\n", n);
    snprintf(lrLastError, sizeof(lrLastError), "no_online_slave");
    publishStatus();
    return;
  }

  lrPhase       = LR_ANNOUNCING;
  lrTarget      = target;
  lrPrevValue   = longRangeEnabled;
  lrAckMask     = 0;
  lrVerifyMask  = 0;
  lrPhaseStart  = millis();
  lrLastSendAt  = 0;
  lrSendIdx     = 0;
  lrLastError[0] = '\0';
  Serial.printf("[LR] 開始切換為%s，先等 %d 台在線 slave 確認（最多 %lu 秒）\n",
                target ? "開啟" : "關閉", lrExpectCount, LR_ANNOUNCE_TIMEOUT_MS / 1000);
  publishStatus();
}

// 結束握手並套用自己。why 只影響訊息內容，行為完全相同。
//
// master 為什麼最後才切自己（計畫「決定 1」）：若兩端 bitmap 不同時真的收不到彼此
// （Task 1 測項 B 不成立），master 一旦先切，所有還沒收到 LR_SET 的 slave 立刻變聾，
// 而 LR_SET 正是要靠 ESP-NOW 送過去的 —— 握手會當場自我毀滅，一台都切不成。
void finishAnnouncing(const char* why) {
  int missing = 0;
  for (int i = 0; i < slaveCount && i < 32; i++) {
    if ((lrExpectMask & (1UL << i)) && !(lrAckMask & (1UL << i))) {
      char id[20];
      hoFormatDeviceId(slaves[i].mac, id);
      Serial.printf("[LR] %s 未確認（切換開始時在線，但沒回 ACK）\n", id);
      missing++;
    }
  }
  Serial.printf("[LR] 結束握手（%s）：%d／%d 台已確認，接著套用自己並進入驗證階段\n",
                why, lrExpectCount - missing, lrExpectCount);

  if (applyLongRangeWithBurst(lrTarget)) {
    // 先寫 NVS，回滾時再寫回去。理由：如果套用之後立刻斷電而 NVS 還是舊值，
    // 重開機會用舊值、但已經 ACK 並套用的 slave 是新值，兩端不一致且 master 不知道。
    // 寫成新值則相反：重開機用新值，若真的連不上，開機守衛（Step 6）90 秒後
    // 會自動關掉它。有一個機制兜底的那一邊才是該選的那一邊。
    longRangeEnabled = lrTarget;
    saveLongRange(lrTarget);
  } else {
    snprintf(lrLastError, sizeof(lrLastError), "apply_failed");
    Serial.println("⚠ [LR] 自己套用失敗（讀回不一致或晶片不支援），"
                   "但部分 slave 可能已經切過去了 —— 直接進驗證階段，讓回滾機制處理");
  }
  lrPhase      = LR_SETTLING;
  lrPhaseStart = millis();
  publishStatus();
}

// 繼電器指令在 LR 握手期間進來時要做的事（計畫「決定 2」）。
//
// 呼叫點只有兩個：sendCmdToSlave() 與 sendCmdToAll() 的**開頭**。
// 本函式「不」送指令 —— 送出仍由呼叫端照原本的流程做。它只負責把切換立刻收尾、
// 並安排一次補送。
//
// 為什麼要補送：在 Task 1 測項 B 不成立的世界裡，指令進來的那一刻名冊已經裂成
// 兩半 —— 已 ACK 的在新 bitmap、還沒 ACK 的在舊 bitmap，而 master 還在舊 bitmap
// （它最後才切）。呼叫端用舊 bitmap 送出的那一次只打得到後者。所以：
//   1. 呼叫端立刻用舊 bitmap 送 → 打到還沒 ACK 的那些台
//   2. 這裡立刻收尾切換（套用新 bitmap）
//   3. LR_RESEND_GAP_MS 後補送一次 → 打到已 ACK 的那些台
// 兩次相隔約 150~200ms，遠小於 Phase 1 sendCmdToAll() 逐台單播就會產生的 400ms
// 落差（規格「系統本質」點名那半秒是動物的逃脫窗口），所以沒有把落差擴大到
// 規格已經明確拒絕的量級。
//
// 為什麼不是「中止切換、回到舊值」：那看起來更保守其實更糟 —— 已經 ACK 並套用
// 新 bitmap 的那些 slave 會永久聽不到 master，只能等自己的 30 秒門檻觸發救援
//（那條路徑一定會先關繼電器）。中止＝把一半的門丟給 30 秒後自救；
// 切完再補送＝150ms 後全部覆蓋到。
//
// 防遞迴：補送會再次呼叫 sendCmdToAll()／sendCmdToSlave()，因而再次走進本函式，
// 但那時 lrPhase 已經是 LR_SETTLING／LR_VERIFYING，第一行的檢查就會 return，
// 迴圈自然封閉。**這一行檢查同時是功能條件也是防遞迴，不可以改成別的判斷。**
void lrNoteRelayCommand(HoRelayCmd cmd, uint16_t pulseMs, int idx) {
  if (lrPhase != LR_ANNOUNCING) return;

  Serial.println("[LR] 握手期間收到繼電器指令 —— 安全指令優先於 LR，"
                 "立刻結束握手，套用後再補送一次同一道指令");
  lrResendCmd     = (uint8_t)cmd;
  lrResendPulseMs = pulseMs;
  lrResendIdx     = idx;
  lrResendPending = true;

  finishAnnouncing("被繼電器指令插隊");
  lrResendAt = millis() + LR_RESEND_GAP_MS;   // 必須在 finishAnnouncing() 之後取時間
}
```

`sendCmdToSlave()` 與 `sendCmdToAll()` 的**第一行**各加一句：
```cpp
  lrNoteRelayCommand(cmd, pulseMs, idx);   // sendCmdToSlave()
  lrNoteRelayCommand(cmd, pulseMs, -1);    // sendCmdToAll()
```
> `sendCmdToAll()` 會逐台呼叫 `sendCmdToSlave()`，所以本函式會被呼叫兩次以上。
> 它是冪等的（第二次進來時 `lrPhase` 已不是 `LR_ANNOUNCING`），不需要額外去重。

- [ ] **Step 5: 驗證與回滾**

```cpp
void finishVerify(bool ok, int replied, int need) {
  if (ok) {
    Serial.printf("[LR] 驗證通過：%d／%d 台回報（門檻 %d 台），切換為%s完成\n",
                  replied, lrExpectCount, need, lrTarget ? "開啟" : "關閉");
    lrLastError[0] = '\0';
    lrPhase = LR_IDLE;
    publishStatus();
    return;
  }

  for (int i = 0; i < slaveCount && i < 32; i++) {
    if ((lrExpectMask & (1UL << i)) && !(lrVerifyMask & (1UL << i))) {
      char id[20];
      hoFormatDeviceId(slaves[i].mac, id);
      Serial.printf("[LR] %s 在驗證階段沒有回報\n", id);
    }
  }
  Serial.printf("[LR] 驗證失敗：只有 %d／%d 台回報（門檻 %d 台），自動回滾為%s\n",
                replied, lrExpectCount, need, lrPrevValue ? "開啟" : "關閉");

  if (applyLongRangeWithBurst(lrPrevValue)) {
    longRangeEnabled = lrPrevValue;
    saveLongRange(lrPrevValue);
  }
  snprintf(lrLastError, sizeof(lrLastError), "verify_failed");
  lrPhase = LR_IDLE;

  Serial.println("[LR] 已回到切換前的設定。刻意不自動重試 —— "
                 "ON→壞→回滾→自動再 ON 會變成無限震盪，每一次震盪都是一次射頻中斷。");
  Serial.println("[LR] 注意：已經套用新設定的 slave 不會因為 master 回滾而恢復，"
                 "它們要靠自己 30 秒失聯後的 LR 對調救援回來（那條路徑會先關閉繼電器）。");
  publishStatus();
}

void updateLongRangeSwitch(unsigned long now) {
  // 緊急補送（決定 2 的第 3 步）優先於一切
  if (lrResendPending && (long)(now - lrResendAt) >= 0) {
    lrResendPending = false;
    Serial.println("[LR] 補送剛才那道繼電器指令（覆蓋在握手期間已經切換過去的那些台）");
    if (lrResendIdx < 0) sendCmdToAll((HoRelayCmd)lrResendCmd, lrResendPulseMs);
    else                 sendCmdToSlave(lrResendIdx, (HoRelayCmd)lrResendCmd, lrResendPulseMs);
  }

  if (lrPhase == LR_ANNOUNCING) {
    bool allAcked = true;
    for (int i = 0; i < slaveCount && i < 32; i++) {
      if ((lrExpectMask & (1UL << i)) && !(lrAckMask & (1UL << i))) { allAcked = false; break; }
    }
    if (allAcked) { finishAnnouncing("全部確認"); return; }
    if (now - lrPhaseStart >= LR_ANNOUNCE_TIMEOUT_MS) { finishAnnouncing("等待逾時"); return; }

    if (now - lrLastSendAt < LR_SEND_GAP_MS) return;
    lrLastSendAt = now;
    int n = slaveCount;
    for (int k = 0; k < n; k++) {
      int i = (lrSendIdx + k) % n;
      if (i < 32 && (lrExpectMask & (1UL << i)) && !(lrAckMask & (1UL << i))) {
        HoLrPayload p;
        p.longRange = lrTarget ? 1 : 0;
        p.applied   = 0;
        espNowSendTo(slaves[i].mac, HO_PKT_LR_SET, &p, sizeof(p));
        lrSendIdx = (i + 1) % n;
        return;   // 每次 loop() 最多送一台，錯開避免同頻碰撞
      }
    }
    return;
  }

  if (lrPhase == LR_SETTLING) {
    if (now - lrPhaseStart < LR_SETTLE_MS) return;
    lrPhase      = LR_VERIFYING;
    lrPhaseStart = now;
    lrLastSendAt = 0;
    lrSendIdx    = 0;
    lrVerifyMask = 0;
    Serial.printf("[LR] 進入驗證階段，%lu 秒內主動探測切換前在線的 %d 台\n",
                  LR_VERIFY_WINDOW_MS / 1000, lrExpectCount);
    return;
  }

  if (lrPhase == LR_VERIFYING) {
    int replied = 0;
    for (int i = 0; i < slaveCount && i < 32; i++) {
      if ((lrExpectMask & (1UL << i)) && (lrVerifyMask & (1UL << i))) replied++;
    }
    // 門檻：一般情況取 60%，但 1~2 台時 60% 等於「掉一台就回滾」，
    // 而現場本來就會有一兩台因為訊號邊緣或剛好在點動而漏答。
    // 用一次假警報換一次真的射頻中斷是負收益，所以小名冊改成「至少一台回應」。
    int need = (lrExpectCount <= 2)
                 ? 1
                 : ((lrExpectCount * LR_VERIFY_MIN_PERCENT + 99) / 100);
    if (replied >= need) { finishVerify(true, replied, need); return; }
    if (now - lrPhaseStart >= LR_VERIFY_WINDOW_MS) { finishVerify(false, replied, need); return; }

    if (now - lrLastSendAt < LR_VERIFY_PROBE_GAP_MS) return;
    lrLastSendAt = now;
    int n = slaveCount;
    for (int k = 0; k < n; k++) {
      int i = (lrSendIdx + k) % n;
      if (i < 32 && (lrExpectMask & (1UL << i)) && !(lrVerifyMask & (1UL << i))) {
        // 主動探測而不是等 pollNextSlave()：後者一輪 15 秒，20 台的話每台 750ms
        // 才輪到一次，10 秒內問不完；這裡每 250ms 一台，10 秒可以問 40 次。
        requestSlaveState(i);
        lrSendIdx = (i + 1) % n;
        return;
      }
    }
  }
}
```

`onEspNowRecv()` 加 `HO_PKT_LR_ACK` 分支（放在 `HO_PKT_STATE` 分支之後）：
```cpp
  if (header.type == HO_PKT_LR_ACK && payloadLen >= sizeof(HoLrPayload)) {
    int idx = findSlave(info->src_addr);
    if (idx >= 0 && idx < 32 && lrPhase == LR_ANNOUNCING) {
      HoLrPayload p;
      memcpy(&p, payload, sizeof(p));
      // 只認「目標值相符」的 ACK，擋掉上一次切換遲到的回覆
      if (p.longRange == (lrTarget ? 1 : 0)) {
        lrAckMask |= (1UL << idx);
        Serial.printf("[LR] %s 已確認\n", senderId);
      }
    }
    return;
  }
```

`onEspNowRecv()` 的 `HO_PKT_STATE` 分支裡（`if (changed) slaves[idx].dirty = true;` 之後）補：
```cpp
    // LR 驗證階段：收到任何一台的狀態回報就記一筆（計畫「決定 3」）
    if (lrPhase == LR_VERIFYING && idx < 32) lrVerifyMask |= (1UL << idx);
    // 開機守衛用：本次開機曾經收過任何一台的回報（計畫「決定 5」第 2 層）
    lrAnySlaveReported = true;
```

- [ ] **Step 6: 開機守衛**

```cpp
// LR 開著、名冊上有 slave、但開機後 LR_BOOT_GUARD_MS 內一台都沒回報過狀態
// → 自動關掉 LR。這是計畫「決定 5」的第 2 層：現場沒有 App、master 連不上 MQTT
// 時，唯一不需要任何人動手的回滾手段。
//
// 誤判的代價刻意設計成無害：如果 slave 只是全部斷電、而 LR 其實是好的，
// 守衛會把 LR 關掉 —— 而 slave 復電後收到心跳、發現 hb.longRange 與自己存的不同，
// 心跳自癒路徑會直接跟著關（Task 3 Step 3）。兩端仍然一致，只是失去距離增益。
// 「誤判的後果只是失去增益」正是這個守衛敢做得這麼積極的原因。
void updateLrBootGuard(unsigned long now) {
  if (lrBootGuardDone) return;
  if (!longRangeEnabled)   { lrBootGuardDone = true; return; }
  if (slaveCount == 0)     { lrBootGuardDone = true; return; }
  if (lrAnySlaveReported)  { lrBootGuardDone = true; return; }
  if (lrPhase != LR_IDLE)  return;   // 正在切換，交給驗證階段處理，不要兩套機制搶著動
  if (now - lrBootGuardStart < LR_BOOT_GUARD_MS) return;

  lrBootGuardDone = true;
  Serial.printf("[LR] 開機守衛：LR 開著，但開機後 %lu 秒內名冊上 %d 台一則狀態都沒回報，"
                "自動關閉 LR 回到已知可用的設定\n",
                LR_BOOT_GUARD_MS / 1000, (int)slaveCount);
  if (applyLongRangeWithBurst(false)) {
    longRangeEnabled = false;
    saveLongRange(false);
  }
  snprintf(lrLastError, sizeof(lrLastError), "boot_guard");
  publishStatus();
}
```
`setup()` 在套用 LR 之後（Task 2 Step 3 那段的下一行）補 `lrBootGuardStart = millis();`。

- [ ] **Step 7: 長按重置一併關閉 LR（對 Phase 2b 決定的實質修正）**

`updateResetButton()` 的確認分支，在 `clearNetConfig();` **之前**插入：

```cpp
  // ── 長按重置一併關閉 Long Range（Phase 5「決定 5」第 3 層）──
  // 這是刻意與 MQTT 的 reset 指令不同的行為，兩者的語義本來就不一樣：
  //   - reset（MQTT）＝「重設網路設定」。此時操作者手上有 App、有其他手段，
  //     不該順手改動一個無關的射頻設定。LR 存在 homaster 而非 hoban 正是為此。
  //   - 長按重置（實體按鈕）＝現場唯一不需要任何工具的手段，語義是
  //     「把設備弄回最保守的已知可用狀態」。而 LR 正是最可能讓 master 連不上
  //     slave 的設定，把它關掉就是這個語義本身。
  //
  // Phase 2b 當初把 LR 放進 homaster 的理由之一是「master 單方面關掉 LR 會與
  // slave 的 EEPROM 永久不一致」。那個前提在 Phase 5 已經不成立：slave 現在會在
  // 失聯 30 秒後自己把 LR 對調試一次（Task 3 Step 5），而且心跳一旦聽到就以
  // master 說的為準寫檔。所以這裡單方面關掉是安全的。
  saveLongRange(false);
  Serial.println("[重置] 已一併關閉 Long Range（現場回滾手段）");
```
> 注意：這裡呼叫的是 `saveLongRange(false)` 而**不是** `applyLongRange(false)` ——
> 下一行就要 `ESP.restart()`，套用射頻沒有意義，寫進 NVS 讓開機流程去套用才是對的。

同時把該分支既有的那行說明訊息改成（**它原本只提名冊，現在多了一項**）：
```cpp
  Serial.println("[重置] 長按重置清除網路設定（WiFi/MQTT）並關閉 Long Range，"
                 "slave 配對記錄（homaster 名冊）保留，不會解除任何已配對的籠子");
```
> **這一行改了字串。Task 6 的回歸清單必須以這裡的最終字串為準，
> 且要一併檢查 `docs/phase2a-regression-checklist.md` 有沒有引用舊字串。**

- [ ] **Step 8: 指令接點（MQTT 與序列埠）**

**4-A（`handleMasterCommand()` 存在時）** —— 在它的 else-if 鏈補：
```cpp
  } else if (message == "LR:ON") {
    startLongRangeSwitch(true);
  } else if (message == "LR:OFF") {
    startLongRangeSwitch(false);
```

**4-B（`handleMasterCommand()` 不存在時）** —— 在 `mqttCallback()` 的
`HASRELAY:ON`／`HASRELAY:OFF` 分支之後、`else` 之前補上完全相同的兩個分支。
**不要順手重構整支 `mqttCallback()`。**

序列埠：`handleSerialCommand()` 補一個**不走 `parseIndexArg()`** 的分支
（`lr` 的參數是 `on`／`off` 不是數字，**絕對不能加進 `needsArg` 清單**，
否則 `lr on` 會被「參數必須是數字」擋掉）：
```cpp
  } else if (verb == "lr") {
    if (argStr == "on")       startLongRangeSwitch(true);
    else if (argStr == "off") startLongRangeSwitch(false);
    else if (argStr.length() == 0) {
      Serial.printf("[LR] 設定=%s 射頻現值=%s 階段=%d 上次錯誤=%s\n",
                    longRangeEnabled ? "開啟" : "關閉",
                    lrRadioValue ? "開啟" : "關閉",
                    (int)lrPhase,
                    lrLastError[0] ? lrLastError : "無");
    } else {
      Serial.println("[LR] 用法：lr on | lr off | lr（查詢）");
    }
```
`printHelp()` 補一行：
```cpp
  Serial.println("  lr [on|off]   查詢／切換 Long Range（不帶參數為查詢）");
```

- [ ] **Step 9: 狀態 JSON 與容量常數重算**

**這一步的順序不可顛倒：先改常數並重驗 `static_assert`，再加欄位。**
Phase 4 的「決定 4.1」已經指出這個陷阱 —— 加欄位卻不同步加大常數時，
`static_assert` 會用舊常數繼續通過，**保護機制看起來還在、實際上已經失效**。

在 Task 1（Phase 2b）建立的容量常數區把 `STATUS_BASE_MAX_BYTES` 改成分項相加：

```cpp
// slaves 陣列與 Phase 5 的 long_range_error 「以外」所有欄位的上界。
// 這個 512 是 Phase 2b 的實算值，且**已經**含了 "long_range_pending":true, 的 27 bytes
//（Phase 2b Task 1 的預算表就列進去了），所以本階段只需要為 long_range_error 加額度。
const size_t STATUS_BASE_WITHOUT_LR_ERR_MAX_BYTES = 512;

// Phase 5 新增：  "long_range_error":"<最長 16 字元>",
//   key 含引號 18 + 冒號 1 + 值含引號 18 + 逗號 1 = 38 → 取 40
// ⚠ lrLastError 放進去的字串長度必須 ≤ 16 字元，新增字串時要回頭檢查這裡。
const size_t STATUS_LR_ERROR_MAX_BYTES = 40;

const size_t STATUS_BASE_MAX_BYTES =
    STATUS_BASE_WITHOUT_LR_ERR_MAX_BYTES + STATUS_LR_ERROR_MAX_BYTES;   // 552
```

> **若 Phase 4 已經執行過**，那裡已經把 `STATUS_BASE_MAX_BYTES` 改成
> `STATUS_BASE_WITHOUT_OTA_MAX_BYTES(512) + STATUS_OTA_MAX_BYTES(128) = 640`。
> 這種情況下本階段是**再加一項**：`512 + 128 + 40 = 680`。
> **兩種情況都要重驗 `static_assert` 並把算式寫進註釋：**
> - 沒有 Phase 4：`(3072 − 1 − 552 − 11) / 96 = 26 ≥ 20` ✓（餘裕 6 台）
> - 有 Phase 4：`(3072 − 1 − 680 − 11) / 96 = 24 ≥ 20` ✓（餘裕 4 台）

`buildStatusDoc()` 的 `dev` 區塊補兩行：
```cpp
  dev["long_range_pending"] = (lrPhase != LR_IDLE);
  dev["long_range_error"] = lrLastError;   // 空字串代表沒有錯誤
```

- [ ] **Step 10: 接進 `loop()`**

在 `maintainEspNow();` **之後**加：
```cpp
  // ── Long Range 切換狀態機與開機守衛（Phase 5）──
  // 放在 maintainEspNow() 之後：本輪的心跳先發出去，再去推進可能會動射頻的狀態機。
  updateLongRangeSwitch(now);
  updateLrBootGuard(now);
```

> **`now` 的取樣時機**：`loop()` 開頭取的 `now` 在經過 `connectToWiFi()`（最壞 15 秒）
> 或 `smartConnect()`（最壞 18 秒）之後就已經過時了。`updateLongRangeSwitch()` 排在
> `maintainEspNow()` 之後、WiFi／MQTT 區塊之前，所以拿到的 `now` 是新鮮的。
> **不要把這兩行搬到 `loop()` 的尾巴** —— 那裡的 `now` 可能落後真實時間十幾秒，
> 會讓 10 秒逾時判斷立刻成立。（Phase 2a Task 3 的 Critical 1 就是同一類問題。）

- [ ] **Step 11: 編譯驗證**

三種型號（master／master-c3／slave）皆 exit 0，記錄 flash／RAM 並與 Task 3 比較。

- [ ] **Step 12: Commit**

commit 訊息必須包含：
- master 為什麼最後才切自己（且理由與 Phase 2b 寫的不同：那個理由若成立反而推不出這個做法）
- 緊急指令為什麼是「送兩次」而不是「中止切換」
- 為什麼加了 `LR_VERIFYING` 與自動回滾，以及「回滾是必要但不充分，恢復保證來自 slave 端」
- 為什麼回滾之後刻意不自動重試
- 開機守衛的 90 秒是怎麼推出來的（不得早於規格算出的 45.6 秒正常恢復路徑）
- **長按重置一併關閉 LR 是對 Phase 2b 決定的實質修正**，以及那個舊前提為什麼不再成立
- 時間預算的 `static_assert`

---

## Task 5：實測儀器 —— 心跳序號、遺失率統計、「全部關」到達率測試

**這個 Task 的產出決定了 Phase 5 的實測到底量得出什麼。**
沒有它，現場測距會退化成「走到收不到為止」，而「收不到」的判準會變成
「我剛才好像沒聽到繼電器聲」。

**Files:**
- Modify: `libraries/HoEspNow/src/HoEspNowProtocol.h`
- Modify: `ho_espnow_test/ho_espnow_test.ino`
- Modify: `ho_master1/ho_master1.ino`
- Modify: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes：Task 4 的 `lrPhase`／`lrNoteRelayCommand()`；既有的 `espNowSendTo()`／
  `requestSlaveState()`／`hoFormatDeviceId()`／slave 的 `requestBlink()`
- Produces（Task 6 的實測程序文件依賴）：
  - `HoHeartbeatPayload.hbSeq`
  - slave 的 `[實測]` 統計行與 `field`／`stats` 序列埠指令
  - master 的 `lrtest <n>` 指令與結果表格

---

- [ ] **Step 1: 心跳加上序號（協定，向前相容的擴充）**

`HoEspNowProtocol.h`：

```c
struct __attribute__((packed)) HoHeartbeatPayload {
  uint8_t  channel;      // master 目前的 WiFi channel（1~13）
  uint8_t  pairingMode;  // 1 = 配對模式中
  uint8_t  longRange;    // 1 = LR 模式已啟用
  uint8_t  slaveCount;   // 目前已配對數量
  // ── Phase 5 新增：心跳序號，用來算「真實」的封包遺失率 ──
  // 沒有它的話，slave 只能用「收到則數 ÷ 經過秒數」推算遺失率，
  // 而 master 在 WiFi 關聯期間會把心跳間隔從 1000ms 改成 200ms
  //（HEARTBEAT_INTERVAL_ASSOC），推算值在那段期間會整個失真。
  // 有了序號，遺失率 = 1 − 收到數 ÷ (最後seq − 第一seq + 1)，
  // 完全不依賴任何對發送間隔的假設。2 bytes 換一個誠實的數字。
  //
  // ⚠ 這是加長 payload：新 master → 舊 slave 相容（舊端用 >= 檢查、只取前 4 bytes），
  //   舊 master → 新 slave 不相容（長度不足被丟棄 → 30 秒後強制關閉繼電器）。
  //   **燒錄順序一律是「先 master、後 slave」。**（規則見協定標頭的相容性註釋）
  uint16_t hbSeq;
};  // 6 bytes
```

`ho_espnow_test/ho_espnow_test.ino` 要同步改，**至少三處**（動手前先
`grep -n "HoHeartbeatPayload" ho_espnow_test/ho_espnow_test.ino` 把全部找出來，
不要只改記得的那幾處）：
1. `check(sizeof(HoHeartbeatPayload) == 4, ...)` → `== 6`，訊息字串也要改
2. 初始化 `HoHeartbeatPayload hb = { 6, 1, 0, 3 };` → 補第五個欄位
3. 解包後的欄位比對，補上 `hbSeq` 的比對
另外**新增一項**測試，把「payload 只增不減」這條相容性規則變成可執行的檢查：
```cpp
  // 加長 payload 的向前相容性：用「舊長度」解包新封包時，前 4 個欄位仍應正確
  check(sizeof(HoHeartbeatPayload) > 4, "心跳 payload 只增不減（Phase 5 由 4 加到 6）");
```

- [ ] **Step 2: master 送出心跳序號**

`sendHeartbeat()` 內，`hb.slaveCount = ...` 之後補：
```cpp
  // 序號由本函式獨佔遞增（不共用 txSeq —— 那是所有封包共用的傳送計數器，
  // 拿來當心跳序號會被單播指令、狀態查詢、LR_SET 一起推高，算出來的遺失率沒有意義）
  static uint16_t hbSeqCounter = 0;
  hb.hbSeq = hbSeqCounter++;
```

- [ ] **Step 3: slave 的心跳遺失率統計**

全域：
```cpp
// ── 實測儀器（Phase 5 Task 5）──
// 用心跳序號的差值算真實遺失率，不依賴任何對發送間隔的假設。
uint32_t hbStatCount = 0;
uint16_t hbStatFirstSeq = 0, hbStatLastSeq = 0;
bool     hbStatHaveFirst = false;
long     hbStatRssiSum = 0;
int      hbStatRssiMin = 0, hbStatRssiMax = -200;
unsigned long hbStatWindowStart = 0;
const unsigned long HB_STAT_WINDOW_MS = 60000;

// 現場模式：每個統計視窗結束時額外用面板 LED 閃 1~4 下表示遺失率等級。
// 預設關閉 —— 正式部署時不需要每分鐘閃一次。現場實測出發前用序列埠 `field on` 開。
// 這是「人在幾百公尺外、手邊沒有序列埠也能判讀」的最低成本手段。
bool fieldMode = false;

void resetHbStats(unsigned long now) {
  hbStatCount = 0; hbStatHaveFirst = false;
  hbStatRssiSum = 0; hbStatRssiMin = 0; hbStatRssiMax = -200;
  hbStatWindowStart = now;
}

void printHbStats() {
  if (!hbStatHaveFirst || hbStatCount == 0) {
    Serial.printf("[實測] %lu 秒內收到 0 則心跳（完全失聯）　LR=%s channel=%u\n",
                  HB_STAT_WINDOW_MS / 1000, lrRadioValue ? "開啟" : "關閉", lockedChannel);
    if (fieldMode) requestBlink(4, 150);
    return;
  }
  // 無號數減法，序號迴繞（65535 → 0）時仍然正確
  uint32_t span = (uint32_t)(uint16_t)(hbStatLastSeq - hbStatFirstSeq) + 1;
  float loss = (span > 0) ? (100.0f * (float)(span - hbStatCount) / (float)span) : 0.0f;
  if (loss < 0) loss = 0;   // 理論上不會發生，防守用
  Serial.printf("[實測] %lu 秒內收到 %u／區間 %u 則心跳（遺失 %.1f%%）"
                "rssi 平均 %ld 最好 %d 最差 %d　LR=%s channel=%u\n",
                HB_STAT_WINDOW_MS / 1000, (unsigned)hbStatCount, (unsigned)span, loss,
                hbStatRssiSum / (long)hbStatCount, hbStatRssiMax, hbStatRssiMin,
                lrRadioValue ? "開啟" : "關閉", lockedChannel);
  if (fieldMode) {
    // 遺失率等級：1 閃 = 很好、4 閃 = 幾乎不通。
    // 遠端的人每分鐘看一次面板 LED 就能判斷「還連不連得上」，不必接序列埠。
    uint8_t level = (loss < 5.0f) ? 1 : (loss < 20.0f) ? 2 : (loss < 50.0f) ? 3 : 4;
    requestBlink(level, 150);
  }
}
```

`onEspNowRecv()` 的 `HO_PKT_HEARTBEAT` 分支，在既有的降頻 log 之前插入：
```cpp
    // ── 實測統計（Phase 5 Task 5）──
    int hbRssi = info->rx_ctrl->rssi;
    // master 重開機會讓序號歸零，跳躍太大就視為新的一輪、整個重來，
    // 否則 span 會被算成六萬多而讓遺失率變成 99.9% 的假數字
    if (hbStatHaveFirst && (uint16_t)(hb.hbSeq - hbStatLastSeq) > 3000) {
      Serial.println("[實測] 心跳序號大幅跳躍（master 可能重開機），統計視窗重來");
      resetHbStats(millis());
    }
    if (!hbStatHaveFirst) {
      hbStatFirstSeq = hb.hbSeq; hbStatHaveFirst = true;
      hbStatRssiMin = hbRssi; hbStatRssiMax = hbRssi;
    }
    hbStatLastSeq = hb.hbSeq;
    hbStatCount++;
    hbStatRssiSum += hbRssi;
    if (hbRssi < hbStatRssiMin) hbStatRssiMin = hbRssi;
    if (hbRssi > hbStatRssiMax) hbStatRssiMax = hbRssi;
```

`loop()` 在 `updateBlink(now);` 那一段之後、`if (scanning)` **之前**插入
（**位置很重要**：失聯掃描期間正是最需要看到統計的時候，
放在 `if (scanning) { ... return; }` 之後會完全印不出來）：
```cpp
  // ── 實測統計視窗（Phase 5 Task 5）──
  // 刻意放在 scanning 的 early return 之前：失聯掃描期間正是最需要這行數字的時候。
  if (now - hbStatWindowStart >= HB_STAT_WINDOW_MS) {
    printHbStats();
    resetHbStats(now);
  }
```
`setup()` 最後補 `resetHbStats(millis());`。

- [ ] **Step 4: slave 加上最小的序列埠指令處理**

`ho_slave1.ino` 目前**完全沒有序列埠輸入處理**。現場實測需要在出發前開現場模式、
以及隨時手動查一次統計，所以補一個最小版本（放在 `loop()` 尾端）：

```cpp
  // ── 序列埠指令（Phase 5 Task 5：現場實測需要）──
  static String serialBuffer = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) { handleSerialCommand(serialBuffer); serialBuffer = ""; }
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 64) serialBuffer = "";   // 防溢位
    }
  }
```
```cpp
void handleSerialCommand(const String& line) {
  String cmd = line; cmd.trim();
  if (cmd.length() == 0) return;
  if (cmd == "stats") {
    printHbStats();          // 不重置視窗，只是提前看一眼
  } else if (cmd == "field on") {
    fieldMode = true;
    Serial.println("[實測] 現場模式開啟：每個統計視窗結束會用面板 LED 閃 1~4 下表示遺失率"
                   "（1=很好 2=尚可 3=不佳 4=幾乎不通）");
  } else if (cmd == "field off") {
    fieldMode = false;
    Serial.println("[實測] 現場模式關閉");
  } else if (cmd == "lr") {
    Serial.printf("[LR] 儲存值=%s 射頻現值=%s 救援中=%s\n",
                  longRangeEnabled ? "開啟" : "關閉",
                  lrRadioValue ? "開啟" : "關閉",
                  lrRescueActive ? "是" : "否");
  } else if (cmd == "info") {
    char id[20]; hoFormatDeviceId(masterMac, id);
    Serial.printf("[資訊] 本機=%s master=%s channel=%u 繼電器=%s 掃描中=%s\n",
                  getDeviceId(), masterKnown ? id : "（未配對）",
                  lockedChannel, relayState ? "開" : "關", scanning ? "是" : "否");
  } else if (cmd == "help") {
    Serial.println("── slave 指令 ──");
    Serial.println("  stats       立刻印一次心跳統計（不重置視窗）");
    Serial.println("  field on    開啟現場模式（LED 每分鐘閃 1~4 下表示遺失率等級）");
    Serial.println("  field off   關閉現場模式");
    Serial.println("  lr          查詢 Long Range 狀態");
    Serial.println("  info        查詢配對與繼電器狀態");
  } else {
    Serial.printf("未知指令：%s（輸入 help 看說明）\n", cmd.c_str());
  }
}
```
> **刻意不提供任何會動繼電器的序列埠指令。** slave 的繼電器只能由 master 控制，
> 多開一條路徑就多一種誤觸發的來源。

- [ ] **Step 5: master 的「全部關」到達率測試 `lrtest <n>`**

**這是 Phase 5 最重要的量測工具。** 它回答的是規格「系統本質」真正在問的問題：
**在這個距離上，一次廣播的「全部關」到底有幾成的門真的關上了。**

量測原理（不需要新增任何協定）：
- 廣播 `HO_PKT_CMD` + `HO_CMD_PULSE`（2000ms）。slave 的 `HO_PKT_CMD` 處理完會
  **主動** `sendState()`，回報 `relay=1`
- **收到 `relay==1` 的 `HO_PKT_STATE` ＝ 這台確實收到了那封廣播**
- 800ms 之後對還沒回報的台補送 `HO_PKT_STATE_REQ`：若補查回來 `relay` 仍是 1
  （點動 2000ms 還沒結束），代表**廣播有到、只是上行回報掉了**
- 於是可以把「到達」拆成「直接回報」與「補查確認」兩欄，
  把下行遺失與上行遺失分開看

```cpp
// ── 「全部關」到達率測試（Phase 5 Task 5）──
// ⚠ 這個測試會實際動作所有已配對 slave 的繼電器。
//    測試用的 slave 不得接在真實籠門機構上。
enum LrTestPhase { LRT_IDLE = 0, LRT_FIRE, LRT_WAIT, LRT_GAP };
LrTestPhase lrTestPhase = LRT_IDLE;
int lrTestRoundsTotal = 0;
int lrTestRound = 0;
unsigned long lrTestPhaseStart = 0;
unsigned long lrTestProbeAt = 0;
volatile uint32_t lrTestRoundMask = 0;   // 本輪已確認的台（callback 設 bit）
uint16_t lrTestHit[HO_ESPNOW_MAX_SLAVES];
uint16_t lrTestDirect[HO_ESPNOW_MAX_SLAVES];
long     lrTestRssiSum[HO_ESPNOW_MAX_SLAVES];
uint16_t lrTestRssiN[HO_ESPNOW_MAX_SLAVES];
bool    lrTestLrAtStart = false;
uint8_t lrTestChannelAtStart = 0;

const uint16_t      LRT_PULSE_MS  = 2000;
const unsigned long LRT_DIRECT_MS = 800;    // 這之前收到的算「直接回報」
const unsigned long LRT_WAIT_MS   = 1500;   // 本輪等待總長（必須 < LRT_PULSE_MS，
                                            // 否則補查時點動已結束、relay 回到 0）
const unsigned long LRT_PROBE_GAP_MS = 120; // 補查時每台之間的間隔
const unsigned long LRT_GAP_MS    = 4000;   // 輪與輪之間，讓點動結束、射頻回到閒置

static_assert(LRT_WAIT_MS < LRT_PULSE_MS,
              "補查窗必須落在點動still ON 的期間內，否則 relay 已回 0，"
              "補查確認會全部誤判成沒收到");

void abortLrTest(const char* why) {
  if (lrTestPhase == LRT_IDLE) return;
  lrTestPhase = LRT_IDLE;
  Serial.printf("[實測] 全關測試已中止：%s\n", why);
}

void printLrTestResult() {
  Serial.printf("[實測] 全關測試結束：共 %d 輪，LR=%s，channel=%u，名冊 %d 台\n",
                lrTestRound, lrTestLrAtStart ? "開啟" : "關閉",
                lrTestChannelAtStart, (int)slaveCount);
  for (int i = 0; i < slaveCount && i < HO_ESPNOW_MAX_SLAVES; i++) {
    char id[20];
    hoFormatDeviceId(slaves[i].mac, id);
    float pct = (lrTestRound > 0) ? (100.0f * lrTestHit[i] / (float)lrTestRound) : 0.0f;
    long avgRssi = (lrTestRssiN[i] > 0) ? (lrTestRssiSum[i] / (long)lrTestRssiN[i]) : 0;
    Serial.printf("[實測]   %s  到達 %u/%d (%.1f%%)  直接回報 %u  補查確認 %u  rssi 平均 %ld\n",
                  id, lrTestHit[i], lrTestRound, pct,
                  lrTestDirect[i], (uint16_t)(lrTestHit[i] - lrTestDirect[i]), avgRssi);
  }
  Serial.println("[實測] 「到達」＝那一輪的廣播確實被該台收到（直接回報或補查時仍在點動）；"
                 "「直接回報」與「補查確認」的差額就是上行回報的遺失");
}

void startLrTest(int rounds) {
  if (lrTestPhase != LRT_IDLE) { Serial.println("[實測] 上一次測試尚未結束"); return; }
  if (lrPhase != LR_IDLE) { Serial.println("[實測] LR 切換進行中，請稍候再測"); return; }
  if (slaveCount == 0) { Serial.println("[實測] 名冊是空的，沒有東西可測"); return; }
  if (rounds < 1) rounds = 1;
  if (rounds > 200) rounds = 200;

  for (int i = 0; i < HO_ESPNOW_MAX_SLAVES; i++) {
    lrTestHit[i] = 0; lrTestDirect[i] = 0; lrTestRssiSum[i] = 0; lrTestRssiN[i] = 0;
  }
  lrTestRoundsTotal = rounds;
  lrTestRound = 0;
  lrTestLrAtStart = longRangeEnabled;
  lrTestChannelAtStart = currentChannel;
  lrTestPhase = LRT_FIRE;
  lrTestPhaseStart = millis();
  Serial.printf("⚠ [實測] 開始全關測試 %d 輪（每輪約 %lu 秒，約 %lu 秒完成）——"
                "所有已配對 slave 的繼電器都會實際動作 %u ms，"
                "測試用的 slave 不得接在真實籠門機構上\n",
                rounds, (LRT_WAIT_MS + LRT_GAP_MS) / 1000,
                rounds * (LRT_WAIT_MS + LRT_GAP_MS) / 1000, LRT_PULSE_MS);
}

void updateLrTest(unsigned long now) {
  if (lrTestPhase == LRT_IDLE) return;

  if (lrTestPhase == LRT_FIRE) {
    lrTestRound++;
    lrTestRoundMask = 0;
    // 直接廣播，刻意「不」走 sendCmdToAll()：
    //   (a) sendCmdToAll() 目前是逐台單播（Phase 1 的寫法），而規格「系統本質」
    //       明確要求群組指令走廣播 —— 本測試要量的正是那條「規格要求的」路徑
    //   (b) 走 sendCmdToAll() 會連 master 自己的繼電器一起動，與量測無關
    //   (c) 走 sendCmdToAll() 會呼叫 lrNoteRelayCommand()，本測試自己就會被自己中止
    HoCmdPayload payload;
    payload.cmd = (uint8_t)HO_CMD_PULSE;
    payload.pulseMs = LRT_PULSE_MS;
    espNowSendTo(BROADCAST_MAC, HO_PKT_CMD, &payload, sizeof(payload));
    Serial.printf("[實測] 第 %d/%d 輪：已廣播點動指令\n", lrTestRound, lrTestRoundsTotal);
    lrTestPhase = LRT_WAIT;
    lrTestPhaseStart = now;
    lrTestProbeAt = 0;
    return;
  }

  if (lrTestPhase == LRT_WAIT) {
    if (now - lrTestPhaseStart >= LRT_WAIT_MS) {
      lrTestPhase = LRT_GAP;
      lrTestPhaseStart = now;
      return;
    }
    // 800ms 之後才開始補查：在那之前收到的算「直接回報」
    if (now - lrTestPhaseStart < LRT_DIRECT_MS) return;
    if (now - lrTestProbeAt < LRT_PROBE_GAP_MS) return;
    lrTestProbeAt = now;
    for (int i = 0; i < slaveCount && i < 32; i++) {
      if (!(lrTestRoundMask & (1UL << i))) { requestSlaveState(i); return; }
    }
    return;
  }

  // LRT_GAP
  if (now - lrTestPhaseStart < LRT_GAP_MS) return;
  if (lrTestRound >= lrTestRoundsTotal) {
    lrTestPhase = LRT_IDLE;
    printLrTestResult();
    return;
  }
  lrTestPhase = LRT_FIRE;
  lrTestPhaseStart = now;
}
```

`onEspNowRecv()` 的 `HO_PKT_STATE` 分支補（在 `lrVerifyMask` 那兩行旁邊）：
```cpp
    // 全關測試計分：收到 relay==1 的回報 ＝ 那封廣播確實被這台收到了
    if (lrTestPhase == LRT_WAIT && idx < 32 && st.relay == 1 &&
        !(lrTestRoundMask & (1UL << idx))) {
      lrTestRoundMask |= (1UL << idx);
      lrTestHit[idx]++;
      if (millis() - lrTestPhaseStart < LRT_DIRECT_MS) lrTestDirect[idx]++;
      lrTestRssiSum[idx] += info->rx_ctrl->rssi;
      lrTestRssiN[idx]++;
    }
```

`lrNoteRelayCommand()` 的**第一行**（在 `if (lrPhase != LR_ANNOUNCING) return;` **之前**）補：
```cpp
  // 使用者下了真的繼電器指令 → 測試立刻讓位。實測是次要的，控制是主要的。
  abortLrTest("收到繼電器指令");
```
`startLongRangeSwitch()` 開頭補：
```cpp
  if (lrTestPhase != LRT_IDLE) {
    Serial.println("[LR] 全關測試進行中，拒絕切換（先用 lrtest 0 中止）");
    return;
  }
```

`loop()` 在 `updateLrBootGuard(now);` 之後補 `updateLrTest(now);`。

`handleSerialCommand()` 加 `lrtest`（**要走 `parseIndexArg()`，加進 `needsArg` 清單**；
`lrtest 0` 代表中止）：
```cpp
  } else if (verb == "lrtest") {
    if (arg == 0) abortLrTest("使用者中止");
    else          startLrTest(arg);
```
`printHelp()` 補：
```cpp
  Serial.println("  lrtest <n>    實測用：連跑 n 輪「全部關」到達率測試（0 = 中止）");
  Serial.println("                ⚠ 會實際動作所有 slave 的繼電器，勿接真實籠門");
```

- [ ] **Step 6: 編譯驗證**

```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
.\flash.ps1 -Model test
```
**四者皆 exit code 0**（動到共用函式庫，`-Model test` 是實質驗證）。
記錄 flash／RAM，並與 Task 4 比較。

**RAM 檢查點**：master 新增 `lrTestHit/Direct/RssiSum/RssiN` 四個陣列，
20 台合計 `20×(2+2+4+2) = 200` bytes，可忽略。

- [ ] **Step 7: Commit**

commit 訊息必須包含：為什麼心跳要加序號（`HEARTBEAT_INTERVAL_ASSOC` 會讓
「則數÷秒數」的推算失真）、加長 payload 的相容性方向與**燒錄順序必須先 master 後 slave**、
`lrtest` 的量測原理（`relay==1` 的回報就是「廣播確實到達」的證明）、
為什麼 `lrtest` 直接廣播而不走 `sendCmdToAll()`（三個理由）、
以及 `LRT_WAIT_MS < LRT_PULSE_MS` 的 `static_assert` 在守什麼。

---

## Task 6：現場實測程序、回歸清單、readme

**Task 5 之前的所有工作都是為了讓這個 Task 的產出有東西可量。**
`docs/phase5-field-test-procedure.md` 是**要交給使用者拿到現場執行**的文件，
不是給實作者看的 —— 寫作對象、用詞、詳細程度都要照這個前提調整。

**Files:**
- Create: `docs/phase5-field-test-procedure.md`
- Create: `docs/phase5-regression-checklist.md`
- Modify: `ho_master1/readme.md`
- Modify: `ho_slave1/readme.md`
- Modify: `docs/lr-idf-behavior-findings.md`（把「對 Phase 5 設計的影響」一節補完）

---

- [ ] **Step 1: 寫 `docs/phase5-field-test-procedure.md`**

### 這份文件的核心設計（實作者必須理解才寫得對）

**問題：人在幾百公尺外看不到序列埠。** 所以整份程序建立在三件事上：

1. **權威數據一律在 master 端產生**（基地台有筆電）。遠端那個人不需要判讀任何數字，
   他只要「就位、回報距離、確認繼電器有沒有動」。
2. **遠端的判讀手段有三層冗餘**，由強到弱：
   - **手機 + USB-C OTG 線 + 序列終端 App** 直接讀 slave 的 USB CDC（ESP32-C3 原生 USB）
     → 完整序列埠輸出，包含每分鐘一行的 `[實測]` 統計。**這是最強的手段，列進器材清單。**
   - **面板 LED 的現場模式**（`field on`）→ 每分鐘閃 1~4 下表示遺失率等級，
     不需要任何工具，抬頭看一眼就知道還連不連得上
   - **繼電器動作的聲音與 LED 亮 2 秒** → `lrtest` 每一輪都會動作，
     遠端的人可以直接確認「剛才那一輪到了沒」
3. **兩人之間必須有獨立於待測系統的通訊**（手機通話或無線電）。
   用被測系統本身當通訊手段是循環論證。

### 文件必須包含的章節

**一、器材與人力**

| 項目 | 數量 | 備註 |
|---|---|---|
| 人 | **最少 2 人**，3 人更順 | 一人守基地台（筆電＋master），一人帶 slave 移動；第三人負責記錄與拍照 |
| `ho_master1` 板 | 1 | WROOM 或 C3 都可，但**兩種模式的測試必須用同一片**，否則天線差異會混進結果 |
| `ho_slave1` 板 | **2** | 一片跟著移動，**一片留在基地台旁 3 公尺內當對照組** —— 對照組的到達率若也掉，代表是 master 或環境的問題，不是距離 |
| 行動電源 + USB 線 | 各 2 | slave 與手機都要 |
| 筆電 | 1 | 基地台，序列終端 115200 |
| Android 手機 + USB-C OTG | 1 | 遠端讀 slave 序列埠（如 Serial USB Terminal 之類的 App） |
| 通話手段 | 2 | 手機或無線電，**不可以用被測系統** |
| 距離量測 | 1 | 手機 GPS + 地圖量距，或測距輪 |
| 安全 | 視場地 | 反光背心、三角錐 |

**二、場地要求**
- **視線可及（LOS）**、無建物遮蔽的直線。河堤自行車道、產業道路、大型空地、農地
- 至少能拉到 **1.5 公里**（LR 模式若真的有效，標準模式的極限會被大幅拉開）
- 避開強 2.4GHz 干擾源（大型賣場、活動會場、微波爐、密集的家用 AP）
- **同一天、同一條路線、同樣的架設高度**做完兩種模式。分兩天做的資料不可互相比較

**三、事前準備（在室內做完，不要到現場才發現）**
1. **燒錄順序：先 master、後 slave。** 心跳 payload 在 Phase 5 加長了，
   舊 master 對新 slave 不相容（心跳被丟棄 → 30 秒後 slave 強制關閉繼電器）
2. 兩片 slave 都完成配對，master `list` 看得到兩台在線
3. **確認測試用的 slave 沒有接任何真實籠門機構** —— `lrtest` 會實際動作繼電器
4. slave 端下 `field on`（現場模式），確認每分鐘會閃燈
5. master 端下 `lr` 確認目前模式，並記下 `channel`
6. 桌上先跑一次 `lrtest 10` 當**距離 0 的基準線**，到達率應接近 100%。
   **基準線若不是接近 100%，先停下來查問題，不要出門**

**四、每一個距離點的標準流程**（兩人分工寫成兩欄，遠端那欄的動作要少而明確）

| 步 | 基地台（筆電 + master） | 遠端（slave + 手機） |
|---|---|---|
| 1 | — | 走到定點，回報 GPS 座標／距離 |
| 2 | `list` → 記錄兩台的在線與 rssi | — |
| 3 | — | 等一次 `[實測]` 統計行（最多 60 秒），念出「收到 X／區間 Y，遺失 Z%，rssi 平均」 |
| 4 | `lrtest 20` → 等約 110 秒 → 抄下結果表格 | 目視確認繼電器有沒有動（LED 亮 2 秒） |
| 5 | `lr` → 確認 LR 狀態沒有被守衛或回滾改掉 | `lr` → 確認兩端一致 |
| 6 | 記錄 master 的 WiFi／MQTT 是否仍連線 | — |

**距離點**：0（桌上）、50、100、200、300、500、800、1200 m
（標準模式通常在前幾點就會結束；LR 模式才需要跑到後面）

**五、記錄表格模板**（每一種模式各一份）

```
模式：□ 標準（LR 關）　□ Long Range（LR 開）
日期：____　地點：____　天氣：____　master 型號：□ WROOM □ C3　channel：__

距離  ｜移動端到達率  ｜直接/補查 ｜移動端心跳遺失｜移動端rssi｜對照組到達率｜備註
─────┼──────────┼────────┼───────────┼────────┼──────────┼────
  0m ｜      /20    ｜   /     ｜      %     ｜        ｜     /20  ｜基準線
 50m ｜      /20    ｜   /     ｜      %     ｜        ｜     /20  ｜
100m ｜      /20    ｜   /     ｜      %     ｜        ｜     /20  ｜
...
```

**六、怎麼判斷「還連得上」（現場判準，不要靠感覺）**

按可信度由高到低：

1. **`lrtest` 的到達率** —— 這是唯一直接量到「一次全部關會不會到」的數字。**以它為準。**
2. **心跳遺失率** —— 廣播下行的純粹指標，走的是與「全部關」完全相同的無線電路徑。
   它會比到達率更早惡化，是很好的**先行指標**
3. **master `list` 顯示的在線／離線** —— 只代表 30 秒內有回過話，**門檻很低**，
   「顯示在線」完全不保證關門指令會到。**不可以拿它當可用距離的判準**
4. **RSSI** —— 只是輔助。**RSSI 好不代表封包會到**，尤其在 LR 模式下兩者的關係會改變。
   絕對不可以單獨用 RSSI 下結論

**中止準則**：連續兩個距離點的**移動端到達率 < 50%**，就把**前一個 ≥ 90% 的點**
記為該模式的可用距離上限，該模式測試結束。

**七、安全與異常處理**
- `lrtest` 期間 slave 繼電器每輪動作 2 秒。**不得接真實籠門機構**
- 遠端 slave 若失聯超過 30 秒，它會**強制關閉繼電器並開始輪掃 channel**，
  之後還會**自動把 LR 對調試 3 秒**（設計如此，不是故障）。
  序列埠會印 `[失聯]`、`[安全]`、`[LR] 失聯救援：…`
- master 若在切換 LR 之後 10 秒的驗證階段沒收到足夠回報，**會自動回滾**並印
  `[LR] 驗證失敗：…`。**這是正常行為**，記錄下來即可
- master 開機後 90 秒內若一台 slave 都沒回報且 LR 是開的，**會自動關閉 LR**
  （`[LR] 開機守衛：…`）。**測試中若看到這行，代表當時真的完全失聯**
- 現場需要把 master 弄回最保守狀態時：**長按 master 的按鈕 5 秒** ——
  會清除網路設定**並關閉 LR**（配對名冊保留）

**八、資料判讀與出貨預設值的決策規則**

先定義：**該模式的「可用距離」＝ 移動端到達率 ≥ 90% 的最遠距離點。**

| 結果 | 出貨預設 |
|---|---|
| LR 的可用距離 ≥ 標準模式的 **1.5 倍** | **預設開啟 LR** |
| 介於 1.2~1.5 倍 | 預設關閉，但在 readme 明確寫出「現場距離不足時可開 LR，可增加約 X 倍」 |
| < 1.2 倍 | **預設關閉**，並記錄「混合 bitmap 下 LR 沒有實質增益」，
  接著評估規格「難點 2」的備案 `esp_now_set_peer_rate_config()`（Task 1 測項 D 已量過可用性） |
| LR 讓**近距離**的到達率下降 | **一律預設關閉**，並把這個現象寫進 readme 已知風險 |

**九、實測結果回填欄位**（文件最後留白，測完由使用者填）
- 兩種模式的可用距離
- 決定的出貨預設值與理由
- 過程中觸發過的自動機制（回滾／開機守衛／slave 救援）各幾次
- Task 1 測項 A 的產品端複驗結果（切換 LR 之後 master 的 WiFi／MQTT 有沒有斷、幾秒回來）

- [ ] **Step 2: 寫 `docs/phase5-regression-checklist.md`**

> ### 這一步有一條硬性規定，違反即視為 Task 未完成
>
> **Phase 2a 有一類缺陷出現了三次：回歸清單的驗收標準與程式碼矛盾，
> 導致實測者把正確行為判成 FAIL。**（最後一次是清單寫「按住 1.5 秒放開不應印出
> 任何訊息」，但程式碼按下瞬間就無條件印出訊息，100% 觸發。）
>
> 因此本清單的**每一條「預期序列埠輸出」都必須逐字對照實際程式碼的
> `Serial.print` / `Serial.printf` 格式字串確認**：
>
> 1. 寫下預期輸出後，用 `Grep` 在 `ho_master1/ho_master1.ino`／`ho_slave1/ho_slave1.ino`
>    搜尋該字串的**固定部分**（去掉 `%d`／`%s`／`%lu` 等格式指示子的那一段）
> 2. **搜不到就是寫錯了**，不准靠記憶或推測補上
> 3. 每一項結尾用一行註記寫出對照過的位置，格式：
>    `> 對照：ho_master1.ino:1234 的 Serial.printf("[LR] 結束握手（%s）：...")`
> 4. **凡是「不應該印出某訊息」這類否定式判準一律禁止**，除非能明確指出程式碼中
>    沒有任何路徑會印出它。改寫成正面判準
> 5. 每一項都要區分「**失敗判定**」與「**觀察項**」。凡是機制未經實機驗證的
>    （`esp_wifi_set_protocol()` 會不會斷線、混合 bitmap 的互通性），
>    **一律列為觀察項，不得列為失敗判定**
>
> **另外兩項本階段特有的硬性規定：**
>
> 6. **本階段改動了兩條既有訊息**（slave 的 `已配對 master: ...` 加了 LR 欄位、
>    master 長按重置的說明訊息加了「並關閉 Long Range」）。
>    **必須回頭 grep `docs/phase1-regression-checklist.md` 與
>    `docs/phase2a-regression-checklist.md` 有沒有引用舊字串當判準**，有就一併更正。
> 7. **Task 1 的 findings 文件若仍是空白骨架**，清單裡任何與「混合 bitmap 互通性」
>    有關的項目一律寫成觀察項，且**不得出現「不同步只會失去距離增益」這句話**

清單開頭必須有與前幾階段同樣的警告：**本清單尚未在任何實體硬體上執行過任何一項。**

必須涵蓋的項目（至少）：

| # | 項目 | 類型 | 重點 |
|---|---|---|---|
| 1 | **燒錄順序**：先 master 後 slave；反過來時 slave 會在 30 秒後印 `[失聯]` 並關閉繼電器 | 前置 | 心跳 payload 加長的相容性方向 |
| 2 | master 開機印出 `[LR] 開機載入設定：關閉`，`applyLongRange` 印出讀回一致 | 失敗判定 | Task 2 |
| 3 | slave 開機印出 `已配對 master: ...，上次 channel=n，LR=關閉` | 失敗判定 | **這行字串本階段改過** |
| 4 | **C3 支援性**：兩端 `lr` 查詢時「射頻現值」與「設定」一致；若讀回不一致會印警告 | 失敗判定 | Task 1 測項 C 的產品端複驗 |
| 5 | `LR:ON`（或序列埠 `lr on`）：master 印開始切換 → 每台 slave 印已對齊 → master 印全部確認 → 印進入驗證階段 → 印驗證通過 | 失敗判定 | 完整成功路徑 |
| 6 | 同上，狀態 JSON 的 `device.long_range` 變 `true`、`long_range_pending` 在切換期間為 `true`、結束後為 `false`、`long_range_error` 為空字串 | 失敗判定 | MQTT Explorer 對照 |
| 7 | **切換全程其他 slave 不失聯**（沒有任何一台印 `[失聯] 超過 30 秒沒收到心跳`） | **失敗判定** | 本階段的核心約束 |
| 8 | **拔掉一台 slave 再切換**：master 逾時 10 秒後印未確認名單並仍套用；該台復電後由心跳對齊 | 失敗判定 | 逾時路徑 + 自癒 |
| 9 | **驗證失敗與回滾**：切換後立刻把所有 slave 斷電 → master 10 秒內印 `[LR] 驗證失敗` 並回滾 → `long_range` 回到舊值、`long_range_error` 為 `verify_failed` | 失敗判定 | 決定 3 |
| 10 | **緊急插隊**：`LR:ON` 之後 2 秒內對 master 送 `ALL:ON`（或序列埠 `allpulse`）→ master 印「握手期間收到繼電器指令」→ 印結束握手 → 印補送 → **所有 slave 都動作了兩次** | **失敗判定** | 決定 2，本計畫最重要的一條 |
| 11 | 同上，兩次動作之間的間隔目測 < 1 秒 | 觀察項 | 落差不得擴大到規格拒絕的 400ms 量級以上太多 |
| 12 | **slave 失聯自我救援**：把 master 斷電 → slave 30 秒後依序印 `[失聯]`、`[安全] 失去 master，繼電器已關閉`、`[掃描]`、`[LR] 失聯救援：把射頻對調成…`，3 秒後印切回原值 | 失敗判定 | 決定 4；**注意訊息順序：關繼電器一定在救援之前** |
| 13 | master 復電後 slave 印 `[鎖定]`，且若 LR 不同會印 `[LR] 依心跳對齊為…` | 失敗判定 | |
| 14 | **開機守衛**：LR 開著時把所有 slave 斷電再重開 master → 90 秒後印 `[LR] 開機守衛：…` 且 `long_range` 變 `false`、`long_range_error` 為 `boot_guard` | 失敗判定 | 決定 5 第 2 層 |
| 15 | 同上，slave 復電後由心跳自癒跟著關閉，兩端一致 | 失敗判定 | 「誤判的代價是無害的」這個宣稱的正面驗證 |
| 16 | **長按重置關閉 LR**：LR 開著時長按 5 秒 → 印 `[重置] 已一併關閉 Long Range（現場回滾手段）` → 重開機後 `[LR] 開機載入設定：關閉` | 失敗判定 | 決定 5 第 3 層；**這是與 `reset` 指令刻意不同的行為** |
| 17 | **`reset` 指令不動 LR**：LR 開著時送 MQTT `reset` → 重開機後 `[LR] 開機載入設定：開啟` | 失敗判定 | 存 `homaster` 而非 `hoban` 的正面驗證 |
| 18 | 全部離線時下 `LR:ON` → 印 `[LR] 名冊上 n 台全部離線，拒絕切換`，`long_range_error` 為 `no_online_slave` | 失敗判定 | 守門 |
| 19 | **`lrtest 5`**：印出每輪的廣播訊息，結束後印出表格；桌上距離的到達率應接近 100% | 失敗判定 | Task 5 |
| 20 | `lrtest` 期間對 master 送 `ALL:OFF` → 印 `[實測] 全關測試已中止：收到繼電器指令` | 失敗判定 | 安全指令優先 |
| 21 | slave 每 60 秒印一行 `[實測] 60 秒內收到 …`，桌上距離遺失率應接近 0% | 失敗判定 | Task 5 |
| 22 | slave `field on` 後每 60 秒面板 LED 閃 1 下（桌上距離） | 失敗判定 | 現場判讀手段 |
| 23 | LR 切換後 master 的 WiFi 是否斷線、幾秒回來、channel 有沒有變 | **觀察項，不是失敗判定** | Task 1 測項 A 的產品端複驗 |
| 24 | 兩端 bitmap 不同步期間 ESP-NOW 是否仍通 | **觀察項** | Task 1 測項 B 的產品端複驗 |
| 25 | **回歸 Phase 2a/2b**：BLE 配網、長按重置、`FIND_BEST_SERVER`、`HASRELAY:*`、代發 topic、`fakeslaves 20` + `jsonsize` 行為不變 | 失敗判定 | `jsonsize` 要重新記錄數字（多了 `long_range_error` 欄位） |
| 26 | **回歸**：WiFi 拔線 60 秒，slave 全程不失聯 | **失敗判定** | Phase 2a 的既有約束 |
| 27 | **協定測試**：`.\flash.ps1 -Model test` 燒錄後全部通過，含新增的心跳 payload 大小檢查 | 失敗判定 | |

- [ ] **Step 3: 更新 `ho_master1/readme.md`**

新增或更新這些章節：
1. **Long Range**（新章節，放在「ESP-NOW 心跳的實際保證」之後）：
   - 切換流程的四個階段與各自的時間上界，附「整條路徑 24.4 秒 < 30 秒門檻」的算式
   - 三層回滾手段（10 秒驗證回滾／90 秒開機守衛／長按重置），**明確寫出長按重置會關 LR
     而 MQTT `reset` 不會，以及兩者為什麼不同**
   - 緊急指令的「送兩次」行為，以及使用者會看到繼電器動作兩次是**設計如此**
   - **`master 端的回滾是必要但不充分的`** —— 已經切過去的 slave 要靠自己的 30 秒救援
2. **MQTT 指令表**：補 `LR:ON` / `LR:OFF`，註明送到 master 自己的 control topic
3. **狀態 JSON**：補 `device.long_range_pending`、`device.long_range_error`
   （列出所有可能的字串值與含義），並更新容量常數的算式
4. **序列埠指令表**：補 `lr [on|off]`、`lrtest <n>`
5. **已知風險**（新增）：
   - **`esp_wifi_set_protocol()` 在已關聯 AP 時的行為**：指向
     `docs/lr-idf-behavior-findings.md`；文件若尚未填寫就明講「未驗證」
   - **混合 bitmap 的互通性**：同上。**findings 未填寫時，readme 不得出現
     「不同步只會失去距離增益」這句話**
   - **心跳 payload 加長的燒錄順序**：先 master 後 slave，反過來會開籠
   - **LR 的距離效益未經實測**：指向 `docs/phase5-field-test-procedure.md`

- [ ] **Step 4: 更新 `ho_slave1/readme.md`**

1. **EEPROM 佈局**章節：`EE_ADDR_LONGRANGE`（位址 8）從「保留」改成「已使用」
2. 新增 **Long Range** 章節：接收切換、心跳自癒、失聯自我救援（含
   **「先關繼電器、再救援」的順序與理由**）
3. 新增 **序列埠指令**章節（slave 本來完全沒有）：`stats`／`field on|off`／`lr`／`info`／`help`，
   並註明**刻意不提供任何會動繼電器的指令**
4. **安全預設**章節補上：失聯 30 秒 → 關繼電器 → LR 對調救援 3 秒 → channel 輪掃，
   三者的順序與各自的時間

- [ ] **Step 5: 補完 `docs/lr-idf-behavior-findings.md` 的「對設計的影響」一節**

把每個測項的結果會改動計畫的哪一個決定寫成對照表，讓日後填表的人知道
「這個數字填下去之後要回頭改什麼」。

- [ ] **Step 6: 完整編譯驗證與資源盤點**

```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model master-c3
.\flash.ps1 -Model slave
.\flash.ps1 -Model test
.\flash.ps1 -Model lrprobe
```
五者皆 exit code 0。

**在 report 中做一張表**，列出從 Phase 5 開工到完成的 flash／RAM 變化：

| 型號 | 開工時 flash | 完成時 flash | 增量 | 對 app0 百分比 | RAM |
|---|---|---|---|---|---|
| master (WROOM) | | | | | |
| master-c3 | | | | | |
| slave | | | | | |
| test | | | | | |

**WROOM 若超過 1,930,035 bytes（95%）就標紅並停下來回報。**

- [ ] **Step 7: 更新 SDD ledger**

在 `.superpowers/sdd/2026-08-17-esp32-phase5-long-range/progress.md` 記錄：
- 每個 Task 的 commit 範圍與 flash 數字
- 所有 Ruling（含被推翻的）
- **Task 1 的 findings 文件是否已由使用者填寫**；未填寫要明確標示為交付項
- **本階段推翻的 Phase 2b 決定**：長按重置一併關閉 LR
- 交付給使用者判斷的殘留項

- [ ] **Step 8: Commit**

---

## 本階段結束後的狀態

- `LR:ON` / `LR:OFF` 可用，且切換的每一條路徑（成功／逾時／驗證失敗／緊急插隊／
  開機守衛／長按重置）都有明確的時間上界，總和 24.4 秒 < 30 秒失聯門檻，
  且用 `static_assert` 鎖在編譯期
- 三個未驗證的 IDF 行為有了獨立的探針與一份可填寫的事實表
- 兩端都有自我恢復能力：master 有 10 秒驗證回滾與 90 秒開機守衛，
  slave 有 30 秒失聯後的 LR 對調救援
- 「一次全部關」的到達率變成一個 master 自己會算、自己會列表的數字
- 有一份可以直接拿到現場執行的實測程序
- **尚未有**：實測數字本身、出貨預設值的決定（那要等使用者執行 Task 6 的程序）

## 已知風險

| 風險 | 影響 | 緩解 |
|---|---|---|
| **Task 1 的 findings 文件沒有人去填** | 整個階段的設計前提停在「未驗證」，readme 與回歸清單只能寫觀察項 | 本計畫的每個決定都刻意不依賴測項 B；探針與文件骨架已備妥，執行成本約 30 分鐘 |
| **ESP32-C3 不支援 `WIFI_PROTOCOL_LR`** | slave 全部是 C3，整個功能只剩 master 單邊 = 無效 | **風險已由文獻查證大幅下降**：官方文件明講例外只有 ESP32-C2。仍由 Task 1 測項 C 實測；`applyLongRange()` 的讀回驗證讓產品韌體自己也看得見；Task 2 Step 0 指定此情況要停下來回報 |
| **混合 bitmap 下兩端不同步會互相收不到**（宣稱 B 不成立，**規劃階段的文獻查證認為這才是比較可能的情況**） | 切換過程中一部分 slave 短暫失聯 | 每台的實際失聯窗口最壞 13.2 秒（遠低於 30 秒）；緊急指令送兩次覆蓋兩邊；slave 端有 30 秒對調救援 |
| **`esp_wifi_set_protocol()` 造成 WiFi／MQTT 斷線** | LR 切換後短暫失去遠端控制 | 套用前後各連發心跳；套用後 `onWifiChannelMayHaveChanged()`；`loop()` 既有的重連機制接手；回歸清單列為**觀察項不是失敗判定** |
| **master 回滾救不了已經切過去的 slave** | 那些台要等自己 30 秒失聯救援，期間繼電器已被強制關閉 | 這是設計上的已知限制，不是缺陷 —— 恢復保證來自 slave 端而非 master 端。已寫入 readme 與 `finishVerify()` 的序列埠輸出 |
| **心跳 payload 加長，燒錄順序反了會開籠** | 舊 master 對新 slave 的心跳被丟棄 → 30 秒後強制關閉繼電器 | 協定標頭寫明相容性方向；readme 與回歸清單第 1 項都是燒錄順序；**本階段沒有現場部署，代價最低** |
| **`lrtest` 會實際動作繼電器** | 測試時若接了真實籠門就是在開關真的門 | 指令本身印出警告；實測程序文件的事前準備第 3 項；readme 序列埠指令表加註 |
| **60% 驗證門檻可能誤判** | 正常切換被判失敗而回滾（多一次射頻中斷） | 1~2 台的小名冊改成「至少一台回應」；回滾不自動重試所以不會震盪；門檻是具名常數，實測後可調 |
| WROOM flash 餘裕 | Phase 4 若已執行，本階段再加約 8~12 KB | 每個 Task 記錄用量；95% 紅線 |
| `lrAckMask`／`lrVerifyMask`／`lrTestRoundMask` 的 read-modify-write 競態 | 漏記一次 ACK 或一次命中 | 只在階段結束的瞬間可能發生；ACK 漏記的後果只是序列埠多印一台「未確認」；`lrtest` 漏記會讓到達率**低估**（保守方向），不會高估 |
