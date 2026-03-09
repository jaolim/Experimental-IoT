#include "./env.h"
#include <WiFi.h>
#include <ezTime.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <PubSubClient.h>
#include <LittleFS.h>

//Leds
const int LED_PIN_SEND = 26;
const int LED_PIN_MQTT = 27;

//Local storage
const char* LOCAL_SETTINGS = "/saved_settings.txt";
 
// ===================== Rotary Encoder + Button =====================
// A=GPIO16 (CLK), B=GPIO17 (DT), SW=GPIO18 (to GND), INPUT_PULLUP
const int PIN_A  = 16;
const int PIN_B  = 17;
const int PIN_SW = 18;
 
// ---- Encoder (robust quadrature table) ----
volatile long encCount = 0;
volatile uint8_t prevAB = 0;
 
const int8_t ENC_TABLE[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};
 
void IRAM_ATTR onEncISR() {
  uint8_t a = (uint8_t)digitalRead(PIN_A);
  uint8_t b = (uint8_t)digitalRead(PIN_B);
  uint8_t currAB = (a << 1) | b;
 
  uint8_t idx = (prevAB << 2) | currAB;
  int8_t step = ENC_TABLE[idx];
 
  if (step) encCount += step;
  prevAB = currAB;
}
 
// Try 4, or 2/1 depending on your encoder
const int COUNTS_PER_DETENT = 4;
long lastDetent = 0;
 
// ---- Button debounce ----
const unsigned long BTN_DEBOUNCE_MS = 80;
bool stableBtn = HIGH;
bool lastRawBtn = HIGH;
unsigned long lastBtnChangeMs = 0;
 
// ===================== UI Menu =====================
enum UiState { UI_NORMAL, UI_SETUP_MENU, UI_SERVER_MENU, UI_SETTINGS_MENU };
UiState ui = UI_NORMAL;
 
// SETUP menu items: 0 Exit, 1 Server, 2 Sending ON/OFF
int menuIndex = 0;
const int SETUP_ITEMS = 3;
 
// SERVER menu items: 0 Mosquitto, 1 HH3D
int serverIndex = 0;
const int SERVER_ITEMS = 3;

// Storage menu items
int settingsIndex = 0;
const int SETTINGS_ITEMS = 3;
 
enum SendTarget { TARGET_MOSQUITTO, TARGET_HH3D };
SendTarget sendTarget = TARGET_MOSQUITTO;
 
bool sendingEnabled = false;
 
// ===================== LCD / Time / Sound =====================
Timezone Helsinki;
LiquidCrystal_I2C lcd(0x27, 20, 4);
 
const int LCD_SDA = 21;
const int LCD_SCL = 22;
 
const int SOUND_PIN = 34;
const int SAMPLE_WINDOW_MS = 50;
 
const uint32_t UPDATE_INTERVAL_MS = 10000;
const uint32_t SAMPLE_EVERY_MS    = 100;

float maxSound = 0;
int maxSoundPP = 0;
int highestAverage = 0;

const char* TEAM_NAME = ENV_SENDER;
const char* LOCATION  = ENV_LOCATION;
 
// Cache for instant redraw
int   lastAvgPP  = 0;
int   lastLastPP = 0;
String lastTimeStr = "";
String startedTime = "";
 
// ===================== WiFi =====================
const char* SSID = ENV_SSID;
const char* PASSWORD = ENV_PASSWORD;

// ===================== MQTT =====================
const char* MQTT_BROKER = ENV_BROKER;
const int   MQTT_PORT   = ENV_PORT;
const char* MQTT_TOPIC  = ENV_TOPIC;
 
WiFiClient net1;
PubSubClient mqtt(net1);
 
// reconnect pacing
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastMqttAttemptMs = 0;
//unsigned long lastMqtt2AttemptMs = 0;
const unsigned long WIFI_RETRY_MS  = 5000;
const unsigned long MQTT_RETRY_MS  = 5000;
 
// Local URL
const char* POST_URL_HH3D = ENV_URL;
 
