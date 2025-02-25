#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
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

// 檢查是否支援藍芽
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` enable it
#endif

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


const char* firmwareVersion = "1.1.1"; // 當前韌體版本
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

// WiFi 設定
char ssid[32] = "";
char password[32] = "";
char mqttServer[32] = "broker.MQTTGO.io"; // 預設 MQTT 伺服器

// MQTT 相關
const int mqttPort = 1883;

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


// WebServer 實例
WebServer server(80);

// 函數前向宣告
void saveWiFiConfig();
void loadWiFiConfig();
void clearWiFiConfig();

// WiFi 設定相關函數實作
void saveWiFiConfig() {
  EEPROM.begin(128);  // 增加 EEPROM 大小以儲存 MQTT 設定
  for (int i = 0; i < 32; i++) {
    EEPROM.write(i, ssid[i]);
    EEPROM.write(i + 32, password[i]);
    EEPROM.write(i + 64, mqttServer[i]);  // 儲存 MQTT 伺服器設定
  }
  EEPROM.commit();
}

void loadWiFiConfig() {
  EEPROM.begin(128);  // 增加 EEPROM 大小以讀取 MQTT 設定
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(i);
    password[i] = EEPROM.read(i + 32);
    mqttServer[i] = EEPROM.read(i + 64);  // 讀取 MQTT 伺服器設定
  }
  ssid[31] = '\0';
  password[31] = '\0';
  mqttServer[31] = '\0';
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
    // 斷線模式：快速閃爍
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
            StaticJsonDocument<200> doc;
            DeserializationError error = deserializeJson(doc, buffer);
            
            if (!error) {
                // 檢查是否有 wifi 物件
                if (doc.containsKey("wifi")) {
                    const char* newSSID = doc["wifi"]["ssid"];
                    const char* newPassword = doc["wifi"]["password"];
                    const char* newMqttServer = doc["wifi"]["server"];  // 修改這裡
                    
                    Serial.println(newMqttServer);
                    Serial.println(newSSID);
                    Serial.println(newPassword);
                    
                    if (newSSID && newPassword && newMqttServer) {
                        // 複製到全域變數
                        strncpy(ssid, newSSID, sizeof(ssid) - 1);
                        strncpy(password, newPassword, sizeof(password) - 1);
                        strncpy(mqttServer, newMqttServer, sizeof(mqttServer) - 1);
                        ssid[sizeof(ssid) - 1] = '\0';
                        password[sizeof(password) - 1] = '\0';
                        mqttServer[sizeof(mqttServer) - 1] = '\0';
                        
                        saveWiFiConfig();
                        
                        // 建立回應 JSON
                        StaticJsonDocument<200> response;
                        response["status"] = "success";
                        response["message"] = "WiFi設定已儲存";
                        response["data"]["ssid"] = ssid;
                        response["data"]["mqttServer"] = mqttServer;
                        
                        // 序列化 JSON 到字串
                        char responseBuffer[200];
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
                        response["message"] = "SSID或密碼格式錯誤";
                        
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

void handleRoot()
{
  String html = "<html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css' rel='stylesheet'>";
  html += "<style>";
  html += "body{background:#f8f9fa}.container{max-width:600px;padding:20px}.card{margin-bottom:20px}";
  html += ".btn-open{background:#34a853;color:#fff}";
  html += "</style></head><body>";

  html += "<div class='container py-4'>";
  html += "<h1 class='text-center mb-4 fw-bold'>齁控－動物管制遠端控制系統 v" + String(firmwareVersion) + "</h1>";

  // 設備資訊卡片
  html += "<div class='card mb-3'>";
  html += "<div class='card-header bg-primary text-white'>設備資訊</div>";
  html += "<div class='card-body'>";
  String deviceId = getDeviceId();
  html += "<p class='card-text'><strong>設備 ID：</strong>" + deviceId + "</p>";
  html += "</div></div>";

  // WiFi 資訊卡片
  html += "<div class='card mb-3'>";
  html += "<div class='card-header bg-success text-white'>網路狀態</div>";
  html += "<div class='card-body'>";
  if (strlen(ssid) > 0)
  {
    html += "<p class='card-text'><strong>目前連線：</strong>" + String(ssid) + "</p>";
    html += "<p class='card-text'><strong>IP 位址：</strong>" + WiFi.localIP().toString() + "</p>";
  }
  else
  {
    html += "<p class='card-text text-warning'><strong>尚未設定 WiFi</strong></p>";
  }
  html += "</div></div>";

  // MQTT 狀態卡片
  html += "<div class='card mb-3'>";
  html += "<div class='card-header bg-info text-white'>MQTT 狀態</div>";
  html += "<div class='card-body'>";
  html += "<p class='card-text'><strong>伺服器：</strong>" + String(mqttServer) + "</p>";
  html += "<p class='card-text'><strong>連接狀態：</strong>";
  
  if (WiFi.status() == WL_CONNECTED && !isAPMode) {
    if (mqttClient.connected()) {
      html += "<span class='badge bg-success'>已連接</span></p>";
    } else {
      html += "<span class='badge bg-warning'>未連接</span></p>";
    }
  } else {
    html += "<span class='badge bg-secondary'>WiFi 未連接</span></p>";
  }

  // 添加 MQTT 設定表單
  html += "<form action='/setmqtt' method='POST' class='mt-3'>";
  html += "<div class='mb-3'>";
  html += "<label class='form-label'>MQTT 伺服器：</label>";
  html += "<input type='text' class='form-control' name='mqtt_server' value='" + String(mqttServer) + "' placeholder='輸入 MQTT 伺服器位址'>";
  html += "</div>";
  html += "<button type='submit' class='btn btn-primary'>更新 MQTT 設定</button>";
  html += "</form>";
  
  html += "</div></div>";

  // WiFi 設定表單
  html += "<div class='card mb-3'>";
  html += "<div class='card-header bg-info text-white'>WiFi 設定</div>";
  html += "<div class='card-body'>";
  html += "<form action='/setwifi' method='POST'>";
  html += "<div class='mb-3'>";
  html += "<label class='form-label'>SSID：</label>";
  html += "<input type='text' class='form-control' name='ssid' placeholder='請輸入 WiFi 名稱'>";
  html += "</div>";
  html += "<div class='mb-3'>";
  html += "<label class='form-label'>密碼：</label>";
  html += "<input type='password' class='form-control' name='password' placeholder='請輸入 WiFi 密碼'>";
  html += "</div>";
  html += "<button type='submit' class='btn btn-primary'>設定 WiFi</button>";
  html += "</form>";

  // 加入清除 WiFi 按鈕
  html += "<hr class='my-3'>";
  html += "<button onclick=\"if(confirm('確定要清除 WiFi 設定嗎？\\n清除後裝置將重新啟動。')) { location.href='/clearwifi' }\" ";
  html += "class='btn btn-danger'>清除 WiFi 設定</button>";
  html += "</div></div>";

  // 控制按鈕
  html += "<button onclick=\"location.href='/relay/on'\" class='btn btn-open btn-lg w-100 text-white'>開門</button>";
  html += "<div class='text-center mt-5 small'>齁斑科技</div>";
  html += "</div>";
  html += "<script src='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js'></script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleSetWiFi()
{
  if (server.hasArg("ssid") && server.hasArg("password"))
  {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");

    newSSID.toCharArray(ssid, 32);
    newPassword.toCharArray(password, 32);

    saveWiFiConfig();

    String html = "<html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css' rel='stylesheet'>";
    html += "</head><body class='bg-light'>";
    html += "<div class='container py-5'>";
    html += "<div class='card mx-auto' style='max-width: 400px;'>";
    html += "<div class='card-body text-center'>";
    html += "<h2 class='card-title text-success mb-3'>WiFi 設定已儲存</h2>";
    html += "<p class='card-text'>系統將在 3 秒後重新啟動...</p>";
    html += "<a href='/' class='btn btn-primary'>返回首頁</a>";
    html += "</div></div></div>";
    html += "<script>setTimeout(function(){ window.location.href = '/'; }, 3000);</script>";
    html += "<script src='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js'></script>";
    html += "</body></html>";

    server.send(200, "text/html", html);
    delay(2000);
    ESP.restart();
  }
  else
  {
    String html = "<html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css' rel='stylesheet'>";
    html += "</head><body class='bg-light'>";
    html += "<div class='container py-5'>";
    html += "<div class='card mx-auto' style='max-width: 400px;'>";
    html += "<div class='card-body text-center'>";
    html += "<h2 class='card-title text-danger mb-3'>錯誤</h2>";
    html += "<p class='card-text'>缺少 SSID 或密碼</p>";
    html += "<a href='/' class='btn btn-primary'>返回首頁</a>";
    html += "</div></div></div>";
    html += "<script src='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js'></script>";
    html += "</body></html>";
    server.send(400, "text/html", html);
  }
}

void handleRelayOn()
{
  relayState = true;
  pulseRelay(); // 使用統一的控制函數

  String html = "<html><body>";
  html += "<h1>已經關門</h1>";
  html += "<p>你將會在3秒後回到首頁。</p>";
  html += "<script>window.location.href = '/';</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleClearWiFi() {
  String html = "<html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css' rel='stylesheet'>";
  html += "</head><body class='bg-light'>";
  html += "<div class='container py-5'>";
  html += "<div class='card mx-auto' style='max-width:400px'>";
  html += "<div class='card-body text-center'>";
  html += "<h2 class='text-danger mb-3'>WiFi 設定已清除</h2>";
  html += "<p>系統將在 3 秒後重新啟動...</p>";
  html += "<div class='spinner-border text-primary' role='status'></div>";
  html += "</div></div></div>";
  html += "<script>setTimeout(function(){location.href='/'},3000)</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
  
  delay(1000);
  clearWiFiConfig();
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

  // 只有在成功連接到 WiFi 且不是 AP 模式時才連接 MQTT
  if (WiFi.status() == WL_CONNECTED && !isAPMode)
  {
    connectToMQTT();
  }

  server.on("/", handleRoot);
  server.on("/setwifi", HTTP_POST, handleSetWiFi);
  server.on("/relay/on", handleRelayOn);
  server.on("/clearwifi", handleClearWiFi);
  server.on("/setmqtt", HTTP_POST, handleSetMQTT);  // 添加新的路由
  
  // 添加韌體更新路由
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "更新失敗" : "更新成功");
  }, handleFirmwareUpload);

  server.begin();
  Serial.println("HTTP 伺服器已啟動。");
}

void loop()
{
  server.handleClient();

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
  } else if (isAPMode) {
    isBlinking = true;
    isBlinking = false;
    waitingConfirm = false;
    blinkLED();
  } else {
    // 按鈕放開，重置所有狀態
    buttonPressTime = 0;
    isBlinking = false;
    waitingConfirm = false;
    digitalWrite(ledOnFace, LOW);  // 確保 LED 關閉
    digitalWrite(ledOnBoard, LOW);  // 確保 LED 關閉
  }


  // MQTT 相關程式碼
  if (WiFi.status() == WL_CONNECTED && !isAPMode) {
    static unsigned long lastReconnectAttempt = 0;
    static unsigned long lastKeepAlive = 0;
    unsigned long now = millis();

    if (!mqttClient.connected() && failedAttempts < 5) {
      if (now - lastReconnectAttempt > 5000) {  // 縮短重連間隔到 5 秒
        lastReconnectAttempt = now;
        Serial.println("MQTT 連接中斷，嘗試重新連接...");
        connectToMQTT();
      }
    } else if (mqttClient.connected()) {
      mqttClient.loop();
      
      // 每 15 秒發送一次保持連線的狀態更新
      if (now - lastKeepAlive > 15000) {
        publishStatus();
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

void connectToMQTT() {
  if (failedAttempts >= 5) {
    Serial.println("已達重試上限，暫時停止 MQTT 連接嘗試");
    return;
  }

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  // 設定離線狀態的 JSON
  StaticJsonDocument<128> offlineDoc;
  offlineDoc["device_id"] = deviceId;
  offlineDoc["status"] = "offline";  // 只有在設備離線時才會發送
  offlineDoc["server"] = mqttServer;
  offlineDoc["timestamp"] = millis() / 1000;
  
  char offlineBuffer[128];
  serializeJson(offlineDoc, offlineBuffer);

  if (mqttClient.connect(deviceId,
                        NULL,  // username
                        NULL,  // password
                        statusTopic.c_str(),  // LWT topic
                        1,     // QoS 改為 1
                        true,  // retain
                        offlineBuffer,  // LWT message
                        true   // clean session 改為 true
                        )) {
    Serial.println("已連接到 MQTT");

    // 立即發布上線狀態
    publishStatus();  // 這會發送 "online" 狀態

    String controlTopic = String("hoban/") + deviceId + "/control";
    mqttClient.subscribe(controlTopic.c_str());

    Serial.print("已訂閱主題: ");
    Serial.println(controlTopic);
    failedAttempts = 0;
  } else {
    Serial.print("連接失敗，錯誤碼=");
    Serial.print(mqttClient.state());
    Serial.print(" 重試次數: ");
    Serial.println(failedAttempts);
    failedAttempts++;
  }
}

// 添加韌體更新處理函數
void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    Serial.println("=== 開始韌體更新 ===");
    Serial.printf("檔案名稱: %s\n", upload.filename.c_str());
    Serial.printf("檔案大小: %u bytes\n", upload.totalSize);
    Serial.printf("可用空間: %u bytes\n", ESP.getFreeSketchSpace());
    
    isUpdating = true;
    updateProgress = 0;
    digitalWrite(ledOnFace, HIGH);  // 開始更新時點亮 LED
    
    // 發送更新開始狀態到 MQTT
    if (mqttClient.connected()) {
      String deviceId = getDeviceId();
      String statusTopic = "hoban/" + deviceId + "/status";
      mqttClient.publish(statusTopic.c_str(), "updating", true);
      Serial.println("已發送更新狀態到 MQTT");
    }
    
    if (!Update.begin(upload.totalSize)) {
      Serial.println("更新初始化失敗！");
      Serial.printf("錯誤: %s\n", Update.errorString());
      Update.printError(Serial);
    } else {
      Serial.println("更新初始化成功");
    }
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) {
    Serial.printf("寫入韌體: %u bytes\n", upload.currentSize);
    
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.println("寫入韌體失敗！");
      Serial.printf("錯誤: %s\n", Update.errorString());
      Update.printError(Serial);
    }
    
    // 計算更新進度
    updateProgress = (Update.progress() * 100) / Update.size();
    Serial.printf("更新進度: %d%%\n", updateProgress);
    
    // LED 閃爍以指示更新進行中
    if (millis() % 200 < 100) {  // 快速閃爍
      digitalWrite(ledOnFace, HIGH);
    } else {
      digitalWrite(ledOnFace, LOW);
    }
    
    // 每 10% 發送一次進度
    if (updateProgress % 10 == 0) {
      if (mqttClient.connected()) {
        String deviceId = getDeviceId();
        String statusTopic = "hoban/" + deviceId + "/status";
        String progressMsg = "updating:" + String(updateProgress);
        mqttClient.publish(statusTopic.c_str(), progressMsg.c_str(), true);
        Serial.printf("已發送進度到 MQTT: %d%%\n", updateProgress);
      }
    }
  } 
  else if (upload.status == UPLOAD_FILE_END) {
    Serial.println("韌體上傳完成");
    Serial.printf("總共寫入: %u bytes\n", upload.totalSize);
    
    if (Update.end(true)) {
      Serial.println("更新成功！準備重新啟動...");
      digitalWrite(ledOnFace, HIGH);  // 更新成功後 LED 恆亮
      
      // 發送更新完成狀態到 MQTT
      if (mqttClient.connected()) {
        String deviceId = getDeviceId();
        String statusTopic = "hoban/" + deviceId + "/status";
        mqttClient.publish(statusTopic.c_str(), "update_success", true);
        Serial.println("已發送更新成功狀態到 MQTT");
      }
      
      delay(1000);
      ESP.restart();
    } else {
      Serial.println("更新失敗！");
      Serial.printf("錯誤: %s\n", Update.errorString());
      Update.printError(Serial);
      digitalWrite(ledOnFace, LOW);  // 更新失敗後關閉 LED
      
      // 發送更新失敗狀態到 MQTT
      if (mqttClient.connected()) {
        String deviceId = getDeviceId();
        String statusTopic = "hoban/" + deviceId + "/status";
        mqttClient.publish(statusTopic.c_str(), "update_failed", true);
        Serial.println("已發送更新失敗狀態到 MQTT");
      }
    }
    isUpdating = false;
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("更新被中止！");
    Update.end();
    digitalWrite(ledOnFace, LOW);
    isUpdating = false;
    
    // 發送更新失敗狀態到 MQTT
    if (mqttClient.connected()) {
      String deviceId = getDeviceId();
      String statusTopic = "hoban/" + deviceId + "/status";
      mqttClient.publish(statusTopic.c_str(), "update_aborted", true);
      Serial.println("已發送更新中止狀態到 MQTT");
    }
  }
}

// 添加韌體下載和更新函數
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

void handleSetMQTT() {
  if (server.hasArg("mqtt_server")) {
    String newServer = server.arg("mqtt_server");
    newServer.trim();
    
    if (newServer.length() > 0) {
      strncpy(mqttServer, newServer.c_str(), sizeof(mqttServer) - 1);
      mqttServer[sizeof(mqttServer) - 1] = '\0';
      
      saveWiFiConfig();  // 儲存新的設定
      
      if (mqttClient.connected()) {
        mqttClient.disconnect();  // 斷開現有連接
      }
      
      String html = "<html><head>";
      html += "<meta charset='UTF-8'>";
      html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
      html += "<link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css' rel='stylesheet'>";
      html += "</head><body class='bg-light'>";
      html += "<div class='container py-5'>";
      html += "<div class='card mx-auto' style='max-width: 400px;'>";
      html += "<div class='card-body text-center'>";
      html += "<h2 class='card-title text-success mb-3'>MQTT 設定已更新</h2>";
      html += "<p class='card-text'>新的伺服器：" + String(mqttServer) + "</p>";
      html += "<p class='card-text'>系統將在 3 秒後重新連接...</p>";
      html += "<a href='/' class='btn btn-primary'>返回首頁</a>";
      html += "</div></div></div>";
      html += "<script>setTimeout(function(){ window.location.href = '/'; }, 3000);</script>";
      html += "</body></html>";
      
      server.send(200, "text/html", html);
      
      // 延遲後重新連接 MQTT
      delay(1000);
      connectToMQTT();
    } else {
      server.send(400, "text/plain", "MQTT 伺服器位址不能為空");
    }
  } else {
    server.send(400, "text/plain", "缺少必要參數");
  }
}