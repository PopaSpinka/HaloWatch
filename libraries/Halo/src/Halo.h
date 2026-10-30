// Halo — своя библиотека интерфейса для Waveshare ESP32-S3-Touch-AMOLED-1.75C, с нуля и под это железо.
// Без LVGL, Arduino_GFX и библиотек датчиков: свой драйвер экрана CO5300 (QSPI + DMA, сигнал TE), свой драйвер
// тача CST9217, свой движок кадров и рисование. От Arduino — только setup()/loop() и Serial для команд.
//
// Главное отличие от AmoledMotion: на экран уходит только то, что изменилось. У дисплея своя память, а шина —
// узкое место (полный круг — 9,6 мс из 16,7, docs/instruments.md). Скетч сообщает, где изменилось
// (halo_damage), движок перерисовывает и отправляет только пачки строк с изменениями, а если не изменилось
// ничего — кадр не рисуется вовсе.
//
// Кадр (как в AmoledMotion, docs/smooth-motion.md, раздел 17):
//   сигнал TE → пачки по 16 строк с изменениями: каждое ядро рисует свою часть прямо в DMA-буфер → очередь DMA
//   → update(dt) — состояние следующего кадра (ядро 1), пока ждём следующий TE.
//
// Пример:
//   static float x = 100;
//   static void update(float dt) { x += 60 * dt; halo_damage_all(); }
//   static void draw() { halo_circle(x, 233, 30, 0x40C4FF); }
//   void setup() { halo_begin(update, draw); Serial.println("setup done"); }
//   void loop() { halo_loop(); }
#pragma once

#include <Arduino.h>
#include <esp_timer.h>

#pragma GCC push_options
#pragma GCC optimize("O3", "fast-math")

#define HALO_W 466
#define HALO_H 466
#define HALO_ROWS 16                                // строк в пачке
#define HALO_BANDS ((HALO_H + HALO_ROWS - 1) / HALO_ROWS)
#define HALO_HOT IRAM_ATTR __attribute__((noinline))  // горячий код рисования — во внутренней памяти

// ---------- Запуск ----------
struct HaloConfig {
  const char *name = "halo";  // "<name> start" в журнале и начало строки скорости
  uint8_t brightness = 180;   // 0..255
  uint32_t bus_hz = 80000000; // шина дисплея: 80 МГц — круг за 9,6 мс
  uint8_t buffers = 3;        // DMA-буферов по 15 КБ внутренней памяти
  bool two_cores = true;      // широкие пачки рисуют два ядра
  bool touch = true;
  uint16_t report_ms = 5000;  // строка скорости в журнал (0 — не писать)
};
typedef void (*HaloUpdate)(float dt);  // ядро 1, раз в кадр: анимации, палец → halo_damage
typedef void (*HaloDraw)();            // оба ядра, на каждую пачку с изменениями: нарисовать всё, что её задевает
bool halo_begin(HaloUpdate update, HaloDraw draw, const HaloConfig &cfg = HaloConfig());
void halo_loop();  // в loop(): команды из порта, журнал, приборы
void halo_brightness(uint8_t level);
// Экран: погасить панель (команда 0x28) — плата работает дальше, кадры рисуются в память панели, касания не
// берутся; включить — снова видна (0x29). Яркость плавно — halo_brightness в каждом кадре
void halo_display_on(bool on);
bool halo_display_is_on();
bool halo_boot_click();  // кнопку BOOT (GPIO0) нажали — в этом кадре, раз на нажатие (команда boot — то же из порта)
// Кнопка PWR — её читает чип питания AXP2101 (опрос раз в 100 мс): что было с прошлого вызова — биты HALO_PWR_*.
// Короткое — после отпускания; долгое — через ~1,5 с, пока ещё держат (держать 6–10 с — плата выключится аппаратно).
// Команды pwr и pwr long — то же из порта. Нужен тач (cfg.touch): чип на его шине
enum : uint8_t { HALO_PWR_CLICK = 1, HALO_PWR_LONG = 2 };
uint8_t halo_pwr_event();
// Батарея — от того же чипа питания, раз в 2 с (из любой задачи; команда battery — в журнал)
struct HaloBattery {
  bool ok;          // чип питания ответил (ok = false — данных ещё нет)
  bool present;     // батарея подключена
  bool usb;         // питание от USB
  bool charging;    // заряжается
  int8_t percent;   // 0..100 («топливомер» чипа), −1 — нет батареи
  uint16_t mv;      // напряжение батареи
};
HaloBattery halo_battery();
// Звук: динамик (кодек ES8311 + усилитель). fill(buf, n) зовёт задача звука (ядро 0): положить n отсчётов моно 16 бит,
// вернуть сколько положил (меньше n — дальше тишина; тишина дольше 150 мс — усилитель выключен). Частота — для
// MCLK = 256·fs (16 000 … 48 000). Нужен тач (cfg.touch): кодек на его шине. Начинать, когда звук понадобился:
// I2S берёт ~6 КБ внутренней памяти
typedef int (*HaloAudioFill)(int16_t *buf, int n);
bool halo_audio_begin(HaloAudioFill fill, int sample_rate = 22050);
void halo_audio_volume(int v);  // 0..100 (громкость кодека; 60 — как в AmoledAudio)
int halo_audio_get_volume();
// Касания: выключить (блокировка) — палец не виден никому; halo_finger_blocked() — коснулись, пока выключены (раз на
// касание: показать подсказку)
void halo_touch_on(bool on);
bool halo_touch_is_on();
bool halo_finger_blocked();

