// Ядро Halo: журнал, команды из порта, трасса событий (бортовой самописец), команда sys, отправка картинок.
// Трасса и выдача данных — в тех же форматах, что у AmoledBoard: их читают tools/trace.py, record.py, screenshot.py.
//
// Устройство трассы:
//   * у каждого ядра свой маленький буфер во внутренней памяти (256 событий). Пишут в него задачи и прерывания
//     этого ядра: на время записи прерывания своего ядра выключены (несколько тактов), поэтому без блокировок.
//     Внутренняя память — потому что прерывания (TE, DMA) работают и тогда, когда кэш выключен (запись во флеш),
//     а PSRAM без кэша не читается;
//   * halo_trace_pump() переносит новые события в большой кольцевой буфер в PSRAM (64 тыс. событий, ~10 с):
//     движок — раз в кадр, halo_loop — каждый проход;
//   * trace dump выдаёт большой буфер в порт: заголовок с именами событий и команд, события, контрольная сумма.
#include "halo_internal.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/task.h>

volatile bool halo_quiet = false;

// ---------- Шаги запуска и безопасный режим ----------
// Шаг запуска пишется в память RTC, которая переживает перезагрузку (но не выключение). Плата упала два раза
// подряд — запускаемся без экрана и тача: порт жив, журнал говорит, на каком шаге было падение, прошивка
// заливается обычным способом. Иначе цикл «упала — перезагрузилась» за минуту вешает USB так, что плату
// спасает только выключение кнопкой.
#define RTC_MAGIC 0x48414C4Fu  // "HALO"
RTC_NOINIT_ATTR static uint32_t rtc_magic, rtc_stage, rtc_crashes;
static const char *const STAGES[] = {"core", "display bus", "display panel", "display dma", "touch", "tasks",
                                     "first frame", "running"};
static uint32_t boot_ms;
static bool safe;

// Пока идёт запуск, задачу setup() сторожит сторожевой таймер ESP-IDF (5 с): шаг завис — плата падает с
// разбором (бэктрейс в порт) и перезагружается, а шаг остаётся в памяти RTC
static bool watched;

void hc_stage(int s) {
  rtc_stage = s;
  if (watched) esp_task_wdt_reset();
}

void hc_boot_done() {  // запуск кончился (или не удался): дальше setup() не сторожим
  if (watched) esp_task_wdt_delete(nullptr), watched = false;
}

bool hc_safe_mode() {
  return safe;
}

// ---------- Порт ----------
// Если USB подключён, но порт никто не читает, запись ждёт до 2 с на строку — поэтому пишем без ожидания (лишнее
// отбрасывается, журнал хранит всё), а ждём только при выдаче данных, когда нас точно читают
static void serial_wait(bool wait) {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(wait ? 100 : 0);
#endif
}

void halo_serial_binary(bool on) {
  if (on) {
    halo_quiet = true;
    esp_log_level_set("*", ESP_LOG_NONE);
    serial_wait(true);
    return;
  }
  Serial.flush();
  serial_wait(false);
  esp_log_level_set("*", ESP_LOG_ERROR);
  halo_quiet = false;
}

// ---------- Журнал: печать в порт и последние 8 КБ строк в PSRAM (команда log) ----------
#define LOG_SIZE 8192
static char *log_ring;
static size_t log_pos;
static bool log_wrapped;
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;

void halo_logf(const char *fmt, ...) {
  char line[192];
  const int n = snprintf(line, sizeof(line), "[%7.1f] ", millis() / 1000.0f);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n, fmt, ap);
  va_end(ap);
  if (!log_ring) {
    char *r = (char *)heap_caps_malloc(LOG_SIZE, MALLOC_CAP_SPIRAM);
    if (!log_ring) log_ring = r;
    else free(r);
  }
  portENTER_CRITICAL(&log_mux);
  for (const char *p = line; *p && log_ring; p++) {
    log_ring[log_pos++] = *p;
    if (log_pos == LOG_SIZE) log_pos = 0, log_wrapped = true;
  }
  portEXIT_CRITICAL(&log_mux);
  if (!halo_quiet) Serial.print(line + n);
}

