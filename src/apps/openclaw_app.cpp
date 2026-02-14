#include "openclaw_app.h"

#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <time.h>

#include <vector>

#include "../core/cc1101_radio.h"
#include "../core/ble_manager.h"
#include "../core/board_pins.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/wifi_manager.h"
#include "../ui/ui_shell.h"

namespace {

constexpr const char *kMessageSenderId = "node-host";
constexpr size_t kVoiceChunkBytes = 360;
constexpr uint32_t kMaxVoiceBytes = 262144;

String boolLabel(bool value) {
  return value ? "Yes" : "No";
}

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

String trimMiddle(const String &value, size_t maxLength) {
  if (value.length() <= maxLength || maxLength < 6) {
    return value;
  }

  const size_t left = (maxLength - 3) / 2;
  const size_t right = maxLength - 3 - left;
  return value.substring(0, left) + "..." +
         value.substring(value.length() - right);
}

String baseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  if (slash < 0 || slash + 1 >= static_cast<int>(path.length())) {
    return path;
  }
  return path.substring(static_cast<unsigned int>(slash + 1));
}

String detectAudioMime(const String &path) {
  String lower = path;
  lower.toLowerCase();
  if (lower.endsWith(".wav")) {
    return "audio/wav";
  }
  if (lower.endsWith(".mp3")) {
    return "audio/mpeg";
  }
  if (lower.endsWith(".m4a")) {
    return "audio/mp4";
  }
  if (lower.endsWith(".aac")) {
    return "audio/aac";
  }
  if (lower.endsWith(".opus")) {
    return "audio/opus";
  }
  if (lower.endsWith(".ogg")) {
    return "audio/ogg";
  }
  return "application/octet-stream";
}

uint64_t currentUnixMs() {
  const time_t nowSec = time(nullptr);
  if (nowSec <= 0) {
    return 0;
  }
  return static_cast<uint64_t>(nowSec) * 1000ULL;
}

String makeMessageId(const char *prefix) {
  static uint32_t seq = 0;
  ++seq;

  String id(prefix ? prefix : "msg");
  id += "-";
  id += String(static_cast<unsigned long>(millis()));
  id += "-";
  id += String(seq);
  return id;
}

String encodeBase64(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return "";
  }

  const size_t outCap = ((len + 2) / 3) * 4 + 4;
  std::vector<unsigned char> encoded(outCap, 0);
  size_t outLen = 0;
  const int rc = mbedtls_base64_encode(encoded.data(),
                                       encoded.size(),
                                       &outLen,
                                       data,
                                       len);
  if (rc != 0 || outLen == 0 || outLen >= encoded.size()) {
    return "";
  }

  encoded[outLen] = '\0';
  return String(reinterpret_cast<const char *>(encoded.data()));
}

bool ensureSdMountedForVoice(String *error = nullptr) {
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = &TFT_eSPI::getSPIinstance();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                25000000,
                                "/sd",
                                8,
                                false);
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
}

bool ensureGatewayReady(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  const GatewayStatus status = ctx.gateway->status();
  if (!status.gatewayReady) {
    ctx.ui->showToast("Messaging", "Gateway is not ready", 1500, backgroundTick);
    return false;
  }
  return true;
}

void sendTextMessage(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return;
  }

  String recipient;
  if (!ctx.ui->textInput("To (optional)", recipient, false, backgroundTick)) {
    return;
  }

  String text;
  if (!ctx.ui->textInput("Text Message", text, false, backgroundTick)) {
    return;
  }
  text.trim();
  if (text.isEmpty()) {
    ctx.ui->showToast("Messaging", "Message is empty", 1400, backgroundTick);
    return;
  }

  DynamicJsonDocument payload(2048);
  payload["id"] = makeMessageId("txt");
  payload["from"] = kMessageSenderId;
  if (!recipient.isEmpty()) {
    payload["to"] = recipient;
  }
  payload["type"] = "text";
  payload["text"] = text;
  const uint64_t ts = currentUnixMs();
  if (ts > 0) {
    payload["ts"] = ts;
  }

  if (!ctx.gateway->sendNodeEvent("msg.text", payload)) {
    ctx.ui->showToast("Messaging", "Text send failed", 1500, backgroundTick);
    return;
  }

  ctx.ui->showToast("Messaging", "Text sent", 1100, backgroundTick);
}

