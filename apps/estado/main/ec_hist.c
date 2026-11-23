/*
 * P4OS - Mi casa: the history.
 *
 * Per service, by the key of its name (ec_svc_key): the last 24 hours in
 * five-minute buckets and the last 7 days in hours, each with how many
 * checks there were, how many went well, and the average and worst latency
 * of the good ones; and the last outages, with when they started and ended
 * and why. 3.8 KB a service, in PSRAM.
 *
 * Buckets are rings indexed by slot % size, a slot being the epoch over the
 * bucket's length; *_at is the newest slot written, and moving it forward
 * clears the slots skipped (the board was off: no data, not "down"). Nothing
 * is recorded until the clock is set (SNTP): a bucket needs a date.
 *
 * Saved to estado_hist.bin every five minutes when something changed, and
 * when the app closes. The records of services that are gone stay there
 * until room is needed, so a service taken off and put back keeps its past.
 * An outage still open when the file was saved is closed at that moment on
 * loading: what happened while the board was off is not known.
 *
 * estado_eventos.txt is for people and the portal's page: a line per change,
 * "2026-11-23 14:02:11<TAB>name<TAB>down|up|cert<TAB>detail<TAB>ms", the
 * detail a word (timeout, refused, code...), the HTTP code, the seconds it
 * was down, or the days the certificate has left. Past 256 KB it moves to
 * estado_eventos.old.txt and starts again.
 */
#include "ec.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#define FILE_NAME   "estado_hist.bin"
#define LOG_NAME    "estado_eventos.txt"
#define LOG_OLD     "estado_eventos.old.txt"
#define LOG_MAX     (256 * 1024)
#define MAGIC       0x31484345u             /* "ECH1" */
#define RECS        (EC_MAX * 2)
#define SAVE_EVERY  (5u * 60u * 1000u)

typedef struct {
    uint32_t magic, version, count, saved_at;
} file_hdr_t;

static struct {
    ec_hist_t *rec;             /* RECS of them */
    int        idx[EC_MAX];     /* service -> record, -1 none */
    uint32_t   used[RECS];      /* when a record was last given to a service (for reuse) */
    bool       dirty;
    uint32_t   saved_ms;
    bool       loaded;
} H;

uint32_t ec_now_epoch(void)
{
    if (!aos_hal_time_is_valid()) return 0;
    time_t t = time(NULL);
    return t > 1600000000 ? (uint32_t)t : 0;
}

static void path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", aos_hal_path_data(), name);
}

static void load(void)
{
    char path[200];
    path_of(FILE_NAME, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    file_hdr_t hd;
    if (fread(&hd, sizeof hd, 1, f) == 1 && hd.magic == MAGIC && hd.version == sizeof(ec_hist_t)) {
        int n = (int)(hd.count < RECS ? hd.count : RECS);
        for (int i = 0; i < n; i++) {
            ec_hist_t *r = &H.rec[i];
            if (fread(r, sizeof *r, 1, f) != 1) {
                memset(r, 0, sizeof *r);
                break;
            }
            if (r->out_n > EC_OUT_N) r->out_n = EC_OUT_N;
            for (int k = 0; k < r->out_n; k++) {
                ec_out_t *o = &r->out[k];
                if (!o->end) o->end = hd.saved_at > o->start ? hd.saved_at : o->start;
            }
        }
    }
    fclose(f);
}

void ec_hist_init(void)
{
    if (!H.rec) {
        H.rec = ec_psram(sizeof(ec_hist_t) * RECS);
        if (H.rec) memset(H.rec, 0, sizeof(ec_hist_t) * RECS);
    }
    for (int i = 0; i < EC_MAX; i++) H.idx[i] = -1;
    if (H.rec && !H.loaded) {
        load();
        H.loaded = true;
    }
    H.saved_ms = (uint32_t)aos_hal_uptime_ms();
}

void ec_hist_free(void)
{
    free(H.rec);
    memset(&H, 0, sizeof H);
}

void ec_hist_sync(const ec_cfg_t *c)
{
    if (!H.rec) return;
    bool taken[RECS] = { false };
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = 0; i < EC_MAX; i++) H.idx[i] = -1;
    for (int i = 0; i < c->n; i++) {
        uint32_t key = ec_svc_key(&c->s[i]);
        for (int r = 0; r < RECS; r++)
            if (H.rec[r].key == key && !taken[r]) {
                H.idx[i] = r;
                taken[r] = true;
                break;
            }
    }
    for (int i = 0; i < c->n; i++) {
        if (H.idx[i] >= 0) continue;
        /* an empty record, or the one given out longest ago */
        int best = -1;
        for (int r = 0; r < RECS; r++) {
            if (taken[r]) continue;
            if (!H.rec[r].key) { best = r; break; }
            if (best < 0 || (int32_t)(H.used[r] - H.used[best]) < 0) best = r;
        }
        if (best < 0) continue;
        memset(&H.rec[best], 0, sizeof H.rec[best]);
        H.rec[best].key = ec_svc_key(&c->s[i]);
        H.idx[i] = best;
        taken[best] = true;
        H.dirty = true;
    }
    for (int i = 0; i < c->n; i++)
        if (H.idx[i] >= 0) H.used[H.idx[i]] = now;
}