// ---------- Что изменилось ----------
// Область (включительно) перерисуется в следующем кадре. Звать из update (или из команд — тоже можно).
// Если в кадре изменений нет — он не рисуется и ничего не уходит на шину.
void halo_damage(int x0, int y0, int x1, int y1);
void halo_damage_f(float x0, float y0, float x1, float y1);  // дробные края — с запасом в пиксель
void halo_damage_all();

// ---------- Пачка, которую сейчас рисует эта задача ----------
struct HaloBand {
  uint16_t *buf;  // DMA-буфер в формате дисплея
  int y, rows;    // строки пачки
  int x0, x1;     // столбцы, которые рисует ЭТА задача (только их и трогать)
  int bx, w;      // первый столбец буфера и его ширина
};
extern __thread HaloBand hb;
static inline uint16_t *halo_row(int y) {
  return hb.buf + (y - hb.y) * hb.w - hb.bx;
}
static inline bool halo_hits(float x0, float y0, float x1, float y1) {  // задевает ли фигура свою часть пачки
  return y1 >= hb.y && y0 < hb.y + hb.rows && x1 >= hb.x0 && x0 <= hb.x1;
}

// ---------- Цвет: RGB565 в порядке дисплея (старший байт первым) ----------
static inline uint16_t halo_565(uint32_t rgb) {
  const uint16_t c = ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
  return __builtin_bswap16(c);
}
// Цвет × яркость k (0..256) — «на чёрном» это и есть прозрачность
static inline uint32_t halo_dim(uint32_t rgb, int k) {
  return (((rgb >> 16 & 255) * k >> 8) << 16) | (((rgb >> 8 & 255) * k >> 8) << 8) | ((rgb & 255) * k >> 8);
}
// Смешать пиксель с цветом (r5, g6, b5 уже в 565) с прозрачностью a 0..256
static inline void halo_blend(uint16_t *p, int r5, int g6, int b5, int a) {
  const uint32_t v = __builtin_bswap16(*p);
  int R = v >> 11, G = (v >> 5) & 63, B = v & 31;
  R += ((r5 - R) * a + 128) >> 8;
  G += ((g6 - G) * a + 128) >> 8;
  B += ((b5 - B) * a + 128) >> 8;
  *p = __builtin_bswap16((R << 11) | (G << 5) | B);
}
// Залить x0..x1 строки одним цветом — по два пикселя за запись
static inline void halo_span(uint16_t *row, int x0, int x1, uint16_t c) {
  if (x0 > x1) return;
  uint16_t *p = row + x0, *e = row + x1 + 1;
  if ((uintptr_t)p & 2) *p++ = c;
  const uint32_t cc = c | ((uint32_t)c << 16);
  uint32_t *q = (uint32_t *)p;
  int n = (e - p) >> 1;  // слов по два пикселя; по 8 слов за проход цикла
  for (; n >= 8; n -= 8, q += 8) q[0] = cc, q[1] = cc, q[2] = cc, q[3] = cc, q[4] = cc, q[5] = cc, q[6] = cc, q[7] = cc;
  for (; n > 0; n--) *q++ = cc;
  if ((uint16_t *)q < e) *(uint16_t *)q = c;
}

// ---------- Рисование (только внутри draw) ----------
// Цвета — 0xRRGGBB, alpha — 0..1. Края фигур сглажены.
void halo_rect(int x0, int y0, int x1, int y1, uint32_t rgb);                                   // непрозрачный
void halo_rrect(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float alpha = 1);  // скруглённый
void halo_circle(float cx, float cy, float r, uint32_t rgb, float alpha = 1);
// Кольцо радиуса r (по середине толщины) толщиной w: дуга от 12 часов по часовой на долю frac (0..1) цветом rgb,
// остальное — дорожка track (0 — без дорожки); круглые концы дуги
void halo_ring(float cx, float cy, float r, float w, float frac, uint32_t rgb, uint32_t track = 0, float alpha = 1);
// Дуга с круглыми концами от угла a0 до a1 (радианы от 12 часов по часовой, a0 < a1) по окружности радиуса r,
// толщина 2·hw: индикатор прокрутки у края, «червяк» точек страниц
void halo_arc(float cx, float cy, float r, float hw, float a0, float a1, uint32_t rgb, float alpha = 1);
// Кнопка у края (EdgeButton): плашка шириной 2·hw от строки top со скруглёнными верхними углами rc, а бока и низ — по
// эллипсу с центром (cx, cy): по вертикали R (низ — по кругу экрана), по горизонтали R − pad (бока — с отступом pad);
// стык плашки с эллипсом скруглён радиусом f
void halo_edge(float cx, float cy, float R, float top, float hw, float rc, float pad, float f, uint32_t rgb,
               float alpha = 1);
