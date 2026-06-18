/*
 * Disco Boy - music player for Leaf, a native Catastrophe app.
 *
 * Two views, both on the Leaf box model:
 *   Library     - tracklist (left 58%) + a now-playing art panel (right 42%),
 *                 the same column split the launcher's browse pages use.
 *   Now Playing - a focused view: large album art, metadata, scrubber, transport.
 * Album art is a sidecar cover.png / folder.jpg (etc.) in the track's folder.
 * Title/artist/album/year come from tags (ID3 / Vorbis comments); per-track
 * durations and tags are read on a background thread after the folder scan.
 *
 * Controls:
 *   Up/Down   Library: move the track list   Now Playing: move between transport rows
 *   Left/Right Now Playing: move the transport focus
 *   A         Library: play the highlighted track   Now Playing: activate focused control
 *   X         play / pause
 *   Y         switch between Library and Now Playing
 *   L1/R1     previous / next track
 *   B         Library: quit   Now Playing: back to the list
 * Now-Playing transport: rewind / play-pause / fast-forward (seek) on the top row,
 * shuffle / repeat below. Transport glyphs use a bundled Material Icons subset.
 */

#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "disco_audio.h"
#include "disco_ff.h"
#include "disco_meta.h"
#include "disco_status.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DISCO_MAX_TRACKS 2048
#define DISCO_MAX_PATH   1024

#define DISCO_SEEK_STEP  5.0    /* seconds per scrub step */
#define DISCO_SCRUB_MS   130    /* repeat interval while a scrub trigger is held */

/* Material Icons codepoints (UTF-8), rendered from the bundled subset font. */
#define ICON_REWIND  "\xEE\x80\xA0"   /* fast_rewind  E020 */
#define ICON_FORWARD "\xEE\x80\x9F"   /* fast_forward E01F */
#define ICON_PLAY    "\xEE\x80\xB7"   /* play_arrow   E037 */
#define ICON_PAUSE   "\xEE\x80\xB4"   /* pause        E034 */
#define ICON_SHUFFLE "\xEE\x81\x83"   /* shuffle      E043 */
#define ICON_REPEAT  "\xEE\x81\x80"   /* repeat       E040 */
#define ICON_REPEAT1 "\xEE\x81\x81"   /* repeat_one   E041 */
#define ICON_PREV    "\xEE\x81\x85"   /* skip_previous E045 */
#define ICON_NEXT    "\xEE\x81\x84"   /* skip_next     E044 */

typedef enum { VIEW_LIBRARY = 0, VIEW_NOWPLAYING } disco_view;
typedef enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE } disco_repeat;

/* Browse tabs in the Library view (L1/R1 switches). */
typedef enum { TAB_ARTISTS = 0, TAB_ALBUMS, TAB_FOLDERS, TAB_COUNT } disco_tab;
static const char *DISCO_TAB_NAMES[TAB_COUNT] = { "Artists", "Albums", "Folders" };

typedef struct {
    char         name[256];          /* raw filename, used for sort order */
    char         title[256];         /* prettified filename (fallback title) */
    char         path[DISCO_MAX_PATH];
    disco_meta   meta;               /* tags + duration, filled by the worker */
    long long    mtime;              /* file mtime + size: cache key (set by worker) */
    long long    size;
    volatile int meta_ready;         /* 0 = pending, 1 = meta is valid to read */
} disco_track;

/* A row in the current browse list: a playable track, or an album / folder you
   drill into. Albums and folders are far fewer than tracks, so the array is
   sized to the track ceiling and the heavier per-row text is kept small. */
typedef enum { ROW_TRACK = 0, ROW_ALBUM, ROW_FOLDER, ROW_ARTIST } disco_rowtype;
typedef struct {
    disco_rowtype type;
    int  ref;            /* ROW_TRACK: g.tracks idx; ROW_ALBUM: album idx; ROW_ARTIST: artist idx */
    char label[224];     /* album / folder / artist display name (tracks render from tags) */
    char sub[128];       /* album artist / track count */
} disco_row;

/* An album = a group of track indices sharing an album tag, sorted by track #. */
typedef struct {
    char album[160];
    char artist[160];
    int  tracks[256];
    int  count;
    int  art_track;      /* a member track to resolve cover art from */
    char cover[512];     /* resolved cover path (cached); cover_state gates it */
    signed char cover_state;  /* 0 = unresolved, 1 = have cover, -1 = none */
} disco_album;

/* An artist = a group of albums sharing an artist tag. */
typedef struct {
    char artist[160];
    int  albums[256];    /* indices into g_albums */
    int  album_count;
    int  track_count;
} disco_artist;

static struct {
    char           music_dir[DISCO_MAX_PATH];
    disco_track    tracks[DISCO_MAX_TRACKS];
    int            count;
    cat_list_state list;
    int            now_playing;            /* index, or -1 */
    bool           paused;
    disco_view     view;
    bool           art_only;               /* SELECT: full-screen cover, no chrome */
    bool           locked;                 /* pocket lock: stick-click blanks screen + swallows input */
    uint32_t       lock_anim_until;        /* ms ticks: flash the lock until here, then power the panel off */
    bool           screen_off;             /* backlight currently powered down (bl_power) */
    disco_tab      tab;                    /* current browse tab */
    int            np_focus;               /* transport focus 0 rw, 1 play, 2 ff, 3 shuffle, 4 repeat */
    bool           shuffle;
    disco_repeat   repeat;
    SDL_Texture   *art;                    /* cover for now_playing's folder, or NULL */
    char           art_dir[DISCO_MAX_PATH];/* folder the cover was last resolved for */
    /* browse state */
    char           folder_dir[DISCO_MAX_PATH]; /* Folders tab: current directory */
    int            artist_open;            /* Artists tab: drilled-in artist idx, or -1 */
    int            album_open;             /* drilled-in album idx, or -1 */
} g;

/* Current browse rows (rebuilt when the tab / folder / album changes). */
static disco_row  g_rows[DISCO_MAX_TRACKS];
static int        g_rowcount = 0;

/* Album + artist indices (built from tags once metadata is ready). */
static disco_album  g_albums[512];
static int          g_album_count = 0;
static bool         g_albums_built = false;
static disco_artist g_artists[512];
static int          g_artist_count = 0;

/* Play queue: track indices in play order; playback walks this, so next/prev/
   shuffle/auto-advance follow the context you played from (album, folder, all). */
static int  g_queue[DISCO_MAX_TRACKS];
static int  g_queue_len = 0;
static int  g_queue_pos = 0;

/* background metadata worker */
static pthread_t     g_meta_thread;
static bool          g_meta_running = false;
static volatile bool g_meta_stop = false;
static volatile bool g_meta_done = false;

/* live audio status (polled from the daemon on a background thread so the IPC
   round-trip never hitches the render loop / marquee scroll). */
static char            g_output[16] = "";  /* current output name, or "" before first poll */
static char            g_bt_name[64] = "";  /* connected BT device friendly name (when on BT) */
static int             g_volume = -1;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_poll_thread;
static bool            g_poll_running = false;
static volatile bool   g_poll_stop = false;

/* L2/R2 hold-to-scrub: triggers don't auto-repeat, so we seek on a timer while held */
static int      g_scrub = 0;               /* -1 back, +1 forward, 0 idle */
static uint32_t g_scrub_next = 0;          /* next scrub-seek deadline (ms) */

/* bundled Material Icons subset, opened at two sizes for the transport */
static TTF_Font *g_icons = NULL;
static TTF_Font *g_icons_sm = NULL;

/* the app draws its own hint bar at a fixed size (the launcher's font-size setting
   bumps every tier, so Leaf's footer overflows at large sizes) - opened once from
   the theme font at an unbumped small size. NULL => fall back to cat_draw_footer. */
static TTF_Font *g_hint_font = NULL;

/* Disco Boy wordmark for the title band (white, tinted to the theme text color). */
static SDL_Texture *g_header = NULL;

/* Vinyl-record placeholder shown when a track has no cover art. */
static SDL_Texture *g_record = NULL;

/* marquee state for overflowing text + per-frame delta. One state per text that
   can scroll at the same time on screen (selected row title + its band line; the
   panel's title + band + album), so they advance independently. */
static cat_marquee g_mq_row, g_mq_row_sub, g_mq_title, g_mq_artist, g_mq_album, g_mq_header;
static int         g_mq_row_key = -1, g_mq_np_key = -2;
static uint32_t    g_frame_ms = 0, g_dt = 0;
static bool        g_mq_anim = false;   /* set by any marquee still scrolling this frame */

/* ---- filename / format helpers ---- */

static bool disco_is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    /* fast vendored decoders */
    static const char *exts[] = { ".mp3", ".flac", ".ogg", ".oga", ".wav", NULL };
    for (int i = 0; exts[i]; i++) if (strcasecmp(dot, exts[i]) == 0) return true;
    return disco_ff_handles(name);   /* m4a/aac/opus/wma/aiff/... via FFmpeg */
}

static const char *disco_format_label(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "";
    if (!strcasecmp(dot, ".wav"))  return "WAV";
    if (!strcasecmp(dot, ".mp3"))  return "MP3";
    if (!strcasecmp(dot, ".flac")) return "FLAC";
    if (!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga")) return "OGG";
    if (!strcasecmp(dot, ".opus")) return "OPUS";
    if (!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".m4b") ||
        !strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".alac")) return "M4A";
    if (!strcasecmp(dot, ".aac"))  return "AAC";
    if (!strcasecmp(dot, ".wma"))  return "WMA";
    if (!strcasecmp(dot, ".aif") || !strcasecmp(dot, ".aiff")) return "AIFF";
    if (!strcasecmp(dot, ".ape"))  return "APE";
    if (!strcasecmp(dot, ".wv"))   return "WV";
    if (!strcasecmp(dot, ".mka"))  return "MKA";
    if (!strcasecmp(dot, ".tta"))  return "TTA";
    if (!strcasecmp(dot, ".ac3"))  return "AC3";
    return "";
}

/* Turn a filename into a display title: drop the extension, a leading "NN - "
   track-number prefix, and a trailing " (...)" parenthetical. */
static void disco_prettify(const char *filename, char *out, size_t n) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", filename);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
    char *s = buf;
    char *p = s;
    while (*p >= '0' && *p <= '9') p++;
    if (p > s) {
        char *q = p;
        while (*q == ' ') q++;
        if (*q == '-' || *q == '.') {
            q++;
            while (*q == ' ') q++;
            s = q;
        }
    }
    size_t len = strlen(s);
    if (len && s[len - 1] == ')') {
        char *op = strrchr(s, '(');
        if (op && op > s && op[-1] == ' ') op[-1] = '\0';
    }
    if (!*s) s = buf;
    snprintf(out, n, "%s", s);
}

/* Sort by full path so album folders group together and "NN " track order holds. */
static int disco_cmp(const void *a, const void *b) {
    return strcasecmp(((const disco_track *)a)->path, ((const disco_track *)b)->path);
}

/* Recurse a folder tree, collecting audio files (albums in subfolders are found,
   and per-folder cover art / folder labels then work for each album). */
static void disco_scan_dir(const char *dir, int depth) {
    if (depth > 6 || g.count >= DISCO_MAX_TRACKS) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g.count < DISCO_MAX_TRACKS) {
        if (e->d_name[0] == '.') continue;
        char full[DISCO_MAX_PATH];
        if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, e->d_name) >= sizeof(full)) continue;

        bool is_dir = false, is_reg = false;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) is_dir = true;
        else if (e->d_type == DT_REG) is_reg = true;
        else
#endif
        {
            struct stat st;
            if (stat(full, &st) == 0) { is_dir = S_ISDIR(st.st_mode); is_reg = S_ISREG(st.st_mode); }
        }

        if (is_dir) {
            disco_scan_dir(full, depth + 1);
        } else if (is_reg && disco_is_audio(e->d_name)) {
            disco_track *t = &g.tracks[g.count];
            snprintf(t->path, sizeof(t->path), "%s", full);
            snprintf(t->name, sizeof(t->name), "%s", e->d_name);
            disco_prettify(e->d_name, t->title, sizeof(t->title));
            t->meta_ready = 0;
            t->mtime = 0; t->size = 0;
            g.count++;
        }
    }
    closedir(d);
}

static void disco_scan(const char *dir) {
    g.count = 0;
    disco_scan_dir(dir, 0);
    qsort(g.tracks, (size_t)g.count, sizeof(disco_track), disco_cmp);
}

/* music dir: $MUSIC_PATH, then $SDCARD_PATH/Music, then ./Music */
static void disco_resolve_music_dir(char *out, size_t n) {
    const char *m = getenv("MUSIC_PATH");
    const char *sd = getenv("SDCARD_PATH");
    if (m && m[0])       snprintf(out, n, "%s", m);
    else if (sd && sd[0]) snprintf(out, n, "%s/Music", sd);
    else                  snprintf(out, n, "Music");
    /* drop a trailing slash so path-prefix compares (grandparent fallback) hold */
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = '\0';
}

