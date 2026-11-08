/*
 * P4OS - SSH client: what it keeps.
 *
 *   <data>/ssh/hosts.txt       the computers, a line each:
 *                              name TAB host TAB port TAB user
 *   <data>/ssh/known_hosts     the keys they showed the first time, a line
 *                              each: host:port TAB type TAB base64 (the key's
 *                              wire form)
 *   <data>/ssh/id_ed25519.pub  the board's public key, to copy to a
 *                              computer's ~/.ssh/authorized_keys
 *   preference "sshc_seed"     the board's private key: an Ed25519 seed in
 *                              base64, in the board's flash and not on the
 *                              card, which comes out
 *
 * No password is kept anywhere: the sheet asks for it every time.
 */
#include "sc.h"

#include "aos_hal.h"
#include "monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define K_SEED "sshc_seed"

/* ---- base64 ---- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_enc(const uint8_t *in, size_t n, char *out, size_t cap, bool pad)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        char q[4] = { B64[v >> 18 & 63], B64[v >> 12 & 63], i + 1 < n ? B64[v >> 6 & 63] : '=',
                      i + 2 < n ? B64[v & 63] : '=' };
        for (int k = 0; k < 4; k++) {
            if (q[k] == '=' && !pad) continue;
            if (o + 1 < cap) out[o++] = q[k];
        }
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return o;
}

static int b64_val(char c)
{
    const char *p = c ? strchr(B64, c) : NULL;
    return p ? (int)(p - B64) : -1;
}

/* NUL-terminated base64 into bytes; the count, -1 if it is not base64 */
static int b64_dec(const char *in, uint8_t *out, size_t cap)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64_val(*in);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

void sc_fingerprint(const uint8_t *blob, size_t n, char *out, size_t on)
{
    uint8_t h[32];
    aos_hal_sha256(blob, n, h);
    char b[48];
    b64_enc(h, 32, b, sizeof b, false);
    snprintf(out, on, "SHA256:%s", b);
}

/* ---- files ---- */

static void path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/ssh", aos_hal_path_data());
    mkdir(out, 0777);
    size_t l = strlen(out);
    snprintf(out + l, n - l, "/%s", name);
}

