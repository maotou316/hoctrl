# -*- coding: utf-8 -*-
"""`ho_master1` OTA **轉送引擎**的邏輯轉寫模擬 —— 對接 `ota_slave_sim.py` 的 slave 轉寫。

用法（在 repo 根目錄）：
    python tools/ota_relay_sim.py
全部通過印 ALL CHECKS PASSED，任何一項失敗以 exit code 1 結束。

## 這支腳本存在的理由

`tools/ota_slave_sim.py` 只驗 slave 那一半。Task 4 把 master 那一半寫出來之後，
**兩半不對稱本身就是缺口**：slave 的轉寫有錨點在擋漂移，master 的沒有，
於是「兩端語義接得上」這句話沒有任何可重跑的證據。

本腳本沿用 slave 模擬那三道自我約束，並依 Task 4 複審的要求加上**第四道（順序錨點）**：

- **方向 A（轉寫錨點）**：轉寫的每一條判斷式都以逐字字串登記在 `TRANSCRIBED_FROM_INO`，
  必須能在 `ho_master1.ino` 命中。改韌體沒改模擬 → 當場變紅。
- **方向 A2（順序錨點）**：`ORDER_IN_INO` 驗「誰必須排在誰前面」。
  **這一維度是 Task 4 複審用實測打進來的**：它自己想的加寬突變 W1
  「四個逐字錨點一字不改、只把成功出口搬到守衛之前」**在只驗存在性的規則下完整存活**。
  位置就是最大的破口，字串比對驗不到它，所以要單獨列。
- **方向 B（協定常數不得寫死）**：`HO_OTA_CHUNK_SIZE` 之類一律從
  `libraries/HoEspNow/src/HoEspNowProtocol.h` 與 `ho_master1.ino` **解析出來**。
  寫死等於再開一個「常數改了但模擬沒跟上」的缺口。找不到就直接失敗，不容許預設值。
- **方向 C（分支覆蓋）**：每一個 master 分支至少要有一個**加寬方向**的突變，
  而且每個突變都必須至少殺掉一條情境。

## 這份模擬擋不住什麼（必須連著讀）

- **它完全驗不到併發，而 Task 4 最嚴重的缺陷（CR1）正是併發的**：
  `onEspNowSent()` 跑在 WiFi task，把 MAC 層 ACK 交給 `groupNoteUnicastAck()`，
  而前一輪排進佇列、尚未完成的 `OTA_DATA` 回呼會落在本輪剛開的歸因閂裡。
  這裡是單執行緒依序呼叫，**那一整類問題原理上不可能在這裡現形**。
  CR1 的守衛只有 `tools/check_doc_claims.py` 的方向 15（靜態、字串層）在看著，
  而那也只驗「送出點有沒有全部收斂」，不驗執行期。
- 它是**轉寫**，不是韌體。方向 A／A2 只保證「被登記的那些判斷式與先後沒漂」，
  沒登記的行為一個字都沒驗。
- 沒有真的無線層：丟包在這裡是獨立同分布，現場是叢發的；沒有 MAC 層重傳、
  沒有通道忙碌、沒有 20 台同時在線的碰撞。
- 沒有 flash：`Update.write()` 每跨 64 KB 的 60~190 ms 抹除窗口不在模型內，
  而那正是目標 slave 最容易靜默漏包的時候。
- 沒有時間：`loop()` 在這裡是固定步長，真板子上會被 MQTT／WiFi 的阻塞呼叫拉長。
  **所有涉及時序與並行的結論都是靜態推演、沒有上界保證。**
"""
import hashlib
import io
import os
import random
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ota_slave_sim as S   # noqa: E402  slave 那一半直接沿用，不重寫

MASTER_INO = os.path.join(ROOT, 'ho_master1', 'ho_master1.ino')
PROTO_H = os.path.join(ROOT, 'libraries', 'HoEspNow', 'src', 'HoEspNowProtocol.h')

FAILURES = []


def read(path):
    with io.open(path, encoding='utf-8') as f:
        return f.read()


MASTER_SRC = read(MASTER_INO)
PROTO_SRC = read(PROTO_H)


# ─────────────── 方向 B：協定與流控常數一律解析，不得寫死 ───────────────
def parse(src, pattern, label):
    m = re.search(pattern, src)
    if not m:
        FAILURES.append('[常數] 解析不到 %s（樣式 %r）—— 模擬不得自己填預設值'
                        % (label, pattern))
        return None
    return int(m.group(1))


CHUNK = parse(PROTO_SRC, r'#define\s+HO_OTA_CHUNK_SIZE\s+(\d+)', 'HO_OTA_CHUNK_SIZE')
WINDOW = parse(PROTO_SRC, r'#define\s+HO_OTA_WINDOW\s+(\d+)', 'HO_OTA_WINDOW')
OK = parse(PROTO_SRC, r'HO_OTA_OK\s*=\s*(\d+)', 'HO_OTA_OK')
READY = parse(PROTO_SRC, r'HO_OTA_READY\s*=\s*(\d+)', 'HO_OTA_READY')
ERR_SIZE = parse(PROTO_SRC, r'HO_OTA_ERR_SIZE\s*=\s*(\d+)', 'HO_OTA_ERR_SIZE')

ACK_TIMEOUT = parse(MASTER_SRC, r'OTA_ACK_TIMEOUT_MS\s*=\s*(\d+)', 'OTA_ACK_TIMEOUT_MS')
POLL_MAX = parse(MASTER_SRC, r'OTA_POLL_MAX\s*=\s*(\d+)', 'OTA_POLL_MAX')
BLOCK_MAX_RETRY = parse(MASTER_SRC, r'OTA_BLOCK_MAX_RETRY\s*=\s*(\d+)', 'OTA_BLOCK_MAX_RETRY')
BEGIN_MAX_TRY = parse(MASTER_SRC, r'OTA_BEGIN_MAX_TRY\s*=\s*(\d+)', 'OTA_BEGIN_MAX_TRY')
SEND_PER_LOOP = parse(MASTER_SRC, r'OTA_SEND_PER_LOOP\s*=\s*(\d+)', 'OTA_SEND_PER_LOOP')

if FAILURES:
    for f in FAILURES:
        print(f)
    sys.exit(1)

# 這幾個在韌體裡是行內字面值（不是具名常數），所以連同它們所在的整行一起登記在
# 方向 A 的錨點表裡 —— 改了數字，錨點就對不上。
BEGIN_RETRY_MS = 3000
END_WAIT_MS = 10000
VERIFY_MAX_MS = 90000


