/* ============================================================================
 *  timer_store.h — countdown-timer state + persistence (alarm backend).
 *
 *  The countdown is stored as an ABSOLUTE target epoch (RTC wall-clock seconds),
 *  not a remaining count, so it stays correct across deep sleep: every wake is a
 *  full reboot that reloads this from NVS and re-derives the remaining time from
 *  the PCF85063. That's why a timer can fire while the screen was off — see the
 *  wake handling in the .ino (timer_is_due() on a TIMER wake) and the alarm UI in
 *  app_timer.h.
 *
 *  INCLUDE AFTER settings_store.h (shares its `prefs` handle), power_model.h
 *  (uses rtc_now_epoch()) and storage_fs.h (the alarm clocks' /alarms.csv lives
 *  on store_fs()). Header-only; compiled into the .ino TU.
 * ========================================================================== */
#pragma once
#include <Preferences.h>
#include <time.h>          // localtime_r/mktime — alarm-clock wall-time math

/* --- timer state (persisted in the shared "watch" NVS namespace) --- */
static bool     s_tmr_active    = false;  // a countdown exists (running OR paused)
static bool     s_tmr_paused    = false;
static uint32_t s_tmr_target    = 0;      // epoch (s) at which it fires (when running)
static uint32_t s_tmr_duration  = 300;    // last-set length (s) — seeds the setup UI
static uint32_t s_tmr_rem_pause = 0;      // remaining seconds captured at pause

/* Whether a touch (not just BOOT) may dismiss a ringing alarm. Persisted. */
static bool     s_alarm_touch   = true;

/* Set true while the alarm is actually ringing. Owned by the alarm UI
 * (app_timer.h); read by the loop (keep-awake) and BOOT handler (dismiss). */
static bool     g_alarm_active  = false;

/* --- alarm-clock state (logic further down in this file) ---
 * Up to ALMC_MAX wall-clock alarms. The LIST lives in /alarms.csv on the shared
 * SD-else-FFat store (hand-editable, like wifi.csv) and is loaded on a full boot;
 * NVS keeps ONLY the earliest armed next-fire epoch ("almc_tgt"), so the
 * deep-sleep wake test works before any filesystem or RTC is up. */
#define ALMC_MAX      8
#define ALMC_CSV_PATH "/alarms.csv"

typedef struct {
  bool     en, rep;
  uint8_t  h, m;          // 0-23 / 0-59
  uint8_t  days;          // bit0=Mon .. bit6=Sun (used when rep)
  uint32_t target;        // next fire epoch (RAM only; 0 = none)
} AlmClk;

static AlmClk   s_almcs[ALMC_MAX];
static uint8_t  s_almc_n      = 0;
static uint32_t s_almc_master = 0;      // earliest enabled target (persisted)

static void timer_save(void) {
  prefs.putBool("tmr_act", s_tmr_active);
  prefs.putBool("tmr_pau", s_tmr_paused);
  prefs.putUInt("tmr_tgt", s_tmr_target);
  prefs.putUInt("tmr_dur", s_tmr_duration);
  prefs.putUInt("tmr_rem", s_tmr_rem_pause);
}

/* Load persisted timer + alarm settings. Call once at boot (after settings_load
 * has opened `prefs`). */
static void timer_load(void) {
  s_tmr_active    = prefs.getBool("tmr_act", false);
  s_tmr_paused    = prefs.getBool("tmr_pau", false);
  s_tmr_target    = prefs.getUInt("tmr_tgt", 0);
  s_tmr_duration  = prefs.getUInt("tmr_dur", 300);
  s_tmr_rem_pause = prefs.getUInt("tmr_rem", 0);
  s_alarm_touch   = prefs.getBool("alm_tch", true);
  s_almc_master   = prefs.getUInt ("almc_tgt", 0);   // saved ABSOLUTE epoch — usable
                                                     // before the RTC/filesystem are up
}

static bool     timer_is_active(void) { return s_tmr_active; }
static bool     timer_is_paused(void) { return s_tmr_active && s_tmr_paused; }
static uint32_t timer_duration_s(void){ return s_tmr_duration; }

static bool     alarm_touch_dismiss(void) { return s_alarm_touch; }
static void     alarm_set_touch_dismiss(bool en) {
  s_alarm_touch = en;
  prefs.putBool("alm_tch", en);
}

/* Seconds left (0 = expired/none). RTC-derived, so correct across sleep reboots. */
static uint32_t timer_remaining_s(void) {
  if (!s_tmr_active) return 0;
  if (s_tmr_paused)  return s_tmr_rem_pause;
  uint32_t now = rtc_now_epoch();
  if (now == 0 || s_tmr_target == 0 || now >= s_tmr_target) return 0;
  return s_tmr_target - now;
}

