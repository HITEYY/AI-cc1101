#include "ui_runtime.h"

#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>

#include <time.h>

#include "../core/board_pins.h"
#include "input_adapter.h"
#include "lvgl_port.h"
#include "user_config.h"

namespace {

constexpr int kHeaderHeight = 22;
constexpr int kSubtitleHeight = 18;
constexpr int kFooterHeight = 14;
constexpr int kRowHeight = 18;
constexpr int kSidePadding = 4;
constexpr int kMinContentHeight = 24;

constexpr unsigned long kHeaderRefreshMs = 1000UL;
constexpr unsigned long kBatteryPollMs = 5000UL;
constexpr unsigned long kNtpRetryMs = 30000UL;

int wrapIndex(int value, int count) {
  if (count <= 0) {
    return 0;
  }
  while (value < 0) {
    value += count;
  }
  while (value >= count) {
    value -= count;
  }
  return value;
}

String maskIfNeeded(const String &value, bool mask) {
  if (!mask) {
    return value;
  }
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    out += "*";
  }
  return out;
}

String formatUptimeClock(unsigned long ms) {
  const unsigned long totalSec = ms / 1000UL;
  const unsigned long hours = (totalSec / 3600UL) % 24UL;
  const unsigned long mins = (totalSec / 60UL) % 60UL;

  char buf[6];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", hours, mins);
  return String(buf);
}

}  // namespace

class UiRuntime::Impl {
 public:
  LvglPort port;
  InputAdapter input;

  String statusLine;
  UiLanguage language = UiLanguage::English;

  String headerTime;
  String headerStatus;
  int batteryPct = -1;
  bool ntpStarted = false;
  unsigned long lastNtpAttemptMs = 0;
  unsigned long lastBatteryPollMs = 0;
  unsigned long lastHeaderUpdateMs = 0;

  lv_obj_t *progressOverlay = nullptr;
  lv_obj_t *progressPanel = nullptr;
  lv_obj_t *progressTitle = nullptr;
  lv_obj_t *progressMessage = nullptr;
  lv_obj_t *progressSpinner = nullptr;
  lv_obj_t *progressBar = nullptr;
  lv_obj_t *progressPercent = nullptr;

  bool begin() {
    if (!port.begin()) {
      return false;
    }

    input.begin(port.display());
    applyTheme();
    return true;
  }

