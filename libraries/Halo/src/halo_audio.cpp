// Звук для Halo: динамик через кодек ES8311 (I2C 0x18 — на шине тача) и усилитель (GPIO46).
//
// I2S (драйвер ESP-IDF i2s_std): ESP32 ведёт шину — MCLK = 256·fs на GPIO16, BCLK 9, LRCK 45, данные 8; 16 бит,
// стерео (в оба канала — одно и то же: динамик один). Кодек — ведомый, тактируется от MCLK. Регистры ES8311 — из
// драйвера Espressif (es8311.c, Apache-2.0; он же в lib/AmoledAudio): сброс, тактирование для MCLK = 256·fs
// (одинаковое для 16 000…48 000 Гц), формат 16 бит, питание ЦАП и выхода, громкость — регистр 0x32.
//
// Звук даёт скетч: задача звука (ядро 0) зовёт fill(buf, n) — n отсчётов моно; вернул меньше n — дальше тишина.
// Пока тишина дольше 150 мс — усилитель выключен (динамик не шипит), задача спит, а DMA сам шлёт нули. Буфер DMA —
// 6 × 256 кадров (~70 мс при 22 050 Гц): запись во флеш (настройки) на миг останавливает задачи из флеша, звук
// за это время доигрывает из буфера.
#include "halo_internal.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#define FRAMES 256
static i2s_chan_handle_t tx;
static i2c_master_dev_handle_t es;
static HaloAudioFill fill_cb;
static int rate;
static volatile int volume = 60;
static volatile bool vol_dirty;

static void es_w(uint8_t r, uint8_t v) {
  const uint8_t b[2] = {r, v};
  i2c_master_transmit(es, b, 2, 20);
}

static uint8_t es_r(uint8_t r) {
  uint8_t v = 0;
  i2c_master_transmit_receive(es, &r, 1, &v, 1, 20);
  return v;
}

static uint8_t vol_reg(int v) {  // 0..100 → регистр 0x32 (как es8311_voice_volume_set: 0 — тишина, 100 — +32 дБ)
  return v <= 0 ? 0 : (uint8_t)(v * 256 / 100 - 1);
}

static void audio_task(void *) {
  static int16_t mono[FRAMES], st[FRAMES * 2];
  bool pa = false;
  int silent = 1 << 30;  // отсчётов тишины подряд
  for (;;) {
    if (vol_dirty) vol_dirty = false, es_w(0x32, vol_reg(volume));
    const int n = fill_cb ? fill_cb(mono, FRAMES) : 0;
    if (n <= 0) {
      silent += FRAMES;
      if (pa && silent > rate * 15 / 100) gpio_set_level((gpio_num_t)HALO_PIN_PA, 0), pa = false;
      if (!pa) {  // тихо: DMA сам шлёт нули (auto_clear), задаче — спать
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
    } else {
      silent = 0;
      if (!pa) gpio_set_level((gpio_num_t)HALO_PIN_PA, 1), pa = true;
    }
    for (int i = 0; i < FRAMES; i++) st[2 * i] = st[2 * i + 1] = i < n ? mono[i] : 0;
    size_t w = 0;
    i2s_channel_write(tx, st, sizeof(st), &w, pdMS_TO_TICKS(200));
  }
}

bool halo_audio_begin(HaloAudioFill fill, int sample_rate) {
  if (tx) return true;
  es = ht_i2c_device(0x18);
  if (!es) {
    LOG("audio: ES8311 NOT FOUND\n");
    return false;
  }
  rate = sample_rate, fill_cb = fill;
  gpio_set_direction((gpio_num_t)HALO_PIN_PA, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)HALO_PIN_PA, 0);
  // I2S: сначала такты (кодеку нужен MCLK, чтобы принять настройки)
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6, cc.dma_frame_num = FRAMES, cc.auto_clear = true;
  if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) return LOG("audio: I2S FAILED\n"), tx = nullptr, false;
  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate);
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = (gpio_num_t)HALO_PIN_MCLK, sc.gpio_cfg.bclk = (gpio_num_t)HALO_PIN_BCLK;
  sc.gpio_cfg.ws = (gpio_num_t)HALO_PIN_LRCK, sc.gpio_cfg.dout = (gpio_num_t)HALO_PIN_DOUT;
  sc.gpio_cfg.din = I2S_GPIO_UNUSED;
  i2s_channel_init_std_mode(tx, &sc);
  i2s_channel_enable(tx);
  // ES8311: сброс и включение, MCLK с вывода, все такты; делители для MCLK = 256·fs (pre_div 1, ×1, adc/dac div 1,
  // одинарная скорость, LRCK = MCLK/256, BCLK = MCLK/4, OSR 0x10)
  es_w(0x00, 0x1F), es_w(0x00, 0x00), es_w(0x00, 0x80);
  es_w(0x01, 0x3F);
  es_w(0x06, es_r(0x06) & ~0x20);                // BCLK не инвертирован
  es_w(0x02, es_r(0x02) & 0x07);                 // pre_div 1, pre_multi ×1
  es_w(0x03, 0x10), es_w(0x04, 0x10), es_w(0x05, 0x00);
  es_w(0x06, (es_r(0x06) & 0xE0) | 0x03);        // BCLK = MCLK / 4
  es_w(0x07, es_r(0x07) & 0xC0), es_w(0x08, 0xFF);  // LRCK = MCLK / 256
  es_w(0x00, es_r(0x00) & 0xBF);                 // ведомый
  es_w(0x09, 0x0C), es_w(0x0A, 0x0C);            // 16 бит на входе и выходе
  es_w(0x0D, 0x01), es_w(0x0E, 0x02), es_w(0x12, 0x00), es_w(0x13, 0x10);  // питание: аналог, ЦАП, выход
  es_w(0x1C, 0x6A), es_w(0x37, 0x08);            // без эквалайзеров
  es_w(0x32, vol_reg(volume));
  xTaskCreatePinnedToCore(audio_task, "halo_audio", 3072, nullptr, 5, nullptr, 0);
  LOG("audio: ES8311 ok, %d Hz\n", rate);
  return true;
}

void halo_audio_volume(int v) {
  volume = constrain(v, 0, 100), vol_dirty = true;
}

int halo_audio_get_volume() {
  return volume;
}