/* True when a running timer has reached its target (needs a valid clock). */
static bool timer_is_due(void) {
  if (!s_tmr_active || s_tmr_paused || s_tmr_target == 0) return false;
  uint32_t now = rtc_now_epoch();
  return now != 0 && now >= s_tmr_target;
}

/* Arm a fresh countdown of `seconds`. */
static void timer_start(uint32_t seconds) {
  if (seconds == 0) return;
  uint32_t now = rtc_now_epoch();
  s_tmr_duration  = seconds;
  s_tmr_target    = now + seconds;   // now==0 (clock unset) -> fires at once; clock is set on this watch
  s_tmr_active    = true;
  s_tmr_paused    = false;
  s_tmr_rem_pause = 0;
  timer_save();
}

static void timer_pause(void) {
  if (!s_tmr_active || s_tmr_paused) return;
  s_tmr_rem_pause = timer_remaining_s();
  s_tmr_paused = true;
  timer_save();
}

static void timer_resume(void) {
  if (!s_tmr_active || !s_tmr_paused) return;
  uint32_t now = rtc_now_epoch();
  s_tmr_target = now + s_tmr_rem_pause;
  s_tmr_paused = false;
  timer_save();
}

/* Stop + clear the countdown (keeps s_tmr_duration as the last-used length). */
static void timer_cancel(void) {
  s_tmr_active = false;
  s_tmr_paused = false;
  s_tmr_target = 0;
  s_tmr_rem_pause = 0;
  timer_save();
}

/* ===================== alarm clocks (multi, CSV-backed) ===================
 * Each alarm rings at its HH:MM — one-shot by default, or on selected weekdays
 * with repeat ON. The list is /alarms.csv on store_fs() (SD if present, else
 * FFat); one alarm per line, hand-editable:
 *     enabled,hour,minute,repeat,days
 * with days a decimal bitmask (bit0=Mon .. bit6=Sun; 31 = Mon-Fri, 127 = all).
 * Lines starting with '#' and blanks are ignored. The whole file is rewritten
 * on every change (it's at most ALMC_MAX lines).
 *
 * The earliest enabled next-fire epoch is mirrored to NVS ("almc_tgt") on every
 * rearm, so the deep-sleep wake decision (which runs before storage mounts) can
 * test it cheaply; the full boot then loads the CSV and rings the right alarm. */

/* Next epoch at which alarm `a` should ring (0 = never: disabled, clock unset,
 * or repeat with no weekday selected). Walks day by day through localtime/mktime
 * so the TZ/DST rules give the true wall-clock HH:MM, same as rtc_now_epoch(). */
static uint32_t almclk_next_epoch(const AlmClk *a) {
  if (!a->en) return 0;
  uint32_t now = rtc_now_epoch();
  if (now == 0) return 0;
  for (int d = 0; d < 8; d++) {
    time_t base = (time_t)now + (time_t)d * 86400;
    struct tm lt;
    localtime_r(&base, &lt);
    lt.tm_hour = a->h; lt.tm_min = a->m; lt.tm_sec = 0; lt.tm_isdst = -1;
    time_t t = mktime(&lt);                  // also normalizes lt.tm_wday
    if (t <= (time_t)now) continue;
    if (a->rep && !(a->days & (1 << ((lt.tm_wday + 6) % 7)))) continue;
    return (uint32_t)t;
  }
  return 0;
}

/* Rewrite the whole CSV from the in-RAM list. */
static void almclk_save_csv(void) {
  if (!store_available()) return;
  File f = store_fs().open(ALMC_CSV_PATH, FILE_WRITE);   // truncates
  if (!f) { USBSerial.println("[almc] csv write failed"); return; }
  f.println("# enabled,hour,minute,repeat,days  (days: bit0=Mon..bit6=Sun; 31=Mon-Fri, 127=all)");
  for (uint8_t i = 0; i < s_almc_n; i++)
    f.printf("%d,%u,%u,%d,%u\n", s_almcs[i].en ? 1 : 0, s_almcs[i].h, s_almcs[i].m,
             s_almcs[i].rep ? 1 : 0, s_almcs[i].days);
  f.close();
}

/* Recompute every alarm's target + persist the earliest to NVS. Call after ANY
 * change and after a fire. Needs a valid RTC (so not from timer_load — the boot
 * wake test uses the previously saved master until almclk_load() runs). */
