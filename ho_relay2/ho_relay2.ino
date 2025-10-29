#include <Arduino.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <PubSubClient.h> // 引入 MQTT 庫
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>  // 添加這行
#include <Update.h>       // 引入 Update 庫
#include <HTTPClient.h>   // 添加 HTTPClient 庫
#include <WiFiClientSecure.h>  // 添加 WiFiClientSecure 庫

// uPesy ESP32 WROOM DevKit
// LED 閃爍模式定義
const unsigned long SHORT_BLINK = 200;  // 短閃持續時間 (毫秒)
const unsigned long LONG_BLINK = 800;   // 長閃持續時間 (毫秒)
const unsigned long PATTERN_PAUSE = 2000; // 模式間暫停時間 (毫秒)
const unsigned long QUICK_BLINK = 100;   // 快閃間隔時間 (毫秒)

// LED 閃爍狀態變數
unsigned long lastBlinkTime = 0;
int blinkPattern = 0;  // 用於追蹤當前閃爍模式位置
bool ledState = false;

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic = NULL;
bool deviceConnected = false;


const char* firmwareVersion = "1.2.1"; // 當前韌體版本
const char* deviceModel = "hoRelay2"; // 設備型號

// ESP32-C3 GPIO 定義
const int bootButton = 9;     // BOOT 按鈕在 GPIO 9
const int resetButton = 1;        // 


const int ledOnBoard = 3;    // 第二個按鈕在 GPIO 8
const int ledOnFace = 0;        // 
const int relayButton = 4;      // 繼電器在 GPIO 4

// 其他全域變數
unsigned long buttonPressTime = 0;    // 記錄按下的時間
unsigned long button2PressTime = 0;   // 第二個按鈕按下的時間
unsigned long ledBlinkStart = 0;      // LED 開始閃爍的時間
const int LONG_PRESS_TIME = 3000;     // 第一階段長按 3 秒
const int BLINK_TIME = 3000;          // LED 閃爍 3 秒
const int CONFIRM_TIME = 3000;        // 確認等待 3 秒
bool isBlinking = false;              // LED 閃爍狀態
bool waitingConfirm = false;          // 等待確認狀態
String deviceIdString;                // 儲存格式化後的設備 ID
int failedAttempts = 0;               // MQTT 重試次數計數器
bool isAPMode = false;                // AP 模式標誌
bool relayState = false;              // 繼電器狀態

// WiFi 設定（預設值）
char ssid[32] = "HBTech";
char password[32] = "94051311";

// 自訂 MQTT 伺服器設定（透過 App 配置，儲存在 EEPROM）
char mqttServer[32] = "";
char mqttUsername[16] = "";
char mqttPassword[16] = "";
int mqttPort = 1883;

// MQTT 伺服器配置結構
struct MqttServerConfig {
    const char* server;
    int port;
    const char* username;
    const char* password;
    const char* displayName;
};

// 5 個預設伺服器配置（所有資訊寫死在這裡）
const MqttServerConfig DEFAULT_SERVERS[5] = {
    // 伺服器地址, Port, 帳號, 密碼, 顯示名稱
    {"mqttgo.io", 1883, NULL, NULL, "台灣 MQTT Go"},
    {"broker.hoban.tw", 1883, NULL, NULL, "齁斑社企"},  // ← 請修改為實際帳密
    {"mqtt.eclipseprojects.io", 1883, NULL, NULL, "Eclipse"},
    {"broker.emqx.io", 1883, NULL, NULL, "EMQX 公共"},
    {"broker.hivemq.com", 1883, NULL, NULL, "HiveMQ 公共"}
};

const int SERVER_COUNT = 5;
int currentServerIndex = 0;         // 當前使用的伺服器索引
bool useCustomServer = false;       // 是否使用自訂伺服器

WiFiClient espClient; // MQTT 客戶端
PubSubClient mqttClient(espClient);

// 韌體更新相關
bool isUpdating = false;
int updateProgress = 0;



// BLE 連接回調
class MyServerCallbacks: public BLEServerCallbacks {
    
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    }

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};



// 函數前向宣告
void saveWiFiConfig();
void loadWiFiConfig();
void clearWiFiConfig();

// WiFi 設定相關函數實作
void saveWiFiConfig() {
  EEPROM.begin(128);
  // 儲存 WiFi 設定
  for (int i = 0; i < 32; i++) {
    EEPROM.write(i, ssid[i]);
    EEPROM.write(i + 32, password[i]);
  }
  // 儲存自訂 MQTT 伺服器設定
  for (int i = 0; i < 32; i++) {
    EEPROM.write(i + 64, mqttServer[i]);
  }
  // 儲存 MQTT 認證資訊
  for (int i = 0; i < 16; i++) {
    EEPROM.write(i + 98, mqttUsername[i]);   // 98-113: MQTT 帳號
    EEPROM.write(i + 114, mqttPassword[i]);  // 114-129: MQTT 密碼
  }
  // 儲存 MQTT Port (2 bytes)
  EEPROM.write(126, mqttPort & 0xFF);        // 低位元組
  EEPROM.write(127, (mqttPort >> 8) & 0xFF); // 高位元組

  EEPROM.commit();
}

