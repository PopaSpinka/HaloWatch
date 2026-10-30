// battery.h — the "Battery" screen (Settings -> Battery) and the battery's history.
//
// The power chip (AXP2101, halo_battery(): every 2 s) knows whether a battery is there, USB or not, charging or not,
// the percent (its fuel gauge) and the voltage. On screen: a battery drawn in shapes, filled to the level (a bolt over
// it on USB), the percent next to it, the status ("Charging", "Fully charged", "On battery for 3 h 20 min") and what was
// used since unplugging; below — the level over the last 24 hours, like the iPhone's "Battery level": a bar per
// 30 minutes, the time on USB shaded and marked with a green strip and a bolt, the hours under it. Opening the screen,
// the bars grow left to right and the percent counts up.
//
// The history lives in flash (namespace "battery", key "hist": a slot per 30 min, 48 slots, and when USB was
// unplugged), written from loop() when a slot closes or the power source changes — kept across reboots. Until the
// clock is set from the internet nothing is recorded (the slots go by real time). Started on battery with nothing
// recorded about the unplugging — it is taken from the history (the last slot on USB). Command batdemo — a made-up
// day for screenshots (not saved); again — the real one back from flash.
#include <Preferences.h>

#define BH_N 48       // slots: 24 h
#define BH_SLOT 1800  // s per slot
struct BattHistory {
  uint32_t last;       // the newest slot: unix time / BH_SLOT (0 — none yet)
  uint32_t unplug;     // when USB was unplugged, unix time (0 — on USB or unknown)
  uint8_t unplug_pct;  // the percent then
  uint8_t v[BH_N];     // a ring by slot % BH_N: bits 0..6 — the percent, bit 7 — on USB at some point; 0xFF — no data
};
static BattHistory bh;
static Preferences bh_prefs;
static bool bh_demo, bh_known, bh_was_usb;
static volatile bool bh_cmd_demo;

static void battery_read() {
  if (bh_prefs.getBytes("hist", &bh, sizeof(bh)) != sizeof(bh)) memset(&bh, 0, sizeof(bh)), memset(bh.v, 0xFF, BH_N);
}

static void battery_load() {
  bh_prefs.begin("battery", false);
  battery_read();
  halo_command("batdemo", [](const char *) { bh_cmd_demo = true; });
}

// A made-up day like the iPhone's chart: down overnight, charged in the morning, down through the day, a short top-up
static void battery_demo() {
  const uint32_t now = time(nullptr), last = now / BH_SLOT;
  float p = 64;
  for (int k = BH_N - 1; k >= 0; k--) {
    const int i = BH_N - 1 - k;  // 0 — the oldest
    const bool usb = (i >= 8 && i < 13) || (i >= 34 && i < 36);
    p = usb ? fminf(100, p + 8.5f) : fmaxf(5, p - (i < 8 ? 1.6f : 2.1f));
    bh.v[(last - k) % BH_N] = (uint8_t)lroundf(p) | (usb ? 0x80 : 0);
  }
  bh.last = last, bh.unplug = now - 5 * BH_SLOT - 1260, bh.unplug_pct = 100, bh_demo = true;
}

