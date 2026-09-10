# -*- coding: utf-8 -*-
"""`ho_slave1` OTA 接收狀態機的邏輯轉寫模擬 —— 情境測試 ＋ **雙向突變驗證**。

用法（在 repo 根目錄）：
    python tools/ota_slave_sim.py
全部通過印 ALL CHECKS PASSED，任何一項失敗以 exit code 1 結束。

## 為什麼一支「轉寫模擬」需要三道自我約束

原始版本只有情境與突變，**它自己就是 A 族病灶的溫床**：轉寫可以跟韌體漂開、
可以把第三方函式庫的行為憑印象模型化、可以有整個分支從來沒被突變碰過，
而三者都不會讓任何一條測試變紅。Task 2 的 review 一次抓到三種：

1. 韌體的判斷式改了、模擬沒改 → 模擬在驗一份不存在的程式
2. **C1**：`Update.end(true)` 被憑語義模型成「會檢查長度」，
   而 `Updater.cpp` 的實際行為是 `_size = progress();`（**放棄長度檢查**，只驗 MD5）。
   那個錯誤同時進了原始碼註釋、報告與模擬三處
3. `on_query()` 整條分支**一次都沒被任何情境呼叫**，
   於是「查詢一律謊報全滿」這種加寬突變**存活**

所以本腳本自己有三道驗收條件，缺一即 exit 1：

- **方向 A（轉寫錨點）**：模擬轉寫的每一條判斷式，都以**逐字字串**登記在
  `TRANSCRIBED_FROM_INO`，必須能在 `ho_slave1.ino` 裡命中。
  改韌體沒改模擬 → **當場變紅**。順序性的要求另外用 `ORDER_IN_INO` 驗。
- **方向 B（第三方行為錨點）**：**禁止憑印象模型第三方函式庫的內部行為**。
  不得不模型時，必須在 `UPDATER_ANCHORS` 逐字引用 `Updater.cpp`／`Update.h`
  的原文，並回讀真實檔案確認。找不到那份原始碼就直接失敗，不容許「跳過就當過」。
- **方向 C（分支覆蓋）**：每一個 `on_*` 分支**至少要有一個加寬方向的突變**，
  而且**每一個突變都必須至少殺掉一條情境**。有這條的話，
  `wide_query_status_ok` 不會等到複審才被發現。

## 這份模擬擋不住什麼（必須連著讀）

- 它是**轉寫**，不是韌體。方向 A 只能保證「被登記的那些判斷式沒漂」，
  **沒登記的行為一個字都沒驗**，轉寫與韌體的語義差異也驗不到。
- 沒有真的 flash：寫入、抹除耗時、RX 佇列溢位、`Update` 的錯誤碼全部沒模型。
  **「失敗不變磚」五道保障裡只有第 5 道（長度雙重把關）在這裡跑得到**，
  其餘四道是 `Updater.cpp` 的內部行為，只有方向 B 的錨點在證明「我讀過它」，
  不等於證明它在真板子上如此。
- 驗不到時序：recv callback 與 `loop()` 在不同 task，這裡是單執行緒依序呼叫。
  **所有涉及並行的結論都是靜態推演、沒有上界保證。**
- 驗不到無線層：丟包率、MAC ACK 與應用層收包的落差（C3 的核心）全部在模型之外。
"""
import hashlib
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INO = os.path.join(ROOT, 'ho_slave1', 'ho_slave1.ino')

# esp32 core 3.3.7 的 Update 函式庫。可用環境變數覆寫（CI 或別台機器路徑不同）。
UPDATER_DIR_DEFAULT = os.path.join(
    os.path.expanduser('~'), 'AppData', 'Local', 'Arduino15', 'packages', 'esp32',
    'hardware', 'esp32', '3.3.7', 'libraries', 'Update', 'src')
UPDATER_DIR = os.environ.get('HO_UPDATER_SRC', UPDATER_DIR_DEFAULT)

HO_OTA_CHUNK_SIZE = 240
HO_OTA_WINDOW = 16
HO_OTA_SESSION_NONE = 0
OTA_SLAVE_IDLE_MS = 30000
HEARTBEAT_TIMEOUT = 30000

HO_OTA_OK, HO_OTA_READY = 0, 1
HO_OTA_ERR_BEGIN, HO_OTA_ERR_WRITE, HO_OTA_ERR_MD5 = 2, 3, 4
HO_OTA_ERR_SIZE, HO_OTA_ERR_SESSION, HO_OTA_ERR_BUSY, HO_OTA_ABORTED = 5, 6, 7, 8


