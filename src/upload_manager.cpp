#include "upload_manager.h"

#include "app_config.h"
#include "mqtt_config.h"
#include "video_uploader.h"

void UploadManager::start(const String &filePath) {
  filePath_ = filePath;
  pending_ = filePath_.length() > 0;
  lastTryMs_ = 0;
}

bool UploadManager::isPending() const {
  return pending_;
}

bool UploadManager::uploadWithFallback(const String &authHeader, UploadManagerResult *outResult) {
  UploadManagerResult result = {true, false, -1, "", ""};

  const char *urls[] = {AppCfg::Upload::PRIMARY_URL, AppCfg::Upload::FALLBACK_URL};
  const size_t count = sizeof(urls) / sizeof(urls[0]);

  for (size_t i = 0; i < count; ++i) {
    const char *url = urls[i];
    if (!url || strlen(url) == 0) {
      continue;
    }

    const char *auth = AppCfg::Upload::STATIC_AUTH_HEADER;
    if (authHeader.length() > 0) {
      auth = authHeader.c_str();
    }

    VideoUploadOptions options = {
      .authorizationHeader = auth,
      .cookieHeader = AppCfg::Upload::COOKIE_HEADER,
    };

    VideoUploadResult r;
    bool ok = videoUploaderUploadAviMultipart(url, MqttCfg::DEVICE_ID, filePath_.c_str(), &options, &r);
    result.usedUrl = String(url);
    result.statusCode = r.statusCode;
    result.response = r.response;
    result.success = ok;

    if (ok) {
      if (outResult) {
        *outResult = result;
      }
      return true;
    }

    if (r.statusCode > 0) {
      break;
    }
  }

  if (outResult) {
    *outResult = result;
  }
  return false;
}

bool UploadManager::process(uint32_t nowMs, const String &authHeader, UploadManagerResult *outResult) {
  if (outResult) {
    *outResult = {false, false, -1, "", ""};
  }

  if (!pending_) {
    return false;
  }

  if (lastTryMs_ != 0 && (nowMs - lastTryMs_ < AppCfg::Upload::RETRY_INTERVAL_MS)) {
    return false;
  }

  lastTryMs_ = nowMs;
  UploadManagerResult result;
  bool ok = uploadWithFallback(authHeader, &result);
  if (outResult) {
    *outResult = result;
  }

  if (ok) {
    pending_ = false;
  }

  return true;
}
