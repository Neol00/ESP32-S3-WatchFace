/* ============================================================================
 *  notif_archive_sd.h — unlimited notification history on the microSD card (CSV).
 *
 *  The flash store (notif_store.h) keeps only the newest 32 notifications with
 *  small fields (a fast, always-available cache used by the popup card, the wake
 *  path and the no-card fallback). When a card is present we ALSO append every
 *  notification to this SD archive, which is:
 *     - unlimited in count (append-only CSV file), and
 *     - much larger per item (title up to NA_TITLE_MAX, body up to NA_BODY_MAX).
 *  So the first 32 live in BOTH flash and SD; everything after the 32nd lives
 *  ONLY on SD (flash is a rolling newest-32 cache: once full it evicts its oldest
 *  to make room). With no card, flash is the whole story (rolling newest 32).
 *
 *  The Notifications app shows the SD archive when a card holds entries (the full
 *  history, newest-first), else the flash store.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER sd_card.h (mount) and
 *  notif_store.h (notif_store_unread_count, used by notif_unread). All na_* functions touch
 *  the SD card and shared counters; callers MUST already hold store_lock() (the
 *  network core appends while the UI core reads/removes) — these functions do NOT
 *  take the lock themselves (it isn't recursive).
 *
 *  ---- on-disk format: /notifications.csv, one record per line ---------------
 *      <id>,<read>,<title>,<body>\n
 *  `id` is a decimal uint64. `read` is a single '0' (unread) or '1' (opened in
 *  the reader). The two text fields are escaped so a record is always exactly
 *  one line and contains no raw commas — parsing is just "split on the first
 *  three commas". Escaping (text fields only):
 *      '\'  -> "\\"        ','  -> "\,"
 *      '\n' -> "\n"        '\r' -> "\r"
 *  Real commas therefore appear as \, in the file; everything round-trips
 *  losslessly. A half-written final line (no newline / bad id) is skipped.
 *
 *  Legacy: files written before the `read` column was added are `<id>,<title>,
 *  <body>`. Such lines are detected (2nd field isn't a bare 0/1) and treated as
 *  unread; they're upgraded to the new format the next time the file is rewritten
 *  (a dismiss or a mark-read).
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <FFat.h>
#include "esp_heap_caps.h"   // heap_caps_malloc — view/line buffers live in PSRAM
#include "sd_card.h"
#include "storage_fs.h"

/* ---- archive backend: SD card if present, else the on-flash FAT partition -----
 * The archive is the SAME append-only CSV regardless of where it lives. When a
 * microSD card is mounted we use it (effectively unlimited, removable). With no
 * card we fall back to the on-flash FAT partition (`ffat`, ~16 MB on this board) so
 * the FULL history still works WITHOUT a card. Both filesystems are rooted, so the
 * record path (NA_PATH = "/notifications.csv") is identical on either; only the
 * fs::FS object differs. The small NVS newest-32 cache (notif_store.h) is UNCHANGED
 * and still powers the instant wake/popup path with zero mount — this archive is the
 * bulk history layered on top.
 *
 * The SD-or-FFat selection itself now lives in storage_fs.h (shared with the WiFi and
 * battery-health stores). These na_* names are thin aliases kept so the rest of this
 * file reads unchanged. */
static inline bool       na_on_sd(void)     { return store_on_sd(); }
static inline fs::FS    &na_fs(void)        { return store_fs(); }

#define NA_PATH       "/notifications.csv"
#define NA_TMP_PATH   "/notifications.tmp"
#define NA_TITLE_MAX  96      // bytes incl NUL — bigger titles than flash (48)
#define NA_BODY_MAX   2048    // bytes incl NUL — MUCH bigger bodies than flash (96)
#define NA_PAGE_SIZE  100     // records shown per page in the Notifications app
#define NA_VIEW_MAX   NA_PAGE_SIZE  // in-memory view holds exactly one page (never more)
#define NA_PREVIEW    48      // one-line body preview kept per row in the list
// Worst-case encoded line: id(<=20) + read flag + 3 commas + fully-escaped
// title + body + NUL.
#define NA_LINE_MAX   (26 + 2 * NA_TITLE_MAX + 2 * NA_BODY_MAX)

/* One archived entry as loaded for the Notifications list. The body itself is NOT
 * held here (it can be 2 KB) — only a short preview; the full body is read on
 * demand (by id) when the user opens the item. */
struct NaViewItem {
  uint64_t id;
  bool     read;                  // true once opened in the reader
  uint8_t  cat;                   // NotifCat — per-app icon in the row
  char     title[NA_TITLE_MAX];
  char     preview[NA_PREVIEW];   // sanitized first chars of the body, for the row
};

