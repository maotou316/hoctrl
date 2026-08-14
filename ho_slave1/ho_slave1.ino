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

bool waitingPairAck = false;
unsigned long pairReqTime = 0;
const unsigned long PAIR_ACK_TIMEOUT = 5000;
bool masterInPairingMode = false;   // 從心跳得知 master 是否在配對模式

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

  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
  }

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

    masterInPairingMode = (hb.pairingMode == 1);
    onMasterFound(info->src_addr, hb.channel);
    return;
  }

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

  // 掃描中：每個 channel 停留 SCAN_DWELL_MS 後換下一個
  if (scanning) {
    if (now - scanChannelStart >= SCAN_DWELL_MS) {
      scanChannel = (scanChannel % 13) + 1;
      scanChannelStart = now;
      setChannel(scanChannel);
    }
    return;
  }

  // 點動時間到，自動關閉
  if (pulseEndTime != 0 && now >= pulseEndTime) {
    pulseEndTime = 0;
    setRelayPins(false);
    Serial.println("[繼電器] 點動結束，已關閉");
    sendState();
  }

  // 已鎖定：超時沒收到心跳就重新掃描
  if (now - lastHeartbeatTime > HEARTBEAT_TIMEOUT) {
    Serial.println("[失聯] 超過 30 秒沒收到心跳");
    startChannelScan();
  }
}
