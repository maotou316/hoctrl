// 齁控 Slave — ESP-NOW 受控端
// 硬體：現有 ho_relay2 的 ESP32-C3 繼電器板
// 不連 WiFi、不跑 MQTT、不跑 BLE，只靠 ESP-NOW 接受 master 控制
#include <Arduino.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Update.h>
#include <HoEspNowProtocol.h>

// ── 韌體版本：唯一真相來源 ──
// 三段數字與 firmwareVersion 字串**必須只有一處可改**。舊寫法是這裡一個字串常數、
// sendState() 裡另外三個寫死的字面值 1／0／0，兩者之間沒有任何連結 ——
// 下一個人改了字串卻改不到欄位，映像仍回報舊版號。
// 那會直接製造一次**假失敗**：master 的 OTA_VERIFYING 階段唯一的驗收依據就是
// slaves[].fwMajor/fwMinor/fwPatch 是否等於它送出的目標版本，90 秒不符就判 no_return，
// 於是一次**實際完全成功**的 OTA 會被記成失敗。
//
// **它擋不住什麼**：它只保證「字串與欄位同步」，不保證「有人記得升版號」。
// 改了程式碼卻沒動這三個數字就重燒，master 一樣會等到 90 秒逾時 —— 那是誤紅方向。
#define HO_SLAVE_FW_MAJOR 1
#define HO_SLAVE_FW_MINOR 0
#define HO_SLAVE_FW_PATCH 0
#define HO_STRINGIFY_(x) #x
#define HO_STRINGIFY(x) HO_STRINGIFY_(x)

const char* firmwareVersion = HO_STRINGIFY(HO_SLAVE_FW_MAJOR) "."
                              HO_STRINGIFY(HO_SLAVE_FW_MINOR) "."
                              HO_STRINGIFY(HO_SLAVE_FW_PATCH);
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

// ── 指令歸因（協定版本 2）──
// 只有**實際走完**繼電器動作的那一次才更新，未知指令碼（switch 的 default）不算。
// 這三個值原樣進 HoStatePayload，讓 master 能把「這個 relay 值」歸因到某一道指令。
// **它們不證明繼電器硬體動作、也不證明籠門關上**，完整的「擋不住什麼」清單
// 寫在 HoEspNowProtocol.h 的 HoStatePayload 上方。
uint16_t lastCmdId = HO_CMD_ID_NONE;   // 最後一次實際執行的 HoCmdPayload::cmdId
uint8_t  lastCmdKind = 0;              // 那道指令的 HoRelayCmd
uint8_t  lastCmdCount = 0;             // 同一個 cmdId 被執行的次數，飽和於 255

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

// 收到心跳「印出序列埠」的降頻倍數（收訊與鎖定邏輯不受影響）。
// master 每 1 秒發一次，每 10 次印一行 ≈ 每 10 秒一行；
// master MAC／channel／配對模式有變化時仍會立即印，詳見 onEspNowRecv()
const int HEARTBEAT_LOG_EVERY = 10;

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

// ── OTA 接收狀態（Phase 4）──
// 區塊緩衝：先把一個區塊的 16 包收齊在 RAM，再一次順序寫進 Update。
// 這讓「亂序抵達」與「選擇性重傳」對 flash 寫入完全透明 ——
// Update 只看得到嚴格遞增的位元組流，不需要任何 seek 能力。
// 3840 bytes 放在檔案層級（.bss）而非函式區域變數：loopTask 堆疊只有 8192 bytes，
// 而 ESP-NOW recv callback 又跑在另一個 task 上，區域變數在這裡是明確的溢位風險。
static uint8_t otaBlockBuf[HO_OTA_WINDOW * HO_OTA_CHUNK_SIZE];

uint8_t  otaSession     = HO_OTA_SESSION_NONE;  // 0 = 目前沒有工作階段
bool     otaActive      = false;  // Update.begin() 已成功、尚未 end/abort
uint32_t otaTotalSize   = 0;
uint16_t otaTotalChunks = 0;
uint16_t otaBlockBase   = 0;      // 目前區塊第一包的 chunkIndex
uint16_t otaBlockMask   = 0;      // bit i = 已收到 otaBlockBase + i
uint32_t otaWritten     = 0;      // 已寫進 Update 的位元組數
unsigned long otaLastPacketAt = 0;

// 這麼久沒收到任何 OTA 封包就自行中止並釋放 Update。
// 與失聯門檻同樣是 30 秒，語義一致：master 不見了就回到已知安全狀態。
const unsigned long OTA_SLAVE_IDLE_MS = 30000;

