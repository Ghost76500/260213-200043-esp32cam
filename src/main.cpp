#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

#include "app_config.h"
#include "board_config.h"
#include "mqtt_config.h"
#include "mqtt_service.h"
#include "token_manager.h"
#include "upload_manager.h"
#include "recorder_manager.h"

#include "FS.h"
#include "SD_MMC.h"

uint8_t kNormalPower_t = 0;

static TokenManager gTokenManager;
static UploadManager gUploadManager;
static RecorderManager gRecorderManager;
static bool gUploadStartedFromCurrentRecording = false;

// 启动摄像头 HTTP 服务（在 app_httpd.cpp 中实现）
void startCameraServer();
// 初始化补光灯控制（在 app_httpd.cpp 中实现，具体取决于板级定义）
void setupLedFlash();

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

static bool initSdCard1Bit() {
  SD_MMC.end();
  delay(20);

  Serial.println("[SD] init start (SD_MMC 1-bit mode)");

  const bool mode1bit = true;
  bool ok = SD_MMC.begin("/sdcard", mode1bit);
  if (!ok) {
    Serial.println("[SD] mount failed (1-bit)");
    Serial.println("[SD] check card insert, card format(FAT32/exFAT), and pull-up resistors.");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] no card (1-bit)");
    SD_MMC.end();
    return false;
  }

  uint64_t cardSizeMB = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  Serial.print("[SD] mount ok, mode=1-bit type=");
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

// 生成当前运行时间字符串（以固定日期 + 运行时分秒形式返回）
static String nowText() {
  uint32_t sec = millis() / 1000;
  uint32_t hh = (sec / 3600) % 24;
  uint32_t mm = (sec / 60) % 60;
  uint32_t ss = sec % 60;

  char buf[32];
  snprintf(buf, sizeof(buf), "2026-02-30 %02lu:%02lu:%02lu", (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}

// MQTT 业务标记请求回调：收到带 flag 的请求后，构造并发布对应回复
static void onFlagRequest(const String &flag, const String &topic, const String &payload) {
  Serial.print("[APP] flag request, topic=");
  Serial.print(topic);
  Serial.print(" flag=");
  Serial.print(flag);
  Serial.print(" payload=");
  Serial.println(payload);

  if (flag.length() > 0) {
    String reply = mqttBuildStatusNormalJson(normalPowerText(), nowText());
    mqttPublishReplyByFlag(flag, reply);
  }
}

// Arduino 初始化入口：完成串口、摄像头、WiFi、HTTP 服务、MQTT 的一次性初始化
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

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
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  bool sdReady = initSdCard1Bit();
  if (sdReady) {
    String recordPath = nextRecordPath();
    if (gRecorderManager.start(recordPath)) {
      gUploadStartedFromCurrentRecording = false;
    }
  }

  WiFi.begin(AppCfg::Wifi::SSID, AppCfg::Wifi::PASSWORD);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected");

  startCameraServer();
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  mqttInit(onFlagRequest);
  gTokenManager.requestToken(true);

  if (mqttIsConnected()) {
    mqttPublishStatusNormal(normalPowerText(), nowText());
  }
}

// Arduino 主循环：维护 MQTT 连接与业务状态机
void loop() {
  static uint32_t lastHeartbeat = 0;

  mqttLoop();

  uint32_t now = millis();
  if (now - lastHeartbeat >= MqttCfg::HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    mqttPublishStatusNormal(normalPowerText(), nowText());
  }

  gRecorderManager.tick(now);
  if (gRecorderManager.isFinished() && !gUploadStartedFromCurrentRecording) {
    gUploadManager.start(gRecorderManager.filePath());
    gUploadStartedFromCurrentRecording = true;
  }

  if (!gTokenManager.hasToken()) {
    gTokenManager.requestToken(false);
  }

  if (gUploadManager.isPending() && WiFi.status() == WL_CONNECTED) {
    if (String(AppCfg::Upload::STATIC_AUTH_HEADER).length() == 0 && !gTokenManager.hasToken()) {
      Serial.println("[UPLOAD] wait mqtt token before upload");
      delay(10);
      return;
    }

    UploadManagerResult result;
    bool attempted = gUploadManager.process(now, gTokenManager.getAuthHeader(), &result);
    if (attempted) {
      Serial.print("[UPLOAD] used url=");
      Serial.println(result.usedUrl);
      Serial.print("[UPLOAD] status=");
      Serial.println(result.statusCode);
      if (result.response.length() > 0) {
        Serial.print("[UPLOAD] response=");
        Serial.println(result.response);
      }

      if (result.success) {
        Serial.println("[UPLOAD] success");
      } else {
        gTokenManager.requestToken(true);
        Serial.println("[UPLOAD] failed, will retry");
      }
    }
  }

  //kNormalPower_t++;
  delay(10);
}