static void print_log(const char *) {
  char *copy = (char *)heap_caps_malloc(LOG_SIZE, MALLOC_CAP_SPIRAM);
  if (!copy || !log_ring) return free(copy);
  size_t len;
  portENTER_CRITICAL(&log_mux);
  if (log_wrapped) {
    len = LOG_SIZE;
    memcpy(copy, log_ring + log_pos, LOG_SIZE - log_pos);
    memcpy(copy + LOG_SIZE - log_pos, log_ring, log_pos);
  } else {
    len = log_pos;
    memcpy(copy, log_ring, len);
  }
  portEXIT_CRITICAL(&log_mux);
  serial_wait(true);
  Serial.println("--- log ---");
  Serial.write((const uint8_t *)copy, len);
  Serial.println("--- end ---");
  Serial.flush();
  serial_wait(false);
  free(copy);
}

// ---------- Картинка в порт (для shot): формат tools/screenshot.py ----------
void hc_send_image(const uint16_t *px, int w, int h) {
  uint32_t sum = 0;
  for (size_t i = 0; i < (size_t)w * h; i++) sum += px[i];
  halo_serial_binary(true);
  Serial.printf("SHOT %d %d\n", w, h);
  const uint8_t *p = (const uint8_t *)px;
  for (size_t left = (size_t)w * h * 2; left;) {
    const size_t n = Serial.write(p, min<size_t>(left, 4096));
    p += n, left -= n;
  }
  Serial.printf("\nSHOT END %lu\n", (unsigned long)sum);
  halo_serial_binary(false);
}

// ---------- Команды ----------
struct Command {
  const char *name;
  void (*fn)(const char *args);
};
static Command commands[48];
static int command_count;

void halo_command(const char *name, void (*fn)(const char *args)) {
  for (int i = 0; i < command_count; i++)
    if (!strcmp(commands[i].name, name)) {
      commands[i].fn = fn;
      return;
    }
  if (command_count < 48) commands[command_count++] = {name, fn};
}

static int hc_command_names(const char **out, int max) {
  const int n = min(max, command_count);
  for (int i = 0; i < n; i++) out[i] = commands[i].name;
  return n;
}

static void run_command(char *line) {
  char *args = strchr(line, ' ');
  if (args) *args++ = 0;
  if (!*line) return;
  for (int i = 0; i < command_count; i++)
    if (!strcmp(line, commands[i].name)) {
      const uint32_t t0 = trace_now();
      commands[i].fn(args ? args : "");
      trace_span(TR_CMD, t0, i);
      return;
    }
  Serial.printf("unknown command: %s (help — список)\n", line);
}

static void hc_trace_tick();
static const char *reset_name(esp_reset_reason_t r);

void hc_loop() {
  static char line[64];
  static size_t len;
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch != '\n' && ch != '\r') {
      if (len < sizeof(line) - 1) line[len++] = ch;
      continue;
    }
    line[len] = 0;
    len = 0;
    run_command(line);
  }
  hc_trace_tick();
  if (!safe && rtc_stage != 7 && millis() - boot_ms > 5000) rtc_stage = 7, rtc_crashes = 0;  // 5 с без падений
}

void hc_begin(const char *name) {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxBufferSize(4096);
#endif
  Serial.begin(115200);
  serial_wait(false);
  delay(300);
  LOG("%s start\n", name);
  const esp_reset_reason_t why = esp_reset_reason();
  const bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
  if (rtc_magic != RTC_MAGIC) rtc_magic = RTC_MAGIC, rtc_crashes = 0, rtc_stage = 0;
  const uint32_t last = rtc_stage < sizeof(STAGES) / sizeof(*STAGES) ? rtc_stage : 0;
  rtc_crashes = crashed ? rtc_crashes + 1 : 0;
  if (crashed) LOG("boot: after a crash (%s) at stage «%s», %lu in a row\n", reset_name(why), STAGES[last],
                   (unsigned long)rtc_crashes);
  else if (rtc_stage != 7 && why != ESP_RST_POWERON)
    LOG("boot: previous run stopped at stage «%s» (reset: %s)\n", STAGES[last], reset_name(why));
  safe = rtc_crashes >= 2;
  if (safe) LOG("boot: SAFE MODE — no display, touch or frames; flash a fixed firmware (log — this message)\n");
  boot_ms = millis();
  watched = !safe && esp_task_wdt_add(nullptr) == ESP_OK;
  hc_stage(0);
  halo_command("log", print_log);
  halo_command("help", [](const char *) {
    Serial.print("commands:");
    for (int i = 0; i < command_count; i++) Serial.printf(" %s", commands[i].name);
    Serial.println();
  });
  hc_trace_setup();
}

