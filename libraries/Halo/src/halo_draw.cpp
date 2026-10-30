// Рисование Halo: фигуры со сглаженными краями, кольца, текст и значки — прямо в DMA-буфер пачки.
//
// Правила скорости (замеры — docs/smooth-motion.md, раздел 17; docs/wear-motion.md, раздел 13):
//   * середина фигуры — заливкой (по два пикселя за запись), по пикселю считаем только край;
//   * корень — один на строку для границ, на пиксель — только у края (~20 тактов, halo_sqrt);
//   * пиксель закрыт целиком — пишем цвет сразу, смешиваем только полупрозрачные;
//   * всё обрезается по своей части пачки (hb.x0..hb.x1, строки hb.y…): другую часть рисует другое ядро.
#include "halo_internal.h"

#pragma GCC optimize("O3", "fast-math")

static inline int ceil_i(float x) {
  return -halo_floor(-x);
}

// ---------- Цвет на экране: 565 с узором ----------
// У экрана 16 бит на пиксель: у R и B по 32 уровня, у G — 64. Тёмный цвет при плавном затемнении меняет не только
// яркость, но и оттенок: каналы переходят на ступеньку ниже в разные моменты (карточка 0x2E3034 от 1 до 0,5 проходит
// 12 цветов — серый, синеватый, бирюзовый, зеленоватый…; так было и в WearKit). Поэтому заливки — узором 4×4 (Байер)
// из двух соседних уровней: средний цвет точный, оттенок не прыгает. Края фигур и текста — ближайшим уровнем
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
// 0..255 → уровень 565 × 16 (дробь — 4 бита). Уровень — как при урезании (v >> 3, v >> 2): цвета, которые ровно
// попадают в уровни экрана (кратные 8 и 4 — так подобраны цвета интерфейса), рисуются ровно, без узора
static inline int lv5(int v) {
  return min(v << 1, 31 * 16);
}
static inline int lv6(int v) {
  return min(v << 2, 63 * 16);
}

struct Paint {  // цвет для одного вызова: в формате дисплея и по каналам 565, прозрачность 0..256
  uint16_t c;
  int r5, g6, b5, A;    // ближайший уровень по каналам; прозрачность
  const uint16_t *lut;  // маски на сплошном фоне: 16 уровней маски → готовые цвета (nullptr — смешивать)
  int r16, g16, b16;    // точный цвет в уровнях 565 × 16 — для узора
  bool dith;            // цвета нет среди уровней 565: заливка — узором
  int a5;               // полупрозрачная заливка: прозрачность 0..32
  uint32_t fa;          // и цвет × прозрачность в полях 0x07E0F81F, точно (в 1/32 уровня)
};
static inline Paint paint(uint32_t rgb, float alpha) {
  Paint P;
  P.r16 = lv5(rgb >> 16 & 255), P.g16 = lv6(rgb >> 8 & 255), P.b16 = lv5(rgb & 255);
  P.r5 = (P.r16 + 8) >> 4, P.g6 = (P.g16 + 8) >> 4, P.b5 = (P.b16 + 8) >> 4;
  P.c = __builtin_bswap16((P.r5 << 11) | (P.g6 << 5) | P.b5);
  P.A = (int)(halo_clamp01(alpha) * 256 + 0.5f), P.lut = nullptr;
  P.dith = ((P.r16 | P.g16 | P.b16) & 15) != 0;
  P.a5 = (P.A + 4) >> 3;
  P.fa = ((P.b16 * P.a5 + 8) >> 4) | ((P.r16 * P.a5 + 8) >> 4) << 11 | ((P.g16 * P.a5 + 8) >> 4) << 21;
  return P;
}
static inline void put(uint16_t *p, const Paint &P, int a) {  // a — 0..256 (уже с прозрачностью P)
  if (a >= 256) *p = P.c;
  else if (a > 0) halo_blend(p, P.r5, P.g6, P.b5, a);
}

// Сплошная заливка узором: в строке y цвет повторяется через 4 пикселя — два слова по два пикселя
static inline void dith_span(uint16_t *row, int y, int x0, int x1, const Paint &P) {
  if (x0 > x1) return;
  const uint8_t *t = BAYER[y & 3];
  uint16_t col[4];
  for (int i = 0; i < 4; i++)
    col[i] = __builtin_bswap16(min(31, (P.r16 + t[i]) >> 4) << 11 | min(63, (P.g16 + t[i]) >> 4) << 5 |
                               min(31, (P.b16 + t[i]) >> 4));
  int x = x0;
  if ((uintptr_t)(row + x) & 2) row[x] = col[x & 3], x++;
  const uint32_t w0 = col[x & 3] | (uint32_t)col[(x + 1) & 3] << 16;
  const uint32_t w1 = col[(x + 2) & 3] | (uint32_t)col[(x + 3) & 3] << 16;
  uint32_t *q = (uint32_t *)(row + x);
  int n = (x1 + 1 - x) >> 1;
  for (; n >= 4; n -= 4, q += 4) q[0] = w0, q[1] = w1, q[2] = w0, q[3] = w1;
  if (n >= 2) q[0] = w0, q[1] = w1, q += 2, n -= 2;
  if (n) *q = w0;
  if ((x1 + 1 - x) & 1) row[x1] = col[x1 & 3];
}

// Полупрозрачная заливка: пиксель 565 растянут в 32 бита (G — сверху, R и B — снизу, между ними зазоры), и все три
// канала смешиваются одним умножением — ~10 тактов на пиксель вместо ~40 (распаковать, три умножения, упаковать).
// Прозрачность — 32 ступени (у R и B всё равно по 5 бит); вместо округления — порог узора 4×4: без скачков оттенка
static inline void fill(uint16_t *row, int y, int x0, int x1, const Paint &P) {
  if (P.A >= 256) {
    if (P.dith) dith_span(row, y, x0, x1, P);
    else halo_span(row, x0, x1, P.c);
    return;
  }
  const uint32_t na = 32 - P.a5;
  const uint8_t *t = BAYER[y & 3];
  uint32_t fb[4];
  for (int i = 0; i < 4; i++) fb[i] = P.fa + (t[i] * 2 + 1) * 0x00200801u;  // порог 1..31 из 32 в каждом канале
  if (!P.fa) {  // чёрный поверх (затемнение под экраном в переходе): чёрное остаётся чёрным — его не трогаем
    for (int x = x0; x <= x1; x++) {
      if (!row[x]) continue;
      uint32_t d = __builtin_bswap16(row[x]);
      d = (d | d << 16) & 0x07E0F81F;
      d = ((d * na + fb[x & 3]) >> 5) & 0x07E0F81F;
      row[x] = __builtin_bswap16((uint16_t)(d | d >> 16));
    }
    return;
  }
  for (int x = x0; x <= x1; x++) {
    uint32_t d = __builtin_bswap16(row[x]);
    d = (d | d << 16) & 0x07E0F81F;
    d = ((d * na + fb[x & 3]) >> 5) & 0x07E0F81F;
    row[x] = __builtin_bswap16((uint16_t)(d | d >> 16));
  }
}

void HALO_HOT halo_rect(int x0, int y0, int x1, int y1, uint32_t rgb) {
  x0 = max(x0, hb.x0), x1 = min(x1, hb.x1), y0 = max(y0, hb.y), y1 = min(y1, hb.y + hb.rows - 1);
  const uint16_t c = halo_565(rgb);
  for (int y = y0; y <= y1; y++) halo_span(halo_row(y), x0, x1, c);
}

