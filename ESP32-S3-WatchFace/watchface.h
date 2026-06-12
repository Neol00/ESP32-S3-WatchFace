/* ============================================================================
 *  watchface.h — the clock face: Fossil-style stat row + enormous HH:MM clock.
 *
 *  Top row, three colored stat columns (bell/unread · battery · wifi) with a
 *  top-right indicator tray (caffeine mug, BLE glyph) and corner voltage; center
 *  is the big clock with weekday + date below. Builds the widgets once
 *  (watchface_create) and exposes setters the loop calls as state changes.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER the app_*.h screens and
 *  ui_fonts.h: it uses the FONT_* aliases, the shared store lock + notif_unread()
 *  (via watchface_refresh_bell, forward-declared in watch_base.h), and the
 *  screenWidth global from the .ino. Nothing here is referenced by the app
 *  modules except watchface_refresh_bell(), so it can safely live after them.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* ---- Watch-face widgets (3 colored stat columns + big clock) --
 * Top row, three columns, each = a colored icon with a value label below it:
 *   [battery icon + NN%]   [blue calendar + "June 4"]   [red day + "Thu"]
 * Center: enormous HH:MM clock.
 * The battery icon is a custom two-part widget (outline + colored fill bar) so
 * only the FILL is tinted green/yellow/red by level — LVGL's symbol glyphs are
 * single-color and can't do that. */
static lv_obj_t *lbl_time, *lbl_weekday, *lbl_date;
static lv_obj_t *lbl_volt;                                  // top-right corner
static lv_obj_t *lbl_bell_icon, *lbl_bell_cnt;              // LEFT column (red)
static lv_obj_t *batt_shell, *batt_fill, *lbl_batt_pct;    // MIDDLE column
static lv_obj_t *lbl_wifi_icon, *lbl_wifi_txt;             // RIGHT column (blue)
static lv_obj_t *caf_ind = nullptr;                        // top-right caffeine (keep-awake) mug, gray
static lv_obj_t *lbl_ble_icon = nullptr;                   // top-right BLE glyph, shown when a phone is connected
/* Mirror indicators for the swap-WiFi/BLE layout: the WiFi glyph ALSO exists in the
 * top-right tray, and the BLE column ALSO exists in the right stat column. Both
 * representations always track real state (watchface_set_wifi/_set_ble update both);
 * watchface_apply_indicator_layout() shows exactly one of each pair per the setting. */
static lv_obj_t *lbl_wifi_tray = nullptr;                  // top-right WiFi glyph (used when swapped)
static lv_obj_t *lbl_ble_col_icon = nullptr, *lbl_ble_col_txt = nullptr;  // RIGHT column BLE (used when swapped)

/* Container handles, kept so the deep-dim "minimal face" (watchface_set_minimal)
 * can hide everything except the center clock in one shot. The three stat columns
 * and the corner indicator tray are otherwise local to watchface_create. */
static lv_obj_t *wf_corner_tray = nullptr;                 // holds BLE + caffeine icons
static lv_obj_t *wf_lcol = nullptr, *wf_mcol = nullptr, *wf_rcol = nullptr;  // bell / battery / wifi columns

static const char *const WEEKDAYS[7] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const MONTHS[12] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};

/* Place a child at horizontal position cx_pct (% of screen width) along the top
 * stat row, top-aligned y px down. Returns a transparent column container with
 * vertical-center flex so an icon + value label stack neatly. */
static lv_obj_t *make_stat_column(lv_obj_t *scr, int cx_pct, int y) {
  lv_obj_t *col = lv_obj_create(scr);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, 110, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 4, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_x(col, (int)(((int)screenWidth * cx_pct) / 100) - (int)screenWidth / 2);
  return col;
}

/* Re-anchor the voltage label to the LEFT of the indicator tray, so it sits snug
 * against the icons that are ACTUALLY visible (the tray is content-sized: it
 * shrinks when an icon hides). Call after any tray icon shows/hides. We update the
 * tray's layout first so its width reflects the current icon set before we align
 * to its edge. The 8 px gap matches the tray's own column padding. Null-safe. */
static void watchface_realign_volt(void) {
  if (!lbl_volt || !wf_corner_tray) return;
  lv_obj_update_layout(wf_corner_tray);     // recompute content-size width NOW
  lv_obj_align_to(lbl_volt, wf_corner_tray, LV_ALIGN_OUT_LEFT_MID, -20, 0);
}

