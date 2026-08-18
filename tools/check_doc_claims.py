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

六個驗證方向：
  1. HIT     —— 文件引用的判準字串必須逐字出現在原始碼裡
  2. BANNED  —— 被取代的舊值不得再以「現行事實」的樣子出現（**含原始碼註釋**）
  3. 行號    —— `檔名.ino:123` 這種對照一律禁止，判準要用字串錨點
  4. 項數    —— 數原始碼的 check() 呼叫，回頭比對文件寫死的「執行 N 項」
  5. 算式    —— 任何 `(A-1-B-C)/D = [E/D =] F` 的容量算式，逐項對原始碼常數複算
  6. 數值    —— 單獨寫出來的 `maxEntries` N 必須等於算出來的值

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
    print('HIT 檢查 %d 項、BANNED 樣式 %d 條 × 檔案 %d 份（含原始碼註釋）'
          % (len(HIT_IN_SOURCE), len(BANNED_IN_DOCS), len(BANNED_SCAN_FILES)))

    if failures:
        print('\n%d 項失敗：' % len(failures))
        for f in failures:
            print('  ' + f)
        return 1
    print('\nALL CHECKS PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
