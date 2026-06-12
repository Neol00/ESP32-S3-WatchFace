/* ============================================================================
 *  app_power.h — Power Manager sub-app: a scrollable "vitals" dashboard.
 *
 *  Redesigned to match the About screen's look: one scrollable flex column with
 *  accent-colored section headers and tidy, left-aligned stat lines. On top of
 *  that it adds three live line-graph cards (battery %, power draw mA, battery
 *  voltage) styled like a system monitor — a dark rounded card with a dim title,
 *  a big blue current-value, and a scrolling chart. Everything refreshes on one
 *  lv_timer every 5 s; pm_cleanup_cb tears the timer down on screen delete so it
 *  can never touch freed objects.
 *
 *  Header-only module compiled into the .ino TU, so it shares the menu statics.
 *  INCLUDE AFTER app_menu.h. Reads WatchFace.ino globals: power (XPowersPMU) /
 *  pmu_ok, power_estimate_ma/mw, drain_*, runtime_*, health_*, calib_*,
 *  core_* / clocks_format, CPU_FREQS[]/CPU_FREQ_COUNT, settings_get/set_cpu_mhz,
 *  FONT_* macros.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* --- palette (shared with About) ---
 * The accent (headers + graph line + live values) is now the user-tunable accent
 * color: ui_accent_soft_hex() for headers, ui_accent_hex() for the brighter line. */
#define PM_CARD_BG     0x141414   // graph-card / row background
#define PM_CARD_BORDER 0x262626   // graph-card / row border
#define PM_DIM         0x888888   // captions / units
#define PM_VAL         0xDDDDDD   // stat values

/* Refresh cadence. The timer ticks at PM_TEXT_MS to keep the text blocks (charging
 * state, gauge, runtime) responsive. The GRAPHS push a new point far less often —
 * once every PM_GRAPH_EVERY ticks — so each chart spans a long, readable window
 * instead of scrolling quickly. At 5 s text / every 2nd tick that's a 10 s/point
 * graph (~5 min across 30 points). */
#define PM_TEXT_MS      5000      // text refresh period (ms)
#define PM_GRAPH_EVERY  2         // push a graph point every Nth text tick
#define PM_GRAPH_POINTS 30        // points per chart (~5 min at 10 s/point)

static lv_obj_t *pm_freq_btns[8];   // up to 8 freq buttons (we use CPU_FREQ_COUNT)

static void app_open_power(void);   // fwd (rebuilt when frequency changes)

/* ---- one live line-graph card ----
 * card: dark rounded panel. Inside: a dim title (top-left), a big blue current
 * value (top-right), and a scrolling line chart filling the bottom. */
typedef struct {
  lv_obj_t         *chart;
  lv_chart_series_t *ser;
  lv_obj_t         *val;     // big current-value label (top-right)
} pm_graph_t;

static pm_graph_t pm_g_batt;   // battery level (%)
static pm_graph_t pm_g_draw;   // power draw (mA)

/* CPU usage graph: one chart, TWO series (core 0 + core 1), 0..100 %. The two
 * live values sit top-right ("c0 / c1"). */
typedef struct {
  lv_obj_t          *chart;
  lv_chart_series_t *ser0;   // core 0 (the WiFi/net core)
  lv_chart_series_t *ser1;   // core 1 (the UI/loop core)
  lv_obj_t          *val;    // "c0% / c1%" label
} pm_cpu_graph_t;
static pm_cpu_graph_t pm_g_cpu;

