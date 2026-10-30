// settings.h — the "Settings" screen: a list of sections as the gallery's cards ("Display": the screen's brightness,
// display.h; "Battery": the level and its history, battery.h). A tap opens the section right over the settings
// (halo_nav_open from an open screen; back — swipe from the left edge, to the face — down from the top or up from the
// bottom edge).
//
// What is set here is kept in flash (NVS, namespace "halowatch": the brightness — "bright", the lock — "lock"): read in
// setup() before halo_begin (the panel starts at the saved brightness, locked or not), written from loop() (a flash
// write stalls the other core for a moment — not in the middle of a frame); the brightness 0.8 s after the last change
// (not on every step of a drag), the lock at once.
//
// The transition to the Weather screen used to be chosen here (Zoom, Fade, Slide; radio buttons and "Try it") — now it
// is Fade, fixed (docs/halo.md, section 10; the command fx N still switches it until a reboot, to compare).
// Included from HaloWatch.ino after display.h and battery.h.
#include <Preferences.h>

struct SeRow {
  const char *title, *sub, *icon;  // the icon — Font Awesome (ic24)
  uint32_t color;
  const HaloScreen *screen;
};
static const SeRow SE_ROWS[] = {
    {"Display", "brightness", "", 0xFDD663, &DISPLAY_SCREEN},
    {"Battery", "level and history", "", 0x5CF08A, &BATTERY_SCREEN},
};
#define SE_N (int)(sizeof(SE_ROWS) / sizeof(SE_ROWS[0]))
static HaloList se_list;
static Press se_press[SE_N];
static int se_armed = -1;
static float se_mid_head, se_mid_icon;
static HaloItem se_head, se_bg[SE_N], se_disc[SE_N], se_icon[SE_N], se_title[SE_N], se_sub[SE_N];
static Preferences se_prefs;
static int se_saved = -1, se_seen = -1;  // the brightness in flash; the last one seen and when it changed
static uint32_t se_changed_at;

static bool se_lock_saved;

static void settings_load() {
  se_prefs.begin("halowatch", false);
  dp_value = se_saved = se_seen = constrain((int)se_prefs.getUChar("bright", DP_DEFAULT), 0, DP_STEPS);
  dp_locked = se_lock_saved = se_prefs.getBool("lock", false);
}
static void settings_save_poll() {  // from loop()
  if (dp_locked != se_lock_saved) se_lock_saved = dp_locked, se_prefs.putBool("lock", se_lock_saved);  // at once
  const int v = dp_value;
  if (v != se_seen) se_seen = v, se_changed_at = millis();
  if (v != se_saved && millis() - se_changed_at > 800) {
    se_prefs.putUChar("bright", v), se_saved = v;
    LOG("brightness %d/%d (%d) saved\n", v, DP_STEPS, DP_LEVEL[v]);
  }
}

static void settings_build() {
  float h[SE_N + 1];
  h[0] = M_HEAD_H;
  for (int i = 0; i < SE_N; i++) h[i + 1] = M_ITEM_H;
  halo_list_init(se_list, h, SE_N + 1, 64, 330, true);
  se_mid_head = halo_text_mid(t_title, "Settings");
  se_mid_icon = halo_text_mid(ic24, SE_ROWS[0].icon);
}

static void settings_enter() {  // from the top
  halo_list_reset(se_list);
  se_armed = -1;
  for (auto &p : se_press) halo_spring_set(p.s, 0), p.going_down = false;
}

static void settings_update(float dt) {
  HaloFinger &f = halo_finger;
  halo_list_update(se_list, dt);
  if (f.pressed) {  // which row the finger came down on (rows are the list's items 1..)
    const int i = halo_list_hit(se_list, f.y0) - 1;
    se_armed = i >= 0 && i < SE_N && fabsf(f.x0 - CX) < M_ITEM_W / 2 * se_list.fx[i + 1].s ? i : -1;
  }
  if (f.dragging) se_armed = -1;
  if (f.tap && se_armed >= 0) halo_nav_open(*SE_ROWS[se_armed].screen);
  for (int i = 0; i < SE_N; i++) press_update(se_press[i], i == se_armed && f.down, dt);
  if (!f.down) se_armed = -1;
}

static void settings_layout(const HaloView &v) {
  const HaloListFx &h = se_list.fx[0];
  if (se_list.moving && v.a > 0 && v.s >= 0.999f) halo_subpixel = true;
  text_mid(se_head, t_title, "Settings", se_mid_head, CX, h.top + M_HEAD_H / 2, 1, TEXT, h.a_bg, &halo_black);
  se_head.on_black = true;
  halo_view_commit(se_head, v);
  for (int k = 0; k < SE_N; k++) {  // a card as in the gallery: a colored circle with the icon, the title, the subtitle
    const HaloListFx &f = se_list.fx[k + 1];
    const SeRow &d = SE_ROWS[k];
    const bool vis = halo_list_item_visible(se_list, k + 1);
    const float s = f.s, hh = M_ITEM_H * s, w = M_ITEM_W * s, x0 = CX - w / 2, y0 = f.top;
    const float p = halo_clamp01(se_press[k].s.x), ac = vis ? f.a_c : 0, ix = x0 + 56 * s, iy = y0 + hh / 2;
    HaloItem &bg = se_bg[k];  // pressed — the corners squeeze (26 dp -> 18 dp)
    bg.kind = HI_RRECT, bg.x = CX, bg.y = y0 + hh / 2, bg.w = M_ITEM_W, bg.h = M_ITEM_H, bg.scale = s;
    bg.r = 52 + (36 - 52) * p, bg.color = CARD_HI, bg.alpha = vis ? f.a_bg : 0, bg.on_black = true;
    HaloItem &dc = se_disc[k];
    dc.kind = HI_CIRCLE, dc.x = ix, dc.y = iy, dc.r = 32, dc.scale = s, dc.color = d.color, dc.alpha = ac;
    dc.on_black = true;
    text_mid(se_icon[k], ic24, d.icon, se_mid_icon, ix, iy, s, 0x1B1B1F, ac, &dc);
    text_base(se_title[k], t24, d.title, x0 + 104 * s, y0 + 46 * s, s, TEXT, ac, &bg);
    text_base(se_sub[k], t20, d.sub, x0 + 104 * s, y0 + 80 * s, s, DIM, ac, &bg);
    halo_view_commit(bg, v), halo_view_commit(dc, v), halo_view_commit(se_icon[k], v);
    halo_view_commit(se_title[k], v), halo_view_commit(se_sub[k], v);
  }
}

static void settings_draw() {
  halo_item_draw(se_head);
  for (int k = 0; k < SE_N; k++) {
    halo_item_draw(se_bg[k]), halo_item_draw(se_disc[k]), halo_item_draw(se_icon[k]);
    halo_item_draw(se_title[k]), halo_item_draw(se_sub[k]);
  }
}

// To the face — both ways, as on the gallery list: down from the top edge or up from the bottom edge (only from the very
// edge: the rest of the screen scrolls the list)
static const HaloScreen SETTINGS = {settings_enter, settings_update, settings_layout, settings_draw, false, HALO_HOME_BOTH};
