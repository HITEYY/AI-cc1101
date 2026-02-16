#include "audio_recorder.h"

#include <SD.h>

#include <algorithm>

#if defined(ARDUINO_ARCH_ESP32)
#include <driver/i2s.h>
#include <esp_err.h>
#if __has_include(<driver/i2s_pdm.h>)
#include <driver/i2s_pdm.h>
#define AUDIO_RECORDER_HAS_I2S_PDM_CHANNEL 1
#else
#define AUDIO_RECORDER_HAS_I2S_PDM_CHANNEL 0
#endif
#endif

#include "user_config.h"
#include "board_pins.h"

namespace {

constexpr uint16_t kWavHeaderBytes = 44;
bool gPdmI2sInstalled = false;

uint32_t sampleRateHz() {
  const uint32_t configured = static_cast<uint32_t>(USER_MIC_SAMPLE_RATE);
  return std::max<uint32_t>(4000U, std::min<uint32_t>(configured, 22050U));
}

uint32_t pdmSampleRateHz(uint32_t configured) {
  // T-Embed onboard PDM MIC is stable at 16kHz or higher.
  return std::max<uint32_t>(16000U, std::min<uint32_t>(configured, 22050U));
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

#if defined(ARDUINO_ARCH_ESP32)
String formatEspErr(const char *prefix, esp_err_t err) {
  String msg(prefix);
  msg += ": ";
  msg += esp_err_to_name(err);
  return msg;
}
#endif

bool hasAdcMicConfigured() {
  return USER_MIC_ADC_PIN >= 0;
}

bool hasPdmMicConfigured() {
#if defined(USER_MIC_PDM_DATA_PIN) && defined(USER_MIC_PDM_CLK_PIN)
  return USER_MIC_PDM_DATA_PIN >= 0 && USER_MIC_PDM_CLK_PIN >= 0;
#else
  return false;
#endif
}

bool hasPdmPinConflict(String *detail) {
#if defined(USER_MIC_PDM_DATA_PIN) && defined(USER_MIC_PDM_CLK_PIN) && \
    defined(USER_NFC_RESET_PIN) && defined(USER_NFC_IRQ_PIN)
  if (USER_MIC_PDM_DATA_PIN == USER_NFC_RESET_PIN ||
      USER_MIC_PDM_CLK_PIN == USER_NFC_RESET_PIN) {
    if (detail) {
      *detail = "NFC reset pin conflicts with MIC PDM pin";
    }
    return true;
  }
  if (USER_MIC_PDM_DATA_PIN == USER_NFC_IRQ_PIN ||
      USER_MIC_PDM_CLK_PIN == USER_NFC_IRQ_PIN) {
    if (detail) {
      *detail = "NFC IRQ pin conflicts with MIC PDM pin";
    }
    return true;
  }
#else
  (void)detail;
#endif
  if (detail) {
    *detail = "";
  }
  return false;
}

bool captureAdcSamples(File &file,
                       uint32_t totalSamples,
                       uint32_t sampleRate,
                       const std::function<void()> &backgroundTick,
                       const std::function<bool()> &stopRequested,
                       uint32_t *samplesWritten,
                       String *error) {
  if (samplesWritten) {
    *samplesWritten = 0;
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
  uint32_t writtenSamples = 0;

  for (uint32_t i = 0; i < totalSamples; ++i) {
    if (stopRequested && stopRequested()) {
      break;
    }

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
      setError(error, "Failed to write voice sample");
      return false;
    }
    ++writtenSamples;

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

  if (samplesWritten) {
    *samplesWritten = writtenSamples;
  }
  return true;
}

#if defined(ARDUINO_ARCH_ESP32)
#if AUDIO_RECORDER_HAS_I2S_PDM_CHANNEL && SOC_I2S_SUPPORTS_PDM_RX
bool capturePdmSamplesWithChannelApi(File &file,
                                     uint32_t targetDataBytes,
                                     uint32_t sampleRate,
                                     const std::function<void()> &backgroundTick,
                                     const std::function<bool()> &stopRequested,
                                     uint32_t *dataBytesWritten,
                                     String *error) {
  if (dataBytesWritten) {
    *dataBytesWritten = 0;
  }

  struct PdmRoute {
    gpio_num_t clkPin = GPIO_NUM_NC;
    gpio_num_t dataPin = GPIO_NUM_NC;
    bool invertClk = false;
    const char *label = "";
  };
  constexpr PdmRoute kRoutes[] = {
      {static_cast<gpio_num_t>(USER_MIC_PDM_CLK_PIN),
       static_cast<gpio_num_t>(USER_MIC_PDM_DATA_PIN),
       false,
       "CLK39/DATA42"},
      {static_cast<gpio_num_t>(USER_MIC_PDM_CLK_PIN),
       static_cast<gpio_num_t>(USER_MIC_PDM_DATA_PIN),
       true,
       "CLK39(inv)/DATA42"},
      {static_cast<gpio_num_t>(USER_MIC_PDM_DATA_PIN),
       static_cast<gpio_num_t>(USER_MIC_PDM_CLK_PIN),
       false,
       "CLK42/DATA39"},
      {static_cast<gpio_num_t>(USER_MIC_PDM_DATA_PIN),
       static_cast<gpio_num_t>(USER_MIC_PDM_CLK_PIN),
       true,
       "CLK42(inv)/DATA39"},
  };
  constexpr size_t kRouteCount = sizeof(kRoutes) / sizeof(kRoutes[0]);
  constexpr size_t kChunkBytes = 2048;
  constexpr uint8_t kMaxEmptyReads = 35;

  uint8_t chunk[kChunkBytes];
  uint32_t written = 0;
  String timeoutRouteLabel;
  String configErr;

  for (size_t route = 0; route < kRouteCount; ++route) {
    i2s_chan_handle_t rxChan = nullptr;

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 8;
    chanCfg.dma_frame_num = 240;
    esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &rxChan);
    if (err != ESP_OK || rxChan == nullptr) {
      configErr = formatEspErr("MIC I2S channel alloc failed", err);
      continue;
    }

    i2s_pdm_rx_config_t pdmCfg = {};
    pdmCfg.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sampleRate);
    pdmCfg.slot_cfg =
        I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    pdmCfg.gpio_cfg.clk = kRoutes[route].clkPin;
    pdmCfg.gpio_cfg.din = kRoutes[route].dataPin;
    pdmCfg.gpio_cfg.invert_flags.clk_inv = kRoutes[route].invertClk;

    err = i2s_channel_init_pdm_rx_mode(rxChan, &pdmCfg);
    if (err != ESP_OK) {
      configErr = formatEspErr("MIC I2S init failed", err);
      i2s_del_channel(rxChan);
      continue;
    }
    err = i2s_channel_enable(rxChan);
    if (err != ESP_OK) {
      configErr = formatEspErr("MIC I2S start failed", err);
      i2s_del_channel(rxChan);
      continue;
    }

    timeoutRouteLabel = String(kRoutes[route].label);
    delay(20);

    uint8_t emptyReads = 0;
    bool routeFailed = false;
    while (written < targetDataBytes) {
      if (stopRequested && stopRequested()) {
        i2s_channel_disable(rxChan);
        i2s_del_channel(rxChan);
        if (dataBytesWritten) {
          *dataBytesWritten = written;
        }
        return true;
      }

      const size_t toRead = std::min<size_t>(kChunkBytes, targetDataBytes - written);
      size_t readBytes = 0;
      err = i2s_channel_read(rxChan, chunk, toRead, &readBytes, 120);
      if (err == ESP_ERR_TIMEOUT) {
        readBytes = 0;
      } else if (err != ESP_OK) {
        i2s_channel_disable(rxChan);
        i2s_del_channel(rxChan);
        setError(error, formatEspErr("MIC I2S read failed", err));
        return false;
      }

      if (readBytes == 0) {
        ++emptyReads;
        if (emptyReads > kMaxEmptyReads) {
          routeFailed = true;
          break;
        }
        if (backgroundTick) {
          backgroundTick();
        }
        continue;
      }

      emptyReads = 0;
      if (file.write(chunk, readBytes) != readBytes) {
        i2s_channel_disable(rxChan);
        i2s_del_channel(rxChan);
        setError(error, "Failed to write voice sample");
        return false;
      }
      written += static_cast<uint32_t>(readBytes);

      if (backgroundTick) {
        backgroundTick();
      }
    }

    i2s_channel_disable(rxChan);
    i2s_del_channel(rxChan);

    if (!routeFailed || written >= targetDataBytes) {
      if (dataBytesWritten) {
        *dataBytesWritten = written;
      }
      return true;
    }
  }

  if (!configErr.isEmpty()) {
    setError(error, configErr);
    return false;
  }

  String msg = "MIC I2S timeout";
  if (!timeoutRouteLabel.isEmpty()) {
    msg += " (";
    msg += timeoutRouteLabel;
    msg += ")";
  }
  msg += ", check onboard MIC";
  setError(error, msg);
  return false;
}
#endif

bool capturePdmSamples(File &file,
                       uint32_t targetDataBytes,
                       uint32_t sampleRate,
                       const std::function<void()> &backgroundTick,
                       const std::function<bool()> &stopRequested,
                       uint32_t *dataBytesWritten,
                       String *error) {
#if AUDIO_RECORDER_HAS_I2S_PDM_CHANNEL && SOC_I2S_SUPPORTS_PDM_RX
  return capturePdmSamplesWithChannelApi(file,
                                         targetDataBytes,
                                         sampleRate,
                                         backgroundTick,
                                         stopRequested,
                                         dataBytesWritten,
                                         error);
#endif
  if (dataBytesWritten) {
    *dataBytesWritten = 0;
  }

  const auto uninstallI2sIfNeeded = []() -> esp_err_t {
    if (!gPdmI2sInstalled) {
      return ESP_OK;
    }
    const esp_err_t err = i2s_driver_uninstall(I2S_NUM_0);
    // Keep local state aligned even if driver reports already-uninstalled.
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
      gPdmI2sInstalled = false;
      return ESP_OK;
    }
    return err;
  };
  const auto shutdownI2s = [&uninstallI2sIfNeeded]() {
    i2s_stop(I2S_NUM_0);
    uninstallI2sIfNeeded();
    // Keep UI button pins in pull-up input mode after I2S teardown.
    pinMode(boardpins::kEncoderOk, INPUT_PULLUP);
    pinMode(boardpins::kEncoderBack, INPUT_PULLUP);
  };
  struct PdmRoute {
    bool duplicateClkToBck = false;
    const char *label = "";
  };
  constexpr PdmRoute kRoutes[] = {
      {false, "WS Clock"},
      {true, "WS+BCK Clock"},
  };
  constexpr size_t kRouteCount = sizeof(kRoutes) / sizeof(kRoutes[0]);

