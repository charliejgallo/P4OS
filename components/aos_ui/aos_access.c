/*
 * P4OS - who may use the web portal. The contract is in aos_access.h.
 *
 * Preferences:
 *   pt_trust   the home networks' names, separated by \x1f
 *   pt_ask     1: the password at home and on the AP too
 *   pt_away    1: other networks open with the password (0: closed)
 *   pt_pass    "salt:iterations:hash", PBKDF2-HMAC-SHA256 in hex
 *   pt_sess    the sessions: up to SESS_MAX hashes of their ids, in hex
 *   pt_token   the scripts' token, in clear: Settings shows it
 *   pt_https   1: the portal also answers HTTPS, on port 443
 *   pt_cac, pt_cak   the board's HTTPS authority and its key, and
 *   pt_crt, pt_key   the portal's certificate and its key: DER in base64,
 *              one line each (the simulator keeps preferences a line each)
 *   pt_cfor    what the portal's certificate was made for
 *
 * A session id is 16 random bytes; what is kept is the start of its SHA-256,
 * so the preferences hold nothing a browser could present. Sessions do not
 * expire: there are eight at most, the oldest goes when a ninth comes, and
 * Settings closes all of them (as a new password does).
 */
#include "aos_access.h"
#include "aos_hal.h"

#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/version.h"
#include "mbedtls/base64.h"
#include "mbedtls/oid.h"
#include "mbedtls/asn1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
static void rnd(void *buf, size_t n) { esp_fill_random(buf, n); }
#else
static void rnd(void *buf, size_t n) { arc4random_buf(buf, n); }
#endif

#define K_TRUST "pt_trust"
#define K_ASK   "pt_ask"
#define K_AWAY  "pt_away"
#define K_PASS  "pt_pass"
#define K_SESS  "pt_sess"
#define K_TOKEN "pt_token"
#define K_HTTPS "pt_https"
#define K_CRT   "pt_crt"
#define K_KEY   "pt_key"
#define SEP     '\x1f'

#define PBKDF2_ITER  10000          /* ~0.2 s on the board, with its SHA engine */
#define SESS_MAX     8
#define SESS_HASH    16             /* bytes of SHA-256 kept per session */

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static bool unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* compared whole, not stopping at the first difference */
static bool same(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    unsigned d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned)(x[i] ^ y[i]);
    return d == 0;
}

static int32_t pref_i(const char *k, int32_t def)
{
    int32_t v = def;
    aos_hal_pref_get_i32(k, &v);
    return v;
}

/* ---- the rules ---- */

bool aos_access_ask_always(void) { return pref_i(K_ASK, 0) != 0; }
void aos_access_set_ask_always(bool on) { aos_hal_pref_set_i32(K_ASK, on); }
bool aos_access_away_login(void) { return pref_i(K_AWAY, 0) != 0; }
void aos_access_set_away_login(bool on) { aos_hal_pref_set_i32(K_AWAY, on); }

aos_access_t aos_access_need(aos_zone_t zone)
{
    bool pw = aos_access_has_password();
    switch (zone) {
    case AOS_ZONE_CABLE:
        return AOS_ACCESS_OPEN;
    case AOS_ZONE_AP:
    case AOS_ZONE_HOME:
        return pw && aos_access_ask_always() ? AOS_ACCESS_LOGIN : AOS_ACCESS_OPEN;
    default:
        return pw && aos_access_away_login() ? AOS_ACCESS_LOGIN : AOS_ACCESS_CLOSED;
    }
}

/* ---- home networks ---- */