// ---------- Круг ----------
void HALO_HOT halo_circle(float cx, float cy, float r, uint32_t rgb, float alpha) {
  if (r <= 0 || !halo_hits(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1)) return;
  const Paint P = paint(rgb, alpha);
  const int ya = max(hb.y, halo_floor(cy - r - 1)), yb = min(hb.y + hb.rows - 1, ceil_i(cy + r + 1));
  const float ro2 = (r + 0.5f) * (r + 0.5f), ri = r - 0.5f, r2 = r * r, inv2r = 0.5f / r;
  for (int y = ya; y <= yb; y++) {
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    uint16_t *row = halo_row(y);
    const float ho = halo_sqrt(ro2 - dy2);
    const int xa = max(hb.x0, ceil_i(cx - ho - 0.5f)), xb = min(hb.x1, halo_floor(cx + ho - 0.5f));
    int xi0 = xb + 1, xi1 = xb;  // внутри — целиком
    if (fabsf(dy) < ri) {
      const float hi = halo_sqrt(ri * ri - dy2);
      xi0 = max(xa, ceil_i(cx - hi - 0.5f)), xi1 = min(xb, halo_floor(cx + hi - 0.5f));
    }
    for (int x = xa; x <= xb; x++) {
      if (x == xi0 && xi0 <= xi1) {
        fill(row, y, xi0, xi1, P);
        x = xi1;
        continue;
      }
      const float dx = x + 0.5f - cx, cov = (r2 - dx * dx - dy2) * inv2r + 0.5f;  // край без корня
      put(row + x, P, (int)(halo_clamp01(cov) * P.A));
    }
  }
}

// ---------- Прямоугольник со скруглёнными углами (края x0..x1, y0..y1 — дробные) ----------
static inline float rrect_cov(float px, float py, float cx, float cy, float hw, float hh, float r, float inv2r) {
  const float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
  float d;
  if (qx > 0 && qy > 0) {  // в углу: рядом с окружностью d − r ≈ (d² − r²)/2r — без корня (у малых углов — корень)
    const float d2 = qx * qx + qy * qy;
    d = r >= 8 ? (d2 - r * r) * inv2r : halo_sqrt(d2) - r;
  } else {
    d = fmaxf(qx, qy) - r;
  }
  return halo_clamp01(0.5f - d);
}

void HALO_HOT halo_rrect(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float alpha) {
  if (x1 <= x0 || y1 <= y0 || !halo_hits(x0 - 1, y0 - 1, x1 + 1, y1 + 1)) return;
  const Paint P = paint(rgb, alpha);
  const float hw = (x1 - x0) / 2, hh = (y1 - y0) / 2, cx = x0 + hw, cy = y0 + hh;
  r = fminf(r, fminf(hw, hh));
  const float inv2r = 0.5f / fmaxf(r, 0.5f);
  const int ya = max(hb.y, halo_floor(y0)), yb = min(hb.y + hb.rows - 1, ceil_i(y1) - 1);
  for (int y = ya; y <= yb; y++) {
    const float py = y + 0.5f;
    uint16_t *row = halo_row(y);
    // Граница строки: в зоне углов — по окружности угла (по верхнему и нижнему краю строки: у пологой части угла
    // граница проходит строку наискось через много пикселей — все они по пикселю), иначе — прямые края
    float lx = x0, rx = x1, lx_in = x0, rx_in = x1;
    const float c_top = y0 + r, c_bot = y1 - r;
    const float d_a = py - 0.5f < c_top ? c_top - (py - 0.5f) : py + 0.5f > c_bot ? (py + 0.5f) - c_bot : 0;
    const float d_b = py + 0.5f < c_top ? c_top - (py + 0.5f) : py - 0.5f > c_bot ? (py - 0.5f) - c_bot : 0;
    const float dyc = fmaxf(d_a, d_b), dyc_in = fminf(d_a, d_b);  // дальняя и ближняя к середине части строки
    if (dyc > 0) {
      const float h = halo_sqrt(fmaxf(0, r * r - dyc * dyc)), h_in = halo_sqrt(fmaxf(0, r * r - dyc_in * dyc_in));
      lx = x0 + r - h, rx = x1 - r + h, lx_in = x0 + r - h_in, rx_in = x1 - r + h_in;
    }
    const bool full_row = py - 0.5f >= y0 && py + 0.5f <= y1;
    const int xa = max(hb.x0, halo_floor(fminf(lx, lx_in)) - 1), xb = min(hb.x1, ceil_i(fmaxf(rx, rx_in)));
    int xi0 = ceil_i(fmaxf(lx, lx_in)) + 1, xi1 = halo_floor(fminf(rx, rx_in)) - 2;
    if (!full_row || dyc >= r) xi0 = xb + 1;  // самый верх и низ углов — по пикселю
    // Строка без углов: края прямые — целиком закрыты ровно пиксели между x0 и x1, по пикселю — только крайние (0–2).
    // С запасом в 1–2 пикселя с каждой стороны узкая плашка (полоса графика батареи, 4 px) считалась по пикселю вся:
    // 48 полос — 9,5 мс на ядро при полной перерисовке
    else if (dyc <= 0) xi0 = ceil_i(x0), xi1 = halo_floor(x1) - 1;
    xi0 = max(xi0, xa), xi1 = min(xi1, xb);
    for (int x = xa; x <= xb; x++) {
      if (x == xi0 && xi0 <= xi1) {
        fill(row, y, xi0, xi1, P);
        x = xi1;
        continue;
      }
      put(row + x, P, (int)(rrect_cov(x + 0.5f, py, cx, cy, hw, hh, r, inv2r) * P.A));
    }
  }
}

