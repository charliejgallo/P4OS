/*
 * P4OS - Traductor: one translation at a time, in the background.
 *
 * Which engine: the one chosen (traductor.txt), or with "auto" the board's
 * own when the packs on the card can do the pair (directly or through
 * English), then the user's server if there is one, then MyMemory.
 *
 * On the board: the app's worker runs the model, and lends it both cores
 * for every matrix (aos_hal_worker_split). The models stay loaded between
 * translations, two at most and only while the PSRAM has room, so the
 * second sentence does not read 11 MB from the card again; the pack stays
 * open because the 8 MB embedding table is read from it, row by row.
 *
 * Online: the request goes out from the UI's timer (aos_hal_http_* does the
 * waiting in a task of its own) and the answer is read there. MyMemory
 * takes 500 bytes a request, so a longer text goes in pieces, one after the
 * other, cut where the sentences end.
 */
#include "td.h"
#include "td_nmt.h"
#include "td_pak.h"

#define MODELS 2
#define MM_PIECE 450

typedef struct {
    char       dir[5];
    td_pak_t   pak;
    td_nmt_t  *m;
    uint32_t   used;
} model_t;

enum { ST_IDLE, ST_LOCAL, ST_NET, ST_DONE };
enum { STG_NONE, STG_LOAD, STG_RUN, STG_PIVOT, STG_NET };

static model_t s_models[MODELS];
static volatile int s_state = ST_IDLE;
static volatile int s_stage = STG_NONE;
static volatile bool s_cancel;
static td_done_fn s_cb;
static td_result_t s_res;
static char *s_text, *s_out, *s_tmp;
static uint32_t s_t0;
static bool s_worker;
/* the network's piece by piece */
static int s_http = -1;
static size_t s_pos;            /* next byte of s_text to send             */
static int s_engine;

/* ---- the two cores ---- */

typedef struct {
    td_job_fn fn;
    void *arg;
} split_t;

static void split_tramp(void *arg, int part)
{
    split_t *s = (split_t *)arg;
    s->fn(s->arg, part, 2);
}

static void par_two(td_job_fn fn, void *arg, void *ctx)
{
    (void)ctx;
    split_t s = {fn, arg};
    if (!aos_hal_worker_split(split_tramp, &s)) fn(arg, 0, 1);
}

static uint32_t clock_ms(void) { return td_now_ms(); }

/* ---- models ---- */

static void model_drop(model_t *k)
{
    if (k->m) {
        td_nmt_free(k->m);
        td_pak_close(&k->pak);
    }
    memset(k, 0, sizeof *k);
}

void td_job_unload(void)
{
    for (int i = 0; i < MODELS; i++) model_drop(&s_models[i]);
}

static uint32_t psram_free(void)
{
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    return fp;
}

/* in the worker */
static td_nmt_t *model_get(const char *src, const char *dst, const char **err)
{
    char dir[5];
    snprintf(dir, sizeof dir, "%.2s%.2s", src, dst);
    for (int i = 0; i < MODELS; i++)
        if (s_models[i].m && !strcmp(s_models[i].dir, dir)) {
            s_models[i].used = td_now_ms();
            return s_models[i].m;
        }
    const td_pack_t *p = td_pack_nmt(src, dst);
    if (!p) {
        *err = "no pack";
        return NULL;
    }
    /* a slot: the empty one, or the one used longest ago; and room for 13 MB */
    int slot = -1;
    for (int i = 0; i < MODELS; i++)
        if (!s_models[i].m) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < MODELS; i++)
            if ((int32_t)(s_models[i].used - s_models[slot].used) < 0) slot = i;
        model_drop(&s_models[slot]);
    }
    if (psram_free() < 16u * 1024 * 1024)
        for (int i = 0; i < MODELS; i++)
            if (i != slot) model_drop(&s_models[i]);
    s_stage = STG_LOAD;
    model_t *k = &s_models[slot];
    uint32_t t0 = td_now_ms();
    if (!td_pak_open(&k->pak, p->path)) {
        *err = "pack";
        return NULL;
    }
    k->m = td_nmt_load(&k->pak, dir, TD_NMT_EMB_CARD, err);
    if (!k->m) {
        td_pak_close(&k->pak);
        return NULL;
    }
    td_scpy(k->dir, sizeof k->dir, dir);
    k->used = td_now_ms();
    td_nmt_set_par(k->m, par_two, NULL);
    td_nmt_set_cancel(k->m, &s_cancel);
    td_nmt_set_clock(k->m, clock_ms);
    aos_hal_log("traductor", "model %s loaded in %lu ms: %u KB, %u KB of PSRAM free", dir,
                (unsigned long)(td_now_ms() - t0), (unsigned)(td_nmt_bytes(k->m) / 1024),
                (unsigned)(psram_free() / 1024));
    return k->m;
}

