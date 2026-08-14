#include "HoEspNowProtocol.h"
#include <ctype.h>

uint8_t hoCrc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

uint8_t hoPayloadCrc(const uint8_t* payload, size_t len) {
  // 先跑 payload，再把共享密鑰接著跑，讓沒有密鑰的封包算不出正確 CRC
  uint8_t crc = hoCrc8(payload, len);
  const char* key = HO_ESPNOW_SHARED_KEY;
  for (size_t i = 0; key[i] != '\0'; i++) {
    crc ^= (uint8_t)key[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

size_t hoPackPacket(uint8_t* out, size_t outSize, HoPacketType type,
                    uint16_t seq, const void* payload, size_t payloadLen) {
  if (payloadLen > HO_ESPNOW_MAX_PAYLOAD) return 0;
  size_t total = sizeof(HoPacketHeader) + payloadLen;
  if (outSize < total) return 0;
  if (payloadLen > 0 && payload == nullptr) return 0;

  HoPacketHeader header;
  header.magic   = HO_ESPNOW_MAGIC;
  header.version = HO_ESPNOW_VERSION;
  header.type    = (uint8_t)type;
  header.seq     = seq;
  header.crc     = hoPayloadCrc((const uint8_t*)payload, payloadLen);

  memcpy(out, &header, sizeof(header));
  if (payloadLen > 0) {
    memcpy(out + sizeof(header), payload, payloadLen);
  }
  return total;
}

bool hoUnpackPacket(const uint8_t* data, size_t len, HoPacketHeader* outHeader,
                    const uint8_t** outPayload, size_t* outPayloadLen) {
  if (data == nullptr || len < sizeof(HoPacketHeader)) return false;

  HoPacketHeader header;
  memcpy(&header, data, sizeof(header));

  if (header.magic != HO_ESPNOW_MAGIC) return false;
  if (header.version != HO_ESPNOW_VERSION) return false;

  size_t payloadLen = len - sizeof(HoPacketHeader);
  const uint8_t* payload = data + sizeof(HoPacketHeader);
  if (header.crc != hoPayloadCrc(payload, payloadLen)) return false;

  if (outHeader)     *outHeader = header;
  if (outPayload)    *outPayload = payload;
  if (outPayloadLen) *outPayloadLen = payloadLen;
  return true;
}

void hoFormatDeviceId(const uint8_t mac[6], char out[20]) {
  snprintf(out, 20, "hoban-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 單一十六進位字元 → 數值。呼叫前必須先用 isxdigit() 確認合法，
// 否則遇到非十六進位字元會回傳未定義的結果。
static uint8_t hoHexNibble(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
  return (uint8_t)(c - 'A' + 10);  // 呼叫端已保證是十六進位字元
}

bool hoParseMacFromDeviceId(const char* deviceId, uint8_t out[6]) {
  if (deviceId == nullptr) return false;
  if (strncmp(deviceId, "hoban-", 6) != 0) return false;
  const char* hex = deviceId + 6;
  if (strlen(hex) != 12) return false;

  // 不用 strtol：它會吃掉前導空白與正負號，"  1"、"-1" 這種畸形輸入
  // 也會被判定為「完整消耗兩字元」而誤判成功。改成逐字元用 isxdigit()
  // 驗證後再手動轉換，才擋得住外部輸入（如 MQTT topic）夾帶的怪字元。
  for (int i = 0; i < 6; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo)) return false;
    out[i] = (uint8_t)((hoHexNibble(hi) << 4) | hoHexNibble(lo));
  }
  return true;
}
