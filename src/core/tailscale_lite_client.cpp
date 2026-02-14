#include "tailscale_lite_client.h"

#include <WiFi.h>
#include <WireGuard-ESP32.h>

namespace {

constexpr unsigned long kLiteReconnectRetryMs = 3000UL;

WireGuard gWireGuard;

}  // namespace

void TailscaleLiteClient::begin() {
  initialized_ = true;
}

void TailscaleLiteClient::configure(const RuntimeConfig &config) {
  const bool changed =
      config_.tailscaleLiteEnabled != config.tailscaleLiteEnabled ||
      config_.tailscaleLiteNodeIp != config.tailscaleLiteNodeIp ||
      config_.tailscaleLitePrivateKey != config.tailscaleLitePrivateKey ||
      config_.tailscaleLitePeerHost != config.tailscaleLitePeerHost ||
      config_.tailscaleLitePeerPort != config.tailscaleLitePeerPort ||
      config_.tailscaleLitePeerPublicKey != config.tailscaleLitePeerPublicKey;

  config_ = config;

  if (!config_.tailscaleLiteEnabled) {
    shouldConnect_ = false;
    stopTunnel();
    lastError_ = "";
    return;
  }

  shouldConnect_ = true;

  if (changed) {
    stopTunnel();
    lastConnectAttemptMs_ = 0;
  }
}

void TailscaleLiteClient::tick() {
  if (!initialized_ || !config_.tailscaleLiteEnabled) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (tunnelUp_) {
      stopTunnel();
      lastError_ = "Wi-Fi disconnected";
    }
    return;
  }

  if (!shouldConnect_ || tunnelUp_) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastConnectAttemptMs_ < kLiteReconnectRetryMs) {
    return;
  }

  String err;
  if (!startTunnel(&err)) {
    if (!err.isEmpty()) {
      lastError_ = err;
    }
  }
}

bool TailscaleLiteClient::connectNow(String *error) {
  shouldConnect_ = true;

  String connectErr;
  if (!startTunnel(&connectErr)) {
    if (!connectErr.isEmpty()) {
      lastError_ = connectErr;
    }
    if (error) {
      *error = connectErr;
    }
    return false;
  }

  if (error) {
    error->clear();
  }
  return true;
}

void TailscaleLiteClient::disconnectNow() {
  shouldConnect_ = false;
  stopTunnel();
}

bool TailscaleLiteClient::isConnected() const {
  return tunnelUp_;
}

String TailscaleLiteClient::lastError() const {
  return lastError_;
}

TailscaleLiteStatus TailscaleLiteClient::status() const {
  TailscaleLiteStatus status;
  status.enabled = config_.tailscaleLiteEnabled;
  status.wifiConnected = WiFi.status() == WL_CONNECTED;
  status.tunnelUp = tunnelUp_;
  status.nodeIp = config_.tailscaleLiteNodeIp;
  status.peerHost = config_.tailscaleLitePeerHost;
  status.peerPort = config_.tailscaleLitePeerPort;
  status.lastError = lastError_;
  return status;
}

bool TailscaleLiteClient::hasRequiredConfig(String *error) const {
  if (!config_.tailscaleLiteEnabled) {
    if (error) {
      *error = "Tailscale Lite is disabled";
    }
    return false;
  }

  if (config_.tailscaleLiteNodeIp.isEmpty()) {
    if (error) {
      *error = "Lite node IP is empty";
    }
    return false;
  }
  if (config_.tailscaleLitePrivateKey.isEmpty()) {
    if (error) {
      *error = "Lite private key is empty";
    }
    return false;
  }
  if (config_.tailscaleLitePeerHost.isEmpty()) {
    if (error) {
      *error = "Lite peer host is empty";
    }
    return false;
  }
  if (config_.tailscaleLitePeerPublicKey.isEmpty()) {
    if (error) {
      *error = "Lite peer public key is empty";
    }
    return false;
  }
  if (config_.tailscaleLitePeerPort == 0) {
    if (error) {
      *error = "Lite peer port is empty";
    }
    return false;
  }

  return true;
}

bool TailscaleLiteClient::startTunnel(String *error) {
  lastConnectAttemptMs_ = millis();

  String configErr;
  if (!hasRequiredConfig(&configErr)) {
    if (error) {
      *error = configErr;
    }
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (error) {
      *error = "Wi-Fi is not connected";
    }
    return false;
  }

  if (gWireGuard.is_initialized()) {
    gWireGuard.end();
    tunnelUp_ = false;
  }

  IPAddress nodeIp;
  if (!nodeIp.fromString(config_.tailscaleLiteNodeIp.c_str())) {
    if (error) {
      *error = "Invalid lite node IP";
    }
    return false;
  }

  const bool ok = gWireGuard.begin(nodeIp,
                                   config_.tailscaleLitePrivateKey.c_str(),
                                   config_.tailscaleLitePeerHost.c_str(),
                                   config_.tailscaleLitePeerPublicKey.c_str(),
                                   config_.tailscaleLitePeerPort);
  if (!ok) {
    if (error) {
      *error = "WireGuard begin failed";
    }
    tunnelUp_ = false;
    return false;
  }

  tunnelUp_ = true;
  lastError_ = "";
  return true;
}

void TailscaleLiteClient::stopTunnel() {
  if (gWireGuard.is_initialized()) {
    gWireGuard.end();
  }
  tunnelUp_ = false;
}

