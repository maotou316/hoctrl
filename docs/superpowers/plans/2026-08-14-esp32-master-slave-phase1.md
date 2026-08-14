# ESP32 Master／Slave Phase 1 實作計畫：ESP-NOW 骨架與配對

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立 `ho_master1` 與 `ho_slave1` 兩個韌體，讓 master 能透過 ESP-NOW 配對並開關多台 slave 的繼電器，全程用序列埠指令驗證，暫不接 MQTT。

**Architecture:** 共用協定定義抽成本機 Arduino library `HoEspNow`（兩個 sketch 用 `--libraries` 參數引入，避免複製貼上不同步）。Master 廣播心跳帶出自己的 channel，slave 靠輪掃 1~13 頻道找到心跳後鎖定並記憶。配對走 `PAIR_REQ` / `PAIR_ACK` 交換 MAC，master 存 NVS、slave 存 EEPROM。

**Tech Stack:** Arduino ESP32 core 3.3.7、ESP-NOW（`esp_now.h`）、`Preferences`（NVS）、`EEPROM`、arduino-cli 1.3.1

**Spec:** `docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md`

## Global Constraints

- **繼電器安全鐵則**：`initRelayPins()` 必須是 `setup()` 的第一行，早於 `Serial.begin()`。ESP32-C3 的 GPIO 4/7 是 JTAG 腳，晚一行拉低就多一分 MOS 誤導通燒毀設備的風險。
- **變數命名**：結果變數用 `res`，不用 `result`。
- **除錯輸出**：序列埠 115200 baud。
- **JSON 記憶體**：用 `StaticJsonDocument`，不用 `DynamicJsonDocument`（Phase 1 用不到 JSON，Phase 2 才會）。
- **不新增外部工具鏈**：不導入 PlatformIO、不裝 pyserial。驗證一律走 `arduino-cli` 1.3.1（`A:\server\arduino-cli\arduino-cli.exe`）。
- **不修改現有 sketch**：`ho_relay1/2/3` 在 Phase 1 完全不動。工作區已有未提交的 1.6.0 修改，不要碰。
- **ESP-NOW 單包上限**：250 bytes。標頭 7 bytes，payload 上限 243 bytes。
- **Slave 數量上限**：20 台（`HO_ESPNOW_MAX_SLAVES`），不使用 ESP-NOW 原生加密（原生加密 peer 上限只有 6）。
- **語言**：所有註釋、序列埠輸出、commit 訊息一律繁體中文。

## 測試策略（與一般專案不同，先讀）

韌體沒有 PC 端測試框架可用（環境無 g++、專案已決議不導入 PlatformIO）。本計畫的測試分兩種：

1. **On-target 單元測試**（Task 1）：`ho_espnow_test` 這個 sketch 在真實 ESP32-C3 上跑 assert，
   結果印到序列埠。測的是真實平台的 struct 對齊與位元組序，比 PC 端測試更有價值。
2. **硬體在環驗證**（Task 2~7）：燒錄後用序列埠指令操作，比對預期輸出。
   每個 Task 都附精確的操作步驟與逐字預期輸出。

驗證用的序列埠讀取腳本（Task 1 建立，後續 Task 重複使用）：
`tools\serial_expect.ps1`，用 `arduino-cli monitor` 抓輸出並比對關鍵字。

**需要兩片板子**：一片當 master（ESP32 WROOM 或另一片 C3），一片當 slave（現有 C3 繼電器板）。
Task 1 只需要一片。

---

## File Structure

```
hoctrl_arduino/
├── libraries/
│   └── HoEspNow/                       # 共用協定 library（兩個 sketch 都引入）
│       ├── library.properties
│       └── src/
│           ├── HoEspNowProtocol.h      # 封包型別、struct、函式宣告
│           └── HoEspNowProtocol.cpp    # CRC8、打包、解包、MAC 格式化
├── ho_espnow_test/
│   └── ho_espnow_test.ino              # on-target 協定單元測試
├── ho_master1/
│   └── ho_master1.ino                  # Master 韌體
├── ho_slave1/
│   └── ho_slave1.ino                   # Slave 韌體
├── tools/
│   └── serial_expect.ps1               # 序列埠輸出比對腳本
└── flash.ps1                           # 加入 master/slave/test 三個新型號 + --libraries
```

責任切分：
- `HoEspNowProtocol.*` 只負責「位元組進、結構出」的純邏輯，不碰 ESP-NOW API，才能被 on-target 測試單獨驗證
- `ho_master1.ino` 負責名冊管理與指令分派
- `ho_slave1.ino` 負責 channel 鎖定與繼電器動作

---

## Task 1：共用協定 library 與 on-target 單元測試

建立協定定義與打包／解包邏輯，並用一個測試 sketch 在真實硬體上驗證。

**Files:**
- Create: `libraries/HoEspNow/library.properties`
- Create: `libraries/HoEspNow/src/HoEspNowProtocol.h`
- Create: `libraries/HoEspNow/src/HoEspNowProtocol.cpp`
- Create: `ho_espnow_test/ho_espnow_test.ino`
- Create: `tools/serial_expect.ps1`
- Modify: `flash.ps1`（加 `test` 型號與 `--libraries` 參數）

**Interfaces:**
- Produces（Task 2~7 全部依賴）：
  - `uint8_t hoCrc8(const uint8_t* data, size_t len)`
  - `uint8_t hoPayloadCrc(const uint8_t* payload, size_t len)` — 含共享密鑰
  - `size_t hoPackPacket(uint8_t* out, size_t outSize, HoPacketType type, uint16_t seq, const void* payload, size_t payloadLen)` — 回傳總長度，失敗回 0
  - `bool hoUnpackPacket(const uint8_t* data, size_t len, HoPacketHeader* outHeader, const uint8_t** outPayload, size_t* outPayloadLen)`
  - `void hoFormatDeviceId(const uint8_t mac[6], char out[20])` — 產生 `hoban-a0b1c2d3e4f5`
  - `bool hoParseMacFromDeviceId(const char* deviceId, uint8_t out[6])`
  - 型別：`HoPacketType`、`HoRelayCmd`、`HoPacketHeader`、`HoHeartbeatPayload`、`HoPairAckPayload`、`HoCmdPayload`、`HoStatePayload`
  - 常數：`HO_ESPNOW_MAGIC`、`HO_ESPNOW_VERSION`、`HO_ESPNOW_MAX_SLAVES`、`HO_ESPNOW_MAX_PAYLOAD`

- [ ] **Step 1: 建立 library 描述檔**

`libraries/HoEspNow/library.properties`：

```properties
name=HoEspNow
version=1.0.0
author=齁斑科技
maintainer=齁斑科技
sentence=齁控 master/slave ESP-NOW 通訊協定
paragraph=定義 master 與 slave 之間的 ESP-NOW 封包格式、CRC 驗證與打包解包函式，供 ho_master1 與 ho_slave1 共用。
category=Communication
url=
architectures=esp32
```

- [ ] **Step 2: 寫協定標頭檔**

`libraries/HoEspNow/src/HoEspNowProtocol.h`：

```cpp
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
```

- [ ] **Step 3: 寫協定實作**

`libraries/HoEspNow/src/HoEspNowProtocol.cpp`：

```cpp
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
```

- [ ] **Step 4: 寫 on-target 單元測試 sketch**

`ho_espnow_test/ho_espnow_test.ino`：

```cpp
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
```

- [ ] **Step 5: 寫序列埠比對腳本**

`tools/serial_expect.ps1`：

```powershell
# 讀取序列埠輸出並比對關鍵字，供韌體驗證使用。
#
# 用法：
#   .\tools\serial_expect.ps1 -Port COM13 -Expect "ALL TESTS PASSED" -Seconds 10
#   .\tools\serial_expect.ps1 -Port COM13 -Seconds 15            # 只印出，不比對
#
# 註：環境沒有 pyserial，這裡走 arduino-cli 1.3.1 的 monitor 指令。

param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Expect = '',
    [int]$Seconds = 10,
    [switch]$Reset   # 開始前先用 DTR 重置板子
)

$ErrorActionPreference = 'Stop'
$cli = 'A:\server\arduino-cli\arduino-cli.exe'
$logFile = Join-Path $env:TEMP "ho_serial_$(Get-Random).log"

if ($Reset) {
    # 開關一次序列埠讓板子重新啟動，才能抓到 setup() 的輸出
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port, 115200
        $sp.DtrEnable = $false
        $sp.Open()
        Start-Sleep -Milliseconds 200
        $sp.Close()
        Start-Sleep -Milliseconds 500
    } catch {
        Write-Host "重置序列埠失敗（可忽略）：$_" -ForegroundColor Yellow
    }
}

Write-Host "監聽 $Port，共 $Seconds 秒…" -ForegroundColor Cyan

$proc = Start-Process -FilePath $cli `
    -ArgumentList 'monitor', '-p', $Port, '--config', 'baudrate=115200', '--quiet' `
    -RedirectStandardOutput $logFile -PassThru -NoNewWindow

Start-Sleep -Seconds $Seconds
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 300

$output = if (Test-Path $logFile) { Get-Content $logFile -Raw -Encoding UTF8 } else { '' }
Remove-Item $logFile -ErrorAction SilentlyContinue

Write-Host "───── 序列埠輸出 ─────" -ForegroundColor Gray
Write-Host $output
Write-Host "──────────────────────" -ForegroundColor Gray

if ([string]::IsNullOrWhiteSpace($Expect)) { exit 0 }

if ($output -match [regex]::Escape($Expect)) {
    Write-Host "通過：找到「$Expect」" -ForegroundColor Green
    exit 0
} else {
    Write-Host "失敗：找不到「$Expect」" -ForegroundColor Red
    exit 1
}
```

- [ ] **Step 6: 擴充 flash.ps1**

三處改動。第一處，`param` 的 `ValidateSet` 加入新型號：

```powershell
param(
    [ValidateSet('1', '2', '3', 'master', 'slave', 'test')]
    [string]$Model = '2',
    [switch]$Upload,
    [switch]$KeepConfig,
    [string]$Port = ''
)
```

