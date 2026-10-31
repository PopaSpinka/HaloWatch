// Свой драйвер тача CST9217 (I2C 0x5A) для Halo.
//
// Пока палец на экране, чип ~60 раз в секунду опускает линию INT (GPIO11) — новый отчёт. Прерывание запоминает
// время и будит задачу чтения (ядро 0). Отчёт: записать 0xD0 0x00, прочитать 15 байт, подтвердить 0xD0 0x00 0xAB.
//   байт 6 — 0xAB (отчёт настоящий); байт 5 & 0x7F — сколько пальцев;
//   палец: [id << 4 | состояние (6 — нажат)] [x >> 4] [y >> 4] [x & 15 << 4 | y & 15];
//   оси зеркальные: x = 466 − x, y = 466 − y.
// Когда палец убрали, приходит отчёт без пальцев — отпускаем сразу (60 мс без отчётов — тоже).
//
// На той же шине — чип питания AXP2101 (0x34): кнопку PWR читает он, а не ESP32. Эта же задача раз в 100 мс читает
// его флаги прерываний (регистр 0x49: бит 3 — короткое нажатие, после отпускания; бит 2 — долгое, ~1,5 с, пока
// держат) и сбрасывает их, записав те же биты. Шина у драйвера i2c_master одна на оба чипа и сама их чередует.
// Раз в 2 с — батарея: есть ли (0x00, бит 3), USB (0x00 бит 5 и 0x01 бит 3 = 0), заряжается (0x01, биты 6..5 = 1),
// процент (0xA4 — «топливомер» чипа, включён битом 3 в 0x18), напряжение (0x34/0x35, мВ — АЦП включён битом 0 в 0x30).
#include "halo_internal.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev, pmu;
static SemaphoreHandle_t bus_mx;  // задача тача держит шину на время своих обменов, сон — на время сна

void ht_bus_lock() {
  if (bus_mx) xSemaphoreTake(bus_mx, portMAX_DELAY);
}
void ht_bus_unlock() {
  if (bus_mx) xSemaphoreGive(bus_mx);
}
static volatile uint8_t pwr_ev;  // HALO_PWR_* с прошлого halo_pwr_event()
static uint32_t pmu_at, batt_at;
static HaloBattery batt;
static portMUX_TYPE batt_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t pmu_reg(uint8_t r) {
  uint8_t v = 0;
  i2c_master_transmit_receive(pmu, &r, 1, &v, 1, 20);
  return v;
}

static void pmu_set_bits(uint8_t r, uint8_t bits) {
  const uint8_t w[2] = {r, (uint8_t)(pmu_reg(r) | bits)};
  i2c_master_transmit(pmu, w, 2, 20);
}

static void battery_poll() {
  if (!pmu || (batt.ok && millis() - batt_at < 2000)) return;
  batt_at = millis();
  const uint8_t s1 = pmu_reg(0x00), s2 = pmu_reg(0x01);
  HaloBattery b;
  b.ok = true, b.present = s1 & 0x08, b.usb = (s1 & 0x20) && !(s2 & 0x08), b.charging = (s2 >> 5) == 1;
  b.percent = b.present ? (int8_t)min(100, (int)pmu_reg(0xA4)) : -1;
  b.mv = b.present ? (uint16_t)((pmu_reg(0x34) & 0x1F) << 8 | pmu_reg(0x35)) : 0;
  portENTER_CRITICAL(&batt_mux);
  batt = b;
  portEXIT_CRITICAL(&batt_mux);
}

HaloBattery halo_battery() {
  portENTER_CRITICAL(&batt_mux);
  const HaloBattery b = batt;
  portEXIT_CRITICAL(&batt_mux);
  return b;
}
static TaskHandle_t task;
static DRAM_ATTR volatile uint32_t irq_us;

#define HIST 8
static HaloTouchSample hist[HIST];
static int hist_n, hist_head;
static portMUX_TYPE hist_mux = portMUX_INITIALIZER_UNLOCKED;

