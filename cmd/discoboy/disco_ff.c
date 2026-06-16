#include "disco_ff.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Extensions routed to FFmpeg (everything the dr_* / stb backends don't own). */
bool disco_ff_handles(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    static const char *exts[] = {
        ".m4a", ".m4b", ".mp4", ".aac", ".alac", ".opus", ".wma",
        ".aif", ".aiff", ".ape", ".wv", ".mka", ".tta", ".ac3", NULL
    };
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return true;
    return false;
}

void disco_ff_tags(const char *path, char *title, char *artist, char *album,
                   char *year, size_t cap, double *dur, int *track) {
    if (title) title[0] = '\0';
    if (artist) artist[0] = '\0';
    if (album) album[0] = '\0';
    if (year) year[0] = '\0';
    if (dur) *dur = 0;
    if (track) *track = 0;

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) return;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return; }

    /* tags live on the container, sometimes on the audio stream — try both. */
    AVDictionary *dicts[2] = { fmt->metadata, NULL };
    int sidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (sidx >= 0) dicts[1] = fmt->streams[sidx]->metadata;

    for (int d = 0; d < 2; d++) {
        AVDictionary *md = dicts[d];
        if (!md) continue;
        AVDictionaryEntry *e;
        if (title  && !title[0]  && (e = av_dict_get(md, "title",  NULL, 0))) snprintf(title,  cap, "%s", e->value);
        if (artist && !artist[0] && ((e = av_dict_get(md, "artist", NULL, 0)) ||
                                     (e = av_dict_get(md, "album_artist", NULL, 0))))
            snprintf(artist, cap, "%s", e->value);   /* prefer track artist, fall back to album artist */
        if (album  && !album[0]  && (e = av_dict_get(md, "album",  NULL, 0))) snprintf(album,  cap, "%s", e->value);
        if (year   && !year[0]   && (e = av_dict_get(md, "date",   NULL, 0))) snprintf(year,   cap, "%.4s", e->value);
        if (track  && !*track    && (e = av_dict_get(md, "track",  NULL, 0))) *track = atoi(e->value); /* "5" or "5/12" */
    }
    if (dur && fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE)
        *dur = (double)fmt->duration / AV_TIME_BASE;
    avformat_close_input(&fmt);
}

/* Single open stream (the playback thread is the only user). */
static AVFormatContext *s_fmt = NULL;
static AVCodecContext  *s_cc  = NULL;
static SwrContext      *s_swr = NULL;
static AVPacket        *s_pkt = NULL;
static AVFrame         *s_frm = NULL;
static int              s_stream = -1;
static unsigned         s_ch = 0, s_rate = 0;

/* Leftover S16-interleaved samples from the last decoded frame that didn't fit
   the caller's request, carried to the next disco_ff_read(). */
static int16_t *s_buf = NULL;     /* interleaved */
static uint64_t s_buf_cap = 0;    /* capacity in frames */
static uint64_t s_buf_fill = 0;   /* valid frames */
static uint64_t s_buf_pos = 0;    /* read cursor (frames) */
static bool     s_draining = false;

static void disco_ff__reset_buf(void) { s_buf_fill = 0; s_buf_pos = 0; }

void disco_ff_close(void) {
    if (s_swr) { swr_free(&s_swr); s_swr = NULL; }
    if (s_frm) { av_frame_free(&s_frm); s_frm = NULL; }
    if (s_pkt) { av_packet_free(&s_pkt); s_pkt = NULL; }
    if (s_cc)  { avcodec_free_context(&s_cc); s_cc = NULL; }
    if (s_fmt) { avformat_close_input(&s_fmt); s_fmt = NULL; }
    free(s_buf); s_buf = NULL; s_buf_cap = 0;
    disco_ff__reset_buf();
    s_stream = -1; s_ch = 0; s_rate = 0; s_draining = false;
}

unsigned char *disco_ff_cover(const char *path, int *out_size) {
    *out_size = 0;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) return NULL;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return NULL; }

    unsigned char *out = NULL;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        int sz = st->attached_pic.size;
        if (sz > 0) {
            out = (unsigned char *)malloc((size_t)sz);
            if (out) { memcpy(out, st->attached_pic.data, (size_t)sz); *out_size = sz; }
        }
        break;
    }
    avformat_close_input(&fmt);
    return out;
}

