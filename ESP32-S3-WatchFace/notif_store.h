/* ============================================================================
 *  notif_store.h — notification history store (NVS-backed).
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER settings_store.h (uses
 *  the shared `prefs` handle) and watch_base.h (store_lock / store_unlock guard
 *  access from the UI core vs the network core). Consumed by the Notifications
 *  app, the popup card, and the fetch path.
 *
 *  The server DRAINS its queue on each GET, so the watch keeps its own copy to
 *  show "all missed notifications". Stored in flash (NVS) so it survives
 *  deep-sleep wakes AND true power-off.
 *
 *  Cap = NOTIF_STORE_MAX (32), newest-first — a ROLLING cache: once full, adding
 *  a newer item EVICTS the oldest so flash always holds the newest 32. (With an SD
 *  card the full unlimited history is on the card; flash is just the fast newest-32
 *  mirror — see notif_archive_sd.h. Without a card, the newest 32 is the whole
 *  story.) Writes are still batched: the fetch path saves the whole list ONCE per
 *  GET (not per item), so a spam flood costs one NVS write per fetch cadence, not
 *  one per notification. A dismiss also writes once.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

#define NOTIF_STORE_MAX   32
#define NOTIF_TITLE_MAX   48     // bytes stored per title (truncated)
#define NOTIF_BODY_MAX    256    // bytes stored per body  (truncated) — no-card fallback
                                 // (with an SD card the full text goes to notif_archive_sd.h)

/* Coarse notification category, derived from the source (ANCS AppIdentifier, or the
 * server "app" field). Drives the per-app icon in the list and the special call
 * layout in the reader. One byte, persisted with each notification. Keep values
 * STABLE across builds — they're written to NVS + the CSV archive. Append new
 * categories at the end; never renumber. */
enum NotifCat : uint8_t {
  NCAT_GENERIC  = 0,   // anything we don't specifically recognise
  NCAT_CALL     = 1,   // phone / missed call / FaceTime
  NCAT_MESSAGE  = 2,   // SMS / iMessage / chat apps
  NCAT_MAIL     = 3,   // email
  NCAT_CALENDAR = 4,   // calendar events
  NCAT_REMINDER = 5,   // reminders / to-do / tasks
  NCAT_SOCIAL   = 6,   // social networks (FB, X, Instagram, Reddit, ...)
  NCAT_MUSIC    = 7,   // music / audio / podcasts
  NCAT_VIDEO    = 8,   // video / streaming (YouTube, Netflix, ...)
  NCAT_PHOTO    = 9,   // photos / camera
  NCAT_MAPS     = 10,  // maps / navigation / rideshare
  NCAT_FINANCE  = 11,  // banking / payments / finance
  NCAT_SHOPPING = 12,  // shopping / delivery / orders
  NCAT_NEWS     = 13,  // news / RSS
  NCAT_HEALTH   = 14,  // health / fitness
  NCAT_WEATHER  = 15,  // weather
  NCAT_GAME     = 16,  // games
  NCAT_SYSTEM   = 17,  // OS / settings / updates
  NCAT_SECURITY = 18,  // security / 2FA / alerts
  NCAT_POWER    = 19,  // charging / charge / power
  NCAT_SAVE     = 20,  // save / disk / floppy
  NCAT_TRASH    = 21,  // trash / garbage
  NCAT_TEMP     = 22,  // temp / degree
  NCAT_IOS      = 23,  // IOS / update
  NCAT_BARCODE  = 24,  // barcode
  NCAT_PHONE    = 25,  // phone / cellular
  NCAT_PRINTER  = 26,  // printer
  NCAT_ERROR    = 27,  // error
  NCAT_USB      = 28,  // usb
  NCAT_CABLE    = 29,  // cable
  NCAT_VIRUS    = 30,  // virus / infection
  NCAT_CAR      = 31,  // car / automotive / EV charging
  NCAT_FOOD     = 32,  // food / restaurant / recipes
  NCAT_COFFEE   = 33,  // coffee / cafe
  NCAT_FLIGHT   = 34,  // flights / airlines / boarding
  NCAT_TRANSIT  = 35,  // public transit / train / bus / metro
  NCAT_PACKAGE  = 36,  // package / parcel / delivery tracking
  NCAT_CLOUD    = 37,  // cloud sync / storage services
  NCAT_DOWNLOAD = 38,  // download / file transfer
  NCAT_BOOK     = 39,  // books / reading / e-reader
  NCAT_HOME     = 40,  // smart home / home automation
  NCAT_BELL     = 41,  // doorbell / smart doorbell
  NCAT_LOCK     = 42,  // smart lock / door lock / access
  NCAT_LIGHT    = 43,  // smart lights / bulbs
  NCAT_WIFI     = 44,  // wifi / network connectivity
  NCAT_BLUETOOTH= 45,  // bluetooth devices / pairing
  NCAT_LOCATION = 46,  // location sharing / find-my
  NCAT_ALARM    = 47,  // alarm / timer (clock app)
  NCAT_NOTE     = 48,  // notes / memos
  NCAT_CRYPTO   = 49,  // crypto / blockchain / wallet
  NCAT_SLEEP    = 50,  // sleep tracking / bedtime
  NCAT_RUN      = 51,  // running / walking / steps
  NCAT_PET      = 52,  // pets / pet trackers
  NCAT_BABY     = 53,  // baby monitor / parenting
  NCAT_GIFT     = 54,  // gifts / rewards / loyalty
  NCAT_TICKET   = 55,  // tickets / events / passes
  NCAT_UMBRELLA = 56,  // rain / precipitation alerts

