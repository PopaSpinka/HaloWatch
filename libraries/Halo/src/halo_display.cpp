// Свой драйвер дисплея CO5300 (QSPI) для Halo.
//
// Шина SPI2 — только наша (в AmoledBoard её делили с Arduino_GFX и брали на каждый кадр — 0,06 мс, а окно первой
// пачки шло через драйвер — ещё 0,21 мс). Драйвер ESP-IDF нужен лишь при запуске: включить панель и выставить
// регистры SPI для трёх видов передачи. Их мы запоминаем и дальше повторяем сами, из IRAM.
//
// Протокол CO5300:
//   * команда — одной линией: байт 0x02, адрес 0x00 команда 0x00, данные;
//   * пиксели — четырьмя линиями: байт 0x32, адрес 0x00 0x3C 0x00 («продолжить запись»), RGB565 старшим байтом;
//   * окно: CASET (0x2A, столбцы со сдвигом 6), PASET (0x2B, строки), RAMWR (0x2C). Начало окна — чётное,
//     конец — нечётный. Окно до низа экрана: следующая пачка под этой с теми же столбцами идёт без команд окна;
//   * CS (GPIO12) — свой: низкий на время передачи.
// Очередь пачек: каждая — в своём DMA-буфере. Её отправляет прерывание канала DMA «всё ушло» (TOTAL_EOF): кончилась
// пачка — тут же следующая. Свободный буфер ждём на семафоре (ядро спит, а не крутится в цикле: так было 2–4 мс
// на кадр в AmoledBoard).
#include "halo_internal.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_intr_alloc.h>
#include <hal/dma_types.h>
#include <hal/gdma_ll.h>
#include <hal/spi_ll.h>
#include <soc/gdma_channel.h>

#define X_OFFSET 6          // смещение столбцов панели
#define MAX_PX (HALO_ROWS * HALO_W)
#define MAX_BUFS 4

static spi_device_handle_t dev_init, dev_fast;

struct Shape {  // регистры SPI для одного вида передачи — как их выставил драйвер
  uint32_t ctrl, user, user1, user2, misc;
};
enum { SHAPE_CMD4, SHAPE_CMD0, SHAPE_PX };
static DRAM_ATTR Shape shapes[3];
static DRAM_ATTR dma_descriptor_t desc[(MAX_PX * 2 + 4091) / 4092];
static int dma_ch = -1;
static intr_handle_t dma_intr;

struct Job {
  const uint16_t *buf;
  int bytes;
  bool win;
  uint32_t caset, paset;  // 4 байта данных окна, как в регистре W0
  uint16_t y;
};
static uint16_t *bufs[MAX_BUFS];
static int nbufs, next_buf;
static DRAM_ATTR Job jobs[MAX_BUFS];
static DRAM_ATTR volatile int q_head, q_count, fly = -1;
static DRAM_ATTR uint32_t fly_t;
static DRAM_ATTR SemaphoreHandle_t free_sem;
static portMUX_TYPE q_mux = portMUX_INITIALIZER_UNLOCKED;
static int last_x = -1, last_w, last_y;  // окно прошлой пачки

// ---------- Свои передачи (IRAM) ----------
static inline void IRAM_ATTR cs_low() {
  REG_WRITE(GPIO_OUT_W1TC_REG, 1u << HALO_PIN_CS);
}
static inline void IRAM_ATTR cs_high() {
  REG_WRITE(GPIO_OUT_W1TS_REG, 1u << HALO_PIN_CS);
}

static void IRAM_ATTR set_shape(int kind, uint32_t addr) {
  spi_dev_t *hw = &GPSPI2;
  const Shape &s = shapes[kind];
  spi_ll_clear_int_stat(hw);
  hw->ctrl.val = s.ctrl, hw->user.val = s.user, hw->user1.val = s.user1, hw->user2.val = s.user2;
  hw->misc.val = s.misc;
  hw->addr = addr << 8;  // 24 бита адреса — от старшего бита регистра
}

