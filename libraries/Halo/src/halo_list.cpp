// Слой интерфейса Halo: лента карточек Wear OS (TransformingLazyColumn) — перенос ленты AmoledWear (wear_core.cpp)
// с теми же числами: палец один к одному, инерция, «резина» за краями, пружина обратно, карточки у края круга
// уменьшаются до 0,6 и бледнеют по S-кривой (0.3, 0, 0.7, 1). Скорость пальца у Halo — в px/с (у AmoledWear — px/мс).
//
// У края — по скорости (в AmoledWear одна жёсткая пружина на все скорости: лента по инерции тормозит на 1–3 тыс. px/с²,
// а у края — на 17–230 тыс., встаёт за кадр и отскакивает; на медленном свайпе это выглядело как удар об стенку):
//   * медленно — за 80 px до края лента мягко подкатывается к нему и встаёт, без отскока;
//   * средне — небольшой мягкий заезд за край, растёт со скоростью;
//   * быстро (у края от 1500 px/с) — удар и отскок, как было;
//   * перетянули пальцем и отпустили — назад тем мягче, чем медленнее отпустили (0,42 с → 0,28 с).
#include "halo_internal.h"

#pragma GCC optimize("O3", "fast-math")

static HaloEase EASE_LIST;  // карточки у края

#define DOCK_ZONE 80       // за сколько px до края решаем, как к нему подойти
#define DOCK_SOFT 350      // скорость у края (px/с), до которой — подкатиться без перелёта
#define DOCK_FAST 1500     // и от которой — удар и отскок, как было

// Пружина без перелёта (критическое затухание) с частотой w, за d px до края, скорость v к нему: насколько заедет за
// край. x(t) = (x0 + (v + w·x0)·t)·e^(−wt), x0 = −d; пик — где x'(t) = 0
static float dock_over(float d, float v, float w) {
  const float a = v - w * d;
  if (a <= 0) return 0;  // подкатится, не переходя края
  const float t = v / (w * a);
  return (-d + a * t) * expf(-w * t);
}
// Частота, при которой заедет за край ровно на over px (0 — подкатится без перелёта): меньше частота — больше заезд
static float dock_w(float d, float v, float over) {
  const float w0 = fmaxf(v / d, 3);
  if (over <= 0.05f) return w0;
  float lo = 0.5f, hi = w0;
  for (int i = 0; i < 24; i++) {
    const float m = (lo + hi) / 2;
    (dock_over(d, v, m) > over ? lo : hi) = m;
  }
  return (lo + hi) / 2;
}
// Назад из-за края: чем медленнее, тем мягче (0,42 с — медленно, 0,28 с — как было, от 1500 px/с)
static HaloSpringK back_k(float v) {
  return halo_spring(0.42f - 0.14f * halo_clamp01(fabsf(v) / DOCK_FAST), 1);
}
// Обратное к halo_rubber: сколько пальцем, чтобы лента заехала за край на r (взять ленту за краем — без скачка)
static float rubber_inv(float r) {
  const float a = fminf(fabsf(r) / HALO_W, 0.95f), d = (1 / (1 - a) - 1) * HALO_W / 0.55f;
  return r < 0 ? -d : d;
}

static void layout(HaloList &l, float base);

void halo_list_init(HaloList &l, const float *heights, int n, float top, float bottom, bool header) {
  if (EASE_LIST.v[64] != 1) {
    halo_ease_make(EASE_LIST, 0.3f, 0, 0.7f, 1);
  }
  l = {};
  l.n = min(n, HALO_LIST_MAX);
  l.gap = 8, l.edge = 0.6f, l.header = header;  // промежуток 4 dp
  l.top = top, l.bottom = bottom;
  for (int i = 0; i < l.n; i++) {
    l.h[i] = heights[i];
    l.cum[i] = l.total;
    l.total += l.h[i] + l.gap;
  }
  l.total -= l.gap;
  l.scroll = top;
  l.last = -1e9f;
  layout(l, l.scroll);  // раскладка — сразу: лента может быть видна раньше своего первого update (выезжает над циферблатом)
}

// Элемент у верхнего или нижнего края круга уменьшается и бледнеет. top, bottom — где он был бы без уменьшения
static HaloListFx fx_at(const HaloList &l, int i, float top, float bottom) {
  const float tf = top / HALO_H, bf = bottom / HALO_H, rel = bf - tf;
  // Где начинается зона перехода: для крупных элементов — дальше от края (ResponsiveTransformationSpec, large)
  float line = 0.40f + 0.20f * halo_clamp01((rel - 0.15f) * (1 / 0.30f));
  line = fminf(line, (1 + rel) / 2);
  const float edge = bf < 1 - tf ? bf : 1 - tf;  // ближе к верху — по нижней кромке, к низу — по верхней
  const float e = halo_ease(EASE_LIST, edge / line);
  HaloListFx f;
  f.top = top;
  f.s = l.edge + (1 - l.edge) * e;
  f.a_bg = 0.5f + 0.5f * e;
  f.a_c = f.a_bg * f.a_bg;  // содержимое лежит в слое фона: 0,5 × 0,5 у края
  if (!i && l.header) {     // заголовок не уменьшается и бледнеет только у нижнего края (так на Pixel Watch)
    f.s = 1;
    if (top + bottom < HALO_H) f.a_bg = f.a_c = 1;
  }
  return f;
}