# ───────────────── 方向 A：轉寫錨點（改韌體沒改模擬就變紅）─────────────────
# 左邊是模擬裡對應的那一段邏輯，右邊是 ho_slave1.ino 裡**逐字**的判斷式。
TRANSCRIBED_FROM_INO = [
    ('BEGIN：重複 sessionId 的分支',
     'if (otaActive && bg.sessionId == otaSession) {'),
    ('BEGIN：其他 sessionId 的分支',
     'if (otaActive && bg.sessionId != otaSession) {'),
    ('BEGIN：殘留判定（不在分支開頭刷新時間戳才成立）',
     'if (millis() - otaLastPacketAt < OTA_SLAVE_IDLE_MS) {'),
    ('BEGIN：BUSY 回覆後還原工作階段編號',
     'otaSession = keepSession;'),
    ('BEGIN：長度合理性',
     'if (bg.totalSize < 65536 || bg.totalSize > 2031616 ||'),
    ('DATA：工作階段檢查',
     'if (!otaActive || dh.sessionId != otaSession) return;'),
    ('DATA：塊號上界守衛',
     'if (dh.chunkIndex >= otaTotalChunks) return;'),
    ('DATA：長度必須剛好等於該塊號應有的長度',
     'if ((uint32_t)dataLen != expectLen) return;'),
    ('DATA：只收目前區塊內的包',
     'if (dh.chunkIndex < otaBlockBase || dh.chunkIndex >= otaBlockBase + HO_OTA_WINDOW) return;'),
    ('DATA：收齊判定',
     'if ((otaBlockMask & fullMask) != fullMask) return;'),
    ('DATA：區塊收齊的回報用 READY（不是 OK）',
     'otaSendAck(otaBlockBase, fullMask, HO_OTA_READY);'),
    ('QUERY：落後的區塊回全滿',
     'if (q.blockBase < otaBlockBase) {'),
    ('QUERY：落後區塊的回覆用 READY',
     'otaSendAck(q.blockBase, 0xFFFF, HO_OTA_READY);'),
    ('QUERY：目前進度的回覆用 READY',
     'otaSendAck(otaBlockBase, otaBlockMask, HO_OTA_READY);'),
    ('END：長度三重比對',
     'if (!otaActive || otaWritten != en.totalSize || otaWritten != otaTotalSize) {'),
    ('END：校驗通過是全檔唯一的 HO_OTA_OK，且帶正向識別',
     'otaSendAck(otaTotalChunks, 0xFFFF, HO_OTA_OK);'),
    ('LOOP：OTA 閒置逾時',
     'if (otaActive && now - otaLastPacketAt >= OTA_SLAVE_IDLE_MS) {'),
    ('LOOP：失聯門檻',
     'if (now - lastHeartbeatTime > HEARTBEAT_TIMEOUT) {'),
    ('LIVENESS：收到任何自家 master 的封包都刷新（錨在它的具名註釋上，'
     '因為 lastHeartbeatTime = millis(); 在檔案裡有三處）',
     '// ── OTA 期間的失聯保護（Phase 4 決定 1a）──'),
    ('版本三段是唯一來源（B 族第 8 次的修正）',
     'st.fwMajor = HO_SLAVE_FW_MAJOR;'),
]

# 順序性的要求：字串比對驗不到順序，這裡用「出現位置先後」補。
ORDER_IN_INO = [
    ('失聯保護那一行必須在「只接受已配對 master」的 guard 之後',
     'if (!masterKnown || memcmp(masterMac, info->src_addr, 6) != 0) {',
     '// ── OTA 期間的失聯保護（Phase 4 決定 1a）──'),
    ('startChannelScan()：先關繼電器，再釋放 Update',
     'Serial.println("[安全] 失去 master，繼電器已關閉");',
     'otaAbort("失去 master，開始輪掃");'),
]

# ───────────────── 方向 B：第三方行為錨點（禁止憑印象模型）─────────────────
# 模擬對 `Update` 的每一項行為假設，都要在這裡逐字引用原始碼。
UPDATER_ANCHORS = [
    ('Updater.cpp', 'end(true) 放棄長度檢查，改成把 _size 覆寫成實際寫入量',
     '  if (evenIfRemaining) {'),
    ('Updater.cpp', '同上：_size = progress()',
     '    _size = progress();'),
    ('Updater.cpp', 'MD5 不符就中止且不切換分區',
     '      _abort(UPDATE_ERROR_MD5);'),
    ('Updater.cpp', '切換開機分區只發生在 _verifyEnd()',
     '    if (esp_ota_set_boot_partition(_partition)) {'),
    ('Updater.cpp', '每跨 64 KB 邊界是一次區塊抹除（不是 4 KB 扇區）',
     'if (!ESP.partitionEraseRange(_partition, _progress, block_erase ? SPI_FLASH_BLOCK_SIZE : SPI_FLASH_SEC_SIZE)) {'),
    ('Updater.cpp', '開頭 16 bytes 先扣住不寫，半寫的映像開不起來（plan 沒列到的第六道）',
     '    memcpy(_skipBuffer, _buffer, skip);'),
    ('Updater.cpp', 'begin() 配置 4096 bytes heap 緩衝（RAM 帳要算進去）',
     '  _buffer = new (std::nothrow) uint8_t[SPI_FLASH_SEC_SIZE];'),
    ('Update.h', 'SPI_FLASH_BLOCK_SIZE 是 16 個扇區 ＝ 64 KB',
     '#define SPI_SECTORS_PER_BLOCK 16'),
]

