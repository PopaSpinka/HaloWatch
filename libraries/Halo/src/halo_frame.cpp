// Движок кадров Halo: по сигналу TE рисует и отправляет только пачки строк с изменениями.
//
//   задача кадров (ядро 1)                         помощник (ядро 0)
//     ждёт TE
//     изменения прошлого update → пачки с изменениями
//     для каждой: буфер (ждёт на семафоре) ───→     правая часть (если пачка широкая): чёрный фон + draw
//                 левая часть: чёрный фон + draw
//                 ждёт помощника ←───              готово
//                 в очередь DMA (не ждёт)
//     ждёт, пока всё уйдёт
//     update(dt) — следующий кадр; изменений нет — следующий кадр не рисуется вовсе
//
// В пачке перерисовывается и уходит не весь круг, а только столбцы с изменениями (объединение по пачке).
//
// Без разрыва (дисплей сам 60 раз в секунду проходит свою память сверху вниз, ~34 мкс на строку, проход начинается
// с концом паузы TE). Строки одного кадра должны попасть в один и тот же проход, есть два способа:
//   * впереди прохода — лёгкий кадр: пишем сразу после TE, каждая пачка успевает раньше, чем до неё дойдёт проход
//     (у верхней строки — 0,6 мс, у нижней — 16 мс). Задержка от расчёта до экрана — меньше;
//   * вслед за проходом — тяжёлый кадр: пачку отправляем, только когда текущий проход её миновал, а следующий
//     покажет кадр целиком. У каждой пачки — почти весь период 16,6 мс, но кадр на экране на проход позже.
// Какой выбрать — по оценке: сумма времён пачек (по прошлым кадрам) до каждой пачки против её срока «впереди».
#include "halo_internal.h"

#pragma GCC optimize("O3", "fast-math")

__thread HaloBand hb;

static HaloConfig cfg;
static HaloUpdate m_update;
static HaloDraw m_draw;

// ---------- Что изменилось: по пачке — отрезок столбцов ----------
static int16_t dmg_x0[HALO_BANDS], dmg_x1[HALO_BANDS];  // x1 < x0 — пачка чистая
static portMUX_TYPE dmg_mux = portMUX_INITIALIZER_UNLOCKED;
static int16_t chord_x0[HALO_BANDS], chord_x1[HALO_BANDS];  // круг экрана в пачке (чётный — нечётный)

static void chords() {
  const float c = (HALO_W - 1) / 2.0f, R = HALO_W / 2.0f;
  for (int b = 0; b < HALO_BANDS; b++) {
    const int y = b * HALO_ROWS, rows = min(HALO_ROWS, HALO_H - y);
    const float dy = (y <= c && y + rows >= c) ? 0 : fminf(fabsf(y - c), fabsf(y + rows - c));
    const float half = halo_sqrt(fmaxf(0, R * R - dy * dy)) + 2;
    chord_x0[b] = max(0, (int)(c - half)) & ~1;
    chord_x1[b] = min(HALO_W - 1, (int)(c + half)) | 1;
    dmg_x0[b] = HALO_W, dmg_x1[b] = -1;
  }
}

void halo_damage(int x0, int y0, int x1, int y1) {
  x0 = max(x0, 0), y0 = max(y0, 0), x1 = min(x1, HALO_W - 1), y1 = min(y1, HALO_H - 1);
  if (x0 > x1 || y0 > y1) return;
  portENTER_CRITICAL(&dmg_mux);
  for (int b = y0 / HALO_ROWS; b <= y1 / HALO_ROWS; b++) {
    if (x0 < dmg_x0[b]) dmg_x0[b] = x0;
    if (x1 > dmg_x1[b]) dmg_x1[b] = x1;
  }
  portEXIT_CRITICAL(&dmg_mux);
}

void halo_damage_f(float x0, float y0, float x1, float y1) {
  halo_damage(halo_floor(x0) - 1, halo_floor(y0) - 1, halo_floor(x1) + 2, halo_floor(y1) + 2);
}

void halo_damage_all() {
  halo_damage(0, 0, HALO_W - 1, HALO_H - 1);
}

// ---------- Время анимаций ----------
static uint64_t m_clock;
static volatile bool vt_paused, vt_fixed;
static volatile int vt_steps;
static volatile float slowmo = 1;