  /* ---- vendor / platform-specific (brand glyphs) ----
   * Checked BEFORE the generic buckets in notif_cat_from_appid(), so a recognised
   * app gets its real logo; anything unrecognised still falls back to the generic
   * MESSAGE / SOCIAL / VIDEO / ... category. */

  NCAT_WHATSAPP  = 57,
  NCAT_MESSENGER = 58,
  NCAT_TELEGRAM  = 59,
  NCAT_SIGNAL    = 60,
  NCAT_DISCORD   = 61,
  NCAT_SLACK     = 62,
  NCAT_INSTAGRAM = 63,
  NCAT_FACEBOOK  = 64,
  NCAT_TWITTER   = 65,  // X / Twitter
  NCAT_SNAPCHAT  = 66,
  NCAT_TIKTOK    = 67,
  NCAT_REDDIT    = 68,
  NCAT_LINKEDIN  = 69,
  NCAT_YOUTUBE   = 70,
  NCAT_SPOTIFY   = 71,
  NCAT_NETFLIX   = 72,
  NCAT_GMAIL     = 73,
  NCAT_OUTLOOK   = 74,
  NCAT_PAYPAL    = 75,
  NCAT_UBER      = 76,

  NCAT_COUNT
  // NOTE: values are PERSISTED (NVS + CSV). Only APPEND new categories here; never
  // renumber an existing one or old stored notifications get the wrong icon.
};

/* Map an iOS AppIdentifier (reverse-DNS bundle id, e.g. "com.apple.MobileSMS") OR a
 * friendly app name to a category. Case-insensitive substring matching, ordered so
 * more-specific patterns win. Unknown -> NCAT_GENERIC. Add patterns freely; this is
 * the single place app recognition lives. */
