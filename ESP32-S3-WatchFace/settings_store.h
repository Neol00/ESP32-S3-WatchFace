/* ============================================================================
 *  settings_store.h — persistent user settings (NVS), get/set/apply + load.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER the hardware objects
 *  (co5300) and watch_base.h, because:
 *    - settings_apply_brightness() drives co5300->setBrightness()
 *    - settings_apply_cpu_mhz()    calls setCpuFrequencyMhz()
 *    - settings_set_wifi_enabled() touches WiFi
 *    - settings_load() reads s_check_interval_min (defined in watch_base.h)
 *  Owns the Preferences handle `prefs` used by the WiFi/notification stores too,
 *  so it must come before them.
 * ========================================================================== */
#pragma once
#include <Preferences.h>

/* Stored in flash via the Preferences library — survives reboots/power-off.
 * The app menu calls settings_get/set_brightness(); this module owns the store
 * and applies brightness to the panel. */
static Preferences prefs;
static uint8_t s_brightness = 204;        // 0-255; ~80%

/* Forward decl: applying a brightness means the panel is at full, so any active
 * auto-dim state is now stale. Defined below with the auto-dim helpers. */
static bool s_dimmed;
static void settings_apply_brightness(uint8_t b) {
  s_brightness = b;
  s_dimmed = false;                       // panel is now at full -> not dimmed
  co5300->setBrightness(b);               // CO5300 normal-mode brightness (0x51)
}

/* Called by the app menu's brightness slider (live preview + persist). */
static uint8_t settings_get_brightness(void) { return s_brightness; }
static void settings_set_brightness(uint8_t b) {
  settings_apply_brightness(b);
  prefs.putUChar("bright", b);            // persist to NVS
}

/* ---- Auto-dim on idle (battery only) ---------------------------------------
 * After a few seconds of no activity (no touch, no notification) the panel dims
 * to save power; any activity restores it. The dim is applied to the PANEL ONLY —
 * it never changes s_brightness — so restoring is exact and the user's chosen
 * brightness is untouched. Toggle + dim level persist in NVS.
 *   - s_autodim_on    : feature enabled (default on)
 *   - s_autodim_pct   : how dim, as a PERCENT of the user's brightness (default 25%)
 *   - s_dimmed        : are we currently dimmed? (RAM only) */
static bool    s_autodim_on  = true;
static uint8_t s_autodim_pct = 25;        // 5..90 % of the user's brightness
// s_dimmed is declared up by settings_apply_brightness (which clears it).

static bool    settings_get_autodim(void)     { return s_autodim_on; }
static uint8_t settings_get_autodim_pct(void) { return s_autodim_pct; }

/* Push the panel to the dimmed level (a fraction of the user's brightness, floored
 * so it never goes fully dark) or back to full — WITHOUT touching s_brightness. */
static void settings_dim_set(bool dim) {
  if (dim == s_dimmed) return;            // debounce: only write the panel on a transition
  s_dimmed = dim;
  if (dim) {
    uint16_t d = (uint16_t)s_brightness * s_autodim_pct / 100;
    if (d < 8) d = 8;                     // keep it visibly on, never black
    co5300->setBrightness((uint8_t)d);    // panel only; s_brightness preserved
  } else {
    co5300->setBrightness(s_brightness);  // restore the user's exact brightness
  }
}
static bool settings_is_dimmed(void) { return s_dimmed; }

static void settings_set_autodim(bool on) {
  s_autodim_on = on;
  prefs.putBool("autodim", on);
  if (!on) settings_dim_set(false);       // turning it off immediately un-dims
}
static void settings_set_autodim_pct(uint8_t pct) {
  if (pct < 5)  pct = 5;
  if (pct > 90) pct = 90;
  s_autodim_pct = pct;
  prefs.putUChar("autodimp", pct);
  // If we're currently dimmed, re-apply at the new level so the change is visible.
  if (s_dimmed) { s_dimmed = false; settings_dim_set(true); }
}