/* ---- on-disk metadata cache --------------------------------------------------

   Reading tags + duration for a whole library is the slow part of startup (each
   file is an FFmpeg/decoder open), and it's identical work every launch. So the
   worker persists what it read to a small cache file keyed by path + mtime + size:
   on the next launch a file whose mtime/size still match is served from the cache
   (no decoder open), and only new or changed files are re-read. First launch pays
   the full scan once; every launch after is near-instant and only touches what
   actually changed. The cache lives on the SD (persists across reboots, unlike
   /tmp); a read/write failure (e.g. a read-only card) just falls back to a full
   scan, so it's purely an optimization. */

#define DISCO_CACHE_MAGIC 0x31434244u   /* "DBC1" */
#define DISCO_CACHE_VER   1u

typedef struct {
    char       path[DISCO_MAX_PATH];
    long long  mtime;
    long long  size;
    disco_meta meta;
} disco_cache_rec;

static disco_cache_rec *g_cache = NULL;
static int              g_cache_count = 0;

/* Resolve the cache file path under a hidden .discoboy dir on the SD (or, with no
   SDCARD_PATH, beside the music dir — that dotdir is skipped by the scan). */
static bool disco_cache_file(char *out, size_t n) {
    const char *sd = getenv("SDCARD_PATH");
    const char *base = (sd && sd[0]) ? sd : g.music_dir;
    char dir[DISCO_MAX_PATH + 16];
    if ((size_t)snprintf(dir, sizeof(dir), "%s/.discoboy", base) >= sizeof(dir)) return false;
    mkdir(dir, 0777);   /* best effort; harmless if it already exists */
    return (size_t)snprintf(out, n, "%s/library.cache", dir) < n;
}

static void disco_cache_load(void) {
    g_cache = NULL; g_cache_count = 0;
    char path[DISCO_MAX_PATH];
    if (!disco_cache_file(path, sizeof(path))) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t magic = 0, ver = 0, msz = 0, count = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != DISCO_CACHE_MAGIC ||
        fread(&ver,   4, 1, f) != 1 || ver   != DISCO_CACHE_VER   ||
        fread(&msz,   4, 1, f) != 1 || msz   != (uint32_t)sizeof(disco_meta) ||
        fread(&count, 4, 1, f) != 1 || count == 0 || count > DISCO_MAX_TRACKS) {
        fclose(f); return;   /* missing/old/foreign cache -> ignore, rescan */
    }
    g_cache = calloc(count, sizeof(disco_cache_rec));
    if (!g_cache) { fclose(f); return; }
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t plen = 0;
        if (fread(&plen, 4, 1, f) != 1 || plen == 0 || plen >= DISCO_MAX_PATH) break;
        disco_cache_rec *r = &g_cache[n];
        if (fread(r->path, 1, plen, f) != plen) break;
        r->path[plen] = '\0';
        if (fread(&r->mtime, 8, 1, f) != 1) break;
        if (fread(&r->size,  8, 1, f) != 1) break;
        if (fread(&r->meta, sizeof(disco_meta), 1, f) != 1) break;
        n++;
    }
    g_cache_count = n;
    fclose(f);
}

static void disco_cache_free(void) {
    free(g_cache); g_cache = NULL; g_cache_count = 0;
}

/* Look up `path` in the loaded cache. `*hint` rolls forward from the last hit so a
   cache that still lines up with the (sorted) track list costs O(1) per lookup.
   Returns 1 and fills *out only when path AND mtime AND size all match. */
static int disco_cache_lookup(int *hint, const char *path,
                              long long mtime, long long size, disco_meta *out) {
    for (int k = 0; k < g_cache_count; k++) {
        int idx = (*hint + k) % g_cache_count;
        if (strcmp(g_cache[idx].path, path) != 0) continue;
        *hint = idx + 1;
        if (g_cache[idx].mtime == mtime && g_cache[idx].size == size) {
            *out = g_cache[idx].meta;
            return 1;
        }
        return 0;   /* same file, changed on disk -> re-read */
    }
    return 0;
}

/* Persist every track's tags + cache key, written to a temp file then renamed so a
   crash mid-write can't leave a torn cache. Only the worker calls this, and only
   after a full pass, so every track's meta is valid. */
static void disco_cache_save(void) {
    char path[DISCO_MAX_PATH], tmp[DISCO_MAX_PATH];
    if (!disco_cache_file(path, sizeof(path))) return;
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) return;
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint32_t magic = DISCO_CACHE_MAGIC, ver = DISCO_CACHE_VER,
             msz = (uint32_t)sizeof(disco_meta), count = (uint32_t)g.count;
    bool ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(&ver,   4, 1, f) == 1 &&
              fwrite(&msz,   4, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1;
    for (int i = 0; ok && i < g.count; i++) {
        const disco_track *t = &g.tracks[i];
        uint32_t plen = (uint32_t)strlen(t->path);
        ok = fwrite(&plen, 4, 1, f) == 1 && fwrite(t->path, 1, plen, f) == plen &&
             fwrite(&t->mtime, 8, 1, f) == 1 && fwrite(&t->size, 8, 1, f) == 1 &&
             fwrite(&t->meta, sizeof(disco_meta), 1, f) == 1;
    }
    if (fclose(f) != 0) ok = false;
    if (ok) rename(tmp, path);
    else    unlink(tmp);
}

/* Background worker: fill duration + tags for every track, served from the cache
   when the file is unchanged and read from the decoder otherwise; publish each
   track as it's ready, then persist the cache if anything changed. */
static void *disco_meta_worker(void *arg) {
    (void)arg;
    disco_cache_load();
    bool any_miss = false;
    int  hint = 0;
    for (int i = 0; i < g.count && !g_meta_stop; i++) {
        disco_track *t = &g.tracks[i];
        struct stat st;
        if (stat(t->path, &st) == 0) {
            t->mtime = (long long)st.st_mtime;
            t->size  = (long long)st.st_size;
        }
        disco_meta m;
        if (!disco_cache_lookup(&hint, t->path, t->mtime, t->size, &m)) {
            disco_meta_read(t->path, &m);
            any_miss = true;
        }
        t->meta = m;                   /* fill fields... */
        __sync_synchronize();          /* ...then publish the ready flag */
        t->meta_ready = 1;
    }
    /* Save only after a complete pass (every track read), and only when something
       changed vs the cache on disk — so a steady-state launch writes nothing, and
       an interrupted run never shrinks a good cache. */
    if (!g_meta_stop && (any_miss || g_cache_count != g.count))
        disco_cache_save();
    disco_cache_free();
    g_meta_done = true;
    return NULL;
}

/* Best-effort friendly name of the connected Bluetooth audio device, read from
   bluealsa (the same service Disco Boy routes BT audio through, so it reflects
   exactly the device producing sound). Standalone: no dependency on the daemon.
   `bluealsa-aplay -l` prints e.g.  "hci0: AA:.. [OpenRun Pro by Shokz], ..." —
   we take the first bracketed name. Empty on failure. Runs off the UI thread. */
static void disco_resolve_bt_name(char *out, size_t n) {
    out[0] = '\0';
    FILE *fp = popen("/usr/bin/bluealsa-aplay -l 2>/dev/null", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *lb = strchr(line, '[');
        char *rb = lb ? strchr(lb, ']') : NULL;
        if (lb && rb && rb > lb + 1) {
            size_t len = (size_t)(rb - lb - 1);
            if (len >= n) len = n - 1;
            memcpy(out, lb + 1, len);
            out[len] = '\0';
            break;
        }
    }
    pclose(fp);
}

/* Background worker: poll the daemon for the live audio output + volume every
   ~500ms. Off the UI thread so the blocking socket round-trip can't stall the
   render loop — that periodic stall was a visible hitch in the marquee scroll. */
static void *disco_status_worker(void *arg) {
    (void)arg;
    while (!g_poll_stop) {
        disco_audio_status as;
        if (disco_status_query(&as)) {
            if (as.output[0]) {
                pthread_mutex_lock(&g_status_lock);
                snprintf(g_output, sizeof(g_output), "%s", as.output);
                pthread_mutex_unlock(&g_status_lock);
                disco_audio_set_output(as.output);   /* cheap: flags a reopen only on change */

                /* Resolve the BT device name lazily while on Bluetooth; clear it
                   otherwise so a later (re)connect re-resolves a fresh name. */
                bool is_bt = (strcasecmp(as.output, "BLUETOOTH") == 0), need;
                pthread_mutex_lock(&g_status_lock);
                if (!is_bt) g_bt_name[0] = '\0';
                need = is_bt && g_bt_name[0] == '\0';
                pthread_mutex_unlock(&g_status_lock);
                if (need) {
                    char nm[64];
                    disco_resolve_bt_name(nm, sizeof(nm));
                    if (nm[0]) {
                        pthread_mutex_lock(&g_status_lock);
                        snprintf(g_bt_name, sizeof(g_bt_name), "%s", nm);
                        pthread_mutex_unlock(&g_status_lock);
                    }
                }
            }
            if (as.volume >= 0) { g_volume = as.volume; disco_audio_set_volume(as.volume); }
        }
        for (int i = 0; i < 10 && !g_poll_stop; i++) usleep(50 * 1000);  /* ~500ms, responsive stop */
    }
    return NULL;
}

/* ---- per-track display fields (tag if ready, else filename/folder fallback) ---- */

static const char *disco_track_title(const disco_track *t) {
    bool r = t->meta_ready;
    if (r) __sync_synchronize();
    return (r && t->meta.title[0]) ? t->meta.title : t->title;
}
static const char *disco_track_artist(const disco_track *t) {
    bool r = t->meta_ready;
    if (r) __sync_synchronize();
    return (r && t->meta.artist[0]) ? t->meta.artist : "";
}
static const char *disco_track_year(const disco_track *t) {
    bool r = t->meta_ready;
    if (r) __sync_synchronize();
    return (r && t->meta.year[0]) ? t->meta.year : "";
}
static double disco_track_duration(const disco_track *t) {
    bool r = t->meta_ready;
    if (r) __sync_synchronize();
    return r ? t->meta.duration : 0.0;
}
static int disco_track_num(const disco_track *t) {
    bool r = t->meta_ready;
    if (r) __sync_synchronize();
    return r ? t->meta.track : 0;
}

/* Folder name of the now-playing track (album-tag fallback / library hint). */
static const char *disco_folder_label(void) {
    if (g.now_playing < 0) return "";
    if (strcmp(g.art_dir, g.music_dir) == 0) return "";
    const char *slash = strrchr(g.art_dir, '/');
    return slash && slash[1] ? slash + 1 : "";
}
static const char *disco_track_album(const disco_track *t) {
    bool r = t->meta_ready;
    if (r) __sync_synchronize();
    if (r && t->meta.album[0]) return t->meta.album;
    return disco_folder_label();
}

static const char *disco_output_label(void) {
    static char snap[16];                  /* UI-thread-only copy of the worker's g_output */
    pthread_mutex_lock(&g_status_lock);
    snprintf(snap, sizeof(snap), "%s", g_output);
    pthread_mutex_unlock(&g_status_lock);
    const char *o = snap[0] ? snap : getenv("JAWAKA_AUDIO_OUTPUT");
    if (!o || !o[0]) o = getenv("UMRK_AUDIO_OUTPUT");
    if (o) {
        if (!strcasecmp(o, "BLUETOOTH")) {
            static char bt[64];            /* the connected device's name, if known */
            pthread_mutex_lock(&g_status_lock);
            snprintf(bt, sizeof(bt), "%s", g_bt_name);
            pthread_mutex_unlock(&g_status_lock);
            return bt[0] ? bt : "Bluetooth";
        }
        if (!strcasecmp(o, "HEADSET"))   return "Headphones";
        if (!strcasecmp(o, "HDMI"))      return "HDMI";
        if (!strcasecmp(o, "SPEAKER"))   return "Speaker";
    }
    return "Speaker";
}

/* ---- playback ---- */

/* Embedded cover (ID3 APIC / FLAC PICTURE / MP4 covr) extracted from `track_path`
   into a /tmp cache file keyed by the TRACK path (embedded art is per-track — a
   folder like the Music root can hold files with different art). Returns its path.
   A negative result is cached too, so an art-less track is FFmpeg-opened once. */
