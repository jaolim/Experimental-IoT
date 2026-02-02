#include "./env.h"
#include <WiFi.h>
#include <PubSubClient.h>

// WiFi
const char* SSID = ENV_SSID;
const char* PASSWORD = ENV_PASSWORD;

// MQTT Broker
const char *mqtt_broker = ENV_BROKER;
const char *topic = ENV_TOPIC;
const int mqtt_port = ENV_PORT;

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  // Set software serial baud to 115200;
  Serial.begin(115200);
  // Connecting to a Wi-Fi network
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.println("Connecting to WiFi..");
  }
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
}

void loop() {
  // put your main code here, to run repeatedly:

}
