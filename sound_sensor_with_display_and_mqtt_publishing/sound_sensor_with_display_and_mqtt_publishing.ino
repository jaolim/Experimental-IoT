#include "./env.h"
#include <WiFi.h>
#include <ezTime.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>   // <-- for log10f
#include <PubSubClient.h>

boolean setupMode = false;
boolean publishMqtt = true;
boolean publishLocal = true;

// ESP32 Rotary Encoder + Button test (ROBUST quadrature decode)
// Pins: CLK(A)=GPIO16, DT(B)=GPIO17, SW=GPIO18 (to GND, INPUT_PULLUP)
// Serial Monitor: 115200
 
const int PIN_A  = 16;   // CLK
const int PIN_B  = 17;   // DT
const int PIN_SW = 18;   // button
 
// ---------- Encoder (quadrature state machine) ----------
volatile long encoderPos = 0;
volatile uint8_t prevAB = 0;
 
// Transition table for quadrature decode (Gray code)
// Index: (prev<<2) | curr, value: -1, 0, +1
// This is a standard robust table that rejects invalid/bouncy transitions.
const int8_t ENC_TABLE[16] = {
  0, -1, +1,  0,
+1,  0,  0, -1,
-1,  0,  0, +1,
  0, +1, -1,  0
};
 
void IRAM_ATTR onEncChange() {
  if (!setupMode) {
    setupMode = true; 
  }
  uint8_t a = (uint8_t)digitalRead(PIN_A);
  uint8_t b = (uint8_t)digitalRead(PIN_B);
  uint8_t currAB = (a << 1) | b;
 
  uint8_t idx = (prevAB << 2) | currAB;
  int8_t step = ENC_TABLE[idx];
 
  encoderPos += step;     // step = +1 or -1 or 0
  prevAB = currAB;
}
 
// If your encoder gives 4 counts per detent, you can divide by 4 when printing.
const int COUNTS_PER_DETENT = 4; // common for many encoders; set to 2 or 1 if needed
 
long lastPrintedDetent = 0;
 
// ---------- Button (single/double/long hold) ----------
const unsigned long DEBOUNCE_MS   = 35;
const unsigned long DOUBLE_TAP_MS = 350;
const unsigned long LONG_HOLD_MS  = 700;
 
bool stableBtn = HIGH;      // debounced stable state
bool lastRawBtn = HIGH;
unsigned long lastDebounceMs = 0;
 
unsigned long pressStartMs = 0;
bool longHoldFired = false;
 
int tapCount = 0;
unsigned long firstTapReleaseMs = 0;

// MQTT Broker
const char *mqtt_broker = ENV_BROKER;
const char *topic = ENV_TOPIC;
const int mqtt_port = ENV_PORT;

WiFiClient espClient;
PubSubClient client(espClient);
 
Timezone Helsinki;
 
// 20x4 LCD (change to 0x3F if your LCD uses that address)
LiquidCrystal_I2C lcd(0x27, 20, 4);
 
const char* SSID = ENV_SSID;
const char* PASSWORD = ENV_PASSWORD;
 
const int SOUND_PIN = 34;
const int SAMPLE_WINDOW_MS = 50;
 
// LCD I2C pins (21 & 22)
const int LCD_SDA = 21;
const int LCD_SCL = 22;
 
// Averaging settings
const uint32_t UPDATE_INTERVAL_MS = 10000; // compute/display every 10s
const uint32_t SAMPLE_EVERY_MS    = 100;   // take one PP reading every 100ms
 
int THRESHOLD = 120;   // threshold in PP units (still ok to keep)
float maxSound  = 0;     // record in PP units
 
// Relative dB reference (quiet baseline). Tune this using Serial output.
// Example: if avgPP in silence is ~30, set PP_REF = 30.
const int PP_REF = 30;
 
// Posting
const char* POST_URL  = ENV_URL;
const char* TEAM_NAME = "J&J";
const char* LOCATION = ENV_LOCATION;

// Led
const int LED_PIN = 2;

// ---------- Helpers ----------
 
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
 
// Convert peak-to-peak to relative dB
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
 
// POST to site: team, message1, message2
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
 
void setup() {
  Serial.begin(115200);
  delay(100);

  //Button setup
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
 
  // Initialize previous state
  uint8_t a = digitalRead(PIN_A);
  uint8_t b = digitalRead(PIN_B);
  prevAB = (a << 1) | b;
 
  // Interrupt on BOTH pins gives best accuracy
  attachInterrupt(digitalPinToInterrupt(PIN_A), onEncChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_B), onEncChange, CHANGE);
 
  Serial.println("=== Rotary Encoder (Robust) + Button Test ===");
  Serial.println("Expected: Right -> +, Left -> -");
  Serial.println("Button: SINGLE / DOUBLE / LONG HOLD");
  Serial.println("--------------------------------------------");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  Wire.begin(LCD_SDA, LCD_SCL);
 
  analogReadResolution(12);
  analogSetPinAttenuation(SOUND_PIN, ADC_11db);
 
  lcd.init();
  lcd.backlight();
  lcd.clear();
  printPadded(0, 0, "Booting...");
 
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi: ");
  Serial.println(SSID);
  printPadded(0, 1, "WiFi connecting...");
 
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
 
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
 
  waitForSync();
  Helsinki.setLocation("Europe/Helsinki");

  client.setServer(mqtt_broker, mqtt_port);
  while (!client.connected()) {
    String client_id = "esp32-client-";
    client_id += String(WiFi.macAddress());
    Serial.printf("The client %s connects to the public MQTT broker\n", client_id.c_str());
    if (client.connect(client_id.c_str())) {
      Serial.println("Public Mosquitto broker connected");
     } else {
       Serial.print("failed with state ");
       Serial.print(client.state());
       delay(2000);    
    }
  }
  client.publish(topic, "Device active");
 
  lcd.clear();
  printPadded(0, 0, "Ready");
  printPadded(0, 1, WiFi.localIP().toString());
  delay(1000);
  lcd.clear();
  digitalWrite(LED_PIN, LOW);
}
 
