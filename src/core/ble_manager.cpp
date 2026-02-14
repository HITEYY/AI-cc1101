#include "ble_manager.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <string>
#include <cstring>

#if __has_include(<NimBLEExtAdvertising.h>)
#define NIMBLE_V2_PLUS 1
#endif

namespace {

constexpr uint32_t kScanTimeMs = 5000;
constexpr uint32_t kScanTimeSec = 5;
constexpr uint16_t kScanInterval = 100;
constexpr uint16_t kScanWindow = 99;

constexpr uint16_t kAppearanceGenericHid = 0x03C0;
constexpr uint16_t kAppearanceKeyboard = 0x03C1;

constexpr uint16_t kUuidHidService = 0x1812;
constexpr uint16_t kUuidHidBootKeyboardInput = 0x2A22;
constexpr uint16_t kUuidHidReport = 0x2A4D;

bool containsAddress(const std::vector<BleDeviceInfo> &list,
                     const String &address) {
  for (std::vector<BleDeviceInfo>::const_iterator it = list.begin();
       it != list.end();
       ++it) {
    if (it->address.equalsIgnoreCase(address)) {
      return true;
    }
  }
  return false;
}

String safeDeviceName(const String &name, const String &fallbackAddress) {
  if (!name.isEmpty()) {
    return name;
  }
  return fallbackAddress;
}

}  // namespace

void BleManager::begin() {
  String error;
  ensureInitialized(&error);
}

void BleManager::configure(const RuntimeConfig &config) {
  const String prevSavedAddress = config_.bleDeviceAddress;
  config_ = config;

  if (connected_ && !prevSavedAddress.equalsIgnoreCase(config_.bleDeviceAddress) &&
      !connectedAddress_.equalsIgnoreCase(config_.bleDeviceAddress)) {
    disconnectNow();
  }
}

void BleManager::tick() {
  if (!client_) {
    return;
  }

  if (!client_->isConnected()) {
    if (connected_) {
      connected_ = false;
      connectedRssi_ = 0;
      resetSessionState();
      if (lastError_.isEmpty()) {
        lastError_ = "BLE device disconnected";
      }
    }
    return;
  }

  connected_ = true;
  connectedRssi_ = client_->getRssi();
}

bool BleManager::scanDevices(std::vector<BleDeviceInfo> &outDevices,
                             String *error) {
  outDevices.clear();

  if (!ensureInitialized(error)) {
    return false;
  }

  if (!scan_) {
    if (error) {
      *error = "BLE scanner is unavailable";
    }
    setError("BLE scanner is unavailable");
    return false;
  }

  scanning_ = true;

  if (scan_->isScanning()) {
    scan_->stop();
  }

#ifdef NIMBLE_V2_PLUS
  NimBLEScanResults results = scan_->getResults(kScanTimeMs, false);
  const int count = results.getCount();

  for (int i = 0; i < count; ++i) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (!device) {
      continue;
    }

    BleDeviceInfo info;
    if (!updateDeviceInfoFromAdvertised(device, info)) {
      continue;
    }

    if (containsAddress(outDevices, info.address)) {
      continue;
    }

    outDevices.push_back(info);
  }
#else
  NimBLEScanResults results = scan_->start(kScanTimeSec, false);
  const int count = results.getCount();

  for (int i = 0; i < count; ++i) {
    NimBLEAdvertisedDevice device = results.getDevice(i);

    BleDeviceInfo info;
    if (!updateDeviceInfoFromAdvertised(&device, info)) {
      continue;
    }

    if (containsAddress(outDevices, info.address)) {
      continue;
    }

    outDevices.push_back(info);
  }
#endif

  std::sort(outDevices.begin(),
            outDevices.end(),
            [](const BleDeviceInfo &a, const BleDeviceInfo &b) {
              if (a.rssi == b.rssi) {
                return a.name < b.name;
              }
              return a.rssi > b.rssi;
            });

  scan_->clearResults();
  scanning_ = false;

  if (outDevices.empty()) {
    setError("No BLE devices found");
  } else {
    setError("");
  }

  return true;
}

