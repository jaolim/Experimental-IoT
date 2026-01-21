#include "./env.h"

const int SOUND_PIN = 34;      // ADC pin for analog sound
const int LED_PIN = 22;
const int LED_PIN_ERROR = 23;
 
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
  return maxV - minV;  // loudness
}
 
void setup() {
  Serial.begin(115200);
 
  analogReadResolution(12); // 0..4095
 
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_PIN_ERROR, OUTPUT);
 
  digitalWrite(LED_PIN, LOW);
  digitalWrite(LED_PIN_ERROR, HIGH);
 
  Serial.println("Analog sound test (peak-to-peak).");
  Serial.println("Clap and watch the 'Level' number.");
}
 
void loop() {
  int level = readSoundLevel();
 
  Serial.print("Level: ");
  Serial.println(level);
 
  if (level > THRESHOLD) {
    Serial.println(">>> DETECTING SOUND! <<<");
    digitalWrite(LED_PIN_ERROR, LOW);
    digitalWrite(LED_PIN, HIGH);
  } else {
    Serial.println("No sound");
    digitalWrite(LED_PIN_ERROR, HIGH);
    digitalWrite(LED_PIN, LOW);
  }
 
  delay(200);
}
