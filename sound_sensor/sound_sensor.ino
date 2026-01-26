#include "./env.h"
#include <WiFi.h>
#include <ezTime.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

Timezone Helsinki;

LiquidCrystal_I2C lcd = LiquidCrystal_I2C(0x27, 20, 4); // For 20x4 LCD
int lcdColumns = 20;
int lcdRows = 4;

const char* SSID = ENV_SSID;
const char* PASSWORD = ENV_PASSWORD;

const int SOUND_PIN = 34;      // ADC pin for analog sound
//const int LED_PIN = 22;
//const int LED_PIN_ERROR = 23;

int timeRead = 0;
 
const int SAMPLE_WINDOW_MS = 50;   // measure over 50ms
int THRESHOLD = 120;               // tune this after you see numbers
 
int readSoundLevel() {
  uint32_t start = millis();
  int minV = 4095;
  int maxV = 0;
 
  while (millis() - start < SAMPLE_WINDOW_MS) {
    int v = analogRead(SOUND_PIN);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  return (maxV + minV) / 2;  // loudness
}
 
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing LCD...");
  delay(2000);
  lcd.clear();

  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected");

  Helsinki.setLocation("Europe/Helsinki");
 
  analogReadResolution(12); // 0..4095
 
  //pinMode(LED_PIN, OUTPUT);
  //pinMode(LED_PIN_ERROR, OUTPUT);
 
  //digitalWrite(LED_PIN, LOW);
  //digitalWrite(LED_PIN_ERROR, HIGH);
 
  Serial.println("Analog sound test (peak-to-peak).");
  Serial.println("Clap and watch the 'Level' number.");
}
 
void loop() {
  waitForSync();
  
  int level = readSoundLevel();
  
  if (level > THRESHOLD) {
    lcd.clear();
    Serial.println(">>> DETECTING SOUND! <<<");
    //digitalWrite(LED_PIN_ERROR, LOW);
    //digitalWrite(LED_PIN, HIGH);
    lcd.setCursor(0,0);
    lcd.print(Helsinki.dateTime());
    lcd.setCursor(0, 2);
    lcd.print("Sound level: ");
    lcd.setCursor(13, 2);
    lcd.print(level);
  } else {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(Helsinki.dateTime());
    lcd.setCursor(0, 2);
    lcd.print("Sound level: ");
    lcd.setCursor(13, 2);
    lcd.print(level);
    lcd.setCursor(0, 3);
    lcd.print("Below threshold");
    Serial.println("No sound");
    //digitalWrite(LED_PIN_ERROR, HIGH);
    //digitalWrite(LED_PIN, LOW);
  }
 
  delay(3000);
}