int aos_access_trusted(char names[][33], int max)
{
    char all[AOS_ACCESS_TRUST_MAX * 34 + 1] = "";
    if (!aos_hal_pref_get_str(K_TRUST, all, sizeof all)) return 0;
    int n = 0;
    for (char *p = all; *p && n < max; ) {
        char *e = strchr(p, SEP);
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && l < 33) {
            memcpy(names[n], p, l);
            names[n][l] = 0;
            n++;
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

bool aos_access_is_trusted(const char *ssid)
{
    char names[AOS_ACCESS_TRUST_MAX][33];
    int n = aos_access_trusted(names, AOS_ACCESS_TRUST_MAX);
    for (int i = 0; ssid && *ssid && i < n; i++)
        if (!strcmp(names[i], ssid)) return true;
    return false;
}

void aos_access_trust(const char *ssid, bool on)
{
    if (!ssid || !*ssid) return;
    char names[AOS_ACCESS_TRUST_MAX + 1][33];
    int n = aos_access_trusted(names, AOS_ACCESS_TRUST_MAX), k = 0;
    char all[AOS_ACCESS_TRUST_MAX * 34 + 1] = "";
    size_t len = 0;
    if (on && !aos_access_is_trusted(ssid)) {
        if (n == AOS_ACCESS_TRUST_MAX) {            /* the oldest goes */
            memmove(names[0], names[1], (size_t)(n - 1) * sizeof names[0]);
            n--;
        }
        snprintf(names[n++], 33, "%s", ssid);
    }
    for (int i = 0; i < n; i++) {
        if (!on && !strcmp(names[i], ssid)) continue;
        len += (size_t)snprintf(all + len, sizeof all - len, "%s%s", k++ ? "\x1f" : "", names[i]);
    }
    aos_hal_pref_set_str(K_TRUST, all);     /* "" is a list too: no home network */
}

/* First time with these rules: the network the board is already on becomes
 * home, so an update does not close the portal on the user. Here and not
 * only in the tick, so that a request in the first second after connecting
 * does not find it closed. */
static void first_home(const char *ssid)
{
    char probe[8];
    if (ssid[0] && !aos_hal_pref_get_str(K_TRUST, probe, sizeof probe)) {
        aos_access_trust(ssid, true);
        aos_hal_log("access", "'%s' marked as a home network (the first one)", ssid);
    }
}

bool aos_access_here_trusted(void)
{
    if (aos_hal_net_state() != AOS_NET_CONNECTED) return false;
    first_home(aos_hal_net_ssid());
    return aos_access_is_trusted(aos_hal_net_ssid());
}

/* ---- the password ---- */

static void pbkdf2(const char *pw, const uint8_t salt[16], int iter, uint8_t out[32])
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw),
                                  salt, 16, (unsigned)iter, 32, out);
}

bool aos_access_has_password(void)
{
    char v[128];
    return aos_hal_pref_get_str(K_PASS, v, sizeof v) && strlen(v) > 40;
}

bool aos_access_set_password(const char *pw)
{
    aos_access_sessions_clear();
    if (!pw || !*pw) return aos_hal_pref_erase(K_PASS) || true;
    uint8_t salt[16], h[32];
    rnd(salt, sizeof salt);
    pbkdf2(pw, salt, PBKDF2_ITER, h);
    char v[128], sh[33], hh[65];
    hex(salt, 16, sh);
    hex(h, 32, hh);
    snprintf(v, sizeof v, "%s:%d:%s", sh, PBKDF2_ITER, hh);
    return aos_hal_pref_set_str(K_PASS, v);
}

static int s_fails;
static uint32_t s_wait_until;

bool aos_access_check_password(const char *pw, int *wait_s)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (wait_s) *wait_s = 0;
    if (s_wait_until && (int32_t)(s_wait_until - now) > 0) {
        if (wait_s) *wait_s = (int)((s_wait_until - now + 999) / 1000);
        return false;
    }
    char v[128];
    uint8_t salt[16], want[32], got[32];
    int iter = 0;
    bool ok = pw && aos_hal_pref_get_str(K_PASS, v, sizeof v) && strlen(v) > 40 && v[32] == ':' &&
              unhex(v, salt, 16) && (iter = atoi(v + 33)) > 0;
    char *last = ok ? strrchr(v, ':') : NULL;
    ok = ok && last && strlen(last + 1) == 64 && unhex(last + 1, want, 32);
    if (ok) {
        pbkdf2(pw, salt, iter, got);
        ok = same(got, want, 32);
    }
    if (ok) {
        s_fails = 0;
        s_wait_until = 0;
        return true;
    }
    /* three free tries, then 2, 4, 8... seconds, up to five minutes */
    if (++s_fails >= 3) {
        int w = 1 << (s_fails - 2 > 8 ? 8 : s_fails - 2);
        if (w > 300) w = 300;
        s_wait_until = now + (uint32_t)w * 1000u;
        if (!s_wait_until) s_wait_until = 1;
        if (wait_s) *wait_s = w;
    }
    aos_hal_log("access", "wrong password (%d in a row)", s_fails);
    return false;
}