static void almclk_rearm(void) {
  uint32_t master = 0;
  for (uint8_t i = 0; i < s_almc_n; i++) {
    s_almcs[i].target = almclk_next_epoch(&s_almcs[i]);
    if (s_almcs[i].target && (!master || s_almcs[i].target < master))
      master = s_almcs[i].target;
  }
  s_almc_master = master;
  prefs.putUInt("almc_tgt", master);
}

/* Load the list from CSV (full boot, after storage + RTC are usable). If no CSV
 * exists yet, migrate the pre-multi single alarm out of NVS, then write the CSV. */
static void almclk_load(void) {
  s_almc_n = 0;
  if (store_available() && store_fs().exists(ALMC_CSV_PATH)) {
    File f = store_fs().open(ALMC_CSV_PATH, FILE_READ);
    while (f && f.available() && s_almc_n < ALMC_MAX) {
      String ln = f.readStringUntil('\n');
      ln.trim();
      if (ln.length() == 0 || ln[0] == '#') continue;
      int en, h, m, rep, days;
      if (sscanf(ln.c_str(), "%d,%d,%d,%d,%d", &en, &h, &m, &rep, &days) == 5 &&
          h >= 0 && h < 24 && m >= 0 && m < 60) {
        s_almcs[s_almc_n++] = { en != 0, rep != 0, (uint8_t)h, (uint8_t)m,
                                (uint8_t)(days & 0x7F), 0 };
      }
    }
    if (f) f.close();
  } else if (prefs.isKey("almc_h")) {
    // one-time migration from the single-alarm NVS keys
    s_almcs[0] = { prefs.getBool("almc_en", false), prefs.getBool("almc_rep", false),
                   prefs.getUChar("almc_h", 7), prefs.getUChar("almc_m", 0),
                   prefs.getUChar("almc_day", 0x1F), 0 };
    s_almc_n = 1;
    prefs.remove("almc_en"); prefs.remove("almc_h"); prefs.remove("almc_m");
    prefs.remove("almc_rep"); prefs.remove("almc_day");
    almclk_save_csv();
    USBSerial.println("[almc] migrated single NVS alarm -> alarms.csv");
  }
  almclk_rearm();
}

static bool almclk_is_due(void) {
  if (s_almc_master == 0) return false;
  uint32_t now = rtc_now_epoch();
  return now != 0 && now >= s_almc_master;
}

/* Seconds until the earliest armed fire (0 = nothing armed; 1 = due right now). */
static uint32_t almclk_seconds_until(void) {
  if (s_almc_master == 0) return 0;
  uint32_t now = rtc_now_epoch();
  if (now == 0) return 0;
  return (s_almc_master > now) ? (s_almc_master - now) : 1;
}

/* Which alarm is due right now? Earliest-due index, or -1. */
static int almclk_due_index(void) {
  uint32_t now = rtc_now_epoch();
  if (now == 0) return -1;
  int best = -1;
  for (uint8_t i = 0; i < s_almc_n; i++)
    if (s_almcs[i].en && s_almcs[i].target && s_almcs[i].target <= now &&
        (best < 0 || s_almcs[i].target < s_almcs[best].target))
      best = i;
  return best;
}

/* Advance alarm `i` past a fire: one-shot disables itself; repeat re-arms. */
static void almclk_fired(int i) {
  if (i < 0 || i >= s_almc_n) return;
  if (!s_almcs[i].rep) { s_almcs[i].en = false; almclk_save_csv(); }
  almclk_rearm();
}

/* Append a fresh alarm (07:00, on, one-shot). Returns its index, or -1 if full. */
static int almclk_add(void) {
  if (s_almc_n >= ALMC_MAX) return -1;
  s_almcs[s_almc_n] = { true, false, 7, 0, 0x1F, 0 };
  return s_almc_n++;
}

static void almclk_remove(int i) {
  if (i < 0 || i >= s_almc_n) return;
  for (int k = i + 1; k < s_almc_n; k++) s_almcs[k - 1] = s_almcs[k];
  s_almc_n--;
}

/* Nudge a running/paused timer by delta seconds (e.g. the +1:00 button), never
 * below zero. */
static void timer_adjust(int32_t delta_s) {
  if (!s_tmr_active) return;
  if (s_tmr_paused) {
    int64_t r = (int64_t)s_tmr_rem_pause + delta_s;
    s_tmr_rem_pause = (r < 0) ? 0 : (uint32_t)r;
  } else {
    uint32_t now = rtc_now_epoch();
    int64_t t = (int64_t)s_tmr_target + delta_s;
    if (now && t < (int64_t)now) t = now;
    s_tmr_target = (uint32_t)t;
  }
  timer_save();
}
