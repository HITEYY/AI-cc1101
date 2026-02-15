#pragma once

#include <Arduino.h>
#include <SPI.h>

namespace sharedspi {

void prepareChipSelects();
void init();
SPIClass *bus();

}  // namespace sharedspi