/* ---- sessions ---- */

static void sess_hash(const char *id, char out[2 * SESS_HASH + 1])
{
    uint8_t h[32];
    mbedtls_sha256((const unsigned char *)id, strlen(id), h, 0);
    hex(h, SESS_HASH, out);
}

static int sess_load(char list[SESS_MAX][2 * SESS_HASH + 1])
{
    char all[SESS_MAX * (2 * SESS_HASH + 1) + 1] = "";
    if (!aos_hal_pref_get_str(K_SESS, all, sizeof all)) return 0;
    int n = 0;
    for (char *p = all; n < SESS_MAX && strlen(p) >= 2 * SESS_HASH; p += 2 * SESS_HASH + 1) {
        memcpy(list[n], p, 2 * SESS_HASH);
        list[n++][2 * SESS_HASH] = 0;
        if (!p[2 * SESS_HASH]) break;
    }
    return n;
}

static void sess_save(char list[SESS_MAX][2 * SESS_HASH + 1], int n)
{
    char all[SESS_MAX * (2 * SESS_HASH + 1) + 1] = "";
    size_t len = 0;
    for (int i = 0; i < n; i++) len += (size_t)snprintf(all + len, sizeof all - len, "%s%s", i ? "," : "", list[i]);
    if (n) aos_hal_pref_set_str(K_SESS, all);
    else aos_hal_pref_erase(K_SESS);
}

bool aos_access_session_new(char out[AOS_ACCESS_SESSION_LEN + 1])
{
    uint8_t b[16];
    rnd(b, sizeof b);
    hex(b, 16, out);
    char list[SESS_MAX][2 * SESS_HASH + 1];
    int n = sess_load(list);
    if (n == SESS_MAX) {
        memmove(list[0], list[1], (size_t)(n - 1) * sizeof list[0]);
        n--;
    }
    sess_hash(out, list[n++]);
    sess_save(list, n);
    return true;
}

bool aos_access_session_ok(const char *id)
{
    if (!id || strlen(id) != AOS_ACCESS_SESSION_LEN) return false;
    char h[2 * SESS_HASH + 1], list[SESS_MAX][2 * SESS_HASH + 1];
    sess_hash(id, h);
    int n = sess_load(list);
    bool ok = false;
    for (int i = 0; i < n; i++) ok |= same(list[i], h, 2 * SESS_HASH);
    return ok;
}

void aos_access_session_end(const char *id)
{
    if (!id) return;
    char h[2 * SESS_HASH + 1], list[SESS_MAX][2 * SESS_HASH + 1];
    sess_hash(id, h);
    int n = sess_load(list), k = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i], h)) memcpy(list[k++], list[i], sizeof list[0]);
    sess_save(list, k);
}

void aos_access_sessions_clear(void) { aos_hal_pref_erase(K_SESS); }

int aos_access_session_count(void)
{
    char list[SESS_MAX][2 * SESS_HASH + 1];
    return sess_load(list);
}

/* ---- the token ---- */

static char s_token[33];

const char *aos_access_token(void)
{
    if (!s_token[0] && (!aos_hal_pref_get_str(K_TOKEN, s_token, sizeof s_token) || strlen(s_token) != 32)) {
        aos_access_token_new();
    }
    return s_token;
}

void aos_access_token_new(void)
{
    uint8_t b[16];
    rnd(b, sizeof b);
    hex(b, 16, s_token);
    aos_hal_pref_set_str(K_TOKEN, s_token);
}

