#pragma once

#include <Arduino.h>

#include "runtime_config.h"

struct TailscaleLiteStatus {
  bool enabled = false;
  bool wifiConnected = false;
  bool tunnelUp = false;
  String nodeIp;
  String peerHost;
  uint16_t peerPort = 0;
  String lastError;
};

class TailscaleLiteClient {
 public:
  void begin();
  void configure(const RuntimeConfig &config);
  void tick();

  bool connectNow(String *error = nullptr);
  void disconnectNow();

  bool isConnected() const;
  String lastError() const;
  TailscaleLiteStatus status() const;

 private:
  RuntimeConfig config_;
  bool initialized_ = false;
  bool shouldConnect_ = false;
  bool tunnelUp_ = false;
  unsigned long lastConnectAttemptMs_ = 0;
  String lastError_;

  bool hasRequiredConfig(String *error = nullptr) const;
  bool startTunnel(String *error = nullptr);
  void stopTunnel();
};

