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
int slaveCount = 0;
Preferences prefs;

// 配對模式
unsigned long pairingStartTime = 0;
const unsigned long PAIRING_TIMEOUT = 60000;  // 60 秒
// slave 超過這麼久沒回應就標記離線
const unsigned long SLAVE_OFFLINE_TIMEOUT = 30000;

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
    registerPeer(info->src_addr);
    espNowSendTo(info->src_addr, HO_PKT_PAIR_ACK, &ack, sizeof(ack));
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

  loadSlaves();
  setupEspNow();

  Serial.printf("設備 ID: %s\n", getDeviceId());
  Serial.println("就緒");
}

void loop() {
  static unsigned long lastHeartbeat = 0;
  unsigned long now = millis();

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

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
}
