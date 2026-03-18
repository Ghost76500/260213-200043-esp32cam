#include "token_manager.h"

#include "app_config.h"
#include "mqtt_config.h"
#include "mqtt_service.h"

extern void mqttSetRawMessageHandler(MqttRawMessageHandler handler);

namespace {
TokenManager *gTokenManagerInstance = nullptr;
}

bool TokenManager::hasToken() const {
  return tokenReady_ && authHeader_.length() > 0;
}

const String &TokenManager::getAuthHeader() const {
  return authHeader_;
}

bool TokenManager::ensureMqttBinding() {
  if (mqttBound_) {
    return true;
  }
  gTokenManagerInstance = this;
  mqttSetRawMessageHandler(TokenManager::onRawMqttMessage);
  mqttBound_ = true;
  return true;
}

String TokenManager::makeFlag() const {
  uint32_t chip = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFUL);
  return String(MqttCfg::DEVICE_ID) + "_" + String(chip, HEX) + "_" + String(millis());
}

String TokenManager::extractJsonStringValue(const String &json, const char *key) {
  String pattern = String("\"") + key + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos < 0) return "";

  int colonPos = json.indexOf(':', keyPos + pattern.length());
  if (colonPos < 0) return "";

  int i = colonPos + 1;
  while (i < json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
    ++i;
  }
  if (i >= json.length()) return "";

  if (json[i] == '"') {
    ++i;
    int end = json.indexOf('"', i);
    if (end < 0) return "";
    return json.substring(i, end);
  }

  int end = i;
  while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != '\r' && json[end] != '\n') {
    ++end;
  }

  String value = json.substring(i, end);
  value.trim();
  return value;
}

void TokenManager::onRawMqttMessage(const String &topic, const String &payload) {
  if (!gTokenManagerInstance) {
    return;
  }
  gTokenManagerInstance->handleRawMqttMessage(topic, payload);
}

void TokenManager::handleRawMqttMessage(const String &topic, const String &payload) {
  if (!awaitingReply_) {
    return;
  }
  if (rspTopic_.length() == 0 || topic != rspTopic_) {
    return;
  }

  String authHeader = extractJsonStringValue(payload, "Authorization");
  if (authHeader.length() == 0) {
    authHeader = payload;
    authHeader.trim();
    if (authHeader.startsWith("\"") && authHeader.endsWith("\"")) {
      authHeader = authHeader.substring(1, authHeader.length() - 1);
      authHeader.trim();
    }
  }

  if (authHeader.length() == 0 || authHeader.startsWith("{")) {
    Serial.println("[TOKEN] mqtt payload parse failed");
    requestPending_ = true;
    awaitingReply_ = false;
    tokenReady_ = false;
    return;
  }

  authHeader_ = authHeader;
  requestPending_ = false;
  awaitingReply_ = false;
  tokenReady_ = true;
  Serial.println("[TOKEN] mqtt token updated");
}

bool TokenManager::requestToken(bool forceNow) {
  ensureMqttBinding();

  if (forceNow) {
    tokenReady_ = false;
    requestPending_ = true;
    awaitingReply_ = false;
  }

  if (hasToken() && !forceNow) {
    return true;
  }

  uint32_t now = millis();

  if (awaitingReply_) {
    if (now - reqStartMs_ < AppCfg::Token::WAIT_TIMEOUT_MS) {
      return false;
    }
    Serial.println("[TOKEN] mqtt wait timeout, retry");
    awaitingReply_ = false;
    requestPending_ = true;
  }

  if (!requestPending_) {
    requestPending_ = true;
  }

  if (!mqttIsConnected()) {
    return false;
  }

  if (!forceNow && lastReqMs_ != 0 && (now - lastReqMs_ < AppCfg::Token::RETRY_INTERVAL_MS)) {
    return false;
  }

  tokenFlag_ = makeFlag();
  reqTopic_ = String(AppCfg::Token::REQ_TOPIC_PREFIX) + tokenFlag_ + AppCfg::Token::REQ_TOPIC_SUFFIX;
  rspTopic_ = String(AppCfg::Token::REQ_TOPIC_PREFIX) + tokenFlag_ + AppCfg::Token::RSP_TOPIC_SUFFIX;

  bool subOk = mqttSubscribeRaw(rspTopic_);
  bool pubOk = mqttPublishRaw(reqTopic_, "");
  bool ok = subOk && pubOk;

  lastReqMs_ = now;
  reqStartMs_ = now;
  awaitingReply_ = ok;
  requestPending_ = !ok;

  Serial.print("[TOKEN] mqtt request ");
  Serial.println(ok ? "sent" : "failed");
  if (ok) {
    Serial.print("[TOKEN] topic req=");
    Serial.println(reqTopic_);
    Serial.print("[TOKEN] topic rsp=");
    Serial.println(rspTopic_);
  }

  return ok;
}
