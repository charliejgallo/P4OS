/* The store's service (components/aos_store) driven from the command line:
 * each argument is a step, done in order, each waited for.
 *
 *   P4_CARD=<folder> t url=<catalogue> refresh install=golf install=pinball:all
 *                      remove=golf adopt list
 *
 * A job prints "ok|FAILED <how it ended>"; "list" prints an app a line:
 * file state ver have fits.
 */
#include "aos_store.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wait_done(uint32_t before)
{
    aos_store_status_t st;
    for (;;) {
        aos_store_status(&st);
        if (!st.busy && st.jobs_done != before) break;
        aos_hal_sleep_ms(5);
    }
    printf("%s %s\n", st.last_ok ? "ok" : "FAILED", st.last);
}

int main(int argc, char **argv)
{
    static const char *states[] = { "absent", "installed", "update", "by_hand" };
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        char *a = argv[i], *v = strchr(a, '=');
        if (v) *v++ = 0;
        aos_store_status_t st;
        aos_store_status(&st);
        bool queued = true;
        if (!strcmp(a, "url")) { aos_store_set_url(v); continue; }
        else if (!strcmp(a, "list")) {
            aos_store_app_t app;
            for (int k = 0; aos_store_app(k, &app); k++)
                printf("%s %s %s %s %s\n", app.file, states[app.state], app.ver, app.have[0] ? app.have : "-", app.fits ? "fits" : "no-fit");
            continue;
        }
        else if (!strcmp(a, "refresh")) queued = aos_store_refresh();
        else if (!strcmp(a, "adopt")) queued = aos_store_adopt();
        else if (!strcmp(a, "remove")) queued = aos_store_remove(v ? v : "");
        else if (!strcmp(a, "install")) {
            char *packs = v ? strchr(v, ':') : NULL;
            if (packs) *packs++ = 0;
            queued = aos_store_install(v ? v : "", packs);
        } else return 2;
        if (!queued) { printf("FAILED refused: %s\n", a); continue; }
        wait_done(st.jobs_done);
    }
    return 0;
}
