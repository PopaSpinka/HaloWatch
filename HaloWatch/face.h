// face.h — the watch face (design/face_mockup.webp): the date in capitals, a big clock and a weather pill. It is the
// home of HaloWatch: swipe up anywhere and the gallery slides up over it; swipe down from the gallery's top edge to
// come back (halo_nav_face in lib/Halo).
//
// Every change moves, at 60 fps: changed digits and letters roll (HaloRoll: the old one goes up and fades, the new one
// comes up from below, several in a cascade from the right), letters of another width push their neighbours aside,
// the pill grows or shrinks with the temperature, and a new weather icon pops in while the old one shrinks away. Only
// what moves is redrawn. The date and time start random (no network: HaloWatch.ino); the weather is fictional (weather.h).
//
// A tap anywhere but the pill — a demo: everything changes at once (all four digits, the date, the weather; 10 s later
// the clock rolls back to the real time). A tap on the pill opens the Weather screen right over the face
// (halo_nav_face_open: back — swipe up from the bottom edge). The transition is Fade (fixed: docs/halo.md, section 10;
// it used to be chosen in Settings and by a long press here).
//
// Positions are measured from the mockup (screen circle 1215 px -> 466 px, scale 0.3837): text by the middle of its
// letters, the icon by its center. Font sizes come from the digit heights, weights from the amount of "ink"
// (assets/halo.conf), the icons from shapes fitted to the mockup (assets/gen_icons.py). The whole face is then drawn
// FACE_SCALE times larger around the screen center (asked for: closer to the edges); fonts and icons are made at that
// size, not stretched at runtime.
// Commands: demo (as a tap), wx N (weather 0..3), fx N (another transition 0..2 until a reboot, to compare), tick [N] (the clock N minutes ahead),
// day (a day ahead). Included from HaloWatch.ino after weather.h.

#define FACE_DATE 0xC2C3CB  // the date: light gray with a cool tint
#define FACE_WHITE 0xF8F7F9
#define FACE_PILL 0x2D2F2F
#define FACE_SCALE 1.2f  // the face is 1.2x the mockup (keep in sync with halo.conf and FACE_SCALE in gen_icons.py)
// Mockup coordinates -> screen, scaled around the screen center; sizes and offsets — just times FACE_SCALE
static inline float fsc(float v) {
  return CX + (v - CX) * FACE_SCALE;
}
// The whole face up by FACE_LIFT: from the top of the date to the bottom of the pill it was 6.5 px below the circle's
// center (measured on the board); lifted to the exact center it looked a little high — on the board 4 px lower was
// picked, then 6 px more: the block's middle is 10 px below the circle's center (horizontally it is centered: the date
// and the pill by their middles, the clock by its colon)
#define FACE_LIFT -3.5f
// Mockup measurements, board px
#define FACE_DATE_Y (fsc(116.0f) - FACE_LIFT)   // middle of the date's capitals
#define FACE_TIME_Y (fsc(219.2f) - FACE_LIFT)   // middle of the clock digits
#define PILL_Y (fsc(336.1f) - FACE_LIFT)        // pill center
#define PILL_H (75.6f * FACE_SCALE)
#define PILL_W (186.1f * FACE_SCALE)          // pill width with "19°"
#define PILL_ICON (53.5f * FACE_SCALE)        // icon center from the pill's left edge
#define PILL_ICON_Y (fsc(337.0f) - FACE_LIFT)
#define PILL_INK (95.2f * FACE_SCALE)         // left ink edge of "19°" from the pill's left edge
#define PILL_TEMP_Y (fsc(338.0f) - FACE_LIFT)   // middle of the temperature digits
#define FACE_ICON_RISE (10 * FACE_SCALE)      // a changing icon moves up by this much