第二處，`$configs` 雜湊表尾端（`'3' = @{...}` 之後）加三筆。
master 用 ESP32 WROOM 的 FQBN（與型號 1 相同），slave 與 test 用 C3 的 FQBN（與型號 2 相同）：

```powershell
    'master' = @{
        Dir  = 'ho_master1'
        Fqbn = 'esp32:esp32:esp32'
        Label = 'hoMaster1 (ESP32 WROOM，ESP-NOW 主控)'
    }
    'slave' = @{
        Dir  = 'ho_slave1'
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'hoSlave1 (ESP32-C3，ESP-NOW 受控端)'
    }
    'test' = @{
        Dir  = 'ho_espnow_test'
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=default,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'ESP-NOW 協定測試'
    }
```

註：`test` 用 `PartitionScheme=default`，因為 `ho_espnow_test` 目錄沒有 `partitions.csv`。

第三處，編譯指令加 `--libraries` 讓兩個 sketch 找得到 `HoEspNow`。
把原本第 69 行的：

```powershell
& $cli compile --fqbn $fqbn --output-dir $outDir $sketch
```

改成：

```powershell
$libDir = Join-Path $root 'libraries'
& $cli compile --fqbn $fqbn --libraries $libDir --output-dir $outDir $sketch
```

- [ ] **Step 7: 編譯測試 sketch，確認通過**

Run：
```powershell
.\flash.ps1 -Model test
```

Expected：`編譯完成 → ...\ho_espnow_test\build\vscode`，exit code 0。

若編譯報 `HoEspNowProtocol.h: No such file or directory`，表示 `--libraries` 沒生效，
檢查 `libraries/HoEspNow/src/` 路徑與 `library.properties` 是否齊全。

- [ ] **Step 8: 燒錄並驗證測試全數通過**

Run：
```powershell
.\flash.ps1 -Model test -Upload
.\tools\serial_expect.ps1 -Port COM13 -Expect "ALL TESTS PASSED" -Seconds 10 -Reset
```

（`COM13` 換成實際埠號，`flash.ps1` 燒錄時會印出偵測到的埠）

Expected：序列埠輸出每項都是 `[PASS]`，最後兩行為
```
執行 32 項，失敗 0 項
ALL TESTS PASSED
```
（32 = struct 大小 5 + CRC 4 + 打包解包 8 + 拒收異常 7 + 設備 ID 8）
腳本 exit code 0。

**若 struct 大小測試失敗**：表示 `__attribute__((packed))` 沒生效或編譯器有額外對齊，
不要改測試去遷就，要改 struct 定義（欄位重排讓自然對齊，或確認 packed 屬性拼寫正確）。
這項失敗放著不管，master 與 slave 之間會出現詭異的欄位錯位。

- [ ] **Step 9: Commit**

```bash
git add libraries/ ho_espnow_test/ tools/serial_expect.ps1 flash.ps1
git commit -m "新增 ESP-NOW 共用協定 library 與 on-target 測試

- HoEspNow library：封包格式、CRC8（含共享密鑰）、打包解包、設備 ID 轉換
- ho_espnow_test：在真實 ESP32-C3 上驗證 struct 對齊與協定行為，32 項全過
- tools/serial_expect.ps1：用 arduino-cli monitor 抓序列埠輸出並比對
- flash.ps1：新增 master/slave/test 三個型號，編譯加 --libraries

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 2：Master 骨架與心跳廣播

建立 master 韌體，初始化 ESP-NOW 並每 5 秒廣播一次心跳。

**Files:**
- Create: `ho_master1/ho_master1.ino`

**Interfaces:**
- Consumes: Task 1 的 `hoPackPacket()`、`HoHeartbeatPayload`、`hoFormatDeviceId()`
- Produces（Task 3~7 依賴）：
  - `const char* getDeviceId()` — 回傳 `hoban-xxxxxxxxxxxx`
  - `void sendHeartbeat()`
  - `bool espNowSendTo(const uint8_t mac[6], HoPacketType type, const void* payload, size_t len)`
  - 全域：`bool pairingMode`、`uint8_t currentChannel`、`bool longRangeEnabled`
  - 廣播位址常數 `BROADCAST_MAC`

- [ ] **Step 1: 先用最小 sketch 確認 callback 簽名**

ESP32 core 3.3.7 的 ESP-NOW callback 簽名與 2.x 不同，且 3.3 系列改用 `wifi_tx_info_t`。
先寫一支只有 callback 的空 sketch 編譯，確認簽名正確再往下做，
否則後面的程式碼會卡在一堆型別錯誤。

暫時寫入 `ho_master1/ho_master1.ino`：

```cpp
#include <WiFi.h>
#include <esp_now.h>

// 確認 core 3.3.7 的 callback 簽名
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {}
void onSent(const wifi_tx_info_t* txInfo, esp_now_send_status_t status) {}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
}
void loop() {}
```

Run：`.\flash.ps1 -Model master`

Expected：編譯成功。

**若報 `invalid conversion from void(*)(const wifi_tx_info_t*, ...)`**：
表示這版 core 的送出 callback 仍是舊簽名，把 `onSent` 改成：
```cpp
void onSent(const uint8_t* mac, esp_now_send_status_t status) {}
```
重新編譯確認。**記下哪一種可用，Step 2 的完整實作要跟著改。**

- [ ] **Step 2: 寫 master 完整骨架**

用以下內容覆蓋 `ho_master1/ho_master1.ino`（`onEspNowSent` 的簽名依 Step 1 的結果調整）：

```cpp
// 齁控 Master — ESP-NOW 主控端
// 硬體：ESP32 WROOM DevKit（GPIO 定義與 ho_relay1 一致）
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HoEspNowProtocol.h>

const char* firmwareVersion = "1.0.0";
const char* deviceModel = "hoMaster1";

// ── GPIO（ESP32 WROOM，沿用 ho_relay1 的定義）──
const int bootButton = 0;
const int secondButton = 14;
const int ledOnBoard = 2;
const int relayPins[] = { 13 };
const int relayPinCount = sizeof(relayPins) / sizeof(relayPins[0]);

// ── 全域狀態 ──
bool relayState = false;
bool pairingMode = false;
uint8_t currentChannel = 1;
bool longRangeEnabled = false;
String deviceIdString;
uint16_t txSeq = 0;

const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const unsigned long HEARTBEAT_INTERVAL = 5000;

// ── 繼電器 ──
// 與 ho_relay2 相同的鐵則：initRelayPins() 必須是 setup() 第一行
void initRelayPins() {
  for (int i = 0; i < relayPinCount; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }
  relayState = false;
}

// ── 設備 ID ──
const char* getDeviceId() {
  if (deviceIdString.length() == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[20];
    hoFormatDeviceId(mac, buf);
    deviceIdString = String(buf);
  }
  return deviceIdString.c_str();
}

// ── ESP-NOW callbacks ──
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  HoPacketHeader header;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  if (!hoUnpackPacket(data, (size_t)len, &header, &payload, &payloadLen)) {
    return;  // 非本系統封包或 CRC 不符，靜默丟棄
  }

  char senderId[20];
  hoFormatDeviceId(info->src_addr, senderId);
  Serial.printf("[ESP-NOW] 收到 type=0x%02x seq=%u 來自 %s rssi=%d\n",
                header.type, header.seq, senderId, info->rx_ctrl->rssi);
  // 各型別的處理在 Task 4、Task 5 補上
}

void onEspNowSent(const wifi_tx_info_t* txInfo, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[ESP-NOW] 送出失敗");
  }
}

// ── 送出封包 ──
bool espNowSendTo(const uint8_t mac[6], HoPacketType type,
                  const void* payload, size_t len) {
  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), type, txSeq++, payload, len);
  if (total == 0) {
    Serial.println("[ESP-NOW] 打包失敗");
    return false;
  }
  esp_err_t res = esp_now_send(mac, buf, total);
  if (res != ESP_OK) {
    Serial.printf("[ESP-NOW] esp_now_send 失敗: %d\n", res);
    return false;
  }
  return true;
}

// ── 心跳廣播 ──
void sendHeartbeat() {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);
  currentChannel = primary;

  HoHeartbeatPayload hb;
  hb.channel = currentChannel;
  hb.pairingMode = pairingMode ? 1 : 0;
  hb.longRange = longRangeEnabled ? 1 : 0;
  hb.slaveCount = 0;  // Task 4 接上名冊後改成實際數量

  espNowSendTo(BROADCAST_MAC, HO_PKT_HEARTBEAT, &hb, sizeof(hb));
  Serial.printf("[心跳] channel=%u 配對模式=%s slave=%u\n",
                hb.channel, hb.pairingMode ? "是" : "否", hb.slaveCount);
}

// ── ESP-NOW 初始化 ──
void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // Phase 1 不連 WiFi，Phase 2 才接上

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失敗，重啟");
    delay(2000);
    ESP.restart();
  }

  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);

  // 廣播位址也必須註冊成 peer 才能送出
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;  // 0 = 沿用目前 channel
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("加入廣播 peer 失敗");
  }

  uint8_t primary = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);
  currentChannel = primary;

  Serial.printf("ESP-NOW 就緒，channel=%u\n", currentChannel);
}

void setup() {
  initRelayPins();  // 必須第一行

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("齁控 Master v" + String(firmwareVersion));
  Serial.println("================");

  pinMode(ledOnBoard, OUTPUT);
  digitalWrite(ledOnBoard, LOW);
  pinMode(bootButton, INPUT_PULLUP);
  pinMode(secondButton, INPUT_PULLUP);

  setupEspNow();

  Serial.printf("設備 ID: %s\n", getDeviceId());
  Serial.println("就緒");
}

void loop() {
  static unsigned long lastHeartbeat = 0;
  unsigned long now = millis();

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
}
```

- [ ] **Step 3: 編譯**

Run：`.\flash.ps1 -Model master`

Expected：編譯成功，無警告中斷。

- [ ] **Step 4: 燒錄並驗證心跳**

Run：
```powershell
.\flash.ps1 -Model master -Upload
.\tools\serial_expect.ps1 -Port COM16 -Expect "[心跳]" -Seconds 15 -Reset
```

Expected：開機訊息出現設備 ID 與 `ESP-NOW 就緒，channel=1`，
之後每 5 秒一行 `[心跳] channel=1 配對模式=否 slave=0`，15 秒內至少 2 行。

**若出現 `[ESP-NOW] 送出失敗`**：檢查廣播 peer 有沒有加成功。
廣播封包不會有 ACK，正常情況下 `status` 一律回 `ESP_NOW_SEND_SUCCESS`。

- [ ] **Step 5: Commit**

```bash
git add ho_master1/
git commit -m "新增 Master 韌體骨架與 ESP-NOW 心跳廣播

