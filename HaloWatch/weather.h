// weather.h — the "Weather" screen, laid out 1:1 like design/weather2_mockup.webp (the Pixel Watch weather tile):
// the city, the current weather icon with the temperature and today's high and low, and a dark pill with the next three
// hours (icon and time). Instead of the mockup's icon at the top — our clock along the top edge, as on the gallery;
// the weather icons are ours (assets/gen_icons.py), sized like the mockup's. Four pages — four cities — slide
// sideways exactly like the gallery's "Pages": the leaving page shrinks to its edge and dims, the dot at the bottom
// stretches like a worm. Page 0 is "here": the same weather as the watch face shows (the face changes it).
//
// Opened from the gallery (back — swipe from the left edge) or by a tap on the face's weather pill (back — swipe up
// from the bottom edge: the dots lift and the current one stretches up after the finger; halo_nav_face_open).
//
// Everything sits on black and nothing moves at rest, so Halo sends nothing to the display after the screen
// has been drawn once. Positions are measured from the mockup (screen circle 1229 px -> 466 px, scale 0.3792), in
// board pixels: text by the middle of its letters (halo_text_mid) and by its ink — centered or right-aligned, as
// the mockup shows it, whatever the side bearings of the digits (wx_ink_x); icons by their centers. Font sizes come
// from the letter heights, weights from the amount of "ink" (assets/halo.conf). The whole screen is then drawn
// WX_SCALE times larger around the screen center (asked for: closer to the edges); fonts and icons are made at that
// size, not stretched at runtime.
// Included from HaloWatch.ino after the shared helpers (TEXT, the baked clock mask tt_mask, K_PAGER).

#define WX_WHITE 0xF9F9F9
#define WX_LO 0xA2A2A2   // today's low: gray
#define WX_PILL 0x2E3132 // the hourly pill
#define WX_PAGES 4
#define WX_HOURS 3

enum : uint8_t { WX_PARTLY, WX_SUN, WX_CLOUD, WX_RAIN };
struct WxDay {
  int8_t now, hi, lo;
  uint8_t kind;
  uint8_t hour_kind[WX_HOURS];  // this hour and the next two
};
// "Here": the watch face cycles through these (a tap on the face); page 0 shows the current one
static const WxDay WX_HERE[] = {
    {19, 21, 12, WX_PARTLY, {WX_PARTLY, WX_SUN, WX_PARTLY}},
    {23, 25, 14, WX_SUN, {WX_SUN, WX_SUN, WX_PARTLY}},
    {16, 17, 11, WX_CLOUD, {WX_CLOUD, WX_PARTLY, WX_CLOUD}},
    {12, 13, 9, WX_RAIN, {WX_RAIN, WX_CLOUD, WX_RAIN}},
};
#define WX_HERE_N (int)(sizeof(WX_HERE) / sizeof(WX_HERE[0]))
static const char *const WX_CITY[WX_PAGES] = {"San Francisco", "New York", "Boston", "Seattle"};
static const WxDay WX_AWAY[WX_PAGES - 1] = {
    {21, 23, 13, WX_SUN, {WX_SUN, WX_PARTLY, WX_PARTLY}},
    {15, 16, 9, WX_CLOUD, {WX_PARTLY, WX_CLOUD, WX_CLOUD}},
    {11, 12, 8, WX_RAIN, {WX_RAIN, WX_RAIN, WX_CLOUD}},
};
static const HaloImage *const WX_BIG[4] = {&img_wx, &img_wsun, &img_wcloud, &img_wrain};
static const HaloImage *const WX_SM[4] = {&img_wx_sm, &img_wsun_sm, &img_wcloud_sm, &img_wrain_sm};
static int wx_here;  // which of WX_HERE is "now"

