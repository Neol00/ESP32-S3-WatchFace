/* ============================================================================
 *  quick_shade.h — a pull-down "quick settings" shade (brightness, always
 *  reachable). Drag DOWN from the very top edge of the screen — on the clock
 *  face OR inside any app — and a panel follows your finger 1:1; release past a
 *  third of the way and it snaps open (animated), otherwise it snaps back.
 *
 *  WHY a shade: brightness is a "right now" control
 *  (sun glare, dark room), so it must be one gesture away from anywhere — never
 *  three taps deep. The watch has no other touch gestures, so this is additive.
 *
 *  HOW it avoids fighting screen scrolling: the gesture only ARMS from a thin
 *  invisible grab strip pinned to the top.
 *  Everything below that scrolls normally.
 *
 *  Layering: scrim + panel + strip live on lv_layer_sys() (ALWAYS above the
 *  app/menu screens on lv_layer_top()), so the shade overlays whatever is open
 *  without us re-foregrounding it on every screen change.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER the FONT_* defines and
 *  the settings_store helpers (settings_get/set_brightness). Call
 *  quick_shade_init() once in setup() after the UI is built.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

#define QS_PANEL_H   368      // shade height (px); also the full pull distance
#define QS_STRIP_H    46      // top grab-strip height (the only place the pull arms)
#define QS_SCRIM_OPA   LV_OPA_COVER   // scrim is fully opaque black (flat fill = cheap; no per-frame blend)
#define QS_BRIGHT_MIN_PCT 3   // slider floor (~raw 8, near-dark but never fully off); 100% = raw 255

static lv_obj_t *qs_scrim  = nullptr;   // full-screen dim behind the panel (modal + tap-to-close)
static lv_obj_t *qs_panel  = nullptr;   // the sliding shade
static lv_obj_t *qs_strip  = nullptr;   // invisible top grab strip (arms the pull)
static lv_obj_t *qs_hdr     = nullptr;  // "Brightness" header (recolored on accent change)
static lv_obj_t *qs_slider  = nullptr;  // brightness slider
static lv_obj_t *qs_val     = nullptr;  // numeric brightness readout
static lv_obj_t *qs_caf_btn  = nullptr; // caffeine (keep-awake) toggle
static lv_obj_t *qs_mute_btn = nullptr; // mute toggle
static lv_obj_t *qs_mute_icon= nullptr; // speaker glyph inside the mute button

static bool    qs_open     = false;     // logically open (panel resting at y=0)?
static bool    qs_dragging = false;     // a finger drag is in progress
static int32_t qs_grab_dy  = 0;         // finger-Y offset within the panel, captured at press
static bool    qs_dirty    = false;     // panel moved -> the loop must push a frame this iteration

/* Legacy DIRECT-mode push flag. In the current PARTIAL render mode LVGL invalidates
 * the shade's area as it moves and my_disp_flush() pushes that rectangle directly, so
 * this flag is no longer what gets the shade onto the panel — it's kept (harmless,
 * read-and-cleared by the loop) for the DIRECT path and to avoid a stuck edge flag. */
static inline bool quick_shade_take_dirty(void) { bool d = qs_dirty; qs_dirty = false; return d; }

static inline int32_t qs_closed_y(void) { return -QS_PANEL_H; }
static inline int32_t qs_open_y(void)   { return 0; }

/* Scrim opacity is set ONCE on press, never recomputed per drag frame.
 *
 * The old scrim was translucent AND its opacity scaled with reveal distance, so it
 * alpha-blended against the live UI underneath on EVERY drag frame, over the whole
 * screen — the dominant cost that made the shade lag. Now the background switches to
 * fully OPAQUE black the instant the pull begins: a flat fill with nothing behind to
 * composite, so it's cheap, and the opaque panel slides over it to cover the screen
 * when fully open. */
static inline void qs_scrim_show(void) { lv_obj_set_style_bg_opa(qs_scrim, QS_SCRIM_OPA, 0); }