/* Build a graph card into `parent` (a flex column). Returns it via *out. */
static void pm_make_graph(lv_obj_t *parent, const char *title,
                          int ymin, int ymax, pm_graph_t *out) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, 132);
  lv_obj_set_style_bg_color(card, lv_color_hex(PM_CARD_BG), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(PM_CARD_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ttl = lv_label_create(card);
  lv_obj_set_style_text_font(ttl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(ttl, lv_color_hex(PM_DIM), 0);
  lv_label_set_text(ttl, title);
  lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 2);

  out->val = lv_label_create(card);
  lv_obj_set_style_text_font(out->val, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(out->val, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(out->val, "--");
  lv_obj_align(out->val, LV_ALIGN_TOP_RIGHT, 0, -2);

  lv_obj_t *chart = lv_chart_create(card);
  lv_obj_set_size(chart, LV_PCT(100), 72);
  lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, PM_GRAPH_POINTS);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(chart, 3, 0);
  // styling: transparent bg, faint grid, 2px blue line, no point dots.
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_line_color(chart, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);

  out->chart = chart;
  out->ser   = lv_chart_add_series(chart, lv_color_hex(ui_accent_hex()),
                                   LV_CHART_AXIS_PRIMARY_Y);
}

/* CPU usage graph card: like pm_make_graph but with TWO series (core 0 = accent,
 * core 1 = amber) sharing a fixed 0..100 % axis, and a "c0 / c1" value label. A
 * tiny legend under the title names the colors. */
static void pm_make_cpu_graph(lv_obj_t *parent, pm_cpu_graph_t *out) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, 132);
  lv_obj_set_style_bg_color(card, lv_color_hex(PM_CARD_BG), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(PM_CARD_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ttl = lv_label_create(card);
  lv_obj_set_style_text_font(ttl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(ttl, lv_color_hex(PM_DIM), 0);
  // Color the legend tokens to match the two series (LVGL recolor markup).
  lv_label_set_recolor(ttl, true);
  // Legend tokens match the two series colors; keep them in lockstep with ser0/ser1
  // (ser1 = lightened accent in mono mode, amber otherwise).
  lv_label_set_text_fmt(ttl, "CPU  #%06X c0#  #%06X c1#",
                        (unsigned)(ui_accent_hex() & 0xFFFFFF),
                        (unsigned)((s_mono_accent ? ui_accent_soft_hex() : 0xC9A227) & 0xFFFFFF));
  lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 2);

  out->val = lv_label_create(card);
  lv_obj_set_style_text_font(out->val, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(out->val, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(out->val, "--");
  lv_obj_align(out->val, LV_ALIGN_TOP_RIGHT, 0, -2);

  lv_obj_t *chart = lv_chart_create(card);
  lv_obj_set_size(chart, LV_PCT(100), 72);
  lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, PM_GRAPH_POINTS);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_line_color(chart, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);

  out->chart = chart;
  out->ser0  = lv_chart_add_series(chart, lv_color_hex(ui_accent_hex()),
                                   LV_CHART_AXIS_PRIMARY_Y);
  // Second series: amber normally. In mono mode it must stay DISTINCT from ser0
  // (both lines on one chart), so use a lightened accent rather than collapsing to
  // the same accent — still monochrome-family, still tellable apart.
  out->ser1  = lv_chart_add_series(chart,
                   lv_color_hex(s_mono_accent ? ui_accent_soft_hex() : 0xC9A227),
                   LV_CHART_AXIS_PRIMARY_Y);
}

/* ---- text widgets (live, refreshed by the timer) ---- */
static lv_obj_t *pm_batt_lbl = nullptr;   // battery detail block
static lv_obj_t *pm_power_lbl = nullptr;  // power/runtime detail block
static lv_obj_t *pm_clk_lbl  = nullptr;   // core voltage + clock + OC block
static lv_timer_t *pm_timer  = nullptr;
static uint8_t   s_pm_graph_tick = 0;     // counts text ticks; pushes a graph point every PM_GRAPH_EVERY

/* DEEP SLEEP section widgets (live, updated in place by their callbacks). */
static lv_obj_t *pm_wake_sw  = nullptr;   // the periodic-wake switch (settings_toggle_row)
static lv_obj_t *pm_intv_lbl = nullptr;   // the "N min" value between the arrows
static lv_obj_t *pm_intv_row = nullptr;   // the stepper row (dimmed when wake is off)

/* DISPLAY section widgets (auto-dim on idle). */
static lv_obj_t *pm_dim_sw   = nullptr;   // the auto-dim switch (settings_toggle_row)
static lv_obj_t *pm_dimp_lbl = nullptr;   // the "N%" dim-level value
static lv_obj_t *pm_dimp_row = nullptr;   // the dim-level stepper row (dimmed when auto-dim off)
static lv_obj_t *pm_dimusb_sw = nullptr;  // the "dim on USB" switch (settings_toggle_row)

/* Set a settings_toggle_row's switch on/off without firing its VALUE_CHANGED cb. */
static void pm_sw_set(lv_obj_t *sw, bool on) {
  if (!sw) return;
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  else    lv_obj_clear_state(sw, LV_STATE_CHECKED);
}
/* Grey out (and disable interaction on) a toggle row by its switch. */
static void pm_row_set_enabled(lv_obj_t *sw, bool en) {
  if (!sw) return;
  lv_obj_t *row = lv_obj_get_parent(sw);
  if (row) lv_obj_set_style_opa(row, en ? LV_OPA_COVER : LV_OPA_50, 0);
  if (en) lv_obj_clear_state(sw, LV_STATE_DISABLED);
  else    lv_obj_add_state(sw, LV_STATE_DISABLED);
}

/* A left-aligned, wrapping detail line inside the scroll column. */
static lv_obj_t *pm_line(lv_obj_t *parent, const lv_font_t *font,
                         uint32_t color, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_label_set_text(l, text);
  return l;
}

/* A dim, spaced blue section header (e.g. "BATTERY") — matches About. */
static void pm_header(lv_obj_t *parent, const char *text) {
  lv_obj_t *h = pm_line(parent, &FONT_SMALL, ui_accent_soft_hex(), text);
  lv_obj_set_style_pad_top(h, 10, 0);
}

/* Push one new point to every graph (battery %, draw, voltage, per-core CPU) and
 * update each card's big current-value. Called LESS often than the text refresh —
 * once every PM_GRAPH_EVERY text ticks (see pm_timer_cb) — so the charts span a
 * long window. Cheap; safe when the PMU is absent (it just skips the PMU graphs). */
static void pm_update_graphs(void) {
  // Per-core CPU usage is independent of the PMU, so always latch + plot it.
  cpu_usage_sample();
  uint8_t c0 = cpu_usage_pct(0), c1 = cpu_usage_pct(1);
  if (pm_g_cpu.ser0) {
    lv_chart_set_next_value(pm_g_cpu.chart, pm_g_cpu.ser0, c0);
    lv_chart_set_next_value(pm_g_cpu.chart, pm_g_cpu.ser1, c1);
    lv_label_set_text_fmt(pm_g_cpu.val, "%u/%u%%", c0, c1);
  }

  if (!pmu_ok) return;
  int      pct  = power.getBatteryPercent();
  uint16_t vbat = power.getBattVoltage();   // mV
  uint16_t ma   = power_estimate_ma();
  if (pm_g_batt.ser) {
    lv_chart_set_next_value(pm_g_batt.chart, pm_g_batt.ser, pct < 0 ? 0 : pct);
    lv_label_set_text_fmt(pm_g_batt.val, "%d%%", pct);
  }
  if (pm_g_draw.ser) {
    lv_chart_set_next_value(pm_g_draw.chart, pm_g_draw.ser, ma);
    lv_label_set_text_fmt(pm_g_draw.val, "%u", ma);
  }
}

/* Refresh the three text detail blocks (battery / power / clock). Called on open
 * and every PM_TEXT_MS by pm_timer while the Power screen is visible. The graphs
 * are handled separately by pm_update_graphs() on a slower cadence. */
static void pm_update_labels(void) {
  if (!pm_batt_lbl || !pm_power_lbl || !pm_clk_lbl) return;
  if (!pmu_ok) { lv_label_set_text(pm_batt_lbl, "PMU not detected"); return; }

  int   pct  = power.getBatteryPercent();
  bool  chg  = power.isCharging();
  bool  vbus = power.isVbusIn();
  uint16_t vbat = power.getBattVoltage();   // mV
  uint16_t vsys = power.getVbusVoltage();   // mV (USB input)

  uint16_t ma  = power_estimate_ma();
  uint32_t mw  = power_estimate_mw(vbat);

  // --- BATTERY detail block ---
  char bb[200];
  int n = snprintf(bb, sizeof(bb),
      "Voltage:   %u.%02u V\n"
      "Charging:  %s\n"
      "USB power: %s",
      vbat / 1000, (vbat % 1000) / 10,
      chg ? "yes" : "no", vbus ? "yes" : "no");
  if (vbus && n > 0 && n < (int)sizeof(bb))
    n += snprintf(bb + n, sizeof(bb) - n, " (%u.%02u V)", vsys / 1000, (vsys % 1000) / 10);
  int hp = health_get_pct();   // running average over learned cycles; -1 = not learned
  if (n > 0 && n < (int)sizeof(bb)) {
    if (hp >= 0)
      snprintf(bb + n, sizeof(bb) - n, "\nHealth:    %d%% (%dmAh, %uc)",
               hp, (int)(health_get_avg_mah() + 0.5f), health_get_cycles());
    else
      snprintf(bb + n, sizeof(bb) - n, "\nHealth:    learning...");
  }
  lv_label_set_text(pm_batt_lbl, bb);

  // --- POWER / runtime block. The AXP2101 has NO current ADC, so "Draw/Power"
  //     are a MODEL (rough); the gauge-derived lines below are the REAL numbers. ---
  float hlc     = runtime_hours_left_capacity(pct);   // from learned/design mAh
  float real_ma = drain_avg_ma();                     // real fuel-gauge avg (0 = not yet)
  char pb[340];
  // Model lines show the gauge-learned correction factor once cycles have taught
  // it (x1.00 until then) — "Draw: 64 mA (model x0.87)" means the raw constants
  // overestimate by ~15% and the shown value is already corrected.
  int m;
  if (calib_get_k_samples() > 0) {
    float ks = calib_awake_scale();
    m = snprintf(pb, sizeof(pb),
        "Draw:    %u mA  (model x%d.%02d)\n"
        "Power:   %u.%02u W  (model)",
        ma, (int)ks, (int)((ks - (int)ks) * 100 + 0.5f),
        mw / 1000, (mw % 1000) / 10);
  } else {
    m = snprintf(pb, sizeof(pb),
        "Draw:    %u mA  (model)\n"
        "Power:   %u.%02u W  (model)",
        ma, mw / 1000, (mw % 1000) / 10);
  }
  // REAL average current from the fuel gauge — the only hardware-grounded value.
  if (m > 0 && m < (int)sizeof(pb)) {
    if (real_ma > 0.0f)
      m += snprintf(pb + m, sizeof(pb) - m, "\nGauge:   %d.%02d mA  (REAL avg)",
                    (int)real_ma, (int)((real_ma - (int)real_ma) * 100 + 0.5f));
    else
      m += snprintf(pb + m, sizeof(pb) - m, "\nGauge:   measuring (needs >=1%% drop)");
  }
  if (hlc > 0.0f && m > 0 && m < (int)sizeof(pb)) {
    int ch = (int)hlc, cm = (int)((hlc - (int)hlc) * 60.0f + 0.5f);
    m += snprintf(pb + m, sizeof(pb) - m, "\nRuntime: ~%dh %02dm", ch, cm);
  }
  // REAL deep-sleep floor current — THE number for judging the rail-cut savings.
  // Labeled "Idle" on screen (user preference): it's the watch-at-rest draw —
  // screen dark, CPU powered down between background checks (wake bursts are
  // accounted as awake time and excluded from this number).
  if (m > 0 && m < (int)sizeof(pb)) {
    if (calib_is_learned())
      snprintf(pb + m, sizeof(pb) - m, "\nIdle:    %d.%02d mA  (REAL floor, %us)",
               (int)calib_get_sleep_ma(),
               (int)((calib_get_sleep_ma() - (int)calib_get_sleep_ma()) * 100 + 0.5f),
               calib_get_samples());
    else
      snprintf(pb + m, sizeof(pb) - m, "\nIdle:    learning floor...");
  }
  lv_label_set_text(pm_power_lbl, pb);

  // --- CORE & CLOCK detail block ---
  // Core voltage SET-POINT read back from the dig_dbias trim register — what the
  // on-die LDO is told to output, NOT a measurement (the S3 can't sense its own
  // core rail). Reflects the real programmed value incl. a boot-time auto-revert.
  char cb[200];
  uint16_t cmv = core_dbias_to_mv(core_get_dig_dbias());
  int k = snprintf(cb, sizeof(cb), "Core:    ~%u mV  (%s)",
                   cmv, s_core_unstable ? "reverted" : "set");
  char clk[64];
  clocks_format(clk, sizeof(clk));   // "Clk: 260 MHz  PLL 520/2  fl 86"
  if (k > 0 && k < (int)sizeof(cb))
    k += snprintf(cb + k, sizeof(cb) - k, "\n%s", clk);
#if OVERCLOCK_ENABLE
  if (k > 0 && k < (int)sizeof(cb)) {
    if (s_oc_died_stage)   k += snprintf(cb + k, sizeof(cb) - k, "\nOC:      hung@stage %u (skipped)", s_oc_died_stage);
    else if (s_oc_failed)  k += snprintf(cb + k, sizeof(cb) - k, "\nOC:      reverted (canary)");
    else if (s_oc_cpu_mhz) k += snprintf(cb + k, sizeof(cb) - k, "\nOC:      %u MHz OK", s_oc_cpu_mhz);
    else                   k += snprintf(cb + k, sizeof(cb) - k, "\nOC:      stock (tap below)");
  }
#endif

  // --- Temperatures: the two real die sensors on this board ---
  // SoC = the S3's internal sensor (the number to watch while undervolting /
  // overclocking — silicon margin shrinks as it warms). PMU = AXP2101 die temp,
  // a proxy for charge-path / regulator heat. The battery NTC (TS pin) isn't
  // populated on this board, so these two ARE the available set. The PMU's Tdie
  // ADC channel is enabled lazily on first read (µA-scale; shared-ADC mux).
  if (k > 0 && k < (int)sizeof(cb)) {
    float soc_c = temperatureRead();           // built-in S3 die sensor (degC)
    float pmu_c = -273.0f;
    if (pmu_ok) {
      static bool s_tdie_en = false;
      i2c_lock();                              // shared bus (core 1 UI vs core 0 net)
      if (!s_tdie_en) { power.enableTemperatureMeasure(); s_tdie_en = true; }
      pmu_c = power.getTemperature();
      i2c_unlock();
    }
    if (pmu_c > -100.0f)
      k += snprintf(cb + k, sizeof(cb) - k, "\nTemp:    SoC %d.%uC  PMU %d.%uC",
                    (int)soc_c, (unsigned)((soc_c - (int)soc_c) * 10) % 10,
                    (int)pmu_c, (unsigned)((pmu_c - (int)pmu_c) * 10) % 10);
    else
      k += snprintf(cb + k, sizeof(cb) - k, "\nTemp:    SoC %d.%uC",
                    (int)soc_c, (unsigned)((soc_c - (int)soc_c) * 10) % 10);
  }
  lv_label_set_text(pm_clk_lbl, cb);
}

/* Drives the screen: text every tick (PM_TEXT_MS), graph points every Nth tick
 * (PM_GRAPH_EVERY) so the charts update on a slower, longer-window cadence. */
static void pm_timer_cb(lv_timer_t *t) {
  (void)t;
  pm_update_labels();
  if (++s_pm_graph_tick >= PM_GRAPH_EVERY) {
    s_pm_graph_tick = 0;
    pm_update_graphs();
  }
}

/* When the Power screen is destroyed (any back/close path), kill its refresh
 * timer and drop every cached pointer so the timer can never touch freed
 * objects. Attaching this to LV_EVENT_DELETE covers every teardown path. */
static void pm_cleanup_cb(lv_event_t *e) {
  (void)e;
  if (pm_timer) { lv_timer_del(pm_timer); pm_timer = nullptr; }
  pm_batt_lbl = nullptr;
  pm_power_lbl = nullptr;
  pm_clk_lbl  = nullptr;
  pm_g_batt = pm_graph_t{};
  pm_g_draw = pm_graph_t{};
  pm_g_cpu  = pm_cpu_graph_t{};
  pm_wake_sw  = nullptr;
  pm_intv_lbl = nullptr;
  pm_intv_row = nullptr;
  pm_dim_sw   = nullptr;
  pm_dimp_lbl = nullptr;
  pm_dimp_row = nullptr;
  pm_dimusb_sw = nullptr;
}

/* Repaint the freq buttons so the active one is highlighted. */
static void pm_refresh_freq_highlight(void) {
  for (uint8_t i = 0; i < CPU_FREQ_COUNT; i++) {
    bool sel = (CPU_FREQS[i] == settings_get_cpu_mhz());
    lv_obj_set_style_bg_color(pm_freq_btns[i],
        sel ? lv_color_hex(ui_accent_hex()) : lv_color_hex(0x1A1A1A), 0);
  }
}

static void pm_freq_btn_cb(lv_event_t *e) {
  uint16_t mhz = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
  settings_set_cpu_mhz(mhz);          // apply + persist
  pm_refresh_freq_highlight();
  pm_update_labels();                 // clock line reflects the new speed at once
}

/* ---- DEEP SLEEP: periodic-wake toggle + check-interval stepper ----
 * Repaint the deep-sleep widgets from current settings: the toggle's label/color,
 * the "N min" value, and the stepper's dim state (greyed + non-interactive when
 * periodic wake is OFF, since the interval is moot then). */
static void pm_refresh_deepsleep(void) {
  bool en = settings_get_checks_enabled();
  pm_sw_set(pm_wake_sw, en);
  if (pm_intv_lbl)
    lv_label_set_text_fmt(pm_intv_lbl, "%u min", (unsigned)settings_get_check_interval());
  if (pm_intv_row) {
    lv_obj_set_style_opa(pm_intv_row, en ? LV_OPA_COVER : LV_OPA_50, 0);
    // Gate the arrow buttons: when wake is off they shouldn't change a moot value.
    uint32_t n = lv_obj_get_child_count(pm_intv_row);
    for (uint32_t i = 0; i < n; i++) {
      lv_obj_t *c = lv_obj_get_child(pm_intv_row, i);
      if (lv_obj_check_type(c, &lv_button_class)) {
        if (en) lv_obj_clear_state(c, LV_STATE_DISABLED);
        else    lv_obj_add_state(c, LV_STATE_DISABLED);
      }
    }
  }
}

/* Toggle periodic notification wake on/off (persisted). When off, the watch does a
 * true power-off at sleep instead of arming the check timer. */
static void pm_checks_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  settings_set_checks_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
  haptics_pulse(12);
  pm_refresh_deepsleep();
}

/* Step the check interval by +/-1 min (user_data carries the signed delta). The
 * setter clamps to CHECK_INTERVAL_MIN_MIN..MAX (2..20). No-op while wake is off. */
static void pm_interval_step_cb(lv_event_t *e) {
  if (!settings_get_checks_enabled()) return;
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  settings_set_check_interval((uint16_t)((int)settings_get_check_interval() + d));
  haptics_pulse(10);
  pm_refresh_deepsleep();
}

/* ---- DISPLAY: auto-dim toggle + dim-level stepper ----
 * Repaint from current settings: the toggle's label/color, the "N%" dim level, and
 * the level stepper's dim state (greyed + disabled when auto-dim is OFF). */
static void pm_refresh_display(void) {
  bool on = settings_get_autodim();
  pm_sw_set(pm_dim_sw, on);
  if (pm_dimp_lbl)
    lv_label_set_text_fmt(pm_dimp_lbl, "%u%%", (unsigned)settings_get_autodim_pct());
  if (pm_dimp_row) {
    lv_obj_set_style_opa(pm_dimp_row, on ? LV_OPA_COVER : LV_OPA_50, 0);
    uint32_t n = lv_obj_get_child_count(pm_dimp_row);
    for (uint32_t i = 0; i < n; i++) {
      lv_obj_t *c = lv_obj_get_child(pm_dimp_row, i);
      if (lv_obj_check_type(c, &lv_button_class)) {
        if (on) lv_obj_clear_state(c, LV_STATE_DISABLED);
        else    lv_obj_add_state(c, LV_STATE_DISABLED);
      }
    }
  }
  // "Dim on USB" sub-toggle: only meaningful when auto-dim is ON, so grey it out
  // (and disable it) accordingly. The switch position shows its own ON/OFF state.
  bool usb = settings_get_dim_on_usb();
  pm_sw_set(pm_dimusb_sw, usb);
  pm_row_set_enabled(pm_dimusb_sw, on);
}

static void pm_autodim_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  settings_set_autodim(lv_obj_has_state(sw, LV_STATE_CHECKED));
  haptics_pulse(12);
  pm_refresh_display();
}

/* Toggle "dim on USB". The row is disabled while auto-dim is OFF, so the switch
 * can't be reached then; guard anyway. */
static void pm_dimusb_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  if (!settings_get_autodim()) return;
  settings_set_dim_on_usb(lv_obj_has_state(sw, LV_STATE_CHECKED));
  haptics_pulse(12);
  pm_refresh_display();
}