void loadWiFiConfig() {
  EEPROM.begin(128);

  // 檢查 EEPROM 是否已初始化（檢查第一個字元是否為可列印字元或 NULL）
  char firstChar = EEPROM.read(0);
  bool isEEPROMValid = (firstChar >= 32 && firstChar <= 126) || firstChar == 0;

  if (!isEEPROMValid) {
    // EEPROM 未初始化或資料無效，使用預設值並儲存
    Serial.println("EEPROM 未初始化，使用預設 WiFi 設定");
    // ssid 和 password 已經有預設值（HBTech / 94051311）
    // mqttServer 等保持空白
    saveWiFiConfig();  // 將預設值寫入 EEPROM
    return;
  }

  // 讀取 WiFi 設定
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(i);
    password[i] = EEPROM.read(i + 32);
  }
  // 讀取自訂 MQTT 伺服器設定
  for (int i = 0; i < 32; i++) {
    mqttServer[i] = EEPROM.read(i + 64);
  }
  // 讀取 MQTT 認證資訊
  for (int i = 0; i < 16; i++) {
    mqttUsername[i] = EEPROM.read(i + 98);   // 98-113: MQTT 帳號
    mqttPassword[i] = EEPROM.read(i + 114);  // 114-129: MQTT 密碼
  }
  // 讀取 MQTT Port (2 bytes)
  mqttPort = EEPROM.read(126) | (EEPROM.read(127) << 8);
  if (mqttPort == 0 || mqttPort == 0xFFFF) {
    mqttPort = 1883;  // 預設值
  }

  // 設置字串結尾
  ssid[31] = '\0';
  password[31] = '\0';
  mqttServer[31] = '\0';
  mqttUsername[15] = '\0';
  mqttPassword[15] = '\0';

  Serial.printf("已從 EEPROM 載入 WiFi 設定: %s\n", ssid);
}

