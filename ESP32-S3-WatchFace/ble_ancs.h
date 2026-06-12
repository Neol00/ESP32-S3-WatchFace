/* ============================================================================
 *  ble_ancs.h — ANCS (Apple Notification Center Service) CLIENT for iPhone.
 *
 *  Reads the iPhone's notification stream over the existing bonded BLE link and
 *  feeds it into the SAME notification store the HTTP fetch path uses. No iOS app
 *  is required — ANCS is built into iOS and exposed to any bonded accessory.
 *
 *  HOW IT WORKS (we are the GATT CLIENT here — the inverse of the rest of our BLE
 *  code, which is a server):
 *    1. On an encrypted+bonded link, discover the ANCS service on the phone and its
 *       three characteristics:
 *         - Notification Source (notify): 8-byte events  EventID/Flags/Cat/Count/UID
 *         - Control Point      (write):   we ask "give me attributes for UID X"
 *         - Data Source        (notify):  iOS streams the attribute reply back
 *    2. Subscribe to Notification Source + Data Source (write their CCCDs).
 *    3. For each ADDED event, write a "Get Notification Attributes" request to the
 *       Control Point asking for AppIdentifier + Title + Message.
 *    4. Reassemble the Data Source reply (it can span multiple GATT packets), parse
 *       the attributes, and push {title, body} into the notification store.
 *
 *  DELAYED DELIVERY / DEEP SLEEP: on every fresh subscribe iOS REPLAYS the
 *  notifications currently in the phone's Notification Center as a burst of ADDED
 *  events. So after a deep-sleep wake + reconnect we receive everything still
 *  pending on the phone (not just what arrives while connected). Cleared-on-phone
 *  notifications are correctly absent.
 *
 *  THREADING: every callback here runs on the NimBLE host task (core 0), like the
 *  net task — it NEVER touches LVGL. It writes the store under store_lock and sets
 *  s_pop_* + s_ancs_ui_dirty; the loop (core 1) shows the popup and refreshes the
 *  bell, exactly as it does for the HTTP fetch result.
 *
 *  DEDUP: ANCS UIDs are per-connection 32-bit ids; the HTTP path uses large
 *  millis-epoch ids. To keep both in one store without collision we map an ANCS UID
 *  into a high id space (ANCS_ID_BASE | uid). notif_store_add() de-dups by id, so a
 *  replayed backlog item already in the store is ignored.
 *
 *  INCLUDE AFTER ble_provision.h is set up to call ancs_on_encrypted/ancs_reset, and
 *  AFTER notif_store.h / notif_archive_sd.h / notif_net.h (uses notif_store_add,
 *  na_available/na_append, s_pop_*, store_lock). Header-only, in the .ino TU.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <host/ble_hs.h>      // ble_gattc_*, ble_uuid*, os_mbuf (pulls in ble_gattc.h)
#include <host/ble_gap.h>

/* ---- ANCS UUIDs (Apple-defined, 128-bit), stored LSB-first for NimBLE ---- */
static const ble_uuid128_t ANCS_SVC_UUID = BLE_UUID128_INIT(   // 7905F431-B5CE-4E99-A40F-4B1E122D00D0
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
static const ble_uuid128_t ANCS_NOTIF_SRC_UUID = BLE_UUID128_INIT( // 9FBF120D-6301-42D9-8C58-25E699A21DBD
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C,
    0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
static const ble_uuid128_t ANCS_CTRL_PT_UUID = BLE_UUID128_INIT(   // 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98,
    0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
static const ble_uuid128_t ANCS_DATA_SRC_UUID = BLE_UUID128_INIT(  // 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE,
    0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);

/* ANCS protocol constants */
#define ANCS_CMD_GET_NOTIF_ATTRS   0x00   // Control Point command: fetch attributes
#define ANCS_CMD_PERFORM_ACTION    0x02   // Control Point command: act on a notification
#define ANCS_ACTION_POSITIVE       0x00   // e.g. "view"/accept
#define ANCS_ACTION_NEGATIVE       0x01   // dismiss / clear (what a watch swipe sends)
#define ANCS_EVENT_ADDED           0x00
#define ANCS_EVENT_MODIFIED        0x01
#define ANCS_EVENT_REMOVED         0x02
#define ANCS_EVENTFLAG_PREEXISTING 0x04   // bit set on backlog-replay items

/* ANCS CategoryID (8-byte NS event byte [2]). Apple-defined. We only special-case
 * the two CALL categories; everything else is treated generically (category for the
 * STORED notification still comes from the app id, see notif_cat_from_appid).
 *
 * WHY THIS MATTERS — the "duplicate missed call" bug: a single missed call produces
 * TWO ANCS notifications from iOS. First an INCOMING_CALL (active/transient: "X is
 * calling…"), which iOS REMOVES when the call ends, then a brand-new MISSED_CALL.
 * If we store the incoming-call like any other notification it lingers as a second
 * entry alongside the missed-call → two cards for one missed call. So we treat
 * INCOMING_CALL as a LIVE, transient event (never written to the store; auto-cleared
 * on its REMOVED) and let only MISSED_CALL become the single stored notification.
 * This split is also the seam for a future live "incoming call" screen + answer/
 * decline (ANCS PerformAction) — the incoming-call UID is tracked in s_incoming_*. */
#define ANCS_CAT_OTHER             0
#define ANCS_CAT_INCOMING_CALL     1
#define ANCS_CAT_MISSED_CALL       2
#define ANCS_CAT_VOICEMAIL         3
#define ANCS_ATTR_APP_ID           0x00
#define ANCS_ATTR_TITLE            0x01
#define ANCS_ATTR_MESSAGE          0x03
#define ANCS_ATTR_DATE             0x05   // notification timestamp "yyyyMMdd'T'HHmmSS"
#define ANCS_ATTR_TITLE_MAXLEN     48     // ask iOS to cap title length
#define ANCS_ATTR_MSG_MAXLEN       220    // ...and message length (fits NOTIF_BODY_MAX)
#define ANCS_ATTR_DATE_LEN         16     // fixed-format date string length (no NUL)

/* Map an ANCS 32-bit UID into the store's 64-bit id space, high above HTTP ids
 * (which are epoch-millis, ~1e12), so the two sources never collide. */
#define ANCS_ID_BASE  ((uint64_t)0x5000000000000000ULL)

/* ---- per-connection discovered handles + state (reset on disconnect) ---- */
static uint16_t s_ancs_conn       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ancs_svc_start  = 0;
static uint16_t s_ancs_svc_end    = 0;
static uint16_t s_ancs_ns_val     = 0;    // Notification Source value handle
static uint16_t s_ancs_ns_cccd    = 0;
static uint16_t s_ancs_cp_val     = 0;    // Control Point value handle (we write requests here)
static uint16_t s_ancs_ds_val     = 0;    // Data Source value handle (replies notify here)
static uint16_t s_ancs_ds_cccd    = 0;
static bool     s_ancs_ns_subbed  = false;
static bool     s_ancs_ds_subbed  = false;

/* One-shot flag: ANCS pushed a new notification into the store; the loop should pop
 * the s_pop_* card + refresh the bell. Mirrors the net task's result handoff. */
static volatile bool s_ancs_ui_dirty = false;

/* One-shot flag: ANCS removed a notification (cleared on the phone); the loop should
 * refresh the bell count and, if the notifications app is open, rebuild its list. */
static volatile bool s_ancs_removed = false;

/* Monotonic count of notifications ANCS has added to the store this power-cycle.
 * The timer-wake background check (sleep_power.h) snapshots this, brings up BLE,
 * waits, and full-boots if it grew — i.e. the iPhone delivered something. The loop
 * doesn't run during that check, so this counter (not s_ancs_ui_dirty) is the
 * source of truth there. */
static volatile uint32_t s_ancs_added_total = 0;

/* ---- Data Source reassembly buffer ----
 * A single attribute reply may arrive split across several GATT notifications. We
 * accumulate bytes here until we've parsed a complete record, then reset. One reply
 * at a time is the norm (we request serially), so a single buffer is sufficient. */
#define ANCS_DS_BUF_MAX  600
static uint8_t  s_ds_buf[ANCS_DS_BUF_MAX];
static uint16_t s_ds_len = 0;

/* ANCS CategoryID of the attribute request currently in flight. We fetch one
 * notification's attributes at a time (serial Control Point round-trips), so a single
 * value tracks "what category is the reply we're reassembling". Set in
 * ancs_request_attrs, read in ancs_parse_and_store to decide store vs. live-call. */
static uint8_t s_inflight_cat = ANCS_CAT_OTHER;

/* ---- pending attribute-request queue ----
 * iOS replays the whole notification backlog as ONE burst of ADDED events right
 * after the NS subscribe. Firing a Control Point write per event as it arrives
 * exhausts NimBLE's few per-connection GATT procedure slots — every write past the
 * first few fails rc=6 (BLE_HS_ENOMEM) and those notifications are silently LOST.
 * So ADDED events are queued here and pumped strictly one-in-flight: the next
 * request goes out when the previous reply has been fully parsed. */
#define ANCS_REQQ_MAX 32
static uint32_t s_reqq_uid[ANCS_REQQ_MAX];
static uint8_t  s_reqq_cat[ANCS_REQQ_MAX];
static uint8_t  s_reqq_head = 0, s_reqq_count = 0;   // ring (head = next to send)
static bool     s_req_inflight   = false;
static uint32_t s_req_started_ms = 0;                // stale-reply watchdog
static void ancs_req_complete(void);                 // defined after the pump

/* ---- content fingerprint (durable de-dup key) ----
 * FNV-1a 32-bit hash of app + '\x1f' + date + '\x1f' + title. Stable for a given
 * notification across reconnects/power-off (unlike the ANCS UID), so the same item
 * isn't re-added after every sleep/wake. The store id is ANCS_ID_BASE | fingerprint. */
static uint32_t ancs_fnv1a(uint32_t h, const char *s) {
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}
static uint32_t ancs_fingerprint(const char *app, const char *date, const char *title) {
  uint32_t h = 2166136261u;                  // FNV offset basis
  h = ancs_fnv1a(h, app);   h = ancs_fnv1a(h, "\x1f");
  h = ancs_fnv1a(h, date);  h = ancs_fnv1a(h, "\x1f");
  h = ancs_fnv1a(h, title);
  return h;
}

/* ---- session UID -> store-id map ----
 * The store id is now content-derived, but a REMOVED event carries only the live
 * UID. This small ring remembers, for the CURRENT connection, which store id each
 * UID mapped to, so a removal can find the right entry. Per-session (UIDs are only
 * valid within a connection); cleared on disconnect. */
#define ANCS_UIDMAP_MAX  64
static uint32_t s_uidmap_uid[ANCS_UIDMAP_MAX];
static uint64_t s_uidmap_id[ANCS_UIDMAP_MAX];
static uint8_t  s_uidmap_head = 0;   // next slot to overwrite (ring)
static uint8_t  s_uidmap_count = 0;

static void ancs_uidmap_put(uint32_t uid, uint64_t id) {
  for (uint8_t i = 0; i < s_uidmap_count; i++)        // update if UID already mapped
    if (s_uidmap_uid[i] == uid) { s_uidmap_id[i] = id; return; }
  s_uidmap_uid[s_uidmap_head] = uid;
  s_uidmap_id[s_uidmap_head]  = id;
  s_uidmap_head = (s_uidmap_head + 1) % ANCS_UIDMAP_MAX;
  if (s_uidmap_count < ANCS_UIDMAP_MAX) s_uidmap_count++;
}
/* Resolve a UID to its store id; returns false if we never saw it this session. */
static bool ancs_uidmap_get(uint32_t uid, uint64_t *out) {
  for (uint8_t i = 0; i < s_uidmap_count; i++)
    if (s_uidmap_uid[i] == uid) { *out = s_uidmap_id[i]; return true; }
  return false;
}
/* Reverse: resolve a store id to its live UID (for dismissing on the phone). Only
 * works for notifications iOS told us about THIS connection — a store entry carried
 * over from a previous session has no live UID until iOS re-sends it. */
static bool ancs_uidmap_get_uid(uint64_t id, uint32_t *out) {
  for (uint8_t i = 0; i < s_uidmap_count; i++)
    if (s_uidmap_id[i] == id) { *out = s_uidmap_uid[i]; return true; }
  return false;
}

/* ---- live incoming-call state (transient; never written to the store) ----
 * Set when iOS ADDs an INCOMING_CALL notification, cleared when it REMOVEs that UID
 * (call answered/declined/ended). This is the hook a future live "incoming call"
 * screen + answer/decline hangs off; for now it just keeps the incoming call OUT of
 * the stored list so a missed call stays a single entry. s_incoming_dirty pulses the
 * loop when the live state changes (call started or ended). */
static volatile bool     s_incoming_active = false;            // a call is ringing now
static volatile uint32_t s_incoming_uid    = 0;                // its live ANCS UID
static char              s_incoming_who[NOTIF_TITLE_MAX] = ""; // caller (title), for the live UI
static volatile bool     s_incoming_dirty  = false;            // loop should refresh call UI

static void ancs_reset(void) {
  s_ancs_conn      = BLE_HS_CONN_HANDLE_NONE;
  s_ancs_svc_start = 0; s_ancs_svc_end = 0;
  s_ancs_ns_val = 0; s_ancs_ns_cccd = 0;
  s_ancs_cp_val = 0;
  s_ancs_ds_val = 0; s_ancs_ds_cccd = 0;
  s_ancs_ns_subbed = false; s_ancs_ds_subbed = false;
  s_ds_len = 0;
  s_uidmap_head = 0; s_uidmap_count = 0;   // UIDs are per-connection
  s_reqq_head = 0; s_reqq_count = 0;       // ...and so are queued attribute requests
  s_req_inflight = false;
  // A ringing call can't survive a disconnect; drop any live incoming-call state so a
  // reconnect doesn't show a stale "incoming call".
  if (s_incoming_active) { s_incoming_active = false; s_incoming_uid = 0; s_incoming_dirty = true; }
}

/* ===================== attribute reply parsing ============================ */

/* Parse a COMPLETE "Get Notification Attributes" response in s_ds_buf and, if it
 * carries a title/message, push it into the notification store. Layout:
 *   [CommandID=0][UID:4 LE][ {AttrID:1}{Len:2 LE}{Data:Len} ... ]
 * Runs on the NimBLE task -> guards the store, never touches LVGL. */
static void ancs_parse_and_store(void) {
  if (s_ds_len < 5 || s_ds_buf[0] != ANCS_CMD_GET_NOTIF_ATTRS) {
    s_ds_len = 0;
    ancs_req_complete();           // malformed reply still frees the request slot
    return;
  }
  uint32_t uid = (uint32_t)s_ds_buf[1] | ((uint32_t)s_ds_buf[2] << 8) |
                 ((uint32_t)s_ds_buf[3] << 16) | ((uint32_t)s_ds_buf[4] << 24);

  char title[NOTIF_TITLE_MAX] = "";
  char body[NOTIF_BODY_MAX]   = "";
  char app[40]                = "";
  char date[ANCS_ATTR_DATE_LEN + 1] = "";

  uint16_t p = 5;
  while (p + 3 <= s_ds_len) {
    uint8_t  attr = s_ds_buf[p];
    uint16_t alen = (uint16_t)s_ds_buf[p + 1] | ((uint16_t)s_ds_buf[p + 2] << 8);
    p += 3;
    if (p + alen > s_ds_len) return;          // attribute spans past what we have -> wait for more
    const char *src = (const char *)&s_ds_buf[p];
    switch (attr) {
      case ANCS_ATTR_APP_ID: {
        uint16_t n = alen < sizeof(app) - 1 ? alen : sizeof(app) - 1;
        memcpy(app, src, n); app[n] = '\0';
        break;
      }
      case ANCS_ATTR_TITLE: {
        uint16_t n = alen < NOTIF_TITLE_MAX - 1 ? alen : NOTIF_TITLE_MAX - 1;
        memcpy(title, src, n); title[n] = '\0';
        break;
      }
      case ANCS_ATTR_MESSAGE: {
        uint16_t n = alen < NOTIF_BODY_MAX - 1 ? alen : NOTIF_BODY_MAX - 1;
        memcpy(body, src, n); body[n] = '\0';
        break;
      }
      case ANCS_ATTR_DATE: {
        uint16_t n = alen < ANCS_ATTR_DATE_LEN ? alen : ANCS_ATTR_DATE_LEN;
        memcpy(date, src, n); date[n] = '\0';
        break;
      }
      default: break;
    }
    p += alen;
  }

  // We have a full record. A title is the minimum worth showing; fall back to the
  // app id if iOS sent an empty title (some notifications have only a body).
  if (title[0] == '\0' && app[0] != '\0') {
    strncpy(title, app, NOTIF_TITLE_MAX - 1);
    title[NOTIF_TITLE_MAX - 1] = '\0';
  }
  if (title[0] == '\0' && body[0] == '\0') {                          // nothing usable
    s_ds_len = 0;
    ancs_req_complete();
    return;
  }

  // INCOMING CALL: transient — do NOT write it to the notification store (that's what
  // caused the duplicate: the incoming call lingered next to the later missed call).
  // Track it as live ringing state instead; the loop can show a call screen and, later,
  // answer/decline via ancs_dismiss_id (PerformAction). It self-clears on the REMOVED
  // event (call ended), and iOS sends a separate MISSED_CALL ADDED if it goes unanswered
  // — that one is stored below like any other notification, as the single entry.
  if (s_inflight_cat == ANCS_CAT_INCOMING_CALL) {
    s_incoming_uid = uid;
    strncpy(s_incoming_who, title, sizeof(s_incoming_who) - 1);
    s_incoming_who[sizeof(s_incoming_who) - 1] = '\0';
    ancs_uidmap_put(uid, ANCS_ID_BASE | (uint64_t)uid);  // so answer/decline can find the live UID
    s_incoming_active = true;
    s_incoming_dirty  = true;
    USBSerial.printf("[ancs] incoming call from \"%s\" (uid=%lu) — transient, not stored\n",
                     s_incoming_who, (unsigned long)uid);
    s_ds_len = 0;
    ancs_req_complete();
    return;
  }

  // DURABLE id: the ANCS UID changes across reconnects, so the SAME notification
  // would be re-added after every sleep/wake if we keyed on the UID. Instead derive
  // the store id from a content fingerprint (app + date + title) that is stable for a
  // given notification across reconnects AND power-off. The store/archive/removal
  // paths all key on this id, so the existing de-dup just works across sessions.
  uint32_t fp = ancs_fingerprint(app, date, title);
  uint64_t id = ANCS_ID_BASE | (uint64_t)fp;
  ancs_uidmap_put(uid, id);        // remember UID->id for this session, so a REMOVED
                                   // (which carries only the UID) can find the entry.
  uint8_t cat = notif_cat_from_appid(app);   // per-app icon/layout (call/message/...)

  store_lock();
  // Add to the NVS cache FIRST: it de-dups by id (now a durable content fingerprint),
  // so its result tells us whether this notification is genuinely NEW. Only then do we
  // append to the archive — na_append() is append-only with NO de-dup of its own, so
  // gating it here is what stops the archive accumulating a duplicate line for every
  // backlog replay after each sleep/wake.
  bool added = notif_store_add(id, title, body, cat);
  if (added && na_available()) na_append(id, title, body, cat);   // full history (SD or FFat)
  if (added) {
    notif_store_save();
    // Stash newest for the popup card, same as the HTTP path does.
    s_pop_id = id;
    strncpy(s_pop_title, title, sizeof(s_pop_title) - 1);
    s_pop_title[sizeof(s_pop_title) - 1] = '\0';
    strncpy(s_pop_body, body, sizeof(s_pop_body) - 1);
    s_pop_body[sizeof(s_pop_body) - 1] = '\0';
    s_pop_have = true;
    s_ancs_ui_dirty = true;       // tell the loop to pop the card + refresh the bell
    s_ancs_added_total++;         // tell the timer-wake background check something arrived
  }
  store_unlock();

  if (added)
    USBSerial.printf("[ancs] stored fp=%08lX app=\"%s\" date=\"%s\" title=\"%s\"\n",
                     (unsigned long)fp, app, date, title);
  else
    USBSerial.printf("[ancs] dup (already stored) fp=%08lX app=\"%s\" title=\"%s\"\n",
                     (unsigned long)fp, app, title);
  s_ds_len = 0;                    // ready for the next reply
  ancs_req_complete();             // reply done -> send the next queued request
}

/* Data Source notify RX: append bytes to the reassembly buffer, then try to parse.
 * Because we request one notification's attributes at a time, treating the buffer as
 * a single growing record is correct: each new request resets it (see ancs_request). */
static void ancs_data_source_rx(const uint8_t *data, uint16_t len) {
  if (len == 0) return;
  if (s_ds_len + len > ANCS_DS_BUF_MAX) {                  // overflow guard -> drop the
    s_ds_len = 0;                                          // reply, free the slot
    ancs_req_complete();
    return;
  }
  memcpy(&s_ds_buf[s_ds_len], data, len);
  s_ds_len += len;
  ancs_parse_and_store();          // no-op until a full record is present
}

/* ===================== Control Point request ============================== */

/* Ask iOS for AppIdentifier + Title + Message + Date of a given notification UID.
 * Request layout: [CmdID=0][UID:4 LE][AttrID][maxlen:2 LE]... (AppID/Date: no maxlen).
 * Date (attr 5) is a FIXED 16-char "yyyyMMdd'T'HHmmSS" string — its stable value is
 * the backbone of our cross-reconnect dedup fingerprint (the UID isn't durable). */
static bool ancs_request_attrs(uint32_t uid, uint8_t ancs_cat) {
  if (!s_ancs_cp_val) return false;
  s_inflight_cat = ancs_cat;       // remember the category for ancs_parse_and_store
  uint8_t req[32];
  uint16_t n = 0;
  req[n++] = ANCS_CMD_GET_NOTIF_ATTRS;
  req[n++] = (uint8_t)(uid);
  req[n++] = (uint8_t)(uid >> 8);
  req[n++] = (uint8_t)(uid >> 16);
  req[n++] = (uint8_t)(uid >> 24);
  req[n++] = ANCS_ATTR_APP_ID;                                  // no length for app id
  req[n++] = ANCS_ATTR_TITLE;
  req[n++] = (uint8_t)(ANCS_ATTR_TITLE_MAXLEN);
  req[n++] = (uint8_t)(ANCS_ATTR_TITLE_MAXLEN >> 8);
  req[n++] = ANCS_ATTR_MESSAGE;
  req[n++] = (uint8_t)(ANCS_ATTR_MSG_MAXLEN);
  req[n++] = (uint8_t)(ANCS_ATTR_MSG_MAXLEN >> 8);
  req[n++] = ANCS_ATTR_DATE;                                    // no length for date (fixed 16)

  s_ds_len = 0;                    // start a fresh reassembly for this reply
  int rc = ble_gattc_write_flat(s_ancs_conn, s_ancs_cp_val, req, n, NULL, NULL);
  if (rc != 0) USBSerial.printf("[ancs] control-point write rc=%d (uid=%lu)\n", rc, (unsigned long)uid);
  return rc == 0;
}

/* Send the next queued attribute request, strictly one in flight. A request whose
 * write kickoff fails is dropped and the next one tried (bounded by the queue). */
static void ancs_req_pump(void) {
  while (!s_req_inflight && s_reqq_count && s_ancs_cp_val) {
    uint32_t uid = s_reqq_uid[s_reqq_head];
    uint8_t  cat = s_reqq_cat[s_reqq_head];
    s_reqq_head  = (s_reqq_head + 1) % ANCS_REQQ_MAX;
    s_reqq_count--;
    if (ancs_request_attrs(uid, cat)) {
      s_req_inflight   = true;
      s_req_started_ms = millis();
    }
  }
}

/* The in-flight reply is fully consumed (or unusable): free the slot, send the next. */
static void ancs_req_complete(void) {
  s_req_inflight = false;
  ancs_req_pump();
}

/* Queue an ADDED/MODIFIED event's attribute fetch. If a reply went missing (iOS
 * never finished it), don't let the stale in-flight flag wedge the queue forever. */
static void ancs_req_enqueue(uint32_t uid, uint8_t cat) {
  if (s_req_inflight && millis() - s_req_started_ms > 3000) s_req_inflight = false;
  if (s_reqq_count >= ANCS_REQQ_MAX) {
    USBSerial.printf("[ancs] request queue full — dropping uid=%lu\n", (unsigned long)uid);
    return;
  }
  uint8_t tail = (s_reqq_head + s_reqq_count) % ANCS_REQQ_MAX;
  s_reqq_uid[tail] = uid;
  s_reqq_cat[tail] = cat;
  s_reqq_count++;
  ancs_req_pump();
}

/* ===================== Notification Source RX ============================= */

/* Remove an ANCS notification (by UID) from BOTH stores so the watch mirrors the
 * phone: the archive (full history view the notifications app reads when an SD/FFat
 * archive is present) AND the NVS cache (the no-archive view + the bell unread
 * count). Runs on the NimBLE task -> guards the store, flags the loop to refresh. */
static void ancs_remove_uid(uint32_t uid) {
  uint64_t id;
  if (!ancs_uidmap_get(uid, &id)) return;   // we never stored this UID this session
  store_lock();
  bool hit = false;
  if (na_available() && na_remove(id)) hit = true;     // archive (CSV rewrite)
  if (notif_store_remove_by_id(id))    hit = true;     // NVS newest-32 cache
  store_unlock();
  if (hit) {
    s_ancs_removed = true;          // loop refreshes the bell + any open list
    USBSerial.printf("[ancs] removed uid=%lu (cleared on phone)\n", (unsigned long)uid);
  }
}

/* Notification Source notify RX: 8-byte event [EventID][Flags][CategoryID][Count][UID:4].
 *   ADDED    -> fetch its attributes; INCOMING_CALL becomes transient live state, all
 *               other categories (incl. MISSED_CALL) are stored.
 *   REMOVED  -> the user cleared it on the phone: drop it from the watch too; if it's
 *               the ringing call's UID, also clear the live incoming-call state.
 *   MODIFIED -> treat like ADDED (re-fetch); notif_store_add de-dups by id, so a
 *               genuine change updates nothing today but does no harm. (Full
 *               in-place update is a later refinement if needed.) */
static void ancs_notif_source_rx(const uint8_t *data, uint16_t len) {
  if (len < 8) return;
  uint8_t  event_id = data[0];
  uint8_t  category = data[2];          // ANCS CategoryID — incoming vs missed call, etc.
  uint32_t uid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                 ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

  if (event_id == ANCS_EVENT_REMOVED) {
    // If the call that was ringing just ended (answered/declined/missed), clear the
    // live incoming-call state. The follow-up MISSED_CALL (if any) arrives as its own
    // ADDED and is stored normally — so the missed call is a single entry.
    if (s_incoming_active && uid == s_incoming_uid) {
      s_incoming_active = false; s_incoming_uid = 0; s_incoming_dirty = true;
      USBSerial.println("[ancs] incoming call ended (removed)");
    }
    ancs_remove_uid(uid);
    return;
  }

  // ADDED or MODIFIED: queue the attribute fetch (serialized — see ancs_req_pump).
  // The category rides along so the parser can route an INCOMING_CALL to the
  // transient live-call state instead of the store.
  ancs_req_enqueue(uid, category);
}

/* ===================== subscribe (write CCCDs) ============================ */

static int ancs_ds_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0) { USBSerial.printf("[ancs] DS CCCD write status=%d\n", err->status); return 0; }
  s_ancs_ds_subbed = true;
  USBSerial.println("[ancs] subscribed to Data Source");
  return 0;
}
static void ams_on_encrypted(uint16_t conn_handle);   // defined in ble_player_ams.h

static int ancs_ns_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg) {
  (void)attr; (void)arg;
  if (err && err->status != 0) { USBSerial.printf("[ancs] NS CCCD write status=%d\n", err->status); return 0; }
  s_ancs_ns_subbed = true;
  USBSerial.println("[ancs] subscribed to Notification Source — backlog + live events will flow");
  // ANCS discovery chain is fully done now. NimBLE allows only ONE GATT procedure in
  // flight per connection, so kick off AMS discovery HERE (serialized) rather than
  // concurrently from onAuthenticationComplete — otherwise AMS's disc_svc collides
  // with ANCS's and silently never runs.
  ams_on_encrypted(conn);
  return 0;
}
static void ancs_write_cccd(uint16_t cccd_handle,
                            int (*cb)(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *)) {
  static const uint8_t en[2] = { 0x01, 0x00 };
  int rc = ble_gattc_write_flat(s_ancs_conn, cccd_handle, en, sizeof(en), cb, NULL);
  if (rc != 0) USBSerial.printf("[ancs] write_flat(cccd 0x%04X) rc=%d\n", cccd_handle, rc);
}

