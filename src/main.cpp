#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

#include "board_config.h"
#include "mqtt_config.h"
#include "mqtt_service.h"
#include "avi_recorder.h"
#include "video_uploader.h"

#include "FS.h"
#include "SD_MMC.h"

const char *ssid = "Link";
const char *password = "0d000721";
uint8_t kNormalPower_t = 0;

static const uint16_t kRecordFps = 10;
static const uint32_t kRecordDurationMs = 10UL * 1000UL;
static const uint32_t kUploadRetryIntervalMs = 10UL * 1000UL;
static const uint32_t kMqttTokenRetryIntervalMs = 5UL * 1000UL;
static const uint32_t kMqttTokenWaitTimeoutMs = 8UL * 1000UL;
static const char *kVideoUploadUrlPrimary = "http://8.148.238.88:26221/api/soft/column/video";
static const char *kVideoUploadUrlFallback = "http://8.148.238.88/api/soft/column/video";
static const char *kMqttTokenReqTopicPrefix = "column/";
static const char *kMqttTokenReqTopicSuffix = "/token-to-soft";
static const char *kMqttTokenRspTopicSuffix = "/token-to-hard";
// 按后端鉴权方式填入（示例："Bearer eyJ..."）。不需要可留空。
static const char *kVideoUploadAuthorization = "";
// 若后端依赖会话 Cookie，可在此填写（示例："JSESSIONID=xxxx"）。
static const char *kVideoUploadCookie = "";
static bool gRecordRequested = false;
static uint32_t gRecordStartMs = 0;
static String gCurrentRecordPath;
static bool gUploadPending = false;
static uint32_t gLastUploadTryMs = 0;
static String gUploadAuthorizationHeader;
static bool gMqttTokenRequestPending = false;
static bool gMqttTokenAwaiting = false;
static bool gMqttTokenReady = false;
static uint32_t gLastMqttTokenReqMs = 0;
static uint32_t gMqttTokenReqStartMs = 0;
static String gMqttTokenFlag;
static String gMqttTokenReqTopic;
static String gMqttTokenRspTopic;

static String extractJsonStringValue(const String &json, const char *key) {
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

static String makeMqttTokenFlag() {
  uint32_t chip = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFUL);
  return String(MqttCfg::DEVICE_ID) + "_" + String(chip, HEX) + "_" + String(millis());
}

static bool requestTokenViaMqtt(bool forceNow) {
  if (!mqttIsConnected()) {
    gMqttTokenRequestPending = true;
    return false;
  }

  if (!forceNow && gMqttTokenAwaiting) {
    return false;
  }

  uint32_t now = millis();
  if (!forceNow && gLastMqttTokenReqMs != 0 && (now - gLastMqttTokenReqMs < kMqttTokenRetryIntervalMs)) {
    return false;
  }

  if (forceNow || gMqttTokenFlag.length() == 0) {
    gMqttTokenFlag = makeMqttTokenFlag();
  }
  gMqttTokenReqTopic = String(kMqttTokenReqTopicPrefix) + gMqttTokenFlag + kMqttTokenReqTopicSuffix;
  gMqttTokenRspTopic = String(kMqttTokenReqTopicPrefix) + gMqttTokenFlag + kMqttTokenRspTopicSuffix;

  bool subOk = mqttSubscribeRaw(gMqttTokenRspTopic);
  bool pubOk = mqttPublishRaw(gMqttTokenReqTopic, "");
  bool ok = subOk && pubOk;

  gLastMqttTokenReqMs = now;
  gMqttTokenReqStartMs = now;
  gMqttTokenAwaiting = ok;
  gMqttTokenReady = false;
  gMqttTokenRequestPending = !ok;

  Serial.print("[TOKEN] mqtt request ");
  Serial.println(ok ? "sent" : "failed");
  if (ok) {
    Serial.print("[TOKEN] topic req=");
    Serial.println(gMqttTokenReqTopic);
    Serial.print("[TOKEN] topic rsp=");
    Serial.println(gMqttTokenRspTopic);
  }

  return ok;
}