- ESP-NOW 初始化、廣播 peer 註冊、收送 callback
- 每 5 秒廣播一次心跳，帶出目前 channel 與配對模式狀態
- 沿用 ho_relay1 的 WROOM GPIO 定義，保留繼電器驅動（硬體可接可不接）

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 3：Slave 骨架與 channel 掃描鎖定

Slave 不連 WiFi，靠輪掃 1~13 頻道找到 master 的心跳後鎖定。

**Files:**
- Create: `ho_slave1/partitions.csv`
- Create: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes: Task 1 全部函式、Task 2 定義的心跳封包格式
- Produces（Task 4~7 依賴）：
  - `void setChannel(uint8_t ch)`
  - `void startChannelScan()` / `void onMasterFound(const uint8_t mac[6], uint8_t channel)`
  - 全域：`uint8_t masterMac[6]`、`bool masterKnown`、`unsigned long lastHeartbeatTime`、`uint8_t lockedChannel`

- [ ] **Step 1: 複製分區表**

`flash.ps1` 的 slave 設定用 `PartitionScheme=custom`，**沒有這個檔案編譯會直接失敗**。
雙 OTA 分區是 Phase 4 轉送 OTA 的前提，現在就要備好。

Run：
```powershell
Copy-Item ho_relay2\partitions.csv ho_slave1\partitions.csv
```

`ho_slave1/partitions.csv` 內容（與 ho_relay2 相同）：

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1F0000,
app1,     app,  ota_1,   0x200000,0x1F0000,
coredump, data, coredump,0x3F0000,0x8000,
spiffs,   data, spiffs,  0x3F8000,0x8000,
```

- [ ] **Step 2: 寫 slave 骨架**

`ho_slave1/ho_slave1.ino`：

```cpp
// 齁控 Slave — ESP-NOW 受控端
// 硬體：現有 ho_relay2 的 ESP32-C3 繼電器板
// 不連 WiFi、不跑 MQTT、不跑 BLE，只靠 ESP-NOW 接受 master 控制
#include <Arduino.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HoEspNowProtocol.h>

const char* firmwareVersion = "1.0.0";
const char* deviceModel = "hoSlave1";

// ── GPIO（與 ho_relay2 完全一致）──
const int bootButton = 9;
const int resetButton = 1;
const int ledOnBoard = 3;
const int ledOnFace = 0;
const int relayPins[] = { 4, 7 };  // 兩版 PCB 走線不同，兩支同時驅動
const int relayPinCount = sizeof(relayPins) / sizeof(relayPins[0]);

// ── EEPROM 佈局（32 bytes 就夠，不沿用 ho_relay2 的 128）──
#define EEPROM_SIZE        32
#define EE_ADDR_MAGIC      0   // 1 byte，0x5A 表示已配對
#define EE_ADDR_MASTER_MAC 1   // 6 bytes
#define EE_ADDR_CHANNEL    7   // 1 byte
#define EE_ADDR_LONGRANGE  8   // 1 byte
#define EE_MAGIC_PAIRED    0x5A

// ── 全域狀態 ──
bool relayState = false;
uint8_t masterMac[6] = { 0 };
bool masterKnown = false;         // EEPROM 裡有 master 記錄
uint8_t lockedChannel = 0;        // 已鎖定的 channel，0 = 未鎖定
unsigned long lastHeartbeatTime = 0;
uint16_t txSeq = 0;

// 掃描狀態
bool scanning = false;
uint8_t scanChannel = 1;
unsigned long scanChannelStart = 0;
const unsigned long SCAN_DWELL_MS = 600;      // 每個 channel 停留時間
const unsigned long HEARTBEAT_TIMEOUT = 30000; // 超過這麼久沒心跳就重掃

const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ── 繼電器（鐵則：initRelayPins() 必須是 setup() 第一行）──
void initRelayPins() {
  for (int i = 0; i < relayPinCount; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }
  relayState = false;
}

void setRelayPins(bool on) {
  for (int i = 0; i < relayPinCount; i++) {
    digitalWrite(relayPins[i], on ? HIGH : LOW);
  }
  digitalWrite(ledOnFace, on ? HIGH : LOW);
  digitalWrite(ledOnBoard, on ? HIGH : LOW);
  relayState = on;
}

// ── 設備 ID ──
String deviceIdString;
const char* getDeviceId() {
  if (deviceIdString.length() == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[20];
    hoFormatDeviceId(mac, buf);
    deviceIdString = String(buf);
  }
  return deviceIdString.c_str();
}

// ── EEPROM ──
void loadPairing() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EE_ADDR_MAGIC) != EE_MAGIC_PAIRED) {
    masterKnown = false;
    Serial.println("EEPROM 無配對記錄");
    return;
  }
  for (int i = 0; i < 6; i++) {
    masterMac[i] = EEPROM.read(EE_ADDR_MASTER_MAC + i);
  }
  lockedChannel = EEPROM.read(EE_ADDR_CHANNEL);
  if (lockedChannel < 1 || lockedChannel > 13) lockedChannel = 1;
  masterKnown = true;

  char id[20];
  hoFormatDeviceId(masterMac, id);
  Serial.printf("已配對 master: %s，上次 channel=%u\n", id, lockedChannel);
}

void savePairing() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC_PAIRED);
  for (int i = 0; i < 6; i++) {
    EEPROM.write(EE_ADDR_MASTER_MAC + i, masterMac[i]);
  }
  EEPROM.write(EE_ADDR_CHANNEL, lockedChannel);
  EEPROM.commit();
}

// ── Channel 控制 ──
void setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[channel] 切換到 %u\n", ch);
}

void startChannelScan() {
  if (scanning) return;
  scanning = true;
  scanChannel = 1;
  scanChannelStart = millis();
  setChannel(scanChannel);
  Serial.println("[掃描] 開始輪掃 channel 1~13 尋找 master");
}

void onMasterFound(const uint8_t mac[6], uint8_t channel) {
  bool isNewMaster = !masterKnown || memcmp(masterMac, mac, 6) != 0;
  bool channelChanged = (lockedChannel != channel);

  memcpy(masterMac, mac, 6);
  lockedChannel = channel;
  scanning = false;
  lastHeartbeatTime = millis();

  if (isNewMaster || channelChanged) {
    char id[20];
    hoFormatDeviceId(mac, id);
    Serial.printf("[鎖定] master=%s channel=%u\n", id, channel);
    if (masterKnown) savePairing();  // 只有已配對過才更新 EEPROM
  }
}

// ── ESP-NOW ──
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  HoPacketHeader header;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  if (!hoUnpackPacket(data, (size_t)len, &header, &payload, &payloadLen)) {
    return;
  }

  if (header.type == HO_PKT_HEARTBEAT && payloadLen >= sizeof(HoHeartbeatPayload)) {
    HoHeartbeatPayload hb;
    memcpy(&hb, payload, sizeof(hb));

    // 已配對的 slave 只理會自己的 master
    if (masterKnown && memcmp(masterMac, info->src_addr, 6) != 0) {
      return;
    }

    char id[20];
    hoFormatDeviceId(info->src_addr, id);
    Serial.printf("[心跳] 來自 %s channel=%u 配對模式=%s rssi=%d\n",
                  id, hb.channel, hb.pairingMode ? "是" : "否", info->rx_ctrl->rssi);

    onMasterFound(info->src_addr, hb.channel);
    return;
  }
  // 其餘型別在 Task 4、Task 5 補上
}

void onEspNowSent(const wifi_tx_info_t* txInfo, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[ESP-NOW] 送出失敗");
  }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // slave 永遠不連 AP

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失敗，重啟");
    delay(2000);
    ESP.restart();
  }

  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);

  Serial.println("ESP-NOW 就緒");
}

void setup() {
  initRelayPins();  // 必須第一行

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("齁控 Slave v" + String(firmwareVersion));
  Serial.println("================");

  pinMode(ledOnBoard, OUTPUT);
  digitalWrite(ledOnBoard, LOW);
  pinMode(ledOnFace, OUTPUT);
  digitalWrite(ledOnFace, LOW);
  pinMode(bootButton, INPUT_PULLUP);
  pinMode(resetButton, INPUT_PULLUP);

  loadPairing();
  setupEspNow();

  Serial.printf("設備 ID: %s\n", getDeviceId());

  if (masterKnown) {
    setChannel(lockedChannel);   // 先試上次的 channel
    lastHeartbeatTime = millis();
  } else {
    startChannelScan();
  }

  Serial.println("就緒");
}