void clearWiFiConfig() {
  // 如果已連接到 MQTT，發送重置狀態
  if (mqttClient.connected()) {
    const char* deviceId = getDeviceId();
    String statusTopic = String("hoban/") + deviceId + "/status";
    
    StaticJsonDocument<200> doc;
    doc["device_id"] = deviceId;
    doc["status"] = "reset";
    doc["server"] = mqttServer;
    doc["timestamp"] = millis() / 1000;
    
    char buffer[200];
    serializeJson(doc, buffer);
    
    mqttClient.publish(statusTopic.c_str(), buffer, true);
    Serial.println("已發送重置狀態到 MQTT");
    delay(1000); // 確保訊息有時間發送
  }

  EEPROM.begin(128);  // 增加 EEPROM 大小
  for (int i = 0; i < 128; i++) {  // 清除所有設定包括 MQTT
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  Serial.println("WiFi 設定已清除。重新啟動中...");
  delay(2000);
  ESP.restart();
}

void blinkLED() {
  unsigned long currentTime = millis();

  if (WiFi.status() != WL_CONNECTED && !isAPMode) {
    // WiFi 未連接模式：快速閃爍
    if (currentTime - lastBlinkTime >= QUICK_BLINK) {
      ledState = !ledState;
      digitalWrite(ledOnFace, ledState);
      digitalWrite(ledOnBoard, ledState);
      lastBlinkTime = currentTime;
    }
  } else if (isAPMode) {
    // AP 模式：短短長模式
    unsigned long patternTime = currentTime % (SHORT_BLINK * 2 + SHORT_BLINK * 2 + LONG_BLINK + PATTERN_PAUSE);

    if (patternTime < SHORT_BLINK) {
      // 第一個短閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else if (patternTime < SHORT_BLINK * 2) {
      // 第一個短閃暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    } else if (patternTime < SHORT_BLINK * 3) {
      // 第二個短閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else if (patternTime < SHORT_BLINK * 4) {
      // 第二個短閃暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    } else if (patternTime < SHORT_BLINK * 4 + LONG_BLINK) {
      // 長閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else {
      // 模式間暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    }
  } else if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    // WiFi 已連接但 MQTT 未連接：一長二短模式
    unsigned long patternTime = currentTime % (LONG_BLINK + SHORT_BLINK * 2 + SHORT_BLINK * 2 + SHORT_BLINK * 2 + PATTERN_PAUSE);

    if (patternTime < LONG_BLINK) {
      // 長閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK) {
      // 長閃後暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK * 2) {
      // 第一個短閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK * 3) {
      // 第一個短閃暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK * 4) {
      // 第二個短閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else {
      // 模式間暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    }
  } else {
    // WiFi 和 MQTT 都已連接：LED 關閉
    digitalWrite(ledOnFace, LOW);
    digitalWrite(ledOnBoard, LOW);
  }
}

// BLE 回調類別
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        Serial.println("onWrite");
        uint8_t* data = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();
        
        if (len > 0) {
            char* buffer = (char*)malloc(len + 1);
            memcpy(buffer, data, len);
            buffer[len] = '\0';
            
            Serial.print("收到的設定：");
            Serial.println(buffer);
            
            // 建立 JSON 文件
            StaticJsonDocument<512> doc;  // 增加容量以支援認證資訊
            DeserializationError error = deserializeJson(doc, buffer);

            if (!error) {
                // 檢查是否有 wifi 物件
                if (doc.containsKey("wifi")) {
                    const char* newSSID = doc["wifi"]["ssid"];
                    const char* newPassword = doc["wifi"]["password"];
                    const char* newMqttServer = doc["wifi"]["server"];
                    const char* newMqttUsername = doc["wifi"]["mqtt_username"];  // MQTT 帳號（選用）
                    const char* newMqttPassword = doc["wifi"]["mqtt_password"];  // MQTT 密碼（選用）
                    int newMqttPort = doc["wifi"]["mqtt_port"] | 1883;  // MQTT 埠（選用，預設 1883）

                    Serial.println("收到設定：");
                    Serial.printf("SSID: %s\n", newSSID);
                    Serial.printf("MQTT Server: %s\n", newMqttServer);
                    Serial.printf("MQTT Port: %d\n", newMqttPort);
                    if (newMqttUsername) Serial.printf("MQTT Username: %s\n", newMqttUsername);

                    if (newSSID && newPassword && newMqttServer) {
                        // 複製 WiFi 設定到全域變數
                        strncpy(ssid, newSSID, sizeof(ssid) - 1);
                        strncpy(password, newPassword, sizeof(password) - 1);
                        strncpy(mqttServer, newMqttServer, sizeof(mqttServer) - 1);
                        ssid[sizeof(ssid) - 1] = '\0';
                        password[sizeof(password) - 1] = '\0';
                        mqttServer[sizeof(mqttServer) - 1] = '\0';

                        // 複製 MQTT 認證資訊（如果提供）
                        if (newMqttUsername) {
                            strncpy(mqttUsername, newMqttUsername, sizeof(mqttUsername) - 1);
                            mqttUsername[sizeof(mqttUsername) - 1] = '\0';
                        } else {
                            mqttUsername[0] = '\0';  // 清空
                        }

                        if (newMqttPassword) {
                            strncpy(mqttPassword, newMqttPassword, sizeof(mqttPassword) - 1);
                            mqttPassword[sizeof(mqttPassword) - 1] = '\0';
                        } else {
                            mqttPassword[0] = '\0';  // 清空
                        }

                        mqttPort = newMqttPort;
                        useCustomServer = true;  // 標記使用自訂伺服器

                        saveWiFiConfig();

                        // 建立回應 JSON
                        StaticJsonDocument<350> response;
                        response["status"] = "success";
                        response["message"] = "WiFi 和 MQTT 設定已儲存";
                        response["data"]["device_id"] = getDeviceId();  // 加入設備 ID
                        response["data"]["ssid"] = ssid;
                        response["data"]["mqttServer"] = mqttServer;
                        response["data"]["mqttPort"] = mqttPort;
                        response["data"]["hasAuth"] = (strlen(mqttUsername) > 0);

                        // 序列化 JSON 到字串
                        char responseBuffer[350];
                        serializeJson(response, responseBuffer);

                        // 印出回應
                        Serial.println("回應：");
                        Serial.println(responseBuffer);

                        // 回傳 JSON 回應
                        pCharacteristic->setValue((uint8_t*)responseBuffer, strlen(responseBuffer));
                        pCharacteristic->notify();

                        free(buffer);
                        delay(2000);
                        ESP.restart();
                    } else {
                        // 錯誤回應
                        StaticJsonDocument<200> response;
                        response["status"] = "error";
                        response["message"] = "SSID、密碼或伺服器格式錯誤";

                        char responseBuffer[200];
                        serializeJson(response, responseBuffer);
                        pCharacteristic->setValue((uint8_t*)responseBuffer, strlen(responseBuffer));
                        pCharacteristic->notify();
                    }
                } else {
                    // 錯誤回應
                    StaticJsonDocument<200> response;
                    response["status"] = "error";
                    response["message"] = "無效的JSON格式";
                    
                    char responseBuffer[200];
                    serializeJson(response, responseBuffer);
                    pCharacteristic->setValue((uint8_t*)responseBuffer, strlen(responseBuffer));
                    pCharacteristic->notify();
                }
            }
            free(buffer);
        }
    }
};