  void applyTheme() {
    lv_theme_t *theme = lv_theme_default_init(port.display(),
                                              lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_GREY),
                                              true,
                                              &lv_font_montserrat_14);
    lv_display_set_theme(port.display(), theme);
  }

  const lv_font_t *font() const {
    if (language == UiLanguage::Korean) {
      return &lv_font_source_han_sans_sc_14_cjk;
    }
    return &lv_font_montserrat_14;
  }

  void service(const std::function<void()> *backgroundTick = nullptr) {
    if (backgroundTick && *backgroundTick) {
      (*backgroundTick)();
    }

    input.tick();
    port.pump();
  }

  UiEvent pollInput() {
    const InputEvent ev = input.pollEvent();
    UiEvent out;
    out.delta = ev.delta;
    out.ok = ev.ok;
    out.back = ev.back;
    return out;
  }

  int readBatteryPercent() {
#if USER_BATTERY_GAUGE_ENABLED
    static bool wireReady = false;
    if (!wireReady) {
      Wire.begin(USER_BATTERY_GAUGE_SDA, USER_BATTERY_GAUGE_SCL);
      Wire.setTimeOut(5);
      wireReady = true;
    }

    Wire.beginTransmission(USER_BATTERY_GAUGE_ADDR);
    Wire.write(USER_BATTERY_GAUGE_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      return -1;
    }

    const int readCount = Wire.requestFrom(static_cast<int>(USER_BATTERY_GAUGE_ADDR), 2);
    if (readCount < 2) {
      return -1;
    }

    const uint8_t lo = static_cast<uint8_t>(Wire.read());
    const uint8_t hi = static_cast<uint8_t>(Wire.read());
    const int pct = (static_cast<int>(hi) << 8) | static_cast<int>(lo);
    if (pct < 0 || pct > 100) {
      return -1;
    }
    return pct;
#else
    return -1;
#endif
  }

  void updateHeaderIndicators() {
    const unsigned long now = millis();
    if (now - lastHeaderUpdateMs < kHeaderRefreshMs) {
      return;
    }
    lastHeaderUpdateMs = now;

    if (WiFi.status() == WL_CONNECTED && !ntpStarted &&
        now - lastNtpAttemptMs >= kNtpRetryMs) {
      lastNtpAttemptMs = now;
      configTzTime(USER_TIMEZONE_TZ, USER_NTP_SERVER_1, USER_NTP_SERVER_2);
      ntpStarted = true;
    }

    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 1)) {
      char buf[6];
      snprintf(buf, sizeof(buf), "%02d:%02d", timeInfo.tm_hour, timeInfo.tm_min);
      headerTime = String(buf);
    } else {
      headerTime = formatUptimeClock(now);
    }

    if (now - lastBatteryPollMs >= kBatteryPollMs || batteryPct < 0) {
      lastBatteryPollMs = now;
      batteryPct = readBatteryPercent();
    }

    String status = "W:";
    if (WiFi.status() == WL_CONNECTED) {
      status += String(WiFi.RSSI());
    } else {
      status += "--";
    }
    status += " B:";
    if (batteryPct >= 0) {
      status += String(batteryPct);
      status += "%";
    } else {
      status += "--";
    }
    headerStatus = status;
  }

  void setLabelFont(lv_obj_t *obj) const {
    lv_obj_set_style_text_font(obj, font(), 0);
    lv_obj_set_style_text_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
  }

  void disableScroll(lv_obj_t *obj) const {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
  }

  void setSingleLineLabel(lv_obj_t *label,
                          int width,
                          lv_text_align_t align = LV_TEXT_ALIGN_LEFT) const {
    setLabelFont(label);
    if (width < 1) {
      width = 1;
    }
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, align, 0);
  }

  void setWrapLabel(lv_obj_t *label, int width, int height = -1) const {
    setLabelFont(label);
    if (width < 1) {
      width = 1;
    }
    lv_obj_set_width(label, width);
    if (height > 0) {
      lv_obj_set_height(label, height);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  }

  void clearProgressHandles() {
    progressOverlay = nullptr;
    progressPanel = nullptr;
    progressTitle = nullptr;
    progressMessage = nullptr;
    progressSpinner = nullptr;
    progressBar = nullptr;
    progressPercent = nullptr;
  }

  void renderBase(const String &title,
                  const String &subtitle,
                  const String &footer,
                  int &contentTop,
                  int &contentBottom) {
    updateHeaderIndicators();

    lv_obj_t *screen = lv_screen_active();
    clearProgressHandles();
    lv_obj_clean(screen);
    disableScroll(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screen, LV_OPA_COVER, 0);
    setLabelFont(screen);

    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());
    const int innerW = w - (kSidePadding * 2);

    lv_obj_t *header = lv_obj_create(screen);
    disableScroll(header);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, w, kHeaderHeight);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x00353F), 0);

    lv_obj_t *timeLabel = lv_label_create(header);
    int timeWidth = innerW / 3;
    if (timeWidth < 42) {
      timeWidth = 42;
    }
    if (timeWidth > 72) {
      timeWidth = 72;
    }
    if (timeWidth > innerW - 28) {
      timeWidth = innerW - 28;
    }
    if (timeWidth < 16) {
      timeWidth = 16;
    }
    setSingleLineLabel(timeLabel, timeWidth, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(timeLabel, headerTime.length() > 0 ? headerTime.c_str() : "--:--");
    lv_obj_set_style_text_color(timeLabel, lv_color_white(), 0);
    lv_obj_set_pos(timeLabel, kSidePadding, 1);

    lv_obj_t *statusLabel = lv_label_create(header);
    int statusWidth = innerW - timeWidth - 4;
    if (statusWidth < 12) {
      statusWidth = 12;
    }
    setSingleLineLabel(statusLabel, statusWidth, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(statusLabel, headerStatus.c_str());
    lv_obj_set_style_text_color(statusLabel, lv_color_white(), 0);
    lv_obj_set_pos(statusLabel, w - kSidePadding - statusWidth, 1);

    lv_obj_t *titleLabel = lv_label_create(header);
    setSingleLineLabel(titleLabel, innerW, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(titleLabel, title.c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
    lv_obj_set_pos(titleLabel, kSidePadding, kHeaderHeight - 14);

    int y = kHeaderHeight;
    if (subtitle.length() > 0) {
      lv_obj_t *sub = lv_obj_create(screen);
      disableScroll(sub);
      lv_obj_remove_style_all(sub);
      lv_obj_set_pos(sub, 0, y);
      lv_obj_set_size(sub, w, kSubtitleHeight);
      lv_obj_set_style_bg_color(sub, lv_color_hex(0x001112), 0);

      lv_obj_t *subLabel = lv_label_create(sub);
      setSingleLineLabel(subLabel, innerW, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(subLabel, subtitle.c_str());
      lv_obj_set_style_text_color(subLabel, lv_color_hex(0x65E7FF), 0);
      lv_obj_set_pos(subLabel, kSidePadding, 1);
      y += kSubtitleHeight;
    }

    int footerY = h - kFooterHeight;

    lv_obj_t *foot = lv_obj_create(screen);
    disableScroll(foot);
    lv_obj_remove_style_all(foot);
    lv_obj_set_pos(foot, 0, h - kFooterHeight);
    lv_obj_set_size(foot, w, kFooterHeight);
    lv_obj_set_style_bg_color(foot, lv_color_hex(0x001E5C), 0);

    lv_obj_t *footLabel = lv_label_create(foot);
    setSingleLineLabel(footLabel, innerW, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(footLabel, footer.c_str());
    lv_obj_set_style_text_color(footLabel, lv_color_white(), 0);
    lv_obj_set_pos(footLabel, kSidePadding, 0);

    contentTop = y + 2;
    contentBottom = footerY - 2;
    if (contentBottom > h - kFooterHeight - 2) {
      contentBottom = h - kFooterHeight - 2;
    }
    if (contentBottom < contentTop + kMinContentHeight) {
      contentBottom = contentTop + kMinContentHeight;
      if (contentBottom > h - kFooterHeight - 2) {
        contentBottom = h - kFooterHeight - 2;
      }
    }
    if (contentBottom < contentTop) {
      contentBottom = contentTop;
    }
  }

  void renderMenu(const String &title,
                  const std::vector<String> &items,
                  int selected,
                  const String &subtitle,
                  const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, subtitle, footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int usableHeight = contentBottom - contentTop + 1;
    if (usableHeight < 1) {
      usableHeight = 1;
    }
    int rowHeight = kRowHeight;
    if (usableHeight < rowHeight) {
      rowHeight = usableHeight;
    }
    if (rowHeight < 14 && usableHeight >= 14) {
      rowHeight = 14;
    }

    int maxRows = usableHeight / rowHeight;
    if (maxRows < 1) {
      maxRows = 1;
    }

    int start = selected - (maxRows / 2);
    if (start < 0) {
      start = 0;
    }
    if (start + maxRows > static_cast<int>(items.size())) {
      start = static_cast<int>(items.size()) - maxRows;
      if (start < 0) {
        start = 0;
      }
    }

    for (int row = 0; row < maxRows; ++row) {
      const int index = start + row;
      const int y = contentTop + row * rowHeight;
      if (index < 0 || index >= static_cast<int>(items.size())) {
        continue;
      }

      lv_obj_t *btn = lv_obj_create(lv_screen_active());
      disableScroll(btn);
      lv_obj_set_pos(btn, 2, y);
      lv_obj_set_size(btn, w - 4, rowHeight - 1);
      lv_obj_set_style_radius(btn, 0, 0);
      lv_obj_set_style_border_width(btn, 0, 0);

      const bool isSelected = index == selected;
      lv_obj_set_style_bg_color(btn,
                                isSelected ? lv_color_hex(0xFFCC33) : lv_color_hex(0x000000),
                                0);

      lv_obj_t *label = lv_label_create(btn);
      setSingleLineLabel(label, w - 14, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(label, items[static_cast<size_t>(index)].c_str());
      lv_obj_set_style_text_color(label,
                                  isSelected ? lv_color_hex(0x000000) : lv_color_white(),
                                  0);
      int labelY = (rowHeight - 16) / 2;
      if (labelY < 0) {
        labelY = 0;
      }
      lv_obj_set_pos(label, 4, labelY);
    }

    service(nullptr);
  }

  void renderLauncher(const String &title,
                      const std::vector<String> &items,
                      int selected) {
    updateHeaderIndicators();

    lv_obj_t *screen = lv_screen_active();
    clearProgressHandles();
    lv_obj_clean(screen);
    disableScroll(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x070B16), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screen, LV_OPA_COVER, 0);
    setLabelFont(screen);

    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());

    lv_obj_t *ambientTop = lv_obj_create(screen);
    disableScroll(ambientTop);
    lv_obj_remove_style_all(ambientTop);
    lv_obj_set_pos(ambientTop, -12, 0);
    lv_obj_set_size(ambientTop, w + 24, h / 2);
    lv_obj_set_style_bg_color(ambientTop, lv_color_hex(0x13254B), 0);
    lv_obj_set_style_bg_opa(ambientTop, LV_OPA_30, 0);

    lv_obj_t *ambientBottom = lv_obj_create(screen);
    disableScroll(ambientBottom);
    lv_obj_remove_style_all(ambientBottom);
    lv_obj_set_pos(ambientBottom, 0, h / 2);
    lv_obj_set_size(ambientBottom, w, h / 2);
    lv_obj_set_style_bg_color(ambientBottom, lv_color_hex(0x04070C), 0);
    lv_obj_set_style_bg_opa(ambientBottom, LV_OPA_COVER, 0);

    lv_obj_t *titleLabel = lv_label_create(screen);
    int timeWidth = w / 3;
    if (timeWidth < 44) {
      timeWidth = 44;
    }
    if (timeWidth > 70) {
      timeWidth = 70;
    }
    if (timeWidth > w - 48) {
      timeWidth = w - 48;
    }
    if (timeWidth < 16) {
      timeWidth = 16;
    }
    int titleWidth = w - 18 - timeWidth;
    if (titleWidth < 20) {
      titleWidth = w - 16;
    }
    setSingleLineLabel(titleLabel, titleWidth, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(titleLabel, title.c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xA8CBFF), 0);
    lv_obj_set_pos(titleLabel, 8, 4);

    lv_obj_t *timeLabel = lv_label_create(screen);
    setSingleLineLabel(timeLabel, timeWidth, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(timeLabel, headerTime.length() > 0 ? headerTime.c_str() : "--:--");
    lv_obj_set_style_text_color(timeLabel, lv_color_hex(0xE8F1FF), 0);
    lv_obj_set_pos(timeLabel, w - timeWidth - 8, 4);

    lv_obj_t *hero = lv_obj_create(screen);
    disableScroll(hero);
    const int heroX = 8;
    const int heroY = 22;
    const int heroW = w - 16;
    int heroH = (h >= 190) ? 68 : 56;
    if (heroY + heroH > h - 62) {
      heroH = h - 62 - heroY;
      if (heroH < 40) {
        heroH = 40;
      }
    }

    lv_obj_set_pos(hero, heroX, heroY);
    lv_obj_set_size(hero, heroW, heroH);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x0E1629), 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_90, 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(0x2B4E8C), 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_radius(hero, 10, 0);

    const String selectedName = items[static_cast<size_t>(selected)];
    lv_obj_t *selectedLabel = lv_label_create(hero);
    setLabelFont(selectedLabel);
    lv_obj_set_width(selectedLabel, heroW - 20);
    lv_label_set_long_mode(selectedLabel, LV_LABEL_LONG_DOT);
    lv_label_set_text(selectedLabel, selectedName.c_str());
    lv_obj_set_style_text_color(selectedLabel, lv_color_white(), 0);
    lv_obj_align(selectedLabel, LV_ALIGN_LEFT_MID, 10, -8);

    lv_obj_t *heroHint = lv_label_create(hero);
    setSingleLineLabel(heroHint, heroW - 20, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(heroHint, "Press OK to Open");
    lv_obj_set_style_text_color(heroHint, lv_color_hex(0x7BD6FF), 0);
    lv_obj_set_pos(heroHint, 10, heroH - 18);

    static const uint32_t kCardPalette[] = {
        0x3764D5, 0x3E8A2E, 0x8A3FC8, 0xD05E1A, 0x287A9F, 0xA13F5F, 0x6D6D20, 0x5A4EC9};
    constexpr int kPaletteCount =
        static_cast<int>(sizeof(kCardPalette) / sizeof(kCardPalette[0]));

    const int cardW = (w >= 280) ? 72 : 64;
    int cardH = (h >= 180) ? 46 : 40;
    const int cardGap = 12;
    const int cardStep = cardW + cardGap;

    const int footerY = h - 16;
    const int cardsTop = heroY + heroH + 10;
    int cardsBottom = footerY - 6;
    if (cardsBottom <= cardsTop) {
      cardsBottom = cardsTop + 1;
    }
    if (cardsBottom - cardsTop < cardH) {
      cardH = cardsBottom - cardsTop;
    }
    if (cardH < 20) {
      cardH = 20;
    }
    int stripY = cardsTop + ((cardsBottom - cardsTop - cardH) / 2);
    if (stripY < cardsTop) {
      stripY = cardsTop;
    }

    const int centerX = w / 2;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
      const bool isSelected = i == selected;
      const int rel = i - selected;

      const int x = centerX + rel * cardStep - (cardW / 2);
      int y = stripY;
      if (isSelected && y > cardsTop) {
        y -= 2;
      }
      if (x > w + 8 || x + cardW < -8) {
        continue;
      }

      lv_obj_t *card = lv_obj_create(screen);
      disableScroll(card);
      lv_obj_set_pos(card, x, y);
      lv_obj_set_size(card, cardW, cardH);
      lv_obj_set_style_radius(card, 12, 0);
      lv_obj_set_style_border_width(card, isSelected ? 2 : 1, 0);
      lv_obj_set_style_border_color(card,
                                    isSelected ? lv_color_hex(0xFFE08A) : lv_color_hex(0x243756),
                                    0);
      lv_obj_set_style_bg_color(card, lv_color_hex(kCardPalette[i % kPaletteCount]), 0);
      lv_obj_set_style_bg_opa(card, isSelected ? LV_OPA_COVER : LV_OPA_70, 0);

      lv_obj_t *cardLabel = lv_label_create(card);
      setLabelFont(cardLabel);
      lv_obj_set_width(cardLabel, cardW - 8);
      lv_label_set_long_mode(cardLabel, LV_LABEL_LONG_DOT);
      lv_label_set_text(cardLabel, items[static_cast<size_t>(i)].c_str());
      lv_obj_set_style_text_align(cardLabel, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(cardLabel, lv_color_white(), 0);
      lv_obj_center(cardLabel);
    }

    lv_obj_t *footerHint = lv_label_create(screen);
    setSingleLineLabel(footerHint, w - 12, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(footerHint, "ROT Select  OK Open  BACK Exit");
    lv_obj_set_style_text_color(footerHint, lv_color_hex(0x7D95B8), 0);
    lv_obj_set_pos(footerHint, 6, h - 16);

    service(nullptr);
  }

  void renderInfo(const String &title,
                  const std::vector<String> &lines,
                  int start,
                  const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, "", footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int usableHeight = contentBottom - contentTop + 1;
    if (usableHeight < 1) {
      usableHeight = 1;
    }
    int rowHeight = kRowHeight;
    if (usableHeight < rowHeight) {
      rowHeight = usableHeight;
    }
    if (rowHeight < 14 && usableHeight >= 14) {
      rowHeight = 14;
    }

    int maxRows = usableHeight / rowHeight;
    if (maxRows < 1) {
      maxRows = 1;
    }

    for (int row = 0; row < maxRows; ++row) {
      const int lineIndex = start + row;
      const int y = contentTop + row * rowHeight;
      if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) {
        continue;
      }

      lv_obj_t *holder = lv_obj_create(lv_screen_active());
      disableScroll(holder);
      lv_obj_set_pos(holder, 2, y);
      lv_obj_set_size(holder, w - 4, rowHeight - 1);
      lv_obj_set_style_bg_color(holder, lv_color_hex(0x000000), 0);
      lv_obj_set_style_border_width(holder, 0, 0);
      lv_obj_set_style_radius(holder, 0, 0);

      lv_obj_t *label = lv_label_create(holder);
      setSingleLineLabel(label, w - 14, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(label, lines[static_cast<size_t>(lineIndex)].c_str());
      lv_obj_set_style_text_color(label, lv_color_white(), 0);
      int labelY = (rowHeight - 16) / 2;
      if (labelY < 0) {
        labelY = 0;
      }
      lv_obj_set_pos(label, 4, labelY);
    }

    service(nullptr);
  }

  void renderToast(const String &title,
                   const String &message,
                   const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, "", footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int areaH = contentBottom - contentTop + 1;
    if (areaH < 1) {
      areaH = 1;
    }

    lv_obj_t *box = lv_obj_create(lv_screen_active());
    disableScroll(box);
    int boxW = w - 16;
    if (boxW < 80) {
      boxW = w - 4;
    }
    int boxH = areaH - 8;
    if (boxH < 24) {
      boxH = areaH;
    }
    int boxY = contentTop + (areaH - boxH) / 2;
    lv_obj_set_size(box, boxW, boxH);
    lv_obj_set_pos(box, (w - boxW) / 2, boxY);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2E6BF0), 0);

    lv_obj_t *label = lv_label_create(box);
    setWrapLabel(label, boxW - 14, boxH - 10);
    lv_label_set_text(label, message.c_str());
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    service(nullptr);
  }

  void renderTextInput(const String &title,
                       const String &preview,
                       const std::vector<String> &keyLabels,
                       int selected,
                       int selectedCapsIndex,
                       const std::vector<lv_area_t> &areas) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title,
               preview,
               "ROT Move  OK Key  BACK Cancel",
               contentTop,
               contentBottom);

    const size_t keyCount = keyLabels.size();
    for (size_t i = 0; i < keyCount; ++i) {
      const lv_area_t &a = areas[i];
      lv_obj_t *btn = lv_button_create(lv_screen_active());
      disableScroll(btn);
      lv_obj_set_pos(btn, a.x1, a.y1);
      lv_obj_set_size(btn,
                      static_cast<int32_t>(a.x2 - a.x1 + 1),
                      static_cast<int32_t>(a.y2 - a.y1 + 1));

      bool isSelected = selected == static_cast<int>(i);
      bool isCapsActive = selectedCapsIndex == static_cast<int>(i);

      lv_color_t bg = lv_color_hex(0x3C3C3C);
      lv_color_t fg = lv_color_white();

      if (isCapsActive) {
        bg = lv_color_hex(0x4FB7FF);
        fg = lv_color_black();
      }
      if (isSelected) {
        bg = lv_color_hex(0xFFCC33);
        fg = lv_color_black();
      }

      lv_obj_set_style_bg_color(btn, bg, 0);
      lv_obj_set_style_border_width(btn, 1, 0);
      lv_obj_set_style_border_color(btn, lv_color_hex(0x000000), 0);

      lv_obj_t *label = lv_label_create(btn);
      setSingleLineLabel(label,
                         static_cast<int>(a.x2 - a.x1),
                         LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(label, keyLabels[i].c_str());
      lv_obj_set_style_text_color(label, fg, 0);
      lv_obj_center(label);
    }

    service(nullptr);
  }

  void renderProgressOverlay(const String &title,
                             const String &message,
                             int percent) {
    lv_obj_t *screen = lv_screen_active();
    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());

    int panelW = w - 20;
    if (panelW > 300) {
      panelW = 300;
    }
    if (panelW < 120) {
      panelW = w - 8;
    }
    if (panelW < 80) {
      panelW = w;
    }

    int panelH = h - 24;
    if (panelH > 118) {
      panelH = 118;
    }
    if (panelH < 72) {
      panelH = h - 6;
    }
    if (panelH < 48) {
      panelH = 48;
    }

    const int innerPad = 10;
    const int titleY = 8;
    const int spinnerSize = 22;
    const int messageY = 34;
    const int barY = panelH - 22;
    int messageHeight = panelH - messageY - 16;
    if (messageHeight < 12) {
      messageHeight = 12;
    }
    if (percent >= 0) {
      messageHeight = barY - messageY - 6;
      if (messageHeight < 12) {
        messageHeight = 12;
      }
    }

    const bool needsCreate =
        progressOverlay == nullptr ||
        !lv_obj_is_valid(progressOverlay) ||
        lv_obj_get_parent(progressOverlay) != screen;

    if (needsCreate) {
      clearProgressHandles();

      progressOverlay = lv_obj_create(screen);
      disableScroll(progressOverlay);
      lv_obj_remove_style_all(progressOverlay);
      lv_obj_set_style_bg_color(progressOverlay, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(progressOverlay, LV_OPA_70, 0);
      lv_obj_set_style_border_width(progressOverlay, 0, 0);
      lv_obj_set_style_radius(progressOverlay, 0, 0);
      lv_obj_move_foreground(progressOverlay);

      progressPanel = lv_obj_create(progressOverlay);
      disableScroll(progressPanel);
      lv_obj_set_style_bg_color(progressPanel, lv_color_hex(0x121212), 0);
      lv_obj_set_style_border_color(progressPanel, lv_color_hex(0x2E6BF0), 0);
      lv_obj_set_style_border_width(progressPanel, 1, 0);
      lv_obj_set_style_radius(progressPanel, 6, 0);

      progressTitle = lv_label_create(progressPanel);
      setSingleLineLabel(progressTitle, panelW - 56, LV_TEXT_ALIGN_LEFT);
      lv_obj_set_style_text_color(progressTitle, lv_color_white(), 0);

      progressSpinner = lv_spinner_create(progressPanel);

      progressMessage = lv_label_create(progressPanel);
      setWrapLabel(progressMessage, panelW - (innerPad * 2), messageHeight);
      lv_obj_set_style_text_color(progressMessage, lv_color_white(), 0);

      progressBar = lv_bar_create(progressPanel);
      lv_bar_set_range(progressBar, 0, 100);
      lv_obj_set_style_bg_color(progressBar, lv_color_hex(0x2A2A2A), 0);
      lv_obj_set_style_bg_color(progressBar, lv_color_hex(0x4FB7FF), LV_PART_INDICATOR);

      progressPercent = lv_label_create(progressPanel);
      setSingleLineLabel(progressPercent, 44, LV_TEXT_ALIGN_RIGHT);
      lv_obj_set_style_text_color(progressPercent, lv_color_hex(0xA5E8FF), 0);
    }

    if (progressOverlay && lv_obj_is_valid(progressOverlay)) {
      lv_obj_set_size(progressOverlay, w, h);
      lv_obj_set_pos(progressOverlay, 0, 0);
    }
    if (progressPanel && lv_obj_is_valid(progressPanel)) {
      lv_obj_set_size(progressPanel, panelW, panelH);
      lv_obj_center(progressPanel);
    }
    if (progressTitle && lv_obj_is_valid(progressTitle)) {
      lv_obj_set_width(progressTitle, panelW - 56);
      lv_obj_set_pos(progressTitle, innerPad, titleY);
    }
    if (progressSpinner && lv_obj_is_valid(progressSpinner)) {
      lv_obj_set_size(progressSpinner, spinnerSize, spinnerSize);
      lv_obj_set_pos(progressSpinner, panelW - innerPad - spinnerSize, 6);
    }
    if (progressMessage && lv_obj_is_valid(progressMessage)) {
      lv_obj_set_width(progressMessage, panelW - (innerPad * 2));
      lv_obj_set_height(progressMessage, messageHeight);
      lv_obj_set_pos(progressMessage, innerPad, messageY);
    }
    if (progressBar && lv_obj_is_valid(progressBar)) {
      lv_obj_set_size(progressBar, panelW - (innerPad * 2), 10);
      lv_obj_set_pos(progressBar, innerPad, barY);
    }
    if (progressPercent && lv_obj_is_valid(progressPercent)) {
      lv_obj_set_pos(progressPercent, panelW - innerPad - 44, barY - 16);
    }

    if (progressTitle) {
      lv_label_set_text(progressTitle, title.c_str());
    }
    if (progressMessage) {
      lv_label_set_text(progressMessage, message.c_str());
    }

    if (progressBar && progressPercent) {
      if (percent < 0) {
        lv_obj_add_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(progressPercent, LV_OBJ_FLAG_HIDDEN);
      } else {
        if (percent > 100) {
          percent = 100;
        }
        lv_obj_clear_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(progressPercent, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progressBar, percent, LV_ANIM_OFF);

        char pct[12];
        snprintf(pct, sizeof(pct), "%d%%", percent);
        lv_label_set_text(progressPercent, pct);
      }
    }

    service(nullptr);
  }

  void hideProgressOverlay() {
    if (progressOverlay && lv_obj_is_valid(progressOverlay)) {
      lv_obj_del(progressOverlay);
    }
    clearProgressHandles();
    service(nullptr);
  }
};

UiRuntime::UiRuntime() : impl_(new Impl()) {}

void UiRuntime::begin() {
  if (!impl_->begin()) {
    Serial.println("[ui] runtime begin failed");
    return;
  }

  impl_->service(nullptr);

  int contentTop = 0;
  int contentBottom = 0;
  impl_->renderBase("Boot", "", "", contentTop, contentBottom);

  lv_obj_t *label = lv_label_create(lv_screen_active());
  impl_->setLabelFont(label);
  lv_label_set_text(label, "Booting...");
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  impl_->service(nullptr);
  delay(40);
  impl_->service(nullptr);
}

void UiRuntime::tick() {
  impl_->service(nullptr);
}

UiEvent UiRuntime::pollInput() {
  return impl_->pollInput();
}

void UiRuntime::setStatusLine(const String &line) {
  impl_->statusLine = line;
}

void UiRuntime::setLanguage(UiLanguage language) {
  impl_->language = language;
}

UiLanguage UiRuntime::language() const {
  return impl_->language;
}

int UiRuntime::launcherLoop(const String &title,
                            const std::vector<String> &items,
                            int selectedIndex,
                            const std::function<void()> &backgroundTick) {
  if (items.empty()) {
    return -1;
  }

  int selected = wrapIndex(selectedIndex, static_cast<int>(items.size()));
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderLauncher(title, items, selected);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      selected = wrapIndex(selected + (ev.delta > 0 ? 1 : -1),
                           static_cast<int>(items.size()));
      redraw = true;
    }
    if (ev.ok) {
      return selected;
    }
    if (ev.back) {
      return -1;
    }

    delay(10);
  }
}