void loop() {
  unsigned long now = millis();

  // 掃描中：每個 channel 停留 SCAN_DWELL_MS 後換下一個
  if (scanning) {
    if (now - scanChannelStart >= SCAN_DWELL_MS) {
      scanChannel = (scanChannel % 13) + 1;
      scanChannelStart = now;
      setChannel(scanChannel);
    }
    return;
  }

  // 已鎖定：超時沒收到心跳就重新掃描
  if (now - lastHeartbeatTime > HEARTBEAT_TIMEOUT) {
    Serial.println("[失聯] 超過 30 秒沒收到心跳");
    startChannelScan();
  }
}
```

- [ ] **Step 3: 編譯**

Run：`.\flash.ps1 -Model slave`

Expected：編譯成功。

**若報 `Error: Cannot find partitions.csv`**：Step 1 的複製沒做成功，回頭確認。

- [ ] **Step 4: 燒錄 slave 並驗證掃描啟動**

此時 master 先斷電，確認 slave 在找不到 master 時會持續輪掃。

Run：
```powershell
.\flash.ps1 -Model slave -Upload
.\tools\serial_expect.ps1 -Port COM13 -Expect "[掃描] 開始輪掃" -Seconds 12 -Reset
```

Expected：
```
EEPROM 無配對記錄
ESP-NOW 就緒
設備 ID: hoban-xxxxxxxxxxxx
[掃描] 開始輪掃 channel 1~13 尋找 master
[channel] 切換到 1
[channel] 切換到 2
...
```
12 秒內應看到 channel 從 1 數到 13 再繞回 1（每個 600ms，一輪約 7.8 秒）。

- [ ] **Step 5: 兩片板子端到端驗證鎖定**

Master 上電（Task 2 已燒好），slave 保持運行。

Run：
```powershell
.\tools\serial_expect.ps1 -Port COM13 -Expect "[鎖定] master=" -Seconds 20
```

Expected：slave 輪掃到 master 所在的 channel 時收到心跳，輸出：
```
[心跳] 來自 hoban-<masterMac> channel=1 配對模式=否 rssi=-42
[鎖定] master=hoban-<masterMac> channel=1
```
之後每 5 秒一行心跳，不再出現 `[channel] 切換到`。

**若一直掃不到**：確認兩片板子的 `HO_ESPNOW_SHARED_KEY` 一致（都來自同一份 library），
以及 master 確實在發心跳（另開一個視窗看 master 的序列埠）。

**若收到心跳但沒有 `[鎖定]`**：檢查 `onMasterFound()` 的條件判斷。
首次發現時 `masterKnown` 為 false，`isNewMaster` 應為 true。

- [ ] **Step 6: Commit**

```bash
git add ho_slave1/
git commit -m "新增 Slave 韌體骨架與 channel 掃描鎖定

- 沿用 ho_relay2 的雙 OTA 分區表，為 Phase 4 的轉送 OTA 預留空間
- 不連 WiFi，只跑 ESP-NOW，EEPROM 只用 32 bytes 存 master MAC 與 channel
- 找不到 master 時輪掃 channel 1~13，每個停留 600ms
- 收到心跳即鎖定 channel，超過 30 秒沒心跳自動重新掃描
- 沿用 ho_relay2 的 GPIO 與繼電器安全初始化順序

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 4：配對流程與名冊儲存

Master 短按進入配對模式，slave 短按送出請求，雙方交換 MAC 並持久化。

**Files:**
- Modify: `ho_master1/ho_master1.ino`
- Modify: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes: Task 2 的 `espNowSendTo()`、Task 3 的 `savePairing()`
- Produces（Task 5~7 依賴）：
  - Master：`struct SlaveEntry { uint8_t mac[6]; bool online; int8_t rssi; unsigned long lastSeen; }`
  - Master：`SlaveEntry slaves[HO_ESPNOW_MAX_SLAVES]`、`int slaveCount`
  - Master：`int findSlave(const uint8_t mac[6])`（找不到回 -1）、`bool addSlave(const uint8_t mac[6])`、`void saveSlaves()`、`void loadSlaves()`
  - Master：`void enterPairingMode()` / `void exitPairingMode()`
  - Slave：`void requestPairing()`

- [ ] **Step 1: Master 加入名冊與 NVS 儲存**

在 `ho_master1.ino` 的 `#include` 區塊加入：

```cpp
#include <Preferences.h>
```

在全域狀態區塊（`uint16_t txSeq = 0;` 之後）加入：

```cpp
// ── Slave 名冊 ──
struct SlaveEntry {
  uint8_t mac[6];
  bool online;
  int8_t rssi;
  unsigned long lastSeen;
};

SlaveEntry slaves[HO_ESPNOW_MAX_SLAVES];
int slaveCount = 0;
Preferences prefs;

// 配對模式
unsigned long pairingStartTime = 0;
const unsigned long PAIRING_TIMEOUT = 60000;  // 60 秒
// slave 超過這麼久沒回應就標記離線
const unsigned long SLAVE_OFFLINE_TIMEOUT = 30000;
```

在 `getDeviceId()` 之後加入名冊管理函式：

```cpp
// ── 名冊管理 ──
int findSlave(const uint8_t mac[6]) {
  for (int i = 0; i < slaveCount; i++) {
    if (memcmp(slaves[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

void saveSlaves() {
  prefs.begin("homaster", false);
  prefs.putInt("count", slaveCount);
  // 只存 MAC，online/rssi/lastSeen 是執行期狀態不需持久化
  uint8_t macs[HO_ESPNOW_MAX_SLAVES * 6];
  for (int i = 0; i < slaveCount; i++) {
    memcpy(macs + i * 6, slaves[i].mac, 6);
  }
  prefs.putBytes("macs", macs, slaveCount * 6);
  prefs.end();
  Serial.printf("[名冊] 已儲存 %d 台\n", slaveCount);
}

void loadSlaves() {
  prefs.begin("homaster", true);
  slaveCount = prefs.getInt("count", 0);
  if (slaveCount < 0 || slaveCount > HO_ESPNOW_MAX_SLAVES) slaveCount = 0;

  if (slaveCount > 0) {
    uint8_t macs[HO_ESPNOW_MAX_SLAVES * 6];
    size_t got = prefs.getBytes("macs", macs, sizeof(macs));
    if (got != (size_t)slaveCount * 6) {
      Serial.println("[名冊] 資料長度不符，重置為空");
      slaveCount = 0;
    } else {
      for (int i = 0; i < slaveCount; i++) {
        memcpy(slaves[i].mac, macs + i * 6, 6);
        slaves[i].online = false;
        slaves[i].rssi = 0;
        slaves[i].lastSeen = 0;
      }
    }
  }
  prefs.end();

  Serial.printf("[名冊] 載入 %d 台 slave\n", slaveCount);
  for (int i = 0; i < slaveCount; i++) {
    char id[20];
    hoFormatDeviceId(slaves[i].mac, id);
    Serial.printf("  %d. %s\n", i + 1, id);
  }
}

// 把 slave 註冊成 ESP-NOW peer，才能對它單播
bool registerPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  esp_err_t res = esp_now_add_peer(&peer);
  if (res != ESP_OK) {
    Serial.printf("[名冊] 註冊 peer 失敗: %d\n", res);
    return false;
  }
  return true;
}

bool addSlave(const uint8_t mac[6]) {
  if (findSlave(mac) >= 0) return true;   // 已存在視為成功
  if (slaveCount >= HO_ESPNOW_MAX_SLAVES) return false;

  memcpy(slaves[slaveCount].mac, mac, 6);
  slaves[slaveCount].online = true;
  slaves[slaveCount].rssi = 0;
  slaves[slaveCount].lastSeen = millis();
  slaveCount++;

  registerPeer(mac);
  saveSlaves();
  return true;
}

void enterPairingMode() {
  pairingMode = true;
  pairingStartTime = millis();
  Serial.println("[配對] 進入配對模式，60 秒內請短按 slave 的按鈕");
  sendHeartbeat();  // 立刻發一次，讓 slave 早點知道
}

void exitPairingMode() {
  if (!pairingMode) return;
  pairingMode = false;
  Serial.println("[配對] 離開配對模式");
}
```

- [ ] **Step 2: Master 處理配對請求**

把 `onEspNowRecv()` 中「各型別的處理在 Task 4、Task 5 補上」那行註釋，換成：

```cpp
  if (header.type == HO_PKT_PAIR_REQ) {
    HoPairAckPayload ack;
    ack.channel = currentChannel;
    ack.longRange = longRangeEnabled ? 1 : 0;

    if (!pairingMode) {
      ack.accepted = 0;
      ack.reason = HO_PAIR_NOT_PAIRING;
      Serial.printf("[配對] 拒絕 %s：不在配對模式\n", senderId);
    } else if (!addSlave(info->src_addr)) {
      ack.accepted = 0;
      ack.reason = HO_PAIR_FULL;
      Serial.printf("[配對] 拒絕 %s：已達 %d 台上限\n", senderId, HO_ESPNOW_MAX_SLAVES);
    } else {
      ack.accepted = 1;
      ack.reason = HO_PAIR_OK;
      Serial.printf("[配對] 接受 %s，目前共 %d 台\n", senderId, slaveCount);
    }

    // 回覆前必須先註冊 peer，否則單播送不出去
    // 注意：一定要在 registerPeer() 之前記錄 wasPeer，且只在「原本就不是 peer」
    // 又被拒絕時才刪除。若沒有這個判斷，已配對成功的 slave 若因故重送
    // PAIR_REQ（例如剛好 master 不在配對模式），會被誤刪 peer 而收不到後續指令。
    bool wasPeer = esp_now_is_peer_exist(info->src_addr);
    registerPeer(info->src_addr);
    espNowSendTo(info->src_addr, HO_PKT_PAIR_ACK, &ack, sizeof(ack));
    if (!ack.accepted && !wasPeer) {
      // 被拒絕又不是原本就存在的 peer：這只是為了送出 ACK 而暫時註冊，
      // 用完就刪，避免陌生設備灌爆 20 台的 peer 表上限。
      esp_now_del_peer(info->src_addr);
    }
    return;
  }

  if (header.type == HO_PKT_UNPAIR) {
    int idx = findSlave(info->src_addr);
    if (idx >= 0) {
      for (int i = idx; i < slaveCount - 1; i++) slaves[i] = slaves[i + 1];
      slaveCount--;
      esp_now_del_peer(info->src_addr);
      saveSlaves();
      Serial.printf("[配對] %s 已解除配對，剩 %d 台\n", senderId, slaveCount);
    }
    return;
  }
```

- [ ] **Step 3: Master 加入按鈕與配對逾時**

在 `setup()` 的 `setupEspNow();` 之前插入 `loadSlaves();`，
並把 `sendHeartbeat()` 裡的 `hb.slaveCount = 0;` 改成：

```cpp
  hb.slaveCount = (uint8_t)slaveCount;
```

在 `loop()` 開頭（`unsigned long now = millis();` 之後）加入按鈕與逾時處理：