/* ===================== descriptor discovery (find CCCDs) ==================
 * IMPORTANT NimBLE quirk: ble_gattc_disc_all_dscs(conn, start, end, cb, arg) echoes
 * the `start` handle you pass back to the callback as `chr_val_handle` — it is NOT
 * "the characteristic this descriptor belongs to". So to attribute a CCCD to a
 * specific characteristic we must call disc_all_dscs ONCE PER CHARACTERISTIC, using
 * that characteristic's own value handle as `start`. We chain: discover NS's
 * descriptors, then (on its EDONE) DS's, then subscribe both. */

/* Step 2b: Data Source descriptors -> its CCCD -> subscribe both channels. */
static int ancs_dsc_ds_cb(uint16_t conn, const struct ble_gatt_error *err,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)conn; (void)chr_val_handle; (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] DS dsc disc error status=%d\n", err->status);
    return 0;
  }
  if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
    s_ancs_ds_cccd = dsc->handle;
  if (err && err->status == BLE_HS_EDONE) {
    USBSerial.printf("[ancs] cccds: NS=0x%04X DS=0x%04X\n", s_ancs_ns_cccd, s_ancs_ds_cccd);
    // Subscribe Data Source FIRST so we never miss the reply to an attribute request,
    // then Notification Source (which triggers the backlog burst).
    if (s_ancs_ds_cccd) ancs_write_cccd(s_ancs_ds_cccd, ancs_ds_sub_done);
    else USBSerial.println("[ancs] Data Source CCCD not found");
    if (s_ancs_ns_cccd) ancs_write_cccd(s_ancs_ns_cccd, ancs_ns_sub_done);
    else USBSerial.println("[ancs] Notification Source CCCD not found");
  }
  return 0;
}

