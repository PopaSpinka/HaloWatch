// HaloWatch — циферблат и карусель из WearKit на своей библиотеке Halo.
//
// Главный — циферблат по макету (face.h): дата, время, плашка погоды; всё, что меняется, — с анимацией (цифры и буквы
// прокручиваются, значок погоды сменяется). Нажатие — меняется всё сразу (показ), нажатие на плашку — погода прямо
// поверх циферблата (переход Zoom, Fade или Slide — долгое нажатие; назад — свайп вверх от нижнего края:
// halo_nav_face_open). Свайп вверх по циферблату — переход Fade к галерее, свайп вниз от её верхнего края — обратно на
// циферблат (halo_nav_face); с любого экрана галереи — свой жест на циферблат (Pages и Weather — вверх от нижнего края,
// Settings и заглушки — оба). Пришли на циферблат — в следующий раз галерея с начала.
//
// Галерея — карусель из WearKit один в один: лента карточек Wear OS
// (TransformingLazyColumn: у краёв круга карточки уменьшаются и бледнеют, инерция, «резина» за краями), время по
// дуге сверху (при прокрутке уезжает), индикатор прокрутки справа (гаснет через 2 с), нажатие на карточку —
// переход Android 16, назад — свайп от левого края за пальцем. Числа — те же, что в projects/WearKit.
//
// Интерфейс на экране — по-английски, шрифт Google Sans Flex со скруглёнными концами (как на Wear OS 6). Пять карточек:
// «Pages» — пример WearKit «Страницы» (точки и листание), «Weather» — погода по макету (weather.h), «Settings» —
// разделы настроек (settings.h; «Display» — яркость ползунком из WearKit, display.h), «Alarm» и «Music» — заглушки:
// экран с названием по центру. Кнопка BOOT гасит и включает экран (плата работает дальше), PWR — блокировка:
// нажатие — касания не работают, яркость — самая низкая; долгое нажатие — снова как было (display.h).
//
// Что откуда (lib/Halo, docs/halo.md): лента — halo_list_* (перенос ленты AmoledWear), переходы — halo_nav_* (как в
// AmoledWear, но оба экрана вживую, без снимков), дуги — HI_ARC, время по дуге — halo_text_arc. Часов реального
// времени на плате нет, а Wi-Fi эта версия не трогает: при включении часы ставят случайные дату и время и идут от них
// (random_clock ниже). На экран уходит только изменившееся: в покое кадры не рисуются вовсе.
// Команды: face / face off (циферблат / галерея), open N (0 — «Страницы» … 4, 5 — кнопка у края), back, page N, demo, wx N, fx N, tick [N], day,
// tap/press/swipe/drag/fling (искусственный палец); приборы — trace, rec, shot, prof, sys, log.
#pragma GCC optimize("O3", "fast-math")
#include <Halo.h>
#include "src/halo_assets.h"

#define W 466
#define CX 233.0f
#define DP 2.0f  // 1 dp Wear OS ≈ 2 пикселя этого экрана
// No network and no real-time clock on the board: the watch starts at a random date and time in 2026 and keeps
// going from there (a demo; for real time, add NTP over Wi-Fi)
static inline bool halo_clock_valid() {
  return true;
}
static void random_clock() {
  setenv("TZ", "UTC0", 1), tzset();
  const struct timeval tv = {(time_t)(1767225600 + esp_random() % (365 * 86400)), 0};  // 2026-01-01 + up to a year
  settimeofday(&tv, nullptr);
}
// Цвета — как в AmoledWear
#define TEXT 0xFFF8EE     // тёплый белый
#define DIM 0x9E9E9E      // второстепенное
#define CARD_HI 0x2E3034  // кнопка (surfaceContainerHigh)

static HaloSpringK K_M3_DEFAULT, K_PRESS, K_RELEASE, K_SC_VIS, K_PAGER;
struct Press {  // нажатие кнопки (ниже — press_update)
  HaloSpring s;
  bool going_down;
};

