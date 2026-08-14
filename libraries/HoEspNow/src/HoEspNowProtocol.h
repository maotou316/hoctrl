#pragma once
#include <Arduino.h>

// ── 協定常數 ──
#define HO_ESPNOW_MAGIC       0x4F48   // "HO"（小端序：低位 'H'=0x48，高位 'O'=0x4F）
#define HO_ESPNOW_VERSION     1
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
};

// ── 繼電器指令 ──
enum HoRelayCmd : uint8_t {
  HO_CMD_OFF   = 0,
  HO_CMD_ON    = 1,  // 持續開啟
  HO_CMD_PULSE = 2,  // 點動，開啟 pulseMs 後自動關閉
};

// ── 配對拒絕原因 ──
enum HoPairReason : uint8_t {
  HO_PAIR_OK          = 0,
  HO_PAIR_FULL        = 1,  // 已達 20 台上限
  HO_PAIR_NOT_PAIRING = 2,  // master 不在配對模式
};

// ── 封包標頭（7 bytes）──
struct __attribute__((packed)) HoPacketHeader {
  uint16_t magic;    // HO_ESPNOW_MAGIC，過濾非本系統封包
  uint8_t  version;  // HO_ESPNOW_VERSION
  uint8_t  type;     // HoPacketType
  uint16_t seq;      // 序號，用於去重與 OTA 分包
  uint8_t  crc;      // payload + 共享密鑰 的 CRC8
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

struct __attribute__((packed)) HoCmdPayload {
  uint8_t  cmd;      // HoRelayCmd
  uint16_t pulseMs;  // HO_CMD_PULSE 時的持續毫秒，其餘忽略
};

struct __attribute__((packed)) HoStatePayload {
  uint8_t  relay;      // 0 / 1
  uint8_t  fwMajor;
  uint8_t  fwMinor;
  uint8_t  fwPatch;
  uint32_t uptimeSec;
};

// ── 函式 ──

// 標準 CRC-8（多項式 0x07，初值 0x00）
uint8_t hoCrc8(const uint8_t* data, size_t len);

// payload 的 CRC：先跑資料，再把共享密鑰接著跑進去
uint8_t hoPayloadCrc(const uint8_t* payload, size_t len);

// 打包。回傳寫入 out 的總長度；outSize 不足或 payload 過長回 0。
size_t hoPackPacket(uint8_t* out, size_t outSize, HoPacketType type,
                    uint16_t seq, const void* payload, size_t payloadLen);

// 解包。驗證 magic / version / crc，通過回 true。
// outPayload 指向 data 內部，不複製；data 生命週期結束後不可再用。
bool hoUnpackPacket(const uint8_t* data, size_t len, HoPacketHeader* outHeader,
                    const uint8_t** outPayload, size_t* outPayloadLen);

// MAC → "hoban-a0b1c2d3e4f5"。out 至少 20 bytes。
void hoFormatDeviceId(const uint8_t mac[6], char out[20]);

// "hoban-a0b1c2d3e4f5" → MAC。格式不符回 false。
bool hoParseMacFromDeviceId(const char* deviceId, uint8_t out[6]);
