#include "wifi_manager.h"

#include <WiFi.h>

#include <algorithm>

namespace {

constexpr unsigned long kConnectRetryMs = 3500UL;

bool containsSsid(const std::vector<String> &list, const String &value) {
  for (std::vector<String>::const_iterator it = list.begin(); it != list.end(); ++it) {
    if (*it == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

void WifiManager::begin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
}

void WifiManager::configure(const RuntimeConfig &config) {
  const bool credentialsChanged =
      targetSsid_ != config.wifiSsid || targetPassword_ != config.wifiPassword;

  targetSsid_ = config.wifiSsid;
  targetPassword_ = config.wifiPassword;

  if (targetSsid_.isEmpty()) {
    WiFi.disconnect(true, false);
    lastConnectAttemptMs_ = 0;
    return;
  }

  if (credentialsChanged) {
    lastConnectAttemptMs_ = 0;
    startConnectAttempt(true);
  }
}

void WifiManager::tick() {
  if (targetSsid_.isEmpty()) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastConnectAttemptMs_ < kConnectRetryMs) {
    return;
  }

  startConnectAttempt(false);
}

void WifiManager::connectNow() {
  if (targetSsid_.isEmpty()) {
    return;
  }
  startConnectAttempt(true);
}

void WifiManager::disconnect() {
  WiFi.disconnect(true, false);
}

bool WifiManager::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::hasCredentials() const {
  return !targetSsid_.isEmpty();
}

String WifiManager::ssid() const {
  return targetSsid_;
}

String WifiManager::ip() const {
  if (WiFi.status() != WL_CONNECTED) {
    return "";
  }
  return WiFi.localIP().toString();
}

int WifiManager::rssi() const {
  if (WiFi.status() != WL_CONNECTED) {
    return 0;
  }
  return WiFi.RSSI();
}

bool WifiManager::scanNetworks(std::vector<String> &outSsids, String *error) {
  outSsids.clear();

  WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks(false, true);
  if (n < 0) {
    if (error) {
      *error = "Wi-Fi scan failed";
    }
    return false;
  }

  std::vector<std::pair<int, String> > candidates;
  candidates.reserve(static_cast<size_t>(n));

  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }
    candidates.push_back(std::make_pair(WiFi.RSSI(i), ssid));
  }
  WiFi.scanDelete();

  std::sort(candidates.begin(), candidates.end(),
            [](const std::pair<int, String> &a, const std::pair<int, String> &b) {
              if (a.first == b.first) {
                return a.second < b.second;
              }
              return a.first > b.first;
            });

  for (std::vector<std::pair<int, String> >::const_iterator it = candidates.begin();
       it != candidates.end(); ++it) {
    if (!containsSsid(outSsids, it->second)) {
      outSsids.push_back(it->second);
    }
  }

  return true;
}

void WifiManager::startConnectAttempt(bool disconnectFirst) {
  if (targetSsid_.isEmpty()) {
    return;
  }

  WiFi.mode(WIFI_STA);
  if (disconnectFirst) {
    WiFi.disconnect(false, false);
    delay(40);
  }

  WiFi.begin(targetSsid_.c_str(), targetPassword_.c_str());
  lastConnectAttemptMs_ = millis();
}
