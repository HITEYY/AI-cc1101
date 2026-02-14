#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

enum class Cc1101Modulation : uint8_t {
  Fsk2 = 0,
  Gfsk = 1,
  AskOok = 2,
  Fsk4 = 3,
  Msk = 4,
};

struct Cc1101PacketConfig {
  uint8_t modulation = static_cast<uint8_t>(Cc1101Modulation::AskOok);
  uint8_t channel = 0;
  float dataRateKbps = 4.8f;
  float deviationKHz = 5.0f;
  float rxBandwidthKHz = 256.0f;
  uint8_t syncMode = 2;
  uint8_t packetFormat = 0;
  bool crcEnabled = true;
  uint8_t lengthConfig = 1;
  uint8_t packetLength = 61;
  bool whitening = false;
  bool manchester = false;
};

bool initCc1101Radio();
bool isCc1101Ready();
float getCc1101FrequencyMhz();
void setCc1101FrequencyMhz(float mhz);

const Cc1101PacketConfig &getCc1101PacketConfig();
bool configureCc1101Packet(const Cc1101PacketConfig &config, String &errorOut);
int readCc1101RssiDbm(String *errorOut = nullptr);

bool sendCc1101Packet(const uint8_t *data,
                      size_t size,
                      int txDelayMs,
                      String &errorOut);
bool sendCc1101PacketText(const String &text,
                          int txDelayMs,
                          String &errorOut);
bool receiveCc1101Packet(std::vector<uint8_t> &outData,
                         int timeoutMs,
                         int *rssiOut,
                         String &errorOut);

bool transmitCc1101(uint32_t code,
                    int bits,
                    int pulseLength,
                    int protocol,
                    int repeat,
                    String &errorOut);

void appendCc1101Info(JsonObject obj);
