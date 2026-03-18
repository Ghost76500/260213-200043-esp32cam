#include "avi_recorder.h"

#include <Arduino.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

namespace {

struct IdxEntry {
  uint32_t offset;
  uint32_t size;
};

struct RecorderState {
  File file;
  bool recording = false;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t fps = 0;
  uint32_t frameIntervalMs = 0;
  uint32_t nextFrameMs = 0;
  uint32_t frameCount = 0;
  uint32_t maxFrameSize = 0;
  uint32_t bytesPerSecEstimate = 0;

  uint32_t riffSizePos = 0;
  uint32_t hdrlSizePos = 0;
  uint32_t strlSizePos = 0;
  uint32_t moviListSizePos = 0;
  uint32_t avihDataPos = 0;
  uint32_t strhDataPos = 0;
  uint32_t strfDataPos = 0;
  uint32_t moviDataStart = 0;

  IdxEntry index[400];
  uint32_t indexCount = 0;
};

RecorderState g;

static void writeU16(File &f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
  f.write(b, sizeof(b));
}

static void writeU32(File &f, uint32_t v) {
  uint8_t b[4] = {
    (uint8_t)(v & 0xFF),
    (uint8_t)((v >> 8) & 0xFF),
    (uint8_t)((v >> 16) & 0xFF),
    (uint8_t)((v >> 24) & 0xFF),
  };
  f.write(b, sizeof(b));
}

static void writeFourCC(File &f, const char *cc) {
  f.write((const uint8_t *)cc, 4);
}

static void patchU32(File &f, uint32_t pos, uint32_t v) {
  size_t cur = f.position();
  f.seek(pos);
  writeU32(f, v);
  f.seek(cur);
}

static bool writeAviHeader() {
  File &f = g.file;

  writeFourCC(f, "RIFF");
  g.riffSizePos = f.position();
  writeU32(f, 0);
  writeFourCC(f, "AVI ");

  writeFourCC(f, "LIST");
  g.hdrlSizePos = f.position();
  writeU32(f, 0);
  writeFourCC(f, "hdrl");

  writeFourCC(f, "avih");
  writeU32(f, 56);
  g.avihDataPos = f.position();

  writeU32(f, 1000000UL / g.fps);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 0x10);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 1);
  writeU32(f, 0);
  writeU32(f, g.width);
  writeU32(f, g.height);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 0);

  writeFourCC(f, "LIST");
  g.strlSizePos = f.position();
  writeU32(f, 0);
  writeFourCC(f, "strl");

  writeFourCC(f, "strh");
  writeU32(f, 56);
  g.strhDataPos = f.position();

  writeFourCC(f, "vids");
  writeFourCC(f, "MJPG");
  writeU32(f, 0);
  writeU16(f, 0);
  writeU16(f, 0);
  writeU32(f, 0);
  writeU32(f, 1);
  writeU32(f, g.fps);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 0xFFFFFFFF);
  writeU32(f, 0);
  writeU16(f, 0);
  writeU16(f, 0);
  writeU16(f, g.width);
  writeU16(f, g.height);

  writeFourCC(f, "strf");
  writeU32(f, 40);
  g.strfDataPos = f.position();

  writeU32(f, 40);
  writeU32(f, g.width);
  writeU32(f, g.height);
  writeU16(f, 1);
  writeU16(f, 24);
  writeFourCC(f, "MJPG");
  writeU32(f, g.width * g.height * 3UL);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 0);
  writeU32(f, 0);

  uint32_t hdrlEnd = f.position();
  patchU32(f, g.strlSizePos, hdrlEnd - (g.strlSizePos + 4));
  patchU32(f, g.hdrlSizePos, hdrlEnd - (g.hdrlSizePos + 4));

  writeFourCC(f, "LIST");
  g.moviListSizePos = f.position();
  writeU32(f, 0);
  writeFourCC(f, "movi");
  g.moviDataStart = f.position();

  return true;
}