void sendState() {
  if (!masterKnown) return;

  HoStatePayload st;
  st.relay = relayState ? 1 : 0;
  // 版本三段一律取自 HO_SLAVE_FW_* 這唯一來源（firmwareVersion 字串也是由它們組出來的），
  // 不得改回字面值 —— master 的 OTA_VERIFYING 只認這三個欄位
  st.fwMajor = HO_SLAVE_FW_MAJOR;
  st.fwMinor = HO_SLAVE_FW_MINOR;
  st.fwPatch = HO_SLAVE_FW_PATCH;
  st.uptimeSec = millis() / 1000;
  st.lastCmdId = lastCmdId;
  st.lastCmdKind = lastCmdKind;
  st.lastCmdCount = lastCmdCount;

  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), HO_PKT_STATE, txSeq++, &st, sizeof(st));
  esp_now_send(masterMac, buf, total);
  Serial.printf("[狀態] 已回報 relay=%u\n", st.relay);
}

// 中止目前的 OTA 工作階段。
// Update.abort() 只是丟棄內部狀態，「不會」碰 otadata ——
// 開機分區維持指向目前正在跑的這一份韌體，所以中止永遠不會變磚。
//
// **它擋不住什麼**：它只保證「這一台在中止時不會變磚」。
// 它不保證新韌體本身開得起來 —— 通過 MD5、切換分區、但新韌體在 setup() 早期就 crash
// 的情況，本階段沒有任何軟體覆蓋（見 ho_slave1/readme.md 的已知限制與 plan 決定 2.5）。
// 它也不會關閉繼電器：關繼電器是失聯保護（startChannelScan）的職責，不是這裡。
void otaAbort(const char* why) {
  if (!otaActive && otaSession == HO_OTA_SESSION_NONE) return;
  if (otaActive) Update.abort();
  Serial.printf("[OTA] 已中止：%s（已寫入 %u/%u bytes，開機分區未變動）\n",
                why, (unsigned)otaWritten, (unsigned)otaTotalSize);
  otaActive = false;
  otaSession = HO_OTA_SESSION_NONE;
  otaWritten = 0;
  otaBlockMask = 0;
  otaBlockBase = 0;
}

// 回一封 OTA_ACK 給 master。mask 只在回報區塊進度時有意義。
//
// ── status 的分工（本 Task review 的 C2；產生端就得分清楚）──
//   HO_OTA_READY  ＝「工作階段還在進行中，這封帶的是我目前的進度」
//                    （BEGIN 接受、重複 BEGIN、區塊收齊、回覆 master 的查詢，四處都用它）
//   HO_OTA_OK     ＝「**整份校驗通過、我要重開機了**」，全檔**只有一處**產生，
//                    而且固定帶 (blockBase = otaTotalChunks, mask = 0xFFFF) 當正向識別
//   其餘          ＝ 各自的錯誤碼
//
// 為什麼非分不可：master 的 OTA_END_SENT 階段**只看 status**（見 plan Task 4
// `case OTA_END_SENT:` 的 `if (otaAckStatus == HO_OTA_OK)`）。原本查詢回覆在
// otaBlockBase==0 && otaBlockMask==0 時產生的 (0, 0, HO_OTA_OK) 與「校驗通過」那封
// **逐位元組相同**，一封延遲抵達的查詢回覆就會讓 master 印「slave 校驗通過」——
// 那是加出來的綠燈。區塊收齊的回覆同理（它也曾用 HO_OTA_OK）。
// 這也才對得上協定標頭自己的定義：`HO_OTA_OK = 0, // 校驗通過，即將重啟`。
//
// **它擋不住什麼**：它只讓「校驗通過」這封無法被別的回覆冒充。
// 它不防重放 —— 協定沒有加密也沒有 nonce，錄下真正那封 (totalChunks, 0xFFFF, OK)
// 再重播，master 一樣會信。這一層要等協定加上工作階段內的單向計數才擋得住。
void otaSendAck(uint16_t blockBase, uint16_t mask, uint8_t status) {
  if (!masterKnown) return;
  HoOtaAckPayload ack;
  ack.sessionId = otaSession;
  ack.blockBase = blockBase;
  ack.mask      = mask;
  ack.status    = status;

  uint8_t buf[250];
  size_t total = hoPackPacket(buf, sizeof(buf), HO_PKT_OTA_ACK, txSeq++, &ack, sizeof(ack));
  esp_now_send(masterMac, buf, total);
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
  //
  // ⚠ 這一段**必須排在 otaAbort() 之前**。繼電器動作是本檔案唯一的實體安全輸出，
  // 不能排在任何「可能變慢」的東西後面：otaAbort() 會走 Update.abort() 並印一行
  // 序列埠訊息（115200 下數十 bytes 約 數 ms），Serial 緩衝滿時還會等。
  // 量級雖小，但排序原則不該讓步 —— **先進入已知安全狀態，再收拾資源。**
  if (relayState) {
    setRelayPins(false);
    pulseActive = false;
    Serial.println("[安全] 失去 master，繼電器已關閉");
  }

  // 失去 master 就不可能繼續接收，把 Update 釋放掉。
  // 不釋放的話，flash 分區會被一個永遠不會完成的工作階段占著，
  // 下一次 OTA 的 Update.begin() 會失敗。
  otaAbort("失去 master，開始輪掃");

  scanning = true;
  scanChannel = 1;
  scanChannelStart = millis();
  setChannel(scanChannel);
  Serial.println("[掃描] 開始輪掃 channel 1~13 尋找 master");
}

