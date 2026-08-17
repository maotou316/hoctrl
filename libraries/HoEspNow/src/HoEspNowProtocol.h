#pragma once
#include <Arduino.h>

// ── 協定常數 ──
#define HO_ESPNOW_MAGIC       0x4F48   // "HO"（小端序：低位 'H'=0x48，高位 'O'=0x4F）

// 協定版本。1 → 2 的改動有兩項，兩項都不相容：
//   (a) CRC 由「只涵蓋 payload」改為「涵蓋標頭前 6 bytes + payload」（見 hoFrameCrc()）
//   (b) HoCmdPayload 加 cmdId、HoStatePayload 加 lastCmdId/lastCmdKind/lastCmdCount
//       （指令歸因，見那兩個 struct 上方的說明）
//
// ⚠ 這是不相容的破壞性改動（flag-day），master 與所有 slave 必須「一起」重燒：
//    - 舊 slave 收到新 master 的封包 → hoUnpackPacket() 的 version 檢查不過 → 直接丟棄
//    - 於是舊 slave 收不到心跳 → 30 秒後 startChannelScan() 判定失聯 →
//      若當下 relayState 為 true 就 setRelayPins(false)（籠門被打開）
//    - 反向也一樣：新 slave 收不到舊 master 的封包，同一個後果
//
// **不相容不會靜默**：兩端的 onEspNowRecv() 都會用 hoPeekVersionMismatch() 印一行
// 版本不符告警（節流過，見各自 sketch），現場看得到原因而不是「莫名其妙全部失聯」。
//
// 燒錄順序與現場程序見 docs/phase4-flag-day-upgrade.md。**兩種順序都有窗口**，
// 沒有哪一種是安全的，唯一安全的做法是升級前先讓籠門處於不依賴繼電器保持的狀態。
//
// 目前沒有任何現場部署（Phase 2a 已裁定 master 在補上現場恢復手段前不得佈到現場），
// 所以現在改的代價最低。日後若真的需要不停機升級，必須改成「master 同時用新舊兩種
// CRC 各發一次心跳」的過渡期做法 —— 那是另一個階段的工作。
#define HO_ESPNOW_VERSION     2

#define HO_ESPNOW_MAX_SLAVES  20       // 不使用原生加密，peer 上限 20
#define HO_ESPNOW_MAX_PAYLOAD 243      // ESP-NOW 單包 250 - 標頭 7

// 共享密鑰：混入 CRC 計算，過濾鄰居的 ESP-NOW 封包與誤觸發。
// 這不是加密，擋得住誤觸但擋不住重放攻擊，本場景可接受。
#define HO_ESPNOW_SHARED_KEY  "hoban-espnow-2026"

// ── 封包型別 ──
enum HoPacketType : uint8_t {
  HO_PKT_HEARTBEAT = 0x01,  // master → 廣播
  HO_PKT_PAIR_REQ  = 0x02,  // slave  → master
  HO_PKT_PAIR_ACK  = 0x03,  // master → slave
  HO_PKT_UNPAIR    = 0x04,  // 雙向
  HO_PKT_CMD       = 0x10,  // master → slave
  HO_PKT_STATE_REQ = 0x11,  // master → slave
  HO_PKT_STATE     = 0x12,  // slave  → master
  HO_PKT_OTA_BEGIN = 0x20,  // master → slave：總長度、包數、MD5、目標版本
  HO_PKT_OTA_DATA  = 0x21,  // master → slave：chunkIndex + 資料塊
  HO_PKT_OTA_END   = 0x22,  // master → slave：結束或中止
  HO_PKT_OTA_ACK   = 0x23,  // 雙向：master 查詢區塊 / slave 回報 bitmap 與狀態
};

// ── 繼電器指令 ──
// ⚠ 實際數值是 OFF=0 / ON=1 / PULSE=2。文件曾寫成「1=ON 2=OFF 3=PULSE」三個全錯，
// 引用時一律回來讀這裡的定義，不要憑名稱推順序。
enum HoRelayCmd : uint8_t {
  HO_CMD_OFF   = 0,
  HO_CMD_ON    = 1,  // 持續開啟
  HO_CMD_PULSE = 2,  // 點動，開啟 pulseMs 後自動關閉
};