/* lv_anim exec: move the panel during the snap. The scrim no longer tracks the panel
 * position — it's set opaque once on press (qs_scrim_show) and stays opaque — so this
 * only moves the panel. */
static void qs_anim_exec(void *var, int32_t v) {
  (void)var;
  lv_obj_set_y(qs_panel, v);
  qs_dirty = true;        // keep pushing frames through the whole snap (incl. the last)
}

/* When a snap animation finishes fully closed, hide the scrim so it stops
 * intercepting touches meant for the UI underneath. The scrim stays fully OPAQUE
 * through the close slide (the panel slides up over opaque black — cheap, no blend);
 * we only hide it once the panel is gone. Reset its opacity to transparent so the
 * NEXT open's fast fade starts cleanly from 0.
 *
 * CRITICAL: once the panel/scrim are gone, quick_shade_active() is false, so the loop
 * STOPS pushing frames — but hiding the opaque scrim needs ONE more render+flush to
 * actually paint the watchface back. Without forcing it, the last (all-black) frame
 * lingers on the panel until something else redraws (the "shade closes to a black
 * screen until you nudge it" bug). So invalidate the whole screen AND set qs_dirty so
 * the loop guarantees that final repaint. */
static void qs_anim_done(lv_anim_t *a) {
  (void)a;
  if (lv_obj_get_y(qs_panel) <= qs_closed_y() + 1) {
    lv_obj_add_flag(qs_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(qs_scrim, LV_OPA_TRANSP, 0);
    lv_obj_invalidate(lv_scr_act());   // force the revealed watchface to repaint
    qs_dirty = true;                   // ...and make the loop push that final frame
  }
}

/* Animate the panel to a target Y (open=0 or closed=-H) with an ease-out snap. */
static void qs_animate_to(int32_t target) {
  lv_obj_clear_flag(qs_scrim, LV_OBJ_FLAG_HIDDEN);   // visible during the whole snap
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, qs_panel);
  lv_anim_set_exec_cb(&a, qs_anim_exec);
  lv_anim_set_values(&a, lv_obj_get_y(qs_panel), target);
  lv_anim_set_time(&a, 200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_completed_cb(&a, qs_anim_done);
  lv_anim_start(&a);
  qs_open = (target == qs_open_y());
}

static void quick_shade_close(void) { qs_animate_to(qs_closed_y()); }
static bool quick_shade_is_open(void) { return qs_open; }

/* True whenever the shade is open or being dragged — i.e. it's interactive and
 * on screen. The loop pushes a frame every iteration while this holds, so widgets
 * INSIDE the shade (the slider knob, the toggles) update live even on the bare
 * watchface, which otherwise has nothing to trigger a framebuffer push. */
static inline bool quick_shade_active(void) { return qs_open || qs_dragging; }

/* Draw a small coffee-cup icon (body + ring handle + two steam dashes) into the
 * caffeine button. LVGL's symbol font has no coffee glyph, so we build it from a
 * few white shapes — no font change needed. The button's bg color signals state. */
static void qs_build_coffee(lv_obj_t *btn) {
  lv_obj_t *c = lv_obj_create(btn);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, 34, 30);
  lv_obj_center(c);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  // The coffee icon is decorative overlay on the button. By default these child
  // objects sit ON TOP of the button and SWALLOW a tap that lands on them — so
  // pressing the mug itself (dead center) never reached the button. Make the icon
  // click-transparent: not clickable, and bubble any event up to the parent button.
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t *body = lv_obj_create(c);          // cup body
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, 22, 18);
  lv_obj_align(body, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(body, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(body, 4, 0);

  lv_obj_t *h = lv_obj_create(c);             // ring handle
  lv_obj_remove_style_all(h);
  lv_obj_set_size(h, 12, 14);
  lv_obj_align(h, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
  lv_obj_set_style_border_color(h, lv_color_white(), 0);
  lv_obj_set_style_border_width(h, 3, 0);
  lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);

  for (int i = 0; i < 2; i++) {               // steam
    lv_obj_t *s = lv_obj_create(c);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 3, 8);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 5 + i * 8, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s, 2, 0);
  }

  // Make every shape click-transparent so a tap anywhere on the icon reaches the
  // button. Each child must clear CLICKABLE and bubble, so an event chains child ->
  // c -> button. (Done in one sweep over c's whole subtree.)
  uint32_t kids = lv_obj_get_child_count(c);
  for (uint32_t i = 0; i < kids; i++) {
    lv_obj_t *k = lv_obj_get_child(c, i);
    lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
  }
}