/* Both working buffers live in PSRAM, not static SRAM (~20 KB of .bss freed for
 * the display's partial render buffers). Allocated on first file operation via
 * na_bufs_ok(); s_na_view reads elsewhere are guarded by s_na_view_count, which
 * only becomes non-zero after a successful alloc. */
static NaViewItem *s_na_view = nullptr;  // [NA_VIEW_MAX]
static uint16_t   s_na_view_count = 0;   // entries in s_na_view (newest-first)
static uint32_t   s_na_total      = 0;   // total records in the archive file
static uint32_t   s_na_unread     = 0;   // records with read==0 (bell badge count)
static char       *s_na_line = nullptr;  // [NA_LINE_MAX] shared line buffer (single-threaded under store_lock)

static bool na_bufs_ok(void) {
  if (!s_na_view)
    s_na_view = (NaViewItem *)heap_caps_calloc(NA_VIEW_MAX, sizeof(NaViewItem),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_na_line)
    s_na_line = (char *)heap_caps_malloc(NA_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s_na_view && s_na_line;
}

/* Is the archive usable right now? True if EITHER the SD card is mounted OR the
 * on-flash FAT partition mounts. SD is preferred; FFat is the no-card fallback. */
static inline bool na_available(void) { return store_available(); }

/* ---- escaping helpers ---------------------------------------------------- */

/* Stream one text field to the file, escaped (see header). */
static void na_write_escaped(File &f, const char *s) {
  for (; s && *s; ++s) {
    switch (*s) {
      case '\\': f.write((const uint8_t *)"\\\\", 2); break;
      case ',':  f.write((const uint8_t *)"\\,",  2); break;
      case '\n': f.write((const uint8_t *)"\\n",  2); break;
      case '\r': f.write((const uint8_t *)"\\r",  2); break;
      default:   f.write((uint8_t)*s);                break;
    }
  }
}

/* Decode an escaped field [s,end) into out (NUL-terminated, capped). */
static void na_decode(const char *s, const char *end, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = s; p < end && n < cap - 1; ++p) {
    char c = *p;
    if (c == '\\' && p + 1 < end) {
      char e = *++p;
      c = (e == 'n') ? '\n' : (e == 'r') ? '\r' : e;   // \\, \, , \n, \r
    }
    out[n++] = c;
  }
  out[n] = '\0';
}

/* Parse one archive line (in s_na_line, length `len`) into its fields. Handles
 * BOTH the current `id,read,title,body` layout and the legacy `id,title,body`
 * one. On success returns true and sets: *id, *read (legacy -> false/unread),
 * and *title_beg/*title_end/*body_beg pointing INTO s_na_line (the body runs to
 * s_na_line+len). Mutates s_na_line (writes a NUL over the id's comma). Returns
 * false for a malformed line (caller should skip it). */
static bool na_parse_line(int len, uint64_t *id, bool *read, uint8_t *cat,
                          char **title_beg, char **title_end, char **body_beg) {
  *cat = NCAT_GENERIC;             // default for legacy / no-cat lines
  char *c1 = strchr(s_na_line, ',');
  if (!c1) return false;
  char *c2 = strchr(c1 + 1, ',');
  if (!c2) return false;
  *c1 = '\0';
  if (s_na_line[0] < '0' || s_na_line[0] > '9') return false;
  *id = strtoull(s_na_line, nullptr, 10);

  // "new" format iff the field between c1 and c2 is a single bare '0'/'1' — the read
  // flag. Else it's a legacy `id,title,body` line (that field is the title).
  bool is_new = (c2 == c1 + 2) && (c1[1] == '0' || c1[1] == '1');
  if (is_new) {
    *read = (c1[1] == '1');
    // After the read flag, is the NEXT field a short bare integer? Then it's the cat
    // (format id,read,cat,title,body). Else this is the older id,read,title,body.
    char *c3 = strchr(c2 + 1, ',');
    bool has_cat = false;
    if (c3) {
      size_t flen = (size_t)(c3 - (c2 + 1));
      if (flen >= 1 && flen <= 2) {
        has_cat = true;
        for (char *p = c2 + 1; p < c3; p++) if (*p < '0' || *p > '9') { has_cat = false; break; }
      }
    }
    if (has_cat) {
      *cat = (uint8_t)atoi(c2 + 1);
      if (*cat >= NCAT_COUNT) *cat = NCAT_GENERIC;
      *title_beg = c3 + 1;
      char *c4 = strchr(c3 + 1, ',');
      if (!c4) return false;
      *title_end = c4;
      *body_beg  = c4 + 1;
    } else {
      *title_beg = c2 + 1;
      char *c3b = strchr(c2 + 1, ',');
      if (!c3b) return false;
      *title_end = c3b;
      *body_beg  = c3b + 1;
    }
  } else {
    *read = false;                 // legacy line: unread
    *title_beg = c1 + 1;
    *title_end = c2;
    *body_beg  = c2 + 1;
  }
  return true;
}

