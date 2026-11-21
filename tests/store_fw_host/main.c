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
    printf("%s %s kind=%d\n", st.last_ok ? "ok" : "FAILED", st.last, st.last_kind);
}

int main(int argc, char **argv)
{
    static const char *states[] = { "absent", "installed", "update", "by_hand", "set_aside" };
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        char *a = argv[i], *v = strchr(a, '=');
        if (v) *v++ = 0;
        aos_store_status_t st;
        aos_store_status(&st);
        bool queued = true;
        if (!strcmp(a, "url")) { aos_store_set_url(v); continue; }
        else if (!strcmp(a, "dev")) { aos_store_set_dev(v && v[0] == '1'); continue; }
        else if (!strcmp(a, "propose")) {
            char url[200] = "";
            bool took = aos_store_propose(v ? v : "");
            aos_store_proposal(url, sizeof url);
            printf("proposed %d %s\n", took, url);
            continue;
        }
        else if (!strcmp(a, "sources")) {
            aos_store_source_t sr;
            for (int k = 0; aos_store_source(k, &sr); k++)
                printf("source %d %s apps=%d skipped=%d loaded=%d %s\n", k, sr.name[0] ? sr.name : "-", sr.apps, sr.skipped, sr.loaded, sr.url);
            continue;
        }
        else if (!strcmp(a, "probed")) {
            aos_store_probe_t pr;
            aos_store_probe_result(&pr);
            printf("probed %d apps=%d %s\n", pr.ok, pr.apps, pr.source_json);
            continue;
        }
        else if (!strcmp(a, "look")) {
            char url[200];
            queued = aos_store_source_url(v ? v : "", url, sizeof url) && aos_store_probe(url);
        }
        else if (!strcmp(a, "add")) {
            char url[200];
            queued = aos_store_source_url(v ? v : "", url, sizeof url) && aos_store_source_add(url);
        }
        else if (!strcmp(a, "unsource")) {
            char *with = v ? strchr(v, ':') : NULL;
            if (with) *with++ = 0;
            queued = aos_store_source_remove(v ? atoi(v) : 0, with && with[0] == '1');
        }
        else if (!strcmp(a, "sideload")) queued = aos_store_sideload(v ? v : "");
        else if (!strcmp(a, "list")) {
            aos_store_app_t app;
            for (int k = 0; aos_store_app(k, &app); k++)
                printf("%s %s %s %s %s%s\n", app.file, states[app.state], app.ver, app.have[0] ? app.have : "-", app.fits ? "fits" : "no-fit",
                       app.src ? "+" : "");
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