void sendVoiceMessage(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return;
  }

  String recipient;
  if (!ctx.ui->textInput("To (optional)", recipient, false, backgroundTick)) {
    return;
  }

  String filePath = "/voice.wav";
  if (!ctx.ui->textInput("Voice File Path", filePath, false, backgroundTick)) {
    return;
  }
  filePath.trim();
  if (filePath.isEmpty()) {
    ctx.ui->showToast("Voice", "Path is empty", 1300, backgroundTick);
    return;
  }
  if (!filePath.startsWith("/")) {
    filePath = "/" + filePath;
  }

  String caption;
  if (!ctx.ui->textInput("Caption(optional)", caption, false, backgroundTick)) {
    return;
  }
  caption.trim();

  String mountErr;
  if (!ensureSdMountedForVoice(&mountErr)) {
    ctx.ui->showToast("Voice",
                      mountErr.isEmpty() ? String("SD mount failed") : mountErr,
                      1600,
                      backgroundTick);
    return;
  }

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    ctx.ui->showToast("Voice", "Open voice file failed", 1600, backgroundTick);
    return;
  }

  const uint32_t totalBytes = static_cast<uint32_t>(file.size());
  if (totalBytes == 0) {
    file.close();
    ctx.ui->showToast("Voice", "Voice file is empty", 1500, backgroundTick);
    return;
  }
  if (totalBytes > kMaxVoiceBytes) {
    file.close();
    ctx.ui->showToast("Voice", "File too large (max 256KB)", 1800, backgroundTick);
    return;
  }

  const uint16_t totalChunks = static_cast<uint16_t>(
      (totalBytes + static_cast<uint32_t>(kVoiceChunkBytes) - 1U) /
      static_cast<uint32_t>(kVoiceChunkBytes));
  const String messageId = makeMessageId("voice");
  const String mimeType = detectAudioMime(filePath);

  DynamicJsonDocument meta(2048);
  meta["id"] = messageId;
  meta["from"] = kMessageSenderId;
  if (!recipient.isEmpty()) {
    meta["to"] = recipient;
  }
  meta["type"] = "voice";
  meta["fileName"] = baseName(filePath);
  meta["contentType"] = mimeType;
  meta["size"] = totalBytes;
  meta["chunks"] = totalChunks;
  if (!caption.isEmpty()) {
    meta["text"] = caption;
  }
  const uint64_t metaTs = currentUnixMs();
  if (metaTs > 0) {
    meta["ts"] = metaTs;
  }

  if (!ctx.gateway->sendNodeEvent("msg.voice.meta", meta)) {
    file.close();
    ctx.ui->showToast("Voice", "Voice meta send failed", 1700, backgroundTick);
    return;
  }

  uint8_t raw[kVoiceChunkBytes] = {0};
  uint16_t chunkIndex = 0;
  while (file.available() && chunkIndex < totalChunks) {
    const size_t readLen = file.read(raw, sizeof(raw));
    if (readLen == 0) {
      break;
    }

    const String encoded = encodeBase64(raw, readLen);
    if (encoded.isEmpty()) {
      file.close();
      ctx.ui->showToast("Voice", "Base64 encode failed", 1700, backgroundTick);
      return;
    }

    DynamicJsonDocument chunk(2048);
    chunk["id"] = messageId;
    chunk["from"] = kMessageSenderId;
    if (!recipient.isEmpty()) {
      chunk["to"] = recipient;
    }
    chunk["seq"] = static_cast<uint32_t>(chunkIndex + 1);
    chunk["chunks"] = totalChunks;
    chunk["last"] = (chunkIndex + 1) >= totalChunks;
    chunk["data"] = encoded;
    const uint64_t chunkTs = currentUnixMs();
    if (chunkTs > 0) {
      chunk["ts"] = chunkTs;
    }

    if (!ctx.gateway->sendNodeEvent("msg.voice.chunk", chunk)) {
      file.close();
      ctx.ui->showToast("Voice", "Voice chunk send failed", 1700, backgroundTick);
      return;
    }

    ++chunkIndex;
    if (backgroundTick) {
      backgroundTick();
    }
  }
  file.close();

  if (chunkIndex != totalChunks) {
    ctx.ui->showToast("Voice", "Voice send incomplete", 1700, backgroundTick);
    return;
  }

  ctx.ui->showToast("Voice", "Voice sent", 1200, backgroundTick);
}