```cpp
  // ── 短按 BOOT 進入配對模式 ──
  // 長按 3 秒以上不觸發，保留給之後的重置功能
  static bool lastButtonState = HIGH;
  static unsigned long buttonDownTime = 0;
  bool buttonState = digitalRead(bootButton);

  if (lastButtonState == HIGH && buttonState == LOW) {
    buttonDownTime = now;
  }
  if (lastButtonState == LOW && buttonState == HIGH) {
    unsigned long pressDuration = now - buttonDownTime;
    if (pressDuration >= 50 && pressDuration < 1000) {   // 50ms 去彈跳，1 秒內算短按
      if (pairingMode) exitPairingMode(); else enterPairingMode();
    }
  }
  lastButtonState = buttonState;

  // ── 配對模式逾時 ──
  if (pairingMode && now - pairingStartTime >= PAIRING_TIMEOUT) {
    Serial.println("[配對] 逾時");
    exitPairingMode();
  }

  // ── 配對模式時 LED 慢閃 ──
  if (pairingMode) {
    digitalWrite(ledOnBoard, ((now / 500) % 2) ? HIGH : LOW);
  }

  // ── 配對模式時心跳加快到 1 秒，讓 slave 更快找到 ──
  static unsigned long lastPairingHeartbeat = 0;
  if (pairingMode && now - lastPairingHeartbeat >= 1000) {
    lastPairingHeartbeat = now;
    sendHeartbeat();
  }
```

- [ ] **Step 4: Slave 送出配對請求並處理 ACK**

在 `ho_slave1.ino` 的全域狀態區塊加入：

```cpp
bool waitingPairAck = false;
unsigned long pairReqTime = 0;
const unsigned long PAIR_ACK_TIMEOUT = 5000;
bool masterInPairingMode = false;   // 從心跳得知 master 是否在配對模式
```

在 `onMasterFound()` 之後加入：

```cpp
// LED 快閃指定次數（阻塞，只在配對結果時用）
void blinkLed(int times, int intervalMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(ledOnFace, HIGH);
    digitalWrite(ledOnBoard, HIGH);
    delay(intervalMs);
    digitalWrite(ledOnFace, LOW);
    digitalWrite(ledOnBoard, LOW);
    delay(intervalMs);
  }
}

void requestPairing() {
  if (lockedChannel == 0) {
    Serial.println("[配對] 還沒找到 master，先開始掃描");
    startChannelScan();
    return;
  }
  if (!masterInPairingMode) {
    Serial.println("[配對] master 不在配對模式，請先短按 master 的按鈕");
    return;
  }

  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), HO_PKT_PAIR_REQ, txSeq++, nullptr, 0);
  esp_now_send(masterMac, buf, total);

  waitingPairAck = true;
  pairReqTime = millis();
  Serial.println("[配對] 已送出配對請求，等待回覆");
}
```

在 `onEspNowRecv()` 的心跳處理區塊內，`onMasterFound(...)` 之前加入一行記錄配對模式：

```cpp
    masterInPairingMode = (hb.pairingMode == 1);
```

在 `onEspNowRecv()` 尾端的「其餘型別在 Task 4、Task 5 補上」註釋處，換成：

```cpp
  if (header.type == HO_PKT_PAIR_ACK && payloadLen >= sizeof(HoPairAckPayload)) {
    HoPairAckPayload ack;
    memcpy(&ack, payload, sizeof(ack));
    waitingPairAck = false;

    if (ack.accepted) {
      memcpy(masterMac, info->src_addr, 6);
      lockedChannel = ack.channel;
      masterKnown = true;
      savePairing();

      char id[20];
      hoFormatDeviceId(masterMac, id);
      Serial.printf("[配對] 成功，master=%s channel=%u\n", id, ack.channel);
      blinkLed(3, 100);   // 快閃 3 下表示成功
    } else {
      const char* why = (ack.reason == HO_PAIR_FULL) ? "master 已達 20 台上限"
                      : (ack.reason == HO_PAIR_NOT_PAIRING) ? "master 不在配對模式"
                      : "未知原因";
      Serial.printf("[配對] 被拒絕：%s\n", why);
      blinkLed(3, 400);   // 慢閃 3 下表示失敗
    }
    return;
  }
```

- [ ] **Step 5: Slave 加入按鈕觸發**

在 `ho_slave1.ino` 的 `loop()` 開頭（`unsigned long now = millis();` 之後）加入：

```cpp
  // ── 短按 BOOT 送出配對請求 ──
  static bool lastButtonState = HIGH;
  static unsigned long buttonDownTime = 0;
  bool buttonState = digitalRead(bootButton);

  if (lastButtonState == HIGH && buttonState == LOW) {
    buttonDownTime = now;
  }
  if (lastButtonState == LOW && buttonState == HIGH) {
    unsigned long pressDuration = now - buttonDownTime;
    if (pressDuration >= 50 && pressDuration < 1000) {
      requestPairing();
    }
  }
  lastButtonState = buttonState;

  // ── 配對請求逾時 ──
  if (waitingPairAck && now - pairReqTime >= PAIR_ACK_TIMEOUT) {
    waitingPairAck = false;
    Serial.println("[配對] 等待回覆逾時");
  }
```

- [ ] **Step 6: 編譯兩支韌體**

Run：
```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model slave
```

Expected：兩支都編譯成功。

- [ ] **Step 7: 端到端驗證配對**

燒錄兩片板子，兩個序列埠各開一個視窗。

Run（先燒錄）：
```powershell
.\flash.ps1 -Model master -Upload -Port COM16
.\flash.ps1 -Model slave -Upload -Port COM13
```

操作順序：
1. 短按 master 的 BOOT 按鈕
2. 5 秒內短按 slave 的 BOOT 按鈕

Master 序列埠 Expected：
```
[配對] 進入配對模式，60 秒內請短按 slave 的按鈕
[心跳] channel=1 配對模式=是 slave=0
[ESP-NOW] 收到 type=0x02 seq=... 來自 hoban-<slaveMac> rssi=-40
[配對] 接受 hoban-<slaveMac>，目前共 1 台
[名冊] 已儲存 1 台
```

Slave 序列埠 Expected：
```
[心跳] 來自 hoban-<masterMac> channel=1 配對模式=是 rssi=-40
[配對] 已送出配對請求，等待回覆
[配對] 成功，master=hoban-<masterMac> channel=1
```
且 slave 的 LED 快閃 3 下。

- [ ] **Step 8: 驗證配對持久化**

兩片板子都斷電再上電。

Run：
```powershell
.\tools\serial_expect.ps1 -Port COM16 -Expect "[名冊] 載入 1 台 slave" -Seconds 8 -Reset
.\tools\serial_expect.ps1 -Port COM13 -Expect "已配對 master:" -Seconds 8 -Reset
```

Expected：master 印出名冊含 1 台；slave 印出已配對的 master ID 與上次 channel，
且**不再進入掃描**（不應出現 `[掃描] 開始輪掃`），因為 EEPROM 記得 channel。

- [ ] **Step 9: 驗證未配對模式下的拒絕**

確認 master **不在**配對模式（LED 不閃），短按 slave 的按鈕。

Slave 序列埠 Expected：
```
[配對] master 不在配對模式，請先短按 master 的按鈕
```
（slave 端就擋下來了，不會送出請求）

- [ ] **Step 10: Commit**

```bash
git add ho_master1/ ho_slave1/
git commit -m "實作 ESP-NOW 配對流程與名冊持久化

- Master：短按 BOOT 進入配對模式 60 秒，名冊存 NVS，上限 20 台
- Master：配對模式下心跳加快到 1 秒、LED 慢閃
- Slave：短按 BOOT 送出配對請求，成功存 EEPROM 並快閃 3 下
- 拒絕情境：master 不在配對模式、名冊已滿，各有對應的 LED 與訊息

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 5：繼電器控制與狀態回報

Master 用序列埠指令控制 slave 的繼電器，slave 回報狀態。

**Files:**
- Modify: `ho_master1/ho_master1.ino`
- Modify: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes: Task 4 的 `slaves[]`、`findSlave()`、`registerPeer()`
- Produces（Task 6~7 與 Phase 2 依賴）：
  - Master：`void sendCmdToSlave(int idx, HoRelayCmd cmd, uint16_t pulseMs)`
  - Master：`void sendCmdToAll(HoRelayCmd cmd, uint16_t pulseMs)`
  - Master：`void requestSlaveState(int idx)`
  - Master：`void handleSerialCommand(const String& line)`
  - Slave：`void pulseRelay(uint16_t ms)`、`void sendState()`

- [ ] **Step 1: Slave 實作繼電器指令與狀態回報**

在 `ho_slave1.ino` 的 `setRelayPins()` 之後加入：

```cpp
// 點動：開啟 ms 毫秒後自動關閉。
// 用非阻塞方式，避免點動期間收不到 ESP-NOW 封包。
unsigned long pulseEndTime = 0;

void pulseRelay(uint16_t ms) {
  setRelayPins(true);
  pulseEndTime = millis() + ms;
  Serial.printf("[繼電器] 點動 %u ms\n", ms);
}

void sendState() {
  if (!masterKnown) return;

  HoStatePayload st;
  st.relay = relayState ? 1 : 0;
  st.fwMajor = 1;
  st.fwMinor = 0;
  st.fwPatch = 0;
  st.uptimeSec = millis() / 1000;

  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), HO_PKT_STATE, txSeq++, &st, sizeof(st));
  esp_now_send(masterMac, buf, total);
  Serial.printf("[狀態] 已回報 relay=%u\n", st.relay);
}
```

在 `onEspNowRecv()` 的 `HO_PKT_PAIR_ACK` 區塊之後加入：

```cpp
  // 只接受已配對 master 的控制指令
  if (!masterKnown || memcmp(masterMac, info->src_addr, 6) != 0) {
    return;
  }

  if (header.type == HO_PKT_CMD && payloadLen >= sizeof(HoCmdPayload)) {
    HoCmdPayload cmd;
    memcpy(&cmd, payload, sizeof(cmd));

    switch (cmd.cmd) {
      case HO_CMD_ON:
        setRelayPins(true);
        pulseEndTime = 0;
        Serial.println("[繼電器] 開啟");
        break;
      case HO_CMD_OFF:
        setRelayPins(false);
        pulseEndTime = 0;
        Serial.println("[繼電器] 關閉");
        break;
      case HO_CMD_PULSE:
        pulseRelay(cmd.pulseMs > 0 ? cmd.pulseMs : 2000);
        break;
      default:
        Serial.printf("[繼電器] 未知指令 %u\n", cmd.cmd);
        return;
    }
    sendState();
    return;
  }

  if (header.type == HO_PKT_STATE_REQ) {
    sendState();
    return;
  }

  if (header.type == HO_PKT_UNPAIR) {
    Serial.println("[配對] master 要求解除配對");
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(EE_ADDR_MAGIC, 0);
    EEPROM.commit();
    delay(500);
    ESP.restart();
    return;
  }