// Текст по середине букв (mid — halo_text_mid) в (x, y): так ставит текст WearKit (text_center). on — на чём лежит
// (сплошная карточка, круг или &halo_black): края букв тогда — готовыми цветами, без смешивания
static void text_mid(HaloItem &it, const HaloFont &f, const char *s, float mid, float x, float y, float sc,
                     uint32_t color, float alpha, const HaloItem *on) {
  it.kind = HI_TEXT, it.font = &f, it.text = s, it.align = 1, it.color = color, it.on_black = false, it.on = on;
  it.x = x, it.y = y + (f.line / 2.0f - mid) * sc, it.scale = sc, it.alpha = alpha;
}
// Текст от левого края x, базовая линия — на y
static void text_base(HaloItem &it, const HaloFont &f, const char *s, float x, float y, float sc, uint32_t color,
                      float alpha, const HaloItem *on) {
  it.kind = HI_TEXT, it.font = &f, it.text = s, it.align = 0, it.color = color, it.on_black = false, it.on = on;
  it.x = x, it.y = y + (f.line / 2.0f - f.ascent) * sc, it.scale = sc, it.alpha = alpha;
}

// ---------- Нажатие (как у кнопок Wear OS: RoundButton.kt) ----------
// 0 — отпущена, 1 — нажата. Нажатие — очень быстрая пружина (fast × 9), отпускание — медленная с отскоком. Даже
// короткий тап доигрывает нажатие хотя бы до 0,75 — и только потом отпускает: нажатие всегда видно
static void press_update(Press &p, bool held, float dt, const HaloSpringK &k_down = K_PRESS) {
  if (held) p.going_down = true;
  else if (p.going_down && p.s.x >= 0.75f) p.going_down = false;
  p.s.target = p.going_down ? 1 : 0;
  halo_spring_run(p.s, p.going_down ? k_down : K_RELEASE, dt);
}

// ---------- Карточки ----------
struct Entry {
  const char *title, *sub, *icon;  // значок — Font Awesome (шрифт ic24)
  uint32_t color;
};
static const Entry ENTRIES[] = {
    {"Pages", "dots and paging", "", 0x78D9EC},
    {"Alarm", "rings at 7:30", "", 0xFDD663},
    {"Music", "piano classics", "", 0xF6B5EA},
    {"Weather", "now and hourly", "", 0xFFB030},
    {"Settings", "display brightness", "", 0xA8C7FA},
};
#define N_ENTRIES (int)(sizeof(ENTRIES) / sizeof(ENTRIES[0]))

// ---------- Меню: лента карточек ----------
#define M_ITEM_H 104.0f  // кнопка с подписью: 52 dp
#define M_ITEM_W 410.0f
#define M_HEAD_H 64.0f
#define TT_R 219.0f      // радиус середины букв времени сверху (отступ от края 2 dp)
// Кнопка у края в конце ленты (EdgeButton, как пример WearKit «Кнопка у края»): сверху — скруглённые углы, бока — по
// изгибу экрана с отступом 20 px, низ — плавной дугой в 3 dp от края (эллипс: внизу нет острия). Растёт снизу, пока ленту листают к концу: высота — ровно по
// прокрутке, а скачок больше 16 dp без прокрутки — пружиной 400/1 (ScreenScaffold.kt). Ниже 30 dp гаснет фон, ниже
// 38 dp — надпись (EdgeButton.kt). Нажатие «сжимает» верхние углы, как у карточек; открывает заглушку «More»
#define EB_FULL 107.0f         // во весь рост: 140 px (70 dp) выдвигалась слишком далеко — край по пробе пальцем
#define EB_R 227.0f            // низ — по кругу экрана с отступом 3 dp
#define EB_W 300.0f
#define EB_ID N_ENTRIES        // open N_ENTRIES — экран кнопки
static HaloList menu;
static Press m_press[N_ENTRIES];
static int m_armed = -1, open_req = -1, cur;  // cur — открытый пункт
static volatile int cmd_open = -1;
static float tt_k = 1;             // время сверху: 1 — на месте, 0 — уехало (ScrollAway)
static float ui_lock;              // блокировка (display.h): 0..1 — время сверху гаснет за первую половину, потом замок
static HaloSpring sc_pos;          // индикатор прокрутки: положение (догоняет за 500 мс)
static HaloSpring sc_vis;          // и видимость: виден, пока листают, гаснет через 2 с покоя (пружина k=400)
static uint32_t sc_idle_ms;
static HaloMask tt_mask;           // время сверху, «запечённое» по дуге в маску (раз в минуту)
static float tt_x, tt_y, mid_head, mid_icon, mid_time;
static char time_str[8] = "--:--";
static int clock_h = -1, clock_m = -1;
static HaloItem m_head, m_time, m_sc[3];
static HaloItem m_bg[N_ENTRIES], m_disc[N_ENTRIES], m_icon[N_ENTRIES], m_title[N_ENTRIES], m_sub[N_ENTRIES];
static HaloSpring eb_h;  // видимая высота кнопки у края, px
static HaloSpringK K_EB;
static Press eb_press;
static bool eb_armed;
static float mid_eb;
static HaloItem m_eb, m_eb_txt;

