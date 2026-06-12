/* ============================================================================
 *  app_timer.h — Timer + Stopwatch app, and the full-screen alarm overlay.
 *
 *  Timer: a persistent countdown (state in timer_store.h) set with tap-or-hold
 *  +/- buttons (holding auto-repeats with a gradual acceleration ramp),
 *  start/pause/cancel, a live progress bar, and a clear warning that the
 *  watch sleeps. Because the target is an absolute RTC time, the countdown keeps
 *  running while the watch is asleep; the .ino re-checks timer_is_due() on every
 *  wake and fires the alarm here.
 *
 *  Stopwatch: a classic awake-only stopwatch (millis based) with laps.
 *
 *  Alarm: when the timer is due, alarm_fire() raises a modal overlay on the sys
 *  layer (above everything, even the pull-down shade), buzzes the motor in a
 *  looping heartbeat pattern, and calls the sound hook. Dismiss with BOOT (always)
 *  or a touch (if enabled). alarm_dismiss() stops the buzz + clears the timer.
 *
 *  INCLUDE AFTER app_menu.h (screen shell + nav), timer_store.h, haptics.h, and
 *  settings_store.h (ui_accent_hex). Header-only; compiled into the .ino TU.
 *  app_open_timer / app_open_stopwatch are forward-declared in app_menu.h.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* ============================ shared helpers ============================== */

/* Format seconds as H:MM:SS (or MM:SS under an hour). */
static void tmr_fmt(uint32_t s, char *buf, size_t n) {
  uint32_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
  if (h > 0) snprintf(buf, n, "%u:%02u:%02u", h, m, sec);
  else       snprintf(buf, n, "%02u:%02u", m, sec);
}

/* A flat pill button with a centered label. */
static lv_obj_t *tmr_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                         void *ud, uint32_t bg, int w) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, 52);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

/* ============================== alarm UI ================================= */

static lv_obj_t *alarm_scr = nullptr;   // full-screen modal overlay (null = not ringing)
static bool      s_alarm_from_timer = false;  // true = a timer fired it (dismiss cancels the timer);
                                              // false = an external ring (e.g. Find My Watch)
static bool      s_alarm_from_clock = false;  // true = the alarm CLOCK fired it (its state was
                                              // already advanced at fire time; dismiss only
                                              // refreshes the Alarm screen if it's open)
static void app_open_alarmclock(void);        // fwd: the Alarm list screen (defined below)
static void app_open_alarmedit(void);         // fwd: the single-alarm editor (defined below)

/* The watch only pushes its RAM framebuffer to the panel when the loop sees
 * something "dirty" (DIRECT_RENDER_MODE). g_alarm_active forces a push every loop
 * while ringing; this one-shot covers the single frame AFTER dismissal so the
 * overlay actually clears off the glass. Read-and-clear in the loop. */
static bool s_alarm_dirty = false;
static inline bool alarm_take_dirty(void) { bool d = s_alarm_dirty; s_alarm_dirty = false; return d; }

/* ---- SOUND HOOK ----
 * Synthesized bell chime through the ES8311 codec (audio_alarm.h) —
 * alarm_fire() starts it, dismiss stops it. */
static void alarm_sound_start(void) {
  if (settings_get_mute()) return;     // muted -> vibration only, no tone
  audio_alarm_start();
}
static void alarm_sound_stop(void)  { audio_alarm_stop(); }

static void alarm_dismiss(void) {
  if (!g_alarm_active) return;
  haptics_stop();
  alarm_sound_stop();
  bool fired_timer = s_alarm_from_timer;
  if (fired_timer) timer_cancel();          // only a fired TIMER is "done"; an
                                            // external ring (Find My Watch) has none
  g_alarm_active = false;
  s_alarm_dirty = true;                 // push one more frame to clear the overlay
  if (alarm_scr) { lv_obj_del(alarm_scr); alarm_scr = nullptr; }
  // If the Timer app is the screen under the overlay, it was built in RUN mode
  // (Pause/Cancel) for a countdown that no longer exists — rebuild it into set
  // mode, same as the Cancel button does.
  if (fired_timer && nav_current == app_open_timer) app_open_timer();
  // Same idea for the Alarm screens (list or editor): a fired one-shot switched
  // itself off, a repeat re-armed — rebuild so toggles + hints show the truth.
  if (s_alarm_from_clock &&
      (nav_current == app_open_alarmclock || nav_current == app_open_alarmedit))
    nav_current();
  s_alarm_from_clock = false;
}

static void alarm_click_cb(lv_event_t *e) {
  (void)e;
  if (alarm_touch_dismiss()) alarm_dismiss();
}