  const auto configurePdmRoute = [sampleRate](const PdmRoute &route, String *cfgError) -> bool {
    i2s_stop(I2S_NUM_0);
    i2s_pin_config_t pins = {};
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32)
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
    pins.bck_io_num = route.duplicateClkToBck ? USER_MIC_PDM_CLK_PIN : I2S_PIN_NO_CHANGE;
    pins.ws_io_num = USER_MIC_PDM_CLK_PIN;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = USER_MIC_PDM_DATA_PIN;
    const esp_err_t pinErr = i2s_set_pin(I2S_NUM_0, &pins);
    if (pinErr != ESP_OK) {
      if (cfgError) {
        *cfgError = formatEspErr("MIC I2S pin config failed", pinErr);
      }
      return false;
    }
    const esp_err_t clkErr =
        i2s_set_clk(I2S_NUM_0, static_cast<uint32_t>(sampleRate), I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    if (clkErr != ESP_OK) {
      if (cfgError) {
        *cfgError = formatEspErr("MIC I2S clock config failed", clkErr);
      }
      return false;
    }
    const esp_err_t dmaResetErr = i2s_zero_dma_buffer(I2S_NUM_0);
    if (dmaResetErr != ESP_OK) {
      if (cfgError) {
        *cfgError = formatEspErr("MIC I2S DMA reset failed", dmaResetErr);
      }
      return false;
    }
    const esp_err_t startErr = i2s_start(I2S_NUM_0);
    if (startErr != ESP_OK) {
      if (cfgError) {
        *cfgError = formatEspErr("MIC I2S start failed", startErr);
      }
      return false;
    }
    delay(20);
    if (cfgError) {
      *cfgError = "";
    }
    return true;
  };

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  config.sample_rate = static_cast<int>(sampleRate);
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL2;
  config.dma_desc_num = 8;
  config.dma_frame_num = 200;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  // Reset stale I2S allocation from a previous run if any.
  uninstallI2sIfNeeded();