const char* getDeviceId() {
  if (deviceIdString.length() == 0) {  // 如果還沒有產生過
    uint64_t chipId = ESP.getEfuseMac();
    uint8_t* chipIdBytes = (uint8_t*)&chipId;
    
    // 按照網路順序（從左到右）組合 MAC 位址
    char tempId[23];
    snprintf(tempId, 23, "hoban-%02x%02x%02x%02x%02x%02x", 
      chipIdBytes[5],  // 最高位元組
      chipIdBytes[4],
      chipIdBytes[3],
      chipIdBytes[2],
      chipIdBytes[1],
      chipIdBytes[0]   // 最低位元組
    );
    
    deviceIdString = String(tempId);
  }
  return deviceIdString.c_str();
}

void pulseRelay() {
  digitalWrite(relayButton, HIGH);
  digitalWrite(ledOnFace, HIGH);
  digitalWrite(ledOnBoard, HIGH);
  delay(1000);
  digitalWrite(relayButton, LOW);
  digitalWrite(ledOnFace, LOW);
  digitalWrite(ledOnBoard, LOW);
  
  // 使用 JSON 格式發布狀態
  publishStatus();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String deviceId = getDeviceId();
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("收到訊息 [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(message);

  if (String(topic) == String("hoban/" + deviceId + "/control")) {
    if (message == "status") {
      publishStatus();  // 使用 JSON 格式發布狀態
    } else if (message == "ON") {
      pulseRelay();
    } else if (message == "reset") {
      Serial.println("收到重置命令，執行重置...");
      clearWiFiConfig();  // 清除 WiFi 設定並重啟
    } else if (message == "FIND_BEST_SERVER") {
      // 重新測試所有伺服器並選擇最快的
      Serial.println("收到重新測試伺服器命令");
      mqttClient.disconnect();
      delay(1000);
      smartConnect();
    } else if (message.startsWith("update:")) {
      // 解析更新命令
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, message.substring(7));

      if (!error) {
        const char* newVersion = doc["version"];
        const char* downloadUrl = doc["url"];

        if (newVersion && downloadUrl) {
          Serial.println("收到韌體更新請求");
          Serial.print("新版本：");
          Serial.println(newVersion);
          Serial.print("下載網址：");
          Serial.println(downloadUrl);

          // 開始更新程序
          startFirmwareUpdate(downloadUrl);
        }
      }
    }
  }
}





void setupBLE() {
  const char* deviceId = getDeviceId();
  BLEDevice::init(deviceId);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );

  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.print("BLE 已啟動，名稱: ");
  Serial.println(deviceId);
}

void setup()
{
  Serial.begin(115200);
  delay(1000); // 等待序列埠穩定

  Serial.println("齁控－動物管制遠端控制系統 v" + String(firmwareVersion));
  Serial.println("================");

  pinMode(relayButton, OUTPUT);
  digitalWrite(relayButton, LOW);
  
  // 設定並關閉內建 LED
  pinMode(ledOnBoard, INPUT);  // 初始化第二個按鈕
  digitalWrite(ledOnBoard, LOW);  // 關閉 LED
  
  pinMode(ledOnFace, OUTPUT);
  digitalWrite(ledOnFace, LOW);  // 關閉 LED

  pinMode(bootButton, INPUT_PULLUP);  // 改用 INPUT_PULLUP
  pinMode(resetButton, INPUT_PULLUP);  // 改用 INPUT_PULLUP

  loadWiFiConfig();

  // 讀取使用自訂伺服器標誌
  EEPROM.begin(128);
  useCustomServer = (EEPROM.read(96) == 1);
  Serial.printf("使用自訂伺服器: %s\n", useCustomServer ? "是" : "否");

  const char* deviceId = getDeviceId();  // 獲取設備 ID

  if (strlen(ssid) > 0) {
    Serial.println("SSID: " + String(ssid));
    connectToWiFi();
  } else {
    Serial.println("找不到 WiFi 設定。啟動 BLE 配對模式。");
    isAPMode = true;
    setupBLE();  // 先啟動 BLE

    // 然後再啟動 AP 模式
    WiFi.mode(WIFI_AP);
    const char* deviceId = getDeviceId();
    WiFi.softAP(deviceId);
    Serial.print("AP 名稱: ");
    Serial.println(deviceId);
  }

  // 只有在成功連接到 WiFi 且不是 AP 模式時才使用智慧連接
  if (WiFi.status() == WL_CONNECTED && !isAPMode)
  {
    smartConnect();  // 使用智慧連接取代 connectToMQTT()
  }

}