// Пришли на циферблат (halo_nav_face зовёт enter главного): в следующий раз галерея — с начала, как при запуске
static void menu_enter() {
  halo_list_reset(menu);
  m_armed = -1, tt_k = 1;
  halo_spring_set(sc_pos, 0), halo_spring_set(sc_vis, 0);
  for (auto &p : m_press) halo_spring_set(p.s, 0), p.going_down = false;
  halo_spring_set(eb_h, 0), halo_spring_set(eb_press.s, 0), eb_press.going_down = eb_armed = false;
}

static void menu_update(float dt) {
  HaloFinger &f = halo_finger;
  halo_list_update(menu, dt);
  // Кнопка у края: во весь рост в конце ленты, ниже — на сколько не долистали (за краем, в «резине», — во весь рост)
  eb_h.target = halo_clamp01(1 - (menu.scroll - halo_list_min(menu)) / EB_FULL) * EB_FULL;
  const bool scrolling = f.owner == &menu || menu.flinging || menu.back_on;
  if (scrolling || fabsf(eb_h.target - eb_h.x) <= 16 * DP) eb_h.x = eb_h.target, eb_h.v = 0;
  halo_spring_run(eb_h, K_EB, dt);
  // Нажатие на карточку (или на кнопку у края — она поверх ленты) — анимация нажатия, отпустил — открыть
  if (f.pressed) {
    const int i = halo_list_hit(menu, f.y0);
    m_armed = i >= 1 ? i - 1 : -1;
    if (m_armed >= 0 && fabsf(f.x0 - CX) > M_ITEM_W / 2 * menu.fx[i].s) m_armed = -1;
    eb_armed = eb_h.x > 60 && f.y0 > CX + EB_R - eb_h.x && fabsf(f.x0 - CX) < EB_W / 2;
    if (eb_armed) m_armed = -1;
  }
  if (f.dragging) m_armed = -1, eb_armed = false;
  if (f.tap && m_armed >= 0) open_req = m_armed;
  if (f.tap && eb_armed) open_req = EB_ID;
  for (int i = 0; i < N_ENTRIES; i++) press_update(m_press[i], i == m_armed && f.down, dt);
  press_update(eb_press, eb_armed && f.down, dt);
  if (!f.down) m_armed = -1, eb_armed = false;
  // Время сверху уезжает и уменьшается за первые 36 dp прокрутки (ScrollAway: сдвиг 24 dp, до 0,5)
  tt_k = 1 - halo_clamp01((menu.top - menu.scroll) / (36 * DP));
  sc_pos.target = halo_list_progress(menu);
  halo_spring_run(sc_pos, K_M3_DEFAULT, dt);
  if (menu.moving || f.owner == &menu) sc_idle_ms = halo_ms();
  sc_vis.target = halo_ms() - sc_idle_ms < 2000;
  halo_spring_run(sc_vis, K_SC_VIS, dt);
}

