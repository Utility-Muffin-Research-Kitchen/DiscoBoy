#include "disco_sources.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int disco_source_list_count(const char *value, int *count,
                                   char *error, size_t error_size) {
    if (!value || !value[0] || !count) {
        disco_source_error(error, error_size, "SDCARD_PATHS is empty");
        return 0;
    }
    int items = 1;
    for (const char *cursor = value; *cursor; cursor++) {
        if (*cursor != ':') continue;
        if (cursor == value || cursor[1] == '\0' || cursor[1] == ':') {
            disco_source_error(error, error_size, "SDCARD_PATHS contains an empty item");
            return 0;
        }
        items++;
    }
    *count = items;
    return 1;
}

static int disco_source_nth_path(const char *value, int index,
                                 char *out, size_t out_size) {
    const char *start = value;
    for (int i = 0; i < index; i++) {
        start = strchr(start, ':');
        if (!start) return 0;
        start++;
    }
    const char *end = strchr(start, ':');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len == 0 || len >= out_size) return 0;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

#if defined(__linux__)
static int disco_mount_field(char *out, size_t out_size, const char *raw) {
    size_t used = 0;
    for (size_t i = 0; raw[i]; i++) {
        unsigned char value = (unsigned char)raw[i];
        if (raw[i] == '\\' &&
            raw[i + 1] >= '0' && raw[i + 1] <= '7' &&
            raw[i + 2] >= '0' && raw[i + 2] <= '7' &&
            raw[i + 3] >= '0' && raw[i + 3] <= '7') {
            value = (unsigned char)(((raw[i + 1] - '0') << 6) |
                                    ((raw[i + 2] - '0') << 3) |
                                    (raw[i + 3] - '0'));
            i += 3;
        }
        if (used + 1 >= out_size) return 0;
        out[used++] = (char)value;
    }
    out[used] = '\0';
    return 1;
}

static int disco_mountinfo_has_root(const char *root) {
    const char *fixture = getenv("DISCO_SOURCE_TEST_MOUNTINFO");
    FILE *fp = fopen(fixture && fixture[0] ? fixture : "/proc/self/mountinfo", "r");
    if (!fp) return 0;
    char *line = NULL;
    size_t capacity = 0;
    int found = 0;
    while (getline(&line, &capacity, fp) >= 0) {
        char *save = NULL;
        char *field = strtok_r(line, " \n", &save);
        for (int index = 0; field && index < 4; index++)
            field = strtok_r(NULL, " \n", &save);
        if (!field) continue;
        char mountpoint[PATH_MAX];
        if (disco_mount_field(mountpoint, sizeof(mountpoint), field) &&
            strcmp(mountpoint, root) == 0) {
            found = 1;
            break;
        }
    }
    free(line);
    fclose(fp);
    return found;
}
#endif

static int disco_secondary_available(const char *card_root,
                                     const char *music_root) {
    struct stat st;
    if (stat(music_root, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    const char *fixture = getenv("DISCO_SOURCE_TEST_AVAILABLE");
    if (fixture && fixture[0])
        return strcmp(fixture, "1") == 0 || strcasecmp(fixture, "true") == 0;
#if defined(__linux__)
    return disco_mountinfo_has_root(card_root);
#else
    (void) card_root;
    return 1;
#endif
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
    int has_music_paths = paths && paths[0];
    char fallback[DISCO_SOURCE_PATH_MAX];
    if (!has_music_paths) {
        const char *music = getenv("MUSIC_PATH");
        const char *sd = getenv("SDCARD_PATH");
        if (music && music[0]) snprintf(fallback, sizeof(fallback), "%s", music);
        else if (sd && sd[0]) snprintf(fallback, sizeof(fallback), "%s/Music", sd);
        else snprintf(fallback, sizeof(fallback), "Music");
        paths = fallback;
    }
    if (!disco_sources_parse(out, paths, error, error_size)) return 0;
    const char *sdcard_paths = getenv("SDCARD_PATHS");
    if (has_music_paths && sdcard_paths && sdcard_paths[0]) {
        int card_count = 0;
        if (!disco_source_list_count(sdcard_paths, &card_count, error, error_size)) {
            memset(out, 0, sizeof(*out));
            return 0;
        }
        if (card_count != out->count) {
            disco_source_error(error, error_size,
                               "MUSIC_PATHS item count does not match SDCARD_PATHS");
            memset(out, 0, sizeof(*out));
            return 0;
        }
        for (int i = 1; i < out->count; i++) {
            char card_root[DISCO_SOURCE_PATH_MAX];
            char normalized[DISCO_SOURCE_PATH_MAX];
            if (!disco_source_nth_path(sdcard_paths, i,
                                       card_root, sizeof(card_root)) ||
                !disco_source_normalize(card_root, normalized,
                                        sizeof(normalized))) {
                disco_source_error(error, error_size,
                                   "SDCARD_PATHS contains an invalid source root");
                memset(out, 0, sizeof(*out));
                return 0;
            }
            out->items[i].available =
                disco_secondary_available(normalized, out->items[i].root);
        }
    }
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
