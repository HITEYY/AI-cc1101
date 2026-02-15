#include "ui_navigator.h"

#include <vector>

#include "../apps/app_market_app.h"
#include "../apps/file_explorer_app.h"
#include "../apps/nfc_app.h"
#include "../apps/nrf24_app.h"
#include "../apps/openclaw_app.h"
#include "../apps/rf_app.h"
#include "../apps/rfid_app.h"
#include "../apps/settings_app.h"
#include "../core/ble_manager.h"
#include "../core/gateway_client.h"
#include "../core/wifi_manager.h"
#include "i18n.h"
#include "ui_runtime.h"

namespace {

String buildLauncherStatus(const AppContext &ctx) {
  String line;
  line += (ctx.wifi && ctx.wifi->isConnected()) ? "WiFi:UP " : "WiFi:DOWN ";

  if (ctx.gateway) {
    const GatewayStatus gs = ctx.gateway->status();
    line += "GW:";
    line += gs.gatewayReady ? "READY" : (gs.wsConnected ? "WS" : "IDLE");
  } else {
    line += "GW:IDLE";
  }

  line += " BLE:";
  line += (ctx.ble && ctx.ble->isConnected()) ? "CONN" : "IDLE";

  if (ctx.configDirty) {
    line += "  *DIRTY";
  }

  return line;
}

}  // namespace

void UiNavigator::runLauncher(AppContext &ctx,
                              const std::function<void()> &backgroundTick) {
  if (!ctx.uiRuntime) {
    return;
  }

  const UiLanguage lang = ctx.uiRuntime->language();

  std::vector<String> items;
  items.push_back(uiText(lang, UiTextKey::OpenClaw));
  items.push_back(uiText(lang, UiTextKey::Settings));
  items.push_back(uiText(lang, UiTextKey::FileExplorer));
  items.push_back(uiText(lang, UiTextKey::AppMarket));
  items.push_back(uiText(lang, UiTextKey::Rf));
  items.push_back(uiText(lang, UiTextKey::Nfc));
  items.push_back(uiText(lang, UiTextKey::Rfid));
  items.push_back(uiText(lang, UiTextKey::Nrf24));

  ctx.uiRuntime->setStatusLine(buildLauncherStatus(ctx));

  const int choice = ctx.uiRuntime->menuLoop(uiText(lang, UiTextKey::Launcher),
                                             items,
                                             selected_,
                                             backgroundTick,
                                             uiText(lang, UiTextKey::OkSelectBackExit),
                                             "T-Embed CC1101");
  if (choice < 0) {
    return;
  }

  selected_ = choice;
  if (choice == 0) {
    runOpenClawApp(ctx, backgroundTick);
  } else if (choice == 1) {
    runSettingsApp(ctx, backgroundTick);
  } else if (choice == 2) {
    runFileExplorerApp(ctx, backgroundTick);
  } else if (choice == 3) {
    runAppMarketApp(ctx, backgroundTick);
  } else if (choice == 4) {
    runRfApp(ctx, backgroundTick);
  } else if (choice == 5) {
    runNfcApp(ctx, backgroundTick);
  } else if (choice == 6) {
    runRfidApp(ctx, backgroundTick);
  } else if (choice == 7) {
    runNrf24App(ctx, backgroundTick);
  }
}