static void menu_layout(const HaloView &v) {
  const HaloListFx &h = menu.fx[0];
  if (menu.moving && v.a > 0 && v.s >= 0.999f) halo_subpixel = true;  // лента едет — буквы с долей пикселя, иначе дрожат
  text_mid(m_head, t_title, "Gallery", mid_head, CX, h.top + M_HEAD_H / 2, 1, TEXT, h.a_bg, &halo_black);
  m_head.on_black = true;
  halo_view_commit(m_head, v);
  for (int k = 0; k < N_ENTRIES; k++) {
    const HaloListFx &f = menu.fx[k + 1];
    const Entry &d = ENTRIES[k];
    const bool vis = halo_list_item_visible(menu, k + 1);
    const float s = f.s, h = M_ITEM_H * s, w = M_ITEM_W * s, x0 = CX - w / 2, y0 = f.top;
    const float p = halo_clamp01(m_press[k].s.x), ac = vis ? f.a_c : 0, ix = x0 + 56 * s, iy = y0 + h / 2;
    // Фон: нажатая кнопка — меньше скругление (форма «сжимается»: 26 dp → 18 dp)
    HaloItem &bg = m_bg[k];
    bg.kind = HI_RRECT, bg.x = CX, bg.y = y0 + h / 2, bg.w = M_ITEM_W, bg.h = M_ITEM_H, bg.scale = s;
    bg.r = 52 + (36 - 52) * p, bg.color = CARD_HI, bg.alpha = vis ? f.a_bg : 0, bg.on_black = true;
    // Круг значка — непрозрачный поверх карточки, значок и текст — с прозрачностью содержимого
    HaloItem &dc = m_disc[k];
    dc.kind = HI_CIRCLE, dc.x = ix, dc.y = iy, dc.r = 32, dc.scale = s, dc.color = d.color, dc.alpha = ac;
    dc.on_black = true;
    text_mid(m_icon[k], ic24, d.icon, mid_icon, ix, iy, s, 0x1B1B1F, ac, &dc);
    text_base(m_title[k], t24, d.title, x0 + 104 * s, y0 + 46 * s, s, TEXT, ac, &bg);
    text_base(m_sub[k], t20, d.sub, x0 + 104 * s, y0 + 80 * s, s, DIM, ac, &bg);
    halo_view_commit(bg, v), halo_view_commit(dc, v), halo_view_commit(m_icon[k], v);
    halo_view_commit(m_title[k], v), halo_view_commit(m_sub[k], v);
  }
  // Кнопка у края: фон гаснет ниже 30 dp высоты, надпись — ниже 38 dp; нажатие «сжимает» верхние углы 48 → 32
  const float eh = eb_h.x, ep = halo_clamp01(eb_press.s.x), e_top = CX + EB_R - eh;
  const float e_bg = halo_clamp01((eh - 4 * DP) / (26 * DP)), e_txt = halo_clamp01((eh - 30 * DP) / (8 * DP));
  m_eb.kind = HI_EDGE, m_eb.x = CX, m_eb.y = CX, m_eb.r = EB_R, m_eb.h = eh, m_eb.w = EB_W, m_eb.v = 48 + (32 - 48) * ep;
  m_eb.scale = 1, m_eb.color = CARD_HI, m_eb.alpha = eh > 1 ? e_bg : 0, m_eb.on_black = true;
  text_mid(m_eb_txt, t24, "More", mid_eb, CX, e_top + fminf(eh, EB_FULL) * 0.4f, 1, TEXT, eh > 1 ? e_txt : 0,
           &m_eb);
  halo_view_commit(m_eb, v), halo_view_commit(m_eb_txt, v);
  // Время по дуге сверху: при прокрутке уезжает вверх на 24 dp, уменьшается и гаснет до 0,5 — и пропадает
  // (маска, запечённая раз в минуту; масштаб — от верхней середины)
  const float k = 0.5f + 0.5f * tt_k, top = tt_y - 24 * DP * (1 - tt_k), left = CX - (CX - tt_x) * k;
  m_time.kind = HI_MASK, m_time.mask = &tt_mask, m_time.color = 0xC6C6C6, m_time.on_black = true, m_time.on = &halo_black;
  m_time.x = left + tt_mask.w * k / 2, m_time.y = top + tt_mask.h * k / 2, m_time.scale = k;
  m_time.alpha = tt_k > 0.02f && tt_mask.bits ? k * (1 - halo_clamp01(ui_lock * 2)) : 0;
  halo_view_commit(m_time, v);
  // Индикатор прокрутки справа (ScrollIndicator): дорожка 50 dp по дуге, ползунок 0,3..0,7 её длины, зазоры 3 dp,
  // толщина 6 dp, круглые концы
  const float r = 233 - 2 * DP - 3 * DP, len = 50 * DP / r, gap = (6 + 3) * DP / r;
  const float a0 = HALO_PI / 2 - len / 2, size = len * constrain(halo_list_visible(menu), 0.3f, 0.7f);
  const float t0 = a0 + (len - size) * halo_clamp01(sc_pos.x), t1 = t0 + size, a = sc_vis.x > 0.01f ? sc_vis.x : 0;
  const float from[3] = {a0, t1 + gap, t0}, to[3] = {t0 - gap, a0 + len, t1};  // дорожка до, дорожка после, ползунок
  for (int i = 0; i < 3; i++) {
    HaloItem &it = m_sc[i];
    it.kind = HI_ARC, it.x = CX, it.y = CX, it.r = r, it.w = 6 * DP, it.scale = 1, it.a0 = from[i], it.a1 = to[i];
    it.color = i == 2 ? 0xC6C6C6 : 0x333333, it.on_black = true, it.alpha = from[i] < to[i] ? a : 0;
    halo_view_commit(it, v);
  }
}