void showInboxMessageDetail(AppContext &ctx,
                            const GatewayInboxMessage &message,
                            const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("ID: " + (message.id.isEmpty() ? String("(none)") : message.id));
  lines.push_back("Event: " + (message.event.isEmpty() ? String("(none)") : message.event));
  lines.push_back("Type: " + (message.type.isEmpty() ? String("text") : message.type));
  lines.push_back("From: " + (message.from.isEmpty() ? String("(unknown)") : message.from));
  lines.push_back("To: " + (message.to.isEmpty() ? String("(broadcast)") : message.to));

  if (!message.text.isEmpty()) {
    lines.push_back("Text: " + message.text);
  }
  if (!message.fileName.isEmpty()) {
    lines.push_back("File: " + message.fileName);
  }
  if (!message.contentType.isEmpty()) {
    lines.push_back("MIME: " + message.contentType);
  }
  if (message.voiceBytes > 0) {
    lines.push_back("Bytes: " + String(message.voiceBytes));
  }
  if (message.tsMs > 0) {
    lines.push_back("TS(ms): " + String(static_cast<unsigned long long>(message.tsMs)));
  }

  ctx.ui->showInfo("Message Detail", lines, backgroundTick, "OK/BACK Exit");
}

void showInbox(AppContext &ctx,
               const std::function<void()> &backgroundTick) {
  if (ctx.gateway->inboxCount() == 0) {
    ctx.ui->showToast("Inbox", "No messages", 1100, backgroundTick);
    return;
  }

  int selected = 0;
  while (true) {
    const size_t count = ctx.gateway->inboxCount();
    if (count == 0) {
      ctx.ui->showToast("Inbox", "No messages", 1000, backgroundTick);
      return;
    }

    std::vector<String> menu;
    menu.reserve(count + 1);
    for (size_t i = 0; i < count; ++i) {
      GatewayInboxMessage message;
      if (!ctx.gateway->inboxMessage(i, message)) {
        continue;
      }
      const bool isVoice = message.type.startsWith("voice");
      String label = isVoice ? "[V] " : "[T] ";
      const String sender = message.from.isEmpty() ? String("unknown") : message.from;
      label += trimMiddle(sender, 12);
      if (isVoice && !message.fileName.isEmpty()) {
        label += " " + trimMiddle(message.fileName, 16);
      } else if (!message.text.isEmpty()) {
        label += " " + trimMiddle(message.text, 16);
      }
      menu.push_back(label);
    }
    menu.push_back("Back");

    if (selected >= static_cast<int>(menu.size())) {
      selected = static_cast<int>(menu.size()) - 1;
      if (selected < 0) {
        selected = 0;
      }
    }

    const String subtitle = "Inbox: " + String(static_cast<unsigned long>(count));
    const int choice = ctx.ui->menuLoop("Messaging Inbox",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Open  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == static_cast<int>(count)) {
      return;
    }

    selected = choice;

    GatewayInboxMessage message;
    if (!ctx.gateway->inboxMessage(static_cast<size_t>(choice), message)) {
      continue;
    }
    showInboxMessageDetail(ctx, message, backgroundTick);
  }
}

void clearInbox(AppContext &ctx,
                const std::function<void()> &backgroundTick) {
  if (!ctx.ui->confirm("Clear Inbox",
                       "Delete all received messages?",
                       backgroundTick,
                       "Clear",
                       "Cancel")) {
    return;
  }
  ctx.gateway->clearInbox();
  ctx.ui->showToast("Inbox", "Inbox cleared", 1100, backgroundTick);
}

void runMessagingMenu(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Send Text");
    menu.push_back("Send Voice (SD)");
    menu.push_back("Inbox");
    menu.push_back("Clear Inbox");
    menu.push_back("Back");

    String subtitle = "Inbox:";
    subtitle += String(static_cast<unsigned long>(ctx.gateway->inboxCount()));
    subtitle += " GW:";
    subtitle += ctx.gateway->status().gatewayReady ? "READY" : "DOWN";

    const int choice = ctx.ui->menuLoop("OpenClaw / Messaging",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == 4) {
      return;
    }

    selected = choice;
    if (choice == 0) {
      sendTextMessage(ctx, backgroundTick);
    } else if (choice == 1) {
      sendVoiceMessage(ctx, backgroundTick);
    } else if (choice == 2) {
      showInbox(ctx, backgroundTick);
    } else if (choice == 3) {
      clearInbox(ctx, backgroundTick);
    }
  }
}