# ─────────────── 方向 A：轉寫錨點（改韌體沒改模擬就變紅）───────────────
TRANSCRIBED_FROM_INO = [
    ('STAGED：otadl 的 stageOnly 原地收尾，不接觸任何 slave',
     'if (otaStageOnly) {'),
    ('STAGED：BEGIN 走統一送出點（CR1）',
     'otaTxToTarget(HO_PKT_OTA_BEGIN, &bg, sizeof(bg), true);'),
    ('STAGED：每次進來都把區塊狀態歸零、從第 0 塊開始',
     '      otaBlockBase  = 0;\n      otaSendMask   = 0;'),
    ('BEGIN_SENT：只有 READY 才開始轉送',
     'if (otaAckStatus == HO_OTA_READY) {\n          Serial.println("[OTA] slave 已就緒，開始轉送");'),
    ('BEGIN_SENT：重送次數用 otaBeginAttempt，不與 otaBlockRetry 共用',
     'if (otaBeginAttempt >= OTA_BEGIN_MAX_TRY) {'),
    ('BEGIN_SENT：3 秒逾時重送',
     'if (now - otaPhaseStart >= 3000) {'),
    ('RELAYING：每輪固定配額，佇列滿就 break（背壓即流控）',
     'for (uint16_t i = 0; i < HO_OTA_WINDOW && sent < OTA_SEND_PER_LOOP; i++) {'),
    ('RELAYING：送不出去就這輪不送，下一輪再試',
     'if (!otaSendChunk(otaBlockBase + i)) break;   // NO_MEM：背壓，下一輪再送'),
    ('RELAYING：整塊送完才送查詢',
     'if (otaSendMask == 0) {'),
    ('WAIT_ACK：落後的 ACK 忽略並繼續等',
     'if (otaAckBase < otaBlockBase) return;'),
    ('WAIT_ACK：slave 已推進就跟上',
     'if (otaAckBase > otaBlockBase) {'),
    ('WAIT_ACK：收齊才推進',
     'if ((otaAckBits & full) == full) {'),
    ('WAIT_ACK：缺包只補送缺的那幾包',
     'otaSendMask = (uint16_t)(full & ~otaAckBits);'),
    ('WAIT_ACK：先重發查詢，查詢用完才整塊重送',
     'if (otaPollCount < OTA_POLL_MAX) {'),
    ('WAIT_ACK：缺包重送用完就中止並叫 slave 丟掉',
     '        if (otaBlockRetry > OTA_BLOCK_MAX_RETRY) {\n          Serial.printf("[OTA] 區塊 %u 重試 %d 次仍失敗'),
    ('WAIT_ACK：查詢也用完之後的整塊重送上限',
     '      if (otaBlockRetry > OTA_BLOCK_MAX_RETRY) {\n        Serial.printf("[OTA] 區塊 %u 查詢 %d 次無回應'),
    ('END_SENT：收到 READY 要繼續等，不得判成失敗（假紅燈）',
     'if (otaAckStatus == HO_OTA_READY) {\n          return;   // 進度回報，不是結果'),
    ('END_SENT：成功判定連 blockBase 與 mask 一起檢查（假綠燈）',
     'otaAckBase == otaTotalChunks && otaAckBits == 0xFFFF'),
    ('END_SENT：10 秒沒結果不判失敗，改落到版本回檢',
     'if (now - otaPhaseStart >= 10000) {'),
    ('VERIFYING：(b) 用 MAC 重查索引，不得沿用會過期的 otaTargetIdx',
     'int vIdx = findSlave(otaTargetMac);'),
    ('VERIFYING：(a) 目標版本不是 0.0.0',
     'if (otaHasVersion && vIdx >= 0 &&\n          slaves[vIdx].online &&'),
    ('VERIFYING：(c) lastSeen 晚於進入 VERIFYING 的時刻',
     '(long)(slaves[vIdx].lastSeen - otaPhaseStart) > 0 &&'),
    ('VERIFYING：版本三段相等（「版本回檢」四個字的全部內容）',
     'slaves[vIdx].fwPatch == otaVerPatch) {'),
    ('VERIFYING：90 秒沒等到就 no_return',
     'if (now - otaPhaseStart >= 90000) {'),
    ('CR1：所有送給目標的單播都經由 otaTxToTarget()（它蓋時間戳）',
     'otaUnicastAt = millis();'),
    ('CR1：群組歸因守衛',
     'if (otaUnicastRecently(desAddr)) {'),
    ('群組指令期間轉送整個讓開（plan 決定 1(b)）',
     'if (groupCmdActive()) {\n      if (otaGroupPauseSince == 0) {'),
    # ── 這一輪複審修的四項，原本**兩支腳本都沒有任何錨點守著**（A 族第 20 次）──
    # 複審實測：MJ3／MJ6 的計時器順延／MJ7／otaEndQueued 全部可以原樣改回去而不會被發現。
    ('MJ6：讓開期間三個計時器都要順延（少一個就會被吃掉重試額度）',
     '      otaPhaseStart += paused;\n      otaWaitStart += paused;\n      otaSessionStart += paused;'),
    ('MJ7：OTA_VERIFYING 不受 5 分鐘總上限管',
     'if (!otaPhaseIsFinal() && otaPhase != OTA_VERIFYING &&'),
    # ── Task 5：本模擬的 'SUCCESS_STAGED' 之所以是一個**獨立的**終局階段，
    #    對應的是韌體這一行。改回 otaFinish() 就等於 otadl 也報 "success"，
    #    而那條路徑一台 slave 都沒碰過（Task 3 的 MJ2）。
    #    位置與呼叫點數量由 tools/check_doc_claims.py 的方向 16 另外釘住。
    ('STAGED：stageOnly 走自己的收尾，不與版本回檢成功共用 "success"',
     'otaFinishStagedOnly();'),
    ('MJ3：心跳只在成功進佇列時才推進計時器',
     'if (sendHeartbeat()) lastBeat = now;'),
    ('END_SENT：OTA_END 沒進佇列要每輪重試（只重試「沒離開 master」那一種）',
     'if (!otaEndQueued) {\n        otaEndQueued = otaSendEnd(0);'),
    # ── N1：程式碼自己指名的危險方向 ──
    # 「拿偏早的時刻當 OTA_VERIFYING 的起點是往假綠燈的方向」這句話就寫在同一個位置，
    # 而把 millis() 換成 otaSessionStart 之前**兩支腳本全綠、前提 (c) 整條失效**。
    ('N1：VERIFYING 的起點必須是 millis()（成功路徑）',
     '          otaPhase = OTA_VERIFYING;\n          // **這裡用 millis() 而不是 now**'),
    ('N1：VERIFYING 的起點必須是 millis()（逾時落下來那條）',
     '        otaPhase = OTA_VERIFYING;\n        otaPhaseStart = millis();\n      }\n      return;'),
]

