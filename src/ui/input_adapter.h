#pragma once

#include <Arduino.h>
#include <RotaryEncoder.h>
#include <lvgl.h>

struct InputEvent {
  int delta = 0;
  bool ok = false;
  bool back = false;
  bool okLong = false;
  uint8_t okCount = 0;
  uint8_t backCount = 0;
  uint8_t okLongCount = 0;
};

class InputAdapter {
 public:
  InputAdapter();

  void begin(lv_display_t *display);
  void tick();
  void resetState();
  void setOkBackBlocked(bool blocked);

  InputEvent pollEvent();

  void setGroup(lv_group_t *group);
  lv_indev_t *indev() const;

 private:
  struct KeyNode {
    uint32_t key = 0;
    lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
  };

  static void readCb(lv_indev_t *indev, lv_indev_data_t *data);
  void enqueueKey(uint32_t key, lv_indev_state_t state);
  void enqueueKeyPressRelease(uint32_t key);
  bool dequeueKey(uint32_t &key, lv_indev_state_t &state);

  RotaryEncoder encoder_;
  lv_indev_t *indev_ = nullptr;

  int32_t lastEncoderPos_ = 0;
  int16_t pendingEncDiff_ = 0;

  bool okPrev_ = false;
  bool backPrev_ = false;
  bool okBackBlocked_ = false;
  unsigned long okPressedAt_ = 0;
  unsigned long backPressedAt_ = 0;
  bool okLongFired_ = false;

  InputEvent pendingEvent_;

  static constexpr uint8_t kQueueSize = 32;
  KeyNode keyQueue_[kQueueSize];
  uint8_t keyHead_ = 0;
  uint8_t keyTail_ = 0;
  uint8_t keyCount_ = 0;
  unsigned long lastPinRefreshAt_ = 0;
  unsigned long lastTraceAt_ = 0;
  int lastTraceA_ = -1;
  int lastTraceB_ = -1;
  int lastTraceOk_ = -1;
  int lastTraceBack_ = -1;
  int32_t lastTracePos_ = 0;
  int16_t lastTraceEncDiff_ = 0;
  uint8_t lastTraceQ_ = 0;
};