/* ---- file operations ----------------------------------------------------- */

/* Read one line (without the trailing newline) into s_na_line. Returns its length,
 * or -1 at end of file. The caller MUST have done f.setTimeout(0) first, else
 * readBytesUntil busy-waits a full second at EOF (Stream's default timeout). */
static int na_read_line(File &f) {
  if (!na_bufs_ok()) return -1;   // alloc failed -> behave like an empty file
  size_t n = f.readBytesUntil('\n', s_na_line, NA_LINE_MAX - 1);
  if (n == 0 && !f.available()) return -1;
  // Strip a trailing CR if the file ever has CRLF line endings.
  if (n > 0 && s_na_line[n - 1] == '\r') n--;
  s_na_line[n] = '\0';
  return (int)n;
}

/* Append one notification to the archive. No-op (returns false) if there's no
 * card. Returns true if a record was written. Dedup is the server's job: the
 * /notify endpoint DRAINS its queue on each GET, so every item we're handed is
 * new — we don't second-guess it with an id guard (that silently dropped items
 * whenever the server's ids weren't strictly increasing). */
static bool na_append(uint64_t id, const char *title, const char *body,
                      uint8_t cat = NCAT_GENERIC) {
  if (!na_available()) return false;

  File f = na_fs().open(NA_PATH, FILE_APPEND);      // creates the file if missing
  if (!f) return false;
  // Format: id,read,cat,title,body  (read=0 new; cat = NotifCat). Backward-compatible
  // with older id,read,title,body and legacy id,title,body lines — see na_parse_line.
  char hdr[40];
  int hn = snprintf(hdr, sizeof(hdr), "%llu,0,%u,", (unsigned long long)id, (unsigned)cat);
  f.write((const uint8_t *)hdr, hn);
  na_write_escaped(f, title);
  f.write((uint8_t)',');
  na_write_escaped(f, body);
  f.write((uint8_t)'\n');
  f.close();

  s_na_total++;
  s_na_unread++;
  return true;
}

/* Count records by scanning the file line by line. Returns the total and, when
 * `unread_out` is non-null, also the number of records whose read flag is 0
 * (legacy lines with no flag count as unread). Line-based (not a raw newline
 * scan) so it can read each record's read flag. */
static uint32_t na_count_records_ex(uint32_t *unread_out) {
  if (unread_out) *unread_out = 0;
  if (!na_available() || !na_fs().exists(NA_PATH)) return 0;
  File f = na_fs().open(NA_PATH, FILE_READ);
  if (!f) return 0;
  f.setTimeout(0);            // EOF returns immediately (see na_read_line)
  uint32_t total = 0, unread = 0;
  int len;
  while ((len = na_read_line(f)) >= 0) {
    if (len == 0) continue;
    uint64_t id; bool read; uint8_t cat; char *tb, *te, *bb;
    if (!na_parse_line(len, &id, &read, &cat, &tb, &te, &bb)) continue;
    total++;
    if (!read) unread++;
  }
  f.close();
  if (unread_out) *unread_out = unread;
  return total;
}
static uint32_t na_count_records(void) { return na_count_records_ex(nullptr); }

/* How many pages the archive spans (1 page = NA_PAGE_SIZE records). At least 1 so
 * an empty archive still has a valid page 0. Reads the cached s_na_total, so call
 * after na_load_view (or na_seed_total) has set it. */
static uint16_t na_page_count(void) {
  if (s_na_total == 0) return 1;
  return (uint16_t)((s_na_total + NA_PAGE_SIZE - 1) / NA_PAGE_SIZE);
}

/* Populate s_na_view[] with one PAGE of records (newest-first) and set
 * s_na_total / s_na_unread from the full file. Page 0 = the newest NA_PAGE_SIZE,
 * page 1 = the next-older NA_PAGE_SIZE, and so on. A page past the end loads
 * empty (s_na_view_count == 0); callers clamp the page to na_page_count(). */