ec_hist_t *ec_hist(int i)
{
    if (!H.rec || i < 0 || i >= EC_MAX || H.idx[i] < 0) return NULL;
    return &H.rec[H.idx[i]];
}

/* moves a ring forward to 'slot'; false if slot is too old to write */
static bool ring_to(ec_bk_t *ring, int n, uint32_t *at, uint32_t slot)
{
    if (!*at || slot >= *at + (uint32_t)n) {
        memset(ring, 0, sizeof *ring * (size_t)n);
        *at = slot;
        return true;
    }
    if (slot > *at) {
        for (uint32_t s = *at + 1; s <= slot; s++) memset(&ring[s % (uint32_t)n], 0, sizeof *ring);
        *at = slot;
        return true;
    }
    return *at - slot < (uint32_t)n;
}

static void bk_add(ec_bk_t *b, bool ok, int ms)
{
    if (b->n < 0xFFFF) b->n++;
    if (!ok || b->ok == 0xFFFF) return;
    if (ms < 0) ms = 0;
    if (ms > 0xFFFF) ms = 0xFFFF;
    b->avg_ms = (uint16_t)(((uint32_t)b->avg_ms * b->ok + (uint32_t)ms) / (uint32_t)(b->ok + 1));
    b->ok++;
    if ((uint16_t)ms > b->max_ms) b->max_ms = (uint16_t)ms;
}

void ec_hist_add(int i, bool ok, int ms)
{
    ec_hist_t *h = ec_hist(i);
    uint32_t e = ec_now_epoch();
    if (!h || !e) return;
    uint32_t s5 = e / 300, sh = e / 3600;
    if (ring_to(h->b5, EC_B5_N, &h->b5_at, s5)) bk_add(&h->b5[s5 % EC_B5_N], ok, ms);
    if (ring_to(h->h, EC_H_N, &h->h_at, sh)) bk_add(&h->h[sh % EC_H_N], ok, ms);
    H.dirty = true;
}

int32_t ec_hist_outage(int i, bool down, int err)
{
    ec_hist_t *h = ec_hist(i);
    uint32_t e = ec_now_epoch();
    if (!h || !e) return -1;
    ec_out_t *last = h->out_n ? &h->out[h->out_n - 1] : NULL;
    if (down) {
        if (last && !last->end) return -1;
        if (h->out_n == EC_OUT_N) {
            memmove(&h->out[0], &h->out[1], sizeof h->out[0] * (EC_OUT_N - 1));
            h->out_n--;
        }
        ec_out_t *o = &h->out[h->out_n++];
        memset(o, 0, sizeof *o);
        o->start = e;
        o->err = (uint8_t)err;
        H.dirty = true;
        return 0;
    }
    if (!last || last->end) return -1;
    last->end = e > last->start ? e : last->start;
    H.dirty = true;
    return (int32_t)(last->end - last->start);
}

