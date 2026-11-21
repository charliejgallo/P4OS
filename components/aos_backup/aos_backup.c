/*
 * P4OS - Backups (docs/plan/RESPALDO.md, stage 1). aos_backup.h says what a
 * backup is and what the calls promise; this is how.
 *
 * The file is one JSON object, not an archive: everything in it is small
 * text (a board with 600 preferences makes some 40 KB), and one object that
 * a person can open and read is worth more than a format.
 *
 *   {"p4bak": 1, "made": <epoch>, "fw": "v0.30.0", "board": "<its name>",
 *    "seq": 0,                               which one of that second
 *    "before": true,                         made by itself before a restore
 *    "left_out": 12,                         credentials a plain one skipped
 *    "prefs": [["i", "volume", 40], ["s", "tz", "ART3"], ...],
 *    "sealed": {"kdf": "pbkdf2-sha256", "iter": N, "salt": b64, "iv": b64,
 *               "tag": b64, "data": b64},    INSTEAD of "prefs": that array,
 *                                            as JSON text, under AES-256-GCM
 *    "nprefs": 600,
 *    "apps": [{"file": "bastion", "ver": "v0.30.0", "packs": ["x.pak"]}],
 *    "files": {"menu.txt": "...", "modules.txt": "..."}}
 *
 * Memory: everything here is heap, from PSRAM on the board (BIG_ALLOC), and
 * the thread is the HAL's, whose stacks are PSRAM too (docs/MEMORY.md).
 * Nothing runs from an interrupt or with the flash's cache off.
 */
#include "aos_backup.h"
#include "aos_hal.h"
#include "cJSON.h"

#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef AOS_SIM
  #define BIG_ALLOC(n)  calloc(1, (n))
#else
  #include "esp_heap_caps.h"
  #define BIG_ALLOC(n)  heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

#define TAG         "backup"
#define FORMAT      1
#define FILE_MAX    (1024 * 1024)       /* a backup, whole */
#define SMALL_MAX   (64 * 1024)         /* menu.txt, modules.txt */
#define FULL_MAX    256
/* PBKDF2's rounds. The passphrase is typed on a 5" screen and will be
 * short: this is what stands between a card somebody found and the Wi-Fi's
 * password. Each round is one HMAC-SHA256; the file says how many it was
 * made with, so the number can grow. Measured on the board: PRUEBAS-PLACA. */
#define KDF_ITER    40000
#define AAD         "p4bak1"
#define STACK       (16 * 1024)

/* ---- which preferences ------------------------------------------------------ */

/* Of this board and this moment: in no backup, and never written back. */
static bool key_of_this_board(const char *k)
{
    static const char *const OWN[] = {
        "boot_bad", "safe_next", "cd_unread", "cd_when", "cd_fp",      /* the last starts and the crash dump */
        "time_ok", "lock_fails",
        "bt_bond",                          /* the pairing: its keys are not preferences */
        "pt_sess",                          /* the portal's open sessions */
        "bat_cap", "bat_min", "chg_cyc", "soc_n", "soc_sag",            /* a battery's history */
        "st_day", "st_today", "pomo_day", "pomo_done",                  /* today's counts */
        "lk_peer", "lk_lmk", "lk_pname",    /* the device it is linked with */
    };
    for (size_t i = 0; i < sizeof OWN / sizeof OWN[0]; i++)
        if (!strcmp(k, OWN[i])) return true;
    return false;
}

/* Whether a preference looks like a credential, by its name: a plain backup
 * leaves these out. A best effort: an app can keep a secret under a name
 * this does not see, which is why the screens say "what looks like". */