/* Spanish opens its questions and exclamations; the models forget to */
static void spanish_marks(char *s, size_t cap)
{
    size_t len = strlen(s);
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = s[i];
        if (c == '?' || c == '!' || c == '.' || c == '\n' || c == 0) {
            if ((c == '?' || c == '!') && len + 2 < cap) {
                const char *open = c == '?' ? "\xC2\xBF" : "\xC2\xA1";
                bool has = false;
                for (size_t k = start; k + 1 < i; k++)
                    if ((unsigned char)s[k] == 0xC2 && (unsigned char)s[k + 1] == (unsigned char)open[1]) has = true;
                size_t at = start;
                while (at < i && (s[at] == ' ' || s[at] == '"')) at++;
                if (!has && at < i) {
                    memmove(s + at + 2, s + at, len - at + 1);
                    memcpy(s + at, open, 2);
                    len += 2;
                    i += 2;
                }
            }
            start = i + 1;
        }
    }
}

/* what traductor.txt says about the model, for each translation */
static void model_opts(td_nmt_t *m)
{
    if (!m) return;
    td_nmt_set_int8(m, td_cfg()->nmt_int8);
    td_nmt_set_par(m, td_cfg()->nmt_one_core ? NULL : par_two, NULL);
}

static void local_worker(void *arg)
{
    (void)arg;
    const char *err = NULL;
    td_nmt_stats_t st;
    memset(&st, 0, sizeof st);
    bool ok = false;
    if (s_res.pivot) {
        td_nmt_t *m = model_get(s_res.src, "en", &err);
        model_opts(m);
        s_stage = STG_PIVOT;
        if (m && td_nmt_translate(m, s_text, s_tmp, TD_OUT_MAX, &st)) {
            td_nmt_t *m2 = model_get("en", s_res.dst, &err);
            model_opts(m2);
            s_stage = STG_PIVOT;
            ok = m2 && td_nmt_translate(m2, s_tmp, s_out, TD_OUT_MAX, &st);
        }
    } else {
        td_nmt_t *m = model_get(s_res.src, s_res.dst, &err);
        model_opts(m);
        s_stage = STG_RUN;
        ok = m && td_nmt_translate(m, s_text, s_out, TD_OUT_MAX, &st);
    }
    if (ok) {
        if (!strcmp(s_res.dst, "es")) spanish_marks(s_out, TD_OUT_MAX);
        s_res.err = TD_OK;
        aos_hal_log("traductor", "local %s>%s%s: %lu pieces in, %lu out, %lu M multiply-adds, encoder %lu ms, decoder %lu ms, all %lu ms (%s, %s)",
                    s_res.src, s_res.dst, s_res.pivot ? " via en" : "", (unsigned long)st.src_tokens,
                    (unsigned long)st.out_tokens, (unsigned long)st.macs_m, (unsigned long)st.ms_enc,
                    (unsigned long)st.ms_dec, (unsigned long)(td_now_ms() - s_t0),
                    td_cfg()->nmt_int8 ? "int8" : "float", td_cfg()->nmt_one_core ? "one core" : "two cores");
    } else {
        s_res.err = s_cancel ? TD_ERR_CANCEL : (err && !strcmp(err, "no memory")) ? TD_ERR_MEM : TD_ERR_NO_ROUTE;
        if (err) td_scpy(s_res.msg, sizeof s_res.msg, err);
    }
    s_state = ST_DONE;
}

/* ---- life ---- */

void td_job_init(void)
{
    s_state = ST_IDLE;
    s_http = -1;
    if (!s_text) s_text = (char *)malloc(TD_TEXT_MAX + 1);
    if (!s_out) s_out = (char *)malloc(TD_OUT_MAX);
    if (!s_tmp) s_tmp = (char *)malloc(TD_OUT_MAX);
}