void loop()
{
  // 在 AP 模式下處理 BLE
  if (isAPMode && deviceConnected) {
    delay(10);
  }

  // 檢查第一個按鈕狀態
  if (digitalRead(bootButton) == LOW || digitalRead(resetButton) == LOW) {  // 按鈕被按下
    if (buttonPressTime == 0) {  // 開始計時
      buttonPressTime = millis();
    } 
    
    unsigned long pressDuration = millis() - buttonPressTime;
    
    if (!isBlinking && pressDuration > LONG_PRESS_TIME) {
      // 第一階段：開始閃爍
      isBlinking = true;
      ledBlinkStart = millis();
      Serial.println("開始 LED 閃爍...");
    }
    
    if (isBlinking) {
      if (!waitingConfirm && (millis() - ledBlinkStart) < BLINK_TIME) {
        // LED 閃爍階段
        blinkLED();
      } else if (!waitingConfirm) {
        // 進入確認等待階段
        waitingConfirm = true;
        digitalWrite(ledOnFace, HIGH);  // LED 恆亮
        digitalWrite(ledOnBoard, HIGH);  // LED 恆亮
        Serial.println("請繼續按住按鈕以確認清除...");
      }
      
      if (waitingConfirm && (millis() - ledBlinkStart) > (BLINK_TIME + CONFIRM_TIME)) {
        // 完成所有階段，執行清除
        Serial.println("清除 WiFi 設定...");
        digitalWrite(ledOnFace, LOW);  // 關閉 LED
        digitalWrite(ledOnBoard, LOW);  // 關閉 LED
        clearWiFiConfig();  // 清除設定並重啟
      }
    }
  } else {
    // 按鈕放開，重置所有狀態
    buttonPressTime = 0;
    isBlinking = false;
    waitingConfirm = false;
    // 不要在這裡關閉 LED，讓 blinkLED() 處理
  }

  // 當不在按鈕長按流程時，根據連接狀態控制 LED 閃燈
  if (!isBlinking && !waitingConfirm) {
    blinkLED();
  }


  // MQTT 相關程式碼
  if (WiFi.status() == WL_CONNECTED && !isAPMode) {
    static unsigned long lastReconnectAttempt = 0;
    static unsigned long lastKeepAlive = 0;
    static int reconnectFailCount = 0;
    unsigned long now = millis();

    if (!mqttClient.connected()) {
      if (now - lastReconnectAttempt > 10000) {  // 每10秒重連一次
        lastReconnectAttempt = now;
        Serial.println("MQTT 連接中斷，重新連接...");

        reconnectFailCount++;

        // 失敗3次後，切換伺服器
        if (reconnectFailCount >= 3) {
          Serial.println("多次重連失敗，切換伺服器...");
          smartConnect();  // 重新智慧連接
          reconnectFailCount = 0;
        } else {
          // 嘗試連接當前伺服器
          if (useCustomServer && strlen(mqttServer) > 0) {
            quickConnectCustom();
          } else {
            quickConnect(DEFAULT_SERVERS[currentServerIndex]);
          }
        }
      }
    } else {
      mqttClient.loop();
      reconnectFailCount = 0;  // 重置失敗計數

      // 每 3 秒發送一次保持連線的狀態更新（帶伺服器資訊）
      if (now - lastKeepAlive > 3000) {
        const char* server = useCustomServer && strlen(mqttServer) > 0 ?
                             mqttServer : DEFAULT_SERVERS[currentServerIndex].server;
        publishStatusWithServer(server);
        lastKeepAlive = now;
      }
    }
  }
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("正在連接 WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi 連接成功！");
    Serial.print("IP 位址: ");
    Serial.println(WiFi.localIP());
    isAPMode = false;
    
    // WiFi 連接成功後發布狀態
    if (mqttClient.connected()) {
      publishStatus();
    }
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    const char* deviceId = getDeviceId();
    Serial.println("\n無法連接到 WiFi，啟動 AP 模式。");
    setupBLE();
    WiFi.softAP(deviceId);
    Serial.print("AP IP 位址: ");
    Serial.println(WiFi.softAPIP());
    isAPMode = true;
    blinkLED(); // 使用現有的 blinkLED 函數
  }
}

void publishStatus() {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";
  
  StaticJsonDocument<1024> doc;  // 將大小從 512 增加到 1024
  
  // 基本資訊
  doc["device_id"] = deviceId;
  doc["status"] = isUpdating ? "updating" : "online";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["timestamp"] = millis() / 1000;
  
  // WiFi 資訊
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["connected"] = (WiFi.status() == WL_CONNECTED);
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();
  
  // 設備狀態
  JsonObject device = doc.createNestedObject("device");
  device["relay"] = digitalRead(relayButton);
  // device["free_heap"] = ESP.getFreeHeap();
  
  if (isUpdating) {
    device["update_progress"] = updateProgress;
  }
  
  char buffer[1024];  // 將緩衝區大小也增加到 1024
  
  // 計算序列化後的大小
  size_t jsonSize = measureJson(doc);
  Serial.print("JSON 大小: ");
  Serial.print(jsonSize);
  Serial.println(" bytes");
  
  if (jsonSize > sizeof(buffer)) {
    Serial.println("警告：JSON 太大，無法放入緩衝區");
    return;
  }
  
  serializeJson(doc, buffer);
  
  bool publishSuccess = mqttClient.publish(statusTopic.c_str(), buffer, true);
  
  Serial.print("發布狀態: ");
  Serial.println(buffer);
  Serial.print("MQTT 伺服器: ");
  Serial.println(mqttServer);
  Serial.print("發布狀態: ");
  Serial.println(publishSuccess ? "成功" : "失敗");
  
  if (!publishSuccess) {
    Serial.println("發布失敗原因可能是：");
    Serial.println("1. 網路連接不穩定");
    Serial.println("2. MQTT 伺服器無回應");
    Serial.println("3. 訊息太大 (目前大小: " + String(jsonSize) + " bytes)");
    Serial.println("4. 連接已斷開");
  }
}