# ───────────────── 突變旗標 ─────────────────
# 全部 False ＝ 目前 ho_slave1.ino 的行為。
# 每一項都要在 MUT_META 標明「屬於哪個分支」與「哪個方向」。
MUT = {
    'plan_begin_refresh_top': False,
    'plan_busy_session_zero': False,
    'wide_begin_accept_any': False,
    'drop_chunk_upper_guard': False,
    'drop_len_exact_guard': False,
    'drop_block_lower_bound': False,
    'wide_data_any_session': False,
    'wide_query_status_ok': False,
    'wide_end_any_session': False,
    'wide_end_skip_length': False,
    'wide_end_skip_md5': False,
    'wide_loop_no_timeout': False,
    'no_liveness_refresh': False,
}

# (分支, 方向)。方向 'remove' ＝ 把防線拿掉／把計畫書原文寫回來；
#              'widen'  ＝ 多開一條綠燈路徑。
MUT_META = {
    'plan_begin_refresh_top': ('on_begin', 'remove'),
    'plan_busy_session_zero': ('on_begin', 'remove'),
    'wide_begin_accept_any':  ('on_begin', 'widen'),
    'drop_chunk_upper_guard': ('on_data', 'remove'),
    'drop_len_exact_guard':   ('on_data', 'remove'),
    'drop_block_lower_bound': ('on_data', 'remove'),
    'wide_data_any_session':  ('on_data', 'widen'),
    'wide_query_status_ok':   ('on_query', 'widen'),
    'wide_end_any_session':   ('on_end', 'widen'),
    'wide_end_skip_length':   ('on_end', 'widen'),
    'wide_end_skip_md5':      ('on_end', 'widen'),
    'wide_loop_no_timeout':   ('loop', 'widen'),
    'no_liveness_refresh':    ('liveness', 'widen'),
}
BRANCHES = ['on_begin', 'on_data', 'on_query', 'on_end', 'loop', 'liveness']


