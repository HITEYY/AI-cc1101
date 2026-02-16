#include "wifi_manager.h"

#include <WiFi.h>

#include <algorithm>

namespace {

constexpr unsigned long kConnectRetryMs = 3500UL;
constexpr unsigned long kConnectAttemptTimeoutMs = 12000UL;

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
  connectInProgress_ = false;
  connectStartedMs_ = 0;
  lastError_ = "";
}

void WifiManager::configure(const RuntimeConfig &config) {
  const bool credentialsChanged =
      targetSsid_ != config.wifiSsid || targetPassword_ != config.wifiPassword;

  targetSsid_ = config.wifiSsid;
  targetPassword_ = config.wifiPassword;

  if (targetSsid_.isEmpty()) {
    WiFi.disconnect(true, false);
    lastConnectAttemptMs_ = 0;
    connectInProgress_ = false;
    connectStartedMs_ = 0;
    lastError_ = "";
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
  refreshConnectState();

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (connectInProgress_) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastConnectAttemptMs_ < kConnectRetryMs) {
    return;
  }

  startConnectAttempt(false);
}

bool WifiManager::connectNow() {
  if (targetSsid_.isEmpty()) {
    lastError_ = "SSID is empty";
    return false;
  }
  refreshConnectState();
  if (connectInProgress_) {
    lastError_ = "Wi-Fi is already connecting";
    return false;
  }
  return startConnectAttempt(true);
}

void WifiManager::disconnect() {
  connectInProgress_ = false;
  connectStartedMs_ = 0;
  lastConnectAttemptMs_ = 0;
  lastError_ = "";
  WiFi.disconnect(true, false);
}

bool WifiManager::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::hasCredentials() const {
  return !targetSsid_.isEmpty();
}

bool WifiManager::hasConnectionError() const {
  return !lastError_.isEmpty();
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

String WifiManager::lastConnectionError() const {
  return lastError_;
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

bool WifiManager::startConnectAttempt(bool disconnectFirst) {
  if (targetSsid_.isEmpty()) {
    lastError_ = "SSID is empty";
    return false;
  }

  if (connectInProgress_ && !disconnectFirst) {
    return false;
  }

  String credentialErr;
  if (!validateCredentials(&credentialErr)) {
    lastError_ = credentialErr;
    return false;
  }

  if (disconnectFirst) {
    connectInProgress_ = false;
    WiFi.disconnect(true, false);
    delay(400);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(targetSsid_.c_str(), targetPassword_.c_str());
  lastConnectAttemptMs_ = millis();
  connectStartedMs_ = lastConnectAttemptMs_;
  connectInProgress_ = true;
  lastError_ = "";
  return true;
}

void WifiManager::refreshConnectState() {
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    connectInProgress_ = false;
    connectStartedMs_ = 0;
    lastError_ = "";
    return;
  }

  if (!connectInProgress_) {
    return;
  }

  if (status == WL_CONNECT_FAILED ||
      status == WL_NO_SSID_AVAIL ||
      status == WL_CONNECTION_LOST) {
    connectInProgress_ = false;
    connectStartedMs_ = 0;
    if (status == WL_NO_SSID_AVAIL) {
      lastError_ = "SSID not found";
    } else if (status == WL_CONNECT_FAILED) {
      // ESP32 often reports this for auth failures (wrong password/security mismatch).
      lastError_ = "Authentication failed (check password)";
    } else {
      lastError_ = "Wi-Fi connection lost";
    }
    return;
  }

  const unsigned long now = millis();
  if (connectStartedMs_ != 0 && now - connectStartedMs_ >= kConnectAttemptTimeoutMs) {
    connectInProgress_ = false;
    connectStartedMs_ = 0;
    lastError_ = "Wi-Fi connection timeout";
  }
}

bool WifiManager::validateCredentials(String *error) const {
  if (targetSsid_.isEmpty()) {
    if (error) {
      *error = "SSID is empty";
    }
    return false;
  }

  if (targetPassword_.isEmpty()) {
    return true;
  }

  const size_t passwordLen = targetPassword_.length();
  const bool is64Hex = passwordLen == 64 && isLikelyHexString(targetPassword_);
  if (passwordLen < 8) {
    if (error) {
      *error = "Wi-Fi password must be 8+ chars";
    }
    return false;
  }

  if (passwordLen > 63 && !is64Hex) {
    if (error) {
      *error = "Wi-Fi password must be 8~63 chars (or 64 hex)";
    }
    return false;
  }

  return true;
}

bool WifiManager::isLikelyHexString(const String &value) {
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[static_cast<unsigned int>(i)];
    const bool isHex = (c >= '0' && c <= '9') ||
                       (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
    if (!isHex) {
      return false;
    }
  }
  return true;
}
