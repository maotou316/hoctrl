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

// ── 長按重置（與 ho_relay2 行為一致）──
const int LONG_PRESS_TIME = 3000;
const int BLINK_CONFIRM_TIME = 2000;
const int BLINK_INTERVAL = 250;
const int CONFIRM_SOLID_TIME = 700;
unsigned long resetPressTime = 0;
unsigned long resetBlinkStart = 0;
bool resetBlinking = false;

// ── 開機按鈕自檢（移植自 ho_relay2.ino）──
// 防止「開機即自動清除配對 → 重啟 → 再清除」的無限迴圈。
// 成因：按鈕接法是 GPIO ──[按鈕]── GND，靠 INPUT_PULLUP 拉高；某支腳一旦短路
// 或走線接地就恆為 LOW，而 lastAnyPressed 初值為 false（代表未按下），
// 開機時就已是 LOW 的腳會被誤判成「使用者剛按下」，長按時間一到就清除配對並重啟。
// 2026-08 在 hoRelay2（完全相同的硬體與 GPIO）實際發生過：RESET 按鈕內部短路。
// slave 沒有 WiFi／MQTT 可自救，未配對的 slave 更是永遠沒機會被配對，故必須防呆。
// 對策：開機時短暫取樣兩支腳，整段都是 LOW 就判定卡住，本次開機停用該腳的按鈕功能。
const unsigned long BTN_SELFTEST_DURATION = 500;  // 自檢取樣總長度 (毫秒)
const unsigned long BTN_SELFTEST_INTERVAL = 50;   // 取樣間隔 (毫秒)
bool bootButtonUsable = true;                     // BOOT 按鈕是否可用
bool resetButtonUsable = true;                    // RESET 按鈕是否可用

// 掃描狀態
bool scanning = false;
uint8_t scanChannel = 1;
unsigned long scanChannelStart = 0;
// 每個 channel 停留時間。必須「明顯大於 master 的心跳間隔（1 秒）」，
// 才能保證只要 master 就在這個 channel，這一輪 dwell 內必定收得到一次心跳。
// 舊值 600ms 小於心跳間隔，命中與否變成兩個週期的機率問題，期望要十幾輪心跳才鎖得回來。
const unsigned long SCAN_DWELL_MS = 1200;
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

void clearPairing() {
  // 先通知 master 解除配對，再清 EEPROM。
  // 協定的 HO_PKT_UNPAIR 是雙向的，master 端已實作接收處理（移除名冊 + esp_now_del_peer）；
  // 若不送，master 名冊那一格與 peer 表項會永久佔用，list 永遠顯示該台離線且無法自動移除，
  // 20 台名額會被慢慢吃光，只能靠人工 unpair <n> 收拾。
  if (masterKnown) {
    uint8_t buf[250];
    size_t total = hoPackPacket(buf, sizeof(buf), HO_PKT_UNPAIR, txSeq++, nullptr, 0);
    esp_now_send(masterMac, buf, total);
    delay(100);   // 給封包送出的時間，之後才會清設定並重啟
    Serial.println("[配對] 已通知 master 解除配對");
  }

  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  EEPROM.commit();
  Serial.println("配對記錄已清除，重新啟動中…");
  delay(1000);
  ESP.restart();
}

// 開機按鈕自檢：短暫取樣兩支按鈕腳，整段都是 LOW 即判定卡住並停用其按鈕功能
// 必須在 pinMode(..., INPUT_PULLUP) 之後、進入任何重置流程之前呼叫，
// 但絕不能早於 initRelayPins()（繼電器安全鐵則）
// 注意：這也會擋掉「按住按鈕再上電」的操作，但那本來就不是合法流程
//（正常重置是設備運作中才長按），放開後重新上電即恢復
void checkStuckButtons() {
  const int totalSamples = BTN_SELFTEST_DURATION / BTN_SELFTEST_INTERVAL;
  int bootLowCount = 0;
  int resetLowCount = 0;

  for (int i = 0; i < totalSamples; i++) {
    if (digitalRead(bootButton) == LOW) bootLowCount++;
    if (digitalRead(resetButton) == LOW) resetLowCount++;
    delay(BTN_SELFTEST_INTERVAL);
  }

  bootButtonUsable = (bootLowCount < totalSamples);
  resetButtonUsable = (resetLowCount < totalSamples);

  if (bootButtonUsable && resetButtonUsable) {
    Serial.println("按鈕自檢: 正常");
    return;
  }

  if (!bootButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: BOOT(GPIO %d) 恆為 LOW，本次開機停用其按鈕功能\n", bootButton);
  }
  if (!resetButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: RESET(GPIO %d) 恆為 LOW，本次開機停用其按鈕功能\n", resetButton);
  }
  Serial.println("  若非按住按鈕開機，代表該腳短路或未接，請檢查硬體");
}

// 是否有「可用的」按鈕正被按下；自檢判定卡住的腳一律視為未按下
bool anyResetButtonPressed() {
  if (bootButtonUsable && digitalRead(bootButton) == LOW) return true;
  if (resetButtonUsable && digitalRead(resetButton) == LOW) return true;
  return false;
}

// ── Channel 控制 ──
void setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[channel] 切換到 %u\n", ch);
}