static bool key_is_secret(const char *k)
{
    static const char *const WORDS[] = { "pass", "pwd", "psk", "token", "secret", "apikey", "api_key", "cred", "auth" };
    static const char *const WHOLE[] = {
        "lock_pin",                         /* the lock screen's code */
        "claude_at", "claude_rt",           /* an account's tokens */
        "pt_key", "pt_cak",                 /* the portal's private keys (its certificates are public) */
        "ssh_hk",                           /* the SSH server's host key */
    };
    char low[40];
    size_t n = 0;
    for (; k[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)k[n]);
    low[n] = 0;
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++)
        if (strstr(low, WORDS[i])) return true;
    for (size_t i = 0; i < sizeof WHOLE / sizeof WHOLE[0]; i++)
        if (!strcmp(low, WHOLE[i])) return true;
    /* a key, as a word of its own: "x_key", "key_x" - not "keyboard" or "hotkeys" */
    const char *p = low;
    while ((p = strstr(p, "key")) != NULL) {
        bool starts = p == low || p[-1] == '_' || p[-1] == '.';
        bool ends = !p[3] || p[3] == '_' || p[3] == '.' || isdigit((unsigned char)p[3]);
        if (starts && ends && strcmp(low, "key_snd")) return true;
        p += 3;
    }
    return false;
}

/* ---- small things ----------------------------------------------------------- */

typedef struct { char why[120]; int kind; } err_t;

static int fail(err_t *e, int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof e->why, fmt, ap);
    va_end(ap);
    e->kind = kind;
    return kind;
}

static const char *full(char out[FULL_MAX], const char *rel)
{
    const char *root = aos_hal_path_sd_root();
    snprintf(out, FULL_MAX, "%s/%s", root ? root : "", rel);
    return out;
}

static char *slurp(const char *rel, size_t max, size_t *len)
{
    char f[FULL_MAX];
    struct stat st;
    if (stat(full(f, rel), &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) return NULL;
    FILE *fp = fopen(f, "rb");
    if (!fp) return NULL;
    char *buf = BIG_ALLOC((size_t)st.st_size + 1);
    size_t got = buf ? fread(buf, 1, (size_t)st.st_size, fp) : 0;
    fclose(fp);
    if (!buf || got != (size_t)st.st_size) { free(buf); return NULL; }
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

/* Whole or not at all: to a name of its own first, then into place. */
static bool spit(const char *rel, const char *data, size_t len)
{
    char f[FULL_MAX], tmp[FULL_MAX + 8];
    full(f, rel);
    snprintf(tmp, sizeof tmp, "%s.new", f);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return false;
    bool ok = fwrite(data, 1, len, fp) == len;
    ok = fflush(fp) == 0 && ok;
    fsync(fileno(fp));
    ok = fclose(fp) == 0 && ok;
    if (ok) {
        remove(f);                      /* FAT's rename does not replace */
        ok = rename(tmp, f) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
    return s ? s : "";
}

static bool ends(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a > b && !strcmp(s + a - b, tail);
}

/* A backup's name is only ever a file of its folder. */
static bool name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) >= AOS_BACKUP_NAME_MAX || !ends(name, AOS_BACKUP_EXT) || name[0] == '.') return false;
    for (const char *c = name; *c; c++)
        if (!isalnum((unsigned char)*c) && !strchr("_-.", *c)) return false;
    return !strstr(name, "..");
}

/* "v0.30.0-4-gabc" as 0x001E00, or -1. */
static int32_t version_of(const char *v)
{
    while (*v && !isdigit((unsigned char)*v)) v++;
    int a = 0, b = 0, c = 0;
    if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return (a & 0xFF) << 16 | (b & 0xFF) << 8 | (c & 0xFF);
}

/* ---- the seal --------------------------------------------------------------- */

/* PBKDF2-HMAC-SHA256, one block: 32 bytes are what AES-256 takes. */
static void kdf(const char *pass, const uint8_t *salt, size_t salt_len, uint32_t iter, uint8_t key[32])
{
    uint8_t first[64], u[32];
    size_t pl = strlen(pass);
    if (salt_len > sizeof first - 4) salt_len = sizeof first - 4;
    memcpy(first, salt, salt_len);
    first[salt_len] = 0; first[salt_len + 1] = 0; first[salt_len + 2] = 0; first[salt_len + 3] = 1;
    aos_hal_hmac_sha256(pass, pl, first, salt_len + 4, u);
    memcpy(key, u, 32);
    for (uint32_t i = 1; i < iter; i++) {
        aos_hal_hmac_sha256(pass, pl, u, 32, u);
        for (int k = 0; k < 32; k++) key[k] ^= u[k];
    }
}