// ---------- Кнопка у края (EdgeButton) ----------
// Пересечение плашки (скруглённые верхние углы rc, прямые бока ±hw, снизу — далеко за экраном) и эллипса с центром
// (cx, cy): по вертикали — R (низ идёт по кругу экрана), по горизонтали — R − pad (бока — по изгибу экрана с отступом
// pad). Раньше вместо эллипса были два круга R, сдвинутые к середине на pad: бока те же, но внизу они сходились
// остриём — «уголок». Там, где верх плашки встречает эллипс, — тоже был угол: стык скруглён радиусом f (пересечение со
// скруглением, fOpIntersectionRound из hg_sdf). Расстояние до края — у эллипса по приближению k0·(k0 − 1)/k1 (iq);
// закрытое целиком — той же заливкой узором, что у всех фигур (одиночный пиксель ближайшим уровнем 565 у тёмно-серого
// заметно другого оттенка). Не в IRAM: рисуется редко (в конце ленты)
void halo_edge(float cx, float cy, float R, float top, float hw, float rc, float pad, float f, uint32_t rgb, float alpha) {
  const float bot = cy + R, ea = R - pad, eb = R;  // полуоси эллипса: по горизонтали и по вертикали
  if (bot - top < 0.5f || hw <= 0 || ea <= 1 || !halo_hits(cx - hw - 1, top - 1, cx + hw + 1, bot + 1)) return;
  const Paint P = paint(rgb, alpha);
  rc = fminf(rc, fminf(hw, bot - top));
  f = fminf(f, fminf(rc, ea * 0.5f));
  const float ia2 = 1 / (ea * ea), ib2 = 1 / (eb * eb), ct = top + rc;
  const int ya = max(hb.y, halo_floor(top)), yb = min(hb.y + hb.rows - 1, ceil_i(bot));
  for (int y = ya; y <= yb; y++) {
    const float py = y + 0.5f, dy = py - cy, qy = ct - py, dy2a = dy * dy * ib2, dy2b = dy2a * ib2;
    // Расстояние до края (< 0 — внутри) в точке строки на t от середины: плашка ∩ эллипс со скруглением стыка
    auto D = [&](float t) {
      const float qx = t - (hw - rc);
      const float da = qx > 0 && qy > 0 ? halo_sqrt(qx * qx + qy * qy) - rc : fmaxf(qx, qy) - rc;
      const float t2 = t * t * ia2, k0 = halo_sqrt(t2 + dy2a), k1 = halo_sqrt(t2 * ia2 + dy2b);
      const float de = k1 > 1e-9f ? k0 * (k0 - 1) / k1 : -eb;
      const float ua = fmaxf(f + da, 0), ue = fmaxf(f + de, 0);
      return fminf(-f, fmaxf(da, de)) + halo_sqrt(ua * ua + ue * ue);
    };
    // Фигура выпуклая и симметричная: по строке расстояние растёт от середины к краям. Где пиксели закрыты целиком
    // (D ≤ −0,5), — поиском половинным делением (8 шагов), это — заливкой; по пикселю — только край, пока не пусто.
    // Раньше по пикселю считалось всё, что ближе f к краям, — у низкой кнопки почти вся она: кадры по 11 мс на ядро
    float t_full = -1;
    if (D(0) <= -0.5f) {
      float lo = 0, hi = hw + 1;
      for (int i = 0; i < 8; i++) {
        const float m = (lo + hi) * 0.5f;
        if (D(m) <= -0.5f) lo = m;
        else hi = m;
      }
      t_full = lo;
    }
    uint16_t *row = halo_row(y);
    int f0, f1;  // закрытые целиком пиксели
    if (t_full >= 0) f0 = ceil_i(cx - t_full - 0.5f), f1 = halo_floor(cx + t_full - 0.5f);
    else f1 = halo_floor(cx) - 1, f0 = f1 + 1;
    if (max(f0, hb.x0) <= min(f1, hb.x1)) fill(row, y, max(f0, hb.x0), min(f1, hb.x1), P);
    for (int x = max(f1 + 1, hb.x0); x <= hb.x1; x++) {  // правый край
      const float cov = halo_clamp01(0.5f - D(x + 0.5f - cx));
      if (cov <= 0) break;
      if (cov >= 0.999f) fill(row, y, x, x, P);
      else put(row + x, P, (int)(cov * P.A));
    }
    for (int x = min(f0 - 1, hb.x1); x >= hb.x0; x--) {  // левый край
      const float cov = halo_clamp01(0.5f - D(cx - x - 0.5f));
      if (cov <= 0) break;
      if (cov >= 0.999f) fill(row, y, x, x, P);
      else put(row + x, P, (int)(cov * P.A));
    }
  }
}

// ---------- Кольцо с дугой ----------
// По строке: границы кольца — два корня на строку, лучи начала (12 часов) и конца дуги — по одному умножению.
// Середина строки — отрезками нужного цвета (дуга или дорожка), по пикселю — только сглаженные края кольца.
// Концы дуги — круги толщиной с кольцо (halo_circle) поверх. Было 110 тактов на пиксель, по пикселю на всё.
static inline void ring_run(uint16_t *row, int y, int x0, int x1, bool arc, const Paint &P, const Paint &T,
                            bool track) {
  if (x0 > x1) return;
  if (arc) fill(row, y, x0, x1, P);
  else if (track) fill(row, y, x0, x1, T);
}

void HALO_HOT halo_ring(float cx, float cy, float r, float w, float frac, uint32_t rgb, uint32_t track, float alpha) {
  const float ro = r + w / 2, ri = r - w / 2, hw = w / 2;
  if (!halo_hits(cx - ro - 1, cy - ro - 1, cx + ro + 1, cy + ro + 1)) return;
  frac = halo_clamp01(frac);
  const Paint P = paint(rgb, alpha), T = paint(track, alpha);
  const bool has_track = track != 0, none = frac <= 0, all = frac >= 1;
  if (none && !has_track) return;
  // Синус и косинус конца дуги — с запоминанием: кольцо рисуется в каждой пачке на каждом ядре (десятки раз за
  // кадр), а sinf/cosf — сотни тактов из флеша
  static float c_frac[2][4], c_ex[2][4], c_ey[2][4];
  static uint8_t c_next[2];
  const int core = xPortGetCoreID();
  float ex = 0, ey = -1;
  int hit = -1;
  for (int i = 0; i < 4; i++)
    if (c_frac[core][i] == frac + 1) hit = i;  // +1: в пустой таблице нули не совпадут с frac = 0
  if (hit >= 0) ex = c_ex[core][hit], ey = c_ey[core][hit];
  else {
    const float th = frac * 2 * HALO_PI;
    ex = sinf(th), ey = -cosf(th);
    const int i = c_next[core]++ & 3;
    c_frac[core][i] = frac + 1, c_ex[core][i] = ex, c_ey[core][i] = ey;
  }
  const float k = fabsf(ey) > 1e-5f ? ex / ey : 0;
  const float ro5 = (ro + 0.5f) * (ro + 0.5f), ri5 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) : 0;
  const float ro_in = ro > 0.5f ? (ro - 0.5f) * (ro - 0.5f) : 0, ri_in = (ri + 0.5f) * (ri + 0.5f);
  // Край без корня: рядом с окружностью радиуса R расстояние до неё ≈ (d² − R²) / 2R — точно в полосе сглаживания
  const float ro2 = ro * ro, ri2 = ri * ri, inv_o = 0.5f / ro, inv_i = ri > 0.5f ? 0.5f / ri : 0;
  const int ya = max(hb.y, halo_floor(cy - ro - 1)), yb = min(hb.y + hb.rows - 1, ceil_i(cy + ro + 1));
  for (int y = ya; y <= yb; y++) {
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro5) continue;
    uint16_t *row = halo_row(y);
    // Дуга на этой строке: отрезок [ia, ib] по dx (центр пикселя − cx); inv — дуга везде, кроме отрезка
    float ia = 1e4f, ib = -1e4f;
    bool inv = false;
    if (all) {
      inv = true;  // кроме пустого отрезка — значит, везде
    } else if (!none) {
      // Луч конца: dx·ey ≥ dy·ex — по одну сторону; решаем относительно dx
      float clo = -1e4f, chi = 1e4f;
      if (fabsf(ey) > 1e-5f) (ey > 0 ? clo : chi) = dy * k;
      else if (-dy * ex < 0) clo = 1e4f, chi = -1e4f;  // строка целиком не с той стороны
      if (frac <= 0.5f) ia = fmaxf(0, clo), ib = chi;  // правая половина ∩ сторона луча
      else {  // дуга — правая половина ∪ сторона луча; дорожка — левая половина без стороны луча
        inv = true;
        if (clo <= -1e4f) ia = chi, ib = 0;
        else ia = -1e4f, ib = fminf(clo, 0);
      }
    }
    const int xA0 = ceil_i(cx + ia - 0.5f), xA1 = halo_floor(cx + ib - 0.5f);  // пиксели внутри отрезка
    const float ho = halo_sqrt(ro5 - dy2), hin = dy2 < ri5 ? halo_sqrt(ri5 - dy2) : -1;
    const float hf_out = dy2 < ro_in ? halo_sqrt(ro_in - dy2) : -1, hf_in = dy2 < ri_in ? halo_sqrt(ri_in - dy2) : 0;
    // Две части строки кольца (между ними — дырка) и в каждой — где кольцо закрывает пиксель целиком
    const int seg[2][4] = {
        {ceil_i(cx - ho - 0.5f), hin > 0 ? halo_floor(cx - hin - 0.5f) : halo_floor(cx + ho - 0.5f),
         hf_out >= 0 ? ceil_i(cx - hf_out - 0.5f) : 1, hf_out >= 0 ? halo_floor(cx - hf_in - 0.5f) : 0},
        {hin > 0 ? ceil_i(cx + hin - 0.5f) : 1, hin > 0 ? halo_floor(cx + ho - 0.5f) : 0,
         hf_out >= 0 ? ceil_i(cx + hf_in - 0.5f) : 1, hf_out >= 0 ? halo_floor(cx + hf_out - 0.5f) : 0}};
    for (auto &sg : seg) {
      const int xa = max(hb.x0, sg[0]), xb = min(hb.x1, sg[1]);
      // Целиком закрытая часть: без дырки посередине строки (hin < 0) — от левой до правой
      int fa = hin > 0 ? sg[2] : (hf_out >= 0 ? ceil_i(cx - hf_out - 0.5f) : 1);
      int fb = hin > 0 ? sg[3] : (hf_out >= 0 ? halo_floor(cx + hf_out - 0.5f) : 0);
      fa = max(fa, xa), fb = min(fb, xb);
      for (int x = xa; x <= xb; x++) {
        const bool in_iv = x >= xA0 && x <= xA1, arc = inv ? !in_iv : in_iv;
        if (x >= fa && x <= fb) {  // середина: отрезок до границы дуги или до края
          const int end = min(fb, x < xA0 ? xA0 - 1 : x <= xA1 ? xA1 : fb);
          ring_run(row, y, x, end, arc, P, T, has_track);
          x = end;
          continue;
        }
        const float dx = x + 0.5f - cx, d2 = dx * dx + dy2;
        const float c_in = inv_i ? (d2 - ri2) * inv_i + 0.5f : 1, c_out = (ro2 - d2) * inv_o + 0.5f;
        const float cov = halo_clamp01(fminf(c_in, c_out));
        if (cov <= 0) continue;
        if (arc) put(row + x, P, (int)(cov * P.A));
        else if (has_track) put(row + x, T, (int)(cov * T.A));
      }
    }
  }
  if (!none && !all) {  // круглые концы дуги
    halo_circle(cx, cy - r, hw, rgb, alpha);
    halo_circle(cx + r * ex, cy + r * ey, hw, rgb, alpha);
  }
}