/* Step 2a: Notification Source descriptors -> its CCCD, then chain to DS. */
static int ancs_dsc_ns_cb(uint16_t conn, const struct ble_gatt_error *err,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)chr_val_handle; (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] NS dsc disc error status=%d\n", err->status);
    return 0;
  }
  if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
    s_ancs_ns_cccd = dsc->handle;
  if (err && err->status == BLE_HS_EDONE) {
    // Now discover the Data Source's descriptors (its CCCD). Bound the scan tightly
    // to a couple of handles past DS's value handle so a CCCD is captured without
    // bleeding into a neighbouring characteristic. (DS is typically last anyway.)
    uint16_t ds_end = s_ancs_ds_val + 2;
    if (ds_end > s_ancs_svc_end) ds_end = s_ancs_svc_end;
    ble_gattc_disc_all_dscs(conn, s_ancs_ds_val, ds_end, ancs_dsc_ds_cb, NULL);
  }
  return 0;
}

/* ===================== characteristic discovery ========================== */

static int ancs_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] chr disc error status=%d\n", err->status);
    return 0;
  }
  if (chr) {
    if      (ble_uuid_cmp(&chr->uuid.u, &ANCS_NOTIF_SRC_UUID.u) == 0) s_ancs_ns_val = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_CTRL_PT_UUID.u)  == 0) s_ancs_cp_val = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_DATA_SRC_UUID.u) == 0) s_ancs_ds_val = chr->val_handle;
  }
  if (err && err->status == BLE_HS_EDONE) {
    USBSerial.printf("[ancs] chars: NS=0x%04X CP=0x%04X DS=0x%04X\n",
                     s_ancs_ns_val, s_ancs_cp_val, s_ancs_ds_val);
    if (s_ancs_ns_val && s_ancs_cp_val && s_ancs_ds_val) {
      // Discover NS's descriptors first (tight bound past its value handle so we get
      // ITS CCCD, not a neighbour's); the chain then does DS, then subscribes both.
      uint16_t ns_end = s_ancs_ns_val + 2;
      if (ns_end > s_ancs_svc_end) ns_end = s_ancs_svc_end;
      ble_gattc_disc_all_dscs(conn, s_ancs_ns_val, ns_end, ancs_dsc_ns_cb, NULL);
    } else {
      USBSerial.println("[ancs] missing a required ANCS characteristic");
    }
  }
  return 0;
}