// 指令識別碼的保留值：「開機以來沒有執行過任何指令」。
// master 配發 cmdId 時**必須跳過 0**，否則 slave 的「沒執行過」與
// 「執行過第 0 號」分不出來 —— 那是誤綠方向的錯誤。
#define HO_CMD_ID_NONE  ((uint16_t)0)

// ── 配對拒絕原因 ──
enum HoPairReason : uint8_t {
  HO_PAIR_OK          = 0,
  HO_PAIR_FULL        = 1,  // 已達 20 台上限
  HO_PAIR_NOT_PAIRING = 2,  // master 不在配對模式
};

// ── OTA 狀態碼 ──
enum HoOtaStatus : uint8_t {
  HO_OTA_OK          = 0,  // 校驗通過，即將重啟
  HO_OTA_READY       = 1,  // OTA_BEGIN 已接受，可以開始送資料
  HO_OTA_ERR_BEGIN   = 2,  // Update.begin() 失敗（分區空間不足等）
  HO_OTA_ERR_WRITE   = 3,  // Update.write() 寫入失敗
  HO_OTA_ERR_MD5     = 4,  // MD5 或長度校驗不符，已中止，開機分區未切換
  HO_OTA_ERR_SIZE    = 5,  // 宣告的長度不合理
  HO_OTA_ERR_SESSION = 6,  // sessionId 不符
  HO_OTA_ERR_BUSY    = 7,  // 已有其他工作階段進行中
  HO_OTA_ABORTED     = 8,  // 已依 master 指示或逾時中止
};

// ── 封包標頭（7 bytes）──
struct __attribute__((packed)) HoPacketHeader {
  uint16_t magic;    // HO_ESPNOW_MAGIC，過濾非本系統封包
  uint8_t  version;  // HO_ESPNOW_VERSION
  uint8_t  type;     // HoPacketType
  uint16_t seq;      // 序號，用於去重與 OTA 分包
  uint8_t  crc;      // 標頭前 6 bytes ＋ payload ＋ 共享密鑰 的 CRC8（見 hoFrameCrc()）
};

// ── 各型別的 payload ──
struct __attribute__((packed)) HoHeartbeatPayload {
  uint8_t channel;      // master 目前的 WiFi channel（1~13）
  uint8_t pairingMode;  // 1 = 配對模式中
  uint8_t longRange;    // 1 = LR 模式已啟用
  uint8_t slaveCount;   // 目前已配對數量
};

struct __attribute__((packed)) HoPairAckPayload {
  uint8_t accepted;   // 1 = 接受
  uint8_t reason;     // HoPairReason
  uint8_t channel;    // master 目前 channel，slave 記起來
  uint8_t longRange;  // master 的 LR 設定，slave 跟著切
};

// ── 繼電器指令（版本 2 起帶 cmdId）──
//
// cmdId 是「**一道邏輯指令**」的識別碼，不是「一個封包」的識別碼。
// 為什麼不直接用標頭的 seq：seq 是**每幀遞增**的傳輸序號，而本系統的一道群組指令
// 會被送出 3 次廣播 ＋ 每台至少 1 次單播 ＋ 最多 2 趟補送（見 ho_master1 的
// sendCmdToAll()／processGroupCmd()）—— 每一幀的 seq 都不同。若拿 seq 做歸因，
// slave 回報「我執行的是 seq=41」而 master 記得自己最後送的是 seq=57，
// 就會把一台**確實執行了**的 slave 判成未執行（誤紅），或反過來對錯 seq（誤綠）。
// 同一道邏輯指令的所有重送與補送共用同一個 cmdId，歸因才對得起來。
struct __attribute__((packed)) HoCmdPayload {
  uint8_t  cmd;      // HoRelayCmd
  uint16_t pulseMs;  // HO_CMD_PULSE 時的持續毫秒，其餘忽略
  uint16_t cmdId;    // 這道邏輯指令的識別碼，slave 執行後原樣回填到 HoStatePayload
};