// ---------- Дуга между двумя углами ----------
// Синус и косинус концов — с запоминанием (дугу рисуют в каждой пачке на каждом ядре с теми же углами)
static void sincos_cached(float a, float *s, float *c) {
  static float key[2][8], vs[2][8], vc[2][8];
  static uint8_t next[2];
  const int core = xPortGetCoreID();
  for (int i = 0; i < 8; i++)
    if (key[core][i] == a && (vs[core][i] != 0 || vc[core][i] != 0)) {
      *s = vs[core][i], *c = vc[core][i];
      return;
    }
  *s = sinf(a), *c = cosf(a);
  const int i = next[core]++ & 7;
  key[core][i] = a, vs[core][i] = *s, vc[core][i] = *c;
}

// Покрытие пикселя — по расстоянию до средней линии дуги: внутри сектора — до окружности (без корня: рядом с ней
// d − r ≈ (d² − r²)/2r), вне сектора — до ближнего конца (круглые концы). Перебираем только рамку дуги и только
// кольцо по радиусу: дуги у нас короткие (индикатор, «червяк»), середина заливкой не нужна
void HALO_HOT halo_arc(float cx, float cy, float r, float hw, float a0, float a1, uint32_t rgb, float alpha) {
  if (a1 <= a0 || hw <= 0 || r <= 0 || alpha <= 0) return;
  const float ro = r + hw + 1;
  if (!halo_hits(cx - ro, cy - ro, cx + ro, cy + ro)) return;
  float s0, c0, s1, c1;
  sincos_cached(a0, &s0, &c0);
  sincos_cached(a1, &s1, &c1);
  const float d0x = s0, d0y = -c0, d1x = s1, d1y = -c1;  // направления на начало и конец
  float bx0 = fminf(d0x, d1x), bx1 = fmaxf(d0x, d1x), by0 = fminf(d0y, d1y), by1 = fmaxf(d0y, d1y);
  for (int k = 0; k < 4; k++) {  // 12, 3, 6, 9 часов внутри дуги
    float ak = k * (HALO_PI / 2);
    while (ak < a0) ak += 2 * HALO_PI;
    if (ak > a1) continue;
    if (k == 0) by0 = -1;
    if (k == 1) bx1 = 1;
    if (k == 2) by1 = 1;
    if (k == 3) bx0 = -1;
  }
  const float e = hw + 1;
  const int ya = max(hb.y, halo_floor(cy + by0 * r - e)), yb = min(hb.y + hb.rows - 1, ceil_i(cy + by1 * r + e));
  const int xa = max(hb.x0, halo_floor(cx + bx0 * r - e)), xb = min(hb.x1, ceil_i(cx + bx1 * r + e));
  if (xa > xb || ya > yb) return;
  const Paint P = paint(rgb, alpha);
  const bool big = a1 - a0 > HALO_PI;
  const float e0x = cx + r * d0x, e0y = cy + r * d0y, e1x = cx + r * d1x, e1y = cy + r * d1y;
  const float lo = fmaxf(0, r - hw - 0.5f), lo2 = lo * lo, hi2 = (r + hw + 0.5f) * (r + hw + 0.5f);
  const float r2 = r * r, inv2r = 0.5f / r, cap2 = (hw + 0.5f) * (hw + 0.5f);
  for (int y = ya; y <= yb; y++) {
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 > hi2) continue;
    uint16_t *row = halo_row(y);
    // Кольцо на этой строке — два отрезка (или один над и под дыркой): перебираем только их, а не всю рамку дуги
    const float ho = halo_sqrt(hi2 - dy2), hi_ = dy2 < lo2 ? halo_sqrt(lo2 - dy2) : -1;
    const int seg[2][2] = {{max(xa, halo_floor(cx - ho - 0.5f)), min(xb, hi_ < 0 ? ceil_i(cx + ho) : ceil_i(cx - hi_))},
                           {hi_ < 0 ? 1 : max(xa, halo_floor(cx + hi_ - 0.5f)), hi_ < 0 ? 0 : min(xb, ceil_i(cx + ho))}};
    for (const auto &sg : seg)
    for (int x = sg[0]; x <= sg[1]; x++) {
      const float dx = x + 0.5f - cx, d2 = dx * dx + dy2;
      if (d2 < lo2 || d2 > hi2) continue;  // круглые концы тоже лежат в кольце по радиусу
      const float cs = d0x * dy - d0y * dx, ce = dx * d1y - dy * d1x;  // ≥ 0 — по часовой от начала / до конца
      float cov;
      if (big ? (cs >= 0 || ce >= 0) : (cs >= 0 && ce >= 0)) {
        cov = hw + 0.5f - fabsf(d2 - r2) * inv2r;
      } else {
        const float ux0 = x + 0.5f - e0x, uy0 = y + 0.5f - e0y, ux1 = x + 0.5f - e1x, uy1 = y + 0.5f - e1y;
        const float q = fminf(ux0 * ux0 + uy0 * uy0, ux1 * ux1 + uy1 * uy1);
        if (q >= cap2) continue;
        cov = hw + 0.5f - halo_sqrt(q);
      }
      if (cov > 0) put(row + x, P, (int)(fminf(cov, 1) * P.A));
    }
  }
}

