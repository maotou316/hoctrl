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

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic = NULL;
bool deviceConnected = false;

// BLE 連接回調
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    }

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

const char* firmwareVersion = "1.0.5"; // 當前韌體版本
const char* deviceModel = "hoRelay1"; // 設備型號

// 在檔案最前面的全域變數區域
const int buttonPin = 0;    // BOOT 按鈕在 GPIO 0
const int button2Pin = 14;  // 第二個按鈕在 GPIO 14
const int relayPin = 13;
const int ledPin = 2;      // 內建 LED
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

char ssid[32] = "";
char password[32] = "";

// 添加韌體更新相關變數
bool isUpdating = false;
int updateProgress = 0;


// 函數前向宣告
void saveWiFiConfig();
void loadWiFiConfig();
void clearWiFiConfig();

// WiFi 設定相關函數實作
void saveWiFiConfig() {
  EEPROM.begin(64);
  for (int i = 0; i < 32; i++) {
    EEPROM.write(i, ssid[i]);
    EEPROM.write(i + 32, password[i]);
  }
  EEPROM.commit();
}

void loadWiFiConfig() {
  EEPROM.begin(64);
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(i);
    password[i] = EEPROM.read(i + 32);
  }
  ssid[31] = '\0';
  password[31] = '\0';
}