# ─────────────── 方向 A2：順序錨點（W1 證明位置就是最大破口）───────────────
# (說明, 必須在前面的字串, 必須在後面的字串[, 限定區塊的起訖錨點])
# **限定區塊是必要的**：otaFinish() 在 OTA_STAGED 與 OTA_VERIFYING 各有一個呼叫點，
# 全檔 find() 會命中比較早的那一個，讓順序檢查驗到一組不相干的先後。
ORDER_IN_INO = [
    ('VERIFYING：三條前提必須排在「已更新到」之前 —— 只搬位置就能繞過整組前提',
     'if (otaHasVersion && vIdx >= 0 &&', '[OTA] 完成：%s 已更新到 %u.%u.%u',
     ('case OTA_VERIFYING: {', 'case OTA_SUCCESS:')),
    ('VERIFYING：版本三段比對必須排在成功訊息之前',
     'slaves[vIdx].fwPatch == otaVerPatch) {', '[OTA] 完成：%s 已更新到 %u.%u.%u',
     ('case OTA_VERIFYING: {', 'case OTA_SUCCESS:')),
    ('VERIFYING：成功訊息必須排在 otaFinish() 之前（先宣告、再收尾）',
     '[OTA] 完成：%s 已更新到 %u.%u.%u', 'otaFinish();',
     ('case OTA_VERIFYING: {', 'case OTA_SUCCESS:')),
    ('END_SENT：忽略 READY 那一條必須排在成功判定之前，否則晚到的查詢回覆會被當結果',
     'if (otaAckStatus == HO_OTA_READY) {\n          return;   // 進度回報',
     'otaAckBase == otaTotalChunks && otaAckBits == 0xFFFF'),
    ('WAIT_ACK：落後 ACK 的守衛必須排在「跟上」與「收齊」判斷之前',
     'if (otaAckBase < otaBlockBase) return;', 'if (otaAckBase > otaBlockBase) {'),
    ('CR1：歸因守衛必須排在寫入 groupDelivered[] 之前',
     'if (otaUnicastRecently(desAddr)) {', 'groupDelivered[i] = true;'),
    ('CR1：時間戳必須蓋在送出之前（回呼可能在 esp_now_send() 返回前就跑起來）',
     'otaUnicastAt = millis();', 'return espNowSendToEx(otaTargetMac, type, payload, len, verbose);'),
]


# ─────────────── 突變旗標 ───────────────
MUT = {
    # staged
    'wide_staged_relay_on_stageonly': False,
    # begin_sent
    'wide_begin_any_status_ok': False,
    'drop_begin_try_cap': False,
    # relaying
    'wide_relay_ignore_backpressure': False,
    # wait_block_ack
    'wide_wait_partial_advance': False,
    'drop_wait_stale_guard': False,
    # end_sent
    'wide_end_ready_is_success': False,
    'wide_end_status_only': False,
    'drop_end_queue_retry': False,
    # verifying
    'wide_verify_roster_only': False,      # ← 複審的 W1
    'wide_verify_timeout_green': False,    # ← 複審的 W2'
    'wide_verify_no_version_flag': False,
    'wide_verify_no_lastseen': False,
}

MUT_META = {
    'wide_staged_relay_on_stageonly': ('staged', 'widen'),
    'wide_begin_any_status_ok':       ('begin_sent', 'widen'),
    'drop_begin_try_cap':             ('begin_sent', 'remove'),
    'wide_relay_ignore_backpressure': ('relaying', 'widen'),
    'wide_wait_partial_advance':      ('wait_block_ack', 'widen'),
    'drop_wait_stale_guard':          ('wait_block_ack', 'remove'),
    'wide_end_ready_is_success':      ('end_sent', 'widen'),
    'wide_end_status_only':           ('end_sent', 'widen'),
    'drop_end_queue_retry':           ('end_sent', 'remove'),
    'wide_verify_roster_only':        ('verifying', 'widen'),
    'wide_verify_timeout_green':      ('verifying', 'widen'),
    'wide_verify_no_version_flag':    ('verifying', 'widen'),
    'wide_verify_no_lastseen':        ('verifying', 'widen'),
}
BRANCHES = ['staged', 'begin_sent', 'relaying', 'wait_block_ack', 'end_sent', 'verifying']

LOOP_MS = 5          # 一次 loop() 推進的時間
POLL_PERIOD = 15000  # pollNextSlave() 一輪問完全部的週期
BOOT_MS = 4000       # slave 重開機到能回應 STATE_REQ 的時間


class Chan(object):
    """可設丟包／背壓的通道。deliver＝下行，up＝ACK 回程，tx_ok＝送出佇列有沒有滿。

    `data_only=True` 時**只丟 OTA_DATA**。那不是為了模擬現場（現場不會挑封包種類丟），
    是為了讓「轉送一定會完成」成為情境的前提 —— 否則 OTA_END 只要掉一次，
    整條情境就變成合法的紅燈，任何「必須完成」的斷言都會變成薛丁格的斷言，
    **殺不掉任何突變**（方向 C 就是這樣把覆蓋不足逼出來的）。
    """

    def __init__(self, loss_dn=0.0, loss_up=0.0, nomem=0.0, seed=1, data_only=False):
        self.loss_dn, self.loss_up, self.nomem = loss_dn, loss_up, nomem
        self.data_only = data_only
        self.r = random.Random(seed)

    def deliver(self, kind='data'):
        if self.data_only and kind != 'data':
            return True
        return self.r.random() >= self.loss_dn

    def up(self):
        return self.r.random() >= self.loss_up

    def tx_ok(self):
        return self.r.random() >= self.nomem