static void captureOneFrame() {
  if (!g.recording) return;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[AVI] frame capture failed");
    return;
  }

  if (fb->format != PIXFORMAT_JPEG) {
    esp_camera_fb_return(fb);
    Serial.println("[AVI] non-JPEG frame, dropped");
    return;
  }

  const uint32_t chunkStart = g.file.position();
  writeFourCC(g.file, "00dc");
  writeU32(g.file, fb->len);
  g.file.write(fb->buf, fb->len);
  if (fb->len & 1U) {
    g.file.write((uint8_t)0);
  }

  if (g.indexCount < (sizeof(g.index) / sizeof(g.index[0]))) {
    g.index[g.indexCount].offset = chunkStart - g.moviDataStart;
    g.index[g.indexCount].size = fb->len;
    g.indexCount++;
  }

  g.frameCount++;
  if (fb->len > g.maxFrameSize) {
    g.maxFrameSize = fb->len;
  }

  esp_camera_fb_return(fb);
}

static void writeIdx1() {
  writeFourCC(g.file, "idx1");
  writeU32(g.file, g.indexCount * 16);

  for (uint32_t i = 0; i < g.indexCount; ++i) {
    writeFourCC(g.file, "00dc");
    writeU32(g.file, 0x10);
    writeU32(g.file, g.index[i].offset);
    writeU32(g.file, g.index[i].size);
  }
}

static void finalizeHeader() {
  uint32_t fileSize = g.file.size();

  uint32_t moviListSize = fileSize - (g.moviListSizePos + 4);
  patchU32(g.file, g.moviListSizePos, moviListSize);
  patchU32(g.file, g.riffSizePos, fileSize - 8);

  g.bytesPerSecEstimate = (g.frameCount > 0) ? ((fileSize * g.fps) / g.frameCount) : 0;

  patchU32(g.file, g.avihDataPos + 4, g.bytesPerSecEstimate);
  patchU32(g.file, g.avihDataPos + 16, g.frameCount);
  patchU32(g.file, g.avihDataPos + 28, g.maxFrameSize);

  patchU32(g.file, g.strhDataPos + 32, g.frameCount);
  patchU32(g.file, g.strhDataPos + 36, g.maxFrameSize);

  patchU32(g.file, g.strfDataPos + 20, g.maxFrameSize);
}

}  // namespace

bool aviRecorderBegin(const char *path, uint16_t width, uint16_t height, uint16_t fps) {
  if (g.recording) {
    Serial.println("[AVI] already recording");
    return false;
  }
  if (fps == 0) {
    Serial.println("[AVI] invalid fps");
    return false;
  }

  g = RecorderState();
  g.width = width;
  g.height = height;
  g.fps = fps;
  g.frameIntervalMs = 1000UL / fps;
  g.nextFrameMs = millis();

  g.file = SD_MMC.open(path, FILE_WRITE);
  if (!g.file) {
    Serial.print("[AVI] open failed: ");
    Serial.println(path);
    return false;
  }

  if (!writeAviHeader()) {
    g.file.close();
    return false;
  }

  g.recording = true;
  Serial.print("[AVI] recording started: ");
  Serial.println(path);
  return true;
}

void aviRecorderTick() {
  if (!g.recording) return;

  uint32_t now = millis();
  if ((int32_t)(now - g.nextFrameMs) < 0) {
    return;
  }

  captureOneFrame();
  g.nextFrameMs += g.frameIntervalMs;

  // 防止系统卡顿后一次追帧过多，保持实时节奏。
  if ((int32_t)(now - g.nextFrameMs) > (int32_t)(g.frameIntervalMs * 5UL)) {
    g.nextFrameMs = now + g.frameIntervalMs;
  }
}

bool aviRecorderStop() {
  if (!g.recording) return true;

  writeIdx1();
  finalizeHeader();
  g.file.flush();
  g.file.close();

  g.recording = false;
  Serial.print("[AVI] recording stopped, frames=");
  Serial.print(g.frameCount);
  Serial.print(" maxFrame=");
  Serial.print(g.maxFrameSize);
  Serial.println("B");
  return true;
}

bool aviRecorderIsRecording() {
  return g.recording;
}

uint32_t aviRecorderFrameCount() {
  return g.frameCount;
}