static inline void IRAM_ATTR go() {
  cs_low();
  spi_ll_apply_config(&GPSPI2);
  spi_ll_user_start(&GPSPI2);
}

static inline void IRAM_ATTR finish() {
  while (!spi_ll_usr_is_done(&GPSPI2)) {
  }
  cs_high();
}

// Команда с 0..4 байтами данных (из регистра W0, без DMA) — ~1 мкс, ждём тут же
static void IRAM_ATTR own_cmd(uint8_t cmd, uint32_t data, int n) {
  spi_dev_t *hw = &GPSPI2;
  set_shape(n ? SHAPE_CMD4 : SHAPE_CMD0, (uint32_t)cmd << 8);
  spi_ll_dma_tx_enable(hw, 0);
  if (n) {
    hw->data_buf[0] = data;
    spi_ll_set_mosi_bitlen(hw, n * 8);
  }
  go();
  finish();
}

static void IRAM_ATTR own_px(const uint16_t *buf, int bytes) {  // пиксели по DMA, конца не ждём
  spi_dev_t *hw = &GPSPI2;
  const uint8_t *p = (const uint8_t *)buf;
  int n = 0;
  for (int left = bytes; left > 0; n++) {
    const int len = left > 4092 ? 4092 : left;
    desc[n].dw0.size = len;
    desc[n].dw0.length = len;
    desc[n].dw0.suc_eof = 0;
    desc[n].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    desc[n].buffer = (void *)p;
    desc[n].next = &desc[n + 1];
    p += len, left -= len;
  }
  desc[n - 1].dw0.suc_eof = 1;
  desc[n - 1].next = nullptr;
  set_shape(SHAPE_PX, 0x003C00);  // Memory Write Continue
  spi_ll_set_mosi_bitlen(hw, bytes * 8);
  gdma_ll_tx_reset_channel(&GDMA, dma_ch);
  spi_ll_dma_tx_fifo_reset(hw);
  spi_ll_outfifo_empty_clr(hw);
  spi_ll_dma_tx_enable(hw, 1);
  gdma_ll_tx_clear_interrupt_status(&GDMA, dma_ch, GDMA_LL_EVENT_TX_TOTAL_EOF);
  gdma_ll_tx_set_desc_addr(&GDMA, dma_ch, (uint32_t)desc);
  gdma_ll_tx_start(&GDMA, dma_ch);
  go();
}

static void IRAM_ATTR kick() {  // под q_mux: шина свободна — следующая пачка из очереди
  if (fly >= 0 || !q_count) return;
  const int i = q_head;
  const Job &j = jobs[i];
  if (j.win) own_cmd(0x2A, j.caset, 4), own_cmd(0x2B, j.paset, 4), own_cmd(0x2C, 0, 0);
  fly = i;
  fly_t = halo_trace_on ? trace_now() : 0;
  q_head = (i + 1) % nbufs, q_count--;
  own_px(j.buf, j.bytes);
}

static void IRAM_ATTR dma_isr(void *) {
  const uint32_t st = gdma_ll_tx_get_interrupt_status(&GDMA, dma_ch, false);
  gdma_ll_tx_clear_interrupt_status(&GDMA, dma_ch, st);
  if (!(st & GDMA_LL_EVENT_TX_TOTAL_EOF)) return;
  bool freed = false;
  portENTER_CRITICAL_ISR(&q_mux);
  if (fly >= 0) {
    finish();  // всё ушло в SPI — дождаться последних байт (~2 мкс) и поднять CS
    if (fly_t) trace_span(TR_DMA, fly_t, jobs[fly].y);
    fly = -1, freed = true;
    kick();
  }
  portEXIT_CRITICAL_ISR(&q_mux);
  if (!freed) return;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(free_sem, &woken);  // вне критической секции
  if (woken) portYIELD_FROM_ISR();
}