uint32_t halo_us() {
  return (uint32_t)m_clock;
}
uint32_t halo_ms() {
  return (uint32_t)(m_clock / 1000);
}

// ---------- Запись кадров (rec): как в AmoledMotion, но кадр — изменения поверх прошлого ----------
// Записи (чётной длины): 'F' 0 номер(2) TE(4) начало_мкс(4) часы_мкс(4) 0(4); 'S' 0 номер(2) y(2) x0(2)
// пикселей(2) слов(2) слова…; 'E' 0 номер(2) кадр_мкс(4) пропущено_TE(2) 0(2). Сжатие: слово с битом 0x8000 —
// столько чёрных пикселей, иначе — столько пикселей следом. На Mac кадр собирается поверх прошлого
static uint8_t *rec_pool[2];
static uint32_t rec_cap, rec_len[2];
static volatile int rec_left;
static volatile bool rec_half, rec_cur, rec_over, rec_first;
static uint16_t rec_no;

static void rec_put(int pool, char kind, const uint32_t *v, int n32, uint16_t extra) {
  uint8_t *h = rec_pool[pool] + rec_len[pool];
  h[0] = kind, h[1] = 0;
  memcpy(h + 2, &rec_no, 2);
  memcpy(h + 4, v, n32 * 4);
  memcpy(h + 4 + n32 * 4, &extra, 2);
  h[6 + n32 * 4] = h[7 + n32 * 4] = 0;
  rec_len[pool] += 8 + n32 * 4;
}

static void __attribute__((noinline, noclone)) rec_band(int pool) {  // во флеше: нужна только при записи
  const int step = rec_half ? 2 : 1;
  const int n = (hb.x1 - hb.x0) / step + 1;
  if (n <= 0) return;
  for (int r = 0; r < hb.rows; r++) {
    const int y = hb.y + r;
    if (rec_half && (y & 1)) continue;
    if (rec_len[pool] + 12 + n * 3 + 8 > rec_cap) {
      rec_over = true;
      return;
    }
    uint16_t *h = (uint16_t *)(rec_pool[pool] + rec_len[pool]);
    uint16_t *o = h + 6;
    const uint16_t *src = hb.buf + r * hb.w + (hb.x0 - hb.bx);
    for (int i = 0; i < n;) {
      int j = i;
      if (!src[i * step]) {
        while (j < n && !src[j * step] && j - i < 0x7FFF) j++;
        *o++ = 0x8000 | (j - i);
      } else {
        uint16_t *cnt = o++;
        while (j < n && src[j * step] && j - i < 0x7FFF) *o++ = src[j * step], j++;
        *cnt = j - i;
      }
      i = j;
    }
    const int words = o - (h + 6);
    h[0] = 'S', h[1] = rec_no, h[2] = y, h[3] = hb.x0, h[4] = n, h[5] = words;
    rec_len[pool] += 12 + words * 2;
  }
}