void td_job_stop(void)
{
    s_cancel = true;
    if (s_worker) {
        aos_hal_worker_stop();      /* waits for local_worker, which sees s_cancel */
        s_worker = false;
    }
    if (s_http >= 0) {
        aos_hal_http_release(s_http);
        s_http = -1;
    }
    td_job_unload();
    free(s_text);
    free(s_out);
    free(s_tmp);
    s_text = s_out = s_tmp = NULL;
    s_state = ST_IDLE;
    s_cb = NULL;
}

bool td_job_busy(void) { return s_state == ST_LOCAL || s_state == ST_NET; }

void td_job_cancel(void)
{
    s_cancel = true;
}

const char *td_job_stage(void)
{
    switch (s_stage) {
    case STG_LOAD: return _("Cargando el modelo…");
    case STG_RUN: return _("Traduciendo en la placa…");
    case STG_PIVOT: return _("Traduciendo por el inglés…");
    case STG_NET: return s_engine == TD_E_SERVER ? _("Esperando al servidor…") : _("Esperando a MyMemory…");
    }
    return "";
}

static void finish(void)
{
    s_res.ms = td_now_ms() - s_t0;
    s_res.text = s_out;
    s_state = ST_IDLE;
    s_stage = STG_NONE;
    if (s_cb) s_cb(&s_res);
}

static void fail(td_err_t e)
{
    s_res.err = e;
    s_out[0] = 0;
    finish();
}

/* the next piece of s_text for MyMemory: at most MM_PIECE bytes, cut after
 * a sentence or at least a space */
static size_t next_piece(char *piece, size_t cap)
{
    size_t len = strlen(s_text + s_pos);
    if (len <= MM_PIECE) {
        td_scpy(piece, cap, s_text + s_pos);
        return len;
    }
    const char *t = s_text + s_pos;
    size_t cut = 0, sp = 0;
    for (size_t i = 0; i < MM_PIECE; i++) {
        if ((t[i] == '.' || t[i] == '?' || t[i] == '!' || t[i] == '\n') && (t[i + 1] == ' ' || t[i + 1] == '\n')) cut = i + 1;
        if (t[i] == ' ') sp = i + 1;
    }
    if (!cut) cut = sp ? sp : MM_PIECE;
    while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) cut--;    /* not inside a letter */
    size_t n = cut < cap ? cut : cap - 1;
    memcpy(piece, t, n);
    piece[n] = 0;
    return n;
}

static bool net_send(void)
{
    static char url[2400], body[3200], headers[64];
    char piece[TD_TEXT_MAX + 1];
    const char *method = "GET";
    const char *text = s_text;
    if (s_engine == TD_E_ONLINE) {
        size_t n = next_piece(piece, sizeof piece);
        s_pos += n;
        text = piece;
        td_quota_add((int)strlen(piece));
    } else {
        s_pos = strlen(s_text);
    }
    if (!td_online_request(s_engine, text, s_res.src, s_res.dst, url, sizeof url, &method, body, sizeof body,
                           headers, sizeof headers))
        return false;
    s_http = aos_hal_http_request(method, url, headers[0] ? headers : NULL, body[0] ? body : NULL,
                                  body[0] ? "application/json" : NULL, 64 * 1024);
    return s_http >= 0;
}