/* ===================== service discovery ================================= */

static int ancs_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] svc disc error status=%d\n", err->status);
    return 0;
  }
  if (svc) { s_ancs_svc_start = svc->start_handle; s_ancs_svc_end = svc->end_handle; }
  if (err && err->status == BLE_HS_EDONE) {
    if (s_ancs_svc_start) {
      USBSerial.printf("[ancs] ANCS service [0x%04X..0x%04X] — discovering chars\n",
                       s_ancs_svc_start, s_ancs_svc_end);
      ble_gattc_disc_all_chrs(conn, s_ancs_svc_start, s_ancs_svc_end, ancs_chr_cb, NULL);
    } else {
      USBSerial.println("[ancs] no ANCS service exposed (not iPhone, or not yet trusted)");
    }
  }
  return 0;
}

/* ===================== public entry points =============================== */

/* Called from onAuthenticationComplete once the link is ENCRYPTED. Kicks the
 * discover -> subscribe chain. iOS replays the pending-notification backlog as soon
 * as Notification Source is subscribed, so no explicit "fetch backlog" call needed. */
static void ancs_on_encrypted(uint16_t conn_handle) {
  if (s_ancs_ns_subbed && conn_handle == s_ancs_conn) return;  // already running this conn
  ancs_reset();
  s_ancs_conn = conn_handle;
  USBSerial.printf("[ancs] link encrypted (conn=0x%04X) — discovering ANCS...\n", conn_handle);
  int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &ANCS_SVC_UUID.u, ancs_svc_cb, NULL);
  if (rc != 0) USBSerial.printf("[ancs] disc_svc_by_uuid rc=%d\n", rc);
}

