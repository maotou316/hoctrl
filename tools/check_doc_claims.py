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

**這支腳本擋不住什麼**（必須連著讀）：
  - 它只做**字串比對**。字串對不代表語義對、更不代表數字算對
    —— A 族第 7 次就是「行號對、字串對，但列舉值抄錯」。
  - 它只檢查下面 HIT/BANNED 兩張表**列出來的**項目。
    沒被列進表裡的判準，這支腳本一個字都不會驗。
    **加了新判準就必須同時加進表裡**，否則覆蓋率會靜靜地退化。
  - 它驗不到時序、順序、以及任何跨板行為。
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
HISTORY_MARK = r'(之前|原本|由 |舊|沿革|曾經|當時|Task 7 時|Phase 2b 的)'

BANNED_IN_DOCS = [
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
]


def main():
    src = '\n'.join(read(f) for f in SRC_FILES)
    flat = flatten_c_strings(src)

    failures = []

    for name, needle in HIT_IN_SOURCE:
        if needle not in flat:
            failures.append('[HIT 缺] %s：原始碼裡找不到 %r' % (name, needle))

    for doc in DOC_FILES:
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
    print('HIT 檢查 %d 項、BANNED 樣式 %d 條 × 文件 %d 份'
          % (len(HIT_IN_SOURCE), len(BANNED_IN_DOCS), len(DOC_FILES)))

    if failures:
        print('\n%d 項失敗：' % len(failures))
        for f in failures:
            print('  ' + f)
        return 1
    print('\nALL CHECKS PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