static void menu_draw() {
  halo_item_draw(m_head);
  for (int k = 0; k < N_ENTRIES; k++) {
    halo_item_draw(m_bg[k]), halo_item_draw(m_disc[k]), halo_item_draw(m_icon[k]);
    halo_item_draw(m_title[k]), halo_item_draw(m_sub[k]);
  }
  halo_item_draw(m_eb), halo_item_draw(m_eb_txt);
  halo_item_draw(m_time);
  for (auto &it : m_sc) halo_item_draw(it);
}

// Время сверху: сменилась минута — запечь по дуге заново
static void clock_tick() {
  static uint32_t checked;
  if (halo_ms() - checked < 250 || !halo_clock_valid()) return;
  checked = halo_ms();
  const time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_min == clock_m && t.tm_hour == clock_h) return;
  clock_h = t.tm_hour, clock_m = t.tm_min;
  snprintf(time_str, sizeof(time_str), "%d:%02d", clock_h % 12 ? clock_h % 12 : 12, clock_m);  // 12 часов, как в США
  halo_text_arc(tt_mask, t24, time_str, mid_time, TT_R, 0, false, 1, &tt_x, &tt_y);
}

static void menu_build() {
  float heights[N_ENTRIES + 1];
  heights[0] = M_HEAD_H;
  for (int i = 0; i < N_ENTRIES; i++) heights[i + 1] = M_ITEM_H;
  // Низ последней карточки в конце ленты — 333 (было 300: лента и кнопка у края выдвигались слишком далеко; край
  // поставлен там, где его оставил пользователь, — на 33 px ниже), до кнопки — 20 px
  halo_list_init(menu, heights, N_ENTRIES + 1, 64, 333, true);
  mid_head = halo_text_mid(t_title, "Gallery");
  mid_icon = halo_text_mid(ic24, ENTRIES[1].icon);  // все значки — по одной середине (как в WearKit)
  mid_time = halo_text_mid(t24, "0123456789");
  mid_eb = halo_text_mid(t24, "More");
  K_EB = halo_spring(halo_stiff(400), 1);
  halo_text_arc(tt_mask, t24, time_str, mid_time, TT_R, 0, false, 1, &tt_x, &tt_y);
}

// ---------- Заглушки: название по центру ----------
static HaloItem st_title;
static float st_mid;

static const char *st_text;  // название: карточки или кнопки у края

static void stub_enter() {
  st_text = cur < N_ENTRIES ? ENTRIES[cur].title : "More";
  st_mid = halo_text_mid(t_title, st_text);
}
static void stub_layout(const HaloView &v) {
  text_mid(st_title, t_title, st_text, st_mid, CX, CX, 1, TEXT, 1, &halo_black);
  st_title.on_black = true;
  halo_view_commit(st_title, v);
}
static void stub_draw() {
  halo_item_draw(st_title);
}