/* The loop calls this each iteration; returns true once when ANCS has queued a new
 * notification so the loop can pop the card + refresh the bell (LVGL-thread safe). */
static bool ancs_take_ui_dirty(void) {
  if (!s_ancs_ui_dirty) return false;
  s_ancs_ui_dirty = false;
  return true;
}

/* The loop calls this each iteration; true once when ANCS removed a notification so
 * the loop can refresh the bell + rebuild an open notifications list (LVGL-safe). */
static bool ancs_take_removed(void) {
  if (!s_ancs_removed) return false;
  s_ancs_removed = false;
  return true;
}

/* ---- live incoming-call accessors (LVGL/loop thread) ----
 * ancs_take_incoming_dirty() returns true once each time the ringing state changes
 * (call started or ended), so the loop can show/hide a live "incoming call" screen.
 * ancs_incoming_active()/who() report the current state. A future answer/decline UI
 * calls ancs_dismiss_id(ANCS_ID_BASE | s_incoming_uid) for decline (ANCS Negative)
 * and a positive PerformAction for answer. */
static bool ancs_take_incoming_dirty(void) {
  if (!s_incoming_dirty) return false;
  s_incoming_dirty = false;
  return true;
}
static bool        ancs_incoming_active(void) { return s_incoming_active; }
static const char *ancs_incoming_who(void)    { return s_incoming_who; }

