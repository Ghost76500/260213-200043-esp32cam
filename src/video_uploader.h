#pragma once

#include <Arduino.h>

struct VideoUploadResult {
  bool ok;
  int statusCode;
  String response;
};

struct VideoUploadOptions {
  const char *authorizationHeader;
  const char *cookieHeader;
};

bool videoUploaderUploadAviMultipart(
  const char *url,
  const char *deviceId,
  const char *aviPath,
  const VideoUploadOptions *options,
  VideoUploadResult *outResult
);

bool videoUploaderUploadAviMultipart(const char *url, const char *deviceId, const char *aviPath, VideoUploadResult *outResult);