static void na_load_view(uint16_t page) {
  s_na_view_count = 0;
  s_na_total      = 0;
  s_na_unread     = 0;
  if (!na_available() || !na_fs().exists(NA_PATH)) return;

  uint32_t unread = 0;
  uint32_t total  = na_count_records_ex(&unread);
  s_na_total  = total;
  s_na_unread = unread;

  // The page's records occupy, in NEWEST-first terms, [page*PAGE, page*PAGE+PAGE).
  // On disk records are OLDEST-first, so in disk order we skip the records that are
  // newer than this page (the pages before it) plus all the records older than the
  // file's start — i.e. skip = total - (newest index after this page).
  uint32_t newest_after = (uint32_t)page * NA_PAGE_SIZE + NA_PAGE_SIZE;  // exclusive, newest-first
  uint32_t take_newest  = (uint32_t)page * NA_PAGE_SIZE;                 // inclusive start, newest-first
  if (take_newest >= total) return;                 // page entirely past the end -> empty
  if (newest_after > total) newest_after = total;   // last page may be short
  uint32_t want = newest_after - take_newest;       // records on this page (<= NA_PAGE_SIZE)
  uint32_t skip = total - newest_after;             // disk-order records before this page's window

  File f = na_fs().open(NA_PATH, FILE_READ);
  if (!f) return;
  f.setTimeout(0);            // EOF returns immediately (see na_read_line)

  uint32_t seen = 0;
  uint16_t n = 0;
  int len;
  while ((len = na_read_line(f)) >= 0 && n < want) {
    if (len == 0) continue;
    if (seen < skip) { seen++; continue; }
    seen++;

    // Parse "id,read,cat,title,body" (or older id,read,title,body / legacy
    // id,title,body) out of s_na_line.
    uint64_t id; bool read; uint8_t cat; char *tb, *te, *bb;
    if (!na_parse_line(len, &id, &read, &cat, &tb, &te, &bb)) continue;
    NaViewItem *v = &s_na_view[n];
    v->id   = id;
    v->read = read;
    v->cat  = cat;
    na_decode(tb, te, v->title, sizeof(v->title));
    na_decode(bb, s_na_line + len, v->preview, sizeof(v->preview));
    for (char *p = v->preview; *p; ++p) if (*p == '\n' || *p == '\r') *p = ' ';
    n++;
  }
  f.close();

  for (uint16_t i = 0; i < n / 2; i++) {           // oldest->newest  =>  newest-first
    NaViewItem tmp = s_na_view[i];
    s_na_view[i] = s_na_view[n - 1 - i];
    s_na_view[n - 1 - i] = tmp;
  }
  s_na_view_count = n;
}

/* Read one archived body (by id) into `out` (always NUL-terminated). */
static void na_read_body(uint64_t id, char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!na_available() || !na_fs().exists(NA_PATH)) return;
  File f = na_fs().open(NA_PATH, FILE_READ);
  if (!f) return;
  f.setTimeout(0);            // EOF returns immediately (see na_read_line)
  int len;
  while ((len = na_read_line(f)) >= 0) {
    if (len == 0) continue;
    uint64_t rid; bool read; uint8_t cat; char *tb, *te, *bb;
    if (!na_parse_line(len, &rid, &read, &cat, &tb, &te, &bb)) continue;
    if (rid != id) continue;
    na_decode(bb, s_na_line + len, out, cap);
    break;
  }
  f.close();
}

/* Remove the record with id `id` by rewriting the file without that line. Returns
 * true if a line was dropped. Infrequent (user dismiss), so an O(n) rewrite is fine. */
static bool na_remove(uint64_t id) {
  if (!na_available() || !na_fs().exists(NA_PATH)) return false;
  File in = na_fs().open(NA_PATH, FILE_READ);
  if (!in) return false;
  in.setTimeout(0);           // EOF returns immediately (see na_read_line)
  na_fs().remove(NA_TMP_PATH);
  File out = na_fs().open(NA_TMP_PATH, FILE_WRITE);
  if (!out) { in.close(); return false; }

  bool removed = false;
  bool removed_unread = false;
  int len;
  while ((len = na_read_line(in)) >= 0) {
    if (len == 0) continue;
    // Peek the id and read flag WITHOUT mutating s_na_line, so the kept lines are
    // re-emitted byte-for-byte (strtoull/strchr don't write).
    uint64_t rid = strtoull(s_na_line, nullptr, 10);
    char *c1 = strchr(s_na_line, ',');
    bool rid_unread = false;
    if (c1) {
      char *c2 = strchr(c1 + 1, ',');
      bool is_new = c2 && (c2 == c1 + 2) && (c1[1] == '0' || c1[1] == '1');
      rid_unread = is_new ? (c1[1] == '0') : true;   // legacy line => unread
    }
    if (c1 && rid == id) { removed = true; removed_unread = rid_unread; continue; }  // drop this line
    out.write((const uint8_t *)s_na_line, len);
    out.write((uint8_t)'\n');
  }
  in.close();
  out.close();
  na_fs().remove(NA_PATH);
  na_fs().rename(NA_TMP_PATH, NA_PATH);
  if (removed && s_na_total) s_na_total--;
  if (removed_unread && s_na_unread) s_na_unread--;
  return removed;
}

