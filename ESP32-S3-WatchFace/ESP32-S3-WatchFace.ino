/* ============================================================================
 *  ESP32-S3-WatchFace.ino — Digital watch OS for Waveshare ESP32-S3-Touch-AMOLED-2.06
 *
 *  Built on Waveshare's proven hardware init (CO5300 QSPI AMOLED via
 *  Arduino_GFX, FT3168 touch, PCF85063 RTC).
 *
 *  REQUIREMENTS:
 *    - esp32 (Espressif Systems) = v3.3.8
 *    - lvgl                      = v9.5.0   (+ lv_conf.h in libraries/)
 *    - GFX_Library_for_Arduino   = v1.6.5
 *    - Arduino_DriveBus          = v1.0.1
 *    - SensorLib                 = v0.4.1
 *    - XPowersLib                = v0.3.3
 *    - Mylibrary
 *
 *  ARDUINO IDE BOARD SETTINGS:
 *    Board:                                "Waveshare ESP32-S3-Touch-AMOLED-2.06"
 *    Erase All Flash Before Sketch Upload: "Enabled" (Only necessary to be enabled for the first flash)
 *    Events Run On:                        "Core 0"
 *    Flash Mode:                           "QIO 120 MHz"
 *    Arduino Runs On:                      "Core 1"
 *    Flash Size:                           32MB (fixed by the board)
 *    Partition:                            "Custom"  -> uses partitions.csv in THIS
 *                                          sketch folder (20KB NVS + 4MB app0/app1
 *                                          + 24MB FAT on the 32MB chip). app0 is
 *                                          pinned at 0x10000 (the core flashes the
 *                                          app there) — moving it (e.g. a bigger NVS
 *                                          before it, or "Max APP 32MB") boots to a
 *                                          BLACK SCREEN.
 *    PSRAM:                                "Enabled"
 *    Upload Mode:                          "UART0 / Hardware CDC"
 * ========================================================================== */

#include <Wire.h>
#include <Arduino.h>
#include "pin_config.h"  // from Mylibrary — real GPIO map
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "SensorPCF85063.hpp"
#include "XPowersLib.h"  // AXP2101 PMU — battery percent / charging
#include "HWCDC.h"
#include <WiFi.h>  // notification fetch
#include <HTTPClient.h>
#include <Preferences.h>  // NVS key-value storage (persists across reboots)
#include <esp_sleep.h>    // deep sleep + timer wake
#include <esp_system.h>   // esp_reset_reason() — on-screen boot-cause banner
#include <driver/rtc_io.h>  // RTC-GPIO pull config for reliable EXT0 (BOOT) wake
#include "device_info.h"    // product name / version / author — used by About + boot log

/* Deep-sleep peripheral-rail cut: turning AXP2101 peripheral rails OFF during
 * deep sleep cuts the 24/7 quiescent draw that actually drains the battery. We
 * can't trust the schematic for which rail powers what, so the firmware PROBES
 * each rail at runtime and only cuts the ones proven safe (PMU still answers on
 * I2C after the rail is disabled). See the rail-probe block above the sleep code
 * further down. DCDC1 (ESP32 3.3V) and the RTC supply are never candidates. */

/* ===================== Notification fetch config =========================
 * The watch pulls queued notifications from the notify-server (Rust) over HTTPS.
 * NOTE: these credentials are stored in firmware.
 *
 * NOTIFY_URL MUST be https:// — the watch validates the server's TLS cert against
 * the pinned Let's Encrypt root in notify_ca.h. Point it at your HTTPS front:
 *   - Tailscale Funnel:  https://<host>.<tailnet>.ts.net/notify   (no port, no
 *     port-forwarding — reachable on ANY network; see notify-server/README)
 *   - or a reverse proxy with a domain: https://notify.example.com/notify
 * Do NOT use a raw IP or a port here: the cert is issued for a HOSTNAME, so TLS
 * validation needs the matching hostname (and Funnel serves on 443). */
#define WIFI_SSID ""
#define WIFI_PASS ""
#define NOTIFY_URL ""
#define NOTIFY_TOKEN ""
#define NOTIFY_FETCH_MS 15000UL  // while screen is on, poll every 15s
#define WIFI_CONNECT_TIMEOUT_MS 8000UL

/* ONE-SHOT SD POWER-RAIL DIAGNOSTIC. Set to 1, flash, open the serial monitor, and
 * read the "[sddiag]" lines — they report which AXP2101 rail powers the microSD so
 * we can keep cutting all the others in sleep. Set back to 0 after reading.
 * (Left at 0: on the original board the SD circuit was hardware-defective — RMA'd —
 * so the diagnostic isn't useful until the replacement board is in hand. The
 * sd_rail_diag() code + the 07_LVGL_SD_Test example folder are kept for re-testing.) */
#define SD_RAIL_DIAG 0

/* ===================== NTP time sync config ==============================
 * NTP returns UTC; NTP_TZ converts it to LOCAL wall-clock time (and handles DST
 * automatically). The current value is Central European Time (UTC+1, DST +2) —
 * change it if you're in a different zone. POSIX TZ format:
 *   CET-1CEST,M3.5.0,M10.5.0/3  => base UTC+1, summer UTC+2, DST Mar/Oct.
 * (US Eastern would be "EST5EDT,M3.2.0,M11.1.0".) We re-sync at most every
 * NTP_SYNC_INTERVAL_S to keep radio time minimal. */
#define NTP_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER1 "pool.ntp.org"
#define NTP_SERVER2 "time.nist.gov"
#define NTP_SYNC_INTERVAL_S (6UL * 3600UL)   // re-sync every 6 hours

HWCDC USBSerial;

/* ---- Display: CO5300 over QSPI (Waveshare's exact construction) ----------- */
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS /* CS */, LCD_SCLK /* SCK */, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

/* Keep a CO5300-typed pointer too: setBrightness() lives on Arduino_CO5300, not
 * the Arduino_GFX base class, so we need the derived type to call it. Both point
 * at the same object. */
Arduino_CO5300 *co5300 = new Arduino_CO5300(
  bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT,
  22 /* col_offset1 */, 0 /* row_offset1 */, 0 /* col_offset2 */, 0 /* row_offset2 */);
Arduino_GFX *gfx = co5300;

/* Bus-typed pointer for the async flush: writePixelsBeAsync()/waitAsync() are
 * LOCAL PATCH additions on Arduino_ESP32QSPI, not the Arduino_DataBus base. */
Arduino_ESP32QSPI *qspi_bus = static_cast<Arduino_ESP32QSPI *>(bus);

/* ---- Touch: FT3168 over I2C ---------------------------------------------- */
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
  std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
  IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, Arduino_IIC_Touch_Interrupt));
/* Set by the touch ISR on every touch edge. The loop consumes it to refresh the
 * idle-sleep timer DIRECTLY — independent of LVGL's indev polling and of the I2C
 * finger-count read, both of which can miss a quick tap and let the 2-minute
 * idle timeout fire "on touch" while on battery. A raw edge = real activity. */
static volatile bool s_touch_activity = false;
void Arduino_IIC_Touch_Interrupt(void) {
  FT3168->IIC_Interrupt_Flag = true;
  s_touch_activity = true;
}

/* ---- RTC ----------------------------------------------------------------- */
SensorPCF85063 rtc;

/* ---- PMU (AXP2101): battery gauge + charge status ------------------------ */
XPowersPMU power;
static bool pmu_ok = false;

/* ---- Sleep / background-notification model -------------------------------
 * When idle, the watch enters DEEP SLEEP (screen off, CPU off, ~low power) and
 * the RTC timer wakes it every CHECK_INTERVAL minutes. On a timer wake it does
 * a LIGHT check: WiFi + server only, WITHOUT initializing the display. If a new
 * notification is found, it boots the full UI and shows it; otherwise it goes
 * straight back to deep sleep, so the screen never lights.
 *
 * Every deep-sleep wake is a full reboot (setup() re-runs). The PCF85063 keeps
 * time across sleep. A long PWR hold (~6s) still does a true hardware power-off
 * via the AXP2101 (escape hatch); a Settings toggle (later) can disable checks.
 */
#define IDLE_SLEEP_MS (2UL * 60UL * 1000UL)  // idle this long on the face -> sleep
#define DIM_IDLE_MS   (10UL * 1000UL)         // idle this long on BATTERY -> auto-dim the panel
static uint32_t lastActivityMs = 0;          // reset on any touch

/* True if THIS boot was woken by a BOOT press (EXT0). That same press is still
 * held LOW as the loop starts; we swallow it so the wake press only wakes the
 * screen and doesn't immediately fire the menu/sleep action. */
static bool s_woke_from_boot = false;

/* While the BOOT button is held, suppress touch input so a stray touch can't
 * interfere with (or be interpreted alongside) the button press. Set/cleared by
 * the loop's BOOT handler; read by the LVGL touch-read callback. */
static volatile bool boot_suppress_touch = false;

/* Set when the watch wakes interactively (BOOT press / cold power-on): the loop
 * does ONE immediate notification fetch instead of waiting out the 30s poll, so
 * waking the face also checks for new notifications right away. */
static bool s_check_on_wake = false;

/* s_check_interval_min and rtc_last_notif_id moved to watch_base.h (shared by
 * settings, the sleep timer-arm, and the network task). */

/* Sleep-intent marker in RTC RAM (survives deep sleep, lost on true power-off).
 * Set to the magic ONLY on the two intentional sleep paths (idle timeout, BOOT
 * double-tap). On boot we check it: if the reset reason is DEEPSLEEP but this
 * marker is NOT set, the chip "deep-slept" via some other path; if the reset
 * reason is a PANIC/WDT/BROWNOUT, this distinguishes a real crash from a clean
 * sleep even when the screen stayed dark with no serial. Cleared right after we
 * read it on boot, so a subsequent crash can't masquerade as an intended sleep. */