void startChannelScan() {
  if (scanning) return;

  // 安全預設：失去 master 時關閉繼電器，避免一直通電
  if (relayState) {
    setRelayPins(false);
    pulseActive = false;
    Serial.println("[安全] 失去 master，繼電器已關閉");
  }

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

// ── 非阻塞 LED 閃爍（配對結果指示）──
// ESP-IDF 明文要求 ESP-NOW 的 recv callback 在 WiFi task 執行、不可做冗長操作。
// 舊版在 callback 內直接跑阻塞式 blinkLed()，配對失敗那次是 3×400×2 = 2400ms，
// 配對瞬間會丟封包，最壞觸發 task watchdog reset。
// 現在 callback 只呼叫 requestBlink() 登記需求（純變數寫入），
// 實際閃爍由 loop() 的 updateBlink() 用 millis 推進狀態，完全不 delay()。
volatile uint8_t blinkRequestTimes = 0;      // 待執行的閃爍次數（由 callback 登記）
volatile uint16_t blinkRequestInterval = 0;  // 亮／滅各自的毫秒數

bool blinkActive = false;        // loop() 是否正在執行閃爍
uint8_t blinkRemaining = 0;      // 還剩幾次
uint16_t blinkInterval = 0;
bool blinkPhaseOn = false;       // 目前是「亮」的半週期
unsigned long blinkPhaseStart = 0;

// 供 ESP-NOW callback 呼叫：只寫旗標，立即返回
void requestBlink(uint8_t times, uint16_t intervalMs) {
  blinkRequestInterval = intervalMs;
  blinkRequestTimes = times;   // 次數最後寫，確保 loop() 讀到時間隔已就緒
}

// 取消閃爍：長按重置流程要接手同兩顆 LED 時呼叫，避免兩邊互相打架
void cancelBlink() {
  blinkRequestTimes = 0;
  blinkActive = false;
  blinkRemaining = 0;
}

void setBlinkLeds(bool on) {
  digitalWrite(ledOnFace, on ? HIGH : LOW);
  digitalWrite(ledOnBoard, on ? HIGH : LOW);
}

void updateBlink(unsigned long now) {
  // 有新的登記且目前沒在閃 → 啟動
  if (!blinkActive && blinkRequestTimes > 0) {
    blinkRemaining = blinkRequestTimes;
    blinkInterval = blinkRequestInterval;
    blinkRequestTimes = 0;
    blinkActive = true;
    blinkPhaseOn = true;
    blinkPhaseStart = now;
    setBlinkLeds(true);
    return;
  }
  if (!blinkActive) return;
  if (now - blinkPhaseStart < blinkInterval) return;   // 無號數減法，不怕 millis() 溢位

  blinkPhaseStart = now;
  if (blinkPhaseOn) {
    blinkPhaseOn = false;   // 亮的半週期結束 → 轉滅
    setBlinkLeds(false);
    return;
  }

  // 滅的半週期結束 → 完成一次閃爍
  blinkRemaining--;
  if (blinkRemaining == 0) {
    blinkActive = false;
    setBlinkLeds(relayState);   // 還原成繼電器對應的 LED 狀態
    return;
  }
  blinkPhaseOn = true;
  setBlinkLeds(true);
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
    // 沒有主動送出配對請求就收到 PAIR_ACK 一律忽略。
    // 否則任何持有共享密鑰的設備只要主動送一個 PAIR_ACK，就能無條件覆寫 masterMac
    // 並寫進 EEPROM，把一台已配對的 slave 劫持到自己名下。
    if (!waitingPairAck) {
      Serial.println("[配對] 收到非預期的 PAIR_ACK，忽略");
      return;
    }

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
      requestBlink(3, 100);   // 快閃 3 下表示成功（實際閃爍在 loop() 執行）
    } else {
      const char* why = (ack.reason == HO_PAIR_FULL) ? "master 已達 20 台上限"
                      : (ack.reason == HO_PAIR_NOT_PAIRING) ? "master 不在配對模式"
                      : "未知原因";
      Serial.printf("[配對] 被拒絕：%s\n", why);
      requestBlink(3, 400);   // 慢閃 3 下表示失敗（實際閃爍在 loop() 執行）
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
        pulseActive = false;
        Serial.println("[繼電器] 開啟");
        break;
      case HO_CMD_OFF:
        setRelayPins(false);
        pulseActive = false;
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
  delay(50);  // 等內部提升電阻把腳位拉穩再取樣

  checkStuckButtons();  // 必須早於任何重置流程，卡住的腳會在此被排除

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

  // ── 按鈕：短按配對，長按重置 ──
  // 兩顆按鈕任一顆都可以，與 ho_relay2 一致；
  // 開機自檢判定卡在 LOW 的腳會被 anyResetButtonPressed() 排除，
  // 避免壞按鈕造成「開機即清除配對 → 重啟 → 再清除」的無限迴圈
  bool anyPressed = anyResetButtonPressed();

  static bool lastAnyPressed = false;

  if (anyPressed && !lastAnyPressed) {
    resetPressTime = now;
  }

  if (anyPressed) {
    unsigned long pressDuration = now - resetPressTime;

    if (!resetBlinking && pressDuration >= LONG_PRESS_TIME) {
      resetBlinking = true;
      resetBlinkStart = now;
      cancelBlink();   // 重置閃爍要接手 LED，先取消配對結果的閃爍
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

  // ── 配對結果的 LED 閃爍（非阻塞，由 ESP-NOW callback 登記）──
  // 長按重置流程正在用同兩顆 LED 時讓給它，避免兩邊互搶
  if (!resetBlinking) {
    updateBlink(now);
  }

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
  if (pulseActive && (now - pulseStartTime) >= pulseDuration) {
    pulseActive = false;
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