void sc_hosts_load(sc_hosts_t *hs)
{
    memset(hs, 0, sizeof *hs);
    char path[160];
    path_of("hosts.txt", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[320];
    while (hs->count < SC_MAX_HOSTS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *field[4] = { line, NULL, NULL, NULL };
        for (int i = 1; i < 4 && field[i - 1]; i++) {
            char *t = strchr(field[i - 1], '\t');
            if (t) {
                *t = 0;
                field[i] = t + 1;
            }
        }
        if (!field[1] || !field[1][0]) continue;
        sc_host_t *h = &hs->h[hs->count++];
        sc_copy(h->name, sizeof h->name, field[0]);
        sc_copy(h->host, sizeof h->host, field[1]);
        h->port = field[2] ? atoi(field[2]) : 22;
        if (h->port <= 0 || h->port > 65535) h->port = 22;
        sc_copy(h->user, sizeof h->user, field[3] ? field[3] : "");
    }
    fclose(f);
}

bool sc_hosts_save(const sc_hosts_t *hs)
{
    char path[160], tmp[168];
    path_of("hosts.txt", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs("# P4OS SSH: name<TAB>host<TAB>port<TAB>user\n", f);
    for (int i = 0; i < hs->count; i++) {
        const sc_host_t *h = &hs->h[i];
        char line[320];
        snprintf(line, sizeof line, "%s\t%s\t%d\t%s\n", h->name, h->host, h->port, h->user);
        fputs(line, f);
    }
    bool ok = fclose(f) == 0;
    remove(path);
    return ok && rename(tmp, path) == 0;
}

/* ---- known_hosts ---- */

static void host_key(const char *host, int port, char *out, size_t n) { snprintf(out, n, "%s:%d", host, port); }

/* calls each line with host:port, type, base64; stops when it returns false */
static void known_each(bool (*fn)(void *ctx, char *who, char *type, char *b64, const char *line), void *ctx)
{
    char path[160];
    path_of("known_hosts", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1600], copy[1600];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        memcpy(copy, line, sizeof copy);
        char *who = line, *type = strchr(who, '\t'), *b64 = type ? strchr(type + 1, '\t') : NULL;
        if (!type || !b64) continue;
        *type++ = 0;
        *b64++ = 0;
        if (!fn(ctx, who, type, b64, copy)) break;
    }
    fclose(f);
}

typedef struct {
    const char *who;
    const uint8_t *blob;
    size_t n;
    int result;
} check_t;

static bool check_one(void *ctx, char *who, char *type, char *b64, const char *line)
{
    (void)type;
    (void)line;
    check_t *c = ctx;
    if (strcmp(who, c->who)) return true;
    uint8_t *k = malloc(1200);
    int n = k ? b64_dec(b64, k, 1200) : -1;
    bool same = n == (int)c->n && !memcmp(k, c->blob, c->n);
    free(k);
    if (same) {
        c->result = 1;
        return false;
    }
    c->result = -1;                 /* another key for it; one more line may still match */
    return true;
}

int sc_known_check(const char *host, int port, const uint8_t *blob, size_t n)
{
    char who[110];
    host_key(host, port, who, sizeof who);
    check_t c = { who, blob, n, 0 };
    known_each(check_one, &c);
    return c.result;
}

typedef struct {
    const char *who;
    FILE *out;
} keep_t;

static bool keep_other(void *ctx, char *who, char *type, char *b64, const char *line)
{
    (void)type;
    (void)b64;
    keep_t *k = ctx;
    if (strcmp(who, k->who)) {
        fputs(line, k->out);
        fputc('\n', k->out);
    }
    return true;
}

void sc_known_forget(const char *host, int port)
{
    char who[110], path[160], tmp[168];
    host_key(host, port, who, sizeof who);
    path_of("known_hosts", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    keep_t k = { who, f };
    known_each(keep_other, &k);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

bool sc_known_add(const char *host, int port, const uint8_t *blob, size_t n)
{
    sc_known_forget(host, port);
    char who[110], path[160];
    host_key(host, port, who, sizeof who);
    path_of("known_hosts", path, sizeof path);
    char *b64 = malloc(1700);
    if (!b64) return false;
    b64_enc(blob, n, b64, 1700, true);
    char type[32] = "?";
    if (n > 4) {
        uint32_t tl = (uint32_t)blob[0] << 24 | (uint32_t)blob[1] << 16 | (uint32_t)blob[2] << 8 | blob[3];
        if (tl < sizeof type && tl + 4 <= n) {
            memcpy(type, blob + 4, tl);
            type[tl] = 0;
        }
    }
    FILE *f = fopen(path, "a");
    bool ok = f != NULL;
    if (f) {
        fputs(who, f);
        fputc('\t', f);
        fputs(type, f);
        fputc('\t', f);
        fputs(b64, f);
        fputc('\n', f);
        ok = fclose(f) == 0;
    }
    free(b64);
    return ok;
}

/* ---- the board's key ---- */

bool sc_key(uint8_t sk[64], uint8_t pub[32])
{
    char b64[64] = "";
    uint8_t seed[32];
    if (!aos_hal_pref_get_str(K_SEED, b64, sizeof b64) || b64_dec(b64, seed, sizeof seed) != 32) {
        aos_hal_random(seed, sizeof seed);
        b64_enc(seed, 32, b64, sizeof b64, true);
        if (!aos_hal_pref_set_str(K_SEED, b64)) {
            crypto_wipe(seed, sizeof seed);
            return false;
        }
        aos_hal_log(SC_TAG, "made the board's SSH client key");
    }
    crypto_wipe(b64, sizeof b64);
    crypto_ed25519_key_pair(sk, pub, seed);     /* wipes seed */
    return true;
}

bool sc_key_line(char *out, size_t n)
{
    uint8_t sk[64], pub[32];
    if (!sc_key(sk, pub)) return false;
    crypto_wipe(sk, sizeof sk);
    uint8_t blob[4 + 11 + 4 + 32] = { 0, 0, 0, 11, 's', 's', 'h', '-', 'e', 'd', '2', '5', '5', '1', '9', 0, 0, 0, 32 };
    memcpy(blob + 19, pub, 32);
    char b64[80];
    b64_enc(blob, sizeof blob, b64, sizeof b64, true);
    snprintf(out, n, "ssh-ed25519 %s p4os@%s", b64, aos_hal_device_name());
    char path[160];
    path_of("id_ed25519.pub", path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
    }
    return true;
}