// From loop(): every 5 s — this slot's value; a new slot or another power source — into flash
static void battery_poll() {
  if (bh_cmd_demo) bh_cmd_demo = false, bh_demo ? (void)(battery_read(), bh_demo = false) : battery_demo();
  static uint32_t at;
  if (millis() - at < 5000) return;
  at = millis();
  const HaloBattery b = halo_battery();
  if (!b.ok || !b.present || b.percent < 0 || !halo_clock_valid() || bh_demo) return;
  const uint32_t now = time(nullptr), slot = now / BH_SLOT;
  bool save = false;
  if (slot != bh.last) {
    if (!bh.last || slot < bh.last) memset(bh.v, 0xFF, BH_N);  // the first one, or the clock went back
    else
      for (uint32_t s = bh.last + 1; s < slot && s <= bh.last + BH_N; s++) bh.v[s % BH_N] = 0xFF;  // missed (off)
    bh.last = slot, save = true;
  }
  const uint8_t old = bh.v[slot % BH_N];
  const bool usb = b.usb || (old != 0xFF && (old & 0x80));
  bh.v[slot % BH_N] = (uint8_t)b.percent | (usb ? 0x80 : 0);
  if (bh_known && bh_was_usb && !b.usb) bh.unplug = now, bh.unplug_pct = b.percent, save = true;  // unplugged now
  if (b.usb && bh.unplug) bh.unplug = 0, save = true;
  if (!bh_known && !b.usb && !bh.unplug)  // started on battery, nothing about the unplugging: the last slot on USB
    for (uint32_t k = 1; k < BH_N && k <= slot; k++) {
      const uint8_t v = bh.v[(slot - k) % BH_N], nx = bh.v[(slot - k + 1) % BH_N];
      if (v != 0xFF && (v & 0x80)) {
        bh.unplug = (slot - k + 1) * BH_SLOT, bh.unplug_pct = nx != 0xFF ? nx & 0x7F : b.percent, save = true;
        break;
      }
    }
  bh_known = true, bh_was_usb = b.usb;
  if (save) bh_prefs.putBytes("hist", &bh, sizeof(bh));
}

// ---------- The screen ----------
#define BT_GREEN 0x30D158  // the iPhone's battery green
#define BT_RED 0xFF453A    // 20 % and lower
#define BT_SHADE 0x173A20  // the chart behind the time on USB
#define BT_GRID 0x2C2C2E
#define BT_HEAD_Y 72.0f
#define BT_HERO_Y 134.0f   // the battery and the percent
#define BT_STATUS_Y 184.0f
#define BT_DETAIL_Y 210.0f
#define BT_CAP_Y 240.0f    // "LAST 24 HOURS"
#define BT_TOP 260.0f      // the chart: 100 % ... 0 %
#define BT_BOT 350.0f
#define BT_LEFT 76.0f
#define BT_PITCH 5.5f      // a bar per slot: 4 px wide
#define BT_STRIP_Y 364.0f  // the green strip under the time on USB
#define BT_HOURS_Y 390.0f
#define BT_RUNS 8          // shaded runs at most
#define BT_MARKS 5         // hour marks at most
static uint32_t bt_enter_ms;
static HaloEase EASE_EMPH;
static char bt_pct[8], bt_status[40], bt_detail[48];
static float bt_mid_head, bt_mid_t20, bt_mid_pct, bt_mid_icon, bt_mid_bolt;
static HaloItem bt_head, bt_out, bt_in, bt_fill, bt_nub, bt_bolt, bt_pct_t, bt_status_t, bt_detail_t, bt_cap;
static HaloItem bt_grid[3], bt_ylab[3], bt_bar[BH_N], bt_shade[BT_RUNS], bt_strip[BT_RUNS], bt_sbolt_bg, bt_sbolt;
static HaloItem bt_vline[BT_MARKS], bt_hour[BT_MARKS], bt_empty;
static const char *const BT_YLAB[3] = {"100%", "50%", "0%"};

static void battery_build() {
  bt_mid_head = halo_text_mid(t_title, "Battery");
  bt_mid_t20 = halo_text_mid(t20, "On battery");
  bt_mid_pct = halo_text_mid(n48, "0123456789");
  bt_mid_icon = halo_text_mid(ic24, "");
  bt_mid_bolt = halo_text_mid(ic18, "");
  halo_ease_make(EASE_EMPH, 0.05f, 0.7f, 0.1f, 1);  // emphasized decelerate
}

static void battery_enter() {
  bt_enter_ms = halo_ms();
}

static void bt_duration(char *out, int n, uint32_t s) {
  const uint32_t m = s / 60;
  if (m < 1) snprintf(out, n, "less than a minute");
  else if (m < 60) snprintf(out, n, "%lu min", (unsigned long)m);
  else snprintf(out, n, "%lu h %lu min", (unsigned long)(m / 60), (unsigned long)(m % 60));
}

