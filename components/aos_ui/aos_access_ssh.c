/*
 * P4OS - who may use SSH: the switch, the board's host key, the keys that may
 * log in and the question a new key puts to the board. The contract is in
 * aos_access.h; the server is components/aos_ssh.
 *
 * Preferences:
 *   ssh_on        1: the server listens on port 22
 *   ssh_hk        the host key's Ed25519 seed, base64 (secret)
 *   ssh_k0..k7    the keys that may log in, an OpenSSH line each
 *
 * The question is a single slot shared by the connection that asks (it
 * waits) and the UI's tick (it shows and answers). A spinlock and not a
 * mutex guards it: both sides may be the first to touch it, from different
 * threads, and the work under it is a copy of a few bytes.
 */
#include "aos_access.h"
#include "aos_hal.h"

#include "monocypher-ed25519.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
static void rnd(void *buf, size_t n) { esp_fill_random(buf, n); }
#else
static void rnd(void *buf, size_t n) { arc4random_buf(buf, n); }
#endif

#define K_ON   "ssh_on"
#define K_HOST "ssh_hk"
#define LINE_MAX_LEN 1024

static atomic_uint s_gen;

unsigned aos_access_ssh_generation(void) { return atomic_load(&s_gen); }
void aos_access_ssh_kick(void) { atomic_fetch_add(&s_gen, 1); }

/* the switch, read once: the listener and every connection ask for it each
 * second, and on the board each preference read is a flash read with the
 * cache off (docs/MEMORY.md) */
static atomic_int s_on = -1;

bool aos_access_ssh(void)
{
    int on = atomic_load(&s_on);
    if (on < 0) {
        int32_t v = 0;
        aos_hal_pref_get_i32(K_ON, &v);
        on = v != 0;
        atomic_store(&s_on, on);
    }
    return on != 0;
}

void aos_access_set_ssh(bool on)
{
    aos_hal_pref_set_i32(K_ON, on);
    atomic_store(&s_on, on);
    if (on) {
        /* the host key now, so Settings shows its fingerprint at once */
        unsigned char seed[32], pub[32];
        aos_access_ssh_host_key(seed, pub);
        memset(seed, 0, sizeof seed);
    } else aos_access_ssh_kick();
}

/* ---- the host key ---- */

static bool host_seed(unsigned char seed[32])
{
    char b64[64];
    size_t len = 0;
    return aos_hal_pref_get_str(K_HOST, b64, sizeof b64) && b64[0] &&
           !mbedtls_base64_decode(seed, 32, &len, (const unsigned char *)b64, strlen(b64)) && len == 32;
}

bool aos_access_ssh_host_key(unsigned char seed[32], unsigned char pub[32])
{
    if (!host_seed(seed)) {
        rnd(seed, 32);
        char b64[64];
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &olen, seed, 32) || !aos_hal_pref_set_str(K_HOST, b64)) {
            memset(seed, 0, 32);
            return false;
        }
        memset(b64, 0, sizeof b64);
        aos_hal_log("ssh", "made the board's host key");
    }
    /* Monocypher's key_pair wipes the seed it is given */
    unsigned char s[32], sk[64];
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(sk, pub, s);
    memset(sk, 0, sizeof sk);
    return true;
}

/* SHA256 of a key's wire form, the way ssh prints it: base64, no padding */
static void fp_of(const unsigned char *blob, size_t len, char *out, size_t n)
{
    unsigned char h[32], b64[48];
    size_t olen = 0;
    mbedtls_sha256(blob, len, h, 0);
    mbedtls_base64_encode(b64, sizeof b64, &olen, h, 32);
    while (olen && b64[olen - 1] == '=') olen--;
    snprintf(out, n, "SHA256:%.*s", (int)olen, (const char *)b64);
}

void aos_access_ssh_host_fingerprint(char *out, size_t n)
{
    out[0] = 0;
    unsigned char seed[32];
    if (!host_seed(seed)) return;
    unsigned char sk[64], pub[32];
    crypto_ed25519_key_pair(sk, pub, seed);
    memset(sk, 0, sizeof sk);
    /* string "ssh-ed25519", string key */
    unsigned char blob[4 + 11 + 4 + 32] = { 0, 0, 0, 11, 's', 's', 'h', '-', 'e', 'd', '2', '5', '5', '1', '9', 0, 0, 0, 32 };
    memcpy(blob + 19, pub, 32);
    fp_of(blob, sizeof blob, out, n);
}

void aos_access_ssh_host_forget(void)
{
    aos_hal_pref_erase(K_HOST);
    aos_access_ssh_kick();
    unsigned char seed[32], pub[32];
    aos_access_ssh_host_key(seed, pub);     /* the new one at once, for Settings */
    memset(seed, 0, sizeof seed);
}

/* ---- the keys that may log in ---- */