/* ===================== dismiss FROM the watch (watch -> phone) ============
 * Write a "Perform Notification Action / Negative" to the Control Point so iOS
 * clears the notification on the phone too. BEST-EFFORT: only works while a phone
 * is connected, ANCS is wired, and we still know the notification's LIVE UID (from
 * THIS session's UID->id map). If any of those is missing — e.g. dismissing while
 * disconnected, or an item carried over from a previous session — we silently do
 * nothing here; the local store removal (the caller's existing behaviour) still
 * happens. iOS will also echo a REMOVED back, which our removal-sync no-ops on a
 * already-gone id. These run on the LVGL/loop thread (called from UI dismiss
 * handlers); ble_gattc_write_flat is safe to call from there. */
/* Write completion callback so we SEE the result of the Control Point write (a NULL
 * callback would hide both GATT errors and the ANCS error code iOS returns when a
 * notification doesn't support the requested action). */
static int ancs_dismiss_done(uint16_t conn, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0)
    USBSerial.printf("[ancs] dismiss write FAILED att_status=%d\n", err->status);
  else
    USBSerial.println("[ancs] dismiss write ACKed by phone");
  return 0;
}

static void ancs_dismiss_id(uint64_t id) {
  if (!s_ble_connected) { USBSerial.println("[ancs] dismiss skipped: not connected"); return; }
  if (!s_ancs_cp_val)   { USBSerial.println("[ancs] dismiss skipped: ANCS Control Point not ready"); return; }
  uint32_t uid;
  if (!ancs_uidmap_get_uid(id, &uid)) {
    USBSerial.printf("[ancs] dismiss skipped: no live UID for id=%08lX (not seen this session). "
                     "uidmap has %u entries\n",
                     (unsigned long)(uint32_t)id, s_uidmap_count);
    return;
  }

  uint8_t req[6];                                      // cmd(1) + uid(4) + action(1)
  req[0] = ANCS_CMD_PERFORM_ACTION;
  req[1] = (uint8_t)(uid);
  req[2] = (uint8_t)(uid >> 8);
  req[3] = (uint8_t)(uid >> 16);
  req[4] = (uint8_t)(uid >> 24);
  req[5] = ANCS_ACTION_NEGATIVE;                       // dismiss
  USBSerial.printf("[ancs] sending dismiss for uid=%lu (cp_handle=0x%04X)\n",
                   (unsigned long)uid, s_ancs_cp_val);
  int rc = ble_gattc_write_flat(s_ancs_conn, s_ancs_cp_val, req, sizeof(req),
                                ancs_dismiss_done, NULL);
  if (rc != 0) USBSerial.printf("[ancs] dismiss write kickoff rc=%d (uid=%lu)\n", rc, (unsigned long)uid);
}

