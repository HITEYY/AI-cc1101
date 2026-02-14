#pragma once

#include <functional>

#include "app_context.h"

void runNrf24App(AppContext &ctx, const std::function<void()> &backgroundTick);
