/* ============================================================================
 *  cpu_usage.h — per-core CPU usage, measured when the libs allow it.
 *
 *  TWO PATHS, same tiny API (init / sample / reset_window / pct), chosen at
 *  compile time from the IDF libs' sdkconfig:
 *
 *  1) MEASURED (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS — set in the custom
 *     lib-builder defconfig): FreeRTOS timestamps every context switch with the
 *     1 us esp_timer clock and accumulates true run time per task. Each core's
 *     busy% is then literally (window - idle_task_runtime) / window. This is
 *     ACTUAL usage, including sub-millisecond bursts the tick proxy can't see.
 *
 *  2) TICK PROXY (stock libs): hang a FreeRTOS idle hook on each core and count
 *     the DISTINCT TICKS in which that core's idle task ran (NOT raw hook calls
 *     — the hook fires thousands of times per tick whenever some other idle hook
 *     keeps the idle loop from WFI-sleeping, which the custom libs build does;
 *     raw counts then always exceed elapsed ticks and read 0% forever). A tick
 *     where idle ran at all counts as idle, so short bursts under-read slightly.
 *
 *  Neither path costs measurable power: the measured path is one timer read per
 *  context switch inside the kernel; the proxy hook returns TRUE so the core
 *  still enters its low-power WFI between ticks.
 *
 *  Call cpu_usage_init() ONCE after the scheduler is up (e.g. end of setup()).
 *  Then cpu_usage_sample() periodically (it latches a 0..100 % per core since the
 *  previous sample) and read it back with cpu_usage_pct(core).
 *
 *  Header-only, compiled into the .ino TU. No dependencies beyond ESP-IDF/FreeRTOS.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint8_t s_cpu_pct[2]  = {0, 0};
static bool    s_cpu_inited  = false;

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* ======================= path 1: MEASURED run time ======================== */
#include "esp_timer.h"

/* Idle task handles (resolved once) + their last-sampled runtime counters.
 * ulRunTimeCounter is 32-bit microseconds -> wraps every ~71 min; all deltas use
 * unsigned subtraction, and the Power app resets the window on open, so wrap is
 * harmless at UI sampling rates. */
static TaskHandle_t s_cpu_idle_task[2]    = {nullptr, nullptr};
static uint32_t     s_cpu_idle_us_prev[2] = {0, 0};
static int64_t      s_cpu_t_prev          = 0;

static uint32_t cpu_idle_runtime_us(uint8_t c) {
  if (!s_cpu_idle_task[c]) return 0;
  TaskStatus_t st;
  // eInvalid: don't compute the task state (we only want ulRunTimeCounter).
  vTaskGetInfo(s_cpu_idle_task[c], &st, pdFALSE, eInvalid);
  return st.ulRunTimeCounter;
}

static void cpu_usage_init(void) {
  if (s_cpu_inited) return;
  s_cpu_idle_task[0] = xTaskGetIdleTaskHandleForCore(0);
  s_cpu_idle_task[1] = xTaskGetIdleTaskHandleForCore(1);
  s_cpu_idle_us_prev[0] = cpu_idle_runtime_us(0);
  s_cpu_idle_us_prev[1] = cpu_idle_runtime_us(1);
  s_cpu_t_prev = esp_timer_get_time();
  s_cpu_inited = true;
}

/* Latch each core's busy% over the window since the previous call. */
static void cpu_usage_sample(void) {
  if (!s_cpu_inited) return;
  int64_t  now     = esp_timer_get_time();
  uint32_t elapsed = (uint32_t)(now - s_cpu_t_prev);     // us; wrap-safe via reset
  if (elapsed < 1000) return;                            // <1ms window -> skip
  for (uint8_t c = 0; c < 2; c++) {
    uint32_t cur  = cpu_idle_runtime_us(c);
    uint32_t idle = cur - s_cpu_idle_us_prev[c];         // unsigned: wrap-safe
    s_cpu_idle_us_prev[c] = cur;
    if (idle > elapsed) idle = elapsed;                  // clock-domain slop
    s_cpu_pct[c] = (uint8_t)(((uint64_t)(elapsed - idle) * 100u) / elapsed);
  }
  s_cpu_t_prev = now;
}

