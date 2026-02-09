#include "./env.h"
#include <WiFi.h>
#include <ezTime.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <PubSubClient.h>
 
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
 
// ---- Button (simple debounce) ----
const unsigned long BTN_DEBOUNCE_MS = 80;
bool stableBtn = HIGH;
bool lastRawBtn = HIGH;
unsigned long lastBtnChangeMs = 0;
 
// ===================== UI Menu =====================
enum UiState { UI_NORMAL, UI_SETUP_MENU, UI_SERVER_MENU };
UiState ui = UI_NORMAL;
 
int menuIndex = 0;   // 0=Exit, 1=Server
int serverIndex = 0; // 0=Mosquitto, 1=HH3D
 
void printPadded(uint8_t col, uint8_t row, const String& text); // forward
void drawNormalScreen();                                       // forward
 
// ===================== WiFi / MQTT / LCD / Sound =====================
const char *mqtt_broker = ENV_BROKER;
const char *topic = ENV_TOPIC;
const int mqtt_port = ENV_PORT;
 
WiFiClient espClient;
PubSubClient client(espClient);
 
Timezone Helsinki;
 
// 20x4 LCD (0x27 or 0x3F depending on module)
LiquidCrystal_I2C lcd(0x27, 20, 4);
 
const char* SSID = ENV_SSID;
const char* PASSWORD = ENV_PASSWORD;
 
const int SOUND_PIN = 34;
const int SAMPLE_WINDOW_MS = 50;
 
// LCD I2C pins
const int LCD_SDA = 21;
const int LCD_SCL = 22;
 
// Averaging settings
const uint32_t UPDATE_INTERVAL_MS = 10000; // compute/display every 10s
const uint32_t SAMPLE_EVERY_MS    = 100;   // take one PP reading every 100ms
 
int THRESHOLD = 120;
float maxSound = 0;
 
// Relative dB reference baseline
const int PP_REF = 30;
 
// Posting
const char* POST_URL  = ENV_URL;
const char* TEAM_NAME = "J&J";
const char* LOCATION  = ENV_LOCATION;
 
// Cache last computed values so we can instantly redraw when exiting menu
int   lastAvgPP = 0;
int   lastLastPP = 0;
float lastDbRel = 0.0f;
bool  lastLoud  = false;
String lastTimeStr = "";
 
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
 
float ppToRelDb(int pp, int ppRef) {
  if (pp < 1) pp = 1;
  if (ppRef < 1) ppRef = 1;
  return 20.0f * log10f((float)pp / (float)ppRef);
}
 
// Peak-to-peak "volume"
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
 
bool postToSite(const String& team, const String& message1, const String& message2) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("POST skipped: WiFi not connected");
    return false;
  }
 
  WiFiClientSecure client;
  client.setInsecure();
 
  HTTPClient https;
  if (!https.begin(client, POST_URL)) {
    Serial.println("POST failed: https.begin() failed");
    return false;
  }
 
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
 
  String body =
    "team=" + formEncode(team) +
    "&message1=" + formEncode(message1) +
    "&message2=" + formEncode(message2);
 
  int httpCode = https.POST(body);
  String resp = https.getString();
  https.end();
 
  Serial.print("POST -> code: ");
  Serial.print(httpCode);
  Serial.print(" | resp: ");
  Serial.println(resp);
 
  return (httpCode > 0 && httpCode < 400);
}
 
// ===================== Menu Draw =====================
void drawSetupMenu() {
  lcd.clear();
  printPadded(0, 0, "SETUP");
  printPadded(0, 1, String(menuIndex == 0 ? ">" : " ") + " Exit");
  printPadded(0, 2, String(menuIndex == 1 ? ">" : " ") + " Server");
  printPadded(0, 3, "");
}
 
void drawServerMenu() {
  lcd.clear();
  printPadded(0, 0, "SERVER");
  printPadded(0, 1, String(serverIndex == 0 ? ">" : " ") + " Mosquitto");
  printPadded(0, 2, String(serverIndex == 1 ? ">" : " ") + " HH3D");
  printPadded(0, 3, "Press to choose");
}
 
// Draw normal screen instantly from cached values
void drawNormalScreen() {
  lcd.clear();
  String t = lastTimeStr.length() ? lastTimeStr : Helsinki.dateTime("H:i:s");
  printPadded(0, 0, "Time: " + t);
  printPadded(0, 1, "AvgPP: " + String(lastAvgPP) + " Th:" + String(THRESHOLD));
  printPadded(0, 2, "Rel dB: " + String(lastDbRel, 1) + " dB");
  printPadded(0, 3, lastLoud ? "DETECTING SOUND" : "Below threshold");
}
 
