#include "disco_audio.h"
#include "disco_ff.h"

#include "dr_wav.h"
#include "dr_mp3.h"
#include "dr_flac.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* Output device: BLUETOOTH -> the BlueALSA `bluealsa` ALSA pcm (A2DP straight to
   the headset); anything else -> ALSA `default` (the PulseAudio plugin, which the
   daemon routes to speaker / headset / HDMI). The desired output is tracked live
   from the daemon (disco_audio_set_output) rather than the static launch-time env,
   so plugging in headphones / connecting BT mid-track re-routes playback. */
static const char *disco__device_for(int bt) { return bt ? "bluealsa" : "default"; }

static int disco__env_is_bt(void) {
    const char *o = getenv("JAWAKA_AUDIO_OUTPUT");
    if (!o || !o[0]) o = getenv("UMRK_AUDIO_OUTPUT");
    return (o && strcasecmp(o, "BLUETOOTH") == 0) ? 1 : 0;
}

/* ---- decoder abstraction: WAV / MP3 / FLAC / OGG behind a uniform S16 read ----
   Each backend is a single-header decoder vendored under third_party/. We pick one
   by file extension, then the playback thread only ever calls disco__dec_read(). */
typedef enum { DEC_NONE = 0, DEC_WAV, DEC_MP3, DEC_FLAC, DEC_OGG, DEC_FF } disco_dec;

static disco_dec   s_kind = DEC_NONE;
static drwav       s_wav;
static drmp3       s_mp3;
static drflac     *s_flac = NULL;
static stb_vorbis *s_ogg  = NULL;
static int         s_ogg_ch = 0;

static disco_dec disco__kind_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return DEC_NONE;
    if (strcasecmp(dot, ".wav")  == 0) return DEC_WAV;
    if (strcasecmp(dot, ".mp3")  == 0) return DEC_MP3;
    if (strcasecmp(dot, ".flac") == 0) return DEC_FLAC;
    if (strcasecmp(dot, ".ogg")  == 0 ||
        strcasecmp(dot, ".oga")  == 0) return DEC_OGG;
    if (disco_ff_handles(path)) return DEC_FF;   /* m4a/aac/opus/wma/... via FFmpeg */
    return DEC_NONE;
}

/* Open `path` with the matching decoder; fill *ch / *rate / *total (frames, 0=unknown). */
static bool disco__dec_open(const char *path, unsigned *ch, unsigned *rate, uint64_t *total) {
    s_kind = disco__kind_for(path);
    switch (s_kind) {
    case DEC_WAV:
        if (!drwav_init_file(&s_wav, path, NULL)) break;
        *ch = s_wav.channels; *rate = s_wav.sampleRate; *total = s_wav.totalPCMFrameCount;
        return true;
    case DEC_MP3:
        if (!drmp3_init_file(&s_mp3, path, NULL)) break;
        *ch = s_mp3.channels; *rate = s_mp3.sampleRate;
        *total = drmp3_get_pcm_frame_count(&s_mp3);   /* scans the whole file... */
        drmp3_seek_to_pcm_frame(&s_mp3, 0);           /* ...so rewind to the start */
        return true;
    case DEC_FLAC:
        s_flac = drflac_open_file(path, NULL);
        if (!s_flac) break;
        *ch = s_flac->channels; *rate = s_flac->sampleRate; *total = s_flac->totalPCMFrameCount;
        return true;
    case DEC_OGG: {
        int err = 0;
        s_ogg = stb_vorbis_open_filename(path, &err, NULL);
        if (!s_ogg) break;
        stb_vorbis_info info = stb_vorbis_get_info(s_ogg);
        s_ogg_ch = info.channels;
        *ch = (unsigned)info.channels; *rate = info.sample_rate;
        *total = stb_vorbis_stream_length_in_samples(s_ogg);
        return true;
    }
    case DEC_FF:
        if (!disco_ff_open(path, ch, rate, total)) break;
        return true;
    default: break;
    }
    s_kind = DEC_NONE;
    return false;
}