// ---------- Трасса ----------
volatile bool halo_trace_on = false;

struct TraceRec {
  uint32_t t;  // мкс (esp_timer)
  uint16_t a;  // длительность, мкс (отрезок) или значение
  uint16_t m;  // вид (6 бит) | ядро << 6 | b << 7 (b — 9 бит)
};

#define HOT_N 256  // степень двойки
#define ARCH_N 65536
struct HotRing {
  volatile uint32_t head, tail, dropped;
  TraceRec *rec;
};
static DRAM_ATTR HotRing hot[2];
static TraceRec *arch;  // PSRAM
static uint32_t arch_n;  // сколько всего перенесено (индекс — arch_n % ARCH_N)
static volatile uint8_t pump_lock;

static const char *tr_names[64];
static char tr_kinds[64];

void IRAM_ATTR halo_trace_put(uint8_t ev, uint32_t t, uint16_t a, uint16_t b) {
  const uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();  // своё ядро не переключится и не прервётся
  const int c = xPortGetCoreID();
  HotRing &r = hot[c];
  const uint32_t h = r.head;
  if (r.rec && h - r.tail < HOT_N) {
    TraceRec &x = r.rec[h & (HOT_N - 1)];
    x.t = t, x.a = a, x.m = (ev & 63) | (c << 6) | ((b & 511) << 7);
    __atomic_store_n(&r.head, h + 1, __ATOMIC_RELEASE);
  } else {
    r.dropped++;
  }
  portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
}

static bool pump_take() {
  return !__atomic_test_and_set(&pump_lock, __ATOMIC_ACQUIRE);
}
static void pump_give() {
  __atomic_clear(&pump_lock, __ATOMIC_RELEASE);
}

static void pump_locked() {
  for (int c = 0; c < 2; c++) {
    HotRing &r = hot[c];
    if (!r.rec) continue;
    const uint32_t h = __atomic_load_n(&r.head, __ATOMIC_ACQUIRE);
    for (uint32_t i = r.tail; i != h; i++) arch[arch_n++ & (ARCH_N - 1)] = r.rec[i & (HOT_N - 1)];
    __atomic_store_n(&r.tail, h, __ATOMIC_RELEASE);
  }
}

void halo_trace_pump() {
  if (!arch || !pump_take()) return;  // переносит другая задача — она и перенесёт
  pump_locked();
  pump_give();
}

void halo_trace_name(uint8_t ev, const char *name, char kind) {
  if (ev < 64) tr_names[ev] = name, tr_kinds[ev] = kind;
}

static bool trace_alloc() {
  for (int c = 0; c < 2; c++)
    if (!hot[c].rec) hot[c].rec = (TraceRec *)heap_caps_malloc(HOT_N * sizeof(TraceRec), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!arch) arch = (TraceRec *)heap_caps_malloc(ARCH_N * sizeof(TraceRec), MALLOC_CAP_SPIRAM);
  return hot[0].rec && hot[1].rec && arch;
}

static void trace_enable(bool on) {
  if (on && !trace_alloc()) {
    LOG("trace: no memory\n");
    return;
  }
  if (on) {  // запись с начала
    halo_trace_on = false;
    while (!pump_take()) delay(1);
    for (int c = 0; c < 2; c++) hot[c].tail = hot[c].head, hot[c].dropped = 0;
    arch_n = 0;
    pump_give();
  }
  halo_trace_on = on;
}

