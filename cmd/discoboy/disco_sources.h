#ifndef DISCO_SOURCES_H
#define DISCO_SOURCES_H

#include <stddef.h>

#define DISCO_SOURCE_MAX 8
#define DISCO_SOURCE_PATH_MAX 1024

typedef struct {
    char id[32];
    char label[16];
    char root[DISCO_SOURCE_PATH_MAX];
    int  available;
} disco_source;

typedef struct {
    disco_source items[DISCO_SOURCE_MAX];
    int count;
} disco_sources;

/* Resolve MUSIC_PATHS in order, falling back to MUSIC_PATH, SDCARD_PATH/Music,
   then ./Music. Existing roots are canonicalized with realpath; duplicates and
   malformed colon lists are rejected. */
int disco_sources_resolve(disco_sources *out, char *error, size_t error_size);
int disco_sources_parse(disco_sources *out, const char *music_paths,
                        char *error, size_t error_size);

/* Album identity intentionally includes artist so identically named releases
   from different artists do not collapse. Matching files still remain distinct. */
int disco_album_identity_equal(const char *album_a, const char *artist_a,
                               const char *album_b, const char *artist_b);

#endif