// Text by the middle of its letters at y; align 0 — x is the left edge, 1 — the middle. on — what it lies on (a solid
// color — edges from a table; nullptr — blended)
static void bt_text(HaloItem &it, const HaloFont &f, const char *s, float mid, float x, float y, uint32_t color,
                    int align, const HaloView &v, float alpha = 1, const HaloItem *on = &halo_black) {
  it.kind = HI_TEXT, it.font = &f, it.text = s, it.align = align, it.color = color, it.on_black = on == &halo_black;
  it.on = on, it.x = x, it.y = y + (f.line / 2.0f - mid), it.scale = 1, it.alpha = alpha, it.spacing = 0;
  halo_view_commit(it, v);
}

static void bt_rect(HaloItem &it, float x0, float y0, float x1, float y1, float r, uint32_t color, float alpha,
                    const HaloView &v) {
  it.kind = HI_RRECT, it.x = (x0 + x1) / 2, it.y = (y0 + y1) / 2, it.w = x1 - x0, it.h = y1 - y0, it.r = r;
  it.scale = 1, it.color = color, it.alpha = x1 - x0 > 0.1f && y1 - y0 > 0.1f ? alpha : 0, it.on_black = true;
  halo_view_commit(it, v);
}

static void battery_layout(const HaloView &v) {
  const HaloBattery b = halo_battery();
  const float t = (halo_ms() - bt_enter_ms) / 1000.0f;
  const bool has = b.ok && b.present && b.percent >= 0;
  const int pct = has ? b.percent : 0;
  bt_text(bt_head, t_title, "Battery", bt_mid_head, CX, BT_HEAD_Y, TEXT, 1, v);
  // The percent counts up as the screen opens; the battery fills along
  const float k = halo_ease(EASE_EMPH, t / 0.7f), shown = pct * k;
  if (has) snprintf(bt_pct, sizeof(bt_pct), "%d%%", (int)lroundf(shown));
  else snprintf(bt_pct, sizeof(bt_pct), "--");
  const uint32_t lvl = !has ? DIM : b.usb ? BT_GREEN : pct <= 20 ? BT_RED : TEXT;
  const float tw = halo_text_width(n48, bt_pct, 0), gw = 54 + 5, x0 = CX - (gw + 16 + tw) / 2, gx = x0 + 27;
  bt_rect(bt_out, gx - 27, BT_HERO_Y - 15, gx + 27, BT_HERO_Y + 15, 8, 0x8E9099, 1, v);
  bt_rect(bt_in, gx - 24, BT_HERO_Y - 12, gx + 24, BT_HERO_Y + 12, 5.5f, 0x000000, 1, v);
  bt_rect(bt_fill, gx - 21, BT_HERO_Y - 9, gx - 21 + 42 * shown / 100, BT_HERO_Y + 9, 3, lvl, 1, v);
  bt_rect(bt_nub, gx + 28, BT_HERO_Y - 6, gx + 32, BT_HERO_Y + 6, 2, 0x8E9099, 1, v);
  bt_text(bt_bolt, ic24, "", bt_mid_icon, gx, BT_HERO_Y, 0xFFFFFF, 1, v, has && b.usb ? 1 : 0, nullptr);
  bt_text(bt_pct_t, n48, bt_pct, bt_mid_pct, x0 + gw + 16, BT_HERO_Y, has ? TEXT : DIM, 0, v);
  // What is going on, and what it cost
  char dur[24];
  if (!b.ok) snprintf(bt_status, sizeof(bt_status), "Reading..."), bt_detail[0] = 0;
  else if (!b.present) snprintf(bt_status, sizeof(bt_status), "No battery"), snprintf(bt_detail, sizeof(bt_detail), "Powered from USB");
  else if (b.charging) snprintf(bt_status, sizeof(bt_status), "Charging"), snprintf(bt_detail, sizeof(bt_detail), "%.2f V", b.mv / 1000.0f);
  else if (b.usb)
    snprintf(bt_status, sizeof(bt_status), pct >= 99 ? "Fully charged" : "On USB, not charging"),
        snprintf(bt_detail, sizeof(bt_detail), "%.2f V", b.mv / 1000.0f);
  else if (bh.unplug && (bh_demo || halo_clock_valid())) {
    bt_duration(dur, sizeof(dur), (uint32_t)time(nullptr) - bh.unplug);
    snprintf(bt_status, sizeof(bt_status), "On battery for %s", dur);
    snprintf(bt_detail, sizeof(bt_detail), "Used %d%% since unplugged", max(0, bh.unplug_pct - pct));
  } else {
    snprintf(bt_status, sizeof(bt_status), "On battery"), snprintf(bt_detail, sizeof(bt_detail), "%.2f V", b.mv / 1000.0f);
  }
  bt_text(bt_status_t, t20, bt_status, bt_mid_t20, CX, BT_STATUS_Y, b.charging ? BT_GREEN : TEXT, 1, v);
  bt_text(bt_detail_t, t20, bt_detail, bt_mid_t20, CX, BT_DETAIL_Y, DIM, 1, v);
  bt_text(bt_cap, t20, "LAST 24 HOURS", bt_mid_t20, BT_LEFT, BT_CAP_Y, DIM, 0, v);
  // The chart: grid at 100 / 50 / 0 %, the time on USB shaded, a bar per slot (grow left to right on opening)
  const float cw = BH_N * BT_PITCH, cr = BT_LEFT + cw, ch = BT_BOT - BT_TOP;  // the chart: width, right, height
  for (int i = 0; i < 3; i++) {
    const float y = BT_TOP + ch * i / 2;
    bt_rect(bt_grid[i], BT_LEFT, y - 0.5f, cr, y + 0.5f, 0, BT_GRID, 1, v);
    bt_text(bt_ylab[i], t20, BT_YLAB[i], bt_mid_t20, cr + 8, y, DIM, 0, v);
  }
  int runs = 0, best = -1;
  float best_w = 0;
  for (int i = 0; i < BH_N;) {  // runs of slots on USB: shaded behind, a strip below
    const uint8_t s = bh.last ? bh.v[(bh.last - (BH_N - 1 - i)) % BH_N] : 0xFF;
    if (s == 0xFF || !(s & 0x80)) {
      i++;
      continue;
    }
    int j = i;
    while (j < BH_N) {
      const uint8_t q = bh.v[(bh.last - (BH_N - 1 - j)) % BH_N];
      if (q == 0xFF || !(q & 0x80)) break;
      j++;
    }
    if (runs < BT_RUNS) {
      const float a = BT_LEFT + i * BT_PITCH, z = BT_LEFT + j * BT_PITCH;
      bt_rect(bt_shade[runs], a, BT_TOP, z, BT_BOT, 0, BT_SHADE, k, v);
      bt_rect(bt_strip[runs], a + 0.75f, BT_STRIP_Y - 4, z - 0.75f, BT_STRIP_Y + 4, 4, BT_GREEN, k, v);
      if (z - a > best_w) best_w = z - a, best = runs;
      runs++;
    }
    i = j;
  }
  for (int r = runs; r < BT_RUNS; r++) bt_rect(bt_shade[r], 0, 0, 0, 0, 0, 0, 0, v), bt_rect(bt_strip[r], 0, 0, 0, 0, 0, 0, 0, v);
  // The bolt on the longest strip (on a black disc, as cut out of it)
  const float bx = best >= 0 ? bt_strip[best].x : CX, show = best >= 0 && best_w >= 20 ? k : 0;
  bt_sbolt_bg.kind = HI_CIRCLE, bt_sbolt_bg.x = bx, bt_sbolt_bg.y = BT_STRIP_Y, bt_sbolt_bg.r = 11, bt_sbolt_bg.scale = 1;
  bt_sbolt_bg.color = 0, bt_sbolt_bg.alpha = show, bt_sbolt_bg.on_black = false;
  halo_view_commit(bt_sbolt_bg, v);
  bt_text(bt_sbolt, ic18, "", bt_mid_bolt, bx, BT_STRIP_Y, BT_GREEN, 1, v, show, &bt_sbolt_bg);
  for (int i = 0; i < BH_N; i++) {
    const uint8_t s = bh.last ? bh.v[(bh.last - (BH_N - 1 - i)) % BH_N] : 0xFF;
    const float g = halo_ease(EASE_EMPH, (t - 0.25f * i / (BH_N - 1)) / 0.5f), p = s == 0xFF ? 0 : (s & 0x7F) / 100.0f;
    const float x = BT_LEFT + i * BT_PITCH + BT_PITCH / 2, h = fmaxf(p * ch * g, s == 0xFF ? 0 : 2 * g);
    bt_rect(bt_bar[i], x - 2, BT_BOT - h, x + 2, BT_BOT, 2, (s & 0x7F) <= 20 ? BT_RED : BT_GREEN, 1, v);
  }
  // Hours under the chart: every 6 hours of local time, with a faint line up through the chart
  const time_t now = bh_demo || halo_clock_valid() ? time(nullptr) : 0;
  const time_t right = (time_t)(bh.last + 1) * BH_SLOT, left = right - BH_N * BH_SLOT;
  struct tm lt;
  localtime_r(&left, &lt);
  time_t mark = left - lt.tm_min * 60 - lt.tm_sec + (6 - lt.tm_hour % 6) * 3600;  // the first 0, 6, 12 or 18 o'clock
  int m = 0;
  for (; now && bh.last && mark < right && m < BT_MARKS; mark += 6 * 3600, m++) {
    localtime_r(&mark, &lt);
    const float x = BT_LEFT + cw * (mark - left) / (float)(BH_N * BH_SLOT);
    static const char *const HL[4] = {"12A", "6", "12P", "6"};
    bt_rect(bt_vline[m], x - 0.5f, BT_TOP, x + 0.5f, BT_BOT + 6, 0, BT_GRID, 1, v);
    bt_text(bt_hour[m], t20, HL[lt.tm_hour / 6], bt_mid_t20, x, BT_HOURS_Y, DIM, 1, v);
  }
  for (; m < BT_MARKS; m++) {
    bt_rect(bt_vline[m], 0, 0, 0, 0, 0, 0, 0, v);
    bt_text(bt_hour[m], t20, "", bt_mid_t20, CX, BT_HOURS_Y, DIM, 1, v, 0);
  }
  // No history yet (the clock not set, or just started): say so in the chart's middle
  bool any = false;
  for (int i = 0; i < BH_N && !any; i++) any = bh.last && bh.v[i] != 0xFF;
  bt_text(bt_empty, t20, "Collecting data", bt_mid_t20, BT_LEFT + cw / 2, (BT_TOP + BT_BOT) / 2, DIM, 1, v, any ? 0 : 1);
  if (t < 1.2f) halo_subpixel = true;  // bars growing
}

static void battery_draw() {
  halo_item_draw(bt_head);
  halo_item_draw(bt_out), halo_item_draw(bt_in), halo_item_draw(bt_fill), halo_item_draw(bt_nub);
  halo_item_draw(bt_bolt), halo_item_draw(bt_pct_t), halo_item_draw(bt_status_t), halo_item_draw(bt_detail_t);
  halo_item_draw(bt_cap);
  for (auto &it : bt_shade) halo_item_draw(it);
  for (auto &it : bt_grid) halo_item_draw(it);
  for (auto &it : bt_vline) halo_item_draw(it);
  for (auto &it : bt_bar) halo_item_draw(it);
  for (auto &it : bt_ylab) halo_item_draw(it);
  for (auto &it : bt_strip) halo_item_draw(it);
  halo_item_draw(bt_sbolt_bg), halo_item_draw(bt_sbolt);
  for (auto &it : bt_hour) halo_item_draw(it);
  halo_item_draw(bt_empty);
}

// Static: to the face either way (down from the top or up from the bottom edge)
static const HaloScreen BATTERY_SCREEN = {battery_enter, nullptr, battery_layout, battery_draw, false, HALO_HOME_BOTH};