/* Paint both toggle buttons to match their state: accent bg = engaged. The mute
 * button also swaps its speaker glyph (muted vs. sound-on). */
static void qs_refresh_toggles(void) {
  if (qs_caf_btn)
    lv_obj_set_style_bg_color(qs_caf_btn,
        lv_color_hex(caffeine_get() ? ui_accent_hex() : 0x1F1F1F), 0);
  if (qs_mute_btn)
    lv_obj_set_style_bg_color(qs_mute_btn,
        lv_color_hex(settings_get_mute() ? ui_accent_hex() : 0x1F1F1F), 0);
  if (qs_mute_icon)
    lv_label_set_text(qs_mute_icon,
        settings_get_mute() ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
}

static void qs_caf_cb(lv_event_t *e) {
  (void)e;
  caffeine_set(!caffeine_get());
  haptics_pulse(15);
  qs_refresh_toggles();
}
static void qs_mute_cb(lv_event_t *e) {
  (void)e;
  settings_set_mute(!settings_get_mute());
  haptics_pulse(15);
  qs_refresh_toggles();
}

/* Recolor the resident shade to the current accent. The shade is built once at
 * boot (unlike the menu screens, which rebuild on open), so the Appearance picker
 * calls this to update it live when the accent changes. */
static void quick_shade_restyle(void) {
  if (qs_hdr)
    lv_obj_set_style_text_color(qs_hdr, lv_color_hex(ui_accent_hex()), 0);
  if (qs_slider) {
    lv_obj_set_style_bg_color(qs_slider, lv_color_hex(ui_accent_hex()), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(qs_slider, lv_color_hex(ui_accent_hex()), LV_PART_KNOB);
  }
  qs_refresh_toggles();                   // engaged-button color follows the accent
}

/* Brightness is exposed as a PERCENT of full (100% = raw 255), floored at
 * QS_BRIGHT_MIN_PCT (~2/3) so the panel never goes uncomfortably dim. */
static inline uint8_t qs_pct_to_raw(int pct) {
  if (pct < QS_BRIGHT_MIN_PCT) pct = QS_BRIGHT_MIN_PCT;
  if (pct > 100) pct = 100;
  int raw = (pct * 255 + 50) / 100;          // rounded; 67%->171, 100%->255
  return (uint8_t)(raw > 255 ? 255 : raw);
}
static inline int qs_raw_to_pct(uint8_t raw) {
  int pct = (raw * 100 + 127) / 255;          // rounded; 255->100, 170->67
  if (pct < QS_BRIGHT_MIN_PCT) pct = QS_BRIGHT_MIN_PCT;
  if (pct > 100) pct = 100;
  return pct;
}

/* Sync the slider/readout to the live brightness (called as a pull begins). */
static void qs_sync_slider(void) {
  int pct = qs_raw_to_pct(settings_get_brightness());
  if (qs_slider) lv_slider_set_value(qs_slider, pct, LV_ANIM_OFF);
  if (qs_val)    lv_label_set_text_fmt(qs_val, "%d%%", pct);
}

/* The drag tracker — attached to BOTH the top strip (to open) and the panel (to
 * drag closed). Uses absolute finger-Y minus the offset captured at press, so
 * the panel tracks the finger 1:1 no matter where the pull started. */
static void qs_drag_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  if (code == LV_EVENT_PRESSED) {
    qs_dragging = true;
    qs_grab_dy  = p.y - lv_obj_get_y(qs_panel);     // finger position within panel space
    lv_anim_del(qs_panel, qs_anim_exec);            // cancel any in-flight snap
    lv_obj_clear_flag(qs_scrim, LV_OBJ_FLAG_HIDDEN);
    qs_scrim_show();                                 // background switches to opaque black instantly
    qs_sync_slider();
    qs_dirty = true;
  } else if (code == LV_EVENT_PRESSING) {
    if (!qs_dragging) return;
    int32_t y = p.y - qs_grab_dy;
    if (y < qs_closed_y()) y = qs_closed_y();
    if (y > qs_open_y())   y = qs_open_y();
    lv_obj_set_y(qs_panel, y);
    // NOTE: scrim opacity is NOT touched here — the per-frame fullscreen blend was the
    // lag. The scrim faded to opaque on press; it just stays opaque while we drag.
    qs_dirty = true;        // follow the finger frame-by-frame on the clock face
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (!qs_dragging) return;
    qs_dragging = false;
    // Snap: open if revealed past ~1/3 of the panel, else fall back closed.
    if (lv_obj_get_y(qs_panel) > qs_closed_y() + QS_PANEL_H / 3)
      qs_animate_to(qs_open_y());
    else
      qs_animate_to(qs_closed_y());
  }
}

/* Tap the dimmed area below the panel to dismiss. */
static void qs_scrim_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) quick_shade_close();
}

