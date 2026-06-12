/* ============================================================================
 *  ble_gadgetbridge.h — Android notifications via Gadgetbridge (Bangle.js protocol).
 *
 *  Android has no ANCS equivalent: notifications are only visible to an APP on the
 *  phone, so something there must forward them. Gadgetbridge (free, open source,
 *  F-Droid) already does that and speaks the Bangle.js protocol: a Nordic UART
 *  Service (NUS) on the watch, to which the phone writes newline-terminated JSON
 *  lines, optionally wrapped in a JS call:
 *
 *      \x10GB({"t":"notify","id":1675,"src":"WhatsApp","title":"Alice","body":"Hi"})\n
 *      \x10GB({"t":"notify-","id":1675})\n                  (dismissed on phone)
 *      \x10GB({"t":"find","n":true})\n                      (find-my-watch ring)
 *      \x10setTime(1749571200);E.setTimeZone(2.0);...\n     (time sync, raw JS)
 *
 *  We are the GATT SERVER here (like ble_provision.h, unlike the ANCS client): the
 *  NUS service is created in ble_begin(); its RX write callback feeds gb_rx_bytes()
 *  below. Writes arrive chunked at the ATT MTU, so bytes are reassembled into a
 *  line buffer (PSRAM) and each complete '\n'-terminated line is parsed here.
 *
 *  STORE / UI HANDOFF — identical to ANCS on purpose: notifications go through
 *  notif_store_add() + na_append() under store_lock, then stash s_pop_* and raise
 *  s_ancs_ui_dirty / s_ancs_removed / s_ancs_added_total. The loop (core 1) already
 *  consumes those flags to pop the card and refresh the bell, so an Android phone
 *  needs ZERO changes in the .ino UI path. (The flags keep their "ancs" names; they
 *  mean "a BLE notification source changed the store".)
 *
 *  IDS / DEDUP: Gadgetbridge ids are its own 32-bit counters, mapped into a high id
 *  space (GB_ID_BASE | id) so they never collide with HTTP epoch-millis ids or
 *  ANCS_ID_BASE ids. Dismissals ("notify-") carry only the id, so the mapping must
 *  stay reconstructible from the id alone — don't mix anything else into it.
 *
 *  TIME SYNC: Gadgetbridge pushes setTime() (UTC epoch) + E.setTimeZone(hours) on
 *  connect, so the RTC stays synced from the phone with WiFi off. Writes the
 *  PCF85063 under i2c_lock and refreshes rtc_last_ntp_epoch so a later WiFi fetch
 *  won't redundantly re-sync.
 *
 *  THREADING: every entry point here runs on the NimBLE host task (core 0). It
 *  NEVER touches LVGL — store writes happen under store_lock, UI work is flagged
 *  to the loop, exactly like ble_ancs.h.
 *
 *  INCLUDE AFTER ble_ancs.h (uses its handoff flags) — which already guarantees
 *  notif_store.h / notif_archive_sd.h / notif_net.h (notif_store_add, na_*,
 *  s_pop_*, json_find_string, store_lock) are in. Header-only, in the .ino TU.
 *
 *  PHONE SETUP: Gadgetbridge detects a Bangle.js by its advertised NAME. Either
 *  long-press the device in Gadgetbridge's discovery list and pick "Bangle.js" as
 *  the device type, or (if your Gadgetbridge build lacks that) advertise a name
 *  starting with "Bangle.js".
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <time.h>

/* Map a Gadgetbridge 32-bit id into the store's 64-bit id space, distinct from
 * both HTTP ids (~1e12 epoch millis) and ANCS_ID_BASE (0x5...). */
#define GB_ID_BASE  ((uint64_t)0x6000000000000000ULL)

/* Line reassembly buffer. Gadgetbridge bodies are capped by its own settings
 * (typically well under 1KB per line); an over-long line is dropped whole. */
#define GB_LINE_MAX 1024