/* Raise the alarm overlay with a custom title + subtitle. `fromTimer` records
 * whether dismissing should cancel the timer. Modal overlay + looping heartbeat
 * buzz + sound hook. Safe to call repeatedly (no-op once active). LVGL-thread only. */
static void alarm_fire_ex(const char *title, const char *subtitle, bool fromTimer) {
  if (g_alarm_active) return;
  g_alarm_active = true;
  s_alarm_from_timer = fromTimer;
  s_alarm_from_clock = false;            // almclk_fire() re-sets this after the call
  s_alarm_dirty = true;
  haptics_play(HAPTICS_HEARTBEAT, true);
  alarm_sound_start();

  alarm_scr = lv_obj_create(lv_layer_sys());   // above apps, menu AND the shade
  lv_obj_remove_style_all(alarm_scr);
  lv_obj_set_size(alarm_scr, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(alarm_scr, lv_color_hex(0x2A0A0A), 0);   // deep red
  lv_obj_set_style_bg_opa(alarm_scr, LV_OPA_COVER, 0);
  lv_obj_add_flag(alarm_scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(alarm_scr, LV_OBJ_FLAG_SCROLLABLE);
  if (alarm_touch_dismiss())
    lv_obj_add_event_cb(alarm_scr, alarm_click_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *ic = lv_label_create(alarm_scr);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(0xFF4D4D), 0);
  lv_label_set_text(ic, LV_SYMBOL_BELL);
  lv_obj_align(ic, LV_ALIGN_CENTER, 0, -96);

  lv_obj_t *t = lv_label_create(alarm_scr);
  lv_obj_set_style_text_font(t, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -36);

  lv_obj_t *d = lv_label_create(alarm_scr);
  lv_obj_set_style_text_font(d, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(d, lv_color_hex(0xCCAAAA), 0);
  lv_label_set_text(d, subtitle);
  lv_obj_align(d, LV_ALIGN_CENTER, 0, 6);

  lv_obj_t *h = lv_label_create(alarm_scr);
  lv_obj_set_style_text_font(h, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(h, lv_color_hex(0x888888), 0);
  lv_label_set_text(h, alarm_touch_dismiss() ? "Tap or press BOOT to dismiss"
                                             : "Press BOOT to dismiss");
  lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -44);
}

/* Timer alarm: "Time's up!" + the timer's duration. Dismiss cancels the timer. */
static void alarm_fire(void) {
  char b[24]; tmr_fmt(timer_duration_s(), b, sizeof b);
  char sub[40]; snprintf(sub, sizeof sub, "Timer  %s", b);
  alarm_fire_ex("Time's up!", sub, true);
}

/* Alarm-clock ring. State advances BEFORE the overlay goes up (a one-shot turns
 * itself off, a repeat re-arms for the next selected day), so even a crash or
 * battery death mid-ring can't make it fire again on the next boot. If several
 * alarms are due at once, the earliest rings now and the loop rings the rest
 * one by one as each overlay is dismissed. */
static void almclk_fire(void) {
  if (g_alarm_active) return;
  int i = almclk_due_index();
  if (i < 0) { almclk_rearm(); return; }   // stale NVS master (e.g. the CSV was
                                           // edited on a PC) — self-heal and move on
  uint8_t h = s_almcs[i].h, m = s_almcs[i].m;
  almclk_fired(i);
  char sub[24]; snprintf(sub, sizeof sub, "Alarm  %02u:%02u", h, m);
  alarm_fire_ex("Wake up!", sub, false);   // not a countdown: dismiss cancels nothing
  s_alarm_from_clock = true;
}

/* ============================== Timer UI ================================= */

static uint32_t  s_tmr_set_s   = 300;     // staged duration while setting (seconds)
static lv_obj_t *tmr_big       = nullptr; // big H:MM:SS label
static lv_obj_t *tmr_bar       = nullptr; // progress bar (run mode only)
static lv_timer_t *tmr_lv_tmr  = nullptr; // live refresh timer

static void tmr_refresh(void) {
  if (!tmr_big) return;
  uint32_t rem = s_tmr_active ? timer_remaining_s() : s_tmr_set_s;
  char b[16]; tmr_fmt(rem, b, sizeof b);
  lv_label_set_text(tmr_big, b);
  // Big clock font fits MM:SS; fall back to the 28px font once hours appear.
  lv_obj_set_style_text_font(tmr_big, rem >= 3600 ? &FONT_LABEL : &FONT_TIME, 0);
  if (tmr_bar) {
    uint32_t dur = s_tmr_duration ? s_tmr_duration : 1;
    int pct = (int)((uint64_t)rem * 100u / dur);
    if (pct > 100) pct = 100;
    lv_bar_set_value(tmr_bar, pct, LV_ANIM_OFF);
  }
}

static void tmr_lv_cb(lv_timer_t *t) { (void)t; tmr_refresh(); }

static void tmr_rep_stop(void);          // fwd: stop the hold-repeat timer (defined below)

static void tmr_cleanup_cb(lv_event_t *e) {
  (void)e;
  if (tmr_lv_tmr) { lv_timer_del(tmr_lv_tmr); tmr_lv_tmr = nullptr; }
  tmr_rep_stop();                        // a button held while the screen is torn down
  tmr_big = nullptr;
  tmr_bar = nullptr;
}

/* ---- editing the staged duration (set mode) ---- */
static void tmr_stage_add(int32_t d) {
  int64_t v = (int64_t)s_tmr_set_s + d;
  if (v < 0) v = 0;
  if (v > 359999) v = 359999;            // ~99h cap
  s_tmr_set_s = (uint32_t)v;
}

/* ---- press-and-hold auto-repeat with a gradual acceleration curve ----
 * A tap on a +/- button adds its step once. HOLDING it keeps adding, starting
 * slow and smoothly speeding up the longer you hold, up to a fast cap — so a
 * brief hold nudges the value and a long hold races through minutes.
 *
 * We drive this ourselves (one shared lv_timer) rather than LVGL's fixed-rate
 * LONG_PRESSED_REPEAT, because that repeats at a constant interval and can't
 * accelerate. Only one button can be held at once on a touchscreen, so a single
 * timer + "active step" is enough. */
#define TMR_REP_START_MS  500     // gap before/at the first auto-repeat (slow)
#define TMR_REP_MIN_MS    55      // fastest gap once fully ramped up
#define TMR_REP_RAMP_MS   2500    // hold time over which it ramps START -> MIN

static int32_t    s_tmr_rep_step  = 0;       // step (±sec) of the held button (0 = none)
static uint32_t   s_tmr_rep_t0    = 0;       // millis() when the press began
static lv_timer_t *s_tmr_rep_tmr  = nullptr; // the accelerating repeat timer

/* Map hold-time (ms) to the next repeat gap (ms). Quadratic ease-in: gaps shrink
 * slowly at first, then faster, reaching TMR_REP_MIN_MS at TMR_REP_RAMP_MS. */
static uint32_t tmr_rep_gap(uint32_t held_ms) {
  if (held_ms >= TMR_REP_RAMP_MS) return TMR_REP_MIN_MS;
  // p in [0,1] across the ramp; ease = p^2 (gentle start, accelerating).
  float p = (float)held_ms / (float)TMR_REP_RAMP_MS;
  float ease = p * p;
  return (uint32_t)(TMR_REP_START_MS - ease * (TMR_REP_START_MS - TMR_REP_MIN_MS));
}

static void tmr_rep_stop(void) {
  if (s_tmr_rep_tmr) { lv_timer_del(s_tmr_rep_tmr); s_tmr_rep_tmr = nullptr; }
  s_tmr_rep_step = 0;
}

/* Repeat tick: apply the step, buzz a tiny tick, and re-arm the timer at the
 * current (shrinking) gap for the elapsed hold time. */
static void tmr_rep_cb(lv_timer_t *t) {
  (void)t;
  if (s_tmr_rep_step == 0 || !tmr_big) { tmr_rep_stop(); return; }
  tmr_stage_add(s_tmr_rep_step);
  haptics_pulse(6);                      // light tick so a fast ramp still feels discrete
  tmr_refresh();
  lv_timer_set_period(s_tmr_rep_tmr, tmr_rep_gap(millis() - s_tmr_rep_t0));
}

/* Press edge: apply ONE step immediately (so a tap works), then arm the repeat
 * timer at the slow starting gap. */
static void tmr_adjust_press_cb(lv_event_t *e) {
  int32_t step = (int32_t)(intptr_t)lv_event_get_user_data(e);
  tmr_stage_add(step);
  haptics_pulse(12);
  tmr_refresh();
  tmr_rep_stop();                        // clear any stale hold (defensive)
  s_tmr_rep_step = step;
  s_tmr_rep_t0   = millis();
  s_tmr_rep_tmr  = lv_timer_create(tmr_rep_cb, TMR_REP_START_MS, nullptr);
}

/* Release / press-lost / delete: end the hold. */
static void tmr_adjust_release_cb(lv_event_t *e) { (void)e; tmr_rep_stop(); }
static void tmr_start_cb(lv_event_t *e) {
  (void)e;
  if (s_tmr_set_s == 0) return;
  timer_start(s_tmr_set_s);
  haptics_pulse(25);
  app_open_timer();                      // rebuild into run mode
}
static void tmr_pauseresume_cb(lv_event_t *e) {
  (void)e;
  if (timer_is_paused()) timer_resume(); else timer_pause();
  app_open_timer();
}
static void tmr_addmin_cb(lv_event_t *e) {
  (void)e;
  timer_adjust(60);
  tmr_refresh();
}
static void tmr_cancel_cb(lv_event_t *e) {
  (void)e;
  timer_cancel();
  app_open_timer();                      // rebuild into set mode
}
static void tmr_open_sw_cb(lv_event_t *e)   { (void)e; nav_open(app_open_stopwatch); }
static void tmr_open_almc_cb(lv_event_t *e) { (void)e; nav_open(app_open_alarmclock); }
static void tmr_touch_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  alarm_set_touch_dismiss(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* A +/- adjust pill that AUTO-REPEATS on hold (gradual acceleration). Unlike
 * tmr_btn (single CLICKED), this wires the press/release edges that drive the
 * repeat timer: PRESSED applies one step + arms the ramp, RELEASED/PRESS_LOST
 * stop it. `step` is the signed seconds delta carried in user_data. */
static lv_obj_t *tmr_adjust_btn(lv_obj_t *parent, const char *txt, int32_t step,
                                int w) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, 52);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1F1F1F), 0);
  void *ud = (void *)(intptr_t)step;
  lv_obj_add_event_cb(b, tmr_adjust_press_cb,   LV_EVENT_PRESSED,    ud);
  lv_obj_add_event_cb(b, tmr_adjust_release_cb, LV_EVENT_RELEASED,   ud);
  lv_obj_add_event_cb(b, tmr_adjust_release_cb, LV_EVENT_PRESS_LOST, ud);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

/* A row container inside a flex column. */
static lv_obj_t *tmr_row(lv_obj_t *col) {
  lv_obj_t *r = lv_obj_create(col);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_height(r, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_pad_all(r, 0, 0);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 8, 0);
  return r;
}

static void app_open_timer(void) {
  app_screen_begin("Timer");

  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 374, 410);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 80);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 12, 0);

  tmr_bar = nullptr;   // only created in run mode below; refresh guards on it

  // Big time readout (shared by both modes).
  tmr_big = lv_label_create(col);
  lv_obj_set_style_text_font(tmr_big, &FONT_TIME, 0);
  lv_obj_set_style_text_color(tmr_big, lv_color_white(), 0);
  lv_label_set_text(tmr_big, "00:00");

  if (!s_tmr_active) {
    // -------- SET MODE --------
    s_tmr_set_s = s_tmr_duration ? s_tmr_duration : 300;

    // Adjust buttons (no presets): tap to nudge, HOLD to auto-repeat with a
    // gradual ramp — a brief hold adds a little, a long hold races through.
    lv_obj_t *a1 = tmr_row(col);
    tmr_adjust_btn(a1, "-1m",  -60, 80);
    tmr_adjust_btn(a1, "+1m",   60, 80);
    lv_obj_t *a2 = tmr_row(col);
    tmr_adjust_btn(a2, "-10s", -10, 80);
    tmr_adjust_btn(a2, "+10s",  10, 80);

    // Start (accent).
    tmr_btn(col, LV_SYMBOL_PLAY "  Start", tmr_start_cb, nullptr, ui_accent_hex(), 220);
  } else {
    // -------- RUN MODE --------
    tmr_bar = lv_bar_create(col);
    lv_obj_set_size(tmr_bar, 320, 10);
    lv_obj_set_style_radius(tmr_bar, 5, 0);
    lv_obj_set_style_bg_color(tmr_bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_color(tmr_bar, lv_color_hex(ui_accent_hex()), LV_PART_INDICATOR);
    lv_bar_set_range(tmr_bar, 0, 100);

    lv_obj_t *r1 = tmr_row(col);
    tmr_btn(r1, timer_is_paused() ? LV_SYMBOL_PLAY "  Resume" : LV_SYMBOL_PAUSE "  Pause",
            tmr_pauseresume_cb, nullptr, ui_accent_hex(), 160);
    tmr_btn(r1, "+1:00", tmr_addmin_cb, nullptr, 0x1F1F1F, 96);

    tmr_btn(col, LV_SYMBOL_TRASH "  Cancel", tmr_cancel_cb, nullptr, 0x5A1A1A, 220);
  }

  // Touch-dismiss toggle (alarm can be silenced by a tap, not only BOOT).
  lv_obj_t *tr = tmr_row(col);
  lv_obj_t *tl = lv_label_create(tr);
  lv_obj_set_style_text_font(tl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(tl, lv_color_hex(0xCCCCCC), 0);
  lv_label_set_text(tl, "Touch can dismiss alarm");
  lv_obj_t *sw = lv_switch_create(tr);
  lv_obj_set_style_bg_color(sw, lv_color_hex(ui_accent_hex()),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (alarm_touch_dismiss()) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, tmr_touch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  // Jump to the stopwatch / alarm clock.
  lv_obj_t *jr = tmr_row(col);
  tmr_btn(jr, LV_SYMBOL_LOOP "  Stopwatch", tmr_open_sw_cb, nullptr, 0x1F1F1F, 168);
  tmr_btn(jr, LV_SYMBOL_BELL "  Alarm", tmr_open_almc_cb, nullptr, 0x1F1F1F, 130);

  tmr_refresh();
  tmr_lv_tmr = lv_timer_create(tmr_lv_cb, 500, nullptr);
  lv_obj_add_event_cb(app_scr, tmr_cleanup_cb, LV_EVENT_DELETE, nullptr);
}

/* ============================ Stopwatch UI =============================== */

static bool      sw_running   = false;
static uint32_t  sw_start_ms  = 0;        // millis() at last start
static uint32_t  sw_accum_ms  = 0;        // accumulated while paused
static uint32_t  sw_laps[20];
static uint8_t   sw_lap_n     = 0;
static lv_obj_t *sw_big       = nullptr;
static lv_obj_t *sw_lap_box   = nullptr;
static lv_obj_t *sw_pp_btn    = nullptr;  // play/pause button label
static lv_timer_t *sw_lv_tmr  = nullptr;

static uint32_t sw_elapsed_ms(void) {
  return sw_accum_ms + (sw_running ? (millis() - sw_start_ms) : 0);
}
static void sw_fmt(uint32_t ms, char *buf, size_t n) {
  uint32_t m = ms / 60000, s = (ms / 1000) % 60, cs = (ms / 10) % 100;
  snprintf(buf, n, "%02u:%02u.%02u", m, s, cs);
}

static void sw_refresh(void) {
  if (!sw_big) return;
  char b[16]; sw_fmt(sw_elapsed_ms(), b, sizeof b);
  lv_label_set_text(sw_big, b);
}
static void sw_lv_cb(lv_timer_t *t) { (void)t; sw_refresh(); }
static void sw_cleanup_cb(lv_event_t *e) {
  (void)e;
  if (sw_lv_tmr) { lv_timer_del(sw_lv_tmr); sw_lv_tmr = nullptr; }
  sw_big = nullptr; sw_lap_box = nullptr; sw_pp_btn = nullptr;
}

static void sw_rebuild_laps(void) {
  if (!sw_lap_box) return;
  lv_obj_clean(sw_lap_box);                // clear then re-add (lap count is small)
  for (uint8_t i = 0; i < sw_lap_n; i++) {
    lv_obj_t *l = lv_label_create(sw_lap_box);
    lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xCCCCCC), 0);
    char b[16]; sw_fmt(sw_laps[i], b, sizeof b);
    lv_label_set_text_fmt(l, "Lap %u    %s", i + 1, b);
  }
}