class Master(object):
    """updateOtaSession() 的轉送半段（OTA_STAGED ~ OTA_VERIFYING）逐段轉寫。"""

    def __init__(self, slave, chan, fw, total_size, total_chunks, md5,
                 ver=(0, 0, 0), stage_only=False):
        self.sl = slave
        self.chan = chan
        self.fw = fw
        self.totalSize = total_size
        self.totalChunks = total_chunks
        self.md5 = md5
        self.ver = ver
        self.hasVersion = (ver != (0, 0, 0))
        self.stageOnly = stage_only

        self.phase = 'STAGED'
        self.session = 7
        self.blockBase = 0
        self.sendMask = 0
        self.ackMask = 0
        self.blockRetry = 0
        self.pollCount = 0
        self.beginAttempt = 0
        self.waitStart = 0
        self.phaseStart = 0
        self.ackPending = False
        self.ackBase = 0
        self.ackBits = 0
        self.ackStatus = 0
        self.err = None
        self.finished = False          # otaFinish()：印出「已更新到」的那條路
        self.touchedSlave = False      # 有沒有對 slave 送過任何一封
        self.sentChunks = 0
        self.sentPolls = 0
        self.endTimedOut = False
        self.endQueued = False
        # 名冊裡目標那一格（master 端 slaves[vIdx]）
        self.roster = {'online': False, 'lastSeen': 0, 'fw': (0, 0, 0), 'present': True}

    # ── otaBlockNeed() / otaFullMask() ──
    def blockNeed(self):
        if self.blockBase >= self.totalChunks:
            return 0
        need = WINDOW
        if self.blockBase + WINDOW > self.totalChunks:
            need = self.totalChunks - self.blockBase
        return need

    def fullMask(self):
        need = self.blockNeed()
        return 0xFFFF if need >= 16 else ((1 << need) - 1)

    # ── otaTxToTarget()：所有送給目標的單播都經由這裡 ──
    def _tx(self, now, fn, kind='data'):
        self.touchedSlave = True
        if not self.chan.tx_ok():
            return False
        if self.chan.deliver(kind):
            fn(now)
        return True

    def sendChunk(self, now, idx):
        off = idx * CHUNK
        if off >= self.totalSize:
            return False
        remain = self.totalSize - off
        dataLen = remain if remain < CHUNK else CHUNK
        payload = self.fw[off:off + dataLen]
        ok = self._tx(now, lambda t: self.sl.on_data(t, self.session, idx, payload))
        if ok:
            self.sentChunks += 1
        return ok

    def sendPoll(self, now):
        self.sentPolls += 1
        self._tx(now, lambda t: self.sl.on_query(t, self.session, self.blockBase),
                 kind='poll')

    def sendEnd(self, now, abort):
        # 回傳「有沒有進到本機的送出佇列」——**只有這一種失敗可以重試**：
        # 封包根本沒離開 master，slave 不可能已經校驗通過。
        return self._tx(now, lambda t: self.sl.on_end(t, self.session, abort,
                                                      self.totalSize), kind='end')

    def fail(self, code):
        self.err = code
        self.phase = 'FAILED'

    # ── onEspNowRecv() 的 OTA_ACK 分支：只搬旗標（pending 最後才設）──
    def take_ack(self, base, bits, status, session):
        if session != self.session:
            return
        self.ackBase, self.ackBits, self.ackStatus = base, bits, status
        self.ackPending = True

    # ── updateOtaSession() 的一輪 ──
    def step(self, now):
        p = self.phase

        if p == 'STAGED':
            if self.stageOnly and not MUT['wide_staged_relay_on_stageonly']:
                self.phase = 'SUCCESS_STAGED'
                return
            self._tx(now, lambda t: self.sl.on_begin(t, self.session, self.totalSize,
                                                     self.totalChunks, self.md5),
                     kind='begin')
            self.beginAttempt += 1
            self.blockBase = 0
            self.sendMask = 0
            self.ackMask = 0
            self.blockRetry = 0
            self.pollCount = 0
            self.ackPending = False
            self.phase = 'BEGIN_SENT'
            self.phaseStart = now
            return

        if p == 'BEGIN_SENT':
            if self.ackPending:
                self.ackPending = False
                if self.ackStatus == READY or MUT['wide_begin_any_status_ok']:
                    self.sendMask = self.fullMask()
                    self.ackMask = 0
                    self.blockRetry = 0
                    self.pollCount = 0
                    self.phase = 'RELAYING'
                    self.phaseStart = now
                else:
                    self.fail('slave_reject')
                return
            if now - self.phaseStart >= BEGIN_RETRY_MS:
                if self.beginAttempt >= BEGIN_MAX_TRY and not MUT['drop_begin_try_cap']:
                    self.fail('slave_timeout')
                    return
                self.phase = 'STAGED'
            return

        if p == 'RELAYING':
            sent = 0
            i = 0
            while i < WINDOW and sent < SEND_PER_LOOP:
                if (self.sendMask & (1 << i)) == 0:
                    i += 1
                    continue
                if not self.sendChunk(now, self.blockBase + i):
                    if not MUT['wide_relay_ignore_backpressure']:
                        break
                self.sendMask &= ~(1 << i) & 0xFFFF
                sent += 1
                i += 1
            if self.sendMask == 0:
                self.sendPoll(now)
                self.pollCount = 1
                self.waitStart = now
                self.phase = 'WAIT_BLOCK_ACK'
            return

        if p == 'WAIT_BLOCK_ACK':
            if self.ackPending:
                self.ackPending = False
                full = self.fullMask()
                if self.ackBase < self.blockBase and not MUT['drop_wait_stale_guard']:
                    return
                if self.ackBase > self.blockBase:
                    self.blockBase = self.ackBase
                    self.blockRetry = 0
                    self.pollCount = 0
                    if self.blockBase >= self.totalChunks:
                        self._to_end(now)
                        return
                    self.sendMask = self.fullMask()
                    self.phase = 'RELAYING'
                    return
                enough = ((self.ackBits & full) == full)
                if MUT['wide_wait_partial_advance']:
                    enough = (self.ackBits != 0)
                if self.ackBase == self.blockBase and enough:
                    self.ackMask = self.ackBits
                    self.blockBase += self.blockNeed()
                    self.blockRetry = 0
                    self.pollCount = 0
                    if self.blockBase >= self.totalChunks:
                        self._to_end(now)
                        return
                    self.sendMask = self.fullMask()
                    self.phase = 'RELAYING'
                    return
                self.ackMask = self.ackBits
                self.sendMask = full & ~self.ackBits & 0xFFFF
                self.blockRetry += 1
                if self.blockRetry > BLOCK_MAX_RETRY:
                    self.sendEnd(now, 1)
                    self.fail('espnow_fail')
                    return
                self.pollCount = 0
                self.phase = 'RELAYING'
                return

            if now - self.waitStart < ACK_TIMEOUT:
                return
            if self.pollCount < POLL_MAX:
                self.pollCount += 1
                self.sendPoll(now)
                self.waitStart = now
                return
            self.blockRetry += 1
            if self.blockRetry > BLOCK_MAX_RETRY:
                self.sendEnd(now, 1)
                self.fail('espnow_fail')
                return
            self.sendMask = self.fullMask()
            self.pollCount = 0
            self.phase = 'RELAYING'
            return

        if p == 'END_SENT':
            if not self.endQueued and not MUT['drop_end_queue_retry']:
                self.endQueued = self.sendEnd(now, 0)
                return
            if self.ackPending:
                self.ackPending = False
                if self.ackStatus == READY and not MUT['wide_end_ready_is_success']:
                    return
                good = (self.ackStatus == OK and self.ackBase == self.totalChunks
                        and self.ackBits == 0xFFFF)
                if MUT['wide_end_status_only']:
                    good = (self.ackStatus == OK)
                if MUT['wide_end_ready_is_success'] and self.ackStatus == READY:
                    good = True
                if good:
                    self.phase = 'VERIFYING'
                    self.phaseStart = now
                else:
                    self.fail('md5_mismatch')
                return
            if now - self.phaseStart >= END_WAIT_MS:
                self.endTimedOut = True
                self.phase = 'VERIFYING'
                self.phaseStart = now
            return

        if p == 'VERIFYING':
            vIdx = 0 if self.roster['present'] else -1
            if MUT['wide_verify_roster_only']:
                # W1：只把成功出口搬到守衛之前 —— 只要目標還在名冊上就宣告已更新
                if vIdx >= 0:
                    self.finished = True
                    self.phase = 'SUCCESS'
                    return
            ver_ok = (self.roster['fw'] == self.ver)
            if MUT['wide_verify_timeout_green']:
                # W2'：三條前提原封不動，版本相等換成「三段相符 或 已等 60 秒」
                ver_ok = ver_ok or (now - self.phaseStart >= 60000)
            has_ver = self.hasVersion or MUT['wide_verify_no_version_flag']
            seen_ok = ((self.roster['lastSeen'] - self.phaseStart) > 0
                       or MUT['wide_verify_no_lastseen'])
            if has_ver and vIdx >= 0 and self.roster['online'] and seen_ok and ver_ok:
                self.finished = True
                self.phase = 'SUCCESS'
                return
            if now - self.phaseStart >= VERIFY_MAX_MS:
                self.fail('no_return')
            return

    def _to_end(self, now):
        self.endQueued = self.sendEnd(now, 0)
        self.phase = 'END_SENT'
        self.phaseStart = now


