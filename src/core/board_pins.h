#pragma once

#include <stdint.h>

#include "user_config.h"

namespace boardpins {

// LILYGO T-Embed CC1101 (T-Embed-CC1101) shared bus/power pins.
constexpr uint8_t kPowerEnable = 15;

constexpr uint8_t kTftCs = 41;
constexpr uint8_t kTftBacklight = 21;
constexpr uint8_t kSdCs = 13;
constexpr uint8_t kCc1101Cs = 12;

constexpr uint8_t kEncoderA = static_cast<uint8_t>(USER_ENCODER_A_PIN);
constexpr uint8_t kEncoderB = static_cast<uint8_t>(USER_ENCODER_B_PIN);
constexpr uint8_t kEncoderOk = static_cast<uint8_t>(USER_ENCODER_OK_PIN);
constexpr uint8_t kEncoderBack = static_cast<uint8_t>(USER_ENCODER_BACK_PIN);

}  // namespace boardpins