static bool b5_valid(const ec_hist_t *h, uint32_t slot)
{
    return h->b5_at && slot <= h->b5_at && h->b5_at - slot < EC_B5_N;
}

static bool h_valid(const ec_hist_t *h, uint32_t slot)
{
    return h->h_at && slot <= h->h_at && h->h_at - slot < EC_H_N;
}

const ec_bk_t *ec_hist_b5(const ec_hist_t *h, uint32_t slot)
{
    if (!h || !b5_valid(h, slot)) return NULL;
    const ec_bk_t *b = &h->b5[slot % EC_B5_N];
    return b->n ? b : NULL;
}

const ec_bk_t *ec_hist_h(const ec_hist_t *h, uint32_t slot)
{
    if (!h || !h_valid(h, slot)) return NULL;
    const ec_bk_t *b = &h->h[slot % EC_H_N];
    return b->n ? b : NULL;
}

float ec_hist_avail5(const ec_hist_t *h, int buckets, uint32_t now_slot)
{
    uint32_t n = 0, ok = 0;
    for (int k = 0; k < buckets; k++) {
        const ec_bk_t *b = ec_hist_b5(h, now_slot - (uint32_t)k);
        if (b) {
            n += b->n;
            ok += b->ok;
        }
    }
    return n ? 100.0f * (float)ok / (float)n : -1.0f;
}

float ec_hist_availh(const ec_hist_t *h, int hours, uint32_t now_slot)
{
    uint32_t n = 0, ok = 0;
    for (int k = 0; k < hours; k++) {
        const ec_bk_t *b = ec_hist_h(h, now_slot - (uint32_t)k);
        if (b) {
            n += b->n;
            ok += b->ok;
        }
    }
    return n ? 100.0f * (float)ok / (float)n : -1.0f;
}

void ec_hist_save_if(bool force)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!H.rec || !H.dirty) return;
    if (!force && now - H.saved_ms < SAVE_EVERY) return;
    H.saved_ms = now;
    char path[200], part[210];
    path_of(FILE_NAME, path, sizeof path);
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) return;
    ec_lock();
    H.dirty = false;
    int count = 0;
    for (int r = 0; r < RECS; r++) count += H.rec[r].key != 0;
    ec_unlock();
    file_hdr_t hd = { MAGIC, sizeof(ec_hist_t), (uint32_t)count, ec_now_epoch() };
    bool ok = fwrite(&hd, sizeof hd, 1, f) == 1;
    static ec_hist_t one;           /* one record at a time, out of the lock */
    int wrote = 0;
    for (int r = 0; r < RECS && ok && wrote < count; r++) {
        ec_lock();
        bool have = H.rec[r].key != 0;
        if (have) one = H.rec[r];
        ec_unlock();
        if (!have) continue;
        ok = fwrite(&one, sizeof one, 1, f) == 1;
        wrote++;
    }
    /* a record taken while writing: the count in the head says how many follow */
    if (ok && wrote != count) {
        hd.count = (uint32_t)wrote;
        ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(&hd, sizeof hd, 1, f) == 1;
    }
    ok = ok && !ferror(f);
    fclose(f);
    if (!ok) {
        unlink(part);
        H.dirty = true;
        return;
    }
    unlink(path);
    rename(part, path);
}

void ec_hist_log(const char *name, const char *what, const char *detail, int ms)
{
    char path[200];
    path_of(LOG_NAME, path, sizeof path);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX) {
        char old[200];
        path_of(LOG_OLD, old, sizeof old);
        unlink(old);
        rename(path, old);
    }
    FILE *f = fopen(path, "a");
    if (!f) return;
    char when[64] = "-";
    uint32_t e = ec_now_epoch();
    if (e) {
        time_t t = (time_t)e;
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    fprintf(f, "%s\t%s\t%s\t%s\t%d\n", when, name, what, detail ? detail : "", ms);
    fclose(f);
}