class Slave(object):
    def __init__(self):
        self.otaSession = HO_OTA_SESSION_NONE
        self.otaActive = False
        self.otaTotalSize = 0
        self.otaTotalChunks = 0
        self.otaBlockBase = 0
        self.otaBlockMask = 0
        self.otaWritten = 0
        self.otaLastPacketAt = 0
        self.otaTargetMd5 = None
        self.acks = []              # (blockBase, mask, status, 封包上的 sessionId)
        self.bootSwitched = False   # ＝ Update.end(true) 走完 _verifyEnd()
        self.aborts = []
        self.flash = bytearray()
        self._buf = bytearray(HO_OTA_WINDOW * HO_OTA_CHUNK_SIZE)   # 從不清空，與韌體一致
        # 失聯保護相關
        self.relayState = True
        self.lastHeartbeatTime = 0
        self.scanning = False

    # ── otaSendAck() ──
    def _ack(self, block_base, mask, status):
        self.acks.append((block_base, mask, status, self.otaSession))

    # ── otaAbort() ──
    def abort(self, why):
        if not self.otaActive and self.otaSession == HO_OTA_SESSION_NONE:
            return
        self.aborts.append(why)
        self.otaActive = False
        self.otaSession = HO_OTA_SESSION_NONE
        self.otaWritten = 0
        self.otaBlockMask = 0
        self.otaBlockBase = 0

    # ── startChannelScan()：先關繼電器，再釋放 Update（順序由 ORDER_IN_INO 驗）──
    def start_channel_scan(self):
        if self.scanning:
            return
        if self.relayState:
            self.relayState = False
        self.abort('失去 master，開始輪掃')
        self.scanning = True

    # ── 「收到任何自家 master 的封包」共同入口 ──
    def on_master_packet(self, now):
        if not MUT['no_liveness_refresh']:
            self.lastHeartbeatTime = now

    # ── HO_PKT_OTA_BEGIN ──
    def on_begin(self, now, session, total_size, total_chunks, md5=None):
        self.on_master_packet(now)
        if MUT['plan_begin_refresh_top']:
            self.otaLastPacketAt = now

        if self.otaActive and session == self.otaSession:
            self.otaLastPacketAt = now
            self._ack(self.otaBlockBase, self.otaBlockMask, HO_OTA_READY)
            return

        if self.otaActive and session != self.otaSession:
            if (now - self.otaLastPacketAt < OTA_SLAVE_IDLE_MS
                    and not MUT['wide_begin_accept_any']):
                keep = self.otaSession
                self.otaSession = session
                self._ack(0, 0, HO_OTA_ERR_BUSY)
                self.otaSession = HO_OTA_SESSION_NONE if MUT['plan_busy_session_zero'] else keep
                return
            self.abort('上一個工作階段殘留')

        self.otaLastPacketAt = now
        self.otaSession = session

        if (total_size < 65536 or total_size > 2031616
                or total_chunks == 0 or total_chunks > 8466):
            self._ack(0, 0, HO_OTA_ERR_SIZE)
            self.otaSession = HO_OTA_SESSION_NONE
            return

        self.otaActive = True
        self.otaTotalSize = total_size
        self.otaTotalChunks = total_chunks
        self.otaTargetMd5 = md5
        self.otaBlockBase = 0
        self.otaBlockMask = 0
        self.otaWritten = 0
        self.flash = bytearray()
        self._ack(0, 0, HO_OTA_READY)

    # ── HO_PKT_OTA_DATA ──
    def on_data(self, now, session, chunk_index, payload):
        self.on_master_packet(now)
        if not self.otaActive:
            return
        if not MUT['wide_data_any_session'] and session != self.otaSession:
            return

        self.otaLastPacketAt = now

        data_len = len(payload)
        if data_len == 0 or data_len > HO_OTA_CHUNK_SIZE:
            return

        if not MUT['drop_chunk_upper_guard'] and chunk_index >= self.otaTotalChunks:
            return

        if not MUT['drop_len_exact_guard']:
            if chunk_index == self.otaTotalChunks - 1:
                expect = self.otaTotalSize - (self.otaTotalChunks - 1) * HO_OTA_CHUNK_SIZE
            else:
                expect = HO_OTA_CHUNK_SIZE
            if data_len != expect:
                return

        # 下界與上界要分開突變：複審的 MY_wide_data_no_lower_bound 拿掉下界之後
        # slot 會變成負數，模擬丟出的是例外而不是斷言失敗 —— 這正是 run() 必須
        # 捕捉 Exception 的理由（只捕捉 AssertionError 會讓整支腳本靜默停工）。
        low_ok = chunk_index >= self.otaBlockBase or MUT['drop_block_lower_bound']
        if not low_ok or chunk_index >= self.otaBlockBase + HO_OTA_WINDOW:
            return

        slot = chunk_index - self.otaBlockBase
        self._buf[slot * HO_OTA_CHUNK_SIZE:slot * HO_OTA_CHUNK_SIZE + data_len] = payload
        self.otaBlockMask |= (1 << slot)

        need = HO_OTA_WINDOW
        if self.otaBlockBase + HO_OTA_WINDOW > self.otaTotalChunks:
            need = self.otaTotalChunks - self.otaBlockBase
        full_mask = 0xFFFF if need >= 16 else ((1 << need) - 1)
        if (self.otaBlockMask & full_mask) != full_mask:
            return

        remain = self.otaTotalSize - self.otaWritten
        write_len = need * HO_OTA_CHUNK_SIZE
        if write_len > remain:
            write_len = remain

        # Update.write()：順序寫入，模擬只保留位元組流（抹除／錯誤碼不在模型內）
        self.flash += self._buf[:write_len]
        self.otaWritten += write_len

        self._ack(self.otaBlockBase, full_mask, HO_OTA_READY)
        self.otaBlockBase += need
        self.otaBlockMask = 0

    # ── HO_PKT_OTA_ACK（master 的查詢）──
    def on_query(self, now, session, block_base):
        self.on_master_packet(now)
        if not self.otaActive or session != self.otaSession:
            return
        self.otaLastPacketAt = now
        status = HO_OTA_OK if MUT['wide_query_status_ok'] else HO_OTA_READY
        if block_base < self.otaBlockBase:
            self._ack(block_base, 0xFFFF, status)
            return
        self._ack(self.otaBlockBase, self.otaBlockMask, status)

    # ── HO_PKT_OTA_END ──
    def on_end(self, now, session, abort, total_size):
        self.on_master_packet(now)
        if not MUT['wide_end_any_session'] and session != self.otaSession:
            return
        self.otaLastPacketAt = now
        if abort:
            self._ack(0, 0, HO_OTA_ABORTED)
            self.abort('master 指示中止')
            return

        if not MUT['wide_end_skip_length']:
            if (not self.otaActive or self.otaWritten != total_size
                    or self.otaWritten != self.otaTotalSize):
                self._ack(0, 0, HO_OTA_ERR_MD5)
                self.abort('長度不符')
                return

        # ── Update.end(true) 的模型 ──
        # **不驗長度**（`Updater.cpp` 的 `_size = progress();`，見 UPDATER_ANCHORS），
        # 只比對 setMD5() 設定的 MD5；相符才走 _verifyEnd() → esp_ota_set_boot_partition()。
        if not MUT['wide_end_skip_md5']:
            actual = hashlib.md5(bytes(self.flash)).hexdigest()
            if self.otaTargetMd5 is not None and actual != self.otaTargetMd5:
                self._ack(0, 0, HO_OTA_ERR_MD5)
                self.abort('校驗失敗')
                return
        self.bootSwitched = True
        self._ack(self.otaTotalChunks, 0xFFFF, HO_OTA_OK)
        self.otaActive = False

    # ── loop() ──
    def loop(self, now):
        if not MUT['wide_loop_no_timeout']:
            if self.otaActive and now - self.otaLastPacketAt >= OTA_SLAVE_IDLE_MS:
                self.abort('超過 30 秒沒收到 OTA 封包')
        if now - self.lastHeartbeatTime > HEARTBEAT_TIMEOUT:
            self.start_channel_scan()