static bool updateTokenFromMqttPayload(const String &payload) {
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
    return false;
  }

  gUploadAuthorizationHeader = authHeader;
  gMqttTokenAwaiting = false;
  gMqttTokenReady = true;
  gMqttTokenRequestPending = false;
  Serial.println("[TOKEN] mqtt token updated");
  return true;
}

static bool uploadRecordedVideoWithFallback(const String &path, VideoUploadResult *outResult, String *outUsedUrl) {
  const char *urls[] = {kVideoUploadUrlPrimary, kVideoUploadUrlFallback};
  const size_t count = sizeof(urls) / sizeof(urls[0]);

  VideoUploadResult last = {false, -1, ""};

  for (size_t i = 0; i < count; ++i) {
    const char *url = urls[i];
    if (!url || strlen(url) == 0) {
      continue;
    }

    Serial.print("[UPLOAD] try -> ");
    Serial.print(url);
    Serial.print(" id=");
    Serial.print(MqttCfg::DEVICE_ID);
    Serial.print(" file=");
    Serial.println(path);

    const char *authHeader = kVideoUploadAuthorization;
    if (gUploadAuthorizationHeader.length() > 0) {
      authHeader = gUploadAuthorizationHeader.c_str();
    }

    VideoUploadOptions options = {
      .authorizationHeader = authHeader,
      .cookieHeader = kVideoUploadCookie,
    };

    VideoUploadResult r;
    bool ok = videoUploaderUploadAviMultipart(url, MqttCfg::DEVICE_ID, path.c_str(), &options, &r);
    if (ok) {
      if (outResult) {
        *outResult = r;
      }
      if (outUsedUrl) {
        *outUsedUrl = String(url);
      }
      return true;
    }

    last = r;

    // 状态码可解析时，说明服务端已响应。此类失败通常是业务/鉴权问题，不再切到其它端口重试。
    if (r.statusCode > 0) {
      if (outResult) {
        *outResult = r;
      }
      if (outUsedUrl) {
        *outUsedUrl = String(url);
      }
      return false;
    }
  }

  if (outResult) {
    *outResult = last;
  }
  if (outUsedUrl) {
    *outUsedUrl = String(urls[count - 1]);
  }
  return false;
}

static String nextRecordPath() {
  for (uint16_t i = 1; i <= 9999; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "/record_%04u.avi", i);
    if (!SD_MMC.exists(path)) {
      return String(path);
    }
  }
  return String("/record_last.avi");
}

static String normalPowerText() {
  if (kNormalPower_t > 100) {
    kNormalPower_t = 100;
  }
  return String(kNormalPower_t);
}

// 启动摄像头 HTTP 服务（在 app_httpd.cpp 中实现）
void startCameraServer();
// 初始化补光灯控制（在 app_httpd.cpp 中实现，具体取决于板级定义）
void setupLedFlash();

static bool initSdCard1Bit() {
  struct SdMode {
    bool mode1bit;
    const char *name;
  };

  const SdMode modes[] = {
    {true, "1-bit"},
    {false, "4-bit"},
  };

  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    SD_MMC.end();
    delay(20);

    Serial.print("[SD] init start (SD_MMC ");
    Serial.print(modes[i].name);
    Serial.println(" mode)");

    bool ok = SD_MMC.begin("/sdcard", modes[i].mode1bit);
    if (!ok) {
      Serial.print("[SD] mount failed (");
      Serial.print(modes[i].name);
      Serial.println(")");
      continue;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
      Serial.print("[SD] no card (");
      Serial.print(modes[i].name);
      Serial.println(")");
      SD_MMC.end();
      continue;
    }

    uint64_t cardSizeMB = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    Serial.print("[SD] mount ok, mode=");
    Serial.print(modes[i].name);
    Serial.print(" type=");
    if (cardType == CARD_MMC) {
      Serial.print("MMC");
    } else if (cardType == CARD_SD) {
      Serial.print("SDSC");
    } else if (cardType == CARD_SDHC) {
      Serial.print("SDHC/SDXC");
    } else {
      Serial.print("UNKNOWN");
    }
    Serial.print(", size=");
    Serial.print((unsigned long)cardSizeMB);
    Serial.println("MB");
    return true;
  }

  Serial.println("[SD] all init modes failed. Check card insert, card format(FAT32/exFAT), and pull-up resistors.");
  return false;
}