bool BleManager::connectToDevice(const String &address,
                                 const String &name,
                                 String *error) {
  if (!ensureInitialized(error)) {
    return false;
  }

  if (address.isEmpty()) {
    if (error) {
      *error = "BLE address is empty";
    }
    setError("BLE address is empty");
    return false;
  }

  if (scan_ && scan_->isScanning()) {
    scan_->stop();
  }

  disconnectNow();

  NimBLEClient *nextClient = NimBLEDevice::createClient();
  if (!nextClient) {
    if (error) {
      *error = "Failed to allocate BLE client";
    }
    setError("Failed to allocate BLE client");
    return false;
  }

  nextClient->setConnectTimeout(5);

  const std::string addressStr(address.c_str());
  const NimBLEAddress publicAddress(addressStr, BLE_ADDR_PUBLIC);
  bool connected = nextClient->connect(publicAddress);

  if (!connected) {
    const NimBLEAddress randomAddress(addressStr, BLE_ADDR_RANDOM);
    connected = nextClient->connect(randomAddress);
  }

  if (!connected) {
    NimBLEDevice::deleteClient(nextClient);
    if (error) {
      *error = "BLE connect failed";
    }
    setError("BLE connect failed");
    return false;
  }

  client_ = nextClient;
  connected_ = true;
  connectedAddress_ = address;
  connectedName_ = name.isEmpty() ? connectedAddress_ : name;
  connectedRssi_ = client_->getRssi();

  analyzeConnectedProfile();

  if (connectedIsKeyboard_) {
    setError("BLE keyboard connected");
  } else if (connectedLikelyAudio_) {
    pairingHint_ = "Audio streaming is unsupported on ESP32-S3 BLE stack";
    setError("Connected, but audio stream profile is unsupported");
  } else if (connectedIsHid_) {
    setError("HID device connected");
  } else {
    setError("");
  }

  return true;
}

