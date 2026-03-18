#pragma once

#include <Arduino.h>

class TokenManager {
 public:
  bool hasToken() const;
  const String &getAuthHeader() const;
  bool requestToken(bool forceNow = false);

 private:
  static void onRawMqttMessage(const String &topic, const String &payload);
  void handleRawMqttMessage(const String &topic, const String &payload);
  bool ensureMqttBinding();

  String makeFlag() const;
  static String extractJsonStringValue(const String &json, const char *key);

  bool mqttBound_ = false;
  bool requestPending_ = false;
  bool awaitingReply_ = false;
  bool tokenReady_ = false;
  uint32_t lastReqMs_ = 0;
  uint32_t reqStartMs_ = 0;

  String authHeader_;
  String tokenFlag_;
  String reqTopic_;
  String rspTopic_;
};