int UiRuntime::menuLoop(const String &title,
                        const std::vector<String> &items,
                        int selectedIndex,
                        const std::function<void()> &backgroundTick,
                        const String &footer,
                        const String &subtitle) {
  if (items.empty()) {
    return -1;
  }

  int selected = wrapIndex(selectedIndex, static_cast<int>(items.size()));
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderMenu(title, items, selected, subtitle, footer);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      selected = wrapIndex(selected + (ev.delta > 0 ? 1 : -1),
                           static_cast<int>(items.size()));
      redraw = true;
    }
    if (ev.ok) {
      return selected;
    }
    if (ev.back) {
      return -1;
    }

    delay(10);
  }
}

void UiRuntime::showInfo(const String &title,
                         const std::vector<String> &lines,
                         const std::function<void()> &backgroundTick,
                         const String &footer) {
  int startIndex = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderInfo(title, lines, startIndex, footer);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      int next = startIndex + (ev.delta > 0 ? 1 : -1);
      if (next < 0) {
        next = 0;
      }
      if (next > static_cast<int>(lines.size()) - 1) {
        next = static_cast<int>(lines.size()) - 1;
      }
      if (next < 0) {
        next = 0;
      }
      if (next != startIndex) {
        startIndex = next;
        redraw = true;
      }
    }

    if (ev.ok || ev.back) {
      return;
    }

    delay(10);
  }
}