/* Reset the measurement window to "now" WITHOUT latching a percentage. Call when
 * a viewer (the Power app) opens, so its first sample reflects recent activity. */
static void cpu_usage_reset_window(void) {
  if (!s_cpu_inited) return;
  s_cpu_idle_us_prev[0] = cpu_idle_runtime_us(0);
  s_cpu_idle_us_prev[1] = cpu_idle_runtime_us(1);
  s_cpu_t_prev = esp_timer_get_time();
}

/* ---- serial CPU profiling toggle ----
 * CPU_PROFILE_SERIAL 1 enables BOTH profilers (compiled out entirely at 0):
 *   [cpu]  per-task table every 10 s — each task's REAL CPU share per core
 *          (cpu_usage_profile_tick below; needs the runtime-stats libs).
 *   [loop] loop micro-profile every 10 s — passes/10s, fast-cadence passes,
 *          passes that drew, avg body us (lives in the .ino loop tail).
 * THE tool for "what is eating core 1?": flip to 1, flash, watch serial.
 * History: found the FT3168 3x-I2C-reads-every-8ms idle drain (~6-10% of core 1,
 * fixed by the TP_INT fast path in my_touchpad_read) and the menu-open 1 ms
 * pacing cost. Healthy idle reference: loopTask ~1%, avg body ~30 us. */
#define CPU_PROFILE_SERIAL    0
#define CPU_PROFILE_PERIOD_MS 10000
#define CPU_PROFILE_MAX       24

#if CPU_PROFILE_SERIAL
struct CpuProfPrev { TaskHandle_t h; uint32_t rt; };

static void cpu_usage_profile_tick(void) {
  static uint32_t      s_last_ms = 0;
  static TaskStatus_t *s_st      = nullptr;   // PSRAM (~1.2KB; SRAM is precious)
  static CpuProfPrev  *s_prev    = nullptr;
  static UBaseType_t   s_prev_n  = 0;
  static int64_t       s_prev_t  = 0;

  uint32_t ms = millis();
  if (s_last_ms && ms - s_last_ms < CPU_PROFILE_PERIOD_MS) return;
  s_last_ms = ms;

  if (!s_st)
    s_st = (TaskStatus_t *)heap_caps_malloc(sizeof(TaskStatus_t) * CPU_PROFILE_MAX,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_prev)
    s_prev = (CpuProfPrev *)heap_caps_calloc(CPU_PROFILE_MAX, sizeof(CpuProfPrev),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_st || !s_prev) return;

  int64_t     now = esp_timer_get_time();
  UBaseType_t n   = uxTaskGetSystemState(s_st, CPU_PROFILE_MAX, nullptr);
  if (n == 0) return;                          // more tasks than the array -> skip

  uint32_t win = (uint32_t)(now - s_prev_t);
  bool have_window = (s_prev_t != 0 && win > 0);
  if (have_window)
    USBSerial.printf("[cpu] per-task CPU over %u.%us (>=0.1%%, per-core):\n",
                     (unsigned)(win / 1000000u), (unsigned)((win / 100000u) % 10));

  for (UBaseType_t i = 0; i < n; i++) {
    uint32_t rt = s_st[i].ulRunTimeCounter;
    uint32_t d  = rt;                          // task new this window -> whole runtime
    for (UBaseType_t j = 0; j < s_prev_n; j++)
      if (s_prev[j].h == s_st[i].xHandle) { d = rt - s_prev[j].rt; break; }
    if (have_window && d >= win / 1000) {      // print only >=0.1%
      if (d > win) d = win;                    // clock-domain slop / new-task clamp
      BaseType_t core = xTaskGetCoreID(s_st[i].xHandle);
      char cc = (core == 0) ? '0' : (core == 1) ? '1' : '*';
      uint32_t pm = (uint32_t)(((uint64_t)d * 1000u) / win);   // permille
      USBSerial.printf("[cpu]   c%c %-18s %3u.%u%%\n",
                       cc, s_st[i].pcTaskName, (unsigned)(pm / 10), (unsigned)(pm % 10));
    }
  }
  for (UBaseType_t i = 0; i < n; i++) { s_prev[i].h = s_st[i].xHandle; s_prev[i].rt = s_st[i].ulRunTimeCounter; }
  s_prev_n = n;
  s_prev_t = now;
}
#else
static void cpu_usage_profile_tick(void) {}
#endif  /* CPU_PROFILE_SERIAL */