  esp_err_t installErr = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  if (installErr != ESP_OK) {
    // On some cores, install rejects PDM mode flag while pin routing still supports PDM RX.
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    installErr = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  }
  if (installErr != ESP_OK) {
    setError(error, formatEspErr("MIC I2S init failed", installErr));
    return false;
  }
  gPdmI2sInstalled = true;

  // Match LilyGo reference flow first (WS clock only), then fallback to WS+BCK.
  constexpr size_t kChunkBytes = 2048;
  uint8_t chunk[kChunkBytes];
  uint32_t written = 0;
  constexpr uint8_t kMaxEmptyReads = 35;
  String configErr;
  String timeoutRouteLabel;
  bool completed = false;

  for (size_t route = 0; route < kRouteCount; ++route) {
    String routeCfgErr;
    if (!configurePdmRoute(kRoutes[route], &routeCfgErr)) {
      configErr = routeCfgErr;
      continue;
    }

    timeoutRouteLabel = String(kRoutes[route].label);
    uint8_t emptyReads = 0;
    while (written < targetDataBytes) {
      if (stopRequested && stopRequested()) {
        completed = true;
        break;
      }

      const size_t toRead = std::min<size_t>(kChunkBytes, targetDataBytes - written);
      size_t readBytes = 0;
      const esp_err_t readErr =
          i2s_read(I2S_NUM_0, chunk, toRead, &readBytes, pdMS_TO_TICKS(100));
      if (readErr != ESP_OK) {
        shutdownI2s();
        setError(error, formatEspErr("MIC I2S read failed", readErr));
        return false;
      }
      if (readBytes == 0) {
        ++emptyReads;
        if (emptyReads > kMaxEmptyReads) {
          break;
        }
        if (backgroundTick) {
          backgroundTick();
        }
        continue;
      }

      emptyReads = 0;
      if (file.write(chunk, readBytes) != readBytes) {
        shutdownI2s();
        setError(error, "Failed to write voice sample");
        return false;
      }
      written += static_cast<uint32_t>(readBytes);

      if (backgroundTick) {
        backgroundTick();
      }
    }

    if (completed || written >= targetDataBytes) {
      completed = true;
      break;
    }
  }