// Всё, что уже нарисовано в пачке вне круга, — чёрным (край сглажен): экран, уменьшенный в переходе, остаётся круглым
void halo_clip_circle(float cx, float cy, float r);
// Затемнить в k раз (0..1) всё, что уже нарисовано внутри круга (край сглажен): так гаснет целый слой — страница при
// листании. Рисуй слой в полную яркость, потом затемняй: фон, значки и текст темнеют одним умножением на пиксель и
// с одним порогом узора 4×4 — вместе и одинаково, без скачков оттенка (а не каждый своим цветом и в свой момент)
void halo_fade_circle(float cx, float cy, float r, float k);

// ---------- Буквы и значки (ресурсы готовит tools/halo_assets.py) ----------
// Маска: 4 бита на пиксель (0..15), строка — (w+1)/2 байт, левый пиксель — старшие 4 бита. Буква шрифта и
// одноцветный значок — одно и то же (значок красится при рисовании). ext — у каждой строки первый и последний
// непустой столбец: пустое не перебираем. Пиксели доступны по адресу — поэтому маску можно плавно масштабировать.
struct HaloMask {
  int16_t w, h;
  int16_t ox, oy, adv;  // для букв: сдвиг от пера (oy — от верха строки), шаг пера
  const uint8_t *bits;
  const uint8_t *ext;   // 2 байта на строку; первый > последнего — строка пустая
};
struct HaloFont {
  int16_t size, line, ascent;  // кегль, высота строки, от верха строки до базовой линии
  uint16_t count;
  const uint32_t *codes;       // коды букв по возрастанию
  const HaloMask *glyphs;
};
// Цветной значок (иконка погоды): цвет уже умножен на прозрачность («как на чёрном»), RGB565 в порядке дисплея,
// плюс прозрачность 4 бита — чтобы рисовать поверх любого фона
struct HaloImage {
  int16_t w, h;
  const uint16_t *px;
  const uint8_t *alpha;  // как bits у маски
  const uint8_t *ext;
};
// Ширина строки текста (UTF-8), пиксели
const HaloMask *halo_glyph(const HaloFont &f, uint32_t code);  // буква шрифта (nullptr — нет такой)
uint32_t halo_utf8_next(const char *&s);                       // следующая буква строки UTF-8
int halo_text_width(const HaloFont &f, const char *s, int spacing = 0);
// Текст: (x, y) — левый верх строки; align: 0 — от x, 1 — по центру x, 2 — до x. scale ≠ 1 — с масштабом.
// bg ≥ 0 — под текстом сплошной цвет bg (0xRRGGBB): края букв — готовыми цветами, без смешивания (в разы быстрее)
void halo_text(const HaloFont &f, const char *s, float x, float y, uint32_t rgb, float alpha = 1, int align = 0,
               float scale = 1, int spacing = 0, int32_t bg = -1);
void halo_mask(const HaloMask &m, float x, float y, uint32_t rgb, float alpha = 1, float scale = 1, int32_t bg = -1);
void halo_image(const HaloImage &im, float x, float y, float alpha = 1, float scale = 1);
// Середина букв строки по высоте — от верха строки шрифта. Центр текста по буквам, а не по строке шрифта:
// середина букв в cy → верх строки = cy − mid·scale (так ставит текст AmoledWear: text_center)
float halo_text_mid(const HaloFont &f, const char *s);
// Буквы и маски в масштабе 1 — с долей пикселя по высоте, а не по целым строкам. Пока лента едет вверх-вниз, иначе
// буквы дрожат на полпикселя относительно плашек (у них края дробные). Сбрасывается перед каждым update; кому нужно —
// ставит true в update (не «= условие»: другие экраны кадра тоже могут его ставить), действует на весь кадр
extern bool halo_subpixel;
// Строка по дуге у края экрана (время сверху, как на Wear OS) — в маску. Делается, когда текст меняется (это
// дорого: каждая буква с поворотом), а рисуется обычной маской — halo_mask или элемент HI_MASK. mid — середина
// букв (halo_text_mid), rad — радиус, на котором она лежит, angle — угол середины строки (0 — 12 часов, по
// часовой), bottom — снизу экрана (читается слева направо, буквы смотрят верхом к центру). *x, *y — где на экране
// левый верхний угол маски. Прошлую маску m (если её сделала эта же функция) освобождает
bool halo_text_arc(HaloMask &m, const HaloFont &f, const char *s, float mid, float rad, float angle, bool bottom = false,
                   int spacing = 0, float *x = nullptr, float *y = nullptr);

