/* ============================================================================
 *  sleep_power.h — deep sleep + the AXP2101 peripheral-rail safety probe.
 *
 *  Everything around powering the watch down and back up:
 *    - the runtime per-rail safety probe (which AXP2101 LDOs can be cut in sleep
 *      without deadlocking the I2C bus / losing the RTC), its NVS journal, and the
 *      Power-screen accessors (rail_* — forward-declared in the .ino for app_power.h);
 *    - rails_restore() (boot) / rails_cut_for_sleep() (pre-sleep);
 *    - the one-shot SD-rail diagnostic (guarded by SD_RAIL_DIAG, default 0);
 *    - the deep-sleep entry paths (enter_deep_sleep / arm_wakes_and_sleep /
 *      rearm_and_deep_sleep) and the light background-check path
 *      (background_check_has_new) that a timer wake runs without the display.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER:
 *    - the hardware objects (power/pmu_ok, gfx, rtc, Wire, USBSerial) and the
 *      config macros (BOOT_BTN_GPIO, SLEEP_INTENT_MAGIC, rtc_sleep_intent,
 *      IIC_SDA/SCL, AXP2101_SLAVE_ADDRESS) defined at the top of the .ino;
 *    - settings_store.h (s_checks_enabled, s_check_interval_min via watch_base.h),
 *      timer_store.h, haptics.h, power_model.h (calib_note_*), ble_provision.h
 *      (ble_end), sd_card.h (SDMMC pins) and notif_net.h (notif_fetch_raw,
 *      rtc_last_notif_id) — all the things the sleep paths call.
 *  The .ino forward-declares the rail_* accessors (for app_power.h, included
 *  earlier); this header defines them.
 *
 *  NOTE: rounder_event_cb (the CO5300 even-alignment LVGL callback) deliberately
 *  does NOT live here — it's a display callback and stays with my_disp_flush /
 *  my_touchpad_read in the .ino.
 * ========================================================================== */
#pragma once
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_system.h>

/* ===================== runtime peripheral-rail safety probe ==================
 * We can't trust the schematic for which AXP2101 rail powers what, and the ONE
 * rail we must never cut is whichever feeds the shared I2C pull-ups (cutting it
 * deadlocks the wake — you can't re-enable a rail over the bus it powers). So the
 * watch probes each candidate rail at runtime: disable it, check the PMU + RTC
 * still ACK on I2C, then re-enable. Rails that pass are marked SAFE — meaning
 * "won't deadlock the wake" — but that is NOT enough on its own: a rail can pass
 * the I2C test yet still power the DISPLAY, so cutting it leaves a blank screen on
 * wake. So actually cutting a rail is OPT-IN PER RAIL (s_rail_cut, default OFF):
 * you enable SAFE rails one at a time from Settings>Power and confirm the screen
 * comes back after a real sleep/wake. Nothing is cut until you say so.
 *
 * Crash-safe probe: the rail being probed is journaled to NVS *before* it's
 * disabled, so if a probe freezes the bus, the next boot (after a cold power-cycle
 * restores defaults) sees the journal and marks that rail UNSAFE. Only peripheral
 * LDOs are candidates — never DCDC1 (ESP32) or the RTC supply.
 *
 * Recovery if you enable a bad rail (blank on wake): cold power-cycle (PWR off,
 * then on) -> the screen works again -> turn on Caffeine (so it won't sleep) ->
 * Settings>Power -> toggle that rail back off. */
enum { RAIL_ALDO1, RAIL_ALDO2, RAIL_ALDO3, RAIL_ALDO4, RAIL_BLDO1, RAIL_BLDO2, RAIL_COUNT };
enum { RAIL_UNKNOWN = 0, RAIL_SAFE = 1, RAIL_UNSAFE = 2 };
#define PCF85063_I2C_ADDR 0x51
static uint8_t s_rail_state[RAIL_COUNT];   // probe verdict (UNKNOWN/SAFE/UNSAFE)
static uint8_t s_rail_cut[RAIL_COUNT];     // user opt-in: actually cut this rail in sleep?

/* Owner-validated default cut set for THIS board: the INSTANT-wake sensor/codec
 * rails. ALDO1/ALDO2/ALDO4 are display rails — cutting them forces a 5-10 s cold
 * AMOLED reinit on wake for ~no saving (the panel is already in µA panel-sleep),
 * so they're left ON. Bumping RAILCUT_VER re-seeds this onto an existing device
 * once; after that your Settings>Power toggles win. */