// ---------- Затемнить слой внутри круга ----------
// Пиксель 565 растянут в 32 бита (G — сверху, R и B — снизу, между ними зазоры): три канала — одним умножением,
// яркость — 32 ступени, вместо урезания — порог узора 4×4 (средний цвет точный). Край круга — с долей покрытия:
// за ним может быть соседний слой
static inline uint16_t fade_px(uint16_t c, uint32_t k5, uint32_t bias) {
  uint32_t d = __builtin_bswap16(c);
  d = (d | d << 16) & 0x07E0F81F;
  d = ((d * k5 + bias) >> 5) & 0x07E0F81F;
  return __builtin_bswap16((uint16_t)(d | d >> 16));
}

void HALO_HOT halo_fade_circle(float cx, float cy, float r, float k) {
  const uint32_t k5 = (uint32_t)(halo_clamp01(k) * 32 + 0.5f);
  if (k5 >= 32 || r <= 0 || !halo_hits(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1)) return;
  const float ro2 = (r + 0.5f) * (r + 0.5f), ri = r - 0.5f, r2 = r * r, inv2r = 0.5f / r;
  const int ya = max(hb.y, halo_floor(cy - r - 1)), yb = min(hb.y + hb.rows - 1, ceil_i(cy + r + 1));
  for (int y = ya; y <= yb; y++) {
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    uint16_t *row = halo_row(y);
    const uint8_t *t = BAYER[y & 3];
    uint32_t bias[4];
    for (int i = 0; i < 4; i++) bias[i] = (t[i] * 2 + 1) * 0x00200801u;
    const float ho = halo_sqrt(ro2 - dy2);
    const int xa = max(hb.x0, ceil_i(cx - ho - 0.5f)), xb = min(hb.x1, halo_floor(cx + ho - 0.5f));
    int xi0 = xb + 1, xi1 = xb;  // целиком внутри
    if (ri > 0 && dy2 < ri * ri) {
      const float hi = halo_sqrt(ri * ri - dy2);
      xi0 = max(xa, ceil_i(cx - hi - 0.5f)), xi1 = min(xb, halo_floor(cx + hi - 0.5f));
    }
    for (int x = xa; x <= xb; x++) {
      if (x == xi0 && xi0 <= xi1) {
        for (; x <= xi1; x++)
          if (row[x]) row[x] = fade_px(row[x], k5, bias[x & 3]);
        x = xi1;
        continue;
      }
      if (!row[x]) continue;
      const float dx = x + 0.5f - cx, cov = halo_clamp01((r2 - dx * dx - dy2) * inv2r + 0.5f);
      const uint32_t kp = 32 - (uint32_t)((32 - k5) * cov + 0.5f);  // край: затемнить на долю покрытия
      if (kp < 32) row[x] = fade_px(row[x], kp, bias[x & 3]);
    }
  }
}

// ---------- Всё вне круга — чёрным ----------
void HALO_HOT halo_clip_circle(float cx, float cy, float r) {
  const float ro2 = (r + 0.5f) * (r + 0.5f), ri = r - 0.5f, r2 = r * r, inv2r = 0.5f / fmaxf(r, 0.5f);
  for (int y = hb.y; y < hb.y + hb.rows; y++) {
    uint16_t *row = halo_row(y);
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro2) {
      halo_span(row, hb.x0, hb.x1, 0);
      continue;
    }
    const float ho = halo_sqrt(ro2 - dy2);
    const int xa = max(hb.x0, ceil_i(cx - ho - 0.5f)), xb = min(hb.x1, halo_floor(cx + ho - 0.5f));
    if (xa > hb.x0) halo_span(row, hb.x0, min(xa - 1, hb.x1), 0);
    if (xb < hb.x1) halo_span(row, max(xb + 1, hb.x0), hb.x1, 0);
    int xi0 = xb + 1, xi1 = xb;  // внутри — не трогаем
    if (ri > 0 && dy2 < ri * ri) {
      const float hi = halo_sqrt(ri * ri - dy2);
      xi0 = max(xa, ceil_i(cx - hi - 0.5f)), xi1 = min(xb, halo_floor(cx + hi - 0.5f));
    }
    for (int x = xa; x <= xb; x++) {
      if (x == xi0 && xi0 <= xi1) {
        x = xi1;
        continue;
      }
      const float dx = x + 0.5f - cx, cov = halo_clamp01((r2 - dx * dx - dy2) * inv2r + 0.5f);
      if (cov < 1) halo_blend(row + x, 0, 0, 0, (int)((1 - cov) * 256));
    }
  }
}

// ---------- Маски: буквы и одноцветные значки ----------
static inline int nib(const uint8_t *row, int i) {
  const uint8_t b = row[i >> 1];
  return (i & 1) ? (b & 15) : (b >> 4);
}

// Пиксель маски с покрытием v (1..15): на сплошном фоне — готовый цвет, иначе смешивание
static inline void pix(uint16_t *p, const Paint &P, int v) {
  if (P.lut) *p = P.lut[v];
  else put(p, P, v == 15 ? P.A : (v * 17 * P.A) >> 8);
}

// Готовые цвета краёв поверх сплошного фона bg: у маски всего 16 уровней — 16 цветов на вызов вместо смешивания
// каждого пикселя (~40 тактов: распаковать, три умножения, упаковать)
static void make_lut(uint16_t lut[16], const Paint &P, uint32_t bg) {
  // фон — ближайшим уровнем: под буквами он узором, и края букв не должны отличаться от него оттенком
  const int br = (lv5(bg >> 16 & 255) + 8) >> 4, bgr = (lv6(bg >> 8 & 255) + 8) >> 4, bb = (lv5(bg & 255) + 8) >> 4;
  for (int v = 0; v < 16; v++) {
    const int a = (v * 17 * P.A) >> 8;  // 0..256
    const int r = br + (((P.r5 - br) * a + 128) >> 8), g = bgr + (((P.g6 - bgr) * a + 128) >> 8);
    const int b = bb + (((P.b5 - bb) * a + 128) >> 8);
    lut[v] = __builtin_bswap16((r << 11) | (g << 5) | b);
  }
}

// Нулевая строка маски — вместо строк за краем маски (ширина маски — до 512 пикселей)
static const uint8_t ZERO[257] = {0};

// Отрезок строки маски, где что-то есть (ext); e0 > e1 — пусто
static inline void ext_of(const HaloMask &m, int r, int &e0, int &e1) {
  if (r >= 0 && r < m.h && m.ext[2 * r] <= m.ext[2 * r + 1]) e0 = min(e0, (int)m.ext[2 * r]), e1 = max(e1, (int)m.ext[2 * r + 1]);
}