static bool disco_embedded_cover(const char *track_path, char *out, size_t n) {
    unsigned h = 2166136261u;
    for (const char *p = track_path; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    char img[64], none[64];
    snprintf(img,  sizeof(img),  "/tmp/discoboy-emb-%08x.img",  h);
    snprintf(none, sizeof(none), "/tmp/discoboy-emb-%08x.none", h);
    if (access(img,  R_OK) == 0) { snprintf(out, n, "%s", img); return true; }
    if (access(none, R_OK) == 0) { out[0] = '\0'; return false; }

    int sz = 0;
    unsigned char *data = disco_ff_cover(track_path, &sz);
    if (data && sz > 0) {
        FILE *f = fopen(img, "wb");
        bool ok = f && fwrite(data, 1, (size_t)sz, f) == (size_t)sz;
        if (f) fclose(f);
        free(data);
        if (ok) { snprintf(out, n, "%s", img); return true; }
    } else {
        free(data);
    }
    FILE *m = fopen(none, "w"); if (m) fclose(m);   /* remember: no embedded art here */
    out[0] = '\0';
    return false;
}

/* Resolve a cover image for `track_path`: first a sidecar in its folder (scanned
   case-insensitively — the SD's FAT mount is case-sensitive to access(), so a
   literal "folder.jpg" misses the common "Folder.jpg"), then the file's own
   embedded art. Returns true + fills `out`; false if none. */
static bool disco_folder_cover(const char *track_path, char *out, size_t n) {
    char dir[DISCO_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", track_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");

    DIR *d = opendir(dir);
    if (!d) { out[0] = '\0'; return false; }
    static const char *stems[] = { "cover", "folder", "front", "album", NULL };
    char best[256] = "";
    int  best_rank = 999;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        const char *dot = strrchr(name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png")) continue;
        int len = (int)(dot - name);
        for (int s = 0; stems[s]; s++) {
            if ((int)strlen(stems[s]) == len && strncasecmp(name, stems[s], (size_t)len) == 0) {
                if (s < best_rank) { best_rank = s; snprintf(best, sizeof(best), "%s", name); }
                break;
            }
        }
    }
    closedir(d);
    if (!best[0]) return disco_embedded_cover(track_path, out, n);   /* try embedded art */
    size_t dl = strlen(dir);
    if (dl + 2 >= n) { out[0] = '\0'; return false; }   /* no room for "/x" */
    memcpy(out, dir, dl);
    out[dl] = '/';
    size_t bl = strlen(best);
    if (bl > n - dl - 2) bl = n - dl - 2;
    memcpy(out + dl + 1, best, bl);
    out[dl + 1 + bl] = '\0';
    return true;
}

/* Now-playing cover: full-resolution (it's the focal image), reloaded only when
   the folder changes. */
static void disco_load_art(const char *track_path) {
    char dir[DISCO_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", track_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    if (strcmp(dir, g.art_dir) == 0) return;
    if (g.art) { SDL_DestroyTexture(g.art); g.art = NULL; }
    snprintf(g.art_dir, sizeof(g.art_dir), "%s", dir);

    char cover[DISCO_MAX_PATH];
    if (disco_folder_cover(track_path, cover, sizeof(cover)))
        g.art = cat_load_image(cover);
}

/* Resolve (once, then cache) an album's cover path — a sidecar or extracted
   embedded art. The album list renders at 60fps while scrolling, so this must
   not re-scan the folder every frame. Returns the path, or NULL if none. */
static const char *disco_album_cover_path(int ai) {
    if (ai < 0 || ai >= g_album_count) return NULL;
    disco_album *al = &g_albums[ai];
    if (al->cover_state == 0)
        al->cover_state = disco_folder_cover(g.tracks[al->art_track].path,
                                             al->cover, sizeof(al->cover)) ? 1 : -1;
    return al->cover_state == 1 ? al->cover : NULL;
}

/* Album-list thumbnail: downscaled + disk/memory cached (many on screen at once).
   Returns a cached texture owned by the cache, or NULL. */
static SDL_Texture *disco_album_thumb(int ai) {
    const char *cover = disco_album_cover_path(ai);
    if (!cover) return NULL;
    SDL_Texture *t = cat_cache_get(cover, NULL, NULL);
    if (t) return t;
    /* per-cover thumbnail in /tmp (FNV-1a of the path keeps the name unique) */
    unsigned hsh = 2166136261u;
    for (const char *p = cover; *p; p++) { hsh ^= (unsigned char)*p; hsh *= 16777619u; }
    char thumb[64];
    snprintf(thumb, sizeof(thumb), "/tmp/discoboy-thumb-%08x.png", hsh);
    int w = 0, h = 0;
    t = cat_load_image_thumbnail(cover, thumb, 256, &w, &h);
    if (t) cat_cache_put(cover, t, w, h);
    return t;
}

/* Drilled-in album hero: full-resolution cover for the album header (one at a
   time, like now-playing). Reloaded only when the open album changes. */
static SDL_Texture *g_album_hero = NULL;
static int          g_album_hero_idx = -1;
static SDL_Texture *disco_album_hero(int ai) {
    if (ai != g_album_hero_idx) {
        if (g_album_hero) { SDL_DestroyTexture(g_album_hero); g_album_hero = NULL; }
        g_album_hero_idx = ai;
        const char *cover = disco_album_cover_path(ai);
        if (cover) g_album_hero = cat_load_image(cover);
    }
    return g_album_hero;
}

/* Low-level: start playing g.tracks[idx] (no queue change). */
static void disco_play_idx(int idx) {
    if (idx < 0 || idx >= g.count) return;
    g.now_playing = idx;
    g.paused = false;
    disco_load_art(g.tracks[idx].path);
    if (!disco_audio_play(g.tracks[idx].path))
        cat_log("discoboy: could not play %s", g.tracks[idx].path);
}

/* Play track `list[pos]`, setting the play queue to `list` (the context you
   played from). next/prev/shuffle/auto-advance then walk this queue. */
static void disco_play_from(const int *list, int n, int pos) {
    if (n <= 0 || !list) return;
    if (n > DISCO_MAX_TRACKS) n = DISCO_MAX_TRACKS;
    if (pos < 0) pos = 0;
    if (pos >= n) pos = n - 1;
    memcpy(g_queue, list, (size_t)n * sizeof(int));
    g_queue_len = n;
    g_queue_pos = pos;
    disco_play_idx(g_queue[pos]);
}

/* A random queue position other than the current one. */
static int disco_random_other_pos(void) {
    if (g_queue_len <= 1) return g_queue_pos;
    int p;
    do { p = rand() % g_queue_len; } while (p == g_queue_pos);
    return p;
}

/* Advance within the queue. `autoadv` = a track just ended (honors repeat-one and
   stops at the end when repeat is off); a manual Next ignores repeat-one. */
static void disco_next(bool autoadv) {
    if (g_queue_len <= 0) return;
    if (autoadv && g.repeat == REPEAT_ONE) { disco_play_idx(g_queue[g_queue_pos]); return; }
    int pos;
    if (g.shuffle) {
        pos = disco_random_other_pos();
    } else {
        pos = g_queue_pos + 1;
        if (pos >= g_queue_len) {
            if (autoadv && g.repeat == REPEAT_OFF) return;   /* stop at the end */
            pos = 0;
        }
    }
    g_queue_pos = pos;
    disco_play_idx(g_queue[pos]);
}

static void disco_prev(void) {
    if (g_queue_len <= 0) return;
    int pos;
    if (g.shuffle) {
        pos = disco_random_other_pos();
    } else {
        pos = (g_queue_pos - 1 + g_queue_len) % g_queue_len;
    }
    g_queue_pos = pos;
    disco_play_idx(g_queue[pos]);
}

/* Build a queue of every track (in g.tracks order) and play `idx` — the fallback
   context for "play/pause" (X) when nothing has been queued from a list yet. */
static void disco_play_all(int idx) {
    if (g.count <= 0) return;
    static int all[DISCO_MAX_TRACKS];
    for (int i = 0; i < g.count; i++) all[i] = i;
    disco_play_from(all, g.count, idx);
}

static void disco_toggle(void) {
    if (g.now_playing < 0) { disco_play_all(g.list.cursor); return; }
    if (disco_audio_is_playing()) { disco_audio_pause();  g.paused = true; }
    else                          { disco_audio_resume(); g.paused = false; }
}

static const char *disco_status_label(void) {
    if (g.now_playing < 0)        return "Stopped";
    if (disco_audio_is_playing()) return "Playing";
    if (g.paused)                 return "Paused";
    return "Stopped";
}

static void disco_fmt_time(double s, char *out, size_t n) {
    if (s < 0) s = 0;
    int t = (int)s;
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

/* ---- drawing primitives ---- */

static void disco_pill(int x, int y, int w, int h, ap_color c) {
    ap_theme *th = cat_get_theme();
    unsigned corners = (unsigned)th->pill_corner_mask;
    if (corners == 0) corners = CAT_CORNER_ALL;
    int r = (int)(th->pill_radius_ratio * (h / 2.0f) + 0.5f);
    cat_draw_rounded_rect_ex(x, y, w, h, r, corners, c);
}

/* Draw a Material Icons glyph centered on (cx, cy). */
static void disco_icon(TTF_Font *font, const char *glyph, int cx, int cy, ap_color c) {
    int w = 0, h = 0;
    TTF_SizeUTF8(font, glyph, &w, &h);
    cat_draw_text(font, glyph, cx - w / 2, cy - h / 2, c);
}

/* Draw text that marquee-scrolls when it overflows visible_w (m persists across
   frames); when m is NULL it ellipsizes instead. Flags g_mq_anim while scrolling. */
static void disco_text(TTF_Font *font, const char *text, int x, int y,
                       ap_color c, int visible_w, cat_marquee *m) {
    if (!m) { cat_draw_text_ellipsized(font, text, x, y, c, visible_w); return; }
    if (cat_draw_text_marquee(font, text, x, y, c, visible_w, m, g_dt)) g_mq_anim = true;
}

static void disco_draw_art(int x, int y, int side) {
    ap_theme *th = cat_get_theme();
    unsigned corners = (unsigned)th->pill_corner_mask;
    if (corners == 0) corners = CAT_CORNER_ALL;

    if (g.art) {
        int tw = 0, thh = 0;
        SDL_QueryTexture(g.art, NULL, NULL, &tw, &thh);
        if (tw > 0 && thh > 0) {
            int dw = side, dh = thh * dw / tw;
            if (dh > side) { dh = side; dw = tw * dh / thh; }
            int dx = x + (side - dw) / 2, dy = y + (side - dh) / 2;
            int smaller = dw < dh ? dw : dh;
            int radius = (int)(th->pill_radius_ratio * smaller * 0.26f + 0.5f);
            cat_draw_image_rounded_ex(g.art, dx, dy, dw, dh, radius, corners);
            return;
        }
    }
    /* No cover art: the vinyl record placeholder (filling the square; its
       transparent corners let the theme background show through). */
    if (g_record) {
        cat_draw_image(g_record, x, y, side, side);
        return;
    }
    int r = (int)(th->pill_radius_ratio * side * 0.26f + 0.5f);
    cat_draw_rounded_rect_ex(x, y, side, side, r, corners, th->accent);
    int disc = side * 60 / 100;
    ap_color dim = { th->background.r, th->background.g, th->background.b, 210 };
    cat_draw_pill(x + (side - disc) / 2, y + (side - disc) / 2, disc, disc, dim);
    int dot = side * 20 / 100;
    cat_draw_pill(x + (side - dot) / 2, y + (side - dot) / 2, dot, dot, th->highlight);
}

/* Progress track + fill + elapsed/total under it. Returns total height consumed. */
static int disco_draw_progress(int x, int y, int w, int barh) {
    ap_theme *th = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    double pos = disco_audio_position(), dur = disco_audio_duration();
    double frac = (dur > 0) ? pos / dur : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    ap_color track = { th->text.r, th->text.g, th->text.b, 40 };
    cat_draw_pill(x, y, w, barh, track);
    int fw = (int)(w * frac + 0.5);
    if (fw >= barh) cat_draw_pill(x, y, fw, barh, th->highlight);

    char a[16], b[16];
    disco_fmt_time(pos, a, sizeof(a));
    disco_fmt_time(dur, b, sizeof(b));
    int ty = y + barh + CAT_S(4);
    cat_draw_text(small, a, x, ty, th->hint);
    int bw = 0, bh = 0;
    TTF_SizeUTF8(small, b, &bw, &bh);
    cat_draw_text(small, b, x + w - bw, ty, th->hint);
    return barh + CAT_S(4) + TTF_FontHeight(small);
}

static void disco_draw_track_row(int tidx, int x, int y, int w, int h, bool selected) {
    ap_theme *th = cat_get_theme();
    TTF_Font *med   = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    const disco_track *t = &g.tracks[tidx];

    int pill_h = h - cat_scale(4);
    int pill_y = y + (h - pill_h) / 2;
    if (selected) disco_pill(x, pill_y, w - cat_scale(4), pill_h, th->highlight);
    ap_color main_c = selected ? th->highlighted_text : th->text;
    ap_color sub_c  = selected
        ? (ap_color){ th->highlighted_text.r, th->highlighted_text.g, th->highlighted_text.b, 190 }
        : th->hint;

    const char *artist = disco_track_artist(t);
    double dur = disco_track_duration(t);
    char durs[16] = "";
    int dw = 0, dh = 0;
    if (dur > 0) { disco_fmt_time(dur, durs, sizeof(durs)); TTF_SizeUTF8(small, durs, &dw, &dh); }

    int lx = x + cat_scale(12);
    int rx = x + w - cat_scale(12);
    int tx = lx;
    int title_y = artist[0] ? pill_y + cat_scale(5)
                            : pill_y + (pill_h - TTF_FontHeight(med)) / 2;

    if (tidx == g.now_playing) {        /* now-playing marker: ▶ from the glyph-complete symbol font */
        TTF_Font *sym = cat_get_symbol_font();
        int mw = 0, mh = 0;
        TTF_SizeUTF8(sym, "\xE2\x96\xB6", &mw, &mh);
        cat_draw_text(sym, "\xE2\x96\xB6", tx, title_y + (TTF_FontHeight(med) - mh) / 2, main_c);
        tx += mw + cat_scale(6);
    }

    int avail = rx - (durs[0] ? dw + cat_scale(10) : 0) - tx;
    if (avail < cat_scale(20)) avail = cat_scale(20);

    disco_text(med, disco_track_title(t), tx, title_y, main_c, avail,
               selected ? &g_mq_row : NULL);
    if (artist[0]) {
        int sub_w = rx - lx - (durs[0] ? dw + cat_scale(10) : 0);  /* clear the duration column */
        if (sub_w < cat_scale(20)) sub_w = cat_scale(20);
        disco_text(small, artist, lx, title_y + TTF_FontHeight(med) - cat_scale(1),
                   sub_c, sub_w, selected ? &g_mq_row_sub : NULL);
    }
    if (durs[0]) {
        int dy = pill_y + (pill_h - dh) / 2;
        cat_draw_text(small, durs, rx - dw, dy, sub_c);
    }
}

/* An album / folder row: name + sub-label, with a ">" chevron marking drill-in. */
static void disco_draw_meta_row(const disco_row *r, int x, int y, int w, int h, bool selected) {
    ap_theme *th = cat_get_theme();
    TTF_Font *med   = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int pill_h = h - cat_scale(4);
    int pill_y = y + (h - pill_h) / 2;
    if (selected) disco_pill(x, pill_y, w - cat_scale(4), pill_h, th->highlight);
    ap_color main_c = selected ? th->highlighted_text : th->text;
    ap_color sub_c  = selected
        ? (ap_color){ th->highlighted_text.r, th->highlighted_text.g, th->highlighted_text.b, 190 }
        : th->hint;

    int lx = x + cat_scale(12);
    int rx = x + w - cat_scale(12);

    /* album + artist rows get a cover thumbnail on the left (artist uses a cover
       from one of its albums). Sized to the full pill height and flush at pill_y
       so the selection pill never peeks as a sliver above or below it. */
    if (r->type == ROW_ALBUM || r->type == ROW_ARTIST) {
        int side = pill_h;
        int album_ref = r->ref;
        if (r->type == ROW_ARTIST) {
            const disco_artist *art = &g_artists[r->ref];
            album_ref = art->album_count > 0 ? art->albums[0] : -1;
        }
        SDL_Texture *thumb = album_ref >= 0 ? disco_album_thumb(album_ref) : NULL;
        unsigned corners = (unsigned)th->pill_corner_mask;
        if (corners == 0) corners = CAT_CORNER_ALL;
        int radius = (int)(th->pill_radius_ratio * side * 0.26f + 0.5f);
        if (thumb) {
            cat_draw_image_rounded_ex(thumb, lx, pill_y, side, side, radius, corners);
        } else if (g_record) {   /* no cover: the vinyl placeholder */
            cat_draw_image(g_record, lx, pill_y, side, side);
        }
        lx += side + cat_scale(10);
    }

    int cw = 0, chh = 0;
    TTF_SizeUTF8(med, "\xE2\x80\xBA", &cw, &chh);   /* › single right-angle quote */
    cat_draw_text(med, "\xE2\x80\xBA", rx - cw, pill_y + (pill_h - chh) / 2, sub_c);

    int avail = rx - cw - cat_scale(10) - lx;
    if (avail < cat_scale(20)) avail = cat_scale(20);
    bool has_sub = r->sub[0] != '\0';
    int title_y = has_sub ? pill_y + cat_scale(5)
                          : pill_y + (pill_h - TTF_FontHeight(med)) / 2;
    disco_text(med, r->label, lx, title_y, main_c, avail, selected ? &g_mq_row : NULL);
    if (has_sub)
        disco_text(small, r->sub, lx, title_y + TTF_FontHeight(med) - cat_scale(1),
                   sub_c, avail, selected ? &g_mq_row_sub : NULL);
}

static void disco_draw_row(int i, int x, int y, int w, int h, bool selected, void *user) {
    (void)user;
    if (i < 0 || i >= g_rowcount) return;
    const disco_row *r = &g_rows[i];
    if (r->type == ROW_TRACK) disco_draw_track_row(r->ref, x, y, w, h, selected);
    else                      disco_draw_meta_row(r, x, y, w, h, selected);
}

/* Right-aligned audio-output chip in the title band, top-right. */
static void disco_draw_output_chip(SDL_Rect content) {
    ap_theme *th = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    const char *lbl = disco_output_label();
    int tw = 0, thh = 0;
    TTF_SizeUTF8(small, lbl, &tw, &thh);
    int padx = CAT_S(10), pady = CAT_S(4);
    int cw = tw + padx * 2, chh = thh + pady * 2;
    int cx = content.x + content.w - CAT_S(12) - cw;
    int cy = (content.y - chh) / 2;
    if (cy < CAT_S(4)) cy = CAT_S(4);
    ap_color chip = { th->text.r, th->text.g, th->text.b, 28 };
    disco_pill(cx, cy, cw, chh, chip);
    cat_draw_text(small, lbl, cx + padx, cy + pady, th->hint);
}

/* Draw the Disco Boy wordmark as-is, filling the title band height with just a
   sliver of padding top/bottom. Falls back to the text title if the image is gone. */
/* Load the wordmark as a white silhouette (RGB forced to white, alpha kept) so
   it can be color-modded to the active theme color at draw time. The original
   art's anti-aliased edges survive via the preserved alpha channel. */
static SDL_Texture *disco_load_wordmark(const char *path) {
    SDL_Surface *raw = IMG_Load(path);
    if (!raw) return NULL;
    SDL_Surface *s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(raw);
    if (!s) return NULL;
    SDL_LockSurface(s);
    Uint32 *px = (Uint32 *)s->pixels;
    int n = (s->pitch / 4) * s->h;
    for (int i = 0; i < n; i++) px[i] |= 0x00FFFFFFu;   /* keep alpha, force RGB white */
    SDL_UnlockSurface(s);
    SDL_Texture *t = SDL_CreateTextureFromSurface(cat_get_renderer(), s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void disco_draw_header(SDL_Rect content) {
    int tw = 0, thh = 0;
    if (!g_header || SDL_QueryTexture(g_header, NULL, NULL, &tw, &thh) != 0 || tw <= 0 || thh <= 0) {
        cat_draw_screen_title("Disco Boy", NULL);
        return;
    }
    ap_theme *th = cat_get_theme();       /* tint the wordmark to the theme accent */
    SDL_SetTextureColorMod(g_header, th->highlight.r, th->highlight.g, th->highlight.b);
    int band = content.y;                 /* the title band is [0, content.y] */
    int h = band - cat_scale(20);         /* ~9px top/bottom padding in the title band */
    if (h < cat_scale(14)) h = cat_scale(14);
    int w = h * tw / thh;
    int y = (band - h) / 2;
    if (y < 0) y = 0;
    cat_draw_image(g_header, cat_scale(12), y, w, h);
}

/* Thin divider under the title (matches the one above the hints); returns the
   content rect pushed down below the rule. */
static SDL_Rect disco_top_rule(SDL_Rect content) {
    ap_theme *th = cat_get_theme();
    int m = cat_scale(12);
    int lh = cat_scale(1); if (lh < 1) lh = 1;
    ap_color line = { th->text.r, th->text.g, th->text.b, 26 };
    cat_draw_rect(m, content.y, cat_get_screen_width() - m * 2, lh, line);
    int gap = cat_scale(10);
    content.y += gap;
    content.h -= gap;
    if (content.h < 0) content.h = 0;
    return content;
}

/* ---- our own hint bar (fixed size, so it fits at any launcher font size) ---- */

typedef struct { const char *btn; const char *label; } disco_hint;

static int disco_hint_btn_w(const char *btn, int pill_h) {
    if (!btn || !btn[0]) return 0;
    if (!btn[1]) return pill_h;                                  /* single char = square */
    return pill_h / 2 + cat_measure_text(g_hint_font, btn);      /* L1 / R1 = wider */
}

static int disco_hint_group_w(const disco_hint *items, int n, int pill_h) {
    int gap = cat_scale(6), item_gap = cat_scale(16);
    int inner = 0;
    for (int i = 0; i < n; i++) {
        if (i) inner += item_gap;
        inner += disco_hint_btn_w(items[i].btn, pill_h) + gap +
                 cat_measure_text(g_hint_font, items[i].label);
    }
    return inner;
}

/* One hint group, drawn straight on the player background (no outer pill). The
   confirm group (A) gets the highlight color; the rest get a faint fill — mirrors
   the mockup's hint bar. */
static void disco_draw_hint_group(const disco_hint *items, int n, int x0, int y, int pill_h,
                                  bool confirm) {
    ap_theme *th = cat_get_theme();
    int gap = cat_scale(6), item_gap = cat_scale(16);
    int fh = TTF_FontHeight(g_hint_font);
    int inner_h = pill_h - cat_scale(6);
    ap_color btn_bg = confirm ? th->highlight
                              : (ap_color){ th->text.r, th->text.g, th->text.b, 28 };
    ap_color btn_fg = confirm ? th->button_label : th->text;
    int x = x0;
    for (int i = 0; i < n; i++) {
        if (i) x += item_gap;
        int bw = disco_hint_btn_w(items[i].btn, pill_h);
        disco_pill(x, y + (pill_h - inner_h) / 2, bw, inner_h, btn_bg);
        int btw = cat_measure_text(g_hint_font, items[i].btn);
        cat_draw_text(g_hint_font, items[i].btn, x + (bw - btw) / 2, y + (pill_h - fh) / 2, btn_fg);
        x += bw + gap;
        cat_draw_text(g_hint_font, items[i].label, x, y + (pill_h - fh) / 2, th->hint);
        x += cat_measure_text(g_hint_font, items[i].label);
    }
}

static int disco_hint_pill_h(void) { return TTF_FontHeight(g_hint_font) + cat_scale(10); }

/* Vertical space the hint bar occupies (reserved out of the content rect). */
static int disco_hint_reserved(void) {
    if (!g_hint_font) return cat_get_footer_height();   /* fall back to Leaf's footer */
    return disco_hint_pill_h() + cat_scale(16);
}

/* Library-view hint bar. The Player view is intentionally hint-less (see the render
   dispatch): the user reaches it by pressing Y from here, so Y-to-return is the
   learned inverse, and the rest of the controls are the on-screen transport. */
static void disco_draw_hints(void) {
    if (!g_hint_font) return;
    ap_theme *th = cat_get_theme();
    int pill_h = disco_hint_pill_h();
    int sw = cat_get_screen_width();
    int m = cat_scale(12);
    int y = cat_get_screen_height() - cat_scale(8) - pill_h;

    /* a thin divider between the player UI and the hints */
    int lh = cat_scale(1); if (lh < 1) lh = 1;
    ap_color line = { th->text.r, th->text.g, th->text.b, 26 };
    cat_draw_rect(m, y - cat_scale(8), sw - m * 2, lh, line);

    static const disco_hint left[] = { {"M","Quit"}, {"L/R1","Tabs"}, {"L/R2","Seek"},
                                       {"X","Pause"}, {"Y","Player"} };
    disco_draw_hint_group(left, 5, m, y, pill_h, false);
    static const disco_hint right[] = { {"A","Play"} };
    disco_draw_hint_group(right, 1, sw - m - disco_hint_group_w(right, 1, pill_h), y, pill_h, true);
}

/* Compose "Album  ·  Year" (either part optional). */
static void disco_album_year(const disco_track *t, char *out, size_t n) {
    const char *al = disco_track_album(t);
    const char *yr = disco_track_year(t);
    if (al[0] && yr[0]) snprintf(out, n, "%s  \xC2\xB7  %s", al, yr);
    else if (al[0])     snprintf(out, n, "%s", al);
    else if (yr[0])     snprintf(out, n, "%s", yr);
    else                out[0] = '\0';
}

/* ═══ browse model: albums, rows, queue context ═══════════════════════════ */

/* Basename of a path's parent folder (album fallback / Folders grouping). */
static void disco_parent_name(const char *path, char *out, size_t n) {
    char tmp[DISCO_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash) *slash = '\0';
    const char *base = strrchr(tmp, '/');
    const char *src = base ? base + 1 : tmp;
    snprintf(out, n, "%.*s", (int)n - 1, src);
}

/* A track's album: tag album if present, else its own folder name (NOT
   disco_track_album, which is tied to the now-playing folder). */
static void disco_album_of(const disco_track *t, char *out, size_t n) {
    bool r = t->meta_ready;
    if (r) __sync_synchronize();
    if (r && t->meta.album[0]) { snprintf(out, n, "%s", t->meta.album); return; }
    disco_parent_name(t->path, out, n);
}

/* Pre-tag artist fallback: the grandparent folder name (the "Artist" dir in an
   Artist/Album/track layout), so the Artists view reads sensibly the instant the
   app opens, before the metadata worker has filled tags in. Loose tracks and album
   folders sitting directly under the music root have no artist dir -> "" (Unknown
   Artist). Replaced by the real tag for each track as the worker publishes it. */
static void disco_grandparent_name(const char *path, char *out, size_t n) {
    out[0] = '\0';
    char tmp[DISCO_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');     /* drop filename  -> parent (album) dir */
    if (!slash) return;
    *slash = '\0';
    slash = strrchr(tmp, '/');           /* drop album dir -> grandparent (artist) */
    if (!slash) return;
    *slash = '\0';
    if (strcmp(tmp, g.music_dir) == 0) return;   /* album folder sits at music root */
    const char *base = strrchr(tmp, '/');
    snprintf(out, n, "%.*s", (int)n - 1, base ? base + 1 : tmp);
}

/* A track's artist: tag artist if present, else the grandparent-folder fallback. */
static void disco_artist_of(const disco_track *t, char *out, size_t n) {
    const char *ar = disco_track_artist(t);
    if (ar && ar[0]) { snprintf(out, n, "%.*s", (int)n - 1, ar); return; }
    disco_grandparent_name(t->path, out, n);
}

static int disco_album_find(const char *album) {
    for (int i = 0; i < g_album_count; i++)
        if (strcasecmp(g_albums[i].album, album) == 0) return i;
    return -1;
}
/* Album track order: by track-number tag when both have one, else by filename
   (which is track order for the common "NN Title" naming). */
static int disco_name_cmp(const void *a, const void *b) {
    const disco_track *ta = &g.tracks[*(const int *)a], *tb = &g.tracks[*(const int *)b];
    int na = disco_track_num(ta), nb = disco_track_num(tb);
    if (na > 0 && nb > 0 && na != nb) return na - nb;
    return strcasecmp(ta->name, tb->name);
}
static int disco_album_cmp(const void *a, const void *b) {
    return strcasecmp(((const disco_album *)a)->album, ((const disco_album *)b)->album);
}

static int disco_artist_find(const char *name) {
    for (int i = 0; i < g_artist_count; i++)
        if (strcasecmp(g_artists[i].artist, name) == 0) return i;
    return -1;
}
static int disco_artist_cmp(const void *a, const void *b) {
    return strcasecmp(((const disco_artist *)a)->artist, ((const disco_artist *)b)->artist);
}

/* Build the artist index from the album index (each album carries one artist;
   its albums[] are added in album-name order, which g_albums is already in). */
static void disco_build_artists(void) {
    g_artist_count = 0;
    for (int i = 0; i < g_album_count; i++) {
        const char *ar = g_albums[i].artist[0] ? g_albums[i].artist : "Unknown Artist";
        int ai = disco_artist_find(ar);
        if (ai < 0) {
            if (g_artist_count >= (int)(sizeof(g_artists) / sizeof(g_artists[0]))) continue;
            ai = g_artist_count++;
            snprintf(g_artists[ai].artist, sizeof(g_artists[ai].artist), "%s", ar);
            g_artists[ai].album_count = 0;
            g_artists[ai].track_count = 0;
        }
        disco_artist *art = &g_artists[ai];
        if (art->album_count < (int)(sizeof(art->albums) / sizeof(art->albums[0])))
            art->albums[art->album_count++] = i;
        art->track_count += g_albums[i].count;
    }
    qsort(g_artists, (size_t)g_artist_count, sizeof(disco_artist), disco_artist_cmp);
}

/* (Re)build the album index from tags (folder name as fallback). Cheap; run once
   metadata is ready. Album tracks sort by filename, which is track order for the
   common "NN Title" naming. */
static void disco_build_albums(void) {
    g_album_count = 0;
    for (int i = 0; i < g.count; i++) {
        const disco_track *t = &g.tracks[i];
        char alb[160];
        disco_album_of(t, alb, sizeof(alb));
        if (!alb[0]) snprintf(alb, sizeof(alb), "Unknown Album");
        int ai = disco_album_find(alb);
        if (ai < 0) {
            if (g_album_count >= (int)(sizeof(g_albums) / sizeof(g_albums[0]))) continue;
            ai = g_album_count++;
            disco_album *al = &g_albums[ai];
            snprintf(al->album, sizeof(al->album), "%s", alb);
            char ar[160];
            disco_artist_of(t, ar, sizeof(ar));
            snprintf(al->artist, sizeof(al->artist), "%s", ar[0] ? ar : "Unknown Artist");
            al->count = 0;
            al->art_track = i;
            al->cover_state = 0;   /* cover resolved lazily */
        }
        disco_album *al = &g_albums[ai];
        if (al->count < (int)(sizeof(al->tracks) / sizeof(al->tracks[0])))
            al->tracks[al->count++] = i;
        char ar[160];
        disco_artist_of(t, ar, sizeof(ar));
        if (ar[0] && strcasecmp(al->artist, ar) != 0
                && strcasecmp(al->artist, "Various Artists") != 0
                && strcasecmp(al->artist, "Unknown Artist") != 0)
            snprintf(al->artist, sizeof(al->artist), "Various Artists");
    }
    for (int i = 0; i < g_album_count; i++)
        qsort(g_albums[i].tracks, (size_t)g_albums[i].count, sizeof(int), disco_name_cmp);
    qsort(g_albums, (size_t)g_album_count, sizeof(disco_album), disco_album_cmp);
    g_albums_built = true;
    disco_build_artists();   /* artists derive from albums; keep them in sync */
}

static void disco_row_track(int ref) {
    if (g_rowcount >= DISCO_MAX_TRACKS) return;
    disco_row *r = &g_rows[g_rowcount++];
    r->type = ROW_TRACK; r->ref = ref; r->label[0] = r->sub[0] = '\0';
}

static void disco_rows_albums(void) {
    if (!g_albums_built) disco_build_albums();
    g_rowcount = 0;
    for (int i = 0; i < g_album_count && g_rowcount < DISCO_MAX_TRACKS; i++) {
        disco_row *r = &g_rows[g_rowcount++];
        r->type = ROW_ALBUM; r->ref = i;
        const char *alb = g_albums[i].album;
        snprintf(r->label, sizeof(r->label), "%.*s", (int)sizeof(r->label) - 1, alb);
        int n = g_albums[i].count;
        snprintf(r->sub, sizeof(r->sub), "%s  \xC2\xB7  %d track%s",
                 g_albums[i].artist, n, n == 1 ? "" : "s");
    }
}

static void disco_rows_album_tracks(int ai) {
    g_rowcount = 0;
    if (ai < 0 || ai >= g_album_count) return;
    for (int i = 0; i < g_albums[ai].count; i++) disco_row_track(g_albums[ai].tracks[i]);
}

static void disco_rows_artists(void) {
    if (!g_albums_built) disco_build_albums();   /* also builds the artist index */
    g_rowcount = 0;
    for (int i = 0; i < g_artist_count && g_rowcount < DISCO_MAX_TRACKS; i++) {
        disco_row *r = &g_rows[g_rowcount++];
        r->type = ROW_ARTIST; r->ref = i;
        const char *an = g_artists[i].artist;
        snprintf(r->label, sizeof(r->label), "%.*s", (int)sizeof(r->label) - 1, an);
        int na = g_artists[i].album_count, nt = g_artists[i].track_count;
        snprintf(r->sub, sizeof(r->sub), "%d album%s  \xC2\xB7  %d track%s",
                 na, na == 1 ? "" : "s", nt, nt == 1 ? "" : "s");
    }
}

/* The albums of one artist (rows reference g_albums; year + track count as sub). */
static void disco_rows_artist_albums(int arti) {
    g_rowcount = 0;
    if (arti < 0 || arti >= g_artist_count) return;
    const disco_artist *art = &g_artists[arti];
    for (int j = 0; j < art->album_count && g_rowcount < DISCO_MAX_TRACKS; j++) {
        int ai = art->albums[j];
        disco_row *r = &g_rows[g_rowcount++];
        r->type = ROW_ALBUM; r->ref = ai;
        snprintf(r->label, sizeof(r->label), "%.*s", (int)sizeof(r->label) - 1, g_albums[ai].album);
        int n = g_albums[ai].count;
        const char *yr = n ? disco_track_year(&g.tracks[g_albums[ai].tracks[0]]) : "";
        if (yr[0]) snprintf(r->sub, sizeof(r->sub), "%s  \xC2\xB7  %d track%s", yr, n, n == 1 ? "" : "s");
        else       snprintf(r->sub, sizeof(r->sub), "%d track%s", n, n == 1 ? "" : "s");
    }
}

/* Folders tab: immediate subfolders (drill-in) then direct tracks of folder_dir,
   derived from the scanned track paths (no extra filesystem walk). */
static void disco_rows_folders(void) {
    g_rowcount = 0;
    const char *dir = g.folder_dir;
    size_t dlen = strlen(dir);
    for (int i = 0; i < g.count && g_rowcount < DISCO_MAX_TRACKS; i++) {     /* subfolders */
        const char *p = g.tracks[i].path;
        if (strncmp(p, dir, dlen) != 0 || p[dlen] != '/') continue;
        const char *rest = p + dlen + 1, *slash = strchr(rest, '/');
        if (!slash) continue;
        int len = (int)(slash - rest);
        bool exists = false;
        for (int j = 0; j < g_rowcount; j++)
            if (g_rows[j].type == ROW_FOLDER && (int)strlen(g_rows[j].label) == len
                && strncmp(g_rows[j].label, rest, (size_t)len) == 0) { exists = true; break; }
        if (exists) continue;
        disco_row *r = &g_rows[g_rowcount++];
        r->type = ROW_FOLDER; r->ref = 0;
        snprintf(r->label, sizeof(r->label), "%.*s", len, rest);
        char pre[DISCO_MAX_PATH];
        int pn = snprintf(pre, sizeof(pre), "%s/%.*s/", dir, len, rest);
        int cnt = 0;
        for (int k = 0; k < g.count; k++)
            if (strncmp(g.tracks[k].path, pre, (size_t)pn) == 0) cnt++;
        snprintf(r->sub, sizeof(r->sub), "%d track%s", cnt, cnt == 1 ? "" : "s");
    }
    for (int i = 0; i < g.count && g_rowcount < DISCO_MAX_TRACKS; i++) {     /* direct tracks */
        const char *p = g.tracks[i].path;
        if (strncmp(p, dir, dlen) != 0 || p[dlen] != '/') continue;
        if (strchr(p + dlen + 1, '/')) continue;
        disco_row_track(i);
    }
}

static void disco_rebuild_rows(void) {
    switch (g.tab) {
        case TAB_ARTISTS: if (g.artist_open < 0)     disco_rows_artists();
                          else if (g.album_open < 0) disco_rows_artist_albums(g.artist_open);
                          else                       disco_rows_album_tracks(g.album_open);
                          break;
        case TAB_ALBUMS:  if (g.album_open >= 0) disco_rows_album_tracks(g.album_open);
                          else                   disco_rows_albums();
                          break;
        case TAB_FOLDERS: disco_rows_folders(); break;
        default:          g_rowcount = 0; break;
    }
    g.list.cursor = 0;
    g.list.scroll_offset = 0;
    g_mq_header.elapsed_ms = 0;   /* restart the album-header scroll on each drill */
}

/* If the now-playing track is one of the current rows, scroll it to the top
   (clamped near the end) with the cursor on it. No-op if it isn't in this list,
   so browsing a different album/folder is left undisturbed. */
static void disco_pin_now_playing(void) {
    if (g.now_playing < 0) return;
    int row = -1;
    for (int i = 0; i < g_rowcount; i++)
        if (g_rows[i].type == ROW_TRACK && g_rows[i].ref == g.now_playing) { row = i; break; }
    if (row < 0) return;
    int maxoff = g_rowcount - g.list.visible_rows;
    if (maxoff < 0) maxoff = 0;
    g.list.scroll_offset = row < maxoff ? row : maxoff;
    g.list.cursor = row;
}

static void disco_set_tab(int delta) {
    g.tab = (disco_tab)(((int)g.tab + delta + TAB_COUNT) % TAB_COUNT);
    g.artist_open = -1;
    g.album_open = -1;
    snprintf(g.folder_dir, sizeof(g.folder_dir), "%s", g.music_dir);
    disco_rebuild_rows();
    disco_pin_now_playing();   /* surface the playing track if this tab contains it */
}

/* Play the track at row `i`, with the queue = every track row in the current
   list (so next/prev/shuffle follow this album / folder context). */
static void disco_play_row_track(int i) {
    static int list[DISCO_MAX_TRACKS];
    int n = 0, pos = 0;
    for (int j = 0; j < g_rowcount; j++) {
        if (g_rows[j].type != ROW_TRACK) continue;
        if (j == i) pos = n;
        list[n++] = g_rows[j].ref;
    }
    if (n > 0) disco_play_from(list, n, pos);
}

static void disco_row_activate(int i) {
    if (i < 0 || i >= g_rowcount) return;
    disco_row *r = &g_rows[i];
    if (r->type == ROW_TRACK)        disco_play_row_track(i);
    else if (r->type == ROW_ARTIST)  { g.artist_open = r->ref; disco_rebuild_rows(); }
    else if (r->type == ROW_ALBUM)   { g.album_open = r->ref; disco_rebuild_rows(); }
    else if (r->type == ROW_FOLDER) {
        size_t fl = strlen(g.folder_dir);
        if (fl + 2 < sizeof(g.folder_dir))
            snprintf(g.folder_dir + fl, sizeof(g.folder_dir) - fl, "/%.*s",
                     (int)(sizeof(g.folder_dir) - fl - 2), r->label);
        disco_rebuild_rows();
    }
}

/* Label whose first character drives letter-jump: artists/albums/folders by their
   name (the order their lists are sorted in), tracks by title. */
static const char *disco_row_label(int i, void *user) {
    (void)user;
    if (i < 0 || i >= g_rowcount) return "";
    const disco_row *r = &g_rows[i];
    if (r->type == ROW_TRACK) return disco_track_title(&g.tracks[r->ref]);
    return r->label;
}

/* Put the cursor on the album row matching `name` in the current rows (by name,
   so it survives an index re-sort). */
static void disco_cursor_to_album(const char *name) {
    if (!name[0]) return;
    for (int i = 0; i < g_rowcount; i++)
        if (g_rows[i].type == ROW_ALBUM && strcasecmp(g_albums[g_rows[i].ref].album, name) == 0) {
            cat_list_state_jump(&g.list, i, g_rowcount);
            return;
        }
}

/* B in the browse list: step up one level, leaving the cursor on the artist /
   album / subfolder you came out of (not the top). Returns false at the top. */
static bool disco_browse_back(void) {
    if (g.tab == TAB_ARTISTS) {
        if (g.album_open >= 0) {                 /* album tracks -> artist's albums */
            char name[160] = "";
            if (g.album_open < g_album_count)
                snprintf(name, sizeof(name), "%s", g_albums[g.album_open].album);
            g.album_open = -1;
            disco_rebuild_rows();
            disco_cursor_to_album(name);
            return true;
        }
        if (g.artist_open >= 0) {                 /* artist's albums -> artist list */
            char name[160] = "";
            if (g.artist_open < g_artist_count)
                snprintf(name, sizeof(name), "%s", g_artists[g.artist_open].artist);
            g.artist_open = -1;
            disco_rebuild_rows();
            for (int i = 0; i < g_rowcount; i++)
                if (g_rows[i].type == ROW_ARTIST
                        && strcasecmp(g_artists[g_rows[i].ref].artist, name) == 0) {
                    cat_list_state_jump(&g.list, i, g_rowcount);
                    break;
                }
            return true;
        }
        return false;                             /* artist list -> exit */
    }
    if (g.tab == TAB_ALBUMS && g.album_open >= 0) {
        char name[160] = "";
        if (g.album_open < g_album_count)
            snprintf(name, sizeof(name), "%s", g_albums[g.album_open].album);
        g.album_open = -1;
        disco_rebuild_rows();
        disco_cursor_to_album(name);
        return true;
    }
    if (g.tab == TAB_FOLDERS && strcmp(g.folder_dir, g.music_dir) != 0) {
        char came[256] = "";
        char *slash = strrchr(g.folder_dir, '/');
        if (slash) { snprintf(came, sizeof(came), "%s", slash + 1); *slash = '\0'; }
        disco_rebuild_rows();
        for (int i = 0; i < g_rowcount; i++)
            if (g_rows[i].type == ROW_FOLDER && strcmp(g_rows[i].label, came) == 0) {
                cat_list_state_jump(&g.list, i, g_rowcount);
                break;
            }
        return true;
    }
    return false;
}

/* ---- views ---- */

/* Browse tab strip: Artists / Albums / Folders, active one in a highlight pill. */
static void disco_draw_tabbar(SDL_Rect r) {
    ap_theme *th = cat_get_theme();
    TTF_Font *med = cat_get_font(CAT_FONT_MEDIUM);
    int seg = r.w / TAB_COUNT;
    int ph  = TTF_FontHeight(med) + CAT_S(8);
    for (int i = 0; i < TAB_COUNT; i++) {
        int sx = r.x + i * seg;
        bool active = (i == (int)g.tab);
        int tw = 0, thh = 0;
        TTF_SizeUTF8(med, DISCO_TAB_NAMES[i], &tw, &thh);
        int tx = sx + (seg - tw) / 2;
        int ty = r.y + (r.h - thh) / 2;
        if (active) {
            disco_pill(sx + CAT_S(3), r.y + (r.h - ph) / 2, seg - CAT_S(6), ph, th->highlight);
            cat_draw_text(med, DISCO_TAB_NAMES[i], tx, ty, th->highlighted_text);
        } else {
            cat_draw_text(med, DISCO_TAB_NAMES[i], tx, ty, th->hint);
        }
    }
}

static void disco_render_library(SDL_Rect content) {
    ap_theme *th = cat_get_theme();
    disco_draw_header(content);
    disco_draw_output_chip(content);
    content = disco_top_rule(content);

    if (g.count == 0) {
        TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
        char msg[DISCO_MAX_PATH + 64];
        snprintf(msg, sizeof(msg), "No music found in %s", g.music_dir);
        cat_draw_text(small, msg, content.x + CAT_S(12), content.y + CAT_S(12), th->hint);
        return;
    }

    int pad = CAT_S(12);   /* match the launcher's browse pages (JW_BROWSE_PAD) */
    cat_box page = { content.x, content.y, content.w, content.h, pad, pad, 0, pad };
    cat_box lb, rb;
    cat_box_split_cols(&page, cat_box_content(&page).w * 58 / 100, pad, &lb, &rb);
    SDL_Rect R  = cat_box_content(&rb);
    SDL_Rect LB = cat_box_content(&lb);

    TTF_Font *med   = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    /* tab bar across the top of the list column */
    int tab_h = TTF_FontHeight(med) + CAT_S(12);
    SDL_Rect tabrect = { LB.x, LB.y, LB.w, tab_h };
    disco_draw_tabbar(tabrect);
    int listy = LB.y + tab_h + CAT_S(4);

    /* drilled-in album: a header (full-res cover + album / artist / year · N ·
       total time); drilled-in artist / subfolder: a one-line breadcrumb. */
    if (g.album_open >= 0 && g.album_open < g_album_count) {
        const disco_album *al = &g_albums[g.album_open];
        int side = TTF_FontHeight(med) + 2 * TTF_FontHeight(small) + CAT_S(8);
        unsigned corners = (unsigned)th->pill_corner_mask;
        if (corners == 0) corners = CAT_CORNER_ALL;
        int radius = (int)(th->pill_radius_ratio * side * 0.26f + 0.5f);
        SDL_Texture *hero = disco_album_hero(g.album_open);
        if (hero)          cat_draw_image_rounded_ex(hero, LB.x, listy, side, side, radius, corners);
        else if (g_record) cat_draw_image(g_record, LB.x, listy, side, side);

        int hx = LB.x + side + CAT_S(10);
        int hw = LB.x + LB.w - hx;
        double total = 0;
        for (int i = 0; i < al->count; i++) total += disco_track_duration(&g.tracks[al->tracks[i]]);
        const char *yr = al->count ? disco_track_year(&g.tracks[al->tracks[0]]) : "";
        char tt[16]; disco_fmt_time(total, tt, sizeof(tt));
        char meta[160];
        if (yr[0]) snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %d tracks  \xC2\xB7  %s", yr, al->count, tt);
        else       snprintf(meta, sizeof(meta), "%d tracks  \xC2\xB7  %s", al->count, tt);
        int hy = listy;
        disco_text(med, al->album, hx, hy, th->text, hw, &g_mq_header);
        hy += TTF_FontHeight(med) + CAT_S(1);
        cat_draw_text_ellipsized(small, al->artist, hx, hy, th->hint, hw);
        hy += TTF_FontHeight(small) + CAT_S(1);
        cat_draw_text_ellipsized(small, meta, hx, hy, th->hint, hw);
        listy += side + CAT_S(6);
    } else if (g.tab == TAB_ARTISTS && g.artist_open >= 0 && g.artist_open < g_artist_count) {
        char crumb[200];
        snprintf(crumb, sizeof(crumb), "\xE2\x80\xB9 %s", g_artists[g.artist_open].artist);
        disco_text(med, crumb, LB.x + CAT_S(2), listy, th->text, LB.w - CAT_S(4), &g_mq_header);
        listy += TTF_FontHeight(med) + CAT_S(4);
    } else if (g.tab == TAB_FOLDERS && strcmp(g.folder_dir, g.music_dir) != 0) {
        const char *rel = g.folder_dir + strlen(g.music_dir);
        if (*rel == '/') rel++;
        char crumb[DISCO_MAX_PATH];
        snprintf(crumb, sizeof(crumb), "\xE2\x80\xB9 %s", rel);
        cat_draw_text_ellipsized(small, crumb, LB.x + CAT_S(2), listy, th->hint, LB.w - CAT_S(4));
        listy += TTF_FontHeight(small) + CAT_S(4);
    }

    /* fit_rows stretches the row pitch so the rows fill the pane exactly when the
       list overflows (no remainder gap below the last row), like the launcher. */
    int base_item_h = TTF_FontHeight(med) + TTF_FontHeight(small) + CAT_S(12);
    int vis = 0, item_h = base_item_h;
    cat_box listbox = { LB.x, listy, LB.w, LB.y + LB.h - listy, 0, 0, 0, 0 };
    SDL_Rect L = cat_box_fit_rows(&listbox, base_item_h, g_rowcount, &vis, &item_h);
    g.list.visible_rows = vis;
    cat_draw_list_pane(L.x, L.y, L.w, L.h, g_rowcount, &g.list, item_h, disco_draw_row, NULL);

    /* right column: art sized to leave room for the metadata + progress below it,
       so the pane never bleeds into the hint bar (worst case = all lines present). */
    int med_h = TTF_FontHeight(med), small_h = TTF_FontHeight(small);
    int text_h = CAT_S(10) + med_h + CAT_S(2)
               + (small_h + CAT_S(1)) * 2 + small_h + CAT_S(10)
               + CAT_S(5) + CAT_S(4) + small_h;
    int side = R.w;
    if (side > R.h - text_h) side = R.h - text_h;
    if (side < 0) side = 0;
    disco_draw_art(R.x + (R.w - side) / 2, R.y, side);

    int y = R.y + side + CAT_S(10);
    if (g.now_playing >= 0) {
        const disco_track *t = &g.tracks[g.now_playing];
        disco_text(med, disco_track_title(t), R.x, y, th->text, R.w, &g_mq_title);
        y += TTF_FontHeight(med) + CAT_S(2);
        const char *ar = disco_track_artist(t);
        if (ar[0]) {
            disco_text(small, ar, R.x, y, th->hint, R.w, &g_mq_artist);
            y += TTF_FontHeight(small) + CAT_S(1);
        }
        char aly[300];
        disco_album_year(t, aly, sizeof(aly));
        if (aly[0]) {
            disco_text(small, aly, R.x, y, th->hint, R.w, &g_mq_album);
            y += TTF_FontHeight(small) + CAT_S(1);
        }
        char fs[64];
        snprintf(fs, sizeof(fs), "%s  \xC2\xB7  %s",
                 disco_format_label(t->path), disco_status_label());
        cat_draw_text_ellipsized(small, fs, R.x, y, th->hint, R.w);
        y += TTF_FontHeight(small) + CAT_S(10);
        disco_draw_progress(R.x, y, R.w, CAT_S(5));
    } else {
        cat_draw_text_ellipsized(med, "Nothing playing", R.x, y, th->hint, R.w);
    }
}

/* "Now Playing" title, tinted to the theme highlight to match the Library
   wordmark. Mirrors cat_draw_screen_title's placement/sizing, just recolored. */
static void disco_draw_np_title(void) {
    ap_theme *th = cat_get_theme();
    int margin = CAT_DS(15);   /* device_padding(10) + 5, as cat_draw_screen_title uses */
    int max_w = cat_get_screen_width() - margin * 2;
    if (max_w < 1) max_w = 1;
    static const cat_font_tier tiers[] = { CAT_FONT_EXTRA_LARGE, CAT_FONT_LARGE, CAT_FONT_MEDIUM };
    TTF_Font *font = NULL;
    for (size_t i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++) {
        TTF_Font *c = cat_get_font(tiers[i]);
        if (!c) continue;
        font = c;
        if (cat_measure_text(c, "Now Playing") <= max_w) break;
    }
    if (font) cat_draw_text_clipped(font, "Now Playing", margin, 0, th->highlight, max_w);
}

static void disco_render_nowplaying(SDL_Rect content) {
    ap_theme *th = cat_get_theme();
    disco_draw_np_title();
    disco_draw_output_chip(content);
    content = disco_top_rule(content);

    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);
    TTF_Font *med   = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int pad = CAT_S(12);   /* match the launcher's browse pages (JW_BROWSE_PAD) */
    cat_box page = { content.x, content.y, content.w, content.h, pad, pad, 0, pad };
    cat_box ab, ib;
    cat_box_split_cols(&page, cat_box_content(&page).w * 44 / 100, CAT_S(18), &ab, &ib);
    SDL_Rect A = cat_box_content(&ab), I = cat_box_content(&ib);

    int side = A.w < A.h ? A.w : A.h;
    disco_draw_art(A.x, A.y + (A.h - side) / 2, side);

    const disco_track *t = g.now_playing >= 0 ? &g.tracks[g.now_playing] : NULL;
    const char *title = t ? disco_track_title(t) : "Nothing playing";

    int btn  = CAT_S(84);   /* top transport row (larger glyphs) */
    int row2 = CAT_S(58);   /* shuffle/repeat row */
    int est = TTF_FontHeight(large) * 2 + CAT_S(4) + TTF_FontHeight(med) +
              TTF_FontHeight(small) + CAT_S(16) + CAT_S(5) + CAT_S(4) +
              TTF_FontHeight(small) + CAT_S(18) + btn + CAT_S(14) + row2;
    int y = I.y + (I.h - est) / 2;
    if (y < I.y + CAT_S(6)) y = I.y + CAT_S(6);

    y += cat_draw_text_wrapped(large, title, I.x, y, I.w, th->text, CAT_ALIGN_LEFT);
    y += CAT_S(4);
    if (t) {
        const char *ar = disco_track_artist(t);
        if (ar[0]) {
            disco_text(med, ar, I.x, y, th->hint, I.w, &g_mq_artist);
            y += TTF_FontHeight(med) + CAT_S(1);
        }
        char aly[300], line[380];
        disco_album_year(t, aly, sizeof(aly));
        const char *fmt = disco_format_label(t->path);
        if (aly[0] && fmt[0]) snprintf(line, sizeof(line), "%s  \xC2\xB7  %s", aly, fmt);
        else if (aly[0])      snprintf(line, sizeof(line), "%s", aly);
        else                  snprintf(line, sizeof(line), "%s", fmt);
        if (line[0]) {
            disco_text(small, line, I.x, y, th->hint, I.w, &g_mq_album);
            y += TTF_FontHeight(small);
        }
    }
    y += CAT_S(16);
    y += disco_draw_progress(I.x, y, I.w, CAT_S(5));
    y += CAT_S(18);

    /* transport — top row: prev-track / rewind / play-pause / fast-forward / next-track
       (rewind/forward scrub +/-5s, prev/next skip tracks); bottom row: shuffle / repeat.
       d-pad navigable: Left/Right within a row, Up/Down between rows, A activates.
       Glyphs = bundled Material Icons. */
    int gap = CAT_S(34);
    int cx  = I.x + I.w / 2;
    bool playing = disco_audio_is_playing();
    TTF_Font *ic  = g_icons    ? g_icons    : med;
    TTF_Font *ics = g_icons_sm ? g_icons_sm : small;
    int pitch = btn + gap;                          /* spacing between the 5 row-1 slots */
    int max_pitch = (I.w - btn - CAT_S(16)) / 4;    /* keep all five inside the column */
    if (max_pitch > 0 && pitch > max_pitch) pitch = max_pitch;
    int slot[5] = { cx - 2 * pitch, cx - pitch, cx, cx + pitch, cx + 2 * pitch };
    int sx[2]   = { cx - CAT_S(48), cx + CAT_S(48) };   /* shuffle, repeat */
    int row1_cy = y + btn / 2;
    int row2_cy = y + btn + CAT_S(14) + row2 / 2;

    int fpad = CAT_S(8);
    ap_color halo = { th->highlight.r, th->highlight.g, th->highlight.b, 70 };
    if (g.np_focus <= 4)
        disco_pill(slot[g.np_focus] - btn / 2 - fpad, row1_cy - btn / 2 - fpad,
                   btn + fpad * 2, btn + fpad * 2, halo);
    else
        disco_pill(sx[g.np_focus - 5] - row2 / 2 - fpad, row2_cy - row2 / 2 - fpad,
                   row2 + fpad * 2, row2 + fpad * 2, halo);

    disco_icon(ic, ICON_PREV,    slot[0], row1_cy, g.np_focus == 0 ? th->text : th->hint);
    disco_icon(ic, ICON_REWIND,  slot[1], row1_cy, g.np_focus == 1 ? th->text : th->hint);
    cat_draw_pill(slot[2] - btn / 2, y, btn, btn, th->highlight);
    disco_icon(ic, playing ? ICON_PAUSE : ICON_PLAY, slot[2], row1_cy, th->highlighted_text);
    disco_icon(ic, ICON_FORWARD, slot[3], row1_cy, g.np_focus == 3 ? th->text : th->hint);
    disco_icon(ic, ICON_NEXT,    slot[4], row1_cy, g.np_focus == 4 ? th->text : th->hint);

    disco_icon(ics, ICON_SHUFFLE, sx[0], row2_cy, g.shuffle ? th->highlight : th->hint);
    disco_icon(ics, g.repeat == REPEAT_ONE ? ICON_REPEAT1 : ICON_REPEAT, sx[1], row2_cy,
               g.repeat == REPEAT_OFF ? th->hint : th->highlight);
}

/* SELECT overlay: the now-playing cover alone, maximized and centered, no chrome —
   a "vinyl sleeve" view. Reuses the player's art (the vinyl placeholder if none).
   Centers in the FULL screen, not the content rect: the overlay has no header/footer,
   so it shouldn't inherit the content rect's reserved strips / asymmetric padding. */
static void disco_render_art_only(void) {
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    int m = cat_scale(16);
    int side = (sw < sh ? sw : sh) - m * 2;
    if (side < 1) side = 1;
    disco_draw_art((sw - side) / 2, (sh - side) / 2, side);
}

/* Pocket lock flashes the padlock for this long, then powers the panel off. */
#define DISCO_LOCK_FLASH_MS 1400

/* Power the panel backlight off/on via the same sysfs knob the launcher uses
   (FB_BLANK: 4 = powerdown, 0 = on). bl_power preserves the user's brightness and
   loong_power won't fight it. Best-effort; the device runs us as root. */
static void disco_set_screen(bool on) {
    FILE *f = fopen("/sys/class/backlight/backlight/bl_power", "w");
    if (!f) return;
    fputc(on ? '0' : '4', f);
    fclose(f);
}

/* Draw a solid padlock centered at (cx,cy) at brightness `v` (0..255), composed
   from primitives so it needs no glyph font. Everything is fully opaque and drawn
   on a black field — shackle, then a black hole-punch, then the body over the
   shackle's base (so they merge into one solid lock), then a black keyhole. The
   flash pulses `v`, not alpha, so the opaque layers composite cleanly. */
static void disco_draw_lock(int cx, int cy, Uint8 v) {
    cat_draw_color icon = (cat_draw_color){ v, v, v, 255 };
    cat_draw_color hole = (cat_draw_color){ 0, 0, 0, 255 };
    int thick = cat_scale(14);
    int bw = cat_scale(132), bh = cat_scale(104);
    int bx = cx - bw / 2, by = cy - bh / 2 + cat_scale(26);
    int sw = cat_scale(84), shh = cat_scale(108);
    int sx = cx - sw / 2, sy = by - cat_scale(58);
    cat_draw_rounded_rect(sx, sy, sw, shh, sw / 2, icon);                               /* shackle */
    cat_draw_rounded_rect(sx + thick, sy + thick, sw - 2 * thick, shh, (sw - 2 * thick) / 2, hole);
    cat_draw_rounded_rect(bx, by, bw, bh, cat_scale(18), icon);                         /* body */
    cat_draw_circle(cx, by + bh * 4 / 10, cat_scale(12), hole);                         /* keyhole */
    cat_draw_rounded_rect(cx - cat_scale(4), by + bh * 4 / 10, cat_scale(8), cat_scale(24), cat_scale(4), hole);
}

/* The lock baked once into a transparent texture (opaque white on a clear field),
   so the flash can fade the whole solid shape uniformly via one alpha mod — no
   internal seams between the hasp and the body. */
static SDL_Texture *g_lock_tex = NULL;
static int g_lock_w = 0, g_lock_h = 0;
static SDL_Texture *disco_lock_texture(void) {
    if (g_lock_tex) return g_lock_tex;
    SDL_Renderer *r = cat_get_renderer();
    int pad = cat_scale(6), bw = cat_scale(132), bh = cat_scale(104);
    g_lock_w = bw + 2 * pad;
    g_lock_h = bh + cat_scale(58) + 2 * pad;
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET, g_lock_w, g_lock_h);
    if (!t) return NULL;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_Texture *prev = SDL_GetRenderTarget(r);
    SDL_SetRenderTarget(r, t);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    disco_draw_lock(pad + bw / 2, pad + bh / 2 + cat_scale(32), 255);   /* solid white */
    SDL_SetRenderTarget(r, prev);
    g_lock_tex = t;
    return t;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.now_playing = -1;
    g.artist_open = -1;
    g.album_open = -1;
    g.tab = TAB_ARTISTS;
    g.np_focus = 2;                 /* default focus = the play/pause button */
    cat_list_state_init(&g.list, 1);

    /* Disco Boy uses Leaf's "Soft" pill/art style - all four corners rounded at a
       0.25 radius ratio - rather than the launcher's default Leaf shape (rounded
       TL+BR only). A Leaf "shape" is just a (corner_mask, radius_ratio) pair, and
       these are the exact values of the Soft preset (Jawaka appearance index 1).
       Catastrophe reads these knobs in cat_init, so setting them here overrides
       whatever the launcher passed in - local to this app, never touches the
       launcher or other paks. */
    setenv("CAT_PILL_CORNER_MASK", "15", 1);     /* 15 = CAT_CORNER_ALL (Soft) */
    setenv("CAT_PILL_RADIUS_RATIO", "0.25", 1);  /* Soft preset radius ratio */

    cat_config cfg = {
        .window_title = "Disco Boy",
        .log_path     = cat_resolve_log_path("discoboy"),
        .cpu_speed    = CAT_CPU_SPEED_NORMAL,
    };
    if (cat_init(&cfg) != CAT_OK) {
        fprintf(stderr, "Disco Boy: failed to initialise Catastrophe\n");
        return 1;
    }

    disco_audio_init();
    srand((unsigned)time(NULL));

    /* bundled Material Icons subset for the transport glyphs (two sizes) + the
       Disco Boy wordmark for the title band */
    {
        const char *pd = getenv("DISCOBOY_PAK_DIR");
        char fp[DISCO_MAX_PATH];
        if (pd && pd[0]) snprintf(fp, sizeof(fp), "%s/res/media-icons.ttf", pd);
        else             snprintf(fp, sizeof(fp), "res/media-icons.ttf");
        g_icons    = TTF_OpenFont(fp, cat_scale(60));
        g_icons_sm = TTF_OpenFont(fp, cat_scale(42));
        if (pd && pd[0]) snprintf(fp, sizeof(fp), "%s/res/header.png", pd);
        else             snprintf(fp, sizeof(fp), "res/header.png");
        g_header = disco_load_wordmark(fp);
        if (pd && pd[0]) snprintf(fp, sizeof(fp), "%s/res/record.png", pd);
        else             snprintf(fp, sizeof(fp), "res/record.png");
        g_record = cat_load_image(fp);
    }
    {
        /* fixed-size hint font from the theme's UI font, ignoring the launcher's
           font-size bump so our hint bar fits at any setting */
        ap_theme *th = cat_get_theme();
        if (th->font_path[0])
            g_hint_font = TTF_OpenFont(th->font_path, cat_font_size_for_resolution(13));
        cat_log("discoboy: hint font %s [%s]", g_hint_font ? "ok" : "fallback to leaf footer",
                th->font_path[0] ? th->font_path : "no theme font path");
    }

    disco_resolve_music_dir(g.music_dir, sizeof(g.music_dir));
    disco_scan(g.music_dir);
    cat_log("discoboy: %d tracks in %s", g.count, g.music_dir);
    snprintf(g.folder_dir, sizeof(g.folder_dir), "%s", g.music_dir);
    disco_rebuild_rows();

    if (g.count > 0 && pthread_create(&g_meta_thread, NULL, disco_meta_worker, NULL) == 0)
        g_meta_running = true;
    else
        g_meta_done = true;

    if (pthread_create(&g_poll_thread, NULL, disco_status_worker, NULL) == 0)
        g_poll_running = true;

    int running = 1;
    while (running) {
        /* The Library view draws Disco Boy's own hint bar (constant, regardless of
           Leaf's "show hints" setting); the Player view is a clean, hint-less screen.
           Reserving the bar's height and drawing it are decided per-view at render. */
        SDL_Rect content = cat_get_content_rect(true, false, false);

        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            /* Pocket lock: an analog-stick (L3) click toggles a locked state that
               swallows every other button, so the player can ride in a pocket
               without firing buttons; the screen goes black while locked. Audio and
               auto-advance keep running. The same click unlocks and restores the UI. */
            if (ev.button == CAT_BTN_STICK) {
                if (ev.pressed && !ev.repeated) {
                    g.locked = !g.locked;
                    g_scrub = 0;
                    if (g.locked) g.lock_anim_until = SDL_GetTicks() + DISCO_LOCK_FLASH_MS;
                    cat_request_frame();   /* repaint now: flash on lock, UI on unlock */
                }
                continue;
            }
            if (g.locked) continue;        /* locked: ignore everything but the stick */
            if (!ev.pressed) {
                if (ev.button == CAT_BTN_L2 || ev.button == CAT_BTN_R2) g_scrub = 0;
                continue;
            }
            /* any other press stops a scrub, in case a trigger release is missed */
            if (ev.button != CAT_BTN_L2 && ev.button != CAT_BTN_R2) g_scrub = 0;
            /* Quit is a deliberate button (MENU), not B — so backing out of
               folders/albums can't drop you out of the app by accident. */
            if (ev.button == CAT_BTN_MENU && !ev.repeated) {
                running = 0; continue;
            }
            /* SELECT toggles the full-screen cover overlay (and exits it again). */
            if (ev.button == CAT_BTN_SELECT && !ev.repeated) {
                g.art_only = !g.art_only; continue;
            }
            if (g.art_only) {
                /* Sleeve view: playback stays controllable; B also exits; the rest
                   is inert (no list/transport underneath to drive). */
                switch (ev.button) {
                    case CAT_BTN_B:  if (!ev.repeated) g.art_only = false; break;
                    case CAT_BTN_X:  if (!ev.repeated) disco_toggle(); break;
                    case CAT_BTN_L1: if (!ev.repeated) disco_prev(); break;
                    case CAT_BTN_R1: if (!ev.repeated) disco_next(false); break;
                    case CAT_BTN_L2: g_scrub = -1; g_scrub_next = 0; break;
                    case CAT_BTN_R2: g_scrub = +1; g_scrub_next = 0; break;
                    default: break;
                }
                continue;
            }
            if (g.view == VIEW_LIBRARY) {
                switch (ev.button) {
                    case CAT_BTN_B:    if (!ev.repeated) disco_browse_back(); break;   /* back only */
                    case CAT_BTN_UP:   cat_list_state_move(&g.list, -1, g_rowcount); break;
                    case CAT_BTN_DOWN: cat_list_state_move(&g.list, +1, g_rowcount); break;
                    case CAT_BTN_LEFT:  cat_list_state_jump_letter(&g.list, disco_row_label, NULL, g_rowcount, -1); break;
                    case CAT_BTN_RIGHT: cat_list_state_jump_letter(&g.list, disco_row_label, NULL, g_rowcount, +1); break;
                    case CAT_BTN_A:    if (!ev.repeated) disco_row_activate(g.list.cursor); break;
                    case CAT_BTN_X:    if (!ev.repeated) disco_toggle(); break;
                    case CAT_BTN_Y:    if (!ev.repeated) g.view = VIEW_NOWPLAYING; break;
                    case CAT_BTN_L1:   if (!ev.repeated) disco_set_tab(-1); break;   /* switch tabs */
                    case CAT_BTN_R1:   if (!ev.repeated) disco_set_tab(+1); break;
                    case CAT_BTN_L2:   g_scrub = -1; g_scrub_next = 0; break;   /* hold to scrub */
                    case CAT_BTN_R2:   g_scrub = +1; g_scrub_next = 0; break;
                    default: break;
                }
            } else {
                switch (ev.button) {
                    case CAT_BTN_Y:     if (!ev.repeated) g.view = VIEW_LIBRARY; break;
                    /* focus slots: row1 0=prev 1=rewind 2=play/pause 3=fwd 4=next,
                       row2 5=shuffle 6=repeat. */
                    case CAT_BTN_LEFT:
                        if (g.np_focus >= 1 && g.np_focus <= 4) g.np_focus--;
                        else if (g.np_focus == 6) g.np_focus = 5;
                        break;
                    case CAT_BTN_RIGHT:
                        if (g.np_focus <= 3) g.np_focus++;
                        else if (g.np_focus == 5) g.np_focus = 6;
                        break;
                    case CAT_BTN_DOWN:  if (g.np_focus <= 4) g.np_focus = (g.np_focus <= 2) ? 5 : 6; break;
                    case CAT_BTN_UP:    if (g.np_focus >= 5) g.np_focus = (g.np_focus == 5) ? 2 : 3; break;
                    case CAT_BTN_A:     if (!ev.repeated) {
                                            switch (g.np_focus) {
                                                case 0: disco_prev(); break;
                                                case 1: disco_audio_seek(-DISCO_SEEK_STEP); break;
                                                case 3: disco_audio_seek(+DISCO_SEEK_STEP); break;
                                                case 4: disco_next(false); break;
                                                case 5: g.shuffle = !g.shuffle; break;
                                                case 6: g.repeat = (g.repeat + 1) % 3; break;
                                                default: disco_toggle(); break;
                                            }
                                        } break;
                    case CAT_BTN_X:     if (!ev.repeated) disco_toggle(); break;
                    case CAT_BTN_L1:    if (!ev.repeated) disco_prev(); break;
                    case CAT_BTN_R1:    if (!ev.repeated) disco_next(false); break;
                    case CAT_BTN_L2:    g_scrub = -1; g_scrub_next = 0; break;   /* hold to scrub */
                    case CAT_BTN_R2:    g_scrub = +1; g_scrub_next = 0; break;
                    default: break;
                }
            }
        }

        /* hold-to-scrub: while L2/R2 is held, seek a step every DISCO_SCRUB_MS */
        if (g_scrub != 0) {
            uint32_t tnow = SDL_GetTicks();
            if (tnow >= g_scrub_next) {
                disco_audio_seek(g_scrub * DISCO_SEEK_STEP);
                g_scrub_next = tnow + DISCO_SCRUB_MS;
            }
            cat_request_frame_in(40);   /* keep ticking while held */
        }

        if (disco_audio_finished()) disco_next(true);
        if (g.now_playing >= 0 && disco_audio_is_playing()) cat_request_frame_in(500);
        if (!g_meta_done) cat_request_frame_in(250);   /* refresh as metadata fills in */

        /* When tags finish loading, the artist/album grouping can change —
           invalidate the index, but DON'T jolt the user: if they're on a top-level
           artist/album list, rebuild it keeping the cursor on the same item by name;
           if drilled in, leave it (the index rebuilds lazily on back-out, restored
           by name); Folders is tag-independent, so untouched. */
        static bool meta_was_done = false;
        if (g_meta_done && !meta_was_done) {
            meta_was_done = true;
            g_albums_built = false;
            bool on_album_list  = (g.tab == TAB_ALBUMS  && g.album_open < 0);
            bool on_artist_list = (g.tab == TAB_ARTISTS && g.artist_open < 0);
            if (on_album_list || on_artist_list) {
                char name[160] = "";
                if (g.list.cursor >= 0 && g.list.cursor < g_rowcount) {
                    const disco_row *cr = &g_rows[g.list.cursor];
                    if (cr->type == ROW_ALBUM)  snprintf(name, sizeof(name), "%s", g_albums[cr->ref].album);
                    else if (cr->type == ROW_ARTIST) snprintf(name, sizeof(name), "%s", g_artists[cr->ref].artist);
                }
                disco_rebuild_rows();
                if (name[0]) {
                    for (int i = 0; i < g_rowcount; i++) {
                        const disco_row *cr = &g_rows[i];
                        const char *n = cr->type == ROW_ALBUM  ? g_albums[cr->ref].album
                                      : cr->type == ROW_ARTIST ? g_artists[cr->ref].artist : NULL;
                        if (n && strcasecmp(n, name) == 0) { cat_list_state_jump(&g.list, i, g_rowcount); break; }
                    }
                }
            }
        }

        /* The daemon poll runs on disco_status_worker (off-thread), so its blocking
           IPC can't hitch the scroll. Still wake ~2x/sec so the output chip tracks
           plug/unplug even while otherwise idle. */
        uint32_t now = SDL_GetTicks();
        cat_request_frame_in(500);

        /* marquee timing: ms since last frame, and reset scroll on track/cursor change */
        g_dt = g_frame_ms ? (now - g_frame_ms) : 0;
        g_frame_ms = now;
        if (g_mq_np_key != g.now_playing) {
            g_mq_np_key = g.now_playing;
            g_mq_title.elapsed_ms = 0; g_mq_artist.elapsed_ms = 0;
            g_mq_album.elapsed_ms = 0;
            /* Pin the now-playing track to the top of whatever list it's in
               (Songs always; the playing album/folder when you're browsing it). */
            if (g.view == VIEW_LIBRARY) disco_pin_now_playing();
        }
        if (g_mq_row_key != g.list.cursor) {
            g_mq_row_key = g.list.cursor;
            g_mq_row.elapsed_ms = 0; g_mq_row_sub.elapsed_ms = 0;
        }
        g_mq_anim = false;

        if (g.locked) {
            /* Pocket lock. First flash a padlock on black (panel still lit) as
               confirmation, then power the backlight off entirely. Playback and
               auto-advance keep running above; a stick click unlocks. */
            SDL_Renderer *rend = cat_get_renderer();
            SDL_SetRenderDrawColor(rend, 0, 0, 0, 255);
            SDL_RenderClear(rend);
            uint32_t tnow = SDL_GetTicks();
            if (tnow < g.lock_anim_until) {
                float since = (float)(tnow - (g.lock_anim_until - DISCO_LOCK_FLASH_MS));
                float pulse = 0.5f * (1.0f + sinf(since / 1000.0f * 2.0f * (float)M_PI * 1.7f));
                SDL_Texture *lt = disco_lock_texture();
                if (lt) {
                    int sw, sh; SDL_GetRendererOutputSize(rend, &sw, &sh);
                    SDL_Rect dst = { sw / 2 - g_lock_w / 2, sh / 2 - g_lock_h / 2, g_lock_w, g_lock_h };
                    SDL_SetTextureAlphaMod(lt, (Uint8)(255.0f * pulse));   /* fade the whole solid lock */
                    SDL_RenderCopy(rend, lt, NULL, &dst);
                }
                cat_present();
                cat_request_frame_in(30);            /* animate the flash */
            } else {
                cat_present();                       /* black */
                if (!g.screen_off) { disco_set_screen(false); g.screen_off = true; }
            }
            continue;
        }
        if (g.screen_off) { disco_set_screen(true); g.screen_off = false; }   /* just unlocked */
        cat_clear_screen();
        if (g.art_only) {
            /* Full-screen cover overlay — clean, no chrome, full height. */
            disco_render_art_only();
        } else if (g.view == VIEW_LIBRARY) {
            content.h -= disco_hint_reserved();   /* make room for the hint bar */
            if (content.h < 0) content.h = 0;
            disco_render_library(content);
            if (g_hint_font) {
                disco_draw_hints();
            } else {
                cat_footer_item footer[] = {
                    { .button = CAT_BTN_L1, .label = "Prev" },
                    { .button = CAT_BTN_R1, .label = "Next" },
                    { .button = CAT_BTN_X,  .label = "Play/Pause" },
                    { .button = CAT_BTN_Y,  .label = "Now Playing" },
                    { .button = CAT_BTN_A,  .label = "Play", .is_confirm = true },
                };
                cat_draw_footer(footer, 5);
            }
        } else {
            /* Player: clean, hint-less view — uses the full content height (no bar). */
            disco_render_nowplaying(content);
        }
        if (g_mq_anim) cat_request_frame();   /* 60fps active pacing while scrolling
                                                 (render-cost-corrected; matches the
                                                 launcher's marquee — smooth, not steppy) */
        cat_present();
    }

    if (g_poll_running) { g_poll_stop = true; pthread_join(g_poll_thread, NULL); }
    if (g_meta_running) { g_meta_stop = true; pthread_join(g_meta_thread, NULL); }
    if (g_icons)     TTF_CloseFont(g_icons);
    if (g_icons_sm)  TTF_CloseFont(g_icons_sm);
    if (g_hint_font) TTF_CloseFont(g_hint_font);
    if (g_header)    SDL_DestroyTexture(g_header);
    if (g_record)    SDL_DestroyTexture(g_record);
    if (g_album_hero) SDL_DestroyTexture(g_album_hero);
    if (g.art) { SDL_DestroyTexture(g.art); g.art = NULL; }
    disco_audio_shutdown();
    if (g.screen_off) disco_set_screen(true);   /* never leave the panel dark on exit */
    if (g_lock_tex) SDL_DestroyTexture(g_lock_tex);
    cat_quit();
    return 0;
}
