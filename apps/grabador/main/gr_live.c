/*
 * P4OS - Grabador: its page in the portal (web/grabador.js), through the
 * live channel (aos_hal_live_*, docs/PORTAL-PAGES.md).
 *
 * While the page looks (it asked something in the last six seconds), once a
 * second: "state", a JSON with how it is, the settings and the clips, and
 * the newest picture as <data>/grabador.jpg (its path in the JSON, for
 * /api/fs/get). The page sends short texts:
 *   arm 0|1        rec 0|1        del <name>
 *   set <key> <value>   (a key of grabador.txt)
 */
#include "gr.h"

#include <stdlib.h>

static uint32_t s_last;
static gr_clip_t s_c[GR_CLIPS_MAX];

static void put_state(void)
{
    gr_status_t st;
    gr_engine_status(&st);
    const gr_cfg_t *c = &gr_app.cfg;
    static char js[8192];
    const char *sd = aos_hal_path_sd_root();
    const char *data = aos_hal_path_data();
    const char *rel = sd && !strncmp(data, sd, strlen(sd)) ? data + strlen(sd) : data;
    int n = snprintf(js, sizeof js,
                     "{\"state\":%d,\"why\":\"%s\",\"camera\":\"%s\",\"w\":%d,\"h\":%d,\"fps\":%.1f,\"kbps\":%u,"
                     "\"level\":%.3f,\"moving\":%s,\"manual\":%s,\"rec_ms\":%u,\"rec_file\":\"%s\",\"viewers\":%d,"
                     "\"jpg\":\"%s/grabador.jpg\",\"dir\":\"/%s\",\"device\":\"%s\","
                     "\"cfg\":{\"armed\":%d,\"res\":%d,\"fps\":%d,\"kbps\":%d,\"sens\":%d,\"pre_s\":%d,\"post_s\":%d,"
                     "\"clip_max_s\":%d,\"space_mb\":%d,\"sched\":%d,\"from_h\":%d,\"to_h\":%d,\"zones\":%d,\"rtsp\":%d,"
                     "\"rtsp_port\":%d,\"notify\":%d},\"clips\":[",
                     st.state, st.why, st.camera, st.w, st.h, (double)st.fps, (unsigned)st.kbps, (double)st.level,
                     st.moving ? "true" : "false", st.manual ? "true" : "false", (unsigned)st.rec_ms, st.rec_file,
                     st.viewers, rel, GR_DIR, aos_hal_device_name(), c->armed, c->res, c->fps, c->kbps, c->sens,
                     c->pre_s, c->post_s, c->clip_max_s, c->space_mb, c->sched, c->from_h, c->to_h, c->zones, c->rtsp,
                     c->rtsp_port, c->notify);
    int nc = gr_clips(s_c, GR_CLIPS_MAX);
    for (int i = 0; i < nc && i < 100 && n < (int)sizeof js - 80; i++)
        n += snprintf(js + n, sizeof js - (size_t)n, "%s{\"name\":\"%s\",\"bytes\":%u}", i ? "," : "", s_c[i].name,
                      (unsigned)s_c[i].bytes);
    if (n < (int)sizeof js - 4) n += snprintf(js + n, sizeof js - (size_t)n, "]}");
    aos_hal_live_put(GR_APP_ID, "state", "application/json", js, (size_t)n);
}

static void set_key(const char *k, int v)
{
    gr_cfg_t *c = &gr_app.cfg;
    struct { const char *k; int *p; int lo, hi; } I[] = {
        { "res", &c->res, 0, 1 }, { "fps", &c->fps, 2, 15 }, { "kbps", &c->kbps, 300, 8000 }, { "sens", &c->sens, 0, 2 },
        { "pre_s", &c->pre_s, 0, 5 }, { "post_s", &c->post_s, 3, 120 }, { "clip_max_s", &c->clip_max_s, 30, 1800 },
        { "space_mb", &c->space_mb, 100, 1000000 }, { "from_h", &c->from_h, 0, 23 }, { "to_h", &c->to_h, 0, 23 },
    };
    for (size_t i = 0; i < sizeof I / sizeof I[0]; i++)
        if (!strcmp(k, I[i].k)) *I[i].p = v < I[i].lo ? I[i].lo : v > I[i].hi ? I[i].hi : v;
    if (!strcmp(k, "zones")) c->zones = (uint16_t)(v & 0xFFF) ? (uint16_t)(v & 0xFFF) : 1;
    if (!strcmp(k, "sched")) c->sched = v != 0;
    if (!strcmp(k, "rtsp")) c->rtsp = v != 0;
    if (!strcmp(k, "notify")) c->notify = v != 0;
    if (!strcmp(k, "armed")) c->armed = v != 0;
}

void gr_live_tick(void)
{
    char m[128];
    bool changed = false;
    while (aos_hal_live_take(GR_APP_ID, m, sizeof m) > 0) {
        char k[32] = "";
        int v = 0;
        if (!strncmp(m, "arm ", 4)) { gr_app.cfg.armed = atoi(m + 4) != 0; changed = true; }
        else if (!strncmp(m, "rec ", 4)) gr_engine_record(atoi(m + 4) != 0);
        else if (!strncmp(m, "del ", 4)) gr_clip_delete(m + 4);
        else if (sscanf(m, "set %31s %d", k, &v) == 2) { set_key(k, v); changed = true; }
        s_last = 0;
    }
    if (changed) {
        gr_cfg_save(&gr_app.cfg);
        gr_engine_cfg(&gr_app.cfg);
        aos_ui_boot_start_set(GR_APP_ID, gr_app.cfg.armed);
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (aos_hal_live_idle_ms(GR_APP_ID) > 6000 || now - s_last < 1000) return;
    s_last = now;
    put_state();
    gr_engine_want_jpeg();
}
