/*
 * P4OS - Cameras: Frigate's HTTP API (cam_frigate.h says what and why).
 *
 * JSON by hand: the firmware lends no parser to apps, and all this needs is
 * the keys of one object and a few fields of an array of objects. The
 * walker below skips any value it is not asked about (strings with escapes,
 * nested objects and arrays), so Frigate can add fields freely.
 */
#include "cam_frigate.h"

#include "cam_conv.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_CAP        (384 * 1024)
#define THUMB_CAP      (128 * 1024)
#define EVENTS_EVERY   20000
#define GET_MS         8000
#define STACK          10240
#define PRIO           2

/* ---- a tiny JSON walker ------------------------------------------------------ */

static const char *ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *skip_value(const char *p);

static const char *skip_string(const char *p)
{
    if (*p != '"') return NULL;
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!*++p) return NULL;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static const char *skip_value(const char *p)
{
    p = ws(p);
    if (!p || !*p) return NULL;
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                p = skip_string(ws(p));
                p = ws(p);
                if (!p || *p != ':') return NULL;
                p++;
            }
            p = ws(skip_value(p));
            if (!p) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    /* number, true, false, null */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* The string at p into out (escapes: the common ones; \u as '?'). */
static void get_string(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    p = ws(p);
    if (!p || *p != '"') return;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            c = *++p;
            if (!c) break;
            if (c == 'n') c = ' ';
            else if (c == 't') c = ' ';
            else if (c == 'u') { c = '?'; for (int i = 0; i < 4 && p[1]; i++) p++; }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = '\0';
}

typedef void (*member_fn)(const char *key, const char *val, void *ctx);

/* For each member of the object at p. */
static const char *obj_each(const char *p, member_fn fn, void *ctx)
{
    p = ws(p);
    if (!p || *p != '{') return NULL;
    p = ws(p + 1);
    if (*p == '}') return p + 1;
    for (;;) {
        char key[64];
        get_string(p, key, sizeof key);
        p = ws(skip_string(ws(p)));
        if (!p || *p != ':') return NULL;
        const char *val = ws(p + 1);
        fn(key, val, ctx);
        p = ws(skip_value(val));
        if (!p) return NULL;
        if (*p == ',') { p = ws(p + 1); continue; }
        if (*p == '}') return p + 1;
        return NULL;
    }
}

static const char *arr_each(const char *p, void (*fn)(const char *val, void *ctx), void *ctx)
{
    p = ws(p);
    if (!p || *p != '[') return NULL;
    p = ws(p + 1);
    if (*p == ']') return p + 1;
    for (;;) {
        fn(p, ctx);
        p = ws(skip_value(p));
        if (!p) return NULL;
        if (*p == ',') { p = ws(p + 1); continue; }
        if (*p == ']') return p + 1;
        return NULL;
    }
}

/* ---- the config: camera names ---------------------------------------------- */

typedef struct {
    char names[FG_MAX_CAMS][32];
    int  n;
    bool enabled;
} cams_ctx_t;

static void cam_field(const char *key, const char *val, void *ctx)
{
    cams_ctx_t *c = ctx;
    if (!strcmp(key, "enabled") && !strncmp(val, "false", 5)) c->enabled = false;
}

static void cam_member(const char *key, const char *val, void *ctx)
{
    cams_ctx_t *c = ctx;
    if (c->n >= FG_MAX_CAMS) return;
    c->enabled = true;
    obj_each(val, cam_field, c);
    if (c->enabled) snprintf(c->names[c->n++], sizeof c->names[0], "%s", key);
}

static void config_member(const char *key, const char *val, void *ctx)
{
    if (!strcmp(key, "cameras")) obj_each(val, cam_member, ctx);
}

/* ---- the events ---------------------------------------------------------------- */

typedef struct {
    fg_event_t ev[FG_MAX_EVENTS];
    int        n;
} ev_ctx_t;

static void ev_data(const char *key, const char *val, void *ctx)
{
    fg_event_t *e = ctx;
    if (!strcmp(key, "top_score") || (!strcmp(key, "score") && e->score < 0)) {
        e->score = (int)(strtod(val, NULL) * 100.0 + 0.5);
    }
}

static void ev_field(const char *key, const char *val, void *ctx)
{
    fg_event_t *e = ctx;
    if (!strcmp(key, "id")) get_string(val, e->id, sizeof e->id);
    else if (!strcmp(key, "camera")) get_string(val, e->camera, sizeof e->camera);
    else if (!strcmp(key, "label")) get_string(val, e->label, sizeof e->label);
    else if (!strcmp(key, "start_time")) e->start = (int64_t)strtod(val, NULL);
    else if (!strcmp(key, "has_snapshot")) e->has_snapshot = !strncmp(val, "true", 4);
    else if (!strcmp(key, "has_clip")) e->has_clip = !strncmp(val, "true", 4);
    else if (!strcmp(key, "top_score")) ev_data(key, val, e);
    else if (!strcmp(key, "data") && *val == '{') obj_each(val, ev_data, e);
}