// ===================== Prototypes =====================
void printPadded(uint8_t col, uint8_t row, const String& text);
String formEncode(const String& s);
int readSoundPP(int &outMinV, int &outMaxV);
bool postToSite(const char* postUrl, const String& team, const String& message1, const String& message2);
 
void drawSetupMenu();
void drawServerMenu();
void drawSettingsMenu();
void drawNormalScreen();
void handleEncoderAndButton(unsigned long now);
void readSettings();
void writeSettings();
 
void ensureWiFi(unsigned long now);
void ensureMqtt(unsigned long now);
bool connectMqttClient(PubSubClient& c, const char* broker, int port, const char* nameTag);

//Settings
void writeSettings () {
  String target;
  String sending;
  if (sendTarget == TARGET_MOSQUITTO) {
    target = "MOS";  
  } else {
    target = "HH3";
  }
  if (sendingEnabled) {
      sending = "ON";
  } else {
    sending = "OFF";  
  }
  File settingsFile = LittleFS.open(LOCAL_SETTINGS, FILE_WRITE);
  if (settingsFile) {
    settingsFile.println(target + "\n" + sending);
    settingsFile.close();
    Serial.println("Settings saved");
    Serial.println("Send target: " + target);
    Serial.println("Sending (1 = on, 0 = off): " + sending);
  } else {
    Serial.println("Error opening settings file");  
  }
}

void readSettings (){
  if (LittleFS.exists(LOCAL_SETTINGS)) {
    File settingsFile = LittleFS.open(LOCAL_SETTINGS, FILE_READ);
    String target = settingsFile.readStringUntil('\n');
    String sending = settingsFile.readStringUntil('\n');
    settingsFile.close();
    if (target == "MOS") {
      serverIndex = 0;
      sendTarget = TARGET_MOSQUITTO;
      Serial.println("Sending target set to Mosquitto");
    } else if (target == "HH3"){
        sendTarget = TARGET_HH3D;
        Serial.println("Sending target set to HH3D");
      }
    if (sending == "ON") {
      sendingEnabled = true;
      Serial.println("Sending enabled");
    } else {
      sendingEnabled = false;
      Serial.println("Sending disabled");
    }
    Serial.println("Send target: " + target);
    Serial.println("Sending: " + sending);
  } else {
    Serial.println("Settings file does not exist");  
  }
}

void defaultSettings () {
  sendTarget = TARGET_MOSQUITTO;
  serverIndex = 0;
  sendingEnabled = false;
  writeSettings();
}
 
// ===================== Helpers =====================
void printPadded(uint8_t col, uint8_t row, const String& text) {
  lcd.setCursor(col, row);
  lcd.print(text);
  int remaining = 20 - col - (int)text.length();
  for (int i = 0; i < remaining; i++) lcd.print(' ');
}
 