// ---------- Тач ----------
struct HaloTouchSample {
  int16_t x, y;
  uint32_t t_us;  // время прерывания тача
};
// Последние отчёты, новые первыми; 0 — пальца нет. Можно звать из любой задачи
int halo_touch_samples(HaloTouchSample *out, int max);

// ---------- Палец: касание, перетаскивание, касание-«тап», откуда начался жест ----------
// halo_finger_update() — раз в кадр в начале update. Искусственный палец для проверки без рук (команды в порт):
//   tap X Y | press X Y [мс] | swipe up|down|left|right [edge] | drag X0 Y0 X1 Y1 [мс]
//   fling X Y СКОРОСТЬ [ПУТЬ] [up|down|left|right] — бросок: палец идёт ровно и отпускает на ходу (px/с)
// edge — жест от края экрана (шторка сверху, список снизу). Палец идёт по часам анимаций: в паузе — по шагам.
enum : uint8_t { HALO_EDGE_NONE, HALO_EDGE_TOP, HALO_EDGE_BOTTOM, HALO_EDGE_LEFT, HALO_EDGE_RIGHT };
struct HaloFinger {
  bool down;            // палец на экране
  bool pressed;         // коснулся в этом кадре
  bool released;        // отпустил в этом кадре
  bool tap;             // отпустил быстро и почти не сдвинув
  bool dragging;        // сдвинул дальше порога (10 px) — это жест, а не нажатие
  bool drag_start;      // в этом кадре стал жестом: направление решено (здесь его и забирают — halo_finger_claim)
  bool horiz;           // жест скорее по горизонтали (решается в момент начала жеста)
  uint8_t edge;         // откуда начался: HALO_EDGE_…
  float x, y, x0, y0;   // сейчас и где коснулся
  float vx, vy;         // скорость, px/с (по последним 60 мс)
  uint32_t t0;          // когда коснулся (halo_ms)
  const void *owner;    // кто забрал жест (halo_finger_claim) — остальные его не трогают
};
extern HaloFinger halo_finger;
void halo_finger_update();
static inline bool halo_finger_claim(const void *who) {  // забрать жест; true — он твой
  if (!halo_finger.owner) halo_finger.owner = who;
  return halo_finger.owner == who;
}

// ---------- Пружины (точные: шаг на кадр — матрица 2×2, как в Compose для Wear OS) ----------
// response — за сколько секунд в основном доходит до цели; damping < 1 — с перелётом, 1 — без.
// Меняешь target — движение продолжается с текущей скоростью (анимацию можно перехватить на середине).
struct HaloSpring {
  float x, v, target;
};
struct HaloSpringK {
  float m00, m01, m10, m11;
};
HaloSpringK halo_spring(float response, float damping);
void halo_spring_run(HaloSpring &s, const HaloSpringK &k, float dt);  // у цели — ставит точно в цель
static inline void halo_spring_set(HaloSpring &s, float v) {
  s.x = s.target = v, s.v = 0;
}
// Пружины Material 3 (androidx MotionScheme): положение и размер — без перелёта, expressive — с лёгким перелётом
#define HALO_M3_DEFAULT 0.281f, 1.0f
#define HALO_M3_FAST 0.168f, 1.0f
#define HALO_M3_SLOW 0.390f, 1.0f
#define HALO_M3X_DEFAULT 0.336f, 0.75f
#define HALO_M3X_FAST 0.222f, 0.70f
#define HALO_M3X_SLOW 0.444f, 0.80f
#define HALO_M3_PAGER 0.1405f, 1.0f  // доводка страниц (Horologist, жёсткость 2000)
// Жёсткость Compose (stiffness) → response
static inline float halo_stiff(float k) {
  return 2 * 3.14159265f / sqrtf(k);
}

// ---------- Кривые (cubic-bezier, как в CSS и Compose) ----------
// Таблица из 65 точек считается один раз, в кадре — две соседние точки и смешивание
struct HaloEase {
  float v[65];
};
void halo_ease_make(HaloEase &e, float x1, float y1, float x2, float y2);
static inline float halo_ease(const HaloEase &e, float t) {
  t = (t < 0 ? 0 : t > 1 ? 1 : t) * 64;
  const int i = (int)t;
  return i >= 64 ? e.v[64] : e.v[i] + (e.v[i + 1] - e.v[i]) * (t - i);
}
// «Резина» за краем: сдвиг пальца за край d → видимый сдвиг (чем дальше, тем туже, как в iOS)
float halo_rubber(float d, float k = 0.55f);