static char *b64(const uint8_t *data, size_t len)
{
    size_t cap = (len + 2) / 3 * 4 + 1, got = 0;
    char *out = BIG_ALLOC(cap);
    if (!out || mbedtls_base64_encode((unsigned char *)out, cap, &got, data, len) != 0) { free(out); return NULL; }
    out[got] = 0;
    return out;
}

static uint8_t *unb64(const char *text, size_t *len)
{
    size_t tl = strlen(text), cap = tl / 4 * 3 + 4;
    uint8_t *out = BIG_ALLOC(cap);
    if (!out || mbedtls_base64_decode(out, cap, len, (const unsigned char *)text, tl) != 0) { free(out); return NULL; }
    return out;
}

/* The text under a passphrase: an object for "sealed", or NULL. */
static cJSON *seal(const char *text, const char *pass)
{
    size_t len = strlen(text);
    uint8_t salt[16], iv[12], tag[16], key[32];
    uint8_t *enc = BIG_ALLOC(len ? len : 1);
    cJSON *o = NULL;
    if (!enc) return NULL;
    aos_hal_random(salt, sizeof salt);
    aos_hal_random(iv, sizeof iv);
    kdf(pass, salt, sizeof salt, KDF_ITER, key);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, iv, sizeof iv, (const unsigned char *)AAD, strlen(AAD),
                                        (const unsigned char *)text, enc, sizeof tag, tag) == 0;
    mbedtls_gcm_free(&g);
    memset(key, 0, sizeof key);
    char *s = b64(salt, sizeof salt), *i = b64(iv, sizeof iv), *t = b64(tag, sizeof tag), *d = ok ? b64(enc, len) : NULL;
    if (ok && s && i && t && d) {
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "kdf", "pbkdf2-sha256");
        cJSON_AddNumberToObject(o, "iter", KDF_ITER);
        cJSON_AddStringToObject(o, "salt", s);
        cJSON_AddStringToObject(o, "iv", i);
        cJSON_AddStringToObject(o, "tag", t);
        cJSON_AddStringToObject(o, "data", d);
    }
    free(s); free(i); free(t); free(d); free(enc);
    return o;
}

/* The text back, NUL-terminated, to free(); NULL when the passphrase is not
 * the one (the tag does not check) or the object is not a seal. */
static char *unseal(const cJSON *sealed, const char *pass)
{
    size_t sl = 0, il = 0, tl = 0, dl = 0;
    uint32_t iter = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(sealed, "iter"));
    if (strcmp(jstr(sealed, "kdf"), "pbkdf2-sha256") || iter < 1000 || iter > 5000000) return NULL;
    uint8_t *salt = unb64(jstr(sealed, "salt"), &sl), *iv = unb64(jstr(sealed, "iv"), &il);
    uint8_t *tag = unb64(jstr(sealed, "tag"), &tl), *data = unb64(jstr(sealed, "data"), &dl);
    char *out = NULL;
    if (salt && iv && tag && data && sl >= 8 && il == 12 && tl == 16) {
        uint8_t key[32];
        kdf(pass, salt, sl, iter, key);
        out = BIG_ALLOC(dl + 1);
        mbedtls_gcm_context g;
        mbedtls_gcm_init(&g);
        bool ok = out && mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                  mbedtls_gcm_auth_decrypt(&g, dl, iv, il, (const unsigned char *)AAD, strlen(AAD), tag, tl, data,
                                           (unsigned char *)out) == 0;
        mbedtls_gcm_free(&g);
        memset(key, 0, sizeof key);
        if (!ok) { free(out); out = NULL; }
        else out[dl] = 0;
    }
    free(salt); free(iv); free(tag); free(data);
    return out;
}

/* ---- making one ------------------------------------------------------------- */

typedef struct { cJSON *arr; bool secrets; int n, left_out; } gather_t;

static void gather(const char *key, bool is_str, int32_t v, const char *s, void *ctx)
{
    gather_t *g = ctx;
    if (key_of_this_board(key)) return;
    if (!g->secrets && key_is_secret(key)) { g->left_out++; return; }
    cJSON *one = cJSON_CreateArray();
    cJSON_AddItemToArray(one, cJSON_CreateString(is_str ? "s" : "i"));
    cJSON_AddItemToArray(one, cJSON_CreateString(key));
    cJSON_AddItemToArray(one, is_str ? cJSON_CreateString(s ? s : "") : cJSON_CreateNumber(v));
    cJSON_AddItemToArray(g->arr, one);
    g->n++;
}

