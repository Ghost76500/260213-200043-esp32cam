#pragma once
#include <Arduino.h>

typedef void (*MqttFlagHandler)(const String &flag, const String &topic, const String &payload);
typedef void (*MqttRawMessageHandler)(const String &topic, const String &payload);

void mqttInit(MqttFlagHandler handler);
void mqttLoop();
bool mqttIsConnected();
void mqttReconnectIfNeeded();

bool mqttPublishStatusNormal(const String &power, const String &timeText);
bool mqttPublishStatusError(const String &timeText);
bool mqttPublishReplyByFlag(const String &flag, const String &jsonPayload);
bool mqttPublishRaw(const String &topic, const String &payload);
bool mqttSubscribeRaw(const String &topic);
void mqttSetRawMessageHandler(MqttRawMessageHandler handler);
String mqttBuildStatusNormalJson(const String &power, const String &timeText);
String mqttBuildStatusErrorJson(const String &timeText);

String mqttTopicToHard();
String mqttTopicToSoft();