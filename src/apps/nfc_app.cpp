#include "nfc_app.h"

#include <Wire.h>

#include <vector>

#include "../ui/ui_runtime.h"
#include "user_config.h"

#if __has_include(<Adafruit_PN532.h>)
#include <Adafruit_PN532.h>
#define NFC_PN532_AVAILABLE 1
#else
#define NFC_PN532_AVAILABLE 0
#endif

namespace {

#if NFC_PN532_AVAILABLE
Adafruit_PN532 gPn532(USER_NFC_IRQ_PIN, USER_NFC_RESET_PIN, &Wire);
bool gNfcInited = false;
bool gNfcPresent = false;
uint32_t gFirmwareVersion = 0;
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

#if NFC_PN532_AVAILABLE
bool ensureNfcReady(String *errorOut) {
  if (gNfcInited) {
    if (gNfcPresent) {
      return true;
    }
    if (errorOut) {
      *errorOut = "PN532 not detected";
    }
    return false;
  }

  Wire.begin(USER_NFC_I2C_SDA, USER_NFC_I2C_SCL);
  gPn532.begin();
  gFirmwareVersion = gPn532.getFirmwareVersion();
  gNfcPresent = gFirmwareVersion != 0;
  gNfcInited = true;

  if (!gNfcPresent) {
    if (errorOut) {
      *errorOut = "PN532 not detected";
    }
    return false;
  }

  gPn532.SAMConfig();
  return true;
}

void showNfcInfo(AppContext &ctx,
                 const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("PN532 (I2C)");
  lines.push_back("SDA: " + String(USER_NFC_I2C_SDA));
  lines.push_back("SCL: " + String(USER_NFC_I2C_SCL));

  String err;
  if (!ensureNfcReady(&err)) {
    lines.push_back("State: Missing");
    lines.push_back(err.isEmpty() ? String("Check wiring/power") : err);
    ctx.uiRuntime->showInfo("NFC", lines, backgroundTick, "OK/BACK Exit");
    return;
  }

  const uint8_t ic = (gFirmwareVersion >> 24) & 0xFF;
  const uint8_t ver = (gFirmwareVersion >> 16) & 0xFF;
  const uint8_t rev = (gFirmwareVersion >> 8) & 0xFF;
  lines.push_back("State: Ready");
  lines.push_back("IC: " + String(ic, HEX));
  lines.push_back("FW: " + String(ver) + "." + String(rev));
  ctx.uiRuntime->showInfo("NFC", lines, backgroundTick, "OK/BACK Exit");
}

void scanNfcTag(AppContext &ctx,
                const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureNfcReady(&err)) {
    ctx.uiRuntime->showToast("NFC",
                      err.isEmpty() ? String("PN532 not ready") : err,
                      1700,
                      backgroundTick);
    return;
  }

  ctx.uiRuntime->showToast("NFC", "Hold tag near antenna", 900, backgroundTick);

  uint8_t uid[10] = {0};
  uint8_t uidLength = 0;
  const bool ok = gPn532.readPassiveTargetID(PN532_MIFARE_ISO14443A,
                                             uid,
                                             &uidLength,
                                             200);
  if (!ok || uidLength == 0) {
    ctx.uiRuntime->showToast("NFC", "No tag detected", 1200, backgroundTick);
    return;
  }

  std::vector<String> lines;
  lines.push_back("Tag detected");
  lines.push_back("UID Len: " + String(uidLength));
  lines.push_back("UID: " + bytesToHex(uid, uidLength));

  ctx.uiRuntime->showInfo("NFC Tag", lines, backgroundTick, "OK/BACK Exit");
}
#endif

}  // namespace

void runNfcApp(AppContext &ctx,
               const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Module Info");
    menu.push_back("Scan Tag UID");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("NFC",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        "PN532 I2C app");
    if (choice < 0 || choice == 2) {
      return;
    }

    selected = choice;

#if NFC_PN532_AVAILABLE
    if (choice == 0) {
      showNfcInfo(ctx, backgroundTick);
    } else if (choice == 1) {
      scanNfcTag(ctx, backgroundTick);
    }
#else
    (void)choice;
    ctx.uiRuntime->showToast("NFC",
                      "Adafruit_PN532 library missing",
                      1800,
                      backgroundTick);
    return;
#endif
  }
}