void BleManager::disconnectNow() {
  if (client_) {
    if (client_->isConnected()) {
      client_->disconnect();
    }
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  connected_ = false;
  connectedRssi_ = 0;
  connectedName_ = "";
  connectedAddress_ = "";
  resetSessionState();
}

void BleManager::clearKeyboardInput() {
  keyboardInputBuffer_ = "";
}

String BleManager::keyboardInputText() const {
  return keyboardInputBuffer_;
}

bool BleManager::isConnected() const {
  return connected_;
}

String BleManager::lastError() const {
  return lastError_;
}

BleStatus BleManager::status() const {
  BleStatus state;
  state.initialized = initialized_;
  state.scanning = scanning_;
  state.connected = connected_;
  state.deviceName = connected_ ? connectedName_ : config_.bleDeviceName;
  state.deviceAddress = connected_ ? connectedAddress_ : config_.bleDeviceAddress;
  state.rssi = connectedRssi_;
  state.profile = connectedProfile_;
  state.hidDevice = connectedIsHid_;
  state.hidKeyboard = connectedIsKeyboard_;
  state.likelyAudio = connectedLikelyAudio_;
  state.keyboardText = keyboardInputBuffer_;
  state.pairingHint = pairingHint_;
  state.lastError = lastError_;
  return state;
}

bool BleManager::ensureInitialized(String *error) {
  if (initialized_) {
    return true;
  }

  NimBLEDevice::init("");
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  NimBLEDevice::setSecurityPasskey(123456);

  scan_ = NimBLEDevice::getScan();
  if (!scan_) {
    if (error) {
      *error = "Failed to initialize BLE scanner";
    }
    setError("Failed to initialize BLE scanner");
    return false;
  }

  scan_->setActiveScan(true);
  scan_->setInterval(kScanInterval);
  scan_->setWindow(kScanWindow);

  initialized_ = true;
  return true;
}

void BleManager::setError(const String &message) {
  lastError_ = message;
}

void BleManager::analyzeConnectedProfile() {
  resetSessionState();

  connectedLikelyAudio_ = detectLikelyAudioByName(connectedName_);
  connectedProfile_ = buildProfileLabel(false, false, connectedLikelyAudio_);

  if (!client_ || !client_->isConnected()) {
    return;
  }

  NimBLERemoteService *hidService = client_->getService(NimBLEUUID(kUuidHidService));
  if (!hidService) {
    return;
  }

  connectedIsHid_ = true;
  connectedIsKeyboard_ = subscribeKeyboardInput();
  connectedProfile_ = buildProfileLabel(connectedIsHid_,
                                        connectedIsKeyboard_,
                                        connectedLikelyAudio_);

  if (!connectedIsKeyboard_ && pairingHint_.isEmpty()) {
    pairingHint_ = "HID connected but no keyboard input report found";
  }
}

bool BleManager::subscribeKeyboardInput() {
  if (!client_ || !client_->isConnected()) {
    return false;
  }

  NimBLERemoteService *hidService = client_->getService(NimBLEUUID(kUuidHidService));
  if (!hidService) {
    return false;
  }

  NimBLERemoteCharacteristic *characteristics[2] = {
      hidService->getCharacteristic(NimBLEUUID(kUuidHidBootKeyboardInput)),
      hidService->getCharacteristic(NimBLEUUID(kUuidHidReport)),
  };

  for (size_t i = 0; i < 2; ++i) {
    NimBLERemoteCharacteristic *chr = characteristics[i];
    if (!chr) {
      continue;
    }

    if (!chr->canNotify() && !chr->canIndicate()) {
      continue;
    }

    const bool useNotify = chr->canNotify();
    const bool ok = chr->subscribe(
        useNotify,
        [this](NimBLERemoteCharacteristic *, uint8_t *pData, size_t length, bool) {
          handleKeyboardReport(pData, length);
        });

    if (ok) {
      std::memset(lastKeyboardKeys_, 0, sizeof(lastKeyboardKeys_));
      pairingHint_ = "";
      return true;
    }
  }

  pairingHint_ = "If pairing is requested, enter passkey 123456 on keyboard";
  return false;
}

void BleManager::handleKeyboardReport(const uint8_t *data, size_t length) {
  if (!data || length < 8) {
    return;
  }

  // Some HID reports prepend report-id; use trailing boot keyboard payload.
  const uint8_t *report = data;
  if (length > 8) {
    report = data + (length - 8);
  }

  const uint8_t modifier = report[0];
  const bool shift = (modifier & 0x22) != 0;

  uint8_t currentKeys[6] = {0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < 6; ++i) {
    currentKeys[i] = report[2 + i];
  }

  for (size_t i = 0; i < 6; ++i) {
    const uint8_t keyCode = currentKeys[i];
    if (keyCode == 0) {
      continue;
    }
    if (containsKeyCode(lastKeyboardKeys_, 6, keyCode)) {
      continue;
    }

    if (keyCode == 42) {  // backspace
      if (keyboardInputBuffer_.length() > 0) {
        keyboardInputBuffer_.remove(keyboardInputBuffer_.length() - 1);
      }
      continue;
    }

    char out = translateKeyboardHidCode(keyCode, shift);
    if (out != 0) {
      keyboardInputBuffer_ += out;
    }
  }

  std::memcpy(lastKeyboardKeys_, currentKeys, sizeof(lastKeyboardKeys_));

  constexpr size_t kMaxKeyboardBuffer = 256;
  if (keyboardInputBuffer_.length() > kMaxKeyboardBuffer) {
    keyboardInputBuffer_.remove(0, keyboardInputBuffer_.length() - kMaxKeyboardBuffer);
  }
}

char BleManager::translateKeyboardHidCode(uint8_t keyCode, bool shift) const {
  if (keyCode >= 4 && keyCode <= 29) {
    char base = static_cast<char>('a' + (keyCode - 4));
    if (shift) {
      base = static_cast<char>(base - ('a' - 'A'));
    }
    return base;
  }

  if (keyCode >= 30 && keyCode <= 39) {
    static const char kNoShiftDigits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    static const char kShiftDigits[] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
    const size_t idx = static_cast<size_t>(keyCode - 30);
    return shift ? kShiftDigits[idx] : kNoShiftDigits[idx];
  }

  switch (keyCode) {
    case 40:
      return '\n';
    case 43:
      return '\t';
    case 44:
      return ' ';
    case 45:
      return shift ? '_' : '-';
    case 46:
      return shift ? '+' : '=';
    case 47:
      return shift ? '{' : '[';
    case 48:
      return shift ? '}' : ']';
    case 49:
      return shift ? '|' : '\\';
    case 51:
      return shift ? ':' : ';';
    case 52:
      return shift ? '"' : '\'';
    case 53:
      return shift ? '~' : '`';
    case 54:
      return shift ? '<' : ',';
    case 55:
      return shift ? '>' : '.';
    case 56:
      return shift ? '?' : '/';
    default:
      return 0;
  }
}

bool BleManager::containsKeyCode(const uint8_t *arr, size_t len, uint8_t code) const {
  if (!arr || len == 0) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    if (arr[i] == code) {
      return true;
    }
  }
  return false;
}