/* The apps the Tienda installed, from its receipts: enough to ask for them
 * again. Parts of the system included: they are asked for the same way. */
static cJSON *apps_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    char d[FULL_MAX], rel[160];
    DIR *dir = opendir(full(d, "apps/.store"));
    struct dirent *de;
    while (dir && (de = readdir(dir)) != NULL) {
        if (!ends(de->d_name, ".json") || de->d_name[0] == '.' || strlen(de->d_name) > 40) continue;
        snprintf(rel, sizeof rel, "apps/.store/%s", de->d_name);
        char *text = slurp(rel, 128 * 1024, NULL);
        cJSON *r = text ? cJSON_Parse(text) : NULL;
        free(text);
        if (r && jstr(r, "file")[0]) {
            cJSON *a = cJSON_CreateObject(), *packs = cJSON_AddArrayToObject(a, "packs");
            const cJSON *p;
            cJSON_AddStringToObject(a, "file", jstr(r, "file"));
            cJSON_AddStringToObject(a, "ver", jstr(r, "ver"));
            cJSON_AddStringToObject(a, "from", jstr(cJSON_GetObjectItem(r, "source"), "catalog"));
            cJSON_ArrayForEach(p, cJSON_GetObjectItem(r, "packs"))
                if (jstr(p, "file")[0]) cJSON_AddItemToArray(packs, cJSON_CreateString(jstr(p, "file")));
            cJSON_AddItemToArray(arr, a);
        }
        cJSON_Delete(r);
    }
    if (dir) closedir(dir);
    return arr;
}

int aos_backup_make_now(const char *passphrase, bool before, char *name_out, size_t name_len, char *why, size_t why_len)
{
    err_t e = { "", 0 };
    bool sealed = passphrase && passphrase[0];
    char name[AOS_BACKUP_NAME_MAX], rel[160], f[FULL_MAX];
    cJSON *o = NULL;
    char *text = NULL;
    if (!aos_hal_path_sd_root()) { fail(&e, AOS_BACKUP_E_CARD, "no card"); goto out; }
    mkdir(full(f, AOS_BACKUP_DIR), 0775);

    /* its name is its date; without a clock, the next number free */
    time_t now = aos_hal_time_is_valid() ? time(NULL) : 0;
    if (now) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(name, sizeof name, "p4os-%Y%m%d-%H%M%S", &tm);
    } else snprintf(name, sizeof name, "p4os-sin-fecha");
    int seq = 0;
    for (; seq < 100; seq++) {
        char tail[24] = "";
        if (seq) snprintf(tail, sizeof tail, "-%d", seq);
        snprintf(rel, sizeof rel, AOS_BACKUP_DIR "/%s%s%s" AOS_BACKUP_EXT, name, before ? "-antes" : "", tail);
        struct stat st;
        if (stat(full(f, rel), &st) != 0) break;
    }

    o = cJSON_CreateObject();
    gather_t g = { cJSON_CreateArray(), sealed, 0, 0 };
    if (!o || !g.arr) { cJSON_Delete(g.arr); fail(&e, AOS_BACKUP_E_MEMORY, "no memory"); goto out; }
    aos_hal_pref_foreach(gather, &g);
    cJSON_AddNumberToObject(o, "p4bak", FORMAT);
    cJSON_AddNumberToObject(o, "made", (double)now);
    cJSON_AddNumberToObject(o, "seq", seq);        /* which one of that second, or of a board with no clock */
    cJSON_AddStringToObject(o, "fw", aos_hal_firmware_version());
    cJSON_AddStringToObject(o, "board", aos_hal_device_name());
    if (before) cJSON_AddTrueToObject(o, "before");
    cJSON_AddNumberToObject(o, "nprefs", g.n);
    cJSON_AddNumberToObject(o, "left_out", g.left_out);
    if (sealed) {
        char *plain = cJSON_PrintUnformatted(g.arr);
        cJSON *s = plain ? seal(plain, passphrase) : NULL;
        if (plain) memset(plain, 0, strlen(plain));
        free(plain);
        cJSON_Delete(g.arr);
        if (!s) { fail(&e, AOS_BACKUP_E_MEMORY, "could not seal it"); goto out; }
        cJSON_AddItemToObject(o, "sealed", s);
    } else cJSON_AddItemToObject(o, "prefs", g.arr);
    cJSON *apps = apps_list();
    cJSON_AddItemToObject(o, "apps", apps);
    cJSON *files = cJSON_AddObjectToObject(o, "files");
    static const char *const SMALL[] = { "menu.txt", "modules.txt" };
    for (size_t i = 0; i < sizeof SMALL / sizeof SMALL[0]; i++) {
        char *t = slurp(SMALL[i], SMALL_MAX, NULL);
        if (t) cJSON_AddStringToObject(files, SMALL[i], t);
        free(t);
    }

    text = cJSON_PrintUnformatted(o);
    if (!text) { fail(&e, AOS_BACKUP_E_MEMORY, "no memory"); goto out; }
    if (!spit(rel, text, strlen(text))) { fail(&e, AOS_BACKUP_E_CARD, "could not write %.80s", rel); goto out; }
    const char *base = strrchr(rel, '/');
    if (name_out) snprintf(name_out, name_len, "%s", base ? base + 1 : rel);
    snprintf(e.why, sizeof e.why, "%.60s: %d preferences%s, %d apps, %u bytes", base ? base + 1 : rel, g.n,
             sealed ? ", sealed" : "", cJSON_GetArraySize(apps), (unsigned)strlen(text));
