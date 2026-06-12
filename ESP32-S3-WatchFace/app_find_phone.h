/* ============================================================================
 *  app_find_phone.h — "Find My Phone" sub-app: ring the paired phone.
 *
 *  Header-only module compiled into the .ino TU, sharing the menu statics.
 *  INCLUDE AFTER app_menu.h (uses app_screen_begin / app_scr) and after
 *  ble_provision.h (uses ble_ping_phone / ble_phone_connected) and
 *  settings_store.h (ui_accent_hex) and the FONT_* macros.
 *
 *  A single big round button. Tapping it calls ble_ping_phone(), which notifies
 *  the connected companion app so the phone sounds a continuous alarm. The status
 *  line reflects the result; if no phone is connected it says so instead.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

static lv_obj_t *fmp_status = nullptr;   // status label (loop/LVGL thread only)
static lv_obj_t *fmp_btn_lbl = nullptr;  // "Ring"/"Stop" caption (reassigned each open)
static bool      fmp_ringing = false;    // we asked the phone to ring; next tap stops

/* Clear our pointer if the label is torn down with the screen, so a later rebuild
 * never writes through a freed object. */
static void fmp_status_deleted_cb(lv_event_t *e) {
  if (lv_event_get_target(e) == fmp_status) fmp_status = nullptr;
}

static void fmp_set_status(const char *msg, uint32_t color) {
  if (!fmp_status) return;
  lv_label_set_text(fmp_status, msg);
  lv_obj_set_style_text_color(fmp_status, lv_color_hex(color), 0);
}

/* Ring button: ping the phone (no-op + hint if nothing is connected). Gadgetbridge
 * rings until dismissed on the phone or told to stop, so the button toggles:
 * first tap rings, second tap stops. */
static void fmp_ring_cb(lv_event_t *e) {
  (void)e;
  if (fmp_ringing) {
    ble_stop_phone_ring();
    fmp_ringing = false;
    if (fmp_btn_lbl) lv_label_set_text(fmp_btn_lbl, "Ring");
    fmp_set_status("Tap to ring your phone.", 0xAAAAAA);
    return;
  }
  if (ble_ping_phone()) {
    fmp_ringing = true;
    if (fmp_btn_lbl) lv_label_set_text(fmp_btn_lbl, "Stop");
    fmp_set_status("Ringing your phone...\nTap again to stop.", ui_accent_hex());
  } else {
    fmp_set_status("No phone connected.\nOpen the companion app first.", 0xFF9F0A);
  }
}

static void app_open_find_phone(void) {
  app_screen_begin("Find Phone");

  // Big round "Ring" button, accent-filled (matches the watch's accent system).
  lv_obj_t *btn = lv_btn_create(app_scr);
  lv_obj_set_size(btn, 180, 180);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, -24);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(ui_accent_hex()), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(btn, fmp_ring_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *ic = lv_label_create(btn);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_34, 0);
  lv_obj_set_style_text_color(ic, lv_color_black(), 0);
  lv_label_set_text(ic, LV_SYMBOL_CALL);
  lv_obj_align(ic, LV_ALIGN_CENTER, 0, -16);

  fmp_btn_lbl = lv_label_create(btn);
  lv_obj_set_style_text_font(fmp_btn_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(fmp_btn_lbl, lv_color_black(), 0);
  lv_label_set_text(fmp_btn_lbl, "Ring");
  lv_obj_align(fmp_btn_lbl, LV_ALIGN_CENTER, 0, 28);
  fmp_ringing = false;   // fresh screen -> fresh toggle state (the phone self-stops
                         // when dismissed there; we just reset our side)

  // Status line below the button.
  fmp_status = lv_label_create(app_scr);
  lv_obj_add_event_cb(fmp_status, fmp_status_deleted_cb, LV_EVENT_DELETE, nullptr);
  lv_obj_set_style_text_font(fmp_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_align(fmp_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(fmp_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(fmp_status, 360);
  lv_obj_align(fmp_status, LV_ALIGN_CENTER, 0, 120);

  if (ble_phone_connected())
    fmp_set_status("Tap to ring your phone.", 0xAAAAAA);
  else
    fmp_set_status("No phone connected.\nOpen the companion app first.", 0xFF9F0A);
}
