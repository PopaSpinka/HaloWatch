// display.h — the "Display" screen (Settings -> Display): the screen's brightness, and the BOOT button that turns the
// screen off and on.
//
// The slider is WearKit's (demo_slider.h: Wear OS 6 InlineSlider, numbers — docs/research/notes/pickers_progress.md)
// ported to Halo: a 52 dp pill with "−" and "+" at its ends (zones of 48 dp), the bar between them — the filled part
// 12 dp thick, the empty one 4 dp, with a dot between every two steps (dark on the filled part, light on the empty one).
// A tap on "−"/"+" — one step (a light circle grows under the icon while pressed), holding — a repeat after 500 ms,
// then every 60 ms; a finger on the bar sets the value by its position (not on Wear OS: watches have a crown). The bar
// catches up with a new value in 150 ms along (0, 0, 0, 1) — sharp at first, soft at the end — and the brightness of
// the screen glides along with it. The level is kept in flash (settings.h) and applied at start.
//
// BOOT (anywhere, any screen) — power.h: the brightness fades to zero in about 0.25 s here (dp_power), then the panel
// goes dark and the board sleeps; a touch, BOOT or picking it up — the screen at full brightness at once. Always On
// (power.h) sets the panel's brightness itself (pw_bright) and keeps the panel lit at zero (pw_keep_lit). Command
// boot — the same press from the serial port.
//
// The lock (PWR, anywhere): a click locks — touches stop working (halo_touch_on: no taps, swipes or gestures) and the
// brightness glides down to the slider's lowest step; a long press (~1.5 s, the power chip decides; holding 6–10 s
// powers the board off in hardware) unlocks — the brightness glides back to the slider's value. While locked, a small
// lock sits at the very top, where the Weather screen and the gallery show the time (that time gives way to it, ui_lock):
// it fades and grows in, wiggles when the locked screen is touched, and opens (the open lock) as it fades out on
// unlocking. The lock is kept in flash (settings.h): after a reboot the watch starts locked. Commands pwr / pwr long —
// the same presses.
// Included from HaloWatch.ino after face.h, before settings.h.

#define DP_STEPS 8
#define DP_DEFAULT 7  // = 180, the brightness before this screen existed
// Panel brightness (command 0x51) for each step: roughly even steps for the eye (it sees brightness about as a
// logarithm). The lowest must stay visible: at 6 the panel looked fully off (only BOOT turns the screen off)
static const uint8_t DP_LEVEL[DP_STEPS + 1] = {24, 34, 48, 66, 90, 118, 150, 180, 255};
#define DP_W 410.0f  // the pill: 205 dp x 52 dp
#define DP_H 104.0f
#define DP_BTN 96.0f  // "−" and "+" zones
#define DP_HEAD_Y 96.0f
#define DP_LABEL_Y 166.0f
#define DP_SL_Y 240.0f
#define DP_HINT_Y 334.0f
#define DP_PRIMARY 0xA8C7FA  // Wear OS 6 dark scheme
#define DP_ON_PRIMARY 0x0B2A5C
#define DP_OUTLINE 0x8E9099
#define DP_TRACK 0x45474D    // the empty bar; a pressed "−"/"+"

static volatile int dp_value = DP_DEFAULT;  // 0..DP_STEPS; settings.h saves it (from loop())
static float dp_shown = DP_DEFAULT, dp_from = DP_DEFAULT, dp_t = 9;  // in steps; the 150 ms catch-up
static HaloEase EASE_SHARP;
static HaloSpring dp_power = {1, 0, 1};  // 1 — the screen is on, 0 — off (BOOT)
static HaloSpringK K_DP_POWER;
static int dp_sent = -1;  // the brightness last sent to the panel
static Press dp_press[2];  // "−", "+"
static int dp_armed;       // the finger came down on: −1 — "−", 1 — "+", 2 — the bar, 0 — nothing
static uint32_t dp_hold_at;
static float dp_mid_head, dp_mid_label, dp_mid_hint, dp_mid_icon;
static HaloItem dp_head, dp_label, dp_hint, dp_pill, dp_track, dp_fill, dp_dot[DP_STEPS - 1];
static HaloItem dp_circle[2], dp_icon[2];
// The lock
#define DP_LOCK_Y 15.0f  // the lock's middle: where the time's letters are at the top (radius 219 from the center)
static volatile bool dp_locked;  // settings.h saves it (from loop())
static HaloSpring dp_lock;       // 0 — unlocked, 1 — locked: the brightness and the lock icon glide between the two
static HaloSpringK K_DP_LOCK;
static uint32_t dp_shake_at;     // the locked screen was touched then: the lock wiggles
static float dp_mid_lock;
static HaloItem dp_lock_ic;

static uint8_t dp_level(float steps) {  // between the table's steps — linearly
  const float s = halo_clamp01(steps / DP_STEPS) * DP_STEPS;
  const int i = min((int)s, DP_STEPS - 1);
  return (uint8_t)lroundf(DP_LEVEL[i] + (DP_LEVEL[i + 1] - DP_LEVEL[i]) * (s - i));
}