// ---------- Элементы: сами замечают, что изменились, и просят перерисовать старое и новое место ----------
// В update поставь поля и вызови halo_item_commit; в draw — halo_item_draw. alpha = 0 — элемент скрыт.
enum : uint8_t { HI_TEXT, HI_MASK, HI_IMAGE, HI_RRECT, HI_CIRCLE, HI_RING, HI_ARC, HI_EDGE };
struct HaloItem {
  uint8_t kind = HI_TEXT;
  float x = 0, y = 0;            // центр; у текста x — по align (0 — левый край, 1 — центр, 2 — правый), y — середина строки
  float scale = 1, alpha = 1;
  uint32_t color = 0xFFF8EE;
  uint32_t color2 = 0;           // кольцо: дорожка
  const HaloFont *font = nullptr;
  const char *text = nullptr;    // строка скетча: изменилось содержимое — элемент заметит
  int8_t align = 1, spacing = 0;
  const HaloMask *mask = nullptr;
  const HaloImage *image = nullptr;
  float w = 0, h = 0, r = 0, v = 0;  // плашка: w×h, углы r; круг: r; кольцо: r, w — толщина, v — доля дуги
  float a0 = 0, a1 = 0;              // дуга (HI_ARC): r, w — толщина, от угла a0 до a1 (радианы от 12 часов)
  // Кнопка у края (HI_EDGE, EdgeButton Wear OS): (x, y) — центр круга экрана, r — радиус, по которому идёт низ кнопки;
  // h — высота кнопки от низа этого круга, w — ширина вверху, v — скругление верхних углов; бока — по изгибу экрана с
  // отступом 20 px (эллипс), низ — плавной дугой, стык верха с боками скруглён на 24 px. Масштаб — от центра круга
  // Элемент лежит на чёрном (или на элементе, который сам на чёрном гаснет так же): прозрачность рисуется
  // тусклым цветом, а не смешиванием — на чёрном это одно и то же, но в 5 раз дешевле (42 такта на пиксель → 7)
  bool on_black = false;
  // Текст или маска целиком лежит на этом элементе — сплошной плашке или круге с on_black (или на чёрном экране:
  // &halo_black): края букв — 16 готовыми цветами поверх его цвета, без смешивания на пиксель (текст в разы дешевле)
  const HaloItem *on = nullptr;
  // служебное
  float b[4] = {0, 0, -1, -1};
  uint32_t hash = 0;
  bool shown = false;
};
void halo_item_commit(HaloItem &it);
void halo_item_draw(const HaloItem &it);
extern const HaloItem halo_black;  // «чёрный экран» для HaloItem.on

// ---------- Страницы вбок с точками внизу (как в галерее WearKit) ----------
struct HaloPager {
  int count = 1, page = 0;
  HaloSpring off = {0, 0, 0};  // смещение в страницах: 0 — первая
  bool drag = false;
  HaloItem dots[8];
};
void halo_pager_begin(HaloPager &p, int count);
void halo_pager_update(HaloPager &p, float dt, bool enabled = true);  // палец (горизонтальный жест) + пружина
static inline float halo_pager_x(const HaloPager &p, int i) {  // сдвиг страницы i по x
  return (i - p.off.x) * HALO_W;
}
static inline bool halo_pager_visible(const HaloPager &p, int i) {
  return fabsf(i - p.off.x) < 0.999f;
}
void halo_pager_go(HaloPager &p, int page);
void halo_pager_draw(const HaloPager &p);  // точки

// ---------- Лента карточек Wear OS (TransformingLazyColumn) — как в AmoledWear (projects/WearKit) ----------
// Палец листает ленту один к одному, отпустил — инерция; за краями — «резина» и пружина обратно. Раскладка — от
// «якоря» (элемента у середины экрана): он идёт ровно за пальцем, остальные пристраиваются к нему уже уменьшенными —
// у краёв круга карточки уменьшаются до 0,6 и бледнеют. Пример — projects/HaloWatch.
//   halo_list_init(menu, heights, n, 64, 300, true);   // при запуске: высоты, верх в покое, низ последнего
//   в update: halo_list_update(menu, dt);  for (i…) if (halo_list_item_visible(menu, i)) — menu.fx[i]: где и как
#define HALO_LIST_MAX 24
struct HaloListFx {
  float top, s, a_bg, a_c;  // верх на экране, масштаб, прозрачность фона и содержимого
};
struct HaloList {
  int n;
  float h[HALO_LIST_MAX], cum[HALO_LIST_MAX], total;
  float top, bottom;  // верх первого элемента в покое; низ последнего — не выше bottom
  float gap, edge;    // промежуток (8 px) и масштаб у края (0,6)
  bool header;        // элемент 0 — заголовок: не уменьшается, бледнеет только у нижнего края
  float scroll;       // верх первого элемента (до раскладки)
  float fling_v;      // px/с
  bool flinging, back_on, moving;
  HaloSpring back;    // к краю: подкатиться, отскочить или вернуться из-за края
  HaloSpringK back_k; // её жёсткость — своя на каждый раз (зависит от скорости)
  float g_start, g_ref, last;
  HaloListFx fx[HALO_LIST_MAX];
};
void halo_list_init(HaloList &l, const float *heights, int n, float top, float bottom, bool header = false);
void halo_list_reset(HaloList &l);  // в начало и на месте (без инерции и доводки), раскладка — сразу
// В update после halo_finger_update. take_touch — можно ли ленте забирать вертикальный жест
void halo_list_update(HaloList &l, float dt, bool take_touch = true);
static inline float halo_list_min(const HaloList &l) {
  return l.bottom - l.total;
}
float halo_list_progress(const HaloList &l);  // где лента: 0 — в начале, 1 — в конце (индикатор прокрутки)
float halo_list_visible(const HaloList &l);   // какая доля ленты видна
static inline bool halo_list_item_visible(const HaloList &l, int i) {
  return l.h[i] > 0.5f && l.fx[i].a_bg > 0.004f && l.fx[i].top < HALO_H && l.fx[i].top + l.h[i] * l.fx[i].s > 0;
}
int halo_list_hit(const HaloList &l, float y);  // какой элемент под точкой экрана y (−1 — никакой)