static void sw_startstop_cb(lv_event_t *e) {
  (void)e;
  if (sw_running) {                        // -> stop/pause
    sw_accum_ms += millis() - sw_start_ms;
    sw_running = false;
  } else {                                 // -> start/resume
    sw_start_ms = millis();
    sw_running = true;
  }
  haptics_pulse(15);
  if (sw_pp_btn)
    lv_label_set_text(sw_pp_btn, sw_running ? LV_SYMBOL_PAUSE "  Stop"
                                            : LV_SYMBOL_PLAY  "  Start");
}
static void sw_lap_cb(lv_event_t *e) {
  (void)e;
  if (sw_running && sw_lap_n < 20) {
    sw_laps[sw_lap_n++] = sw_elapsed_ms();
    haptics_pulse(12);
    sw_rebuild_laps();
  }
}
static void sw_reset_cb(lv_event_t *e) {
  (void)e;
  sw_running = false; sw_accum_ms = 0; sw_lap_n = 0;
  haptics_pulse(15);
  sw_refresh();
  sw_rebuild_laps();
}

static void app_open_stopwatch(void) {
  app_screen_begin("Stopwatch");

  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 374, 410);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 12, 0);

  sw_big = lv_label_create(col);
  lv_obj_set_style_text_font(sw_big, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(sw_big, lv_color_white(), 0);
  lv_label_set_text(sw_big, "00:00.00");

  lv_obj_t *r = tmr_row(col);
  lv_obj_t *ssb = tmr_btn(r, sw_running ? LV_SYMBOL_PAUSE "  Stop"
                                        : LV_SYMBOL_PLAY  "  Start",
                          sw_startstop_cb, nullptr, ui_accent_hex(), 150);
  sw_pp_btn = lv_obj_get_child(ssb, 0);    // its label, so we can flip the text
  tmr_btn(r, "Lap", sw_lap_cb, nullptr, 0x1F1F1F, 96);
  tmr_btn(col, LV_SYMBOL_TRASH "  Reset", sw_reset_cb, nullptr, 0x5A1A1A, 220);

  // Lap list (scrolls within the column).
  sw_lap_box = lv_obj_create(col);
  lv_obj_set_width(sw_lap_box, LV_PCT(100));
  lv_obj_set_height(sw_lap_box, 150);
  lv_obj_set_style_bg_opa(sw_lap_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sw_lap_box, 0, 0);
  lv_obj_set_flex_flow(sw_lap_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sw_lap_box, 4, 0);
  sw_rebuild_laps();

  sw_refresh();
  sw_lv_tmr = lv_timer_create(sw_lv_cb, 50, nullptr);   // smooth centiseconds
  lv_obj_add_event_cb(app_scr, sw_cleanup_cb, LV_EVENT_DELETE, nullptr);
}

