// Sleep and wake-up for Halo: light sleep of the ESP32 — memory and program stay, the board goes on where it
// stopped, the picture is back in ~20–35 ms — and the motion sensor that can wake it. docs/halo.md, section 12.
//
// The sketch asks (halo_sleep) inside update; the frame engine calls hp_frame_end() once that frame is on the
// display, and the board sleeps right there. Wake-ups: a touch (CST9217 INT, GPIO11, low), BOOT (GPIO0, low),
// motion (QMI8658 INT1, GPIO21 — the level opposite to the current one; after halo_motion_begin), a timer. The
// next update sees halo_woke().
// On USB power, or while someone holds halo_stay_awake (Wi-Fi), the board only pretends: a really sleeping board
// leaves the USB port silent and cannot be flashed. The same wake-ups are then watched once a frame.
// Time during sleep is counted by the RTC slow clock. The default one, the slow RC oscillator (~140 kHz), ran 0.54 %
// fast in light sleep (half a minute per hour asleep); the fast RC oscillator / 256 (~70 kHz) — 0.03 %. It is
// switched over before the first sleep, kept running during sleep and measured against the crystal every 5 minutes.
#include "halo_internal.h"

#include <driver/gpio.h>
#include <esp_private/esp_clk.h>
#include <esp_sleep.h>
#include <soc/rtc.h>

// ---------- Motion sensor: wake-on-motion on INT1 (register sequence as SensorLib configWakeOnMotion) ----------
static HaloI2c imu;
static bool imu_on;
static volatile uint32_t imu_edges;
// The board wakes on motion only after lying still: pressing BOOT or putting it down in a hand moves it too. A burst
// starts with an edge after at least SETTLE_US of quiet; motion wakes only if a burst started after the sleep did
#define SETTLE_US 1500000
static volatile int64_t imu_last_us = -SETTLE_US, imu_burst_us = -SETTLE_US;

static void IRAM_ATTR imu_isr(void *) {
  const int64_t t = esp_timer_get_time();
  if (t - imu_last_us >= SETTLE_US) imu_burst_us = t;
  imu_last_us = t, imu_edges++;
}

static void imu_edge_now() {  // the INT1 flip that woke the board: its interrupt was off in sleep
  portDISABLE_INTERRUPTS();
  imu_isr(nullptr);
  portENABLE_INTERRUPTS();
}

static uint8_t imu_reg(uint8_t r) {
  uint8_t v = 0;
  halo_i2c_read(imu, r, &v, 1);
  return v;
}

static bool imu_wait(uint8_t r, uint8_t mask, uint8_t want) {
  for (int i = 0; i < 100; i++) {
    if ((imu_reg(r) & mask) == want) return true;
    delay(5);
  }
  return false;
}

bool halo_motion_begin(int mg) {
  if (!imu) imu = halo_i2c(0x6B);
  const uint8_t who = imu ? imu_reg(0x00) : 0;
  if (who != 0x05) return LOG("motion: QMI8658 not found (who am i 0x%02x)\n", who), false;
  halo_i2c_write(imu, 0x60, 0xB0);  // reset; done when 0x4D reads 0x80
  if (!imu_wait(0x4D, 0xFF, 0x80)) return LOG("motion: QMI8658 reset failed\n"), false;
  halo_i2c_write(imu, 0x02, 0x40);                            // CTRL1: address auto-increment, INT pins off
  halo_i2c_write(imu, 0x08, 0x00);                            // CTRL7: sensors off
  halo_i2c_write(imu, 0x03, 0x2C);                            // CTRL2: ±8 g, low-power 128 Hz
  halo_i2c_write(imu, 0x0B, (uint8_t)constrain(mg, 1, 255));  // CAL1_L: threshold, 1 mg per step
  halo_i2c_write(imu, 0x0C, 0x80 | 0x20);                     // CAL1_H: INT1 starting high, skip 32 samples
  halo_i2c_write(imu, 0x0A, 0x08);                            // CTRL9: apply the settings, wait, acknowledge
  const bool done = imu_wait(0x2D, 0x80, 0x80);
  halo_i2c_write(imu, 0x0A, 0x00);
  imu_wait(0x2D, 0x80, 0x00);
  halo_i2c_write(imu, 0x08, 0x01);       // CTRL7: accelerometer on
  halo_i2c_write(imu, 0x02, 0x40 | 0x08);  // CTRL1: INT1 output on
  if (!imu_on) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ull << HALO_PIN_IMU_INT1;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io);
    gpio_isr_handler_add((gpio_num_t)HALO_PIN_IMU_INT1, imu_isr, nullptr);
    imu_on = true;
  }
  LOG("motion: wake-on-motion %d mg (QMI8658 INT1, GPIO21)%s\n", mg, done ? "" : ", settings NOT confirmed");
  return done;
}