# ─────────────── 情境 ───────────────
FW_SIZE = S.FW_SIZE
FW = S.FW
FW_MD5 = S.FW_MD5
FW_CHUNKS = (FW_SIZE + CHUNK - 1) // CHUNK
OLD_VER = (1, 0, 0)
NEW_VER = (1, 2, 3)


class World(object):
    """master ＋ slave ＋ 通道 ＋ 「重開機後由 pollNextSlave() 問到它」的模型。"""

    def __init__(self, chan, ver=NEW_VER, stage_only=False, silent=False,
                 reboot_to=None, roster_present=True, prereport=None,
                 ack_delay=0, corrupt=False, drop_status_ack=False,
                 repair_during_verify=False):
        self.chan = chan
        self.sl = S.Slave()
        self.sl.lastHeartbeatTime = 0
        # corrupt：master 送出的位元組與它在 OTA_BEGIN 宣告的 MD5 不一致
        #（模擬暫存區被寫壞／下載到別的檔）。slave 的 Update.end(true) 只驗 MD5，
        # 所以這是「校驗一定會失敗」的情境。
        fw = FW if not corrupt else (FW[:1000] + bytes([FW[1000] ^ 0xFF]) + FW[1001:])
        self.m = Master(self.sl, chan, fw, FW_SIZE, FW_CHUNKS, FW_MD5,
                        ver=ver, stage_only=stage_only)
        self.ack_delay = ack_delay
        self.drop_status_ack = drop_status_ack
        self.repair_during_verify = repair_during_verify
        self.repaired = False
        self.pending_acks = []
        self.m.roster['present'] = roster_present
        self.silent = silent
        # slave 重開機後跑起來的版本。None ＝ 它根本沒回線
        self.reboot_to = reboot_to
        self.slave_ver = OLD_VER
        self.rebooted_at = None
        self.seen = 0
        self.now = 1000
        # 工作階段開始「之前」就有的一份狀態回報（用來驗前提 (c)）
        if prereport is not None:
            self.m.roster['online'] = True
            self.m.roster['lastSeen'] = self.now
            self.m.roster['fw'] = prereport

    def run(self, max_steps=400000, inject_late_ready=False, inject_fake_ok=False):
        injected = False
        fake_injected = False
        last_poll = 0
        for _ in range(max_steps):
            self.now += LOOP_MS
            self.m.step(self.now)

            # slave 校驗通過 → 重開機
            if self.sl.bootSwitched and self.rebooted_at is None:
                self.rebooted_at = self.now
                self.slave_ver = self.reboot_to

            # pollNextSlave()：VERIFYING 期間**照常輪詢目標**（C6），
            # 這是 lastSeen 唯一的前進來源（slave 開機不會自己送狀態）
            if self.now - last_poll >= POLL_PERIOD:
                last_poll = self.now
                alive = (self.rebooted_at is None or
                         (self.now - self.rebooted_at) >= BOOT_MS)
                if (self.m.roster['present'] and alive and self.slave_ver is not None
                        and self.chan.deliver() and self.chan.up()):
                    self.m.roster['online'] = True
                    self.m.roster['lastSeen'] = self.now
                    self.m.roster['fw'] = self.slave_ver

            while self.seen < len(self.sl.acks):
                base, mask, status, sess = self.sl.acks[self.seen]
                self.seen += 1
                if self.silent:
                    continue
                # drop_status_ack：把「校驗結果」那一封（OK／ERR_MD5）丟掉，
                # 只讓進度回報通得過 —— 用來製造「END_SENT 只收到 READY」。
                if self.drop_status_ack and status != READY:
                    continue
                if not self.chan.up():
                    continue
                if self.ack_delay > 0:
                    # **每封各自抖動**：固定延遲等於整體平移，兩封 ACK 仍會在同一輪
                    # 一起到達，而單槽旗標會讓後一封覆寫前一封 ——
                    # 「落後的 ACK」那個情境就永遠構造不出來（方向 C 逼出來的）。
                    jitter = self.chan.r.randint(0, self.ack_delay)
                    self.pending_acks.append((self.now + jitter,
                                              base, mask, status, sess))
                else:
                    self.m.take_ack(base, mask, status, sess)
            if self.pending_acks:
                due = [a for a in self.pending_acks if a[0] <= self.now]
                self.pending_acks = [a for a in self.pending_acks if a[0] > self.now]
                for _t, base, mask, status, sess in due:
                    self.m.take_ack(base, mask, status, sess)

            if inject_late_ready and self.m.phase == 'END_SENT' and not injected:
                injected = True
                self.m.take_ack(0, 0, READY, self.m.session)
            # 注入一封 (0, 0, OK) —— **原始設計下查詢回覆長的就是這個樣子**，
            # 與「整份校驗通過」逐位元組相同。Task 2 已在產生端消滅它，
            # 這裡注入是為了驗 master 端那道第二層（連 blockBase／mask 一起檢查）。
            if inject_fake_ok and self.m.phase == 'END_SENT' and not fake_injected:
                fake_injected = True
                self.m.take_ack(0, 0, OK, self.m.session)
            # VERIFYING 期間目標重新配對：addSlave() 會把 online 設為 true、
            # lastSeen 設成 millis()、而 **fw 三段全是 0**（已回去讀過 addSlave()）。
            # 那正好讓前提 (b)(c) 成立而版本欄位是 0.0.0 —— 唯一擋住它的是前提 (a)。
            if self.repair_during_verify and self.m.phase == 'VERIFYING' and not self.repaired:
                # **必須晚於 phaseStart**：同一毫秒不算（韌體的條件是
                # `(long)(lastSeen - otaPhaseStart) > 0`，嚴格大於）。
                # 轉送本身只花約 1.2 秒，不隔開的話重新配對會剛好落在同一刻，
                # 於是擋住它的是前提 (c) 而不是 (a)，這條情境就驗不到它想驗的東西。
                if self.now - self.m.phaseStart < 2000:
                    continue
                self.repaired = True
                self.m.roster['online'] = True
                self.m.roster['lastSeen'] = self.now
                self.m.roster['fw'] = (0, 0, 0)

            if self.m.phase in ('SUCCESS', 'FAILED', 'SUCCESS_STAGED'):
                break
        return self.m, self.sl