// ---------- Страницы (пример WearKit «Страницы») ----------
// Пейджер (HorizontalPagerScaffold + AnimatedPage): страница, которая уходит, уменьшается до 0,55 к тому краю, куда
// уходит, и темнеет до 0,5; порог — 10 % (страниц мало) или бросок; доводка — пружина 2000/1. Внизу — точки страниц
// (PageIndicator): 12 px через 20 px по дуге r = 217, неактивные — белые 0,3; выбранная «червяком» тянется к соседней
// прямо за пальцем. Числа — docs/research/notes/dialogs_indicators.md, раздел 7.
//
// Фон страниц — чёрный (в WearKit — тёмный круг 0x15161A на весь экран). Круг на весь экран менялся в каждом кадре
// листания: почти весь экран заново по шине и затемнение каждого пикселя, а тёмный цвет в 16 битах при затемнении
// менял оттенок ступеньками. На чёрном меняются только кружок, значок, число и подпись, и гаснут они сами (тусклым
// цветом на чёрном): кадр листания 12,6 → 2,9 мс, шина 53 → 14 %, 0 разрывов (замеры — docs/halo.md, раздел 7).
#define PG_N 4
static const char *const PG_TITLE[PG_N] = {"Heart rate", "Steps", "Sleep", "Weather"};
static const char *const PG_VALUE[PG_N] = {"72", "6,420", "7:45", "66°"};
static const char *const PG_ICON[PG_N] = {"\uf004", "\uf624", "\uf186", "\uf185"};  // сердце, спидометр, луна, солнце
static const uint32_t PG_COLOR[PG_N] = {0xFF8BCB, 0x5CF08A, 0xC58AF9, 0xFDD663};
static float pg_mid_t, pg_mid_v, pg_mid_i;
static HaloSpring pg;  // страница (дробная — пока листают)
static float pg_g0;
static bool pg_drag;
static HaloItem pg_disc[PG_N], pg_icon[PG_N], pg_value[PG_N], pg_title[PG_N], pg_dot[PG_N], pg_worm, pg_pull;

static void pages_build() {
  pg_mid_t = halo_text_mid(t24, PG_TITLE[0]), pg_mid_v = halo_text_mid(n48, PG_VALUE[2]);  // по цифрам: запятая и градус середину не сдвигают
  pg_mid_i = halo_text_mid(ic32, PG_ICON[0]);
}

static void pages_enter() {
  halo_spring_set(pg, 0);
}

static void pages_update(float dt) {
  HaloFinger &f = halo_finger;
  // Вбок — листать (кроме полосы у левого края: там свайп назад)
  if (f.drag_start && f.horiz && !f.owner && f.x0 > 30 * DP && halo_finger_claim(&pg)) pg_g0 = pg.x, pg_drag = true;
  if (pg_drag && f.owner == &pg) {
    if (f.down) {
      const float raw = pg_g0 - (f.x - f.x0) / W;
      pg.x = raw < 0 ? halo_rubber(raw * W) / W : raw > PG_N - 1 ? PG_N - 1 + halo_rubber((raw - PG_N + 1) * W) / W : raw;
      pg.v = -f.vx / W;
    } else {  // отпустили: дальше 10 % или бросок (400 px/с) — на соседнюю
      const int p = (int)lroundf(pg_g0);
      int to = p;
      const float fr = pg.x - p;
      if (fr > 0.1f || f.vx < -400) to = p + 1;
      if (fr < -0.1f || f.vx > 400) to = p - 1;
      pg.target = constrain(to, 0, PG_N - 1);
      pg_drag = false;
    }
  }
  if (!pg_drag) halo_spring_run(pg, K_PAGER, dt);
}