/* Live brightness as the slider moves (applies + persists, same as the old
 * Brightness sub-app). */
static void qs_slider_cb(lv_event_t *e) {
  lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
  int pct = lv_slider_get_value(sl);          // 67..100
  settings_set_brightness(qs_pct_to_raw(pct));
  if (qs_val) lv_label_set_text_fmt(qs_val, "%d%%", pct);
}

/* Build the shade once. Call from setup() after the display/menu exist. */
static void quick_shade_init(void) {
  lv_obj_t *sys = lv_layer_sys();   // above lv_layer_top() -> always over apps/menu

  // --- scrim: full-screen dim, hidden (so it can't block touch) until opening ---
  qs_scrim = lv_obj_create(sys);
  lv_obj_remove_style_all(qs_scrim);
  lv_obj_set_size(qs_scrim, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(qs_scrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(qs_scrim, 0, 0);
  lv_obj_add_flag(qs_scrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(qs_scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(qs_scrim, qs_scrim_cb, LV_EVENT_CLICKED, nullptr);

  // --- panel: the sliding shade, parked just above the top edge ---
  qs_panel = lv_obj_create(sys);
  lv_obj_set_size(qs_panel, LV_PCT(100), QS_PANEL_H);
  lv_obj_set_pos(qs_panel, 0, qs_closed_y());
  lv_obj_set_style_bg_color(qs_panel, lv_color_hex(0x0E0E0E), 0);
  lv_obj_set_style_bg_opa(qs_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(qs_panel, 2, 0);    // thin divider at the bottom edge only
  lv_obj_set_style_border_color(qs_panel, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_border_side(qs_panel, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_radius(qs_panel, 0, 0);
  lv_obj_set_style_pad_all(qs_panel, 0, 0);
  lv_obj_clear_flag(qs_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(qs_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(qs_panel, qs_drag_cb, LV_EVENT_ALL, nullptr);

  // header (sun glyph + label) and live value
  qs_hdr = lv_label_create(qs_panel);
  lv_obj_set_style_text_font(qs_hdr, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(qs_hdr, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(qs_hdr, LV_SYMBOL_EYE_OPEN "  Brightness");
  lv_obj_align(qs_hdr, LV_ALIGN_TOP_LEFT, 32, 54);

  qs_val = lv_label_create(qs_panel);
  lv_obj_set_style_text_font(qs_val, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(qs_val, lv_color_white(), 0);
  lv_label_set_text(qs_val, "--");
  lv_obj_align(qs_val, LV_ALIGN_TOP_RIGHT, -66, 54);

  // the slider (10..255 so the panel itself never goes pitch black)
  qs_slider = lv_slider_create(qs_panel);
  lv_obj_set_width(qs_slider, 330);
  lv_obj_set_height(qs_slider, 16);
  lv_obj_align(qs_slider, LV_ALIGN_CENTER, 0, -38);   // sits higher to leave room for the toggles
  lv_slider_set_range(qs_slider, QS_BRIGHT_MIN_PCT, 100);   // percent of full (100% = raw 255)
  lv_obj_set_style_bg_color(qs_slider, lv_color_hex(ui_accent_hex()), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(qs_slider, lv_color_hex(ui_accent_hex()), LV_PART_KNOB);
  lv_obj_add_event_cb(qs_slider, qs_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  qs_sync_slider();

  // --- quick toggles: Caffeine (keep-awake, LEFT) + Mute (sound off / still
  //     vibrate, RIGHT). Wide buttons spread to the panel edges = big hit areas. ---
  // NOTE: the container MUST be at least as tall as the buttons inside it. LVGL
  // clips a child's TOUCH area to its parent's bounds (even though it still DRAWS
  // the overflow), so an 80px button in a 72px box had its top/bottom ~4px render
  // but not register taps — the buttons looked bigger than they were tappable.
  // Sized to 80 (== button height) so the whole visible button accepts touch.
  lv_obj_t *toggles = lv_obj_create(qs_panel);
  lv_obj_remove_style_all(toggles);
  lv_obj_set_size(toggles, 330, 80);
  lv_obj_align(toggles, LV_ALIGN_CENTER, 0, 46);
  lv_obj_clear_flag(toggles, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(toggles, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(toggles, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  qs_caf_btn = lv_btn_create(toggles);              // LEFT
  lv_obj_set_size(qs_caf_btn, 150, 80);
  lv_obj_set_style_radius(qs_caf_btn, 16, 0);
  lv_obj_set_style_shadow_width(qs_caf_btn, 0, 0);
  lv_obj_add_event_cb(qs_caf_btn, qs_caf_cb, LV_EVENT_CLICKED, nullptr);
  qs_build_coffee(qs_caf_btn);

  qs_mute_btn = lv_btn_create(toggles);             // RIGHT
  lv_obj_set_size(qs_mute_btn, 150, 80);
  lv_obj_set_style_radius(qs_mute_btn, 16, 0);
  lv_obj_set_style_shadow_width(qs_mute_btn, 0, 0);
  lv_obj_add_event_cb(qs_mute_btn, qs_mute_cb, LV_EVENT_CLICKED, nullptr);
  qs_mute_icon = lv_label_create(qs_mute_btn);
  lv_obj_set_style_text_font(qs_mute_icon, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(qs_mute_icon, lv_color_white(), 0);
  // Fixed-width, LEFT-aligned box so the speaker cone (the left of both glyphs)
  // stays put when the "waves" vanish — otherwise the narrower mute glyph
  // recenters and the speaker looks like it slides forward.
  lv_obj_set_width(qs_mute_icon, 40);
  lv_label_set_long_mode(qs_mute_icon, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(qs_mute_icon, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_center(qs_mute_icon);

  qs_refresh_toggles();   // set initial bg colors + the mute glyph

  // a little grab-handle pill at the bottom edge as a "pull me" affordance
  lv_obj_t *handle = lv_obj_create(qs_panel);
  lv_obj_remove_style_all(handle);
  lv_obj_set_size(handle, 54, 5);
  lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_bg_color(handle, lv_color_hex(0x555555), 0);
  lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(handle, 3, 0);

  // --- strip: invisible top-edge grab area; the ONLY place the pull arms, so it
  //     never steals downward scrolls from the screens below it. Created LAST so
  //     it sits on top and is always pressable. ---
  qs_strip = lv_obj_create(sys);
  lv_obj_remove_style_all(qs_strip);
  lv_obj_set_size(qs_strip, LV_PCT(100), QS_STRIP_H);
  lv_obj_set_pos(qs_strip, 0, 0);
  lv_obj_set_style_bg_opa(qs_strip, 0, 0);          // fully transparent
  lv_obj_add_flag(qs_strip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(qs_strip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(qs_strip, qs_drag_cb, LV_EVENT_ALL, nullptr);
}