/* Mark the record with id `id` as read by rewriting the file with its read flag
 * set to 1 (a legacy line is upgraded to the new format in the process). Returns
 * true if a record was flipped from unread to read. Infrequent (user opens an
 * item), so an O(n) rewrite — matching na_remove — is fine. */
static bool na_mark_read(uint64_t id) {
  if (!na_available() || !na_fs().exists(NA_PATH)) return false;
  File in = na_fs().open(NA_PATH, FILE_READ);
  if (!in) return false;
  in.setTimeout(0);           // EOF returns immediately (see na_read_line)
  na_fs().remove(NA_TMP_PATH);
  File out = na_fs().open(NA_TMP_PATH, FILE_WRITE);
  if (!out) { in.close(); return false; }

  bool flipped = false;
  int len;
  while ((len = na_read_line(in)) >= 0) {
    if (len == 0) continue;
    // Inspect id + read flag WITHOUT mutating s_na_line, so any line we don't
    // rewrite is re-emitted byte-for-byte (mirrors na_remove). Only the matching,
    // currently-unread record is rewritten; everything else passes through verbatim.
    // Cheap, non-mutating peek: is this our id and currently unread? Only then do we
    // re-parse + rewrite (which preserves the cat field via na_parse_line). Every
    // other line is emitted byte-for-byte (na_parse_line mutates s_na_line, so we must
    // NOT call it on pass-through lines).
    uint64_t rid = strtoull(s_na_line, nullptr, 10);
    char *c1 = strchr(s_na_line, ',');
    bool is_ours_unread = false;
    if (c1 && rid == id) {
      char *c2 = strchr(c1 + 1, ',');
      bool is_new = c2 && (c2 == c1 + 2) && (c1[1] == '0' || c1[1] == '1');
      bool cur_read = is_new ? (c1[1] == '1') : false;   // legacy => unread
      is_ours_unread = !cur_read;
    }
    bool rewritten = false;
    if (is_ours_unread) {
      uint64_t pid; bool pr; uint8_t pcat; char *tb, *te, *bb;
      // Re-parse to get cat + title/body slices (handles all line formats). Falls
      // through to verbatim if parsing somehow fails.
      if (na_parse_line(len, &pid, &pr, &pcat, &tb, &te, &bb)) {
        flipped = true;
        rewritten = true;
        char hdr[40];
        int hn = snprintf(hdr, sizeof(hdr), "%llu,1,%u,", (unsigned long long)id, (unsigned)pcat);
        out.write((const uint8_t *)hdr, hn);                              // id,1,cat,
        out.write((const uint8_t *)tb, (size_t)(te - tb));                // escaped title
        out.write((uint8_t)',');
        out.write((const uint8_t *)bb, (size_t)((s_na_line + len) - bb)); // escaped body
        out.write((uint8_t)'\n');
      }
    }
    if (!rewritten && !is_ours_unread) {
      // Normal pass-through: na_parse_line was NOT called on this line, so s_na_line
      // is intact — emit it byte-for-byte.
      out.write((const uint8_t *)s_na_line, len);
      out.write((uint8_t)'\n');
    }
    // (is_ours_unread but parse failed -> malformed line for our id; drop it. Can't
    // happen for a well-formed record, and emitting the na_parse_line-mutated buffer
    // would corrupt the file.)
  }
  in.close();
  out.close();
  na_fs().remove(NA_PATH);
  na_fs().rename(NA_TMP_PATH, NA_PATH);
  if (flipped && s_na_unread) s_na_unread--;
  return flipped;
}

/* Wipe the whole archive (the id high-water mark is kept, so a drained server
 * queue can't re-deliver the cleared items). */
static void na_clear(void) {
  if (!na_available()) return;
  na_fs().remove(NA_PATH);
  s_na_total      = 0;
  s_na_unread     = 0;
  s_na_view_count = 0;
}

/* Seed s_na_total / s_na_unread from the archive on disk (call once at full boot
 * so the bell count is right before the app is ever opened). */
static void na_seed_total(void) {
  if (na_available()) { uint32_t u = 0; s_na_total = na_count_records_ex(&u); s_na_unread = u; }
}

/* Unread notification count for the bell badge: the SD archive when a card is
 * present (the real history), else the flash store. */
static uint32_t notif_unread(void) {
  return na_available() ? s_na_unread : notif_store_unread_count();
}