/* Read up to `frames` interleaved S16 frames into buf; returns frames decoded (0 = EOF). */
static uint64_t disco__dec_read(int16_t *buf, uint64_t frames) {
    switch (s_kind) {
    case DEC_WAV:  return drwav_read_pcm_frames_s16(&s_wav, frames, buf);
    case DEC_MP3:  return drmp3_read_pcm_frames_s16(&s_mp3, frames, buf);
    case DEC_FLAC: return drflac_read_pcm_frames_s16(s_flac, frames, buf);
    case DEC_OGG: {
        int got = stb_vorbis_get_samples_short_interleaved(
                      s_ogg, s_ogg_ch, buf, (int)(frames * (uint64_t)s_ogg_ch));
        return got < 0 ? 0 : (uint64_t)got;   /* returns frames (samples per channel) */
    }
    case DEC_FF:   return disco_ff_read(buf, frames);
    default: return 0;
    }
}

static void disco__dec_close(void) {
    switch (s_kind) {
    case DEC_WAV:  drwav_uninit(&s_wav); break;
    case DEC_MP3:  drmp3_uninit(&s_mp3); break;
    case DEC_FLAC: if (s_flac) { drflac_close(s_flac);  s_flac = NULL; } break;
    case DEC_OGG:  if (s_ogg)  { stb_vorbis_close(s_ogg); s_ogg  = NULL; } break;
    case DEC_FF:   disco_ff_close(); break;
    default: break;
    }
    s_kind = DEC_NONE;
}

/* ---- ALSA output ---- */
static snd_pcm_t        *s_pcm = NULL;
static bool              s_open = false;
static unsigned          s_channels = 0;
static unsigned          s_rate = 0;
static uint64_t          s_total = 0;        /* total frames, 0 = unknown */
static volatile uint64_t s_cursor = 0;       /* frames decoded so far */
static pthread_t         s_thread;
static bool              s_have_thread = false;
static volatile bool     s_paused = false;
static volatile bool     s_stop = false;
static volatile bool     s_finished = false;
static volatile int      s_desired_bt = 0;   /* live target: 1 = route to bluealsa */
static int               s_cur_bt = 0;       /* what the open pcm currently targets */
static volatile int      s_reopen = 0;       /* set to ask the thread to re-route */
static volatile int      s_volume = 100;     /* 0-100 software gain, applied on BT only */
static volatile int      s_seek = 0;         /* set to ask the thread to seek */
static volatile uint64_t s_seek_target = 0;  /* target frame for the pending seek */

/* Swap the open PCM to the desired output device (runs in the playback thread when
   the daemon reports a routing change). Keeps the decoder position - a brief gap is
   expected. Keeps the current device if the new one fails to open. */
static void disco__reopen_device(void) {
    int bt = s_desired_bt;
    if (bt == s_cur_bt) return;
    snd_pcm_t *np = NULL;
    if (snd_pcm_open(&np, disco__device_for(bt), SND_PCM_STREAM_PLAYBACK, 0) == 0 &&
        snd_pcm_set_params(np, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                           s_channels, s_rate, 1, 200000) == 0) {
        if (s_pcm) { snd_pcm_drop(s_pcm); snd_pcm_close(s_pcm); }
        s_pcm = np;
        s_cur_bt = bt;
    } else {
        if (np) snd_pcm_close(np);
        s_desired_bt = s_cur_bt;   /* give up this switch, don't spin retrying */
    }
}

/* Seek the decoder to s_seek_target frames (runs in the playback thread, so no
   decoder race). Drops the buffered audio so the new position is heard promptly. */
static void disco__do_seek(void) {
    uint64_t f = s_seek_target;
    switch (s_kind) {
    case DEC_WAV:  drwav_seek_to_pcm_frame(&s_wav, f); break;
    case DEC_MP3:  drmp3_seek_to_pcm_frame(&s_mp3, f); break;
    case DEC_FLAC: drflac_seek_to_pcm_frame(s_flac, f); break;
    case DEC_OGG:  if (s_ogg) stb_vorbis_seek_frame(s_ogg, (unsigned)f); break;
    case DEC_FF:   disco_ff_seek(f); break;
    default: break;
    }
    s_cursor = f;
    if (s_pcm) { snd_pcm_drop(s_pcm); snd_pcm_prepare(s_pcm); }
}

