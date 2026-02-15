#include "rfid_app.h"

#include <SPI.h>

#include <vector>

#include <TFT_eSPI.h>

#include "../core/board_pins.h"
#include "../ui/ui_runtime.h"
#include "user_config.h"

#if __has_include(<MFRC522.h>)
#include <MFRC522.h>
#define RFID_RC522_AVAILABLE 1
#else
#define RFID_RC522_AVAILABLE 0
#endif

namespace {

#if RFID_RC522_AVAILABLE
MFRC522 gRfid(USER_RFID_SS_PIN, USER_RFID_RST_PIN);
bool gRfidInited = false;
bool gRfidPresent = false;
uint8_t gVersionReg = 0;
#endif

String bytesToHex(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return "";
  }

  String out;
  char buf[4] = {0};
  for (size_t i = 0; i < len; ++i) {
    if (i > 0) {
      out += ':';
    }
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    out += buf;
  }
  return out;
}

#if RFID_RC522_AVAILABLE
bool ensureRfidReady(String *errorOut) {
  if (gRfidInited) {
    if (gRfidPresent) {
      return true;
    }
    if (errorOut) {
      *errorOut = "RC522 not detected";
    }
    return false;
  }

  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);

  pinMode(USER_RFID_SS_PIN, OUTPUT);
  digitalWrite(USER_RFID_SS_PIN, HIGH);
  pinMode(USER_RFID_RST_PIN, OUTPUT);
  digitalWrite(USER_RFID_RST_PIN, HIGH);

  SPI.begin(11, 10, 9, USER_RFID_SS_PIN);
  delay(10);

  gRfid.PCD_Init();
  delay(30);

  gVersionReg = gRfid.PCD_ReadRegister(MFRC522::VersionReg);
  gRfidPresent = (gVersionReg != 0x00 && gVersionReg != 0xFF);
  gRfidInited = true;

  if (!gRfidPresent) {
    if (errorOut) {
      *errorOut = "RC522 not detected";
    }
    return false;
  }

  return true;
}

String versionLabel(uint8_t versionReg) {
  if (versionReg == 0x91) {
    return "v1.0 (0x91)";
  }
  if (versionReg == 0x92) {
    return "v2.0 (0x92)";
  }
  if (versionReg == 0x88) {
    return "clone (0x88)";
  }
  return String("0x") + String(versionReg, HEX);
}

void showRfidInfo(AppContext &ctx,
                  const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("MFRC522 (SPI)");
  lines.push_back("SCK/MISO/MOSI: 11/10/9");
  lines.push_back("SS: " + String(USER_RFID_SS_PIN));
  lines.push_back("RST: " + String(USER_RFID_RST_PIN));

  String err;
  if (!ensureRfidReady(&err)) {
    lines.push_back("State: Missing");
    lines.push_back(err.isEmpty() ? String("Check wiring/power") : err);
    ctx.uiRuntime->showInfo("RFID", lines, backgroundTick, "OK/BACK Exit");
    return;
  }

  lines.push_back("State: Ready");
  lines.push_back("Version: " + versionLabel(gVersionReg));
  ctx.uiRuntime->showInfo("RFID", lines, backgroundTick, "OK/BACK Exit");
}

void scanRfidTag(AppContext &ctx,
                 const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureRfidReady(&err)) {
    ctx.uiRuntime->showToast("RFID",
                      err.isEmpty() ? String("RC522 not ready") : err,
                      1700,
                      backgroundTick);
    return;
  }

  ctx.uiRuntime->showToast("RFID", "Tap MIFARE card", 900, backgroundTick);

  gRfid.uid.size = 0;
  const unsigned long started = millis();
  while (millis() - started < 800) {
    if (gRfid.PICC_IsNewCardPresent() && gRfid.PICC_ReadCardSerial()) {
      break;
    }
    if (backgroundTick) {
      backgroundTick();
    }
    delay(10);
  }

  if (!gRfid.uid.size) {
    ctx.uiRuntime->showToast("RFID", "No card detected", 1200, backgroundTick);
    return;
  }

  const MFRC522::PICC_Type piccType = gRfid.PICC_GetType(gRfid.uid.sak);

  std::vector<String> lines;
  lines.push_back("Card detected");
  lines.push_back("UID Len: " + String(gRfid.uid.size));
  lines.push_back("UID: " + bytesToHex(gRfid.uid.uidByte, gRfid.uid.size));
  lines.push_back("Type: " + String(gRfid.PICC_GetTypeName(piccType)));
  lines.push_back("SAK: 0x" + String(gRfid.uid.sak, HEX));

  gRfid.PICC_HaltA();
  gRfid.PCD_StopCrypto1();

  ctx.uiRuntime->showInfo("RFID Tag", lines, backgroundTick, "OK/BACK Exit");
}
#endif

}  // namespace

void runRfidApp(AppContext &ctx,
                const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Module Info");
    menu.push_back("Scan Card UID");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("RFID",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        "RC522 SPI app");
    if (choice < 0 || choice == 2) {
      return;
    }

    selected = choice;

#if RFID_RC522_AVAILABLE
    if (choice == 0) {
      showRfidInfo(ctx, backgroundTick);
    } else if (choice == 1) {
      scanRfidTag(ctx, backgroundTick);
    }
#else
    (void)choice;
    ctx.uiRuntime->showToast("RFID",
                      "MFRC522 library missing",
                      1800,
                      backgroundTick);
    return;
#endif
  }
}
