#include "disco_sources.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static void disco_source_error(char *out, size_t n, const char *message) {
    if (out && n) snprintf(out, n, "%s", message);
}

static int disco_source_normalize(const char *input, char *out, size_t n) {
    char resolved[PATH_MAX];
    const char *value = realpath(input, resolved) ? resolved : input;
    size_t value_len = strlen(value);
    if (value_len >= n) return 0;
    memcpy(out, value, value_len + 1);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    return 1;
}

int disco_sources_parse(disco_sources *out, const char *music_paths,
                        char *error, size_t error_size) {
    if (!out || !music_paths || !music_paths[0]) {
        disco_source_error(error, error_size, "music path list is empty");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    const char *start = music_paths;
    while (1) {
        const char *end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len == 0) {
            disco_source_error(error, error_size, "MUSIC_PATHS contains an empty item");
            return 0;
        }
        if (out->count >= DISCO_SOURCE_MAX || len >= DISCO_SOURCE_PATH_MAX) {
            disco_source_error(error, error_size, "MUSIC_PATHS has too many or oversized items");
            return 0;
        }
        char raw[DISCO_SOURCE_PATH_MAX];
        memcpy(raw, start, len);
        raw[len] = '\0';
        disco_source *source = &out->items[out->count];
        if (!disco_source_normalize(raw, source->root, sizeof(source->root))) {
            disco_source_error(error, error_size, "MUSIC_PATHS resolved path is too long");
            memset(out, 0, sizeof(*out));
            return 0;
        }
        for (int i = 0; i < out->count; i++) {
            if (strcmp(source->root, out->items[i].root) == 0) {
                disco_source_error(error, error_size, "MUSIC_PATHS contains a duplicate root");
                memset(out, 0, sizeof(*out));
                return 0;
            }
        }
        if (out->count == 0) snprintf(source->id, sizeof(source->id), "primary");
        else if (out->count == 1) snprintf(source->id, sizeof(source->id), "secondary_sd");
        else snprintf(source->id, sizeof(source->id), "source%d", out->count + 1);
        snprintf(source->label, sizeof(source->label), "SD%d", out->count + 1);
        struct stat st;
        source->available = stat(source->root, &st) == 0 && S_ISDIR(st.st_mode);
        out->count++;
        if (!end) break;
        start = end + 1;
    }
    if (error && error_size) error[0] = '\0';
    return 1;
}

int disco_sources_resolve(disco_sources *out, char *error, size_t error_size) {
    const char *paths = getenv("MUSIC_PATHS");
    char fallback[DISCO_SOURCE_PATH_MAX];
    if (!paths || !paths[0]) {
        const char *music = getenv("MUSIC_PATH");
        const char *sd = getenv("SDCARD_PATH");
        if (music && music[0]) snprintf(fallback, sizeof(fallback), "%s", music);
        else if (sd && sd[0]) snprintf(fallback, sizeof(fallback), "%s/Music", sd);
        else snprintf(fallback, sizeof(fallback), "Music");
        paths = fallback;
    }
    if (!disco_sources_parse(out, paths, error, error_size)) return 0;
    /* MUSIC_PATHS source count is intentionally NOT cross-checked against
       SDCARD_PATHS. The two are published by different, independently-versioned
       layers (the music roots vs. the mounted-card list), so a mismatch is normal
       -- e.g. firmware that exports SDCARD_PATHS but not yet MUSIC_PATHS. */
    const char *primary = getenv("MUSIC_PATH");
    if (primary && primary[0]) {
        char normalized[DISCO_SOURCE_PATH_MAX];
        if (!disco_source_normalize(primary, normalized, sizeof(normalized)) ||
            strcmp(normalized, out->items[0].root) != 0) {
            disco_source_error(error, error_size, "MUSIC_PATH does not match MUSIC_PATHS primary");
            memset(out, 0, sizeof(*out));
            return 0;
        }
    }
    return 1;
}

void disco_sources_single_fallback(disco_sources *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const char *music = getenv("MUSIC_PATH");
    const char *sd = getenv("SDCARD_PATH");
    char raw[DISCO_SOURCE_PATH_MAX];
    if (music && music[0]) snprintf(raw, sizeof(raw), "%s", music);
    else if (sd && sd[0]) snprintf(raw, sizeof(raw), "%s/Music", sd);
    else snprintf(raw, sizeof(raw), "Music");
    disco_source *source = &out->items[0];
    if (!disco_source_normalize(raw, source->root, sizeof(source->root)))
        snprintf(source->root, sizeof(source->root), "%s", raw);
    snprintf(source->id, sizeof(source->id), "primary");
    snprintf(source->label, sizeof(source->label), "SD1");
    struct stat st;
    source->available = stat(source->root, &st) == 0 && S_ISDIR(st.st_mode);
    out->count = 1;
}

int disco_album_identity_equal(const char *album_a, const char *artist_a,
                               const char *album_b, const char *artist_b) {
    return strcasecmp(album_a ? album_a : "", album_b ? album_b : "") == 0 &&
           strcasecmp(artist_a ? artist_a : "", artist_b ? artist_b : "") == 0;
}
