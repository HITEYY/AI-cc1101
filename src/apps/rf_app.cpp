#include "rf_app.h"

#include <cstdlib>

#include <vector>

#include "../core/cc1101_radio.h"
#include "../ui/ui_runtime.h"

namespace {

String boolLabel(bool value) {
  return value ? "On" : "Off";
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

bool parseIntToken(const String &token, int &out) {
  char *endPtr = nullptr;
  const long value = strtol(token.c_str(), &endPtr, 0);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool parseUInt32Token(const String &token, uint32_t &out) {
  char *endPtr = nullptr;
  const unsigned long value = strtoul(token.c_str(), &endPtr, 0);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

bool parseFloatToken(const String &token, float &out) {
  char *endPtr = nullptr;
  const float value = strtof(token.c_str(), &endPtr);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = value;
  return true;
}

String modulationName(uint8_t modulation) {
  switch (modulation) {
    case 0:
      return "2-FSK";
    case 1:
      return "GFSK";
    case 2:
      return "ASK/OOK";
    case 3:
      return "4-FSK";
    case 4:
      return "MSK";
    default:
      return "Unknown";
  }
}

String packetFormatName(uint8_t format) {
  switch (format) {
    case 0:
      return "FIFO";
    case 1:
      return "Sync Serial";
    case 2:
      return "Random TX";
    case 3:
      return "Async Serial";
    default:
      return "Unknown";
  }
}

String lengthConfigName(uint8_t value) {
  switch (value) {
    case 0:
      return "Fixed";
    case 1:
      return "Variable";
    case 2:
      return "Infinite";
    case 3:
      return "Reserved";
    default:
      return "Unknown";
  }
}

String toAsciiPreview(const std::vector<uint8_t> &bytes) {
  if (bytes.empty()) {
    return "(empty)";
  }

  String out;
  out.reserve(bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i) {
    const uint8_t c = bytes[i];
    if (c >= 32 && c <= 126) {
      out += static_cast<char>(c);
    } else {
      out += '.';
    }
  }

  if (out.length() > 80) {
    out = out.substring(0, 77) + "...";
  }
  return out;
}

void appendHexLines(const std::vector<uint8_t> &bytes, std::vector<String> &lines) {
  constexpr size_t kBytesPerLine = 8;
  char byteHex[4] = {0};

  for (size_t i = 0; i < bytes.size(); i += kBytesPerLine) {
    String line;
    for (size_t j = 0; j < kBytesPerLine && i + j < bytes.size(); ++j) {
      snprintf(byteHex, sizeof(byteHex), "%02X", bytes[i + j]);
      if (!line.isEmpty()) {
        line += ' ';
      }
      line += byteHex;
    }
    lines.push_back(line);
  }
}

void showRadioInfo(AppContext &ctx,
                   const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back(String("Ready: ") + (isCc1101Ready() ? "Yes" : "No"));
  lines.push_back("Freq: " + String(getCc1101FrequencyMhz(), 2) + " MHz");

  const Cc1101PacketConfig &cfg = getCc1101PacketConfig();
  lines.push_back("Mod: " + modulationName(cfg.modulation));
  lines.push_back("Ch: " + String(cfg.channel));
  lines.push_back("Rate: " + String(cfg.dataRateKbps, 2) + " kbps");
  lines.push_back("Dev: " + String(cfg.deviationKHz, 1) + " kHz");
  lines.push_back("RxBW: " + String(cfg.rxBandwidthKHz, 1) + " kHz");
  lines.push_back("Sync: " + String(cfg.syncMode));
  lines.push_back("Fmt: " + packetFormatName(cfg.packetFormat));
  lines.push_back("Len: " + lengthConfigName(cfg.lengthConfig) +
                  " / " + String(cfg.packetLength));
  lines.push_back("CRC: " + boolLabel(cfg.crcEnabled));
  lines.push_back("Whitening: " + boolLabel(cfg.whitening));
  lines.push_back("Manchester: " + boolLabel(cfg.manchester));

  String rssiErr;
  const int rssi = readCc1101RssiDbm(&rssiErr);
  if (rssiErr.isEmpty()) {
    lines.push_back("RSSI: " + String(rssi) + " dBm");
  }

  ctx.uiRuntime->showInfo("RF Info", lines, backgroundTick, "OK/BACK Exit");
}

void editFrequency(AppContext &ctx,
                   const std::function<void()> &backgroundTick) {
  String value = String(getCc1101FrequencyMhz(), 2);
  if (!ctx.uiRuntime->textInput("RF Frequency MHz", value, false, backgroundTick)) {
    return;
  }

  float mhz = 0.0f;
  if (!parseFloatToken(value, mhz)) {
    ctx.uiRuntime->showToast("RF", "Invalid frequency", 1300, backgroundTick);
    return;
  }

  setCc1101FrequencyMhz(mhz);
  ctx.uiRuntime->showToast("RF",
                    "Frequency set " + String(getCc1101FrequencyMhz(), 2) + " MHz",
                    1300,
                    backgroundTick);
}

void chooseModulation(AppContext &ctx,
                      Cc1101PacketConfig &cfg,
                      const std::function<void()> &backgroundTick) {
  std::vector<String> menu;
  menu.push_back("0: 2-FSK");
  menu.push_back("1: GFSK");
  menu.push_back("2: ASK/OOK");
  menu.push_back("3: 4-FSK");
  menu.push_back("4: MSK");

  int selected = cfg.modulation <= 4 ? static_cast<int>(cfg.modulation) : 0;
  const int choice = ctx.uiRuntime->menuLoop("RF / Modulation",
                                       menu,
                                       selected,
                                       backgroundTick,
                                       "OK Select  BACK Exit",
                                       modulationName(cfg.modulation));
  if (choice < 0) {
    return;
  }
  cfg.modulation = static_cast<uint8_t>(choice);
}

void choosePacketFormat(AppContext &ctx,
                        Cc1101PacketConfig &cfg,
                        const std::function<void()> &backgroundTick) {
  std::vector<String> menu;
  menu.push_back("0: FIFO");
  menu.push_back("1: Sync Serial");
  menu.push_back("2: Random TX");
  menu.push_back("3: Async Serial");

  int selected = cfg.packetFormat <= 3 ? static_cast<int>(cfg.packetFormat) : 0;
  const int choice = ctx.uiRuntime->menuLoop("RF / Packet Format",
                                       menu,
                                       selected,
                                       backgroundTick,
                                       "OK Select  BACK Exit",
                                       packetFormatName(cfg.packetFormat));
  if (choice < 0) {
    return;
  }
  cfg.packetFormat = static_cast<uint8_t>(choice);
}

void chooseLengthConfig(AppContext &ctx,
                        Cc1101PacketConfig &cfg,
                        const std::function<void()> &backgroundTick) {
  std::vector<String> menu;
  menu.push_back("0: Fixed");
  menu.push_back("1: Variable");
  menu.push_back("2: Infinite");
  menu.push_back("3: Reserved");

  int selected = cfg.lengthConfig <= 3 ? static_cast<int>(cfg.lengthConfig) : 1;
  const int choice = ctx.uiRuntime->menuLoop("RF / Length Mode",
                                       menu,
                                       selected,
                                       backgroundTick,
                                       "OK Select  BACK Exit",
                                       lengthConfigName(cfg.lengthConfig));
  if (choice < 0) {
    return;
  }
  cfg.lengthConfig = static_cast<uint8_t>(choice);
}

void chooseSyncMode(AppContext &ctx,
                    Cc1101PacketConfig &cfg,
                    const std::function<void()> &backgroundTick) {
  std::vector<String> menu;
  for (int i = 0; i <= 7; ++i) {
    menu.push_back("Sync Mode " + String(i));
  }

  int selected = cfg.syncMode <= 7 ? static_cast<int>(cfg.syncMode) : 2;
  const int choice = ctx.uiRuntime->menuLoop("RF / Sync Mode",
                                       menu,
                                       selected,
                                       backgroundTick,
                                       "OK Select  BACK Exit",
                                       "Current: " + String(cfg.syncMode));
  if (choice < 0) {
    return;
  }
  cfg.syncMode = static_cast<uint8_t>(choice);
}

void editUint8Value(AppContext &ctx,
                    const String &title,
                    uint8_t &target,
                    const std::function<void()> &backgroundTick) {
  String value = String(target);
  if (!ctx.uiRuntime->textInput(title, value, false, backgroundTick)) {
    return;
  }

  int parsed = 0;
  if (!parseIntToken(value, parsed) || parsed < 0 || parsed > 255) {
    ctx.uiRuntime->showToast("RF", "Invalid number", 1200, backgroundTick);
    return;
  }
  target = static_cast<uint8_t>(parsed);
}

void editFloatValue(AppContext &ctx,
                    const String &title,
                    float &target,
                    const std::function<void()> &backgroundTick) {
  String value = String(target, 3);
  if (!ctx.uiRuntime->textInput(title, value, false, backgroundTick)) {
    return;
  }

  float parsed = 0.0f;
  if (!parseFloatToken(value, parsed)) {
    ctx.uiRuntime->showToast("RF", "Invalid number", 1200, backgroundTick);
    return;
  }
  target = parsed;
}

void runPacketProfileMenu(AppContext &ctx,
                          const std::function<void()> &backgroundTick) {
  int selected = 0;
  Cc1101PacketConfig working = getCc1101PacketConfig();

  while (true) {
    std::vector<String> menu;
    menu.push_back("Modulation: " + modulationName(working.modulation));
    menu.push_back("Channel: " + String(working.channel));
    menu.push_back("DataRate: " + String(working.dataRateKbps, 2));
    menu.push_back("Deviation: " + String(working.deviationKHz, 1));
    menu.push_back("RxBW: " + String(working.rxBandwidthKHz, 1));
    menu.push_back("SyncMode: " + String(working.syncMode));
    menu.push_back("PacketFormat: " + packetFormatName(working.packetFormat));
    menu.push_back("LengthMode: " + lengthConfigName(working.lengthConfig));
    menu.push_back("PacketLen: " + String(working.packetLength));
    menu.push_back("CRC: " + boolLabel(working.crcEnabled));
    menu.push_back("Whitening: " + boolLabel(working.whitening));
    menu.push_back("Manchester: " + boolLabel(working.manchester));
    menu.push_back("Apply");
    menu.push_back("Reset Defaults");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("RF / Packet Profile",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        "Edit then Apply");
    if (choice < 0 || choice == 14) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      chooseModulation(ctx, working, backgroundTick);
    } else if (choice == 1) {
      editUint8Value(ctx, "Channel (0..255)", working.channel, backgroundTick);
    } else if (choice == 2) {
      editFloatValue(ctx, "DataRate kbps", working.dataRateKbps, backgroundTick);
    } else if (choice == 3) {
      editFloatValue(ctx, "Deviation kHz", working.deviationKHz, backgroundTick);
    } else if (choice == 4) {
      editFloatValue(ctx, "RxBW kHz", working.rxBandwidthKHz, backgroundTick);
    } else if (choice == 5) {
      chooseSyncMode(ctx, working, backgroundTick);
    } else if (choice == 6) {
      choosePacketFormat(ctx, working, backgroundTick);
    } else if (choice == 7) {
      chooseLengthConfig(ctx, working, backgroundTick);
    } else if (choice == 8) {
      editUint8Value(ctx, "PacketLen (1..255)", working.packetLength, backgroundTick);
    } else if (choice == 9) {
      working.crcEnabled = !working.crcEnabled;
    } else if (choice == 10) {
      working.whitening = !working.whitening;
    } else if (choice == 11) {
      working.manchester = !working.manchester;
    } else if (choice == 12) {
      String err;
      if (!configureCc1101Packet(working, err)) {
        ctx.uiRuntime->showToast("RF Apply",
                          err.isEmpty() ? String("Apply failed") : err,
                          1700,
                          backgroundTick);
      } else {
        ctx.uiRuntime->showToast("RF Apply", "Packet profile applied", 1200, backgroundTick);
      }
    } else if (choice == 13) {
      working = Cc1101PacketConfig{};
      ctx.uiRuntime->showToast("RF", "Default profile loaded", 1200, backgroundTick);
    }
  }
}

void sendPacketText(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  String text;
  if (!ctx.uiRuntime->textInput("Packet Text", text, false, backgroundTick)) {
    return;
  }
  if (text.isEmpty()) {
    ctx.uiRuntime->showToast("RF TX", "Text is empty", 1200, backgroundTick);
    return;
  }

  String delayMs = "25";
  if (!ctx.uiRuntime->textInput("TX Delay ms", delayMs, false, backgroundTick)) {
    return;
  }

  int txDelay = 25;
  if (!parseIntToken(delayMs, txDelay)) {
    ctx.uiRuntime->showToast("RF TX", "Invalid delay", 1200, backgroundTick);
    return;
  }

  String err;
  if (!sendCc1101PacketText(text, txDelay, err)) {
    ctx.uiRuntime->showToast("RF TX",
                      err.isEmpty() ? String("TX failed") : err,
                      1700,
                      backgroundTick);
    return;
  }

  ctx.uiRuntime->showToast("RF TX", "Packet sent", 1000, backgroundTick);
}

void receivePacketOnce(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  String timeoutInput = "5000";
  if (!ctx.uiRuntime->textInput("RX Timeout ms", timeoutInput, false, backgroundTick)) {
    return;
  }

  int timeoutMs = 0;
  if (!parseIntToken(timeoutInput, timeoutMs)) {
    ctx.uiRuntime->showToast("RF RX", "Invalid timeout", 1200, backgroundTick);
    return;
  }

  std::vector<uint8_t> packet;
  int rssi = 0;
  String err;
  if (!receiveCc1101Packet(packet, timeoutMs, &rssi, err)) {
    ctx.uiRuntime->showToast("RF RX",
                      err.isEmpty() ? String("No packet") : err,
                      1600,
                      backgroundTick);
    return;
  }

  std::vector<String> lines;
  lines.push_back("Bytes: " + String(static_cast<unsigned long>(packet.size())));
  lines.push_back("RSSI: " + String(rssi) + " dBm");
  lines.push_back("ASCII: " + trimMiddle(toAsciiPreview(packet), 40));
  lines.push_back("HEX:");
  appendHexLines(packet, lines);

  ctx.uiRuntime->showInfo("RF RX Packet", lines, backgroundTick, "OK/BACK Exit");
}

void readRssi(AppContext &ctx,
              const std::function<void()> &backgroundTick) {
  String err;
  const int rssi = readCc1101RssiDbm(&err);
  if (!err.isEmpty()) {
    ctx.uiRuntime->showToast("RF RSSI", err, 1500, backgroundTick);
    return;
  }

  ctx.uiRuntime->showToast("RF RSSI", String(rssi) + " dBm", 1200, backgroundTick);
}

void sendOok(AppContext &ctx,
             const std::function<void()> &backgroundTick) {
  String codeInput = "0xABCDEF";
  String bitsInput = "24";
  String pulseInput = "350";
  String protoInput = "1";
  String repeatInput = "10";

  if (!ctx.uiRuntime->textInput("OOK Code", codeInput, false, backgroundTick) ||
      !ctx.uiRuntime->textInput("Bits", bitsInput, false, backgroundTick) ||
      !ctx.uiRuntime->textInput("PulseLen", pulseInput, false, backgroundTick) ||
      !ctx.uiRuntime->textInput("Protocol", protoInput, false, backgroundTick) ||
      !ctx.uiRuntime->textInput("Repeat", repeatInput, false, backgroundTick)) {
    return;
  }

  uint32_t code = 0;
  uint32_t bits = 0;
  int pulse = 0;
  int proto = 0;
  int repeat = 0;

  if (!parseUInt32Token(codeInput, code) ||
      !parseUInt32Token(bitsInput, bits) ||
      !parseIntToken(pulseInput, pulse) ||
      !parseIntToken(protoInput, proto) ||
      !parseIntToken(repeatInput, repeat)) {
    ctx.uiRuntime->showToast("OOK TX", "Invalid value", 1300, backgroundTick);
    return;
  }

  String err;
  if (!transmitCc1101(code,
                      static_cast<int>(bits),
                      pulse,
                      proto,
                      repeat,
                      err)) {
    ctx.uiRuntime->showToast("OOK TX",
                      err.isEmpty() ? String("TX failed") : err,
                      1700,
                      backgroundTick);
    return;
  }

  ctx.uiRuntime->showToast("OOK TX", "Signal sent", 1000, backgroundTick);
}

}  // namespace

void runRfApp(AppContext &ctx,
              const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Radio Info");
    menu.push_back("Set Frequency");
    menu.push_back("Packet Profile");
    menu.push_back("Packet TX (Text)");
    menu.push_back("Packet RX (Once)");
    menu.push_back("Read RSSI");
    menu.push_back("OOK TX (RCSwitch)");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("RF",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        isCc1101Ready() ? "CC1101 Ready" : "CC1101 Missing");
    if (choice < 0 || choice == 7) {
      return;
    }

    selected = choice;

    if (!isCc1101Ready()) {
      ctx.uiRuntime->showToast("RF", "CC1101 not initialized", 1500, backgroundTick);
      continue;
    }

    if (choice == 0) {
      showRadioInfo(ctx, backgroundTick);
    } else if (choice == 1) {
      editFrequency(ctx, backgroundTick);
    } else if (choice == 2) {
      runPacketProfileMenu(ctx, backgroundTick);
    } else if (choice == 3) {
      sendPacketText(ctx, backgroundTick);
    } else if (choice == 4) {
      receivePacketOnce(ctx, backgroundTick);
    } else if (choice == 5) {
      readRssi(ctx, backgroundTick);
    } else if (choice == 6) {
      sendOok(ctx, backgroundTick);
    }
  }
}
