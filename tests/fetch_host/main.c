/* One download with aos_fetch.c, and its outcome as one line of JSON.
 *
 *   t <url> <path> [sha256|-] [size] [cancel-at-bytes]
 */
#include "aos_fetch.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *sha = argc > 3 && strcmp(argv[3], "-") ? argv[3] : NULL;
    uint32_t size = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 0;
    uint32_t cancel_at = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);

    int id = aos_fetch_start(argv[1], argv[2], sha, size);
    if (id <= 0) {
        printf("{\"state\":\"refused\",\"rc\":%d}\n", id);
        return 1;
    }
    aos_fetch_info_t in;
    for (;;) {
        if (!aos_fetch_info(id, &in)) return 3;
        if (in.state != AOS_FETCH_BUSY) break;
        if (cancel_at && in.got >= cancel_at) aos_fetch_cancel(id);
        aos_hal_sleep_ms(5);
    }
    static const char *names[] = { "none", "busy", "done", "failed", "cancelled" };
    for (char *p = in.why; *p; p++)
        if (*p == '"' || *p == '\\') *p = '\'';
    printf("{\"state\":\"%s\",\"status\":%d,\"got\":%u,\"total\":%u,\"resumed\":%u,\"connects\":%u,"
           "\"redirects\":%u,\"retries\":%u,\"ms\":%u,\"host\":\"%s\",\"why\":\"%s\"}\n",
           names[in.state], in.status, (unsigned)in.got, (unsigned)in.total, (unsigned)in.resumed,
           (unsigned)in.connects, (unsigned)in.redirects, (unsigned)in.retries, (unsigned)in.ms, in.host, in.why);
    aos_fetch_release(id);
    return in.state == AOS_FETCH_DONE ? 0 : 1;
}
