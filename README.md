# ESP32-CAM 录制上传项目（模块化版）

本项目基于 PlatformIO + Arduino 框架，运行在 AI Thinker ESP32-CAM。

当前主功能链路：
1. 初始化摄像头与 SD 卡（1-bit 模式）
2. 录制 MJPEG-AVI 到 SD 卡
3. 通过 MQTT 动态 topic 请求 token
4. 将 token 写入 HTTP 请求头
5. 上传 AVI 到服务端（主/备 URL + 重试）
6. 定时 MQTT 状态上报（心跳）
7. 响应平台下行请求（按 flag 回应）

## 功能总览

- 摄像头初始化与局域网预览（本地 HTTP 服务）
- SD 卡挂载（1-bit）与 AVI 文件落盘
- 录制生命周期管理（开始/轮询/停止/完成判定）
- MQTT 连接维护与自动重连
- 定时状态上报（topic：`column/{deviceId}/to-soft`）
- 下行请求响应（收到 `flag` 后回发到 `column/{deviceId}/to-soft/{flag}`）
- 动态 token 请求（空 payload，请求/响应 topic 动态构造）
- HTTP multipart 上传（`id` + `video`），支持主备 URL 与重试

## 目录结构（核心模块）

- `src/main.cpp`：系统编排层（setup/loop），只做流程调度
- `src/app_config.h`：业务配置常量（WiFi、录制参数、上传参数、token 参数）
- `src/recorder_manager.h/.cpp`：录制生命周期管理（start/tick/stop/isFinished）
- `src/avi_recorder.h/.cpp`：AVI 文件写入实现（RIFF、idx1、头回填）
- `src/token_manager.h/.cpp`：token 获取管理（动态 flag、topic 构造、请求发送、解析、超时重试）
- `src/upload_manager.h/.cpp`：上传管理（重试节奏、主/备 URL 策略）
- `src/video_uploader.h/.cpp`：HTTP multipart 上传实现
- `src/mqtt_service.h/.cpp`：MQTT 基础连接、订阅、发布、原始消息回调

## 程序流程

### setup 阶段

1. 初始化串口和摄像头参数
2. 初始化 SD 卡（仅 1-bit）
3. 若 SD 成功，启动录制
4. 连接 WiFi，启动本地摄像头 HTTP 服务
5. 初始化 MQTT
6. 触发一次 token 请求

### loop 阶段

1. 调用 `mqttLoop()` 维持连接
2. 周期上报设备状态（MQTT 心跳）
3. 调用 `RecorderManager::tick(now)` 驱动录制
4. 录制结束后启动上传任务
5. 若 token 未就绪，持续请求 token
6. token 就绪后执行上传
7. 上传失败触发强制 token 刷新并等待下次重试

## MQTT 通信说明

### 1) 定时状态上报

- 上报主题：`column/{deviceId}/to-soft`
- 上报内容（示例）：
  `{"id":"1111","status":"正常","power":"88","updataTime":"2026-02-30 00:01:23"}`
- 触发方式：按 `mqtt_config.h` 中 `HEARTBEAT_INTERVAL_MS` 周期发送

### 2) 下行请求回应（flag 机制）

- 设备订阅主题：`column/{deviceId}/to-hard`
- 收到 payload 且包含 `flag` 字段时，设备构造响应并回发：
  `column/{deviceId}/to-soft/{flag}`
- 该能力由 `main.cpp` 的 `onFlagRequest` + `mqtt_service` 共同实现

### 3) token 获取（动态 topic）

- 设备生成动态 flag：`{deviceId}_{chipId}_{timestamp}`（不含 `/`）
- 请求主题：`column/{flag}/token-to-soft`，payload 为空字符串
- 响应主题：`column/{flag}/token-to-hard`
- 响应 payload 预期：
  `{"Authorization":"Bearer ..."}`
- 解析成功后写入上传请求头 Authorization

## 模块使用方式

### TokenManager

对外仅暴露三个方法：

- `bool hasToken() const`：当前是否已拿到可用 token
- `const String& getAuthHeader() const`：获取完整 Authorization 头值
- `bool requestToken(bool forceNow = false)`：请求 token（支持重试/超时）

说明：
- token 请求使用动态 flag（由设备号 + 芯片 ID + 时间戳构造）
- 请求 topic：`column/{flag}/token-to-soft`
- 回复 topic：`column/{flag}/token-to-hard`
- 请求 payload：空字符串
- 响应 payload：期望 JSON 中有 `Authorization` 键，例如：
  `{"Authorization":"Bearer xxx"}`

### UploadManager

对外方法：

- `start(filePath)`：设置待上传文件
- `isPending()`：是否仍有上传任务
- `process(nowMs, authHeader, &result)`：按重试间隔尝试上传一次

特点：
- 内部封装主/备 URL 策略
- 若主 URL 网络失败会尝试备 URL
- 对业务层只依赖 `authHeader + filePath`

### RecorderManager

对外方法：

- `start(path)`：启动录制
- `tick(nowMs)`：循环驱动录制与到时停止
- `stop()`：手动停止
- `isFinished()`：是否已完成本轮录制
- `isRecording()`：是否正在录制
- `filePath()`：返回当前录制文件路径

## 配置方式

所有业务常量在 `src/app_config.h`：

- `AppCfg::Wifi`：WiFi SSID 和密码
- `AppCfg::Record`：录制宽高、FPS、录制时长
- `AppCfg::Upload`：主备上传 URL、静态请求头参数
- `AppCfg::Token`：token 请求重试间隔、等待超时、topic 前后缀

如需环境切换（测试/正式），建议扩展为：
- `app_config_dev.h`
- `app_config_prod.h`
- 在 `app_config.h` 中统一选择

## 构建与烧录

在项目根目录执行：

```bash
platformio run
platformio run --target upload
platformio device monitor
```

## 运行日志关键字

- SD：`[SD] ...`
- 录制：`[AVI] recording started/stopped ...`
- token：`[TOKEN] ...`
- 上传：`[UPLOAD] ...`
- MQTT：`[MQTT] ...`
- 心跳上报：`[MQTT] TX column/{deviceId}/to-soft ...`
- 请求应答：`[APP] flag request ...` 与 `[MQTT] TX column/{deviceId}/to-soft/{flag} ...`

可用成功判据：
- `[TOKEN] mqtt token updated`
- `[UPLOAD] status=200`
- `response` 中返回 `success`

## 当前已知说明

- SD 仅启用 1-bit 模式
- `nowText()` 使用运行时时分秒拼接固定日期文本（非 NTP 时间）
- token 当前走 MQTT 动态 topic 流程，不走 HTTP 获取