static void ev_item(const char *val, void *ctx)
{
    ev_ctx_t *c = ctx;
    if (c->n >= FG_MAX_EVENTS) return;
    fg_event_t *e = &c->ev[c->n];
    memset(e, 0, sizeof *e);
    e->score = -1;
    obj_each(val, ev_field, e);
    /* the id goes into URLs: letters, digits, '.', '-' and '_' only */
    for (const char *p = e->id; *p; p++) {
        char ch = *p;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              ch == '.' || ch == '-' || ch == '_')) {
            e->id[0] = '\0';
            break;
        }
    }
    if (e->id[0]) c->n++;
}

/* ---- the thread ---------------------------------------------------------------- */

static bool fg_stop(void *ctx)
{
    return ((cam_frigate_t *)ctx)->stop;
}

static int get(cam_frigate_t *f, const char *path, uint8_t *buf, int cap, int *len)
{
    cam_url_t u = f->base;
    snprintf(u.path, sizeof u.path, "%s%s", f->base.path, path);
    return cam_http_get(&u, f->user, f->pass, buf, cap, len, GET_MS, fg_stop, f);
}

static void set_error(cam_frigate_t *f, int st)
{
    aos_hal_mutex_lock(f->mx);
    if (st == AOS_TCP_ERR_DNS) snprintf(f->error, sizeof f->error, "%s", _("No se encuentra Frigate"));
    else if (st == 401) snprintf(f->error, sizeof f->error, "%s", _("Frigate pide usuario y contraseña"));
    else if (st < 0) snprintf(f->error, sizeof f->error, "%s", _("Frigate no responde"));
    else snprintf(f->error, sizeof f->error, _("Frigate contestó %d"), st);
    f->state = FG_ERROR;
    f->gen++;
    aos_hal_mutex_unlock(f->mx);
}

static uint16_t *thumb_of(cam_frigate_t *f, aos_jpeg_t *dec, cam_conv_t *conv, const uint8_t *jpg, int n)
{
    int w, h, stride;
    const uint16_t *px = aos_hal_jpeg_decode(dec, jpg, (size_t)n, &w, &h, &stride);
    if (!px) return NULL;
    cam_geom_t g;
    cam_geometry(w, h, f->thumb_w, f->thumb_h, CAM_FILL, &g);
    if (g.ow != f->thumb_w || g.oh != f->thumb_h) return NULL;
    uint16_t *out = malloc((size_t)g.ow * g.oh * 2);
    if (out && !cam_conv_rgb565(conv, px, stride, &g, out)) {
        free(out);
        out = NULL;
    }
    return out;
}

static void fg_free(cam_frigate_t *f)
{
    for (int i = 0; i < f->nev; i++) free(f->ev[i].thumb);
    free(f);
}

