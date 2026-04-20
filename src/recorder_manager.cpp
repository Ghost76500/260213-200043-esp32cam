#include "recorder_manager.h"

#include "app_config.h"
#include "avi_recorder.h"

void RecorderManager::init() {
  active_ = false;
  finished_ = false;
  startMs_ = 0;
  path_ = "";
}

bool RecorderManager::start(const String &path) {
  if (path.length() == 0) {
    return false;
  }

  bool ok = aviRecorderBegin(path.c_str(), AppCfg::Record::WIDTH, AppCfg::Record::HEIGHT, AppCfg::Record::FPS);
  if (!ok) {
    return false;
  }

  path_ = path;
  active_ = true;
  finished_ = false;
  startMs_ = millis();
  return true;
}

void RecorderManager::tick(uint32_t nowMs) {
  if (!active_) {
    return;
  }

  int32_t elapsed = (int32_t)(nowMs - startMs_);
  if (elapsed < 0) {
    return;
  }

  if (aviRecorderIsRecording()) {
    aviRecorderTick();
  }

  if ((uint32_t)elapsed >= AppCfg::Record::DURATION_MS) {
    stop();
  }
}

bool RecorderManager::stop() {
  if (!active_) {
    return true;
  }

  bool ok = aviRecorderStop();
  active_ = false;
  finished_ = ok;
  return ok;
}

bool RecorderManager::isFinished() const {
  return finished_;
}

bool RecorderManager::isRecording() const {
  return active_;
}

const String &RecorderManager::filePath() const {
  return path_;
}