uint32_t halo_motion_count() {
  return imu_edges;
}

// ---------- Sleep ----------
static volatile bool req;
static volatile uint32_t req_ms;
static volatile int holds;
static volatile bool on_usb_ok;
static bool asleep, boot_armed, touch_armed, told_usb;
static uint32_t sleep_ms;
static int64_t asleep_at_us;
static uint8_t woke;
static bool clock_ready;
static uint32_t cal_at_ms;

void halo_sleep(uint32_t ms) {
  req_ms = ms, req = true;
}

uint8_t halo_woke() {
  return woke;
}

bool halo_asleep() {
  return asleep;
}

void halo_sleep_on_usb(bool on) {
  on_usb_ok = on;
}

void halo_stay_awake(bool hold) {
  __atomic_add_fetch(&holds, hold ? 1 : -1, __ATOMIC_RELAXED);
}

static const char *wake_name(uint8_t w) {
  return w == HALO_WAKE_TOUCH ? "touch" : w == HALO_WAKE_BOOT ? "BOOT" : w == HALO_WAKE_MOTION ? "motion" : "timer";
}

// The slow clock keeps time while asleep: the fast RC oscillator / 256 instead of the slow RC one, re-measured
// against the 40 MHz crystal every 5 minutes (1024 cycles — 15 ms)
static void clock_prepare() {
  if (!clock_ready) {
    rtc_clk_8m_enable(true, true);
    rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_ON);
    clock_ready = true, cal_at_ms = 0;
  }
  if (!cal_at_ms || millis() - cal_at_ms > 300000) {
    esp_clk_slowclk_cal_set(rtc_clk_cal(RTC_CAL_RTC_MUX, 1024));
    cal_at_ms = millis() | 1;
  }
}

static bool finger_down() {
  HaloTouchSample s;
  return halo_touch_samples(&s, 1) > 0;
}

static bool motion_new() {  // the board was lying still, and then it moved — after it was put to sleep
  return imu_on && imu_burst_us > asleep_at_us;
}