bool aos_access_token_ok(const char *token)
{
    const char *t = aos_access_token();
    return token && strlen(token) == 32 && same(token, t, 32);
}


/* ---- HTTPS: the board's own authority and the portal's certificate ----
 *
 * Two certificates, as a real site has, so that trusting the board is done
 * once: an authority (a small CA) that stays for ten years, and the portal's
 * certificate, signed by it, that the board renews by itself. The authority
 * is what goes into a Mac's keychain or an iPhone's profiles.
 *
 * The authority can only vouch for .local names, the board's own name and
 * the private address ranges (a critical nameConstraints extension): its
 * key is on the board, in flash that is not encrypted, and someone who took
 * it from a board must not be able to impersonate any real site to the
 * devices that trust it.
 *
 * The portal's certificate follows what Apple asks of a server certificate
 * that a user trusts (and Chrome on a Mac uses Apple's verifier): the name
 * in subjectAltName, serverAuth in extendedKeyUsage, at most 825 days. It
 * is made again when it has 60 days left, or when the board's name or its
 * Wi-Fi address changed (the address goes in it too).
 */

#define K_CAC   "pt_cac"            /* the authority's certificate */
#define K_CAK   "pt_cak"            /* and its key */
#define K_CFOR  "pt_cfor"           /* what the portal's was made for: "name|ip|until" */
#define LEAF_DAYS 800

bool aos_access_https(void) { return pref_i(K_HTTPS, 0) != 0; }
void aos_access_set_https(bool on) { aos_hal_pref_set_i32(K_HTTPS, on); }

static int drbg_rng(void *ctx, unsigned char *out, size_t n)
{
    (void)ctx;
    rnd(out, n);
    return 0;
}

/* a preference in base64 back to bytes; NULL if there is none */
static unsigned char *pref_der(const char *k, size_t *len)
{
    char *b64 = malloc(1400);
    unsigned char *der = malloc(1024);
    bool ok = b64 && der && aos_hal_pref_get_str(k, b64, 1400) && b64[0] &&
              !mbedtls_base64_decode(der, 1024, len, (const unsigned char *)b64, strlen(b64));
    free(b64);
    if (ok) return der;
    free(der);
    return NULL;
}

static bool pref_set_der(const char *k, const unsigned char *der, size_t len)
{
    char b64[1400];
    size_t olen = 0;
    return !mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &olen, der, len) && aos_hal_pref_set_str(k, b64);
}

static bool key_load(const char *k, mbedtls_pk_context *key)
{
    size_t len = 0;
    unsigned char *der = pref_der(k, &len);
    bool ok = der && !mbedtls_pk_parse_key(key, der, len, NULL, 0, drbg_rng, NULL);
    if (der) memset(der, 0, len);
    free(der);
    return ok;
}

static bool key_new(const char *k, mbedtls_pk_context *key)
{
    unsigned char buf[256];
    bool ok = !mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) &&
              !mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*key), drbg_rng, NULL);
    int l = ok ? mbedtls_pk_write_key_der(key, buf, sizeof buf) : -1;    /* from the buffer's end */
    ok = l > 0 && pref_set_der(k, buf + sizeof buf - l, (size_t)l);
    memset(buf, 0, sizeof buf);
    return ok;
}

static bool crt_save(const char *k, mbedtls_x509write_cert *crt)
{
    unsigned char *buf = malloc(1024);
    int l = buf ? mbedtls_x509write_crt_der(crt, buf, 1024, drbg_rng, NULL) : -1;
    bool ok = l > 0 && pref_set_der(k, buf + 1024 - l, (size_t)l);
    free(buf);
    return ok;
}

/* "YYYYMMDDhhmmss", days from now; with no clock yet, from 2026-01-01 */
static void when(int days, char out[16])
{
    time_t t = aos_hal_time_is_valid() ? time(NULL) : 1767225600;
    t += (time_t)days * 86400;
    struct tm g;
    gmtime_r(&t, &g);
    strftime(out, 16, "%Y%m%d%H%M%S", &g);
}