# ────────────────────────── 情境 ──────────────────────────
FW_SIZE = 100000          # 100000 / 240 → 417 包、26 個區塊（最後一塊只有 1 包）
FW_CHUNKS = (FW_SIZE + HO_OTA_CHUNK_SIZE - 1) // HO_OTA_CHUNK_SIZE
FW = bytes(bytearray((i * 7 + 3) & 0xFF for i in range(FW_SIZE)))
FW_MD5 = hashlib.md5(FW).hexdigest()


def chunk(i):
    return FW[i * HO_OTA_CHUNK_SIZE:(i + 1) * HO_OTA_CHUNK_SIZE]


def begin(s, t=1000, session=7):
    s.on_begin(t, session, FW_SIZE, FW_CHUNKS, FW_MD5)


def deliver_all(s, t0=1000, session=7, order=lambda idx: idx):
    t = t0
    for base in range(0, FW_CHUNKS, HO_OTA_WINDOW):
        idxs = list(range(base, min(base + HO_OTA_WINDOW, FW_CHUNKS)))
        for i in order(idxs):
            t += 5
            s.on_data(t, session, i, chunk(i))
    return t


def t_happy():
    s = Slave()
    begin(s)
    t = deliver_all(s)
    s.on_end(t + 10, 7, 0, FW_SIZE)
    assert s.otaWritten == FW_SIZE, '寫入長度 %d != %d' % (s.otaWritten, FW_SIZE)
    assert bytes(s.flash) == FW, '寫進去的位元組流與原韌體不同'
    assert s.bootSwitched, '校驗通過卻沒切換開機分區'
    assert s.acks[-1][:3] == (FW_CHUNKS, 0xFFFF, HO_OTA_OK), \
        '成功那封 ACK 不是 (totalChunks, 0xFFFF, OK)，實際 %s' % (s.acks[-1][:3],)


def t_out_of_order_and_dup():
    s = Slave()
    begin(s)

    def scramble(idxs):
        return list(reversed(idxs)) + [idxs[0], idxs[-1]]   # 亂序＋重複兩包
    t = deliver_all(s, order=scramble)
    s.on_end(t + 10, 7, 0, FW_SIZE)
    assert bytes(s.flash) == FW, '亂序＋重複之後位元組流不正確'
    assert s.bootSwitched


def t_stale_session_recovers():
    """殘留的工作階段必須能被釋放：master 一直重送新 sessionId 也不能把 slave 卡死。"""
    s = Slave()
    begin(s)
    s.on_data(1100, 7, 0, chunk(0))
    t, accepted = 1100, False
    for _ in range(24):                       # 24 × 5 秒 ＝ 120 秒
        t += 5000
        s.loop(t)
        s.on_begin(t, 9, FW_SIZE, FW_CHUNKS, FW_MD5)
        if s.otaActive and s.otaSession == 9:
            accepted = True
            break
    assert accepted, '殘留工作階段沒被釋放，120 秒內新的 session 一直被拒絕'


def t_busy_reject_keeps_running_session():
    """拒絕外來 BEGIN 之後，原本那場 OTA 必須完好無損地繼續。"""
    s = Slave()
    begin(s)
    s.on_data(1100, 7, 0, chunk(0))
    s.on_begin(1200, 9, FW_SIZE, FW_CHUNKS, FW_MD5)
    assert s.acks[-1][2] == HO_OTA_ERR_BUSY, '沒有回 BUSY（狀態碼 %s）' % s.acks[-1][2]
    assert s.acks[-1][3] == 9, 'BUSY 的 ACK 沒帶插隊者的 sessionId'
    t = 1200
    for base in range(0, FW_CHUNKS, HO_OTA_WINDOW):
        for i in range(base, min(base + HO_OTA_WINDOW, FW_CHUNKS)):
            if i == 0:
                continue
            t += 5
            s.on_data(t, 7, i, chunk(i))
    s.on_end(t + 10, 7, 0, FW_SIZE)
    assert s.otaWritten == FW_SIZE, '插隊之後原本的工作階段掉了 %d bytes' % (FW_SIZE - s.otaWritten)
    assert s.bootSwitched, '插隊之後原本的工作階段沒能完成'


