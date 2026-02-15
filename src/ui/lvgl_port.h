#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

class LvglPort {
 public:
  LvglPort();

  bool begin();
  void pump();

  lv_display_t *display() const;
  TFT_eSPI &tft();
  bool ready() const;

 private:
  static void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap);
  static void *allocateBuffer(size_t bytes);

  TFT_eSPI tft_;
  lv_display_t *display_ = nullptr;
  lv_color_t *buf1_ = nullptr;
  lv_color_t *buf2_ = nullptr;
  uint32_t lastTickMs_ = 0;
  bool initialized_ = false;
};