static NotifCat notif_cat_from_appid(const char *appid) {
  if (!appid || !appid[0]) return NCAT_GENERIC;
  char a[64];
  size_t n = 0;
  for (; appid[n] && n < sizeof(a) - 1; n++) {
    char c = appid[n];
    a[n] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
  }
  a[n] = '\0';
  #define M(s) (strstr(a, s) != nullptr)

  // ---- vendor / platform-specific (checked FIRST so a known app gets its real
  // logo; unknown apps fall through to the generic buckets below) ----
  if (M("whatsapp"))                                          return NCAT_WHATSAPP;
  if (M("messenger") || M("fb-messenger"))                    return NCAT_MESSENGER;
  if (M("telegram") || M("org.telegram"))                     return NCAT_TELEGRAM;
  if (M("signal"))                                            return NCAT_SIGNAL;
  if (M("discord"))                                           return NCAT_DISCORD;
  if (M("slack"))                                             return NCAT_SLACK;
  if (M("instagram"))                                         return NCAT_INSTAGRAM;
  if (M("facebook") || M("com.facebook.facebook"))            return NCAT_FACEBOOK;
  if (M("twitter") || M("com.atebits") || M(".x.") || M("/x ") || M("x.com"))
    return NCAT_TWITTER;
  if (M("snapchat"))                                          return NCAT_SNAPCHAT;
  if (M("tiktok") || M("musical.ly") || M("bytedance"))       return NCAT_TIKTOK;
  if (M("reddit"))                                            return NCAT_REDDIT;
  if (M("linkedin"))                                          return NCAT_LINKEDIN;
  if (M("youtube"))                                           return NCAT_YOUTUBE;
  if (M("spotify"))                                           return NCAT_SPOTIFY;
  if (M("netflix"))                                           return NCAT_NETFLIX;
  if (M("gmail") || M("com.google.gmail"))                    return NCAT_GMAIL;
  if (M("outlook"))                                           return NCAT_OUTLOOK;
  if (M("paypal"))                                            return NCAT_PAYPAL;
  // Uber the rideshare app — but NOT Uber Eats (let that fall to FOOD/SHOPPING below).
  if (M("uber") && !M("ubereats") && !M("uber eats"))         return NCAT_UBER;

  // Communication
  if (M("mobilephone") || M("facetime") || M(".phone") || M("missedcall") || M("dialer"))
    return NCAT_CALL;
  if (M("mobilesms") || M("message") || M("whatsapp") || M("telegram") || M("signal") ||
      M("messenger") || M("imessage") || M("wechat") || M("viber") || M("discord") ||
      M("slack") || M("teams"))
    return NCAT_MESSAGE;
  if (M("mail") || M("gmail") || M("outlook") || M("yahoo") || M("proton") || M("spark"))
    return NCAT_MAIL;

  // Time / tasks
  if (M("calendar") || M("fantastical") || M("calshow"))      return NCAT_CALENDAR;
  if (M("reminder") || M("todo") || M("to-do") || M("things") || M("ticktick") ||
      M("task") || M("clock") || M("alarm"))                  return NCAT_REMINDER;

  // Social
  if (M("facebook") || M("instagram") || M("twitter") || M(".x.") || M("/x ") ||
      M("reddit") || M("tiktok") || M("snapchat") || M("linkedin") || M("mastodon") ||
      M("threads") || M("pinterest") || M("bsky") || M("bluesky"))
    return NCAT_SOCIAL;

  // Media
  if (M("music") || M("spotify") || M("podcast") || M("soundcloud") || M("audible") ||
      M("deezer") || M("tidal"))                              return NCAT_MUSIC;
  if (M("youtube") || M("netflix") || M("video") || M("hulu") || M("disney") ||
      M("twitch") || M("primevideo") || M("hbo") || M("plex"))return NCAT_VIDEO;
  if (M("photo") || M("camera") || M("mobileslideshow") || M("gallery"))
    return NCAT_PHOTO;

  // Places / money / shopping
  if (M("maps") || M("waze") || M("uber") || M("lyft") || M("bolt") || M("navigation") || M("karta") ||
      M("citymapper"))                                        return NCAT_MAPS;
  if (M("bank") || M("handel") || M("paypal") || M("venmo") || M("wallet") || M("revolut") || M("cash") ||
      M("finance") || M("coinbase") || M("stripe") || M("klarna") || M("id") || M("swish"))
    return NCAT_FINANCE;
  if (M("amazon") || M("shop") || M("store") || M("ebay") || M("aliexpress") || M("blocket") ||
      M("doordash") || M("ubereats") || M("deliveroo") || M("wolt") || M("tradera") || M("order"))
    return NCAT_SHOPPING;

  // Info
  if (M("news") || M("rss") || M("feedly") || M("nytimes") || M("guardian") || M("bbc"))
    return NCAT_NEWS;
  if (M("health") || M("fitness") || M("workout") || M("strava") || M("activity") ||
      M("garmin") || M("fitbit"))                             return NCAT_HEALTH;
  if (M("weather") || M("forecast") || M("carrot"))          return NCAT_WEATHER;
  if (M("game") || M("playstation") || M("xbox") || M("steam") || M("nintendo"))
    return NCAT_GAME;

  // System / security
  if (M("antivirus") || M("malware") || M("mcafee") || M("norton") || M("avast") ||
      M("kaspersky") || M("bitdefender") || M("threat"))      return NCAT_VIRUS;
  if (M("security") || M("authenticator") || M("2fa") || M("duo") || M("1password") ||
      M("bitwarden") || M("lastpass") || M("vpn"))            return NCAT_SECURITY;

  // Device / hardware state
  if (M("battery") || M("charging") || M("charge") || M("power") || M("lowpower") ||
      M("energy"))                                            return NCAT_POWER;
  if (M("thermostat") || M("nest") || M("temperature") || M("ecobee") ||
      M("thermometer") || M("Temp"))                         return NCAT_TEMP;
  if (M("printer") || M("airprint") || M("hpsmart") || M("hp smart") || M("epson") ||
      M("canon print"))                                       return NCAT_PRINTER;
  if (M("usb") || M("thumbdrive") || M("flashdrive"))         return NCAT_USB;
  if (M("cable") || M("ethernet") || M("hdmi"))               return NCAT_CABLE;
  if (M("barcode") || M("scanner") || M("qr ") || M("qrcode")) return NCAT_BARCODE;
  if (M("car") || M("tesla") || M("carplay") || M("android auto") || M("automotive") ||
      M("bmw") || M("volvo") || M("mercedes") || M("evcharg"))return NCAT_CAR;
  if (M("save") || M("backup") || M("timemachine") || M("icloud") || M("dropbox") ||
      M("onedrive") || M("drive"))                            return NCAT_SAVE;
  if (M("trash") || M("recyclebin") || M("recycle bin") || M("bin "))
    return NCAT_TRASH;
  if (M("error") || M("crash") || M("failure") || M("failed"))return NCAT_ERROR;

  // Food / travel / logistics
  if (M("coffee") || M("starbucks") || M("cafe") || M("espresso") || M("dunkin"))
    return NCAT_COFFEE;
  if (M("food") || M("restaurant") || M("recipe") || M("yelp") || M("opentable") ||
      M("grubhub") || M("mcdonald") || M("pizza") || M("hellofresh"))
    return NCAT_FOOD;
  if (M("flight") || M("airline") || M("airport") || M("boarding") || M("ryanair") ||
      M("lufthansa") || M("delta") || M("united") || M("sas") || M("skyscanner") ||
      M("kayak"))                                             return NCAT_FLIGHT;
  if (M("transit") || M("train") || M("metro") || M("subway") || M("bus ") ||
      M("rail") || M("sj ") || M("trafik") || M("commute"))   return NCAT_TRANSIT;
  if (M("package") || M("parcel") || M("tracking") || M("ups") || M("fedex") ||
      M("dhl") || M("usps") || M("postnord") || M("posten") || M("shipment"))
    return NCAT_PACKAGE;
  if (M("ticket") || M("ticketmaster") || M("eventbrite") || M("passbook") ||
      M("seatgeek") || M("stubhub"))                          return NCAT_TICKET;

  // Files / cloud
  if (M("download") || M("transfer") || M("torrent") || M("filetransfer"))
    return NCAT_DOWNLOAD;
  if (M("cloud") || M("googledrive") || M("box") || M("mega") || M("nextcloud") ||
      M("sync"))                                              return NCAT_CLOUD;
  if (M("note") || M("notes") || M("memo") || M("evernote") || M("onenote") ||
      M("obsidian") || M("bear"))                             return NCAT_NOTE;
  if (M("book") || M("kindle") || M("kobo") || M("ibooks") || M("ereader") ||
      M("goodreads") || M("pocket"))                          return NCAT_BOOK;

  // Smart home / IoT
  if (M("doorbell") || M("ring") || M("nest hello"))          return NCAT_BELL;
  if (M("lock") || M("august") || M("yale") || M("schlage") || M("deadbolt"))
    return NCAT_LOCK;
  if (M("light") || M("hue") || M("lifx") || M("bulb") || M("lamp"))
    return NCAT_LIGHT;
  if (M("homekit") || M("smarthome") || M("smartthings") || M("home assistant") ||
      M("homeassistant") || M("wink") || M("alexa"))          return NCAT_HOME;
  if (M("wifi") || M("wi-fi") || M("router") || M("network") || M("hotspot"))
    return NCAT_WIFI;
  if (M("bluetooth") || M("airpods") || M("pairing"))         return NCAT_BLUETOOTH;
  if (M("findmy") || M("find my") || M("airtag") || M("tile") || M("location") ||
      M("geofence"))                                          return NCAT_LOCATION;

  // Activity / lifestyle
  if (M("alarm") || M("timer") || M("stopwatch") || M("clock"))return NCAT_ALARM;
  if (M("sleep") || M("bedtime") || M("sleepcycle") || M("oura"))
    return NCAT_SLEEP;
  if (M("run") || M("running") || M("walk") || M("steps") || M("nikerun") ||
      M("runkeeper"))                                         return NCAT_RUN;
  if (M("crypto") || M("bitcoin") || M("ethereum") || M("blockchain") || M("binance") ||
      M("kraken") || M("metamask") || M("ledger"))            return NCAT_CRYPTO;
  if (M("pet") || M("dog ") || M("cat ") || M("tractive") || M("fitbark"))
    return NCAT_PET;
  if (M("baby") || M("nanit") || M("owlet") || M("babymonitor")) return NCAT_BABY;
  if (M("gift") || M("reward") || M("loyalty") || M("points") || M("perks"))
    return NCAT_GIFT;
  if (M("rain") || M("precip") || M("umbrella") || M("storm alert"))
    return NCAT_UMBRELLA;

  // OS — keep AFTER the device-state buckets so e.g. "com.apple.…" doesn't swallow
  // a charging/printer notification, but still catches generic Apple system items.
  if (M("ios") || M("ipados") || M("watchos") || M("macos"))  return NCAT_IOS;
  if (M("phone") || M("cellular") || M("carrier") || M("sim ") || M("dialer"))
    return NCAT_PHONE;
  if (M("preferences") || M("settings") || M("software") || M("update") ||
      M("com.apple.") )                                       return NCAT_SYSTEM;

  #undef M
  return NCAT_GENERIC;
}