/* Step the dim level by +/-5 % (user_data carries the signed delta). The setter
 * clamps to 5..90 %. No-op while auto-dim is off. */
static void pm_dim_step_cb(lv_event_t *e) {
  if (!settings_get_autodim()) return;
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  settings_set_autodim_pct((uint8_t)((int)settings_get_autodim_pct() + d));
  haptics_pulse(10);
  pm_refresh_display();
}

/* Toggle whether a (bus-safe) rail is cut in deep sleep; repaint the row in place.
 * The rail index is the button's user-data; its first child is the text label. */
static void pm_rail_toggle_cb(lv_event_t *e) {
  uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  rail_cut_set(i, !rail_cut_get(i));
  haptics_pulse(12);
  lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
  bool on = rail_cut_get(i);
  lv_obj_set_style_bg_color(b, lv_color_hex(on ? ui_accent_hex() : 0x1F1F1F), 0);
  lv_obj_t *l = lv_obj_get_child(b, 0);
  if (l) lv_label_set_text_fmt(l, "%-6s  %s", rail_name(i), on ? "CUT in sleep" : "tap to cut");
}

#if OVERCLOCK_ENABLE
/* Trigger the overclock NOW (runtime only). Boot stays stock, so if this kills
 * USB (e.g. 300 MHz) a reboot restores it — no ROM download mode. */