// ── 狀態回報（版本 2 起帶指令歸因）──
//
// **這三個欄位還的是 Phase 2b Task 5 登記的技術債。** 在版本 1 的協定下，
// 狀態回報只有 relay/版本/uptime，master 拿不到任何「這個 relay 值是哪一道指令
// 造成的」的資訊，於是「指令有被執行」**原理上無法證明** ——
// Task 5 因此只敢宣稱「已送達」（單播的 MAC 層 ACK 由對方射頻產生、master 無法自製）。
//
// **這三個欄位證明什麼**：slave 的韌體確實走完了 HO_PKT_CMD 分支的
// setRelayPins()／pulseRelay() 呼叫，而且那一次的 cmdId 就是 master 送的那一道。
//
// **它們擋不住什麼（務必連著讀）**：
//   1. **不證明繼電器硬體真的動作**。setRelayPins() 只寫 GPIO；MOS 燒毀、線路脫落、
//      繼電器觸點黏死都不會反映在這裡。
//   2. **不證明籠門關上了**。HO_CMD_PULSE 只證明點動計時器被啟動。
//   3. **不區分「執行了幾次」的因果**。廣播 3 次 ＋ 單播 1 次會讓同一個 cmdId 被執行
//      多次，lastCmdCount 只是把次數如實帶回來，不代表哪一次生效。
//   4. **不擋重放**。協定沒有加密也沒有 nonce，錄下一則舊的 HO_PKT_STATE 重播，
//      在 CRC 與共享密鑰之下仍然合法。master 端額外要求「回報時間 ≥ 指令送出時間」
//      才採信（見 ho_master1 的 groupExecutedIdx()），那擋得住**跨指令**的舊回報，
//      擋不住指令送出後才被重播的那則。
//   5. **cmdId 只有 16 bits**，master 端每下一道指令 +1，理論上 65535 道之後會繞回。
//      master 開機時用 esp_random() 取初值，讓「重開機後撞到上一輪同一個 cmdId」
//      需要同時滿足「值相同」與「舊回報晚於新指令送出時間」兩個條件。
struct __attribute__((packed)) HoStatePayload {
  uint8_t  relay;      // 0 / 1
  uint8_t  fwMajor;
  uint8_t  fwMinor;
  uint8_t  fwPatch;
  uint32_t uptimeSec;
  uint16_t lastCmdId;    // 最後一次「實際執行」的 HoCmdPayload::cmdId；HO_CMD_ID_NONE = 沒執行過
  uint8_t  lastCmdKind;  // 那道指令的 HoRelayCmd（OFF=0/ON=1/PULSE=2）；lastCmdId 為 0 時無意義
  uint8_t  lastCmdCount; // 同一個 cmdId 被執行的次數（重送與補送會 >1），飽和於 255
};

// ── OTA ──

// 一包資料的上限。243（HO_ESPNOW_MAX_PAYLOAD）− 3（HoOtaDataPayload 的前置欄位）= 240。
// 因此 OTA_DATA 的封包總長 = 7 + 3 + 240 = 250，剛好貼滿 ESP-NOW 單包上限。
#define HO_OTA_CHUNK_SIZE  240

// 一個區塊的包數。16 讓 ACK 的 bitmap 剛好是一個 uint16_t，
// slave 端的區塊緩衝也剛好 16 × 240 = 3840 bytes。
#define HO_OTA_WINDOW      16

// totalChunks 是 uint16_t，理論上限 65535 包 = 15.7 MB，遠大於 app 分區的 1.94 MB。
// 這個常數只是給兩端做合理性檢查用（2031616 是 ho_master1/partitions.csv 的 app0 大小）。
#define HO_OTA_MAX_CHUNKS  ((uint16_t)((2031616 + HO_OTA_CHUNK_SIZE - 1) / HO_OTA_CHUNK_SIZE))

