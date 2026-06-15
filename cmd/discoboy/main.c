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
#include "disco_meta.h"
#include "disco_status.h"

#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DISCO_MAX_TRACKS 2048
#define DISCO_MAX_PATH   1024

#define DISCO_SEEK_STEP  10.0   /* seconds per scrub press */

/* Material Icons codepoints (UTF-8), rendered from the bundled subset font. */
#define ICON_REWIND  "\xEE\x80\xA0"   /* fast_rewind  E020 */
#define ICON_FORWARD "\xEE\x80\x9F"   /* fast_forward E01F */
#define ICON_PLAY    "\xEE\x80\xB7"   /* play_arrow   E037 */
#define ICON_PAUSE   "\xEE\x80\xB4"   /* pause        E034 */
#define ICON_SHUFFLE "\xEE\x81\x83"   /* shuffle      E043 */
#define ICON_REPEAT  "\xEE\x81\x80"   /* repeat       E040 */
#define ICON_REPEAT1 "\xEE\x81\x81"   /* repeat_one   E041 */

typedef enum { VIEW_LIBRARY = 0, VIEW_NOWPLAYING } disco_view;
typedef enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE } disco_repeat;

typedef struct {
    char         name[256];          /* raw filename, used for sort order */
    char         title[256];         /* prettified filename (fallback title) */
    char         path[DISCO_MAX_PATH];
    disco_meta   meta;               /* tags + duration, filled by the worker */
    volatile int meta_ready;         /* 0 = pending, 1 = meta is valid to read */
} disco_track;

static struct {
    char           music_dir[DISCO_MAX_PATH];
    disco_track    tracks[DISCO_MAX_TRACKS];
    int            count;
    cat_list_state list;
    int            now_playing;            /* index, or -1 */
    bool           paused;
    disco_view     view;
    int            np_focus;               /* transport focus 0 rw, 1 play, 2 ff, 3 shuffle, 4 repeat */
    bool           shuffle;
    disco_repeat   repeat;
    SDL_Texture   *art;                    /* cover for now_playing's folder, or NULL */
    char           art_dir[DISCO_MAX_PATH];/* folder the cover was last resolved for */
} g;

/* background metadata worker */
static pthread_t     g_meta_thread;
static bool          g_meta_running = false;
static volatile bool g_meta_stop = false;
static volatile bool g_meta_done = false;

/* live audio status (polled from the daemon) */
static char     g_output[16] = "";         /* current output name, or "" before first poll */
static int      g_volume = -1;
static uint32_t g_last_poll = 0;

/* bundled Material Icons subset, opened at two sizes for the transport */
static TTF_Font *g_icons = NULL;
static TTF_Font *g_icons_sm = NULL;

/* marquee state for overflowing text + per-frame delta */
static cat_marquee g_mq_row, g_mq_title, g_mq_artist;
static int         g_mq_row_key = -1, g_mq_np_key = -2;
static uint32_t    g_frame_ms = 0, g_dt = 0;
static bool        g_mq_anim = false;   /* set by any marquee still scrolling this frame */

/* ---- filename / format helpers ---- */

static bool disco_is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *exts[] = { ".mp3", ".flac", ".ogg", ".opus", ".wav", ".m4a", NULL };
    for (int i = 0; exts[i]; i++) if (strcasecmp(dot, exts[i]) == 0) return true;
    return false;
}