static void HALO_HOT mask_1x(const HaloMask &m, int X, int Y, const Paint &P) {
  const int stride = (m.w + 1) / 2;
  const int ya = max(Y, hb.y), yb = min(Y + m.h - 1, hb.y + hb.rows - 1);
  for (int y = ya; y <= yb; y++) {
    const int r = y - Y, e0 = m.ext[2 * r], e1 = m.ext[2 * r + 1];
    if (e0 > e1) continue;
    const int xa = max(X + e0, hb.x0), xb = min(X + e1, hb.x1);
    const uint8_t *bits = m.bits + r * stride;
    uint16_t *row = halo_row(y);
    for (int x = xa; x <= xb; x++) {
      const int v = nib(bits, x - X);
      if (v) pix(row + x, P, v);
    }
  }
}

// Масштаб 1, но верх — с долей пикселя (лента едет вверх-вниз, буквы не дрожат относительно плашек): строка экрана —
// смесь двух соседних строк маски, только там, где в них что-то есть (2 чтения на пиксель вместо 4 у масштаба)
static void HALO_HOT mask_suby(const HaloMask &m, int X, float fy, const Paint &P) {
  const int stride = (m.w + 1) / 2, Y = halo_floor(fy), t = (int)((fy - Y) * 256 + 0.5f);  // t — вес строки выше
  if (t <= 0 || t >= 256) return mask_1x(m, X, Y + (t >= 256), P);
  const int ya = max(Y, hb.y), yb = min(Y + m.h, hb.y + hb.rows - 1);
  for (int y = ya; y <= yb; y++) {
    const int r = y - Y;
    int e0 = 1 << 14, e1 = -1;
    ext_of(m, r, e0, e1), ext_of(m, r - 1, e0, e1);
    if (e0 > e1) continue;
    // Строки маски выше и ниже края — нулевые (ZERO): без проверок на каждом пикселе
    const uint8_t *b0 = r < m.h ? m.bits + r * stride : ZERO, *b1 = r >= 1 ? m.bits + (r - 1) * stride : ZERO;
    const int xa = max(X + e0, hb.x0), xb = min(X + e1, hb.x1), t0 = 256 - t;
    uint16_t *row = halo_row(y);
    for (int x = xa; x <= xb; x++) {
      const int i = x - X, v = (nib(b0, i) * t0 + nib(b1, i) * t + 128) >> 8;
      if (v) pix(row + x, P, v);
    }
  }
}

// Масштаб: для пикселя экрана — точка маски (16.16) и среднее четырёх соседей (билинейно: без дрожи). По строке —
// только столбцы, где в двух строках маски что-то есть (ext)
static void HALO_HOT mask_scaled(const HaloMask &m, float fx, float fy, float s, const Paint &P) {
  const int stride = (m.w + 1) / 2;
  const float inv = 1 / s;
  const int xa = max(hb.x0, halo_floor(fx)), xb = min(hb.x1, ceil_i(fx + m.w * s));
  const int ya = max(hb.y, halo_floor(fy)), yb = min(hb.y + hb.rows - 1, ceil_i(fy + m.h * s));
  const int32_t du = (int32_t)(inv * 65536);
  for (int y = ya; y <= yb; y++) {
    const int32_t v = (int32_t)(((y + 0.5f - fy) * inv - 0.5f) * 65536);
    const int j = v >> 16, fj = (v >> 8) & 255;
    if (j < -1 || j >= m.h) continue;
    int e0 = 1 << 14, e1 = -1;
    ext_of(m, j, e0, e1), ext_of(m, j + 1, e0, e1);
    if (e0 > e1) continue;
    const int xs = max(xa, halo_floor(fx + (e0 - 1) * s)), xe = min(xb, ceil_i(fx + (e1 + 2) * s));
    const uint8_t *r0 = j >= 0 ? m.bits + j * stride : ZERO, *r1 = j + 1 < m.h ? m.bits + (j + 1) * stride : ZERO;
    uint16_t *row = halo_row(y);
    int32_t u = (int32_t)(((xs + 0.5f - fx) * inv - 0.5f) * 65536);
    for (int x = xs; x <= xe; x++, u += du) {
      const int i = u >> 16, fi = (u >> 8) & 255;
      int a00, a01, a10, a11;
      if (i >= 0 && i < m.w - 1) {  // внутри маски: две соседние точки — из одного байта или двух соседних
        const uint8_t *p0 = r0 + (i >> 1), *p1 = r1 + (i >> 1);
        if (i & 1) a00 = p0[0] & 15, a01 = p0[1] >> 4, a10 = p1[0] & 15, a11 = p1[1] >> 4;
        else a00 = p0[0] >> 4, a01 = p0[0] & 15, a10 = p1[0] >> 4, a11 = p1[0] & 15;
      } else {  // у левого и правого края маски
        if (i < -1 || i >= m.w) continue;
        const bool i0 = i >= 0, i1 = i + 1 < m.w;
        a00 = i0 ? nib(r0, i) : 0, a01 = i1 ? nib(r0, i + 1) : 0, a10 = i0 ? nib(r1, i) : 0, a11 = i1 ? nib(r1, i + 1) : 0;
      }
      if (!(a00 | a01 | a10 | a11)) continue;
      const int top = (a00 << 8) + (a01 - a00) * fi, bot = (a10 << 8) + (a11 - a10) * fi;
      const int a = ((top << 8) + (bot - top) * fj) >> 16;  // 0..15
      if (a) pix(row + x, P, a);
    }
  }
}

bool halo_subpixel;

void HALO_HOT halo_mask(const HaloMask &m, float x, float y, uint32_t rgb, float alpha, float scale, int32_t bg) {
  if (alpha <= 0 || !halo_hits(x - 1, y - 1, x + m.w * scale + 1, y + m.h * scale + 1)) return;
  Paint P = paint(rgb, alpha);
  uint16_t lut[16];
  if (bg >= 0) make_lut(lut, P, bg), P.lut = lut;
  if (fabsf(scale - 1) >= 0.002f) mask_scaled(m, x, y, scale, P);
  else if (halo_subpixel) mask_suby(m, halo_floor(x + 0.5f), y, P);
  else mask_1x(m, halo_floor(x + 0.5f), halo_floor(y + 0.5f), P);
}

// ---------- Текст ----------
static uint32_t utf8_next(const char *&s) {
  const uint8_t c = *s++;
  if (c < 0x80) return c;
  if ((c >> 5) == 6) return ((c & 31) << 6) | (*s++ & 63);
  if ((c >> 4) == 14) {
    uint32_t v = (c & 15) << 12;
    v |= (*s++ & 63) << 6;
    return v | (*s++ & 63);
  }
  uint32_t v = (c & 7) << 18;
  v |= (*s++ & 63) << 12;
  v |= (*s++ & 63) << 6;
  return v | (*s++ & 63);
}

