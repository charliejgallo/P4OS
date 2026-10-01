/*
 * P4OS - the Claude plan's usage, from Anthropic's own endpoint
 * (aos_claude.h has the what and the why).
 *
 * The OAuth details are Claude Code's own public client: the same login
 * page, the same token endpoint. They are in one block below and each can
 * be overridden from the preferences (claude_auth, claude_token,
 * claude_redir, claude_api, claude_client) without a new firmware, for the
 * day Anthropic moves one; the simulator's tests point them at
 * tools/fake_claude_api.py the same way.
 */
#include "aos_claude.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

/* ---- the OAuth client (Claude Code's) ---- */
#define DEF_AUTH    "https://claude.ai/oauth/authorize"
#define DEF_TOKEN   "https://console.anthropic.com/v1/oauth/token"
#define DEF_REDIR   "https://console.anthropic.com/oauth/code/callback"
#define DEF_API     "https://api.anthropic.com"
#define DEF_CLIENT  "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define SCOPES      "user:profile user:inference"
#define BETA        "oauth-2025-04-20"

#define POLL_S       180         /* between readings, nobody watching */
#define POLL_LOOK_S   60         /* with the app open */
#define HIST_STEP_S  300         /* one history point every 5 minutes */

typedef struct { uint32_t t; uint8_t p5, p7; } __attribute__((packed)) hist_t;   /* halves of a percent */

static struct {
    void *mx;
    bool started;
    aos_claude_status_t st;
    /* the login in progress */
    char verifier[64], state_nonce[48];
    char pending_code[256];
    bool have_pending;
    /* the tokens */
    char at[512], rt[512];
    int64_t exp;                /* epoch seconds */
    bool kick;
    uint32_t look_ms;
    /* history, a ring */
    hist_t *hist;
    int hn, hpos;
    uint32_t hist_last;
} C;

static void lock(void) { aos_hal_mutex_lock(C.mx); }
static void unlock(void) { aos_hal_mutex_unlock(C.mx); }

static void cfg(const char *key, const char *def, char *out, size_t n)
{
    if (!aos_hal_pref_get_str(key, out, n) || !out[0]) snprintf(out, n, "%s", def);
}

static void set_state(aos_claude_state_t s, const char *err)
{
    lock();
    C.st.state = s;
    snprintf(C.st.error, sizeof C.st.error, "%s", err ? err : "");
    C.st.seq++;
    unlock();
}

/* ---- small pieces: randomness, base64url, the URL encoding ---- */

static void random_bytes(uint8_t *b, size_t n)
{
#ifdef ESP_PLATFORM
    esp_fill_random(b, n);
#else
    arc4random_buf(b, n);
#endif
}

static void b64url(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t k = 0;
    for (size_t i = 0; i < n && k + 4 < cap; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[k++] = A[v >> 18 & 63];
        out[k++] = A[v >> 12 & 63];
        if (i + 1 < n) out[k++] = A[v >> 6 & 63];
        if (i + 2 < n) out[k++] = A[v & 63];
    }
    out[k] = 0;
}

static void urlenc(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    for (; *in && k + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) out[k++] = (char)c;
        else k += (size_t)snprintf(out + k, cap - k, "%%%02X", c);
    }
    out[k] = 0;
}

/* ---- HTTP: aos_hal_http_* is asynchronous; this thread just waits ---- */

static int http(const char *method, const char *url, const char *headers, const char *body, char **resp)
{
    *resp = NULL;
    int id = aos_hal_http_request(method, url, headers, body, body ? "application/json" : NULL, 64 * 1024);
    if (id < 0) return AOS_HTTP_ERR_CONNECT;
    for (int i = 0; i < 400 && aos_hal_http_state(id) == AOS_HTTP_BUSY; i++) aos_hal_sleep_ms(50);   /* 20 s */
    aos_http_state_t s = aos_hal_http_state(id);
    int code = s == AOS_HTTP_BUSY ? AOS_HTTP_ERR_RECV : aos_hal_http_status(id);
    if (s != AOS_HTTP_BUSY && aos_hal_http_body(id)) *resp = strdup(aos_hal_http_body(id));   /* a 4xx has its reason in the body */
    aos_hal_http_release(id);
    return code;
}

static const char *net_reason(int code)
{
    switch (code) {
    case AOS_HTTP_ERR_DNS: return _("no se encuentra el servidor (DNS)");
    case AOS_HTTP_ERR_CONNECT: return _("no se pudo conectar");
    case AOS_HTTP_ERR_SIN_HORA: return _("la placa todavía no tiene hora para validar el certificado");
    case AOS_HTTP_ERR_TLS: return _("el certificado del servidor no se pudo verificar");
    default: return _("la red cortó la respuesta");
    }
}

