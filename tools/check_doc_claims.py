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

**十三個**驗證方向（第 7 個 PLAN 方向見下方 BANNED_IN_PLAN）。
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
  9. LR 掛鉤 —— `otaSessionBusy()` 的呼叫點數量必須仍是 2
                （readme 宣稱「LR 互斥守衛現在並不存在」，這條讓那句話會過期時吵）
 10. 文件必含 —— 「宣告某道保護不存在」的句子本身不得被刪（方向 9 的另一半）
 11. 假綠燈  —— `otaFinish()` 的函式本體不得出現「已更新到」
 12. 設定唯一 —— 關鍵 timeout／重定向設定必須**剛好出現一次**且逐字如此
                （HIT 擋不住「再加一行覆蓋它」，也擋不住「整行被刪掉」）
 13. 順序    —— `otaHttp->begin()` 必須排在 `otaTls->connect()` 之前（C5）

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
]

# ── 方向 3：協定測試的項數必須與文件的判準一致 ──
# 文件寫死「執行 N 項」當判準，程式卻是執行期累加 —— 兩者靠人工同步過一次就會漂。
EXPECTED_TEST_COUNT_DOC = 'docs/phase4-flag-day-upgrade.md'

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
    doc = read(EXPECTED_TEST_COUNT_DOC)
    doc_counts = set(re.findall(r'執行 (\d+) 項，失敗 0 項', doc))
    doc_counts |= set(re.findall(r'項數\*\*不是 (\d+)\*\*', doc))
    doc_counts |= set(re.findall(r'這 (\d+) 項全部是', doc))
    bad_counts = {c for c in doc_counts if int(c) != n_checks}
    if bad_counts:
        failures.append('[項數] 原始碼有 %d 個 check()，但 %s 寫成 %s'
                        % (n_checks, EXPECTED_TEST_COUNT_DOC, sorted(bad_counts)))
    print('協定測試項數：原始碼 %d、文件 %s' % (n_checks, sorted(doc_counts)))

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
    # ho_master1/readme.md 白紙黑字寫著「除了 otaStart() 自己的重入檢查以外
    # **沒有任何呼叫端**，所以『OTA 進行中拒絕切 LR』這道守衛現在並不存在」。
    # 那句話會在 Phase 5 有人補上守衛的當天變成假的 —— 而假的方向剛好是
    # 「文件低估了保護」→ 下一個人以為還沒做、又做一次；或反過來被當成已經做了。
    # 所以把「呼叫點數量」釘住：定義 1 處 + otaStart() 內 1 處 = 2。
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
    if n_busy != 2:
        failures.append('[LR 掛鉤] otaSessionBusy() 的非註釋出現次數是 %d（期望 2：'
                        '定義 1 處 + otaStart() 重入檢查 1 處）。'
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

    # ── 方向 13：C5 的順序不變量（begin() 必須排在 TLS 握手之前）──
    # HTTPClient.cpp:283 的 beginInternal 尾端：
    #   `if (_host != the_host && connected()) { _canReuse = false; disconnect(true); }`
    # 新 HTTPClient 的 _host 是空字串，所以「先握手再 begin()」會**當場把連線拆掉**，
    # 接著 GET() 自己重連 —— C3 消除的那次 DNS 會整條回來，而且多付一次握手。
    # 這個順序沒有任何編譯期或執行期訊號，只能靠檢查釘住。
    #
    # **它擋不住什麼**：只比對這兩行在檔案裡的先後位置，不理解控制流。
    # 把 begin() 搬到另一個函式、或用別的方式重連，它都抓不到。
    src_m = master_src
    i_begin = src_m.find('if (!otaHttp->begin(*otaTls, otaUrl)) {')
    i_conn = src_m.find('int cres = otaTls->connect(otaHostIp')
    if i_begin < 0 or i_conn < 0:
        failures.append('[順序] 找不到 begin() 或 otaTls->connect() 的錨點，方向 13 無法檢查')
    elif i_begin > i_conn:
        failures.append('[順序] otaHttp->begin() 排在 otaTls->connect() **之後**：'
                        'beginInternal 會因為 _host 不同而 disconnect(true) 把剛握好的 '
                        'TLS 連線拆掉，GET() 會自己重連並重新做一次 DNS（C5／A 族第 17 次）')

    print('禁用 API 靜態檢查：ho_master1.ino 非註釋行 %d 行；'
          'otaSessionBusy() 呼叫點 %d 處；文件必含 %d 條；'
          '設定唯一 %d 條；順序不變量 1 條'
          % (len(code_lines), n_busy, len(DOC_MUST_CONTAIN), len(EXACT_ONCE_IN_MASTER)))

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
