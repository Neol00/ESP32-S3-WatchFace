/* ============================================================================
 *  wifi_store.h — known-WiFi networks store (SD-CSV first, NVS fallback).
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER settings_store.h (shared
 *  `prefs`), sd_card.h (sd_mount), and the WIFI_SSID / WIFI_PASS macros at the top
 *  of WatchFace.ino. Consumed by wifi_connect() and the WiFi & BLE screen.
 *
 *  WHERE THE LIST LIVES:
 *    - With an SD card in: /wifi.csv on the card is the SOURCE OF TRUTH and may
 *      hold ANY number of networks. The card takes PRECEDENCE over flash, so you
 *      can edit credentials by pulling the card, editing the CSV on a PC, and
 *      re-inserting it. If the card has no CSV yet, the current flash networks are
 *      written to it (flash -> SD duplicate). Whenever the list changes, the full
 *      list is written back to the CSV and the first few are mirrored to flash.
 *    - With NO card: the classic flash list (up to WIFI_NET_MAX) is used, exactly
 *      as before — so the watch always works cardless.
 *    - Flash always keeps the first <=WIFI_NET_MAX networks as a no-card fallback,
 *      so removing the card later still leaves you connected to recent networks.
 *
 *  CSV FORMAT (/wifi.csv), one network per line:
 *      SSID,password
 *    Split on the FIRST comma, so the PASSWORD may contain commas (the SSID may
 *    not). Blank lines and lines starting with '#' are ignored. An open network
 *    is just "SSID" with no comma. The file is hand-editable.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "esp_heap_caps.h"   // heap_caps_calloc — the network list lives in PSRAM
#include "sd_card.h"
#include "storage_fs.h"

#define WIFI_NET_MAX      5      // flash (NVS) cap — the no-card fallback list
#define WIFI_NET_CAP      32     // in-RAM cap — how many an SD card may provide
#define WIFI_SSID_MAX     33     // 32-char SSID + NUL
#define WIFI_PASS_MAX     64     // up to 63-char WPA2 passphrase + NUL
#define WIFI_NET_SCHEMA   1      // bump if WifiNet layout changes -> wipe+reseed
#define WIFI_CSV_PATH     "/wifi.csv"

struct WifiNet {
  char ssid[WIFI_SSID_MAX];
  char pass[WIFI_PASS_MAX];
};
/* The list lives in PSRAM, not static SRAM (~3 KB of .bss freed for the display's
 * partial render buffers). Allocated on first use; s_wifi_net_count stays 0 until
 * the alloc succeeds, so the count-guarded loops below never deref a NULL list. */
static WifiNet *s_wifi_nets = nullptr;     // [WIFI_NET_CAP]
static uint8_t s_wifi_net_count = 0;
static bool    s_wifi_sd        = false;   // true when the CSV backend (SD or FFat) is live
static bool wifi_nets_buf_ok(void) {
  if (!s_wifi_nets)
    s_wifi_nets = (WifiNet *)heap_caps_calloc(WIFI_NET_CAP, sizeof(WifiNet),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_wifi_nets)  // PSRAM missing/full (shouldn't happen) -> internal heap so WiFi still works
    s_wifi_nets = (WifiNet *)calloc(WIFI_NET_CAP, sizeof(WifiNet));
  return s_wifi_nets != nullptr;
}

/* ---- low-level: copy strings into a slot (always NUL-terminated) ---- */
static void wifi_net_set(WifiNet &n, const char *ssid, const char *pass) {
  strncpy(n.ssid, ssid ? ssid : "", WIFI_SSID_MAX - 1); n.ssid[WIFI_SSID_MAX - 1] = '\0';
  strncpy(n.pass, pass ? pass : "", WIFI_PASS_MAX - 1); n.pass[WIFI_PASS_MAX - 1] = '\0';
}

/* ---- flash (NVS) backing — the no-card fallback, capped at WIFI_NET_MAX ---- */
static void wifi_flash_save(void) {
  if (!wifi_nets_buf_ok()) return;   // never hand NVS a NULL blob pointer
  uint8_t cnt = s_wifi_net_count > WIFI_NET_MAX ? WIFI_NET_MAX : s_wifi_net_count;
  prefs.putUChar("wifiver", WIFI_NET_SCHEMA);
  prefs.putBytes("wifinets", s_wifi_nets, sizeof(WifiNet) * cnt);
  prefs.putUChar("wificnt", cnt);
}

static void wifi_flash_load(void) {
  if (!wifi_nets_buf_ok()) { s_wifi_net_count = 0; return; }
  // Schema guard: NVS survives a reflash, so reject a list from a different layout.
  if (prefs.getUChar("wifiver", 0) != WIFI_NET_SCHEMA) {
    prefs.remove("wifinets");
    prefs.remove("wificnt");
    s_wifi_net_count = 0;
  } else {
    s_wifi_net_count = prefs.getUChar("wificnt", 0);
    if (s_wifi_net_count > WIFI_NET_MAX) s_wifi_net_count = 0;
    size_t want = sizeof(WifiNet) * s_wifi_net_count;
    size_t got  = prefs.getBytes("wifinets", s_wifi_nets, want);
    if (got != want) s_wifi_net_count = 0;
  }
  for (uint8_t i = 0; i < s_wifi_net_count; i++) {  // harden against torn writes
    s_wifi_nets[i].ssid[WIFI_SSID_MAX - 1] = '\0';
    s_wifi_nets[i].pass[WIFI_PASS_MAX - 1] = '\0';
  }
}