static char    *s_gb_line     = nullptr;   // [GB_LINE_MAX], PSRAM, lazy
static uint16_t s_gb_len      = 0;
static bool     s_gb_overflow = false;     // drop bytes until the next '\n'
static BLECharacteristic *s_gb_tx = nullptr;   // NUS TX (watch -> phone), set by ble_begin

static bool gb_buf_ok(void) {
  if (!s_gb_line)
    s_gb_line = (char *)heap_caps_malloc(GB_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_gb_line)
    s_gb_line = (char *)malloc(GB_LINE_MAX);
  return s_gb_line != nullptr;
}

/* ---------------------------- tiny JSON helpers --------------------------- */
/* json_find_string() (notif_net.h) covers string values; this covers numbers.
 * Finds "key":<int> and parses it. Same not-a-real-parser caveat: fine for the
 * fixed, flat Gadgetbridge schema. */
static bool gb_json_u32(const String &src, const char *key, uint32_t *out) {
  String pat = String("\"") + key + "\":";
  int k = src.indexOf(pat);
  if (k < 0) return false;
  k += pat.length();
  while (k < (int)src.length() && src[k] == ' ') k++;
  bool neg = (k < (int)src.length() && src[k] == '-');
  if (neg) k++;
  uint64_t v = 0;
  bool any = false;
  while (k < (int)src.length() && src[k] >= '0' && src[k] <= '9') {
    v = v * 10 + (src[k] - '0');
    k++; any = true;
  }
  if (!any) return false;
  *out = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
  return true;
}

/* --------------------------- message handlers ----------------------------- */

/* {"t":"notify",...} — a new (or updated) phone notification. Store + popup,
 * mirroring ancs_parse_and_store()'s tail exactly. */
static void gb_handle_notify(const String &js) {
  uint32_t nid = 0;
  gb_json_u32(js, "id", &nid);

  String src, title, body;
  json_find_string(js, 0, "src", src);       // app name, e.g. "WhatsApp"
  json_find_string(js, 0, "title", title);   // sender / headline
  json_find_string(js, 0, "body", body);
  if (title.length() == 0) json_find_string(js, 0, "subject", title);
  if (title.length() == 0) title = src;
  if (title.length() == 0 && body.length() == 0) return;   // nothing usable

  // No id (shouldn't happen) -> fall back to a content hash so dedup still keys
  // on something stable rather than colliding everything onto GB_ID_BASE|0.
  if (nid == 0) {
    uint32_t h = 2166136261u;                // FNV-1a over title+body
    for (size_t i = 0; i < (size_t)title.length(); i++) { h ^= (uint8_t)title[i]; h *= 16777619u; }
    for (size_t i = 0; i < (size_t)body.length();  i++) { h ^= (uint8_t)body[i];  h *= 16777619u; }
    nid = h ? h : 1;
  }
  uint64_t id  = GB_ID_BASE | (uint64_t)nid;
  uint8_t  cat = notif_cat_from_appid(src.c_str());   // matches friendly names too

  store_lock();
  bool added = notif_store_add(id, title.c_str(), body.c_str(), cat);
  if (added && na_available()) na_append(id, title.c_str(), body.c_str(), cat);
  if (added) {
    notif_store_save();
    s_pop_id = id;
    strncpy(s_pop_title, title.c_str(), sizeof(s_pop_title) - 1);
    s_pop_title[sizeof(s_pop_title) - 1] = '\0';
    strncpy(s_pop_body, body.c_str(), sizeof(s_pop_body) - 1);
    s_pop_body[sizeof(s_pop_body) - 1] = '\0';
    s_pop_have = true;
    s_ancs_ui_dirty = true;       // loop: pop the card + refresh the bell
    s_ancs_added_total++;         // timer-wake background check parity
  }
  store_unlock();

  USBSerial.printf("[gb] %s id=%lu src=\"%s\" title=\"%s\"\n",
                   added ? "stored" : "dup", (unsigned long)nid, src.c_str(), title.c_str());
}

/* {"t":"notify-","id":N} — dismissed on the phone: mirror the removal, like
 * ancs_remove_uid(). The id alone reconstructs our store id (see GB_ID_BASE). */