static void pages_layout(const HaloView &v) {
  const float p = pg.x;
  for (int i = 0; i < PG_N; i++) {
    // Уходящая страница: масштаб 1 → 0,55 к краю, куда уходит, и темнеет до 0,5; f — доля ухода
    const float off = i - p, f = halo_clamp01(fabsf(off)), s = 1 - 0.45f * f;
    const float k = fabsf(off) < 0.999f ? 1 - 0.5f * f : 0;  // яркость (0 — страницы не видно)
    const float pivot = off < 0 ? 0 : W;  // край, к которому уменьшается
    const float cx = pivot + (CX + off * W - pivot) * s;
    auto X = [&](float x) { return cx + (x - CX) * s; };
    auto Y = [&](float y) { return CX + (y - CX) * s; };
    HaloItem &dc = pg_disc[i];  // кружок значка — цвет страницы на 0,25 (на чёрном — просто тусклый цвет)
    dc.kind = HI_CIRCLE, dc.x = X(CX), dc.y = Y(150), dc.r = 46, dc.scale = s, dc.color = PG_COLOR[i];
    dc.alpha = k * 0.25f, dc.on_black = true;
    text_mid(pg_icon[i], ic32, PG_ICON[i], pg_mid_i, X(CX), Y(150), s, PG_COLOR[i], k, &dc);
    text_mid(pg_value[i], n48, PG_VALUE[i], pg_mid_v, X(CX), Y(250), s, TEXT, k, &halo_black);
    text_mid(pg_title[i], t24, PG_TITLE[i], pg_mid_t, X(CX), Y(318), s, DIM, k, &halo_black);
    halo_view_commit(dc, v), halo_view_commit(pg_icon[i], v);
    halo_view_commit(pg_value[i], v), halo_view_commit(pg_title[i], v);
  }
  // Точки по дуге снизу: шаг 20 px на радиусе 217 → 5,28°; выбранная тянется «червяком». Свайп вверх от нижнего края
  // (на циферблат) — точки чуть поднимаются, а выбранная тянется вверх за пальцем капсулой (как на погоде)
  const float P = halo_nav_pull(), lift = 26 * (1 - expf(-P / 140)), stretch = 30 * (1 - expf(-P / 120));
  const float R = 217, step = 20 / R, a0 = HALO_PI + step * (PG_N - 1) / 2;
  for (int i = 0; i < PG_N; i++) {
    const float a = a0 - i * step;
    HaloItem &d = pg_dot[i];  // белая 0,3 на чёрном
    d.kind = HI_CIRCLE, d.x = CX + R * sinf(a), d.y = CX - R * cosf(a) - lift, d.r = 6, d.scale = 1, d.color = 0xFFFFFF;
    d.alpha = 0.3f, d.on_black = true;
    halo_view_commit(d, v);
  }
  const float pc = halo_clamp01(p / (PG_N - 1)) * (PG_N - 1);
  const int i0 = (int)pc;
  const float fr = pc - i0, head = fminf(1, fr * 2), tail = fmaxf(0, fr * 2 - 1);
  const float aa = a0 - (i0 + tail) * step, ab = a0 - (i0 + head) * step;
  const bool pulled = stretch > 0.3f;
  pg_worm.kind = HI_ARC, pg_worm.x = CX, pg_worm.y = CX - lift, pg_worm.r = R, pg_worm.w = 12, pg_worm.scale = 1;
  pg_worm.a0 = fminf(aa, ab), pg_worm.a1 = fmaxf(aa, ab) + 0.0001f, pg_worm.color = 0xFFFFFF;
  pg_worm.alpha = pulled ? 0 : 1, pg_worm.on_black = true;
  halo_view_commit(pg_worm, v);
  const float ac = a0 - lroundf(pc) * step, dx = CX + R * sinf(ac), dy = CX - R * cosf(ac) - lift;
  pg_pull.kind = HI_RRECT, pg_pull.x = dx, pg_pull.y = dy - stretch / 2, pg_pull.w = 12, pg_pull.h = 12 + stretch;
  pg_pull.r = 6, pg_pull.scale = 1, pg_pull.color = 0xFFFFFF, pg_pull.alpha = pulled ? 1 : 0, pg_pull.on_black = true;
  halo_view_commit(pg_pull, v);
}

static void pages_draw() {
  for (int i = 0; i < PG_N; i++) {
    halo_item_draw(pg_disc[i]), halo_item_draw(pg_icon[i]);
    halo_item_draw(pg_value[i]), halo_item_draw(pg_title[i]);
  }
  for (auto &d : pg_dot) halo_item_draw(d);
  halo_item_draw(pg_worm), halo_item_draw(pg_pull);
}

