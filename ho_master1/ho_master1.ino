// 齁控 Master — ESP-NOW 主控端
// 硬體：ESP32 WROOM DevKit（GPIO 定義與 ho_relay1 一致）
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HoEspNowProtocol.h>
#include <Preferences.h>

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

// ── Slave 名冊 ──
struct SlaveEntry {
  uint8_t mac[6];
  bool online;
  int8_t rssi;
  unsigned long lastSeen;
};

SlaveEntry slaves[HO_ESPNOW_MAX_SLAVES];
// slaveCount 會被 ESP-NOW callback（WiFi task）寫入、sendHeartbeat()（主 task）讀取，
// 屬跨 context 存取，加 volatile 避免編譯器快取舊值
volatile int slaveCount = 0;
Preferences prefs;

// 配對模式
unsigned long pairingStartTime = 0;
const unsigned long PAIRING_TIMEOUT = 60000;  // 60 秒
// slave 超過這麼久沒回應就標記離線
const unsigned long SLAVE_OFFLINE_TIMEOUT = 30000;

const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
// 心跳間隔。必須「明顯小於 slave 每個 channel 的停留時間（SCAN_DWELL_MS = 1200ms）」，
// slave 輪掃時才能保證每停留一個 channel 至少涵蓋一次心跳、一輪內必定命中。
// 舊值 5000ms 大於 dwell，命中變成兩個週期的機率問題（約 7.7%），
// 期望要十幾次心跳、約一分鐘才鎖得回來。廣播封包只有 11 bytes，加快到 1 秒的流量代價可忽略。
const unsigned long HEARTBEAT_INTERVAL = 1000;

// channel 改變時立刻連發數次心跳，讓正在輪掃的 slave 更快命中
const int HEARTBEAT_BURST_COUNT = 4;
const int HEARTBEAT_BURST_GAP = 200;

// ── 開機按鈕自檢（移植自 ho_relay2.ino）──
// 按鈕接法是 GPIO ──[按鈕]── GND，靠 INPUT_PULLUP 拉高；某支腳一旦短路或走線接地
// 就恆為 LOW，會被按鈕狀態機誤判成「使用者一直按著」。
// master 目前只有短按進配對（風險低），但重置功能的位置已預留在 loop()，
// Phase 2 一補上就會繼承 hoRelay2 那個「開機即清設定 → 重啟 → 再清除」的無限迴圈缺陷，
// 所以現在先把防呆一併備妥。
const unsigned long BTN_SELFTEST_DURATION = 500;  // 自檢取樣總長度 (毫秒)
const unsigned long BTN_SELFTEST_INTERVAL = 50;   // 取樣間隔 (毫秒)
bool bootButtonUsable = true;                     // BOOT 按鈕是否可用
bool secondButtonUsable = true;                   // 第二按鈕是否可用

// ── 繼電器 ──
// 與 ho_relay2 相同的鐵則：initRelayPins() 必須是 setup() 第一行
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
  relayState = on;
}

// 用「起始時間＋持續時間」搭配無號數減法比較，而非「結束時間」搭配絕對值比較，
// 避免 millis() 約 49.7 天溢位時，迴繞後的極小值被誤判為「時間已到」而提前關閉繼電器
unsigned long pulseStartTime = 0;
uint16_t pulseDuration = 0;
bool pulseActive = false;