static void gb_handle_dismiss(const String &js) {
  uint32_t nid = 0;
  if (!gb_json_u32(js, "id", &nid)) return;
  uint64_t id = GB_ID_BASE | (uint64_t)nid;
  store_lock();
  bool hit = false;
  if (na_available() && na_remove(id)) hit = true;
  if (notif_store_remove_by_id(id))    hit = true;
  store_unlock();
  if (hit) {
    s_ancs_removed = true;        // loop refreshes the bell + any open list
    USBSerial.printf("[gb] removed id=%lu (cleared on phone)\n", (unsigned long)nid);
  }
}

/* ---------------- music: Player app integration (player_state.h) ---------------- */

/* Transport command from the Player UI -> Gadgetbridge -> the phone's active media
 * session (controls whatever app is playing). Runs on the loop thread; NimBLE host
 * APIs are task-safe, same as ble_ping_phone() notifying from the UI. */
static void gb_player_cmd(PlayerCmd cmd) {
  switch (cmd) {
    case PCMD_TOGGLE: gb_send("{\"t\":\"music\",\"n\":\"playpause\"}"); break;
    case PCMD_NEXT:   gb_send("{\"t\":\"music\",\"n\":\"next\"}");      break;
    case PCMD_PREV:   gb_send("{\"t\":\"music\",\"n\":\"previous\"}");  break;
  }
}

/* {"t":"musicinfo","artist":"..","album":"..","track":"..",...} — now-playing
 * metadata. Receiving it makes us the active player source (last-active-wins,
 * same as AMS taking over when the iPhone plays). No sync callback: Gadgetbridge
 * pushes musicstate on every change, there is nothing to poll. */
static void gb_handle_musicinfo(const String &js) {
  String artist, album, track;
  json_find_string(js, 0, "artist", artist);
  json_find_string(js, 0, "album",  album);
  json_find_string(js, 0, "track",  track);
  player_set_sink(PSRC_ANDROID, gb_player_cmd, nullptr);
  player_set_track(track.c_str(), artist.c_str(), album.c_str());
}

/* {"t":"musicstate","state":"play"|"pause",...} — playback state. */
static void gb_handle_musicstate(const String &js) {
  String st;
  if (!json_find_string(js, 0, "state", st)) return;
  player_set_sink(PSRC_ANDROID, gb_player_cmd, nullptr);
  if      (st == "play")  player_set_state(PLAYING);
  else if (st == "pause") player_set_state(PLAY_PAUSED);
  else                    player_set_state(PLAY_STOPPED);
}

/* setTime(EPOCH);E.setTimeZone(H);... — Gadgetbridge time sync (raw JS, sent on
 * connect). Epoch is UTC; the RTC holds LOCAL wall-clock time, so apply the
 * timezone offset before writing the PCF85063. */
static void gb_handle_settime(const char *line) {
  const char *p = strstr(line, "setTime(");
  uint32_t utc = (uint32_t)strtoul(p + 8, nullptr, 10);
  if (utc < 1600000000UL) return;            // garbage / pre-2020 -> ignore
  float tzh = 0.0f;
  const char *z = strstr(line, "setTimeZone(");
  if (z) tzh = strtof(z + 12, nullptr);
  time_t local = (time_t)utc + (time_t)(tzh * 3600.0f);
  struct tm tmv;
  gmtime_r(&local, &tmv);                    // offset already applied -> use gmtime

  i2c_lock();                                // shared bus: UI reads touch on core 1
  rtc.setDateTime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  i2c_unlock();
  rtc_last_ntp_epoch = (uint32_t)local;      // suppress a redundant NTP re-sync
  USBSerial.printf("[gb] time sync: %04d-%02d-%02d %02d:%02d:%02d (tz %+.1f)\n",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (double)tzh);
}