#include "weather.h"  // экран «Weather» (погода по макету design/weather_mockup.webp)
#include "face.h"     // циферблат (по макету design/face_mockup.webp)
#include "display.h"  // экран Display: яркость, кнопка BOOT гасит экран, PWR — блокировка
#include "battery.h"  // экран Battery: уровень, заряд, график за сутки; история — во флеше
#include "music.h"    // музыка: треки в разделе флеша, декодер IMA ADPCM, звук через Halo
#include "player.h"   // экран Music: плеер WearKit на Halo
#include "settings.h" // настройки: разделы (Display) и хранение во флеше

// ---------- Кадр ----------
// Жест на циферблат — под то, как экран движется: листалки с точками (Pages, Weather) — вверх от нижнего края, как
// назад с погоды поверх циферблата; Settings — оба, но только от самого края (вниз — от верхних 90 px, вверх — от
// нижних 70), остальное — прокрутка списка; заглушки — оба; сама галерея — только вниз от верхнего края
static const HaloScreen MENU = {menu_enter, menu_update, menu_layout, menu_draw};
static const HaloScreen PAGES = {pages_enter, pages_update, pages_layout, pages_draw, false, HALO_HOME_UP};
static const HaloScreen STUB = {stub_enter, nullptr, stub_layout, stub_draw, false, HALO_HOME_BOTH};
static const HaloScreen FACE = {nullptr, face_update, face_layout, face_draw};

static void update(float dt) {
  halo_finger_update();  // палец — раз в кадр, в начале
  clock_tick();
  face_tick(dt);  // время, дата и анимации циферблата — в каждом кадре, даже когда он закрыт галереей
  display_tick(dt);     // кнопка BOOT и яркость (плавно) — где бы ни были
  player_tick(dt);      // музыка и анимации плеера — играет и при закрытом экране
  if (cmd_open >= 0) open_req = cmd_open, cmd_open = -1;
  if (open_req >= 0 && halo_nav_at_home()) {  // открыть: переход Android 16
    cur = open_req;
    halo_nav_open(cur == 0 ? PAGES : cur == 2 ? PLAYER : cur == 3 ? WEATHER : cur == 4 ? SETTINGS : STUB);
  }
  open_req = -1;
  halo_nav_update(dt);  // свайпы к циферблату и назад, переходы, update и раскладка экранов
  display_overlay_layout();  // подсказка блокировки — поверх всего
}

static void draw() {  // оба ядра, каждое — свою часть пачки строк
  halo_nav_draw();
  display_overlay_draw();
}

void setup() {
  random_clock();  // случайные дата и время (Wi-Fi нет)
  K_M3_DEFAULT = halo_spring(HALO_M3_DEFAULT);
  K_PRESS = halo_spring(0.222f / 3, 0.70f);  // нажатие: в 3 раза быстрее expressive fast (жёсткость × 9)
  K_RELEASE = halo_spring(HALO_M3X_SLOW);    // отпускание — медленно, с лёгким отскоком
  K_SC_VIS = halo_spring(halo_stiff(400), 1);
  K_PAGER = halo_spring(HALO_M3_PAGER);
  menu_build();
  pages_build();
  weather_build();
  settings_load();  // яркость — из флеша
  face_build();
  display_build();
  battery_load();   // история батареи — из флеша
  battery_build();
  music_begin();    // треки — из раздела флеша (tools/halo_music.py)
  player_build();
  settings_build();
  halo_nav_begin(MENU);  // главный — меню; команда back — назад
  halo_nav_face(FACE);   // под ним — циферблат: с него запуск
  halo_command("open", [](const char *a) { cmd_open = constrain(atoi(a), 0, EB_ID); });
  halo_command("page", [](const char *a) { pg.target = constrain(atoi(a), 0, PG_N - 1); });
  HaloConfig cfg;
  cfg.name = "HaloWatch";
  cfg.brightness = dp_start_level();  // сохранённая яркость (или самая низкая, если заблокировано) — с первого кадра
  halo_begin(update, draw, cfg);
  Serial.println("setup done");
}

void loop() {
  halo_loop();
  settings_save_poll();  // яркость — во флеш (не из задачи кадров)
  battery_poll();        // уровень батареи раз в 5 с, история — во флеш
}