out:
    if (text) memset(text, 0, strlen(text));
    free(text);
    cJSON_Delete(o);
    if (why) snprintf(why, why_len, "%s", e.why);
    return e.kind;
}

/* ---- reading one ------------------------------------------------------------ */

static cJSON *open_backup(const char *name, uint32_t *size, err_t *e)
{
    char rel[160];
    size_t len = 0;
    if (!name_ok(name)) { fail(e, AOS_BACKUP_E_NOT_FOUND, "not a backup's name"); return NULL; }
    snprintf(rel, sizeof rel, AOS_BACKUP_DIR "/%s", name);
    char *text = slurp(rel, FILE_MAX, &len);
    if (!text) { fail(e, AOS_BACKUP_E_NOT_FOUND, "%.60s is not on the card", name); return NULL; }
    cJSON *o = cJSON_ParseWithLength(text, len);
    free(text);
    if (!cJSON_IsObject(o) || cJSON_GetNumberValue(cJSON_GetObjectItem(o, "p4bak")) != FORMAT ||
        (!cJSON_IsArray(cJSON_GetObjectItem(o, "prefs")) && !cJSON_IsObject(cJSON_GetObjectItem(o, "sealed")))) {
        cJSON_Delete(o);
        fail(e, AOS_BACKUP_E_NOT_ONE, "%.60s is not a backup this P4OS reads", name);
        return NULL;
    }
    if (size) *size = (uint32_t)len;
    return o;
}

static void info_of(const cJSON *o, const char *name, uint32_t size, aos_backup_info_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);
    out->made = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "made"));
    out->seq = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "seq"));
    snprintf(out->fw, sizeof out->fw, "%s", jstr(o, "fw"));
    snprintf(out->board, sizeof out->board, "%s", jstr(o, "board"));
    out->sealed = cJSON_IsObject(cJSON_GetObjectItem(o, "sealed"));
    out->before = cJSON_IsTrue(cJSON_GetObjectItem(o, "before"));
    out->prefs = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "nprefs"));
    out->left_out = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "left_out"));
    out->apps = cJSON_GetArraySize(cJSON_GetObjectItem(o, "apps"));
    out->size = size;
}

bool aos_backup_info(const char *name, aos_backup_info_t *out)
{
    err_t e = { "", 0 };
    uint32_t size = 0;
    cJSON *o = open_backup(name, &size, &e);
    if (!o) return false;
    info_of(o, name, size, out);
    cJSON_Delete(o);
    return true;
}

