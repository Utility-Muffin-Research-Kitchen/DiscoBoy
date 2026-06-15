#include "disco_meta.h"

#include "dr_wav.h"
#include "dr_mp3.h"
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
}

/* ---- ID3v2 (mp3) ---- */

static uint32_t id3_syncsafe(const unsigned char *p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7)  |  (uint32_t)(p[3] & 0x7f);
}

/* Decode an ID3 text-frame body (1 encoding byte + text) to UTF-8 in `dst`. */
static void id3_decode_text(char *dst, size_t cap, const unsigned char *body, uint32_t size,
                            bool is_year) {
    if (size < 1) return;
    unsigned char enc = body[0];
    const unsigned char *t = body + 1;
    uint32_t n = size - 1;
    char tmp[256];
    size_t o = 0;

    if (enc == 0) {                     /* ISO-8859-1 -> UTF-8 */
        for (uint32_t i = 0; i < n && o + 2 < sizeof(tmp); i++) {
            unsigned char c = t[i];
            if (c == 0) break;
            if (c < 0x80) tmp[o++] = (char)c;
            else { tmp[o++] = (char)(0xC0 | (c >> 6)); tmp[o++] = (char)(0x80 | (c & 0x3F)); }
        }
    } else if (enc == 3) {              /* UTF-8 */
        for (uint32_t i = 0; i < n && o + 1 < sizeof(tmp); i++) {
            if (t[i] == 0) break;
            tmp[o++] = (char)t[i];
        }
    } else {                            /* UTF-16 (enc 1 = BOM, enc 2 = BE), BMP only */
        bool be = (enc == 2);
        uint32_t i = 0;
        if (enc == 1 && n >= 2) {       /* byte-order mark */
            if (t[0] == 0xFF && t[1] == 0xFE) { be = false; i = 2; }
            else if (t[0] == 0xFE && t[1] == 0xFF) { be = true; i = 2; }
        }
        for (; i + 1 < n && o + 3 < sizeof(tmp); i += 2) {
            unsigned u = be ? (unsigned)(t[i] << 8 | t[i + 1])
                            : (unsigned)(t[i + 1] << 8 | t[i]);
            if (u == 0) break;
            if (u < 0x80) tmp[o++] = (char)u;
            else if (u < 0x800) {
                tmp[o++] = (char)(0xC0 | (u >> 6));
                tmp[o++] = (char)(0x80 | (u & 0x3F));
            } else {
                tmp[o++] = (char)(0xE0 | (u >> 12));
                tmp[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
                tmp[o++] = (char)(0x80 | (u & 0x3F));
            }
        }
    }
    if (is_year) meta_set_year(dst, cap, tmp, o);
    else         meta_set(dst, cap, tmp, o);
}

static void id3_read(const char *path, disco_meta *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) { fclose(f); return; }
    unsigned ver = hdr[3];
    if (ver != 3 && ver != 4) { fclose(f); return; }   /* handle v2.3 / v2.4 */
    unsigned flags = hdr[5];
    uint32_t tagsize = id3_syncsafe(hdr + 6);
    if (tagsize == 0 || tagsize > 8u * 1024 * 1024) { fclose(f); return; }

    unsigned char *buf = (unsigned char *)malloc(tagsize);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, tagsize, f) != tagsize) { free(buf); fclose(f); return; }
    fclose(f);

    uint32_t pos = 0;
    if (flags & 0x40) {                 /* skip an extended header if present */
        if (pos + 4 <= tagsize) {
            uint32_t ext = (ver == 4) ? id3_syncsafe(buf + pos)
                         : ((uint32_t)buf[pos] << 24 | (uint32_t)buf[pos+1] << 16 |
                            (uint32_t)buf[pos+2] << 8 | buf[pos+3]);
            pos += (ver == 4) ? ext : ext + 4;
        }
    }
    while (pos + 10 <= tagsize) {
        const unsigned char *fr = buf + pos;
        if (fr[0] == 0) break;          /* padding */
        uint32_t fsize = (ver == 4) ? id3_syncsafe(fr + 4)
                       : ((uint32_t)fr[4] << 24 | (uint32_t)fr[5] << 16 |
                          (uint32_t)fr[6] << 8 | fr[7]);
        if (fsize == 0 || pos + 10 + fsize > tagsize) break;
        const unsigned char *body = fr + 10;
        if      (!memcmp(fr, "TIT2", 4)) id3_decode_text(out->title,  sizeof(out->title),  body, fsize, false);
        else if (!memcmp(fr, "TPE1", 4)) id3_decode_text(out->artist, sizeof(out->artist), body, fsize, false);
        else if (!memcmp(fr, "TALB", 4)) id3_decode_text(out->album,  sizeof(out->album),  body, fsize, false);
        else if (!memcmp(fr, "TDRC", 4) || !memcmp(fr, "TYER", 4) || !memcmp(fr, "TDRL", 4))
            id3_decode_text(out->year, sizeof(out->year), body, fsize, true);
        pos += 10 + fsize;
    }
    free(buf);
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
    case 'm': {
        drmp3 m;
        if (drmp3_init_file(&m, path, NULL)) {
            if (m.sampleRate) out->duration = (double)drmp3_get_pcm_frame_count(&m) / m.sampleRate;
            drmp3_uninit(&m);
        }
        id3_read(path, out);
        break;
    }
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
    default: break;
    }
}