// ---------- Счётчики: память, загрузка ядер, температура ----------
static void sample_counters() {
  static uint32_t heap_at, load_at, temp_at, idle_prev[2], load_t_prev;
  const uint32_t now = millis();
  if (now - heap_at >= 100) {
    heap_at = now;
    trace_count(TR_HEAP, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    trace_count(TR_HEAP_BLOCK, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    trace_count(TR_PSRAM, heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  }
  if (now - load_at >= 250) {  // время задачи простоя каждого ядра (мкс) против прошедшего
    load_at = now;
    const uint32_t t = (uint32_t)esp_timer_get_time();
    for (int c = 0; c < 2; c++) {
      const uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(c);
      if (load_t_prev) {
        const float busy = 1 - (float)(idle - idle_prev[c]) / (float)(t - load_t_prev);
        trace_count(c ? TR_LOAD1 : TR_LOAD0, (uint16_t)constrain(busy * 1000, 0.0f, 1000.0f));
      }
      idle_prev[c] = idle;
    }
    load_t_prev = t;
  }
  if (now - temp_at >= 1000) {
    temp_at = now;
    trace_count(TR_TEMP, (uint16_t)max(0.0f, temperatureRead() * 10));
  }
}

// halo_loop: перенести события и снять счётчики
static void hc_trace_tick() {
  if (!halo_trace_on) return;
  halo_trace_pump();
  sample_counters();
}

// ---------- Выдача в порт ----------

static void trace_dump() {
  if (!arch) {
    Serial.println("TRACE 0 0 0");
    Serial.println("TRACE END 0");
    return;
  }
  const bool was_on = halo_trace_on;
  halo_trace_on = false;
  delay(2);  // запись, начатая до выключения, успевает закончиться
  while (!pump_take()) delay(1);
  pump_locked();
  const uint32_t n = min<uint32_t>(arch_n, ARCH_N), first = arch_n - n;
  const uint32_t dropped = hot[0].dropped + hot[1].dropped;  // буфер ядра переполнился до переноса
  uint32_t sum = 0;
  for (uint32_t i = 0; i < n; i++) {
    const TraceRec &r = arch[(first + i) & (ARCH_N - 1)];
    sum += r.t + (r.a | ((uint32_t)r.m << 16));
  }

  halo_serial_binary(true);
  Serial.printf("TRACE %lu %lu %lu\n", (unsigned long)n, (unsigned long)dropped, (unsigned long)trace_now());
  Serial.print("NAMES");
  for (int i = 0; i < 64; i++)
    if (tr_names[i]) Serial.printf(" %d:%c:%s", i, tr_kinds[i], tr_names[i]);
  Serial.println();
  const char *cmds[48];
  const int nc = hc_command_names(cmds, 48);
  Serial.print("CMDS");
  for (int i = 0; i < nc; i++) Serial.printf(" %s", cmds[i]);
  Serial.println();
  // Кольцо — двумя кусками (от первого события до конца буфера и с начала)
  const uint32_t s0 = first & (ARCH_N - 1), n0 = min<uint32_t>(n, ARCH_N - s0);
  const uint8_t *parts[2] = {(const uint8_t *)(arch + s0), (const uint8_t *)arch};
  const size_t lens[2] = {n0 * sizeof(TraceRec), (n - n0) * sizeof(TraceRec)};
  for (int k = 0; k < 2; k++)
    for (size_t off = 0; off < lens[k];) off += Serial.write(parts[k] + off, min<size_t>(lens[k] - off, 4096));
  Serial.printf("\nTRACE END %lu\n", (unsigned long)sum);
  halo_serial_binary(false);
  pump_give();
  halo_trace_on = was_on;
}

// Сколько стоит запись события: выключенной трассы и включённой, и сколько — само время esp_timer.
// Каждый замер — лучший из 50 повторов по 20 вызовов: команда идёт в loop(), её перебивают кадры и прерывания
static uint32_t bench_min(int what) {
  uint32_t best = UINT32_MAX;
  for (int k = 0; k < 50; k++) {
    const uint32_t c0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < 20; i++) {
      if (what == 0) (void)trace_now();
      else trace_mark(TR_LAST);
    }
    best = min(best, esp_cpu_get_cycle_count() - c0);
    halo_trace_pump();
  }
  return best / 20;
}

static void trace_bench() {
  const bool was_on = halo_trace_on;
  if (!trace_alloc()) return;
  const uint32_t c_time = bench_min(0);
  halo_trace_on = false;
  const uint32_t c_off = bench_min(1);
  halo_trace_on = true;
  const uint32_t c_on = bench_min(1);
  trace_enable(false);
  if (was_on) trace_enable(true);  // замер испортил запись — начать заново
  LOG("trace bench: event %lu cycles = %.2f us (time %lu, off %lu)\n", (unsigned long)c_on, c_on / 240.0f,
      (unsigned long)c_time, (unsigned long)c_off);
}

// ---------- sys: задачи, память, температура, питание ----------
static const char *reset_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "usb";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
  }
}