int halo_touch_samples(HaloTouchSample *out, int max) {
  portENTER_CRITICAL(&hist_mux);
  const int n = min(max, hist_n);
  for (int i = 0; i < n; i++) out[i] = hist[(hist_head - i + HIST) % HIST];
  portEXIT_CRITICAL(&hist_mux);
  return n;
}

static void IRAM_ATTR tp_isr(void *) {
  irq_us = (uint32_t)esp_timer_get_time();
  if (halo_trace_on) halo_trace_put(TR_TOUCH, irq_us, 0, 0);
  BaseType_t woken = pdFALSE;
  if (task) vTaskNotifyGiveFromISR(task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// Для журнала: где коснулся и отпустил, сколько отчётов
static int16_t down_x, down_y, last_x, last_y;
static uint16_t reports;

static void release(const char *why) {
  portENTER_CRITICAL(&hist_mux);
  hist_n = 0;
  portEXIT_CRITICAL(&hist_mux);
  if (reports) LOG("touch up %d,%d: moved %+d,%+d, %u reports (%s)\n", last_x, last_y, last_x - down_x, last_y - down_y,
                   reports, why);
  reports = 0;
}

// Кнопка PWR: флаги чипа питания — раз в 100 мс (из задачи тача: она просыпается не реже чем через 60 мс)
static void pmu_poll() {
  if (!pmu || millis() - pmu_at < 100) return;
  pmu_at = millis();
  uint8_t reg = 0x49, st = 0;
  if (i2c_master_transmit_receive(pmu, &reg, 1, &st, 1, 20) != ESP_OK || !(st &= 0x0C)) return;
  const uint8_t clr[2] = {0x49, st};
  i2c_master_transmit(pmu, clr, 2, 20);
  const uint8_t ev = (st & 0x08 ? HALO_PWR_CLICK : 0) | (st & 0x04 ? HALO_PWR_LONG : 0);
  __atomic_fetch_or(&pwr_ev, ev, __ATOMIC_RELAXED);
  LOG("pwr:%s%s\n", ev & HALO_PWR_CLICK ? " click" : "", ev & HALO_PWR_LONG ? " long" : "");
}

uint8_t halo_pwr_event() {
  return __atomic_exchange_n(&pwr_ev, 0, __ATOMIC_RELAXED);
}

static void touch_task(void *) {
  for (;;) {
    ht_bus_lock();
    pmu_poll();
    battery_poll();
    ht_bus_unlock();
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60))) {  // 60 мс без отчётов — палец убрали
      if (hist_n) release("timeout");
      continue;
    }
    const uint32_t t = irq_us;
    static const uint8_t cmd[2] = {0xD0, 0x00}, ack[3] = {0xD0, 0x00, 0xAB};
    uint8_t b[15] = {};
    ht_bus_lock();
    const bool ok = i2c_master_transmit_receive(dev, cmd, 2, b, sizeof(b), 20) == ESP_OK;
    if (ok) i2c_master_transmit(dev, ack, 3, 20);
    ht_bus_unlock();
    if (!ok) continue;
    if (b[6] != 0xAB) continue;
    const int n = b[5] & 0x7F;
    if (!n || (b[0] & 0x0F) != 6) {
      release("lift");
      continue;
    }
    const int x = HALO_W - ((b[1] << 4) | (b[3] >> 4)), y = HALO_H - ((b[2] << 4) | (b[3] & 0x0F));
    const HaloTouchSample s = {(int16_t)constrain(x, 0, HALO_W - 1), (int16_t)constrain(y, 0, HALO_H - 1), t};
    portENTER_CRITICAL(&hist_mux);
    hist_head = (hist_head + 1) % HIST;
    hist[hist_head] = s;
    if (hist_n < HIST) hist_n++;
    portEXIT_CRITICAL(&hist_mux);
    trace_mark(TR_TOUCH_READ, s.x, s.y);
    if (!reports++) down_x = s.x, down_y = s.y, LOG("touch down %d,%d\n", s.x, s.y);
    last_x = s.x, last_y = s.y;
  }
}

void ht_kick() {  // read a report now (its interrupt may have been missed: halo_power.cpp turns it off in sleep)
  if (task) xTaskNotifyGive(task);
}