// ---------- Текст, буквы которого сменяются прокруткой (цифры часов, дата, температура) ----------
// Сменилась буква — старая уезжает вверх и гаснет, новая поднимается снизу и проявляется (пружина с лёгким перелётом),
// несколько сменившихся — каскадом справа налево (как в механическом счётчике). Буквы разной ширины: соседи
// переезжают на новые места пружиной, ширина строки (для плашки под текстом) — тоже. Каждая буква — свой элемент:
// на шину уходят только меняющиеся. Пример — циферблат projects/HaloWatch (face.h).
//   halo_roll_init(clock, f_time, "0123456789");   при запуске (середина букв — по этой строке)
//   halo_roll_set(clock, "09:42");                 сменилось — анимация (animate = false — сразу)
//   в update: halo_roll_update(clock, dt);  halo_roll_layout(clock, 233, 219, 0xFFFFFF, 1, &halo_black, v);
//   в draw: halo_roll_draw(clock);
#define HALO_ROLL_MAX 16
struct HaloView;
struct HaloRollCell {
  uint32_t code, old;  // буква сейчас и та, что уходит (0 — нет)
  HaloSpring x;        // левый край буквы от точки привязки, px
  HaloSpring t;        // смена: 0 → 1
  float wait;          // сколько ещё ждать до начала смены (каскад), с
  HaloItem in, out;
};
struct HaloRoll {
  const HaloFont *font = nullptr;
  int8_t align = 1, spacing = 0;  // 0 — от x, 1 — по центру, 2 — до x
  float mid = 0;                  // середина букв от верха строки (halo_text_mid)
  float rise = 0;                 // на сколько px уезжают буквы (0 — на 0,4 высоты цифры)
  float stagger = 0.07f;          // задержка между буквами каскада, с (весь каскад — не дольше 0,3 с)
  // Табличные цифры (ставить до halo_roll_init): у каждой цифры ячейка одной ширины (самой широкой цифры шрифта), буква —
  // по середине своей ячейки (и не-цифры — по середине своей). «11:11» и «08:58» тогда одной ширины, ничего не
  // сдвигается при смене цифр, а у «ЧЧ:ММ» по центру двоеточие стоит ровно в точке привязки
  bool tabular = false;
  float cell = 0;                 // ширина ячейки цифры (считает halo_roll_init)
  int n = 0;
  HaloSpring w;                   // ширина строки (едет вместе с буквами)
  HaloRollCell c[HALO_ROLL_MAX];
};
void halo_roll_init(HaloRoll &r, const HaloFont &f, const char *mid_ref, int align = 1, int spacing = 0);
void halo_roll_set(HaloRoll &r, const char *s, bool animate = true);
void halo_roll_update(HaloRoll &r, float dt);
// (x, y): x — по align, y — середина букв. on — на чём лежит текст: &halo_black или сплошная плашка с on_black
// (тогда буква гаснет к цвету плашки, а не к чёрному). Через вид экрана v — как halo_view_commit
void halo_roll_layout(HaloRoll &r, float x, float y, uint32_t color, float alpha, const HaloItem *on, const HaloView &v);
void halo_roll_draw(const HaloRoll &r);
static inline float halo_roll_width(const HaloRoll &r) {  // ширина строки сейчас (в анимации — промежуточная)
  return r.w.x;
}
bool halo_roll_busy(const HaloRoll &r);  // идёт смена или переезд
// Что нарисует строка, когда смена закончится: «краска» от x0 до x1 относительно точки привязки (x в halo_roll_layout);
// false — пусто. Чтобы ставить соседей по настоящей ширине цифр, а не по месту из макета. from, to — только буквы с
// этими номерами (например, цифры без знака градуса)
bool halo_roll_ink(const HaloRoll &r, float &x0, float &x1, int from = 0, int to = HALO_ROLL_MAX);