void onMasterFound(const uint8_t mac[6], uint8_t channel) {
  bool isNewMaster = !masterKnown || memcmp(masterMac, mac, 6) != 0;
  bool channelChanged = (lockedChannel != channel);
  bool wasScanning = scanning;   // 這次是「從輪掃中恢復」

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

  // 「從輪掃中恢復」也要印 [鎖定]：master 只是重開機、channel 沒變時，
  // isNewMaster 與 channelChanged 都是 false，加上心跳 log 已降頻，
  // 序列埠上會完全看不出恢復發生過，回歸清單第 10 項就無從判定成功
  if (isNewMaster || channelChanged || wasScanning) {
    char id[20];
    hoFormatDeviceId(mac, id);
    Serial.printf("[鎖定] master=%s channel=%u\n", id, channel);
  }

  // EEPROM 只在真的換了 master 或 channel 時才寫，
  // 單純的掃描恢復不必多寫一次（減少 EEPROM 寫入次數）
  if ((isNewMaster || channelChanged) && masterKnown) {
    savePairing();
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
    // ── flag-day 告警：讓「協定版本不相容」看得見 ──
    // 沒有這一段，v1 master 配 v2 slave 的現場症狀是「兩邊都在跑、就是完全不通、
    // 30 秒後 [失聯] 然後 [安全] 失去 master，繼電器已關閉」，而原因不顯示。
    // 節流成每 10 秒最多一行：master 每秒廣播一次心跳，不節流會直接洗版。
    //
    // **它擋不住什麼**：只是回報，不做任何補救。封包照樣被丟棄、心跳照樣中斷、
    // 30 秒失聯保護照樣觸發。它也偵測不到「版本相同但欄位語義改了」的不相容。
    uint8_t theirVersion = 0;
    if (hoPeekVersionMismatch(data, (size_t)len, &theirVersion)) {
      static unsigned long lastVerWarn = 0;
      unsigned long now = millis();
      if (lastVerWarn == 0 || (now - lastVerWarn) >= 10000) {
        lastVerWarn = now;
        Serial.printf("⚠ [協定] 收到版本 %u 的封包，本機是版本 %u，全部丟棄；"
                      "master 與所有 slave 必須一起重燒\n",
                      theirVersion, (unsigned)HO_ESPNOW_VERSION);
      }
    }
    return;
  }

  if (header.type == HO_PKT_HEARTBEAT && payloadLen >= sizeof(HoHeartbeatPayload)) {
    HoHeartbeatPayload hb;
    memcpy(&hb, payload, sizeof(hb));

    // 已配對的 slave 只理會自己的 master
    if (masterKnown && memcmp(masterMac, info->src_addr, 6) != 0) {
      return;
    }

    // ── 心跳 log 降頻 ──
    // master 每 1 秒廣播一次心跳，若每次都印，序列埠會被洗版、
    // 人工照回歸清單逐項比對時看不到其他訊息。
    // 只降低印出頻率，收訊與鎖定邏輯完全不受影響（[鎖定] 那行仍照舊印）；
    // master MAC／channel／配對模式任一項與上次印出時不同就立即印，狀態變化不會被吃掉。
    static int hbLogCounter = 0;
    static uint8_t lastLoggedMac[6] = { 0 };
    static uint8_t lastLoggedChannel = 0xFF;   // 0xFF 為不可能值，確保第一次必定印
    static uint8_t lastLoggedPairing = 0xFF;

    hbLogCounter++;
    bool logChanged = (memcmp(lastLoggedMac, info->src_addr, 6) != 0) ||
                      (hb.channel != lastLoggedChannel) ||
                      (hb.pairingMode != lastLoggedPairing);

    if (logChanged || hbLogCounter >= HEARTBEAT_LOG_EVERY) {
      hbLogCounter = 0;
      memcpy(lastLoggedMac, info->src_addr, 6);
      lastLoggedChannel = hb.channel;
      lastLoggedPairing = hb.pairingMode;

      char id[20];
      hoFormatDeviceId(info->src_addr, id);
      Serial.printf("[心跳] 來自 %s channel=%u 配對模式=%s rssi=%d\n",
                    id, hb.channel, hb.pairingMode ? "是" : "否", info->rx_ctrl->rssi);
    }

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

  // ── OTA 期間的失聯保護（Phase 4 決定 1a）──
  // 走到這裡代表：封包通過了 magic/version/CRC 驗證（CRC 含共享密鑰與標頭），
  // 而且來源正是本機已配對的 master。這比廣播心跳是更強的存活證據 ——
  // 單播同時證明了「master 活著」和「我跟它在同一個 channel 上」，廣播只證明前者。
  //
  // 沒有這一行的話：OTA 轉送要跑 30~90 秒，期間 ESP-NOW 通道被單播塞滿，
  // 沒有 ACK、沒有重傳的廣播心跳丟包率明顯上升。一旦累積 30 秒沒收到心跳，
  // startChannelScan() 會 (1) 強制關閉繼電器＝籠子被打開，
  // (2) 開始每 1200ms 換一個 channel，當場把正在進行的 OTA 打斷。
  //
  // 這是收緊而非放寬：沒有封包就是沒有封包，30 秒計時照常走完。
  //
  // **它擋不住什麼**：它只延長「判定失聯」的時機，不改變失聯後的動作。
  // master 真的掛掉、或跳到別的 channel 之後，30 秒照樣走完、繼電器照樣被強制關閉。
  // 它也不保證 OTA 一定不被打斷 —— 只要連續 30 秒一封 master 封包都沒收到
  //（例如整個區塊連續重送都失敗），輪掃仍會啟動並中止 OTA。
  lastHeartbeatTime = millis();

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

    // ── 指令歸因：只有真的走完上面某一個 case 才會執行到這裡 ──
    // default 分支已經 return，所以「未知指令」不會被記成執行過 ——
    // 那正是誤綠方向，必須擋住。
    //
    // 同一個 cmdId 再次進來就累加次數，不重置。群組指令會廣播 3 次再加至少一次
    // 單播（見 master 的 sendCmdToAll()），所以 lastCmdCount 通常是 4，
    // 補送還會更多。**這是設計行為，不是異常**：master 只比對 cmdId，不看次數。
    if (cmd.cmdId != HO_CMD_ID_NONE) {
      if (cmd.cmdId == lastCmdId && lastCmdCount < 255) {
        lastCmdCount++;
      } else if (cmd.cmdId != lastCmdId) {
        lastCmdId = cmd.cmdId;
        lastCmdCount = 1;
      }
      lastCmdKind = cmd.cmd;
    }
    sendState();
    return;
  }

  if (header.type == HO_PKT_STATE_REQ) {
    sendState();
    return;
  }

  if (header.type == HO_PKT_OTA_BEGIN && payloadLen >= sizeof(HoOtaBeginPayload)) {
    HoOtaBeginPayload bg;
    memcpy(&bg, payload, sizeof(bg));

    // 同一個 sessionId 再次收到 BEGIN：多半是我的 READY 回程丟了，master 重發。
    // 重回一次 READY 就好，「絕不」重置已經收到的進度。
    if (otaActive && bg.sessionId == otaSession) {
      otaLastPacketAt = millis();
      Serial.println("[OTA] 重複收到 OTA_BEGIN，重新回覆 READY（進度保留）");
      otaSendAck(otaBlockBase, otaBlockMask, HO_OTA_READY);
      return;
    }

    // 已有其他工作階段：只有在它已經很久沒動靜（殘留）時才讓位。
    //
    // ⚠ 這裡「刻意不」在分支開頭就刷新 otaLastPacketAt（計畫書原文是在開頭刷新，
    //    實作時改掉）。理由：BEGIN 也是 OTA 封包，先刷新的話下面這個
    //    「millis() - otaLastPacketAt < OTA_SLAVE_IDLE_MS」差值恆為 0，殘留判定永遠不成立；
    //    更糟的是每一次被拒絕的 BEGIN 都會把時間戳推後，連 loop() 的 30 秒逾時
    //    也永遠不會觸發 —— 一個殘留的工作階段會把 slave 永久鎖在 BUSY，只能斷電。
    //    現在只有「屬於目前工作階段的封包」才刷新時間戳。
    if (otaActive && bg.sessionId != otaSession) {
      if (millis() - otaLastPacketAt < OTA_SLAVE_IDLE_MS) {
        Serial.printf("[OTA] 已有工作階段 %u 進行中，拒絕 %u\n", otaSession, bg.sessionId);
        // 暫時借用 otaSession 欄位讓 master 認得出這封 ACK 是回給誰的，回完「立刻還原」。
        // 計畫書原文是還原成 0，那會把正在進行的工作階段編號抹掉 ——
        // 之後合法 master 送來的 OTA_DATA 全部會因 sessionId 不符而被靜默丟棄，
        // 等於被一封來路不明的 BEGIN 打斷整場 OTA。
        uint8_t keepSession = otaSession;
        otaSession = bg.sessionId;
        otaSendAck(0, 0, HO_OTA_ERR_BUSY);
        otaSession = keepSession;
        return;
      }
      // 走到這裡代表殘留超過 30 秒。實務上 loop() 的逾時通常會先一步 abort，
      // 這是同一條判準的第二道，不依賴 loop() 的排程時機。
      otaAbort("上一個工作階段殘留");
    }

    otaLastPacketAt = millis();
    otaSession = bg.sessionId;

    // 合理性檢查：擋掉明顯錯誤的長度，避免白白抹掉整個分區
    if (bg.totalSize < 65536 || bg.totalSize > 2031616 ||
        bg.totalChunks == 0 || bg.totalChunks > HO_OTA_MAX_CHUNKS) {
      Serial.printf("[OTA] 拒絕：長度不合理 size=%u chunks=%u\n",
                    (unsigned)bg.totalSize, bg.totalChunks);
      otaSendAck(0, 0, HO_OTA_ERR_SIZE);
      otaSession = HO_OTA_SESSION_NONE;
      return;
    }

    if (!Update.begin(bg.totalSize, U_FLASH)) {
      Serial.printf("[OTA] Update.begin 失敗，錯誤碼 %u（可用空間 %u）\n",
                    Update.getError(), (unsigned)ESP.getFreeSketchSpace());
      otaSendAck(0, 0, HO_OTA_ERR_BEGIN);
      otaSession = HO_OTA_SESSION_NONE;
      return;
    }

    // 把期望的 MD5 交給 Update。`Update.end(true)` 會在 `_verifyEnd()`
    //（也就是 `esp_ota_set_boot_partition()` 那一步）「之前」比對它，不符就回 false 且不切換。
    //
    // ⚠ 這是**唯一**擋得住「內容錯誤或長度截斷」的一道：
    // `end(true)` 的 evenIfRemaining 分支會把 `_size = progress()`
    //（esp32 core 3.3.7 `Updater.cpp` 的 `bool UpdateClass::end(bool evenIfRemaining)`
    //  裡的 `_size = progress();`）—— 也就是說 **`end(true)` 根本不驗長度**，
    // 它只驗 MD5。本檔案曾寫成「end(true) 會依序檢查長度與 MD5」，那是假宣稱。
    char md5hex[33];
    for (int i = 0; i < 16; i++) snprintf(md5hex + i * 2, 3, "%02x", bg.md5[i]);
    Update.setMD5(md5hex);

    otaActive      = true;
    otaTotalSize   = bg.totalSize;
    otaTotalChunks = bg.totalChunks;
    otaBlockBase   = 0;
    otaBlockMask   = 0;
    otaWritten     = 0;

    Serial.printf("[OTA] 開始接收：%u bytes／%u 包，目標版本 %u.%u.%u，MD5 %s\n",
                  (unsigned)bg.totalSize, bg.totalChunks,
                  bg.verMajor, bg.verMinor, bg.verPatch, md5hex);
    otaSendAck(0, 0, HO_OTA_READY);
    return;
  }

  if (header.type == HO_PKT_OTA_DATA && payloadLen > sizeof(HoOtaDataPayload)) {
    HoOtaDataPayload dh;
    memcpy(&dh, payload, sizeof(dh));
    if (!otaActive || dh.sessionId != otaSession) return;   // 殘留封包，靜默丟棄

    otaLastPacketAt = millis();

    size_t dataLen = payloadLen - sizeof(HoOtaDataPayload);
    if (dataLen == 0 || dataLen > HO_OTA_CHUNK_SIZE) return;

    // 超出本次宣告包數的塊號一律丟棄（計畫書沒有這一條，實作時補上）。
    // 沒有它的話，最後一個區塊寫完之後 otaBlockBase == otaTotalChunks，
    // 一封 chunkIndex 落在 [otaBlockBase, otaBlockBase+16) 但 >= totalChunks 的封包
    // 會讓 need 算成 0、fullMask 算成 0，於是「條件當場成立」跑進零長度寫入並回一封
    // 假的 OTA_OK。**這是誤綠方向**，所以擋在最前面。
    // **它擋不住什麼**：它只檢查塊號範圍，不檢查資料內容 ——
    // 塊號合法但內容錯誤的封包照收，那一層由整份韌體的 MD5 在 Update.end(true) 攔下。
    if (dh.chunkIndex >= otaTotalChunks) return;

    // 長度必須「剛好等於這個塊號應有的長度」：中段一律 240，只有最後一包可以短，
    // 而且短多少是由 totalSize 與 totalChunks 唯一決定的。
    //
    // 沒有這一條的話：一封中段短包只覆寫 otaBlockBuf 那一格的前段，
    // **尾巴是前一個區塊留下的舊位元組**（緩衝從不清空 —— 也刻意不清空：
    // 每塊清 3840 bytes 只是把成本換個地方付，真正該做的是不接受長度不對的包），
    // 而 bitmap 照樣記成「這一格收到了」，湊滿之後那段殘留就被寫進映像。
    //
    // **它擋不住什麼**：它只檢查長度。長度剛好、內容錯誤的封包照收照寫 ——
    // 那一層只有整份韌體的 MD5 在 Update.end(true) 擋得住（而且是整份重來，不是重傳那一包）。
    uint32_t expectLen = (dh.chunkIndex == (uint16_t)(otaTotalChunks - 1))
                           ? (otaTotalSize - (uint32_t)(otaTotalChunks - 1) * HO_OTA_CHUNK_SIZE)
                           : (uint32_t)HO_OTA_CHUNK_SIZE;
    if ((uint32_t)dataLen != expectLen) return;

    // 只收目前這個區塊內的包。前一個區塊的重複包（master 沒收到 ACK 而重送）
    // 落在區間外，直接丟棄即可 —— 資料已經寫進 Update 了。
    if (dh.chunkIndex < otaBlockBase || dh.chunkIndex >= otaBlockBase + HO_OTA_WINDOW) return;

    uint16_t slot = dh.chunkIndex - otaBlockBase;
    memcpy(otaBlockBuf + (size_t)slot * HO_OTA_CHUNK_SIZE,
           payload + sizeof(HoOtaDataPayload), dataLen);
    otaBlockMask |= (uint16_t)(1u << slot);

    // 這個區塊要幾包才算滿？最後一個區塊可能不足 16 包。
    uint16_t need = HO_OTA_WINDOW;
    if ((uint32_t)otaBlockBase + HO_OTA_WINDOW > otaTotalChunks) {
      need = (uint16_t)(otaTotalChunks - otaBlockBase);
    }
    uint16_t fullMask = (need >= 16) ? 0xFFFF : (uint16_t)((1u << need) - 1u);
    if ((otaBlockMask & fullMask) != fullMask) return;   // 還沒收齊，等 master 查詢

    // 收齊了 → 一次順序寫入。最後一個區塊的長度要用總長度回推，不能假設是滿的。
    //
    // ⚠ 這一行會阻塞 WiFi task（本函式就跑在 WiFi task 上），代價比原本估的大一級：
    // esp32 core 3.3.7 的 `Updater.cpp` 在 `_writeBuffer()` 裡是
    // `ESP.partitionEraseRange(_partition, _progress, block_erase ? SPI_FLASH_BLOCK_SIZE : SPI_FLASH_SEC_SIZE)`，
    // 而 `Update.h` 的 `#define SPI_SECTORS_PER_BLOCK 16` 讓 SPI_FLASH_BLOCK_SIZE ＝ **64 KB**
    //（不是 4 KB 扇區）。988 KB 的映像會跨 15 次 64 KB 邊界，每次抹除依 plan 的換算是
    // **60~190 ms**，期間本 task 完全不處理任何 ESP-NOW 收包。
    //
    // **它擋不住什麼（這是誠實敘述，不是防線）**：那 60~190 ms 內抵達的封包，
    // MAC 層 ACK 由硬體回、master 因此認定「已送達」，但應用層是否收得到取決於
    // RX 佇列有沒有滿 —— 而轉送期間 master 正在全速灌 OTA_DATA，佇列本來就吃緊。
    // **群組安全指令（App 的「全部關門」送的是 ALL:ON ＝廣播 PULSE）在這個窗口內
    // 有可能被靜默丟失，而 master 端的補送判準是 MAC 層 ACK，接不住這一種。**
    // 唯一會讓它現形的是 Phase 2b 的指令歸因（該台不會回報 lastCmdId →
    // master 印「⚠ [群組]   無執行證明：<id>」），那是回報、不是補救。
    // 暴露面僅限「正在接收 OTA 的那一台」，但**不要把它讀成暴露面很小**：
    // Task 5 對 relay==1 的 slave 預設拒絕 OTA（除非帶 force:true），
    // 所以 **OTA 目標依建構必然是 relay==0 的那一台 —— 也就是門還開著、
    // 正是「全部關門」要去關的那一台**。那道拒絕擋的是另一件事
    //（避免 OTA 重啟把正在通電的繼電器斷開），對「一次要全部關」
    // **一點暴露面都沒有減少**。本階段沒有解，補救屬於 Task 4／5。
    uint32_t remain = otaTotalSize - otaWritten;
    size_t writeLen = (size_t)need * HO_OTA_CHUNK_SIZE;
    if (writeLen > remain) writeLen = remain;

    size_t wrote = Update.write(otaBlockBuf, writeLen);
    if (wrote != writeLen) {
      Serial.printf("[OTA] 寫入失敗：預期 %u 實際 %u，錯誤碼 %u\n",
                    (unsigned)writeLen, (unsigned)wrote, Update.getError());
      otaSendAck(otaBlockBase, otaBlockMask, HO_OTA_ERR_WRITE);
      otaAbort("寫入失敗");
      return;
    }
    otaWritten += wrote;

    // 主動回報「這一塊收齊了」，master 就不必等查詢逾時，直接推進下一塊。
    // status 用 HO_OTA_READY（＝進度回報）而非 HO_OTA_OK —— 理由見 otaSendAck() 上方，
    // master 的 OTA_WAIT_BLOCK_ACK 只看 blockBase 與 mask，不看 status，所以這樣改不影響推進。
    otaSendAck(otaBlockBase, fullMask, HO_OTA_READY);

    otaBlockBase += need;
    otaBlockMask = 0;

    // 每 40 個區塊（約 150 KB）印一行，避免序列埠被 270 行進度洗版
    if ((otaBlockBase / HO_OTA_WINDOW) % 40 == 0) {
      Serial.printf("[OTA] 進度 %u%%（%u/%u bytes）\n",
                    (unsigned)(otaWritten * 100 / otaTotalSize),
                    (unsigned)otaWritten, (unsigned)otaTotalSize);
    }
    return;
  }

  // 到達 slave 的 OTA_ACK 一律是 master 的查詢：「這個區塊你收到哪幾包了？」
  // 方向本身就是語義，不需要額外的 request 欄位。
  if (header.type == HO_PKT_OTA_ACK && payloadLen >= sizeof(HoOtaAckPayload)) {
    HoOtaAckPayload q;
    memcpy(&q, payload, sizeof(q));
    if (!otaActive || q.sessionId != otaSession) return;
    otaLastPacketAt = millis();

    // 查詢的區塊已經被我寫完並推進了 → 回一個「全滿」讓 master 直接往前走。
    // 兩條路徑的 status 都是 HO_OTA_READY（進度回報），**絕不可改回 HO_OTA_OK**：
    // 在 otaBlockBase==0 && otaBlockMask==0 時，(0, 0, HO_OTA_OK) 與「整份校驗通過」
    // 那封逐位元組相同，而 master 的 OTA_END_SENT 只看 status。
    if (q.blockBase < otaBlockBase) {
      otaSendAck(q.blockBase, 0xFFFF, HO_OTA_READY);
      return;
    }
    otaSendAck(otaBlockBase, otaBlockMask, HO_OTA_READY);
    return;
  }

  if (header.type == HO_PKT_OTA_END && payloadLen >= sizeof(HoOtaEndPayload)) {
    HoOtaEndPayload en;
    memcpy(&en, payload, sizeof(en));
    // sessionId 不符一律「靜默」丟棄，不回 HO_OTA_ERR_SESSION。
    // 這也是全檔 HO_OTA_ERR_SESSION 沒有產生點的原因，刻意如此：
    // 會走到這裡的是上一場失敗留下的殘留封包，對一個「已經不存在的工作階段」回 ACK
    // 只會在 master 那邊製造回音（它的 onEspNowRecv 只比對 sessionId 後就設旗標），
    // 而 master 端本來就有查詢逾時與 5 分鐘總上限會把它收掉 —— **誤紅方向，可接受**。
    // **它擋不住什麼**：代價是 master 分不出「我送錯 session」與「slave 不見了」，
    // 兩者都只會表現成逾時。真的需要區分時，該補的是 master 端的診斷，不是這裡多回一封。
    if (en.sessionId != otaSession) return;
    otaLastPacketAt = millis();

    if (en.abort) {
      otaSendAck(0, 0, HO_OTA_ABORTED);
      otaAbort("master 指示中止");
      return;
    }

    // 進 Update.end() 之前先自己比一次長度。
    //
    // ⚠ 這一道**不是**「end(true) 的第二層」，它擋的是 end(true) **根本看不到**的東西：
    // master 在 OTA_END 裡宣告的 totalSize 與它在 OTA_BEGIN 宣告的不一致。
    // 位元組流本身可能完全正確（MD5 會過、end(true) 會回 true 並切換開機分區），
    // 兩邊對長度的認知卻已經分歧 —— 只有這裡比得出來。
    //
    // 至於「我實際寫入量 < 宣告長度」（截斷），擋下它的是 **MD5**，不是長度：
    // esp32 core 3.3.7 的 `end(true)` 會先 `_size = progress();` 再比 MD5，
    // **等於放棄長度檢查**。本檔案原本的註釋寫「end(true) 會依序檢查長度與 MD5」，
    // 那是照語義模型寫的假宣稱，已更正。
    if (!otaActive || otaWritten != en.totalSize || otaWritten != otaTotalSize) {
      Serial.printf("[OTA] 長度不符：已寫 %u，master 宣告 %u\n",
                    (unsigned)otaWritten, (unsigned)en.totalSize);
      otaSendAck(0, 0, HO_OTA_ERR_MD5);
      otaAbort("長度不符");
      return;
    }

    // end(true) 的實際行為（照 esp32 core 3.3.7 的 `Updater.cpp` 讀出來的，不是推的）：
    //   1. `_size = progress();` —— **跳過長度檢查**
    //   2. 比對 `setMD5()` 設定的 MD5，不符就 `_abort(UPDATE_ERROR_MD5)` 回 false
    //   3. 才進 `_verifyEnd()` → `_enablePartition()`（補寫回開頭 16 bytes）
    //      → `_partitionIsBootable()` → `esp_ota_set_boot_partition()`
    // 所以擋住「內容錯」與「長度截斷」的是第 2 步的 MD5，全靠它一道。
    // 另外第 3 步之前，映像的開頭 16 bytes 一直是空的（`Updater.cpp` 在第一次寫入時
    // 把它們 stash 起來不寫），半途中斷的映像因此開不起來 —— 這是 plan 沒列到的第六道。
    if (!Update.end(true)) {
      Serial.printf("[OTA] 校驗失敗，錯誤碼 %u —— 開機分區未變動，重啟後仍是舊韌體\n",
                    Update.getError());
      otaSendAck(0, 0, HO_OTA_ERR_MD5);
      otaAbort("校驗失敗");
      return;
    }

    Serial.println("[OTA] 校驗通過，1 秒後重新啟動");
    // 全檔唯一一處產生 HO_OTA_OK，且固定帶 (otaTotalChunks, 0xFFFF) 當正向識別 ——
    // 查詢回覆與區塊回報都不可能產生這個組合（它們的 blockBase 恆 < otaTotalChunks，
    // 而 blockBase == otaTotalChunks 的查詢回覆走不到「全滿」那條路徑）。
    otaSendAck(otaTotalChunks, 0xFFFF, HO_OTA_OK);
    otaActive = false;
    // 給 ACK 送出的時間再重啟。這裡用裸 delay() 是可以的：
    // 本機即將重開機，繼電器與心跳計時都會被重置，沒有「維持心跳」可言。
    delay(1000);
    ESP.restart();
    return;
  }

  if (header.type == HO_PKT_UNPAIR) {
    otaAbort("收到解除配對");
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

  // OTA 工作階段逾時：master 不見了就回到已知安全狀態，釋放 Update
  if (otaActive && now - otaLastPacketAt >= OTA_SLAVE_IDLE_MS) {
    otaAbort("超過 30 秒沒收到 OTA 封包");
  }

  // 已鎖定：超時沒收到心跳就重新掃描
  if (now - lastHeartbeatTime > HEARTBEAT_TIMEOUT) {
    Serial.println("[失聯] 超過 30 秒沒收到心跳");
    startChannelScan();
  }
}
