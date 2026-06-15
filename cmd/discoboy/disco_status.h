/* Disco Boy live audio-status poll.
 *
 * Queries the jawakad daemon's `platform-audio-status` over its UNIX socket so the
 * app tracks the CURRENT audio output (SPEAKER / HEADSET / HDMI / BLUETOOTH) and
 * volume live - the launch-time JAWAKA_AUDIO_OUTPUT env is only a snapshot, so it
 * goes stale when the user plugs in headphones or connects Bluetooth mid-session.
 * Used to label the output, switch the playback device live, and (on Bluetooth,
 * where the daemon's volume doesn't reach our raw bluealsa stream) drive a software
 * volume. */
#ifndef DISCO_STATUS_H
#define DISCO_STATUS_H

#include <stdbool.h>

typedef struct {
    char output[16];   /* "SPEAKER" / "HEADSET" / "HDMI" / "BLUETOOTH", or "" */
    int  volume;       /* 0-100, or -1 if unknown */
    bool ok;
} disco_audio_status;

/* Query the daemon. Returns true and fills *out on success; cheap, short timeout. */
bool disco_status_query(disco_audio_status *out);

#endif /* DISCO_STATUS_H */
