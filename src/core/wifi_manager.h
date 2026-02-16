#pragma once

#include <Arduino.h>

#include <vector>

#include "runtime_config.h"

class WifiManager {
 public:
  void begin();
  void configure(const RuntimeConfig &config);
  void tick();
  bool connectNow();
  void disconnect();

  bool isConnected() const;
  bool hasCredentials() const;
  bool hasConnectionError() const;
  String ssid() const;
  String ip() const;
  int rssi() const;
  String lastConnectionError() const;

  bool scanNetworks(std::vector<String> &outSsids, String *error = nullptr);

 private:
  String targetSsid_;
  String targetPassword_;
  String lastError_;
  unsigned long lastConnectAttemptMs_ = 0;
  unsigned long connectStartedMs_ = 0;
  bool connectInProgress_ = false;

  bool startConnectAttempt(bool disconnectFirst);
  void refreshConnectState();
  bool validateCredentials(String *error) const;
  static bool isLikelyHexString(const String &value);
};