```

在 `loop()` 尾端（失聯檢查之前）加入點動結束處理：

```cpp
  // 點動時間到，自動關閉
  if (pulseEndTime != 0 && now >= pulseEndTime) {
    pulseEndTime = 0;
    setRelayPins(false);
    Serial.println("[繼電器] 點動結束，已關閉");
    sendState();
  }
```

**注意**：master 必須先被註冊成 peer，slave 才能對它單播回覆。
在 `onMasterFound()` 的 `memcpy(masterMac, mac, 6);` 之後加入：

```cpp
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
  }
```

- [ ] **Step 2: Master 實作送指令與接收狀態**

在 `ho_master1.ino` 的 `exitPairingMode()` 之後加入：

```cpp
// ── 控制 slave ──
void sendCmdToSlave(int idx, HoRelayCmd cmd, uint16_t pulseMs) {
  if (idx < 0 || idx >= slaveCount) {
    Serial.println("[控制] 編號超出範圍");
    return;
  }
  HoCmdPayload payload;
  payload.cmd = (uint8_t)cmd;
  payload.pulseMs = pulseMs;

  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);
  Serial.printf("[控制] 送指令 %u 給 %s\n", (uint8_t)cmd, id);
  espNowSendTo(slaves[idx].mac, HO_PKT_CMD, &payload, sizeof(payload));
}

void sendCmdToAll(HoRelayCmd cmd, uint16_t pulseMs) {
  Serial.printf("[控制] 廣播指令 %u 給 %d 台\n", (uint8_t)cmd, slaveCount);
  for (int i = 0; i < slaveCount; i++) {
    sendCmdToSlave(i, cmd, pulseMs);
    delay(20);   // 錯開送出時間，降低同頻碰撞
  }
  // master 自己的繼電器也跟著動作
  if (cmd == HO_CMD_ON) {
    setRelayPins(true);
  } else if (cmd == HO_CMD_OFF) {
    setRelayPins(false);
  } else if (cmd == HO_CMD_PULSE) {
    pulseRelay(pulseMs > 0 ? pulseMs : 2000);
  }
}

void requestSlaveState(int idx) {
  if (idx < 0 || idx >= slaveCount) return;
  espNowSendTo(slaves[idx].mac, HO_PKT_STATE_REQ, nullptr, 0);
}

void unpairSlave(int idx) {
  if (idx < 0 || idx >= slaveCount) {
    Serial.println("[配對] 編號超出範圍");
    return;
  }
  char id[20];
  hoFormatDeviceId(slaves[idx].mac, id);

  espNowSendTo(slaves[idx].mac, HO_PKT_UNPAIR, nullptr, 0);
  delay(100);   // 給對方時間收到再刪 peer

  esp_now_del_peer(slaves[idx].mac);
  for (int i = idx; i < slaveCount - 1; i++) slaves[i] = slaves[i + 1];
  slaveCount--;
  saveSlaves();
  Serial.printf("[配對] 已移除 %s，剩 %d 台\n", id, slaveCount);
}

void printSlaveList() {
  Serial.printf("── Slave 名冊（%d／%d）──\n", slaveCount, HO_ESPNOW_MAX_SLAVES);
  if (slaveCount == 0) {
    Serial.println("  （空）");
    return;
  }
  unsigned long now = millis();
  for (int i = 0; i < slaveCount; i++) {
    char id[20];
    hoFormatDeviceId(slaves[i].mac, id);
    bool online = slaves[i].lastSeen > 0 &&
                  (now - slaves[i].lastSeen) < SLAVE_OFFLINE_TIMEOUT;
    Serial.printf("  %d. %s  %s  rssi=%d\n",
                  i, id, online ? "在線" : "離線", slaves[i].rssi);
  }
}
```

Master 的繼電器也需要點動函式。在 `initRelayPins()` 之後加入：

```cpp
void setRelayPins(bool on) {
  for (int i = 0; i < relayPinCount; i++) {
    digitalWrite(relayPins[i], on ? HIGH : LOW);
  }
  relayState = on;
}

unsigned long pulseEndTime = 0;

void pulseRelay(uint16_t ms) {
  setRelayPins(true);
  pulseEndTime = millis() + ms;
}
```

在 `loop()` 尾端加入點動結束處理：

```cpp
  if (pulseEndTime != 0 && now >= pulseEndTime) {
    pulseEndTime = 0;
    setRelayPins(false);
  }
```

- [ ] **Step 3: Master 處理狀態回報**

在 `onEspNowRecv()` 的 `HO_PKT_UNPAIR` 區塊之後加入：

```cpp
  if (header.type == HO_PKT_STATE && payloadLen >= sizeof(HoStatePayload)) {
    int idx = findSlave(info->src_addr);
    if (idx < 0) {
      Serial.printf("[狀態] 收到未配對設備 %s 的回報，忽略\n", senderId);
      return;
    }
    HoStatePayload st;
    memcpy(&st, payload, sizeof(st));

    slaves[idx].online = true;
    slaves[idx].rssi = info->rx_ctrl->rssi;
    slaves[idx].lastSeen = millis();

    Serial.printf("[狀態] %s relay=%u 版本=%u.%u.%u 運行=%lus rssi=%d\n",
                  senderId, st.relay, st.fwMajor, st.fwMinor, st.fwPatch,
                  (unsigned long)st.uptimeSec, info->rx_ctrl->rssi);
    return;
  }
```

- [ ] **Step 4: Master 加入序列埠指令介面**

在 `printSlaveList()` 之後加入：

```cpp
// ── 序列埠測試指令（Phase 2 接上 MQTT 後仍保留，方便現場除錯）──
void printHelp() {
  Serial.println("── 可用指令 ──");
  Serial.println("  list          列出所有 slave");
  Serial.println("  pair          進入／離開配對模式");
  Serial.println("  on <n>        開啟第 n 台（n 為 list 的編號）");
  Serial.println("  off <n>       關閉第 n 台");
  Serial.println("  pulse <n>     點動第 n 台 2 秒");
  Serial.println("  allon         全部開啟（含 master 自己）");
  Serial.println("  alloff        全部關閉（含 master 自己）");
  Serial.println("  allpulse      全部點動 2 秒");
  Serial.println("  state <n>     要求第 n 台回報狀態");
  Serial.println("  unpair <n>    解除第 n 台配對");
  Serial.println("  help          顯示這份說明");
}

void handleSerialCommand(const String& line) {
  String cmd = line;
  cmd.trim();
  if (cmd.length() == 0) return;

  int spacePos = cmd.indexOf(' ');
  String verb = (spacePos < 0) ? cmd : cmd.substring(0, spacePos);
  int arg = (spacePos < 0) ? -1 : cmd.substring(spacePos + 1).toInt();

  if (verb == "list") {
    printSlaveList();
  } else if (verb == "pair") {
    if (pairingMode) exitPairingMode(); else enterPairingMode();
  } else if (verb == "on") {
    sendCmdToSlave(arg, HO_CMD_ON, 0);
  } else if (verb == "off") {
    sendCmdToSlave(arg, HO_CMD_OFF, 0);
  } else if (verb == "pulse") {
    sendCmdToSlave(arg, HO_CMD_PULSE, 2000);
  } else if (verb == "allon") {
    sendCmdToAll(HO_CMD_ON, 0);
  } else if (verb == "alloff") {
    sendCmdToAll(HO_CMD_OFF, 0);
  } else if (verb == "allpulse") {
    sendCmdToAll(HO_CMD_PULSE, 2000);
  } else if (verb == "state") {
    requestSlaveState(arg);
  } else if (verb == "unpair") {
    unpairSlave(arg);
  } else if (verb == "help") {
    printHelp();
  } else {
    Serial.printf("未知指令：%s（輸入 help 看說明）\n", verb.c_str());
  }
}
```

在 `loop()` 尾端加入讀取：

```cpp
  // ── 序列埠指令 ──
  static String serialBuffer = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        handleSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 64) serialBuffer = "";  // 防溢位
    }
  }
```

在 `setup()` 尾端 `Serial.println("就緒");` 之前加入 `printHelp();`。

- [ ] **Step 5: 編譯兩支韌體**

Run：
```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model slave
```

Expected：兩支都編譯成功。

- [ ] **Step 6: 驗證單台控制**

燒錄兩片板子（用 `-KeepConfig` 保留配對記錄）：
```powershell
.\flash.ps1 -Model master -Upload -Port COM16 -KeepConfig
.\flash.ps1 -Model slave -Upload -Port COM13 -KeepConfig
```

用 `arduino-cli monitor` 開啟 master 的互動序列埠：
```powershell
A:\server\arduino-cli\arduino-cli.exe monitor -p COM16 --config baudrate=115200
```

依序輸入 `list`、`pulse 0`、`on 0`、`off 0`。

Master Expected：
```
── Slave 名冊（1／20）──
  0. hoban-<slaveMac>  離線  rssi=0
[控制] 送指令 2 給 hoban-<slaveMac>
[狀態] hoban-<slaveMac> relay=1 版本=1.0.0 運行=..s rssi=-42
[狀態] hoban-<slaveMac> relay=0 版本=1.0.0 運行=..s rssi=-42
```
（點動會收到兩次狀態：開啟時一次、2 秒後自動關閉時一次）

Slave 端 Expected：
```
[繼電器] 點動 2000 ms
[狀態] 已回報 relay=1
[繼電器] 點動結束，已關閉
[狀態] 已回報 relay=0
```
**實體確認**：slave 的繼電器有動作聲／LED 亮起 2 秒後熄滅。

- [ ] **Step 7: 驗證群組控制**

在 master 序列埠輸入 `allpulse`。

Expected：master 印出 `[控制] 廣播指令 2 給 1 台`，
slave 繼電器動作，master 自己的繼電器（若有接）也同時動作。

若有第三片板子可再配對一台，確認兩台 slave 同時動作。

- [ ] **Step 8: Commit**

```bash
git add ho_master1/ ho_slave1/
git commit -m "實作繼電器控制與狀態回報