static const HaloImage *const FACE_ICON[4] = {&img_partly, &img_sun, &img_cloud, &img_rain};  // by WX_PARTLY ...
static const char *const WEEKDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const MONTH[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static HaloRoll r_time, r_date, r_temp;
static HaloItem fc_pill, fc_icon_in, fc_icon_out;
static HaloSpring fc_icon_t;  // icon change: 0 -> 1
static HaloSpringK K_ICON;
static const HaloImage *fc_icon, *fc_icon_old;
static char fc_temp[8];
static int fc_min = -1, fc_hour = -1, fc_yday = -1;
static float fc_pen, fc_pad_r;  // the temperature's pen from the pill's left edge, and the pill after the text
static int32_t fc_offset;       // shift of the clock, s (the demo, tick, day)
static uint32_t fc_demo_until;               // the demo's clock goes back to real time then
static uint8_t fc_fx = HALO_FX_FADE;         // the transition to the Weather screen: Fade (fx N — another, to compare)
static bool fc_check;
static bool fc_still;  // Always On (power.h): digits and date change without rolling — asleep there is no time to play it
static volatile int fc_cmd_wx = -1, fc_cmd_fx = -1;
static volatile bool fc_cmd_demo;
static volatile int32_t fc_cmd_shift;

static float pill_width() {
  return fc_pen + halo_roll_width(r_temp) + fc_pad_r;
}

// The weather "here" (WX_HERE in weather.h): the pill on the face and page 0 of the Weather screen
static void face_weather(int i, bool animate = true) {
  wx_here = (i % WX_HERE_N + WX_HERE_N) % WX_HERE_N;
  const WxDay &d = WX_HERE[wx_here];
  if (FACE_ICON[d.kind] != fc_icon && animate) {
    fc_icon_old = fc_icon;
    halo_spring_set(fc_icon_t, 0);
    fc_icon_t.target = 1;
  }
  fc_icon = FACE_ICON[d.kind];
  snprintf(fc_temp, sizeof(fc_temp), "%d°", d.now);
  halo_roll_set(r_temp, fc_temp, animate);
  wx_fill(animate);  // page 0 of the Weather screen: the same weather (its temperature rolls too)
}

// Demo: the clock jumps 20..320 days and some minutes ahead, picked so that every digit of the time, the weekday, the
// month and the day's last digit change — the whole face rolls at once; and the next weather. 10 s later — real time
static void face_demo() {
  face_weather(wx_here + 1);
  if (!halo_clock_valid()) return;
  const time_t real = time(nullptr), shown = real + fc_offset;
  struct tm a, b;
  localtime_r(&shown, &a);
  const int ah = a.tm_hour % 12 ? a.tm_hour % 12 : 12;
  for (int k = 0; k < 100; k++) {
    const int32_t off = (int32_t)(20 + esp_random() % 300) * 86400 + (int32_t)(esp_random() % 1440) * 60;
    const time_t t = real + off;
    localtime_r(&t, &b);
    const int bh = b.tm_hour % 12 ? b.tm_hour % 12 : 12;
    if (bh / 10 != ah / 10 && bh % 10 != ah % 10 && b.tm_min / 10 != a.tm_min / 10 && b.tm_min % 10 != a.tm_min % 10 &&
        b.tm_wday != a.tm_wday && b.tm_mon != a.tm_mon && b.tm_mday % 10 != a.tm_mday % 10) {
      fc_offset = off;
      break;
    }
  }
  fc_demo_until = halo_ms() + 10000, fc_check = true;
}

static void face_build() {
  K_ICON = halo_spring(0.5f, 0.7f);  // the icon pops with a visible overshoot
  r_time.tabular = true;  // a cell per digit: nothing shifts when digits change, the colon is right at the center
  halo_roll_init(r_time, f_time, "0123456789", 1, -1);  // letter spacing -1, as on the mockup
  halo_roll_init(r_date, f_date, "HOT0123");
  r_temp.rise = 15 * FACE_SCALE;  // the temperature rolls inside the pill (its letters are drawn with the pill's colors)
  halo_roll_init(r_temp, f_temp, "0123456789", 0);
  halo_roll_set(r_time, "--:--", false);
  face_weather(0, false);
  halo_spring_set(fc_icon_t, 1);
  // The pill is laid out by the pen of the text (the mockup gives the ink edge of "1")
  const HaloMask *one = halo_glyph(f_temp, '1');
  fc_pen = PILL_INK - (one ? one->ox : 0);
  fc_pad_r = PILL_W - fc_pen - halo_text_width(f_temp, "19°");
  halo_command("wx", [](const char *a) { fc_cmd_wx = atoi(a); });
  halo_command("fx", [](const char *a) { fc_cmd_fx = atoi(a); });
  halo_command("demo", [](const char *) { fc_cmd_demo = true; });
  halo_command("tick", [](const char *a) { fc_cmd_shift = 60 * (*a ? atoi(a) : 1); });
  halo_command("day", [](const char *) { fc_cmd_shift = 86400; });
}

// Every frame, whatever is on screen: the clock and the date, the rolls and the icon spring
static void face_tick(float dt) {
  static uint32_t checked;
  if (fc_cmd_wx >= 0) face_weather(fc_cmd_wx), fc_cmd_wx = -1;
  if (fc_cmd_fx >= 0) fc_fx = fc_cmd_fx % HALO_FX_COUNT, fc_cmd_fx = -1;
  if (fc_cmd_demo) face_demo(), fc_cmd_demo = false;
  bool now_check = halo_ms() - checked >= 250 || fc_check;
  fc_check = false;
  if (fc_cmd_shift) fc_offset += fc_cmd_shift, fc_cmd_shift = 0, now_check = true;
  if (fc_demo_until && (int32_t)(halo_ms() - fc_demo_until) > 0) fc_offset = 0, fc_demo_until = 0, now_check = true;
  if (now_check && halo_clock_valid()) {
    checked = halo_ms();
    const time_t now = time(nullptr) + fc_offset;
    struct tm t;
    localtime_r(&now, &t);
    char s[20];
    if (t.tm_min != fc_min || t.tm_hour != fc_hour) {
      fc_min = t.tm_min, fc_hour = t.tm_hour;
      snprintf(s, sizeof(s), "%02d:%02d", fc_hour % 12 ? fc_hour % 12 : 12, fc_min);  // 12 hours, as on the gallery
      halo_roll_set(r_time, s, !fc_still);
    }
    if (t.tm_yday != fc_yday) {  // "THU, 29 OCT" — the mockup's format
      fc_yday = t.tm_yday;
      snprintf(s, sizeof(s), "%s, %d %s", WEEKDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
      halo_roll_set(r_date, s, !fc_still);
    }
  }
  halo_roll_update(r_time, dt), halo_roll_update(r_date, dt), halo_roll_update(r_temp, dt);
  halo_spring_run(fc_icon_t, K_ICON, dt);
  if (fc_icon_t.x == 1 && fc_icon_t.v == 0) fc_icon_old = nullptr;
}

// Always On (power.h): on — the clock and the date as they are now, at once (a roll still playing would freeze in
// sleep half-way) and from then on without rolling; off — rolling again
static void face_still(bool on) {
  fc_still = on;
  if (on) fc_min = fc_hour = fc_yday = -1, fc_check = true;
}

// The Weather screen right over the face, with the Fade transition
static void face_open_weather() {
  halo_nav_face_open(WEATHER, fc_fx, CX, PILL_Y, pill_width(), PILL_H);
}

// Only while the face is on screen and still: the pill opens the Weather screen, anything else — the demo
static void face_update(float) {
  const HaloFinger &f = halo_finger;
  if (!f.tap || f.owner) return;
  if (fabsf(f.x - CX) < pill_width() / 2 + 10 && fabsf(f.y - PILL_Y) < PILL_H / 2 + 10)
    face_open_weather();
  else
    face_demo();
}

static void face_layout(const HaloView &v) {
  halo_roll_layout(r_date, CX, FACE_DATE_Y, FACE_DATE, 1, &halo_black, v);
  halo_roll_layout(r_time, CX, FACE_TIME_Y, FACE_WHITE, 1, &halo_black, v);
  // The pill: as wide as its text right now (it follows the temperature's width spring)
  const float w = pill_width(), left = CX - w / 2;
  fc_pill.kind = HI_RRECT, fc_pill.x = CX, fc_pill.y = PILL_Y, fc_pill.w = w, fc_pill.h = PILL_H, fc_pill.r = PILL_H / 2;
  fc_pill.color = FACE_PILL, fc_pill.on_black = true, fc_pill.scale = 1, fc_pill.alpha = 1;
  halo_view_commit(fc_pill, v);
  // The icon: the new one grows from 0.5 with an overshoot and rises a little, the old one shrinks, rises and fades
  // out quickly (gone by a third of the way: two half-transparent icons on top of each other look muddy)
  const float t = fc_icon_t.x, ix = left + PILL_ICON;
  HaloItem &in = fc_icon_in, &out = fc_icon_out;
  in.kind = out.kind = HI_IMAGE, in.on_black = out.on_black = false;
  in.image = fc_icon, in.x = ix, in.y = PILL_ICON_Y + (1 - t) * FACE_ICON_RISE, in.scale = 0.5f + 0.5f * t;
  in.alpha = halo_clamp01((t - 0.12f) * 1.8f);
  out.image = fc_icon_old, out.x = ix, out.y = PILL_ICON_Y - t * FACE_ICON_RISE, out.scale = 1 - 0.5f * t;
  out.alpha = fc_icon_old ? halo_clamp01(1 - t * 3) : 0;
  halo_view_commit(out, v), halo_view_commit(in, v);
  halo_roll_layout(r_temp, left + fc_pen, PILL_TEMP_Y, FACE_WHITE, 1, &fc_pill, v);
}

static void face_draw() {
  halo_roll_draw(r_date), halo_roll_draw(r_time);
  halo_item_draw(fc_pill), halo_item_draw(fc_icon_out), halo_item_draw(fc_icon_in);
  halo_roll_draw(r_temp);
}