static void watchface_create(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);  // AMOLED black = low power
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  const int TOP_Y = 128;  // vertical offset of the stat row

  /* --- Voltage readout: tucked in the top-right corner (its own little label).
   *     Its X is NOT fixed — it's re-anchored to the LEFT of the indicator tray
   *     (watchface_realign_volt) so it sits snug against whatever icons are actually
   *     showing. A fixed offset used to reserve space for BOTH icons, leaving a gap
   *     when only one (or none) was visible. --- */
  lbl_volt = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_volt, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(lbl_volt, lv_color_hex(0x777777), 0);
  lv_label_set_text(lbl_volt, "--.--V");

  /* --- Top-right indicator tray: a right-anchored, content-sized flex row that
   *     holds the small status icons (caffeine mug, BLE glyph, …). LVGL flex skips
   *     HIDDEN children, so icons pack tightly from the rounded corner inward and
   *     gaps collapse: the rightmost VISIBLE child always lands in the corner. Add
   *     future corner icons as children (left of the mug) and they slot in.
   *     The -56 inset pulls the tray in far enough that the rightmost icon clears
   *     the rounded display corner (the old -46 let the BLE glyph clip off-edge). --- */
  lv_obj_t *corner_tray = lv_obj_create(scr);
  wf_corner_tray = corner_tray;             // kept for watchface_set_minimal
  lv_obj_remove_style_all(corner_tray);
  lv_obj_set_size(corner_tray, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(corner_tray, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(corner_tray, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(corner_tray, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(corner_tray, 6, 0);
  lv_obj_align(corner_tray, LV_ALIGN_TOP_RIGHT, -56, 14);  // -56 clears the rounded corner

  /* --- BLE indicator: a BLUE Bluetooth glyph, hidden unless a phone is connected.
   *     First (leftmost) tray child, so it sits to the LEFT of the caffeine mug when
   *     both show, and slides into the corner when the mug is hidden. --- */
  lbl_ble_icon = lv_label_create(corner_tray);
  lv_obj_set_style_text_font(lbl_ble_icon, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lbl_ble_icon, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue, matches wifi
  lv_label_set_text(lbl_ble_icon, LV_SYMBOL_BLUETOOTH);
  lv_obj_add_flag(lbl_ble_icon, LV_OBJ_FLAG_HIDDEN);

  /* --- WiFi indicator (tray copy): the BLUE wifi glyph that takes the corner when
   *     the user swaps WiFi/BLE. Hidden unless the swap layout is active (and then
   *     shown/dimmed by connection state, like the column copy). Sits between the BLE
   *     glyph and the caffeine mug; flex skips it while hidden so nothing shifts. --- */
  lbl_wifi_tray = lv_label_create(corner_tray);
  lv_obj_set_style_text_font(lbl_wifi_tray, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lbl_wifi_tray, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue
  lv_label_set_text(lbl_wifi_tray, LV_SYMBOL_WIFI);
  lv_obj_add_flag(lbl_wifi_tray, LV_OBJ_FLAG_HIDDEN);

  /* --- Caffeine (keep-awake) indicator: a tiny GRAY coffee mug, hidden unless
   *     caffeine is on. Last (rightmost) tray child, so it holds the corner. Built
   *     from shapes (no coffee glyph in the symbol font), matching the shade's
   *     toggle icon. --- */
  caf_ind = lv_obj_create(corner_tray);
  lv_obj_remove_style_all(caf_ind);
  lv_obj_set_size(caf_ind, 24, 20);
  lv_obj_clear_flag(caf_ind, LV_OBJ_FLAG_SCROLLABLE);
  {
    lv_obj_t *body = lv_obj_create(caf_ind);          // mug body
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 15, 11);
    lv_obj_align(body, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x999999), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_t *hd = lv_obj_create(caf_ind);            // ring handle
    lv_obj_remove_style_all(hd);
    lv_obj_set_size(hd, 8, 9);
    lv_obj_align(hd, LV_ALIGN_BOTTOM_RIGHT, 0, -1);
    lv_obj_set_style_border_color(hd, lv_color_hex(0x999999), 0);
    lv_obj_set_style_border_width(hd, 2, 0);
    lv_obj_set_style_radius(hd, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hd, LV_OPA_TRANSP, 0);
    for (int i = 0; i < 2; i++) {                     // steam
      lv_obj_t *st = lv_obj_create(caf_ind);
      lv_obj_remove_style_all(st);
      lv_obj_set_size(st, 2, 5);
      lv_obj_align(st, LV_ALIGN_TOP_LEFT, 3 + i * 6, 0);
      lv_obj_set_style_bg_color(st, lv_color_hex(0x666666), 0);
      lv_obj_set_style_bg_opa(st, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(st, 1, 0);
    }
  }
  lv_obj_add_flag(caf_ind, LV_OBJ_FLAG_HIDDEN);       // shown only when caffeine is on

  /* --- LEFT column: RED bell + unread count --- */
  lv_obj_t *lcol = make_stat_column(scr, 22, TOP_Y);
  wf_lcol = lcol;                           // kept for watchface_set_minimal
  lbl_bell_icon = lv_label_create(lcol);
  lv_obj_set_style_text_font(lbl_bell_icon, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lbl_bell_icon, lv_color_hex(0xFF4D4D), 0);  // red
  lv_label_set_text(lbl_bell_icon, LV_SYMBOL_BELL);
  lbl_bell_cnt = lv_label_create(lcol);
  lv_obj_set_style_text_font(lbl_bell_cnt, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_bell_cnt, lv_color_white(), 0);
  lv_label_set_text(lbl_bell_cnt, "0");

  /* --- MIDDLE column: custom battery widget (colored fill) + percent --- */
  lv_obj_t *mcol = make_stat_column(scr, 50, TOP_Y);
  wf_mcol = mcol;                           // kept for watchface_set_minimal
  batt_shell = lv_obj_create(mcol);
  lv_obj_remove_style_all(batt_shell);
  lv_obj_set_size(batt_shell, 40, 20);
  lv_obj_set_style_radius(batt_shell, 3, 0);
  lv_obj_set_style_border_width(batt_shell, 2, 0);
  lv_obj_set_style_border_color(batt_shell, lv_color_hex(0x999999), 0);
  lv_obj_set_style_pad_all(batt_shell, 2, 0);
  lv_obj_clear_flag(batt_shell, LV_OBJ_FLAG_SCROLLABLE);
  // little positive terminal nub on the right end of the battery
  lv_obj_t *nub = lv_obj_create(batt_shell);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, 3, 8);
  lv_obj_set_style_bg_color(nub, lv_color_hex(0x999999), 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 5, 0);
  // inner fill bar: width = %, color = level (set in watchface_set_battery)
  batt_fill = lv_obj_create(batt_shell);
  lv_obj_remove_style_all(batt_fill);
  lv_obj_set_style_radius(batt_fill, 1, 0);
  lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
  lv_obj_set_height(batt_fill, LV_PCT(100));
  lv_obj_set_width(batt_fill, LV_PCT(100));
  lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

  lbl_batt_pct = lv_label_create(mcol);
  lv_obj_set_style_text_font(lbl_batt_pct, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_batt_pct, lv_color_white(), 0);
  lv_label_set_text(lbl_batt_pct, "--%");

  /* --- RIGHT column: BLUE wifi + status --- */
  lv_obj_t *rcol = make_stat_column(scr, 78, TOP_Y);
  wf_rcol = rcol;                           // kept for watchface_set_minimal
  lbl_wifi_icon = lv_label_create(rcol);
  lv_obj_set_style_text_font(lbl_wifi_icon, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lbl_wifi_icon, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue
  lv_label_set_text(lbl_wifi_icon, LV_SYMBOL_WIFI);
  lbl_wifi_txt = lv_label_create(rcol);
  lv_obj_set_style_text_font(lbl_wifi_txt, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_wifi_txt, lv_color_white(), 0);
  lv_label_set_text(lbl_wifi_txt, "--");

  /* --- BLE indicator (column copy): the BLUE Bluetooth glyph + status text that
   *     takes the right stat column when the user swaps WiFi/BLE. Built into the SAME
   *     column as wifi; watchface_apply_indicator_layout() hides one pair and shows the
   *     other so the column holds exactly one indicator. Hidden by default. --- */
  lbl_ble_col_icon = lv_label_create(rcol);
  lv_obj_set_style_text_font(lbl_ble_col_icon, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lbl_ble_col_icon, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue
  lv_label_set_text(lbl_ble_col_icon, LV_SYMBOL_BLUETOOTH);
  lv_obj_add_flag(lbl_ble_col_icon, LV_OBJ_FLAG_HIDDEN);
  lbl_ble_col_txt = lv_label_create(rcol);
  lv_obj_set_style_text_font(lbl_ble_col_txt, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_ble_col_txt, lv_color_white(), 0);
  lv_label_set_text(lbl_ble_col_txt, "--");
  lv_obj_add_flag(lbl_ble_col_txt, LV_OBJ_FLAG_HIDDEN);

  /* --- CENTER: enormous clock --- */
  lbl_time = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_time, &FONT_TIME, 0);
  lv_obj_set_style_text_color(lbl_time, lv_color_white(), 0);
  lv_label_set_text(lbl_time, "--:--");
  lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, 0);

  /* --- BELOW clock: weekday · date --- */
  lbl_weekday = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_weekday, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_weekday, lv_color_white(), 0);
  lv_label_set_text(lbl_weekday, "");
  lv_obj_align(lbl_weekday, LV_ALIGN_CENTER, 0, 95);

  lbl_date = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_date, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xAAAAAA), 0);
  lv_label_set_text(lbl_date, "");
  lv_obj_align_to(lbl_date, lbl_weekday, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

  watchface_apply_volt_visible();   // honor the saved show-voltage setting from boot
  watchface_apply_indicator_layout(); // honor the saved swap-WiFi/BLE setting from boot
  watchface_realign_volt();         // position the voltage label against the (empty) tray
}

static void watchface_update(const RTC_DateTime &dt) {
  lv_label_set_text_fmt(lbl_time, "%02d:%02d", dt.getHour(), dt.getMinute());

  int wd = dt.getWeek();  // 0=Sun..6=Sat
  if (wd >= 0 && wd < 7) lv_label_set_text(lbl_weekday, WEEKDAYS[wd]);

  int mo = dt.getMonth();
  if (mo >= 1 && mo <= 12)
    lv_label_set_text_fmt(lbl_date, "%d %s %d", dt.getDay(), MONTHS[mo - 1], dt.getYear());
  lv_obj_align_to(lbl_date, lbl_weekday, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
}

/* Update the MIDDLE battery widget + the top-right voltage.
 *   pct      : 0-100, or <0 if unknown/no battery
 *   charging : true when the cell is being charged
 *   mv       : voltage in mV, shown in the corner readout
 * The fill bar's WIDTH tracks % and its COLOR tracks level (green/yellow/red);
 * only the fill is tinted — the shell outline stays grey. */
static void watchface_set_battery(int pct, bool charging, uint16_t mv) {
  lv_label_set_text_fmt(lbl_volt, "%u.%02uV", mv / 1000, (mv % 1000) / 10);

  if (pct < 0) {  // no battery / not readable
    lv_obj_set_width(batt_fill, LV_PCT(0));
    lv_label_set_text(lbl_batt_pct, "USB");
    return;
  }
  if (pct > 100) pct = 100;

  lv_obj_set_width(batt_fill, LV_PCT(pct < 8 ? 8 : pct));  // keep a sliver visible
  lv_label_set_text_fmt(lbl_batt_pct, "%d%%", pct);

  lv_color_t fill;
  if (charging)        fill = lv_color_hex(0x8080FF);   // purple while charging
  else if (pct >= 60)  fill = lv_color_hex(0x33CC55);   // green: healthy
  else if (pct >= 25)  fill = lv_color_hex(0xFFC233);   // yellow: middle
  else                 fill = lv_color_hex(0xFF4D4D);   // red: low
  lv_obj_set_style_bg_color(batt_fill, fill, 0);
}

/* Update the LEFT bell column with the unread notification count, and the RIGHT
 * wifi column with connection status. Called from the loop alongside battery. */
static void watchface_set_bell(uint32_t count) {
  if (count > 999) lv_label_set_text(lbl_bell_cnt, "999+");   // SD archive can be huge
  else             lv_label_set_text_fmt(lbl_bell_cnt, "%u", (unsigned)count);
  // Keep the bell RED always: dim/dark red when nothing unread, bright red when
  // there are notifications (pops without losing its color identity).
  lv_obj_set_style_text_color(lbl_bell_icon,
      count ? lv_color_hex(0xFF3030)     // bright red = unread
            : lv_color_hex(0x661414), 0); // dim dark red = idle
}
/* Repaint the bell badge from the current unread count (see watch_base.h). Takes
 * the store lock because notif_unread() reads the shared notif counters that the
 * core-0 network task updates. UI-core only (touches LVGL via watchface_set_bell). */
static void watchface_refresh_bell(void) {
  store_lock();
  uint32_t u = notif_unread();
  store_unlock();
  watchface_set_bell(u);
}

/* Show/hide the top-right caffeine mug. Re-anchors the voltage label since the
 * tray's width changes when the mug appears/disappears. */
static void watchface_set_caffeine(bool on) {
  if (!caf_ind) return;
  if (on) lv_obj_clear_flag(caf_ind, LV_OBJ_FLAG_HIDDEN);
  else    lv_obj_add_flag(caf_ind, LV_OBJ_FLAG_HIDDEN);
  watchface_realign_volt();
}

/* Last-known indicator states, so watchface_apply_indicator_layout() can repaint the
 * newly-shown copy correctly after a swap (the setters cache here; the layout fn replays). */
static bool wf_ble_connected = false;
static bool wf_wifi_connected = false;

/* Paint the BLE glyph in the corner tray: visible only when a phone is connected.
 * Used as BLE's home when NOT swapped. Null-safe. */
static void watchface_paint_ble_tray(bool connected) {
  if (!lbl_ble_icon) return;
  if (connected) lv_obj_clear_flag(lbl_ble_icon, LV_OBJ_FLAG_HIDDEN);
  else           lv_obj_add_flag(lbl_ble_icon, LV_OBJ_FLAG_HIDDEN);
}

/* Paint the BLE indicator in the right stat column (icon dim/bright + status text),
 * mirroring how wifi paints its column. Used as BLE's home when swapped. The column
 * copy stays present (icon dimmed) when disconnected, matching the wifi column's look,
 * rather than vanishing like the tray glyph. Null-safe. */
static void watchface_paint_ble_column(bool connected) {
  if (!lbl_ble_col_icon) return;
  lv_label_set_text(lbl_ble_col_txt, connected ? "ok" : "off");
  lv_obj_set_style_text_color(lbl_ble_col_icon,
      connected ? lv_color_hex(ui_deco_hex(0x33A0FF))    // bright blue = connected
                : lv_color_hex(ui_deco_dim_hex(0x123A5C)), 0); // dim dark blue = disconnected
}

/* Paint the WiFi indicator in the right stat column (its default home). */
static void watchface_paint_wifi_column(bool connected) {
  if (!lbl_wifi_icon) return;
  lv_label_set_text(lbl_wifi_txt, connected ? "ok" : "off");
  // Keep the wifi BLUE always: dim/dark blue when offline, bright blue when up.
  lv_obj_set_style_text_color(lbl_wifi_icon,
      connected ? lv_color_hex(ui_deco_hex(0x33A0FF))    // bright blue = connected
                : lv_color_hex(ui_deco_dim_hex(0x123A5C)), 0); // dim dark blue = offline
}

/* Paint the WiFi glyph in the corner tray (its home when swapped): visible ONLY when
 * connected, hidden otherwise — mirroring the BLE tray glyph. A corner tray icon is an
 * at-a-glance "this is active" cue, so an off/disconnected WiFi shouldn't sit dimmed in
 * the corner; it just disappears (and flex collapses the gap). Null-safe. */
static void watchface_paint_wifi_tray(bool connected) {
  if (!lbl_wifi_tray) return;
  if (connected) lv_obj_clear_flag(lbl_wifi_tray, LV_OBJ_FLAG_HIDDEN);
  else           lv_obj_add_flag(lbl_wifi_tray, LV_OBJ_FLAG_HIDDEN);
}

/* Apply the swap-WiFi/BLE layout: pick which indicator owns the right stat column and
 * which owns the top-right tray, hide the unused copy of each, and replay the cached
 * states so the now-visible copies are painted correctly. Re-anchors the voltage label
 * since the tray's visible-icon set (hence width) changes. Null-safe before build. */
static void watchface_apply_indicator_layout(void) {
  if (!lbl_wifi_icon || !lbl_ble_icon) return;     // face not built yet
  bool swap = settings_get_swap_wifi_ble();

  // Column: show wifi pair OR ble pair (never both).
  if (swap) {
    lv_obj_add_flag(lbl_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_wifi_txt,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_ble_col_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_ble_col_txt,  LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(lbl_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_wifi_txt,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_ble_col_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_ble_col_txt,  LV_OBJ_FLAG_HIDDEN);
  }

  // Tray: exactly one of the WiFi/BLE glyphs may live there at a time. Force-hide the
  // one that's NOT in the tray for this layout; the in-tray one's own visibility is
  // connection-driven and set by its paint helper in the replay below (both are
  // "visible only when connected"). This keeps the off/disconnected glyph from sitting
  // in the corner where the other layout's glyph belongs.
  if (swap) lv_obj_add_flag(lbl_ble_icon,   LV_OBJ_FLAG_HIDDEN);   // BLE leaves the tray
  else      lv_obj_add_flag(lbl_wifi_tray,  LV_OBJ_FLAG_HIDDEN);   // WiFi leaves the tray

  // Replay cached states into whichever copies are now live (this also sets the
  // in-tray glyph's connection-driven visibility).
  if (swap) {
    watchface_paint_ble_column(wf_ble_connected);
    watchface_paint_wifi_tray(wf_wifi_connected);
  } else {
    watchface_paint_ble_tray(wf_ble_connected);
    watchface_paint_wifi_column(wf_wifi_connected);
  }
  watchface_realign_volt();   // tray's visible icon set changed -> re-anchor voltage
}

/* Reflect a BLE phone connect/disconnect. Caches the state and paints whichever BLE
 * copy is currently live (tray glyph when default, right column when swapped).
 * Re-anchors the voltage label (tray width can change). */
static void watchface_set_ble(bool connected) {
  wf_ble_connected = connected;
  if (settings_get_swap_wifi_ble()) watchface_paint_ble_column(connected);
  else                              watchface_paint_ble_tray(connected);
  watchface_realign_volt();
}

/* Reflect WiFi connect/disconnect. Caches the state and paints whichever WiFi copy is
 * currently live (right column when default, tray glyph when swapped). */
static void watchface_set_wifi(bool connected) {
  wf_wifi_connected = connected;
  if (settings_get_swap_wifi_ble()) watchface_paint_wifi_tray(connected);
  else                              watchface_paint_wifi_column(connected);
}

/* Deep-dim "minimal face": when true, hide EVERYTHING on the watch face except the
 * center clock — the three stat columns (bell/battery/wifi), the date/weekday, the
 * corner voltage, and the indicator tray. On an AMOLED a hidden (black) pixel is a
 * pixel that's physically OFF, so blanking the periphery saves real power ON TOP of
 * the brightness drop the panel dim already does. When false, everything comes back.
 *
 * Only meaningful on the watch face itself — the loop gates this so it's never
 * called while a menu/app/shade is covering the face. Idempotent (tracks its own
 * state) and null-safe (no-op before watchface_create has built the widgets), so
 * the loop can call it every iteration cheaply. The clock (lbl_time) is never
 * touched, so the bare time stays readable while dimmed. */
static bool wf_minimal = false;

/* Apply the user's "show voltage readout" setting to the corner label — but never
 * show it while the minimal face is active (a dimmed face shows the clock only).
 * Single source of truth for lbl_volt's visibility, so the minimal-face restore
 * below can't override the user's choice to hide it. Null-safe. */
static void watchface_apply_volt_visible(void) {
  if (!lbl_volt) return;
  if (settings_get_show_volt() && !wf_minimal)
    lv_obj_clear_flag(lbl_volt, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(lbl_volt, LV_OBJ_FLAG_HIDDEN);
}

static void watchface_set_minimal(bool minimal) {
  if (minimal == wf_minimal) return;        // debounce: only touch the tree on a transition
  wf_minimal = minimal;
  // Hide/show as whole containers so a single flag covers each column's children.
  // lbl_volt is handled separately (watchface_apply_volt_visible) because its
  // visibility ALSO depends on the user's show-voltage setting — a blind clear_flag
  // here would re-show it on un-dim even if the user turned it off.
  lv_obj_t *parts[] = { wf_lcol, wf_mcol, wf_rcol, wf_corner_tray,
                        lbl_weekday, lbl_date };
  for (lv_obj_t *p : parts) {
    if (!p) continue;
    if (minimal) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
  }
  watchface_apply_volt_visible();           // respects both minimal + the user setting
  if (!minimal) watchface_realign_volt();   // tray is back -> re-anchor voltage to it
}
static bool watchface_is_minimal(void) { return wf_minimal; }