static void pm_oc_btn_cb(lv_event_t *e) {
  (void)e;
  overclock_apply();      // bumps + self-reverts on canary failure
  pm_update_labels();     // refresh the Clk / OC readout immediately
}
#endif

static void app_open_power(void) {
  app_screen_begin("Power");

  // One scrollable column from just below the title to the bottom (matches
  // About). All content — graph cards, stat blocks, the CPU picker — lives here.
  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 374, 408);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  // Wide vertical spacing between stacked elements (was 6). With more gap, fewer
  // expensive elements (charts/text/buttons) share the viewport at any scroll
  // position — more of each scroll frame is cheap empty background instead of
  // glyph/chart rasterization. Matches About's airy 16px (the screen that already
  // scrolls smoothly); this is the same trick applied to the dense Power list.
  lv_obj_set_style_pad_row(col, 20, 0);

  // ---- BATTERY ----
  pm_header(col, "BATTERY");
  pm_make_graph(col, "Level", 0, 100, &pm_g_batt);
  pm_batt_lbl = pm_line(col, &FONT_SMALL, PM_VAL, "");

  // ---- POWER ----
  pm_header(col, "POWER");
  pm_make_graph(col, "Draw  (model mA)", 0, 400, &pm_g_draw);
  pm_power_lbl = pm_line(col, &FONT_SMALL, PM_VAL, "");

  // ---- CPU (per-core usage) ----
  pm_header(col, "CPU  (per-core %)");
  pm_make_cpu_graph(col, &pm_g_cpu);

  // ---- CORE & CLOCK ----
  pm_header(col, "CORE & CLOCK");
  pm_clk_lbl = pm_line(col, &FONT_SMALL, PM_VAL, "");

  s_pm_graph_tick = 0;
  cpu_usage_reset_window();   // measure CPU% from now, not the whole idle-since-boot window
  pm_update_labels();         // initial text fill
  pm_update_graphs();         // seed the first point of every graph

  // ---- DISPLAY (auto-dim the panel after idle, on battery) ----
  pm_header(col, "DISPLAY");

  // Toggle: dim the screen after ~10 s idle when on battery? A touch or an incoming
  // notification restores full brightness. Never dims on USB power or caffeine.
  pm_dim_sw = settings_toggle_row(col, LV_SYMBOL_EYE_CLOSE, "Auto-dim idle",
                                  settings_get_autodim(), pm_autodim_toggle_cb);

  // Caption + stepper for the dim level (5..90 % of brightness, 5% arrows).
  pm_line(col, &FONT_SMALL, 0xAAAAAA, "Dim to:");
  pm_dimp_row = lv_obj_create(col);
  lv_obj_set_width(pm_dimp_row, LV_PCT(100));
  lv_obj_set_height(pm_dimp_row, 56);
  lv_obj_set_style_bg_opa(pm_dimp_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pm_dimp_row, 0, 0);
  lv_obj_set_style_pad_all(pm_dimp_row, 0, 0);
  lv_obj_clear_flag(pm_dimp_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(pm_dimp_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pm_dimp_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *ddn = lv_btn_create(pm_dimp_row);
  lv_obj_set_size(ddn, 64, 52);
  lv_obj_set_style_radius(ddn, 12, 0);
  lv_obj_set_style_bg_color(ddn, lv_color_hex(0x1F1F1F), 0);
  lv_obj_add_event_cb(ddn, pm_dim_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-5);
  lv_obj_t *ddnl = lv_label_create(ddn);
  lv_obj_set_style_text_font(ddnl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(ddnl, lv_color_white(), 0);
  lv_label_set_text(ddnl, LV_SYMBOL_MINUS);
  lv_obj_center(ddnl);

  pm_dimp_lbl = lv_label_create(pm_dimp_row);
  lv_obj_set_style_text_font(pm_dimp_lbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(pm_dimp_lbl, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(pm_dimp_lbl, "--%");

  lv_obj_t *dup = lv_btn_create(pm_dimp_row);
  lv_obj_set_size(dup, 64, 52);
  lv_obj_set_style_radius(dup, 12, 0);
  lv_obj_set_style_bg_color(dup, lv_color_hex(0x1F1F1F), 0);
  lv_obj_add_event_cb(dup, pm_dim_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)5);
  lv_obj_t *dupl = lv_label_create(dup);
  lv_obj_set_style_text_font(dupl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(dupl, lv_color_white(), 0);
  lv_label_set_text(dupl, LV_SYMBOL_PLUS);
  lv_obj_center(dupl);

  // Sub-toggle: also dim while on USB power? Default OFF (historically the watch
  // never dims while plugged in). Even ON, it only dims — the watch still never
  // deep-sleeps on USB. Greyed out while auto-dim itself is off.
  pm_dimusb_sw = settings_toggle_row(col, LV_SYMBOL_CHARGE, "Dim on USB",
                                     settings_get_dim_on_usb(), pm_dimusb_toggle_cb);

  pm_refresh_display();   // paint toggle/value/dim-state from saved settings

  // ---- DEEP SLEEP (periodic notification wake on/off + how often) ----
  pm_header(col, "DEEP SLEEP");

  // Toggle: does the watch wake periodically to check for notifications? When OFF
  // it sleeps with only the BOOT-press wake armed (no periodic timer) — lowest
  // power, no background fetches; a BOOT press still brings it back.
  pm_wake_sw = settings_toggle_row(col, LV_SYMBOL_REFRESH, "Periodic wake",
                                   settings_get_checks_enabled(), pm_checks_toggle_cb);

  // Caption + stepper for the wake cadence (2..20 min, 1-min arrows).
  pm_line(col, &FONT_SMALL, 0xAAAAAA, "Check every:");
  pm_intv_row = lv_obj_create(col);
  lv_obj_set_width(pm_intv_row, LV_PCT(100));
  lv_obj_set_height(pm_intv_row, 56);
  lv_obj_set_style_bg_opa(pm_intv_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pm_intv_row, 0, 0);
  lv_obj_set_style_pad_all(pm_intv_row, 0, 0);
  lv_obj_clear_flag(pm_intv_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(pm_intv_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pm_intv_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // "-" button
  lv_obj_t *dn = lv_btn_create(pm_intv_row);
  lv_obj_set_size(dn, 64, 52);
  lv_obj_set_style_radius(dn, 12, 0);
  lv_obj_set_style_bg_color(dn, lv_color_hex(0x1F1F1F), 0);
  lv_obj_add_event_cb(dn, pm_interval_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
  lv_obj_t *dnl = lv_label_create(dn);
  lv_obj_set_style_text_font(dnl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(dnl, lv_color_white(), 0);
  lv_label_set_text(dnl, LV_SYMBOL_MINUS);
  lv_obj_center(dnl);

  // value
  pm_intv_lbl = lv_label_create(pm_intv_row);
  lv_obj_set_style_text_font(pm_intv_lbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(pm_intv_lbl, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(pm_intv_lbl, "-- min");

  // "+" button
  lv_obj_t *up = lv_btn_create(pm_intv_row);
  lv_obj_set_size(up, 64, 52);
  lv_obj_set_style_radius(up, 12, 0);
  lv_obj_set_style_bg_color(up, lv_color_hex(0x1F1F1F), 0);
  lv_obj_add_event_cb(up, pm_interval_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
  lv_obj_t *upl = lv_label_create(up);
  lv_obj_set_style_text_font(upl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(upl, lv_color_white(), 0);
  lv_label_set_text(upl, LV_SYMBOL_PLUS);
  lv_obj_center(upl);

  pm_refresh_deepsleep();   // paint toggle/value/dim-state from saved settings

  // ---- POWER RAILS (opt-in deep-sleep cut; bus-safe rails are tappable) ----
  pm_header(col, "POWER RAILS  (deep-sleep)");
  {
    char sum[64];
    snprintf(sum, sizeof sum, "Cutting %u of %u.",
             rails_cut_count(), rail_state_count());
    pm_line(col, &FONT_SMALL, 0xAAAAAA, sum);
  }
  for (uint8_t i = 0; i < rail_state_count(); i++) {
    if (rail_state_raw(i) == 1) {                     // SAFE -> a tappable cut toggle
      lv_obj_t *b = lv_btn_create(col);
      lv_obj_set_width(b, LV_PCT(100));
      lv_obj_set_height(b, 44);
      lv_obj_set_style_radius(b, 10, 0);
      bool on = rail_cut_get(i);
      lv_obj_set_style_bg_color(b, lv_color_hex(on ? ui_accent_hex() : 0x1F1F1F), 0);
      lv_obj_add_event_cb(b, pm_rail_toggle_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
      lv_obj_t *l = lv_label_create(b);
      lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(l, lv_color_white(), 0);
      lv_label_set_text_fmt(l, "%-6s  %s", rail_name(i), on ? "CUT in sleep" : "tap to cut");
      lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
    } else {                                          // unsafe/untested -> info only
      char rl[48];
      snprintf(rl, sizeof rl, "%-6s  %s", rail_name(i), rail_verdict_str(i));
      pm_line(col, &FONT_SMALL, rail_state_raw(i) == 2 ? 0x888888 : 0xC9A227, rl);
    }
  }

  // ---- CPU SPEED ----
  pm_header(col, "CPU SPEED  (MHz)");

  lv_obj_t *rowc = lv_obj_create(col);
  lv_obj_set_width(rowc, LV_PCT(100));
  lv_obj_set_height(rowc, 64);
  lv_obj_set_style_bg_opa(rowc, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rowc, 0, 0);
  lv_obj_set_style_pad_all(rowc, 0, 0);
  lv_obj_clear_flag(rowc, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(rowc, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowc, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(rowc, 8, 0);

  for (uint8_t i = 0; i < CPU_FREQ_COUNT; i++) {
    lv_obj_t *b = lv_btn_create(rowc);
    lv_obj_set_size(b, 78, 56);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_add_event_cb(b, pm_freq_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)CPU_FREQS[i]);
    lv_obj_t *t = lv_label_create(b);
    lv_obj_set_style_text_font(t, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text_fmt(t, "%u", CPU_FREQS[i]);
    lv_obj_center(t);
    pm_freq_btns[i] = b;
  }
  pm_refresh_freq_highlight();

#if OVERCLOCK_ENABLE
  // Runtime overclock trigger. Never auto-applies at boot, so USB/flashing always
  // work; reboot undoes a bad bump.
  lv_obj_t *ocb = lv_btn_create(col);
  lv_obj_set_size(ocb, 220, 46);
  lv_obj_set_style_bg_color(ocb, lv_color_hex(0x3A2A0A), 0);
  lv_obj_set_style_radius(ocb, 12, 0);
  lv_obj_add_event_cb(ocb, pm_oc_btn_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *ocl = lv_label_create(ocb);
  lv_obj_set_style_text_font(ocl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(ocl, lv_color_hex(0xFFCC66), 0);
  lv_label_set_text_fmt(ocl, "Overclock %u MHz", (unsigned)OVERCLOCK_TARGET_MHZ);
  lv_obj_center(ocl);
#endif

  // Text refreshes every PM_TEXT_MS; graphs update on the slower cadence inside
  // the callback. Auto-torn-down on screen delete.
  pm_timer = lv_timer_create(pm_timer_cb, PM_TEXT_MS, nullptr);
  lv_obj_add_event_cb(app_scr, pm_cleanup_cb, LV_EVENT_DELETE, nullptr);
}