// ---------- Сторож очереди ----------
// Пока работает Wi-Fi, DMA иногда не успевает подать байты: SPI досчитывает свои биты и останавливается (usr = 0), а
// у DMA в очереди остаются данные — «всё ушло» (TOTAL_EOF) не наступит никогда, и кадры встали бы навсегда (так и
// было: sys показывал dma busy, spi2 usr 0, fifo полон). Не от скорости шины (на 40 МГц так же) и не от приоритета
// каналов DMA. AmoledBoard в том же случае считает пачку законченной, как только закончил SPI (blit_poll), — здесь
// так же: буфер не освободился за 3 мс (пачка идёт 0,4 мс), SPI стоит, а «всё ушло» нет — сбросить канал, запустить
// следующую пачку и перерисовать эту (её строки могли уйти с мусором) в следующем кадре
static volatile uint32_t stalls;

static void take_free() {
  while (xSemaphoreTake(free_sem, pdMS_TO_TICKS(3)) != pdTRUE) {
    int y = -1;
    portENTER_CRITICAL(&q_mux);
    if (fly >= 0 && spi_ll_usr_is_done(&GPSPI2) &&
        !(gdma_ll_tx_get_interrupt_status(&GDMA, dma_ch, true) & GDMA_LL_EVENT_TX_TOTAL_EOF)) {
      gdma_ll_tx_stop(&GDMA, dma_ch);
      gdma_ll_tx_reset_channel(&GDMA, dma_ch);
      spi_ll_dma_tx_fifo_reset(&GPSPI2);
      gdma_ll_tx_clear_interrupt_status(&GDMA, dma_ch, 0xFF);
      cs_high();
      y = jobs[fly].y, fly = -1;
      kick();
    }
    portEXIT_CRITICAL(&q_mux);
    if (y < 0) continue;
    xSemaphoreGive(free_sem);
    halo_damage(0, y, HALO_W - 1, y + HALO_ROWS - 1);
    if (++stalls == 1 || stalls % 100 == 0) LOG("display: DMA stalled %lu times (SPI ended first), queue restarted\n",
                                                (unsigned long)stalls);
  }
}

uint16_t *hd_buffer() {
  if (uxSemaphoreGetCount(free_sem) == 0) {
    const uint32_t t0 = trace_now();
    take_free();
    trace_span(TR_BLIT_WAIT, t0);
  } else {
    xSemaphoreTake(free_sem, 0);
  }
  return bufs[next_buf];
}

void hd_send(int x0, int y, int w, int rows) {
  const bool cont = x0 == last_x && w == last_w && y == last_y;
  const int x1 = x0 + X_OFFSET, x2 = x1 + w - 1, y2 = HALO_H - 1;  // окно — до низа экрана
  const uint8_t win[2][4] = {{(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2},
                             {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y2 >> 8), (uint8_t)y2}};
  last_x = x0, last_w = w, last_y = y + rows;
  Job j = {bufs[next_buf], w * rows * 2, !cont, 0, 0, (uint16_t)y};
  memcpy(&j.caset, win[0], 4);
  memcpy(&j.paset, win[1], 4);
  portENTER_CRITICAL(&q_mux);
  jobs[(q_head + q_count) % nbufs] = j;
  q_count++;
  kick();
  portEXIT_CRITICAL(&q_mux);
  next_buf = (next_buf + 1) % nbufs;
}

void hd_wait_idle() {
  for (int i = 0; i < nbufs; i++) take_free();
  for (int i = 0; i < nbufs; i++) xSemaphoreGive(free_sem);
  last_x = -1;  // первая пачка следующего кадра — всегда с командами окна
}

void hd_set_brightness(uint8_t level) {  // шина свободна (между кадрами)
  own_cmd(0x51, level, 1);
}

void hd_set_power(bool on) {  // 0x28 — панель гаснет (картинка в её памяти остаётся), 0x29 — снова видна
  own_cmd(on ? 0x29 : 0x28, 0, 0);
}

int hd_buffers() {
  return nbufs;
}

