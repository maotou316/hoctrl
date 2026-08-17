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
  check(sizeof(HoCmdPayload) == 5, "HoCmdPayload 為 5 bytes");
  check(sizeof(HoStatePayload) == 12, "HoStatePayload 為 12 bytes");
}

void testCrc() {
  Serial.println("── CRC8 ──");
  const uint8_t data[] = { 0x01, 0x02, 0x03 };
  uint8_t a = hoCrc8(data, 3);
  uint8_t b = hoCrc8(data, 3);
  check(a == b, "同樣輸入產生同樣 CRC");

  const uint8_t other[] = { 0x01, 0x02, 0x04 };
  check(hoCrc8(data, 3) != hoCrc8(other, 3), "不同輸入產生不同 CRC");
  check(hoCrc8(nullptr, 0) == 0x00, "空資料 CRC 為 0");

  // hoFrameCrc 涵蓋標頭前 6 bytes + payload + 共享密鑰
  const uint8_t hdr[6] = { 0x48, 0x4F, 0x02, 0x10, 0x01, 0x00 };
  check(hoFrameCrc(hdr, data, 3) != hoCrc8(data, 3), "含標頭與密鑰的 CRC 與純 CRC 不同");
  const uint8_t hdr2[6] = { 0x48, 0x4F, 0x02, 0x21, 0x01, 0x00 };   // 只有 type 不同
  check(hoFrameCrc(hdr, data, 3) != hoFrameCrc(hdr2, data, 3), "type 不同會產生不同 CRC");
}

// 標頭被竄改時必須拒收（本 Task 的核心目的）。
//
// **這組測試擋住什麼**：標頭前 6 bytes（magic/version/type/seq）任一 bit 翻轉後，
// 舊實作（CRC 只算 payload）會照樣解包成功，新實作必須拒收。
//
// **它擋不住什麼**：
//   - CRC-8 只有 256 種值，**平均每 256 次隨機竄改就有一次會撞上正確值**。
//     這組測試只驗了 type 與 seq 各一種特定的竄改，不是「所有竄改都擋得住」。
//   - 擋不住重放：原封不動重送一封合法封包，CRC 完全正確。
//   - 擋不住知道共享密鑰的人偽造。
void testHeaderTamper() {
  Serial.println("── 標頭竄改偵測 ──");
  HoCmdPayload cmd = { HO_CMD_PULSE, 2000, 1234 };
  uint8_t buf[250];
  size_t len = hoPackPacket(buf, sizeof(buf), HO_PKT_CMD, 7, &cmd, sizeof(cmd));
  check(len > 0, "打包成功");
  check(hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "未竄改時可解包");

  buf[3] = HO_PKT_OTA_DATA;   // 竄改 type
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "type 被竄改時拒收");

  buf[3] = HO_PKT_CMD;        // 還原 type，改竄改 seq
  buf[4] ^= 0x01;
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "seq 被竄改時拒收");
}

// OTA payload 的大小必須與協定文件一致（跨端一致性）。
// 註：這幾項在 HoEspNowProtocol.h 已有同樣內容的 static_assert，編譯期就會擋下。
// 這裡重複一次是為了讓**燒到板子上**的人也看得到結果 —— 兩者不衝突。
void testOtaStructSizes() {
  Serial.println("── OTA 結構大小 ──");
  check(sizeof(HoOtaBeginPayload) == 26, "HoOtaBeginPayload 為 26 bytes");
  check(sizeof(HoOtaDataPayload) == 3,  "HoOtaDataPayload 為 3 bytes");
  check(sizeof(HoOtaEndPayload) == 6,   "HoOtaEndPayload 為 6 bytes");
  check(sizeof(HoOtaAckPayload) == 6,   "HoOtaAckPayload 為 6 bytes");
  check(sizeof(HoPacketHeader) + sizeof(HoOtaDataPayload) + HO_OTA_CHUNK_SIZE == 250,
        "OTA_DATA 封包總長剛好 250 bytes");
}

// 指令歸因欄位（協定版本 2）的來回一致性。
//
// **這組測試擋住什麼**：cmdId／lastCmdId 在打包解包之間被 padding 或欄位順序
// 搞錯位（那會讓 master 與 slave 安靜地對不起來）。
//
// **它擋不住什麼**：它**完全不驗行為** —— slave 有沒有在執行後正確填回
// lastCmdId、master 有沒有正確比對，這裡一項都測不到（那需要兩片板子）。
// 這只是一組結構與序列化的檢查。
void testCmdAttribution() {
  Serial.println("── 指令歸因欄位 ──");
  check(HO_CMD_ID_NONE == 0, "HO_CMD_ID_NONE 是 0");

  uint8_t buf[250];
  HoCmdPayload cmd = { HO_CMD_OFF, 0, 0xBEEF };
  size_t len = hoPackPacket(buf, sizeof(buf), HO_PKT_CMD, 3, &cmd, sizeof(cmd));
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  check(hoUnpackPacket(buf, len, nullptr, &payload, &payloadLen), "指令封包解包成功");
  HoCmdPayload outCmd;
  memcpy(&outCmd, payload, sizeof(outCmd));
  check(outCmd.cmdId == 0xBEEF, "cmdId 來回一致");
  check(outCmd.cmd == HO_CMD_OFF && outCmd.pulseMs == 0, "cmd 與 pulseMs 未被 cmdId 擠掉");

  HoStatePayload st = { 1, 1, 0, 0, 12345, 0xBEEF, HO_CMD_OFF, 4 };
  len = hoPackPacket(buf, sizeof(buf), HO_PKT_STATE, 4, &st, sizeof(st));
  check(hoUnpackPacket(buf, len, nullptr, &payload, &payloadLen), "狀態封包解包成功");
  HoStatePayload outSt;
  memcpy(&outSt, payload, sizeof(outSt));
  check(outSt.lastCmdId == 0xBEEF, "lastCmdId 來回一致");
  check(outSt.lastCmdKind == HO_CMD_OFF, "lastCmdKind 來回一致");
  check(outSt.lastCmdCount == 4, "lastCmdCount 來回一致");
  check(outSt.uptimeSec == 12345, "uptimeSec 未被新欄位擠掉");
}

// 版本不符必須被偵測到，而不是靜默丟棄（flag-day 的可見性）。
void testVersionMismatch() {
  Serial.println("── 版本不符偵測 ──");
  uint8_t buf[250];
  HoHeartbeatPayload hb = { 6, 0, 0, 0 };
  size_t len = hoPackPacket(buf, sizeof(buf), HO_PKT_HEARTBEAT, 1, &hb, sizeof(hb));

  uint8_t ver = 0;
  check(!hoPeekVersionMismatch(buf, len, &ver), "版本相符時不報");

  buf[2] = 1;   // 假裝是舊版 master 送來的
  check(hoPeekVersionMismatch(buf, len, &ver), "版本不符時回報");
  check(ver == 1, "回報的是對方的版本號");
  check(!hoUnpackPacket(buf, len, nullptr, nullptr, nullptr), "版本不符仍然拒收");

  buf[0] ^= 0xFF;   // magic 壞掉：那是別人的封包，不該報成版本不符
  check(!hoPeekVersionMismatch(buf, len, &ver), "magic 不符時不報版本不符");
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
  HoCmdPayload cmd = { HO_CMD_PULSE, 2000, 1 };
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
  testHeaderTamper();
  testOtaStructSizes();
  testCmdAttribution();
  testVersionMismatch();
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