static void key_pref(int i, char k[8]) { snprintf(k, 8, "ssh_k%d", i); }

bool aos_access_ssh_key_get(int i, char *line, size_t n)
{
    if (i < 0 || i >= AOS_ACCESS_SSH_KEYS) return false;
    char k[8];
    key_pref(i, k);
    return aos_hal_pref_get_str(k, line, n) && line[0];
}

/* "type b64 comment": the type and the base64 as pointers into the line,
 * cut in place */
static bool split(char *line, char **type, char **b64, char **comment)
{
    char *s = line;
    while (*s == ' ') s++;
    *type = s;
    s = strchr(s, ' ');
    if (!s) return false;
    *s++ = 0;
    while (*s == ' ') s++;
    *b64 = s;
    s = strpbrk(s, " \r\n");
    if (s) {
        *s++ = 0;
        while (*s == ' ') s++;
        char *e = strpbrk(s, "\r\n");
        if (e) *e = 0;
    }
    *comment = s ? s : (char *)"";
    return **type && **b64;
}

int aos_access_ssh_key_find(const char *type, const char *b64)
{
    char *line = malloc(LINE_MAX_LEN);
    int found = -1;
    for (int i = 0; line && i < AOS_ACCESS_SSH_KEYS && found < 0; i++) {
        char *t, *b, *c;
        if (aos_access_ssh_key_get(i, line, LINE_MAX_LEN) && split(line, &t, &b, &c) && !strcmp(t, type) && !strcmp(b, b64))
            found = i;
    }
    free(line);
    return found;
}

bool aos_access_ssh_key_add(const char *line)
{
    if (!line || strlen(line) >= LINE_MAX_LEN) return false;
    char *probe = malloc(LINE_MAX_LEN);
    bool ok = false;
    for (int i = 0; probe && i < AOS_ACCESS_SSH_KEYS && !ok; i++) {
        if (aos_access_ssh_key_get(i, probe, LINE_MAX_LEN)) continue;
        char k[8];
        key_pref(i, k);
        ok = aos_hal_pref_set_str(k, line);
    }
    free(probe);
    return ok;
}

void aos_access_ssh_key_remove(int i)
{
    if (i < 0 || i >= AOS_ACCESS_SSH_KEYS) return;
    char k[8];
    key_pref(i, k);
    aos_hal_pref_erase(k);
    aos_access_ssh_kick();
}

void aos_access_ssh_key_fingerprint(const char *line, char *out, size_t n)
{
    out[0] = 0;
    char *copy = strdup(line ? line : "");
    unsigned char *blob = malloc(LINE_MAX_LEN);
    char *t, *b, *c;
    size_t len = 0;
    if (copy && blob && split(copy, &t, &b, &c) &&
        !mbedtls_base64_decode(blob, LINE_MAX_LEN, &len, (const unsigned char *)b, strlen(b)))
        fp_of(blob, len, out, n);
    free(copy);
    free(blob);
}

/* ---- the question on the board ---- */

enum { ASK_NONE, ASK_OPEN, ASK_YES, ASK_NO };
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static int s_state;
static aos_access_ssh_ask_t s_q;

static void lock(void)
{
    while (atomic_flag_test_and_set(&s_lock)) aos_hal_sleep_ms(1);
}
static void unlock(void) { atomic_flag_clear(&s_lock); }

int aos_access_ssh_ask(const aos_access_ssh_ask_t *q, const char *line, int timeout_s)
{
    lock();
    if (s_state != ASK_NONE) {
        unlock();
        return -1;
    }
    s_q = *q;
    s_state = ASK_OPEN;
    unlock();
    aos_hal_log("ssh", "asking the board about %s key %s from %s", q->type, q->fp, q->from);

    int answer = 0;
    for (int t = 0; t < timeout_s * 10; t++) {
        lock();
        int st = s_state;
        unlock();
        if (st == ASK_YES || st == ASK_NO) {
            answer = st == ASK_YES;
            break;
        }
        aos_hal_sleep_ms(100);
    }
    lock();
    s_state = ASK_NONE;             /* answered, or no one did: the UI closes it */
    unlock();
    if (answer && !aos_access_ssh_key_add(line)) {
        aos_hal_log("ssh", "the key list is full (%d)", AOS_ACCESS_SSH_KEYS);
        answer = 0;
    }
    aos_hal_log("ssh", "key %s %s", q->fp, answer ? "let in and kept" : "refused");
    return answer;
}

bool aos_access_ssh_asking(aos_access_ssh_ask_t *out)
{
    lock();
    bool open = s_state == ASK_OPEN;
    if (open && out) *out = s_q;
    unlock();
    return open;
}

void aos_access_ssh_answer(bool yes)
{
    lock();
    if (s_state == ASK_OPEN) s_state = yes ? ASK_YES : ASK_NO;
    unlock();
}