/* =========================== Alarm clock UI ===============================
 * TWO screens. app_open_alarmclock = the LIST: one row per alarm (time, days
 * summary, on/off switch), tap a row to edit, "+ Add alarm" appends. The list
 * itself lives in /alarms.csv — see timer_store.h. app_open_alarmedit = the
 * EDITOR for s_almc_edit: HH:MM steppers, repeat + weekday picks, Delete.
 * Ringing reuses the countdown's overlay/chime via almclk_fire(). */

static int       s_almc_edit   = -1;       // index being edited (editor screen)
static lv_obj_t *almc_time_lbl = nullptr;  // editor: big HH:MM
static lv_obj_t *almc_next_lbl = nullptr;  // editor/list: "rings in" hint

/* The alarm under edit, or null if the index went stale. */
static AlmClk *almc_ed(void) {
  return (s_almc_edit >= 0 && s_almc_edit < s_almc_n) ? &s_almcs[s_almc_edit]
                                                      : nullptr;
}

/* "Once" / "Every day" / "Mon-Fri" / "Mo We Fr" — the list row's summary. */
static void almc_days_str(const AlmClk *a, char *buf, size_t n) {
  if (!a->rep)          { strlcpy(buf, "Once", n);      return; }
  if (a->days == 0x7F)  { strlcpy(buf, "Every day", n); return; }
  if (a->days == 0x1F)  { strlcpy(buf, "Mon-Fri", n);   return; }
  if (a->days == 0)     { strlcpy(buf, "No days", n);   return; }
  static const char *DN[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
  buf[0] = '\0';
  for (int d = 0; d < 7; d++)
    if (a->days & (1 << d)) { strlcat(buf, DN[d], n); strlcat(buf, " ", n); }
}

static void almc_fmt_in(char *buf, size_t n, uint32_t target) {
  uint32_t now = rtc_now_epoch();
  uint32_t in  = (target > now) ? target - now : 0;
  uint32_t h = in / 3600, m = (in % 3600) / 60;
  if (h) snprintf(buf, n, "Rings in %luh %02lum", (unsigned long)h, (unsigned long)m);
  else   snprintf(buf, n, "Rings in %lum", (unsigned long)(m ? m : 1));
}

/* Editor refresh: the big HH:MM + this alarm's own "rings in" hint. */
static void almc_refresh(void) {
  AlmClk *a = almc_ed();
  if (!almc_time_lbl || !a) return;
  lv_label_set_text_fmt(almc_time_lbl, "%02u:%02u", a->h, a->m);
  if (!almc_next_lbl) return;
  if (!a->en)         { lv_label_set_text(almc_next_lbl, "Alarm off"); return; }
  if (!a->target)     { lv_label_set_text(almc_next_lbl, "No repeat days selected"); return; }
  char b[40]; almc_fmt_in(b, sizeof b, a->target);
  lv_label_set_text(almc_next_lbl, b);
}

/* List-screen hint: the soonest ring across ALL alarms. */
static void almc_list_hint(void) {
  if (!almc_next_lbl) return;
  uint32_t in = almclk_seconds_until();
  if (s_almc_n == 0)  { lv_label_set_text(almc_next_lbl, "No alarms yet"); return; }
  if (in == 0)        { lv_label_set_text(almc_next_lbl, "All alarms off"); return; }
  char b[40]; almc_fmt_in(b, sizeof b, s_almc_master);
  lv_label_set_text(almc_next_lbl, b);
}

/* Tap or hold-repeat a +/- pill: shift HH:MM by the step (minutes), wrapping
 * around the day. NVS is only written on release (almc_commit_cb), so a held
 * repeat doesn't hammer flash. */
static void almc_step_cb(lv_event_t *e) {
  AlmClk *a = almc_ed();
  if (!a) return;
  int32_t d = (int32_t)(intptr_t)lv_event_get_user_data(e);
  int32_t mins = (int32_t)a->h * 60 + a->m + d;
  mins %= 1440; if (mins < 0) mins += 1440;
  a->h = (uint8_t)(mins / 60);
  a->m = (uint8_t)(mins % 60);
  haptics_pulse(8);
  almc_refresh();
}
static void almc_commit_cb(lv_event_t *e) {
  (void)e;
  almclk_save_csv();
  almclk_rearm();
  almc_refresh();           // "rings in" needs the re-armed target
}
static lv_obj_t *almc_step_btn(lv_obj_t *parent, const char *txt, int32_t step_min) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 80, 52);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1F1F1F), 0);
  void *ud = (void *)(intptr_t)step_min;
  lv_obj_add_event_cb(b, almc_step_cb, LV_EVENT_SHORT_CLICKED,       ud);
  lv_obj_add_event_cb(b, almc_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, ud);
  lv_obj_add_event_cb(b, almc_commit_cb, LV_EVENT_RELEASED,   nullptr);
  lv_obj_add_event_cb(b, almc_commit_cb, LV_EVENT_PRESS_LOST, nullptr);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