bool td_job_start(const char *text, const char *src, const char *dst, td_done_fn cb)
{
    if (td_job_busy() || !s_text) return false;
    if (s_worker) {
        aos_hal_worker_stop();
        s_worker = false;
    }
    memset(&s_res, 0, sizeof s_res);
    td_scpy(s_text, TD_TEXT_MAX + 1, text);
    s_out[0] = 0;
    s_cb = cb;
    s_cancel = false;
    s_t0 = td_now_ms();
    td_scpy(s_res.src, sizeof s_res.src, src);
    td_scpy(s_res.dst, sizeof s_res.dst, dst);

    td_cfg_t *c = td_cfg();
    /* "auto": guessed here when it can be; the online ones also guess */
    if (!strcmp(src, "auto")) {
        static const char *const ALL[] = {"es", "en", "pt", "it", "fr", "de", "ca", "nl"};
        const char *d = td_detect(text, ALL, 8);
        if (d && strcmp(d, dst)) td_scpy(s_res.src, sizeof s_res.src, d);
        else if (d) {
            /* it is already in the target: the other way round is meant */
            td_scpy(s_res.src, sizeof s_res.src, d);
        }
    }
    bool guessed = strcmp(s_res.src, "auto") != 0;
    int steps = guessed ? td_local_steps(s_res.src, dst) : 0;
    int e = c->engine;
    if (e == TD_E_AUTO) e = steps ? TD_E_LOCAL : c->server[0] ? TD_E_SERVER : TD_E_ONLINE;
    s_res.engine = e;
    s_engine = e;
    if (guessed && !strcmp(s_res.src, dst)) {
        s_res.err = TD_OK;
        td_scpy(s_out, TD_OUT_MAX, text);
        s_state = ST_DONE;      /* same language: nothing to do, said in the timer */
        return true;
    }
    if (e == TD_E_LOCAL) {
        if (!steps) {
            s_res.err = TD_ERR_NO_ROUTE;
            s_state = ST_DONE;
            return true;
        }
        s_res.pivot = steps == 2;
        s_state = ST_LOCAL;
        s_stage = STG_LOAD;
        if (!aos_hal_worker_start_on("traductor", local_worker, NULL, 16 * 1024, 0, -1)) {
            s_state = ST_IDLE;
            return false;
        }
        s_worker = true;
        return true;
    }
    if (e == TD_E_SERVER && !c->server[0]) {
        s_res.err = TD_ERR_NO_SERVER;
        s_state = ST_DONE;
        return true;
    }
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        s_res.err = TD_ERR_NO_NET;
        s_state = ST_DONE;
        return true;
    }
    s_pos = 0;
    s_state = ST_NET;
    s_stage = STG_NET;
    if (!net_send()) {
        s_state = ST_IDLE;
        s_res.err = TD_ERR_NET;
        s_state = ST_DONE;
    }
    return true;
}

void td_job_poll(void)
{
    if (s_state == ST_DONE) {
        if (s_worker) {
            aos_hal_worker_stop();
            s_worker = false;
        }
        finish();
        return;
    }
    if (s_state != ST_NET || s_http < 0) return;
    if (s_cancel) {
        aos_hal_http_release(s_http);
        s_http = -1;
        fail(TD_ERR_CANCEL);
        return;
    }
    aos_http_state_t st = aos_hal_http_state(s_http);
    if (st == AOS_HTTP_BUSY) return;
    int status = aos_hal_http_status(s_http);
    /* an HTTP error (400, 403, 429...) is FAILED too, with its code and its
     * body, which says why: only the negative codes are the network's */
    if (st == AOS_HTTP_FAILED && status <= 0) {
        aos_hal_http_release(s_http);
        s_http = -1;
        snprintf(s_res.msg, sizeof s_res.msg, "%d", status);
        aos_hal_log("traductor", "%s: request failed (%d)", s_engine == TD_E_SERVER ? "server" : "mymemory", status);
        fail(TD_ERR_NET);
        return;
    }
    char det[8] = "";
    size_t o = strlen(s_out);
    if (o && o + 2 < TD_OUT_MAX) {
        s_out[o++] = ' ';
        s_out[o] = 0;
    }
    td_err_t err = td_online_parse(s_engine, status, aos_hal_http_body(s_http), s_out + o, TD_OUT_MAX - o, det,
                                   s_res.msg, sizeof s_res.msg);
    aos_hal_http_release(s_http);
    s_http = -1;
    if (err != TD_OK) {
        aos_hal_log("traductor", "%s: error %d (HTTP %d) %.80s", s_engine == TD_E_SERVER ? "server" : "mymemory",
                    (int)err, status, s_res.msg);
        fail(err);
        return;
    }
    if (det[0] && !strcmp(s_res.src, "auto")) td_scpy(s_res.src, sizeof s_res.src, det);
    if (s_text[s_pos] && s_engine == TD_E_ONLINE) {
        if (!net_send()) fail(TD_ERR_NET);
        return;
    }
    s_res.err = TD_OK;
    finish();
}