static void cmd_rec(const char *a) {
  if (!strncmp(a, "dump", 4)) {
    rec_left = 0;
    delay(40);
    if (!rec_pool[0] || !rec_pool[1]) rec_len[0] = rec_len[1] = 0;
    uint32_t sum = 0;
    for (int p = 0; p < 2; p++)
      for (uint32_t i = 0; i < rec_len[p]; i++) sum += rec_pool[p][i];
    halo_serial_binary(true);
    // 6-е поле 1: кадры — изменения поверх прошлого (record.py собирает их по очереди)
    Serial.printf("REC %lu %lu %d %u %d 1\n", (unsigned long)rec_len[0], (unsigned long)rec_len[1], rec_half ? 2 : 1,
                  (unsigned)rec_no, HALO_W);
    for (int p = 0; p < 2; p++)
      for (uint32_t off = 0; off < rec_len[p];) off += Serial.write(rec_pool[p] + off, min<uint32_t>(rec_len[p] - off, 4096));
    Serial.printf("\nREC END %lu\n", (unsigned long)sum);
    halo_serial_binary(false);
    return;
  }
  if (!strncmp(a, "free", 4)) {
    rec_left = 0;
    delay(40);
    for (auto &p : rec_pool) free(p), p = nullptr;
    rec_cap = rec_len[0] = rec_len[1] = rec_no = 0;
    return;
  }
  int n = 0;
  float mb = 0;
  char mode[8] = "half";
  if (sscanf(a, "%d %7s %f", &n, mode, &mb) < 1 || n <= 0) {
    Serial.println("rec N [half|full] [MB] | rec dump | rec free");
    return;
  }
  rec_left = 0;
  delay(40);
  const uint32_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) + rec_cap * 2;
  uint32_t total = mb > 0 ? (uint32_t)(mb * 1048576) : 4u << 20;
  total = min<uint32_t>(total, free_ps > (3u << 19) ? free_ps - (3u << 19) : 0) & ~15u;
  if (total / 2 != rec_cap) {
    for (auto &p : rec_pool) free(p), p = nullptr;
    rec_cap = total / 2;
    for (auto &p : rec_pool) p = (uint8_t *)heap_caps_malloc(rec_cap, MALLOC_CAP_SPIRAM);
    if (!rec_pool[0] || !rec_pool[1]) {
      for (auto &p : rec_pool) free(p), p = nullptr;
      rec_cap = 0;
      LOG("rec: no memory\n");
      return;
    }
  }
  rec_len[0] = rec_len[1] = 0;
  rec_no = 0;
  rec_over = false;
  rec_half = strcmp(mode, "full") != 0;
  rec_first = true;
  rec_left = n;
  halo_damage_all();  // первый кадр записи — целиком, дальше — изменения
  LOG("rec: %d frames, %s, %lu KB\n", n, rec_half ? "half" : "full", (unsigned long)(rec_cap * 2 / 1024));
}

// ---------- Помощник на ядре 0 ----------
static SemaphoreHandle_t half_go, half_done;
static volatile int job_y, job_rows, job_x0, job_x1, job_bx, job_w;
static uint16_t *volatile job_buf;
static volatile uint32_t job_us;

static void clear_part() {  // чёрный фон своей части пачки
  const uint32_t c0 = hp_on ? esp_cpu_get_cycle_count() : 0;
  for (int r = 0; r < hb.rows; r++) memset(hb.buf + r * hb.w + (hb.x0 - hb.bx), 0, (hb.x1 - hb.x0 + 1) * 2);
  if (hp_on) hp_cyc[xPortGetCoreID()][HP_CLEAR] += esp_cpu_get_cycle_count() - c0;
}

static void helper_task(void *) {
  for (;;) {
    xSemaphoreTake(half_go, portMAX_DELAY);
    const uint32_t t0 = trace_now();
    hb = {job_buf, job_y, job_rows, job_x0, job_x1, job_bx, job_w};
    clear_part();
    m_draw();
    if (rec_cur) rec_band(0);
    job_us = trace_now() - t0;
    trace_span(TR_BAND, t0, job_y / HALO_ROWS, t0 + job_us);
    xSemaphoreGive(half_done);
  }
}

// ---------- Скриншот и замер: весь кадр одним ядром в память ----------
static uint16_t *volatile shot_buf;
static volatile bool shot_req, shot_ready, bench_ring_req;

static void render_full(uint16_t *dst, bool swap) {  // dst — 466×466 в PSRAM
  // Пачка — во внутренней памяти, как у настоящего кадра: иначе замер bench врёт (запись в PSRAM медленная)
  static uint16_t *tmp = (uint16_t *)heap_caps_malloc(HALO_ROWS * HALO_W * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!tmp) return;
  const HaloBand saved = hb;
  for (int y = 0; y < HALO_H; y += HALO_ROWS) {
    hb = {tmp, y, min(HALO_ROWS, HALO_H - y), 0, HALO_W - 1, 0, HALO_W};
    clear_part();
    m_draw();
    if (!dst) continue;
    for (int i = 0; i < hb.rows * HALO_W; i++) dst[y * HALO_W + i] = swap ? __builtin_bswap16(tmp[i]) : tmp[i];
  }
  hb = saved;
}

// ---------- Задача кадров ----------
struct Stats {
  uint32_t ticks, drawn, bands, px, sum_us, max_us, missed, update_us, update_max, chased;
};
static Stats stats, report;
static uint32_t hp_report[2][HP_PARTS];
static volatile bool report_ready;
static volatile int bright_req = -1, power_req = -1;
static volatile bool disp_on = true, boot_cmd;
static int16_t split[HALO_BANDS];
// Когда граница пачки делилась в последний раз (номер кадра): старая граница — от другой картинки (экран стоял, потом
// сменился целиком: первый кадр перехода Settings — правое ядро 18 мс, левое 6), начинать с середины
static uint32_t split_seq[HALO_BANDS], frame_seq;
// Сколько стоила каждая пачка в прошлых кадрах (рисование + ожидание буфера), мкс: середина с кольцами и цифрами
// в разы дороже краёв — по среднему выбор ошибался
static float band_cost[HALO_BANDS];