static void almc_enable_cb(lv_event_t *e) {
  AlmClk *a = almc_ed();
  if (!a) return;
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  a->en = lv_obj_has_state(sw, LV_STATE_CHECKED);
  almclk_save_csv();
  almclk_rearm();
  almc_refresh();
}

static void almc_repeat_cb(lv_event_t *e) {
  AlmClk *a = almc_ed();
  if (!a) return;
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  a->rep = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (a->rep && a->days == 0) a->days = 0x7F;  // fresh repeat: every day
  almclk_save_csv();
  almclk_rearm();
  app_open_alarmedit();     // rebuild: the weekday row appears/disappears
}

static void almc_day_cb(lv_event_t *e) {
  AlmClk *a = almc_ed();
  if (!a) return;
  lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
  uint8_t bit = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  if (lv_obj_has_state(b, LV_STATE_CHECKED)) a->days |=  (1 << bit);
  else                                       a->days &= ~(1 << bit);
  haptics_pulse(8);
  almclk_save_csv();
  almclk_rearm();
  almc_refresh();
}

static void almc_delete_cb(lv_event_t *e) {
  (void)e;
  almclk_remove(s_almc_edit);
  s_almc_edit = -1;
  almclk_save_csv();
  almclk_rearm();
  haptics_pulse(15);
  nav_back();               // back to the list, rebuilt without this alarm
}