/* Dim while on USB power too? Default OFF: historically the watch never auto-dims
 * while plugged in (the assumption being you're at a desk / programming). But when
 * you're just charging — e.g. overnight — a screen that keeps lighting up is the
 * annoyance, so this opt-in lets the idle dim apply on USB as well. It ONLY affects
 * dimming: the watch still never deep-sleeps on USB (so it stays reachable while
 * charging), and CAFFEINE still always wins (its whole job is to keep the screen
 * on, so it's never dimmed regardless of this toggle). */
static bool s_dim_on_usb = false;
static bool settings_get_dim_on_usb(void) { return s_dim_on_usb; }
static void settings_set_dim_on_usb(bool on) {
  s_dim_on_usb = on;
  prefs.putBool("dimusb", on);
}

/* Show the little voltage readout in the watch-face top-right corner? Default ON.
 * Purely cosmetic — the value is still read for the battery widget regardless; this
 * only controls whether that corner label is visible. Persisted to NVS. Applied
 * live by watchface_apply_volt_visible() (forward-declared in watch_base.h). */
static bool s_show_volt = true;
static bool settings_get_show_volt(void) { return s_show_volt; }
static void settings_set_show_volt(bool on) {
  s_show_volt = on;
  prefs.putBool("showvolt", on);
  watchface_apply_volt_visible();         // reflect on the face immediately
}

/* Swap the WiFi and BLE indicators on the watch face? Default OFF.
 *   OFF (default): WiFi lives in the right stat column, BLE glyph in the top-right tray.
 *   ON           : WiFi glyph moves to the top-right tray, BLE takes the right column.
 * Purely a layout choice — both indicators still reflect their real state regardless;
 * this only controls WHERE each is drawn. Persisted to NVS. Applied live by
 * watchface_apply_indicator_layout() (forward-declared in watch_base.h). */
static bool s_swap_wifi_ble = false;
static bool settings_get_swap_wifi_ble(void) { return s_swap_wifi_ble; }
static void settings_set_swap_wifi_ble(bool on) {
  s_swap_wifi_ble = on;
  prefs.putBool("swapwb", on);
  watchface_apply_indicator_layout();     // reflect on the face immediately
}

/* ---- UI accent color (personalization) -------------------------------------
 * One RGB value (0xRRGGBB) used for every accent in the UI: section headers,
 * graph lines, sliders, switches, the freq highlight and the pull-down shade.
 * Stored as a plain uint32 (no LVGL type here so this header stays UI-agnostic);
 * the UI wraps it in lv_color_hex(). Screens that rebuild on open pick it up for
 * free; the always-resident shade is restyled live via quick_shade_restyle(). */
static uint32_t s_accent = 0x00B0FF;      // default: the blue we started with
static uint32_t settings_get_accent(void) { return s_accent; }
static void settings_set_accent(uint32_t rgb) {
  s_accent = rgb & 0xFFFFFF;
  prefs.putUInt("accent", s_accent);      // persist to NVS (survives reboot)
}

/* Lighten an 0xRRGGBB color by `amt`/255 toward white (integer-only, so it works
 * without LVGL). Used to derive the softer header tint from the base accent so a
 * single knob keeps the two shades coordinated for ANY chosen color. */
