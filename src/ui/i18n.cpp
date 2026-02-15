#include "i18n.h"

namespace {

const char *textEn(UiTextKey key) {
  switch (key) {
    case UiTextKey::OkSelectBackExit:
      return "OK Select  BACK Exit";
    case UiTextKey::OkBackExit:
      return "OK/BACK Exit";
    case UiTextKey::OkBackClose:
      return "OK/BACK Close";
    case UiTextKey::BackExit:
      return "BACK Exit";
    case UiTextKey::BackCancel:
      return "BACK Cancel";
    case UiTextKey::Select:
      return "Select";
    case UiTextKey::Launcher:
      return "Launcher";
    case UiTextKey::Settings:
      return "Setting";
    case UiTextKey::FileExplorer:
      return "File Explorer";
    case UiTextKey::AppMarket:
      return "APPMarket";
    case UiTextKey::Rf:
      return "RF";
    case UiTextKey::Nfc:
      return "NFC";
    case UiTextKey::Rfid:
      return "RFID";
    case UiTextKey::Nrf24:
      return "NRF24";
    case UiTextKey::OpenClaw:
      return "OpenClaw";
    case UiTextKey::Language:
      return "Language";
    case UiTextKey::English:
      return "English";
    case UiTextKey::Korean:
      return "Korean";
    case UiTextKey::Saved:
      return "Saved";
    case UiTextKey::UnsavedChanges:
      return "Unsaved changes";
    default:
      return "";
  }
}

const char *textKo(UiTextKey key) {
  switch (key) {
    case UiTextKey::OkSelectBackExit:
      return "OK Select  BACK Exit";
    case UiTextKey::OkBackExit:
      return "OK/BACK Exit";
    case UiTextKey::OkBackClose:
      return "OK/BACK Close";
    case UiTextKey::BackExit:
      return "BACK Exit";
    case UiTextKey::BackCancel:
      return "BACK Cancel";
    case UiTextKey::Select:
      return "Select";
    case UiTextKey::Launcher:
      return "Launcher";
    case UiTextKey::Settings:
      return "Setting";
    case UiTextKey::FileExplorer:
      return "File Explorer";
    case UiTextKey::AppMarket:
      return "APPMarket";
    case UiTextKey::Rf:
      return "RF";
    case UiTextKey::Nfc:
      return "NFC";
    case UiTextKey::Rfid:
      return "RFID";
    case UiTextKey::Nrf24:
      return "NRF24";
    case UiTextKey::OpenClaw:
      return "OpenClaw";
    case UiTextKey::Language:
      return "Language";
    case UiTextKey::English:
      return "English";
    case UiTextKey::Korean:
      return "Korean";
    case UiTextKey::Saved:
      return "Saved";
    case UiTextKey::UnsavedChanges:
      return "Unsaved changes";
    default:
      return "";
  }
}

}  // namespace

UiLanguage uiLanguageFromConfigCode(const String &code) {
  String normalized = code;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "ko" || normalized == "korean" || normalized == "kr") {
    return UiLanguage::Korean;
  }
  return UiLanguage::English;
}

const char *uiLanguageCode(UiLanguage lang) {
  return lang == UiLanguage::Korean ? "ko" : "en";
}

const char *uiLanguageLabel(UiLanguage lang) {
  return lang == UiLanguage::Korean ? "Korean" : "English";
}

const char *uiText(UiLanguage lang, UiTextKey key) {
  return lang == UiLanguage::Korean ? textKo(key) : textEn(key);
}