struct NotifEntry {
  uint64_t id;
  bool     read;                 // true once the user has opened it in the reader
  uint8_t  cat;                  // NotifCat — drives the per-app icon + call layout
  char title[NOTIF_TITLE_MAX];
  char body[NOTIF_BODY_MAX];
};
#include "esp_heap_caps.h"   // heap_caps_calloc — the list lives in PSRAM

/* The entry list lives in PSRAM, not static SRAM (~10 KB of .bss freed for the
 * display's partial render buffers). Allocated on first use; s_notif_count stays
 * 0 until the alloc succeeds, so every count-guarded loop/index below is safe
 * even while the pointer is still NULL. */
static NotifEntry *s_notifs = nullptr;       // [NOTIF_STORE_MAX], [0] = newest
static uint8_t     s_notif_count = 0;
static bool notif_store_buf_ok(void) {
  if (!s_notifs)
    s_notifs = (NotifEntry *)heap_caps_calloc(NOTIF_STORE_MAX, sizeof(NotifEntry),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_notifs)  // PSRAM missing/full (shouldn't happen) -> internal heap so notifs still work
    s_notifs = (NotifEntry *)calloc(NOTIF_STORE_MAX, sizeof(NotifEntry));
  return s_notifs != nullptr;
}

/* Schema version for the persisted NotifEntry layout. NVS SURVIVES a firmware
 * re-flash, so a new build can read a blob written by an OLD build whose
 * NotifEntry had a different size/layout (e.g. different TITLE/BODY_MAX or extra
 * fields from the old SD-archive design). Reading that stale blob loads garbage —
 * non-terminated strings / misaligned ids — and the Notifications app crashes
 * walking off the end of a char[]. BUMP this whenever NotifEntry changes; on a
 * mismatch we wipe the store so we never interpret an old layout. */