static inline uint32_t ui_lighten_hex(uint32_t c, uint8_t amt) {
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r += ((255 - r) * amt) / 255;
  g += ((255 - g) * amt) / 255;
  b += ((255 - b) * amt) / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static inline uint32_t ui_accent_hex(void)      { return s_accent; }              // base accent
static inline uint32_t ui_accent_soft_hex(void) { return ui_lighten_hex(s_accent, 60); } // header tint

/* ---- Monochrome-accent mode (personalization) ------------------------------
 * When ON, every DECORATIVE color in the UI (the otherwise-varied category icon
 * tints, graph lines, the green pager buttons, the music pink, etc.) collapses to
 * the single accent color, for a plain, uniform look. Structural colors — dark
 * backgrounds, greys and white text — are left alone so contrast/readability are
 * unchanged. Decorative call sites pass their normal color through ui_deco_hex():
 * it returns the accent in mono mode, else the original color, so a single flag
 * recolors the whole UI without each screen knowing about the mode. Persisted. */
static bool s_mono_accent = false;
static bool settings_get_mono_accent(void) { return s_mono_accent; }
static void settings_set_mono_accent(bool on) {
  s_mono_accent = on;
  prefs.putBool("monoacc", on);
}

/* Map a DECORATIVE color through the mono-accent mode. Pass the color a site would
 * normally use; get the accent back when mono mode is on, else the original. Use
 * ONLY for decorative accents (icon tints, graph/slider colors, colored buttons) —
 * never for backgrounds, greys or white, which must stay put for readability. */
static inline uint32_t ui_deco_hex(uint32_t normal) {
  return s_mono_accent ? s_accent : normal;
}

/* Darken an 0xRRGGBB color by `amt`/255 toward black (integer-only). Mirror of
 * ui_lighten_hex; used to derive a DIM accent for "inactive" decorative states. */
static inline uint32_t ui_darken_hex(uint32_t c, uint8_t amt) {
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r -= (r * amt) / 255;
  g -= (g * amt) / 255;
  b -= (b * amt) / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Decorative color for an INACTIVE/dim state (e.g. a disconnected indicator). In
 * mono mode returns a strongly-dimmed accent (so it reads as "the accent, but off");
 * otherwise the site's normal dim color. Pairs with ui_deco_hex for the bright state. */
static inline uint32_t ui_deco_dim_hex(uint32_t normal) {
  return s_mono_accent ? ui_darken_hex(s_accent, 200) : normal;
}

/* ---- Mute (persisted) ----
 * Silences any device sound (alarm tone, future UI sounds) while STILL allowing
 * vibration. Sound paths must check settings_get_mute() before making noise. */
static bool s_mute = false;
static bool settings_get_mute(void) { return s_mute; }
static void settings_set_mute(bool m) { s_mute = m; prefs.putBool("mute", m); }

/* ---- Caffeine / keep-awake (session only, NOT persisted) ----
 * While on, the idle-sleep timeout never fires, so the watch stays awake and
 * timers/stopwatch run to the exact second. Deliberately resets to OFF on every
 * boot so a crash/reboot can't strand the watch awake draining the battery. */
static bool s_caffeine = false;
static bool caffeine_get(void) { return s_caffeine; }
static void caffeine_set(bool e) { s_caffeine = e; }

/* Notification check interval (minutes), persisted. The value itself
 * (s_check_interval_min) lives in watch_base.h. Clamped to the range the Power
 * app's stepper exposes (2..20 min): shorter than 2 wakes too aggressively for the
 * battery, longer than 20 is the cap the UI offers. */
#define CHECK_INTERVAL_MIN_MIN  2
#define CHECK_INTERVAL_MIN_MAX  20
static uint16_t settings_get_check_interval(void) { return s_check_interval_min; }
static void settings_set_check_interval(uint16_t m) {
  if (m < CHECK_INTERVAL_MIN_MIN) m = CHECK_INTERVAL_MIN_MIN;
  if (m > CHECK_INTERVAL_MIN_MAX) m = CHECK_INTERVAL_MIN_MAX;
  s_check_interval_min = m;
  prefs.putUShort("checkmin", m);
}

/* Background-check enable toggle (Settings can flip it off for true power-off). */
static bool s_checks_enabled = true;
static bool settings_get_checks_enabled(void) { return s_checks_enabled; }
static void settings_set_checks_enabled(bool en) {
  s_checks_enabled = en;
  prefs.putBool("checks", en);
}

/* Radio enable toggles, persisted. When WiFi is OFF the watch never
 * brings up the STA radio, so no notification fetches happen — fully silent and
 * lowest-power. BLE is the (future) short-lived credential-sharing channel; the
 * flag gates whether it may advertise at all. Default: WiFi on, BLE off. */
static bool s_wifi_enabled = true;
static bool s_ble_enabled  = false;
static bool settings_get_wifi_enabled(void) { return s_wifi_enabled; }
static void settings_set_wifi_enabled(bool en) {
  s_wifi_enabled = en;
  prefs.putBool("wifien", en);
  if (!en) {                       // going dark: tear down the radio immediately
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}
static bool settings_get_ble_enabled(void) { return s_ble_enabled; }
static void settings_set_ble_enabled(bool en) {
  s_ble_enabled = en;
  prefs.putBool("bleen", en);
  // NOTE: the BLE stack itself isn't wired up yet — this only persists intent so
  // the future provisioning code can honor it. No radio is started here.
}

/* ---- Radio TX power (router-style range knob) ----
 * Lower TX power = lower current during every radio burst (WiFi TX peaks ~300 mA
 * at full power, well under half that at the low tiers) at the cost of range. A
 * small apartment is fine on the low tiers; Max = stock behavior. Five shared
 * tiers, persisted as an index.
 *
 * Apply points (the drivers reset power on init, so set-and-forget won't stick):
 *   - WiFi: settings_apply_wifi_txp() after every successful connect
 *     (wifi_connect in notif_net.h) — esp_wifi_set_max_tx_power needs the driver
 *     STARTED, and each fetch restarts it.
 *   - BLE:  settings_apply_ble_txp() right after BLEDevice::init() (ble_begin),
 *     gated on the controller actually being up so a stray call can't fault. */
#include <esp_wifi.h>   // esp_wifi_set_max_tx_power
#include <esp_bt.h>     // esp_ble_tx_power_set / esp_bt_controller_get_status

#define RADIO_TXP_COUNT 7
static const char  *RADIO_TXP_NAMES[RADIO_TXP_COUNT] = { "Min", "VLow", "Low", "Mid", "High", "VHigh", "Max" };
static const int8_t WIFI_TXP_QDBM[RADIO_TXP_COUNT]   = { 8, 20, 28, 44, 60, 72, 80 };  // 0.25 dBm units
static const int8_t WIFI_TXP_DBM[RADIO_TXP_COUNT]    = { 2, 5, 7, 11, 15, 18, 20 };    // UI labels
/* BLE ladder spread low on purpose: the phone is on the same body, not across the
 * room. Min = the S3 controller's hardware floor (-24 dBm) — works desk-distance,
 * can get marginal wrist-to-pocket THROUGH the body (link shows as lag/reconnects,
 * not silence; just cycle up a tier). Low (-12) is the safe everyday low; VHigh (+9)
 * is the old stock maximum; Max (+20) is the controller's ceiling for long range at
 * a real battery cost. WiFi has no headroom in either direction: its Min (8 quarter-
 * dBm = 2 dBm) and Max (80 = 20 dBm) are already the API floor and ceiling. */
static const esp_power_level_t BLE_TXP_LVL[RADIO_TXP_COUNT] = {
  ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, ESP_PWR_LVL_N6,
  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  ESP_PWR_LVL_P20 };
static const int8_t BLE_TXP_DBM[RADIO_TXP_COUNT]     = { -24, -18, -12, -6, 0, 9, 20 };

static uint8_t s_wifi_txp = RADIO_TXP_COUNT - 1;   // default Max = stock (20 dBm)
static uint8_t s_ble_txp  = RADIO_TXP_COUNT - 2;   // default VHigh = stock (+9); Max
                                                   // (+20) is opt-in, it costs real mA

static void settings_apply_wifi_txp(void) {
  // No-op (harmless error) unless the WiFi driver is started; called after each
  // connect so the setting survives the driver's off/on cycles between fetches.
  esp_wifi_set_max_tx_power(WIFI_TXP_QDBM[s_wifi_txp]);
}
static void settings_apply_ble_txp(void) {
  if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) return;
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, BLE_TXP_LVL[s_ble_txp]);  // connections
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     BLE_TXP_LVL[s_ble_txp]);  // advertising
}