static void layout(HaloList &l, float base) {
  int a = 0;
  float best = 1e9f;
  for (int i = 0; i < l.n; i++) {
    const float d = fabsf(base + l.cum[i] + l.h[i] / 2 - HALO_H / 2.0f);
    if (d < best) best = d, a = i;
  }
  auto gap = [&](int i) { return l.h[i] > 0.5f ? l.gap : 0.0f; };  // схлопнутый элемент — без промежутка
  auto up = [](float v) { return (float)-halo_floor(-v); };
  const float ya = base + l.cum[a];
  l.fx[a] = fx_at(l, a, ya, ya + l.h[a]);
  float y = ya + up(l.fx[a].s * l.h[a]) + gap(a);
  for (int i = a + 1; i < l.n; i++) {  // вниз: известен верх места
    l.fx[i] = fx_at(l, i, y, y + l.h[i]);
    y += up(l.fx[i].s * l.h[i]) + gap(i);
  }
  float yb = ya - (a > 0 ? gap(a - 1) : 0);
  for (int i = a - 1; i >= 0; i--) {  // вверх: известен низ места
    l.fx[i] = fx_at(l, i, yb - l.h[i], yb);
    const float th = up(l.fx[i].s * l.h[i]);
    l.fx[i].top = yb - th;
    yb -= th + (i > 0 ? gap(i - 1) : 0);
  }
}

void halo_list_reset(HaloList &l) {
  l.scroll = l.last = l.top, l.fling_v = 0, l.flinging = l.back_on = l.moving = false;
  layout(l, l.scroll);
}

void halo_list_update(HaloList &l, float dt, bool take_touch) {
  HaloFinger &f = halo_finger;
  const float mn = fminf(l.top, halo_list_min(l));
  if (f.pressed) l.flinging = false;  // ленту, летящую по инерции, палец останавливает
  if (f.drag_start && !f.horiz && take_touch && halo_finger_claim(&l)) {
    // Взяли ленту за краем (она отскакивает) — продолжить «резину» с того же места, без скачка
    l.g_start = l.scroll > l.top ? l.top + rubber_inv(l.scroll - l.top) : l.scroll < mn ? mn + rubber_inv(l.scroll - mn) : l.scroll;
    l.g_ref = f.y, l.back_on = false;
  }
  if (f.owner == &l) {
    if (f.down) {  // ведём пальцем — ровно за ним, за краями — резина
      const float raw = l.g_start + f.y - l.g_ref;
      l.scroll = raw > l.top ? l.top + halo_rubber(raw - l.top) : raw < mn ? mn + halo_rubber(raw - mn) : raw;
    } else if (f.released) {
      l.fling_v = f.vy, l.flinging = true;
    }
  }
  if (l.flinging) {
    // Подходит к краю: решить по скорости, с которой долетела бы до него (инерция гасит скорость на 3,2 px/с на
    // каждый пиксель пути). Медленно и средне — пружина, которая подкатит ленту к краю и заедет за него на over px
    const float edge = l.fling_v > 0 ? l.top : mn, d = fabsf(edge - l.scroll), v = fabsf(l.fling_v);
    const bool inside = l.scroll <= l.top && l.scroll >= mn;
    if (inside && d > 0.5f && d <= DOCK_ZONE) {
      const float ve = v - 3.2f * d;
      if (ve > 0 && ve < DOCK_FAST) {
        const float u = halo_clamp01((ve - DOCK_SOFT) / (DOCK_FAST - DOCK_SOFT)), over = u * u * (3 - 2 * u) * ve / 60.8f;
        l.back = {l.scroll, l.fling_v, edge};
        l.back_k = halo_spring(2 * HALO_PI / dock_w(d, v, over), 1);
        l.back_on = true, l.flinging = false;
      }
    }
  }
  if (l.flinging) {  // инерция гаснет; вылетела за край — дальше пружиной обратно, с той же скоростью
    l.scroll += l.fling_v * dt;
    l.fling_v *= expf(-dt * 3.2f);
    if (l.scroll > l.top || l.scroll < mn) {
      l.flinging = false;
      l.back = {l.scroll, l.fling_v, l.scroll > l.top ? l.top : mn};
      l.back_k = back_k(l.fling_v);
      l.back_on = true;
    } else if (fabsf(l.fling_v) < 8) {
      l.flinging = false;
    }
  } else if (!(f.owner == &l && f.down) && !l.back_on && (l.scroll > l.top + 0.05f || l.scroll < mn - 0.05f)) {
    l.back = {l.scroll, 0, l.scroll > l.top ? l.top : mn};  // отпустили за краем
    l.back_k = back_k(0);
    l.back_on = true;
  }
  if (l.back_on) {
    halo_spring_run(l.back, l.back_k, dt);
    l.scroll = l.back.x;
    if (fabsf(l.back.x - l.back.target) < 0.05f && fabsf(l.back.v) < 2) l.scroll = l.back.target, l.back_on = false;
  }
  l.moving = fabsf(l.scroll - l.last) > 0.01f;
  l.last = l.scroll;
  layout(l, l.scroll);
}

float halo_list_progress(const HaloList &l) {
  const float range = l.top - halo_list_min(l);
  return range > 1 ? halo_clamp01((l.top - l.scroll) / range) : 0;
}

float halo_list_visible(const HaloList &l) {
  return halo_clamp01((l.bottom - l.top) / fmaxf(l.total, 1));
}

int halo_list_hit(const HaloList &l, float y) {
  for (int i = 0; i < l.n; i++)
    if (y >= l.fx[i].top && y <= l.fx[i].top + l.h[i] * l.fx[i].s) return i;
  return -1;
}