static const char *disco_format_label(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "";
    if (!strcasecmp(dot, ".wav"))  return "WAV";
    if (!strcasecmp(dot, ".mp3"))  return "MP3";
    if (!strcasecmp(dot, ".flac")) return "FLAC";
    if (!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga")) return "OGG";
    if (!strcasecmp(dot, ".opus")) return "OPUS";
    if (!strcasecmp(dot, ".m4a"))  return "M4A";
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
    if (m && m[0]) { snprintf(out, n, "%s", m); return; }
    const char *sd = getenv("SDCARD_PATH");
    if (sd && sd[0]) { snprintf(out, n, "%s/Music", sd); return; }
    snprintf(out, n, "Music");
}

/* Background worker: read duration + tags for every track, publish per-track. */
static void *disco_meta_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < g.count && !g_meta_stop; i++) {
        disco_meta m;
        disco_meta_read(g.tracks[i].path, &m);
        g.tracks[i].meta = m;          /* fill fields... */
        __sync_synchronize();          /* ...then publish the ready flag */
        g.tracks[i].meta_ready = 1;
    }
    g_meta_done = true;
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
    const char *o = g_output[0] ? g_output : getenv("JAWAKA_AUDIO_OUTPUT");
    if (!o || !o[0]) o = getenv("UMRK_AUDIO_OUTPUT");
    if (o) {
        if (!strcasecmp(o, "BLUETOOTH")) return "Bluetooth";
        if (!strcasecmp(o, "HEADSET"))   return "Headphones";
        if (!strcasecmp(o, "HDMI"))      return "HDMI";
        if (!strcasecmp(o, "SPEAKER"))   return "Speaker";
    }
    return "Speaker";
}

/* ---- playback ---- */

static void disco_load_art(const char *track_path) {
    char dir[DISCO_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", track_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");

    if (strcmp(dir, g.art_dir) == 0) return;
    if (g.art) { SDL_DestroyTexture(g.art); g.art = NULL; }
    snprintf(g.art_dir, sizeof(g.art_dir), "%s", dir);

    static const char *names[] = {
        "cover.png", "cover.jpg", "folder.jpg", "folder.png",
        "front.png", "front.jpg", "album.png", "album.jpg", NULL };
    for (int i = 0; names[i]; i++) {
        char p[DISCO_MAX_PATH];
        if (snprintf(p, sizeof(p), "%s/%s", dir, names[i]) >= (int)sizeof(p)) continue;
        if (access(p, R_OK) == 0) {
            g.art = cat_load_image(p);
            if (g.art) break;
        }
    }
}

static void disco_play(int idx) {
    if (idx < 0 || idx >= g.count) return;
    g.now_playing = idx;
    g.paused = false;
    disco_load_art(g.tracks[idx].path);
    if (!disco_audio_play(g.tracks[idx].path))
        cat_log("discoboy: could not play %s", g.tracks[idx].path);
}

static int disco_random_other(void) {
    if (g.count <= 1) return g.now_playing >= 0 ? g.now_playing : 0;
    int n;
    do { n = rand() % g.count; } while (n == g.now_playing);
    return n;
}

/* Advance to the next track. `autoadv` = a track just ended (honors repeat-one and
   stops at the end when repeat is off); a manual Next ignores repeat-one. */
static void disco_next(bool autoadv) {
    if (g.count <= 0) return;
    if (autoadv && g.repeat == REPEAT_ONE) { disco_play(g.now_playing); return; }
    int n;
    if (g.shuffle) {
        n = disco_random_other();
    } else {
        int cur = g.now_playing < 0 ? g.list.cursor : g.now_playing;
        n = cur + 1;
        if (n >= g.count) {
            if (autoadv && g.repeat == REPEAT_OFF) return;   /* stop at the end */
            n = 0;
        }
    }
    g.list.cursor = n;
    disco_play(n);
}

static void disco_prev(void) {
    if (g.count <= 0) return;
    int n;
    if (g.shuffle) {
        n = disco_random_other();
    } else {
        int cur = g.now_playing < 0 ? g.list.cursor : g.now_playing;
        n = (cur - 1 + g.count) % g.count;
    }
    g.list.cursor = n;
    disco_play(n);
}