void runGatewayMenu(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Edit URL");
    menu.push_back("Auth Mode");
    menu.push_back("Edit Credential");
    menu.push_back("Clear Gateway");
    menu.push_back("Back");

    String subtitle = "Auth: ";
    subtitle += gatewayAuthModeName(ctx.config.gatewayAuthMode);

    const int choice = ctx.ui->menuLoop("OpenClaw / Gateway",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == 4) {
      return;
    }
    selected = choice;

    if (choice == 0) {
      String url = ctx.config.gatewayUrl;
      if (ctx.ui->textInput("Gateway URL", url, false, backgroundTick)) {
        ctx.config.gatewayUrl = url;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 1) {
      std::vector<String> authItems;
      authItems.push_back("Token");
      authItems.push_back("Password");

      const int current = ctx.config.gatewayAuthMode == GatewayAuthMode::Password ? 1 : 0;
      const int authChoice = ctx.ui->menuLoop("Gateway Auth",
                                              authItems,
                                              current,
                                              backgroundTick,
                                              "OK Select  BACK Exit",
                                              "Choose auth mode");
      if (authChoice >= 0) {
        ctx.config.gatewayAuthMode = authChoice == 1
                                         ? GatewayAuthMode::Password
                                         : GatewayAuthMode::Token;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 2) {
      if (ctx.config.gatewayAuthMode == GatewayAuthMode::Password) {
        String password = ctx.config.gatewayPassword;
        if (ctx.ui->textInput("Gateway Password", password, true, backgroundTick)) {
          ctx.config.gatewayPassword = password;
          markDirty(ctx);
        }
      } else {
        String token = ctx.config.gatewayToken;
        if (ctx.ui->textInput("Gateway Token", token, true, backgroundTick)) {
          ctx.config.gatewayToken = token;
          markDirty(ctx);
        }
      }
      continue;
    }

    if (choice == 3) {
      ctx.config.gatewayUrl = "";
      ctx.config.gatewayToken = "";
      ctx.config.gatewayPassword = "";
      ctx.config.gatewayDeviceToken = "";
      markDirty(ctx);
      ctx.ui->showToast("Gateway", "Gateway config cleared", 1200, backgroundTick);
      continue;
    }
  }
}

void applyRuntimeConfig(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Validation", validateErr, 1800, backgroundTick);
    return;
  }

  String saveErr;
  if (!saveConfig(ctx.config, &saveErr)) {
    String message = saveErr.isEmpty() ? String("Failed to save config") : saveErr;
    message += " / previous config kept";
    ctx.ui->showToast("Save Error", message, 1900, backgroundTick);
    return;
  }

  ctx.configDirty = false;

  ctx.wifi->configure(ctx.config);
  ctx.gateway->configure(ctx.config);
  ctx.ble->configure(ctx.config);

  if (!ctx.config.gatewayUrl.isEmpty() && hasGatewayCredentials(ctx.config)) {
    ctx.gateway->reconnectNow();
  } else {
    ctx.gateway->disconnectNow();
  }

  if (ctx.config.bleDeviceAddress.isEmpty()) {
    ctx.ble->disconnectNow();
  } else if (ctx.config.bleAutoConnect) {
    String bleErr;
    if (!ctx.ble->connectToDevice(ctx.config.bleDeviceAddress,
                                  ctx.config.bleDeviceName,
                                  &bleErr)) {
      ctx.ui->showToast("BLE", bleErr, 1500, backgroundTick);
    }
  }

  ctx.ui->showToast("OpenClaw", "Saved and applied", 1400, backgroundTick);
}

