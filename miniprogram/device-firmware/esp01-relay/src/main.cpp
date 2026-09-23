#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>

// Fill these values only with the broker that you own or are explicitly
// authorised to use. The placeholder deliberately prevents a public-broker
// connection until this template is configured.
static const char* MQTT_HOST = "YOUR_MQTT_BROKER_HOST";
static const uint16_t MQTT_PORT = 1883;
static const char* MQTT_USERNAME = "";
static const char* MQTT_PASSWORD = "";

// ESP-01S relay boards commonly use GPIO0 and active-low logic.
static const uint8_t RELAY_PIN = 0;
static const bool RELAY_ACTIVE_LOW = true;

static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 3000UL;
static const unsigned long HEARTBEAT_INTERVAL_MS = 15000UL;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

String deviceId;
String controlTopic;
String stateTopic;
String availabilityTopic;
bool relayOn = false;
unsigned long lastMqttAttempt = 0;
unsigned long lastHeartbeat = 0;

bool brokerConfigured() {
  return MQTT_HOST != nullptr
      && strlen(MQTT_HOST) > 0
      && strcmp(MQTT_HOST, "YOUR_MQTT_BROKER_HOST") != 0
      && MQTT_PORT > 0;
}

void buildTopics() {
  deviceId = String(ESP.getChipId(), HEX);
  deviceId.toUpperCase();
  controlTopic = String("shuangshou/control/") + deviceId;
  stateTopic = String("shuangshou/status/") + deviceId;
  availabilityTopic = String("shuangshou/availability/") + deviceId;
}

void publishAvailability(const char* value) {
  if (!mqttClient.connected()) return;
  mqttClient.publish(availabilityTopic.c_str(), value, true);
}

void publishRelayState() {
  if (!mqttClient.connected()) return;
  // The mini program accepts this deliberately small, interoperable payload.
  mqttClient.publish(stateTopic.c_str(), relayOn ? "ON" : "OFF", true);
}

void applyRelay(bool on) {
  relayOn = on;
  const uint8_t activeLevel = RELAY_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t inactiveLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(RELAY_PIN, on ? activeLevel : inactiveLevel);
  Serial.print("[RELAY] state=");
  Serial.println(on ? "ON" : "OFF");
  publishRelayState();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != controlTopic) return;

  String body;
  body.reserve(length);
  for (unsigned int index = 0; index < length; index++) {
    body += static_cast<char>(payload[index]);
  }
  body.trim();
  body.toUpperCase();

  Serial.print("[MQTT] command=");
  Serial.println(body);
  if (body == "ON") {
    applyRelay(true);
  } else if (body == "OFF") {
    applyRelay(false);
  } else {
    Serial.println("[MQTT] unsupported payload; expected plain ON or OFF");
  }
}

bool connectMqtt() {
  if (!brokerConfigured()) {
    Serial.println("[MQTT] broker not configured; no public broker connection is attempted");
    return false;
  }

  const String clientId = String("esp01-relay-") + deviceId;
  Serial.print("[MQTT] connecting as ");
  Serial.println(clientId);

  bool connected = false;
  if (strlen(MQTT_USERNAME) > 0) {
    connected = mqttClient.connect(
      clientId.c_str(),
      MQTT_USERNAME,
      MQTT_PASSWORD,
      availabilityTopic.c_str(),
      0,
      true,
      "OFFLINE"
    );
  } else {
    connected = mqttClient.connect(
      clientId.c_str(),
      availabilityTopic.c_str(),
      0,
      true,
      "OFFLINE"
    );
  }

  if (!connected) {
    Serial.print("[MQTT] connect failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }

  mqttClient.subscribe(controlTopic.c_str());
  Serial.print("[MQTT] subscribed ");
  Serial.println(controlTopic);
  publishAvailability("ONLINE");
  publishRelayState();
  lastHeartbeat = millis();
  return true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  const String apName = String("Shuangshou-") + deviceId;

  if (!wifiManager.autoConnect(apName.c_str())) {
    Serial.println("[WIFI] config portal timeout; restarting");
    delay(1000);
    ESP.restart();
  }

  Serial.println("[WIFI] connected");
  Serial.print("[WIFI] ip=");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(100);
  buildTopics();

  // Set the inactive level before enabling the output to reduce boot-time
  // relay chatter. GPIO0 is also a boot strap pin; follow the wiring notes.
  const uint8_t inactiveLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(RELAY_PIN, inactiveLevel);
  pinMode(RELAY_PIN, OUTPUT);
  applyRelay(false);

  Serial.print("[DEVICE] id=");
  Serial.println(deviceId);
  Serial.print("[DEVICE] control=");
  Serial.println(controlTopic);
  Serial.print("[DEVICE] state=");
  Serial.println(stateTopic);
  Serial.print("[DEVICE] availability=");
  Serial.println(availabilityTopic);

  connectWiFi();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] disconnected; restarting");
    delay(1000);
    ESP.restart();
    return;
  }

  if (!brokerConfigured()) {
    delay(1000);
    return;
  }

  const unsigned long now = millis();
  if (!mqttClient.connected()) {
    if (now - lastMqttAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
      lastMqttAttempt = now;
      connectMqtt();
    }
    return;
  }

  mqttClient.loop();
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    publishAvailability("ONLINE");
    lastHeartbeat = now;
  }
}