static uint8_t dp_start_level() {  // the first frame's brightness: the slider's, or the lowest when locked
  return dp_locked ? DP_LEVEL[0] : DP_LEVEL[dp_value];
}

static void display_build() {
  dp_mid_head = halo_text_mid(t_title, "Display");
  dp_mid_label = halo_text_mid(t20, "Brightness");
  dp_mid_hint = halo_text_mid(t20, "Press BOOT to turn the screen off");
  dp_mid_icon = halo_text_mid(ic32, "");
  halo_ease_make(EASE_SHARP, 0, 0, 0, 1);
  K_DP_POWER = halo_spring(0.25f, 1);
  K_DP_LOCK = halo_spring(0.4f, 1);
  dp_mid_lock = halo_text_mid(ic18, "\uf023");
  halo_spring_set(dp_lock, dp_locked);  // started locked (flash) — dim and without touches from the first frame
  halo_touch_on(!dp_locked);
  dp_shown = dp_from = dp_value, dp_sent = dp_start_level();  // halo_begin starts at this brightness
}

static void dp_lock_set(bool on) {
  dp_locked = on, dp_lock.target = on;
  halo_touch_on(!on);
  LOG("%s\n", on ? "locked" : "unlocked");
}

static void dp_set(int v) {
  v = constrain(v, 0, DP_STEPS);
  if (v == dp_value) return;
  dp_from = dp_shown, dp_t = 0, dp_value = v;
}

// Every frame, whatever is on screen: the catch-up, the brightness and the lock (BOOT — power.h)
static void display_tick(float dt) {
  // PWR: a click locks, a long press unlocks (a long press doesn't also click: the chip reports one or the other)
  const uint8_t pwr = halo_pwr_event();
  if (pwr & HALO_PWR_LONG) {
    if (dp_locked) dp_lock_set(false);
  } else if (pwr & HALO_PWR_CLICK) {
    if (!dp_locked) dp_lock_set(true);
  }
  if (halo_finger_blocked() && dp_locked) dp_shake_at = halo_ms() | 1;
  halo_spring_run(dp_power, K_DP_POWER, dt);
  halo_spring_run(dp_lock, K_DP_LOCK, dt);
  dp_t = fminf(dp_t + dt, 9);
  dp_shown = dp_from + (dp_value - dp_from) * halo_ease(EASE_SHARP, dp_t / 0.15f);
  const float k = halo_clamp01(dp_power.x), lk = halo_clamp01(dp_lock.x), lv = dp_level(dp_shown);
  ui_lock = lk;
  int level = (int)lroundf((lv + (DP_LEVEL[0] - lv) * lk) * k * k);  // the eye: the fade's end is the slow part
  if (pw_bright >= 0) level = pw_bright;  // Always On
  if (level != dp_sent) halo_brightness(level), dp_sent = level;
  if (dp_power.target < 0.5f && dp_power.x < 0.01f && halo_display_is_on() && !pw_keep_lit)
    halo_display_on(false), LOG("screen off\n");
}

static void display_enter() {
  dp_armed = 0;
  for (auto &p : dp_press) halo_spring_set(p.s, 0), p.going_down = false;
}

static void display_update(float dt) {
  HaloFinger &f = halo_finger;
  const float x0 = CX - DP_W / 2, x1 = CX + DP_W / 2, bx = x0 + DP_BTN, bw = DP_W - 2 * DP_BTN;
  const bool on_pill = fabsf(f.y0 - DP_SL_Y) < DP_H / 2 && f.x0 > x0 && f.x0 < x1;
  if (f.pressed) {
    dp_armed = !on_pill || f.owner ? 0 : f.x0 < bx ? -1 : f.x0 > bx + bw ? 1 : 2;
    dp_hold_at = halo_ms() + 500;
  }
  if (dp_armed == 2 && f.down && (!f.dragging || f.horiz) && halo_finger_claim(&dp_armed))  // the bar: by position
    dp_set((int)lroundf(halo_clamp01((f.x - bx) / bw) * DP_STEPS));
  if ((dp_armed == -1 || dp_armed == 1) && f.dragging) dp_armed = 0;  // moved — a swipe, not a press
  if ((dp_armed == -1 || dp_armed == 1) && f.tap) dp_set(dp_value + dp_armed);
  if ((dp_armed == -1 || dp_armed == 1) && f.down && (int32_t)(halo_ms() - dp_hold_at) >= 0)  // held: a repeat
    dp_set(dp_value + dp_armed), dp_hold_at = halo_ms() + 60;
  press_update(dp_press[0], dp_armed == -1 && f.down, dt);
  press_update(dp_press[1], dp_armed == 1 && f.down, dt);
  if (!f.down) dp_armed = 0;
}