/* Clear-all from the watch: dismiss every notification we currently know a live UID
 * for, on the phone. Best-effort, same constraints as ancs_dismiss_id. Caller still
 * clears the local store itself. */
static void ancs_dismiss_all(void) {
  if (!s_ble_connected || !s_ancs_cp_val) return;
  for (uint8_t i = 0; i < s_uidmap_count; i++) {
    uint8_t req[6];
    uint32_t uid = s_uidmap_uid[i];
    req[0] = ANCS_CMD_PERFORM_ACTION;
    req[1] = (uint8_t)(uid);
    req[2] = (uint8_t)(uid >> 8);
    req[3] = (uint8_t)(uid >> 16);
    req[4] = (uint8_t)(uid >> 24);
    req[5] = ANCS_ACTION_NEGATIVE;
    ble_gattc_write_flat(s_ancs_conn, s_ancs_cp_val, req, 6, NULL, NULL);
  }
  if (s_uidmap_count)
    USBSerial.printf("[ancs] clear-all: sent dismiss for %u notifications\n", s_uidmap_count);
}

/* TIMER-WAKE background check (called from sleep_power.h's light check, on the
 * NimBLE-less wake path — there's no loop running). Bring up BLE if it isn't, then
 * wait up to `budget_ms` for the iPhone to RECONNECT, ANCS to subscribe, and the
 * pending-notification backlog to DRAIN into the store. Returns true if ANY new
 * notification arrived (caller then does a full UI boot to show it); false if the
 * window elapsed with nothing new (caller goes back to sleep).
 *
 * Power note: this spends BLE-on time every timer wake, which partly offsets the
 * deep-sleep floor. The budget is the lever — short enough to stay frugal, long
 * enough that iOS (which reconnects on ITS schedule) usually gets in. We bail early
 * the moment something lands, so a quiet check is only as long as the connect wait. */