bool BleManager::detectLikelyAudioByName(const String &name) const {
  if (name.isEmpty()) {
    return false;
  }

  String lower = name;
  lower.toLowerCase();

  return lower.indexOf("ear") >= 0 ||
         lower.indexOf("bud") >= 0 ||
         lower.indexOf("headset") >= 0 ||
         lower.indexOf("speaker") >= 0 ||
         lower.indexOf("audio") >= 0 ||
         lower.indexOf("mic") >= 0;
}

String BleManager::buildProfileLabel(bool hid,
                                     bool keyboard,
                                     bool likelyAudio) const {
  if (keyboard) {
    return "HID Keyboard";
  }
  if (hid) {
    return "HID Device";
  }
  if (likelyAudio) {
    return "Audio-like BLE";
  }
  return "Generic BLE";
}

void BleManager::resetSessionState() {
  connectedProfile_ = "";
  connectedIsHid_ = false;
  connectedIsKeyboard_ = false;
  connectedLikelyAudio_ = false;
  pairingHint_ = "";
  std::memset(lastKeyboardKeys_, 0, sizeof(lastKeyboardKeys_));
}

bool BleManager::updateDeviceInfoFromAdvertised(const NimBLEAdvertisedDevice *device,
                                                BleDeviceInfo &info) const {
  if (!device) {
    return false;
  }

  const String address = String(device->getAddress().toString().c_str());
  if (address.isEmpty()) {
    return false;
  }

  const String name = safeDeviceName(String(device->getName().c_str()), address);
  const bool hasHidService = device->isAdvertisingService(NimBLEUUID(kUuidHidService));

  bool appearsHid = false;
  bool appearsKeyboard = false;
  if (device->haveAppearance()) {
    const uint16_t appearance = device->getAppearance();
    appearsKeyboard = appearance == kAppearanceKeyboard;
    appearsHid = (appearance >= kAppearanceGenericHid &&
                  appearance < (kAppearanceGenericHid + 16));
  }

  String lowerName = name;
  lowerName.toLowerCase();
  const bool nameKeyboard = lowerName.indexOf("kbd") >= 0 ||
                            lowerName.indexOf("keyboard") >= 0;

  const bool isKeyboard = appearsKeyboard || (hasHidService && nameKeyboard);
  const bool isHid = hasHidService || appearsHid || isKeyboard;
  const bool isLikelyAudio = detectLikelyAudioByName(name);

  info.name = name;
  info.address = address;
  info.rssi = device->getRSSI();
  info.isHid = isHid;
  info.isKeyboard = isKeyboard;
  info.isLikelyAudio = isLikelyAudio;
  info.profile = buildProfileLabel(isHid, isKeyboard, isLikelyAudio);
  return true;
}