static void almc_cleanup_cb(lv_event_t *e) {
  (void)e;
  almc_time_lbl = nullptr;
  almc_next_lbl = nullptr;
}

/* ---- the list screen ---- */

static void almc_row_click_cb(lv_event_t *e) {
  s_almc_edit = (int)(intptr_t)lv_event_get_user_data(e);
  nav_open(app_open_alarmedit);
}
static void almc_row_sw_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_almc_n) return;
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  s_almcs[i].en = lv_obj_has_state(sw, LV_STATE_CHECKED);
  almclk_save_csv();
  almclk_rearm();
  almc_list_hint();
}
static void almc_add_cb(lv_event_t *e) {
  (void)e;
  int i = almclk_add();
  if (i < 0) return;                       // full (button is hidden then anyway)
  almclk_save_csv();
  almclk_rearm();
  s_almc_edit = i;
  nav_open(app_open_alarmedit);            // edit the fresh 07:00 alarm right away
}

static void app_open_alarmclock(void) {
  app_screen_begin("Alarms");

  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 374, 410);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 80);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 8, 0);     // scrolls if 8 alarms overflow it

  almc_time_lbl = nullptr;                 // list screen has no editor widgets
  almc_next_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(almc_next_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(almc_next_lbl, lv_color_hex(0x999999), 0);

  for (int i = 0; i < s_almc_n; i++) {
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161616), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, almc_row_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);

    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(t, s_almcs[i].en ? lv_color_white()
                                                 : lv_color_hex(0x777777), 0);
    lv_label_set_text_fmt(t, "%02u:%02u", s_almcs[i].h, s_almcs[i].m);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, -2);

    char ds[32]; almc_days_str(&s_almcs[i], ds, sizeof ds);
    lv_obj_t *d = lv_label_create(row);
    lv_obj_set_style_text_font(d, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(0x999999), 0);
    lv_label_set_text(d, ds);
    lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 4, 2);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(ui_accent_hex()),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_almcs[i].en) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(sw, almc_row_sw_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)i);
  }

  if (s_almc_n < ALMC_MAX)
    tmr_btn(col, LV_SYMBOL_PLUS "  Add alarm", almc_add_cb, nullptr,
            ui_accent_hex(), 220);

  almc_list_hint();
  lv_obj_add_event_cb(app_scr, almc_cleanup_cb, LV_EVENT_DELETE, nullptr);
}