// One light sleep; motion — whether INT1 may wake it. The log line is the caller's: some wake-ups are internal
static uint8_t sleep_now(uint32_t ms, bool motion) {
  clock_prepare();
  ht_bus_lock();  // the touch task is not in the middle of an I2C transfer
  const gpio_num_t tp = (gpio_num_t)HALO_PIN_TP_INT, boot = (gpio_num_t)HALO_PIN_BOOT,
                   mo = (gpio_num_t)HALO_PIN_IMU_INT1;
  // The pins' own interrupts are off for the sleep. Their wake-up is by level, and so would be their interrupt: a
  // level that stays (INT1 after one motion event, when the board is then held still) fired it again and again —
  // core 1 frozen, the interrupt watchdog rebooted the board, and the level setting, which survives the reboot,
  // crashed the next start too: safe mode, a black screen (experiments/SleepLab: storm 0). The wake-up itself does
  // not need the interrupt
  gpio_intr_disable(tp);
  if (motion) gpio_intr_disable(mo);
  gpio_wakeup_enable(tp, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(boot, GPIO_INTR_LOW_LEVEL);
  const int mo_was = motion ? gpio_get_level(mo) : 0;
  if (motion) gpio_wakeup_enable(mo, mo_was ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  if (ms) esp_sleep_enable_timer_wakeup(ms * 1000ull);
  const uint32_t e0 = imu_edges;
  esp_light_sleep_start();
  // Who woke us — at once (a touch pulse is short; INT1 may be back where it was, its interrupt has counted it)
  const int lb = gpio_get_level(boot), lm = motion ? gpio_get_level(mo) : mo_was;
  const esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  // gpio_wakeup_disable also switches the pin interrupt off: give the touch and the sensor back their edges
  gpio_wakeup_disable(tp), gpio_set_intr_type(tp, GPIO_INTR_NEGEDGE), gpio_intr_enable(tp);
  gpio_wakeup_disable(boot);
  if (motion) gpio_wakeup_disable(mo), gpio_set_intr_type(mo, GPIO_INTR_ANYEDGE), gpio_intr_enable(mo);
  ht_bus_unlock();
  uint8_t w = c == ESP_SLEEP_WAKEUP_TIMER                      ? HALO_WAKE_TIMER
              : !lb                                            ? HALO_WAKE_BOOT
              : motion && (lm != mo_was || imu_edges != e0) ? HALO_WAKE_MOTION
                                                               : HALO_WAKE_TOUCH;
  if (w == HALO_WAKE_MOTION && imu_edges == e0) imu_edge_now();
  if (w != HALO_WAKE_TIMER) ht_kick();  // the report whose pulse woke the board: its interrupt was off
  // A finger moves the board too: give the touch task a moment to read a report — a finger there means a touch
  if (w == HALO_WAKE_MOTION) {
    vTaskDelay(pdMS_TO_TICKS(4));
    if (finger_down()) w = HALO_WAKE_TOUCH;
  }
  return w;
}

// Asleep for real until a wake-up the sketch should see. Motion that is not new (the board still moving since it
// was put to sleep) — 300 ms deaf to motion, then listen again: ~3 short wake-ups a second while it is carried
static uint8_t sleep_real() {
  static int64_t deaf_until;
  int quiet = 0;
  for (;;) {
    const int64_t now = esp_timer_get_time(), gone_ms = (now - asleep_at_us) / 1000;
    if (sleep_ms && gone_ms >= sleep_ms) return HALO_WAKE_TIMER;
    uint32_t ms = sleep_ms ? (uint32_t)(sleep_ms - gone_ms) : 0;
    const bool deaf = now < deaf_until;
    if (deaf) {
      const uint32_t d = (uint32_t)((deaf_until - now) / 1000) + 1;
      ms = ms ? min(ms, d) : d;
    }
    const uint8_t w = sleep_now(ms, imu_on && !deaf);
    if (w == HALO_WAKE_TIMER && !(sleep_ms && (esp_timer_get_time() - asleep_at_us) / 1000 >= sleep_ms)) {
      quiet++;
      continue;  // the end of a deaf spell, not the sketch's timer
    }
    if (w == HALO_WAKE_MOTION && !motion_new()) {
      deaf_until = esp_timer_get_time() + 300000, quiet++;
      continue;
    }
    if (quiet) LOG("sleep: %d wake-ups on motion that was not new (still moving since sleep)\n", quiet);
    return w;
  }
}

void hp_frame_end() {
  woke = 0;
  if (req) {
    req = false;
    if (!asleep) asleep_at_us = esp_timer_get_time(), boot_armed = touch_armed = false;
    sleep_ms = req_ms, asleep = true;
  }
  if (!asleep) return;
  const int64_t gone_ms = (esp_timer_get_time() - asleep_at_us) / 1000;
  // BOOT and a touch count once released: the press or tap that put the board to sleep is not a wake-up
  const bool boot_up = gpio_get_level((gpio_num_t)HALO_PIN_BOOT), finger = finger_down();
  if (boot_up) boot_armed = true;
  if (!finger) touch_armed = true;
  uint8_t w = 0;
  if (sleep_ms && gone_ms >= sleep_ms) w = HALO_WAKE_TIMER;
  else if (boot_armed && !boot_up) w = HALO_WAKE_BOOT;
  else if (touch_armed && finger) w = HALO_WAKE_TOUCH;
  else if (motion_new()) w = HALO_WAKE_MOTION;
  if (w) {
    LOG("sleep: %.1f s up (USB or held), woke by %s\n", gone_ms / 1000.0f, wake_name(w));
  } else {
    const HaloBattery b = halo_battery();
    const bool usb = !b.ok || b.usb;  // no data yet — as on USB: stay up
    // Really asleep: nobody holds the board up, it is not on USB (or allowed), BOOT and the finger are up
    if (!holds && (!usb || on_usb_ok) && boot_up && !finger) {
      w = sleep_real();
      LOG("sleep: %.1f s, woke by %s\n", (esp_timer_get_time() - asleep_at_us) / 1e6f, wake_name(w));
    } else if (usb && !on_usb_ok && !told_usb) {
      told_usb = true;
      LOG("sleep: on USB power — staying up, waking the same way (halo_sleep_on_usb to really sleep)\n");
    }
  }
  if (!w) return;
  asleep = false, woke = w;
  if (w == HALO_WAKE_TOUCH) hu_swallow_touch();
  if (w == HALO_WAKE_BOOT) hf_swallow_boot();
}
