#include "audio_recorder.h"

#include <SD.h>

#include <algorithm>

#include "user_config.h"

namespace {

constexpr uint16_t kWavHeaderBytes = 44;

uint32_t sampleRateHz() {
  const uint32_t configured = static_cast<uint32_t>(USER_MIC_SAMPLE_RATE);
  return std::max<uint32_t>(4000U, std::min<uint32_t>(configured, 22050U));
}

void writeLe16(uint8_t *out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFU);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void writeLe32(uint8_t *out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFU);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

bool writeWavHeader(File &file, uint32_t sampleRate, uint32_t dataBytes) {
  uint8_t header[kWavHeaderBytes] = {0};

  const uint16_t channels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate = sampleRate * static_cast<uint32_t>(channels) *
                            (static_cast<uint32_t>(bitsPerSample) / 8U);
  const uint16_t blockAlign =
      static_cast<uint16_t>(channels * (bitsPerSample / 8U));
  const uint32_t riffSize = 36U + dataBytes;

  // RIFF chunk
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  writeLe32(header + 4, riffSize);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';

  // fmt subchunk
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  writeLe32(header + 16, 16);  // PCM fmt size
  writeLe16(header + 20, 1);   // PCM format
  writeLe16(header + 22, channels);
  writeLe32(header + 24, sampleRate);
  writeLe32(header + 28, byteRate);
  writeLe16(header + 32, blockAlign);
  writeLe16(header + 34, bitsPerSample);

  // data subchunk
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  writeLe32(header + 40, dataBytes);

  if (!file.seek(0)) {
    return false;
  }
  return file.write(header, sizeof(header)) == sizeof(header);
}

void setError(String *error, const String &value) {
  if (error) {
    *error = value;
  }
}

}  // namespace

bool isMicRecordingAvailable() {
  return USER_MIC_ADC_PIN >= 0;
}

bool recordMicWavToSd(const String &path,
                      uint16_t seconds,
                      const std::function<void()> &backgroundTick,
                      String *error,
                      uint32_t *bytesWritten) {
  if (!isMicRecordingAvailable()) {
    setError(error, "MIC pin is not configured");
    return false;
  }

  if (path.isEmpty() || !path.startsWith("/")) {
    setError(error, "Invalid file path");
    return false;
  }

  if (seconds == 0) {
    setError(error, "Recording time must be > 0 sec");
    return false;
  }

  const uint16_t maxSeconds = static_cast<uint16_t>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_MAX_SECONDS)));
  if (seconds > maxSeconds) {
    setError(error, "Recording time exceeds limit");
    return false;
  }

  const uint32_t sampleRate = sampleRateHz();
  const uint32_t totalSamples = sampleRate * static_cast<uint32_t>(seconds);
  const uint32_t dataBytes = totalSamples * 2U;

  if (SD.exists(path.c_str())) {
    SD.remove(path.c_str());
  }

  File file = SD.open(path.c_str(), FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    setError(error, "Failed to create voice file");
    return false;
  }

  uint8_t blankHeader[kWavHeaderBytes] = {0};
  if (file.write(blankHeader, sizeof(blankHeader)) != sizeof(blankHeader)) {
    file.close();
    SD.remove(path.c_str());
    setError(error, "Failed to write WAV header");
    return false;
  }

#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(12);
#if defined(ADC_11db)
  analogSetPinAttenuation(USER_MIC_ADC_PIN, ADC_11db);
#endif
#endif
  pinMode(USER_MIC_ADC_PIN, INPUT);

  const uint32_t sampleIntervalUs = 1000000UL / sampleRate;
  uint32_t nextSampleUs = micros();
  int32_t dcTrackQ8 = 0;
  const uint16_t tickStride = 192;

  for (uint32_t i = 0; i < totalSamples; ++i) {
    const int raw = analogRead(USER_MIC_ADC_PIN);
    int32_t centered = static_cast<int32_t>(raw) - 2048;
    centered <<= 4;

    const int32_t sampleQ8 = centered << 8;
    dcTrackQ8 += (sampleQ8 - dcTrackQ8) / 64;
    int32_t hp = centered - (dcTrackQ8 >> 8);
    hp = std::max<int32_t>(-32768, std::min<int32_t>(32767, hp));

    const int16_t sample = static_cast<int16_t>(hp);
    if (file.write(reinterpret_cast<const uint8_t *>(&sample), sizeof(sample)) !=
        sizeof(sample)) {
      file.close();
      SD.remove(path.c_str());
      setError(error, "Failed to write voice sample");
      return false;
    }

    if (backgroundTick && ((i % tickStride) == 0U)) {
      backgroundTick();
    }

    nextSampleUs += sampleIntervalUs;
    const int32_t waitUs = static_cast<int32_t>(nextSampleUs - micros());
    if (waitUs > 0) {
      delayMicroseconds(static_cast<uint32_t>(waitUs));
    } else if (waitUs < -2000000) {
      nextSampleUs = micros();
    }
  }

  if (!writeWavHeader(file, sampleRate, dataBytes)) {
    file.close();
    SD.remove(path.c_str());
    setError(error, "Failed to finalize WAV header");
    return false;
  }

  file.flush();
  file.close();

  if (bytesWritten) {
    *bytesWritten = dataBytes + kWavHeaderBytes;
  }
  setError(error, "");
  return true;
}