/* ---- CSV backing — the source of truth. Lives on the SD card when present, else on
 * the on-flash FAT partition (store_fs picks). So the CSV is ALWAYS available now, not
 * only with a card. ---- */
static bool wifi_csv_save(void) {
  if (!store_available()) return false;
  File f = store_fs().open(WIFI_CSV_PATH, FILE_WRITE);   // FILE_WRITE truncates/overwrites
  if (!f) return false;
  f.println("# ESP32-S3-WatchFace saved WiFi networks");
  f.println("# one per line:  SSID,password   (split on the FIRST comma; the password");
  f.println("# may contain commas; blank lines and lines starting with # are ignored)");
  for (uint8_t i = 0; i < s_wifi_net_count; i++)
    f.printf("%s,%s\n", s_wifi_nets[i].ssid, s_wifi_nets[i].pass);
  f.close();
  return true;
}

/* Read /wifi.csv into the list. Returns the count loaded, or -1 if the file is
 * missing/unreadable (caller then falls back to flash). */
static int wifi_csv_load(void) {
  if (!wifi_nets_buf_ok()) return -1;
  if (!store_available() || !store_fs().exists(WIFI_CSV_PATH)) return -1;
  File f = store_fs().open(WIFI_CSV_PATH, FILE_READ);
  if (!f) return -1;
  uint8_t n = 0;
  while (f.available() && n < WIFI_NET_CAP) {
    String line = f.readStringUntil('\n');
    line.trim();                                    // strips \r and surrounding spaces
    if (line.length() == 0 || line[0] == '#') continue;
    int comma = line.indexOf(',');
    String ssid = (comma < 0) ? line : line.substring(0, comma);
    String pass = (comma < 0) ? String("") : line.substring(comma + 1);
    ssid.trim();                                    // SSID can't have leading/trailing spaces
    if (ssid.length() == 0) continue;
    wifi_net_set(s_wifi_nets[n], ssid.c_str(), pass.c_str());
    n++;
  }
  f.close();
  s_wifi_net_count = n;
  return n;
}

/* ---- public API (unchanged signatures; used by the WiFi & BLE screen) ---- */

/* Persist the current list to wherever it's backed: flash always (fallback), and
 * the CSV too when a card is present. */
static void wifi_nets_save(void) {
  wifi_flash_save();
  if (s_wifi_sd) wifi_csv_save();
}

/* Append a network (ignores duplicate SSIDs and a full list). Returns true if
 * added. The cap is the SD cap when card-backed, else the flash cap. Caller
 * persists via wifi_nets_save(). */
static bool wifi_nets_add(const char *ssid, const char *pass) {
  if (!ssid || !ssid[0] || !wifi_nets_buf_ok()) return false;
  for (uint8_t i = 0; i < s_wifi_net_count; i++)
    if (strncmp(s_wifi_nets[i].ssid, ssid, WIFI_SSID_MAX) == 0) return false;
  uint8_t cap = s_wifi_sd ? WIFI_NET_CAP : WIFI_NET_MAX;
  if (s_wifi_net_count >= cap) return false;
  wifi_net_set(s_wifi_nets[s_wifi_net_count], ssid, pass);
  s_wifi_net_count++;
  return true;
}

/* Forget the network at index i (compacts the list) and persists everywhere. */
static void wifi_nets_remove(uint8_t i) {
  if (i >= s_wifi_net_count) return;
  for (uint8_t j = i; j + 1 < s_wifi_net_count; j++) s_wifi_nets[j] = s_wifi_nets[j + 1];
  s_wifi_net_count--;
  wifi_nets_save();
}

/* Seed the compile-time default network if the list ended up empty. */
static void wifi_seed_default_if_empty(void) {
  if (s_wifi_net_count == 0 && WIFI_SSID[0]) {
    wifi_nets_add(WIFI_SSID, WIFI_PASS);
    wifi_nets_save();
  }
}

/* Load the network list. The CSV (on SD if present, else the on-flash FAT partition)
 * is the source of truth; NVS flash is a redundant fast fallback. Called once at boot.
 *   CSV exists       -> use it (any count); refresh the flash fallback
 *   backend, no CSV  -> load flash, then write it out as a fresh CSV
 *   no backend at all -> flash only (shouldn't happen: FFat always mounts) */
static void wifi_nets_load(void) {
  s_wifi_sd = false;

  if (store_available()) {
    const char *where = store_on_sd() ? "SD card" : "flash (FFat)";
    int n = wifi_csv_load();                 // CSV is the source of truth if present
    if (n >= 0) {
      s_wifi_sd = true;
      wifi_seed_default_if_empty();          // empty CSV -> at least the home network
      wifi_flash_save();                     // mirror the first few back to flash (wake fallback)
      USBSerial.printf("[wifi] loaded %u network(s) from %s on %s\n",
                       s_wifi_net_count, WIFI_CSV_PATH, where);
      return;
    }
    // Backend present but no CSV yet: take what's in flash and write the CSV.
    wifi_flash_load();
    s_wifi_sd = true;
    wifi_seed_default_if_empty();
    wifi_csv_save();
    USBSerial.printf("[wifi] no %s -> wrote %u flash network(s) to %s\n",
                     WIFI_CSV_PATH, s_wifi_net_count, where);
    return;
  }

  // No backend at all (FFat failed to mount): classic flash-only list.
  wifi_flash_load();
  wifi_seed_default_if_empty();
  USBSerial.printf("[wifi] no storage -> using %u flash network(s)\n", s_wifi_net_count);
}