static uint8_t settings_get_wifi_txp(void) { return s_wifi_txp; }
static void settings_set_wifi_txp(uint8_t idx) {
  s_wifi_txp = idx % RADIO_TXP_COUNT;
  prefs.putUChar("wifitxp", s_wifi_txp);
  settings_apply_wifi_txp();               // live if the radio happens to be up
}
static uint8_t settings_get_ble_txp(void) { return s_ble_txp; }
static void settings_set_ble_txp(uint8_t idx) {
  s_ble_txp = idx % RADIO_TXP_COUNT;
  prefs.putUChar("bletxp", s_ble_txp);
  settings_apply_ble_txp();                // live if the controller is up
}

/* CPU frequency (MHz) while AWAKE. Lower = less power on the watch face; deep
 * sleep is unaffected (CPU powers down regardless). Only specific S3 values are
 * valid AND WiFi-capable: 240/160/80. setCpuFrequencyMhz() applies it. */
static const uint16_t CPU_FREQS[] = { 240, 160, 80 };
static const uint8_t  CPU_FREQ_COUNT = sizeof(CPU_FREQS) / sizeof(CPU_FREQS[0]);
static uint16_t s_cpu_mhz = 240;          // default speed

/* ===========================================================================
 *  EXPERIMENTAL — per-frequency system-rail UNDERVOLT (AXP2101 DCDC1)
 *  ---------------------------------------------------------------------------
 *  Read this before enabling. This is NOT classic core-voltage scaling: the
 *  ESP32-S3 regulates its own ~1.1V digital core with an INTERNAL LDO that
 *  Arduino cannot set. The only software voltage knob on THIS board is the
 *  AXP2101 DCDC1 buck, which feeds the chip's 3.3V VDD pins (plus flash and the
 *  3.3V peripherals). Trimming it shaves the LDO + peripheral conversion loss —
 *  modest savings (single-digit mW), real brownout risk. Lower CPU clocks pull
 *  smaller current spikes, so they tolerate a slightly lower rail before sagging
 *  into a brownout — hence one voltage per frequency.
 *
 *  SAFE WINDOW: the S3 needs 3.0-3.6V and its brownout detector sits near ~3.0V;
 *  flash + the AMOLED want a healthy 3.3V. Stay in [3000, 3300] mV on the AXP2101
 *  100mV grid (driver hard limits are 1500-3400). WiFi TX bursts are the usual
 *  brownout trigger, so this is applied ONLY on the interactive (full-UI) path,
 *  never during background WiFi checks (those stay at the stock 3.3V rail).
 *
 *  TUNING (slow + safe): the voltage is a PURE FUNCTION of the table below and is
 *  NEVER persisted on its own, so every experiment is just a reflash. Lower ONE
 *  entry by 100mV, flash, then stress it (max brightness + a WiFi fetch + busy
 *  CPU) for a while. If you ever see the on-screen "BROWNOUT" boot banner or
 *  random resets, that value is too low — raise it 100mV. Can't boot? Hold BOOT,
 *  tap RST (ROM download mode ignores the app) and reflash; a cold power-cycle
 *  also returns DCDC1 to its 3.3V default. That's why this is hard to brick.
 *
 *  DEFAULT = 3300 everywhere = STOCK (no undervolt). Flip UNDERVOLT_ENABLE to 1
 *  and edit CPU_UVOLT_MV[] (1:1 with CPU_FREQS[] above) to opt in.
 * ======================================================================== */
