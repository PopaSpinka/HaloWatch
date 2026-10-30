// Внутреннее устройство Halo: то, чем файлы библиотеки пользуются друг у друга. Скетчу не нужно.
#pragma once

#include "Halo.h"

// Пины платы (схема Waveshare ESP32-S3-Touch-AMOLED-1.75C)
#define HALO_PIN_SD0 4
#define HALO_PIN_SD1 5
#define HALO_PIN_SD2 6
#define HALO_PIN_SD3 7
#define HALO_PIN_SCLK 38
#define HALO_PIN_CS 12
#define HALO_PIN_RST 1
#define HALO_PIN_TE 13
#define HALO_PIN_SDA 15
#define HALO_PIN_SCL 14
#define HALO_PIN_TP_INT 11
#define HALO_PIN_TP_RST 2
// Звук: I2S к кодеку ES8311 (динамик) и усилитель динамика
#define HALO_PIN_MCLK 16
#define HALO_PIN_BCLK 9
#define HALO_PIN_LRCK 45
#define HALO_PIN_DOUT 8
#define HALO_PIN_PA 46

// ---------- Дисплей (halo_display.cpp) ----------
bool hd_begin(uint32_t hz, int buffers, uint8_t brightness);
uint16_t *hd_buffer();                       // свободный DMA-буфер (ждёт, если все в очереди)
void hd_send(int x0, int y, int w, int rows);  // буфер, выданный hd_buffer, — в очередь (не ждёт)
void hd_wait_idle();                         // всё ушло на дисплей
bool hd_wait_te(uint32_t timeout_ms);        // ждать сигнала TE (начала паузы дисплея)
uint32_t hd_te_count();
uint32_t hd_te_period();
uint32_t hd_te_rise();
uint32_t hd_te_blank();                    // пауза TE: с её концом начинается проход по строкам
void hd_set_brightness(uint8_t level);       // между кадрами (зовёт движок)
void hd_set_power(bool on);                  // панель видна / погашена — тоже между кадрами
int hd_buffers();
void hd_state(int *fly, int *queued, int *free_bufs);  // очередь вывода — для sys (кто чего ждёт)
void hd_regs(char *out, int n);  // регистры канала DMA и SPI2 — для sys (разбор зависаний)

// Шина I2C тача (halo_touch.cpp): ещё одно устройство на ней (400 кГц), nullptr — не ответило или шины нет
struct i2c_master_dev_t;
i2c_master_dev_t *ht_i2c_device(uint8_t addr);

// ---------- Тач (halo_touch.cpp) ----------
bool ht_begin();

// ---------- Интерфейс (halo_ui.cpp) ----------
void hu_begin();  // пружины по умолчанию, команды искусственного пальца

// Профайлер по видам элементов (halo_ui.cpp пишет, halo_frame.cpp печатает по команде prof)
#define HP_PARTS 10  // 0..7 — виды HaloItem, 8 — чёрный фон пачки, 9 — остальное в draw
#define HP_CLEAR 8
extern volatile bool hp_on;
extern uint32_t hp_cyc[2][HP_PARTS];

// ---------- Ядро (halo_core.cpp) ----------
void hc_begin(const char *name);
void hc_trace_setup();
void hc_loop();
void hc_send_image(const uint16_t *px, int w, int h);  // для shot: SHOT w h / пиксели / SHOT END сумма
void hc_stage(int s);   // шаг запуска (0 core, 1 display bus, 2 panel, 3 dma, 4 touch, 5 tasks, 6 first frame, 7 running)
bool hc_safe_mode();    // упали два раза подряд — запуск без экрана
void hc_boot_done();    // запуск кончился: снять сторожевой таймер с setup()
