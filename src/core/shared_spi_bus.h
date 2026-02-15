#pragma once

#include <Arduino.h>
#include <SPI.h>

namespace sharedspi {

void prepareChipSelects();
void init();
void adoptInitializedBus(SPIClass *externalBus = nullptr);
SPIClass *bus();

}  // namespace sharedspi