#define UNDERVOLT_ENABLE  0       // 0 = always stock 3.3V rail; 1 = apply table
#define UNDERVOLT_MIN_MV  3000    // hard floor; the apply fn clamps to this
#define UNDERVOLT_MAX_MV  3300    // never push the rail above the board's 3.3V

//   CPU MHz index:                      240   160    80
static const uint16_t CPU_UVOLT_MV[] = { 3300, 3300, 3300 };  // STOCK (safe default)
//   Conservative first step to try (validate EACH before trusting it):
//   static const uint16_t CPU_UVOLT_MV[] = { 3300, 3200, 3200 };
//   Aggressive (no WiFi; expect occasional brownouts at the low edges):
//   static const uint16_t CPU_UVOLT_MV[] = { 3200, 3100, 3100 };
static_assert(sizeof(CPU_UVOLT_MV) == sizeof(CPU_FREQS),
              "CPU_UVOLT_MV[] must have exactly one entry per CPU_FREQS[] entry");

/* Push the rail voltage paired with `mhz` to DCDC1. No-op unless the feature is
 * enabled AND the PMU is present. Clamped to [MIN,MAX] and snapped to the AXP2101
 * 100mV grid (so setDC1Voltage() always accepts it). `power`/`pmu_ok` live in the
 * main sketch, declared before this header is included. */