/* The error an OAuth server or the API put in the body, for the user. */
static void body_error(const char *body, int code, char *out, size_t n)
{
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *msg = NULL;
    if (j) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
        if (cJSON_IsString(e)) msg = e->valuestring;
        else if (cJSON_IsObject(e)) {
            const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
            if (cJSON_IsString(m)) msg = m->valuestring;
        }
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "error_description");
        if (cJSON_IsString(d)) msg = d->valuestring;
    }
    snprintf(out, n, "HTTP %d%s%.100s", code, msg ? ": " : "", msg ? msg : "");
    cJSON_Delete(j);
}

/* ---- the tokens ---- */

static void tokens_load(void)
{
    if (!aos_hal_pref_get_str("claude_at", C.at, sizeof C.at)) C.at[0] = 0;
    if (!aos_hal_pref_get_str("claude_rt", C.rt, sizeof C.rt)) C.rt[0] = 0;
    int32_t e = 0;
    aos_hal_pref_get_i32("claude_exp", &e);
    C.exp = (uint32_t)e;
}

static void tokens_save(void)
{
    aos_hal_pref_set_str("claude_at", C.at);
    aos_hal_pref_set_str("claude_rt", C.rt);
    aos_hal_pref_set_i32("claude_exp", (int32_t)(uint32_t)C.exp);
}

/* An answer of the token endpoint: keep what it gave. */
static bool tokens_take(const char *body)
{
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "access_token");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "refresh_token");
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
    bool ok = cJSON_IsString(a) && a->valuestring[0] && strlen(a->valuestring) < sizeof C.at;
    if (ok) {
        lock();
        snprintf(C.at, sizeof C.at, "%s", a->valuestring);
        if (cJSON_IsString(r) && strlen(r->valuestring) < sizeof C.rt) snprintf(C.rt, sizeof C.rt, "%s", r->valuestring);
        C.exp = (int64_t)time(NULL) + (cJSON_IsNumber(e) ? (int64_t)e->valuedouble : 3600);
        unlock();
        tokens_save();
    }
    cJSON_Delete(j);
    return ok;
}

static bool refresh_token(void)
{
    char url[160], client[64];
    cfg("claude_token", DEF_TOKEN, url, sizeof url);
    cfg("claude_client", DEF_CLIENT, client, sizeof client);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "grant_type", "refresh_token");
    cJSON_AddStringToObject(b, "refresh_token", C.rt);
    cJSON_AddStringToObject(b, "client_id", client);
    char *js = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    char *resp;
    int code = http("POST", url, NULL, js, &resp);
    free(js);
    bool ok = code == 200 && tokens_take(resp);
    if (!ok) {
        char err[128];
        if (code < 0) { snprintf(err, sizeof err, _("Renovando la sesión: %s"), net_reason(code)); set_state(AOS_CLAUDE_ERROR, err); }
        else if (code == 400 || code == 401 || code == 403) {
            body_error(resp, code, err, sizeof err);
            char msg[160];
            snprintf(msg, sizeof msg, _("La sesión venció (%.100s). Volvé a iniciarla desde el portal."), err);
            set_state(AOS_CLAUDE_EXPIRED, msg);
        } else { body_error(resp, code, err, sizeof err); set_state(AOS_CLAUDE_ERROR, err); }
    }
    free(resp);
    return ok;
}

/* ---- the login ---- */

bool aos_claude_login_url(char *out, size_t n)
{
    aos_claude_start();
    uint8_t v[32], st[24], dig[32];
    random_bytes(v, sizeof v);
    random_bytes(st, sizeof st);
    lock();
    b64url(v, sizeof v, C.verifier, sizeof C.verifier);
    b64url(st, sizeof st, C.state_nonce, sizeof C.state_nonce);
    mbedtls_sha256((const unsigned char *)C.verifier, strlen(C.verifier), dig, 0);
    char challenge[64], state[48];
    b64url(dig, sizeof dig, challenge, sizeof challenge);
    snprintf(state, sizeof state, "%s", C.state_nonce);
    unlock();
    char auth[160], redir[160], client[64], er[240], es[80];
    cfg("claude_auth", DEF_AUTH, auth, sizeof auth);
    cfg("claude_redir", DEF_REDIR, redir, sizeof redir);
    cfg("claude_client", DEF_CLIENT, client, sizeof client);
    urlenc(redir, er, sizeof er);
    urlenc(SCOPES, es, sizeof es);
    int k = snprintf(out, n, "%s?code=true&client_id=%s&response_type=code&redirect_uri=%s&scope=%s"
                     "&code_challenge=%s&code_challenge_method=S256&state=%s", auth, client, er, es, challenge, state);
    set_state(AOS_CLAUDE_WAITING_CODE, NULL);
    return k > 0 && (size_t)k < n;
}