// 發布帶有伺服器資訊的狀態
void publishStatusWithServer(const char* server) {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  StaticJsonDocument<1024> doc;
  doc["device_id"] = deviceId;
  doc["status"] = isUpdating ? "updating" : "online";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["server"] = server;  // 加入伺服器資訊
  doc["timestamp"] = millis() / 1000;

  // WiFi 資訊
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["connected"] = (WiFi.status() == WL_CONNECTED);
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();

  // 設備狀態
  JsonObject device = doc.createNestedObject("device");
  device["relay"] = digitalRead(relayButton);

  if (isUpdating) {
    device["update_progress"] = updateProgress;
  }

  char buffer[1024];
  serializeJson(doc, buffer);
  mqttClient.publish(statusTopic.c_str(), buffer, true);

  Serial.printf("已發布狀態 (伺服器: %s)\n", server);
}

// 發布伺服器切換事件
void publishServerChangeEvent(const char* switchType, const char* server) {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  StaticJsonDocument<256> doc;
  doc["device_id"] = deviceId;
  doc["status"] = "online";
  doc["event"] = "server_changed";
  doc["switch_type"] = switchType;  // "auto" 或 "custom"
  doc["server"] = server;
  doc["timestamp"] = millis() / 1000;

  char buffer[256];
  serializeJson(doc, buffer);
  mqttClient.publish(statusTopic.c_str(), buffer, true);

  Serial.printf("已發布伺服器切換事件: %s (%s)\n", server, switchType);
}

// 快速連接：嘗試連接單個伺服器，1秒內成功就返回 true（支援認證）
bool quickConnect(const MqttServerConfig& config) {
  Serial.printf("快速測試: %s:%d ... ", config.server, config.port);

  mqttClient.setServer(config.server, config.port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);

  unsigned long startTime = millis();
  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  // 設定離線訊息
  StaticJsonDocument<128> offlineDoc;
  offlineDoc["device_id"] = deviceId;
  offlineDoc["status"] = "offline";
  offlineDoc["server"] = config.server;
  offlineDoc["timestamp"] = millis() / 1000;

  char offlineBuffer[128];
  serializeJson(offlineDoc, offlineBuffer);

  // 嘗試連接（1秒超時），使用設定的帳密（可能為 NULL）
  if (mqttClient.connect(deviceId,
                        config.username,   // 從 config 取得帳號（可為 NULL）
                        config.password,   // 從 config 取得密碼（可為 NULL）
                        statusTopic.c_str(), 1, true,
                        offlineBuffer, true)) {
    unsigned long connectTime = millis() - startTime;

    // 1秒內成功，直接接受
    if (connectTime < 1000) {
      Serial.printf("成功 (%lu ms) ✓\n", connectTime);

      // 訂閱控制主題
      String controlTopic = String("hoban/") + deviceId + "/control";
      mqttClient.subscribe(controlTopic.c_str());

      // 發布上線狀態（包含伺服器資訊）
      publishStatusWithServer(config.server);

      return true;
    } else {
      // 超過1秒，斷開並嘗試下一個
      Serial.printf("太慢 (%lu ms) ✗\n", connectTime);
      mqttClient.disconnect();
      return false;
    }
  }

  Serial.println("失敗 ✗");
  return false;
}

// 快速連接自訂伺服器（使用全域變數中的設定）
bool quickConnectCustom() {
  Serial.printf("快速測試自訂伺服器: %s:%d ... ", mqttServer, mqttPort);

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);

  unsigned long startTime = millis();
  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  // 設定離線訊息
  StaticJsonDocument<128> offlineDoc;
  offlineDoc["device_id"] = deviceId;
  offlineDoc["status"] = "offline";
  offlineDoc["server"] = mqttServer;
  offlineDoc["timestamp"] = millis() / 1000;

  char offlineBuffer[128];
  serializeJson(offlineDoc, offlineBuffer);

  // 使用自訂伺服器的帳密（如果為空字串則傳 NULL）
  const char* username = (strlen(mqttUsername) > 0) ? mqttUsername : NULL;
  const char* password = (strlen(mqttPassword) > 0) ? mqttPassword : NULL;

  // 嘗試連接（1秒超時）
  if (mqttClient.connect(deviceId,
                        username,
                        password,
                        statusTopic.c_str(), 1, true,
                        offlineBuffer, true)) {
    unsigned long connectTime = millis() - startTime;

    // 1秒內成功，直接接受
    if (connectTime < 1000) {
      Serial.printf("成功 (%lu ms) ✓\n", connectTime);

      // 訂閱控制主題
      String controlTopic = String("hoban/") + deviceId + "/control";
      mqttClient.subscribe(controlTopic.c_str());

      // 發布上線狀態（包含伺服器資訊）
      publishStatusWithServer(mqttServer);

      return true;
    } else {
      // 超過1秒，斷開並嘗試下一個
      Serial.printf("太慢 (%lu ms) ✗\n", connectTime);
      mqttClient.disconnect();
      return false;
    }
  }

  Serial.println("失敗 ✗");
  return false;
}