#define WX_SCALE 1.2f  // the screen is 1.2x the mockup (keep in sync with halo.conf and WX2_SCALE in gen_icons.py)
// Mockup coordinates -> screen, scaled around the screen center; sizes — just times WX_SCALE
static inline float wsc(float v) {
  return CX + (v - CX) * WX_SCALE;
}
// The whole page up by WX_LIFT: from the city to the bottom of the pill it was 12 px below the circle's center (and
// 29 px above the page dots against 54 px under the clock); now it is centered and the gaps above and below are even
#define WX_LIFT 12.0f
// Mockup measurements, board px
#define WX_CITY_X wsc(234.6f)            // ink center of the city
#define WX_CITY_Y (wsc(115.4f) - WX_LIFT)  // middle of its letters
// The row "icon — temperature — high/low" (the user's rules): the temperature's digits are exactly at the screen
// center, in cells of one width (tabular: 1 or 8 — nothing moves; the degree sign hangs after them, as on the
// mockup); two lines along the top and the bottom of the digits — the icon of any kind fills the space between them
// (gen_icons.py makes each one exactly as tall as the digits), the high's top is on the upper line, the low's bottom on
// the lower one; from the icon's rightmost pixel to the digits — the gap plus the degree sign's width, from the degree
// sign to the high/low — the gap: around the digits themselves both sides are equal
#define WX_ROW_GAP (27.0f * WX_SCALE)  // the mockup's gap between its icon and its temperature
#define WX_ICON_NEAR 0.7f  // the icon side: (gap + degree sign) × 0.7 — the full one looked too far (picked on the board)
#define WX_NOW_Y (wsc(195.95f) - WX_LIFT)  // middle of the temperature's digits
#define WX_PILL_X wsc(232.7f)   // the pill: center and size (a capsule; its ends are 15 px from the screen edge)
#define WX_PILL_Y (wsc(314.45f) - WX_LIFT)
#define WX_PILL_W (293.2f * WX_SCALE)
#define WX_PILL_H (136.9f * WX_SCALE)
#define WX_HICON_Y (wsc(299.85f) - WX_LIFT)  // hourly icons: centers
#define WX_HTIME_Y (wsc(346.1f) - WX_LIFT)   // hourly times: middle of the letters
static const float WX_COL[WX_HOURS] = {147.8f, 233.3f, 317.65f};  // hourly columns, mockup px (icons and times share them)

struct WxPage {
  char now[8], hi[8], lo[8];
  HaloRoll t_now;  // the temperature: tabular digits, a new value rolls in
  HaloItem city, icon, t_hi, t_lo, pill, hicon[WX_HOURS], htime[WX_HOURS];
};
static WxPage wxp[WX_PAGES];
static char wx_hour[WX_HOURS][8];  // "7AM" ... — the same on every page
static HaloItem wx_clock, wx_dot[WX_PAGES], wx_worm, wx_pull;
static HaloSpring wx_pg;  // page (fractional while paging)
static float wx_g0, wx_mid_city, wx_mid_hl, wx_mid_hour;
// Digit rows from the line top (ink, as drawn): the temperature's and the high/low's
static float wx_now_top, wx_now_bottom, wx_hl_top, wx_hl_bottom;
static bool wx_drag;

static const WxDay &wx_day(int page) {
  return page == 0 ? WX_HERE[wx_here] : WX_AWAY[page - 1];
}

// Strings of all pages; hours — this one and the next two, as on the mockup: "7AM" (12 hours, as everywhere)
static void wx_fill(bool animate = false) {
  for (int p = 0; p < WX_PAGES; p++) {
    const WxDay &d = wx_day(p);
    WxPage &w = wxp[p];
    snprintf(w.now, sizeof(w.now), "%d°", d.now);
    snprintf(w.hi, sizeof(w.hi), "%d", d.hi);
    snprintf(w.lo, sizeof(w.lo), "%d", d.lo);
    halo_roll_set(w.t_now, w.now, animate);
  }
  int h = 12;
  if (halo_clock_valid()) {
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    h = t.tm_hour;
  }
  for (int i = 0; i < WX_HOURS; i++) {
    const int hh = (h + i) % 24;
    snprintf(wx_hour[i], sizeof(wx_hour[i]), "%d%s", hh % 12 ? hh % 12 : 12, hh < 12 ? "AM" : "PM");
  }
}

// Digits of a font, as Halo draws them: from the line top to the first and past the last row with ink (glyph ext rows)
// — by the flat digits "1" and "4": round ones (0, 3, 6, 8, 9) overshoot a row up and down, as in every font, and the
// lines go along the flat ones (gen_icons.py, digit_band — the same reference)
static void wx_digit_rows(const HaloFont &f, float &top, float &bottom) {
  top = 1e9f, bottom = -1e9f;
  for (uint32_t d : {(uint32_t)'1', (uint32_t)'4'}) {
    const HaloMask *g = halo_glyph(f, d);
    if (!g) continue;
    for (int r = 0; r < g->h; r++)
      if (g->ext[2 * r] <= g->ext[2 * r + 1]) top = fminf(top, g->oy + r), bottom = fmaxf(bottom, g->oy + r + 1);
  }
}