/* One complete '\n'-terminated line from the phone. */
static void gb_handle_line(char *line) {
  // Strip leading control bytes (Gadgetbridge prefixes \x10) and whitespace.
  while (*line && (uint8_t)*line <= ' ') line++;
  if (!*line) return;

  if (strstr(line, "setTime(")) { gb_handle_settime(line); return; }

  const char *o = strchr(line, '{');         // tolerate the GB( ... ) wrapper
  if (!o) return;
  String js(o);

  String t;
  if (!json_find_string(js, 0, "t", t)) return;

  if      (t == "notify")     gb_handle_notify(js);
  else if (t == "notify-")    gb_handle_dismiss(js);
  else if (t == "musicinfo")  gb_handle_musicinfo(js);
  else if (t == "musicstate") gb_handle_musicstate(js);
  else if (t == "find") {
    // {"t":"find","n":true} rings the watch — same path as the find-watch write.
    if (js.indexOf("\"n\":true") >= 0) s_ble_findwatch_req = true;
  }
  // TODO: "call" (live incoming-call screen, like the ANCS s_incoming_* seam),
  //       "weather".
  else USBSerial.printf("[gb] ignored t=\"%s\"\n", t.c_str());
}

/* ----------------------------- public entry ------------------------------- */

/* NUS RX bytes (NimBLE task): reassemble into lines; writes arrive chunked at the
 * ATT MTU, and one write may also carry several short lines. */
static void gb_rx_bytes(const uint8_t *data, uint16_t len) {
  if (len == 0 || !gb_buf_ok()) return;
  for (uint16_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n') {
      if (!s_gb_overflow) { s_gb_line[s_gb_len] = '\0'; gb_handle_line(s_gb_line); }
      s_gb_len = 0; s_gb_overflow = false;
      continue;
    }
    if (s_gb_overflow) continue;             // dropping an over-long line
    if (s_gb_len < GB_LINE_MAX - 1) s_gb_line[s_gb_len++] = c;
    else { s_gb_len = 0; s_gb_overflow = true; }
  }
}

/* Drop any half-received line (call on disconnect / BLE down). Also clears the
 * Player if WE were its active source — the phone is gone, the track is stale.
 * (player_clear is src-guarded, but gate here too so an AMS-owned track isn't
 * wiped by an Android phone that merely connected and left.) */
static void gb_reset(void) {
  s_gb_len = 0;
  s_gb_overflow = false;
  if (s_play_src == PSRC_ANDROID) player_clear(PSRC_ANDROID);
}

/* ble_begin/ble_end hand us the NUS TX characteristic (nullptr when down). */
static void gb_set_tx(BLECharacteristic *tx) { s_gb_tx = tx; }

/* Send one JSON line watch -> phone (Gadgetbridge expects '\n'-terminated JSON,
 * e.g. {"t":"status","bat":85,"chg":0} or {"t":"findPhone","n":true}). For the
 * future: battery reporting, find-my-phone, music controls. */
static bool gb_send(const char *json) {
  if (!s_gb_tx || !s_ble_connected) return false;
  // MUST end "\r\n", not "\n": Gadgetbridge's line splitter assumes Espruino's
  // println() framing and strips the byte BEFORE the '\n' as a presumed '\r'.
  // With a bare '\n' that byte is our closing '}' -> "Malformed JSON" toast.
  String v = String(json) + "\r\n";
  // CHUNK AT 20 BYTES: a notification carries at most ATT_MTU-3 bytes and the
  // default MTU is 23, so a longer value gets silently TRUNCATED — the '\n' never
  // arrives and Gadgetbridge waits forever for the rest of the line. Real Bangle.js
  // firmware chunks exactly like this; Gadgetbridge reassembles until the newline,
  // so 20-byte chunks are safe regardless of the negotiated MTU.
  const uint8_t *p = (const uint8_t *)v.c_str();
  size_t left = v.length();
  while (left) {
    size_t n = left > 20 ? 20 : left;
    s_gb_tx->setValue((uint8_t *)p, n);
    s_gb_tx->notify();
    p += n; left -= n;
  }
  USBSerial.printf("[gb] tx: %s", v.c_str());   // v ends in '\n'
  return true;
}