void clearWiFiConfig() {
  EEPROM.begin(64);
  for (int i = 0; i < 64; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  Serial.println("WiFi 設定已清除。重新啟動中...");
  delay(2000);
  ESP.restart();
}

void blinkLED() {
  static unsigned long lastBlink = 0;
  const int blinkInterval = 100;  // 快速閃爍間隔 (100ms)
  
  if (millis() - lastBlink >= blinkInterval) {
    digitalWrite(ledPin, !digitalRead(ledPin));  // 切換 LED 狀態
    lastBlink = millis();
  }
}

// BLE 回調類別
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        uint8_t* data = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();
        
        if (len > 0) {
            char* buffer = (char*)malloc(len + 1);
            memcpy(buffer, data, len);
            buffer[len] = '\0';
            
            // 建立 JSON 文件
            StaticJsonDocument<200> doc;
            DeserializationError error = deserializeJson(doc, buffer);
            
            if (!error) {
                // 檢查是否有 wifi 物件
                if (doc.containsKey("wifi")) {
                    const char* newSSID = doc["wifi"]["ssid"];
                    const char* newPassword = doc["wifi"]["password"];
                    
                    if (newSSID && newPassword) {
                        // 複製到全域變數
                        strncpy(ssid, newSSID, sizeof(ssid) - 1);
                        strncpy(password, newPassword, sizeof(password) - 1);
                        ssid[sizeof(ssid) - 1] = '\0';
                        password[sizeof(password) - 1] = '\0';
                        
                        saveWiFiConfig();
                        
                        // 建立回應 JSON
                        StaticJsonDocument<200> response;
                        response["status"] = "success";
                        response["message"] = "WiFi設定已儲存";
                        response["data"]["ssid"] = ssid;
                        
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

WebServer server(80);

WiFiClient espClient; // MQTT 客戶端
PubSubClient mqttClient(espClient);

char mqttServer[32] = "broker.MQTTGO.io"; // 預設 MQTT 伺服器
const int mqttPort = 1883;

bool relayState = false;
bool isAPMode = false;

String mqttStatus = "未連接";  // MQTT 狀態追踪

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
  digitalWrite(relayPin, HIGH);
  delay(1000);
  digitalWrite(relayPin, LOW);
  
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
  
  String statusTopic = "hoban/" + deviceId + "/status";
  String controlTopic = "hoban/" + deviceId + "/control";
  
  html += "<p class='card-text'><strong>狀態主題：</strong><br><code>" + statusTopic + "</code></p>";
  html += "<p class='card-text'><strong>控制主題：</strong><br><code>" + controlTopic + "</code></p>";
  html += "<p class='card-text'><strong>連線狀態：</strong>";
  
  if (WiFi.status() == WL_CONNECTED && !isAPMode) {
    if (mqttClient.connected()) {
      html += "<span class='badge bg-success'>已連接</span></p>";
      html += "<p class='card-text'><small class='text-muted'>最後更新時間：" + String(millis()/1000) + " 秒</small></p>";
      html += "<p class='card-text'><strong>目前狀態：</strong><span class='badge bg-success'>online</span></p>";
    } else {
      html += "<span class='badge bg-warning'>" + mqttStatus + "</span></p>";
    }
  } else {
    html += "<span class='badge bg-secondary'>WiFi 未連接</span></p>";
  }
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

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  
  // 設定並關閉內建 LED
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);  // 關閉 LED

  loadWiFiConfig();

  // 設定按鈕腳位
  pinMode(buttonPin, INPUT);
  pinMode(button2Pin, INPUT);  // 初始化第二個按鈕

  const char* deviceId = getDeviceId();  // 獲取設備 ID
  
  if (strlen(ssid) > 0) {
    connectToWiFi();
  } else {
    WiFi.mode(WIFI_AP);
    Serial.println("找不到 WiFi 設定。啟動 AP 模式和 BLE 配對模式。");
    WiFi.softAP(deviceId);
    Serial.print("AP 名稱: ");
    Serial.println(deviceId);
    
    isAPMode = true;
    setupBLE();
  }

  // 只有在成功連接到 WiFi 且不是 AP 模式時才連接 MQTT
  if (WiFi.status() == WL_CONNECTED && !isAPMode)
  {
    connectToMQTT();
  }

  server.on("/", handleRoot);
  server.on("/setwifi", HTTP_POST, handleSetWiFi);
  server.on("/relay/on", handleRelayOn);
  server.on("/clearwifi", handleClearWiFi);  // 新增路由
  
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
  if (digitalRead(buttonPin) == LOW) {  // 按鈕被按下
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
        digitalWrite(ledPin, HIGH);  // LED 恆亮
        Serial.println("請繼續按住按鈕以確認清除...");
      }
      
      if (waitingConfirm && (millis() - ledBlinkStart) > (BLINK_TIME + CONFIRM_TIME)) {
        // 完成所有階段，執行清除
        Serial.println("清除 WiFi 設定...");
        digitalWrite(ledPin, LOW);  // 關閉 LED
        clearWiFiConfig();  // 清除設定並重啟
      }
    }
  } else {
    // 按鈕放開，重置所有狀態
    buttonPressTime = 0;
    isBlinking = false;
    waitingConfirm = false;
    digitalWrite(ledPin, LOW);  // 確保 LED 關閉
  }

  // 檢查第二個按鈕狀態
  if (digitalRead(button2Pin) == LOW) {  // 第二個按鈕被按下
    if (button2PressTime == 0) {  // 開始計時
      button2PressTime = millis();
    }
    
    // 如果按下超過 100ms（防彈跳）
    if (millis() - button2PressTime > 100) {
      digitalWrite(ledPin, HIGH);  // LED 亮起
      Serial.println("第二個按鈕觸發繼電器和 LED");
      button2PressTime = 0;  // 重置計時器
      delay(500);  // 防止重複觸發
    }
  } else {
    button2PressTime = 0;  // 按鈕放開，重置計時器
    digitalWrite(ledPin, LOW);  // LED 熄滅
  }

  // MQTT 相關程式碼
  if (WiFi.status() == WL_CONNECTED && !isAPMode) {
    static unsigned long lastReconnectAttempt = 0;
    static unsigned long lastKeepAlive = 0;
    unsigned long now = millis();

    if (!mqttClient.connected() && failedAttempts < 5) {
      mqttStatus = "等待重連";
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
  } else {
    mqttStatus = "WiFi 未連接";
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
    WiFi.softAP(deviceId);
    Serial.print("AP IP 位址: ");
    Serial.println(WiFi.softAPIP());
    isAPMode = true;
  }
}

void publishStatus() {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";
  
  StaticJsonDocument<512> doc;
  
  // 基本資訊
  doc["device_id"] = deviceId;
  doc["status"] = isUpdating ? "updating" : "online";
  doc["version"] = firmwareVersion;  // 加入韌體版本
  doc["model"] = deviceModel;        // 加入設備型號
  doc["timestamp"] = millis() / 1000;
  
  // WiFi 資訊
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["connected"] = (WiFi.status() == WL_CONNECTED);
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();
  
  // 設備狀態
  JsonObject device = doc.createNestedObject("device");
  device["relay"] = digitalRead(relayPin);
  device["free_heap"] = ESP.getFreeHeap();
  
  if (isUpdating) {
    device["update_progress"] = updateProgress;
  }
  
  char buffer[512];
  serializeJson(doc, buffer);
  
  mqttClient.publish(statusTopic.c_str(), buffer, true);
  
  Serial.print("發布狀態: ");
  Serial.println(buffer);
}

void connectToMQTT() {
  if (failedAttempts >= 5) {
    mqttStatus = "已達重試上限";
    Serial.println("已達到最大重試次數，暫時停止 MQTT 連接嘗試");
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
    mqttStatus = "已連接";
    Serial.println("已連接到 MQTT");

    // 立即發布上線狀態
    publishStatus();  // 這會發送 "online" 狀態

    String controlTopic = String("hoban/") + deviceId + "/control";
    mqttClient.subscribe(controlTopic.c_str());

    Serial.print("已訂閱主題: ");
    Serial.println(controlTopic);
    failedAttempts = 0;
  } else {
    mqttStatus = "連接失敗 (錯誤碼: " + String(mqttClient.state()) + ")";
    failedAttempts++;
    Serial.print("連接失敗，錯誤碼=");
    Serial.print(mqttClient.state());
    Serial.print(" 重試次數: ");
    Serial.println(failedAttempts);
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
    digitalWrite(ledPin, HIGH);  // 開始更新時點亮 LED
    
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
      digitalWrite(ledPin, HIGH);
    } else {
      digitalWrite(ledPin, LOW);
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
      digitalWrite(ledPin, HIGH);  // 更新成功後 LED 恆亮
      
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
      digitalWrite(ledPin, LOW);  // 更新失敗後關閉 LED
      
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
    digitalWrite(ledPin, LOW);
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
  digitalWrite(ledPin, HIGH);
  
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
  digitalWrite(ledPin, LOW);
  
  if (mqttClient.connected()) {
    String deviceId = getDeviceId();
    String statusTopic = "hoban/" + deviceId + "/status";
    String errorMsg = "update_failed";
    mqttClient.publish(statusTopic.c_str(), errorMsg.c_str(), true);
    Serial.println("已發送更新失敗狀態到 MQTT");
  }
}