// 智慧連接：按優先順序嘗試
void smartConnect() {
  Serial.println("=== 開始智慧連接 ===");

  // 1. 如果有自訂伺服器，先試自訂
  if (useCustomServer && strlen(mqttServer) > 0) {
    Serial.println("優先嘗試自訂伺服器...");
    if (quickConnectCustom()) {
      Serial.println("✓ 已連接到自訂伺服器");
      publishServerChangeEvent("custom", mqttServer);
      failedAttempts = 0;
      return;
    }
    Serial.println("自訂伺服器失敗，切換到預設列表");
  }

  // 2. 嘗試上次成功的伺服器
  EEPROM.begin(128);
  int lastSuccessIndex = EEPROM.read(97);  // 讀取上次成功的索引

  if (lastSuccessIndex >= 0 && lastSuccessIndex < SERVER_COUNT) {
    Serial.printf("嘗試上次成功的伺服器 [%d]: %s\n",
                  lastSuccessIndex, DEFAULT_SERVERS[lastSuccessIndex].displayName);
    if (quickConnect(DEFAULT_SERVERS[lastSuccessIndex])) {
      currentServerIndex = lastSuccessIndex;
      Serial.printf("✓ 已連接到伺服器: %s\n", DEFAULT_SERVERS[lastSuccessIndex].displayName);
      failedAttempts = 0;
      return;
    }
  }

  // 3. 循環嘗試所有預設伺服器
  Serial.println("循環測試所有預設伺服器...");
  for (int attempt = 0; attempt < SERVER_COUNT * 2; attempt++) {
    int index = attempt % SERVER_COUNT;

    // 跳過剛才失敗的伺服器
    if (index == lastSuccessIndex && attempt < SERVER_COUNT) continue;

    Serial.printf("嘗試伺服器 [%d/%d]: %s\n",
                  attempt + 1, SERVER_COUNT * 2,
                  DEFAULT_SERVERS[index].displayName);

    if (quickConnect(DEFAULT_SERVERS[index])) {
      currentServerIndex = index;

      // 儲存成功的伺服器索引
      EEPROM.begin(128);
      EEPROM.write(97, index);
      EEPROM.commit();

      Serial.printf("✓ 已連接並儲存伺服器 [%d]: %s\n",
                   index, DEFAULT_SERVERS[index].displayName);

      // 發布伺服器切換事件
      publishServerChangeEvent("auto", DEFAULT_SERVERS[index].server);
      failedAttempts = 0;
      return;
    }

    delay(500);  // 短暫延遲避免過快請求
  }

  Serial.println("✗ 所有伺服器連接失敗");
  failedAttempts = 5;  // 設置為最大值，避免持續重試
}

// 保留原有的 connectToMQTT 函數作為向後兼容（現在內部使用 smartConnect）
void connectToMQTT() {
  smartConnect();
}