static void disco_toggle(void) {
    if (g.now_playing < 0) { disco_play(g.list.cursor); return; }
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

static void disco_draw_item(int idx, int x, int y, int w, int h, bool selected, void *user) {
    (void)user;
    ap_theme *th = cat_get_theme();
    TTF_Font *med   = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    const disco_track *t = &g.tracks[idx];

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

    if (idx == g.now_playing) {        /* now-playing marker: ▶ from the glyph-complete symbol font */
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
    if (artist[0])
        cat_draw_text_ellipsized(small, artist, lx, title_y + TTF_FontHeight(med) - cat_scale(1),
                                 sub_c, rx - lx);
    if (durs[0]) {
        int dy = pill_y + (pill_h - dh) / 2;
        cat_draw_text(small, durs, rx - dw, dy, sub_c);
    }
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

/* Compose "Album  ·  Year" (either part optional). */
static void disco_album_year(const disco_track *t, char *out, size_t n) {
    const char *al = disco_track_album(t);
    const char *yr = disco_track_year(t);
    if (al[0] && yr[0]) snprintf(out, n, "%s  \xC2\xB7  %s", al, yr);
    else if (al[0])     snprintf(out, n, "%s", al);
    else if (yr[0])     snprintf(out, n, "%s", yr);
    else                out[0] = '\0';
}

/* ---- views ---- */

static void disco_render_library(SDL_Rect content) {
    ap_theme *th = cat_get_theme();
    cat_draw_screen_title("Disco Boy", NULL);
    disco_draw_output_chip(content);

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
    SDL_Rect L = cat_box_content(&lb), R = cat_box_content(&rb);

    TTF_Font *med   = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int item_h = TTF_FontHeight(med) + TTF_FontHeight(small) + CAT_S(12);
    g.list.visible_rows = (item_h > 0 && L.h > 0) ? L.h / item_h : 1;
    if (g.list.visible_rows < 1) g.list.visible_rows = 1;
    cat_draw_list_pane(L.x, L.y, L.w, L.h, g.count, &g.list, item_h, disco_draw_item, NULL);

    /* right column: album art, then now-playing metadata + progress */
    int side = R.w;
    if (side > R.h * 55 / 100) side = R.h * 55 / 100;
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
            cat_draw_text_ellipsized(small, aly, R.x, y, th->hint, R.w);
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

static void disco_render_nowplaying(SDL_Rect content) {
    ap_theme *th = cat_get_theme();
    cat_draw_screen_title("Now Playing", NULL);
    disco_draw_output_chip(content);

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

    int btn  = CAT_S(46);   /* top transport row */
    int row2 = CAT_S(34);   /* shuffle/repeat row */
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
            disco_text(small, line, I.x, y, th->hint, I.w, NULL);
            y += TTF_FontHeight(small);
        }
    }
    y += CAT_S(16);
    y += disco_draw_progress(I.x, y, I.w, CAT_S(5));
    y += CAT_S(18);

    /* transport — top row: rewind / play-pause / fast-forward (the sides scrub the
       track +/-10s); bottom row: shuffle / repeat. d-pad navigable: Left/Right within
       a row, Up/Down between rows, A activates. Glyphs = bundled Material Icons. */
    int gap = CAT_S(20);
    int cx  = I.x + I.w / 2;
    bool playing = disco_audio_is_playing();
    TTF_Font *ic  = g_icons    ? g_icons    : med;
    TTF_Font *ics = g_icons_sm ? g_icons_sm : small;
    int slot[3] = { cx - btn - gap, cx, cx + btn + gap };
    int sx[2]   = { cx - CAT_S(30), cx + CAT_S(30) };   /* shuffle, repeat */
    int row1_cy = y + btn / 2;
    int row2_cy = y + btn + CAT_S(14) + row2 / 2;

    int fpad = CAT_S(6);
    ap_color halo = { th->highlight.r, th->highlight.g, th->highlight.b, 70 };
    if (g.np_focus <= 2)
        disco_pill(slot[g.np_focus] - btn / 2 - fpad, row1_cy - btn / 2 - fpad,
                   btn + fpad * 2, btn + fpad * 2, halo);
    else
        disco_pill(sx[g.np_focus - 3] - row2 / 2 - fpad, row2_cy - row2 / 2 - fpad,
                   row2 + fpad * 2, row2 + fpad * 2, halo);

    disco_icon(ic, ICON_REWIND, slot[0], row1_cy, g.np_focus == 0 ? th->text : th->hint);
    cat_draw_pill(slot[1] - btn / 2, y, btn, btn, th->highlight);
    disco_icon(ic, playing ? ICON_PAUSE : ICON_PLAY, slot[1], row1_cy, th->highlighted_text);
    disco_icon(ic, ICON_FORWARD, slot[2], row1_cy, g.np_focus == 2 ? th->text : th->hint);

    disco_icon(ics, ICON_SHUFFLE, sx[0], row2_cy, g.shuffle ? th->highlight : th->hint);
    disco_icon(ics, g.repeat == REPEAT_ONE ? ICON_REPEAT1 : ICON_REPEAT, sx[1], row2_cy,
               g.repeat == REPEAT_OFF ? th->hint : th->highlight);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.now_playing = -1;
    g.np_focus = 1;                 /* default focus = the play/pause button */
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

    /* bundled Material Icons subset for the transport glyphs (two sizes) */
    {
        const char *pd = getenv("DISCOBOY_PAK_DIR");
        char fp[DISCO_MAX_PATH];
        if (pd && pd[0]) snprintf(fp, sizeof(fp), "%s/res/media-icons.ttf", pd);
        else             snprintf(fp, sizeof(fp), "res/media-icons.ttf");
        g_icons    = TTF_OpenFont(fp, cat_scale(30));
        g_icons_sm = TTF_OpenFont(fp, cat_scale(22));
    }

    disco_resolve_music_dir(g.music_dir, sizeof(g.music_dir));
    disco_scan(g.music_dir);
    cat_log("discoboy: %d tracks in %s", g.count, g.music_dir);

    if (g.count > 0 && pthread_create(&g_meta_thread, NULL, disco_meta_worker, NULL) == 0)
        g_meta_running = true;
    else
        g_meta_done = true;

    int running = 1;
    while (running) {
        bool show_hints = cat_hints_enabled_from_env();
        SDL_Rect content = cat_get_content_rect(true, show_hints, false);

        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            if (g.view == VIEW_LIBRARY) {
                switch (ev.button) {
                    case CAT_BTN_B:    if (!ev.repeated) running = 0; break;
                    case CAT_BTN_UP:   cat_list_state_move(&g.list, -1, g.count); break;
                    case CAT_BTN_DOWN: cat_list_state_move(&g.list, +1, g.count); break;
                    case CAT_BTN_A:    if (!ev.repeated) disco_play(g.list.cursor); break;
                    case CAT_BTN_X:    if (!ev.repeated) disco_toggle(); break;
                    case CAT_BTN_Y:    if (!ev.repeated && g.now_playing >= 0) g.view = VIEW_NOWPLAYING; break;
                    case CAT_BTN_L1:   if (!ev.repeated) disco_prev(); break;
                    case CAT_BTN_R1:   if (!ev.repeated) disco_next(false); break;
                    default: break;
                }
            } else {
                switch (ev.button) {
                    case CAT_BTN_B:
                    case CAT_BTN_Y:     if (!ev.repeated) g.view = VIEW_LIBRARY; break;
                    case CAT_BTN_LEFT:
                        if (g.np_focus == 1 || g.np_focus == 2) g.np_focus--;
                        else if (g.np_focus == 4) g.np_focus = 3;
                        break;
                    case CAT_BTN_RIGHT:
                        if (g.np_focus == 0 || g.np_focus == 1) g.np_focus++;
                        else if (g.np_focus == 3) g.np_focus = 4;
                        break;
                    case CAT_BTN_DOWN:  if (g.np_focus <= 2) g.np_focus = (g.np_focus == 2) ? 4 : 3; break;
                    case CAT_BTN_UP:    if (g.np_focus >= 3) g.np_focus = (g.np_focus == 4) ? 2 : 1; break;
                    case CAT_BTN_A:     if (!ev.repeated) {
                                            switch (g.np_focus) {
                                                case 0: disco_audio_seek(-DISCO_SEEK_STEP); break;
                                                case 2: disco_audio_seek(+DISCO_SEEK_STEP); break;
                                                case 3: g.shuffle = !g.shuffle; break;
                                                case 4: g.repeat = (g.repeat + 1) % 3; break;
                                                default: disco_toggle(); break;
                                            }
                                        } break;
                    case CAT_BTN_X:     if (!ev.repeated) disco_toggle(); break;
                    case CAT_BTN_L1:    if (!ev.repeated) disco_prev(); break;
                    case CAT_BTN_R1:    if (!ev.repeated) disco_next(false); break;
                    default: break;
                }
            }
        }

        if (disco_audio_finished()) disco_next(true);
        if (g.now_playing >= 0 && disco_audio_is_playing()) cat_request_frame_in(500);
        if (!g_meta_done) cat_request_frame_in(250);   /* refresh as metadata fills in */

        /* poll the daemon for the live audio output + volume (the launch-time env
           goes stale on plug/unplug), then push routing + software volume down. */
        uint32_t now = SDL_GetTicks();
        if (now - g_last_poll >= 500) {
            g_last_poll = now;
            disco_audio_status as;
            if (disco_status_query(&as)) {
                if (as.output[0]) {
                    snprintf(g_output, sizeof(g_output), "%s", as.output);
                    disco_audio_set_output(as.output);
                }
                if (as.volume >= 0) { g_volume = as.volume; disco_audio_set_volume(as.volume); }
            }
        }
        cat_request_frame_in(500);   /* keep the poll cadence alive even when idle */

        /* marquee timing: ms since last frame, and reset scroll on track/cursor change */
        g_dt = g_frame_ms ? (now - g_frame_ms) : 0;
        g_frame_ms = now;
        if (g_mq_np_key != g.now_playing) {
            g_mq_np_key = g.now_playing;
            g_mq_title.elapsed_ms = 0; g_mq_artist.elapsed_ms = 0;
        }
        if (g_mq_row_key != g.list.cursor) {
            g_mq_row_key = g.list.cursor; g_mq_row.elapsed_ms = 0;
        }
        g_mq_anim = false;

        cat_clear_screen();
        if (g.view == VIEW_LIBRARY) disco_render_library(content);
        else                        disco_render_nowplaying(content);

        if (show_hints) {
            if (g.view == VIEW_LIBRARY) {
                cat_footer_item footer[] = {
                    { .button = CAT_BTN_L1, .label = "Prev" },
                    { .button = CAT_BTN_R1, .label = "Next" },
                    { .button = CAT_BTN_X,  .label = "Play/Pause" },
                    { .button = CAT_BTN_Y,  .label = "Now Playing" },
                    { .button = CAT_BTN_A,  .label = "Play", .is_confirm = true },
                };
                cat_draw_footer(footer, 5);
            } else {
                cat_footer_item footer[] = {
                    { .button = CAT_BTN_L1, .label = "Prev" },
                    { .button = CAT_BTN_R1, .label = "Next" },
                    { .button = CAT_BTN_X,  .label = "Play/Pause" },
                    { .button = CAT_BTN_B,  .label = "List" },
                    { .button = CAT_BTN_A,  .label = "Select", .is_confirm = true },
                };
                cat_draw_footer(footer, 5);
            }
        }
        if (g_mq_anim) cat_request_frame_in(33);   /* keep the marquee scrolling */
        cat_present();
    }

    if (g_meta_running) { g_meta_stop = true; pthread_join(g_meta_thread, NULL); }
    if (g_icons)    TTF_CloseFont(g_icons);
    if (g_icons_sm) TTF_CloseFont(g_icons_sm);
    if (g.art) { SDL_DestroyTexture(g.art); g.art = NULL; }
    disco_audio_shutdown();
    cat_quit();
    return 0;
}