static void display_layout(const HaloView &v) {
  text_mid(dp_head, t_title, "Display", dp_mid_head, CX, DP_HEAD_Y, 1, TEXT, 1, &halo_black);
  text_mid(dp_label, t20, "Brightness", dp_mid_label, CX, DP_LABEL_Y, 1, DIM, 1, &halo_black);
  text_mid(dp_hint, t20, "Press BOOT to turn the screen off", dp_mid_hint, CX, DP_HINT_Y, 1, DIM, 1, &halo_black);
  dp_head.on_black = dp_label.on_black = dp_hint.on_black = true;
  halo_view_commit(dp_head, v), halo_view_commit(dp_label, v), halo_view_commit(dp_hint, v);
  // The pill and the pressed buttons' circles under the icons
  dp_pill.kind = HI_RRECT, dp_pill.x = CX, dp_pill.y = DP_SL_Y, dp_pill.w = DP_W, dp_pill.h = DP_H, dp_pill.r = DP_H / 2;
  dp_pill.scale = 1, dp_pill.alpha = 1, dp_pill.color = CARD_HI, dp_pill.on_black = true;
  halo_view_commit(dp_pill, v);
  for (int i = 0; i < 2; i++) {
    const float p = halo_clamp01(dp_press[i].s.x), cx = i ? CX + DP_W / 2 - DP_BTN / 2 : CX - DP_W / 2 + DP_BTN / 2;
    HaloItem &c = dp_circle[i];
    c.kind = HI_CIRCLE, c.x = cx, c.y = DP_SL_Y, c.r = 40 * p, c.scale = 1, c.color = DP_TRACK, c.on_black = false;
    c.alpha = p > 0.01f ? 1 : 0;
    halo_view_commit(c, v);
    text_mid(dp_icon[i], ic32, i ? "" : "", dp_mid_icon, cx, DP_SL_Y, 1, TEXT, 1, nullptr);
    halo_view_commit(dp_icon[i], v);
  }
  // The bar: the empty part thin (4 dp), the filled one thick (12 dp), round ends; dots between the steps
  const float bx = CX - DP_W / 2 + DP_BTN, bw = DP_W - 2 * DP_BTN, r = halo_clamp01(dp_shown / DP_STEPS), filled = bw * r;
  dp_track.kind = HI_RRECT, dp_track.x = bx + bw / 2, dp_track.y = DP_SL_Y, dp_track.w = bw, dp_track.h = 8, dp_track.r = 4;
  dp_track.scale = 1, dp_track.alpha = 1, dp_track.color = DP_TRACK, dp_track.on_black = false;
  dp_fill.kind = HI_RRECT, dp_fill.x = bx + filled / 2, dp_fill.y = DP_SL_Y, dp_fill.w = filled, dp_fill.h = 24, dp_fill.r = 12;
  dp_fill.scale = 1, dp_fill.alpha = filled > 1 ? 1 : 0, dp_fill.color = DP_PRIMARY, dp_fill.on_black = false;
  halo_view_commit(dp_track, v), halo_view_commit(dp_fill, v);
  for (int i = 1; i < DP_STEPS; i++) {  // at the filled part's round end — no dot: the end itself marks the step
    HaloItem &d = dp_dot[i - 1];
    d.kind = HI_CIRCLE, d.x = bx + i * (bw + 8) / DP_STEPS - 4, d.y = DP_SL_Y, d.r = 4, d.scale = 1;
    d.alpha = halo_clamp01((fabsf(d.x - (bx + filled)) - 6) / 6);
    d.color = d.x < bx + filled ? DP_ON_PRIMARY : DP_OUTLINE, d.on_black = false;
    halo_view_commit(d, v);
  }
}

static void display_draw() {
  halo_item_draw(dp_head), halo_item_draw(dp_label), halo_item_draw(dp_hint);
  halo_item_draw(dp_pill);
  for (int i = 0; i < 2; i++) halo_item_draw(dp_circle[i]), halo_item_draw(dp_icon[i]);
  halo_item_draw(dp_track), halo_item_draw(dp_fill);
  for (auto &d : dp_dot) halo_item_draw(d);
}

// The lock icon over everything (laid out after halo_nav_update, drawn after halo_nav_draw). As in our Fade, never
// together with the time it replaces: the time fades out over the first half of the lock spring, the lock grows from
// 0.7 and fades in over the second; touched while locked — a short wiggle (10 Hz, 0.45 s, dying away); unlocking — the
// open lock fades out first, then the time comes back
static void display_overlay_layout() {
  const float a = halo_clamp01(dp_lock.x * 2 - 1), t = dp_shake_at ? (halo_ms() - dp_shake_at) / 1000.0f : 9;
  const float shake = t < 0.45f ? 5 * sinf(t * 2 * HALO_PI * 10) * (1 - t / 0.45f) : 0;
  text_mid(dp_lock_ic, ic18, dp_locked ? "\uf023" : "\uf09c", dp_mid_lock, CX + shake, DP_LOCK_Y, 0.7f + 0.3f * a,
           0xC6C6C6, a > 0.01f ? a : 0, &halo_black);
  dp_lock_ic.on_black = true;
  if (shake != 0) halo_subpixel = true;
  halo_item_commit(dp_lock_ic);
}

static void display_overlay_draw() {
  halo_item_draw(dp_lock_ic);
}

// Static: no vertical gestures of its own — to the face either way (down from the top or up from the bottom edge)
static const HaloScreen DISPLAY_SCREEN = {display_enter, display_update, display_layout, display_draw, false,
                                          HALO_HOME_BOTH};