static void serial(mbedtls_x509write_cert *crt)
{
    uint8_t s[16];
    rnd(s, sizeof s);
    s[0] &= 0x7F;                               /* a positive number */
    mbedtls_x509write_crt_set_serial_raw(crt, s, sizeof s);
}

/* a DER element: tag, length (short or two bytes) and content */
static size_t tlv(unsigned char *o, unsigned char tag, const void *c, size_t n)
{
    size_t h = 0;
    o[h++] = tag;
    if (n < 128) o[h++] = (unsigned char)n;
    else { o[h++] = 0x82; o[h++] = (unsigned char)(n >> 8); o[h++] = (unsigned char)n; }
    memmove(o + h, c, n);
    return h + n;
}

/* NameConstraints { permittedSubtrees [0] { dNSName "local", dNSName <name>,
 * iPAddress 10/8, 172.16/12, 192.168/16 } } */
static size_t name_constraints(unsigned char *out, const char *name)
{
    static const uint8_t NETS[3][8] = {
        { 10, 0, 0, 0, 255, 0, 0, 0 }, { 172, 16, 0, 0, 255, 240, 0, 0 }, { 192, 168, 0, 0, 255, 255, 0, 0 },
    };
    unsigned char subtrees[256], one[300], gn[300];     /* gcc cannot tell how long the name is */
    size_t n = 0, g;
    g = tlv(gn, 0x82, "local", 5);
    n += tlv(subtrees + n, 0x30, gn, g);
    g = tlv(gn, 0x82, name, strlen(name));
    n += tlv(subtrees + n, 0x30, gn, g);
    for (int i = 0; i < 3; i++) {
        g = tlv(gn, 0x87, NETS[i], 8);
        n += tlv(subtrees + n, 0x30, gn, g);
    }
    size_t p = tlv(one, 0xA0, subtrees, n);
    return tlv(out, 0x30, one, p);
}

/* nameConstraints, which this mbedTLS does not parse: the board's own
 * authority carries it (critical), and the board reads that certificate
 * itself. Browsers do apply it. */
static int accept_name_constraints(void *ctx, mbedtls_x509_crt const *crt, mbedtls_x509_buf const *oid,
                                   int critical, const unsigned char *p, const unsigned char *end)
{
    (void)ctx; (void)crt; (void)critical; (void)p; (void)end;
    return MBEDTLS_OID_CMP(MBEDTLS_OID_NAME_CONSTRAINTS, oid) == 0 ? 0 : -1;
}

/* each step of making a certificate, logged by name when it fails */
static bool step(int e, const char *what)
{
    if (e) aos_hal_log("access", "certificate: %s failed (-0x%04x)", what, (unsigned)-e);
    return !e;
}
#define STEP(call) step((call), #call)

static bool make_ca(void)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    const char *name = aos_hal_device_name();
    char subj[96], from[16], until[16];
    unsigned char nc[300];
    snprintf(subj, sizeof subj, "CN=P4OS %s,O=P4OS", name);
    when(-1, from);
    when(3650, until);
    bool ok = key_new(K_CAK, &key);
    if (ok) {
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        serial(&crt);
        size_t ncl = name_constraints(nc, name);
        ok = !mbedtls_x509write_crt_set_subject_name(&crt, subj) &&
             !mbedtls_x509write_crt_set_issuer_name(&crt, subj) &&
             !mbedtls_x509write_crt_set_validity(&crt, from, until) &&
             !mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0) &&
             !mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN) &&
             !mbedtls_x509write_crt_set_subject_key_identifier(&crt) &&
             !mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_NAME_CONSTRAINTS,
                                                  MBEDTLS_OID_SIZE(MBEDTLS_OID_NAME_CONSTRAINTS), 1, nc, ncl) &&
             crt_save(K_CAC, &crt);
    }
    aos_hal_log("access", ok ? "a new authority for %s's HTTPS" : "could not make %s's HTTPS authority", name);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return ok;
}

