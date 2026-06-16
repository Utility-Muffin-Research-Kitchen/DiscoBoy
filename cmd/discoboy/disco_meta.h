/* Disco Boy metadata reader: per-file duration + tags (title/artist/album/year).
 *
 * Standalone from the playback backend (disco_audio.c) so it can run on a worker
 * thread while a track plays. Tags come from FFmpeg (mp3 + everything it handles),
 * Vorbis comments (flac via dr_flac's metadata callback, ogg via stb_vorbis), and
 * nothing for wav. Any field not found is left as an empty string / 0. */
#ifndef DISCO_META_H
#define DISCO_META_H

typedef struct {
    double duration;     /* seconds, 0 if unknown */
    char   title[160];
    char   artist[160];
    char   album[160];
    char   year[16];
    int    track;        /* track number (0 = unknown) */
} disco_meta;

/* Read duration + tags for `path` into *out (caller zeroes it first, or not -
   this function clears it). Safe to call off the main thread. */
void disco_meta_read(const char *path, disco_meta *out);

#endif /* DISCO_META_H */