#define RAILCUT_VER 1
//                                          ALDO1 ALDO2 ALDO3 ALDO4 BLDO1 BLDO2
static const uint8_t RAIL_DEFAULT_CUT[RAIL_COUNT] = { 0,    0,    1,    0,    1,    1 };

static const char *rail_name(uint8_t i) {
  static const char *N[RAIL_COUNT] = { "ALDO1","ALDO2","ALDO3","ALDO4","BLDO1","BLDO2" };
  return (i < RAIL_COUNT) ? N[i] : "?";
}
static void rail_set(uint8_t i, bool on) {
  switch (i) {
    case RAIL_ALDO1: on ? power.enableALDO1() : power.disableALDO1(); break;
    case RAIL_ALDO2: on ? power.enableALDO2() : power.disableALDO2(); break;
    case RAIL_ALDO3: on ? power.enableALDO3() : power.disableALDO3(); break;
    case RAIL_ALDO4: on ? power.enableALDO4() : power.disableALDO4(); break;
    case RAIL_BLDO1: on ? power.enableBLDO1() : power.disableBLDO1(); break;
    case RAIL_BLDO2: on ? power.enableBLDO2() : power.disableBLDO2(); break;
  }
}
static bool rail_is_on(uint8_t i) {
  switch (i) {
    case RAIL_ALDO1: return power.isEnableALDO1();
    case RAIL_ALDO2: return power.isEnableALDO2();
    case RAIL_ALDO3: return power.isEnableALDO3();
    case RAIL_ALDO4: return power.isEnableALDO4();
    case RAIL_BLDO1: return power.isEnableBLDO1();
    case RAIL_BLDO2: return power.isEnableBLDO2();
  }
  return false;
}
static bool i2c_acks(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}
/* Bus is "alive" only if BOTH the PMU and the RTC still answer — so a rail that
 * powers either the I2C pull-ups OR the RTC counts as unsafe (cutting it would
 * deadlock the wake or lose the clock). */
static bool i2c_bus_alive(void) {
  return i2c_acks(AXP2101_SLAVE_ADDRESS) && i2c_acks(PCF85063_I2C_ADDR);
}

static void rails_load_state(void) {
  if (prefs.getBytes("railstate", s_rail_state, RAIL_COUNT) != RAIL_COUNT)
    memset(s_rail_state, RAIL_UNKNOWN, RAIL_COUNT);
  // Cut opt-ins: seed the validated default set on a fresh device OR once after a
  // RAILCUT_VER bump; otherwise honor the saved per-rail toggles.
  if (prefs.getUChar("railcutver", 0) != RAILCUT_VER) {
    memcpy(s_rail_cut, RAIL_DEFAULT_CUT, RAIL_COUNT);
    prefs.putBytes("railcut", s_rail_cut, RAIL_COUNT);
    prefs.putUChar("railcutver", RAILCUT_VER);
  } else if (prefs.getBytes("railcut", s_rail_cut, RAIL_COUNT) != RAIL_COUNT) {
    memcpy(s_rail_cut, RAIL_DEFAULT_CUT, RAIL_COUNT);
  }
}
static void rails_save_state(void) {
  prefs.putBytes("railstate", s_rail_state, RAIL_COUNT);
  prefs.putBytes("railcut", s_rail_cut, RAIL_COUNT);
}

/* ---- accessors for the Power screen ---- */
static uint8_t rail_state_count(void) { return RAIL_COUNT; }
static uint8_t rail_state_raw(uint8_t i) { return i < RAIL_COUNT ? s_rail_state[i] : RAIL_UNKNOWN; }
static bool    rail_cut_get(uint8_t i) { return i < RAIL_COUNT && s_rail_cut[i]; }
static void    rail_cut_set(uint8_t i, bool on) {
  if (i >= RAIL_COUNT) return;
  s_rail_cut[i] = on ? 1 : 0;
  rails_save_state();
}
static const char *rail_verdict_str(uint8_t i) {
  switch (i < RAIL_COUNT ? s_rail_state[i] : RAIL_UNKNOWN) {
    case RAIL_SAFE:   return "bus-safe";       // passed the I2C test (can be opted-in)
    case RAIL_UNSAFE: return "kept on";        // powers I2C/RTC -> never cut
    default:          return "untested";
  }
}
static uint8_t rails_cut_count(void) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < RAIL_COUNT; i++) if (s_rail_cut[i]) n++;
  return n;
}

/* Bring EVERY peripheral rail back up at boot — BEFORE the RTC read + display init.
 * Unconditional (not just opted-in rails) so the watch always recovers to full
 * power no matter what a prior sleep — or prior firmware — left disabled: a blank
 * screen after a reflash is just the previous cut still latched in the AXP2101.
 * Generous settle so the AMOLED rail is stable before gfx->begin(). */
