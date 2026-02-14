#pragma once

#include <functional>

#include "app_context.h"

void runRfidApp(AppContext &ctx, const std::function<void()> &backgroundTick);
