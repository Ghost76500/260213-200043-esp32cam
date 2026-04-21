#include "mqtt_service.h"
#include "mqtt_config.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

static WiFiClient gWifiClient;
static PubSubClient gMqttClient(gWifiClient);
static MqttFlagHandler gFlagHandler = nullptr;
static MqttRawMessageHandler gRawHandler = nullptr;
static uint32_t gLastReconnectTryMs = 0;

String mqttTopicToHard() {
  return String("column/") + MqttCfg::DEVICE_ID + "/to-hard";
}

String mqttTopicToSoft() {
  return String("column/") + MqttCfg::DEVICE_ID + "/to-soft";
}

static String extractFlag(const String &json) {
  int keyPos = json.indexOf("\"flag\"");
  if (keyPos < 0) return "";

  int colonPos = json.indexOf(':', keyPos);
  if (colonPos < 0) return "";

  int q1 = json.indexOf('"', colonPos + 1);
  if (q1 < 0) return "";

  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return "";

  return json.substring(q1 + 1, q2);
}

static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; ++i) {
    body += (char)payload[i];
  }

  Serial.print("[MQTT] RX topic=");
  Serial.print(topic);
  Serial.print(" payload=");
  Serial.println(body);

  if (gRawHandler != nullptr) {
    gRawHandler(String(topic), body);
  }

  String flag = extractFlag(body);
  if (gFlagHandler != nullptr) {
    gFlagHandler(flag, String(topic), body);
  }
}

static bool mqttConnectNow() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String clientId = String(MqttCfg::CLIENT_ID_PREFIX) + MqttCfg::DEVICE_ID + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);

  bool ok = gMqttClient.connect(clientId.c_str(), MqttCfg::MQTT_USER, MqttCfg::MQTT_PASS);
  if (!ok) {
    Serial.print("[MQTT] connect failed, state=");
    Serial.println(gMqttClient.state());
    return false;
  }

  bool subOk = gMqttClient.subscribe(mqttTopicToHard().c_str());
  Serial.print("[MQTT] connected, subscribe ");
  Serial.print(mqttTopicToHard());
  Serial.print(" result=");
  Serial.println(subOk ? "ok" : "fail");

  return true;
}

void mqttInit(MqttFlagHandler handler) {
  gFlagHandler = handler;
  gMqttClient.setServer(MqttCfg::MQTT_HOST, MqttCfg::MQTT_PORT);
  gMqttClient.setCallback(onMqttMessage);
}

bool mqttIsConnected() {
  return gMqttClient.connected();
}

void mqttReconnectIfNeeded() {
  if (gMqttClient.connected()) return;

  uint32_t now = millis();
  if (now - gLastReconnectTryMs < 3000) return;
  gLastReconnectTryMs = now;

  mqttConnectNow();
}

void mqttLoop() {
  mqttReconnectIfNeeded();
  gMqttClient.loop();
}

String mqttBuildStatusNormalJson(const String &power, const String &timeText) {
  return String("{\"id\":\"") + MqttCfg::DEVICE_ID +
         "\",\"status\":\"normal\",\"power\":\"" + power +
         "\",\"updataTime\":\"" + timeText + "\"}";
}

String mqttBuildStatusErrorJson(const String &timeText) {
  return String("{\"id\":\"") + MqttCfg::DEVICE_ID +
         "\",\"status\":\"error\",\"updateTime\":\"" + timeText + "\"}";
}

bool mqttPublishStatusNormal(const String &power, const String &timeText) {
  if (!gMqttClient.connected()) return false;

  String json = mqttBuildStatusNormalJson(power, timeText);

  bool ok = gMqttClient.publish(mqttTopicToSoft().c_str(), json.c_str());
  Serial.print("[MQTT] TX ");
  Serial.print(mqttTopicToSoft());
  Serial.print(" ");
  Serial.println(json);
  return ok;
}

bool mqttPublishStatusError(const String &timeText) {
  if (!gMqttClient.connected()) return false;

  String json = mqttBuildStatusErrorJson(timeText);

  bool ok = gMqttClient.publish(mqttTopicToSoft().c_str(), json.c_str());
  Serial.print("[MQTT] TX ");
  Serial.print(mqttTopicToSoft());
  Serial.print(" ");
  Serial.println(json);
  return ok;
}

bool mqttPublishReplyByFlag(const String &flag, const String &jsonPayload) {
  if (!gMqttClient.connected()) return false;

  String topic = mqttTopicToSoft() + "/" + flag;
  bool ok = gMqttClient.publish(topic.c_str(), jsonPayload.c_str());

  Serial.print("[MQTT] TX ");
  Serial.print(topic);
  Serial.print(" ");
  Serial.println(jsonPayload);

  return ok;
}

bool mqttPublishRaw(const String &topic, const String &payload) {
  if (!gMqttClient.connected()) return false;

  bool ok = gMqttClient.publish(topic.c_str(), payload.c_str());
  Serial.print("[MQTT] TX ");
  Serial.print(topic);
  Serial.print(" ");
  Serial.println(payload);
  return ok;
}

bool mqttSubscribeRaw(const String &topic) {
  if (!gMqttClient.connected()) return false;

  bool ok = gMqttClient.subscribe(topic.c_str());
  Serial.print("[MQTT] SUB ");
  Serial.print(topic);
  Serial.print(" result=");
  Serial.println(ok ? "ok" : "fail");
  return ok;
}

void mqttSetRawMessageHandler(MqttRawMessageHandler handler) {
  gRawHandler = handler;
}