static void fg_thread(void *arg)
{
    cam_frigate_t *f = arg;
    uint8_t *buf = malloc(BUF_CAP);
    uint8_t *tb = malloc(THUMB_CAP);
    ev_ctx_t *evc = malloc(sizeof *evc);
    aos_jpeg_t *dec = aos_hal_jpeg_open();
    cam_conv_t conv = { 0 };
    bool have_cams = false;
    uint64_t last = 0;
    while (buf && tb && evc && !f->stop) {
        uint64_t now = aos_hal_uptime_ms();
        if (!f->refresh_req && last && now - last < EVENTS_EVERY) {
            aos_hal_sleep_ms(100);
            continue;
        }
        bool forced = f->refresh_req;
        f->refresh_req = false;
        last = now;
        int len = 0, st;
        if (!have_cams || forced) {
            aos_hal_mutex_lock(f->mx);
            if (f->state != FG_OK) f->state = FG_LOADING;
            f->gen++;
            aos_hal_mutex_unlock(f->mx);
            st = get(f, "/api/config", buf, BUF_CAP, &len);
            if (f->stop) break;
            if (st != 200) { set_error(f, st); continue; }
            cams_ctx_t cc = { .n = 0 };
            if (!obj_each((const char *)buf, config_member, &cc)) {
                set_error(f, 0);
                continue;
            }
            aos_hal_mutex_lock(f->mx);
            memcpy(f->cams, cc.names, sizeof cc.names);
            f->ncams = cc.n;
            f->gen++;
            aos_hal_mutex_unlock(f->mx);
            have_cams = true;
        }
        char path[64];
        snprintf(path, sizeof path, "/api/events?limit=%d&include_thumbnails=0", FG_MAX_EVENTS);
        st = get(f, path, buf, BUF_CAP, &len);
        if (f->stop) break;
        if (st != 200) { set_error(f, st); continue; }
        memset(evc, 0, sizeof *evc);
        arr_each((const char *)buf, ev_item, evc);
        /* keep the thumbnails already fetched */
        aos_hal_mutex_lock(f->mx);
        for (int i = 0; i < evc->n; i++) {
            for (int j = 0; j < f->nev; j++) {
                if (f->ev[j].thumb && !strcmp(f->ev[j].id, evc->ev[i].id)) {
                    evc->ev[i].thumb = f->ev[j].thumb;
                    f->ev[j].thumb = NULL;
                    break;
                }
            }
        }
        for (int j = 0; j < f->nev; j++) free(f->ev[j].thumb);
        memcpy(f->ev, evc->ev, sizeof(fg_event_t) * (size_t)evc->n);
        f->nev = evc->n;
        f->state = FG_OK;
        f->gen++;
        aos_hal_mutex_unlock(f->mx);
        /* then the thumbnails, one by one */
        for (int i = 0; i < evc->n && !f->stop && dec; i++) {
            if (evc->ev[i].thumb) continue;
            char tp[96];
            snprintf(tp, sizeof tp, "/api/events/%s/thumbnail.jpg", evc->ev[i].id);
            if (get(f, tp, tb, THUMB_CAP, &len) != 200 || len < 4) continue;
            uint16_t *px = thumb_of(f, dec, &conv, tb, len);
            if (!px) continue;
            aos_hal_mutex_lock(f->mx);
            bool placed = false;
            for (int j = 0; j < f->nev; j++) {
                if (!f->ev[j].thumb && !strcmp(f->ev[j].id, evc->ev[i].id)) {
                    f->ev[j].thumb = px;
                    placed = true;
                    f->gen++;
                    break;
                }
            }
            aos_hal_mutex_unlock(f->mx);
            if (!placed) free(px);
        }
    }
    if (dec) aos_hal_jpeg_close(dec);
    cam_conv_release(&conv);
    free(evc);
    free(tb);
    free(buf);
    if (!__sync_bool_compare_and_swap(&f->owner, 0, 1)) {
        fg_free(f);
    }
}

cam_frigate_t *cam_frigate_start(const char *url, const char *user, const char *pass,
                                 int thumb_w, int thumb_h)
{
    cam_frigate_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    if (!cam_url_parse(url, &f->base) || f->base.rtsp || !(f->mx = aos_hal_mutex_create())) {
        free(f);
        return NULL;
    }
    /* the base path without its trailing '/', so "/api/..." can follow */
    size_t n = strlen(f->base.path);
    while (n > 0 && f->base.path[n - 1] == '/') f->base.path[--n] = '\0';
    snprintf(f->base_url, sizeof f->base_url, "%s", url);
    n = strlen(f->base_url);
    while (n > 0 && f->base_url[n - 1] == '/') f->base_url[--n] = '\0';
    snprintf(f->user, sizeof f->user, "%s", user && user[0] ? user : f->base.user);
    snprintf(f->pass, sizeof f->pass, "%s", pass && pass[0] ? pass : f->base.pass);
    f->thumb_w = thumb_w;
    f->thumb_h = thumb_h;
    f->state = FG_LOADING;
    if (!aos_hal_thread_start("frigate", fg_thread, f, STACK, PRIO)) {
        free(f);
        return NULL;
    }
    return f;
}

void cam_frigate_refresh(cam_frigate_t *f)
{
    if (f) f->refresh_req = true;
}

void cam_frigate_release(cam_frigate_t *f)
{
    if (!f) return;
    f->stop = true;
    __sync_synchronize();
    if (!__sync_bool_compare_and_swap(&f->owner, 0, 2)) {
        fg_free(f);
    }
}

void cam_frigate_cam(const cam_frigate_t *f, const char *camera, cam_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", camera);
    /* a URL cut short would ask for something else: none at all instead */
    if (snprintf(out->url, sizeof out->url, "%s/api/%s/latest.jpg?h=360", f->base_url, camera) >= (int)sizeof out->url) {
        out->url[0] = '\0';
    }
    if (snprintf(out->url_full, sizeof out->url_full, "%s/api/%s?fps=10&h=720", f->base_url, camera) >=
        (int)sizeof out->url_full) {
        out->url_full[0] = '\0';
    }
    snprintf(out->user, sizeof out->user, "%s", f->user);
    snprintf(out->pass, sizeof out->pass, "%s", f->pass);
    out->refresh_ms = 2000;
}

void cam_frigate_event(const cam_frigate_t *f, const fg_event_t *e, const char *title, cam_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", title);
    if (snprintf(out->url, sizeof out->url, "%s/api/events/%s/snapshot.jpg", f->base_url, e->id) >= (int)sizeof out->url) {
        out->url[0] = '\0';
    }
    snprintf(out->user, sizeof out->user, "%s", f->user);
    snprintf(out->pass, sizeof out->pass, "%s", f->pass);
    out->refresh_ms = 0;
}