std::vector<String> buildStatusLines(AppContext &ctx) {
  std::vector<String> lines;

  GatewayStatus gs = ctx.gateway->status();
  String cfgErr;
  const bool configOk = validateConfig(ctx.config, &cfgErr);

  lines.push_back("Config Valid: " + boolLabel(configOk));
  if (!configOk) {
    lines.push_back("OpenClaw settings required");
    lines.push_back("Config Error: " + cfgErr);
  }
  lines.push_back("Wi-Fi Connected: " + boolLabel(ctx.wifi->isConnected()));
  lines.push_back("Wi-Fi SSID: " + (ctx.wifi->ssid().isEmpty() ? String("(empty)") : ctx.wifi->ssid()));
  lines.push_back("IP: " + (ctx.wifi->ip().isEmpty() ? String("-") : ctx.wifi->ip()));
  lines.push_back("RSSI: " + String(ctx.wifi->rssi()));
  lines.push_back("Gateway URL: " + (ctx.config.gatewayUrl.isEmpty() ? String("(empty)") : ctx.config.gatewayUrl));
  lines.push_back("WS Connected: " + boolLabel(gs.wsConnected));
  lines.push_back("Gateway Ready: " + boolLabel(gs.gatewayReady));
  lines.push_back("Should Connect: " + boolLabel(gs.shouldConnect));
  lines.push_back("Inbox Messages: " + String(static_cast<unsigned long>(ctx.gateway->inboxCount())));
  lines.push_back("Auth Mode: " + String(gatewayAuthModeName(ctx.config.gatewayAuthMode)));
  lines.push_back("Device Token: " + boolLabel(!ctx.config.gatewayDeviceToken.isEmpty()));
  lines.push_back("Device ID: " +
                  (ctx.config.gatewayDeviceId.isEmpty() ? String("(empty)")
                                                        : ctx.config.gatewayDeviceId));
  lines.push_back("CC1101 Ready: " + boolLabel(isCc1101Ready()));
  lines.push_back("CC1101 Freq MHz: " + String(getCc1101FrequencyMhz(), 2));

  BleStatus bs = ctx.ble->status();
  lines.push_back("BLE Connected: " + boolLabel(bs.connected));
  lines.push_back("BLE Device: " +
                  (bs.deviceName.isEmpty() ? String("(none)") : bs.deviceName));
  lines.push_back("BLE Address: " +
                  (bs.deviceAddress.isEmpty() ? String("(none)") : bs.deviceAddress));
  if (bs.rssi != 0) {
    lines.push_back("BLE RSSI: " + String(bs.rssi));
  }
  if (!bs.lastError.isEmpty()) {
    lines.push_back("BLE Last Error: " + bs.lastError);
  }

  if (!gs.lastError.isEmpty()) {
    lines.push_back("Last Error: " + gs.lastError);
  }

  return lines;
}

}  // namespace

void runOpenClawApp(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    const GatewayStatus gs = ctx.gateway->status();
    String subtitle = "Wi-Fi:";
    subtitle += ctx.wifi->isConnected() ? "UP " : "DOWN ";
    subtitle += "GW:";
    subtitle += gs.gatewayReady ? "READY" : (gs.wsConnected ? "WS" : "IDLE");
    if (ctx.configDirty) {
      subtitle += " *DIRTY";
    }

    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Gateway");
    menu.push_back("Messaging");
    menu.push_back("Save & Apply");
    menu.push_back("Connect");
    menu.push_back("Disconnect");
    menu.push_back("Reconnect");
    menu.push_back("Back");

    const int choice = ctx.ui->menuLoop("OpenClaw",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);

    if (choice < 0 || choice == 7) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      ctx.ui->showInfo("OpenClaw Status",
                       buildStatusLines(ctx),
                       backgroundTick,
                       "OK/BACK Exit");
      continue;
    }

    if (choice == 1) {
      runGatewayMenu(ctx, backgroundTick);
      continue;
    }

    if (choice == 2) {
      runMessagingMenu(ctx, backgroundTick);
      continue;
    }

    if (choice == 3) {
      applyRuntimeConfig(ctx, backgroundTick);
      continue;
    }

    if (choice == 4) {
      String validateErr;
      if (!validateConfig(ctx.config, &validateErr)) {
        ctx.ui->showToast("Config Error", validateErr, 1800, backgroundTick);
        continue;
      }
      if (ctx.config.gatewayUrl.isEmpty()) {
        ctx.ui->showToast("Config Error",
                          "Set gateway URL first",
                          1600,
                          backgroundTick);
        continue;
      }
      ctx.gateway->configure(ctx.config);
      ctx.gateway->connectNow();
      ctx.ui->showToast("OpenClaw", "Connect requested", 1200, backgroundTick);
      continue;
    }

    if (choice == 5) {
      ctx.gateway->disconnectNow();
      ctx.ui->showToast("OpenClaw", "Disconnected", 1200, backgroundTick);
      continue;
    }

    if (choice == 6) {
      String validateErr;
      if (!validateConfig(ctx.config, &validateErr)) {
        ctx.ui->showToast("Config Error", validateErr, 1800, backgroundTick);
        continue;
      }
      ctx.gateway->configure(ctx.config);
      ctx.gateway->reconnectNow();
      ctx.ui->showToast("OpenClaw", "Reconnect requested", 1400, backgroundTick);
      continue;
    }
  }
}