String formEncode(const String& s) {
  String out;
  out.reserve(s.length() * 2);
  const char* hex = "0123456789ABCDEF";
 
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~'
    ) {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

int readSoundPP(int &outMinV, int &outMaxV) {
  uint32_t start = millis();
  int minV = 4095;
  int maxV = 0;
 
  while (millis() - start < SAMPLE_WINDOW_MS) {
    int v = analogRead(SOUND_PIN);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
 
  outMinV = minV;
  outMaxV = maxV;
  return maxV - minV;
}
 
bool postToSite(const char* postUrl, const String& team, const String& message1, const String& message2) {
  if (!sendingEnabled) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
 
  WiFiClientSecure tls;
  tls.setInsecure();
 
  HTTPClient https;
  if (!https.begin(tls, postUrl)) {
    Serial.println("POST failed: https.begin() failed");
    return false;
  }
 
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body =
    "team=" + formEncode(team) +
    "&message1=" + formEncode(message1) +
    "&message2=" + formEncode(message2);
 
  int httpCode = https.POST(body);
  https.end();
  return (httpCode > 0 && httpCode < 400);
}
 
// ===================== Reconnect logic =====================
void ensureWiFi(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) return;
 
  if (now - lastWiFiAttemptMs < WIFI_RETRY_MS) return;
  lastWiFiAttemptMs = now;
 
  Serial.println("WiFi: reconnecting...");
  WiFi.disconnect(true, true);
  delay(50);
  WiFi.begin(SSID, PASSWORD);
}
 
bool connectMqttClient(PubSubClient& c, const char* broker, int port, const char* nameTag) {
  if (WiFi.status() != WL_CONNECTED) return false;
 
  c.setServer(broker, port);
 
  String client_id = "esp32-";
  client_id += nameTag;
  client_id += "-";
  client_id += String((uint32_t)ESP.getEfuseMac(), HEX);
 
  bool ok = c.connect(client_id.c_str());
  if (!ok) {
    Serial.print("MQTT ");
    Serial.print(nameTag);
    Serial.print(" connect fail, state=");
    Serial.println(c.state());
  }
  return ok;
}

void ensureMqtt(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) return;
 
  mqtt.loop();
 
  if (!mqtt.connected() && (now - lastMqttAttemptMs >= MQTT_RETRY_MS)) {
    lastMqttAttemptMs = now;
    Serial.println("MQTT: reconnect attempt");
    connectMqttClient(mqtt, MQTT_BROKER, MQTT_PORT, "m1");
  }
}
 
// ===================== Menu Draw =====================
//Sending status selection and target menu
void drawSetupMenu() {
  lcd.clear();
  printPadded(0, 0, "SETUP");
  printPadded(0, 1, String(menuIndex == 0 ? ">" : " ") + " Exit");
  printPadded(0, 2, String(menuIndex == 1 ? ">" : " ") + " Server");
  printPadded(0, 3, String(menuIndex == 2 ? ">" : " ") + " Settings");
  //String sendLine = String(menuIndex == 2 ? ">" : " ") + " Sending: " + (sendingEnabled ? "ON" : "OFF");
  //printPadded(0, 3, sendLine);
}

//Sending target selection
void drawServerMenu() {
  lcd.clear();
  printPadded(0, 0, "SERVER");
  printPadded(0, 1, String(serverIndex == 0 ? ">" : " ") + " Mosquitto");
  printPadded(0, 2, String(serverIndex == 1 ? ">" : " ") + " HH3D");
  //String cur = (sendTarget == TARGET_MOSQUITTO) ? "Send-> Mosquitto" : "Send-> HH3D";
  //printPadded(0, 3, cur);
  String sendLine = String(serverIndex == 2 ? ">" : " ") + " Sending: " + (sendingEnabled ? "ON" : "OFF");
  printPadded(0, 3, sendLine);
}

//Settings menu
void drawSettingsMenu() {
  lcd.clear();
  printPadded(0, 0, "SETTINGS");
  printPadded(0, 1, String(settingsIndex == 0 ? ">" : " ") + " Exit");
  printPadded(0, 2, String(settingsIndex == 1 ? ">" : " ") + " Reset Defaults");
  printPadded(0, 3, String(settingsIndex == 2 ? ">" : " ") + " Save Settings");
}

//Current time, sound readings, started time, sending status and target
void drawNormalScreen() {
  lcd.clear();
  String t = lastTimeStr.length() ? lastTimeStr : Helsinki.dateTime("H:i:s");
  printPadded(0, 0, "Time: " + t);
  printPadded(0, 1, "Avg: " + String(lastAvgPP) + " Max: " + String(highestAverage));
  printPadded(0, 2, "Started at: " + startedTime);
 
  String bottom;
  if (!sendingEnabled) bottom = "Sending OFF";
  else bottom = (sendTarget == TARGET_MOSQUITTO) ? "Send->Mosquitto" : "Send->HH3D";
  printPadded(0, 3, bottom);
}
 