static void settings_apply_rail_for_mhz(uint16_t mhz) {
#if UNDERVOLT_ENABLE
  if (!pmu_ok) return;                          // no PMU -> never touch the rail
  uint16_t mv = UNDERVOLT_MAX_MV;               // fall back to stock if unmatched
  for (uint8_t i = 0; i < CPU_FREQ_COUNT; i++)
    if (CPU_FREQS[i] == mhz) { mv = CPU_UVOLT_MV[i]; break; }
  if (mv < UNDERVOLT_MIN_MV) mv = UNDERVOLT_MIN_MV;
  if (mv > UNDERVOLT_MAX_MV) mv = UNDERVOLT_MAX_MV;
  mv = (uint16_t)((mv / 100) * 100);            // snap to the 100mV step grid
  power.setDC1Voltage(mv);                      // DCDC1 = ESP32-S3 main 3.3V rail
#else
  (void)mhz;
#endif
}

static void settings_apply_cpu_mhz(uint16_t mhz) {
  s_cpu_mhz = mhz;
  // Order matters when SPEEDING UP: raise the rail before the clock so the chip
  // never runs fast on a low rail. At boot the PMU isn't up yet, so this call
  // no-ops and the explicit apply after power.begin() handles the initial set.
  settings_apply_rail_for_mhz(mhz);
  setCpuFrequencyMhz(mhz);                 // takes effect immediately
  // Re-apply the REAL core undervolt AFTER the clock change: setCpuFrequencyMhz()
  // reprograms dig_dbias for the new frequency, so our override must follow it.
  // (No-op unless UNDERVOLT_CORE_ENABLE is set in core_voltage.h.) This is the
  // preferred undervolt path; the rail trick above is minor and higher-risk.
  core_apply_for_mhz(mhz);
}
static uint16_t settings_get_cpu_mhz(void) { return s_cpu_mhz; }
static void settings_set_cpu_mhz(uint16_t mhz) {
  settings_apply_cpu_mhz(mhz);
  prefs.putUShort("cpumhz", mhz);          // persist to NVS
}

/* Load all persisted settings from NVS into the globals above. Opens the
 * "watch" namespace (shared by the WiFi/notification stores). */
static void settings_load(void) {
  prefs.begin("watch", false);            // namespace "watch", read/write
  s_brightness        = prefs.getUChar ("bright",   s_brightness);
  s_accent            = prefs.getUInt  ("accent",   s_accent);
  s_mute              = prefs.getBool  ("mute",     s_mute);
  s_check_interval_min= prefs.getUShort("checkmin", s_check_interval_min);
  s_checks_enabled    = prefs.getBool  ("checks",   s_checks_enabled);
  s_cpu_mhz           = prefs.getUShort("cpumhz",   s_cpu_mhz);
  s_wifi_enabled      = prefs.getBool  ("wifien",   s_wifi_enabled);
  s_ble_enabled       = prefs.getBool  ("bleen",    s_ble_enabled);
  s_wifi_txp          = prefs.getUChar ("wifitxp",  s_wifi_txp) % RADIO_TXP_COUNT;
  s_ble_txp           = prefs.getUChar ("bletxp",   s_ble_txp)  % RADIO_TXP_COUNT;
  s_autodim_on        = prefs.getBool  ("autodim",  s_autodim_on);
  s_autodim_pct       = prefs.getUChar ("autodimp", s_autodim_pct);
  s_dim_on_usb        = prefs.getBool  ("dimusb",   s_dim_on_usb);
  s_show_volt         = prefs.getBool  ("showvolt", s_show_volt);
  s_swap_wifi_ble     = prefs.getBool  ("swapwb",   s_swap_wifi_ble);
  s_mono_accent       = prefs.getBool  ("monoacc",  s_mono_accent);
}