// 生成当前运行时间字符串（以固定日期 + 运行时分秒形式返回）
static String nowText() {
  // 基于设备上电后的毫秒计数换算时分秒
  uint32_t sec = millis() / 1000;
  uint32_t hh = (sec / 3600) % 24;
  uint32_t mm = (sec / 60) % 60;
  uint32_t ss = sec % 60;

  char buf[32];
  // 组装业务需要的时间文本
  snprintf(buf, sizeof(buf), "2026-02-15 %02lu:%02lu:%02lu", (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}

// MQTT 业务标记请求回调：收到带 flag 的请求后，构造并发布对应回复
static void onFlagRequest(const String &flag, const String &topic, const String &payload) {
  // 打印收到的请求信息，便于串口调试
  Serial.print("[APP] flag request, topic=");
  Serial.print(topic);
  Serial.print(" flag=");
  Serial.print(flag);
  Serial.print(" payload=");
  Serial.println(payload);

  if (gMqttTokenRspTopic.length() > 0 && topic == gMqttTokenRspTopic) {
    updateTokenFromMqttPayload(payload);
    return;
  }

  //if (flag == "getStatus") {
  // 组装设备状态 JSON 并按 flag 回复到约定主题
  if (flag.length() > 0) {
    String reply = mqttBuildStatusNormalJson(normalPowerText(), nowText());
    mqttPublishReplyByFlag(flag, reply);
  }
  
}

// Arduino 初始化入口：完成串口、摄像头、WiFi、HTTP 服务、MQTT 的一次性初始化
void setup() {
  // 初始化串口并开启调试输出
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // 填充摄像头硬件配置（引脚、时钟、图像参数、帧缓存策略）
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // 根据像素格式与是否存在 PSRAM，动态调整画质与缓存策略
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  // ESP-EYE 板型按键输入初始化
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // 初始化摄像头驱动
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    // 初始化失败直接返回，避免继续运行导致异常
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  // 获取传感器句柄并进行针对型号的参数微调
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  // JPEG 模式下默认将输出帧尺寸降到 QVGA，兼顾流畅度与带宽
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  // M5STACK 某些板型需要翻转与镜像修正
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  // S3-EYE 板型进行垂直翻转修正
  s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
  // 若定义了 LED 引脚，则初始化闪光灯功能
  setupLedFlash();
#endif

  // 初始化 SD 卡（1-bit 模式，释放 DAT1/DAT2，GPIO4 不再作为 SD 数据脚）
  bool sdReady = initSdCard1Bit();

  // 演示流程：开机自动录制 30 秒 MJPEG-AVI（10fps）。
  if (sdReady) {
    String recordPath = nextRecordPath();
    gRecordRequested = aviRecorderBegin(recordPath.c_str(), 320, 240, kRecordFps);
    if (gRecordRequested) {
      gCurrentRecordPath = recordPath;
      gRecordStartMs = millis();
    }
  }

  // 连接 WiFi，并关闭省电以降低网络延迟
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  // 阻塞等待网络连通
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected");

  // 启动摄像头 Web 服务并打印访问地址
  startCameraServer();
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  // 初始化 MQTT，并注册业务请求回调
  mqttInit(onFlagRequest);

  // 开机先触发一次 MQTT token 获取。
  gMqttTokenRequestPending = true;
  requestTokenViaMqtt(true);
  
  // 连接 MQTT 服务器并发布初始在线状态
  if (mqttIsConnected()) {
    mqttPublishStatusNormal(normalPowerText(), nowText());
  }
  
}

// Arduino 主循环：维护 MQTT 连接与周期性心跳上报
void loop() {
  // 记录上次心跳发送时间
  static uint32_t lastHeartbeat = 0;

  // 轮询 MQTT 客户端（处理收发与重连）
  mqttLoop();

  
  // 到达心跳周期后，上报设备在线状态
  uint32_t now = millis();
  if (now - lastHeartbeat >= MqttCfg::HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    mqttPublishStatusNormal(normalPowerText(), nowText());
  }

  if (gRecordRequested && aviRecorderIsRecording()) {
    aviRecorderTick();
    if (now - gRecordStartMs >= kRecordDurationMs) {
      bool stopped = aviRecorderStop();
      gRecordRequested = false;
      if (stopped && gCurrentRecordPath.length() > 0) {
        gUploadPending = true;
        gLastUploadTryMs = 0;
      }
    }
  }

  if (gUploadPending && WiFi.status() == WL_CONNECTED) {
    if (gLastUploadTryMs == 0 || (now - gLastUploadTryMs >= kUploadRetryIntervalMs)) {
      // 按需求：先拿到 MQTT token，再上传视频。
      if (String(kVideoUploadAuthorization).length() == 0) {
        if (gMqttTokenAwaiting && (now - gMqttTokenReqStartMs >= kMqttTokenWaitTimeoutMs)) {
          Serial.println("[TOKEN] mqtt wait timeout, retry");
          gMqttTokenAwaiting = false;
          gMqttTokenRequestPending = true;
        }

        if (!gMqttTokenReady) {
          if (gMqttTokenRequestPending && !gMqttTokenAwaiting) {
            requestTokenViaMqtt(false);
          }
        }

        if (!gMqttTokenReady) {
          // 发起 HTTP 前必须先拿到一次 token。
          if (!gMqttTokenAwaiting && !gMqttTokenRequestPending) {
            gMqttTokenRequestPending = true;
          }
          requestTokenViaMqtt(false);
          Serial.println("[UPLOAD] wait mqtt token before upload");
          delay(10);
          return;
        }
      }

      gLastUploadTryMs = now;
      VideoUploadResult result;
      String usedUrl;
      if (uploadRecordedVideoWithFallback(gCurrentRecordPath, &result, &usedUrl)) {
        Serial.print("[UPLOAD] used url=");
        Serial.println(usedUrl);
        Serial.print("[UPLOAD] status=");
        Serial.println(result.statusCode);
        if (result.response.length() > 0) {
          Serial.print("[UPLOAD] response=");
          Serial.println(result.response);
        }
        Serial.println("[UPLOAD] success");
        gUploadPending = false;
        // 下一次 HTTP 上传前重新走一次 MQTT token 获取。
        gMqttTokenReady = false;
      } else {
        Serial.print("[UPLOAD] used url=");
        Serial.println(usedUrl);
        Serial.print("[UPLOAD] status=");
        Serial.println(result.statusCode);
        if (result.response.length() > 0) {
          Serial.print("[UPLOAD] response=");
          Serial.println(result.response);
        }
        // 按需求：上传失败后触发 MQTT token 获取，并由重试逻辑持续拉取。
        gMqttTokenReady = false;
        gMqttTokenAwaiting = false;
        gMqttTokenRequestPending = true;
        requestTokenViaMqtt(true);
        Serial.println("[UPLOAD] failed, will retry");
      }
    }
  }

  // MQTT token 获取失败时持续重试。
  if (gMqttTokenRequestPending && WiFi.status() == WL_CONNECTED) {
    requestTokenViaMqtt(false);
  }

  kNormalPower_t++;
  // 小延时，避免空转占满 CPU
  
  delay(10);
}