// ===================== Input Handling =====================
void handleEncoderAndButton(unsigned long now) {
  ensureWiFi(now);
  ensureMqtt(now);
 
  // ---- Encoder movement ----
  long raw;
  noInterrupts();
  raw = encCount;
  interrupts();
 
  long detent = raw / COUNTS_PER_DETENT;
 
  if (detent != lastDetent) {
    long delta = detent - lastDetent;
    lastDetent = detent;
 
    if (ui == UI_SETUP_MENU) {
      menuIndex += (delta > 0 ? 1 : -1);
      if (menuIndex < 0) menuIndex = SETUP_ITEMS - 1;
      if (menuIndex >= SETUP_ITEMS) menuIndex = 0;
      drawSetupMenu();
    } else if (ui == UI_SERVER_MENU) {
      serverIndex += (delta > 0 ? 1 : -1);
      if (serverIndex < 0) serverIndex = SERVER_ITEMS - 1;
      //if (serverIndex < 0) serverIndex = 1;
      if (serverIndex >= SERVER_ITEMS) serverIndex = 0;
      //if (serverIndex > 1) serverIndex = 0;
      drawServerMenu();
    } else if (ui == UI_SETTINGS_MENU) {
      settingsIndex += (delta > 0 ? 1 : -1);
      if (settingsIndex < 0) settingsIndex = SETTINGS_ITEMS - 1;
      if (settingsIndex >= SETTINGS_ITEMS) settingsIndex = 0;
      drawSettingsMenu();
      }
  }
 
  // ---- Button debounce ----
  bool rawBtn = digitalRead(PIN_SW); // HIGH idle, LOW pressed
 
  if (rawBtn != lastRawBtn) {
    lastRawBtn = rawBtn;
    lastBtnChangeMs = now;
  }
 
  if ((now - lastBtnChangeMs) > BTN_DEBOUNCE_MS && rawBtn != stableBtn) {
    stableBtn = rawBtn;
    Serial.println("Click!");
    if (stableBtn == LOW) {
      
      if (ui == UI_NORMAL) {
        ui = UI_SETUP_MENU;
        menuIndex = 0;
        drawSetupMenu();
        return;
      }
 
      if (ui == UI_SETUP_MENU) {
        if (menuIndex == 0) {
          ui = UI_NORMAL;
          drawNormalScreen();
        } else if (menuIndex == 1) {
          ui = UI_SERVER_MENU;
          serverIndex = (sendTarget == TARGET_MOSQUITTO) ? 0 : 1;
          drawServerMenu();
        } else if (menuIndex == 2) {
          ui = UI_SETTINGS_MENU;
          drawSettingsMenu();
        }
        return;
      }
      
      if (ui == UI_SERVER_MENU) {
        if (serverIndex == 0) {
          sendTarget = TARGET_MOSQUITTO;
          ui = UI_SETUP_MENU;  
          drawSetupMenu();
        } else if (serverIndex == 1) {
          sendTarget = TARGET_HH3D;
          ui = UI_SETUP_MENU;  
          drawSetupMenu();
        } else if (serverIndex == 2) {
          sendingEnabled = !sendingEnabled;
          ui = UI_SETUP_MENU;
          drawSetupMenu();
        }
        return;
      }
      if (ui = UI_SETTINGS_MENU){
        
        if (settingsIndex == 0) {
          ui = UI_SETUP_MENU;
          drawSetupMenu();
        } else if (settingsIndex == 1) {
          defaultSettings();
          ui = UI_SETUP_MENU;
          drawSetupMenu();
        } else if (settingsIndex == 2) {
          writeSettings();
          ui = UI_SETUP_MENU;
          drawSetupMenu();
        }
        return;
      }
    }
  }
}
 
// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);
  delay(100);

  //Led pin initialization
  pinMode(LED_PIN_SEND, OUTPUT);
  pinMode(LED_PIN_MQTT, OUTPUT);
  digitalWrite(LED_PIN_SEND, HIGH);
  digitalWrite(LED_PIN_MQTT, HIGH);

   if (!LittleFS.begin(true)) {
    Serial.println("LittleFS: Error in initalization");
    return;
  }

 //Rotary button pin initialization
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
 
  prevAB = ((uint8_t)digitalRead(PIN_A) << 1) | (uint8_t)digitalRead(PIN_B);
  attachInterrupt(digitalPinToInterrupt(PIN_A), onEncISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_B), onEncISR, CHANGE);
 
  Wire.begin(LCD_SDA, LCD_SCL);
 
  analogReadResolution(12);
  analogSetPinAttenuation(SOUND_PIN, ADC_11db);
 
  lcd.init();
  lcd.backlight();
  lcd.clear();
  printPadded(0, 0, "Booting...");
 
  WiFi.begin(SSID, PASSWORD);
 
  // time
  waitForSync();
  Helsinki.setLocation("Europe/Helsinki");
 
  // init MQTT servers
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
 
  // attempt initial connects (non-fatal if fail)
  ensureWiFi(millis());
  ensureMqtt(millis());
 
  lastTimeStr = Helsinki.dateTime("H:i:s");
  drawNormalScreen();

  digitalWrite(LED_PIN_SEND, LOW);
  digitalWrite(LED_PIN_SEND, LOW);

  startedTime = Helsinki.dateTime("H:i:s");

  //Read saved settings
  readSettings();
}
 
void loop() {
  handleEncoderAndButton(millis());
 
  if (ui != UI_NORMAL) {
    delay(5);
    return;
  }

  if (sendingEnabled) {
      digitalWrite(LED_PIN_SEND, HIGH);
  } else {
    digitalWrite(LED_PIN_SEND, LOW);
  }

  if (serverIndex == 0) {
    digitalWrite(LED_PIN_MQTT, HIGH);
  } else {
    digitalWrite(LED_PIN_MQTT, LOW);
  }
 
  // ---- Sound averaging ----
  uint32_t start = millis();
  uint32_t sumPP = 0;
  uint16_t count = 0;
 
  int lastPP = 0;
  int lastMinV = 0, lastMaxV = 0;
 
  int lowestReading  = -1;
  int highestReading = -1;
 
  while (millis() - start < UPDATE_INTERVAL_MS) {
    handleEncoderAndButton(millis());
    if (ui != UI_NORMAL) return;
 
    lastPP = readSoundPP(lastMinV, lastMaxV);
    sumPP += (uint32_t)lastPP;
    count++;
 
    if (lowestReading == -1 || lowestReading > lastPP) lowestReading = lastPP;
    if (highestReading < lastPP) highestReading = lastPP;
 
    delay(SAMPLE_EVERY_MS);
  }
 
  int avgPP = (count > 0) ? (int)(sumPP / count) : 0;
  lastAvgPP   = avgPP;
  lastLastPP  = lastPP;
  lastTimeStr = Helsinki.dateTime("H:i:s");
 
  drawNormalScreen();
 
  PubSubClient& chosenMqtt  = mqtt;
  const char* chosenTopic   = MQTT_TOPIC;
 
  // Record sound record
  if (highestReading > maxSoundPP) {
    maxSoundPP = highestReading;
  }

  // Record average
  if (avgPP > highestAverage) {
    highestAverage = avgPP;  
  }

  //Local publishing
  if (sendingEnabled && serverIndex == 1) {
    String localMessage1 = "Noise last reading: Avg = " + String(avgPP) +
                    ", Min = " + String(lowestReading) +
                    ", Max = " + String(highestReading);
    String localMessage2 = "Noise Record = " + String(maxSoundPP);
    postToSite(POST_URL_HH3D, TEAM_NAME, localMessage1, localMessage2);
  }
  
  // MQTT publishing
  if (sendingEnabled && WiFi.status() == WL_CONNECTED && serverIndex == 0) {
    String mqttMessage = "Average, " + String(avgPP) + "; " +
                         "Min, " + String(lowestReading) + "; " +
                         "Max, " + String(highestReading) + ";";
    if (chosenMqtt.connected()) {
      chosenMqtt.publish(chosenTopic, mqttMessage.c_str());
    }
  }
  
}
