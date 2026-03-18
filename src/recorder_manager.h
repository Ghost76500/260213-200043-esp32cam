#pragma once

#include <Arduino.h>

class RecorderManager {
 public:
  bool start(const String &path);
  void tick(uint32_t nowMs);
  bool stop();
  bool isFinished() const;
  bool isRecording() const;
  const String &filePath() const;

 private:
  bool active_ = false;
  bool finished_ = false;
  uint32_t startMs_ = 0;
  String path_;
};