static bool ancs_background_check(uint32_t budget_ms) {
  uint32_t before = s_ancs_added_total;

  bool we_started_ble = false;
  if (!ble_is_up()) { ble_begin(); we_started_ble = true; }   // start advertising; iOS reconnects

  uint32_t t0 = millis();
  bool got = false;
  while (millis() - t0 < budget_ms) {
    if (s_ancs_added_total != before) { got = true; break; }  // a notification landed
    delay(50);                                                 // let the NimBLE task run
  }

  // If we found something, keep BLE up a brief grace window so a multi-item backlog
  // finishes draining into the store before the caller boots (each item is a
  // separate Control Point round-trip).
  if (got) {
    uint32_t grace0 = millis();
    uint32_t last = s_ancs_added_total;
    while (millis() - grace0 < 1500) {
      delay(100);
      if (s_ancs_added_total != last) { last = s_ancs_added_total; grace0 = millis(); }  // reset grace on each new item
    }
  }

  // Nothing new: tear down BLE only if WE brought it up, so we don't disturb a
  // user-enabled always-on BLE session. (On the got==true path we leave BLE up; the
  // full boot wants it for the live link anyway.)
  if (!got && we_started_ble) ble_end();
  return got;
}

/* ===================== notify-RX intake (custom GAP handler) ============== */

/* Custom GAP event handler registered via BLEDevice::setCustomGapHandler(). The
 * core's NimBLE dispatcher calls this for EVERY GAP event, in addition to its own
 * server handler — so we can observe inbound notifications (BLE_GAP_EVENT_NOTIFY_RX)
 * WITHOUT patching the core. We only act on ANCS notifications (Notification Source
 * or Data Source value handles on our connection) and ignore everything else.
 * Signature matches the NimBLE build's gap_event_handler typedef:
 *   int (*)(struct ble_gap_event *event, void *param)
 * Runs on the NimBLE host task. Return 0 (we don't override the wrapper's handling). */
/* AMS shares this connection's notify stream; its handler lives in ble_player_ams.h
 * (included later). Forward-declared so the one GAP handler can route to both. */
static bool ams_handle_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                                 const struct os_mbuf *om);

static int ancs_gap_event(struct ble_gap_event *event, void *param) {
  (void)param;
  if (event->type != BLE_GAP_EVENT_NOTIFY_RX) return 0;

  uint16_t conn = event->notify_rx.conn_handle;
  uint16_t attr = event->notify_rx.attr_handle;
  const struct os_mbuf *om = event->notify_rx.om;
  if (!om) return 0;

  // AMS (media) shares the same connection + notify path as ANCS. Give the AMS
  // router first refusal; if it consumed the notification, we're done.
  if (ams_handle_notify_rx(conn, attr, om)) return 0;

  if (conn != s_ancs_conn) return 0;
  if (attr != s_ancs_ns_val && attr != s_ancs_ds_val) return 0;

  uint8_t buf[256];
  uint16_t len = OS_MBUF_PKTLEN((struct os_mbuf *)om);
  if (len > sizeof(buf)) len = sizeof(buf);
  if (os_mbuf_copydata(om, 0, len, buf) != 0) return 0;

  if (attr == s_ancs_ns_val) ancs_notif_source_rx(buf, len);
  else                       ancs_data_source_rx(buf, len);
  return 0;
}

/* Register the custom GAP handler so inbound ANCS notifications reach us. Call once
 * from ble_begin(). Idempotent (setting the same handler again is harmless). */
static void ancs_install_gap_handler(void) {
  BLEDevice::setCustomGapHandler(ancs_gap_event);
}