static void leaf_for(char *out, size_t n)
{
    snprintf(out, n, "%s|%s", aos_hal_device_name(), aos_hal_net_state() == AOS_NET_CONNECTED ? aos_hal_net_ip() : "");
}

static bool make_leaf(void)
{
    mbedtls_pk_context cakey, key;
    mbedtls_x509_crt ca;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&cakey);
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&ca);
    mbedtls_x509write_crt_init(&crt);
    const char *name = aos_hal_device_name();
    char subj[96], issuer[160], local[80], from[16], until[16], ip[16] = "";
    snprintf(local, sizeof local, "%s.local", name);
    snprintf(subj, sizeof subj, "CN=%s,O=P4OS", local);
    when(-1, from);
    when(LEAF_DAYS, until);
    size_t cal = 0;
    unsigned char *cader = pref_der(K_CAC, &cal);
    bool ok = cader && STEP(mbedtls_x509_crt_parse_der_with_ext_cb(&ca, cader, cal, 1, accept_name_constraints, NULL)) &&
              mbedtls_x509_dn_gets(issuer, sizeof issuer, &ca.subject) > 0 &&
              step(!key_load(K_CAK, &cakey), "the authority's key") && (key_load(K_KEY, &key) || key_new(K_KEY, &key));
    free(cader);
    if (ok) {
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &cakey);
        serial(&crt);
        static const char SERVER_AUTH[] = MBEDTLS_OID_SERVER_AUTH;
        mbedtls_asn1_sequence eku = { .buf = { .tag = MBEDTLS_ASN1_OID, .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH),
                                               .p = (unsigned char *)SERVER_AUTH }, .next = NULL };
        ok = STEP(mbedtls_x509write_crt_set_subject_name(&crt, subj)) &&
             STEP(mbedtls_x509write_crt_set_issuer_name(&crt, issuer)) &&
             STEP(mbedtls_x509write_crt_set_validity(&crt, from, until)) &&
             STEP(mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) &&
             STEP(mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) &&
             STEP(mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku)) &&
             STEP(mbedtls_x509write_crt_set_subject_key_identifier(&crt)) &&
             STEP(mbedtls_x509write_crt_set_authority_key_identifier(&crt));
#if MBEDTLS_VERSION_NUMBER >= 0x03050000
        /* <name>.local, the bare name, the AP, the cable and, if there is
         * one, the Wi-Fi address */
        static const uint8_t AP_IP[4] = { 192, 168, 4, 1 }, USB_IP[4] = { 192, 168, 7, 1 };
        uint8_t sta[4];
        int nsan = 4;
        unsigned a0, a1, a2, a3;
        if (aos_hal_net_state() == AOS_NET_CONNECTED && sscanf(aos_hal_net_ip(), "%u.%u.%u.%u", &a0, &a1, &a2, &a3) == 4) {
            sta[0] = (uint8_t)a0; sta[1] = (uint8_t)a1; sta[2] = (uint8_t)a2; sta[3] = (uint8_t)a3;
            snprintf(ip, sizeof ip, "%s", aos_hal_net_ip());
            nsan = 5;
        }
        mbedtls_x509_san_list san[5];
        memset(san, 0, sizeof san);
        const struct { int type; const void *p; size_t len; } S[5] = {
            { MBEDTLS_X509_SAN_DNS_NAME, local, strlen(local) }, { MBEDTLS_X509_SAN_DNS_NAME, name, strlen(name) },
            { MBEDTLS_X509_SAN_IP_ADDRESS, AP_IP, 4 }, { MBEDTLS_X509_SAN_IP_ADDRESS, USB_IP, 4 },
            { MBEDTLS_X509_SAN_IP_ADDRESS, sta, 4 },
        };
        for (int i = 0; i < nsan; i++) {
            san[i].node.type = S[i].type;
            san[i].node.san.unstructured_name.p = (unsigned char *)S[i].p;
            san[i].node.san.unstructured_name.len = S[i].len;
            san[i].next = i + 1 < nsan ? &san[i + 1] : NULL;
        }
        ok = ok && STEP(mbedtls_x509write_crt_set_subject_alternative_name(&crt, san));
