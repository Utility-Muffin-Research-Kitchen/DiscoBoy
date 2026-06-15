/* Disco Boy audio backend.
 *
 * Decodes a file and streams it to an ALSA pcm in a playback thread. WAV, MP3,
 * FLAC and OGG (Vorbis) via vendored single-header decoders (dr_wav / dr_mp3 /
 * dr_flac / stb_vorbis), dispatched by file extension. The output device follows
 * Leaf's audio routing:
 * with JAWAKA_AUDIO_OUTPUT=BLUETOOTH it opens the BlueALSA `bluealsa` pcm (A2DP
 * straight to the headset), otherwise ALSA `default` (PulseAudio -> speaker). */
#ifndef DISCO_AUDIO_H
#define DISCO_AUDIO_H

#include <stdbool.h>

void   disco_audio_init(void);
void   disco_audio_shutdown(void);

/* Start playing a file. Returns false if it could not be opened/decoded. */
bool   disco_audio_play(const char *path);
void   disco_audio_pause(void);
void   disco_audio_resume(void);
void   disco_audio_stop(void);

/* Live audio routing + volume (driven by the daemon's platform-audio-status):
   set_output re-routes the playback device (SPEAKER/HEADSET/HDMI/BLUETOOTH);
   set_volume applies a software gain on the Bluetooth path (where the daemon's
   volume doesn't reach our raw bluealsa stream). */
void   disco_audio_set_output(const char *name);
void   disco_audio_set_volume(int percent);
void   disco_audio_seek(double delta_seconds);   /* relative seek, clamped to the track */

bool   disco_audio_is_playing(void);   /* device open and not paused */
bool   disco_audio_finished(void);     /* the current track reached its end */
double disco_audio_position(void);     /* seconds into the track */
double disco_audio_duration(void);     /* track length in seconds, 0 if unknown */

#endif /* DISCO_AUDIO_H */