static void cmd_sys(const char *) {
  // Загрузка задач — с прошлого вызова sys (первый раз — с включения)
  static struct {
    TaskHandle_t h;
    uint32_t run;
  } prev[40];
  static uint32_t prev_total;
  static int prev_n;
  TaskStatus_t st[40];
  uint32_t total;
  const int n = uxTaskGetSystemState(st, 40, &total);
  const uint32_t dt = total - prev_total;

  halo_serial_binary(true);  // ответ длинный — пусть порт подождёт, журнал не вклинивается
  Serial.printf("sys: up %.1f s, cpu %lu MHz, reset: %s\n", millis() / 1000.0f, (unsigned long)getCpuFrequencyMhz(),
                reset_name(esp_reset_reason()));
  Serial.printf("memory: internal free %u KB (largest %u KB, lowest ever %u KB), dma %u KB, psram free %u KB of %u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
                heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024, heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
                heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
  float load[2] = {0, 0};
  Serial.println("tasks: name             core prio stack_free  cpu%");
  for (int i = 0; i < n; i++) {
    uint32_t run = st[i].ulRunTimeCounter, was = 0;
    for (int k = 0; k < prev_n; k++)
      if (prev[k].h == st[i].xHandle) was = prev[k].run;
    const float pct = dt ? 100.0f * (run - was) / dt : 0;
    const int core = st[i].xCoreID;
    if (!strncmp(st[i].pcTaskName, "IDLE", 4) && core >= 0 && core < 2) load[core] = 100 - pct;
    Serial.printf("  %-18s %4s %4u %10lu %5.1f\n", st[i].pcTaskName, core > 1 ? "any" : core ? "1" : "0",
                  (unsigned)st[i].uxCurrentPriority, (unsigned long)st[i].usStackHighWaterMark, pct);
  }
  Serial.printf("load: core0 %.0f%%, core1 %.0f%%\n", load[0], load[1]);
  int fly, queued, free_bufs;
  hd_state(&fly, &queued, &free_bufs);
  char regs[200];
  hd_regs(regs, sizeof(regs));
  Serial.printf("display: dma %s, queued %d, free buffers %d of %d; %s\n", fly >= 0 ? "busy" : "idle", queued,
                free_bufs, hd_buffers(), regs);
  for (int i = 0; i < n && i < 40; i++) prev[i] = {st[i].xHandle, st[i].ulRunTimeCounter};
  prev_n = min(n, 40), prev_total = total;
  Serial.printf("temp %.1f C", temperatureRead());
  Serial.printf(", vsync %lu us, trace %s\n", (unsigned long)hd_te_period(), halo_trace_on ? "on" : "off");
  halo_serial_binary(false);
}

static void cmd_trace(const char *a) {
  if (!strcmp(a, "on")) trace_enable(true), LOG("trace: on\n");
  else if (!strcmp(a, "off")) trace_enable(false), LOG("trace: off\n");
  else if (!strcmp(a, "dump")) trace_dump();
  else if (!strcmp(a, "bench")) trace_bench();
  else Serial.println("trace on | off | dump | bench");
}

void hc_trace_setup() {
  halo_trace_name(TR_TE, "te", 'i');
  halo_trace_name(TR_SCAN, "scan", 'i');
  halo_trace_name(TR_DMA, "dma", 'X');
  halo_trace_name(TR_WIN, "window", 'X');
  halo_trace_name(TR_BLIT_WAIT, "buffer wait", 'X');
  halo_trace_name(TR_TOUCH, "touch irq", 'i');
  halo_trace_name(TR_TOUCH_READ, "touch", 'i');
  halo_trace_name(TR_CMD, "command", 'X');
  halo_trace_name(TR_HEAP, "internal free KB", 'C');
  halo_trace_name(TR_HEAP_BLOCK, "internal largest KB", 'C');
  halo_trace_name(TR_PSRAM, "psram free KB", 'C');
  halo_trace_name(TR_LOAD0, "core0 load x10", 'C');
  halo_trace_name(TR_LOAD1, "core1 load x10", 'C');
  halo_trace_name(TR_TEMP, "temp x10", 'C');
  halo_trace_name(TR_LAST, "bench", 'i');
  halo_command("trace", cmd_trace);
  halo_command("sys", cmd_sys);
}