i2c_master_dev_t *ht_i2c_device(uint8_t addr) {
  if (!bus || i2c_master_probe(bus, addr, 50) != ESP_OK) return nullptr;
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7, dc.device_address = addr, dc.scl_speed_hz = 400000;
  i2c_master_dev_handle_t h = nullptr;
  return i2c_master_bus_add_device(bus, &dc, &h) == ESP_OK ? h : nullptr;
}

HaloI2c halo_i2c(uint8_t addr) {
  return ht_i2c_device(addr);
}

bool halo_i2c_write(HaloI2c d, uint8_t reg, uint8_t val) {
  const uint8_t w[2] = {reg, val};
  if (!d) return false;
  ht_bus_lock();
  const bool ok = i2c_master_transmit(d, w, 2, 20) == ESP_OK;
  ht_bus_unlock();
  return ok;
}

bool halo_i2c_read(HaloI2c d, uint8_t reg, uint8_t *buf, int n) {
  if (!d) return false;
  ht_bus_lock();
  const bool ok = i2c_master_transmit_receive(d, &reg, 1, buf, n, 20) == ESP_OK;
  ht_bus_unlock();
  return ok;
}

bool ht_begin() {
  bus_mx = xSemaphoreCreateMutex();
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_0;
  bc.sda_io_num = (gpio_num_t)HALO_PIN_SDA;
  bc.scl_io_num = (gpio_num_t)HALO_PIN_SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = 1;
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = 0x5A;
  dc.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return false;

  gpio_set_direction((gpio_num_t)HALO_PIN_TP_RST, GPIO_MODE_OUTPUT);  // сброс: низкий 10 мс
  gpio_set_level((gpio_num_t)HALO_PIN_TP_RST, 0);
  delay(10);
  gpio_set_level((gpio_num_t)HALO_PIN_TP_RST, 1);
  delay(50);
  const bool found = i2c_master_probe(bus, 0x5A, 50) == ESP_OK;
  // Чип питания: прерывания короткого и долгого нажатия PWR (регистр 0x41, биты 3 и 2), старые флаги — сбросить
  dc.device_address = 0x34;
  if (i2c_master_probe(bus, 0x34, 50) == ESP_OK && i2c_master_bus_add_device(bus, &dc, &pmu) == ESP_OK) {
    uint8_t reg = 0x41, en = 0;
    i2c_master_transmit_receive(pmu, &reg, 1, &en, 1, 20);
    const uint8_t w[2] = {0x41, (uint8_t)(en | 0x0C)}, c[2] = {0x49, 0x0F};
    i2c_master_transmit(pmu, w, 2, 20);
    i2c_master_transmit(pmu, c, 2, 20);
    pmu_set_bits(0x30, 0x01), pmu_set_bits(0x68, 0x01), pmu_set_bits(0x18, 0x08);  // АЦП батареи, её поиск, топливомер
  } else {
    pmu = nullptr;
  }
  halo_command("battery", [](const char *) {
    const HaloBattery b = halo_battery();
    LOG("battery: %s, %d%%, %u mV, usb %s, %s\n", b.present ? "present" : "none", b.percent, b.mv, b.usb ? "yes" : "no",
        b.charging ? "charging" : "not charging");
  });
  halo_command("pwr", [](const char *a) {  // pwr / pwr long — то же, что нажатие кнопки PWR
    __atomic_fetch_or(&pwr_ev, strstr(a, "long") ? HALO_PWR_LONG : HALO_PWR_CLICK, __ATOMIC_RELAXED);
  });

  xTaskCreatePinnedToCore(touch_task, "halo_touch", 3072, nullptr, 12, &task, 0);
  gpio_config_t io = {};
  io.pin_bit_mask = 1ull << HALO_PIN_TP_INT;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.intr_type = GPIO_INTR_NEGEDGE;
  gpio_config(&io);
  gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  gpio_isr_handler_add((gpio_num_t)HALO_PIN_TP_INT, tp_isr, nullptr);
  LOG("touch: CST9217 %s, PWR button (AXP2101) %s\n", found ? "ok" : "NOT FOUND", pmu ? "ok" : "NOT FOUND");
  return found;
}