- Slave：ON/OFF/PULSE 三種指令，點動用非阻塞計時避免漏收封包
- Slave：只接受已配對 master 的指令，動作後主動回報狀態
- Master：序列埠指令介面（list/on/off/pulse/allon/alloff/state/unpair）
- Master：群組指令會連自己的繼電器一起動作，逐台送出並錯開 20ms

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 6：失聯偵測與自動恢復

驗證並補強三種失聯情境的恢復行為。

**Files:**
- Modify: `ho_master1/ho_master1.ino`
- Modify: `ho_slave1/ho_slave1.ino`

**Interfaces:**
- Consumes: Task 5 的 `requestSlaveState()`、`slaves[].lastSeen`
- Produces: Master 定期輪詢機制；`void pollSlaveStates()`

- [ ] **Step 1: Master 定期輪詢 slave 狀態**

只靠 slave 主動回報無法得知離線。Master 每 15 秒輪詢一次，
超過 30 秒沒回應就標記離線（Phase 2 會用這個狀態代發 MQTT 的 offline）。

在 `ho_master1.ino` 的 `requestSlaveState()` 之後加入：

```cpp
// 每輪只問一台，20 台輪完一圈約 5 分鐘太慢，
// 所以每次輪詢就把全部問一遍，用 20ms 錯開避免碰撞。
void pollSlaveStates() {
  if (slaveCount == 0) return;
  for (int i = 0; i < slaveCount; i++) {
    requestSlaveState(i);
    delay(20);
  }
}

void updateSlaveOnlineStatus() {
  unsigned long now = millis();
  for (int i = 0; i < slaveCount; i++) {
    bool wasOnline = slaves[i].online;
    bool isOnline = slaves[i].lastSeen > 0 &&
                    (now - slaves[i].lastSeen) < SLAVE_OFFLINE_TIMEOUT;
    if (wasOnline && !isOnline) {
      char id[20];
      hoFormatDeviceId(slaves[i].mac, id);
      Serial.printf("[離線] %s 超過 %lu 秒沒回應\n",
                    id, SLAVE_OFFLINE_TIMEOUT / 1000);
    }
    slaves[i].online = isOnline;
  }
}
```

在 `loop()` 的心跳區塊之後加入：

```cpp
  // ── 每 15 秒輪詢一次 slave 狀態 ──
  static unsigned long lastPoll = 0;
  if (now - lastPoll >= 15000) {
    lastPoll = now;
    pollSlaveStates();
    updateSlaveOnlineStatus();
  }
```

- [ ] **Step 2: Slave 失聯時關閉繼電器（安全預設）**

失聯時繼電器停在開啟狀態是危險的。加入安全機制：
slave 超過心跳逾時仍找不到 master，強制關閉繼電器。

在 `ho_slave1.ino` 的 `startChannelScan()` 開頭（`if (scanning) return;` 之後）加入：

```cpp
  // 安全預設：失去 master 時關閉繼電器，避免一直通電
  if (relayState) {
    setRelayPins(false);
    pulseEndTime = 0;
    Serial.println("[安全] 失去 master，繼電器已關閉");
  }
```

- [ ] **Step 3: 編譯**

Run：
```powershell
.\flash.ps1 -Model master
.\flash.ps1 -Model slave
```

Expected：兩支都編譯成功。

- [ ] **Step 4: 驗證情境一 — Slave 斷電後復電**

燒錄兩片板子，確認已配對且在線。拔掉 slave 電源 40 秒。

Master Expected（40 秒內）：
```
[離線] hoban-<slaveMac> 超過 30 秒沒回應
```
輸入 `list` 應顯示該台為「離線」。

Slave 復電後 Expected：
- Slave 印出 `已配對 master: ...，上次 channel=N`，直接鎖定不掃描
- Master 在下一次輪詢（15 秒內）收到狀態，`list` 恢復顯示「在線」

- [ ] **Step 5: 驗證情境二 — Master 斷電後復電**

拔掉 master 電源 40 秒。

Slave Expected：
```
[失聯] 超過 30 秒沒收到心跳
[安全] 失去 master，繼電器已關閉   ← 若失聯前繼電器是開的才會出現
[掃描] 開始輪掃 channel 1~13 尋找 master
[channel] 切換到 1
...
```

Master 復電後 Expected：slave 在一輪掃描內（約 8 秒）重新鎖定：
```
[心跳] 來自 hoban-<masterMac> channel=1 ...
```
（此時 channel 沒變，不會再印 `[鎖定]`，因為 `onMasterFound()` 只在 master 或 channel 改變時才印）

- [ ] **Step 6: 驗證情境三 — Master channel 改變**

Phase 1 的 master 不連 WiFi，channel 固定為 1，無法自然改變。
用序列埠指令手動模擬：在 master 的 `handleSerialCommand()` 加入一個測試指令。

在 `handleSerialCommand()` 的 `} else if (verb == "help") {` 之前插入：

```cpp
  } else if (verb == "ch") {
    // 測試用：手動切換 channel，模擬 Phase 2 連上不同路由器的情況
    if (arg >= 1 && arg <= 13) {
      esp_wifi_set_channel((uint8_t)arg, WIFI_SECOND_CHAN_NONE);
      currentChannel = (uint8_t)arg;
      Serial.printf("[channel] master 切換到 %d\n", arg);
      sendHeartbeat();
    } else {
      Serial.println("channel 需在 1~13 之間");
    }
```

並在 `printHelp()` 的 `help` 那行之前加入：

```cpp
  Serial.println("  ch <n>        測試用：切換 master 的 channel（1~13）");
```

重新編譯燒錄 master，在序列埠輸入 `ch 6`。

Slave Expected（30 秒失聯逾時後）：
```
[失聯] 超過 30 秒沒收到心跳
[掃描] 開始輪掃 channel 1~13 尋找 master
[channel] 切換到 1
...
[channel] 切換到 6
[心跳] 來自 hoban-<masterMac> channel=6 ...
[鎖定] master=hoban-<masterMac> channel=6
```
之後 slave 斷電再上電，應直接鎖定 channel 6 不掃描（EEPROM 已更新）。

**這一項是整個 Phase 1 風險最高的驗證，務必實測通過。**
恢復時間應在 40 秒內（30 秒逾時 + 最多 8 秒掃描一輪）。

- [ ] **Step 7: Commit**

```bash
git add ho_master1/ ho_slave1/
git commit -m "實作失聯偵測與自動恢復

- Master：每 15 秒輪詢所有 slave 狀態，超過 30 秒無回應標記離線
- Slave：失去 master 時強制關閉繼電器（安全預設），再開始輪掃
- Master 新增測試指令 ch <n>，可手動切 channel 驗證 slave 的重掃恢復
- 三種失聯情境（slave 斷電、master 斷電、channel 改變）皆實測恢復

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 7：Phase 1 收尾與文件

補上長按重置、更新專案文件、完成完整回歸測試。

**Files:**
- Modify: `ho_slave1/ho_slave1.ino`
- Modify: `CLAUDE.md`
- Create: `ho_master1/readme.md`
- Create: `ho_slave1/readme.md`

**Interfaces:**
- Consumes: 前六個 Task 的全部成果
- Produces: 可交付的 Phase 1 韌體，Phase 2 從這裡接手

- [ ] **Step 1: Slave 加入長按重置**

沿用 `ho_relay2` 的兩段式重置（長按 3 秒 → 閃爍 2 秒 → 長亮 0.7 秒 → 清除），
但清除的是配對記錄而非 WiFi 設定。

在 `ho_slave1.ino` 的全域區塊加入：

```cpp
// ── 長按重置（與 ho_relay2 行為一致）──
const int LONG_PRESS_TIME = 3000;
const int BLINK_CONFIRM_TIME = 2000;
const int BLINK_INTERVAL = 250;
const int CONFIRM_SOLID_TIME = 700;
unsigned long resetPressTime = 0;
unsigned long resetBlinkStart = 0;
bool resetBlinking = false;
```

在 `savePairing()` 之後加入：

```cpp
void clearPairing() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  EEPROM.commit();
  Serial.println("配對記錄已清除，重新啟動中…");
  delay(1000);
  ESP.restart();
}
```

把 `loop()` 裡 Task 4 Step 5 加入的「短按 BOOT 送出配對請求」整塊刪掉
（從 `static bool lastButtonState = HIGH;` 到 `lastButtonState = buttonState;`，
連同上面的註釋），換成下面同時處理短按與長按的版本。
**Task 4 的「配對請求逾時」區塊要保留**，不要一起刪掉：

```cpp
  // ── 按鈕：短按配對，長按重置 ──
  // 兩顆按鈕任一顆都可以，與 ho_relay2 一致
  bool bootState = digitalRead(bootButton);
  bool resetState = digitalRead(resetButton);
  bool anyPressed = (bootState == LOW || resetState == LOW);

  static bool lastAnyPressed = false;

  if (anyPressed && !lastAnyPressed) {
    resetPressTime = now;
  }

  if (anyPressed) {
    unsigned long pressDuration = now - resetPressTime;

    if (!resetBlinking && pressDuration >= LONG_PRESS_TIME) {
      resetBlinking = true;
      resetBlinkStart = now;
      Serial.println("長按 3 秒達成，繼續按住 2 秒清除配對…");
    }

    if (resetBlinking) {
      unsigned long blinkDuration = now - resetBlinkStart;
      if (blinkDuration < BLINK_CONFIRM_TIME) {
        bool ledOn = (blinkDuration % BLINK_INTERVAL) < (BLINK_INTERVAL / 2);
        digitalWrite(ledOnFace, ledOn ? HIGH : LOW);
        digitalWrite(ledOnBoard, ledOn ? HIGH : LOW);
      } else {
        Serial.println("確認清除配對");
        digitalWrite(ledOnFace, HIGH);
        digitalWrite(ledOnBoard, HIGH);
        delay(CONFIRM_SOLID_TIME);
        digitalWrite(ledOnFace, LOW);
        digitalWrite(ledOnBoard, LOW);
        clearPairing();
      }
    }
  } else if (lastAnyPressed) {
    // 放開：短按觸發配對，長按中途放開則取消
    unsigned long pressDuration = now - resetPressTime;
    if (!resetBlinking && pressDuration >= 50 && pressDuration < 1000) {
      requestPairing();
    }
    if (resetBlinking) {
      Serial.println("按鈕放開，取消重置");
    }
    resetBlinking = false;
    resetPressTime = 0;
    // 還原繼電器對應的 LED 狀態
    digitalWrite(ledOnFace, relayState ? HIGH : LOW);
    digitalWrite(ledOnBoard, relayState ? HIGH : LOW);
  }

  lastAnyPressed = anyPressed;