/* Playback thread: pull S16 frames from the decoder and write them to ALSA. */
static void *disco__thread(void *arg) {
    (void)arg;
    int16_t buf[8192];
    uint64_t cap = sizeof(buf) / sizeof(buf[0]) / (s_channels ? s_channels : 1);
    while (!s_stop) {
        if (s_paused) {
            snd_pcm_drop(s_pcm);
            while (s_paused && !s_stop) usleep(20000);
            if (s_stop) break;
            snd_pcm_prepare(s_pcm);
            continue;
        }
        if (s_reopen) { disco__reopen_device(); s_reopen = 0; }
        if (s_seek)   { disco__do_seek(); s_seek = 0; }
        uint64_t got = disco__dec_read(buf, cap);
        if (got == 0) { s_finished = true; break; }
        /* Volume is the daemon's job now (jawakad's watch-only proxy adjusts the
           system volume on the hardware keys while an app is foreground), so the
           app no longer applies its own gain — same path as games. */
        uint64_t off = 0;
        while (off < got && !s_stop) {
            snd_pcm_sframes_t w =
                snd_pcm_writei(s_pcm, buf + off * s_channels, (snd_pcm_uframes_t)(got - off));
            if (w < 0) {
                w = snd_pcm_recover(s_pcm, (int)w, 1);
                if (w < 0) { s_finished = true; goto done; }
                continue;
            }
            off += (uint64_t)w;
        }
        s_cursor += got;
    }
done:
    if (s_pcm) snd_pcm_drain(s_pcm);
    return NULL;
}

void disco_audio_init(void) {
    s_desired_bt = disco__env_is_bt();   /* seed from the launch-time output */
    s_cur_bt = s_desired_bt;
}

void disco_audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
}

/* name: SPEAKER / HEADSET / HDMI / BLUETOOTH (case-insensitive). */
void disco_audio_set_output(const char *name) {
    int bt = (name && strcasecmp(name, "BLUETOOTH") == 0) ? 1 : 0;
    if (bt != s_desired_bt) { s_desired_bt = bt; s_reopen = 1; }
}

/* Seek by delta_seconds relative to the current position (clamped to the track). */
void disco_audio_seek(double delta_seconds) {
    if (!s_open || !s_rate) return;
    long long tgt = (long long)s_cursor + (long long)(delta_seconds * (double)s_rate);
    if (tgt < 0) tgt = 0;
    if (s_total && (uint64_t)tgt > s_total) tgt = (long long)s_total;
    s_seek_target = (uint64_t)tgt;
    s_seek = 1;
}

void disco_audio_stop(void) {
    if (s_have_thread) {
        s_stop = true;
        pthread_join(s_thread, NULL);
        s_have_thread = false;
    }
    if (s_pcm)  { snd_pcm_close(s_pcm); s_pcm = NULL; }
    if (s_open) { disco__dec_close(); s_open = false; }
}

void disco_audio_shutdown(void) { disco_audio_stop(); }

bool disco_audio_play(const char *path) {
    disco_audio_stop();
    s_stop = false; s_paused = false; s_finished = false; s_cursor = 0;
    s_channels = 0; s_rate = 0; s_total = 0;

    if (!disco__dec_open(path, &s_channels, &s_rate, &s_total)) return false;
    s_open = true;
    if (s_channels == 0 || s_rate == 0) { disco__dec_close(); s_open = false; return false; }

    int bt = s_desired_bt;
    s_reopen = 0;
    if (snd_pcm_open(&s_pcm, disco__device_for(bt), SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        s_pcm = NULL; bt = 0;
        if (snd_pcm_open(&s_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            s_pcm = NULL; disco__dec_close(); s_open = false; return false;
        }
    }
    s_cur_bt = bt;
    if (snd_pcm_set_params(s_pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                           s_channels, s_rate, 1, 200000) < 0) {
        snd_pcm_close(s_pcm); s_pcm = NULL; disco__dec_close(); s_open = false; return false;
    }
    if (pthread_create(&s_thread, NULL, disco__thread, NULL) != 0) {
        snd_pcm_close(s_pcm); s_pcm = NULL; disco__dec_close(); s_open = false; return false;
    }
    s_have_thread = true;
    return true;
}

void disco_audio_pause(void)  { s_paused = true; }
void disco_audio_resume(void) { s_paused = false; }

bool disco_audio_is_playing(void) { return s_have_thread && !s_paused && !s_finished; }
bool disco_audio_finished(void)   { return s_finished; }

double disco_audio_duration(void) {
    return (s_rate && s_total) ? (double)s_total / (double)s_rate : 0.0;
}
double disco_audio_position(void) {
    return s_rate ? (double)s_cursor / (double)s_rate : 0.0;
}
