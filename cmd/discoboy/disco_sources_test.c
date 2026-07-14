#include "disco_sources.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char temp[] = "/tmp/discoboy-sources-XXXXXX";
    char *base = mkdtemp(temp);
    assert(base);
    char primary[1024], missing[1024], list[2050], error[256];
    snprintf(primary, sizeof(primary), "%s/one", base);
    snprintf(missing, sizeof(missing), "%s/two", base);
    assert(mkdir(primary, 0700) == 0);
    snprintf(list, sizeof(list), "%s/:%s/", primary, missing);

    disco_sources sources;
    assert(disco_sources_parse(&sources, list, error, sizeof(error)));
    assert(sources.count == 2);
    assert(strcmp(sources.items[0].id, "primary") == 0);
    assert(strcmp(sources.items[1].id, "secondary_sd") == 0);
    assert(sources.items[0].available == 1);
    assert(sources.items[1].available == 0); /* absent secondary is retained, not fatal */

    snprintf(list, sizeof(list), "%s:%s/", primary, primary);
    assert(!disco_sources_parse(&sources, list, error, sizeof(error)));
    assert(strstr(error, "duplicate") != NULL);
    assert(!disco_sources_parse(&sources, ":/tmp/music", error, sizeof(error)));

    /* A MUSIC_PATHS/SDCARD_PATHS count mismatch is no longer fatal: the two are
       published by independent layers, so resolve keeps the music roots as given. */
    snprintf(list, sizeof(list), "%s:%s", primary, missing);
    setenv("MUSIC_PATHS", list, 1);
    setenv("SDCARD_PATHS", "/card1:/card2:/card3", 1);
    unsetenv("MUSIC_PATH");
    assert(disco_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.count == 2);
    unsetenv("MUSIC_PATHS");

    /* Regression: current firmware exports SDCARD_PATHS (two entries) but not
       MUSIC_PATHS. This must resolve to a single primary root, not fail. */
    setenv("SDCARD_PATH", primary, 1);
    setenv("SDCARD_PATHS", "/card1:/card2", 1);
    assert(disco_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.count == 1);
    unsetenv("SDCARD_PATH");
    unsetenv("SDCARD_PATHS");

    /* The single-root fallback always yields one usable source. */
    setenv("MUSIC_PATH", primary, 1);
    disco_sources_single_fallback(&sources);
    assert(sources.count == 1);
    assert(sources.items[0].available == 1); /* root canonicalized via realpath */
    unsetenv("MUSIC_PATH");

    /* Same metadata means one merged album containing both physical tracks;
       artist remains part of the key so unrelated albums never collapse. */
    assert(disco_album_identity_equal("OST", "Composer", "ost", "composer"));
    assert(!disco_album_identity_equal("OST", "Composer A", "OST", "Composer B"));

    assert(rmdir(primary) == 0);
    assert(rmdir(base) == 0);
    puts("disco_sources_test: ok");
    return 0;
}