bool aos_claude_login_code(const char *pasted)
{
    aos_claude_start();
    if (!pasted) return false;
    while (*pasted == ' ' || *pasted == '\n' || *pasted == '\t') pasted++;
    size_t l = strlen(pasted);
    while (l && (pasted[l - 1] == ' ' || pasted[l - 1] == '\n' || pasted[l - 1] == '\r')) l--;
    if (l < 8 || l >= sizeof C.pending_code) return false;
    lock();
    bool started = C.verifier[0] != 0;
    if (started) {
        memcpy(C.pending_code, pasted, l);
        C.pending_code[l] = 0;
        C.have_pending = true;
        C.kick = true;
    }
    unlock();
    if (!started) { set_state(AOS_CLAUDE_SIGNED_OUT, _("Primero tocá «Iniciar sesión»: el código sirve sólo para ese pedido.")); return false; }
    set_state(AOS_CLAUDE_EXCHANGING, NULL);
    return true;
}

static void exchange(void)
{
    lock();
    char code[256], state[64] = "", verifier[64];
    snprintf(code, sizeof code, "%s", C.pending_code);
    snprintf(verifier, sizeof verifier, "%s", C.verifier);
    C.have_pending = false;
    unlock();
    /* Claude's page shows "code#state" */
    char *h = strchr(code, '#');
    if (h) { *h = 0; snprintf(state, sizeof state, "%s", h + 1); }
    if (state[0] && strcmp(state, C.state_nonce)) {
        set_state(AOS_CLAUDE_WAITING_CODE, _("Ese código es de otro pedido de inicio de sesión: empezá de nuevo."));
        return;
    }
    char url[160], redir[160], client[64];
    cfg("claude_token", DEF_TOKEN, url, sizeof url);
    cfg("claude_redir", DEF_REDIR, redir, sizeof redir);
    cfg("claude_client", DEF_CLIENT, client, sizeof client);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "grant_type", "authorization_code");
    cJSON_AddStringToObject(b, "code", code);
    cJSON_AddStringToObject(b, "state", state[0] ? state : C.state_nonce);
    cJSON_AddStringToObject(b, "client_id", client);
    cJSON_AddStringToObject(b, "redirect_uri", redir);
    cJSON_AddStringToObject(b, "code_verifier", verifier);
    char *js = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    char *resp;
    int rc = http("POST", url, NULL, js, &resp);
    free(js);
    if (rc == 200 && tokens_take(resp)) {
        lock();
        C.verifier[0] = 0;          /* used */
        C.kick = true;
        unlock();
        set_state(AOS_CLAUDE_OK, NULL);
        aos_hal_log("claude", "signed in");
    } else {
        char err[128];
        if (rc < 0) snprintf(err, sizeof err, "%s", net_reason(rc));
        else body_error(resp, rc, err, sizeof err);
        char msg[160];
        snprintf(msg, sizeof msg, _("No se pudo iniciar la sesión: %.130s"), err);
        set_state(AOS_CLAUDE_WAITING_CODE, msg);
    }
    free(resp);
}

void aos_claude_logout(void)
{
    aos_claude_start();
    lock();
    C.at[0] = C.rt[0] = 0;
    C.exp = 0;
    C.verifier[0] = 0;
    memset(&C.st.five_hour, 0, sizeof C.st.five_hour);
    memset(&C.st.seven_day, 0, sizeof C.st.seven_day);
    memset(&C.st.seven_day_opus, 0, sizeof C.st.seven_day_opus);
    memset(&C.st.seven_day_sonnet, 0, sizeof C.st.seven_day_sonnet);
    C.st.fetched = 0;
    unlock();
    aos_hal_pref_erase("claude_at");
    aos_hal_pref_erase("claude_rt");
    aos_hal_pref_erase("claude_exp");
    set_state(AOS_CLAUDE_SIGNED_OUT, NULL);
}

/* ---- the reading ---- */