// ⚠ 以下四個 struct 的欄位名稱與順序**逐字取自設計文件的「決定 2.2」**
//（docs/superpowers/plans/2026-08-17-esp32-phase4-ota-relay.md）。
// Task 2~5 會照那份文件實作，這裡自作主張改名或調換順序就會兩邊對不起來。

// 0x20 OTA_BEGIN  master → slave。26 bytes。
struct __attribute__((packed)) HoOtaBeginPayload {
  uint32_t totalSize;    // 韌體總位元組數
  uint16_t totalChunks;  // ceil(totalSize / HO_OTA_CHUNK_SIZE)；uint16 上限 65535 包 ≈ 15 MB，夠用
  uint8_t  md5[16];      // 整份韌體的 MD5，16 bytes 原始值（不是 32 字元 hex，省 16 bytes）
  uint8_t  verMajor;
  uint8_t  verMinor;
  uint8_t  verPatch;
  uint8_t  sessionId;    // 1~255（**0 保留給「無工作階段」**），用來丟棄上一次失敗殘留的封包
};  // 4+2+16+3+1 = 26 bytes

// 0x21 OTA_DATA  master → slave
struct __attribute__((packed)) HoOtaDataPayload {
  uint8_t  sessionId;
  uint16_t chunkIndex;   // 0-based。刻意「不」重用標頭的 seq —— seq 是 espNowSendTo()
                         // 自動遞增的全域傳送計數器，挪作塊號會同時破壞去重語義並
                         // 逼所有呼叫端改簽名。多花 2 bytes 換一個乾淨的邊界。
  // 之後緊接著 1~HO_OTA_CHUNK_SIZE bytes 的韌體資料（長度由封包總長推得）
};  // 3 bytes + 資料

// 0x22 OTA_END  master → slave
struct __attribute__((packed)) HoOtaEndPayload {
  uint8_t  sessionId;
  uint8_t  abort;      // 1 = 中止，slave 應 Update.abort() 並丟棄
  uint32_t totalSize;  // 再帶一次，讓 slave 交叉核對自己實際寫入的位元組數
};  // 6 bytes

// 0x23 OTA_ACK  雙向。方向本身就是語義，不需要額外的 request 欄位：
//   到達 slave 的 OTA_ACK ＝ master 在查詢這個區塊收到了哪幾包
//   到達 master 的 OTA_ACK ＝ slave 的回報
struct __attribute__((packed)) HoOtaAckPayload {
  uint8_t  sessionId;
  uint16_t blockBase;  // 本區塊第一包的 chunkIndex
  uint16_t mask;       // slave→master：bit i = 已收到 blockBase+i；master→slave 查詢時填 0
  uint8_t  status;     // HoOtaStatus，只有 slave→master 有意義
};  // 6 bytes

// OTA 工作階段編號的保留值：無工作階段。sessionId 的合法範圍是 1~255。
#define HO_OTA_SESSION_NONE  ((uint8_t)0)

// 編譯期保證跨端一致。struct 大小一旦被 padding 或欄位順序改動影響，
// master 與 slave 會安靜地對不起來 —— 這裡直接讓它編不過。
static_assert(sizeof(HoPacketHeader)     == 7,  "HoPacketHeader 必須是 7 bytes");
static_assert(sizeof(HoHeartbeatPayload) == 4,  "HoHeartbeatPayload 必須是 4 bytes");
static_assert(sizeof(HoPairAckPayload)   == 4,  "HoPairAckPayload 必須是 4 bytes");
static_assert(sizeof(HoCmdPayload)       == 5,  "HoCmdPayload 必須是 5 bytes");
static_assert(sizeof(HoStatePayload)     == 12, "HoStatePayload 必須是 12 bytes");
static_assert(sizeof(HoOtaBeginPayload)  == 26, "HoOtaBeginPayload 必須是 26 bytes");
static_assert(sizeof(HoOtaDataPayload)   == 3,  "HoOtaDataPayload 必須是 3 bytes");
static_assert(sizeof(HoOtaEndPayload)    == 6,  "HoOtaEndPayload 必須是 6 bytes");
static_assert(sizeof(HoOtaAckPayload)    == 6,  "HoOtaAckPayload 必須是 6 bytes");
static_assert(sizeof(HoPacketHeader) + sizeof(HoOtaDataPayload) + HO_OTA_CHUNK_SIZE == 250,
              "OTA_DATA 封包總長必須剛好 250 bytes（ESP-NOW 單包上限）");
