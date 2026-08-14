// 齁控 ESP-NOW 協定 on-target 單元測試
// 燒錄到任一片 ESP32-C3，序列埠會印出測試結果。
// 全部通過會印出 "ALL TESTS PASSED"，任何一項失敗印出 "TESTS FAILED"。
#include <HoEspNowProtocol.h>

int testsRun = 0;
int testsFailed = 0;

void check(bool cond, const char* name) {
  testsRun++;
  if (cond) {
    Serial.printf("  [PASS] %s\n", name);
  } else {
    testsFailed++;
    Serial.printf("  [FAIL] %s\n", name);
  }
}

// struct 大小必須固定，否則 master 與 slave 用不同編譯器選項會對不起來
void testStructSizes() {
  Serial.println("── struct 大小 ──");
  check(sizeof(HoPacketHeader) == 7, "HoPacketHeader 為 7 bytes");
  check(sizeof(HoHeartbeatPayload) == 4, "HoHeartbeatPayload 為 4 bytes");
  check(sizeof(HoPairAckPayload) == 4, "HoPairAckPayload 為 4 bytes");
  check(sizeof(HoCmdPayload) == 3, "HoCmdPayload 為 3 bytes");
  check(sizeof(HoStatePayload) == 8, "HoStatePayload 為 8 bytes");
}

void testCrc() {
  Serial.println("── CRC8 ──");
  const uint8_t data[] = { 0x01, 0x02, 0x03 };
  uint8_t a = hoCrc8(data, 3);
  uint8_t b = hoCrc8(data, 3);
  check(a == b, "同樣輸入產生同樣 CRC");

  const uint8_t other[] = { 0x01, 0x02, 0x04 };
  check(hoCrc8(data, 3) != hoCrc8(other, 3), "不同輸入產生不同 CRC");

  check(hoPayloadCrc(data, 3) != hoCrc8(data, 3), "含密鑰的 CRC 與純 CRC 不同");
  check(hoCrc8(nullptr, 0) == 0x00, "空資料 CRC 為 0");
}

void testPackUnpack() {
  Serial.println("── 打包／解包 ──");
  uint8_t buf[250];
  HoHeartbeatPayload hb = { 6, 1, 0, 3 };

  size_t len = hoPackPacket(buf, sizeof(buf), HO_PKT_HEARTBEAT, 42, &hb, sizeof(hb));
  check(len == sizeof(HoPacketHeader) + sizeof(hb), "打包長度正確");

  HoPacketHeader header;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  bool res = hoUnpackPacket(buf, len, &header, &payload, &payloadLen);

  check(res, "解包成功");
  check(header.type == HO_PKT_HEARTBEAT, "型別正確");
  check(header.seq == 42, "序號正確");
  check(payloadLen == sizeof(hb), "payload 長度正確");

  HoHeartbeatPayload out;
  memcpy(&out, payload, sizeof(out));
  check(out.channel == 6, "channel 欄位正確");
  check(out.pairingMode == 1, "pairingMode 欄位正確");
  check(out.slaveCount == 3, "slaveCount 欄位正確");
}

void testRejectBadPackets() {
  Serial.println("── 拒收異常封包 ──");
  uint8_t buf[250];
  HoCmdPayload cmd = { HO_CMD_PULSE, 2000 };
  size_t len = hoPackPacket(buf, sizeof(buf), HO_PKT_CMD, 1, &cmd, sizeof(cmd));

  check(!hoUnpackPacket(buf, 3, nullptr, nullptr, nullptr), "長度不足時拒收");

  buf[0] ^= 0xFF;  // 破壞 magic
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "magic 錯誤時拒收");
  buf[0] ^= 0xFF;

  buf[2] = 99;     // 破壞 version
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "version 不符時拒收");
  buf[2] = HO_ESPNOW_VERSION;

  buf[sizeof(HoPacketHeader)] ^= 0xFF;  // 破壞 payload
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "payload 被竄改時 CRC 不符拒收");

  check(hoPackPacket(buf, 5, HO_PKT_CMD, 1, &cmd, sizeof(cmd)) == 0, "輸出緩衝不足時回 0");

  uint8_t huge[300];
  check(hoPackPacket(buf, sizeof(buf), HO_PKT_CMD, 1, huge, 300) == 0, "payload 超長時回 0");

  // 正向邊界：payload 長度剛好等於上限時應該打包成功，不能被上面的超長檢查誤傷
  uint8_t maxPayload[HO_ESPNOW_MAX_PAYLOAD];
  memset(maxPayload, 0xAB, sizeof(maxPayload));
  size_t maxLen = hoPackPacket(buf, sizeof(buf), HO_PKT_CMD, 1, maxPayload, HO_ESPNOW_MAX_PAYLOAD);
  check(maxLen == sizeof(HoPacketHeader) + HO_ESPNOW_MAX_PAYLOAD, "payload 剛好等於上限時打包成功");
}

void testDeviceId() {
  Serial.println("── 設備 ID ──");
  const uint8_t mac[6] = { 0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5 };
  char id[20];
  hoFormatDeviceId(mac, id);
  check(strcmp(id, "hoban-a0b1c2d3e4f5") == 0, "MAC 轉設備 ID 正確");

  uint8_t back[6];
  check(hoParseMacFromDeviceId("hoban-a0b1c2d3e4f5", back), "設備 ID 解析成功");
  check(memcmp(mac, back, 6) == 0, "來回轉換結果一致");

  check(!hoParseMacFromDeviceId("hoban-xyz", back), "長度不符時拒絕");
  check(!hoParseMacFromDeviceId("relay-a0b1c2d3e4f5", back), "前綴不符時拒絕");
  check(!hoParseMacFromDeviceId("hoban-a0b1c2d3e4gg", back), "非十六進位字元時拒絕");

  // 迴歸測試：strtol 會吃掉前導空白與正負號，仍回報「完整消耗兩字元」，
  // 換成 isxdigit() 逐字元驗證後，這兩種畸形輸入必須被拒絕。
  check(!hoParseMacFromDeviceId("hoban- 0b1c2d3e4f5", back), "前導空白時拒絕");
  check(!hoParseMacFromDeviceId("hoban--0b1c2d3e4f5", back), "正負號時拒絕");
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // 等 USB CDC 列舉完成，否則前幾行會被吃掉

  Serial.println();
  Serial.println("═══ 齁控 ESP-NOW 協定測試 ═══");

  testStructSizes();
  testCrc();
  testPackUnpack();
  testRejectBadPackets();
  testDeviceId();

  Serial.println("──────────────────────");
  Serial.printf("執行 %d 項，失敗 %d 項\n", testsRun, testsFailed);
  if (testsFailed == 0) {
    Serial.println("ALL TESTS PASSED");
  } else {
    Serial.println("TESTS FAILED");
  }
}

void loop() {
  delay(1000);
}
