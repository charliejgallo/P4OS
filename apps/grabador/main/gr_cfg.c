/*
 * P4OS - Grabador: the settings, and the recordings on the card.
 *
 * <card>/grabador/grabador.txt, one "key=value" a line, so the portal's page
 * (and anyone with Archivos) can read and write them.
 */
#include "gr.h"

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

void gr_cfg_defaults(gr_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->armed = false;
    c->res = 0;
    c->fps = 10;
    c->kbps = 1500;
    c->sens = 1;
    c->pre_s = 3;
    c->post_s = 10;
    c->clip_max_s = 300;
    c->space_mb = 2048;
    c->from_h = 22;
    c->to_h = 7;
    c->zones = (1u << (GR_ZONES_W * GR_ZONES_H)) - 1;
    c->rtsp = true;
    c->rtsp_port = 8554;
    c->notify = true;
}

const char *gr_dir(char *out, size_t n)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd) return NULL;
    snprintf(out, n, "%s/%s", sd, GR_DIR);
    mkdir(out, 0777);
    return out;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void gr_cfg_load(gr_cfg_t *c)
{
    gr_cfg_defaults(c);
    char dir[96], path[128];
    if (!gr_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/grabador.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "armed")) c->armed = v != 0;
        else if (!strcmp(line, "res")) c->res = clampi(v, 0, 1);
        else if (!strcmp(line, "fps")) c->fps = clampi(v, 2, 15);
        else if (!strcmp(line, "kbps")) c->kbps = clampi(v, 300, 8000);
        else if (!strcmp(line, "sens")) c->sens = clampi(v, 0, 2);
        else if (!strcmp(line, "pre_s")) c->pre_s = clampi(v, 0, 5);
        else if (!strcmp(line, "post_s")) c->post_s = clampi(v, 3, 120);
        else if (!strcmp(line, "clip_max_s")) c->clip_max_s = clampi(v, 30, 1800);
        else if (!strcmp(line, "space_mb")) c->space_mb = clampi(v, 100, 1000000);
        else if (!strcmp(line, "sched")) c->sched = v != 0;
        else if (!strcmp(line, "from_h")) c->from_h = clampi(v, 0, 23);
        else if (!strcmp(line, "to_h")) c->to_h = clampi(v, 0, 23);
        else if (!strcmp(line, "zones")) c->zones = (uint16_t)(v & 0xFFF);
        else if (!strcmp(line, "rtsp")) c->rtsp = v != 0;
        else if (!strcmp(line, "rtsp_port")) c->rtsp_port = clampi(v, 1, 65535);
        else if (!strcmp(line, "notify")) c->notify = v != 0;
    }
    fclose(f);
}

void gr_cfg_save(const gr_cfg_t *c)
{
    char dir[96], path[128];
    if (!gr_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/grabador.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "armed=%d\nres=%d\nfps=%d\nkbps=%d\nsens=%d\npre_s=%d\npost_s=%d\nclip_max_s=%d\nspace_mb=%d\n"
               "sched=%d\nfrom_h=%d\nto_h=%d\nzones=%d\nrtsp=%d\nrtsp_port=%d\nnotify=%d\n",
            c->armed, c->res, c->fps, c->kbps, c->sens, c->pre_s, c->post_s, c->clip_max_s, c->space_mb,
            c->sched, c->from_h, c->to_h, c->zones, c->rtsp, c->rtsp_port, c->notify);
    fclose(f);
}

void gr_res_size(int res, int *w, int *h)
{
    *w = res ? 1280 : 640;
    *h = res ? 720 : 480;
}

/* ---------------------------------------------------------------- recordings */

static bool clip_name(const char *n)
{
    size_t l = strlen(n);
    return l == 19 && n[8] == '-' && !strcmp(n + 15, ".mp4");
}

static int newest_first(const void *a, const void *b)
{
    return strcmp(((const gr_clip_t *)b)->name, ((const gr_clip_t *)a)->name);
}

int gr_clips(gr_clip_t *out, int max)
{
    char dir[96];
    if (!gr_dir(dir, sizeof dir)) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!clip_name(e->d_name)) continue;
        char p[160];
        snprintf(p, sizeof p, "%s/%.31s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        memcpy(out[n].name, e->d_name, 20);     /* clip_name(): 19 and the NUL */
        out[n].bytes = (uint32_t)st.st_size;
        struct tm tm = { 0 };
        if (sscanf(e->d_name, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                   &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            tm.tm_isdst = -1;
            out[n].when = mktime(&tm);
        }
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, newest_first);
    return n;
}

bool gr_clip_delete(const char *name)
{
    char dir[96], p[160];
    if (!gr_dir(dir, sizeof dir) || !clip_name(name)) return false;
    snprintf(p, sizeof p, "%s/%.31s", dir, name);
    bool ok = remove(p) == 0;
    snprintf(p, sizeof p, "%s/%.15s.jpg", dir, name);
    remove(p);
    return ok;
}

void gr_clips_trim(int space_mb)
{
    static gr_clip_t c[GR_CLIPS_MAX];
    int n = gr_clips(c, GR_CLIPS_MAX);
    uint64_t total = 0, limit = (uint64_t)space_mb * 1024 * 1024;
    for (int i = 0; i < n; i++) total += c[i].bytes;
    for (int i = n - 1; i > 0 && total > limit; i--) {      /* never the newest */
        aos_hal_log("grabador", "space: %s goes (%u MB in all, the limit %d)", c[i].name, (unsigned)(total >> 20), space_mb);
        total -= c[i].bytes;
        gr_clip_delete(c[i].name);
    }
}
