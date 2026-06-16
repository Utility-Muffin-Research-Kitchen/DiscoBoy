#ifndef DISCO_FF_H
#define DISCO_FF_H

/* FFmpeg-backed decoder for the formats the vendored single-header decoders
   don't cover (m4a/AAC/ALAC, Opus, WMA, AIFF, APE, ...). Links against the
   device's libav* (FFmpeg 4.4); same uniform S16-interleaved contract as the
   dr_* backends so disco_audio.c can dispatch to it. Not thread-safe; one
   stream at a time (matches the single playback thread). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if this path's extension is one we route to FFmpeg (the formats the
   vendored dr_* decoders don't cover). Single source of truth shared by the
   decoder dispatch, the metadata reader, and the file scanner. */
bool     disco_ff_handles(const char *path);

/* Read container tags + duration via an independent context (safe alongside
   playback). Each out buffer is `cap` bytes; any may be left empty. *dur in
   seconds (0 = unknown). *track = track number (0 = unknown). Any out may be NULL. */
void     disco_ff_tags(const char *path, char *title, char *artist, char *album,
                       char *year, size_t cap, double *dur, int *track);

/* Extract embedded cover art (the file's "attached picture", e.g. ID3 APIC /
   FLAC PICTURE / MP4 covr) into a malloc'd buffer of encoded image bytes
   (JPEG/PNG). Returns the buffer (caller frees) + *out_size, or NULL if none. */
unsigned char *disco_ff_cover(const char *path, int *out_size);

/* Open `path`; fill *ch / *rate / *total (frames, 0 = unknown). */
bool     disco_ff_open(const char *path, unsigned *ch, unsigned *rate, uint64_t *total);
/* Read up to `frames` interleaved S16 frames into buf; returns frames (0 = EOF). */
uint64_t disco_ff_read(int16_t *buf, uint64_t frames);
/* Seek to `frame` (best-effort; clamps internally). */
void     disco_ff_seek(uint64_t frame);
void     disco_ff_close(void);

#endif /* DISCO_FF_H */