void hd_regs(char *out, int n) {
  if (dma_ch < 0) return (void)snprintf(out, n, "no dma");
  snprintf(out, n, "gdma ch%d: peri %lu, int raw %02lx ena %02lx, fifo %08lx, link %08lx, state %08lx; spi2 usr %d",
           dma_ch, (unsigned long)GDMA.channel[dma_ch].out.peri_sel.sel,
           (unsigned long)GDMA.channel[dma_ch].out.int_raw.val, (unsigned long)GDMA.channel[dma_ch].out.int_ena.val,
           (unsigned long)GDMA.channel[dma_ch].out.outfifo_status.val, (unsigned long)GDMA.channel[dma_ch].out.link.val,
           (unsigned long)GDMA.channel[dma_ch].out.state.val, (int)GPSPI2.cmd.usr);
  snprintf(out + strlen(out), n - strlen(out), "; stalls %lu", (unsigned long)stalls);
}

void hd_state(int *f, int *queued, int *free_bufs) {
  *f = fly, *queued = q_count, *free_bufs = free_sem ? (int)uxSemaphoreGetCount(free_sem) : -1;
}

// ---------- Сигнал TE ----------
static DRAM_ATTR volatile uint32_t te_rise, te_period, te_count, te_blank = 616;
static DRAM_ATTR volatile TaskHandle_t te_waiter;
static portMUX_TYPE te_mux = portMUX_INITIALIZER_UNLOCKED;  // номер сигнала и его время — парой (hd_te_snapshot)

static void IRAM_ATTR te_isr(void *) {
  const uint32_t now = (uint32_t)esp_timer_get_time();
  if ((REG_READ(GPIO_IN_REG) >> HALO_PIN_TE) & 1) {  // начало паузы между проходами
    if (halo_trace_on) halo_trace_put(TR_TE, now, 0, 0);
    portENTER_CRITICAL_ISR(&te_mux);
    // Период — только между соседними сигналами: после сна или паузы панели «период» был бы длиной в сон, и движок
    // растянул бы по нему проход (вслед за проходом ждал бы секунды)
    if (te_rise && now - te_rise < 25000) te_period = now - te_rise;
    te_rise = now;
    te_count++;
    portEXIT_CRITICAL_ISR(&te_mux);
    TaskHandle_t w = te_waiter;
    if (w) {
      BaseType_t woken = pdFALSE;
      vTaskNotifyGiveFromISR(w, &woken);
      if (woken) portYIELD_FROM_ISR();
    }
  } else {  // начало прохода по строкам
    if (now - te_rise < 2000) te_blank = now - te_rise;
    if (halo_trace_on) halo_trace_put(TR_SCAN, now, 0, 0);
  }
}

// Ждущий записан до проверки счётчика: сигнал, пришедший между проверкой и сном, не теряется
bool hd_wait_te(uint32_t count, uint32_t timeout_ms) {
  te_waiter = xTaskGetCurrentTaskHandle();
  ulTaskNotifyTake(pdTRUE, 0);  // старые уведомления не считаются — смотрим на счётчик
  bool ok = true;
  while (ok && (int32_t)(te_count - count) < 0) ok = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) > 0;
  te_waiter = nullptr;
  return ok;
}

void hd_te_snapshot(uint32_t *count, uint32_t *rise) {
  portENTER_CRITICAL(&te_mux);
  *count = te_count, *rise = te_rise;
  portEXIT_CRITICAL(&te_mux);
}

uint32_t hd_te_count() {
  return te_count;
}
uint32_t hd_te_period() {
  return (uint32_t)esp_timer_get_time() - te_rise < 100000 ? te_period : 0;
}
uint32_t hd_te_rise() {
  return te_rise;
}
uint32_t hd_te_blank() {
  return te_blank;
}

// ---------- Запуск ----------
static void IRAM_ATTR cb_cs_low(spi_transaction_t *) {
  cs_low();
}
static void IRAM_ATTR cb_cs_high(spi_transaction_t *) {
  cs_high();
}