// ---------- Навигация (Wear OS 6): главный экран и экран, открытый поверх него ----------
// Открыть — переход Android 16 (новый въезжает справа и вырастает с 0,8, главный уменьшается до 0,85, уходит влево
// и гаснет до 0,6); назад — свайп от левого края за пальцем (и команда back). Числа — как в AmoledWear (wear_nav.cpp,
// docs/wear-motion.md). Снимков экрана нет: оба экрана рисуются вживую — каждый ставит свои элементы через «вид»
// (сдвиг, масштаб от середины, яркость), и переход начинается в том же кадре.
//   static void menu_layout(const HaloView &v) { title.x = 233, title.y = 100; …; halo_view_commit(title, v); }
//   halo_nav_begin({nullptr, menu_update, menu_layout, menu_draw});   в update: halo_nav_update(dt);  в draw: halo_nav_draw();
//   в menu_update: if (нажали пункт) halo_nav_open({detail_enter, detail_update, detail_layout, detail_draw});
struct HaloView {
  float x = 0, y = 0, s = 1, a = 1;  // сдвиг, масштаб от середины экрана, яркость (0 — экран не виден)
  bool over = false;                 // экран поверх другого: при a < 1 — смешиванием (под ним не чёрный)
};
static inline float halo_view_x(const HaloView &v, float x) {
  return HALO_W / 2.0f + v.x + (x - HALO_W / 2.0f) * v.s;
}
static inline float halo_view_y(const HaloView &v, float y) {
  return HALO_H / 2.0f + v.y + (y - HALO_H / 2.0f) * v.s;
}
// Элемент, поставленный как на экране в покое, — через вид экрана, и halo_item_commit. Вид меняет поля самого
// элемента: x, y, scale, alpha (и on_black, on) ставь заново в КАЖДОМ кадре, даже если они постоянные (scale = 1),
// иначе масштаб переходов накопится
void halo_view_commit(HaloItem &it, const HaloView &v);
struct HaloScreen {
  void (*enter)();                    // открыли (палец этого кадра — не ему)
  void (*update)(float dt);           // палец и пружины — только пока экран на месте (в переходе он замирает)
  void (*layout)(const HaloView &v);  // в каждом кадре: поставить свои элементы через вид (a = 0 — спрятать)
  void (*draw)();                     // halo_item_draw своих элементов
  // Экран сам закрывает свой круг непрозрачным фоном (как страницы в HaloWatch): чёрный круг под ним, пока он
  // проявляется, не нужен — полупрозрачный фон экрана и так притушит главный под собой
  bool opaque;
  // Жест с этого экрана (главного или открытого на нём) на циферблат (halo_nav_face) — под то, как экран движется:
  // HALO_HOME_*; 0 — свайп вниз от верхнего края
  uint8_t home;
};
enum : uint8_t {
  HALO_HOME_DOWN = 1,  // свайп вниз от верхнего края: всё уходит вниз (списки — как у ленты галереи)
  HALO_HOME_UP = 2,    // свайп вверх от нижнего края: всё уходит вверх, точки страниц тянутся (листалки — как погода)
  HALO_HOME_BOTH = 3,  // оба; у списка — только от самого края (сверху 90 px, снизу 70), остальное — прокрутка
};
void halo_nav_begin(const HaloScreen &home);
// Циферблат под всем: запуск — с него; переходы между ним и главным — Fade за пальцем. Свайп вверх по циферблату —
// к главному; с главного и с открытого на нём экрана — его жест (HaloScreen::home: вниз от верхнего края, начат выше
// zone px, или вверх от нижнего, начат ниже H − 70, или оба). Пришли на циферблат — открытый
// экран закрывается, а у главного зовётся enter: в следующий раз он открывается с начала (назад жестом слева главный,
// наоборот, остаётся, где был). Команда face — на циферблат, face off — на главный
void halo_nav_face(const HaloScreen &face, float zone = 90);
bool halo_nav_on_face();  // виден циферблат (и не едет)
void halo_nav_to_face(bool face);  // на циферблат / на главный экран (как команды face, face off; с главного или открытого на нём)
// Экран прямо поверх циферблата (нажали на его элемент — плашку погоды): переход fx из элемента с центром (x, y) и
// размером w×h. Назад — свайп вверх от нижнего края (начат ниже H − 70): переход идёт за пальцем, отпустили на трети
// пути или бросок вверх — закрыть, иначе обратно (и команда back). Галерея в это время не выезжает
enum : uint8_t {
  HALO_FX_ZOOM,   // экран вырастает из элемента, циферблат «наезжает» на элемент и гаснет (как приложение из значка)
  HALO_FX_FADE,   // Material fade through: циферблат гаснет, потом экран проявляется, вырастая с 0,92 — дешевле всех
  HALO_FX_SLIDE,  // всё едет вверх: экран снизу, циферблат вверх; назад — экран уходит вверх, циферблат снизу
  HALO_FX_COUNT
};
extern const char *const HALO_FX_NAME[HALO_FX_COUNT];
void halo_nav_face_open(const HaloScreen &screen, uint8_t fx, float x, float y, float w, float h);
float halo_nav_pull();  // жест назад снизу (и на циферблат снизу): на сколько px тянут (после — пружиной к 0); 0 — не тянут
// С главного или с открытого на нём (стопка до 4 уровней); главный ещё доезжал к месту — сразу на место.
// Назад (жест слева, halo_nav_back) — на уровень ниже
void halo_nav_open(const HaloScreen &screen);
void halo_nav_back();
void halo_nav_back_zone(float x);  // свайп назад — если начат левее x (по умолчанию 60 px = 30 dp, как в Android 16)
void halo_nav_update(float dt);    // в update после halo_finger_update
void halo_nav_draw();              // в draw
bool halo_nav_at_home();  // главный на месте (с циферблатом — и пока доезжает, отпущенный): можно открыть экран