void pulseRelay(uint16_t ms) {
  setRelayPins(true);
  pulseStartTime = millis();
  pulseDuration = ms;
  pulseActive = true;
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

// ── 開機按鈕自檢 ──
// 短暫取樣兩支按鈕腳，整段都是 LOW 即判定卡住並停用其按鈕功能。
// 必須在 pinMode(..., INPUT_PULLUP) 之後呼叫，但絕不能早於 initRelayPins()（繼電器安全鐵則）。
// 注意：這也會擋掉「按住按鈕再上電」的操作，放開後重新上電即恢復。
void checkStuckButtons() {
  const int totalSamples = BTN_SELFTEST_DURATION / BTN_SELFTEST_INTERVAL;
  int bootLowCount = 0;
  int secondLowCount = 0;

  for (int i = 0; i < totalSamples; i++) {
    if (digitalRead(bootButton) == LOW) bootLowCount++;
    if (digitalRead(secondButton) == LOW) secondLowCount++;
    delay(BTN_SELFTEST_INTERVAL);
  }

  bootButtonUsable = (bootLowCount < totalSamples);
  secondButtonUsable = (secondLowCount < totalSamples);

  if (bootButtonUsable && secondButtonUsable) {
    Serial.println("按鈕自檢: 正常");
    return;
  }

  if (!bootButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: BOOT(GPIO %d) 恆為 LOW，本次開機停用其按鈕功能\n", bootButton);
  }
  if (!secondButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: 第二按鈕(GPIO %d) 恆為 LOW，本次開機停用其按鈕功能\n", secondButton);
  }
  Serial.println("  若非按住按鈕開機，代表該腳短路或未接，請檢查硬體");
}

// 是否有「可用的」按鈕正被按下；自檢判定卡住的腳一律視為未按下。
// 目前只有 BOOT 用於短按配對，這支保留給 Phase 2 的長按重置流程，屆時一律走這裡判斷。
bool anyResetButtonPressed() {
  if (bootButtonUsable && digitalRead(bootButton) == LOW) return true;
  if (secondButtonUsable && digitalRead(secondButton) == LOW) return true;
  return false;
}

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

// 把 NVS 名冊裡的每一台都重新註冊成 ESP-NOW peer。
// 這步不能省，也必須在 esp_now_init() 之後才叫得動：
// peer 表只存在 RAM，重開機後全空，而 loadSlaves() 只從 NVS 還原 MAC 而已。
// 少了它，master 拔電重插這種例行操作就會讓 esp_now_send() 回 ESP_ERR_ESPNOW_NOT_FOUND，
// 所有輪詢與控制指令全部失敗、list 顯示全部離線，而且不會自我修復
//（slave 不會主動送封包來觸發重新註冊），只能一台一台重新配對。
// 更嚴重的是：slave 仍持續收到廣播心跳，lastHeartbeatTime 一直更新，
// slave 端「30 秒失聯就強制關閉繼電器」的安全保護永遠不會觸發 ——
// master 重開機前若某台繼電器是 ON，它會無限期保持通電，等於籠子無限期停在錯誤狀態。
void registerAllPeers() {
  int okCount = 0;
  for (int i = 0; i < slaveCount; i++) {
    if (registerPeer(slaves[i].mac)) okCount++;
  }
  Serial.printf("[名冊] 已重新註冊 %d／%d 台為 ESP-NOW peer\n", okCount, slaveCount);
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

// 分散式輪詢：每次 loop() 呼叫最多只問一台，用「15000ms ÷ 台數」的間隔
// 平均分攤，一輪剛好 15 秒問完全部，且完全不阻塞 loop()。
// （舊版用 for 迴圈搭配 delay(20) 一次問完全部，20 台會阻塞 loop() 達
// 400ms，若與 master 自身點動的關閉時機重疊，會讓點動時間被拖長超過設定
// 值，故改為此設計，見 Task 6 review 修正 2）
void pollNextSlave() {
  if (slaveCount == 0) return;

  static unsigned long lastPollAt = 0;
  static int pollIdx = 0;

  // 下限 20ms：台數很多時避免間隔過短、無線封包擠在一起碰撞
  unsigned long interval = 15000UL / (unsigned long)slaveCount;
  if (interval < 20) interval = 20;

  unsigned long now = millis();
  if (now - lastPollAt < interval) return;
  lastPollAt = now;

  // slaveCount 可能因配對／解除配對中途變動，索引越界就重頭開始，
  // 不特別處理「跳過某台」，反正下一輪就會輪到
  if (pollIdx >= slaveCount) pollIdx = 0;
  requestSlaveState(pollIdx);
  pollIdx = (pollIdx + 1) % slaveCount;
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
  Serial.println("  ch <n>        測試用：切換 master 的 channel（1~13）");
  Serial.println("  help          顯示這份說明");
}

// 驗證序列埠指令的編號參數是否為合法非負整數。
// String::toInt() 對非數字輸入會靜默回傳 0，若不驗證，打錯字（如 on a）
// 會被誤當成「編號 0」直接執行，動物管制設備誤觸發繼電器等於誤關籠子，故視為安全性驗證。
bool parseIndexArg(const String& argStr, int& outIdx) {
  if (argStr.length() == 0) return false;
  for (unsigned int i = 0; i < argStr.length(); i++) {
    char c = argStr.charAt(i);
    if (c < '0' || c > '9') return false;  // 只接受純數字，不支援負號
  }
  outIdx = argStr.toInt();
  return true;
}

void handleSerialCommand(const String& line) {
  String cmd = line;
  cmd.trim();
  if (cmd.length() == 0) return;

  int spacePos = cmd.indexOf(' ');
  String verb = (spacePos < 0) ? cmd : cmd.substring(0, spacePos);
  String argStr = (spacePos < 0) ? "" : cmd.substring(spacePos + 1);
  argStr.trim();

  // 這些指令需要數字參數，其餘指令（list/pair/allon/alloff/allpulse/help）不受影響
  // ch 雖然不是編號而是 channel，但同樣要求純數字，沿用同一套驗證避免 String::toInt()
  // 對非數字輸入靜默回傳 0（例如 ch abc 誤觸發切到 channel 0）
  bool needsArg = (verb == "on" || verb == "off" || verb == "pulse" ||
                   verb == "state" || verb == "unpair" || verb == "ch");
  int arg = -1;
  if (needsArg && !parseIndexArg(argStr, arg)) {
    Serial.println("[指令] 參數必須是數字，例如：on 0（輸入 help 看說明）");
    return;
  }

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
  } else if (verb == "ch") {
    // 測試用：手動切換 channel，模擬 Phase 2 連上不同路由器的情況
    if (arg >= 1 && arg <= 13) {
      esp_wifi_set_channel((uint8_t)arg, WIFI_SECOND_CHAN_NONE);
      currentChannel = (uint8_t)arg;
      Serial.printf("[channel] master 切換到 %d\n", arg);
      sendHeartbeatBurst();   // 立刻連發數次，讓正在輪掃的 slave 早點命中
    } else {
      Serial.println("channel 需在 1~13 之間");
    }
  } else if (verb == "help") {
    printHelp();
  } else {
    Serial.printf("未知指令：%s（輸入 help 看說明）\n", verb.c_str());
  }
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
  hb.slaveCount = (uint8_t)slaveCount;

  espNowSendTo(BROADCAST_MAC, HO_PKT_HEARTBEAT, &hb, sizeof(hb));
  Serial.printf("[心跳] channel=%u 配對模式=%s slave=%u\n",
                hb.channel, hb.pairingMode ? "是" : "否", hb.slaveCount);
}

// channel 改變的當下連發數次心跳（設計規格要求）。
// 換 channel 後 slave 還停在舊 channel，得等 30 秒失聯門檻才開始輪掃；
// 連發能讓「剛好已在輪掃、正巧停在新 channel」的 slave 立刻命中，不必再等下一輪。
void sendHeartbeatBurst() {
  for (int i = 0; i < HEARTBEAT_BURST_COUNT; i++) {
    sendHeartbeat();
    if (i < HEARTBEAT_BURST_COUNT - 1) delay(HEARTBEAT_BURST_GAP);
  }
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
  delay(50);  // 等內部提升電阻把腳位拉穩再取樣

  checkStuckButtons();  // 必須早於任何按鈕流程，卡住的腳會在此被排除

  loadSlaves();     // 只讀 NVS，可以在 ESP-NOW 初始化之前
  setupEspNow();
  registerAllPeers();  // 必須在 esp_now_init() 之後，否則名冊上的 slave 全部送不出指令

  Serial.printf("設備 ID: %s\n", getDeviceId());
  printHelp();
  Serial.println("就緒");
}

void loop() {
  static unsigned long lastHeartbeat = 0;
  unsigned long now = millis();

  // ── 短按 BOOT 進入配對模式 ──
  // 長按 3 秒以上不觸發，保留給之後的重置功能（屆時請一律走 anyResetButtonPressed()）
  // 自檢判定卡在 LOW 的腳一律視為未按下，避免壞按鈕不斷觸發按鈕流程
  static bool lastButtonState = HIGH;
  static unsigned long buttonDownTime = 0;
  bool buttonState = (bootButtonUsable && digitalRead(bootButton) == LOW) ? LOW : HIGH;

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

  // 心跳固定 1 秒一次（HEARTBEAT_INTERVAL），配對模式不再另開一組等值計時器
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  // ── 分散式輪詢 slave 狀態，每次 loop() 最多問一台（見 pollNextSlave()）──
  pollNextSlave();

  // ── 每 15 秒檢查一次 slave 是否離線 ──
  static unsigned long lastOnlineCheck = 0;
  if (now - lastOnlineCheck >= 15000) {
    lastOnlineCheck = now;
    updateSlaveOnlineStatus();
  }

  if (pulseActive && (now - pulseStartTime) >= pulseDuration) {
    pulseActive = false;
    setRelayPins(false);
  }

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
}