// ===================== Input Handling =====================
void handleEncoderAndButton(unsigned long now) {
  // ---- Encoder detent movement ----
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
      if (menuIndex < 0) menuIndex = 1;
      if (menuIndex > 1) menuIndex = 0;
      drawSetupMenu();
    } else if (ui == UI_SERVER_MENU) {
      serverIndex += (delta > 0 ? 1 : -1);
      if (serverIndex < 0) serverIndex = 1;
      if (serverIndex > 1) serverIndex = 0;
      drawServerMenu();
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
 
    // Select on press-down
    if (stableBtn == LOW) {
      if (ui == UI_NORMAL) {
        ui = UI_SETUP_MENU;
        menuIndex = 0;
        drawSetupMenu();
        return;
      }
 
      if (ui == UI_SETUP_MENU) {
        if (menuIndex == 0) {
          // Exit -> instantly back to normal screen
          ui = UI_NORMAL;
          drawNormalScreen();
        } else {
          // Server submenu
          ui = UI_SERVER_MENU;
          serverIndex = 0;
          drawServerMenu();
        }
        return;
      }
 
      if (ui == UI_SERVER_MENU) {
        // No real server switching yet — just show what was chosen
        lcd.clear();
        if (serverIndex == 0) printPadded(0, 0, "Selected:");
        else                 printPadded(0, 0, "Selected:");
        printPadded(0, 1, serverIndex == 0 ? "Mosquitto" : "HH3D");
        printPadded(0, 2, ""); 
        printPadded(0, 3, "Returning...");
 
        delay(700); // brief feedback
 
        ui = UI_SETUP_MENU;
        drawSetupMenu();
        return;
      }
    }
  }
}
 
// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);
  delay(100);
 
  // Encoder + Button init
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
 
  prevAB = ((uint8_t)digitalRead(PIN_A) << 1) | (uint8_t)digitalRead(PIN_B);
  attachInterrupt(digitalPinToInterrupt(PIN_A), onEncISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_B), onEncISR, CHANGE);
 
  // LCD / I2C
  Wire.begin(LCD_SDA, LCD_SCL);
 
  analogReadResolution(12);
  analogSetPinAttenuation(SOUND_PIN, ADC_11db);
 
  lcd.init();
  lcd.backlight();
  lcd.clear();
  printPadded(0, 0, "Booting...");
 
  // WiFi
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi: ");
  Serial.println(SSID);
  printPadded(0, 1, "WiFi connecting...");
 
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
    handleEncoderAndButton(millis());
  }
 
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
 
  // Time
  waitForSync();
  Helsinki.setLocation("Europe/Helsinki");
 
  // MQTT
  client.setServer(mqtt_broker, mqtt_port);
  while (!client.connected()) {
    String client_id = "esp32-client-";
    client_id += String(WiFi.macAddress());
    Serial.printf("MQTT connect as %s\n", client_id.c_str());
    if (client.connect(client_id.c_str())) {
      Serial.println("MQTT connected");
    } else {
      Serial.print("failed state ");
      Serial.println(client.state());
      delay(1000);
    }
    handleEncoderAndButton(millis());
  }
  client.publish(topic, "Device active");
 
  lcd.clear();
  printPadded(0, 0, "Ready");
  printPadded(0, 1, WiFi.localIP().toString());
  delay(1000);
 
  // Initial normal screen
  lastTimeStr = Helsinki.dateTime("H:i:s");
  drawNormalScreen();
}
 
void loop() {
  handleEncoderAndButton(millis());
 
  // If in menu, pause normal updating (menu stays responsive)
  if (ui != UI_NORMAL) {
    delay(5);
    return;
  }
 
  // ---- Sound averaging cycle, but keep menu responsive inside ----
  uint32_t start = millis();
  uint32_t sumPP = 0;
  uint16_t count = 0;
 
  int lastPP = 0;
  int lastMinV = 0, lastMaxV = 0;

  int lowestReading = -1;
  int highestReading = -1;
 
  while (millis() - start < UPDATE_INTERVAL_MS) {
    handleEncoderAndButton(millis());
    if (ui != UI_NORMAL) return;
 
    lastPP = readSoundPP(lastMinV, lastMaxV);
    sumPP += (uint32_t)lastPP;
    count++;
    if (lowestReading == -1 || lowestReading > lastPP) {
        lowestReading = lastPP;  
    }
    if (highestReading < lastPP) {
        highestReading = lastPP;  
    }
    delay(SAMPLE_EVERY_MS);
  }
 
  int avgPP = (count > 0) ? (int)(sumPP / count) : 0;
  float dbRel = ppToRelDb(avgPP, PP_REF);
  float dbLowest = ppToRelDb(lowestReading, PP_REF);
  float dbHighest = ppToRelDb(highestReading, PP_REF);
  bool loud = (avgPP > THRESHOLD);
 
  // Cache for instant redraw after exiting menu
  lastAvgPP = avgPP;
  lastLastPP = lastPP;
  lastDbRel = dbRel;
  lastLoud = loud;
  lastTimeStr = Helsinki.dateTime("H:i:s");
 
  // Serial debug
  Serial.print("lastPP="); Serial.print(lastPP);
  Serial.print(" avgPP="); Serial.print(avgPP);
  Serial.print(" rel_dB="); Serial.print(dbRel, 1);
  Serial.print(" ");
  Serial.println(loud ? "LOUD" : "quiet");
 
  // Normal LCD output
  drawNormalScreen();
 
  // Record logic (unchanged)
  if (dbRel > maxSound) {
    String newRecord = "New noise record: " + String(dbRel, 1);
    String oldRecord = "Old record: " + String(maxSound, 1);
    maxSound = dbRel;
 
    postToSite(TEAM_NAME, newRecord, oldRecord);
  }
  String mqttMessage = "Average, " + String(dbRel, 1) + "; " + 
                        "Min, " + String(dbLowest, 1) + "; " +
                        "Max, " + String(dbHighest, 1) + ";";
                        
  client.publish(topic, mqttMessage.c_str());
}