#define SLEEP_INTENT_MAGIC 0x5EED5EEDu
RTC_DATA_ATTR static uint32_t rtc_sleep_intent = 0;

/* Unix time (seconds) of the last successful NTP sync, kept in RTC RAM so it
 * survives deep-sleep wakes (every wake is a reboot). 0 = never synced this
 * power session -> sync at the next WiFi connect. Lost on true power-off, which
 * is fine: a cold boot syncs anyway. */
RTC_DATA_ATTR static uint32_t rtc_last_ntp_epoch = 0;

/* ---- LVGL plumbing ------------------------------------------------------- */
/* RENDER MODE — PARTIAL.
 * DIRECT_RENDER_MODE kept a full-screen framebuffer in PSRAM and pushed ALL ~410KB
 * to the panel every frame, regardless of how little changed. That serial, fixed-cost
 * fullscreen blit (CPU read from slow PSRAM -> QSPI) was the real FPS ceiling (~20-30
 * FPS) — parallel rendering finished early and then waited on it (idle cores).
 *
 * PARTIAL mode renders into small line-buffers and flushes ONLY the dirty rectangle
 * LVGL computed (my_disp_flush gets `area`). A moving clock hand / scrolling list now
 * pushes a small region instead of the whole screen, so present cost collapses. The
 * buffers are small enough to live in fast INTERNAL SRAM (the other half of the win:
 * no slow PSRAM read per push), and we use TWO so LVGL can render the next tile while
 * the previous one is being sent. Leave DIRECT_RENDER_MODE undefined.
 *
 * Smaller tiles => more flush calls per frame,
 * but each is a fast SRAM->QSPI partial push — still far cheaper than the old fullscreen
 * blit.
 *
 * SIZING IS FIXED AT COMPILE TIME — do NOT size this from free-heap at boot. I
 * tried that (tallest tier that left a reserve): a boot-time free-RAM check can't
 * know what BLE/WiFi bring-up will allocate LATER, and a too-tall tier starved the
 * radios -> alloc-failure panic -> bootloop. 110 spends part of the ~74KB of .bss
 * the notif/wifi stores freed when they moved to PSRAM: pair = ~148KB, 
 * so net headroom is still ~20KB ABOVE . */
// #define DIRECT_RENDER_MODE
#define PARTIAL_BUF_LINES 104           // lines per partial buffer (x2)
static uint32_t screenWidth, screenHeight, bufSize;
static lv_display_t *disp;
static lv_color_t *disp_draw_buf;
static lv_color_t *disp_draw_buf2;     // second partial buffer (double-buffered flush)
static uint32_t lastMillis = 0;

/* ============================ Watch-face UI =============================== */
/* Shared UI font aliases (FONT_TIME/LABEL/SMALL/TOP). Pulled out into their own
 * header because the whole UI layer — every app_*.h screen, quick_shade.h, and
 * watchface.h — references them, so they must be defined before those includes. */
#include "ui_fonts.h"

/* ===================== Data modules (split out of this sketch) ============
 * The settings store, power model, and the WiFi/notification stores each live in
 * their own header now. They are header-only and compiled into this single
 * translation unit, so they share these globals — they just have to be included
 * HERE: after the hardware objects (co5300 / rtc / power / WiFi) they drive, and
 * before the watch-face UI + menu below that consume them. ORDER MATTERS:
 *   watch_base.h    — mutexes + shared RTC-session globals (first; others use it)
 *   settings_store.h— owns `prefs`; drives co5300/CPU/WiFi
 *   power_model.h   — reads settings globals + rtc + i2c_lock
 *   wifi_store.h    — uses `prefs` + the WIFI_SSID/WIFI_PASS macros above
 *   notif_store.h   — uses `prefs` + store_lock */
#include "watch_base.h"
#include "player_state.h"      // source-agnostic Now Playing state (fed by AMS/HTTP/Android)
#include "cpu_usage.h"         // power-safe per-core CPU usage estimate (idle-hook based; for the Power app)
#include "core_voltage.h"      // EXPERIMENTAL core (dig_dbias) undervolt — default OFF, see it's header
#include "clocks.h"            // read-only clock-tree dump (observability for the overclock work)
#include "overclock.h"         // EXPERIMENTAL CPU overclock (>240 MHz) — default OFF, see it's header
#include "settings_store.h"
#include "power_model.h"
#include "haptics.h"          // vibration motor (GPIO18) — non-blocking pattern player
#define AUDIO_SMALL_SPEAKER 0 // 1 = THIS watch has the tiny in-ear speaker mod (full drive).
                              // Set to 0 (or delete) when building for a stock watch — the
                              // original speaker clips and can be damaged at full drive!
#include "audio_alarm.h"      // alarm chime: ES8311 codec + speaker amp (synthesized, no files)
#include "sd_card.h"          // shared 1-bit SD_MMC mount (WiFi CSV + battery log)
#include "storage_fs.h"       // pick SD-if-present-else-FFat for ALL persistent files
#include "timer_store.h"      // countdown timer + alarm clocks (rtc_now_epoch, prefs,
                              // alarms.csv via store_fs — hence AFTER storage_fs.h)
/* Optional SD trend-logger for battery health — MUST follow power_model.h, which
 * forward-declares batt_health_sd_log(); this defines it. Degrades to a no-op if
 * no SD card is present (the NVS running-average health is unaffected). */
#include "batt_health_sd.h"
#include "wifi_store.h"
#include "notif_store.h"
#include "notif_archive_sd.h"   // unlimited notification history on SD (CSV); flash is the newest-32 cache
#include "notif_icons.h"        // per-category notification icons (MDI font; built-in fallback)
#include "ble_provision.h"      // BLE peripheral: encrypted WiFi provisioning + find-phone (lazy; off in sleep)

/* Rail-probe read accessors + recalibrate, used by the Power screen (app_power.h).
 * Defined later in this .ino next to the sleep code; forward-declared here so the
 * screen can show each rail's verdict and offer a re-test. */
static const char *rail_name(uint8_t i);
static uint8_t      rail_state_count(void);
static uint8_t      rail_state_raw(uint8_t i);
static const char  *rail_verdict_str(uint8_t i);
static bool         rail_cut_get(uint8_t i);
static void         rail_cut_set(uint8_t i, bool on);
static uint8_t      rails_cut_count(void);

/* App menu, split into per-screen modules. Included AFTER the FONT_* defines
 * (they use FONT_LABEL / FONT_SMALL) and after the settings_* helpers + the
 * notification/WiFi stores above. ORDER MATTERS:
 *   - app_menu.h FIRST: defines the screen shell (app_scr / app_screen_begin)
 *     and forward-declares every app_open_* entry point.
 *   - the leaf screens next (power / wifi_ble / notifications):
 *     they use the shell and define their own app_open_*.
 *   - app_settings.h LAST of the settings group: it dispatches to
 *     app_open_power / _wifi_ble, so those must be defined first. */
#include "screen_cache.h"       // PSRAM pre-render cache: blit a static screen instantly on open, then render live over it
#include "app_menu.h"
#include "app_power.h"
#include "app_wifi_ble.h"
#include "app_settings.h"
#include "app_notifications.h"
#include "app_player.h"         // Now Playing (reads player_state; AMS/HTTP/Android feed it)
#include "quick_shade.h"        // pull-down brightness shade (uses settings_get/set_brightness)
#include "app_appearance.h"     // accent-color picker (uses quick_shade_restyle + ui_accent_*)
#include "app_timer.h"          // Timer + Stopwatch + alarm overlay (uses timer_store + haptics)
#include "app_find_phone.h"     // Find My Phone: ring the paired phone (uses ble_ping_phone)
#include "app_files.h"          // Files: on-device browser for FFat + SD (view/delete/space)

/* BOOT button (Key1) is wired to GPIO0. While the watch is running we can read
 * it as a normal input (LOW = pressed, it has a pull-up). A press toggles the
 * app menu. (BOOT cannot power the device on from full-off — different thing —
 * but works fine as an input while running.) */
#define BOOT_BTN_GPIO 0

/* The watch-face widgets (the big clock) and all their
 * watchface_*() setters live in watchface.h. Included HERE, after the app
 * modules above: the only watch-face symbol they use is watchface_refresh_bell(),
 * which watch_base.h forward-declares, so the face's definitions can follow them.
 * Uses the FONT_* aliases and the screenWidth global. */
#include "watchface.h"

/* ===================== Notification overlay + fetch ====================== */
/* A simple popup card at the bottom of the screen showing the latest fetched
 * notification. Tapping the watch (any touch) dismisses it. */
static lv_obj_t *notif_card = nullptr;
static lv_obj_t *notif_title = nullptr;
static lv_obj_t *notif_body = nullptr;

