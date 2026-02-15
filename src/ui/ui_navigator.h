#pragma once

#include <functional>

#include "../apps/app_context.h"

class UiNavigator {
 public:
  void runLauncher(AppContext &ctx, const std::function<void()> &backgroundTick);

 private:
  int selected_ = 0;
};