bool disco_ff_open(const char *path, unsigned *ch, unsigned *rate, uint64_t *total) {
    disco_ff_close();
    av_log_set_level(AV_LOG_QUIET);

    if (avformat_open_input(&s_fmt, path, NULL, NULL) != 0) return false;
    if (avformat_find_stream_info(s_fmt, NULL) < 0) { disco_ff_close(); return false; }

    s_stream = av_find_best_stream(s_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (s_stream < 0) { disco_ff_close(); return false; }
    AVStream *st = s_fmt->streams[s_stream];

    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) { disco_ff_close(); return false; }
    s_cc = avcodec_alloc_context3(dec);
    if (!s_cc) { disco_ff_close(); return false; }
    if (avcodec_parameters_to_context(s_cc, st->codecpar) < 0) { disco_ff_close(); return false; }
    if (avcodec_open2(s_cc, dec, NULL) < 0) { disco_ff_close(); return false; }

    s_ch   = (unsigned)s_cc->channels;
    s_rate = (unsigned)s_cc->sample_rate;
    if (s_ch == 0 || s_rate == 0) { disco_ff_close(); return false; }

    /* Resample whatever the codec produces (often planar float) to S16
       interleaved, keeping the source rate + channel count. */
    int64_t layout = s_cc->channel_layout
                   ? (int64_t)s_cc->channel_layout
                   : av_get_default_channel_layout(s_cc->channels);
    s_swr = swr_alloc_set_opts(NULL,
                layout, AV_SAMPLE_FMT_S16,  (int)s_rate,
                layout, s_cc->sample_fmt,   (int)s_rate,
                0, NULL);
    if (!s_swr || swr_init(s_swr) < 0) { disco_ff_close(); return false; }

    s_pkt = av_packet_alloc();
    s_frm = av_frame_alloc();
    if (!s_pkt || !s_frm) { disco_ff_close(); return false; }

    /* Total frames: prefer the stream's sample count, else derive from duration. */
    uint64_t tot = 0;
    if (st->duration > 0 && st->duration != AV_NOPTS_VALUE)
        tot = (uint64_t)av_rescale_q(st->duration, st->time_base,
                                     (AVRational){ 1, (int)s_rate });
    else if (s_fmt->duration > 0 && s_fmt->duration != AV_NOPTS_VALUE)
        tot = (uint64_t)((double)s_fmt->duration / AV_TIME_BASE * s_rate);

    *ch = s_ch; *rate = s_rate; *total = tot;
    return true;
}

/* Ensure s_buf holds at least `frames` capacity. */
static bool disco_ff__ensure_cap(uint64_t frames) {
    if (s_buf_cap >= frames) return true;
    int16_t *nb = realloc(s_buf, (size_t)frames * s_ch * sizeof(int16_t));
    if (!nb) return false;
    s_buf = nb; s_buf_cap = frames;
    return true;
}

/* Pull the next decoded+resampled frame into s_buf (replacing any prior
   leftover, which is only called when empty). Returns frames produced, 0=EOF. */
static uint64_t disco_ff__decode_into_buf(void) {
    for (;;) {
        int r = avcodec_receive_frame(s_cc, s_frm);
        if (r == 0) {
            int in_n = s_frm->nb_samples;
            /* worst case swr can emit slightly more than in_n; pad capacity. */
            if (!disco_ff__ensure_cap((uint64_t)in_n + 64)) return 0;
            int out = swr_convert(s_swr, (uint8_t *[]){ (uint8_t *)s_buf },
                                  (int)s_buf_cap,
                                  (const uint8_t **)s_frm->extended_data, in_n);
            av_frame_unref(s_frm);
            if (out <= 0) continue;          /* nothing emitted yet; keep going */
            s_buf_fill = (uint64_t)out; s_buf_pos = 0;
            return s_buf_fill;
        }
        if (r == AVERROR(EAGAIN)) {
            /* feed more input */
            int got = av_read_frame(s_fmt, s_pkt);
            if (got < 0) {
                if (s_draining) return 0;     /* already flushed -> real EOF */
                s_draining = true;
                avcodec_send_packet(s_cc, NULL);   /* enter drain mode */
                continue;
            }
            if (s_pkt->stream_index == s_stream)
                avcodec_send_packet(s_cc, s_pkt);
            av_packet_unref(s_pkt);
            continue;
        }
        return 0;                              /* AVERROR_EOF or hard error */
    }
}

uint64_t disco_ff_read(int16_t *out, uint64_t frames) {
    if (!s_cc) return 0;
    uint64_t produced = 0;
    while (produced < frames) {
        if (s_buf_pos >= s_buf_fill) {
            if (disco_ff__decode_into_buf() == 0) break;   /* EOF */
        }
        uint64_t avail = s_buf_fill - s_buf_pos;
        uint64_t take  = frames - produced;
        if (take > avail) take = avail;
        memcpy(out + produced * s_ch,
               s_buf + s_buf_pos * s_ch,
               (size_t)take * s_ch * sizeof(int16_t));
        s_buf_pos += take;
        produced  += take;
    }
    return produced;
}

void disco_ff_seek(uint64_t frame) {
    if (!s_fmt || s_stream < 0) return;
    AVStream *st = s_fmt->streams[s_stream];
    int64_t ts = av_rescale_q((int64_t)frame, (AVRational){ 1, (int)s_rate }, st->time_base);
    if (av_seek_frame(s_fmt, s_stream, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(s_cc);
        disco_ff__reset_buf();
        s_draining = false;
    }
}