static spi_device_handle_t add_dev(uint32_t hz) {
  spi_device_interface_config_t c = {};
  c.command_bits = 8;
  c.address_bits = 24;
  c.mode = 0;
  c.clock_source = SPI_CLK_SRC_DEFAULT;
  c.clock_speed_hz = hz;
  c.spics_io_num = -1;  // CS ведём сами
  c.flags = SPI_DEVICE_HALFDUPLEX;
  c.queue_size = 1;
  c.pre_cb = cb_cs_low;
  c.post_cb = cb_cs_high;
  spi_device_handle_t h = nullptr;
  spi_bus_add_device(SPI2_HOST, &c, &h);
  return h;
}

// Передача через драйвер ESP-IDF (только при запуске); shape ≥ 0 — запомнить её регистры
static void idf_tx(spi_device_handle_t d, uint8_t cmd, const void *data, int bytes, bool quad, int shape = -1) {
  spi_transaction_t t = {};
  t.cmd = quad ? 0x32 : 0x02;
  t.addr = quad ? 0x003C00 : (uint32_t)cmd << 8;
  t.flags = quad ? SPI_TRANS_MODE_QIO : 0;
  if (bytes && bytes <= 4 && !quad) {
    t.flags |= SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, data, bytes);
  } else {
    t.tx_buffer = bytes ? data : nullptr;  // без данных — только nullptr, иначе драйвер отказывает (и assert)
  }
  t.length = bytes * 8;
  spi_device_polling_start(d, &t, portMAX_DELAY);
  if (shape >= 0) {
    spi_dev_t *hw = &GPSPI2;
    shapes[shape] = {hw->ctrl.val, hw->user.val, hw->user1.val, hw->user2.val, hw->misc.val};
  }
  spi_device_polling_end(d, portMAX_DELAY);
}

static void panel_cmd(uint8_t cmd, int data = -1) {
  const uint8_t d = data;
  idf_tx(dev_init, cmd, &d, data >= 0 ? 1 : 0, false);
}

