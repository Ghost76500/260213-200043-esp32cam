#pragma once

#include <Arduino.h>

bool aviRecorderBegin(const char *path, uint16_t width, uint16_t height, uint16_t fps);
void aviRecorderTick();
bool aviRecorderStop();
bool aviRecorderIsRecording();
uint32_t aviRecorderFrameCount();