// What an image really draws: the first and past the last column and row with ink (ext rows)
static void wx_image_ink(const HaloImage &im, float &x0, float &x1, float &y0, float &y1) {
  x0 = y0 = 1e9f, x1 = y1 = -1e9f;
  for (int r = 0; r < im.h; r++)
    if (im.ext[2 * r] <= im.ext[2 * r + 1])
      x0 = fminf(x0, im.ext[2 * r]), x1 = fmaxf(x1, im.ext[2 * r + 1] + 1), y0 = fminf(y0, r), y1 = fmaxf(y1, r + 1);
}

static void weather_build() {
  wx_mid_city = halo_text_mid(t_title, "San Francisco");
  wx_mid_hl = halo_text_mid(w_hl, "0123456789");
  for (auto &w : wxp) {
    w.t_now.tabular = true;
    halo_roll_init(w.t_now, w_now, "0123456789", 0);
  }
  wx_digit_rows(w_now, wx_now_top, wx_now_bottom);
  wx_digit_rows(w_hl, wx_hl_top, wx_hl_bottom);
  wx_mid_hour = halo_text_mid(w_hour, "7AM");
  wx_fill();
}

static void weather_enter() {
  wx_fill();  // no rolling: the screen opens with its values in place
  halo_spring_set(wx_pg, 0);
  wx_drag = false;
}

// Paging — as on "Pages": sideways (not from the left edge: that is the gallery's swipe back); past 10 % or a flick
// faster than 400 px/s — to the neighbour; a 2000/1 spring
static void weather_update(float dt) {
  HaloFinger &f = halo_finger;
  if (f.drag_start && f.horiz && !f.owner && f.x0 > 30 * DP && halo_finger_claim(&wx_pg)) wx_g0 = wx_pg.x, wx_drag = true;
  if (wx_drag && f.owner == &wx_pg) {
    if (f.down) {
      const float raw = wx_g0 - (f.x - f.x0) / W;
      wx_pg.x = raw < 0 ? halo_rubber(raw * W) / W
              : raw > WX_PAGES - 1 ? WX_PAGES - 1 + halo_rubber((raw - WX_PAGES + 1) * W) / W : raw;
      wx_pg.v = -f.vx / W;
    } else {
      const int p = (int)lroundf(wx_g0);
      const float fr = wx_pg.x - p;
      int to = p;
      if (fr > 0.1f || f.vx < -400) to = p + 1;
      if (fr < -0.1f || f.vx > 400) to = p - 1;
      wx_pg.target = constrain(to, 0, WX_PAGES - 1);
      wx_drag = false;
    }
  }
  if (!wx_drag) halo_spring_run(wx_pg, K_PAGER, dt);
  for (auto &w : wxp) halo_roll_update(w.t_now, dt);
}

static void image_at(HaloItem &it, const HaloImage &im, float x, float y, float s, float a) {
  it.kind = HI_IMAGE, it.image = &im, it.x = x, it.y = y, it.scale = s, it.alpha = a, it.on_black = false;
}

// Where the anchor of a line goes (align 1 — its center, 2 — its right end) so that its ink, not its advance box, is
// centered at / ends at x: digits have side bearings, and the mockup aligns what is drawn
// What a line really draws: ink from ink0 to ink1 from its pen start, the pen advances by pen; false — nothing
static bool wx_ink(const HaloFont &f, const char *s, float &ink0, float &ink1, float &pen) {
  pen = 0, ink0 = 1e9f, ink1 = -1e9f;
  while (*s) {
    const HaloMask *g = halo_glyph(f, halo_utf8_next(s));
    if (!g) continue;
    ink0 = fminf(ink0, pen + g->ox), ink1 = fmaxf(ink1, pen + g->ox + g->w), pen += g->adv;
  }
  return ink0 <= ink1;
}
static float wx_ink_w(const HaloFont &f, const char *s) {
  float i0, i1, pen;
  return wx_ink(f, s, i0, i1, pen) ? i1 - i0 : 0;
}
static float wx_ink_x(const HaloFont &f, const char *s, int align, float x, float sc) {
  float ink0, ink1, pen;
  if (!wx_ink(f, s, ink0, ink1, pen)) return x;
  return align == 2 ? x + (pen - ink1) * sc : x + (pen / 2 - (ink0 + ink1) / 2) * sc;
}