def t_no_bogus_ok_after_last_block():
    """全部收完之後，塊號超出宣告包數的封包不得換來任何 ACK。"""
    s = Slave()
    begin(s)
    t = deliver_all(s)
    n_before = len(s.acks)
    s.on_data(t + 10, 7, FW_CHUNKS, chunk(0))
    s.on_data(t + 20, 7, FW_CHUNKS + 3, chunk(1))
    assert len(s.acks) == n_before, '越界塊號換來了 %d 封多餘的 ACK' % (len(s.acks) - n_before)


def t_short_packet_never_reaches_image():
    """中段短包必須被拒絕；緩衝從不清空，收了就會把前一塊的殘留寫進映像。"""
    s = Slave()
    begin(s)
    t = deliver_all(s, order=lambda idx: idx) if False else 1000
    # 第一個區塊照常收滿
    for i in range(0, HO_OTA_WINDOW):
        t += 5
        s.on_data(t, 7, i, chunk(i))
    # 第二個區塊：其中一包故意送短的（100 bytes），其餘正常
    base = HO_OTA_WINDOW
    for i in range(base, base + HO_OTA_WINDOW):
        t += 5
        s.on_data(t, 7, i, chunk(i)[:100] if i == base + 3 else chunk(i))
    assert (s.otaBlockMask & (1 << 3)) == 0 or s.otaBlockBase == base, \
        '短包被記進 bitmap'
    assert s.otaBlockBase == base, '第二塊在缺一包的情況下就被判定收齊'
    # 補送正確長度的那一包後才應該收齊
    t += 5
    s.on_data(t, 7, base + 3, chunk(base + 3))
    assert s.otaBlockBase == base + HO_OTA_WINDOW, '補齊之後仍未推進'
    assert bytes(s.flash) == FW[:len(s.flash)], '寫進映像的位元組與原韌體不符'


def t_foreign_session_data_ignored():
    """別的 sessionId 的 DATA 一個位元組都不能進映像。"""
    s = Slave()
    begin(s)
    for i in range(HO_OTA_WINDOW):
        s.on_data(1100 + i, 9, i, chunk(i))       # session 9，不是 7
    assert s.otaWritten == 0, '外來 session 的資料被寫進去了 %d bytes' % s.otaWritten
    assert s.otaBlockMask == 0, '外來 session 的資料進了區塊 bitmap'


def t_query_reply_never_claims_success():
    """查詢回覆絕不能長得像「校驗通過」那一封。"""
    s = Slave()
    begin(s)
    s.on_query(1100, 7, 0)                       # 一包都還沒收就被查詢
    assert s.acks[-1][:3] != (0, 0, HO_OTA_OK), \
        '查詢回覆與「校驗通過」逐位元組相同：%s' % (s.acks[-1][:3],)
    assert s.acks[-1][2] != HO_OTA_OK, '查詢回覆用了 HO_OTA_OK'
    # 收完第一塊之後再查一次，以及查一個已經寫完的舊區塊
    t = 1100
    for i in range(HO_OTA_WINDOW):
        t += 5
        s.on_data(t, 7, i, chunk(i))
    s.on_query(t + 5, 7, 0)
    assert s.acks[-1][:3] == (0, 0xFFFF, HO_OTA_READY), \
        '對已寫完區塊的查詢回覆不正確：%s' % (s.acks[-1][:3],)
    s.on_query(t + 10, 7, HO_OTA_WINDOW)
    assert s.acks[-1][2] != HO_OTA_OK, '對目前區塊的查詢回覆用了 HO_OTA_OK'


def t_short_firmware_never_switches_boot():
    """長度不足時必須中止，且絕不切換開機分區。"""
    s = Slave()
    begin(s)
    t = 1000
    for i in range(HO_OTA_WINDOW):
        t += 5
        s.on_data(t, 7, i, chunk(i))
    s.on_end(t + 10, 7, 0, FW_SIZE)
    assert not s.bootSwitched, '長度不符卻切換了開機分區＝變磚路徑'
    assert s.aborts, '長度不符卻沒有中止'


def t_end_declares_different_size():
    """END 宣告的長度與 BEGIN 不一致時必須中止。

    這是**唯一只有 END 前置比對擋得住**的情境：位元組流完全正確、MD5 會過，
    `Update.end(true)` 會回 true 並切換開機分區（它根本不驗長度，
    見 UPDATER_ANCHORS 的 `_size = progress();`），
    不一致的只有 master 在 OTA_END 裡宣告的 totalSize。
    """
    s = Slave()
    begin(s)
    t = deliver_all(s)
    s.on_end(t + 10, 7, 0, FW_SIZE - HO_OTA_CHUNK_SIZE)
    assert not s.bootSwitched, 'master 的 END 長度與 BEGIN 不一致卻仍切換了開機分區'
    assert s.aborts, 'END 長度不一致卻沒有中止'


