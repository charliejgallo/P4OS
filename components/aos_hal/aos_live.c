/*
 * P4OS - live data between an app and its page in the portal (aos_hal.h,
 * docs/PORTAL-PAGES.md "Live data"). The board and the simulator alike.
 *
 * A page that only had the card to talk through could follow a file that
 * changes every few seconds (Notes), not a spectrum 25 times a second: the
 * card is slow for that and wears. This keeps it in memory instead:
 *
 *   - blobs: the app puts named values ("state", "spec"), the latest of each
 *     kept, in PSRAM; the page reads them, GET /api/live?app=&key= (the
 *     portal, aos_portal_live.c);
 *   - messages: the page posts short texts, POST /api/live?app=, queued
 *     for the app, which takes them from its timer;
 *   - and the app can ask how long ago a page last asked for anything of
 *     it, so it makes the data only while someone looks.
 *
 * Everything under one mutex; a blob is copied in and out whole (64 KB at
 * most), never handed out.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#define BLOBS   24
#define QUEUE   16

typedef struct {
    char app[24], key[16], type[32];
    void *data;
    size_t len, cap;
    uint32_t seq;
    uint64_t at;
} blob_t;

typedef struct {
    char app[24];
    char text[AOS_LIVE_MSG_MAX + 1];
} msg_t;

static struct {
    void *mx;
    blob_t b[BLOBS];
    msg_t *q;                       /* QUEUE of them, a ring, in PSRAM */
    int qhead, qn;
    char seen_app[8][24];           /* who a page asked about, and when */
    uint64_t seen_at[8];
} L;

static void *big(size_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

static bool ready(void)
{
    if (!L.mx) {
        L.mx = aos_hal_mutex_create();
        L.q = big(QUEUE * sizeof(msg_t));
    }
    return L.mx && L.q;
}

static bool name_ok(const char *s, size_t max)
{
    return s && *s && strlen(s) < max;
}

bool aos_hal_live_put(const char *app, const char *key, const char *type, const void *data, size_t len)
{
    if (!ready() || !name_ok(app, 24) || !name_ok(key, 16) || len > AOS_LIVE_BLOB_MAX) return false;
    aos_hal_mutex_lock(L.mx);
    blob_t *b = NULL, *free_one = NULL;
    for (int i = 0; i < BLOBS && !b; i++) {
        if (!L.b[i].app[0]) {
            if (!free_one) free_one = &L.b[i];
        } else if (!strcmp(L.b[i].app, app) && !strcmp(L.b[i].key, key)) b = &L.b[i];
    }
    if (!b) b = free_one;
    bool ok = b != NULL;
    if (b && b->cap < len) {
        void *p = big(len ? len : 1);
        if (p) {
            free(b->data);
            b->data = p;
            b->cap = len;
        } else ok = false;
    }
    if (ok) {
        snprintf(b->app, sizeof b->app, "%s", app);
        snprintf(b->key, sizeof b->key, "%s", key);
        snprintf(b->type, sizeof b->type, "%s", type && *type ? type : "application/octet-stream");
        if (len) memcpy(b->data, data, len);
        b->len = len;
        b->seq++;
        b->at = aos_hal_uptime_ms();
    }
    aos_hal_mutex_unlock(L.mx);
    return ok;
}

static void seen(const char *app)
{
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(L.seen_app[i], app)) {
            slot = i;
            break;
        }
        if (L.seen_at[i] < L.seen_at[slot]) slot = i;
    }
    snprintf(L.seen_app[slot], sizeof L.seen_app[slot], "%s", app);
    L.seen_at[slot] = aos_hal_uptime_ms();
}

int aos_hal_live_get(const char *app, const char *key, void *out, size_t max, char *type, size_t type_len,
                     uint32_t *seq)
{
    if (!ready() || !name_ok(app, 24) || !name_ok(key, 16)) return -1;
    aos_hal_mutex_lock(L.mx);
    seen(app);
    int r = -1;
    for (int i = 0; i < BLOBS; i++) {
        blob_t *b = &L.b[i];
        if (!b->app[0] || strcmp(b->app, app) || strcmp(b->key, key)) continue;
        if (b->len <= max) {
            if (b->len) memcpy(out, b->data, b->len);
            r = (int)b->len;
            if (type) snprintf(type, type_len, "%s", b->type);
            if (seq) *seq = b->seq;
        }
        break;
    }
    aos_hal_mutex_unlock(L.mx);
    return r;
}