// A line of text by the middle of its letters at y, aligned by align at x; it lies on `on` (black or the pill)
static void wx_text(HaloItem &it, const HaloFont &f, const char *s, float mid, int align, float x, float y, float sc,
                    uint32_t color, float alpha, const HaloItem *on) {
  it.kind = HI_TEXT, it.font = &f, it.text = s, it.align = align, it.color = color, it.on = on, it.on_black = true;
  it.x = wx_ink_x(f, s, align, x, sc), it.y = y + (f.line / 2.0f - mid) * sc, it.scale = sc, it.alpha = alpha;
}

static void weather_layout(const HaloView &v) {
  // Clock along the top edge: the same baked arc as on the gallery, at rest
  wx_clock.kind = HI_MASK, wx_clock.mask = &tt_mask, wx_clock.color = 0xC6C6C6, wx_clock.on_black = true;
  wx_clock.on = &halo_black, wx_clock.x = tt_x + tt_mask.w / 2.0f, wx_clock.y = tt_y + tt_mask.h / 2.0f;
  wx_clock.scale = 1, wx_clock.alpha = tt_mask.bits ? 1 - halo_clamp01(ui_lock * 2) : 0;  // locked — gives way to the lock
  halo_view_commit(wx_clock, v);
  const float p = wx_pg.x;
  for (int i = 0; i < WX_PAGES; i++) {
    // The leaving page: scale 1 -> 0.55 toward the edge it leaves through, dims to 0.5 (as on "Pages")
    const float off = i - p, f = halo_clamp01(fabsf(off)), s = 1 - 0.45f * f;
    const float k = fabsf(off) < 0.999f ? 1 - 0.5f * f : 0;
    const float pivot = off < 0 ? 0 : W, cx = pivot + (CX + off * W - pivot) * s;
    auto X = [&](float x) { return cx + (x - CX) * s; };
    auto Y = [&](float y) { return CX + (y - CX) * s; };
    WxPage &w = wxp[i];
    const WxDay &d = wx_day(i);
    // City; the current weather icon, the temperature, today's high and low
    wx_text(w.city, t_title, WX_CITY[i], wx_mid_city, 1, X(WX_CITY_X), Y(WX_CITY_Y), s, WX_WHITE, k, &halo_black);
    // The row (WX_ROW_GAP above): the temperature's digits centered at CX by their cells, then everything around it by
    // what is really drawn
    int nd = 0;
    while (w.now[nd] >= '0' && w.now[nd] <= '9') nd++;
    // The lines in whole pixels, as the letters are drawn (a line's top lands on a whole row): the high/low and the
    // icon are put so that their first and last rows land exactly on them
    const float now_line = halo_floor(WX_NOW_Y - w.t_now.mid + 0.5f);
    const float anchor = CX - nd * w.t_now.cell / 2, top = now_line + wx_now_top, bottom = now_line + wx_now_bottom;
    float t0 = 0, t1 = 0, d0 = 0, d1 = 0;
    halo_roll_ink(w.t_now, t0, t1);         // the digits and the degree sign
    halo_roll_ink(w.t_now, d0, d1, 0, nd);  // the digits alone
    const float ink_l = anchor + d0, ink_r = anchor + t1, degree = t1 - d1;
    const HaloImage &im = *WX_BIG[d.kind];
    float ix0, ix1, iy0, iy1;
    wx_image_ink(im, ix0, ix1, iy0, iy1);
    const float icon_x = ink_l - (WX_ROW_GAP + degree) * WX_ICON_NEAR - ix1 + im.w / 2.0f, icon_y = top - iy0 + im.h / 2.0f;
    const float hl_r = ink_r + WX_ROW_GAP + fmaxf(wx_ink_w(w_hl, w.hi), wx_ink_w(w_hl, w.lo));
    image_at(w.icon, im, X(icon_x), Y(icon_y), s, k);
    const HaloView pv = {v.x + (cx - CX) * v.s, v.y, s * v.s, v.a, v.over};  // the page's place and size, in the view
    halo_roll_layout(w.t_now, anchor, now_line + w.t_now.mid, WX_WHITE, k, &halo_black, pv);  // on a whole row
    wx_text(w.t_hi, w_hl, w.hi, wx_mid_hl, 2, X(hl_r), Y(top - wx_hl_top + wx_mid_hl), s, WX_WHITE, k, &halo_black);
    wx_text(w.t_lo, w_hl, w.lo, wx_mid_hl, 2, X(hl_r), Y(bottom - wx_hl_bottom + wx_mid_hl), s, WX_LO, k, &halo_black);
    halo_view_commit(w.city, v), halo_view_commit(w.icon, v);
    halo_view_commit(w.t_hi, v), halo_view_commit(w.t_lo, v);
    // The pill with the next three hours: icon and time
    HaloItem &pl = w.pill;
    pl.kind = HI_RRECT, pl.x = X(WX_PILL_X), pl.y = Y(WX_PILL_Y), pl.w = WX_PILL_W, pl.h = WX_PILL_H, pl.r = WX_PILL_H / 2;
    pl.scale = s, pl.color = WX_PILL, pl.alpha = k, pl.on_black = true;
    halo_view_commit(pl, v);
    for (int j = 0; j < WX_HOURS; j++) {
      image_at(w.hicon[j], *WX_SM[d.hour_kind[j]], X(wsc(WX_COL[j])), Y(WX_HICON_Y), s, k);
      wx_text(w.htime[j], w_hour, wx_hour[j], wx_mid_hour, 1, X(wsc(WX_COL[j])), Y(WX_HTIME_Y), s, WX_WHITE, k, &pl);
      halo_view_commit(w.hicon[j], v), halo_view_commit(w.htime[j], v);
    }
  }
  // Page dots along the bottom arc (as on "Pages": 12 px every 20 px on r = 217, the current one — a worm). While the
  // swipe back is pulled from the bottom, the dots lift a little and the current one stretches up after the finger
  const float P = halo_nav_pull(), lift = 26 * (1 - expf(-P / 140)), stretch = 30 * (1 - expf(-P / 120));
  const float R = 217, step = 20 / R, a0 = HALO_PI + step * (WX_PAGES - 1) / 2;
  for (int i = 0; i < WX_PAGES; i++) {
    const float a = a0 - i * step;
    HaloItem &d = wx_dot[i];
    d.kind = HI_CIRCLE, d.x = CX + R * sinf(a), d.y = CX - R * cosf(a) - lift, d.r = 6, d.scale = 1, d.color = 0xFFFFFF;
    d.alpha = 0.3f, d.on_black = true;
    halo_view_commit(d, v);
  }
  const float pc = halo_clamp01(p / (WX_PAGES - 1)) * (WX_PAGES - 1);
  const int i0 = (int)pc;
  const float fr = pc - i0, head = fminf(1, fr * 2), tail = fmaxf(0, fr * 2 - 1);
  const float aa = a0 - (i0 + tail) * step, ab = a0 - (i0 + head) * step;
  const bool pulled = stretch > 0.3f;
  wx_worm.kind = HI_ARC, wx_worm.x = CX, wx_worm.y = CX - lift, wx_worm.r = R, wx_worm.w = 12, wx_worm.scale = 1;
  wx_worm.a0 = fminf(aa, ab), wx_worm.a1 = fmaxf(aa, ab) + 0.0001f, wx_worm.color = 0xFFFFFF;
  wx_worm.alpha = pulled ? 0 : 1, wx_worm.on_black = true;
  halo_view_commit(wx_worm, v);
  // Pulled: the current dot becomes a capsule growing upward from its place
  const float ac = a0 - lroundf(pc) * step, dx = CX + R * sinf(ac), dy = CX - R * cosf(ac) - lift;
  wx_pull.kind = HI_RRECT, wx_pull.x = dx, wx_pull.y = dy - stretch / 2, wx_pull.w = 12, wx_pull.h = 12 + stretch;
  wx_pull.r = 6, wx_pull.scale = 1, wx_pull.color = 0xFFFFFF, wx_pull.alpha = pulled ? 1 : 0, wx_pull.on_black = true;
  halo_view_commit(wx_pull, v);
}

static void weather_draw() {
  halo_item_draw(wx_clock);
  for (auto &w : wxp) {
    halo_item_draw(w.city), halo_item_draw(w.icon), halo_roll_draw(w.t_now);
    halo_item_draw(w.t_hi), halo_item_draw(w.t_lo), halo_item_draw(w.pill);
    for (int j = 0; j < WX_HOURS; j++) halo_item_draw(w.hicon[j]), halo_item_draw(w.htime[j]);
  }
  for (auto &d : wx_dot) halo_item_draw(d);
  halo_item_draw(wx_worm), halo_item_draw(wx_pull);
}

// In the gallery: to the face — swipe up from the bottom edge, as back over the face
static const HaloScreen WEATHER = {weather_enter, weather_update, weather_layout, weather_draw, false, HALO_HOME_UP};
