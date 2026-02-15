#include "shared_spi_bus.h"

#include "board_pins.h"

namespace {

constexpr uint8_t kSck = 11;
constexpr uint8_t kMiso = 10;
constexpr uint8_t kMosi = 9;

bool gInited = false;

}  // namespace

namespace sharedspi {

void prepareChipSelects() {
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
}

void init() {
  if (gInited) {
    return;
  }

  prepareChipSelects();
  SPI.begin(kSck, kMiso, kMosi);
  gInited = true;
}

SPIClass *bus() {
  init();
  return &SPI;
}

}  // namespace sharedspi