static void rails_restore(void) {
  if (!pmu_ok) return;
  // Bring rails up ONE AT A TIME with a gap between them. Enabling all at once
  // yanks a big inrush spike that sags VSYS into a brownout — the random hang seen
  // with several rails cut. Staggering spreads the current. Each rail is confirmed
  // on with a short retry, since the AXP2101 can NAK a write right at boot before
  // its I2C is fully ready (a silent miss would leave a peripheral, e.g. the
  // display, dark — the intermittent blank wake).
  bool turned_any = false;
  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
    if (rail_is_on(i)) continue;                 // already on -> no inrush, no wait
    for (int t = 0; t < 5 && !rail_is_on(i); t++) { rail_set(i, true); delay(4); }
    delay(10);                   // stagger gap (limits combined inrush)
    turned_any = true;
  }
  if (turned_any) delay(80);     // settle the (re-powered) peripherals before gfx->begin()
}

/* ===================== SD-rail diagnostic (one-shot) ========================
 * Find WHICH AXP2101 peripheral rail powers the microSD card, so we can keep
 * cutting every OTHER rail in sleep but never the SD one. Method: bring all rails
 * up and confirm the card mounts; then disable rails ONE AT A TIME and re-mount —
 * the rail whose removal breaks the mount is the SD's supply. Prints a verdict per
 * rail + a summary line. Set SD_RAIL_DIAG to 1 (top of the .ino), flash, read the
 * serial log, then set it back to 0. (Leaves all rails ON so the watch runs.) */
#ifndef SD_RAIL_DIAG
#define SD_RAIL_DIAG 0
#endif
#if SD_RAIL_DIAG
/* Fresh mount attempt for the diagnostic (bypasses sd_card.h's latch/retry state). */
static bool sd_diag_try_mount(void) {
  SD_MMC.end();
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  bool ok = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
  ok = ok && (SD_MMC.cardType() != CARD_NONE);
  SD_MMC.end();                 // leave the peripheral clean for the next test
  return ok;
}
static void sd_rail_diag(void) {
  if (!pmu_ok) { USBSerial.println("[sddiag] no PMU -> skip"); return; }
  USBSerial.println("[sddiag] ==== SD power-rail diagnostic ====");

  // 1) All rails ON -> baseline. If this fails, it's NOT a rail (pins/socket/card).
  for (uint8_t i = 0; i < RAIL_COUNT; i++) { rail_set(i, true); delay(8); }
  delay(120);                   // let everything settle + the card power up
  bool base = sd_diag_try_mount();
  USBSerial.printf("[sddiag] all rails ON -> mount %s\n", base ? "OK" : "FAILED");
  if (!base) {
    USBSerial.println("[sddiag] card does NOT mount even with all rails on ->");
    USBSerial.println("[sddiag]   the cause is NOT a cut rail (check socket/pins/card).");
    return;
  }

  // 2) Drop each rail in turn; if the mount breaks, that rail feeds the SD card.
  //    SKIP rails the probe already marked UNSAFE — those power the I2C pull-ups or
  //    RTC, so cutting one here could prevent us re-enabling it (bus deadlock). They
  //    can't be the SD rail in any case (we never cut them in sleep).
  int sd_rail = -1;
  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
    if (s_rail_state[i] == RAIL_UNSAFE) {
      USBSerial.printf("[sddiag]   %s skipped (UNSAFE: powers I2C/RTC)\n", rail_name(i));
      continue;
    }
    rail_set(i, false); delay(120);             // cut just this rail, settle
    bool ok = sd_diag_try_mount();
    rail_set(i, true);  delay(60);              // restore it before the next test
    USBSerial.printf("[sddiag]   %s OFF -> mount %s%s\n",
                     rail_name(i), ok ? "OK" : "FAILED",
                     ok ? "" : "   <== SD card is on this rail");
    if (!ok && sd_rail < 0) sd_rail = i;
  }

  // Make sure everything is back ON before normal boot continues.
  for (uint8_t i = 0; i < RAIL_COUNT; i++) { rail_set(i, true); delay(8); }
  delay(80);

  if (sd_rail >= 0)
    USBSerial.printf("[sddiag] RESULT: SD card is powered by %s — exclude it from the cut set.\n",
                     rail_name(sd_rail));
  else
    USBSerial.println("[sddiag] RESULT: no single rail broke the mount (SD may be on an "
                      "always-on rail; cutting any one is safe).");
  USBSerial.println("[sddiag] ==== end (all rails left ON) ====");
}
#endif  // SD_RAIL_DIAG