def t_md5_mismatch_never_switches_boot():
    """內容錯誤（長度全對）只有 MD5 擋得住 —— 擋下來就絕不能切換開機分區。"""
    s = Slave()
    begin(s)
    t = 1000
    for base in range(0, FW_CHUNKS, HO_OTA_WINDOW):
        for i in range(base, min(base + HO_OTA_WINDOW, FW_CHUNKS)):
            t += 5
            data = chunk(i)
            if i == 5:                                   # 長度不變、內容改一個位元組
                data = bytes(bytearray([data[0] ^ 0xFF])) + data[1:]
            s.on_data(t, 7, i, data)
    # 長度必須在 END 之前確認：otaAbort() 會把 otaWritten 歸零（與韌體一致），
    # 在 END 之後才斷言長度等於零斷言 —— 這一行的位置本身就是判準的一部分。
    assert s.otaWritten == FW_SIZE, '長度應該是對的（實際 %d）' % s.otaWritten
    s.on_end(t + 10, 7, 0, FW_SIZE)
    assert not s.bootSwitched, '內容錯誤卻切換了開機分區'
    assert s.acks[-1][2] == HO_OTA_ERR_MD5, '沒有回報 MD5 失敗'


def t_liveness_keeps_ota_alive():
    """OTA 期間廣播心跳全丟，只靠單播維持存活：不得進輪掃、不得關繼電器。"""
    s = Slave()
    s.lastHeartbeatTime = 1000
    begin(s)
    t = 1000
    # 40 秒（＞30 秒門檻）內完全沒有廣播心跳，只有 OTA 單播
    for _ in range(200):
        t += 200
        idx = ((t // 200) % FW_CHUNKS)
        s.on_data(t, 7, idx if idx < s.otaBlockBase + HO_OTA_WINDOW else s.otaBlockBase,
                  chunk(s.otaBlockBase))
        s.loop(t)
    assert not s.scanning, 'OTA 期間被判失聯並進入輪掃 —— OTA 會被打斷'
    assert s.relayState, 'OTA 期間繼電器被強制關閉＝籠門被打開'
    assert s.otaActive, 'OTA 工作階段被中止'


def t_end_from_wrong_session_is_ignored():
    """`OTA_END` 是全檔**唯一**能產生 `HO_OTA_OK`、也**唯一**能切換開機分區的分支。

    別的 sessionId 送來的 END 必須被靜默丟棄（M5 的敘述就寫在那裡，
    但在複審之前**沒有任何情境在守它** —— `MY_wide_end_any_session` 因此存活）。
    兩種殘留 END 都要驗：宣告成功的、以及 abort=1 的。
    """
    s = Slave()
    begin(s)
    t = deliver_all(s)
    n_before = len(s.acks)
    s.on_end(t + 10, 9, 0, FW_SIZE)          # session 9，不是 7：宣告「收工吧」
    assert not s.bootSwitched, '外來 session 的 END 竟然切換了開機分區'
    assert len(s.acks) == n_before, '對外來 session 的 END 回了 %d 封 ACK（應靜默丟棄）'         % (len(s.acks) - n_before)
    assert s.otaActive, '外來 session 的 END 把進行中的工作階段關掉了'

    s.on_end(t + 20, 9, 1, FW_SIZE)          # 同一台的殘留 abort
    assert s.otaActive and not s.aborts, '外來 session 的 abort 打斷了進行中的工作階段'

    s.on_end(t + 30, 7, 0, FW_SIZE)          # 真正的 END 仍然要能收工
    assert s.bootSwitched, '擋掉外來 END 之後，真正的 END 也收不了工'


def t_idle_timeout_releases_update():
    s = Slave()
    s.lastHeartbeatTime = 10 ** 9      # 排除失聯路徑，只驗 OTA 逾時
    begin(s, t=1000)
    s.loop(1000 + OTA_SLAVE_IDLE_MS - 1)
    assert s.otaActive, '還沒到 30 秒就中止'
    s.loop(1000 + OTA_SLAVE_IDLE_MS)
    assert not s.otaActive and s.otaSession == HO_OTA_SESSION_NONE, '30 秒到了沒中止'


TESTS = [
    ('順序送完整份韌體', t_happy),
    ('亂序＋重複包', t_out_of_order_and_dup),
    ('殘留工作階段可被釋放', t_stale_session_recovers),
    ('BUSY 拒絕不打斷原本的工作階段', t_busy_reject_keeps_running_session),
    ('收完後越界塊號不產生假 ACK', t_no_bogus_ok_after_last_block),
    ('中段短包不得進入映像', t_short_packet_never_reaches_image),
    ('外來 session 的 DATA 被忽略', t_foreign_session_data_ignored),
    ('查詢回覆絕不冒充校驗通過', t_query_reply_never_claims_success),
    ('長度不足絕不切換開機分區', t_short_firmware_never_switches_boot),
    ('END 宣告長度不一致必中止', t_end_declares_different_size),
    ('內容錯誤由 MD5 擋下', t_md5_mismatch_never_switches_boot),
    ('外來 session 的 END 被靜默丟棄', t_end_from_wrong_session_is_ignored),
    ('單播維持存活，OTA 不被自己打斷', t_liveness_keeps_ota_alive),
    ('30 秒閒置逾時釋放 Update', t_idle_timeout_releases_update),
]


def read(path):
    with io.open(path, encoding='utf-8') as f:
        return f.read()


def run():
    """跑一輪情境。

    **必須捕捉 Exception 而不是只捕捉 AssertionError。**
    複審的 `MY_wide_data_no_lower_bound` 突變讓模擬丟出 IndexError，
    原本只 `except AssertionError` 的寫法讓整支腳本帶著 traceback 中止 ——
    **後面的突變與方向 C 的分支覆蓋檢查全部靜默不執行**。
    一個會靜默停止工作的守衛比沒有守衛更危險：它看起來還在。
    例外本身也算一條紅（突變讓程式爆掉，同樣證明那條防線有在做事）。
    """
    failed = []
    for name, fn in TESTS:
        try:
            fn()
        except AssertionError as e:
            failed.append((name, str(e)))
        except Exception as e:                      # noqa: BLE001 —— 見上方
            failed.append((name, '%s：%s' % (type(e).__name__, e)))
    return failed


def main():
    failures = []

    # ── 方向 A：轉寫錨點 ──
    ino = read(INO)
    flat = re.sub(r'"\s*\n\s*"', '', ino)
    for name, needle in TRANSCRIBED_FROM_INO:
        if needle not in flat:
            failures.append('[轉寫錨點] %s：ho_slave1.ino 裡找不到 %r' % (name, needle))
    for name, first, second in ORDER_IN_INO:
        i, j = flat.find(first), flat.find(second)
        if i < 0 or j < 0:
            failures.append('[順序] %s：錨點字串找不到' % name)
        elif i > j:
            failures.append('[順序] %s：順序反了' % name)
    print('轉寫錨點 %d 條、順序 %d 條' % (len(TRANSCRIBED_FROM_INO), len(ORDER_IN_INO)))

    # ── 方向 B：第三方行為錨點 ──
    for fname, name, needle in UPDATER_ANCHORS:
        path = os.path.join(UPDATER_DIR, fname)
        if not os.path.exists(path):
            failures.append('[函式庫錨點] 找不到 %s —— 無法驗證「%s」。'
                            '請用 HO_UPDATER_SRC 指定 esp32 core 3.3.7 的 Update/src 路徑'
                            % (path, name))
            continue
        if needle not in read(path):
            failures.append('[函式庫錨點] %s：%s 裡找不到 %r（core 版本換了？模型就不成立了）'
                            % (name, fname, needle))
    print('函式庫錨點 %d 條（來源：%s）' % (len(UPDATER_ANCHORS), UPDATER_DIR))

    # ── 基準 ──
    print('\n── 基準（目前 ho_slave1.ino 的行為）──')
    base_failed = run()
    for n, e in base_failed:
        print('  FAIL %s：%s' % (n, e))
        failures.append('[基準] %s：%s' % (n, e))
    print('  %d/%d 通過' % (len(TESTS) - len(base_failed), len(TESTS)))

    # ── 突變（兩個方向）──
    print('\n── 突變驗證（每一項都必須讓至少一條情境變紅）──')
    for flag in MUT:
        MUT[flag] = True
        f = run()
        MUT[flag] = False
        branch, direction = MUT_META[flag]
        print('  %-24s %-9s %-6s → %d 條紅  %s'
              % (flag, branch, direction, len(f), '、'.join(n for n, _ in f)))
        if not f:
            failures.append('[突變存活] %s（%s／%s）沒有殺掉任何情境 —— 那條防線是裝飾品'
                            % (flag, branch, direction))

    # ── 方向 C：分支覆蓋 ──
    for br in BRANCHES:
        widen = [k for k, (b, d) in MUT_META.items() if b == br and d == 'widen']
        if not widen:
            failures.append('[分支覆蓋] %s 沒有任何加寬方向的突變 —— '
                            '「沒有人能偷偷加一條綠燈進來」在這個分支上沒有證據' % br)
    print('\n分支覆蓋：%d 個分支，每個都要有加寬突變' % len(BRANCHES))

    if failures:
        print('\n%d 項失敗：' % len(failures))
        for f in failures:
            print('  ' + f)
        return 1
    print('\nALL CHECKS PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
