#include "disco_status.h"
#include "cJSON.h"

#include <arpa/inet.h>      /* htonl / ntohl */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* The daemon frames messages as a 4-byte big-endian length + JSON payload
   (Jawaka/internal/ipc/ipc.c). Same contract as scan-library. */

static int io_all(int fd, void *buf, size_t n, bool writing) {
    char *p = (char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = writing ? write(fd, p + off, n - off) : read(fd, p + off, n - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

bool disco_status_query(disco_audio_status *out) {
    out->ok = false;
    out->output[0] = '\0';
    out->volume = -1;

    const char *rt = getenv("JAWAKA_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp/jawaka-runtime";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    /* short timeouts: this runs on the UI thread, never block it for long */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/jawakad.sock", rt)
            >= sizeof(addr.sun_path)) { close(fd); return false; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return false; }

    static const char req[] = "{\"type\":\"platform-audio-status\"}";
    uint32_t len = htonl((uint32_t)(sizeof(req) - 1));
    if (io_all(fd, &len, 4, true) != 0 || io_all(fd, (void *)req, sizeof(req) - 1, true) != 0) {
        close(fd); return false;
    }

    uint32_t rlen = 0;
    if (io_all(fd, &rlen, 4, false) != 0) { close(fd); return false; }
    rlen = ntohl(rlen);
    if (rlen == 0 || rlen > 65536) { close(fd); return false; }
    char *buf = (char *)malloc(rlen + 1);
    if (!buf) { close(fd); return false; }
    if (io_all(fd, buf, rlen, false) != 0) { free(buf); close(fd); return false; }
    buf[rlen] = '\0';
    close(fd);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;
    cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (st) {
        cJSON *ao = cJSON_GetObjectItemCaseSensitive(st, "audio_output");
        cJSON *vp = cJSON_GetObjectItemCaseSensitive(st, "volume_percent");
        if (cJSON_IsString(ao) && ao->valuestring)
            snprintf(out->output, sizeof(out->output), "%s", ao->valuestring);
        if (cJSON_IsNumber(vp)) out->volume = vp->valueint;
        out->ok = true;
    }
    cJSON_Delete(root);
    return out->ok;
}