static void notif_card_create(void) {
  lv_obj_t *scr = lv_scr_act();
  notif_card = lv_obj_create(scr);
  lv_obj_set_size(notif_card, 320, 120);
  lv_obj_align(notif_card, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_bg_color(notif_card, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_width(notif_card, 0, 0);  // no border
  lv_obj_set_style_radius(notif_card, 16, 0);
  lv_obj_clear_flag(notif_card, LV_OBJ_FLAG_SCROLLABLE);

  notif_title = lv_label_create(notif_card);
  lv_obj_set_style_text_font(notif_title, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(notif_title, lv_color_white(), 0);
  lv_obj_align(notif_title, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_label_set_long_mode(notif_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(notif_title, 300);

  notif_body = lv_label_create(notif_card);
  lv_obj_set_style_text_font(notif_body, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(notif_body, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(notif_body, LV_ALIGN_TOP_LEFT, 0, 36);
  lv_label_set_long_mode(notif_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(notif_body, 300);

  lv_obj_add_flag(notif_card, LV_OBJ_FLAG_HIDDEN);  // hidden until a notif arrives
}

/* Id of the notification currently shown on the popup card (0 = none). A tap
 * dismiss removes that entry from the stored list too. */
static uint64_t notif_card_id = 0;

/* Set by notif_dismiss() (called from the touch callback) to ask the loop for a
 * repaint — otherwise hiding the card wouldn't actually clear it from the panel,
 * since the loop only pushes the framebuffer when something is "dirty". */
static volatile bool notif_needs_repaint = false;

static void notif_show(uint64_t id, const char *title, const char *body) {
  if (!notif_card) return;
  notif_card_id = id;
  lv_label_set_text(notif_title, title);
  lv_label_set_text(notif_body, body);
  lv_obj_clear_flag(notif_card, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(notif_card);
  audio_notify_ding();   // single short note (mute-aware; silent while the alarm rings)
}

/* Hide the pop-up card from the watch face. This ONLY dismisses the card off the
 * screen — it does NOT delete the notification from history. The notification
 * stays in the Notifications app until you explicitly remove it there (tap a
 * row) or Clear all. (Previously any touch that dismissed the card also erased
 * the notification, which made things vanish from the app — that's fixed.)
 * No-op if the card isn't currently visible. */
static void notif_dismiss(void) {
  if (!notif_card || lv_obj_has_flag(notif_card, LV_OBJ_FLAG_HIDDEN)) return;
  lv_obj_add_flag(notif_card, LV_OBJ_FLAG_HIDDEN);
  notif_needs_repaint = true;            // ask the loop to clear it from the panel
  notif_card_id = 0;                     // forget which card was shown; keep it in history
}

/* ===================== Async networking (core 0 task) ====================
 * The WiFi connect + HTTP GET + NTP sync + the core-0 net task all live in
 * notif_net.h now (json_find_string, wifi_connect, ntp_sync_if_due,
 * notif_fetch_raw, net_task_fn, notif_request_fetch, plus the s_net_ and s_pop_
 * state). It uses the wifi/notif stores + watch_base locks included above and
 * the rtc object + the WIFI_/NTP_/NOTIFY_ config macros at the top of this file.
 * The deep-sleep light-check path below calls notif_fetch_raw, so it must be
 * included here, before setup()/loop(). */
#include "notif_net.h"
#include "ble_ancs.h"           // ANCS client: iPhone notifications over BLE -> the same store.
#include "ble_player_ams.h"     // AMS client: iPhone media (now-playing + controls) -> the Player.
                                // Included here (after notif_store/archive/net) because it feeds
                                // notif_store_add / na_append / s_pop_* and is called by ble_provision.h.
#include "ble_gadgetbridge.h"   // Gadgetbridge server: ANDROID notifications (Bangle.js protocol)
                                // -> the same store + the same s_pop_*/dirty handoff as ANCS.

/* Read the PMU and update the status row. Shows VBUS (charging input) voltage
 * while charging, otherwise the battery voltage. Returns true if anything
 * visible changed (so the caller can decide whether to repaint the panel). */
static int s_batt_pct = -2;  // -2 = never read yet
static bool s_charging = false;
static uint16_t s_mv = 0;
static bool refresh_battery(void) {
  if (!pmu_ok) {  // no PMU -> show USB-only state once
    if (s_batt_pct != -1) {
      s_batt_pct = -1;
      watchface_set_battery(-1, false, 0);
      return true;
    }
    return false;
  }
  i2c_lock();   // shared bus
  int pct = power.getBatteryPercent();
  bool chg = power.isCharging();
  uint16_t mv = chg ? power.getVbusVoltage() : power.getBattVoltage();
  i2c_unlock();

  drain_update(pct, chg);   // feed the %/hour tracker (runs even if display unchanged)

  // Report battery to a connected phone (BAS notify + Gadgetbridge status line) on
  // any %/charger change, plus ONCE per connection — delayed ~3s past connect so it
  // lands after the phone has subscribed (CCCD writes take a moment). Runs before
  // the display early-out below so the initial report fires even when nothing
  // visible changed.
  static bool     s_batt_reported   = false;
  static uint32_t s_batt_conn_since = 0;
  if (!ble_phone_connected()) {
    s_batt_conn_since = 0;
    s_batt_reported   = false;
  } else {
    if (!s_batt_conn_since) s_batt_conn_since = millis();
    bool settled = millis() - s_batt_conn_since > 3000;
    if ((settled && !s_batt_reported) || pct != s_batt_pct || chg != s_charging) {
      ble_report_battery(pct, chg);
      s_batt_reported = true;
    }
  }

  if (pct == s_batt_pct && chg == s_charging && mv == s_mv) return false;
  s_batt_pct = pct;
  s_charging = chg;
  s_mv = mv;
  watchface_set_battery(pct, chg, mv);
  return true;
}

/* ===================== LVGL display / input callbacks ===================== */
#if LV_USE_LOG != 0
static void my_print(lv_log_level_t level, const char *buf) {
  LV_UNUSED(level);
  USBSerial.println(buf);
  USBSerial.flush();
}
#endif
static uint32_t millis_cb(void) {
  return millis();
}

/* Set whenever LVGL actually rendered into the framebuffer this cycle. In direct-
 * render mode LVGL calls the flush cb once per refresh that had dirty areas — i.e.
 * exactly when something visually changed (a redraw, a button press ripple, scroll
 * inertia, an animation frame, a live label update). The loop reads-and-clears this
 * to push the panel ONLY then, so EVERY screen (watch face, menu, sub-apps) idles
 * the QSPI bus when nothing is moving — the same "freeze when nothing changes" the
 * clock face had, now driven by LVGL's own dirty-tracking instead of hand-set flags. */
static volatile bool s_lvgl_drew = false;
static inline bool lvgl_took_dirty(void) { bool d = s_lvgl_drew; s_lvgl_drew = false; return d; }

#if CPU_PROFILE_SERIAL
/* Flush-vs-render split: total time the CPU is tied up by the panel push. With the
 * sync path that was the whole (blocking) transfer — measured 38-43% of loop body.
 * With the async path it's only the swap+addr-window+queue cost; the wire time
 * overlaps the next tile's render. Printed with the [loop] line every 10 s. */
static uint32_t s_flush_us = 0, s_flush_calls = 0, s_flush_px = 0;
/* Async-path phase breakdown: where exactly the blocked time inside the flush
 * goes. swap = byte-order pass; addr = address-window commands (CS_LOW here also
 * DRAINS any still-flying previous tile, so a fat addr number = pipeline stall =
 * wire longer than render); queue = handing segments to the DMA driver. */
static uint32_t s_fl_swap_us = 0, s_fl_addr_us = 0, s_fl_queue_us = 0;
#endif

/* ASYNC FLUSH (set in setup): true when both partial buffers are DMA-capable
 * internal SRAM. False on the PSRAM fallback (SPI DMA can't read PSRAM) — then
 * the legacy blocking draw16bitRGBBitmap path runs instead. */
static bool s_flush_async = false;

/* A/B knob: 1 = LVGL renders in panel byte order (no swap pass in the flush, but
 * every blend runs through the RGB565_SWAPPED loops). 0 = plain RGB565 render
 * (the better-trodden blend loops) + a ~2.5 ms/tile swap pass in the flush.
 * Compare the [draw] label/fill times between builds to measure the swapped-
 * blend tax — decides the endgame, since the PIE SIMD asm targets plain RGB565. */
/* A/B RESULT (2026-06-11): per-label cost IDENTICAL in both formats (~1.0 ms) —
 * no swapped-blend tax. 1 is strictly better (saves the ~2.5 ms/tile swap). */
#define FLUSH_RENDER_SWAPPED 1

/* Glyph-buffer allocator for LVGL's FONT draw-buf handlers (hooked in setup —
 * see the comment there). lv_draw_buf_handlers_t's fields live in a private
 * header; including it is fine, the lib is vendored in this repo. */
#include <draw/lv_draw_buf_private.h>
static void *glyph_buf_malloc(size_t size, lv_color_format_t cf) {
  (void)cf;
  void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p;
}
static void glyph_buf_free(void *p) {
  heap_caps_free(p);
}

/* LOCAL PATCH in lvgl/src/font/fmt_txt/lv_font_fmt_txt.c: decoded-glyph cache —
 * the 4bpp->A8 unpack was 45% of label render time ([draw] g.unpack), re-run
 * 63k times/10s for a few hundred distinct glyphs. Init from setup() while
 * still single-threaded; before init the cache is a transparent no-op. */
extern "C" void lv_font_fmt_txt_glyph_cache_init(void);

/* SPI transfer-done -> hand the buffer back to LVGL. INTERRUPT context: this runs
 * from the SPI master ISR after the tile's last DMA segment. lv_disp_flush_ready
 * only clears the display's flushing flags, which is the canonical ISR use. */
static void flush_done_cb(void *arg) {
  lv_disp_flush_ready((lv_display_t *)arg);
}

static void my_disp_flush(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
#ifndef DIRECT_RENDER_MODE
  uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
#if CPU_PROFILE_SERIAL
  uint32_t f0 = micros();
#endif
  if (s_flush_async) {
    // ASYNC: send the address window (sync commands — these drain any still-in-
    // flight previous tile, which is exactly the pipelining handshake), queue the
    // whole tile as DMA and RETURN. lv_disp_flush_ready fires from the transfer-
    // done ISR, so LVGL renders the next tile while this one is on the wire.
#if FLUSH_RENDER_SWAPPED && LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
    // Display renders LV_COLOR_FORMAT_RGB565_SWAPPED — tile is wire-ready as is.
#else
    // Plain RGB565 render: convert to panel byte order here (~2.5 ms/tile).
    lv_draw_sw_rgb565_swap(px_map, w * h);
#endif
#if CPU_PROFILE_SERIAL
    uint32_t f1 = micros(); s_fl_swap_us += (uint32_t)(f1 - f0);
#endif
    gfx->startWrite();
    co5300->writeAddrWindow(area->x1, area->y1, w, h);
#if CPU_PROFILE_SERIAL
    uint32_t f2 = micros(); s_fl_addr_us += (uint32_t)(f2 - f1);
#endif
    if (qspi_bus->writePixelsBeAsync(px_map, w * h * 2, flush_done_cb, d)) {
#if CPU_PROFILE_SERIAL
      s_fl_queue_us += (uint32_t)(micros() - f2);
#endif
      gfx->endWrite();
      s_lvgl_drew = true;
#if CPU_PROFILE_SERIAL
      s_flush_us += (uint32_t)(micros() - f0);   // CPU-side cost only (swap+queue)
      s_flush_calls++;
      s_flush_px += w * h;
#endif
      return;                  // NO flush_ready here — the DMA-done callback delivers it
    }
    // Tile too big for the async segments (can't happen at current buffer sizes,
    // but stay correct): the buffer is in PANEL byte order and the legacy path
    // byte-swaps during its copy, so convert back to little-endian first.
    lv_draw_sw_rgb565_swap(px_map, w * h);
    gfx->endWrite();
  }
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
#if CPU_PROFILE_SERIAL
  s_flush_us += (uint32_t)(micros() - f0);
  s_flush_calls++;
  s_flush_px += w * h;
#endif
#endif
  s_lvgl_drew = true;        // LVGL re-rendered -> the loop will push the panel once
  lv_disp_flush_ready(d);
}

static void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  // While BOOT is held, report "released" and ignore touch entirely so a stray
  // touch can't interfere with the button press.
  if (boot_suppress_touch) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  // FAST PATH — the 24/7 idle case. LVGL polls this at LV_DEF_REFR_PERIOD (8 ms,
  // ~125 Hz) and the full read below is THREE I2C transactions (~0.5 ms blocked on
  // the shared bus) — that was a constant ~6-10% of core 1 with nobody touching
  // (found via the [cpu]/[loop] profilers). The TP_INT interrupt already tells us
  // when a touch ARRIVES (ISR sets IIC_Interrupt_Flag), so: no edge since the last
  // poll AND no finger down at the last poll -> nobody is touching -> report
  // "released" with ZERO I2C. While a finger IS down we keep polling at the full
  // rate (monitor mode keeps the flag false during a hold — the s_tp_was_down
  // latch covers that), so drag/scroll tracking is completely unaffected.
  static bool s_tp_was_down = false;
  if (!FT3168->IIC_Interrupt_Flag && !s_tp_was_down) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  i2c_lock();   // shared bus: don't interleave with the net task's RTC ops (core 0)
  int32_t fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  i2c_unlock();

  // Two SEPARATE questions, deliberately not conflated:
  //
  //  (1) "Did the user just interact?" -> for the idle timer / wake only. The edge
  //      interrupt (touch-down) OR a finger currently down both count. We can't rely
  //      on the flag alone here because it's edge-triggered and, in the FT3168's
  //      low-power monitor mode, stays false while a finger is HELD.
  //
  //  (2) "Is a finger physically down RIGHT NOW, and where?" -> for LVGL's press.
  //      ONLY the finger-count register (TOUCH_FINGER_NUMBER > 0) answers this. The
  //      X/Y coordinate registers are NOT cleared on finger-up — they latch the LAST
  //      touch's position forever. So if we report a PRESS based on the interrupt
  //      flag while fingers == 0, we hand LVGL the STALE coordinates of the PREVIOUS
  //      touch -> LVGL fires the previously-pressed button instead of where you
  //      actually tapped. (The bug: "tap A, wait, tap B -> A fires again.") We must
  //      therefore gate the reported press POSITION on a live finger, never the flag.
  bool activity   = FT3168->IIC_Interrupt_Flag || (fingers > 0);
  bool finger_down = (fingers > 0);                 // live coords guaranteed iff true
  FT3168->IIC_Interrupt_Flag = false;               // consume the edge regardless

  if (activity) {
    lastActivityMs = millis();  // ANY touch (even a flag-only edge) keeps the screen awake
    s_touch_activity = true;    // also feed the loop's idle-timer refresh (held touch)
  }

  if (finger_down) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;          // x/y are live ONLY because a finger is currently down
    data->point.y = y;
    notif_dismiss();            // a tap clears any notification card
  } else {
    data->state = LV_INDEV_STATE_REL;   // no live finger -> released (never a stale press)
  }
  s_tp_was_down = finger_down;  // keep polling I2C until the release is seen (fast path gate)
}

/* CO5300 requires draw areas aligned to even pixel boundaries. (An LVGL display
 * callback — kept here with my_disp_flush / my_touchpad_read above.) */
static void rounder_event_cb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  area->x1 = (area->x1 >> 1) << 1;
  area->y1 = (area->y1 >> 1) << 1;
  area->x2 = ((area->x2 >> 1) << 1) + 1;
  area->y2 = ((area->y2 >> 1) << 1) + 1;
}

/* Deep sleep + the AXP2101 peripheral-rail safety probe + the light background-
 * check path all live in sleep_power.h now (rail_ and rails_ helpers, the SD-rail
 * diagnostic, enter_deep_sleep / arm_wakes_and_sleep / rearm_and_deep_sleep,
 * pending_notif, background_check_has_new). Included here, after notif_net.h
 * (background_check_has_new calls notif_fetch_raw) and the .ino's hardware
 * objects + config macros; setup()/loop() below drive it. The rail_* accessors
 * are forward-declared earlier for app_power.h; this defines them. */
#include "sleep_power.h"

/* ================================ setup ================================== */
void setup() {
  USBSerial.begin(115200);
  USBSerial.println(DEVICE_NAME " v" DEVICE_VERSION " boot");

  // Release the RTC-GPIO hold we placed on GPIO0 before sleeping, so BOOT works
  // as a normal input again for the menu/sleep press handling below.
  rtc_gpio_hold_dis((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_deinit((gpio_num_t)BOOT_BTN_GPIO);

  // Settings (interval, checks-enabled) are needed even on the light path.
  settings_load();
  timer_load();            // countdown state (so a TIMER wake can ring it)
  haptics_init();          // vibration motor pin (cheap; ready on every path)
  audio_alarm_init();      // park the codec/amp rail OFF (ready if a TIMER wake rings)
  // Saved WiFi networks — needed before any wifi_connect(), including the light
  // background-check path, so it can connect on a timer wake too.
  wifi_nets_load();
  // Battery-health running average (NVS). Loaded on all paths so the background
  // wake's drain_update() can keep learning across deep-sleep reboots.
  health_load();
  calib_load();   // learned sleep-floor current (NVS); same all-paths rationale

  // Core-undervolt safety net: capture a "golden" compute/RAM self-test result
  // NOW, while the core is still at the trusted IDF-stock voltage. Then apply the
  // saved CPU speed (which also applies the core undervolt, if enabled) and
  // re-run the self-test. If the lowered voltage made the core miscompute, revert
  // to the stock 1.25V so a too-aggressive table can never brick a boot.
#if UNDERVOLT_CORE_ENABLE
  core_selftest_capture_golden();
#endif
  settings_apply_cpu_mhz(s_cpu_mhz);   // apply saved CPU speed early (affects all paths)
#if UNDERVOLT_CORE_ENABLE
  if (!core_selftest_ok()) {
    core_set_dig_dbias(CORE_DBIAS_STOCK);   // back off to a known-good voltage
    s_core_unstable = true;
    USBSerial.printf("[uv] core self-test FAILED @ %u MHz -> reverted to 1250 mV\n", s_cpu_mhz);
  } else {
    USBSerial.printf("[uv] core undervolt OK: ~%u mV @ %u MHz\n",
                     core_dbias_to_mv(core_get_dig_dbias()), s_cpu_mhz);
  }
#endif
  clocks_dump("boot");   // baseline clock state (read-only) — our overclock reference

  // EXPERIMENTAL overclock is now BUTTON-triggered (Settings > Power), never at
  // boot — so the watch always boots stock with working USB and stays flashable
  // (no ROM download mode). This only checks/reports a prior hung attempt.
  overclock_check_recovery();

  // Load the stored notification list (needed on the light path too, so the
  // background check appends to the existing history rather than starting fresh).
  notif_store_load();
  na_seed_total();   // count the SD archive once so the bell badge is right from boot

  // Bring up I2C + RTC first (cheap, no display) so the background check can use
  // them. Then branch on WHY the ESP32 woke:
  //   TIMER wake          -> scheduled background notification check (light path)
  //   PWR press / power-on -> full interactive UI
  Wire.begin(IIC_SDA, IIC_SCL);
  audio_alarm_quiesce_codec();  // ES8311 is on the always-on rail and survives every
                                // reboot — force it to standby in case a crash mid-ring
                                // (or older firmware) left its analog blocks powered.

  // A previous deep sleep may have cut proven-safe peripheral rails. Bring the PMU
  // up and restore them NOW — before the RTC read and gfx->begin() below — so the
  // screen/touch are powered on this wake. (SAFE rails always keep I2C alive, so
  // this gets through.) The full PMU config still runs later as usual.
  rails_load_state();
  if (power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    pmu_ok = true;
    rails_restore();
    // NOTE: the SD card draws from a peripheral rail that rails_restore() may have
    // just re-enabled, and the first sd_mount() (in wifi_nets_load above) ran before
    // power was up, so it may have failed on an unpowered card. We do NOT block here
    // waiting for the rail to settle — instead the loop retries the mount in the
    // background as the watch runs (see sd_retry_tick), so boot stays snappy and a
    // present-but-slow-to-power card still gets picked up.
#if SD_RAIL_DIAG
    sd_rail_diag();   // one-shot: identify which rail powers the SD (then set back to 0)
#endif
  }

  if (!rtc.begin(Wire, IIC_SDA, IIC_SCL)) {
    USBSerial.println("PCF85063 not found - check wiring!");
    while (1) delay(1000);
  }

  // Bump the shared I2C bus (touch + PMU + RTC + IMU) to 400 kHz Fast-mode. This board
  // is designed for it (per Waveshare's docs) and all four devices spec Fast-mode, so
  // each I2C transaction is ~4x quicker — most noticeable as crisper touch reads. Set
  // AFTER the PMU/RTC begin() calls above, since those can reconfigure the bus clock
  // back to the 100 kHz default during their own init. (Re-applied on the background-
  // wake path too, where power.begin() runs again — see background_check_has_new.)
  Wire.setClock(400000);

  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  // Remember a BOOT wake so the loop swallows that same still-held press.
  s_woke_from_boot = (wake == ESP_SLEEP_WAKEUP_EXT0);
  if (wake == ESP_SLEEP_WAKEUP_TIMER && (timer_is_due() || almclk_is_due())) {
    // The countdown elapsed / the alarm clock's time arrived while asleep: do NOT
    // go back to sleep — fall through to the full UI boot so the loop rings it.
    USBSerial.println("[wake] timer/alarm-clock due -> full boot");
  } else if (s_checks_enabled && wake == ESP_SLEEP_WAKEUP_TIMER) {
    USBSerial.println("[wake] timer -> background check");
    background_check_has_new();           // returns (new notif) OR sleeps again
    USBSerial.println("[wake] new notification -> full boot");
  } else if (s_checks_enabled) {
    // Interactive wake (BOOT press or cold power-on): the timer path didn't run,
    // so do one immediate fetch once the UI is up rather than waiting 30s.
    s_check_on_wake = true;
  }

  // Full boot from here on. Load the alarm-clock list (alarms.csv on SD/FFat) and
  // recompute its targets — AFTER the wake branch, so a background-check wake that
  // goes straight back to sleep never mounts storage for it (the wake test above
  // only needs the NVS-mirrored earliest target loaded by timer_load()).
  almclk_load();

  // Drive the display QSPI bus at 80 MHz instead of the library default 40 MHz.
  // The full-frame push is ~2/3 of each animation frame; doubling the bus clock
  // roughly halves it. If the panel ever shows tearing/garbage, drop to 60000000
  // (or back to leaving begin() empty = 40 MHz).
  if (!gfx->begin(80000000)) USBSerial.println("gfx->begin() failed!");
  gfx->fillScreen(RGB565_BLACK);

  // Apply saved brightness before anything is shown.
  settings_apply_brightness(s_brightness);

  // On-screen reset banner — shown ONLY for an UNEXPECTED reset (a crash: PANIC/
  // WDT/BROWNOUT, or a deep-sleep that didn't go through our intended sleep path).
  // Normal boots (clean idle/BOOT sleep, power-on, etc.) show nothing. Painted
  // with raw GFX (LVGL isn't up yet) so it can't itself fail.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char *txt = "?";
    switch (rr) {
      case ESP_RST_POWERON:   txt = "POWERON";   break;
      case ESP_RST_SW:        txt = "SW";        break;
      case ESP_RST_PANIC:     txt = "PANIC";     break;
      case ESP_RST_INT_WDT:   txt = "INT_WDT";   break;
      case ESP_RST_TASK_WDT:  txt = "TASK_WDT";  break;
      case ESP_RST_WDT:       txt = "WDT";       break;
      case ESP_RST_BROWNOUT:  txt = "BROWNOUT";  break;
      case ESP_RST_DEEPSLEEP: txt = "DEEPSLEEP"; break;
      case ESP_RST_EXT:       txt = "EXT";       break;
      default:                txt = "OTHER";     break;
    }
    // Was the last shutdown a CLEAN, intended sleep? Only the idle/BOOT sleep
    // paths set the magic. A DEEPSLEEP reset WITHOUT the magic, or any PANIC/WDT/
    // BROWNOUT, means an UNEXPECTED reset — even if the screen had stayed dark
    // with no serial. Read then clear it so the next boot reflects the next event.
    bool clean_sleep = (rtc_sleep_intent == SLEEP_INTENT_MAGIC);
    rtc_sleep_intent = 0;
    bool unexpected = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                       rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                       rr == ESP_RST_BROWNOUT ||
                       (rr == ESP_RST_DEEPSLEEP && !clean_sleep));

    USBSerial.printf("[reset] reason=%d (%s) clean_sleep=%d\n",
                     (int)rr, txt, (int)clean_sleep, (int)unexpected);
    // Only paint for an unexpected reset (a crash) — every normal boot is silent.
    if (unexpected) {
      gfx->setTextColor(RGB565(255, 60, 60));
      gfx->setTextSize(3);
      gfx->setCursor(40, 200);
      gfx->print("RST: ");
      gfx->print(txt);
      gfx->setCursor(40, 240);
      gfx->print("UNEXPECTED!");
      delay(3000);                  // hold the crash banner long enough to read
      gfx->fillScreen(RGB565_BLACK);
    }
  }

  // The FT3168 sits in low-power MONITOR mode (set below) and hibernates after
  // ~10 s; a longer deep sleep can leave it deep enough that the first I2C probe
  // NAKs. A quick wake finds it awake and inits on the first try (stays instant).
  // If the first try fails, hardware-reset it via TP_RESET to bring it back, then
  // retry FAST — the old loop waited 2 s per attempt, so 1-3 misses made the watch
  // face take 2-6 s to appear (and spammed the log).
  if (!FT3168->begin()) {
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);  delay(10);    // assert reset
    digitalWrite(TP_RESET, HIGH); delay(200);   // release + let the controller boot
    int tries = 0;
    while (!FT3168->begin()) {
      if (++tries >= 12) { USBSerial.println("FT3168 init fail after reset"); break; }
      delay(30);                                // ~30 ms apart, not 2 s
    }
  }
  FT3168->IIC_Write_Device_State(FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
                                 FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
  // (RTC already initialized near the top of setup for the alarm-flag check.)

  // PMU (AXP2101) on the same I2C bus. Enables the battery fuel gauge so
  // getBatteryPercent()/isCharging() return real values. Non-fatal if absent.
  pmu_ok = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmu_ok) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.enableSystemVoltageMeasure();
    power.enableVbusVoltageMeasure();

    // Power-key behaviour:
    //  - ONLEVEL: how long PWR must be held to power the system ON.
    //    512ms (1) is a quick, deliberate press and is known-good. If you want
    //    to experiment with 128ms (0) it MAY be too short for the PMU debounce;
    //    revert to 1 if power-on becomes unreliable.
    power.setOnLevel(1);  // 0:128ms 1:512ms 2:1s 3:2s
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    // EXPERIMENTAL undervolt: now that DCDC1 is reachable, set the rail voltage
    // paired with the saved CPU speed (no-op unless UNDERVOLT_ENABLE is set in
    // settings_store.h). Interactive path only — the background WiFi-check path
    // deliberately leaves the rail at the stock 3.3V to survive TX current spikes.
    settings_apply_rail_for_mhz(s_cpu_mhz);
  } else {
    USBSerial.println("AXP2101 not found - battery indicator disabled");
  }

// ---- Set the clock ONCE, never on every boot ----
// The PCF85063 is battery-backed: it keeps perfect time across reboots, deep
// sleep, and PMU power-off. So we must NOT overwrite it on every boot (that's
// why pressing PWR/reset made the time look "stuck" — setup() re-stamped it).
//
// To set the time: put the current time in TIME_SET_* below, set
// FORCE_TIME_SET to 1, flash ONCE, then set it back to 0 and flash again.
// (Later, NTP or BLE will set the time automatically and this goes away.)
#define FORCE_TIME_SET 0
#define TIME_SET_Y 2026
#define TIME_SET_MO 6
#define TIME_SET_D 4
#define TIME_SET_H 15
#define TIME_SET_MI 28
#define TIME_SET_S 0
  {
    RTC_DateTime cur = rtc.getDateTime();
    bool rtc_unset = (cur.getYear() < 2024);  // never set / lost backup
    if (FORCE_TIME_SET || rtc_unset) {
      rtc.setDateTime(TIME_SET_Y, TIME_SET_MO, TIME_SET_D,
                      TIME_SET_H, TIME_SET_MI, TIME_SET_S);
      USBSerial.println(FORCE_TIME_SET ? "[time] forced set" : "[time] RTC was unset, initialized");
    } else {
      USBSerial.println("[time] kept from battery-backed RTC");
    }
  }

  lv_init();
  lv_tick_set_cb(millis_cb);

  // --- Glyph draw-bufs -> internal SRAM -------------------------------------
  // LVGL unpacks every glyph's 4bpp bitmap into an A8 "font draw buf" and the
  // blender reads it back. Those buffers come from the FONT draw-buf handlers,
  // whose default malloc is lv_malloc = our PSRAM allocator — so every glyph on
  // screen was WRITTEN to and READ back from PSRAM, every frame. The [draw]
  // profiler showed labels at ~80% of render time (~1 ms/label) because of it.
  // The buffers are tiny (box_w x ~32 rows of A8 = a few KB, transient), so give
  // the font handlers a dedicated SRAM alloc with PSRAM fallback. Must run before
  // any text renders; the workers allocate via these cbs (heap_caps is task-safe).
  {
    lv_draw_buf_handlers_t *fh = lv_draw_buf_get_font_handlers();
    fh->buf_malloc_cb = glyph_buf_malloc;
    fh->buf_free_cb   = glyph_buf_free;
  }
  lv_font_fmt_txt_glyph_cache_init();   // decoded-glyph cache (see decl above)
#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print);
#endif

  screenWidth = gfx->width();
  screenHeight = gfx->height();
#ifdef DIRECT_RENDER_MODE
  bufSize = screenWidth * screenHeight;
#else
  bufSize = screenWidth * PARTIAL_BUF_LINES;   // partial: small line-bufs (x2), fit in SRAM
#endif

  // Full-frame buffer in PSRAM (DIRECT mode -> ~screen*2 bytes, ~410 KB). This is
  // SAFE in PSRAM because the framebuffer is NEVER a DMA source: LVGL's software
  // renderer fills it with the CPU, and our Arduino_ESP32QSPI::writePixels reads it
  // pixel-by-pixel with the CPU and reformats into the small INTERNAL DMA buffers
  // that actually feed the QSPI engine. So nothing DMAs straight from here. Moving
  // it off internal SRAM frees ~410 KB of the scarce fast RAM. Cost: the CPU's
  // per-frame fill/read is slower from PSRAM — but access is sequential (PSRAM
  // burst-friendly) and our render-gating only pushes on real changes, so it's a
  // good trade for the SRAM. Fall back to INTERNAL only if PSRAM is unavailable.
#ifdef DIRECT_RENDER_MODE
  // Full-frame buffer in PSRAM (~410KB). Only used in the legacy direct path.
  disp_draw_buf = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!disp_draw_buf) disp_draw_buf = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  disp_draw_buf2 = NULL;
#else
  // PARTIAL: two line-buffers in INTERNAL SRAM. SRAM (not PSRAM) so the CPU read
  // that feeds QSPI in writePixels is fast; two of them so LVGL renders the next tile
  // while the previous flushes. Fixed size — see the PARTIAL_BUF_LINES comment for
  // why this must NOT be sized from free-heap at boot. Fall back to PSRAM only if
  // internal SRAM can't satisfy it (slower, but still partial-area pushes).
  // MALLOC_CAP_DMA: the async flush queues SPI DMA straight from these buffers
  // (no bounce copy), so they must come from DMA-capable internal SRAM.
  disp_draw_buf  = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!disp_draw_buf || !disp_draw_buf2) {
    USBSerial.println("[gfx] partial bufs didn't fit in SRAM — falling back to PSRAM");
    if (disp_draw_buf)  { heap_caps_free(disp_draw_buf);  disp_draw_buf  = NULL; }
    if (disp_draw_buf2) { heap_caps_free(disp_draw_buf2); disp_draw_buf2 = NULL; }
    disp_draw_buf  = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
#endif
  if (!disp_draw_buf) {
    USBSerial.println("draw buf alloc failed!");
    return;
  }
#ifndef DIRECT_RENDER_MODE
  // Confirm where the partial buffers actually landed (SRAM = fast path, the FPS win;
  // PSRAM = fallback, slower) and how much internal SRAM is left for BLE/WiFi/stacks.
  {
    bool b1_sram = esp_ptr_internal(disp_draw_buf);
    bool b2_sram = disp_draw_buf2 ? esp_ptr_internal(disp_draw_buf2) : true;
    USBSerial.printf("[gfx] partial bufs: %u lines, %u KB x2 in %s; internal SRAM now %u KB free\n",
                     (unsigned)(bufSize / screenWidth),
                     (unsigned)((bufSize * 2) / 1024),
                     (b1_sram && b2_sram) ? "SRAM (fast)" : "PSRAM (SLOW fallback)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    // Async flush needs SPI-DMA-readable buffers; PSRAM fallback can't, so it
    // keeps the legacy blocking flush.
    s_flush_async = disp_draw_buf2 &&
                    esp_ptr_dma_capable(disp_draw_buf) &&
                    esp_ptr_dma_capable(disp_draw_buf2);
    USBSerial.printf("[gfx] flush path: %s\n",
                     s_flush_async ? "ASYNC (DMA tile overlaps next render)" : "sync (blocking)");
  }
#endif

  disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
#ifdef DIRECT_RENDER_MODE
  lv_display_set_buffers(disp, disp_draw_buf, NULL, bufSize * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
#else
  lv_display_set_buffers(disp, disp_draw_buf, disp_draw_buf2, bufSize * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
#if FLUSH_RENDER_SWAPPED && LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
  if (s_flush_async) {
    // Render directly in PANEL byte order (big-endian RGB565). The [flush] profiler
    // showed the post-render swap pass eating ~2.5 ms/tile (93% of the flush cost);
    // with this format the swap happens inside the blend loops while pixels are
    // already in registers — effectively free — and my_disp_flush queues the tile
    // exactly as rendered. screen_cache snapshots pin LV_COLOR_FORMAT_RGB565 and
    // blit via the legacy converting path, so they are unaffected. Same 16 bpp, so
    // buffer sizing is unchanged.
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  }
#endif  /* FLUSH_RENDER_SWAPPED */
#endif

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);
  lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

  watchface_create();
  notif_card_create();
  app_menu_init();                        // build the (hidden) app menu overlay
  quick_shade_init();                     // build the pull-down brightness shade (sys layer)
  pinMode(BOOT_BTN_GPIO, INPUT_PULLUP);   // BOOT button as input

  // Draw the current time + battery once immediately so the face isn't blank
  // until the first minute rollover.
  watchface_update(rtc.getDateTime());
  refresh_battery();
  watchface_refresh_bell();
  watchface_set_wifi(WiFi.status() == WL_CONNECTED);

  // If we woke for a notification (background check found one), show it now that
  // the UI exists. The fetch (during the light check) stashed the newest in
  // s_pop_* regardless of whether it went to flash or the SD archive.
  if (pending_notif && s_pop_have) {
    notif_show(s_pop_id, s_pop_title, s_pop_body);
    pending_notif = false;
  }

  lv_task_handler();   // partial mode: my_disp_flush pushes the dirty area(s) itself
#ifdef DIRECT_RENDER_MODE
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)disp_draw_buf, screenWidth, screenHeight);
#endif

  // One-time per-rail safety probe: classify which AXP2101 peripheral rails are
  // safe to cut in deep sleep. Runs here while I2C is still SINGLE-THREADED (before
  // the net task below), and only does work while rails remain unclassified —
  // normal boots skip straight through. Interactive boots only (the background
  // path sleeps before reaching here).
  rails_probe();

  // Spin up the async networking: a mutex guarding the shared notif store, and a
  // task pinned to CORE 0 so WiFi/HTTP never block the UI loop (core 1).
  s_store_mutex = xSemaphoreCreateMutex();
  // Guard the shared I2C bus across the two cores. Must exist BEFORE the net task
  // starts (it does RTC I2C ops on core 0). All setup() I2C above ran single-
  // threaded, so it needed no lock; from here on every access takes i2c_lock().
  s_i2c_mutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(net_task_fn, "net", 8192, nullptr, 1, &s_net_task, 0);

  ble_apply_enabled();        // bring BLE up if its toggle was left on (lazy; off in sleep)

  cpu_usage_init();           // per-core idle hooks for the Power app's CPU graph

  lastActivityMs = millis();  // start the idle timer
  USBSerial.println("Setup done");
}

/* Push the current framebuffer to the panel (direct-render mode). */
static inline void watchface_present(void) {
#ifdef DIRECT_RENDER_MODE
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)disp_draw_buf, screenWidth, screenHeight);
#endif
}

/* ================================ loop =================================== */
static int lastMinuteShown = -1;  // -1 = nothing drawn yet
static uint32_t lastBattMs = 0;   // battery poll timer
static uint32_t lastNotifMs = 0;  // notification fetch timer

void loop() {
  uint32_t ms = millis();
  bool dirty = false;  // did anything visible change?
#if CPU_PROFILE_SERIAL
  uint32_t lp_t0 = micros();   // loop micro-profile: body-time capture (see loop tail)
#endif

  // NOTE: BOOT is polled FIRST, before lv_task_handler(), so heavy touch/render
  // work in LVGL can't delay or starve the button check.

  // --- BOOT button: wake + menu + sleep (single tap = menu, double-tap = sleep) ---
  // BOOT (GPIO0): HIGH = released, LOW = pressed. The press that WOKE us from
  // deep sleep is STILL HELD as the loop starts; it's fully ignored until
  // released so it can't trip anything (no wake/sleep loop).
  //   * Single tap  -> toggle the menu (acts on the press edge, can't be missed).
  //   * Double tap   (2 presses within BOOT_DTAP_MS) -> deep sleep.
  //   * While BOOT is down, touch is suppressed so a stray touch can't interfere.
  #define BOOT_DTAP_MS 450
  static bool     bootDown     = false;  // currently pressed (debounced)?
  static uint32_t bootLastTap  = 0;      // ms of the previous tap's press edge
  bool bootRaw = (digitalRead(BOOT_BTN_GPIO) == LOW);

  if (s_woke_from_boot) {
    // Ignore the still-held wake press entirely; arm only once it's released.
    boot_suppress_touch = bootRaw;
    if (!bootRaw) s_woke_from_boot = false;
    bootDown = bootRaw;                   // track so the release isn't seen as a tap
  } else if (bootRaw && !bootDown) {
    // New press edge: confirm with a tiny inline debounce.
    delayMicroseconds(1500);
    if (digitalRead(BOOT_BTN_GPIO) == LOW) {
      bootDown = true;
      boot_suppress_touch = true;
      lastActivityMs = ms;
      if (bootLastTap != 0 && (ms - bootLastTap) <= BOOT_DTAP_MS) {
        // Second tap within the window -> double-tap -> sleep.
        bootLastTap = 0;
        enter_deep_sleep();               // does not return
      } else {
        // First tap -> remember the time for a possible 2nd. If the shade is
        // open, BOOT just closes it; otherwise it does the usual one-level back.
        bootLastTap = ms;
        if (g_alarm_active)        alarm_dismiss();   // ringing -> BOOT silences it
        else if (quick_shade_is_open()) quick_shade_close();
        else                       app_menu_back();
        dirty = true;
      }
    }
  } else if (!bootRaw && bootDown) {
    // Released: clear pressed state, re-enable touch.
    bootDown = false;
    boot_suppress_touch = false;
  }

  // Service LVGL (timers, input) every loop — AFTER the BOOT check so the button
  // is always polled first and never delayed by render/touch work.
  lv_task_handler();
  ble_ui_tick();   // render/hide the BLE pair-code overlay (LVGL-thread side of BLE)

  // Find My Watch: the phone wrote the find-watch characteristic -> ring the watch
  // using the timer's alarm overlay, with its own text. Checked every loop so it's
  // responsive; ignored if an alarm is already up.
  if (ble_take_find_watch_req() && !g_alarm_active)
    alarm_fire_ex("Find My Watch", "Your phone is looking for me", false);

  // Drive the vibration motor's pattern state machine every loop (just GPIO, no
  // I2C) so the alarm's heartbeat buzz stays crisp.
  haptics_tick(ms);
  audio_alarm_tick();    // finish a notification ding's teardown once it's done sounding

  // Background-mount the SD card: the early-boot attempts ran before the SD rail was
  // powered, so this retries (rate-limited) until the card mounts or is ruled absent.
  sd_retry_tick(ms);

  // Reflect the caffeine (keep-awake) state in the watchface corner mug, painting
  // only when it actually changes.
  static bool s_caf_shown = false;
  if (s_caffeine != s_caf_shown) {
    s_caf_shown = s_caffeine;
    watchface_set_caffeine(s_caffeine);
    dirty = true;
  }

  // Reflect BLE connection state in the watchface corner glyph, painting only on
  // change (edge-detected like the caffeine mug above).
  static bool s_ble_shown = false;
  bool ble_now = ble_phone_connected();
  if (ble_now != s_ble_shown) {
    s_ble_shown = ble_now;
    watchface_set_ble(ble_now);
    dirty = true;
  }

  // --- Time: check each second, redraw only when the MINUTE changes ---
  if (ms - lastMillis > 1000) {
    lastMillis = ms;
    i2c_lock();   // shared bus
    RTC_DateTime now = rtc.getDateTime();
    i2c_unlock();
    if (now.getMinute() != lastMinuteShown) {
      lastMinuteShown = now.getMinute();
      watchface_update(now);
      dirty = true;
    }
    // Countdown reached zero while we're awake -> ring. (timer_is_due() reads the
    // RTC with its own i2c_lock, so it must run OUTSIDE the lock taken above.)
    if (!g_alarm_active && timer_is_due())  alarm_fire();
    // Wall-clock alarm reached its set time -> ring (same overlay/chime).
    if (!g_alarm_active && almclk_is_due()) almclk_fire();
  }

  // --- Battery + bell: poll every 20s. (WiFi indicator is refreshed on the 15s
  //     notification cadence below, in phase with the fetch that actually drives
  //     the radio's connect/disconnect — see there.) ---
  if (ms - lastBattMs > 20000) {
    lastBattMs = ms;
    if (refresh_battery()) dirty = true;
    watchface_refresh_bell();   // unread count from the active store (SD/FAT or flash)
    dirty = true;
  }

  // --- Notifications: ask the CORE-0 network task to poll the server every 15s
  //     (NOTIFY_FETCH_MS) and once immediately on wake. This is NON-BLOCKING — the
  //     UI/buttons stay fully responsive while WiFi connects on the other core. ---
  // Refresh the watchface WiFi indicator on the SAME 15s cadence as the fetch, so
  // it's in phase with the network task that actually connects/disconnects the radio.
  // (It used to ride the 20s battery poll, which drifted out of phase with the 15s
  // fetch — toggling WiFi off/on in settings could leave the icon stuck on the old
  // state until you happened to catch a poll: the "doesn't update until I revisit the
  // menu" bug.) Change-detected, so it repaints only when the state actually flips.
  // s_wifi_shown: -1 = never drawn, 0 = off, 1 = connected.
  static int s_wifi_shown = -1;
  if (s_check_on_wake || ms - lastNotifMs > NOTIFY_FETCH_MS) {
    s_check_on_wake = false;
    lastNotifMs = ms;
    notif_request_fetch();              // returns instantly; task does the work

    int wifi_now = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    if (wifi_now != s_wifi_shown) {
      s_wifi_shown = wifi_now;
      watchface_set_wifi(wifi_now == 1);
      dirty = true;
    }
  }

  // Consume a finished fetch result (set by the network task). All LVGL happens
  // HERE on the loop/core-1 side — the task never touches the UI.
  if (s_net_result_ready) {
    s_net_result_ready = false;
    if (s_net_new_count > 0) {
      // Pop the newest notification as a card (unless a menu is open). The fetch
      // stashed it in s_pop_* regardless of which store it went to.
      store_lock();
      bool have = s_pop_have;
      uint64_t id = have ? s_pop_id : 0;
      static char ct[NOTIF_TITLE_MAX], cb[sizeof(s_pop_body)];
      if (have) { strcpy(ct, s_pop_title); strcpy(cb, s_pop_body); }
      store_unlock();
      if (have && !app_menu_is_open()) {
        notif_show(id, ct, cb);
        lastActivityMs = ms;             // keep screen awake to read it
      }
      watchface_refresh_bell();           // refresh unread count either way
      dirty = true;
    }
  }

  // ANCS (iPhone-over-BLE) pushed a notification into the store from the NimBLE task.
  // Pop its card + refresh the bell here on the loop/LVGL side, mirroring the fetch
  // result path above. s_pop_* was filled under store_lock by the ANCS parser.
  if (ancs_take_ui_dirty()) {
    store_lock();
    bool have = s_pop_have;
    uint64_t id = have ? s_pop_id : 0;
    static char act[NOTIF_TITLE_MAX], acb[sizeof(s_pop_body)];
    if (have) { strcpy(act, s_pop_title); strcpy(acb, s_pop_body); }
    store_unlock();
    if (have && !app_menu_is_open()) {
      notif_show(id, act, acb);
      lastActivityMs = ms;                // wake the screen so it can be read
    }
    watchface_refresh_bell();
    dirty = true;
  }

  // ANCS removed a notification (the user cleared it on the iPhone). Refresh the
  // bell, and if the Notifications app is the open screen, rebuild its list so the
  // dismissed item disappears there too — the watch mirrors the phone.
  if (ancs_take_removed()) {
    watchface_refresh_bell();
    if (nav_current == app_open_notifications)
      lv_async_call(notif_rebuild_async, nullptr);   // safe deferred rebuild
    dirty = true;
  }

  // A card dismiss (from the touch callback) needs a repaint to actually clear
  // the card off the panel.
  if (notif_needs_repaint) { notif_needs_repaint = false; dirty = true; }

  // Re-render + push the panel ONLY when something actually changed on screen.
  // lvgl_took_dirty() is the general signal: lv_task_handler() above re-rendered
  // (and flushed) iff LVGL had dirty areas — true for ANY screen during a redraw,
  // touch feedback, scroll inertia, animation, or live-label tick, and false once
  // everything settles. This replaces the old "always push while the menu/shade/
  // alarm/BLE overlay is open" terms, so every screen now idles the QSPI bus when
  // static — the clock face's "freeze when nothing changes", generalized.
  //   - `dirty` stays as a belt-and-suspenders for our hand-tracked changes.
  //   - the *_take_dirty() reads stay so those modules' edge flags don't get stuck;
  //     they're effectively subsumed by lvgl_took_dirty() but harmless to keep.
  // Evaluate every read-and-clear flag FIRST (no short-circuit), so none is left
  // un-consumed and stuck when an earlier term is already true.
  // PARTIAL mode: my_disp_flush() already pushed each dirty rectangle to the panel
  // during lv_task_handler() above, so watchface_present() is a no-op (guarded by
  // DIRECT_RENDER_MODE) and this block does nothing visible. The take_dirty() reads
  // are kept so those edge flags still get consumed (and the DIRECT path still works
  // if ever re-enabled). The per-area push IS the FPS win: a small change sends a
  // small region instead of the whole 410KB screen every frame.
  bool drew  = lvgl_took_dirty();
  bool shade = quick_shade_take_dirty();
  bool alrm  = alarm_take_dirty();
  bool bled  = ble_take_dirty();
  if (drew || dirty || shade || alrm || bled) {
    watchface_present();   // DIRECT only; no-op in PARTIAL (flush already pushed the areas)
  }

  // PSRAM pre-render cache: once the launcher grid has actually rendered this open
  // (drew == true while the menu is up and not yet cached), snapshot it into PSRAM so
  // the NEXT open blits instantly. Done here — after the render/flush — so the capture
  // (which re-rasterizes the object) lands off the visible open frame, and on a frame
  // we know is fully drawn. app_menu_cache_capture() self-gates on cache validity, so
  // this is a no-op every loop except the first post-open (and after a restyle).
  if (drew && app_menu_is_open()) app_menu_cache_capture();

  // --- Sleep triggers ---
  // NOTE: we deliberately do NOT sleep on a short PWR press. PWR only reaches the
  // AXP2101, which CANNOT wake the CPU from deep sleep — so PWR-to-sleep would
  // strand the watch (only a ~7s hardware power-cycle would recover it). Sleep is
  // owned by BOOT (long press) and the idle timeout; BOOT (EXT0) wakes it back.
  // A long PWR hold still does the AXP2101 hardware OFF as a true-off escape.
  //
  // Idle timeout: sleep after no touch for IDLE_SLEEP_MS — but NOT while USB is
  // connected (charging/programming). Reset the timer while on USB so unplugging
  // gives a fresh idle period.
  // A raw touch edge (set by the ISR) is unconditional proof of user activity —
  // refresh the idle timer here, NOT only inside my_touchpad_read(). The indev
  // callback can classify a quick tap as "no touch" (interrupt flag already
  // consumed, finger-count reads 0 in the FT3168's low-power monitor mode), which
  // on battery let the 2-minute timeout fire on the very tap meant to keep it
  // awake. Consuming the ISR flag here makes any tap reliably reset the timer.
  if (s_touch_activity) {
    s_touch_activity = false;
    lastActivityMs = ms;
  }

  // A ringing alarm must never idle-sleep mid-buzz: keep the idle timer fresh so
  // it stays awake until dismissed (BOOT or touch).
  if (g_alarm_active) lastActivityMs = ms;

  // VBUS (USB-power) state feeds only the 2-minute idle timer, so polling it twice
  // a second is plenty — doing the I2C read EVERY loop would flood the shared bus
  // now that the loop spins ~1000x/sec.
  static uint32_t lastVbusMs = 0;
  static bool     usb_connected = false;
  if (ms - lastVbusMs > 500) {
    lastVbusMs = ms;
    i2c_lock();   // shared bus
    usb_connected = pmu_ok && power.isVbusIn();
    i2c_unlock();
  }
  // Auto-dim on idle: after DIM_IDLE_MS with no touch/notification the panel dims
  // to save power; any activity (which refreshed lastActivityMs above) restores it.
  // A no-op when the user disabled it in the Power app. settings_dim_set() only
  // touches the panel brightness register (debounced), not the stored brightness.
  //   - CAFFEINE / a ringing ALARM always block dimming (their job is to stay lit).
  //   - USB power blocks dimming UNLESS the user opted into "Dim on USB" (Power app);
  //     even then the watch still never deep-sleeps on USB (see below), only dims —
  //     so a charging watch can stay dark-but-reachable instead of lit all night.
  // The BLE pair-code overlay must always be readable: keep the panel at full
  // brightness while it's up (and brighten immediately if it appeared while dimmed).
  // ble_overlay_active() is true while the pair code box / "Paired!" toast is on
  // screen. We both EXCLUDE it from dim_now (block dimming) and refresh the idle timer
  // (un-dim + don't sleep) so it can't go dark mid-pairing.
  bool ble_overlay = ble_overlay_active();
  if (ble_overlay) lastActivityMs = ms;   // treat the reveal as activity (un-dim, stay awake)

  bool usb_blocks = usb_connected && !settings_get_dim_on_usb();
  bool dim_now = settings_get_autodim() && !usb_blocks && !s_caffeine &&
                 !g_alarm_active && !ble_overlay &&
                 (ms - lastActivityMs > DIM_IDLE_MS);
  settings_dim_set(dim_now);              // panel brightness drop (any screen)

  // Deep-dim "minimal face": on top of the brightness drop, blank everything on
  // the WATCH FACE except the center clock — on AMOLED a black pixel is OFF, so
  // this saves real power beyond the dim. ONLY when the face is actually the
  // visible screen: if a menu/app or the pull-down shade is up, leave the full UI
  // (the dim already lowered its brightness). watchface_set_minimal is debounced +
  // null-safe, so calling it every loop is cheap; a transition marks the screen
  // dirty so the loop's lvgl_took_dirty() push repaints it.
  bool face_visible = !app_menu_is_open() && !quick_shade_is_open();
  bool want_minimal = dim_now && face_visible;
  if (want_minimal != watchface_is_minimal()) {
    watchface_set_minimal(want_minimal);
    dirty = true;                        // show/hide changed the tree -> repaint
  }

  // Deep-sleep gating. USB power or caffeine must never let the watch idle-sleep —
  // but we must NOT do that by pinning lastActivityMs every loop, because that also
  // freezes the idle DIM timer (the "Dim on USB" option could never reach
  // DIM_IDLE_MS). So: keep the idle timer RUNNING on USB (dim still works), and just
  // refuse to actually sleep while plugged in / caffeinated. On the USB->battery
  // unplug edge, reset the timer once so unplugging gives a fresh idle period
  // instead of possibly sleeping immediately if it already elapsed while charging.
  static bool s_was_usb = false;
  if (s_was_usb && !usb_connected) lastActivityMs = ms;   // just unplugged -> fresh period
  s_was_usb = usb_connected;

  if (s_caffeine) {                      // caffeine = stay awake, don't even count toward sleep
    lastActivityMs = ms;
  } else if (!usb_connected && ms - lastActivityMs > IDLE_SLEEP_MS) {
    enter_deep_sleep();  // does not return (never while on USB)
  }

  // Per-task CPU profiler -> serial, every 10 s (measured-runtime libs only; no-op
  // on stock libs). Attribute standing load here; CPU_PROFILE_SERIAL 0 to silence.
  cpu_usage_profile_tick();

  // Adaptive pacing: spin fast (1 ms) only while something drew RECENTLY — scroll
  // inertia, animations and touch feedback mark `dirty` every frame, so motion holds
  // the fast cadence and the 500 ms tail covers between-frame gaps. A menu sitting
  // STILL drops to the same cheap 5 ms poll as the idle clock face (it used to hold
  // 1 ms the whole time a menu was open — a constant ~10% of core 1 for nothing;
  // the per-task profiler caught it). Tap pickup latency is unchanged either way:
  // LVGL reads the touch controller on its own ~30 ms indev timer, and the first
  // touch-feedback frame marks dirty, which restores the 1 ms cadence instantly.
  static uint32_t s_last_motion_ms = 0;
  if (dirty || quick_shade_active() || g_alarm_active) s_last_motion_ms = ms;
  bool fast_pace = (ms - s_last_motion_ms < 500);

#if CPU_PROFILE_SERIAL
  // Loop micro-profile companion to the per-task dump: pass rate, average body
  // time, and how many passes were fast / actually drew. Distinguishes "each pass
  // is too expensive" from "the cadence is stuck fast because something keeps
  // marking dirty". Printed on the same ~10 s rhythm as the task table.
  {
    static uint32_t lp_win0 = 0, lp_passes = 0, lp_fast = 0, lp_drew = 0;
    static uint64_t lp_body_us = 0;
    lp_passes++;
    if (fast_pace) lp_fast++;
    if (dirty)     lp_drew++;
    lp_body_us += (uint32_t)(micros() - lp_t0);
    if (lp_win0 == 0) lp_win0 = ms;
    else if (ms - lp_win0 >= 10000) {
      USBSerial.printf("[loop] %lu passes/10s (%lu fast, %lu drew), avg body %lu us\n",
                       (unsigned long)lp_passes, (unsigned long)lp_fast,
                       (unsigned long)lp_drew, (unsigned long)(lp_body_us / lp_passes));
      if (s_flush_calls) {
        // flush share of body time = how much an async (non-blocking) flush could
        // reclaim for rendering; MB/s = effective panel-link throughput incl. setup.
        uint32_t share = lp_body_us ? (uint32_t)((uint64_t)s_flush_us * 100 / lp_body_us) : 0;
        uint32_t mbps10 = s_flush_us ? (uint32_t)((uint64_t)s_flush_px * 2 * 10 / s_flush_us) : 0;
        USBSerial.printf("[flush] %lu calls, %lu us total (avg %lu us/call), %lu%% of body, %lu.%lu MB/s\n",
                         (unsigned long)s_flush_calls, (unsigned long)s_flush_us,
                         (unsigned long)(s_flush_us / s_flush_calls), (unsigned long)share,
                         (unsigned long)(mbps10 / 10), (unsigned long)(mbps10 % 10));
        USBSerial.printf("[flush]   avg/call: swap %lu us, addr+drain %lu us, queue %lu us\n",
                         (unsigned long)(s_fl_swap_us / s_flush_calls),
                         (unsigned long)(s_fl_addr_us / s_flush_calls),
                         (unsigned long)(s_fl_queue_us / s_flush_calls));
        s_flush_us = s_flush_calls = s_flush_px = 0;
        s_fl_swap_us = s_fl_addr_us = s_fl_queue_us = 0;
      }
      // Per-task-TYPE render time (accumulated inside lv_draw_sw.c's
      // execute_drawing — LOCAL PATCH). Sums BOTH workers, so totals can exceed
      // 10 s/window. This is the "where do the pixels actually cost" table.
      {
        extern uint32_t g_lv_draw_type_us[20], g_lv_draw_type_cnt[20];
        static const char *dt_names[20] = {
          "none", "fill", "border", "shadow", "letter", "label", "image",
          "LAYER", "line", "arc", "tri", "maskrect", "maskbmp", "blur",
          "vector", "3d", "g.look", "g.unpack", "g.blend", "?"
        };
        char db[192]; int dk = 0;
        for (int i = 0; i < 20 && dk < (int)sizeof(db) - 24; i++) {
          if (!g_lv_draw_type_cnt[i]) continue;
          dk += snprintf(db + dk, sizeof(db) - dk, " %s %lums/%lu",
                         dt_names[i],
                         (unsigned long)(g_lv_draw_type_us[i] / 1000),
                         (unsigned long)g_lv_draw_type_cnt[i]);
          g_lv_draw_type_us[i] = g_lv_draw_type_cnt[i] = 0;
        }
        if (dk) USBSerial.printf("[draw]%s\n", db);
      }
      lp_win0 = ms; lp_passes = lp_fast = lp_drew = 0; lp_body_us = 0;
    }
  }
#endif

  delay(fast_pace ? 1 : 5);
}
