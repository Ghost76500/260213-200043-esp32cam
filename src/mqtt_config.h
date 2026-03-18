#pragma once
#include <Arduino.h>

namespace MqttCfg {
constexpr char MQTT_HOST[] = "8.148.238.88";
constexpr uint16_t MQTT_PORT = 11115;
constexpr char MQTT_USER[] = "iotuser";
constexpr char MQTT_PASS[] = "studentIotTest";

/*
  你要改的行：
  1) DEVICE_ID 改成你的设备号（1111 或 2222）
*/
constexpr char DEVICE_ID[] = "1111";

/*
  你要改的行：
  2) 上报周期，单位毫秒。30000 = 30秒
*/
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 5000;

constexpr char CLIENT_ID_PREFIX[] = "esp32cam-";
}