def scen_clean():
    w = World(Chan(), reboot_to=NEW_VER)
    m, sl = w.run()
    assert m.phase == 'SUCCESS', '乾淨通道應該走到成功，實際 %s/%s' % (m.phase, m.err)
    assert m.finished, '應該印出「已更新到」'
    assert bytes(sl.flash) == FW, '位元組流必須逐位元組相符'
    assert m.sentChunks == FW_CHUNKS, '乾淨通道不該有重傳'
    assert m.roster['lastSeen'] > 0


def scen_lossy_down():
    for seed in range(2, 8):
        w = World(Chan(loss_dn=0.10, seed=seed), reboot_to=NEW_VER)
        m, sl = w.run()
        if sl.otaWritten == FW_SIZE:
            assert bytes(sl.flash) == FW, 'seed%d 位元組流錯了' % seed
        if sl.bootSwitched:
            assert m.phase != 'FAILED', \
                'seed%d：slave 已切換分區，master 卻判失敗（假紅燈）%s' % (seed, m.err)
        assert (not sl.bootSwitched) or bytes(sl.flash) == FW, \
            'seed%d：位元組流不對卻切換了分區（假綠燈）' % seed


def scen_lossy_up():
    w = World(Chan(loss_up=0.30, seed=3), reboot_to=NEW_VER)
    m, sl = w.run()
    assert m.phase == 'SUCCESS', 'ACK 回程 30%% 丟包仍應完成，實際 %s/%s' % (m.phase, m.err)
    assert m.sentPolls > (FW_CHUNKS + WINDOW - 1) // WINDOW, '應該有重發查詢'
    assert m.sentChunks < FW_CHUNKS * 1.05, '查詢那一層應該擋住大部分整塊重送'


def scen_backpressure():
    """送出佇列 20%% 滿。**這條情境抓出了一個真的缺陷**：OTA_END 的入佇列失敗
    原本沒有任何重試路徑，於是整場轉送白做、90 秒後 no_return。"""
    for seed in (4, 11, 12, 13):
        w = World(Chan(nomem=0.20, seed=seed), reboot_to=NEW_VER)
        m, sl = w.run()
        assert m.phase == 'SUCCESS', \
            'seed%d：背壓 20%% 仍應完成，實際 %s/%s' % (seed, m.phase, m.err)
        assert bytes(sl.flash) == FW
        # 把「沒送出去」當成「送出去了」會讓每一塊都要多跑幾輪查詢才補齊 ——
        # 用查詢封數當判準（1 塊本來只要 1 封查詢）。
        blocks = (FW_CHUNKS + WINDOW - 1) // WINDOW
        assert m.sentPolls <= blocks * 2, \
            'seed%d：查詢 %d 封／區塊 %d 塊，背壓被當成已送出了' % (seed, m.sentPolls, blocks)


def scen_silent_slave():
    w = World(Chan(), silent=True, reboot_to=None)
    m, sl = w.run()
    assert m.err == 'slave_timeout', '應報 slave_timeout，實際 %s' % m.err
    assert m.beginAttempt == BEGIN_MAX_TRY, \
        'BEGIN 應該恰好送 %d 次（計數器不得被 OTA_STAGED 歸零），實際 %d' \
        % (BEGIN_MAX_TRY, m.beginAttempt)
    assert (w.now - 1000) < 20000, '應該在 20 秒內收手，實際 %d ms' % (w.now - 1000)


def scen_late_ready():
    w = World(Chan(), reboot_to=NEW_VER)
    m, sl = w.run(inject_late_ready=True)
    assert sl.bootSwitched, 'slave 應該已經切換分區'
    assert m.phase == 'SUCCESS', \
        '晚到的 READY 蓋掉 OTA_OK 之後不得判失敗（假紅燈），實際 %s/%s' % (m.phase, m.err)


def scen_all_dropped():
    w = World(Chan(loss_dn=1.0), reboot_to=None)
    m, sl = w.run(max_steps=200000)
    assert m.err == 'slave_timeout', '完全打不通應報 slave_timeout，實際 %s' % m.err
    assert not sl.bootSwitched


def scen_data_dropped():
    class OnlyBegin(Chan):
        def __init__(self):
            Chan.__init__(self, seed=9)
            self.n = 0

        def deliver(self, kind='data'):
            self.n += 1
            return self.n <= 1

    w = World(OnlyBegin(), reboot_to=None)
    m, sl = w.run(max_steps=200000)
    assert m.err == 'espnow_fail', '資料全丟應報 espnow_fail，實際 %s' % m.err
    assert m.blockBase == 0
    assert not sl.bootSwitched


def scen_slave_rejects():
    """slave 回 ERR_SIZE（長度不合理）→ slave_reject，不得當成 READY。"""
    w = World(Chan(), reboot_to=None)
    # 直接讓 slave 拒絕：宣告一個過小的長度
    w.m.totalSize = 1024
    w.m.totalChunks = 5
    m, sl = w.run(max_steps=200000)
    assert m.err == 'slave_reject', '應報 slave_reject，實際 %s' % m.err
    assert not sl.bootSwitched


def scen_stage_only():
    """otadl 的 stageOnly：原地收尾，**一封封包都不送給 slave**。"""
    w = World(Chan(), stage_only=True, reboot_to=None)
    m, sl = w.run()
    assert m.phase == 'SUCCESS_STAGED', '應停在暫存完成，實際 %s' % m.phase
    assert not m.touchedSlave, 'stageOnly 不得對 slave 送出任何封包'
    assert not m.finished, 'stageOnly 不得宣告「已更新到」'
    assert not sl.bootSwitched


def scen_verify_no_return():
    """slave 校驗通過、重開機，但再也沒回線 → no_return（誠實紅燈）。"""
    w = World(Chan(), reboot_to=None)
    m, sl = w.run()
    assert sl.bootSwitched, 'slave 應該已經切換分區'
    assert m.err == 'no_return', '沒回線應報 no_return，實際 %s/%s' % (m.phase, m.err)
    assert not m.finished


def scen_verify_wrong_version():
    """回線了、但回報的是舊版本 → 不得宣告成功。"""
    w = World(Chan(), reboot_to=OLD_VER)
    m, sl = w.run()
    assert m.err == 'no_return', '版本不符不得宣告成功，實際 %s/%s' % (m.phase, m.err)
    assert not m.finished