/* ---- the per-alarm editor screen ---- */

static void app_open_alarmedit(void) {
  AlmClk *a = almc_ed();
  if (!a) { nav_back(); return; }          // stale index — bail to the list

  app_screen_begin("Alarm");

  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 374, 410);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 80);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 10, 0);

  almc_time_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(almc_time_lbl, &FONT_TIME, 0);
  lv_obj_set_style_text_color(almc_time_lbl, lv_color_white(), 0);

  almc_next_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(almc_next_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(almc_next_lbl, lv_color_hex(0x999999), 0);

  lv_obj_t *a1 = tmr_row(col);
  almc_step_btn(a1, "-1h", -60);
  almc_step_btn(a1, "+1h",  60);
  lv_obj_t *a2 = tmr_row(col);
  almc_step_btn(a2, "-5m",  -5);
  almc_step_btn(a2, "+5m",   5);

  // On/off switch.
  lv_obj_t *er = tmr_row(col);
  lv_obj_t *el = lv_label_create(er);
  lv_obj_set_style_text_font(el, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(el, lv_color_hex(0xCCCCCC), 0);
  lv_label_set_text(el, "Alarm enabled");
  lv_obj_t *esw = lv_switch_create(er);
  lv_obj_set_style_bg_color(esw, lv_color_hex(ui_accent_hex()),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (a->en) lv_obj_add_state(esw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(esw, almc_enable_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  // Repeat switch; checked reveals the weekday row below.
  lv_obj_t *rr = tmr_row(col);
  lv_obj_t *rl = lv_label_create(rr);
  lv_obj_set_style_text_font(rl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(rl, lv_color_hex(0xCCCCCC), 0);
  lv_label_set_text(rl, "Repeat weekly");
  lv_obj_t *rsw = lv_switch_create(rr);
  lv_obj_set_style_bg_color(rsw, lv_color_hex(ui_accent_hex()),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (a->rep) lv_obj_add_state(rsw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(rsw, almc_repeat_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  if (a->rep) {
    // One checkable pill per weekday (bit0=Mon .. bit6=Sun).
    static const char *DN[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    lv_obj_t *dr = tmr_row(col);
    lv_obj_set_style_pad_column(dr, 4, 0);
    for (int d = 0; d < 7; d++) {
      lv_obj_t *b = lv_btn_create(dr);
      lv_obj_set_size(b, 44, 40);
      lv_obj_set_style_radius(b, 10, 0);
      lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
      lv_obj_set_style_bg_color(b, lv_color_hex(0x1F1F1F), 0);
      lv_obj_set_style_bg_color(b, lv_color_hex(ui_accent_hex()), LV_STATE_CHECKED);
      if (a->days & (1 << d)) lv_obj_add_state(b, LV_STATE_CHECKED);
      lv_obj_add_event_cb(b, almc_day_cb, LV_EVENT_VALUE_CHANGED,
                          (void *)(intptr_t)d);
      lv_obj_t *l = lv_label_create(b);
      lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(l, lv_color_white(), 0);
      lv_label_set_text(l, DN[d]);
      lv_obj_center(l);
    }
  }

  tmr_btn(col, LV_SYMBOL_TRASH "  Delete", almc_delete_cb, nullptr, 0x5A1A1A, 220);

  almc_refresh();
  lv_obj_add_event_cb(app_scr, almc_cleanup_cb, LV_EVENT_DELETE, nullptr);
}
