#pragma once

#include <Arduino.h>

#include <functional>

bool isMicRecordingAvailable();

bool recordMicWavToSd(const String &path,
                      uint16_t seconds,
                      const std::function<void()> &backgroundTick,
                      String *error = nullptr,
                      uint32_t *bytesWritten = nullptr);