def scen_verify_never_reported():
    """目標版本 0.0.0 ＋ 那台從沒回報過狀態（fw 全 0）→ 前提 (a) 必須擋住。"""
    w = World(Chan(), ver=(0, 0, 0), reboot_to=None)
    m, sl = w.run()
    assert not m.finished, '前提 (a) 沒擋住：版本 0.0.0 對上了預設的 fw=0.0.0（假綠燈）'
    assert m.err == 'no_return'


def scen_verify_stale_report():
    """工作階段**開始之前**就有一份「已經是目標版本」的回報，且那台沒有回線 →
    前提 (c) 必須擋住（這正是 lastSeen 條件存在的理由）。"""
    w = World(Chan(), ver=NEW_VER, reboot_to=None, prereport=NEW_VER)
    m, sl = w.run()
    assert not m.finished, '前提 (c) 沒擋住：拿工作階段開始前的舊回報宣告成功（假綠燈）'
    assert m.err == 'no_return'


def scen_data_only_loss():
    """只丟 OTA_DATA 的 10%：轉送**必須**完成。
    這條是 wide_wait_partial_advance（收到部分 bitmap 就推進）的殺手 ——
    提早推進會讓那一塊在 slave 端永遠收不齊，最後長度對不上、校驗失敗。"""
    for seed in (21, 22, 23):
        w = World(Chan(loss_dn=0.10, seed=seed, data_only=True), reboot_to=NEW_VER)
        m, sl = w.run()
        assert sl.otaWritten == FW_SIZE, \
            'seed%d：只丟資料包時必須收齊，實際寫入 %d' % (seed, sl.otaWritten)
        assert sl.bootSwitched, 'seed%d：應該校驗通過並切換分區' % seed
        assert m.phase == 'SUCCESS', 'seed%d：實際 %s/%s' % (seed, m.phase, m.err)
        assert bytes(sl.flash) == FW


def scen_out_of_order_ack():
    """ACK 延遲抵達：上一塊的回音會在 master 已經推進之後才到。
    落後 ACK 的守衛若拿掉，那封回音會被當成「本塊缺包」→ 整塊白重送一輪。
    這裡用**送出包數**當判準（時間就是空中佔用，也就是心跳的餘裕）。"""
    blocks = (FW_CHUNKS + WINDOW - 1) // WINDOW
    for seed in (31, 32, 33):
        w = World(Chan(seed=seed), reboot_to=NEW_VER, ack_delay=120)
        m, sl = w.run()
        assert m.phase == 'SUCCESS', \
            'seed%d：延遲 ACK 下仍應完成，實際 %s/%s' % (seed, m.phase, m.err)
        assert bytes(sl.flash) == FW
        # 判準用**查詢封數**而不是資料包數：落後的 ACK 被當成「本塊缺包」時，
        # 算出來的補送遮罩是 full & ~0xFFFF ＝ 0，於是那一輪一包資料都不送、
        # 直接又送一封查詢 —— 浪費的是來回，不是資料量。
        assert m.sentPolls <= blocks * 1.5, \
            'seed%d：查詢 %d 封／區塊 %d 塊 —— 落後的 ACK 被當成缺包了' \
            % (seed, m.sentPolls, blocks)


def scen_corrupt_only_ready():
    """映像壞掉（slave 校驗必失敗）＋ 那封 ERR_MD5 掉了 ＋ 一封晚到的 READY。
    READY 若被當成成功，就是**假綠燈**：slave 根本沒切換分區。"""
    w = World(Chan(), reboot_to=None, corrupt=True, drop_status_ack=True)
    m, sl = w.run(inject_late_ready=True)
    assert not sl.bootSwitched, 'slave 校驗應該失敗、不得切換分區'
    assert not m.finished, 'READY 被當成成功 ＝ 假綠燈'
    assert m.err == 'no_return', '應該落到版本回檢後誠實紅燈，實際 %s' % m.err


def scen_corrupt_fake_ok():
    """注入一封 (0, 0, OK)（原始設計下與查詢回覆逐位元組相同）。
    master 若只看 status 就宣告成功，那是**假綠燈**：slave 沒切換分區。"""
    w = World(Chan(), reboot_to=None, corrupt=True, drop_status_ack=True)
    m, sl = w.run(inject_fake_ok=True)
    assert not sl.bootSwitched
    assert not m.finished, '(0,0,OK) 被當成校驗通過 ＝ 假綠燈'


def scen_repair_during_verify():
    """VERIFYING 期間目標**重新配對**：addSlave() 會設 online＝true、
    lastSeen＝millis()、fw 三段全 0。目標版本又是 0.0.0（otadl 固定傳的就是它）——
    前提 (b)(c) 全部成立，**唯一擋住假綠燈的是前提 (a)**。"""
    w = World(Chan(), ver=(0, 0, 0), reboot_to=None, repair_during_verify=True)
    m, sl = w.run()
    assert w.repaired, '這條情境必須真的走到重新配對'
    assert not m.finished, \
        '前提 (a) 沒擋住：重新配對的預設 fw=0.0.0 對上目標版本 0.0.0（假綠燈）'
    assert m.err == 'no_return'


def scen_ready_before_err():
    """映像壞掉、slave 會回 HO_OTA_ERR_MD5，但**一封晚到的 READY 先到**。
    正確行為：忽略 READY、繼續等，然後收到 ERR_MD5 → 報 md5_mismatch。
    把 READY 當成成功的話，master 會提早離開 END_SENT，之後那封 ERR_MD5 沒人看，
    最後報成 no_return —— **兩者都是紅燈，但診斷指向完全不同的地方**
    （「它校驗失敗」vs「它沒開起來」），現場會去查錯的東西。"""
    w = World(Chan(seed=41), reboot_to=None, corrupt=True, ack_delay=300)
    m, sl = w.run(inject_late_ready=True)
    assert not sl.bootSwitched, 'slave 校驗應該失敗'
    assert not m.finished
    assert m.err == 'md5_mismatch', \
        '晚到的 READY 讓 master 錯過真正的校驗結果，實際 %s' % m.err


def scen_fake_ok_before_err():
    """同上，但插進來的是一封 (0, 0, OK) —— **原始設計下查詢回覆長的就是這樣**。
    master 若只看 status 就會把它當成校驗通過，錯過真正的 ERR_MD5。"""
    w = World(Chan(seed=42), reboot_to=None, corrupt=True, ack_delay=300)
    m, sl = w.run(inject_fake_ok=True)
    assert not sl.bootSwitched
    assert not m.finished, '(0,0,OK) 被當成校驗通過 ＝ 假綠燈'
    assert m.err == 'md5_mismatch', \
        '只看 status 讓 master 錯過真正的校驗結果，實際 %s' % m.err