static_assert(sizeof(HoOtaDataPayload) + HO_OTA_CHUNK_SIZE == HO_ESPNOW_MAX_PAYLOAD,
              "HO_OTA_CHUNK_SIZE 與 HO_ESPNOW_MAX_PAYLOAD 對不起來");

// ── 函式 ──

// 標準 CRC-8（多項式 0x07，初值 0x00）
uint8_t hoCrc8(const uint8_t* data, size_t len);

// 封包的 CRC。涵蓋範圍：標頭的前 6 bytes（magic/version/type/seq）＋ payload ＋ 共享密鑰。
//
// 為什麼一定要涵蓋標頭（Phase 1 最終審查殘留第 5 項，指定「Phase 4 做 OTA 前必須改」）：
// 舊版只算 payload，於是 type 欄位的位元翻轉「不會」被偵測。一封 OTA_DATA（0x21）的
// type 翻成 HO_PKT_CMD（0x10）之後 CRC 仍然正確，slave 會把韌體資料的前 5 bytes 當成
// HoCmdPayload 解讀 —— cmd 欄位剛好落在 0/1/2 就會實際動作繼電器。
// 這是動物管制設備上的誤觸發，OTA 要送出數千封封包，遇上的機率不再是理論值。
//
// 不含 crc 欄位自己（它就是本函式的輸出），所以只算前 6 bytes。
//
// **它擋不住什麼**：CRC-8 本身只是錯誤偵測碼，不是訊息鑑別碼。共享密鑰只讓
// 「不知道密鑰的人算不出正確值」，擋不住重放（錄下一封合法封包原樣重送仍然合法），
// 也擋不住知道密鑰的人偽造。要擋那些得上真正的 MAC／nonce，不在本階段範圍。
uint8_t hoFrameCrc(const uint8_t* headerFirst6, const uint8_t* payload, size_t payloadLen);

// 打包。回傳寫入 out 的總長度；outSize 不足或 payload 過長回 0。
size_t hoPackPacket(uint8_t* out, size_t outSize, HoPacketType type,
                    uint16_t seq, const void* payload, size_t payloadLen);

// 解包。驗證 magic / version / crc，通過回 true。
// outPayload 指向 data 內部，不複製；data 生命週期結束後不可再用。
bool hoUnpackPacket(const uint8_t* data, size_t len, HoPacketHeader* outHeader,
                    const uint8_t** outPayload, size_t* outPayloadLen);

// 「magic 對、但版本不是 HO_ESPNOW_VERSION」時回 true，並把對方的版本寫進 outVersion。
//
// 存在的唯一理由是**讓 flag-day 的不相容看得見**：hoUnpackPacket() 對版本不符
// 只會安靜地回 false，現場看到的症狀會是「兩邊都在跑、就是完全不通、30 秒後籠門
// 全開」，而原因完全不顯示。兩端的 onEspNowRecv() 用這個函式印一行告警（節流過）。
//
// **它擋不住什麼**：它只是**回報**，不做任何補救 —— 版本不符的封包照樣被丟棄，
// 心跳照樣中斷，30 秒失聯保護照樣會觸發。它也偵測不到「版本相同但欄位語義改了」
// 的那種不相容（那種只能靠 struct 大小的 static_assert 與人工紀律）。
bool hoPeekVersionMismatch(const uint8_t* data, size_t len, uint8_t* outVersion);

// MAC → "hoban-a0b1c2d3e4f5"。out 至少 20 bytes。
void hoFormatDeviceId(const uint8_t mac[6], char out[20]);

// "hoban-a0b1c2d3e4f5" → MAC。格式不符回 false。
bool hoParseMacFromDeviceId(const char* deviceId, uint8_t out[6]);