// ---------- Время анимаций (виртуальное в паузе и по шагам) ----------
// Идёт вместе с кадрами. Всё, что меряет время жестов и анимаций, — отсюда: тогда pause/step показывают любой
// момент точно. Команды: pause, step N, resume, clock fixed|real
uint32_t halo_us();
uint32_t halo_ms();

// ---------- Команды, журнал ----------
void halo_command(const char *name, void (*fn)(const char *args));
extern volatile bool halo_quiet;  // идёт выдача данных в порт — журнал молчит
void halo_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) halo_logf(__VA_ARGS__)
void halo_serial_binary(bool on);  // своя выдача данных: журнал молчит, порт ждёт читателя

// ---------- Приборы: трасса (формат — как у AmoledBoard, читает tools/trace.py) ----------
enum : uint8_t {
  TR_TE = 1, TR_SCAN, TR_DMA, TR_WIN, TR_BLIT_WAIT, TR_TOUCH, TR_TOUCH_READ, TR_LVGL, TR_LVGL_FLUSH, TR_CMD,
  TR_HEAP = 12, TR_HEAP_BLOCK, TR_PSRAM, TR_LOAD0, TR_LOAD1, TR_TEMP,
  TR_FRAME = 20, TR_PREPARE, TR_BAND, TR_WAIT_HALF, TR_FINISH, TR_REC, TR_STEP,
  TR_USER = 40, TR_LAST = 63,
};
extern volatile bool halo_trace_on;
void halo_trace_put(uint8_t ev, uint32_t t, uint16_t a, uint16_t b);
// Всегда встраиваются: их зовут и прерывания вывода (из IRAM). Отдельная копия, которую компилятор кладёт во флеш,
// роняла плату, когда прерывание приходило во время записи во флеш с другого ядра (Wi-Fi, настройки): «Cache disabled
// but cached memory region accessed» — при включённой трассе
#define HALO_INLINE static inline __attribute__((always_inline))
HALO_INLINE uint32_t trace_now() {
  return (uint32_t)esp_timer_get_time();
}
HALO_INLINE void trace_mark(uint8_t ev, uint16_t a = 0, uint16_t b = 0) {
  if (halo_trace_on) halo_trace_put(ev, trace_now(), a, b);
}
HALO_INLINE void trace_span(uint8_t ev, uint32_t t0, uint16_t b = 0, uint32_t t1 = 0) {
  if (!halo_trace_on) return;
  const uint32_t d = (t1 ? t1 : trace_now()) - t0;
  halo_trace_put(ev, t0, d > 65535 ? 65535 : d, b);
}
HALO_INLINE void trace_count(uint8_t ev, uint16_t v) {
  if (halo_trace_on) halo_trace_put(ev, trace_now(), v, 0);
}
void halo_trace_name(uint8_t ev, const char *name, char kind);
void halo_trace_pump();

// ---------- Быстрые замены медленного на ESP32-S3 ----------
#define HALO_PI 3.14159265f
static inline float halo_sqrt(float x) {  // 1/√x трюком с битами + 2 шага Ньютона: ~20 тактов вместо ~300
  if (x <= 0) return 0;
  union {
    float f;
    uint32_t i;
  } u = {x};
  u.i = 0x5f3759df - (u.i >> 1);
  float y = u.f;
  y = y * (1.5f - 0.5f * x * y * y);
  y = y * (1.5f - 0.5f * x * y * y);
  return x * y;
}
static inline int halo_floor(float x) {
  const int i = (int)x;
  return i - (x < i);
}
static inline float halo_clamp01(float v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

#pragma GCC pop_options