/* "2026-09-28T21:00:00.123456+00:00", or epoch seconds */
static time_t parse_time(const cJSON *v)
{
    if (cJSON_IsNumber(v)) return (time_t)v->valuedouble;
    if (!cJSON_IsString(v)) return 0;
    int Y, M, D, h, m, s;
    const char *p = v->valuestring;
    if (sscanf(p, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return 0;
    int y = Y - (M <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long t = ((long long)era * 146097 + (long long)doe - 719468) * 86400 + h * 3600 + m * 60 + s;
    const char *z = p + 19;
    while (*z == '.' || (*z >= '0' && *z <= '9')) z++;
    if ((*z == '+' || *z == '-') && strlen(z) >= 6) {
        long off = atoi(z + 1) * 3600L + atoi(z + 4) * 60L;
        t -= *z == '+' ? off : -off;
    }
    return (time_t)t;
}

static void window(const cJSON *j, const char *key, aos_claude_window_t *w)
{
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(j, key);
    memset(w, 0, sizeof *w);
    if (!cJSON_IsObject(o)) return;
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(o, "utilization");
    if (!cJSON_IsNumber(u)) return;
    w->valid = true;
    w->pct = (float)u->valuedouble;
    w->resets = parse_time(cJSON_GetObjectItemCaseSensitive(o, "resets_at"));
}

static void hist_add(float p5, float p7)
{
    time_t now = time(NULL);
    if (!C.hist || (uint32_t)now - C.hist_last < HIST_STEP_S - 5) return;
    C.hist_last = (uint32_t)now;
    hist_t h = { (uint32_t)now, (uint8_t)lroundf(fminf(100, fmaxf(0, p5)) * 2), (uint8_t)lroundf(fminf(100, fmaxf(0, p7)) * 2) };
    lock();
    C.hist[C.hpos] = h;
    C.hpos = (C.hpos + 1) % AOS_CLAUDE_HIST_MAX;
    if (C.hn < AOS_CLAUDE_HIST_MAX) C.hn++;
    unlock();
    /* the card: appended, and rewritten from the ring when it gets long */
    char path[160];
    snprintf(path, sizeof path, "%.140s/claude_hist.bin", aos_hal_path_data());
    struct stat sb;
    bool rewrite = !stat(path, &sb) && sb.st_size > (off_t)(2 * AOS_CLAUDE_HIST_MAX * sizeof(hist_t));
    FILE *f = fopen(path, rewrite ? "wb" : "ab");
    if (!f) return;
    if (rewrite) {
        lock();
        int start = (C.hpos - C.hn + AOS_CLAUDE_HIST_MAX) % AOS_CLAUDE_HIST_MAX;
        for (int i = 0; i < C.hn; i++) fwrite(&C.hist[(start + i) % AOS_CLAUDE_HIST_MAX], sizeof(hist_t), 1, f);
        unlock();
    } else {
        fwrite(&h, sizeof h, 1, f);
    }
    fclose(f);
}

static void hist_load(void)
{
    C.hist = malloc(AOS_CLAUDE_HIST_MAX * sizeof(hist_t));
    if (!C.hist) return;
    char path[160];
    snprintf(path, sizeof path, "%.140s/claude_hist.bin", aos_hal_path_data());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    hist_t h;
    time_t week_ago = time(NULL) - 7 * 86400;
    while (fread(&h, sizeof h, 1, f) == 1) {
        if ((time_t)h.t < week_ago) continue;
        C.hist[C.hpos] = h;
        C.hpos = (C.hpos + 1) % AOS_CLAUDE_HIST_MAX;
        if (C.hn < AOS_CLAUDE_HIST_MAX) C.hn++;
        C.hist_last = h.t;
    }
    fclose(f);
}

int aos_claude_history(int which, float *pct, time_t *when, int max)
{
    aos_claude_start();
    lock();
    int n = C.hn < max ? C.hn : max;
    int start = (C.hpos - n + AOS_CLAUDE_HIST_MAX) % AOS_CLAUDE_HIST_MAX;
    for (int i = 0; i < n && C.hist; i++) {
        const hist_t *h = &C.hist[(start + i) % AOS_CLAUDE_HIST_MAX];
        pct[i] = (which ? h->p7 : h->p5) / 2.0f;
        when[i] = (time_t)h->t;
    }
    unlock();
    return C.hist ? n : 0;
}

static void fetch(void)
{
    if ((int64_t)time(NULL) > C.exp - 120 && C.rt[0] && !refresh_token()) return;
    char base[160], url[200], hdr[640];
    cfg("claude_api", DEF_API, base, sizeof base);
    snprintf(url, sizeof url, "%s/api/oauth/usage", base);
    lock();
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\nanthropic-beta: %s\r\n", C.at, BETA);
    unlock();
    char *resp;
    int code = http("GET", url, hdr, NULL, &resp);
    if (code == 401 && C.rt[0]) {           /* expired early: once more with a fresh one */
        free(resp);
        if (!refresh_token()) return;
        lock();
        snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\nanthropic-beta: %s\r\n", C.at, BETA);
        unlock();
        code = http("GET", url, hdr, NULL, &resp);
    }
    if (code != 200) {
        char err[128];
        if (code < 0) snprintf(err, sizeof err, "%s", net_reason(code));
        else body_error(resp, code, err, sizeof err);
        set_state(code == 401 || code == 403 ? AOS_CLAUDE_EXPIRED : AOS_CLAUDE_ERROR, err);
        free(resp);
        return;
    }
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (!j) { set_state(AOS_CLAUDE_ERROR, _("La respuesta no es JSON: ¿cambió el formato?")); return; }
    aos_claude_status_t n = { 0 };
    window(j, "five_hour", &n.five_hour);
    window(j, "seven_day", &n.seven_day);
    window(j, "seven_day_opus", &n.seven_day_opus);
    window(j, "seven_day_sonnet", &n.seven_day_sonnet);
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(j, "extra_usage");
    n.extra_pct = -1;
    if (cJSON_IsObject(x)) {
        n.extra_enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "is_enabled"));
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(x, "utilization");
        if (cJSON_IsNumber(u)) n.extra_pct = (float)u->valuedouble;
    }
    cJSON_Delete(j);
    if (!n.five_hour.valid && !n.seven_day.valid) {
        set_state(AOS_CLAUDE_ERROR, _("La respuesta no trae five_hour ni seven_day: ¿cambió el formato?"));
        return;
    }
    lock();
    C.st.five_hour = n.five_hour;
    C.st.seven_day = n.seven_day;
    C.st.seven_day_opus = n.seven_day_opus;
    C.st.seven_day_sonnet = n.seven_day_sonnet;
    C.st.extra_enabled = n.extra_enabled;
    C.st.extra_pct = n.extra_pct;
    C.st.fetched = time(NULL);
    C.st.state = AOS_CLAUDE_OK;
    C.st.error[0] = 0;
    C.st.seq++;
    unlock();
    hist_add(n.five_hour.valid ? n.five_hour.pct : 0, n.seven_day.valid ? n.seven_day.pct : 0);
}

static void worker(void *arg)
{
    (void)arg;
    hist_load();
    uint32_t last = 0;
    int backoff = 0;
    for (;;) {
        lock();
        bool pending = C.have_pending, signed_in = C.at[0] != 0, kick = C.kick;
        bool looking = C.look_ms && (uint32_t)aos_hal_uptime_ms() - C.look_ms < 10000;
        aos_claude_state_t s = C.st.state;
        unlock();
        if (pending) { exchange(); continue; }
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        uint32_t every = (looking ? POLL_LOOK_S : POLL_S) * 1000u + (uint32_t)backoff * 60000u;
        bool due = !last || now - last >= every || kick;
        if (signed_in && s != AOS_CLAUDE_EXPIRED && due && aos_hal_net_state() == AOS_NET_CONNECTED && aos_hal_time_is_valid()) {
            lock(); C.kick = false; unlock();
            last = now;
            fetch();
            lock();
            backoff = C.st.state == AOS_CLAUDE_OK ? 0 : backoff < 10 ? backoff + 1 : 10;
            unlock();
        }
        aos_hal_sleep_ms(500);
    }
}

void aos_claude_start(void)
{
    if (C.started) return;
    C.started = true;
    C.mx = aos_hal_mutex_create();
    tokens_load();
    C.st.extra_pct = -1;
    C.st.state = C.at[0] ? AOS_CLAUDE_OK : AOS_CLAUDE_SIGNED_OUT;
    if (!aos_hal_thread_start("claude", worker, NULL, 8192, 3)) set_state(AOS_CLAUDE_ERROR, "no thread");
}

void aos_claude_status(aos_claude_status_t *out)
{
    aos_claude_start();
    lock();
    *out = C.st;
    unlock();
}

void aos_claude_refresh_now(void)
{
    aos_claude_start();
    lock();
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!C.look_ms || now - C.look_ms > 10000) C.kick = C.st.fetched == 0 || time(NULL) - C.st.fetched > 60;
    C.look_ms = now | 1;
    unlock();
}
