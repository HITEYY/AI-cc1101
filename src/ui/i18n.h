#pragma once

#include <Arduino.h>

enum class UiLanguage : uint8_t {
  English = 0,
  Korean = 1,
};

enum class UiTextKey : uint8_t {
  OkSelectBackExit = 0,
  OkBackExit,
  OkBackClose,
  BackExit,
  BackCancel,
  Select,
  Launcher,
  Settings,
  FileExplorer,
  AppMarket,
  Rf,
  Nfc,
  Rfid,
  Nrf24,
  OpenClaw,
  Language,
  English,
  Korean,
  Saved,
  UnsavedChanges,
};

UiLanguage uiLanguageFromConfigCode(const String &code);
const char *uiLanguageCode(UiLanguage lang);
const char *uiLanguageLabel(UiLanguage lang);
const char *uiText(UiLanguage lang, UiTextKey key);