// Ждать до момента t (мкс): долго — спим, остаток — крутимся
static void wait_until(uint32_t t) {
  int32_t left = (int32_t)(t - trace_now());
  if (left > 2500) vTaskDelay(pdMS_TO_TICKS(left / 1000 - 1));
  while ((int32_t)(t - trace_now()) > 0) {
  }
}

static void bench_ring();

static void frame_task(void *) {
  for (auto &c : band_cost) c = 300;
  chords();
  halo_damage_all();
  halo_subpixel = false;
  m_update(1.0f / 60);
  uint32_t last = trace_now(), te_prev = hd_te_count(), report_at = millis() + cfg.report_ms;
  bool frame_new = true;
  uint32_t frame_clock = 0;
  for (;;) {
    // Ждём сигнал TE; если он уже пришёл, пока рисовался прошлый кадр (тяжёлый, вслед за проходом), — не ждём
    // следующего, а начинаем сразу: этот кадр пойдёт вслед за проходом, который уже начался
    if (hd_te_count() == te_prev) hd_wait_te(50);
    const uint32_t te = hd_te_count();
    if (!stats.ticks && !report.ticks) hc_stage(6);  // первый кадр
    stats.ticks++;
    if (bright_req >= 0) hd_set_brightness(bright_req), bright_req = -1;
    if (power_req >= 0) hd_set_power(power_req), power_req = -1;

    int16_t fx0[HALO_BANDS], fx1[HALO_BANDS];
    portENTER_CRITICAL(&dmg_mux);
    memcpy(fx0, dmg_x0, sizeof(fx0));
    memcpy(fx1, dmg_x1, sizeof(fx1));
    for (int b = 0; b < HALO_BANDS; b++) dmg_x0[b] = HALO_W, dmg_x1[b] = -1;
    portEXIT_CRITICAL(&dmg_mux);

    const uint32_t t0 = trace_now();
    int bands = 0, px = 0;
    // Впереди прохода или вслед за ним: успеет ли каждая пачка раньше, чем до неё дойдёт проход
    const uint32_t te_t = hd_te_rise(), blank = hd_te_blank(), period = hd_te_period() ? hd_te_period() : 16650;
    const float row_us = (float)(period - blank) / HALO_H;
    const uint32_t scan = te_t + blank;  // начало прохода после этого TE
    bool chase = false;
    float sum = t0 - te_t + 100;
    for (int b = 0; b < HALO_BANDS && !chase; b++)
      if (fx0[b] <= fx1[b]) sum += band_cost[b] * 1.15f + 20, chase = sum + 250 > blank + b * HALO_ROWS * row_us;
    rec_cur = rec_left > 0 && (frame_new || rec_first) && !rec_over && rec_len[1] + 64 < rec_cap;
    frame_seq++;
    for (int b = 0; b < HALO_BANDS; b++) {
      if (fx0[b] > fx1[b]) continue;
      const int x0 = max(fx0[b] & ~1, (int)chord_x0[b]), x1 = min(fx1[b] | 1, (int)chord_x1[b]);
      if (x0 > x1) continue;
      if (!bands && rec_cur) {
        const uint32_t v[3] = {te, t0, frame_clock};
        rec_put(1, 'F', v, 3, 0);
        rec_first = false;
      }
      const int y = b * HALO_ROWS, rows = min(HALO_ROWS, HALO_H - y), w = x1 - x0 + 1;
      const uint32_t bw0 = trace_now();
      uint16_t *buf = hd_buffer();
      const uint32_t c0 = trace_now();
      if (cfg.two_cores && w >= 160) {  // широкая пачка — пополам; граница — туда, где работы меньше
        const bool fresh = frame_seq - split_seq[b] <= 3;  // делилась в одном из 3 прошлых кадров
        int sx = fresh && split[b] > x0 + 32 && split[b] < x1 - 32 ? split[b] : ((x0 + x1) / 2) & ~1;
        job_buf = buf, job_y = y, job_rows = rows, job_x0 = sx, job_x1 = x1, job_bx = x0, job_w = w;
        xSemaphoreGive(half_go);
        hb = {buf, y, rows, x0, sx - 1, x0, w};
        clear_part();
        m_draw();
        if (rec_cur) rec_band(1);
        const uint32_t c1 = trace_now();
        trace_span(TR_BAND, c0, b, c1);
        xSemaphoreTake(half_done, portMAX_DELAY);
        const uint32_t c2 = trace_now();
        if (c2 - c1 >= 5) trace_span(TR_WAIT_HALF, c1, b, c2);
        const int left = c1 - c0, right = job_us, tot = left + right;
        const int step = tot > 0 ? constrain((int)((left - right) * (float)w / (2.0f * tot)), -64, 64) : 0;
        split[b] = constrain(sx - step, x0 + 32, x1 - 32) & ~1, split_seq[b] = frame_seq;
      } else {
        hb = {buf, y, rows, x0, x1, x0, w};
        clear_part();
        m_draw();
        if (rec_cur) rec_band(1);
        trace_span(TR_BAND, c0, b);
      }
      band_cost[b] = band_cost[b] * 0.5f + 0.5f * (float)(trace_now() - bw0);
      if (chase) wait_until(scan + (uint32_t)((y + rows) * row_us) + 40);  // текущий проход миновал пачку
      hd_send(x0, y, w, rows);
      bands++, px += w * rows;
    }
    uint32_t frame_us = 0, missed = 0;
    if (bands) {
      const uint32_t f0 = trace_now();
      hd_wait_idle();
      trace_span(TR_FINISH, f0);
      frame_us = trace_now() - t0;
      missed = te - te_prev > 1 ? te - te_prev - 1 : 0;  // кадр начался не с первого сигнала после прошлого
      trace_span(TR_FRAME, t0, min(missed, (uint32_t)255) | (chase ? 256 : 0), t0 + frame_us);
      stats.drawn++, stats.bands += bands, stats.px += px, stats.sum_us += frame_us, stats.missed += missed;
      stats.chased += chase;
      stats.max_us = max(stats.max_us, frame_us);
    }
    if (rec_cur && bands) {
      if (!rec_over) {
        const uint32_t v[1] = {frame_us};
        rec_put(1, 'E', v, 1, missed);
        rec_no++;
      }
      if (rec_over || --rec_left <= 0) {
        if (rec_over) LOG("rec: memory is full after %u frames\n", (unsigned)rec_no);
        rec_left = 0;
      }
    }
    rec_cur = false;
    te_prev = te;

    // Следующий кадр: виртуальное время — в паузе только по шагам (ровно 1/60 с)
    const uint32_t p0 = trace_now();
    float dt = constrain((int32_t)(p0 - last) / 1e6f, 0.001f, 0.1f);
    last = p0;
    frame_new = !vt_paused || vt_steps > 0;
    if (vt_paused && frame_new) vt_steps--, trace_mark(TR_STEP, vt_steps);
    if (vt_paused || vt_fixed) dt = 1.0f / 60;
    if (frame_new) {
      m_clock += (uint32_t)(dt * 1e6f + 0.5f);
      frame_clock = (uint32_t)m_clock;
      halo_subpixel = false;  // каждый кадр заново: кому нужно — ставит в update
      m_update(vt_paused || vt_fixed ? dt : dt * slowmo);
      const uint32_t u = trace_now() - p0;
      trace_span(TR_PREPARE, p0, 0, p0 + u);
      stats.update_us += u, stats.update_max = max(stats.update_max, u);
    }
    if (shot_req) {
      render_full(shot_buf, true);
      shot_req = false, shot_ready = true;
    }
    if (bench_ring_req) bench_ring(), bench_ring_req = false;
    if (cfg.report_ms && (int32_t)(millis() - report_at) >= 0) {
      report_at += cfg.report_ms;
      if (!report_ready) {
        report = stats, report_ready = true;
        memcpy(hp_report, hp_cyc, sizeof(hp_report));
      }
      stats = {};
      memset(hp_cyc, 0, sizeof(hp_cyc));
    }
    halo_trace_pump();
  }
}

