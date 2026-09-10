# -*- coding: utf-8 -*-
"""文件判準回讀驗證 —— **必須跑兩個方向**。

為什麼要有這支腳本，以及為什麼「只跑一個方向」是不夠的：

Phase 4 Task 1 的 review M4 抓到 `ho_master1/readme.md` 的
「`static_assert` 的算式與現行數字」整節沒更新（仍寫 `/104 = 23`），
**而同一份文件的其他兩處已經改成新值**。當時的驗證腳本只跑了
「新字串必須出現（HIT）」一個方向，從來沒跑「**被取代的舊值必須消失（MISS）**」，
所以整節過期完全驗不出來 —— 而 report 還同時宣稱「欄位表已更新」與
「腳本逐字回讀全部命中」，兩句字面為真、合起來造成假的覆蓋印象。

這正是 A 族（覆蓋宣稱與事實不符）與 B 族（判準與程式碼矛盾）在文件層的同一個形狀。
規則見 .claude/rules/claim-what-it-does-not-block.md。

用法（在 repo 根目錄）：
    python tools/check_doc_claims.py
全部通過印 ALL CHECKS PASSED，任何一項失敗以 exit code 1 結束。

**十九個**驗證方向（第 7 個 PLAN 方向見下方 BANNED_IN_PLAN）。
這個數字自己就是一個判準 —— 初版寫「八個」而實際是九個，
**一支專門檢查「文件寫死的 N 項」的腳本自己數錯**，所以改動方向時請一起數：
  1. HIT     —— 文件引用的判準字串必須逐字出現在原始碼裡
  2. BANNED  —— 被取代的舊值不得再以「現行事實」的樣子出現（**含原始碼註釋**）
  3. 行號    —— `檔名.ino:123` 這種對照一律禁止，判準要用字串錨點
  4. 項數    —— 數原始碼的 check() 呼叫，回頭比對文件寫死的「執行 N 項」
  5. 算式    —— 任何 `(A-1-B-C)/D = [E/D =] F` 的容量算式，逐項對原始碼常數複算
  6. 數值    —— 單獨寫出來的 `maxEntries` N 必須等於算出來的值
  8. 禁用 API —— `ho_master1.ino` 的**非註釋行**不得出現
                `esp_ota_set_boot_partition` / `#include <Update.h>` / `Update.begin(`
                / `setRedirectLimit` / `HTTPC_STRICT_FOLLOW_REDIRECTS`
                / `HTTPC_FORCE_FOLLOW_REDIRECTS`
  9. LR 掛鉤 —— `otaSessionBusy()` 的呼叫點數量必須仍是 **6**（定義 1 ＋ `otaStart()` 1
                ＋ `otaRelayStaged()` 1 ＋ `update_slave` 2 ＋ `updateStatusLed()` 1）
                （readme 宣稱「LR 互斥守衛現在並不存在」，這條讓那句話會過期時吵）
                ⚠ 這個數字在 `00a934c` 由 3 改成 6，**而這一行當時漏改** ——
                一支專門檢查「文件寫死的 N 項」的腳本，自己的說明過期了三輪。
 10. 文件必含 —— 「宣告某道保護不存在」的句子本身不得被刪（方向 9 的另一半）
 11. 假綠燈  —— `otaFinish()` 的函式本體不得出現「已更新到」
 12. 設定唯一 —— 關鍵 timeout／重定向設定必須**剛好出現一次**且逐字如此
                （HIT 擋不住「再加一行覆蓋它」，也擋不住「整行被刪掉」）
 13. 順序    —— `otaHttp->begin()` 必須排在 `otaTls->connect()` 之前（C5）
 14. 轉送燈號 —— Task 4 的兩個假燈號出口。**存在性、位置、以及「不得多開一條
                析取」三個維度一起驗**（只驗存在性會被「一字不改、只搬位置」
                整份繞過 —— A 族第 18、19 次）
 15. 轉送送出點 —— 送給 OTA 目標的單播必須全部經由 `otaTxToTarget()`（CR1）：
                `espNowSendToEx(otaTargetMac` 在非註釋行剛好一次，且
                `groupNoteUnicastAck()` 裡的 `otaUnicastRecently()` 守衛排在
                寫入 `groupDelivered[]` 之前

 16 終局階段  —— 三個終局階段不得被合併回一個 `"success"`（Task 5 的第一責任）：
                名單**從 `otaPhaseIsFinal()` 解析**，每個都要在 `otaPhaseName()`
                與「停留 30 秒再回 idle」的 case 群組裡；兩個收尾函式各剛好一個
                呼叫點；`otaFinishStagedOnly()` 必須長在 `if (otaStageOnly) {` 裡，
                而那個分支不得出現 `otaFinish()`
 17 slave 正面證據 —— `otaSlaveVerified = true;` 剛好一處，且必須排在
                「HO_OTA_OK ＋ base ＋ mask 三項齊備」之後（"verifying" 與
                "unconfirmed" 唯一的分野，**一字不改只搬位置**就能造假）
 18 代發 updating —— `publishSlaveStatus()` 的 `isOtaTarget` **連同 if/else 整段**
                逐字釘住（中間一行都不准插）；`isOtaTarget` 全檔只准賦值一次；
                函式內 `doc["status"]` 剛好 2 處、`"updating"` 剛好 1 處；
                列舉 `OTA_BEGIN_SENT..OTA_VERIFYING` 在**文字順序**與**實際數值**
                兩個維度上都剛好是那五個轉送中階段
                （**這一條被繞過四次**：只釘前半行／加一行覆蓋／重排列舉文字／
                 **只改列舉的顯式數值而文字一個字都不動**）
 19 ota 上界前提 —— `STATUS_OTA_MAX_BYTES = 128` 那四條前提要真的有人守：
                `otaSetTarget()` 整個函式體逐字（字元白名單就是前提 3 的唯一保證）、
                `otaTargetId` 的寫入點必須全部在該函式或 `fakeOtaForCapacityTest()` 內、
                `otaTargetId[20]`／`otaErrCode[20]` 兩個宣告逐字、
                `otaPhaseName()` 的字串 ≤ 12、錯誤碼字面 ≤ 16。
                ⚠ 寫入點是**函式名白名單**，沒被列進那串名字的寫法一個都驗不到
                （`strlcpy` 就這樣繞過去過一次）

**這支腳本擋不住什麼**（必須連著讀）：
  - 它只做**字串／算式比對**。字串對不代表語義對
    —— A 族第 7 次就是「行號對、字串對，但列舉值抄錯」。
  - 它只檢查 HIT/BANNED 兩張表**列出來的**項目。
    沒被列進表裡的判準，這支腳本一個字都不會驗。
    **加了新判準就必須同時加進表裡**，否則覆蓋率會靜靜地退化。
  - 方向 3 只擋 `檔名.ino:123` 與 `` `:123` `` 兩種寫法；
    用「第 3218 行」之類的中文敘述寫行號一樣會漂移，抓不到。
  - 方向 5 只認 `(A-1-B-C)/D = …` 這個形狀。把同一件事寫成散文
    （「扣掉基礎欄位還能放二十六台」）抓不到。
  - 它驗不到時序、順序、以及任何跨板行為。

**維護規則：改完必須跑突變驗證。**
每個方向故意打壞一次，確認腳本真的會叫，再還原。
沒跑過突變的規則等於「宣稱一道沒看過它啟動的防線」——
本專案 A 族病灶的標準形狀。實際案例：第三輪突變驗證才發現
BANNED 只禁了「mqtt buffer 3328」這種散文寫法，
**沒禁「| `MQTT_BUFFER_SIZE` | 3328 |」這種常數表格列** ——
那個缺口讀十遍腳本也看不出來，是打壞它才浮現的。
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 換行字元。本檔多處要用**多行字面**當錨點（方向 14／17／18／19），
# 而在 Python 原始碼裡直接寫跳脫序列會讓那些錨點自己變得難以逐字核對，
# 所以一律用這個名字接起來（既有程式碼裡的 chr(10) 是同一件事）。
NL = chr(10)

# ── 整段字面錨點的失敗訊息共用尾巴（M-4，複審第 3 輪）──
#
# 為什麼一定要有這一段：`ho_master1.ino` 裡有三處「整段逐字」錨點
#（isOtaTarget 那八行、otaSetTarget() 整個函式體、ota 物件那六行）。
# 它們**對合法重構同樣會變紅** —— 那是刻意的摩擦（中斷器），但第 2 輪的報告
# 宣稱「每個錨點的失敗訊息裡都寫了『要回來確認什麼』」，而**三則訊息當時都只講
# 「改壞了會有什麼後果」，一則都沒有告訴讀者合法重構時該確認什麼**。
# 那段話只存在於錨點上方的註釋裡，而**看到失敗訊息的人不一定會去讀註釋**。
# 「摩擦的唯一緩解手段被寫在報告裡當成已經做了」正是 A 族的形狀，所以真的補上。
RECONFIRM = '。**這是中斷器，不是「你改壞了」的判決**：如果你是在做合法重構，請先確認三件事再更新這裡的錨點 ——（1）新寫法在**所有**階段值下的結果與舊寫法逐一相同；（2）沒有任何一條新路徑能在「零 slave 接觸」或「已離線」時說出更樂觀的話；（3）把新寫法本身做一次加寬突變、確認這支腳本會叫。三件確認完再改錨點，否則你只是把守衛拆了'

RECONFIRM_19A = '。**這是中斷器，不是「你改壞了」的判決**：如果你是在做合法重構，請先確認三件事再更新這裡的錨點 ——（1）新的字元集**不含任何需要 JSON 逃逸的字元**（雙引號、反斜線、控制字元）；（2）重算 `"target":"…",` 那一項的最壞 bytes（現行是 31 ＝ 9 ＋ 引號含 19 字元 ＋ 逗號），並回頭確認 ota 物件總和仍 ≤ 128；（3）把新寫法做一次加寬突變、確認這支腳本會叫。三件確認完再改錨點'

RECONFIRM_19E = '。**這是中斷器，不是「你改壞了」的判決**：如果你是在做合法重構或真的要加欄位，請先確認三件事再更新這裡的錨點 ——（1）逐項重算新的 ota 物件最壞 bytes；（2）把 STATUS_OTA_MAX_BYTES 一起改大，並重跑方向 5 的 static_assert 算式複算；（3）確認新欄位的來源字串有長度與字元集的上界（遠端來的一律沒有）。三件確認完再改錨點'

SRC_FILES = [
    'ho_master1/ho_master1.ino',
    'ho_slave1/ho_slave1.ino',
    'ho_espnow_test/ho_espnow_test.ino',
    'libraries/HoEspNow/src/HoEspNowProtocol.h',
    'libraries/HoEspNow/src/HoEspNowProtocol.cpp',
]
DOC_FILES = [
    'docs/phase1-regression-checklist.md',
    'docs/phase2b-regression-checklist.md',
    'docs/phase4-flag-day-upgrade.md',
    'ho_master1/readme.md',
    'ho_slave1/readme.md',
]


def read(rel):
    with io.open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def flatten_c_strings(text):
    """把 C 的跨行字串接續（"…"\n  "…"）攤平，才能整段比對序列埠訊息。"""
    return re.sub(r'"\s*\n\s*"', '', text)


# ── 方向 1（HIT）：文件引用的判準字串必須逐字出現在原始碼裡 ──
# 格式：(說明, 必須出現在原始碼的字串)
HIT_IN_SOURCE = [
    ('收工：韌體層已執行',   '[群組] 韌體層已執行 %d／%d 台（slave 回報 cmdId=%u）'),
    ('收工：無執行證明',     '⚠ [群組]   無執行證明：%s'),
    ('收工說明 1',           '[群組] 注意：MAC 層 ACK 只證明「封包已送達」，不能證明繼電器真的動作'),
    ('收工說明 2',           '[群組] 執行證明只到韌體層：證明 slave 走完了繼電器動作那段程式，不證明繼電器硬體動作，更不證明籠門關上'),
    ('收工說明 3',           '[群組] 沒有執行證明不等於沒執行 —— 回報可能還在路上；它只能維持紅色，不能宣稱已確認未執行'),
    ('歸因行',               '[歸因] %s 回報已執行 cmdId=%u 種類=%u 次數=%u'),
    ('協定版本告警',         '⚠ [協定] 收到版本 %u 的封包，本機是版本 %u，全部丟棄；master 與所有 slave 必須一起重燒'),
    ('slave 安全關閉',       '[安全] 失去 master，繼電器已關閉'),
    ('slave 失聯',           '[失聯] 超過 30 秒沒收到心跳'),
    ('群組收工首行',         '[群組] 指令 %u 收工%s：單播 MAC 層已送達 %d／%d 台'),
    ('關門路徑警語',         '⚠ [群組] 這是關門路徑：未送達的籠門必然沒關；已送達的也只代表封包到了，一律以現場確認為準，不要當成已關閉'),
    ('exec 值',              'g["exec"] = "attributed";'),
    ('exed 欄位',            'g["exed"] = groupCountExecuted();'),
    ('exe 欄位',             'o["exe"] = exe;'),
    ('SLAVE_ENTRY 常數',     'const size_t SLAVE_ENTRY_MAX_BYTES = 112;'),
    ('STATUS_BUF 常數',      'const size_t STATUS_BUF_SIZE = 3584;'),
    ('MQTT_BUFFER 常數',     'const size_t MQTT_BUFFER_SIZE = 3840;'),
    ('BASE 分項 a',          'const size_t STATUS_BASE_WITHOUT_GROUP_OTA_MAX_BYTES = 480;'),
    ('BASE 分項 b',          'const size_t STATUS_GROUP_MAX_BYTES = 120;'),
    ('BASE 分項 c',          'const size_t STATUS_OTA_MAX_BYTES = 128;'),
    ('點動',                 '[繼電器] 點動 %u ms'),
    # ── Phase 4 Task 3（master 端暫存下載）──
    ('otadl 指令',           '} else if (verb == "otadl") {'),
    ('otastat 指令',         '} else if (verb == "otastat") {'),
    ('OTA 只收 https',       "[OTA] 網址不可用：只接受 https:// 開頭、host 非空且不含 '@' 的網址"),
    ('OTA 走 IP 連線（C3）',  'otaTls->connect(otaHostIp, otaPort, otaHost, nullptr, nullptr, nullptr);'),
    ('OTA 握手逾時（C3）',    'otaTls->setHandshakeTimeout(6);'),
    ('OTA 暫存取閒置分區',    'esp_ota_get_next_update_partition(NULL)'),
    ('OTA 階段名 downloading', 'return "downloading";'),
    # C2 修正後的收尾訊息。**刻意不再拿「已更新到」當錨點** ——
    # 那句話在 Task 3 是假綠燈（見方向 11）。
    ('OTA 暫存完成訊息',     '「未轉送、未接觸任何 slave」，目標 %s 的韌體版本沒有任何改變'),
    ('OTA 工作階段結束訊息',  '[OTA] 工作階段 %u 結束（目標 %s，階段轉為 success）'),
    ('OTA 自己跟隨重定向',    'otaHttp->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);'),
    # ── Phase 4 Task 4（master 端轉送引擎）──
    # 這幾條是 Task 6 的回歸清單會拿來當判準的序列埠字串，先在這裡釘住，
    # 免得日後改措辭時清單靜靜過期（B 族的標準形狀）。
    ('otarelay 指令',        '} else if (verb == "otarelay") {'),
    ('轉送：送出 BEGIN',      '[OTA] 已送出 OTA_BEGIN 給 %s（第 %d／%d 次），等待回應'),
    ('轉送：slave 就緒',      '[OTA] slave 已就緒，開始轉送'),
    ('轉送：進度',            '[OTA] 轉送進度 %u%%（%u/%u 包）'),
    ('轉送：送出 END',        '[OTA] 全部區塊已送達，已送出 OTA_END，等待校驗結果'),
    ('轉送：校驗通過',        '[OTA] slave 校驗通過，正在重新啟動，等它回線確認版本'),
    ('轉送：版本回檢成功',    '[OTA] 完成：%s 已更新到 %u.%u.%u'),
    ('轉送：回線逾時',        '[OTA] 等待 %s 回線 90 秒逾時，它可能沒有開起來'),
    # 這一條同時是「END_SENT 逾時不得改回 otaFail()」的中斷器：改回去的話
    # 這句話會被刪掉，HIT 當場變紅（理由見該處的長註釋，兩種單封丟包的假紅燈）。
    ('轉送：校驗結果逾時改判版本', '[OTA] 10 秒沒收到校驗結果（OTA_END 或它的回覆掉了一封），改用版本回檢判定：等它重開機回報版本'),
    # CR1 之後所有送給目標的單播都走 otaTxToTarget()（方向 15 驗那個「所有」）
    ('轉送：資料包走統一送出點', 'return otaTxToTarget(HO_PKT_OTA_DATA, pkt, sizeof(dh) + dataLen, false);'),
    ('轉送：統一送出點蓋時間戳', 'otaUnicastAt = millis();'),
    ('CR1：群組歸因守衛',      'if (otaUnicastRecently(desAddr)) {'),
    ('CR1：群組指令期間轉送讓開', '[OTA] 群組指令進行中，轉送讓開（安全指令優先於 OTA；計時器一併暫停）'),
    ('MJ3：心跳佇列滿要重試',   '⚠ [心跳] 送出佇列滿，這一則沒進佇列；不推進計時器，下一輪 loop() 立刻重試'),
    ('MJ4：送出失敗彙總',       '[OTA] 轉送期間單播送出失敗 %u 次（累計，每 5 秒彙總一行）'),
    # ── 下面三條 ota_relay_sim.py **沒有模型**（它不模擬心跳、不模擬群組指令、
    #    也沒有 5 分鐘總上限），所以只能由這裡的逐字錨點守。
    #    A 族第 20 次：這三項原本兩支腳本都沒守著，可以原樣改回去而不被發現。
    ('MJ3：心跳只在成功進佇列時才推進計時器', 'if (sendHeartbeat()) lastBeat = now;'),
    ('MJ6：讓開期間三個計時器都要順延', 'otaSessionStart += paused;'),
    ('MJ7：OTA_VERIFYING 不受 5 分鐘總上限管',
     'if (!otaPhaseIsFinal() && otaPhase != OTA_VERIFYING &&'),
    ('轉送：收包 log 排除 ACK', 'if (header.type != HO_PKT_OTA_ACK) {'),
    # ── Phase 4 Task 5（MQTT 入口、進度回報）──
    # 這一組全部圍繞同一件事：**App 拿到的每一個狀態字串都要對應真實發生的事**。
    ('Task 5：update_slave 指令', '} else if (message.startsWith("update_slave:")) {'),
    ('Task 5：update_slave 一律不是 stageOnly', 'otaStart(id, url, ver, md5, force, false);'),
    ('Task 5：fakeota 指令', '} else if (verb == "fakeota") {'),
    ('Task 5：ota 物件的 phase 走查表函式，不得寫死字串', 'ota["phase"]    = otaPhaseName();'),
    ('Task 5：OTA 期間發布週期縮短為 5 秒',
     'return (otaPhase != OTA_IDLE) ? 5000UL : 10000UL;'),
    ('Task 5：階段轉換立刻補發，進度變化不補發',
     'if (otaStatusDirty || now - lastStatusPub > masterStatusIntervalMs()) {'),
    ('Task 5：只暫存的收尾訊息（零 slave 接觸）',
     '[OTA] 工作階段 %u 結束（目標 %s，階段轉為 staged_only：只到 master 暫存區，未接觸任何 slave）'),
    ('Task 5：staged_only 階段名', 'case OTA_STAGED_OK:      return "staged_only";'),
    ('Task 5：沒有 slave 正面證據時報 unconfirmed',
     'case OTA_VERIFYING:      return otaSlaveVerified ? "verifying" : "unconfirmed";'),
]

# ── 方向 3：協定測試的項數必須與文件的判準一致 ──
# 文件寫死「執行 N 項」當判準，程式卻是執行期累加 —— 兩者靠人工同步過一次就會漂。
# B 族第 13 次：plan 的回歸清單第 1 項寫「應顯示 41 項全過」、:877 也寫 41，
# 而實際是 57（腳本自己數得出來）。方向 4 本來就是為抓這個而生的，
# **卻只掃 phase4-flag-day-upgrade.md、掃不到 plan** —— 覆蓋缺口，不是判準錯。
# 現在改成掃一整組文件。
EXPECTED_TEST_COUNT_DOCS = [
    'docs/phase4-flag-day-upgrade.md',
    'docs/superpowers/plans/2026-08-17-esp32-phase4-ota-relay.md',
]

# ── 方向 2（BANNED）：被取代的舊值不得再以「現行事實」的樣子出現在文件裡 ──
#
# 格式：(說明, 正規表示式[, 這一條專屬的例外樣式])
#
# 通用例外 HISTORY_MARK：同一行內明確標成歷史沿革（「之前」「原本」「舊」…）才放行。
# **例外必須寫得夠窄。** 一個太寬的例外會把整條規則變成裝飾品 ——
# 那就是「宣稱一道其實不存在的防線」，正是本專案 A 族病灶的形狀。
# 所以「舊值 → 新值」的遷移表格用**每條規則自己的**例外樣式，
# 而不是放寬 HISTORY_MARK（放寬會連「maxEntries 是 **21**」一起漏掉）。
HISTORY_MARK = r'(之前|原本|由 |舊|沿革|曾|當時|Task 7 時|Phase 2b 的)'

# BANNED 的掃描對象。**原始碼註釋也要掃** —— 容量預算的權威敘述正好長在
# ho_master1.ino 的註釋裡（review ② 就在那裡抓到 `2324`，實際應為 2332），
# 只掃 .md 等於漏掉最權威的那一份。
BANNED_SCAN_FILES = DOC_FILES + SRC_FILES

BANNED_IN_DOCS = [
    # 行號對照（review ①）：Phase 4 Task 1 一動 ho_master1.ino，phase2b 清單裡
    # 31 處可判定的行號就錯了 30 處。規則檔早就寫著「判準以字串為準，不以行號為準」，
    # 所以 117 處全部移除，並用這條規則擋住它們被加回來。
    # **它擋不住什麼**：只擋 `檔名.ino:123` 與 `` `:123` `` 這兩種寫法。
    # 用「第 3218 行」之類的中文敘述寫行號一樣會漂移，這條抓不到。
    # 例外限定成「同一行明講是被移除的／已經漂掉的」，不放寬成通用例外。
    ('行號對照（改用字串錨點）', r'`[A-Za-z0-9_]*\.ino:\d+|`:\d+', r'(移除|實際已是)'),
    ('舊除法 /104',            r'/\s*104\s*='),
    ('舊除法 2420',            r'2420\s*/'),
    ('中途值 maxEntries 21',   r'maxEntries[^\n]{0,20}(是|＝|=)\s*\*{0,2}21'),
    ('舊值 maxEntries 23',     r'maxEntries[^\n]{0,20}(是|＝|=)\s*\*{0,2}23'),
    ('舊 statusBuf 3072',      r'statusBuf(\[|\s+只有\s*|\s+)3072'),
    ('舊 mqtt buffer 3328',    r'mqtt buffer\s*(只有\s*)?3328'),
    ('舊 setBufferSize 3328',  r'setBufferSize\(3328\)'),
    ('已移除的 cid 欄位',       r'"cid"\s*:'),
    ('舊 exec 值當現行',        r'`?"exec"`?\s*(固定|是)\s*`?"unprovable"'),
    ('擋不住什麼寫成三項',      r'擋不住什麼」?三項'),
    ('舊警語「餘裕只剩 1 台」',  r'餘裕只剩 1 台'),
    # 遷移表格「| 常數 | 舊值 | **新值** |」是合法寫法，例外限定成
    # 「markdown 表格列，且同一列出現粗體的新值」——換行敘述句不適用。
    ('舊 SLAVE_ENTRY 104',     r'SLAVE_ENTRY_MAX_BYTES\D{0,4}104', r'^\|.*\*\*112\*\*'),
    ('舊 STATUS_BASE 640',     r'STATUS_BASE_MAX_BYTES\s*(=|＝|\|)\s*640', r'^\|.*\*\*728\*\*'),
    # 這兩條是突變驗證抓出來的缺口：原本只禁「mqtt buffer 3328」這種散文寫法，
    # 沒禁「| `MQTT_BUFFER_SIZE` | 3328 |」這種**常數表格列**，於是表格裡寫回舊值
    # 完全不會被抓到。缺口是靠「故意打壞→看腳本會不會叫」發現的，不是靠讀腳本。
    ('舊 MQTT_BUFFER 3328',    r'MQTT_BUFFER_SIZE\D{0,4}3328', r'^\|.*\*\*3840\*\*'),
    ('舊 STATUS_BUF 3072',     r'STATUS_BUF_SIZE\D{0,4}3072', r'^\|.*\*\*3584\*\*'),
]


# ── 方向 7（PLAN）：設計文件裡的假宣稱不得留著等下一個 Task 抄走 ──
#
# A 族第 14 次的形狀：`end(true)` 的假宣稱在原始碼註釋、report 與模擬三處都改掉了，
# **卻完整留在 plan 的「五道保障」第 2 條與 Task 2 的程式碼範本裡**，
# 而 plan Task 6 Step 2 明文要求 `ho_slave1/readme.md` 逐條抄錄那一節。
# 修掉的話正排隊等著被重新發表 —— 所以把 plan 也納入掃描，但**只掃這一條規則**
#（plan 有大量刻意保留的歷史敘述，整份套用既有規則會把歷史記錄一起判成錯）。
#
# 事實依據（esp32 core 3.3.7 `Updater.cpp`）：`end(true)` 的 evenIfRemaining 分支
# 會 `_size = progress();`，**放棄長度檢查**，之後只比對 setMD5() 設定的 MD5。
#
# **它擋不住什麼**：只認「end(true) … 長度」與「已寫入長度 == 宣告長度」兩種寫法。
# 換個措辭說同一件事（「它會先確認寫滿了才切換」）一樣抓不到；
# 也擋不住別的函式庫行為被憑印象寫進文件。
# ── 方向 10 的表：文件裡「宣告某道保護不存在」的句子，本身必須還在 ──
# 格式：(檔案, 說明, 必須逐字出現的字串)
DOC_MUST_CONTAIN = [
    ('ho_master1/readme.md', 'LR 互斥守衛尚未存在的告知',
     '「OTA 進行中拒絕切 LR」這道守衛現在並不存在'),
    ('ho_master1/readme.md', 'otadl 不代表 slave 被更新過',
     '兩行都不會出現「已更新到 x.y.z」'),
    # MJ2：Task 3 最重要的約束原本只寫在 report 裡，readme／plan／腳本零命中。
    ('ho_master1/readme.md', 'Task 5 之前不得開 MQTT 入口',
     'Task 5 開 MQTT 入口之前，轉送 OTA 這條路徑不該對外'),
    ('docs/superpowers/plans/2026-08-17-esp32-phase4-ota-relay.md',
     'Task 4 版本回檢的三條硬性前提',
     '不得**沿用會過期的 `otaTargetIdx`'),
    # C6：這句話一旦被刪或改回去，Task 4 就會把輪詢範圍寫回含 VERIFYING。
    ('docs/superpowers/plans/2026-08-17-esp32-phase4-ota-relay.md',
     'VERIFYING 期間必須繼續輪詢（C6）',
     '`OTA_VERIFYING` 期間**必須繼續輪詢目標**'),
]

# ── 方向 12 的表：「必須剛好出現一次、而且逐字長這樣」的設定 ──
# 複審實測出兩個逃逸，兩個都不是 HIT 擋得住的形狀：
#   (1) 保留 setHandshakeTimeout(6)、**後面再加一行 setHandshakeTimeout(600)** → 全過
#   (2) **直接刪掉** setConnectionTimeout(6000) → 全過（_timeout 回到預設 30000，
#       TCP select 變 30 秒 → 單一窗口 ≥36 秒 → 籠門放開）
# HIT 只證明「某一行存在」，不證明「沒有第二行推翻它」，也不證明「該設定沒被刪」。
# 所以這一張表同時驗三件事：API 名稱出現次數 == 1、那一行逐字存在、且都在非註釋行。
#
# **它擋不住什麼**：只認列出來的這幾個 API 名稱與字面。改用別的 API 達到同樣效果
#（例如 connect(host, port, timeout) 那條多載自帶 timeout）它抓不到；
# 也不驗數值是否合理 —— 把 6 改成 60 仍然只有一次呼叫，**逐字比對才是擋住那個的原因**。
EXACT_ONCE_IN_MASTER = [
    ('TLS 握手逾時（C3）', 'setHandshakeTimeout', 'otaTls->setHandshakeTimeout(6);'),
    ('TCP connect 逾時（C3）', 'setConnectionTimeout', 'otaTls->setConnectionTimeout(6000);'),
    ('不跟隨重定向（C1）', 'setFollowRedirects',
     'otaHttp->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);'),
    ('走 IP 連線、帶 host 做 SNI（C3）', 'otaTls->connect(',
     'otaTls->connect(otaHostIp, otaPort, otaHost, nullptr, nullptr, nullptr);'),
    # C5：begin() 必須剛好一次，且在 OTA_DL_CONNECT 裡（順序由下面的方向 13 驗）
    ('HTTPClient::begin 只有一處（C5）', 'otaHttp->begin(', 'if (!otaHttp->begin(*otaTls, otaUrl)) {'),
    # C6：跳過範圍必須止於 OTA_END_SENT。寫成 OTA_VERIFYING 會讓 lastSeen 永遠不前進，
    # Task 4 的版本回檢永遠不成立 → 回歸清單第 12 項把正確行為判成 FAIL。
    ('輪詢跳過範圍止於 END_SENT（C6）', 'pollIdx == otaTargetIdx',
     'if (otaPhase >= OTA_BEGIN_SENT && otaPhase <= OTA_END_SENT && pollIdx == otaTargetIdx) {'),
]

PLAN_FILES = ['docs/superpowers/plans/2026-08-17-esp32-phase4-ota-relay.md']
BANNED_IN_PLAN = [
    ('end(true) 會檢查長度的假宣稱（A 族第 13／14 次）',
     r'end\(true\){0,1}.{0,80}(檢查|驗).{0,24}長度|已寫入長度\s*==\s*宣告長度',
     r'(不驗|不檢查|不會檢查|放棄|跳過)'),
]


def main():
    src = '\n'.join(read(f) for f in SRC_FILES)
    flat = flatten_c_strings(src)

    failures = []

    for name, needle in HIT_IN_SOURCE:
        if needle not in flat:
            failures.append('[HIT 缺] %s：原始碼裡找不到 %r' % (name, needle))

    for doc in BANNED_SCAN_FILES:
        text = read(doc)
        for lineno, line in enumerate(text.splitlines(), 1):
            for rule in BANNED_IN_DOCS:
                name, pattern = rule[0], rule[1]
                own_exception = rule[2] if len(rule) > 2 else None
                if not re.search(pattern, line):
                    continue
                if re.search(HISTORY_MARK, line):
                    continue
                if own_exception and re.search(own_exception, line.strip()):
                    continue
                failures.append('[BANNED] %s:%d %s → %s'
                                % (doc, lineno, name, line.strip()[:90]))

    for doc in PLAN_FILES:
        text = read(doc)
        for lineno, line in enumerate(text.splitlines(), 1):
            for name, pattern, own_exception in BANNED_IN_PLAN:
                if not re.search(pattern, line):
                    continue
                if re.search(own_exception, line):
                    continue
                failures.append('[PLAN] %s:%d %s → %s'
                                % (doc, lineno, name, line.strip()[:90]))

    # ── 正向數值交叉比對（review ②）──
    # BANNED 只禁「舊值」，所以兩種漂移逃得掉：
    #   (a) 文件寫了一個**錯的新值**（例如 maxEntries 寫成 26）
    #   (b) 換個措辭寫舊值（禁清單的正則沒涵蓋那個寫法）
    # 這裡改成**正向**比對：凡是文件／註釋裡出現這些名詞後面接數字，
    # 那個數字就必須等於從原始碼算出來的唯一正解。
    #
    # **它擋不住什麼**：只認下面這幾個具名樣式。用完全不同的措辭敘述同一個數字
    # （「二十五台」「約 3.5KB」）一樣抓不到；也驗不到數字背後的推導是否正確。
    def cross_check(expected_map):
        for path in BANNED_SCAN_FILES:
            text = read(path)
            for lineno, line in enumerate(text.splitlines(), 1):
                for label, (pattern, want) in expected_map.items():
                    for m in re.finditer(pattern, line):
                        got = int(m.group(1))
                        if got != want and not re.search(HISTORY_MARK, line):
                            failures.append('[數值] %s:%d %s 寫成 %d，正解 %d → %s'
                                            % (path, lineno, label, got, want,
                                               line.strip()[:80]))

    # ── 協定測試項數：數原始碼的 check() 呼叫，回頭比對文件寫死的數字 ──
    test_src = read('ho_espnow_test/ho_espnow_test.ino')
    body = test_src.split('void check(bool cond', 1)[1]
    n_checks = len(re.findall(r'\bcheck\(', body))
    # **它擋不住什麼**：只認下面這五種措辭。用別的句型寫同一個數字
    # （「總共四十一項」「41/41 通過」）一樣抓不到。
    doc_counts = set()
    for docpath in EXPECTED_TEST_COUNT_DOCS:
        doc = read(docpath)
        found = set(re.findall(r'執行 (\d+) 項，失敗 0 項', doc))
        found |= set(re.findall(r'項數\*\*不是 (\d+)\*\*', doc))
        found |= set(re.findall(r'這 (\d+) 項全部是', doc))
        found |= set(re.findall(r'應顯示 (\d+) 項全過', doc))
        found |= set(re.findall(r'協定測試由 \d+ 項增為 (\d+) 項', doc))
        doc_counts |= found
        bad = {c for c in found if int(c) != n_checks}
        if bad:
            failures.append('[項數] 原始碼有 %d 個 check()，但 %s 寫成 %s'
                            % (n_checks, docpath, sorted(bad)))
    print('協定測試項數：原始碼 %d、文件 %s（掃 %d 份）'
          % (n_checks, sorted(doc_counts), len(EXPECTED_TEST_COUNT_DOCS)))

    # ── 容量算式獨立複算 ──
    # **每一個數字都從原始碼解析出來**，不從文件抄、也不寫死在這支腳本裡。
    # 寫死等於再開一個「常數改了但驗證沒跟上」的缺口 —— 那正是本腳本要防的東西。
    def const(name):
        m = re.search(r'%s\s*=\s*(\d+)\s*;' % name, src)
        if not m:
            failures.append('[算式] 原始碼裡找不到常數 %s' % name)
            return None
        return int(m.group(1))

    buf = const('STATUS_BUF_SIZE')
    entry = const('SLAVE_ENTRY_MAX_BYTES')
    key_overhead = const('SLAVES_KEY_OVERHEAD')
    base_parts = [const('STATUS_BASE_WITHOUT_GROUP_OTA_MAX_BYTES'),
                  const('STATUS_GROUP_MAX_BYTES'),
                  const('STATUS_OTA_MAX_BYTES')]
    max_slaves = None
    m = re.search(r'#define HO_ESPNOW_MAX_SLAVES\s+(\d+)', src)
    if m:
        max_slaves = int(m.group(1))

    if None not in base_parts and None not in (buf, entry, key_overhead, max_slaves):
        base = sum(base_parts)
        max_entries = (buf - 1 - base - key_overhead) // entry
        ok = max_entries >= max_slaves
        if not ok:
            failures.append('[算式] maxEntries=%d < HO_ESPNOW_MAX_SLAVES=%d'
                            % (max_entries, max_slaves))
        print('容量複算：(%d-1-%d-%d)/%d = %d ≥ %d  %s'
              % (buf, base, key_overhead, entry, max_entries, max_slaves,
                 'OK' if ok else 'FAIL'))
        print('  STATUS_BASE 分項：%d + %d + %d = %d' % tuple(base_parts + [base]))

        # 「maxEntries 是 N」這類**單獨寫出來的**數字，必須等於算出來的 25。
        cross_check({
            'maxEntries（單獨寫出）':
                (r'`maxEntries`[^\d\n]{0,8}?\*{0,2}(\d+)', max_entries),
        })

        # ── 容量算式的結構化複算（review ② 的 (a)(b) 兩種漂移都涵蓋）──
        # 不比對「某個特定寫法」，而是把任何一條長成
        #   (A - 1 - B - C) / D = [E / D =] F
        # 的算式抓出來，逐項驗證：
        #   - B/C/D 必須等於原始碼的 STATUS_BASE／SLAVES_KEY_OVERHEAD／SLAVE_ENTRY
        #   - E（若有寫出分子）必須等於 A-1-B-C
        #   - F 必須等於 floor((A-1-B-C)/D)
        # A 允許 3584（現行）與 3072（「不放大會怎樣」的對照），其餘一律報錯。
        #
        # 這樣一來：**寫錯的新值**（26）與**換措辭寫的舊值**（2420/104）都會被抓到，
        # 而且完全不依賴作者用什麼中文措辭來包裝這條算式。
        #
        # **它擋不住什麼**：只認 `(A-1-B-C)/D = …` 這個形狀。
        # 把同一件事寫成散文（「扣掉基礎欄位之後還能放二十六台」）一樣抓不到。
        expr = re.compile(r'\((\d{3,4})\s*-\s*1\s*-\s*(\d+)\s*-\s*(\d+)\)\s*/\s*(\d+)'
                          r'\s*=\s*(?:(\d+)\s*/\s*\d+\s*=\s*)?\*{0,2}(\d+)')
        n_expr = 0
        for path in BANNED_SCAN_FILES:
            for lineno, line in enumerate(read(path).splitlines(), 1):
                for m in expr.finditer(line):
                    n_expr += 1
                    a, b, c, d = (int(m.group(i)) for i in (1, 2, 3, 4))
                    e = int(m.group(5)) if m.group(5) else None
                    f = int(m.group(6))
                    want_num = a - 1 - b - c
                    problems = []
                    if a not in (buf, 3072):
                        problems.append('分子起始 %d 不是 %d 或 3072' % (a, buf))
                    if b != base:
                        problems.append('STATUS_BASE 寫成 %d，應為 %d' % (b, base))
                    if c != key_overhead:
                        problems.append('SLAVES_KEY_OVERHEAD 寫成 %d，應為 %d'
                                        % (c, key_overhead))
                    if d != entry:
                        problems.append('SLAVE_ENTRY 寫成 %d，應為 %d' % (d, entry))
                    if e is not None and e != want_num:
                        problems.append('分子寫成 %d，實算 %d' % (e, want_num))
                    if f != want_num // d:
                        problems.append('商寫成 %d，實算 %d' % (f, want_num // d))
                    if problems:
                        failures.append('[算式] %s:%d %s → %s'
                                        % (path, lineno, m.group(0), '；'.join(problems)))
        print('容量算式結構化複算：%d 條' % n_expr)
    # ── 方向 8：master 的禁用 API 清單（兩條安全鐵則的機械化檢查）──
    # (1) 絕不切換自己的開機分區；(2) 絕不讓 HTTPClient 自己跟隨重定向。
    # ho_master1.ino 的註釋、ho_master1/readme.md 都寫著「master 不會變磚，
    # 因為全檔沒有一行呼叫 esp_ota_set_boot_partition()，也沒有 include Update.h」。
    # 那是一句**可被機械驗證**的宣稱，所以就驗它 —— 否則它只是一句話。
    #
    # **它擋不住什麼**：
    #   - 只掃 ho_master1.ino 的**非註釋行**，而且只認清單裡那幾個字面樣式。
    #     透過函式指標、巨集別名或 esp_ota_ops.h 的其他 API（例如
    #     esp_ota_begin/esp_ota_end 這組也會動 otadata）繞過去，它一個都抓不到。
    #   - 它是靜態檢查，不證明執行期真的沒切換分區。
    #   - 重定向那兩條只擋「函式庫自動跟隨」被加回來，**不保證**我們自己那套
    #     跟隨邏輯是對的（次數、Location 驗證、回 OTA_RESOLVING 都驗不到）。
    master_src = read('ho_master1/ho_master1.ino')
    code_lines = [(i, ln) for i, ln in enumerate(master_src.splitlines(), 1)
                  if not ln.lstrip().startswith('//')]
    # setRedirectLimit / STRICT：C1 的錨點原本只驗「DISABLE 那一行還在」，
    # 而**保留它、後面再加一組 STRICT + setRedirectLimit(3)** 會整份通過 ——
    # 複審實測過。HIT 只能證明「某行存在」，證明不了「沒有別的行把它推翻」。
    # 所以把「不得出現」這一側也寫成規則。
    for pat in ('esp_ota_set_boot_partition', '#include <Update.h>', 'Update.begin(',
                'setRedirectLimit', 'HTTPC_STRICT_FOLLOW_REDIRECTS',
                'HTTPC_FORCE_FOLLOW_REDIRECTS'):
        for lineno, line in code_lines:
            if pat in line:
                failures.append('[禁用 API] ho_master1/ho_master1.ino:%d 出現 %s → %s'
                                % (lineno, pat, line.strip()[:80]))

    # ── 方向 9：otaSessionBusy() 的呼叫點數量（Phase 5 技術債的防腐）──
    # ho_master1/readme.md 白紙黑字寫著「目前的呼叫端只有兩處，而且兩處都是
    # 『同時只能有一個工作階段』的重入檢查，所以『OTA 進行中拒絕切 LR』這道守衛
    # 現在並不存在」。那句話會在 Phase 5 有人補上守衛的當天變成假的 —— 而假的方向
    # 剛好是「文件低估了保護」→ 下一個人以為還沒做、又做一次；或反過來被當成已經做了。
    # 所以把「呼叫點數量」釘住：**定義 1 處 + otaStart() 1 處 + otaRelayStaged() 1 處 = 3**。
    # （B 族第 15 次：Task 4 把呼叫點加到 3 之後，這一叢敘述有三處還寫著「= 2」，
    #  就長在 `if (n_busy != 3):` 的正上方 —— 而上一輪的 MJ5 修的正是這一叢。
    #  判準改了就要回頭掃自己的散文，數字型的敘述尤其容易漏。）
    #
    # **它擋不住什麼**：它只數字面出現次數。有人加了呼叫端**同時**改掉這個
    #   期望值卻不改 readme，它就會靜靜通過；它也不檢查那個呼叫端是不是
    #   真的擋住了 LR 切換（那要看語義，字串比對驗不到）。
    #
    # **這種「把不存在翻成可數量」的檢查有四項副作用，要跟著推廣一起講**：
    #   1. **正當重構會誤觸**（把重入檢查抽成一個函式、加一行 assert 都會 +1），
    #      久了會養成「看到就把期望值 +1」的反射 —— 那等於這道檢查失效。
    #   2. **它是中斷器，不是守衛**：它只保證「有人動了就會停下來看一眼」，
    #      不保證動的方向是對的。
    #   3. **只適用小而穩定的量**。呼叫點會自然成長的東西（例如 espNowDelay()）
    #      套上去只會製造噪音。
    #   4. **它會誘使人為了讓數字好看而不重構**。發現這種壓力時，
    #      正確的反應是改期望值＋改文件，不是放棄重構。
    n_busy = len([1 for _, line in code_lines if 'otaSessionBusy(' in line])
    if n_busy != 6:
        failures.append('[LR 掛鉤] otaSessionBusy() 的非註釋出現次數是 %d（期望 6：'
                        '定義 1 處 + otaStart() 1 處 + otaRelayStaged() 1 處 + '
                        'update_slave 2 處（拒絕重入、格式錯誤不得殺掉進行中的工作階段）'
                        ' + updateStatusLed() 1 處）。'
                        '若是 Phase 5 補上了 LR 互斥守衛，請同時更新 '
                        'ho_master1/readme.md 那段「守衛現在並不存在」的敘述'
                        '與本檢查的期望值' % n_busy)
    # ── 方向 10：文件裡「宣告某道保護不存在」的句子本身必須還在 ──
    # M3：方向 9 只釘住了不變量的一半。它保證「有人加了呼叫端會被抓到」，
    # **但沒有保護 readme 那句「守衛現在並不存在」本身** —— 有人把那句話刪掉、
    # 呼叫點數量不變，腳本一聲不吭，於是「這道保護不存在」的告知就靜靜消失了。
    # 兩半要成對：方向 9 顧程式碼側，方向 10 顧文件側。
    #
    # **它擋不住什麼**：只比對整段字串在不在。有人改寫成語氣相反的句子
    #（「已由 otaSessionBusy() 擋住」）而剛好不含這段字，它一樣抓不到；
    # 它也不檢查那句話講的是不是事實。
    for path, label, needle in DOC_MUST_CONTAIN:
        if needle not in read(path):
            failures.append('[文件必含] %s 少了「%s」的敘述：%r'
                            % (path, label, needle))

    # ── 方向 11：otaFinish() 不得宣稱 slave 已更新（C2／B 族第 10 次）──
    # otaFinish() 是「工作階段結束」的共用收尾，Task 3 的唯一呼叫點是 OTA_STAGED，
    # 那時一台 slave 都沒接觸過。「已更新到 x.y.z」只能由 Task 4 的 OTA_VERIFYING
    # 版本回檢路徑印（前提是 slave 自己回報了新版本）。
    #
    # **它擋不住什麼**：只看 otaFinish() 函式本體的**非註釋行**、只認「已更新到」四個字。
    # 註釋要排除的理由與方向 8 相同：解釋「為什麼這裡不能印那句話」的註釋本身
    # 就得寫出這四個字，連它一起判違規的話，規則會逼人刪掉自己的說明。
    # 換句話說同一件事（「升級成功」「版本已生效」）它抓不到；
    # 也不管別的函式怎麼寫 —— Task 4 在 OTA_VERIFYING 裡印這句話是**正當的**，
    # 這條規則刻意放行那裡。
    body = master_src.split('void otaFinish()', 1)
    if len(body) < 2:
        failures.append('[假綠燈] 找不到 otaFinish() 的定義，方向 11 無法檢查')
    else:
        fn = '\n'.join(ln for ln in body[1].split('\n}', 1)[0].split('\n')
                       if not ln.lstrip().startswith('//'))
        if '已更新到' in fn:
            failures.append('[假綠燈] otaFinish() 內出現「已更新到」：'
                            '它的呼叫點包含 OTA_STAGED（下載＋暫存＋MD5，零 slave 接觸），'
                            '這句話會讓回歸清單第 12 項的判準在第 3 項就成立')

    # ── 方向 12：關鍵設定必須「剛好一次、逐字如此」──
    for label, api, exact in EXACT_ONCE_IN_MASTER:
        n = len([1 for _, line in code_lines if api in line])
        if n != 1:
            failures.append('[設定唯一] %s：非註釋行裡 %r 出現 %d 次（必須剛好 1 次）。'
                            '多一次＝後面那行會覆蓋前面那行；零次＝設定被刪掉、'
                            '回到函式庫預設值' % (label, api, n))
        if not any(exact in line for _, line in code_lines):
            failures.append('[設定唯一] %s：找不到逐字的 %r' % (label, exact))

    # ── 方向 13：**一組**順序不變量 —— 全部必須排在 otaTls->connect() 之前 ──
    #
    # X1（A 族第 18 次）：方向 12 只驗「剛好一次、逐字如此」，
    # 所以 `setConnectionTimeout(6000)` **一字不改、只搬到 connect() 之後**
    # → 13 個方向全過，而執行期 `_timeout` 還是建構子的 30000
    # → TCP select 30 秒 ＋ 握手 6 秒 ＝ **36 秒單一阻塞呼叫 → 籠門放開**。
    # 那正是 M20 想擋的後果，改用「搬位置」達成。
    # **同一個 commit 才剛因為 C5 學到「順序沒有任何編譯期／執行期訊號」，
    # 卻沒把那個維度套到隔壁的設定上** —— 這次一起釘。
    #
    # 三條不變量（都必須在 `otaTls->connect(...)` 那一行之前）：
    #   1. begin()               —— 之後才呼叫會被 beginInternal 的 disconnect(true) 拆線
    #                               （HTTPClient.cpp:283，_host 是空字串 → 條件成立）
    #   2. setConnectionTimeout  —— 之後才設，TCP select 用的是預設 30000
    #   3. setHandshakeTimeout   —— 之後才設，握手用的是預設 120000
    #
    # **它擋不住什麼**（三項，逐項寫明）：
    #   - **它比的是「文字在檔案裡的先後」，不是控制流。** 把設定包進一個更早出現、
    #     但執行時走不到的分支，它照樣過（本腳本自己的 X2 逃逸就是這個形狀）。
    #   - 只認下面這幾個字面錨點。改用別的多載或把設定搬進 helper 呼叫，它抓不到。
    #   - **比對範圍限定在 `case OTA_DL_CONNECT:` 這個區塊內**（見下方），
    #     所以「helper 的定義寫在 updateOtaSession() 後面」這種**執行順序正確的重構
    #     不會再誤觸**（那是上一版全檔比對的缺陷）；但把**呼叫本身**搬出區塊仍會叫 ——
    #     那時它是**中斷器**：請人回來確認新寫法仍然滿足三條不變量，再更新這張表。
    src_m = master_src
    blk_start = src_m.find('case OTA_DL_CONNECT: {')
    blk_end = src_m.find('case OTA_DL_REQUEST: {')
    if blk_start < 0 or blk_end < 0 or blk_end < blk_start:
        failures.append('[順序] 找不到 OTA_DL_CONNECT 區塊，方向 13 無法檢查')
    else:
        blk = src_m[blk_start:blk_end]
        i_conn = blk.find('int cres = otaTls->connect(otaHostIp')
        ORDER_BEFORE_CONNECT = [
            ('HTTPClient::begin（C5）', 'if (!otaHttp->begin(*otaTls, otaUrl)) {',
             'beginInternal 會因為 _host 不同而 disconnect(true) 把剛握好的 TLS 連線拆掉，'
             'GET() 會自己重連並重新做一次 DNS'),
            ('setConnectionTimeout（X1）', 'otaTls->setConnectionTimeout(6000);',
             'TCP connect 的 select 會用建構子預設的 30000 → 單一阻塞 30+6=36 秒 → 籠門放開'),
            ('setHandshakeTimeout（X1）', 'otaTls->setHandshakeTimeout(6);',
             'TLS 握手會用建構子預設的 120000 → 單一阻塞 120 秒 → 籠門放開'),
        ]
        if i_conn < 0:
            failures.append('[順序] OTA_DL_CONNECT 區塊裡找不到 otaTls->connect() 錨點')
        else:
            for label, needle, why in ORDER_BEFORE_CONNECT:
                i = blk.find(needle)
                if i < 0:
                    failures.append('[順序] OTA_DL_CONNECT 區塊裡找不到 %s 的錨點 %r'
                                    % (label, needle))
                elif i > i_conn:
                    failures.append('[順序] %s 排在 otaTls->connect() **之後**：%s'
                                    % (label, why))

    # ── 方向 14：轉送半段的兩個假燈號出口（Task 4）──
    #
    # 方向 11 只釘住 otaFinish() 不得宣稱「已更新到」，那是**下載半段**的假綠燈。
    # 轉送半段還有兩個獨立的出口，兩個都不是 HIT 擋得住的形狀：
    #
    #   (甲) `OTA_VERIFYING` 的版本回檢。三條硬性前提缺任何一條都會製造假綠燈：
    #        (a) otaHasVersion —— 目標版本是 0.0.0 時，會對上「從沒回報過狀態」
    #            的 slave 的預設 fw*=0，**第一輪就成立**而 slave 還在重開機
    #        (b) findSlave(otaTargetMac) 重查 —— otaTargetIdx 的宣告處逐字寫著
    #            「會過期」，用它等於拿**別台**的版本宣告成功（開錯門的 MAC 版）
    #        (c) lastSeen 必須晚於進入 VERIFYING 的時刻 —— 否則用的是工作階段
    #            開始前的舊值
    #   (乙) `OTA_END_SENT` 收到 ACK 之後的分流。缺第一道是**假紅燈**
    #        （晚到的查詢回覆帶 READY，而 slave 其實已校驗通過正在重開機）；
    #        缺第二道是**假綠燈**（只看 status 的話，查詢回覆與「校驗通過」
    #        在 slave 端曾經逐位元組相同 —— 已在產生端消滅，這裡是第二層）。
    #
    # **它擋不住什麼**（逐項）：
    #   - 只認下面這幾個逐字錨點。把同一個判斷改寫成別的等價寫法
    #     （例如把三條前提抽進一個 helper 函式）它會叫，而那時它是**中斷器**：
    #     請人回來確認新寫法仍然滿足三條前提，再更新這張表。
    #   - 它比對的是**文字**，不是控制流。三條前提若被寫成 `||` 而不是 `&&`，
    #     逐字錨點仍然命中（錨點含 `&&`，所以這一種其實會被抓到，但
    #     「把整段包進一個永遠為真的 if」這類結構性繞過抓不到）。
    #   - 它不驗執行期：`lastSeen` 是不是真的由重開機後的回報寫進去的，
    #     字串比對一個字都證明不了。
    #   - **它擋不住「重送同一版」**：slave 本來就是目標版本時，(a)(b)(c) 全部
    #     成立而韌體其實沒換過。要分辨得比對重開機事件本身（uptime／開機計數），
    #     本階段的 HoStatePayload 沒有那個欄位 —— 這是已知缺口，不是本檢查的漏網。
    #   - **位置維度只驗「文字先後」，不驗控制流**（與方向 13 同一個限制）：
    #     把成功出口包進一個更晚出現、但執行時先走到的分支，它照樣過。
    #   - **`||` 的禁令只涵蓋 OTA_VERIFYING 區塊**。同樣的加寬手法用在
    #     OTA_END_SENT（例如把成功判定寫成「OK 或 base 對」）它抓不到；
    #     那一段目前靠的是兩條逐字錨點，**覆蓋比 VERIFYING 弱，照實記在這裡**。
    #   - 它不驗數值：把 90000 改成 1、把三段版本比成別的變數，只要字面在、
    #     順序對、沒有 `||`，它一個字都不會說。
    # 區塊要在 **updateOtaSession() 的函式本體內**找：otaPhaseName() 也有一個
    # 逐項列出同一組列舉的 switch，`case OTA_SUCCESS:` 在它裡面**更早**出現，
    # 直接全檔 find() 會切出一段負長度而誤報「找不到」。
    fn_i = master_src.find('void updateOtaSession(unsigned long now) {')
    fn_j = master_src.find(chr(10) + '}' + chr(10), fn_i) if fn_i >= 0 else -1
    ota_fn = master_src[fn_i:fn_j] if (fn_i >= 0 and fn_j > fn_i) else ''
    if not ota_fn:
        failures.append('[轉送燈號] 找不到 updateOtaSession() 的函式本體，方向 14 無法檢查')

    def block_of(start_anchor, end_anchor, label):
        i = ota_fn.find(start_anchor)
        j = ota_fn.find(end_anchor)
        if i < 0 or j < 0 or j < i:
            failures.append('[轉送燈號] updateOtaSession() 裡找不到 %s 區塊'
                            '（錨點 %r → %r），方向 14 無法檢查'
                            % (label, start_anchor, end_anchor))
            return None
        return ota_fn[i:j]

    # ── A 族第 19 次的補課：**位置維度** ──
    # 方向 13 存在的唯一理由就寫著 A-18「一字不改、只搬位置」，
    # 而方向 14 的第一版在同一支腳本往下 60 行**又只驗存在性**。
    # 複審實測出兩個存活的加寬突變，兩個都不是「刪掉某一行」：
    #   W1：四個逐字錨點一字不改、「已更新到」仍剛好一次，
    #       **只把成功出口搬到守衛之前** → 只要目標還在名冊上就宣告已更新
    #   W2'：三條前提原封不動，把版本相等換成
    #       `verOk = (三段相符) || (now - otaPhaseStart >= 60000)`
    #       → **等 60 秒就綠、完全不看版本**
    # 所以這一版加三件事：(甲) 錨點必須**按順序**出現；
    # (乙) **版本三段比對本身**列進必含清單（前一版沒釘它，
    #      而那正是「版本回檢」四個字的全部內容）；
    # (丙) **OTA_VERIFYING 區塊的非註釋行不得出現 `||`** ——
    #      那一段的判定按設計是純合取，任何析取都等於多開一條綠燈路徑。
    n_upgraded = len([1 for _, line in code_lines if '已更新到' in line])
    if n_upgraded != 1:
        failures.append('[轉送燈號] 「已更新到」在 ho_master1.ino 的非註釋行出現 %d 次'
                        '（必須剛好 1 次，且只能長在 OTA_VERIFYING 的版本回檢裡）。'
                        '0 次＝Task 4 的版本回檢還沒寫、或成功路徑不再印出回歸清單第 12 項的判準字串；'
                        '2 次以上＝有第二條路徑在宣稱 slave 已更新' % n_upgraded)

    verifying = block_of('case OTA_VERIFYING: {', 'case OTA_SUCCESS:', 'OTA_VERIFYING')
    if verifying is not None:
        # **順序與計數一律在「去掉註釋行」的版本上做**。註釋裡本來就會逐字引用
        # 這些判斷式（解釋「為什麼一定要有這一條」的說明必須寫得出那一行），
        # 拿含註釋的原文比先後，等於讓一句說明就能決定順序檢查的結果。
        verifying = chr(10).join(ln for ln in verifying.split(chr(10))
                                 if not ln.lstrip().startswith('//'))
        # **這張表同時是順序表**：由上而下就是要求的出現順序。
        # 守衛的每一段都必須排在「印出已更新到」與 otaFinish() 之前 —— 那正是 W1 的破口。
        VERIFY_ORDERED = [
            ('(b) 用 MAC 重查索引', 'int vIdx = findSlave(otaTargetMac);'),
            ('(a) 目標版本不是 0.0.0', 'if (otaHasVersion && vIdx >= 0 &&'),
            ('線上判定', 'slaves[vIdx].online &&'),
            ('(c) lastSeen 晚於進入 VERIFYING 的時刻',
             '(long)(slaves[vIdx].lastSeen - otaPhaseStart) > 0 &&'),
            ('版本比對 major', 'slaves[vIdx].fwMajor == otaVerMajor'),
            ('版本比對 minor', 'slaves[vIdx].fwMinor == otaVerMinor'),
            ('版本比對 patch', 'slaves[vIdx].fwPatch == otaVerPatch'),
            ('成功路徑印出回歸清單第 12 項的判準字串', '[OTA] 完成：%s 已更新到 %u.%u.%u'),
            ('成功收尾', 'otaFinish();'),
        ]
        prev_i, prev_label = -1, None
        for label, needle in VERIFY_ORDERED:
            i = verifying.find(needle)
            if i < 0:
                failures.append('[轉送燈號] OTA_VERIFYING 區塊少了 %s 的逐字錨點：%r'
                                % (label, needle))
                continue
            if i < prev_i:
                failures.append('[轉送燈號] OTA_VERIFYING 的 %s 排在 %s **之前**：'
                                '守衛的每一段都必須早於「已更新到」與 otaFinish()，'
                                '否則只要把成功出口搬到守衛前面就能繞過整組前提'
                                '（A 族第 18／19 次的形狀）' % (label, prev_label))
            prev_i, prev_label = i, label

        # otaFinish() 在本區塊只能有一個呼叫點（多一個＝多一條成功出口）
        n_fin = len([1 for ln in verifying.split(chr(10))
                     if not ln.lstrip().startswith('//') and 'otaFinish();' in ln])
        if n_fin != 1:
            failures.append('[轉送燈號] OTA_VERIFYING 區塊的 otaFinish() 呼叫點有 %d 個'
                            '（必須剛好 1 個）' % n_fin)

        for lineno, line in code_lines:
            if 'otaTargetIdx' in line and line in verifying:
                failures.append('[轉送燈號] OTA_VERIFYING 區塊用了會過期的 otaTargetIdx：'
                                'ho_master1.ino:%d %s' % (lineno, line.strip()[:80]))

        # (丙) 禁止析取與三元：版本回檢按設計是**純合取**，
        # 任何 `||` 或 `?:` 都是多開的綠燈路徑。
        # **這一條才是擋住 W2' 的主力** —— 逐字錨點只是剛好因為尾巴含 `) {` 而
        # 附帶抓到（那是意外覆蓋，不是設計；M10 當初成立也是同一個意外，照實記）。
        #
        # **範圍限定在「守衛的條件式」之內**，不是整個 case 區塊（複審實測）：
        # 整段禁的話，加一行純診斷 `if (vIdx < 0 || !online) Serial.println(...)`
        # 就會變紅 —— 而這支腳本自己在方向 9 才剛寫過「正當重構誤觸會養成
        # 把期望值 +1 的反射，那等於這道檢查失效」。誤觸不是零成本。
        #
        # **一起禁 `?:` 的理由**：三元版的 W2'
        #（`waited ? true : 三段相符`）**穿得過逐字錨點與 `||` 的禁令**，
        # 上一輪是靠 ota_relay_sim.py 的括號差異才被擋下來的 —— 那是運氣，不是規則。
        #
        # **它擋不住什麼**：只看條件式那幾行的字面。把析取搬到條件式**外面**
        #（先算一個 bool 再拿進來，例如 `bool ok = a || b;`）它抓不到 ——
        # 那一種要靠順序不變量與 ota_relay_sim.py 的情境。
        gi = verifying.find('if (otaHasVersion && vIdx >= 0 &&')
        gj = verifying.find('slaves[vIdx].fwPatch == otaVerPatch')
        if gi >= 0 and gj > gi:
            guard_expr = verifying[gi:gj]
            for bad, why in (('||', '析取等於多開一條綠燈路徑（例如「或已經等了 60 秒」）'),
                             ('?', '三元運算子同樣是多開一條路徑，而且穿得過 `||` 的禁令')):
                if bad in guard_expr:
                    failures.append('[轉送燈號] OTA_VERIFYING 的守衛條件式裡出現 `%s`：%s。'
                                    '版本回檢按設計是純合取。若真的需要，那是**中斷器**：'
                                    '請先確認新寫法沒有放寬任何一條硬性前提，再更新這條規則'
                                    % (bad, why))

    # ── N1：**程式碼自己指名的危險方向，原本沒有任何錨點守著** ──
    #
    # `case OTA_END_SENT:` 裡逐字寫著「拿一個偏早的時刻當 OTA_VERIFYING 的起點，
    # 會讓第 (c) 條前提被**重開機之前**就送達的那份狀態回報滿足 —— 那是往假綠燈的
    # 方向」。複審實測：把那兩行的 `millis()` 換成 `otaSessionStart`，
    # **兩支腳本全綠、前提 (c) 整條失效** —— 目標本來就是目標版本時，
    # 連重開機都不必就會印「已更新到」。
    #
    # 規則做成機械的：**全檔對 `otaPhaseStart` 的指派只能是 `millis()`**
    #（唯一的例外是讓開段的 `+= paused`，那是順延不是重設）。
    #
    # **它擋不住什麼**：只認直接指派的字面。先把一個偏早的時刻存進別的變數
    # 再 `otaPhaseStart = thatVar;` 會被抓到（RHS 不是 millis()），
    # 但 `otaPhaseStart = millis() - 60000;` 會被抓到、
    # 而 `unsigned long t = millis(); … otaPhaseStart = t;` 也會被抓到（RHS 不是字面的
    # millis()）—— 代價是這條規則**不允許任何合法的重構**，它是中斷器。
    for lineno, line in code_lines:
        if 'otaPhaseStart' not in line:
            continue
        m = re.search(r'otaPhaseStart\s*(\+?=)\s*([^;]+);', line)
        if not m:
            continue
        op, rhs = m.group(1), m.group(2).strip()
        if op == '+=':
            if rhs != 'paused':
                failures.append('[VERIFYING 起點] ho_master1.ino:%d otaPhaseStart += %s'
                                '（只允許讓開段的 `+= paused`）' % (lineno, rhs))
            continue
        if rhs not in ('millis()', '0'):
            failures.append('[VERIFYING 起點] ho_master1.ino:%d otaPhaseStart = %s —— '
                            '**只能指派 millis()**。拿一個偏早的時刻當起點，'
                            '會讓版本回檢的第 (c) 條前提被「重開機之前」送達的舊回報滿足，'
                            '那是假綠燈（程式碼在 OTA_END_SENT 裡就寫著這句話，'
                            '而在補上這條規則之前沒有任何錨點守著它）' % (lineno, rhs))

    # ── N2：名冊上的版本欄位只能由「slave 自己回報」與「addSlave() 的初值」寫 ──
    # 複審的 N2：在 VERIFYING 裡等 60 秒就把 slaves[vIdx].fw* 改寫成目標版本
    # → 版本比對必然相等 → 全綠。那不是「回檢」，是 master 自己填答案。
    #
    # **它擋不住什麼**：只數 `.fwMajor =` 這個字面的出現次數，是**中斷器不是守衛**——
    # 它只保證「有人動了寫入點就會停下來看一眼」，不保證新增的那個寫入點是錯的。
    # 用別的寫法（memcpy 整個 struct、透過指標）它一個都抓不到。
    # `=(?!=)` 是必要的：少了那個否定環視，版本**比對**那一行
    # （`slaves[vIdx].fwMajor == otaVerMajor &&`）也會被算成寫入點。
    fw_writes = [(n, l) for n, l in code_lines
                 if re.search(r'\.fwMajor\s*=(?!=)', l)]
    if len(fw_writes) != 4:
        failures.append('[版本來源] `.fwMajor =` 的非註釋寫入點有 %d 處（期望 4：'
                        'loadSlaves() 的初值、addSlave() 的初值、'
                        'onEspNowRecv() 的 HO_PKT_STATE 分支、'
                        'fakeSlavesForCapacityTest() 的假值）。'
                        '版本回檢比對的就是這個欄位 —— 多一個寫入點就要先問'
                        '「那份版本是不是 slave 自己回報的」：%s'
                        % (len(fw_writes), [n for n, _ in fw_writes]))

    end_sent = block_of('case OTA_END_SENT: {', 'case OTA_VERIFYING: {', 'OTA_END_SENT')
    if end_sent is not None:
        END_MUST = [
            ('收到 READY 要繼續等（少了它＝假紅燈）',
             'if (otaAckStatus == HO_OTA_READY) {'),
            ('成功判定連 blockBase 與 mask 一起檢查（少了它＝只看 status 的假綠燈）',
             'otaAckBase == otaTotalChunks && otaAckBits == 0xFFFF'),
        ]
        for label, needle in END_MUST:
            if needle not in end_sent:
                failures.append('[轉送燈號] OTA_END_SENT 區塊少了 %s 的逐字錨點：%r'
                                % (label, needle))

    # ── 方向 15：轉送送出點的結構不變量（CR1）──
    #
    # onEspNowSent() 跑在 WiFi task，**無條件**把每一則 MAC 層 ACK 交給
    # groupNoteUnicastAck() 當群組指令的送達證據。Task 4 是第一個「對單一 slave 的
    # MAC 持續送單播」的功能，所以前一輪排進佇列、尚未完成的 OTA_DATA 回呼會落在
    # 本輪剛開的歸因閂裡 → 假的 "grp":1，而且**補送迴圈整台跳過**那台
    #（依建構必然 relay==0、門正開著的那一台）。
    #
    # 修法的結構面是「所有送給目標的單播都經由 otaTxToTarget()，由它蓋時間戳」。
    # 這條規則驗的就是那個「所有」：
    #   (1) `espNowSendToEx(otaTargetMac` 在非註釋行**剛好一次**（就是 otaTxToTarget 裡那行）
    #   (2) groupNoteUnicastAck() 裡有 otaUnicastRecently() 守衛，
    #       且**排在寫入 groupDelivered[] 之前**
    #
    # **它擋不住什麼**：
    #   - 只認 `espNowSendToEx(otaTargetMac` 這個字面。有人先把 MAC 複製到別的變數
    #     再送（`espNowSendToEx(mac, …)`）它抓不到。
    #   - 它不驗時間戳蓋在送出**之前**（那是順序，且錨點都在同一行內）。
    #   - **它完全不驗併發**：兩個 task 之間沒有鎖，正確性靠「單一寫者 + volatile」
    #     的約定，**原理上無法用字串比對或黑箱測試證明**。
    n_direct = len([1 for _, line in code_lines if 'espNowSendToEx(otaTargetMac' in line])
    if n_direct != 1:
        failures.append('[轉送送出點] `espNowSendToEx(otaTargetMac` 在非註釋行出現 %d 次'
                        '（必須剛好 1 次，就是 otaTxToTarget() 裡那一行）。'
                        '多一次＝有一條送出點沒有蓋 otaUnicastAt 時間戳，'
                        '那一處的 MAC 層 ACK 會冒名頂替群組指令的送達證據，'
                        '而補送迴圈會整台跳過那台 slave（CR1）' % n_direct)
    gi = master_src.find('void groupNoteUnicastAck(')
    gj = master_src.find('void groupSendUnicast(')
    if gi < 0 or gj < 0 or gj < gi:
        failures.append('[轉送送出點] 找不到 groupNoteUnicastAck() 的函式範圍，方向 15 無法檢查')
    else:
        gblk = master_src[gi:gj]
        i_guard = gblk.find('if (otaUnicastRecently(desAddr)) {')
        i_write = gblk.find('groupDelivered[i] = true;')
        if i_guard < 0:
            failures.append('[轉送送出點] groupNoteUnicastAck() 少了 CR1 守衛的逐字錨點：'
                            "'if (otaUnicastRecently(desAddr)) {'")
        elif i_write < 0:
            failures.append('[轉送送出點] groupNoteUnicastAck() 裡找不到 groupDelivered[] 的寫入點')
        elif i_guard > i_write:
            failures.append('[轉送送出點] CR1 守衛排在 groupDelivered[] 寫入**之後**：'
                            '那等於先把假的送達證據記下去再檢查')

    # ── 方向 16：三個終局階段不得被合併回一個 "success"（Task 5 的第一責任）──
    #
    # Task 3 的 MJ2 逐字寫著：`otadl` 的成功**只到「下載＋暫存＋MD5」，一台 slave
    # 都沒被碰過**，而 Task 5 一開 MQTT 入口，那個階段值就會直接送到 App 顯示成
    # 「更新完成」。Task 5 的作法是讓那條路徑走自己的終局階段 OTA_STAGED_OK
    #（對 App 是 "staged_only"），與版本回檢成功的 "success" 分開。
    #
    # 這一條驗三件事：
    #   (1) **完整性**：otaPhaseIsFinal() 列出的每一個階段，都必須在
    #       otaPhaseName() 有自己的 case（否則 App 讀到的是 fallback 的 "idle"），
    #       也必須在「停留 30 秒再回 idle」那個 case 群組裡（否則永遠不回 idle）。
    #       **名單從原始碼解析，不寫死在這支腳本裡** —— 寫死等於再開一個
    #       「加了階段但驗證沒跟上」的缺口。
    #   (2) **兩個收尾函式各自剛好一個呼叫點**。
    #   (3) **位置**：otaFinishStagedOnly() 必須長在 `if (otaStageOnly) {` 分支裡，
    #       而那個分支裡**不得**出現 otaFinish() —— 「把 otaFinishStagedOnly() 換回
    #       otaFinish()」正是這個 Task 存在的理由，也是最容易被「順手簡化」掉的一行。
    #
    # **它擋不住什麼**：
    #   - 它不檢查字串**內容**是否誠實。有人把 "staged_only" 改成 "success"，
    #     方向 1 的逐字錨點會叫，但若同時改掉錨點就通得過 —— 那時擋住的是人不是腳本。
    #   - 它比對的是**文字位置**，不是控制流（與方向 13／14 同一個限制）。
    #   - 它不驗 otaStageOnly 這個旗標本身有沒有被正確設定（那是 otaStart() 的
    #     呼叫端責任，靠方向 1 的 `otaStart(id, url, ver, md5, force, false);` 錨點）。
    final_phases = []
    m_final = re.search(r'bool otaPhaseIsFinal\(\) \{(.*?)\n\}', master_src, re.S)
    if not m_final:
        failures.append('[終局階段] 找不到 otaPhaseIsFinal() 的函式本體，方向 16 無法檢查')
    else:
        final_phases = sorted(set(re.findall(r'OTA_[A-Z_]+', m_final.group(1))))
        if len(final_phases) < 2:
            failures.append('[終局階段] otaPhaseIsFinal() 解析出 %s，少於 2 個 —— '
                            '解析樣式可能已經對不上函式寫法' % final_phases)
        name_fn = master_src.split('const char* otaPhaseName() {', 1)
        dwell_i = ota_fn.find('case OTA_SUCCESS:') if ota_fn else -1
        dwell_j = ota_fn.find('otaPhase = OTA_IDLE;', dwell_i) if dwell_i >= 0 else -1
        dwell = ota_fn[dwell_i:dwell_j] if (dwell_i >= 0 and dwell_j > dwell_i) else ''
        if len(name_fn) < 2:
            failures.append('[終局階段] 找不到 otaPhaseName() 的定義，方向 16 無法檢查')
        elif not dwell:
            failures.append('[終局階段] 找不到「停留 30 秒再回 idle」的 case 群組'
                            '（錨點 case OTA_SUCCESS: → otaPhase = OTA_IDLE;）')
        else:
            name_body = name_fn[1].split('\n}', 1)[0]
            for ph in final_phases:
                if ('case %s:' % ph) not in name_body:
                    failures.append('[終局階段] %s 是終局階段，但 otaPhaseName() 沒有它的 case：'
                                    'App 會讀到 fallback 的 "idle"，等於一個真的發生過的'
                                    '終局狀態對外完全隱形' % ph)
                if ('case %s:' % ph) not in dwell:
                    failures.append('[終局階段] %s 是終局階段，但不在「停留 30 秒再回 idle」'
                                    '的 case 群組裡：otaPhase 會永遠卡在它上面，'
                                    'App 的 ota.phase 再也不會回到 idle' % ph)
    for label, needle, want in (
            ('零 slave 接觸的收尾', 'otaFinishStagedOnly();', 1),
            ('版本回檢成功的收尾', 'otaFinish();', 1)):
        n = len([1 for _, line in code_lines if needle in line])
        if n != want:
            failures.append('[終局階段] %s %r 的非註釋呼叫點有 %d 個（必須剛好 %d 個）。'
                            '多一個＝多一條宣告結束的路徑；零個＝那條路徑改接到別的收尾了'
                            % (label, needle, n, want))
    staged_i = ota_fn.find('if (otaStageOnly) {') if ota_fn else -1
    staged_j = ota_fn.find('HoOtaBeginPayload bg;', staged_i) if staged_i >= 0 else -1
    if staged_i < 0 or staged_j < staged_i:
        failures.append('[終局階段] 找不到 OTA_STAGED 的 stageOnly 分支'
                        '（錨點 if (otaStageOnly) { → HoOtaBeginPayload bg;）')
    else:
        staged_blk = '\n'.join(ln for ln in ota_fn[staged_i:staged_j].split('\n')
                               if not ln.lstrip().startswith('//'))
        if 'otaFinishStagedOnly();' not in staged_blk:
            failures.append('[終局階段] otadl 的 stageOnly 分支裡找不到 otaFinishStagedOnly()：'
                            '那條路徑一台 slave 都沒碰過，不得與版本回檢成功共用收尾')
        if 'otaFinish();' in staged_blk:
            failures.append('[終局階段] otadl 的 stageOnly 分支裡出現 otaFinish()：'
                            '它會把階段設成 OTA_SUCCESS，而那條路徑只完成「下載＋暫存＋MD5」，'
                            '**一台 slave 都沒被接觸過** —— App 會顯示「更新完成」'
                            '（Task 3 的 MJ2 逐字警告過的假綠燈）')

    # ── 方向 17：「slave 親口說校驗通過」這件事的唯一產生點 ──
    #
    # Task 4 把 OTA_VERIFYING 的入口從「必須收到 HO_OTA_OK」放寬成「END 送出後
    # 10 秒沒回應也進來」。於是同一個階段底下混著兩種事實：有 slave 正面證據的，
    # 與**10 秒內沒收到整份校驗結果**的（不是「一封回應都沒收到」：
    # BEGIN 的 READY 與窗口 ACK 本來就收過了）。otaSlaveVerified 是兩者對外唯一的分野
    #（"verifying" vs "unconfirmed"）。
    #
    # 這一條驗：設成 true 的地方**剛好一處**，而且必須排在
    # 「(HO_OTA_OK, base==totalChunks, mask==0xFFFF) 三項齊備」的判斷**之後**。
    # 搬到那個 if 之外 ＝ 對 App 宣稱一件沒發生的事，而且**一字不改、只搬位置**
    #（A 族第 18 次的形狀）—— 所以位置要單獨驗。
    #
    # **它擋不住什麼**：
    #   - 它只認直接指派的字面。用別的變數轉一手（`bool v = true; otaSlaveVerified = v;`）
    #     它抓不到。
    #   - 它比對文字先後，不驗控制流。
    #   - 它**完全不影響狀態機**：即使 otaSlaveVerified 是 false，那條路徑一樣會走
    #     版本回檢、一樣可能走到 "success"。這一條保護的是「回報的字串誠不誠實」，
    #     不是「有沒有多開一條綠燈」（多開綠燈那一側由方向 14 顧）。
    n_verified = len([1 for _, line in code_lines if 'otaSlaveVerified = true;' in line])
    if n_verified != 1:
        failures.append('[slave 正面證據] `otaSlaveVerified = true;` 的非註釋指派有 %d 處'
                        '（必須剛好 1 處，就在 OTA_END_SENT 收到 HO_OTA_OK 那個分支裡）。'
                        '多一處＝有第二條路徑在宣稱「slave 說它校驗通過了」' % n_verified)
    # ── C1（複審）：位置維度**必須是雙邊的**（A 族第 18／20 次的同型第三次）──
    #
    # 這一條的第一版只驗 `e_set > e_ok`，也就是「不准往上搬」。複審實測出一個
    # 一字不改的突變：**把 `otaSlaveVerified = true;` 往下搬進同一個 case 底部的
    # 10 秒逾時分支** → 三支工具全綠，而效果是把 "verifying" 與 "unconfirmed"
    # **完全對調** —— 沒有「校驗通過」證據的那條路徑會顯示成「slave 親口回報校驗通過」。
    #
    # 所以改成夾在**兩個**錨點之間：必須排在證據判斷之後，**而且**必須排在
    # 「校驗失敗」那條 else 分支之前（那是 OK 分支的結尾，逾時分支還在更後面）。
    #
    # **它擋不住什麼**：這仍然是「文字先後」而不是括號結構 —— 把整段連同 else
    # 一起重排，或把賦值包進一個更早出現、但執行時走不到的分支，它照樣過。
    # 那時它是**中斷器**：請人回來確認新寫法仍然只在有證據時為真，再更新這張表。
    if end_sent is not None:
        # 位置比對一律在**去掉註釋行**的版本上做（與方向 14 同一個理由：
        # 解釋「為什麼一定要在這裡」的註釋本身就得寫出這幾個字）。
        end_code = chr(10).join(ln for ln in end_sent.split(chr(10))
                                if not ln.lstrip().startswith('//'))
        e_ok = end_code.find('otaAckBase == otaTotalChunks && otaAckBits == 0xFFFF')
        e_set = end_code.find('otaSlaveVerified = true;')
        e_end = end_code.find('[OTA] slave 校驗失敗，狀態碼 %u')
        if e_set < 0:
            failures.append('[slave 正面證據] OTA_END_SENT 區塊裡找不到 '
                            '`otaSlaveVerified = true;`：那是 "verifying" 與 "unconfirmed" '
                            '唯一的分野，搬出這個區塊就沒有任何東西保證它只在有證據時為真')
        elif e_ok < 0 or e_set < e_ok:
            failures.append('[slave 正面證據] `otaSlaveVerified = true;` 排在'
                            '「HO_OTA_OK ＋ base ＋ mask 三項齊備」的判斷**之前**：'
                            '那等於在還沒確認證據之前就先宣告有證據')
        elif e_end < 0:
            failures.append('[slave 正面證據] OTA_END_SENT 區塊裡找不到「校驗失敗」'
                            'else 分支的錨點，位置檢查的**下界**無法成立')
        elif e_set > e_end:
            failures.append('[slave 正面證據] `otaSlaveVerified = true;` 排在'
                            '「校驗失敗」的 else 分支**之後** —— 它已經不在收到 '
                            'HO_OTA_OK 的那個分支裡了。最可能的落點是同一個 case 底部的'
                            '**10 秒逾時分支**，而那條路徑正是「沒收到整份校驗結果」的那一條：'
                            '這樣會把 "verifying" 與 "unconfirmed" 完全對調，'
                            '**零證據的情況會顯示成「slave 親口回報校驗通過」**')

    # ── MJ3（複審）：session reset 無人守 ──
    # 複審突變：刪掉 otaRelayStaged() 的 `otaSlaveVerified = false;` → 三支工具全綠。
    # 後果是**上一場的 true 漏到下一場**：新工作階段一封 OK 都沒收到，逾時落到版本
    # 回檢時卻報 "verifying"。方向 17 的第一版只驗「設成 true 的地方」，從不驗 reset ——
    # 而一個「只會被設成 true、永遠不歸零」的旗標等於沒有旗標。
    #
    # **它擋不住什麼**：只數字面與所在函式。有人把 reset 搬到函式尾端（在 otaPhase
    # 已經被設定之後）它抓不到；也不驗兩個起手函式之外有沒有第三條起手路徑。
    # `^\s*otaSlaveVerified` 是必要的：宣告那一行（`bool     otaSlaveVerified = false;`）
    # 也含同樣的字面，算進來會讓期望值變成一個「有兩個來源」的數字。
    n_reset = len([1 for _, line in code_lines
                   if re.match(r'^\s*otaSlaveVerified\s*=\s*false;', line)])
    if n_reset != 2:
        failures.append('[slave 正面證據] `otaSlaveVerified = false;` 的非註釋指派有 %d 處'
                        '（必須剛好 2 處：otaStart() 與 otaRelayStaged() 兩個起手點各一）。'
                        '少一處＝**上一場的 true 會漏到下一場**，'
                        '新工作階段在零證據的情況下報 "verifying"' % n_reset)
    for fname, start_a, end_a in (
            ('otaStart()', 'bool otaStart(const char* slaveId', 'bool otaRelayStaged('),
            ('otaRelayStaged()', 'bool otaRelayStaged(', 'void updateOtaSession(')):
        i, j = master_src.find(start_a), master_src.find(end_a)
        if i < 0 or j < 0 or j < i:
            failures.append('[slave 正面證據] 找不到 %s 的函式範圍，reset 檢查無法定位' % fname)
        elif 'otaSlaveVerified = false;' not in master_src[i:j]:
            failures.append('[slave 正面證據] %s 裡沒有 `otaSlaveVerified = false;`：'
                            '那條起手路徑會沿用上一場的證據旗標' % fname)
    if 'otaSlaveVerified ? "verifying" : "unconfirmed"' not in master_src:
        failures.append('[slave 正面證據] otaPhaseName() 不再用 otaSlaveVerified 區分'
                        '"verifying"／"unconfirmed"：沒有 slave 正面證據的那條路徑'
                        '會對 App 宣稱 slave 已經回報校驗通過')

    # ── 方向 18：代發狀態的 "updating" 只能在「真的正在被轉送」時出現 ──
    #
    # plan Task 5 Step 3 的範本寫的是 `idx == otaTargetIdx`，而 otaTargetIdx 的宣告處
    # 逐字寫著「會過期」（unpairSlave() 會把 slaves[] 往前搬）。用過期索引會把**另一台**
    # 的代發狀態蓋成 "updating" —— 那台若其實已經離線，App 看到的是「更新中」而不是
    # 「離線」，等於用一個進行中的假象蓋掉一個真實的壞消息。
    # 本檔的慣例是「真相一律以 otaTargetMac 為準」，這裡把它釘住。
    #
    # ── 這一條被繞過的沿革（每一次都是「只擋了一半」）──
    # MJ4（第一輪複審）：第一版只釘前半行 → 把階段上界從 OTA_VERIFYING 改成
    #   OTA_STAGED_OK 就全綠。改成兩行一起釘。
    # N1（第二輪複審）：那兩行**一字不動**，在後面另加一行
    #   `isOtaTarget = isOtaTarget || otaPhaseIsFinal();`
    #   → 三支工具全綠，效果與 MJ4 **完全相同**。
    #   本檔方向 12 的註釋早就寫著「HIT 擋不住『再加一行覆蓋它』」，
    #   而方向 18 的「它擋不住什麼」段落**沒有寫這一格** —— A 族的定義。
    # 本輪自己補跑的三個加寬突變（三個在補完前都活著）：
    #   (i)   在 if/else **之後**再加一段 `if (otaPhaseIsFinal() && memcmp(...) == 0)
    #         doc["status"] = "updating";` —— 上面那幾行字面完全不動。
    #   (ii)  把 else 分支改成 `slaves[idx].online ? "online"
    #         : (otaPhaseIsFinal() ? "updating" : "offline")` —— 直接拿「更新中」
    #         蓋掉「離線」，那是誤綠最壞的一格。
    #   (iii) **一個字都不改**，把 `OTA_STAGED_OK` 從列舉最後搬到 `OTA_BEGIN_SENT`
    #         後面 —— `otaPhase <= OTA_VERIFYING` 這行字面不變，涵蓋範圍卻靜靜
    #         多吃一個終局階段。（列舉宣告處自己就寫著「刻意排在列舉最後」。）
    #
    # 所以現在驗四件事：
    #   (a) 從 `bool isOtaTarget` 到 else 分支的右大括號**整段連續逐字如此**
    #       —— 擋 N1 的「中間加一行」與「把 if 條件改成 isOtaTarget || …」
    #   (b) `isOtaTarget` 的非註釋賦值全檔剛好 1 處 —— 擋「賦值搬到別的地方去做」
    #   (c) publishSlaveStatus() 的非註釋行裡 `doc["status"]` 剛好 2 處、
    #       `"updating"` 剛好 1 處 —— 擋上面的 (i) 與 (ii)
    #   (d) 列舉 OtaPhase 裡 OTA_BEGIN_SENT..OTA_VERIFYING 這段**剛好是**那五個
    #       轉送中階段 —— 擋上面的 (iii)
    #
    # **它擋不住什麼**（照實寫）：
    #   - 它不驗 otaTargetMac 本身是不是對的（那是方向 15 與 otaSetTarget() 的事）。
    #   - (a) 是字面比對：把這段重構成一個具名述詞（例如 `slaveIsOtaTarget(idx)`）
    #     會當場變紅，即使新寫法完全正確。那時它是**中斷器**不是守衛。
    #   - (c) 只數 `doc["status"]` 這個字面。改用 `doc[F("status")]`、或先算進一個
    #     `const char* st` 再一次寫進去，它就數不到 —— 那時 (a) 仍然會擋下這種改法。
    #   - **(a)+(c) 同時被繞過的那一格（第 3 輪實測，三支工具 exit 0）：
    #     「留一份不被呼叫的死程式碼餵錨點」。** 把這八行原封不動搬進一個放在
    #     `publishSlaveOffline()` **之後**（也就是 (c) 的計數範圍之外）、永遠不會被呼叫的
    #     函式，真正的程式碼改用具名述詞並維持 `doc["status"]` 2 次／`"updating"` 1 次 →
    #     全綠。**must-contain 型的錨點證明的是「這幾個字還在檔案裡」，
    #     不是「執行路徑上還在做這件事」** —— 純字串比對原理上的極限，依 Ruling 不補守衛。
    #     （順帶更正一個曾寫進報告的錯誤機制：「整段連同 else 改寫成具名函式」
    #      **是紅的、不是繞過** —— (a) 是 must-contain，把那八行刪掉就會叫。）
    #   - (d) 現在驗文字順序**與**實際數值兩個維度，但它仍不驗 `>=`／`<=` 兩個
    #     運算子沒被對調 —— 那一側由 (a) 的整段字面顧。
    #   - **X3（複審第 3 輪，本方向最大的已知缺口）：出口釘了，入口沒釘。**
    #     (a) 把 `doc["status"] = slaves[idx].online ? "online" : "offline";` 這個**出口**
    #     釘死了，卻沒有任何人守它的**輸入** `slaves[idx].online`。複審實測
    #     （可編譯、三支 exit 0）：把 updateSlaveOnline 那段改成
    #     `slaves[i].online = isOnline || (otaPhase != OTA_IDLE &&`
    #     ` memcmp(slaves[i].mac, otaTargetMac, 6) == 0);`
    #     → **一台在 OTA 期間真的死掉的 slave，else 分支照樣回報 "online"**。
    #     這是本輪新規則「守衛要蓋到值最後被寫進哪個欄位」的**鏡像**：
    #     同一個規則往上游看就是「也要問這個值是從哪裡來的」。
    #     **依控制者第 3 輪的 Ruling，這一格不補守衛、照實列在這裡**：
    #     純字串比對的框架原理上無法把一條資料流的兩端同時封死，
    #     再疊一層只會製造下一個「宣稱擋住、其實擋不住」。
    ISOTATARGET_BLOCK = NL.join([
        '  bool isOtaTarget = (memcmp(slaves[idx].mac, otaTargetMac, 6) == 0) &&',
        '                     (otaPhase >= OTA_BEGIN_SENT && otaPhase <= OTA_VERIFYING);',
        '  if (isOtaTarget) {',
        '    doc["status"] = "updating";',
        '    doc["ota_progress"] = otaProgressPercent();',
        '  } else {',
        '    doc["status"] = slaves[idx].online ? "online" : "offline";',
        '  }',
    ])
    if ISOTATARGET_BLOCK not in master_src:
        failures.append('[代發 updating] publishSlaveStatus() 的 isOtaTarget 判斷**連同 '
                        'if/else 整段**不再逐字如此。這一段必須是連續的八行，中間'
                        '**一行都不准插**：(a) 改用 otaTargetIdx 會在名冊變動後把**別台**的'
                        '狀態蓋成 "updating"（開錯門的 MAC 版）；(b) 階段上界放寬到終局階段，'
                        '或事後補一行 `isOtaTarget = isOtaTarget || …` 覆蓋它，都會讓 '
                        'staged_only（**零 slave 接觸**）與 failed 期間的目標顯示成 '
                        '"updating" + ota_progress:100 —— failed 那一格是拿「更新中」'
                        '蓋掉一台真的離線的設備，那是誤綠'
                        + RECONFIRM)
    n_isota_assign = sum(len(re.findall(r'\bisOtaTarget\s*[|&^+\-]?=(?!=)', line))
                         for _, line in code_lines)
    if n_isota_assign != 1:
        failures.append('[代發 updating] `isOtaTarget` 的非註釋賦值有 %d 處（必須剛好 1 處，'
                        '就是宣告時那一次）。多一處＝有人在別的地方重新決定「這台算不算 '
                        'OTA 目標」，而整段字面比對看不到那一行' % n_isota_assign)
    i_pss = master_src.find('void publishSlaveStatus(int idx) {')
    j_pss = master_src.find('void publishSlaveOffline(int idx) {')
    if i_pss < 0 or j_pss < 0 or j_pss < i_pss:
        failures.append('[代發 updating] 找不到 publishSlaveStatus() 的函式範圍，'
                        '「status 只被寫兩次」的檢查無法定位')
    else:
        pss_code = NL.join(ln for ln in master_src[i_pss:j_pss].split(NL)
                           if not ln.lstrip().startswith('//'))
        n_set_status = pss_code.count('doc["status"]')
        n_updating = pss_code.count('"updating"')
        if n_set_status != 2:
            failures.append('[代發 updating] publishSlaveStatus() 的非註釋行裡 `doc["status"]` '
                            '出現 %d 次（必須剛好 2 次：if 分支與 else 分支各一）。'
                            '第三次＝在 if/else 之後又蓋了一層，而那一層可以在'
                            '**零 slave 接觸**的終局階段把離線的設備寫成 "updating"'
                            % n_set_status)
        if n_updating != 1:
            failures.append('[代發 updating] publishSlaveStatus() 的非註釋行裡 `"updating"` '
                            '出現 %d 次（必須剛好 1 次）。把它塞進 else 分支的三元式'
                            '（`online ? "online" : (… ? "updating" : "offline")`）'
                            '就是拿「更新中」蓋掉「離線」的那一格' % n_updating)
    # (d) 列舉範圍：`otaPhase >= OTA_BEGIN_SENT && otaPhase <= OTA_VERIFYING` 這個
    #     範圍的**意義**完全來自列舉。OTA_STAGED_OK 的宣告處自己就寫著
    #     「刻意排在列舉最後」，但那句話原本沒有任何機械檢查。
    #
    # ⚠ I-2（複審第 3 輪）：**第一版的 (d) 只驗「名字的文字順序」，而涵蓋範圍來自「值」。**
    #   複審實測（可編譯、三支工具 exit 0）：把 `OTA_VERIFYING,` 改成 `OTA_VERIFYING = 10,`
    #   （註釋寫「留 9 給未來的階段」）、`OTA_STAGED_OK,` 改成 `OTA_STAGED_OK = 9,` ——
    #   **文字順序一個字都沒動**，而 `<= OTA_VERIFYING(10)` 現在涵蓋 `OTA_STAGED_OK(9)`，
    #   零 slave 接觸的 staged_only 期間目標顯示「更新中、100%」。與 MJ4／N1 同一個誤綠。
    #   (d) 的失敗訊息當時逐字寫著「把那句話釘住」—— **它沒有釘住**。
    #
    #   所以現在**兩個維度都驗**：文字順序（擋「搬位置」）＋ 實際數值（擋「改初值」）。
    #   數值是照 C 的規則自己算的：有顯式初值就跟著跳，沒有就 +1。
    #   同一份列舉的註釋自己列出三處有序比較，這裡把其中兩個區間都用**值**驗一次
    #   （`>= OTA_BEGIN_SENT && <= OTA_END_SENT` 是 pollNextSlave() 的跳過範圍）。
    m_enum = re.search(r'enum OtaPhase : uint8_t \{(.*?)' + NL + r'\};', master_src, re.S)
    if m_enum is None:
        failures.append('[代發 updating] 解析不出 `enum OtaPhase : uint8_t { … };`，'
                        '階段範圍的順序檢查無法進行')
    else:
        enum_code = NL.join(ln for ln in m_enum.group(1).split(NL)
                            if not ln.lstrip().startswith('//'))
        # ── 逐行解析，**任何一行看不懂就當場失敗**（fail-closed）──
        #
        # 自補突變（本輪自己想的，補之前**兩個都能造出誤綠**）：
        #   Z1：初值寫成**運算式** `OTA_STAGED_OK = OTA_END_SENT + 1,`
        #       —— 第一版的 `=\s*(-?\d+)` 對不上，那一項會被整個丟掉。
        #   Z5：**最後一個成員不寫尾逗號** `OTA_STAGED_OK = 9`
        #       —— 第一版的樣式尾巴要求 `,`，那一項會被整個丟掉，
        #       而「被丟掉」正好讓它不出現在區間名單裡 → **靜靜變綠**。
        # 兩個縫的形狀一樣：**解析失敗被當成「沒問題」**。
        # 所以改成逐行走，看不懂就 append 一則失敗，不再靠一個正則「撈得到多少算多少」。
        enum_items = []
        for _raw in enum_code.split(NL):
            _body = _raw.split('//')[0].strip()
            if not _body:
                continue
            _body = _body.rstrip(',').strip()
            if not _body:
                continue
            _m = re.match(r'^(OTA_[A-Z_]+)\s*(?:=\s*(.+))?$', _body)
            if _m is None:
                failures.append('[代發 updating] 列舉 OtaPhase 裡有一行解析不出來 → %r。'
                                '本檢查要算出每個成員的**實際數值**才能驗涵蓋範圍，'
                                '看不懂就一律當失敗（fail-closed）—— '
                                '「解析失敗被當成沒問題」正是這一條被繞過的形狀' % _body[:60])
                continue
            _init = _m.group(2)
            if _init is not None and re.match(r'^-?\d+$', _init.strip()) is None:
                failures.append('[代發 updating] 列舉成員 %s 的初值是 `%s`，不是整數字面。'
                                '本檢查不求值運算式（求了也只是把 C 的規則抄一遍、抄錯就是'
                                '另一個假綠燈），所以**初值請寫整數字面或乾脆不要寫**。'
                                '這一格是刻意 fail-closed：`= OTA_END_SENT + 1` 這種寫法'
                                '在第一版會讓整個成員被丟掉、靜靜變綠'
                                % (_m.group(1), _init.strip()[:40]))
                continue
            enum_items.append((_m.group(1), _init))
        order = [n for n, _ in enum_items]
        enum_val = {}
        _next = 0
        for _name, _init in enum_items:
            if _init:
                _next = int(_init)
            enum_val[_name] = _next
            _next += 1
        RELAY_RANGE = ['OTA_BEGIN_SENT', 'OTA_RELAYING', 'OTA_WAIT_BLOCK_ACK',
                       'OTA_END_SENT', 'OTA_VERIFYING']
        POLL_SKIP_RANGE = ['OTA_BEGIN_SENT', 'OTA_RELAYING', 'OTA_WAIT_BLOCK_ACK',
                           'OTA_END_SENT']
        if 'OTA_BEGIN_SENT' not in order or 'OTA_VERIFYING' not in order:
            failures.append('[代發 updating] 列舉 OtaPhase 裡找不到 OTA_BEGIN_SENT／'
                            'OTA_VERIFYING，範圍檢查無法定位（解析到 %s）' % order)
        else:
            got = order[order.index('OTA_BEGIN_SENT'):order.index('OTA_VERIFYING') + 1]
            if got != RELAY_RANGE:
                failures.append('[代發 updating] 列舉 OtaPhase 的**文字順序**裡 '
                                'OTA_BEGIN_SENT..OTA_VERIFYING 這段是 %s，必須剛好是 %s。'
                                '`otaPhase <= OTA_VERIFYING` 這行**字面一個字都不用改**，'
                                '只要把終局階段（例如 OTA_STAGED_OK）搬進這個區間，'
                                '零 slave 接觸的階段就會被判成「更新中」'
                                % (got, RELAY_RANGE))
            # ── 值的維度（I-2）：涵蓋範圍是由**值**決定的，不是由文字位置決定的 ──
            for _hi, _want, _why in (
                    ('OTA_VERIFYING', RELAY_RANGE,
                     'publishSlaveStatus() 的代發 "updating" 範圍：多吃一個終局階段就會讓'
                     '**零 slave 接觸**的 staged_only 期間顯示「更新中、100%」，'
                     '而 failed 那一格是拿「更新中」蓋掉一台真的離線的設備'),
                    ('OTA_END_SENT', POLL_SKIP_RANGE,
                     'pollNextSlave() 的「轉送期間跳過目標」範圍（列舉註釋自己列出的'
                     '三處有序比較之一）：多吃一個階段就會多跳一輪輪詢，'
                     '少一個就會在轉送中去打擾目標')):
                _lo_v = enum_val.get('OTA_BEGIN_SENT')
                _hi_v = enum_val.get(_hi)
                if _lo_v is None or _hi_v is None:
                    failures.append('[代發 updating] 列舉 OtaPhase 解析不出 OTA_BEGIN_SENT／%s '
                                    '的數值，**值維度**的範圍檢查無法進行' % _hi)
                    continue
                inside = sorted((n for n, v in enum_val.items() if _lo_v <= v <= _hi_v),
                                key=lambda n: (enum_val[n], n))
                if inside != _want:
                    failures.append('[代發 updating] 列舉 OtaPhase 的**實際數值**裡 '
                                    '[OTA_BEGIN_SENT(%d) .. %s(%d)] 這個區間涵蓋 %s，'
                                    '必須剛好是 %s。**文字順序一個字都不用改，只要給某個成員'
                                    '一個顯式初值就能靜默改變涵蓋範圍**（複審實測：'
                                    '`OTA_VERIFYING = 10` ＋ `OTA_STAGED_OK = 9` → '
                                    '三支工具全綠、編得過，而 staged_only 期間目標顯示'
                                    '「更新中、100%%」）。%s'
                                    % (_lo_v, _hi, _hi_v, inside, _want, _why))

    # ── 方向 19：STATUS_OTA_MAX_BYTES = 128 那四條前提，必須真的有人守 ──
    #
    # N2（複審第 2 輪）：ho_master1.ino 的常數註釋逐字寫著
    # 「這一條由 otaSetTarget() 的字元過濾保證」，而那個過濾迴圈**沒有任何守衛**。
    # 複審實測兩個突變，兩個都三支工具全綠：
    #   (i)  白名單尾端加一個雙引號；(ii) **整段刪掉過濾迴圈**。
    # 後果是 ota 物件最壞值回到 138 > 128，STATUS_OTA_MAX_BYTES 靜靜停止是上界 ——
    # 而那正是 Task 5 Step 1「拆成具名分項」整個設計要防的那件事。
    # **宣稱有保證、實際沒有守衛 ＝ A 族的定義。**
    #
    # 本輪自己補跑、補完前都活著的兩個加寬突變：
    #   (iii) 過濾迴圈**一字不動**，在它後面再抄一次未過濾的原字串
    #   (iv)  `char otaTargetId[20]` 放大成 `[64]` —— 過濾迴圈完全不動，
    #         但 31 bytes 那一項變成 75，ota 物件上界變成 163。
    #
    # 所以這一條驗四件事，對應那四條前提：
    #   (a) otaSetTarget() 的**整個函式體**逐字如此（含過濾迴圈與白名單字元集）——
    #       前提 (3)。擋 (i)(ii)(iii)。
    #   (b) otaTargetId 的寫入點必須全部落在 otaSetTarget()／fakeOtaForCapacityTest()
    #       之內，且全檔剛好 3 處 —— 擋「在別處塞一份未過濾的複本」。
    #   (c) 兩個字元陣列的宣告逐字釘住 —— 前提 (4)（otaTargetId[20] ＝ 最長 19 字元）
    #       與前提 (2) 的緩衝上限（otaErrCode[20]）。擋 (iv)。
    #   (d) otaPhaseName() 回傳的字串字面 ≤ 12、otaFail()／otaErrCode 的錯誤碼
    #       字面 ≤ 16 —— 前提 (1) 與 (2)。
    #
    # **它擋不住什麼**（照實寫）：
    #   - (a) 是整段字面比對。把過濾抽成 `sanitizeTargetId()` 之類的具名函式會當場
    #     變紅，即使新寫法完全正確 —— 那時它是**中斷器**：請人回來確認新字元集
    #     仍不含任何需要 JSON 逃逸的字元，並重算 31 這個數字，再更新錨點。
    #   - **(b) 是一張函式名白名單，原理上不封閉。** 沒被列進 OTA_ID_WRITE 那串名字的
    #     寫法它一個字都驗不到，包含但不限於：
    #       * `char* p = otaTargetId; snprintf(p, …, id);`（**先指派給指標再寫**）
    #       * `*(otaTargetId + n) = c;` / `&otaTargetId[0]` 之類的位址算術
    #       * 把 otaTargetId 傳進某個自己寫的函式，由那個函式去寫
    #       * 任何未來新加的字串 API（第一版就漏了 `strlcpy`，被實測繞過一次）
    #     **它擋的是「照直覺寫」的那條路，不是「刻意繞」的那條路。**
    #   - (d) 只認 `otaFail("字面")` 與 `snprintf(otaErrCode, …, "字面")` 兩種寫法。
    #     **用變數呼叫 `otaFail(code)` 它一個字都驗不到**（現行程式沒有這種呼叫點，
    #     但那是真的缺口，不是「原理上驗不出」）。
    #   - `ota["size"]` 的位數完全不在檢查範圍內：otaTotalSize 是 uint32_t，
    #     十進位最多 10 位，靠的是下載階段那道**執行期**的 too_big 檢查壓在 7 位以內。
    #     最壞多 3 bytes，落在 128−119＝9 的餘裕裡，但那不是這支腳本證明的。
    #   - 它完全不驗 ArduinoJson 真的照這個規則序列化 —— 那要 `fakeota` + `jsonsize`
    #     實機量測，而那一步至今沒跑過（見 task-5-report.md 的疑慮清單）。
    OTA_SET_TARGET_FN = NL.join([
        'void otaSetTarget(const char* slaveId, const uint8_t* mac) {',
        '  snprintf(otaTargetId, sizeof(otaTargetId), "%s", slaveId);',
        "  for (size_t i = 0; otaTargetId[i] != '\\0'; i++) {",
        '    char c = otaTargetId[i];',
        "    bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||",
        "                (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||",
        "                c == '.' || c == ':';",
        "    if (!safe) otaTargetId[i] = '?';",
        '  }',
        '  if (mac != nullptr) {',
        '    memcpy(otaTargetMac, mac, 6);',
        '  } else {',
        '    memset(otaTargetMac, 0, sizeof(otaTargetMac));',
        '    otaTargetIdx = -1;',
        '  }',
        '}',
    ])
    if OTA_SET_TARGET_FN not in master_src:
        failures.append('[ota 上界前提] otaSetTarget() 的函式體不再逐字如此。'
                        '這個函式是 STATUS_OTA_MAX_BYTES = 128 第 (3) 條前提'
                        '（otaTargetId 不含任何需要 JSON 逃逸的字元）的**唯一**保證：'
                        '白名單多一個需要逃逸的字元、整段刪掉、'
                        '或在後面再抄一次未過濾的原字串，'
                        'ota 物件的最壞值都會從 119 回到 138 以上、超過 128，'
                        '而 static_assert 用的是常數、抓不到。'
                        '**update_slave 的 id 欄位完全來自遠端，沒有其他格式限制。**'
                        + RECONFIRM_19A)

    def _fn_line_range(sig_a, sig_b):
        a, b = master_src.find(sig_a), master_src.find(sig_b)
        if a < 0 or b < 0 or b < a:
            return None
        return (master_src.count(NL, 0, a) + 1, master_src.count(NL, 0, b) + 1)

    allowed_ranges = []
    for name, sig_a, sig_b in (
            ('otaSetTarget()', 'void otaSetTarget(const char* slaveId',
             'bool otaStart(const char* slaveId'),
            ('fakeOtaForCapacityTest()', 'void fakeOtaForCapacityTest() {',
             'void printStatusJsonSize() {')):
        rng = _fn_line_range(sig_a, sig_b)
        if rng is None:
            failures.append('[ota 上界前提] 找不到 %s 的函式範圍，'
                            'otaTargetId 寫入點的定位檢查無法進行' % name)
        else:
            allowed_ranges.append(rng)
    # ⚠ I-3（複審第 3 輪）：第一版的名單漏了 `strlcpy`。複審實測（可編譯、三支 exit 0）：
    #   在 update_slave 的 bad_json 分支加一行
    #   `strlcpy(otaTargetId, id, sizeof(otaTargetId));`，註釋還照抄 otaSetTarget()
    #   自己寫的意圖「錯誤回報要帶著使用者送的那個 id」——
    #   而 `id` 來自 `cmd["id"] | ""`，**完全來自遠端、零格式限制**。
    #   19 個雙引號直接進 otaTargetId → ota 物件 138 > 128，
    #   STATUS_OTA_MAX_BYTES 靜靜停止是上界。**N2 修完當場復發。**
    #   名單補成下面這串。**這仍然是白名單，不是封閉的**（見本方向的「擋不住什麼」）。
    OTA_ID_WRITE = re.compile(
        r'(?:snprintf|sprintf|vsnprintf|vsprintf'
        r'|strcpy|strncpy|strlcpy|strcat|strncat|strlcat'
        r'|memcpy|memmove|memset|toCharArray|getBytes)\s*\(\s*otaTargetId\b'
        r'|\botaTargetId\s*(?:\[[^\]]*\])?\s*[|&^+\-]?=(?!=)')
    n_id_write = 0
    for lineno, line in code_lines:
        if re.match(r'\s*char\s+otaTargetId\[', line):
            continue        # 宣告那一行由下面的陣列容量錨點顧
        if OTA_ID_WRITE.search(line):
            n_id_write += 1
            if allowed_ranges and not any(a <= lineno <= b for a, b in allowed_ranges):
                failures.append('[ota 上界前提] ho_master1.ino:%d 在 otaSetTarget()／'
                                'fakeOtaForCapacityTest() **之外**寫入 otaTargetId → %s'
                                '。那條路徑繞過了字元過濾，遠端字串會直接進 ota["target"]'
                                % (lineno, line.strip()[:80]))
    if n_id_write != 3:
        failures.append('[ota 上界前提] otaTargetId 的非註釋寫入點有 %d 處'
                        '（必須剛好 3 處：otaSetTarget() 的 snprintf 與逐字元覆寫兩處，'
                        '加上 fakeOtaForCapacityTest() 的固定字面 1 處）。'
                        '多一處＝有人在過濾之外又寫了一次' % n_id_write)
    for decl, why in (
            ('char     otaTargetId[20] = "";',
             '前提 (4)：19 字元的上限完全掛在這個容量上。放大成 [64] 會讓 '
             'target 那一項從 31 漲到 75、ota 物件上界變成 163，'
             '而過濾迴圈一個字都沒動、方向 19 的其他檢查全部照過'),
            ('char     otaErrCode[20] = "";',
             '前提 (2)：錯誤碼 16 字元的假設靠這個緩衝封頂。放大它，'
             'error 那一項就不再是 26 bytes')):
        if decl not in master_src:
            failures.append('[ota 上界前提] 找不到逐字的 `%s` —— %s' % (decl, why))
    m_pn = re.search(r'const char\* otaPhaseName\(\) \{(.*?)' + NL + r'\}', master_src, re.S)
    if m_pn is None:
        failures.append('[ota 上界前提] 解析不出 otaPhaseName() 的函式體，'
                        'phase 字串長度（前提 1）無法檢查')
    else:
        pn_code = NL.join(ln for ln in m_pn.group(1).split(NL)
                          if not ln.lstrip().startswith('//'))
        for lit in re.findall(r'"([^"]*)"', pn_code):
            if len(lit) > 12:
                failures.append('[ota 上界前提] otaPhaseName() 回傳的 "%s" 有 %d 字元 > 12：'
                                'STATUS_OTA_MAX_BYTES 的 phase 那一項是照 12 字元'
                                '算成 23 bytes 的，超過就要回頭重算那個常數' % (lit, len(lit)))
    err_lits = re.findall(r'otaFail\("([^"]*)"\)', master_src)
    err_lits += re.findall(r'snprintf\(otaErrCode, sizeof\(otaErrCode\), "([^"%]*)"\)', master_src)
    for lit in err_lits:
        if len(lit) > 16:
            failures.append('[ota 上界前提] 錯誤碼 "%s" 有 %d 字元 > 16：'
                            'STATUS_OTA_MAX_BYTES 的 error 那一項是照 16 字元'
                            '算成 26 bytes 的' % (lit, len(lit)))
    # (e) `ota` 物件本身的**欄位清單**。
    #
    # 本輪自己再補跑的兩個加寬突變，**兩個在補這一段之前都活著**：
    #   (v)  `ota["error"] = otaErrCode;` 後面再加一行 `ota["url"] = otaHost;`
    #        —— otaHost[64] 的內容來自遠端的 url，ota 物件直接爆掉 128。
    #        而那五行**正上方的註釋逐字寫著**「加欄位必須回頭重算那個常數」——
    #        那句話原本沒有任何守衛，正是 A 族的形狀，也正是 Task 5 Step 1
    #        整個「拆成具名分項」的設計要防的那件事。
    #   (vi) `ota["target"] = otaTargetId;` 改成 `= otaHost;`
    #        —— otaSetTarget() 的過濾迴圈**一個字都沒動**，卻整條繞過去了：
    #        前提 (3) 保護的是 otaTargetId 這個變數，不是 `ota["target"]` 這個欄位。
    #        中間那條「誰被發出去」的連線原本沒人釘。
    #
    # 所以把那六行整段釘住，再數兩個字面：欄位數（`ota["`）必須剛好 5、
    # `"ota"` 這個 key 在 buildStatusDoc() 內必須剛好出現 1 次
    # （擋 `doc["ota"]["url"] = …` 這種不經 `ota` 這個區域變數的加欄位寫法）。
    #
    # **它擋不住什麼**：它不驗 `otaProgressPercent()`／`otaTotalSize` 的值域
    #（size 的位數見上面那段），也不驗 slaves[] 或其他頂層區塊有沒有偷偷加欄位
    # —— 那一側靠 SLAVE_ENTRY_MAX_BYTES 與方向 5 的算式複算，不在這一條裡。
    OTA_JSON_BLOCK = NL.join([
        '  JsonObject ota = doc["ota"].to<JsonObject>();',
        '  ota["target"]   = otaTargetId;',
        '  ota["phase"]    = otaPhaseName();',
        '  ota["progress"] = otaProgressPercent();',
        '  ota["size"]     = otaTotalSize;',
        '  ota["error"]    = otaErrCode;',
    ])
    if OTA_JSON_BLOCK not in master_src:
        failures.append('[ota 上界前提] buildStatusDoc() 組 `ota` 物件的那六行不再逐字如此。'
                        '這六行**就是** STATUS_OTA_MAX_BYTES = 128 的實算內容：'
                        '多一個欄位、或把某個欄位改讀別的變數（例如把 target 改成讀'
                        '未經字元過濾的 otaHost），都會讓 128 靜靜停止是上界，'
                        '而 static_assert 用的是常數、抓不到 —— 那正是 Task 5 Step 1 '
                        '要防的「靜默截斷換一個面貌回來」'
                        + RECONFIRM_19E)
    i_bsd = master_src.find('void buildStatusDoc(JsonDocument& doc) {')
    j_bsd = master_src.find('void publishStatus() {')
    if i_bsd < 0 or j_bsd < 0 or j_bsd < i_bsd:
        failures.append('[ota 上界前提] 找不到 buildStatusDoc() 的函式範圍，'
                        'ota 欄位數的檢查無法定位')
    else:
        bsd_code = NL.join(ln for ln in master_src[i_bsd:j_bsd].split(NL)
                           if not ln.lstrip().startswith('//'))
        n_ota_field = bsd_code.count('ota["')
        n_ota_key = bsd_code.count('"ota"')
        if n_ota_field != 5:
            failures.append('[ota 上界前提] buildStatusDoc() 裡 `ota["…"]` 有 %d 個欄位'
                            '（必須剛好 5：target／phase／progress／size／error）。'
                            '多一個字串欄位就足以讓 ota 物件超過 128 bytes' % n_ota_field)
        if n_ota_key != 1:
            failures.append('[ota 上界前提] buildStatusDoc() 裡 `"ota"` 這個 key 出現 %d 次'
                            '（必須剛好 1 次）。第二次＝有人用 `doc["ota"]["…"] = …` '
                            '繞過 ota 這個區域變數再加欄位' % n_ota_key)

    print('禁用 API 靜態檢查：ho_master1.ino 非註釋行 %d 行；'
          'otaSessionBusy() 呼叫點 %d 處；文件必含 %d 條；'
          '設定唯一 %d 條；順序不變量 3 條（皆須早於 otaTls->connect()）'
          % (len(code_lines), n_busy, len(DOC_MUST_CONTAIN), len(EXACT_ONCE_IN_MASTER)))
    print('終局階段（方向 16）：otaPhaseIsFinal() 解析出 %s，'
          '每個都回頭比對 otaPhaseName() 與「停留 30 秒再回 idle」的 case 群組；'
          'otaSlaveVerified = true 指派 %d 處（方向 17）'
          % (final_phases or '解析失敗', n_verified))
    print('代發 updating（方向 18）：isOtaTarget 整段字面 ＋ 賦值 %d 處 ＋ '
          'publishSlaveStatus() 內 doc["status"] %s／"updating" %s ＋ 列舉範圍 %s；'
          'ota 上界前提（方向 19）：otaSetTarget() 整段字面 ＋ '
          'otaTargetId 寫入點 %d 處（限 otaSetTarget()／fakeOtaForCapacityTest()） ＋ '
          '兩個陣列宣告 ＋ phase≤12／error≤16 共 %d 個字面'
          % (n_isota_assign,
             locals().get('n_set_status', '未量'), locals().get('n_updating', '未量'),
             ('%s（值 %s..%s）' % (locals().get('got', '解析失敗'),
                                 locals().get('enum_val', {}).get('OTA_BEGIN_SENT', '?'),
                                 locals().get('enum_val', {}).get('OTA_VERIFYING', '?'))),
             n_id_write,
             len(re.findall(r'"([^"]*)"', locals().get('pn_code', ''))) + len(err_lits)))

    print('HIT 檢查 %d 項、BANNED 樣式 %d 條 × 檔案 %d 份（含原始碼註釋）；'
          'PLAN 樣式 %d 條 × %d 份'
          % (len(HIT_IN_SOURCE), len(BANNED_IN_DOCS), len(BANNED_SCAN_FILES),
             len(BANNED_IN_PLAN), len(PLAN_FILES)))

    if failures:
        print('\n%d 項失敗：' % len(failures))
        for f in failures:
            print('  ' + f)
        return 1
    print('\nALL CHECKS PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
