#pragma once

#include <Arduino.h>

struct UploadManagerResult {
  bool attempted;
  bool success;
  int statusCode;
  String response;
  String usedUrl;
};

class UploadManager {
 public:
  void init();
  void start(const String &filePath);
  bool isPending() const;
  bool process(uint32_t nowMs, const String &authHeader, UploadManagerResult *outResult);

 private:
  bool uploadWithFallback(const String &authHeader, UploadManagerResult *outResult);

  bool pending_ = false;
  uint32_t lastTryMs_ = 0;
  String filePath_;
};