int aos_backup_list(aos_backup_info_t *out, int max)
{
    char d[FULL_MAX];
    int n = 0;
    if (!aos_hal_path_sd_root()) return 0;
    DIR *dir = opendir(full(d, AOS_BACKUP_DIR));
    struct dirent *de;
    while (dir && (de = readdir(dir)) != NULL) {
        aos_backup_info_t one;
        if (!name_ok(de->d_name) || !aos_backup_info(de->d_name, &one)) continue;
        /* the newest first; two of one second, by which was made after */
        int at = n < max ? n : max;
        while (at > 0 && (out[at - 1].made < one.made || (out[at - 1].made == one.made && out[at - 1].seq < one.seq))) at--;
        if (at >= max) continue;
        int last = n < max ? n : max - 1;
        memmove(&out[at + 1], &out[at], sizeof *out * (size_t)(last - at));
        out[at] = one;
        if (n < max) n++;
    }
    if (dir) closedir(dir);
    return n;
}

bool aos_backup_delete(const char *name)
{
    char rel[160], f[FULL_MAX];
    if (!name_ok(name)) return false;
    snprintf(rel, sizeof rel, AOS_BACKUP_DIR "/%s", name);
    return remove(full(f, rel)) == 0;
}

int aos_backup_missing_apps(const char *name, aos_backup_app_t *out, int max)
{
    err_t e = { "", 0 };
    cJSON *o = open_backup(name, NULL, &e);
    const cJSON *a, *p;
    int n = 0;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(o, "apps")) {
        const char *file = jstr(a, "file");
        char rel[96], f[FULL_MAX];
        struct stat st;
        if (!file[0] || strlen(file) >= sizeof out[0].file || n >= max) continue;
        /* there when the Tienda has its receipt: an app's or a part of the system's */
        snprintf(rel, sizeof rel, "apps/.store/%s.json", file);
        if (stat(full(f, rel), &st) == 0) continue;
        aos_backup_app_t *x = &out[n++];
        memset(x, 0, sizeof *x);
        snprintf(x->file, sizeof x->file, "%s", file);
        cJSON_ArrayForEach(p, cJSON_GetObjectItem(a, "packs")) {
            const char *pf = cJSON_GetStringValue(p);
            if (!pf || strlen(x->packs) + strlen(pf) + 2 >= sizeof x->packs) continue;
            if (x->packs[0]) strcat(x->packs, ",");
            strcat(x->packs, pf);
        }
    }
    cJSON_Delete(o);
    return n;
}

int aos_backup_restore_now(const char *name, const char *passphrase, int *written, char *why, size_t why_len)
{
    err_t e = { "", 0 };
    cJSON *o = open_backup(name, NULL, &e), *prefs = NULL;
    char *plain = NULL;
    int n = 0;
    if (!o) goto out;

    int32_t made_by = version_of(jstr(o, "fw")), mine = version_of(aos_hal_firmware_version());
    if (made_by >= 0 && mine >= 0 && made_by > mine) {
        fail(&e, AOS_BACKUP_E_NEWER, "made by P4OS %.20s, newer than this one: update first", jstr(o, "fw"));
        goto out;
    }
    const cJSON *sealed = cJSON_GetObjectItem(o, "sealed");
    if (cJSON_IsObject(sealed)) {
        plain = passphrase && passphrase[0] ? unseal(sealed, passphrase) : NULL;
        prefs = plain ? cJSON_Parse(plain) : NULL;
        if (!cJSON_IsArray(prefs)) { fail(&e, AOS_BACKUP_E_PASSPHRASE, "sealed, and that is not its passphrase"); goto out; }
    } else prefs = cJSON_Duplicate(cJSON_GetObjectItem(o, "prefs"), true);
    if (!cJSON_IsArray(prefs)) { fail(&e, AOS_BACKUP_E_MEMORY, "no memory"); goto out; }

    /* what there is now, first: a restore that turns out wrong goes back */
    char kept[AOS_BACKUP_NAME_MAX] = "", w2[120];
    int rc = aos_backup_make_now(cJSON_IsObject(sealed) ? passphrase : NULL, true, kept, sizeof kept, w2, sizeof w2);
    if (rc != AOS_BACKUP_E_NONE) { fail(&e, rc, "could not keep what there is first: %.70s", w2); goto out; }

    const cJSON *p;
    cJSON_ArrayForEach(p, prefs) {
        const char *type = cJSON_GetStringValue(cJSON_GetArrayItem(p, 0));
        const char *key = cJSON_GetStringValue(cJSON_GetArrayItem(p, 1));
        const cJSON *v = cJSON_GetArrayItem(p, 2);
        if (!type || !key || !key[0] || strlen(key) > 15 || !v || key_of_this_board(key)) continue;
        bool ok = !strcmp(type, "s") ? cJSON_IsString(v) && aos_hal_pref_set_str(key, v->valuestring) :
                  !strcmp(type, "i") ? cJSON_IsNumber(v) && aos_hal_pref_set_i32(key, (int32_t)v->valuedouble) : false;
        if (ok) n++;
        else aos_hal_log(TAG, "%s was not written back", key);
    }
    /* how the card is arranged: the two files a person edits */
    const cJSON *file;
    cJSON_ArrayForEach(file, cJSON_GetObjectItem(o, "files")) {
        if (!cJSON_IsString(file) || (strcmp(file->string, "menu.txt") && strcmp(file->string, "modules.txt"))) continue;
        if (!spit(file->string, file->valuestring, strlen(file->valuestring))) aos_hal_log(TAG, "%s was not written back", file->string);
    }
    snprintf(e.why, sizeof e.why, "%d preferences written; what there was is in %.60s", n, kept);
out:
    if (plain) memset(plain, 0, strlen(plain));
    free(plain);
    cJSON_Delete(prefs);
    cJSON_Delete(o);
    if (written) *written = n;
    if (why) snprintf(why, why_len, "%s", e.why);
    return e.kind;
}