static const HaloMask *glyph(const HaloFont &f, uint32_t cp) {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (f.codes[mid] == cp) return &f.glyphs[mid];
    if (f.codes[mid] < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

const HaloMask *halo_glyph(const HaloFont &f, uint32_t code) {
  return glyph(f, code);
}

uint32_t halo_utf8_next(const char *&s) {
  return utf8_next(s);
}

int halo_text_width(const HaloFont &f, const char *s, int spacing) {
  int w = 0;
  while (*s) {
    const HaloMask *g = glyph(f, utf8_next(s));
    if (g) w += g->adv + spacing;
  }
  return w > 0 ? w - spacing : 0;
}

void HALO_HOT halo_text(const HaloFont &f, const char *s, float x, float y, uint32_t rgb, float alpha, int align,
                        float scale, int spacing, int32_t bg) {
  if (alpha <= 0 || y + f.line * scale + 2 < hb.y || y - 2 >= hb.y + hb.rows) return;
  // Буквы строки — один раз: и для ширины, и для рисования (раньше — два поиска каждой буквы в каждой пачке)
  const HaloMask *gs[64];
  int n = 0, width = 0;
  for (const char *p = s; *p && n < 64;) {
    const HaloMask *g = glyph(f, utf8_next(p));
    if (g) gs[n++] = g, width += g->adv + spacing;
  }
  const float tw = (n ? width - spacing : 0) * scale;
  if (align == 1) x -= tw / 2;
  else if (align == 2) x -= tw;
  if (!halo_hits(x - 2, y - 2, x + tw + 2, y + f.line * scale + 2)) return;
  Paint P = paint(rgb, alpha);
  uint16_t lut[16];
  if (bg >= 0) make_lut(lut, P, bg), P.lut = lut;
  const bool one = fabsf(scale - 1) < 0.002f, suby = one && halo_subpixel;
  const int X = halo_floor(x + 0.5f), Y = halo_floor(y + 0.5f);
  int pen = 0;
  for (int k = 0; k < n; k++) {
    const HaloMask *g = gs[k];
    if (suby) mask_suby(*g, X + pen + g->ox, y + g->oy, P);
    else if (one) mask_1x(*g, X + pen + g->ox, Y + g->oy, P);
    else {
      const float gx = x + (pen + g->ox) * scale, gy = y + g->oy * scale;
      if (halo_hits(gx - 1, gy - 1, gx + g->w * scale + 1, gy + g->h * scale + 1)) mask_scaled(*g, gx, gy, scale, P);
    }
    pen += g->adv + spacing;
  }
}

float halo_text_mid(const HaloFont &f, const char *s) {
  float t = 1e9f, b = -1e9f;
  while (*s) {
    const HaloMask *g = glyph(f, utf8_next(s));
    if (!g || (g->w <= 1 && g->h <= 1)) continue;  // пробел — пустая маска 1×1
    t = fminf(t, g->oy), b = fmaxf(b, g->oy + g->h);
  }
  return t <= b ? (t + b) / 2 : f.line / 2.0f;
}

// ---------- Строка по дуге — в маску (как make_text_arc в AmoledWear) ----------
bool halo_text_arc(HaloMask &m, const HaloFont &f, const char *s, float mid, float rad, float angle, bool bottom,
                   int spacing, float *ox, float *oy) {
  struct G {
    const HaloMask *g;
    float px, py, rot;
  };
  G gs[48];
  int n = 0;
  const float width = halo_text_width(f, s, spacing), C = HALO_W / 2.0f;
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  int pen = 0;
  for (const char *p = s; *p && n < 48;) {
    const HaloMask *g = glyph(f, utf8_next(p));
    if (!g) continue;
    // Центр буквы вдоль строки — от её середины; насколько центр буквы выше середины строки
    const float gx = pen + g->ox + g->w * 0.5f - width * 0.5f, up = mid - (g->oy + g->h * 0.5f);
    pen += g->adv + spacing;
    if (g->w <= 1 && g->h <= 1) continue;
    const float th = bottom ? angle - gx / rad : angle + gx / rad, R = bottom ? rad - up : rad + up;
    G &q = gs[n++];
    q = {g, C + R * sinf(th), C - R * cosf(th), bottom ? th + HALO_PI : th};
    const float e = sqrtf((float)(g->w * g->w + g->h * g->h)) / 2 + 1;
    x0 = fminf(x0, q.px - e), y0 = fminf(y0, q.py - e), x1 = fmaxf(x1, q.px + e), y1 = fmaxf(y1, q.py + e);
  }
  if (!n) return false;
  const int ix = halo_floor(x0), iy = halo_floor(y0), w = ceil_i(x1) - ix + 1, h = ceil_i(y1) - iy + 1;
  uint8_t *a8 = (uint8_t *)calloc(w * h, 1);
  const int stride = (w + 1) / 2;
  uint8_t *bits = (uint8_t *)calloc(stride * h, 1), *ext = (uint8_t *)malloc(2 * h);
  if (!a8 || !bits || !ext) {
    free(a8), free(bits), free(ext);
    return false;
  }
  for (int i = 0; i < n; i++) {  // каждая буква — с поворотом, билинейно; из двух букв берём большую прозрачность
    const HaloMask &g = *gs[i].g;
    const float cs = cosf(gs[i].rot), sn = sinf(gs[i].rot), e = sqrtf((float)(g.w * g.w + g.h * g.h)) / 2 + 1;
    const int gstride = (g.w + 1) / 2;
    auto at = [&](int u, int v) { return u < 0 || v < 0 || u >= g.w || v >= g.h ? 0 : nib(g.bits + v * gstride, u); };
    for (int yy = halo_floor(gs[i].py - e); yy <= ceil_i(gs[i].py + e); yy++)
      for (int xx = halo_floor(gs[i].px - e); xx <= ceil_i(gs[i].px + e); xx++) {
        const int mx = xx - ix, my = yy - iy;
        if (mx < 0 || my < 0 || mx >= w || my >= h) continue;
        const float dx = xx + 0.5f - gs[i].px, dy = yy + 0.5f - gs[i].py;
        const float u = dx * cs + dy * sn + g.w / 2.0f - 0.5f, v = -dx * sn + dy * cs + g.h / 2.0f - 0.5f;
        if (u <= -1 || v <= -1 || u >= g.w || v >= g.h) continue;
        const int iu = halo_floor(u), iv = halo_floor(v);
        const float fu = u - iu, fv = v - iv;
        const float a = (at(iu, iv) * (1 - fu) + at(iu + 1, iv) * fu) * (1 - fv) +
                        (at(iu, iv + 1) * (1 - fu) + at(iu + 1, iv + 1) * fu) * fv;  // 0..15
        const int v8 = (int)(a * 17 + 0.5f);
        uint8_t &d = a8[my * w + mx];
        if (v8 > d) d = v8 > 255 ? 255 : v8;
      }
  }
  for (int y = 0; y < h; y++) {  // в 4 бита; у строки — первый и последний непустой столбец
    int e0 = w, e1 = -1;
    for (int x = 0; x < w; x++) {
      const int v4 = (a8[y * w + x] * 15 + 127) / 255;
      if (!v4) continue;
      bits[y * stride + x / 2] |= (x & 1) ? v4 : v4 << 4;
      if (x < e0) e0 = x;
      e1 = x;
    }
    ext[2 * y] = e1 < 0 ? 1 : min(e0, 255), ext[2 * y + 1] = e1 < 0 ? 0 : min(e1, 255);
  }
  free(a8);
  // Сначала новая, потом старую — так у новой точно другой адрес (элемент замечает смену маски по адресу)
  uint8_t *old_bits = (uint8_t *)m.bits, *old_ext = (uint8_t *)m.ext;
  m = {(int16_t)w, (int16_t)h, 0, 0, (int16_t)w, bits, ext};
  free(old_bits), free(old_ext);
  if (ox) *ox = ix;
  if (oy) *oy = iy;
  return true;
}

// ---------- Цветные значки (цвет уже × прозрачность) ----------
// Поверх: d = d · (1 − a·A) + s · A, по каналам 565
// Пиксель дисплея (старший байт первым) ↔ «разложенный» RGB565: B — биты 0..4, R — 11..15, G — 21..26; между
// полями — запас, чтобы умножать все три канала на вес до 32 одним умножением
static inline uint32_t spread565(uint16_t px) {
  const uint32_t c = __builtin_bswap16(px);
  return (c | (c << 16)) & 0x07E0F81F;
}
static inline uint16_t pack565(uint32_t c) {
  return __builtin_bswap16((uint16_t)(c | (c >> 16)));
}
// Как img_put, но цвет уже разложен, а прозрачность целиком — A5 0..32
static inline void img_put5(uint16_t *p, uint32_t s, int a, int A5) {
  if (a == 15 && A5 >= 32) {
    *p = pack565(s);
    return;
  }
  const int k = (a * 17 * A5 + 128) >> 8;  // насколько закрываем фон, 0..32
  *p = pack565(((spread565(*p) * (32 - k) + s * A5) >> 5) & 0x07E0F81F);  // цвет уже умножен на прозрачность
}

static inline void img_put(uint16_t *p, uint16_t src, int a, int A) {  // a — 0..15, A — 0..256
  if (a == 15 && A >= 256) {
    *p = src;
    return;
  }
  const int k = (a * 17 * A) >> 8;  // насколько закрываем фон, 0..256
  const uint32_t d = __builtin_bswap16(*p), s = __builtin_bswap16(src);
  const int R = ((d >> 11) * (256 - k) + (s >> 11) * A) >> 8;
  const int G = (((d >> 5) & 63) * (256 - k) + ((s >> 5) & 63) * A) >> 8;
  const int B = ((d & 31) * (256 - k) + (s & 31) * A) >> 8;
  *p = __builtin_bswap16((min(R, 31) << 11) | (min(G, 63) << 5) | min(B, 31));
}

void HALO_HOT halo_image(const HaloImage &im, float x, float y, float alpha, float scale) {
  if (alpha <= 0 || !halo_hits(x - 1, y - 1, x + im.w * scale + 1, y + im.h * scale + 1)) return;
  const int A = (int)(halo_clamp01(alpha) * 256 + 0.5f), stride = (im.w + 1) / 2;
  if (fabsf(scale - 1) < 0.002f) {
    const int X = halo_floor(x + 0.5f), Y = halo_floor(y + 0.5f);
    const int ya = max(Y, hb.y), yb = min(Y + im.h - 1, hb.y + hb.rows - 1);
    for (int yy = ya; yy <= yb; yy++) {
      const int r = yy - Y, e0 = im.ext[2 * r], e1 = im.ext[2 * r + 1];
      if (e0 > e1) continue;
      const int xa = max(X + e0, hb.x0), xb = min(X + e1, hb.x1);
      const uint8_t *al = im.alpha + r * stride;
      const uint16_t *px = im.px + r * im.w - X;
      uint16_t *row = halo_row(yy);
      for (int xx = xa; xx <= xb; xx++) {
        const int a = nib(al, xx - X);
        if (a) img_put(row + xx, px[xx], a, A);
      }
    }
    return;
  }
  // С масштабом: билинейно по цвету и прозрачности. Только непустая часть строк (ext двух соседних строк картинки),
  // веса по 1/32 и три канала одним умножением (цвет «разложен» по 32 битам маской 0x07E0F81F): было ~100 тактов на
  // пиксель всего прямоугольника, значки погоды в переходах занимали 3–4,5 мс кадра
  const float inv = 1 / scale;
  const int xa = max(hb.x0, halo_floor(x)), xb = min(hb.x1, ceil_i(x + im.w * scale));
  const int ya = max(hb.y, halo_floor(y)), yb = min(hb.y + hb.rows - 1, ceil_i(y + im.h * scale));
  const int32_t du = (int32_t)(inv * 65536);
  const int A5 = (A + 4) >> 3;  // 0..32
  for (int yy = ya; yy <= yb; yy++) {
    const int32_t v = (int32_t)(((yy + 0.5f - y) * inv - 0.5f) * 65536);
    const int j = v >> 16, fj = (v >> 11) & 31;
    if (j < -1 || j >= im.h) continue;
    int e0 = 1 << 14, e1 = -1;
    for (int r = j; r <= j + 1; r++)
      if (r >= 0 && r < im.h && im.ext[2 * r] <= im.ext[2 * r + 1])
        e0 = min(e0, (int)im.ext[2 * r]), e1 = max(e1, (int)im.ext[2 * r + 1]);
    if (e0 > e1) continue;
    const int xs = max(xa, halo_floor(x + (e0 - 1) * scale)), xe = min(xb, ceil_i(x + (e1 + 2) * scale));
    const bool j0 = j >= 0, j1 = j + 1 < im.h;
    const uint8_t *a0 = j0 ? im.alpha + j * stride : ZERO, *a1 = j1 ? im.alpha + (j + 1) * stride : ZERO;
    const uint16_t *p0 = im.px + (j0 ? j : 0) * im.w, *p1 = im.px + (j1 ? j + 1 : 0) * im.w;
    uint16_t *row = halo_row(yy);
    int32_t u = (int32_t)(((xs + 0.5f - x) * inv - 0.5f) * 65536);
    for (int xx = xs; xx <= xe; xx++, u += du) {
      const int i = u >> 16, fi = (u >> 11) & 31;
      if (i < -1 || i >= im.w) continue;
      const bool i0 = i >= 0, i1 = i + 1 < im.w;
      const int a00 = i0 ? nib(a0, i) : 0, a01 = i1 ? nib(a0, i + 1) : 0;
      const int a10 = i0 ? nib(a1, i) : 0, a11 = i1 ? nib(a1, i + 1) : 0;
      if (!(a00 | a01 | a10 | a11)) continue;
      // Внутри заливки (значки погоды — ровные цвета): все четыре соседа непрозрачны и одного цвета — его и ставим
      if ((a00 & a01 & a10 & a11) == 15) {
        const uint16_t q = p0[i];
        if (q == p0[i + 1] && q == p1[i] && q == p1[i + 1]) {
          if (A5 >= 32) row[xx] = q;
          else img_put5(row + xx, spread565(q), 15, A5);
          continue;
        }
      }
      const int w11 = (fi * fj) >> 5, w01 = fi - w11, w10 = fj - w11, w00 = 32 - fi - fj + w11;  // сумма — 32
      const int a = (a00 * w00 + a01 * w01 + a10 * w10 + a11 * w11 + 16) >> 5;                  // 0..15
      if (!a) continue;
      uint32_t c = 0;  // цвет уже умножен на прозрачность: у пустых точек он 0, их можно не брать
      if (a00) c += spread565(p0[i]) * w00;
      if (a01) c += spread565(p0[i + 1]) * w01;
      if (a10) c += spread565(p1[i]) * w10;
      if (a11) c += spread565(p1[i + 1]) * w11;
      img_put5(row + xx, (c >> 5) & 0x07E0F81F, a, A5);
    }
  }
}