#else
/* ======================= path 2: idle-tick proxy ========================== */
#include "esp_freertos_hooks.h"

/* Distinct ticks in which each core's idle task ran. volatile: written from the
 * idle task on each core, read from the UI; aligned 32-bit accesses are atomic
 * on the S3. The seen-tick latches are hook-private (one writer each). */
static volatile uint32_t s_cpu_idle_cnt[2]   = {0, 0};
static uint32_t s_cpu_idle_seen_tick[2]      = {0, 0};
static uint32_t s_cpu_idle_prev[2]           = {0, 0};
static uint32_t s_cpu_tick_prev              = 0;

static bool cpu_idle_hook_core0(void) {
  uint32_t t = xTaskGetTickCount();
  if (t != s_cpu_idle_seen_tick[0]) { s_cpu_idle_seen_tick[0] = t; s_cpu_idle_cnt[0]++; }
  return true;
}
static bool cpu_idle_hook_core1(void) {
  uint32_t t = xTaskGetTickCount();
  if (t != s_cpu_idle_seen_tick[1]) { s_cpu_idle_seen_tick[1] = t; s_cpu_idle_cnt[1]++; }
  return true;
}

static void cpu_usage_init(void) {
  if (s_cpu_inited) return;
  esp_register_freertos_idle_hook_for_cpu(cpu_idle_hook_core0, 0);
  esp_register_freertos_idle_hook_for_cpu(cpu_idle_hook_core1, 1);
  s_cpu_idle_prev[0] = s_cpu_idle_cnt[0];
  s_cpu_idle_prev[1] = s_cpu_idle_cnt[1];
  s_cpu_tick_prev    = xTaskGetTickCount();
  s_cpu_inited       = true;
}

static void cpu_usage_sample(void) {
  if (!s_cpu_inited) return;
  uint32_t now_tick = xTaskGetTickCount();
  uint32_t elapsed  = now_tick - s_cpu_tick_prev;        // wrap-safe (unsigned)
  if (elapsed == 0) return;
  for (uint8_t c = 0; c < 2; c++) {
    uint32_t cur  = s_cpu_idle_cnt[c];
    uint32_t idle = cur - s_cpu_idle_prev[c];            // idle ticks this window
    s_cpu_idle_prev[c] = cur;
    if (idle > elapsed) idle = elapsed;
    s_cpu_pct[c] = (uint8_t)(((elapsed - idle) * 100u) / elapsed);
  }
  s_cpu_tick_prev = now_tick;
}

static void cpu_usage_reset_window(void) {
  if (!s_cpu_inited) return;
  s_cpu_idle_prev[0] = s_cpu_idle_cnt[0];
  s_cpu_idle_prev[1] = s_cpu_idle_cnt[1];
  s_cpu_tick_prev    = xTaskGetTickCount();
}

/* No measured runtime on stock libs -> the profiler has nothing real to report. */
static void cpu_usage_profile_tick(void) {}
#endif  /* CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

/* Last latched busy percentage (0..100) for core 0 or 1. */
static uint8_t cpu_usage_pct(uint8_t core) {
  return (core < 2) ? s_cpu_pct[core] : 0;
}