void loop() {
  if (setupMode){
      // ----- Encoder: read safely and print detents -----
    long rawPos;
    noInterrupts();
    rawPos = encoderPos;
    interrupts();
   
    // Convert raw counts to detents (so it doesn't "rise too fast")
    long detentPos = rawPos / COUNTS_PER_DETENT;
   
    if (detentPos != lastPrintedDetent) {
      Serial.print("Encoder detent position: ");
      Serial.print(detentPos);
      Serial.print(" (");
      Serial.print(detentPos > lastPrintedDetent ? "RIGHT/+ " : "LEFT/- ");
      Serial.println(")");
      lastPrintedDetent = detentPos;
    }
   
    // ----- Button handling (debounced) -----
    unsigned long now = millis();
    bool rawBtn = digitalRead(PIN_SW); // HIGH idle, LOW pressed
   
    if (rawBtn != lastRawBtn) {
      lastDebounceMs = now;
      lastRawBtn = rawBtn;
    }
   
    if ((now - lastDebounceMs) > DEBOUNCE_MS && rawBtn != stableBtn) {
      stableBtn = rawBtn;
   
      if (stableBtn == LOW) {
        pressStartMs = now;
        longHoldFired = false;
        Serial.println("Button: DOWN");
      } else {
        unsigned long heldMs = now - pressStartMs;
        Serial.print("Button: UP (held ");
        Serial.print(heldMs);
        Serial.println(" ms)");
   
        if (!longHoldFired) {
          tapCount++;
          if (tapCount == 1) firstTapReleaseMs = now;
        } else {
          tapCount = 0;
        }
      }
    }
   
    if (stableBtn == LOW && !longHoldFired) {
      if (now - pressStartMs >= LONG_HOLD_MS) {
        longHoldFired = true;
        tapCount = 0;
        Serial.println("Button: LONG HOLD");
      }
    }
   
    if (tapCount > 0 && stableBtn == HIGH) {
      if (now - firstTapReleaseMs > DOUBLE_TAP_MS) {
        if (tapCount == 1) Serial.println("Button: SINGLE TAP");
        else               Serial.println("Button: DOUBLE TAP");
        tapCount = 0;
      }
    }
   
    delay(2);
  } else {
      
    uint32_t start = millis();
    uint32_t sumPP = 0;
    uint16_t count = 0;
   
    int lastPP = 0;
    int lastMinV = 0, lastMaxV = 0;
  
    int lowestReading = -1;
    int highestReading = -1;
    
    while (millis() - start < UPDATE_INTERVAL_MS) {
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
   
    // Compute relative dB from avgPP
    float dbRel = ppToRelDb(avgPP, PP_REF);
    float dbLowest = ppToRelDb(lowestReading, PP_REF);
    float dbHighest = ppToRelDb(highestReading, PP_REF);
   
    bool loud = (avgPP > THRESHOLD);
   
    // Serial debug
    Serial.print("min=");
    Serial.print(lastMinV);
    Serial.print(" max=");
    Serial.print(lastMaxV);
    Serial.print(" lastPP=");
    Serial.print(lastPP);
    Serial.print(" avgPP=");
    Serial.print(avgPP);
    Serial.print(" rel_dB=");
    Serial.print(dbRel, 1);
    Serial.print(" (ref=");
    Serial.print(PP_REF);
    Serial.print(") n=");
    Serial.print(count);
    Serial.print(" ");
    Serial.println(loud ? "LOUD" : "quiet");
   
    // LCD output (show dB)
    lcd.clear();
    printPadded(0, 0, "Time: " + Helsinki.dateTime("H:i:s"));
    printPadded(0, 1, "AvgPP: " + String(avgPP) + " Th:" + String(THRESHOLD));
    printPadded(0, 2, "Rel dB: " + String(dbRel, 1) + " dB");
    printPadded(0, 3, loud ? "DETECTING SOUND" : "Below threshold");
   
    // Build website messages (include dB)
    String msg2 = "LastPP:" + String(lastPP) + " dB:" + String(dbRel, 1);
  
    Serial.println("LastPP: " + String(dbRel, 1));
    Serial.println("maxSound: " + String(maxSound, 1));
    
    if (dbRel > maxSound) {
      String newRecord = "New noise record: " + String(dbRel, 1);
      String oldRecord = "Old record: " + String(maxSound);
      maxSound = dbRel;
      if (publishLocal) {
        postToSite(TEAM_NAME, newRecord, oldRecord);
      }
    }
    String mqttMessage = "Average, " + String(dbRel, 1) + "; " + 
                          "Min, " + String(dbLowest, 1) + "; " +
                          "Max, " + String(dbHighest, 1) + ";";

    if (publishMqtt) {
      client.publish(topic, mqttMessage.c_str());
    }
  }
}
