#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace AppCfg {
namespace Wifi {
constexpr char SSID[] = "Link";
constexpr char PASSWORD[] = "0d000721";
}

namespace Record {
constexpr uint16_t FPS = 10;
constexpr uint16_t WIDTH = 320;
constexpr uint16_t HEIGHT = 240;
constexpr uint32_t DURATION_MS = 10UL * 1000UL;
}

namespace Upload {
constexpr uint32_t RETRY_INTERVAL_MS = 10UL * 1000UL;
constexpr char PRIMARY_URL[] = "http://8.148.238.88:26221/api/soft/column/video";
constexpr char FALLBACK_URL[] = "http://8.148.238.88/api/soft/column/video";
constexpr char STATIC_AUTH_HEADER[] = "";
constexpr char COOKIE_HEADER[] = "";
}

namespace Token {
constexpr uint32_t RETRY_INTERVAL_MS = 5UL * 1000UL;
constexpr uint32_t WAIT_TIMEOUT_MS = 8UL * 1000UL;
constexpr char REQ_TOPIC_PREFIX[] = "column/";
constexpr char REQ_TOPIC_SUFFIX[] = "/token-to-soft";
constexpr char RSP_TOPIC_SUFFIX[] = "/token-to-hard";
}
}