  if (!completed) {
    shutdownI2s();
    if (!configErr.isEmpty()) {
      setError(error, configErr);
      return false;
    }
    String msg = "MIC I2S timeout";
    if (!timeoutRouteLabel.isEmpty()) {
      msg += " (";
      msg += timeoutRouteLabel;
      msg += ")";
    }
    msg += ", check onboard MIC";
    setError(error, msg);
    return false;
  }

  shutdownI2s();
  if (dataBytesWritten) {
    *dataBytesWritten = written;
  }
  return true;
}
#endif

}  // namespace

bool isMicRecordingAvailable() {
  if (hasAdcMicConfigured()) {
    return true;
  }
#if defined(ARDUINO_ARCH_ESP32)
  if (hasPdmMicConfigured()) {
    return true;
  }
#endif
  return false;
}

bool recordMicWavToSd(const String &path,
                      uint16_t seconds,
                      const std::function<void()> &backgroundTick,
                      const std::function<bool()> &stopRequested,
                      String *error,
                      uint32_t *bytesWritten) {
  if (!isMicRecordingAvailable()) {
    setError(error, "MIC is not configured");
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

  if (!hasAdcMicConfigured() && hasPdmMicConfigured()) {
    String pinConflict;
    if (hasPdmPinConflict(&pinConflict)) {
      setError(error, pinConflict);
      return false;
    }
  }

  const uint16_t maxSeconds = static_cast<uint16_t>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_MAX_SECONDS)));
  if (seconds > maxSeconds) {
    setError(error, "Recording time exceeds limit");
    return false;
  }

  uint32_t sampleRate = sampleRateHz();
  if (!hasAdcMicConfigured() && hasPdmMicConfigured()) {
    sampleRate = pdmSampleRateHz(sampleRate);
  }
  const uint32_t maxSamples = sampleRate * static_cast<uint32_t>(seconds);

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

  bool captured = false;
  uint32_t capturedDataBytes = 0;
  if (hasAdcMicConfigured()) {
    uint32_t capturedSamples = 0;
    captured = captureAdcSamples(file,
                                 maxSamples,
                                 sampleRate,
                                 backgroundTick,
                                 stopRequested,
                                 &capturedSamples,
                                 error);
    capturedDataBytes = capturedSamples * 2U;
  }
#if defined(ARDUINO_ARCH_ESP32)
  else if (hasPdmMicConfigured()) {
    const uint32_t targetDataBytes = maxSamples * 2U;
    captured = capturePdmSamples(file,
                                 targetDataBytes,
                                 sampleRate,
                                 backgroundTick,
                                 stopRequested,
                                 &capturedDataBytes,
                                 error);
  }
#endif

  if (!captured) {
    file.close();
    SD.remove(path.c_str());
    if (error && error->isEmpty()) {
      setError(error, "MIC capture failed");
    }
    return false;
  }

  if (capturedDataBytes == 0) {
    file.close();
    SD.remove(path.c_str());
    setError(error, "No audio captured");
    return false;
  }

  if (!writeWavHeader(file, sampleRate, capturedDataBytes)) {
    file.close();
    SD.remove(path.c_str());
    setError(error, "Failed to finalize WAV header");
    return false;
  }

  file.flush();
  file.close();

  if (bytesWritten) {
    *bytesWritten = capturedDataBytes + kWavHeaderBytes;
  }
  setError(error, "");
  return true;
}