int aos_hal_live_list(const char *app, aos_live_info_t *out, int max)
{
    if (!ready() || !name_ok(app, 24)) return 0;
    aos_hal_mutex_lock(L.mx);
    seen(app);
    int n = 0;
    uint64_t now = aos_hal_uptime_ms();
    for (int i = 0; i < BLOBS && n < max; i++) {
        blob_t *b = &L.b[i];
        if (!b->app[0] || strcmp(b->app, app)) continue;
        snprintf(out[n].key, sizeof out[n].key, "%s", b->key);
        snprintf(out[n].type, sizeof out[n].type, "%s", b->type);
        out[n].len = (uint32_t)b->len;
        out[n].seq = b->seq;
        out[n].age_ms = (uint32_t)(now - b->at);
        n++;
    }
    aos_hal_mutex_unlock(L.mx);
    return n;
}

bool aos_hal_live_push(const char *app, const char *text, size_t len)
{
    if (!ready() || !name_ok(app, 24) || len > AOS_LIVE_MSG_MAX) return false;
    aos_hal_mutex_lock(L.mx);
    seen(app);
    bool ok = L.qn < QUEUE;
    if (ok) {
        msg_t *m = &L.q[(L.qhead + L.qn) % QUEUE];
        snprintf(m->app, sizeof m->app, "%s", app);
        memcpy(m->text, text, len);
        m->text[len] = 0;
        L.qn++;
    }
    aos_hal_mutex_unlock(L.mx);
    return ok;
}

int aos_hal_live_take(const char *app, char *out, int max)
{
    if (!ready() || !name_ok(app, 24) || max <= 0) return 0;
    aos_hal_mutex_lock(L.mx);
    int r = 0;
    for (int k = 0; k < L.qn; k++) {
        msg_t *m = &L.q[(L.qhead + k) % QUEUE];
        if (strcmp(m->app, app)) continue;
        r = snprintf(out, max, "%s", m->text);
        if (r >= max) r = max - 1;
        /* out of the queue, keeping the others in order */
        for (int j = k; j + 1 < L.qn; j++) L.q[(L.qhead + j) % QUEUE] = L.q[(L.qhead + j + 1) % QUEUE];
        L.qn--;
        break;
    }
    aos_hal_mutex_unlock(L.mx);
    return r;
}

uint32_t aos_hal_live_idle_ms(const char *app)
{
    if (!ready() || !name_ok(app, 24)) return UINT32_MAX;
    aos_hal_mutex_lock(L.mx);
    uint32_t r = UINT32_MAX;
    for (int i = 0; i < 8; i++)
        if (!strcmp(L.seen_app[i], app)) r = (uint32_t)(aos_hal_uptime_ms() - L.seen_at[i]);
    aos_hal_mutex_unlock(L.mx);
    return r;
}

void aos_hal_live_clear(const char *app)
{
    if (!ready() || !name_ok(app, 24)) return;
    aos_hal_mutex_lock(L.mx);
    for (int i = 0; i < BLOBS; i++)
        if (L.b[i].app[0] && !strcmp(L.b[i].app, app)) {
            free(L.b[i].data);
            memset(&L.b[i], 0, sizeof L.b[i]);
        }
    for (int k = 0; k < L.qn;) {
        msg_t *m = &L.q[(L.qhead + k) % QUEUE];
        if (strcmp(m->app, app)) {
            k++;
            continue;
        }
        for (int j = k; j + 1 < L.qn; j++) L.q[(L.qhead + j) % QUEUE] = L.q[(L.qhead + j + 1) % QUEUE];
        L.qn--;
    }
    aos_hal_mutex_unlock(L.mx);
}
