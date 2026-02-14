#pragma once

#include <functional>

#include "app_context.h"

void runRfApp(AppContext &ctx, const std::function<void()> &backgroundTick);
