#include "launcher_icons.h"

namespace {

constexpr int kDesignSize = 46;
constexpr int kMainRenderSize = 69;
constexpr int kSideRenderSize = 36;
constexpr int kIconCount = 4;

bool gInitialized = false;

constexpr LauncherIconId kIconUserData[kIconCount] = {
    LauncherIconId::AppMarket,
    LauncherIconId::Settings,
    LauncherIconId::FileExplorer,
    LauncherIconId::OpenClaw,
};

struct DrawCtx {
  lv_layer_t *layer;
  lv_area_t area;
  lv_color_t color;
  int32_t w;
  int32_t h;
  int32_t minSide;
};

int32_t scaleX(const DrawCtx &ctx, int32_t x) {
  return ctx.area.x1 + ((x * ctx.w) + (kDesignSize / 2)) / kDesignSize;
}

int32_t scaleY(const DrawCtx &ctx, int32_t y) {
  return ctx.area.y1 + ((y * ctx.h) + (kDesignSize / 2)) / kDesignSize;
}

int32_t scaleLen(const DrawCtx &ctx, int32_t len) {
  int32_t out = ((len * ctx.minSide) + (kDesignSize / 2)) / kDesignSize;
  if (out < 1) {
    out = 1;
  }
  return out;
}

lv_area_t makeRect(const DrawCtx &ctx, int32_t x, int32_t y, int32_t rw, int32_t rh) {
  lv_area_t area;
  area.x1 = ctx.area.x1 + (x * ctx.w) / kDesignSize;
  area.y1 = ctx.area.y1 + (y * ctx.h) / kDesignSize;
  area.x2 = ctx.area.x1 + ((x + rw) * ctx.w) / kDesignSize - 1;
  area.y2 = ctx.area.y1 + ((y + rh) * ctx.h) / kDesignSize - 1;

  if (area.x2 < area.x1) {
    area.x2 = area.x1;
  }
  if (area.y2 < area.y1) {
    area.y2 = area.y1;
  }
  return area;
}

void drawFilledRect(const DrawCtx &ctx, int32_t x, int32_t y, int32_t rw, int32_t rh) {
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.base.layer = ctx.layer;
  fill.radius = 0;
  fill.opa = LV_OPA_COVER;
  fill.color = ctx.color;

  const lv_area_t area = makeRect(ctx, x, y, rw, rh);
  lv_draw_fill(ctx.layer, &fill, &area);
}

void drawRectBorder(const DrawCtx &ctx, int32_t x, int32_t y, int32_t rw, int32_t rh, int32_t t) {
  lv_draw_border_dsc_t border;
  lv_draw_border_dsc_init(&border);
  border.base.layer = ctx.layer;
  border.radius = 0;
  border.opa = LV_OPA_COVER;
  border.color = ctx.color;
  border.width = scaleLen(ctx, t);
  border.side = LV_BORDER_SIDE_FULL;

  const lv_area_t area = makeRect(ctx, x, y, rw, rh);
  lv_draw_border(ctx.layer, &border, &area);
}

void drawLineSegment(const DrawCtx &ctx,
                     int32_t x0,
                     int32_t y0,
                     int32_t x1,
                     int32_t y1,
                     int32_t t) {
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.base.layer = ctx.layer;
  line.color = ctx.color;
  line.opa = LV_OPA_COVER;
  line.width = scaleLen(ctx, t);
  line.round_start = 1;
  line.round_end = 1;
  line.p1.x = scaleX(ctx, x0);
  line.p1.y = scaleY(ctx, y0);
  line.p2.x = scaleX(ctx, x1);
  line.p2.y = scaleY(ctx, y1);

  lv_draw_line(ctx.layer, &line);
}

void drawCircleOutline(const DrawCtx &ctx, int32_t cx, int32_t cy, int32_t r, int32_t t) {
  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.base.layer = ctx.layer;
  arc.color = ctx.color;
  arc.opa = LV_OPA_COVER;
  arc.width = scaleLen(ctx, t);
  arc.center.x = scaleX(ctx, cx);
  arc.center.y = scaleY(ctx, cy);
  arc.radius = static_cast<uint16_t>(scaleLen(ctx, r));
  arc.start_angle = 0;
  arc.end_angle = 359;
  arc.rounded = 0;

  lv_draw_arc(ctx.layer, &arc);
}

void drawAppMarketIcon(const DrawCtx &ctx) {
  constexpr int cx = 23;
  constexpr int boxW = 24;
  constexpr int boxH = 13;
  constexpr int boxX = cx - (boxW / 2);
  constexpr int boxY = 22;

  drawRectBorder(ctx, boxX, boxY, boxW, boxH, 2);
  drawRectBorder(ctx, boxX + 3, boxY - 5, boxW - 6, 4, 1);

  constexpr int stemTop = 8;
  constexpr int stemBottom = boxY - 2;
  drawLineSegment(ctx, cx, stemTop, cx, stemBottom, 2);

  for (int i = 0; i < 5; ++i) {
    drawFilledRect(ctx, cx - i, stemBottom + i, (i * 2) + 1, 1);
  }
}

void drawSettingsIcon(const DrawCtx &ctx) {
  constexpr int cx = 23;
  constexpr int cy = 23;
  constexpr int outerR = 10;
  constexpr int innerR = 4;
  constexpr int toothLen = 4;
  constexpr int toothW = 4;
  constexpr int diag = 7;

  drawCircleOutline(ctx, cx, cy, outerR, outerR - innerR);
  drawCircleOutline(ctx, cx, cy, outerR, 1);

  drawFilledRect(ctx, cx - (toothW / 2), cy - outerR - toothLen + 1, toothW, toothLen);
  drawFilledRect(ctx, cx - (toothW / 2), cy + outerR, toothW, toothLen);
  drawFilledRect(ctx, cx - outerR - toothLen + 1, cy - (toothW / 2), toothLen, toothW);
  drawFilledRect(ctx, cx + outerR, cy - (toothW / 2), toothLen, toothW);

  drawFilledRect(ctx, cx - diag - (toothW / 2), cy - diag - (toothW / 2), toothW, toothW);
  drawFilledRect(ctx, cx + diag - (toothW / 2), cy - diag - (toothW / 2), toothW, toothW);
  drawFilledRect(ctx, cx - diag - (toothW / 2), cy + diag - (toothW / 2), toothW, toothW);
  drawFilledRect(ctx, cx + diag - (toothW / 2), cy + diag - (toothW / 2), toothW, toothW);
}

void drawFileExplorerIcon(const DrawCtx &ctx) {
  constexpr int fw = 30;
  constexpr int fh = 18;
  constexpr int fx = 8;
  constexpr int fy = 18;

  drawRectBorder(ctx, fx, fy, fw, fh, 2);

  constexpr int tabW = 12;
  constexpr int tabH = 5;
  drawRectBorder(ctx, fx + 2, fy - tabH + 1, tabW, tabH, 1);

  drawFilledRect(ctx, fx + 4, fy + 6, fw - 8, 2);
}

void drawOpenClawIcon(const DrawCtx &ctx) {
  constexpr int cx = 23;
  constexpr int cy = 24;
  constexpr int nodeR = 3;

  constexpr int lx = 12;
  constexpr int ly = 15;
  constexpr int rx = 34;
  constexpr int ry = 15;
  constexpr int bx = cx;
  constexpr int by = 34;

  drawLineSegment(ctx, cx, cy, lx, ly, 2);
  drawLineSegment(ctx, cx, cy, rx, ry, 2);
  drawLineSegment(ctx, cx, cy, bx, by, 2);
  drawLineSegment(ctx, lx, ly, rx, ry, 1);

  drawCircleOutline(ctx, cx, cy, nodeR + 1, 2);
  drawFilledRect(ctx, cx - 1, cy - 1, 3, 3);
  drawCircleOutline(ctx, lx, ly, nodeR, 2);
  drawCircleOutline(ctx, rx, ry, nodeR, 2);
  drawCircleOutline(ctx, bx, by, nodeR, 2);
}

void drawById(const DrawCtx &ctx, LauncherIconId id) {
  switch (id) {
    case LauncherIconId::AppMarket:
      drawAppMarketIcon(ctx);
      break;
    case LauncherIconId::Settings:
      drawSettingsIcon(ctx);
      break;
    case LauncherIconId::FileExplorer:
      drawFileExplorerIcon(ctx);
      break;
    case LauncherIconId::OpenClaw:
      drawOpenClawIcon(ctx);
      break;
    default:
      break;
  }
}

void launcherIconEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
    int32_t *s = static_cast<int32_t *>(lv_event_get_param(e));
    if (s && *s < 2) {
      *s = 2;
    }
    return;
  }

  if (code != LV_EVENT_DRAW_MAIN) {
    return;
  }

  const LauncherIconId *id = static_cast<const LauncherIconId *>(lv_event_get_user_data(e));
  if (!id) {
    return;
  }

  lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  lv_layer_t *layer = lv_event_get_layer(e);
  if (!obj || !layer) {
    return;
  }

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);

  DrawCtx ctx;
  ctx.layer = layer;
  ctx.area = coords;
  ctx.color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
  ctx.w = lv_area_get_width(&coords);
  ctx.h = lv_area_get_height(&coords);
  ctx.minSide = ctx.w < ctx.h ? ctx.w : ctx.h;

  if (ctx.w <= 0 || ctx.h <= 0) {
    return;
  }

  drawById(ctx, *id);
}

}  // namespace

bool initLauncherIcons() {
  gInitialized = true;
  return true;
}

bool launcherIconsReady() {
  return gInitialized;
}

int launcherIconRenderSize(LauncherIconVariant variant) {
  if (variant == LauncherIconVariant::Side) {
    return kSideRenderSize;
  }
  return kMainRenderSize;
}

lv_obj_t *createLauncherIcon(lv_obj_t *parent,
                             LauncherIconId id,
                             LauncherIconVariant variant,
                             lv_color_t color) {
  if (!parent) {
    return nullptr;
  }

  const int idx = static_cast<int>(id);
  if (idx < 0 || idx >= kIconCount) {
    return nullptr;
  }

  lv_obj_t *icon = lv_obj_create(parent);
  if (!icon) {
    return nullptr;
  }

  lv_obj_remove_style_all(icon);
  const int size = launcherIconRenderSize(variant);
  lv_obj_set_size(icon, size, size);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_outline_width(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_set_style_radius(icon, 0, 0);
  lv_obj_set_style_text_color(icon, color, 0);
  lv_obj_set_style_opa(icon, LV_OPA_COVER, 0);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_add_event_cb(icon, launcherIconEvent, LV_EVENT_ALL, (void *)&kIconUserData[idx]);
  return icon;
}