SCENARIOS = [
    ('乾淨通道走完全程並宣告成功', scen_clean),
    ('下行 10% 丟包：不得假紅燈也不得假綠燈', scen_lossy_down),
    ('ACK 回程 30% 丟包：靠重發查詢救回來', scen_lossy_up),
    ('送出佇列 20% 滿：背壓即流控', scen_backpressure),
    ('slave 完全不回：BEGIN 恰好 5 次後 slave_timeout', scen_silent_slave),
    ('END_SENT 收到晚到的 READY：不得判失敗', scen_late_ready),
    ('下行 100% 丟包：誠實紅燈', scen_all_dropped),
    ('只有 BEGIN 通得過：8 輪重送後 espnow_fail', scen_data_dropped),
    ('slave 拒絕 BEGIN：slave_reject', scen_slave_rejects),
    ('otadl 的 stageOnly：一封都不送、不宣告已更新', scen_stage_only),
    ('校驗通過但沒回線：no_return', scen_verify_no_return),
    ('回線了但版本是舊的：不得宣告成功', scen_verify_wrong_version),
    ('版本 0.0.0 對上從沒回報過的 fw=0：前提 (a)', scen_verify_never_reported),
    ('工作階段開始前的舊回報：前提 (c)', scen_verify_stale_report),
    ('只丟資料包的 10%：一定要收齊（提早推進會被抓到）', scen_data_only_loss),
    ('ACK 延遲抵達：落後的回音不得造成整塊重送', scen_out_of_order_ack),
    ('映像壞掉＋只收到晚到的 READY：不得宣告成功', scen_corrupt_only_ready),
    ('注入 (0,0,OK)：只看 status 就是假綠燈', scen_corrupt_fake_ok),
    ('VERIFYING 期間重新配對＋目標版本 0.0.0：前提 (a)', scen_repair_during_verify),
    ('晚到的 READY 插在真 ERR_MD5 之前：診斷不得被帶偏', scen_ready_before_err),
    ('(0,0,OK) 插在真 ERR_MD5 之前：不得只看 status', scen_fake_ok_before_err),
]


def run_all():
    failed = []
    for name, fn in SCENARIOS:
        for k in MUT:
            pass
        try:
            fn()
        except Exception as e:     # noqa: BLE001 —— 突變可能丟出斷言以外的例外
            failed.append((name, str(e)[:100]))
    return failed


def main():
    src = MASTER_SRC
    flat = re.sub(r'"\s*\n\s*"', '', src)

    print('常數（全部解析自原始碼，沒有一個寫死）：'
          'CHUNK=%d WINDOW=%d ACK_TIMEOUT=%d POLL_MAX=%d BLOCK_RETRY=%d '
          'BEGIN_TRY=%d SEND/LOOP=%d'
          % (CHUNK, WINDOW, ACK_TIMEOUT, POLL_MAX, BLOCK_MAX_RETRY,
             BEGIN_MAX_TRY, SEND_PER_LOOP))

    # ── 方向 A ──
    # **錨點必須唯一**（A 族第 20 次）：只驗「有沒有出現」的話，一條同時命中
    # 程式碼與旁邊那段說明註釋的錨點會**永遠命中**——把它守著的那一行整個刪掉、
    # 或改成別的寫法，腳本一個字都不會說。複審實測：本表原本 26 條裡有 6 條非唯一，
    # 而這一輪修的四項（MJ3／MJ6 計時器順延／MJ7／otaEndQueued）**全部可以原樣改回去**。
    # 這道檢查與同一份 report §2.1 剛學到的教訓是同一條，卻沒有套到自己身上。
    for label, needle in TRANSCRIBED_FROM_INO:
        n = flat.count(needle)
        if n == 0:
            FAILURES.append('[轉寫錨點] %s：ho_master1.ino 裡找不到 %r' % (label, needle))
        elif n > 1:
            FAILURES.append('[轉寫錨點] %s：錨點在 ho_master1.ino 出現 %d 次，**不唯一** —— '
                            '它可能命中的是旁邊的說明註釋而不是程式碼本身，'
                            '等於這條錨點什麼都沒守著。請補上下一行讓它唯一（%r）'
                            % (label, n, needle[:60]))

    # ── 方向 A2 ──
    for entry in ORDER_IN_INO:
        label, first, second = entry[0], entry[1], entry[2]
        hay = flat
        if len(entry) > 3:
            a, b = entry[3]
            ia, ib = flat.find(a), flat.find(b, flat.find(a) + 1 if flat.find(a) >= 0 else 0)
            if ia < 0 or ib < 0 or ib < ia:
                FAILURES.append('[順序錨點] %s：限定區塊的起訖錨點找不到（%r → %r）'
                                % (label, a, b))
                continue
            hay = flat[ia:ib]
        i, j = hay.find(first), hay.find(second)
        if i < 0 or j < 0:
            FAILURES.append('[順序錨點] %s：錨點不見了（%r / %r）' % (label, first, second))
        elif i > j:
            FAILURES.append('[順序錨點] %s：%r 排在 %r **之後**' % (label, first, second))
        else:
            # 同一個理由：不唯一的錨點比的可能是註釋裡的引用，不是程式碼的先後。
            for who in (first, second):
                if hay.count(who) > 1:
                    FAILURES.append('[順序錨點] %s：錨點 %r 在比對範圍內出現 %d 次，'
                                    '**不唯一** —— 順序檢查會比到不相干的位置'
                                    % (label, who[:50], hay.count(who)))

    # ── 基準：全部情境必須通過 ──
    base_failed = run_all()
    for name, why in base_failed:
        FAILURES.append('[情境] %s → %s' % (name, why))
    print('情境：%d 條，基準失敗 %d 條' % (len(SCENARIOS), len(base_failed)))

    # ── 方向 C：每個突變都必須至少殺掉一條情境 ──
    if not base_failed:
        for key in sorted(MUT):
            MUT[key] = True
            killed = run_all()
            MUT[key] = False
            branch, direction = MUT_META[key]
            if not killed:
                FAILURES.append('[突變存活] %s（%s／%s）沒有殺掉任何情境 —— '
                                '要嘛情境覆蓋不足、要嘛這條防線根本沒在做事'
                                % (key, branch, direction))
            else:
                print('  %-32s %-15s %-7s → 殺掉 %d 條  例：%s'
                      % (key, branch, direction, len(killed), killed[0][0]))

        covered = set()
        for key, (branch, direction) in MUT_META.items():
            if direction == 'widen':
                covered.add(branch)
        missing = [b for b in BRANCHES if b not in covered]
        if missing:
            FAILURES.append('[分支覆蓋] 這些分支沒有任何加寬方向的突變：%s' % missing)
        print('分支覆蓋：%d 個分支，每個都要有加寬突變 → %s'
              % (len(BRANCHES), '完整' if not missing else '缺 %s' % missing))

    if FAILURES:
        print('\n%d 項失敗：' % len(FAILURES))
        for f in FAILURES:
            print('  ' + f)
        return 1
    print('\nALL CHECKS PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
