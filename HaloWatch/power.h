// power.h — Always On and sleep (lib/Halo: halo_sleep; docs/halo.md, section 12; tried first in experiments/SleepLab).
//
// 15 s without touches and motion: the screen fades out (0.25 s, display.h's brightness spring) and, at zero, only
// the face's date and clock come back, dim on black — Always On. The board sleeps (light sleep: memory and program
// stay) and wakes once a minute for one frame: new digits, set without rolling (asleep there is no time to play a
// roll), and the pair a few pixels elsewhere (AMOLED burn-in: 8 places on a 7 px circle). The panel keeps the picture
// by itself. BOOT (anywhere) fades the screen out to fully dark instead, no Always On.
// A touch, BOOT or picking the board up (after it lay still) — the screen where it was, at full brightness, at once;
// the waking touch presses nothing. A touch while the screen is fading out — it stays.
// While music plays the board does not really sleep (the sound needs the processor): halo_stay_awake. On USB power it
// does not either (Halo: the USB port would go quiet and the board could not be flashed) — the same screens and
// wake-ups, the frames just go on.
// Commands: aod on|off (Always On, or dark when idle), idle N (s; 0 — never), sleep (Always On now), wake (as a
// touch — only while the board stays up: on USB), usb on|off (really sleep on USB too — the port then goes quiet:
// wake the board by hand before flashing it).
// Included from HaloWatch.ino after player.h.
#include <sys/time.h>

#define PW_IDLE_S 15
#define PW_AOD_BRIGHT 40   // the panel in Always On (of 255); the text is dimmed too
#define PW_TIME 0xB8B2A8   // Always On: the clock — a dim warm white
#define PW_DATE 0x6A6B70   // the date — a dim cool gray, as the face's

enum { PW_ACTIVE, PW_FADING, PW_AOD, PW_DARK };
static int pw_mode = PW_ACTIVE;
static bool pw_to_aod;        // where the fade goes: Always On or dark
static float pw_k;            // Always On brightness ramp, 0..1
static int64_t pw_active_us;  // the last touch or motion
static uint32_t pw_motion;
static bool pw_held;          // halo_stay_awake held for the music
static volatile bool pw_cmd_sleep, pw_cmd_wake, pw_aod_on = true;
static volatile int pw_idle_s = PW_IDLE_S;

static uint32_t pw_ms_to_minute() {  // until the next minute, +30 ms to be sure it has turned
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return 60000 - (uint32_t)((tv.tv_sec % 60) * 1000 + tv.tv_usec / 1000) + 30;
}

static void pw_fade(bool to_aod) {
  pw_mode = PW_FADING, pw_to_aod = pw_keep_lit = to_aod, dp_power.target = 0;
}

static void pw_wake(uint8_t w) {  // the screen where it was, at full brightness, at once
  static const char *const NAMES[] = {"", "touch", "BOOT", "motion", "timer"};
  LOG("power: woke by %s\n", NAMES[w]);
  pw_mode = PW_ACTIVE, pw_bright = -1, pw_keep_lit = false, pw_active_us = esp_timer_get_time();
  face_still(false);
  if (!halo_display_is_on()) halo_display_on(true);
  halo_spring_set(dp_power, 1);
  halo_damage_all();
}

// Every frame, first (before the face: Always On wants its digits without rolling)
static void power_tick(float dt) {
  const HaloFinger &f = halo_finger;
  const int64_t now = esp_timer_get_time();
  const bool moved = halo_motion_count() != pw_motion;
  if (f.down || moved) pw_active_us = now;  // touched or moved — not idle
  pw_motion = halo_motion_count();
  if (pl_playing != pw_held) pw_held = pl_playing, halo_stay_awake(pw_held);
  uint8_t w = halo_woke();
  if (pw_cmd_wake) pw_cmd_wake = false, w = w ? w : (pw_mode == PW_AOD || pw_mode == PW_DARK) ? HALO_WAKE_TOUCH : 0;
  const bool boot = halo_boot_click();
  switch (pw_mode) {
    case PW_ACTIVE:
      if (boot) pw_fade(false);
      else if (pw_cmd_sleep || (pw_idle_s && now - pw_active_us > pw_idle_s * 1000000ll)) pw_fade(pw_aod_on);
      break;
    case PW_FADING:
      // Touched while fading out — the screen stays; picked up too, but only on the way to Always On (BOOT, pressed
      // in a hand, moves the board as well — the dark fade is not cancelled by that)
      if (f.down || boot || (pw_to_aod && moved)) {
        pw_mode = PW_ACTIVE, pw_keep_lit = false, dp_power.target = 1, pw_active_us = now;
      } else if (dp_power.x < 0.01f) {
        if (pw_to_aod) {
          pw_mode = PW_AOD, pw_k = 0, pw_bright = 0;
          face_still(true);
          halo_damage_all();
        } else {
          pw_mode = PW_DARK;  // display.h has turned the panel off at zero
          halo_sleep();
        }
      }
      break;
    case PW_AOD:
      if (w && w != HALO_WAKE_TIMER) {
        pw_wake(w);
      } else {
        if (w == HALO_WAKE_TIMER) fc_check = true;  // a new minute: face_tick sets the digits
        if (pw_k < 1) {  // Always On comes up in 0.4 s, then sleep until the next minute
          pw_k = fminf(1, pw_k + dt / 0.4f), pw_bright = (int)lroundf(PW_AOD_BRIGHT * pw_k);
        } else if (!halo_asleep()) {
          halo_sleep(pw_ms_to_minute());
        }
      }
      break;
    case PW_DARK:
      if (w) pw_wake(w);
      break;
  }
  pw_cmd_sleep = false;  // sent while already asleep — it must not put the board back to sleep right after it wakes
}

// Always On: the face's date and clock, dim, a few pixels elsewhere every minute
static void power_layout() {
  static const int8_t SX[8] = {0, 5, 7, 5, 0, -5, -7, -5}, SY[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
  const int i = fc_min >= 0 ? fc_min & 7 : 0;
  const HaloView v;
  halo_roll_layout(r_date, CX + SX[i], FACE_DATE_Y + SY[i], PW_DATE, 1, &halo_black, v);
  halo_roll_layout(r_time, CX + SX[i], FACE_TIME_Y + SY[i], PW_TIME, 1, &halo_black, v);
}

static void power_draw() {
  halo_roll_draw(r_date), halo_roll_draw(r_time);
}

static void power_build() {  // after halo_begin: the motion sensor is on its I2C bus
  halo_motion_begin(150);
  pw_active_us = esp_timer_get_time();
  halo_command("aod", [](const char *a) { pw_aod_on = strcmp(a, "off") != 0, LOG("aod: %s\n", pw_aod_on ? "on" : "off"); });
  halo_command("idle", [](const char *a) { pw_idle_s = max(0, atoi(a)), LOG("idle: %d s\n", pw_idle_s); });
  halo_command("sleep", [](const char *) { pw_cmd_sleep = true; });
  halo_command("wake", [](const char *) { pw_cmd_wake = true; });
  halo_command("usb", [](const char *a) { halo_sleep_on_usb(strcmp(a, "off") != 0); });
}