// 韌體下載和更新函數（透過 MQTT 觸發）
void startFirmwareUpdate(const char* downloadUrl) {
  if (isUpdating) {
    Serial.println("更新已在進行中，無法開始新的更新");
    return;
  }
  
  Serial.println("=== 開始韌體下載更新 ===");
  Serial.printf("下載網址：%s\n", downloadUrl);
  Serial.printf("可用空間：%u bytes\n", ESP.getFreeSketchSpace());
  Serial.printf("當前韌體版本：%s\n", firmwareVersion);
  
  isUpdating = true;
  updateProgress = 0;
  digitalWrite(ledOnFace, HIGH);
  
  // 發送更新開始狀態到 MQTT
  if (mqttClient.connected()) {
    String deviceId = getDeviceId();
    String statusTopic = "hoban/" + deviceId + "/status";
    mqttClient.publish(statusTopic.c_str(), "updating", true);
    Serial.println("已發送更新開始狀態到 MQTT");
  }
  
  // 使用 HTTPClient
  WiFiClientSecure client;
  HTTPClient http;
  
  // 設定 SSL/TLS
  client.setInsecure(); // 允許自簽名證書
  
  Serial.println("檢查網路狀態：");
  Serial.printf("WiFi SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("WiFi 訊號強度: %d dBm\n", WiFi.RSSI());
  Serial.printf("本地 IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("DNS 伺服器: %s\n", WiFi.dnsIP().toString().c_str());
  
  const int maxRetries = 3;
  const int baseDelay = 5000; // 基礎延遲 5 秒
  int retryCount = 0;
  bool downloadSuccess = false;
  String finalUrl = downloadUrl;
  
  while (retryCount < maxRetries && !downloadSuccess) {
    if (retryCount > 0) {
      int delayTime = baseDelay * (1 << retryCount); // 指數退避
      Serial.printf("重試第 %d 次，等待 %d 毫秒...\n", retryCount, delayTime);
      delay(delayTime);
    }
    
    Serial.println("正在連接到下載伺服器...");
    
    // 設定超時
    http.setTimeout(30000); // 30 秒超時
    
    if (http.begin(client, finalUrl)) {
      // 添加請求標頭
      http.addHeader("User-Agent", "ESP32-FirmwareUpdate/1.0");
      http.addHeader("Accept", "*/*");
      
      Serial.println("發送 GET 請求下載檔案...");
      int httpCode = http.GET();
      
      if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        Serial.printf("檔案大小: %d bytes\n", contentLength);
        
        // 檢查空間是否足夠
        if (contentLength > ESP.getFreeSketchSpace()) {
          Serial.println("錯誤：空間不足");
          break;
        }
        
        if (!Update.begin(contentLength)) {
          Serial.printf("錯誤：無法開始更新，錯誤碼：%d\n", Update.getError());
          break;
        }
        
        WiFiClient* stream = http.getStreamPtr();
        size_t written = 0;
        uint8_t buff[1024] = { 0 };
        
        // 下載超時設定
        const unsigned long downloadTimeout = 300000; // 5 分鐘
        unsigned long startTime = millis();
        unsigned long lastProgressTime = startTime;
        
        while (http.connected() && (written < contentLength)) {
          size_t available = stream->available();
          if (available) {
            size_t bytesRead = stream->readBytes(buff, min(available, sizeof(buff)));
            size_t bytesWritten = Update.write(buff, bytesRead);
            if (bytesWritten > 0) {
              written += bytesWritten;
              updateProgress = (written * 100) / contentLength;
              
              if (millis() - lastProgressTime >= 1000) {
                Serial.printf("下載進度：%d%%（%u/%d bytes）\n", updateProgress, written, contentLength);
                lastProgressTime = millis();
              }
            }
          }
          
          // 檢查超時
          if (millis() - startTime > downloadTimeout) {
            Serial.println("錯誤：下載超時");
            break;
          }
          
          delay(1); // 避免看門狗重置
        }
        
        if (written == contentLength && Update.end(true)) {
          Serial.println("更新成功！準備重新啟動...");
          downloadSuccess = true;
          
          if (mqttClient.connected()) {
            String deviceId = getDeviceId();
            String statusTopic = "hoban/" + deviceId + "/status";
            mqttClient.publish(statusTopic.c_str(), "update_success", true);
          }
          
          delay(1000);
          ESP.restart();
          return;
        }
      } else if (httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        // 處理重定向
        String newUrl = http.getLocation();
        Serial.printf("收到重定向到新網址：%s\n", newUrl.c_str());
        finalUrl = newUrl;
        http.end();
        continue; // 使用新的 URL 重試
      } else {
        Serial.printf("GET 請求失敗，錯誤碼：%d\n", httpCode);
        Serial.printf("錯誤訊息：%s\n", http.errorToString(httpCode).c_str());
        
        if (httpCode == HTTPC_ERROR_CONNECTION_REFUSED) {
          Serial.println("伺服器拒絕連接");
        } else if (httpCode == HTTPC_ERROR_SEND_HEADER_FAILED) {
          Serial.println("發送請求標頭失敗");
        } else if (httpCode == HTTPC_ERROR_SEND_PAYLOAD_FAILED) {
          Serial.println("發送請求內容失敗");
        } else if (httpCode == HTTPC_ERROR_NOT_CONNECTED) {
          Serial.println("未連接到伺服器");
        } else if (httpCode == HTTPC_ERROR_CONNECTION_LOST) {
          Serial.println("連接丟失，可能原因：");
          Serial.println("1. GitHub 伺服器連接不穩定");
          Serial.println("2. SSL/TLS 握手失敗");
          Serial.println("3. 網路延遲過高");
          Serial.println("4. DNS 解析失敗");
        } else if (httpCode == HTTPC_ERROR_NO_HTTP_SERVER) {
          Serial.println("找不到 HTTP 伺服器");
        }
      }
    } else {
      Serial.println("無法初始化 HTTP 客戶端");
    }
    
    http.end();
    retryCount++;
  }
  
  // 更新失敗處理
  isUpdating = false;
  digitalWrite(ledOnFace, LOW);
  
  if (mqttClient.connected()) {
    String deviceId = getDeviceId();
    String statusTopic = "hoban/" + deviceId + "/status";
    String errorMsg = "update_failed";
    mqttClient.publish(statusTopic.c_str(), errorMsg.c_str(), true);
    Serial.println("已發送更新失敗狀態到 MQTT");
  }
}