bool UiRuntime::confirm(const String &title,
                        const String &message,
                        const std::function<void()> &backgroundTick,
                        const String &confirmLabel,
                        const String &cancelLabel) {
  std::vector<String> options;
  options.push_back(confirmLabel);
  options.push_back(cancelLabel);
  const int selected = menuLoop(title,
                                options,
                                1,
                                backgroundTick,
                                "OK Select  BACK Cancel",
                                message);
  return selected == 0;
}

bool UiRuntime::textInput(const String &title,
                          String &inOutValue,
                          bool mask,
                          const std::function<void()> &backgroundTick) {
  struct CharKeyPair {
    char normal;
    char shifted;
  };

  enum class KeyAction : uint8_t {
    Character,
    Done,
    Caps,
    Del,
    Space,
    Cancel,
  };

  struct KeySlot {
    KeyAction action = KeyAction::Character;
    char normal = 0;
    char shifted = 0;
    const char *label = "";
    lv_area_t area{};
  };

  static const CharKeyPair kRow0[] = {
      {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
      {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}};
  static const CharKeyPair kRow1[] = {
      {'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'},
      {'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'[', '{'}, {']', '}'}};
  static const CharKeyPair kRow2[] = {
      {'a', 'A'}, {'s', 'S'}, {'d', 'D'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'},
      {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'}, {'\'', '"'}, {'\\', '|'}};
  static const CharKeyPair kRow3[] = {
      {'z', 'Z'}, {'x', 'X'}, {'c', 'C'}, {'v', 'V'}, {'b', 'B'},
      {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'}};

  String working = inOutValue;
  bool caps = false;
  int selected = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  const int displayWidth = lv_display_get_horizontal_resolution(impl_->port.display());
  const int displayHeight = lv_display_get_vertical_resolution(impl_->port.display());
  const int maxColumns = 12;
  const int keyGap = displayWidth >= 260 ? 2 : 1;
  int keyWidth = (displayWidth - 8 - (keyGap * (maxColumns - 1))) / maxColumns;
  if (keyWidth < 10) {
    keyWidth = 10;
  }

  int fullRowWidth = maxColumns * keyWidth + (maxColumns - 1) * keyGap;
  if (fullRowWidth > displayWidth - 4) {
    keyWidth = (displayWidth - 4 - (keyGap * (maxColumns - 1))) / maxColumns;
    if (keyWidth < 8) {
      keyWidth = 8;
    }
    fullRowWidth = maxColumns * keyWidth + (maxColumns - 1) * keyGap;
  }

  const int contentTop = kHeaderHeight + kSubtitleHeight + 2;
  int contentBottom = displayHeight - kFooterHeight - 2;
  if (contentBottom <= contentTop) {
    contentBottom = contentTop + 60;
  }
  const int availableHeight = contentBottom - contentTop + 1;
  const int rowCount = 5;
  int keyHeight = (availableHeight - (keyGap * (rowCount - 1))) / rowCount;
  if (keyHeight < 12) {
    keyHeight = 12;
  }
  if (keyHeight > 24) {
    keyHeight = 24;
  }
  const int keyboardHeight = rowCount * keyHeight + (rowCount - 1) * keyGap;
  int keyboardTop = contentTop + (availableHeight - keyboardHeight) / 2;
  if (keyboardTop < contentTop) {
    keyboardTop = contentTop;
  }
  int keyboardLeft = (displayWidth - fullRowWidth) / 2;
  if (keyboardLeft < 2) {
    keyboardLeft = 2;
  }
  const bool compactKeyLabels = keyWidth < 16;

  std::vector<KeySlot> keys;
  keys.reserve(64);

  auto addCharRow = [&](const CharKeyPair *row, size_t len, int rowIndex) {
    const int y = keyboardTop + rowIndex * (keyHeight + keyGap);
    const int rowWidth = static_cast<int>(len) * keyWidth +
                         (static_cast<int>(len) - 1) * keyGap;
    int x = (displayWidth - rowWidth) / 2;
    if (x < 2) {
      x = 2;
    }

    for (size_t i = 0; i < len; ++i) {
      KeySlot slot;
      slot.action = KeyAction::Character;
      slot.normal = row[i].normal;
      slot.shifted = row[i].shifted;
      slot.area.x1 = x;
      slot.area.y1 = y;
      slot.area.x2 = x + keyWidth - 1;
      slot.area.y2 = y + keyHeight - 1;
      keys.push_back(slot);
      x += keyWidth + keyGap;
    }
  };

  addCharRow(kRow0, sizeof(kRow0) / sizeof(kRow0[0]), 0);
  addCharRow(kRow1, sizeof(kRow1) / sizeof(kRow1[0]), 1);
  addCharRow(kRow2, sizeof(kRow2) / sizeof(kRow2[0]), 2);
  addCharRow(kRow3, sizeof(kRow3) / sizeof(kRow3[0]), 3);

  const int actionRowY = keyboardTop + (keyHeight + keyGap) * 4;
  static const int kActionUnits[5] = {2, 2, 2, 4, 2};
  static const KeyAction kActionKinds[5] = {
      KeyAction::Done,
      KeyAction::Caps,
      KeyAction::Del,
      KeyAction::Space,
      KeyAction::Cancel};
  static const char *kActionLabelsWide[5] = {"DONE", "CAPS", "DEL", "SPACE", "CANCEL"};
  static const char *kActionLabelsCompact[5] = {"OK", "CAP", "DEL", "SPC", "ESC"};
  const char *const *actionLabels = compactKeyLabels ? kActionLabelsCompact : kActionLabelsWide;

  int actionX = keyboardLeft;
  int capsIndex = -1;
  for (int i = 0; i < 5; ++i) {
    KeySlot slot;
    slot.action = kActionKinds[i];
    slot.label = actionLabels[i];
    const int width = kActionUnits[i] * keyWidth + (kActionUnits[i] - 1) * keyGap;
    slot.area.x1 = actionX;
    slot.area.y1 = actionRowY;
    slot.area.x2 = actionX + width - 1;
    slot.area.y2 = actionRowY + keyHeight - 1;
    if (slot.action == KeyAction::Caps) {
      capsIndex = static_cast<int>(keys.size());
    }
    keys.push_back(slot);
    actionX += width + keyGap;
  }

  auto buildPreview = [&]() -> String {
    String preview = maskIfNeeded(working, mask);
    if (preview.isEmpty()) {
      preview = "(empty)";
    }

    const size_t kMaxPreviewChars = displayWidth >= 260 ? 40 : 24;
    if (preview.length() > kMaxPreviewChars) {
      const size_t tail = kMaxPreviewChars - 3;
      preview = "..." + preview.substring(preview.length() - tail);
    }
    return preview;
  };

  auto labelForKey = [&](const KeySlot &slot) -> String {
    if (slot.action == KeyAction::Character) {
      return String(caps ? slot.shifted : slot.normal);
    }
    if (slot.action == KeyAction::Caps) {
      if (compactKeyLabels) {
        return caps ? "ON" : "CAP";
      }
      return caps ? "CAPS ON" : "CAPS";
    }
    return String(slot.label);
  };

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      std::vector<String> labels;
      labels.reserve(keys.size());
      for (std::vector<KeySlot>::const_iterator it = keys.begin();
           it != keys.end();
           ++it) {
        labels.push_back(labelForKey(*it));
      }

      impl_->renderTextInput(title,
                             buildPreview(),
                             labels,
                             selected,
                             (caps && capsIndex >= 0) ? capsIndex : -1,
                             [&]() {
                               std::vector<lv_area_t> areas;
                               areas.reserve(keys.size());
                               for (size_t i = 0; i < keys.size(); ++i) {
                                 areas.push_back(keys[i].area);
                               }
                               return areas;
                             }());
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      selected = wrapIndex(selected + (ev.delta > 0 ? 1 : -1),
                           static_cast<int>(keys.size()));
      redraw = true;
    }

    if (ev.back) {
      return false;
    }

    if (ev.ok) {
      const KeySlot &slot = keys[static_cast<size_t>(selected)];
      if (slot.action == KeyAction::Character) {
        working += caps ? slot.shifted : slot.normal;
        redraw = true;
      } else if (slot.action == KeyAction::Done) {
        inOutValue = working;
        return true;
      } else if (slot.action == KeyAction::Caps) {
        caps = !caps;
        redraw = true;
      } else if (slot.action == KeyAction::Del) {
        if (working.length() > 0) {
          working.remove(working.length() - 1);
        }
        redraw = true;
      } else if (slot.action == KeyAction::Space) {
        working += " ";
        redraw = true;
      } else if (slot.action == KeyAction::Cancel) {
        return false;
      }
    }

    delay(10);
  }
}

void UiRuntime::showProgressOverlay(const String &title,
                                    const String &message,
                                    int percent) {
  impl_->renderProgressOverlay(title, message, percent);
}

void UiRuntime::hideProgressOverlay() {
  impl_->hideProgressOverlay();
}

void UiRuntime::showToast(const String &title,
                          const String &message,
                          unsigned long showMs,
                          const std::function<void()> &backgroundTick) {
  const unsigned long start = millis();
  unsigned long lastRefreshMs = 0;

  while (true) {
    const unsigned long now = millis();
    if (lastRefreshMs == 0 || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderToast(title, message, uiText(language(), UiTextKey::OkBackClose));
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();
    if (ev.ok || ev.back || now - start >= showMs) {
      return;
    }

    delay(10);
  }
}