#endif
        ok = ok && step(!crt_save(K_CRT, &crt), "writing it");
    }
    if (ok) {
        char f[96];
        leaf_for(f, sizeof f);
        size_t l = strlen(f);
        snprintf(f + l, sizeof f - l, "|%.8s", until);
        aos_hal_pref_set_str(K_CFOR, f);
    }
    aos_hal_log("access", ok ? "a new HTTPS certificate for %s.local, until %.8s" : "could not make %s.local's HTTPS certificate%.0s",
                name, until);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_x509_crt_free(&ca);
    mbedtls_pk_free(&key);
    mbedtls_pk_free(&cakey);
    return ok;
}

/* whether the portal's certificate is still the right one: the same name and
 * address, and 60 days or more left (only known with a clock) */
static bool leaf_fresh(void)
{
    char was[96], now[96], soon[16];
    if (!aos_hal_pref_get_str(K_CFOR, was, sizeof was)) return false;
    char *last = strrchr(was, '|');
    if (!last) return false;
    *last++ = 0;
    leaf_for(now, sizeof now);
    if (strcmp(was, now)) return false;
    when(60, soon);
    return !aos_hal_time_is_valid() || strncmp(last, soon, 8) > 0;
}

bool aos_access_tls_der(unsigned char **cert, size_t *cert_len, unsigned char **ca, size_t *ca_len,
                        unsigned char **key, size_t *key_len)
{
    *cert = *ca = *key = NULL;
    size_t l;
    unsigned char *probe = pref_der(K_CAC, &l);
    if (!probe) {
        aos_hal_pref_erase(K_CRT);              /* a new authority: the old certificate is not its */
        aos_hal_pref_erase(K_CFOR);
        if (!make_ca()) return false;
    }
    free(probe);
    if (!leaf_fresh() && !make_leaf()) return false;
    *cert = pref_der(K_CRT, cert_len);
    *ca = pref_der(K_CAC, ca_len);
    *key = pref_der(K_KEY, key_len);
    if (*cert && *ca && *key) return true;
    free(*cert);
    free(*ca);
    free(*key);
    *cert = *ca = *key = NULL;
    return false;
}

bool aos_access_tls_ca(unsigned char **der, size_t *len)
{
    *der = pref_der(K_CAC, len);
    return *der != NULL;
}

void aos_access_tls_forget(void)
{
    static const char *const K[] = { K_CAC, K_CAK, K_CRT, K_KEY, K_CFOR };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) aos_hal_pref_erase(K[i]);
}

/* the authority's: what a computer or a phone is told to trust */
void aos_access_tls_fingerprint(char *out, size_t n)
{
    out[0] = 0;
    size_t len = 0;
    unsigned char *der = pref_der(K_CAC, &len);
    if (!der) return;
    uint8_t h[32];
    mbedtls_sha256(der, len, h, 0);
    size_t o = 0;
    for (int i = 0; i < 32 && o + 4 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%02X", i ? ":" : "", h[i]);
    free(der);
}

/* ---- the tick ---- */

void aos_access_tick(void)
{
    static uint32_t last;
    static int shown = -1;          /* the mDNS state applied: -1 none yet */
    static char shown_ssid[33];
    static bool was_up;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - last < 1000) return;
    last = now;

    bool up = aos_hal_net_state() == AOS_NET_CONNECTED;
    const char *ssid = aos_hal_net_ssid();
    if (up) first_home(ssid);
    /* mDNS answers on a home network and is quiet on any other: it would tell
     * everyone there that a P4OS is around. It is applied again on every
     * connection, because mDNS turns itself back on when an address comes. */
    int want = up && aos_access_is_trusted(ssid);
    if (up != was_up || want != shown || strcmp(ssid, shown_ssid)) {
        if (up) aos_hal_net_mdns_visible(want);
        shown = want;
        was_up = up;
        snprintf(shown_ssid, sizeof shown_ssid, "%s", ssid);
    }
}