// ---------- Команды ----------
static void cmd_shot(const char *) {
  if (!shot_buf) shot_buf = (uint16_t *)heap_caps_malloc(HALO_W * HALO_H * 2, MALLOC_CAP_SPIRAM);
  if (!shot_buf) return;
  shot_ready = false, shot_req = true;
  const uint32_t t0 = millis();
  while (!shot_ready && millis() - t0 < 1000) delay(2);
  if (shot_ready) hc_send_image(shot_buf, HALO_W, HALO_H);
}

// bench ring — одно кольцо (r 100, толщина 20) во внутреннюю пачку в разных вариантах: тактов на пиксель кольца
static void bench_ring() {
  static uint16_t *tmp = (uint16_t *)heap_caps_malloc(HALO_ROWS * HALO_W * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const HaloBand saved = hb;
  const float area = 2 * HALO_PI * 100 * 20;
  struct V {
    const char *name;
    float frac;
    uint32_t track;
  } vs[] = {{"full", 1, 0}, {"arc 0.7", 0.7f, 0}, {"arc 0.7 + track", 0.7f, 0x303030}, {"arc 0.3 + track", 0.3f, 0x303030}};
  for (auto &v : vs) {
    uint32_t cyc = 0;
    for (int y = 0; y < HALO_H; y += HALO_ROWS) {
      hb = {tmp, y, min(HALO_ROWS, HALO_H - y), 0, HALO_W - 1, 0, HALO_W};
      const uint32_t c0 = esp_cpu_get_cycle_count();
      halo_ring(233, 233, 100, 20, v.frac, 0xFF3060, v.track);
      cyc += esp_cpu_get_cycle_count() - c0;
    }
    LOG("bench ring %s: %.2f ms, %.0f cycles/px\n", v.name, cyc / 240000.0f, cyc / area);
  }
  struct R {
    const char *name;
    float x0, y0, x1, y1, r, a;
  } rs[] = {{"panel 466x466 r120", 0, -100, 466, 366, 120, 1}, {"row 300x62 r31", 83, 200, 383, 262, 31, 1},
            {"row 300x62 r31 alpha 0.5", 83, 200, 383, 262, 31, 0.5f}};
  for (auto &v : rs) {
    uint32_t cyc = 0;
    for (int y = 0; y < HALO_H; y += HALO_ROWS) {
      hb = {tmp, y, min(HALO_ROWS, HALO_H - y), 0, HALO_W - 1, 0, HALO_W};
      const uint32_t c0 = esp_cpu_get_cycle_count();
      halo_rrect(v.x0, v.y0, v.x1, v.y1, v.r, 0x202024, v.a);
      cyc += esp_cpu_get_cycle_count() - c0;
    }
    const float area = (v.x1 - v.x0) * (fminf(v.y1, 466) - fmaxf(v.y0, 0));
    LOG("bench rrect %s: %.2f ms, %.1f cycles/px\n", v.name, cyc / 240000.0f, cyc / area);
  }
  hb = saved;
}

static volatile bool bench_req;
static void cmd_bench(const char *a) {  // весь кадр одним ядром, без вывода — сколько стоит рисование
  if (!strcmp(a, "ring")) {  // в задаче кадров, между кадрами: её никто не перебивает
    bench_ring_req = true;
    return;
  }
  static uint16_t *dummy;
  vt_paused = true;  // состояние не меняется, пока меряем
  delay(40);
  const uint32_t t0 = trace_now();
  for (int k = 0; k < 5; k++) render_full(dummy, false);
  LOG("bench: full frame on one core %.2f ms\n", (trace_now() - t0) / 5000.0f);
  vt_paused = false;
}

void halo_brightness(uint8_t level) {
  bright_req = level;
}

void halo_display_on(bool on) {
  disp_on = on, power_req = on;
}

bool halo_display_is_on() {
  return disp_on;
}

// Кнопка BOOT — GPIO0 (нажата — 0): нажатие засчитывается, когда она нажата два кадра подряд после двух отпущенных
// (дребезг контактов — короче кадра). Команда boot — то же нажатие из порта
bool halo_boot_click() {
  static uint8_t hist = 0xFF;
  static bool down;
  hist = hist << 1 | (gpio_get_level(GPIO_NUM_0) ? 1 : 0);
  if (boot_cmd) return boot_cmd = false, true;
  if (!down && (hist & 3) == 0) return down = true;
  if (down && (hist & 3) == 3) down = false;
  return false;
}

bool halo_begin(HaloUpdate update, HaloDraw draw, const HaloConfig &c) {
  cfg = c;
  m_update = update;
  m_draw = draw;
  hc_begin(cfg.name);
  if (hc_safe_mode()) return false;
  if (!hd_begin(cfg.bus_hz, cfg.buffers, cfg.brightness)) return hc_boot_done(), false;
  hc_stage(4);
  if (cfg.touch) ht_begin();
  pinMode(0, INPUT_PULLUP);  // кнопка BOOT
  hc_stage(5);
  hc_boot_done();
  hu_begin();
  half_go = xSemaphoreCreateBinary();
  half_done = xSemaphoreCreateBinary();
  halo_trace_name(TR_FRAME, "frame", 'X');
  halo_trace_name(TR_PREPARE, "prepare", 'X');
  halo_trace_name(TR_BAND, "band", 'X');
  halo_trace_name(TR_WAIT_HALF, "wait right half", 'X');
  halo_trace_name(TR_FINISH, "last band out", 'X');
  halo_trace_name(TR_REC, "rec", 'X');
  halo_trace_name(TR_STEP, "step", 'i');
  halo_command("shot", cmd_shot);
  halo_command("bench", cmd_bench);
  halo_command("rec", cmd_rec);
  halo_command("pause", [](const char *) { vt_steps = 0, vt_paused = true; });
  halo_command("resume", [](const char *) { vt_steps = 0, vt_paused = false; });
  halo_command("step", [](const char *a) { vt_steps += max(1, atoi(a)), vt_paused = true; });
  halo_command("clock", [](const char *a) { vt_fixed = !strcmp(a, "fixed"); });
  halo_command("slow", [](const char *a) { slowmo = 1 / constrain(atof(a), 1.0f, 100.0f); });
  halo_command("bright", [](const char *a) { halo_brightness(atoi(a)); });
  halo_command("boot", [](const char *) { boot_cmd = true; });
  halo_command("redraw", [](const char *) { halo_damage_all(); });
  halo_command("prof", [](const char *) { hp_on = !hp_on; });
  if (cfg.two_cores) xTaskCreatePinnedToCore(helper_task, "halo_helper", 4096, nullptr, 10, nullptr, 0);
  xTaskCreatePinnedToCore(frame_task, "halo_frames", 6144, nullptr, 5, nullptr, 1);
  LOG("halo: started, free internal %u KB, psram %u KB\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  return true;
}

void halo_loop() {
  hc_loop();
  if (report_ready) {
    const Stats &s = report;
    const float sec = cfg.report_ms / 1000.0f;
    if (s.drawn)
      LOG("%s: %.0f fps drawn of %.0f (%lu%% behind the scan), frame %.2f ms (max %.2f), %.1f bands %.0f%% of "
          "screen, update %.2f ms (max %.2f), missed %lu\n", cfg.name, s.drawn / sec, s.ticks / sec,
          (unsigned long)(100 * s.chased / s.drawn), s.sum_us / 1000.0f / s.drawn, s.max_us / 1000.0f,
          (float)s.bands / s.drawn, 100.0f * s.px / s.drawn / (HALO_W * HALO_H),
          s.update_us / 1000.0f / max((uint32_t)1, s.ticks), s.update_max / 1000.0f, (unsigned long)s.missed);
    else
      LOG("%s: idle (%.0f ticks/s), update %.2f ms\n", cfg.name, s.ticks / sec, s.update_us / 1000.0f / max((uint32_t)1, s.ticks));
    if (hp_on && s.drawn) {  // мс на нарисованный кадр: ядро 1 / ядро 0
      static const char *const N[HP_PARTS] = {"text", "mask", "image", "rrect", "circle", "ring", "arc", "edge", "clear", "-"};
      char line[280];
      int n = snprintf(line, sizeof(line), "%s prof ms/frame (core1/core0):", cfg.name);
      for (int i = 0; i < HP_PARTS - 1; i++)
        n += snprintf(line + n, sizeof(line) - n, " %s %.2f/%.2f", N[i], hp_report[1][i] / 240000.0f / s.drawn,
                      hp_report[0][i] / 240000.0f / s.drawn);
      LOG("%s\n", line);
    }
    report_ready = false;
  }
  delay(5);
}