/* ---- the thread ------------------------------------------------------------- */

static void *s_lock;
static aos_backup_status_t s_st;
static struct { bool restore; char name[AOS_BACKUP_NAME_MAX]; char pass[96]; } s_job;

static void lock(void)
{
    if (!s_lock) s_lock = aos_hal_mutex_create();
    aos_hal_mutex_lock(s_lock);
}
static void unlock(void) { aos_hal_mutex_unlock(s_lock); }

static void work(void *arg)
{
    (void)arg;
    char why[120] = "", name[AOS_BACKUP_NAME_MAX] = "";
    int written = 0;
    int kind = s_job.restore ? aos_backup_restore_now(s_job.name, s_job.pass, &written, why, sizeof why)
                             : aos_backup_make_now(s_job.pass, false, name, sizeof name, why, sizeof why);
    memset(s_job.pass, 0, sizeof s_job.pass);
    aos_hal_log(TAG, "%s: %s", s_job.restore ? "restore" : "make", why);
    lock();
    s_st.busy = false;
    s_st.jobs_done++;
    s_st.last_ok = kind == AOS_BACKUP_E_NONE;
    s_st.last_kind = kind;
    snprintf(s_st.last_job, sizeof s_st.last_job, "%s", s_job.restore ? "restore" : "make");
    snprintf(s_st.last, sizeof s_st.last, "%s", why);
    snprintf(s_st.name, sizeof s_st.name, "%s", s_job.restore ? s_job.name : name);
    s_st.written = written;
    if (s_job.restore && kind == AOS_BACKUP_E_NONE) s_st.restart = true;
    unlock();
}

static bool start(bool restore, const char *name, const char *pass)
{
    lock();
    bool ok = !s_st.busy;
    if (ok) {
        s_job.restore = restore;
        snprintf(s_job.name, sizeof s_job.name, "%s", name ? name : "");
        snprintf(s_job.pass, sizeof s_job.pass, "%s", pass ? pass : "");
        s_st.busy = true;
        snprintf(s_st.job, sizeof s_st.job, "%s", restore ? "restore" : "make");
    }
    unlock();
    if (ok && !aos_hal_thread_start("aos_backup", work, NULL, STACK, 2)) {
        lock();
        s_st.busy = false;
        unlock();
        ok = false;
    }
    return ok;
}

bool aos_backup_make(const char *passphrase) { return start(false, NULL, passphrase); }

bool aos_backup_restore(const char *name, const char *passphrase)
{
    return name_ok(name) && start(true, name, passphrase);
}

void aos_backup_status(aos_backup_status_t *out)
{
    lock();
    *out = s_st;
    unlock();
}