/* Cut the opted-in rails right before deep sleep — but only ones the probe also
 * verified bus-safe, so a seeded default can never cut a rail this unit hasn't
 * cleared (e.g. before the probe has run, or on different silicon). */
static void rails_cut_for_sleep(void) {
  if (!pmu_ok) return;
  for (uint8_t i = 0; i < RAIL_COUNT; i++)
    if (s_rail_cut[i] && s_rail_state[i] == RAIL_SAFE) rail_set(i, false);
}

/* Probe any not-yet-classified rails. Runs once the full UI is up (interactive
 * boots only). Stops early if a probe leaves the bus stuck — a power-cycle then
 * resumes via the NVS journal. */
static void rails_probe(void) {
  if (!pmu_ok) return;
  rails_load_state();
  bool changed = false, stuck = false;

  // A journaled probe that didn't clear = it froze the bus last boot -> UNSAFE.
  uint8_t pj = prefs.getUChar("railprobe", 255);
  if (pj < RAIL_COUNT) {
    s_rail_state[pj] = RAIL_UNSAFE;
    rails_save_state();
    prefs.putUChar("railprobe", 255);
    changed = true;
    USBSerial.printf("[rails] %s stuck the bus last boot -> UNSAFE\n", rail_name(pj));
  }

  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
    if (s_rail_state[i] != RAIL_UNKNOWN) continue;
    prefs.putUChar("railprobe", i);          // journal BEFORE disabling
    rail_set(i, false);
    delay(30);                               // settle
    bool alive = i2c_bus_alive();            // PMU + RTC still reachable?
    rail_set(i, true);                       // restore (may fail if the bus is dead)
    delay(15);
    bool recovered = i2c_bus_alive();
    s_rail_state[i] = alive ? RAIL_SAFE : RAIL_UNSAFE;
    rails_save_state();
    prefs.putUChar("railprobe", 255);
    changed = true;
    USBSerial.printf("[rails] %s -> %s\n", rail_name(i), alive ? "SAFE (will cut in sleep)" : "UNSAFE");
    if (!recovered) { stuck = true; break; } // bus dead -> needs a cold power-cycle
  }

  // If we classified anything (and the bus is still healthy), reboot into a clean
  // state: probing a rail can momentarily glitch a peripheral (e.g. the display),
  // and a fresh boot re-runs rails_restore() + a clean display init. If the bus got
  // stuck, DON'T restart (it would just reboot into the dead bus) — the watch will
  // sit until a cold power-cycle (hold PWR off, then on) restores the rail, after
  // which the next boot marks it UNSAFE and continues.
  if (changed && !stuck) {
    USBSerial.println("[rails] classification updated -> restarting clean");
    USBSerial.flush();
    esp_restart();
  }
}

