/* ============================================================================
 *  player_state.h — source-agnostic "Now Playing" media state + command routing.
 *
 *  Like the notification store, the Player is decoupled from its TRANSPORT. Any
 *  number of producers can feed it the current track and receive its commands:
 *    - AMS (Apple Media Service) over BLE       — ble_player_ams.h   (iPhone)
 *    - HTTP / server push                        — future
 *    - Android companion / other bridges        
 *  They all call the SAME small API here; the Player app UI (app_player.h) reads
 *  this state and sends commands without knowing which source is active.
 *
 *  DATA MODEL (the common denominator every source can provide): track title,
 *  artist, album, and a playback state (playing/paused/stopped). Transport controls
 *  are play/pause, next, previous. Position/volume are intentionally omitted for now
 *  (not every source reports them reliably); add later if wanted.
 *
 *  COMMAND ROUTING: bidirectional, unlike notifications. Whichever source last
 *  reported a PLAYING (or paused) track becomes the active "command sink"; a UI
 *  button tap routes the command there via its registered callback. Last-active
 *  wins — matches one-thing-playing-at-a-time, which is the norm.
 *
 *  THREADING: producers run on the BLE/net task; the UI on the loop. Mutate the
 *  state under player_lock() (a thin wrapper over the shared store_lock used
 *  elsewhere) and set the dirty flag the loop consumes to refresh an open Player
 *  screen. Strings are fixed buffers — no allocation on the producer path.
 *
 *  INCLUDE AFTER watch_base.h (store_lock/unlock). Header-only, in the .ino TU.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

#define PLAYER_STR_MAX  96      // per-field cap (title/artist/album), truncated

enum PlayState : uint8_t { PLAY_STOPPED = 0, PLAY_PAUSED = 1, PLAYING = 2 };

/* Transport commands a source must be able to action (those it can't, it ignores). */
enum PlayerCmd : uint8_t {
  PCMD_TOGGLE = 0,   // play/pause toggle
  PCMD_NEXT   = 1,
  PCMD_PREV   = 2,
};

/* A source registers two callbacks when it becomes active:
 *   cmd  — "do this transport command" (play/pause/next/prev).
 *   sync — "re-read the true playback state from the device NOW" (optional). Lets the
 *          UI periodically pull truth so the button self-corrects if a push update was
 *          missed. A source that can't poll passes nullptr. */
typedef void (*player_cmd_fn)(PlayerCmd cmd);
typedef void (*player_sync_fn)(void);

enum PlayerSrc : uint8_t { PSRC_NONE = 0, PSRC_AMS = 1, PSRC_HTTP = 2, PSRC_ANDROID = 3 };

/* ---- the single current-track state (guarded by store_lock) ---- */
static char      s_play_title[PLAYER_STR_MAX]  = "";
static char      s_play_artist[PLAYER_STR_MAX] = "";
static char      s_play_album[PLAYER_STR_MAX]  = "";
static PlayState s_play_state  = PLAY_STOPPED;
static PlayerSrc s_play_src    = PSRC_NONE;        // which source last fed us
static player_cmd_fn  s_play_sink = nullptr;       // where commands route
static player_sync_fn s_play_sync = nullptr;       // re-read truth (optional)
static volatile bool s_play_dirty = false;         // loop refreshes the Player screen

/* Thin lock wrappers (reuse the shared store mutex so producers on the BLE/net task
 * and the UI on the loop never tear a half-written field). */
static inline void player_lock(void)   { store_lock(); }
static inline void player_unlock(void) { store_unlock(); }

static void player_copy(char *dst, const char *src) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, PLAYER_STR_MAX - 1);
  dst[PLAYER_STR_MAX - 1] = '\0';
}

/* ---- producer API (called by AMS / HTTP / Android sources) ---- */

/* Register/refresh this source as the active command sink. Call whenever the source
 * has (or resumes) playback so its commands + sync route here. `sync` may be nullptr
 * if the source can't poll its device for the current state. */
static void player_set_sink(PlayerSrc src, player_cmd_fn cmd, player_sync_fn sync) {
  player_lock();
  s_play_src  = src;
  s_play_sink = cmd;
  s_play_sync = sync;
  player_unlock();
}

/* Update the now-playing track metadata. Any field may be empty/null. Marks the UI
 * dirty so an open Player screen refreshes. Does NOT change playback state. */
static void player_set_track(const char *title, const char *artist, const char *album) {
  player_lock();
  player_copy(s_play_title,  title);
  player_copy(s_play_artist, artist);
  player_copy(s_play_album,  album);
  s_play_dirty = true;
  player_unlock();
}

/* Update playback state (playing / paused / stopped). */
static void player_set_state(PlayState st) {
  player_lock();
  if (s_play_state != st) { s_play_state = st; s_play_dirty = true; }
  player_unlock();
}

/* Nothing is playing anymore (source stopped / went away). Clears the track and, if
 * this source owned the sink, relinquishes it. */
static void player_clear(PlayerSrc src) {
  player_lock();
  s_play_title[0] = s_play_artist[0] = s_play_album[0] = '\0';
  s_play_state = PLAY_STOPPED;
  if (s_play_src == src) { s_play_src = PSRC_NONE; s_play_sink = nullptr; s_play_sync = nullptr; }
  s_play_dirty = true;
  player_unlock();
}

/* ---- UI API (called by app_player.h on the loop thread) ---- */

/* True iff some source currently has a track (playing or paused) worth showing. */
static bool player_has_track(void) {
  return s_play_src != PSRC_NONE && (s_play_title[0] || s_play_artist[0]);
}

/* Send a transport command to the active source. No-op if nothing is active. Safe to
 * call from the loop; the sink callback decides how to deliver it (e.g. AMS writes a
 * GATT characteristic). */
static void player_send_command(PlayerCmd cmd) {
  player_cmd_fn fn;
  player_lock();
  fn = s_play_sink;
  player_unlock();
  if (fn) fn(cmd);
}

/* Ask the active source to re-read the device's true playback state (a poll). The UI
 * calls this periodically so the play/pause button self-corrects if a push update was
 * ever missed. No-op if the source can't poll. */
static void player_request_sync(void) {
  player_sync_fn fn;
  player_lock();
  fn = s_play_sync;
  player_unlock();
  if (fn) fn();
}

/* One-shot dirty: the loop calls this to know when to refresh an open Player screen. */
static bool player_take_dirty(void) {
  if (!s_play_dirty) return false;
  s_play_dirty = false;
  return true;
}