bool hd_begin(uint32_t hz, int buffers, uint8_t brightness) {
  // Падение во сне оставляет выводы тача и датчика движения «будить по уровню» с включённым прерыванием — перезагрузка
  // это сохраняет, и первое же прерывание (здесь ставится их служба) зацикливалось: второе падение подряд и безопасный
  // режим с чёрным экраном (halo_power.cpp). Сначала — сбросить
  for (int p : {HALO_PIN_TP_INT, HALO_PIN_IMU_INT1, HALO_PIN_BOOT})
    gpio_wakeup_disable((gpio_num_t)p), gpio_intr_disable((gpio_num_t)p);
  gpio_set_direction((gpio_num_t)HALO_PIN_CS, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)HALO_PIN_CS, 1);
  gpio_set_direction((gpio_num_t)HALO_PIN_RST, GPIO_MODE_OUTPUT);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = HALO_PIN_SD0;
  bus.miso_io_num = HALO_PIN_SD1;
  bus.quadwp_io_num = HALO_PIN_SD2;
  bus.quadhd_io_num = HALO_PIN_SD3;
  bus.sclk_io_num = HALO_PIN_SCLK;
  bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
  bus.max_transfer_sz = MAX_PX * 2;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS | SPICOMMON_BUSFLAG_QUAD;
  hc_stage(1);
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    LOG("display: SPI bus init failed\n");
    return false;
  }
  dev_init = add_dev(40000000);  // включение панели — на паспортной частоте
  dev_fast = add_dev(hz);
  if (!dev_init || !dev_fast) return false;

  hc_stage(2);
  // Сброс и включение (последовательность — как у Waveshare)
  gpio_set_level((gpio_num_t)HALO_PIN_RST, 1);
  delay(10);
  gpio_set_level((gpio_num_t)HALO_PIN_RST, 0);
  delay(120);
  gpio_set_level((gpio_num_t)HALO_PIN_RST, 1);
  delay(120);
  panel_cmd(0x11);  // Sleep Out
  delay(120);
  panel_cmd(0xFE, 0x00);
  panel_cmd(0xC4, 0x80);  // SPI mode control
  panel_cmd(0x3A, 0x55);  // 16 бит на пиксель
  panel_cmd(0x53, 0x20);  // яркость управляется командой 0x51
  panel_cmd(0x63, 0xFF);  // яркость HBM
  panel_cmd(0x36, 0x00);  // MADCTL: без поворота, RGB
  panel_cmd(0x20);        // без инверсии

  // Буферы и первая передача пикселей через драйвер — чёрный экран и заодно регистры видов передачи
  nbufs = constrain(buffers, 2, MAX_BUFS);
  for (int i = 0; i < nbufs; i++) {
    bufs[i] = (uint16_t *)heap_caps_aligned_alloc(16, MAX_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!bufs[i]) {
      LOG("display: no DMA memory\n");
      return false;
    }
    memset(bufs[i], 0, MAX_PX * 2);
  }
  const uint8_t caset[4] = {0, X_OFFSET, (uint8_t)((X_OFFSET + HALO_W - 1) >> 8), (uint8_t)(X_OFFSET + HALO_W - 1)};
  const uint8_t paset[4] = {0, 0, (uint8_t)((HALO_H - 1) >> 8), (uint8_t)(HALO_H - 1)};
  idf_tx(dev_fast, 0x2A, caset, 4, false, SHAPE_CMD4);
  idf_tx(dev_fast, 0x2B, paset, 4, false);
  idf_tx(dev_fast, 0x2C, nullptr, 0, false, SHAPE_CMD0);
  for (int y = 0; y < HALO_H; y += HALO_ROWS)
    idf_tx(dev_fast, 0, bufs[0], min(HALO_ROWS, HALO_H - y) * HALO_W * 2, true, SHAPE_PX);

  hc_stage(3);
  // Канал GDMA, который драйвер подключил к SPI2, и своё прерывание «всё ушло»
  for (int ch = 0; ch < SOC_GDMA_PAIRS_PER_GROUP; ch++)
    if (GDMA.channel[ch].out.peri_sel.sel == SOC_GDMA_TRIG_PERIPH_SPI2) dma_ch = ch;
  if (dma_ch < 0 || esp_intr_alloc(ETS_DMA_OUT_CH0_INTR_SOURCE + dma_ch, ESP_INTR_FLAG_IRAM, dma_isr, nullptr,
                                   &dma_intr) != ESP_OK) {
    LOG("display: no DMA interrupt\n");
    return false;
  }
  gdma_ll_tx_enable_interrupt(&GDMA, dma_ch, GDMA_LL_EVENT_TX_TOTAL_EOF, true);
  free_sem = xSemaphoreCreateCounting(nbufs, nbufs);
  spi_device_acquire_bus(dev_fast, portMAX_DELAY);  // шина наша навсегда: драйвер её больше не трогает

  own_cmd(0x29, 0, 0);           // Display On — уже чёрный
  own_cmd(0x51, brightness, 1);  // яркость
  own_cmd(0x58, 0x00, 1);        // без «усиления контраста»
  own_cmd(0x35, 0x00, 1);        // TE: линия поднимается в паузе между проходами

  gpio_config_t te = {};
  te.pin_bit_mask = 1ull << HALO_PIN_TE;
  te.mode = GPIO_MODE_INPUT;
  te.intr_type = GPIO_INTR_ANYEDGE;
  gpio_config(&te);
  gpio_install_isr_service(ESP_INTR_FLAG_IRAM);  // уже стоит — не страшно
  gpio_isr_handler_add((gpio_num_t)HALO_PIN_TE, te_isr, nullptr);
  LOG("display: CO5300 ok, bus %lu MHz, %d DMA buffers, DMA channel %d\n", (unsigned long)(hz / 1000000), nbufs,
      dma_ch);
  return true;
}