#define NOTIF_SCHEMA_VER  5   // 5: added NotifEntry.cat (per-app category/icon)
                              // 4: added NotifEntry.read (unread tracking)

/* Persist the whole list as one NVS blob under namespace "watch", key "notlist".
 * One blob = one write per change, regardless of list length. */
static void notif_store_save(void) {
  if (!notif_store_buf_ok()) return;   // never hand NVS a NULL blob pointer
  prefs.putUChar("notver", NOTIF_SCHEMA_VER);
  prefs.putBytes("notlist", s_notifs, sizeof(NotifEntry) * s_notif_count);
  prefs.putUChar("notcnt", s_notif_count);
}
static void notif_store_load(void) {
  if (!notif_store_buf_ok()) { s_notif_count = 0; return; }
  // Reject a store written by a different NotifEntry layout (or a pre-version
  // build, where the key is absent -> 0). Wipe it so we start clean rather than
  // deserializing an incompatible/garbage blob.
  if (prefs.getUChar("notver", 0) != NOTIF_SCHEMA_VER) {
    prefs.remove("notlist");
    prefs.remove("notcnt");
    prefs.putUChar("notver", NOTIF_SCHEMA_VER);
    s_notif_count = 0;
    return;
  }
  s_notif_count = prefs.getUChar("notcnt", 0);
  if (s_notif_count > NOTIF_STORE_MAX) s_notif_count = 0;  // corrupt -> reset
  size_t want = sizeof(NotifEntry) * s_notif_count;
  size_t got  = prefs.getBytes("notlist", s_notifs, want);
  if (got != want) { s_notif_count = 0; return; }  // size mismatch -> reset

  // Defensive: even with a matching schema version, a torn/partial write could
  // leave a title/body without a NUL terminator. LVGL reads these as C strings,
  // so an unterminated buffer makes it walk off the end -> crash (the
  // "Notifications app crashes / touch reboots" symptom). Force-terminate every
  // entry so rendering is always safe regardless of what's in flash.
  for (uint8_t i = 0; i < s_notif_count; i++) {
    s_notifs[i].title[NOTIF_TITLE_MAX - 1] = '\0';
    s_notifs[i].body[NOTIF_BODY_MAX - 1]  = '\0';
  }
}