```

- [ ] **Step 2: 編譯並驗證長按重置**

Run：
```powershell
.\flash.ps1 -Model slave -Upload -Port COM13 -KeepConfig
```

操作：按住 slave 的 BOOT 按鈕不放，觀察 LED。

Expected：
- 3 秒時 LED 開始 250ms 閃爍，序列埠印 `長按 3 秒達成，繼續按住 2 秒清除配對…`
- 再 2 秒 LED 長亮 0.7 秒，序列埠印 `確認清除配對`、`配對記錄已清除，重新啟動中…`
- 重啟後印 `EEPROM 無配對記錄` 並開始掃描

再測一次中途放開：按住 4 秒後放開，Expected 印 `按鈕放開，取消重置`，配對記錄仍在。

- [ ] **Step 3: 寫 master readme**

`ho_master1/readme.md`：

```markdown
# hoMaster1 — ESP-NOW 主控端

## 硬體

- 開發板：ESP32 WROOM DevKit
- GPIO：BOOT 按鈕 0、第二按鈕 14、板載 LED 2、繼電器 13
- 繼電器為**選配**：接了就能用 `on`/`off` 控制，沒接則指令空跑

## 角色

Phase 1 階段是純 ESP-NOW 主控，用序列埠指令操作。
Phase 2 會接上 WiFi + MQTT，成為 App 與所有 slave 之間的唯一對外窗口。

## 序列埠指令

| 指令 | 說明 |
|---|---|
| `list` | 列出所有 slave 與在線狀態 |
| `pair` | 進入／離開配對模式（60 秒） |
| `on <n>` / `off <n>` | 開啟／關閉第 n 台 |
| `pulse <n>` | 點動第 n 台 2 秒 |
| `allon` / `alloff` / `allpulse` | 群組控制，含 master 自己 |
| `state <n>` | 要求第 n 台回報狀態 |
| `unpair <n>` | 解除第 n 台配對 |
| `ch <n>` | 測試用：切換 channel，驗證 slave 重掃 |
| `help` | 顯示說明 |

## 配對

短按 BOOT 進入配對模式 60 秒，LED 慢閃，心跳加快到 1 秒。
此時短按 slave 的按鈕即可加入。上限 20 台。

名冊存在 NVS（`Preferences`，命名空間 `homaster`），斷電不遺失。

## 編譯與燒錄

```powershell
.\flash.ps1 -Model master           # 只編譯
.\flash.ps1 -Model master -Upload   # 編譯並燒錄
```
```

- [ ] **Step 4: 寫 slave readme**

`ho_slave1/readme.md`：

```markdown
# hoSlave1 — ESP-NOW 受控端

## 硬體

沿用 hoRelay2 的 ESP32-C3 繼電器板，GPIO 定義完全相同：
BOOT 9、RESET 1、板載 LED 3、面板 LED 0、繼電器 4 與 7（兩支同時驅動）。

**開機瞬間繼電器短暫通電是硬體限制**，與 hoRelay2 相同，
詳見 `ho_relay2/readme.md`。`initRelayPins()` 必須維持在 `setup()` 第一行。

## 角色

不連 WiFi、不跑 MQTT、不跑 BLE，只透過 ESP-NOW 接受 master 控制。
因為沒有網路，韌體更新只能靠 USB 燒錄（Phase 4 會加上經 master 轉送的 OTA）。

## 按鈕

| 操作 | 行為 |
|---|---|
| 短按（< 1 秒） | 送出配對請求（需 master 先進入配對模式） |
| 長按 3 秒 → LED 閃爍 → 再按住 2 秒 | 清除配對記錄並重啟 |

兩顆按鈕（BOOT／RESET）任一顆都可以。

## Channel 同步

Slave 不連 WiFi，無從得知 master 在哪個 channel，靠三層機制解決：

1. 配對時把 master 的 channel 存進 EEPROM，開機直接切過去
2. Master 每 5 秒廣播心跳（配對模式時 1 秒），帶出目前 channel
3. 超過 30 秒沒收到心跳，輪掃 channel 1~13（每個停 600ms，一輪約 8 秒）

Master 換路由器導致 channel 改變時，恢復時間約 40 秒。

## 安全預設

失去 master 連線時會強制關閉繼電器，避免長時間通電。

## EEPROM 佈局（32 bytes）

| 位址 | 長度 | 內容 |
|---|---|---|
| 0 | 1 | 魔術數 0x5A，表示已配對 |
| 1 | 6 | Master MAC |
| 7 | 1 | 鎖定的 channel |
| 8 | 1 | Long Range 旗標（Phase 5 使用） |

## 編譯與燒錄

```powershell
.\flash.ps1 -Model slave                 # 只編譯
.\flash.ps1 -Model slave -Upload         # 編譯並燒錄（會抹掉配對記錄）
.\flash.ps1 -Model slave -Upload -KeepConfig  # 保留配對記錄
```
```

- [ ] **Step 5: 更新 CLAUDE.md**

在 `CLAUDE.md` 的「## 硬體型號」章節，`### hoRelay2` 區塊之後加入：

```markdown
### hoMaster1 / hoSlave1（ESP-NOW 多機聯動）

一台 master 透過 ESP-NOW 控制最多 20 台 slave，用於「一鍵同時觸發多台」
與「slave 現場沒有網路」的場景。設計文件見
`docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md`。

- **hoMaster1**（`ho_master1/`）：ESP32 WROOM，對外 MQTT 窗口 + ESP-NOW 主控，繼電器選配
- **hoSlave1**（`ho_slave1/`）：沿用 hoRelay2 的 C3 繼電器板，不連 WiFi

兩者共用 `libraries/HoEspNow/` 的協定定義，編譯時需帶 `--libraries`（`flash.ps1` 已處理）。

協定摘要：封包 7 bytes 標頭（magic/version/type/seq/crc），
CRC 混入共享密鑰過濾誤觸發，不使用 ESP-NOW 原生加密（原生加密 peer 上限只有 6 台）。

各 sketch 的詳細說明見 `ho_master1/readme.md` 與 `ho_slave1/readme.md`。
```

在「### 編譯與上傳」章節的程式碼區塊之後加入：

```markdown
本專案實際燒錄一律走 `flash.ps1`（見 `.claude/rules/vscode-arduino-toolchain.md`）：

```powershell
.\flash.ps1 -Model 2 -Upload          # hoRelay2
.\flash.ps1 -Model master -Upload     # hoMaster1
.\flash.ps1 -Model slave -Upload      # hoSlave1
.\flash.ps1 -Model test -Upload       # ESP-NOW 協定測試
```
```

- [ ] **Step 6: 完整回歸測試**

依序執行，全部通過才算 Phase 1 完成：

```powershell
# 1. 協定測試
.\flash.ps1 -Model test -Upload -Port COM13
.\tools\serial_expect.ps1 -Port COM13 -Expect "ALL TESTS PASSED" -Seconds 10 -Reset

# 2. 燒回 slave 韌體
.\flash.ps1 -Model slave -Upload -Port COM13

# 3. 燒錄 master
.\flash.ps1 -Model master -Upload -Port COM16
```

接著人工驗證這份清單，每項打勾：

- [ ] Master 開機每 5 秒發一次心跳
- [ ] Slave 首次開機進入輪掃，找到 master 後鎖定
- [ ] 短按 master → 短按 slave → 配對成功，slave LED 快閃 3 下
- [ ] 兩邊斷電再上電，配對記錄都還在，slave 不需重新掃描
- [ ] `list` 顯示 slave 在線
- [ ] `pulse 0` 讓 slave 繼電器動作 2 秒後自動關閉
- [ ] `on 0` / `off 0` 正確開關
- [ ] `allpulse` 讓所有 slave 與 master 自己同時動作
- [ ] Slave 斷電 40 秒後 master 標記離線，復電後自動恢復在線
- [ ] Master 斷電 40 秒後 slave 開始輪掃，復電後自動找回
- [ ] `ch 6` 切換 channel 後，slave 在 40 秒內重新鎖定到 channel 6
- [ ] Slave 長按 5 秒清除配對，重啟後回到未配對狀態
- [ ] Slave 長按中途放開會取消，配對記錄保留
- [ ] `unpair 0` 後 slave 重啟並回到未配對狀態，master 名冊剩 0 台

- [ ] **Step 7: Commit**

```bash
git add ho_master1/ ho_slave1/ CLAUDE.md
git commit -m "Phase 1 收尾：slave 長按重置與專案文件

- Slave 加入與 hoRelay2 一致的兩段式長按重置，清除的是配對記錄
- 新增 ho_master1/readme.md 與 ho_slave1/readme.md
- CLAUDE.md 補上 hoMaster1/hoSlave1 型號說明與 flash.ps1 用法
- 完整回歸測試 14 項全數通過

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Phase 1 完成後的狀態

- 兩支可燒錄的韌體，master 能配對並控制最多 20 台 slave
- 三種失聯情境都能自動恢復
- 協定層有 on-target 測試護著，Phase 2~5 改動時可隨時回歸
- **尚未有**：MQTT、App 整合、OTA 轉送、Long Range —— 分別是 Phase 2~5

Phase 2 的接手點：`ho_master1.ino` 的 `setupEspNow()` 目前呼叫 `WiFi.disconnect()`，
Phase 2 要改成連上 AP，並在連線後用 `esp_wifi_get_channel()` 更新 `currentChannel`、
立刻多發幾次心跳讓 slave 跟上新 channel。
`handleSerialCommand()` 的指令分派可直接對應到 MQTT 指令，不需重寫。
