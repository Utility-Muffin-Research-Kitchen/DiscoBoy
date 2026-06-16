#include "disco_meta.h"
#include "disco_ff.h"

#include "dr_wav.h"
#include "dr_flac.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- small text helpers ---- */

static void meta_set(char *dst, size_t cap, const char *src, size_t n) {
    if (dst[0]) return;                 /* first value wins */
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Keep just the leading 4-digit year out of a DATE like "1969-05-01". */
static void meta_set_year(char *dst, size_t cap, const char *src, size_t n) {
    size_t y = 0;
    while (y < n && y < 4 && src[y] >= '0' && src[y] <= '9') y++;
    if (y == 4) meta_set(dst, cap, src, 4);
    else        meta_set(dst, cap, src, n);
}

/* Apply one Vorbis "KEY=VALUE" comment (value is UTF-8 already). */
static void meta_vorbis_kv(disco_meta *out, const char *kv, size_t len) {
    const char *eq = memchr(kv, '=', len);
    if (!eq) return;
    size_t klen = (size_t)(eq - kv);
    const char *val = eq + 1;
    size_t vlen = len - klen - 1;
    char key[24];
    if (klen >= sizeof(key)) return;
    for (size_t i = 0; i < klen; i++) key[i] = (char)toupper((unsigned char)kv[i]);
    key[klen] = '\0';
    if      (!strcmp(key, "TITLE"))  meta_set(out->title,  sizeof(out->title),  val, vlen);
    else if (!strcmp(key, "ARTIST")) meta_set(out->artist, sizeof(out->artist), val, vlen);
    else if (!strcmp(key, "ALBUM"))  meta_set(out->album,  sizeof(out->album),  val, vlen);
    else if (!strcmp(key, "DATE") || !strcmp(key, "YEAR"))
        meta_set_year(out->year, sizeof(out->year), val, vlen);
    else if (!strcmp(key, "TRACKNUMBER") && out->track == 0) {
        char num[12]; size_t k = 0;                    /* val isn't NUL-terminated */
        for (size_t i = 0; i < vlen && k < sizeof(num) - 1 && val[i] >= '0' && val[i] <= '9'; i++)
            num[k++] = val[i];
        num[k] = '\0';
        out->track = atoi(num);
    }
}

/* ---- FLAC ---- */

static void flac_on_meta(void *user, drflac_metadata *md) {
    if (md->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    disco_meta *out = (disco_meta *)user;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, md->data.vorbis_comment.commentCount,
                                        md->data.vorbis_comment.pComments);
    drflac_uint32 clen;
    const char *c;
    while ((c = drflac_next_vorbis_comment(&it, &clen)) != NULL)
        meta_vorbis_kv(out, c, clen);
}

/* ---- format detect + entry point ---- */

static int meta_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    if (!strcasecmp(dot, ".wav"))  return 'w';
    if (!strcasecmp(dot, ".mp3"))  return 'm';
    if (!strcasecmp(dot, ".flac")) return 'f';
    if (!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga")) return 'o';
    return 0;
}

void disco_meta_read(const char *path, disco_meta *out) {
    memset(out, 0, sizeof(*out));
    switch (meta_ext(path)) {
    case 'w': {
        drwav w;
        if (drwav_init_file(&w, path, NULL)) {
            if (w.sampleRate) out->duration = (double)w.totalPCMFrameCount / w.sampleRate;
            drwav_uninit(&w);
        }
        break;
    }
    case 'm':
        /* ID3 tags + duration via FFmpeg: reads every ID3 version robustly (the
           hand-rolled parser lost frame-sync on v2.3 tags with embedded art) and
           skips the slow full-file duration scan. */
        disco_ff_tags(path, out->title, out->artist, out->album, out->year,
                      sizeof(out->title), &out->duration, &out->track);
        break;
    case 'f': {
        drflac *fl = drflac_open_file_with_metadata(path, flac_on_meta, out, NULL);
        if (fl) {
            if (fl->sampleRate) out->duration = (double)fl->totalPCMFrameCount / fl->sampleRate;
            drflac_close(fl);
        }
        break;
    }
    case 'o': {
        int err = 0;
        stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
        if (v) {
            stb_vorbis_info info = stb_vorbis_get_info(v);
            if (info.sample_rate)
                out->duration = (double)stb_vorbis_stream_length_in_samples(v) / info.sample_rate;
            stb_vorbis_comment cm = stb_vorbis_get_comment(v);
            for (int i = 0; i < cm.comment_list_length; i++)
                if (cm.comment_list[i]) meta_vorbis_kv(out, cm.comment_list[i], strlen(cm.comment_list[i]));
            stb_vorbis_close(v);
        }
        break;
    }
    default:
        /* m4a/aac/opus/wma/... : tags + duration via FFmpeg (%.4s bounds year). */
        if (disco_ff_handles(path))
            disco_ff_tags(path, out->title, out->artist, out->album, out->year,
                          sizeof(out->title), &out->duration, &out->track);
        break;
    }
}