/* Insert one fetched notification at the front (newest-first). Returns false on a
 * duplicate id. When the store is FULL we EVICT THE OLDEST entry to make room, so
 * flash always holds the NEWEST NOTIF_STORE_MAX (32) — a rolling cache. (With an SD
 * card the full history is on the card; flash is just the fast newest-32 mirror, so
 * dropping the oldest here loses nothing. Without a card the newest 32 is also what
 * you want to keep.) */
static bool notif_store_add(uint64_t id, const char *title, const char *body,
                            uint8_t cat = NCAT_GENERIC) {
  if (!notif_store_buf_ok()) return false;
  for (uint8_t i = 0; i < s_notif_count; i++)                // de-dup by id
    if (s_notifs[i].id == id) return false;
  // Full -> drop the oldest (last) so the incoming newest can take index 0.
  uint8_t n = (s_notif_count >= NOTIF_STORE_MAX) ? (NOTIF_STORE_MAX - 1) : s_notif_count;
  for (int i = n; i > 0; i--) s_notifs[i] = s_notifs[i - 1];  // shift down, overwriting the oldest
  if (s_notif_count < NOTIF_STORE_MAX) s_notif_count++;
  s_notifs[0].id = id;
  s_notifs[0].read = false;                                  // freshly arrived -> unread
  s_notifs[0].cat = cat;
  strncpy(s_notifs[0].title, title ? title : "", NOTIF_TITLE_MAX - 1);
  s_notifs[0].title[NOTIF_TITLE_MAX - 1] = '\0';
  strncpy(s_notifs[0].body, body ? body : "", NOTIF_BODY_MAX - 1);
  s_notifs[0].body[NOTIF_BODY_MAX - 1] = '\0';
  return true;
}

/* Remove the entry at index `i` (a dismiss). Compacts and persists. */
static void notif_store_remove(uint8_t i) {
  if (i >= s_notif_count) return;
  for (uint8_t j = i; j + 1 < s_notif_count; j++) s_notifs[j] = s_notifs[j + 1];
  s_notif_count--;
  notif_store_save();
}
static void notif_store_clear(void) {
  s_notif_count = 0;
  notif_store_save();
}

/* Number of stored notifications the user hasn't opened in the reader yet. This
 * is what the watchface bell badge shows (via notif_unread) when there's no SD card. */
static uint8_t notif_store_unread_count(void) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < s_notif_count; i++) if (!s_notifs[i].read) n++;
  return n;
}

/* Mark the entry with id `id` as read (opened in the reader). Persists only when
 * a flag actually flips, so re-opening an already-read item costs no flash write. */
static void notif_store_mark_read(uint64_t id) {
  for (uint8_t i = 0; i < s_notif_count; i++) {
    if (s_notifs[i].id == id) {
      if (!s_notifs[i].read) { s_notifs[i].read = true; notif_store_save(); }
      return;
    }
  }
}

/* Remove the entry with the given id, if present. Returns true if one was removed
 * (caller may want to refresh the UI / bell). Used by ANCS removal sync: when a
 * notification is cleared on the iPhone, the watch drops it too. Compacts + persists
 * only on an actual hit, so a REMOVED for something we never stored costs nothing. */
static bool notif_store_remove_by_id(uint64_t id) {
  for (uint8_t i = 0; i < s_notif_count; i++) {
    if (s_notifs[i].id == id) {
      notif_store_remove(i);   // compacts + saves
      return true;
    }
  }
  return false;
}