static void arm_wakes_and_sleep(void) {
  rtc_sleep_intent = SLEEP_INTENT_MAGIC;   // mark this as a CLEAN, intended sleep

  ble_end();                               // BLE can't run in deep sleep — tear it down + free its RAM

  // Periodic notification wake: only arm the timer if the user left background
  // checks ON (Power app toggle). With it OFF the watch sleeps with ONLY the BOOT
  // (EXT0) wake armed below — it never wakes on its own, so no battery is spent on
  // background fetches; you bring it back with a BOOT press. A RUNNING countdown
  // still arms a timer wake regardless, so the alarm always fires on time.
  bool     timer_due_wake = timer_is_active() && !timer_is_paused();
  uint32_t almc_in        = almclk_seconds_until();   // 0 = no alarm clock armed
  if (s_checks_enabled || timer_due_wake || almc_in) {
    // Wake at the SOONEST deadline: the next notification check (only if checks
    // are on), a running countdown's due time, the alarm clock's fire time.
    // (Previously a running countdown with checks OFF woke at the check interval
    // anyway; now it sleeps straight through to its due time.)
    uint64_t us = UINT64_MAX;
    if (s_checks_enabled)
      us = (uint64_t)s_check_interval_min * 60ULL * 1000000ULL;
    if (timer_due_wake) {
      uint64_t tus = (uint64_t)timer_remaining_s() * 1000000ULL;
      if (tus < us) us = tus;
    }
    if (almc_in) {
      uint64_t aus = (uint64_t)almc_in * 1000000ULL;
      if (aus < us) us = aus;
    }
    if (us < 1000000ULL) us = 1000000ULL;   // floor at 1s so we never busy-wake
    esp_sleep_enable_timer_wakeup(us);
  }

  rtc_gpio_init((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_set_direction((gpio_num_t)BOOT_BTN_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BOOT_BTN_GPIO);              // idle HIGH
  rtc_gpio_pulldown_dis((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_hold_en((gpio_num_t)BOOT_BTN_GPIO);                // keep pull in sleep
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BTN_GPIO, 0); // wake on LOW (press)

  haptics_prepare_sleep();                 // latch motor LOW so it can't float-buzz
  audio_alarm_prepare_sleep();             // latch the codec/amp rail (GPIO46) OFF too
  rails_cut_for_sleep();                    // cut the proven-safe peripheral rails, last

  esp_deep_sleep_start();                  // does not return
}

static void enter_deep_sleep(void) {
  USBSerial.println("[power] entering deep sleep");
  USBSerial.flush();

  // Calibration duty accounting: millis() at this point is how long this wake
  // stayed awake; the upcoming deep sleep lasts the check interval. Both feed the
  // measured awake/sleep duty used to learn the sleep-floor current. With periodic
  // wake OFF the sleep is open-ended (until BOOT), so its length is unknown — skip
  // the sleep note then so a bogus duration can't skew the learned floor.
  calib_note_awake_ms(millis());
  if (s_checks_enabled)
    calib_note_sleep_s((uint32_t)s_check_interval_min * 60UL);

  gfx->fillScreen(RGB565_BLACK);
  gfx->displayOff();                      // AMOLED into its own low-power sleep

  arm_wakes_and_sleep();                  // timer + BOOT wake; does not return
}

/* Re-arm the timer (and BOOT wake) and go back to deep sleep WITHOUT touching
 * the display. Used by the light-check path when there's nothing new, so the
 * screen never lights. */
static void rearm_and_deep_sleep(void) {
  USBSerial.println("[check] nothing new; back to deep sleep");
  USBSerial.flush();
  // Account this brief background-check wake + the next sleep for the duty cycle,
  // same as the interactive sleep path, so the floor-current learner sees the
  // full timeline (these short checks are most of the watch's "awake" time).
  calib_note_awake_ms(millis());
  calib_note_sleep_s((uint32_t)s_check_interval_min * 60UL);
  arm_wakes_and_sleep();                  // timer + BOOT wake; does not return
}

/* If the light check found a notification, this flag tells the full UI boot to
 * pop the card for the newest stored item (s_notifs[0]). The item itself is
 * already persisted in the NVS store by notif_fetch_raw. */
static bool pending_notif = false;

/* LIGHT background check: bring up ONLY I2C(PMU/RTC) + WiFi, check the server.
 * Returns true if there's a new notification (caller then does the full UI
 * boot). On "nothing new", re-arms the alarm and powers off — never returns,
 * so the display is never initialized (screen stays dark). */
static bool background_check_has_new(void) {
  pmu_ok = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  Wire.setClock(400000);   // match setup(): 400 kHz Fast-mode (power.begin may reset it to 100 kHz)
  // Keep the charge cap + low undervoltage cutoff applied on background wakes too
  // (in case begin() reset PMU registers).
  if (pmu_ok) {
    power.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    power.setSysPowerDownVoltage(2600);
  }

  String title, body; uint64_t maxId = 0;
  int count = notif_fetch_raw(title, body, maxId);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if (count > 0 && maxId > rtc_last_notif_id) {
    rtc_last_notif_id = maxId;
    pending_notif = true;                  // newest stashed in s_pop_* by the fetch
    return true;                          // -> full boot + show it
  }

  // No NEW server notification — now check the iPhone over BLE (ANCS). If BLE is
  // enabled, bring it up and give iOS a window to reconnect + drain its pending
  // notifications into our store. Any new ANCS item also means "full boot to show
  // it". The ANCS parser already stashed the newest in s_pop_* + set pending_notif
  // semantics via the store, so we just flag pending_notif and boot.
  if (settings_get_ble_enabled()) {
    USBSerial.println("[wake] no server item -> checking iPhone via ANCS...");
    // ~9s budget: long enough for iOS to usually reconnect on a timer wake, short
    // enough to stay frugal. Bails early the instant something lands.
    if (ancs_background_check(9000)) {
      pending_notif = true;                // newest stashed in s_pop_* by the ANCS parser
      return true;                        // -> full boot + show it (BLE left up)
    }
  }

  rearm_and_deep_sleep();                // nothing new (server + iPhone): back to sleep
  return false;                          // unreachable
}
