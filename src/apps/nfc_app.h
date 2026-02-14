#pragma once

#include <functional>

#include "app_context.h"

void runNfcApp(AppContext &ctx, const std::function<void()> &backgroundTick);
