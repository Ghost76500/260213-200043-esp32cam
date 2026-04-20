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
#include "app_runtime_utils.h"
#include "datapack_receive.h"

//uint8_t kNormalPower_t = 98;

static TokenManager gTokenManager;
static UploadManager gUploadManager;
static RecorderManager gRecorderManager;
static bool gUploadStartedFromCurrentRecording = false;
static bool gRecorderUploadFlowEnabled = false;
static bool gSdReady = false;
static constexpr uint8_t kWarningHongwaiCode = 0xAA;
static constexpr uint8_t kWarningQingxieCode = 0xBB;
static constexpr uint32_t kRecordTriggerCooldownMs = 60UL * 1000UL;
static uint32_t gLastRecordTriggerMs = 0;
static bool gLastWarningActive = false;

// 启动摄像头 HTTP 服务（在 app_httpd.cpp 中实现）
void startCameraServer();
// 初始化补光灯控制（在 app_httpd.cpp 中实现，具体取决于板级定义）
void setupLedFlash();
void gRecorder_Upload(void); // 视频上传函数声明，供 RecorderManager 在录制完成后调用以触发上传流程

static bool tryEnableRecorderUploadFlow(const char *source, uint32_t nowMs, bool bypassCooldown = false) {
  if (!bypassCooldown && gLastRecordTriggerMs != 0 && (nowMs - gLastRecordTriggerMs < kRecordTriggerCooldownMs)) {
    Serial.print("[REC] trigger ignored (cooldown), source=");
    Serial.println(source);
    return false;
  }

  if (gRecorderUploadFlowEnabled || gRecorderManager.isRecording() || gUploadManager.isPending()) {
    Serial.print("[REC] trigger ignored (busy), source=");
    Serial.println(source);
    return false;
  }

  gRecorderUploadFlowEnabled = true;
  gLastRecordTriggerMs = nowMs;
  Serial.print("[REC] trigger accepted, source=");
  Serial.println(source);
  return true;
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
    String reply = mqttBuildStatusNormalJson(appNormalPowerText(kNormalPower_t), appNowText());
    mqttPublishReplyByFlag(flag, reply);
  }
}

// Arduino 初始化入口：完成串口、摄像头、WiFi、HTTP 服务、MQTT 的一次性初始化
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  dataPackInitUart();
  Serial.println("[PACK] UART2 init: RX=13, TX=4");

  gTokenManager.init();
  gUploadManager.init();
  gRecorderManager.init();

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
  //tryEnableRecorderUploadFlow("boot", millis(), true); // 开机后进行一次录制触发，验证流程并获取初始视频以供后续上传测试使用

  if (mqttIsConnected()) {
    mqttPublishStatusNormal(appNormalPowerText(kNormalPower_t), appNowText());
  }
}

// Arduino 主循环：维护 MQTT 连接与业务状态机
void loop() {
  // 记录上次心跳上报时间
  static uint32_t lastHeartbeat = 0;

  // 串口解包轮询：收到完整帧后置位，供主循环读取。
  dataPackPoll();
  if (dataPackTakeUpdatedFlag()) {
    uint8_t warningCode = dataPackLatestWarningCode();
    bool warningActive = (warningCode == kWarningHongwaiCode || warningCode == kWarningQingxieCode);

    Serial.print("[PACK] rx warning=0x");
    Serial.println(warningCode, HEX);

    // 告警码(0xAA/0xBB)在告警态可触发，实际频率由冷却时间限制。
    if (warningActive) {
      tryEnableRecorderUploadFlow("pack-alarm-level", millis());
    } else if (gLastWarningActive) {
      Serial.println("[PACK] alarm cleared");
    } else {
      Serial.println("[PACK] non-alarm packet ignored");
    }

    gLastWarningActive = warningActive;
  }

  // 轮询 MQTT：处理收发并在断开时尝试重连
  mqttLoop();

  // 周期性上报设备状态（心跳）
  uint32_t now = millis();
  if (now - lastHeartbeat >= MqttCfg::HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    mqttPublishStatusNormal(appNormalPowerText(kNormalPower_t), appNowText());
  }

  gRecorder_Upload();

  delay(10);
}

void gRecorder_Upload(void)
{
  if (!gRecorderUploadFlowEnabled) {
    return;
  }

  if (!gSdReady) {
    gSdReady = appInitSdCard1Bit();
    if (!gSdReady) {
      Serial.println("[REC] wait SD ready");
      return;
    }
  }

  uint32_t now = millis();

  // 触发后，若当前为空闲态则启动新一轮录制。
  if (!gRecorderManager.isRecording() && !gRecorderManager.isFinished() && !gUploadManager.isPending()) {
    String recordPath = appNextRecordPath();
    if (gRecorderManager.start(recordPath)) {
      gUploadStartedFromCurrentRecording = false;
      Serial.print("[REC] started: ");
      Serial.println(recordPath);
      // 录制刚启动后刷新一次时间戳，避免使用启动前的旧 now 触发错误超时判断。
      now = millis();
    } else {
      Serial.println("[REC] start failed");
      return;
    }
  }

  // 驱动录制状态机：按帧写入并在到达时长后停止
  gRecorderManager.tick(now);

  // 录制完成后仅触发一次上传任务
  if (gRecorderManager.isFinished() && !gUploadStartedFromCurrentRecording) {
    gUploadManager.start(gRecorderManager.filePath());
    gUploadStartedFromCurrentRecording = true;
  }

  // token 不可用时持续发起/重试 MQTT token 请求
  if (!gTokenManager.hasToken()) {
    gTokenManager.requestToken(false);
  }

  // 有待上传任务且网络可用时，执行上传流程
  if (gUploadManager.isPending() && WiFi.status() == WL_CONNECTED) {
    // 若未配置静态鉴权头，则必须先拿到动态 token 才允许上传
    if (String(AppCfg::Upload::STATIC_AUTH_HEADER).length() == 0 && !gTokenManager.hasToken()) {
      static uint32_t lastWaitTokenLogMs = 0;
      if (now - lastWaitTokenLogMs >= 2000UL) {
        Serial.println("[UPLOAD] wait mqtt token before upload");
        lastWaitTokenLogMs = now;
      }
      return;
    }

    // UploadManager 按重试间隔决定是否真正发起一次上传
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
        gRecorderUploadFlowEnabled = false;
        gRecorderManager.init();
        gUploadStartedFromCurrentRecording = false;
      } else {
        // 上传失败后强制刷新 token，供下次重试使用
        gTokenManager.requestToken(true);
        Serial.println("[UPLOAD] failed, will retry");
      }
    }
  }
}