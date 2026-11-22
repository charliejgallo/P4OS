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
#include "aos_sshc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#define SALT_LEN    16
#define KEEP        5                   /* the automatic ones the card keeps, and the "before a restore" ones */
#define PRUNE_MAX   64
/* The automatic copies' preferences (stage 2): whether they are made
 * ("bk_auto", on when absent), the key they are sealed under when the
 * person chose a passphrase for them ("bk_auto_key": its salt and the key
 * PBKDF2 gave, never the passphrase), the day of the last look ("bk_day",
 * 20261121), what the preferences added up to at the last copy ("bk_sum")
 * and whether this board has answered the offer of a new board or made a
 * backup of its own ("bk_seen"). */
#define K_AUTO      "bk_auto"
#define K_AUTO_KEY  "bk_auto_key"
#define K_DAY       "bk_day"
#define K_SUM       "bk_sum"
#define K_SEEN      "bk_seen"
/* Stage 3, the pendrive: which folders of the card go with the backups
 * ("bk_dirs": "notas=1,music=0", only the ones a person changed), and the
 * folder they go to on the drive. */
#define K_DIRS      "bk_dirs"
/* Stage 4, a server of the person's own, by SFTP: where ("bk_srv_host",
 * "_port", "_user", "_dir"), the server's key as the person accepted it at
 * the board ("bk_srv_hk": public, and what keeps another server from
 * taking its place), whether a copy goes by itself once a week
 * ("bk_srv_on") and when the last one went ("bk_srv_last"). The key the
 * board logs in with is the SSH client's ("sshc_seed", apps/ssh): one
 * identity for the board, authorised once on the server. */
#define K_SRV_HOST  "bk_srv_host"
#define K_SRV_PORT  "bk_srv_port"
#define K_SRV_USER  "bk_srv_user"
#define K_SRV_DIR   "bk_srv_dir"
#define K_SRV_HK    "bk_srv_hk"
#define K_SRV_ON    "bk_srv_on"
#define K_SRV_LAST  "bk_srv_last"
#define K_SSH_SEED  "sshc_seed"
#define SRV_DIR_DEF "p4os-backups"
#define SRV_EVERY_S (7 * 24 * 3600)
#define HK_MAX      600
#define DRIVE_DIR   "p4os-backup"
#define COPY_CHUNK  (32 * 1024)
#define COPY_DEPTH  8

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
        K_DAY, K_SUM, K_SEEN, K_SRV_LAST,   /* this module's own marks */
    };
    for (size_t i = 0; i < sizeof OWN / sizeof OWN[0]; i++)
        if (!strcmp(k, OWN[i])) return true;
    return false;
}

/* Whether a preference looks like a credential, by its name: a plain backup
 * leaves these out. A best effort: an app can keep a secret under a name
 * this does not see, which is why the screens say "what looks like". An app
 * that wants to be sure names its secret with one of these words, or starts
 * it with "sec_". The first plain backup made on the board (2026-11-21)
 * carried the SSH client's private key, "sshc_seed": a name with none of
 * them, found by looking at what the file had. */
static bool key_is_secret(const char *k)
{
    static const char *const WORDS[] = { "pass", "pwd", "psk", "token", "secret", "apikey", "api_key", "cred", "auth", "priv" };
    static const char *const WHOLE[] = {
        "lock_pin",                         /* the lock screen's code */
        "claude_at", "claude_rt",           /* an account's tokens */
        "pt_key", "pt_cak",                 /* the portal's private keys (its certificates are public) */
        "ssh_hk",                           /* the SSH server's host key */
        "sshc_seed",                        /* and the SSH client's own private key */
    };
    char low[40];
    size_t n = 0;
    for (; k[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)k[n]);
    low[n] = 0;
    if (!strncmp(low, "sec_", 4)) return true;
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

/* The text under a key that PBKDF2 gave from a passphrase and 'salt': an
 * object for "sealed", or NULL. The automatic copies come here with the
 * salt and the key the board keeps, and a new IV each time. */
static cJSON *seal_with(const char *text, const uint8_t salt[SALT_LEN], const uint8_t key[32])
{
    size_t len = strlen(text);
    uint8_t iv[12], tag[16];
    uint8_t *enc = BIG_ALLOC(len ? len : 1);
    cJSON *o = NULL;
    if (!enc) return NULL;
    aos_hal_random(iv, sizeof iv);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, iv, sizeof iv, (const unsigned char *)AAD, strlen(AAD),
                                        (const unsigned char *)text, enc, sizeof tag, tag) == 0;
    mbedtls_gcm_free(&g);
    char *s = b64(salt, SALT_LEN), *i = b64(iv, sizeof iv), *t = b64(tag, sizeof tag), *d = ok ? b64(enc, len) : NULL;
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

/* The same under a passphrase, with a salt of its own. */
static cJSON *seal(const char *text, const char *pass)
{
    uint8_t salt[SALT_LEN], key[32];
    aos_hal_random(salt, sizeof salt);
    kdf(pass, salt, sizeof salt, KDF_ITER, key);
    cJSON *o = seal_with(text, salt, key);
    memset(key, 0, sizeof key);
    return o;
}

/* The salt and the key of the automatic copies, when the person set a
 * passphrase for them. */
static bool auto_key(uint8_t salt[SALT_LEN], uint8_t key[32])
{
    char text[96] = "";
    size_t len = 0;
    if (!aos_hal_pref_get_str(K_AUTO_KEY, text, sizeof text) || !text[0]) return false;
    uint8_t *raw = unb64(text, &len);
    bool ok = raw && len == SALT_LEN + 32;
    if (ok) { memcpy(salt, raw, SALT_LEN); memcpy(key, raw + SALT_LEN, 32); }
    if (raw) memset(raw, 0, len);
    free(raw);
    memset(text, 0, sizeof text);
    return ok;
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

/* What the preferences add up to: a number that changes when any of them
 * does, to know whether a day needs its copy. Not for anything else: FNV-1a
 * of each one, added, so the order they are listed in does not matter (the
 * flash moves them around). The ones of this board and moment are out, or
 * a counter would make a copy every day. */
static void sum_one(const char *key, bool is_str, int32_t v, const char *s, void *ctx)
{
    if (key_of_this_board(key)) return;
    uint64_t h = 1469598103934665603ull;
    char num[16];
    if (!is_str) snprintf(num, sizeof num, "%d", (int)v);
    const char *parts[] = { key, is_str ? "\x01s" : "\x01i", is_str ? (s ? s : "") : num };
    for (int i = 0; i < 3; i++)
        for (const unsigned char *c = (const unsigned char *)parts[i]; *c; c++) { h ^= *c; h *= 1099511628211ull; }
    *(uint64_t *)ctx += h;
}

static void prefs_sum(char out[20])
{
    uint64_t sum = 0;
    aos_hal_pref_foreach(sum_one, &sum);
    snprintf(out, 20, "%016llx", (unsigned long long)sum);
}

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

/* Why a backup was made, in its name and in its file. */
static const char *const WHY_TAIL[] = { "", "-antes", "-auto", "-fw" };
static const char *const WHY_WORD[] = { "", "restore", "daily", "firmware" };

static void prune(void);

/* One backup. 'passphrase' seals it; without one, 'stored' (the automatic
 * copies' salt and key) does; without either it is plain. */
static int make(const char *passphrase, const uint8_t *stored, int reason, char *name_out, size_t name_len, char *why, size_t why_len)
{
    err_t e = { "", 0 };
    bool sealed = (passphrase && passphrase[0]) || stored;
    bool before = reason == AOS_BACKUP_BEFORE_RESTORE;
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
    /* Which one of that second (or of a board with no clock): one more than
     * the last there is, whatever each was made for. The list is ordered by
     * it among equals, and a number freed by a copy that was deleted must
     * not come back as the newest. */
    int seq = 0;
    {
        DIR *dir = opendir(full(f, AOS_BACKUP_DIR));
        struct dirent *de;
        size_t nl = strlen(name);
        while (dir && (de = readdir(dir)) != NULL) {
            const char *d = de->d_name, *ext = strrchr(d, '.'), *dash = strrchr(d, '-');
            if (strncmp(d, name, nl) || (d[nl] != '-' && d[nl] != '.') || !ext) continue;
            int n = 0;
            if (dash && dash >= d + nl && ext - dash >= 2 && ext - dash <= 4 && isdigit((unsigned char)dash[1])) n = atoi(dash + 1);
            if (n + 1 > seq) seq = n + 1;
        }
        if (dir) closedir(dir);
        char tail[24] = "";
        if (seq) snprintf(tail, sizeof tail, "-%d", seq);
        snprintf(rel, sizeof rel, AOS_BACKUP_DIR "/%s%s%s" AOS_BACKUP_EXT, name, WHY_TAIL[reason], tail);
    }

    o = cJSON_CreateObject();
    gather_t g = { cJSON_CreateArray(), sealed, 0, 0 };
    if (!o || !g.arr) { cJSON_Delete(g.arr); fail(&e, AOS_BACKUP_E_MEMORY, "no memory"); goto out; }
    aos_hal_pref_foreach(gather, &g);
    cJSON_AddNumberToObject(o, "p4bak", FORMAT);
    cJSON_AddNumberToObject(o, "made", (double)now);
    cJSON_AddNumberToObject(o, "seq", seq);
    cJSON_AddStringToObject(o, "fw", aos_hal_firmware_version());
    cJSON_AddStringToObject(o, "board", aos_hal_device_name());
    if (before) cJSON_AddTrueToObject(o, "before");
    if (reason) cJSON_AddStringToObject(o, "why", WHY_WORD[reason]);
    cJSON_AddNumberToObject(o, "nprefs", g.n);
    cJSON_AddNumberToObject(o, "left_out", g.left_out);
    if (sealed) {
        char *plain = cJSON_PrintUnformatted(g.arr);
        cJSON *s = !plain ? NULL : passphrase && passphrase[0] ? seal(plain, passphrase) : seal_with(plain, stored, stored + SALT_LEN);
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
    /* a board that makes backups is not a new one: the offer is not for it */
    int32_t seen = 0;
    if (!aos_hal_pref_get_i32(K_SEEN, &seen) || !seen) aos_hal_pref_set_i32(K_SEEN, 1);
out:
    if (text) memset(text, 0, strlen(text));
    free(text);
    cJSON_Delete(o);
    if (why) snprintf(why, why_len, "%s", e.why);
    if (!e.kind && reason) prune();
    return e.kind;
}

int aos_backup_make_now(const char *passphrase, bool before, char *name_out, size_t name_len, char *why, size_t why_len)
{
    return make(passphrase, NULL, before ? AOS_BACKUP_BEFORE_RESTORE : AOS_BACKUP_BY_HAND, name_out, name_len, why, why_len);
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
    out->why = out->before ? AOS_BACKUP_BEFORE_RESTORE : AOS_BACKUP_BY_HAND;
    for (int i = 2; i < (int)(sizeof WHY_WORD / sizeof WHY_WORD[0]); i++)
        if (!strcmp(jstr(o, "why"), WHY_WORD[i])) out->why = i;
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

/* ---- the ones it makes by itself -------------------------------------------- */

/* The card keeps the last KEEP automatic copies (a day's and the ones made
 * before a firmware, together) and the last KEEP made before a restore. The
 * ones a person made are theirs to delete. */
static void prune(void)
{
    aos_backup_info_t *all = BIG_ALLOC(sizeof *all * PRUNE_MAX);
    int n = all ? aos_backup_list(all, PRUNE_MAX) : 0, autos = 0, befores = 0;
    for (int i = 0; i < n; i++) {       /* the newest first */
        bool is_auto = all[i].why == AOS_BACKUP_DAILY || all[i].why == AOS_BACKUP_BEFORE_FW;
        bool is_before = all[i].why == AOS_BACKUP_BEFORE_RESTORE;
        if ((is_auto && ++autos > KEEP) || (is_before && ++befores > KEEP)) {
            aos_hal_log(TAG, "%s goes: the card keeps the last %d", all[i].name, KEEP);
            aos_backup_delete(all[i].name);
        }
    }
    free(all);
}

bool aos_backup_auto_on(void)
{
    int32_t on = 1;
    return !aos_hal_pref_get_i32(K_AUTO, &on) || on != 0;
}

void aos_backup_auto_set(bool on) { aos_hal_pref_set_i32(K_AUTO, on ? 1 : 0); }

bool aos_backup_auto_sealed(void)
{
    uint8_t salt[SALT_LEN], key[32];
    bool ok = auto_key(salt, key);
    memset(key, 0, sizeof key);
    return ok;
}

bool aos_backup_auto_passphrase_now(const char *passphrase)
{
    if (!passphrase || !passphrase[0]) return aos_hal_pref_set_str(K_AUTO_KEY, "");
    uint8_t raw[SALT_LEN + 32];
    aos_hal_random(raw, SALT_LEN);
    kdf(passphrase, raw, SALT_LEN, KDF_ITER, raw + SALT_LEN);
    char *text = b64(raw, sizeof raw);
    bool ok = text && aos_hal_pref_set_str(K_AUTO_KEY, text);
    memset(raw, 0, sizeof raw);
    if (text) memset(text, 0, strlen(text));
    free(text);
    return ok;
}

uint32_t aos_backup_auto_day(void)
{
    int32_t day = 0;
    aos_hal_pref_get_i32(K_DAY, &day);
    return (uint32_t)day;
}

static int32_t today(void)
{
    if (!aos_hal_time_is_valid()) return 0;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* The copy of what changed since the last one. 'asked' is a person asking
 * for it (the pendrive): made whatever the switch says. */
static int copy_if_changed(int reason, bool asked, bool *made, char *why, size_t why_len)
{
    char sum[20], last[20] = "", name[AOS_BACKUP_NAME_MAX];
    uint8_t stored[SALT_LEN + 32];
    if (made) *made = false;
    if (why && why_len) why[0] = 0;
    if (!asked && !aos_backup_auto_on()) return AOS_BACKUP_E_NONE;
    prefs_sum(sum);
    aos_hal_pref_get_str(K_SUM, last, sizeof last);
    int rc = AOS_BACKUP_E_NONE;
    if (strcmp(sum, last)) {
        bool sealed = auto_key(stored, stored + SALT_LEN);
        rc = make(NULL, sealed ? stored : NULL, reason, name, sizeof name, why, why_len);
        memset(stored, 0, sizeof stored);
        if (rc == AOS_BACKUP_E_NONE) {
            aos_hal_pref_set_str(K_SUM, sum);
            if (made) *made = true;
        }
    } else if (why) snprintf(why, why_len, "nothing changed since the last copy");
    /* a day is done when its copy is made or was not needed; a card that
     * could not be written is asked again */
    int32_t day = today();
    if (rc == AOS_BACKUP_E_NONE && reason == AOS_BACKUP_DAILY && day) aos_hal_pref_set_i32(K_DAY, day);
    return rc;
}

int aos_backup_auto_now(int reason, bool *made, char *why, size_t why_len)
{
    return copy_if_changed(reason, false, made, why, why_len);
}

bool aos_backup_offer(aos_backup_info_t *out)
{
    int32_t seen = 0;
    if (aos_hal_pref_get_i32(K_SEEN, &seen) && seen) return false;
    if (!aos_hal_path_sd_root()) return false;
    /* a board that knows a network was set up by somebody: not a new one */
    if (aos_hal_net_ssid()[0]) { aos_hal_pref_set_i32(K_SEEN, 1); return false; }
    aos_backup_info_t one;
    if (aos_backup_list(&one, 1) < 1) return false;
    if (out) *out = one;
    return true;
}

void aos_backup_offer_done(void) { aos_hal_pref_set_i32(K_SEEN, 1); }

/* ---- to a pendrive and back (stage 3) --------------------------------------- */

/* What a backup does not carry is what the apps wrote on the card: notes,
 * saved networks, scripts, scores. A pendrive on the USB host takes them
 * file by file, beside the backups, and gives them back to a new card.
 *
 * Which folders: every folder at the card's root, but the ones the Tienda
 * fetches again and this module's own. The big ones that come from a
 * computer in the first place are off until a person turns them on. */
static const char *const NOT_OURS[] = { "apps", "web", "lang", "fonts", "firmware", AOS_BACKUP_DIR, "System Volume Information" };
static const char *const BIG[] = { "photos", "music", "videos", "maps", "recordings", "roms" };

static bool in_list(const char *name, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!strcasecmp(name, list[i])) return true;
    return false;
}

/* What a person chose for a folder: 1, 0, or -1 for nothing said. */
static int dir_choice(const char *dirs, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = dirs; p && *p; ) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == nl + 2 && !strncmp(p, name, nl) && p[nl] == '=') return p[nl + 1] == '1';
        p = end ? end + 1 : p + len;
    }
    return -1;
}

int aos_backup_folders(aos_backup_folder_t *out, int max)
{
    char d[FULL_MAX], f[FULL_MAX + 80], *dirs = BIG_ALLOC(1024);
    int n = 0;
    if (!aos_hal_path_sd_root() || !dirs) { free(dirs); return 0; }
    aos_hal_pref_get_str(K_DIRS, dirs, 1024);
    DIR *dir = opendir(full(d, ""));
    struct dirent *de;
    while (dir && (de = readdir(dir)) != NULL && n < max) {
        struct stat st;
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof out[0].name || strchr(de->d_name, ',') || strchr(de->d_name, '=') ||
            in_list(de->d_name, NOT_OURS, sizeof NOT_OURS / sizeof NOT_OURS[0])) continue;
        snprintf(f, sizeof f, "%s/%s", d, de->d_name);
        if (stat(f, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        int said = dir_choice(dirs, de->d_name);
        /* in the order of their names: the card lists them as they were made */
        int at = n;
        while (at > 0 && strcasecmp(out[at - 1].name, de->d_name) > 0) at--;
        memmove(&out[at + 1], &out[at], sizeof *out * (size_t)(n - at));
        snprintf(out[at].name, sizeof out[at].name, "%s", de->d_name);
        out[at].on = said >= 0 ? said == 1 : !in_list(de->d_name, BIG, sizeof BIG / sizeof BIG[0]);
        n++;
    }
    if (dir) closedir(dir);
    free(dirs);
    return n;
}

void aos_backup_folder_set(const char *name, bool on)
{
    char *dirs = BIG_ALLOC(1024), *next = BIG_ALLOC(1100);
    size_t nl = strlen(name), o = 0;
    if (dirs && next && nl && nl < 40 && !strchr(name, ',') && !strchr(name, '=')) {
        aos_hal_pref_get_str(K_DIRS, dirs, 1024);
        for (const char *p = dirs; *p; ) {      /* the others, as they were */
            const char *end = strchr(p, ',');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            bool this_one = len == nl + 2 && !strncmp(p, name, nl) && p[nl] == '=';
            if (!this_one && len && o + len + 2 < 1000) { if (o) next[o++] = ','; memcpy(next + o, p, len); o += len; }
            p = end ? end + 1 : p + len;
        }
        if (o + nl + 4 < 1000) o += (size_t)snprintf(next + o, 1100 - o, "%s%s=%d", o ? "," : "", name, on ? 1 : 0);
        next[o] = 0;
        aos_hal_pref_set_str(K_DIRS, next);
    }
    free(dirs); free(next);
}

static volatile bool s_cancel;
static volatile uint32_t s_copy_files, s_copy_kb, s_copy_skipped;

/* One file, unless the other side has it already or has a newer one: a
 * file that was edited after the copy is never replaced by the copy. Of the
 * same date (a board with no clock dates them all alike), the size says
 * whether it is the same. It is written beside its name and renamed, so a
 * drive pulled out in the middle leaves no half file under a good name. */
static bool copy_file(const char *src, const char *dst, const struct stat *ss, uint8_t *buf)
{
    struct stat ds;
    if (stat(dst, &ds) == 0 && (ds.st_mtime > ss->st_mtime || (ds.st_mtime == ss->st_mtime && ds.st_size == ss->st_size))) {
        s_copy_skipped++;
        return true;
    }
    char *tmp = BIG_ALLOC(strlen(dst) + 8);
    if (!tmp) return false;
    sprintf(tmp, "%s.p4t", dst);
    FILE *in = fopen(src, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
    bool ok = in && out;
    size_t got;
    while (ok && !s_cancel && (got = fread(buf, 1, COPY_CHUNK, in)) > 0) {
        ok = fwrite(buf, 1, got, out) == got;
        s_copy_kb += (uint32_t)((got + 1023) / 1024);
    }
    if (in) { ok = ok && !ferror(in); fclose(in); }
    if (out) ok = fclose(out) == 0 && ok;
    ok = ok && !s_cancel;
    if (ok) { remove(dst); ok = rename(tmp, dst) == 0; }
    if (!ok) remove(tmp);
    else s_copy_files++;
    free(tmp);
    return ok;
}

/* A folder and what is under it. Paths on the heap: this calls itself. */
static bool copy_tree(const char *src, const char *dst, uint8_t *buf, int depth)
{
    DIR *dir = opendir(src);
    if (!dir) return true;              /* not there: nothing to copy */
    mkdir(dst, 0775);
    size_t cap = strlen(src) + strlen(dst) + 2 * 260 + 4;
    char *a = BIG_ALLOC(cap), *b = a ? a + cap / 2 : NULL;
    bool ok = a != NULL;
    struct dirent *de;
    while (ok && !s_cancel && (de = readdir(dir)) != NULL) {
        struct stat st;
        size_t nl = strlen(de->d_name);
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || nl > 255) continue;
        if (nl > 4 && !strcmp(de->d_name + nl - 4, ".p4t")) continue;      /* a copy that was cut */
        snprintf(a, cap / 2, "%s/%s", src, de->d_name);
        snprintf(b, cap / 2, "%s/%s", dst, de->d_name);
        if (stat(a, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < COPY_DEPTH) ok = copy_tree(a, b, buf, depth + 1); }
        else ok = copy_file(a, b, &st, buf);
    }
    closedir(dir);
    free(a);
    return ok;
}

/* The pendrive there is: the first one mounted. */
static bool drive_root(char out[FULL_MAX])
{
    aos_usb_host_info_t info;
    if (!aos_hal_usb_host_info(&info) || !info.mounted || !info.path[0]) return false;
    snprintf(out, FULL_MAX, "%s/" DRIVE_DIR, info.path);
    return true;
}

bool aos_backup_drive(void)
{
    char root[FULL_MAX];
    return drive_root(root);
}

int aos_backup_copy_now(bool to_drive, char *why, size_t why_len)
{
    err_t e = { "", 0 };
    char root[FULL_MAX], card[FULL_MAX], *a = BIG_ALLOC(2 * 400), *b = a ? a + 400 : NULL;
    uint8_t *buf = BIG_ALLOC(COPY_CHUNK);
    aos_backup_folder_t *dirs = BIG_ALLOC(sizeof *dirs * AOS_BACKUP_FOLDERS_MAX);
    s_cancel = false;
    s_copy_files = s_copy_kb = s_copy_skipped = 0;
    if (!a || !buf || !dirs) { fail(&e, AOS_BACKUP_E_MEMORY, "no memory"); goto out; }
    if (!aos_hal_path_sd_root()) { fail(&e, AOS_BACKUP_E_CARD, "no card"); goto out; }
    if (!drive_root(root)) { fail(&e, AOS_BACKUP_E_DRIVE, "no pendrive"); goto out; }
    full(card, "");
    bool ok = true;
    if (to_drive) {
        /* what the board holds now goes too: a copy when something changed
         * since the last one, as the day's would be */
        char w2[120];
        copy_if_changed(AOS_BACKUP_DAILY, true, NULL, w2, sizeof w2);
        if (mkdir(root, 0775) != 0 && errno != EEXIST) { fail(&e, AOS_BACKUP_E_DRIVE, "the pendrive cannot be written"); goto out; }
        snprintf(a, 400, "%s/" AOS_BACKUP_DIR, card);
        snprintf(b, 400, "%s/" AOS_BACKUP_DIR, root);
        ok = copy_tree(a, b, buf, 0);
        snprintf(b, 400, "%s/card", root);
        mkdir(b, 0775);
        int n = aos_backup_folders(dirs, AOS_BACKUP_FOLDERS_MAX);
        for (int i = 0; ok && i < n; i++) {
            if (!dirs[i].on) continue;
            snprintf(a, 400, "%s/%s", card, dirs[i].name);
            snprintf(b, 400, "%s/card/%s", root, dirs[i].name);
            ok = copy_tree(a, b, buf, 0);
        }
    } else {
        struct stat st;
        if (stat(root, &st) != 0) { fail(&e, AOS_BACKUP_E_NOT_FOUND, "this pendrive has no copy of a board"); goto out; }
        snprintf(a, 400, "%s/" AOS_BACKUP_DIR, root);
        snprintf(b, 400, "%s/" AOS_BACKUP_DIR, card);
        ok = copy_tree(a, b, buf, 0);
        /* every folder the drive has, whatever is chosen here: a new card
         * has nothing to choose from yet. Never the ones that are not data. */
        snprintf(a, 400, "%s/card", root);
        DIR *dir = opendir(a);
        struct dirent *de;
        while (ok && dir && (de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof dirs[0].name ||
                in_list(de->d_name, NOT_OURS, sizeof NOT_OURS / sizeof NOT_OURS[0])) continue;
            snprintf(a, 400, "%s/card/%s", root, de->d_name);
            snprintf(b, 400, "%s/%s", card, de->d_name);
            if (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) ok = copy_tree(a, b, buf, 0);
        }
        if (dir) closedir(dir);
    }
    if (s_cancel) fail(&e, AOS_BACKUP_E_OTHER, "stopped: %u files copied", (unsigned)s_copy_files);
    else if (!ok) fail(&e, to_drive ? AOS_BACKUP_E_DRIVE : AOS_BACKUP_E_CARD, "a file could not be written, after %u (is it full?)", (unsigned)s_copy_files);
    else snprintf(e.why, sizeof e.why, "%u files copied %s, %u KB; %u were there already", (unsigned)s_copy_files,
                  to_drive ? "to the pendrive" : "to the card", (unsigned)s_copy_kb, (unsigned)s_copy_skipped);
out:
    free(a); free(buf); free(dirs);
    if (why) snprintf(why, why_len, "%s", e.why);
    return e.kind;
}

void aos_backup_cancel(void) { s_cancel = true; }

/* ---- to a server of the person's own (stage 4) ------------------------------ */

/* The key the board logs in with: the SSH client's, made the first time
 * either of the two needs it. One at a time: the portal's thread asks for
 * its line while a job of this module logs in, and two that found none
 * would each make one, the first to be written over by the second after
 * its line was shown. */
static void lock(void);
static void unlock(void);

static bool board_seed(uint8_t seed[32])
{
    char b64[64] = "";
    size_t len = 0;
    lock();
    bool ok = aos_hal_pref_get_str(K_SSH_SEED, b64, sizeof b64) &&
              mbedtls_base64_decode(seed, 32, &len, (const unsigned char *)b64, strlen(b64)) == 0 && len == 32;
    if (!ok) {
        aos_hal_random(seed, 32);
        ok = mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &len, seed, 32) == 0;
        if (ok) { b64[len] = 0; ok = aos_hal_pref_set_str(K_SSH_SEED, b64); }
        if (ok) aos_hal_log(TAG, "made the board's SSH key");
    }
    unlock();
    memset(b64, 0, sizeof b64);
    return ok;
}

bool aos_backup_server_key_line(char *out, size_t n)
{
    uint8_t seed[32];
    char who[64];
    if (!board_seed(seed)) return false;
    snprintf(who, sizeof who, "p4os@%.40s", aos_hal_device_name());
    bool ok = aos_sshc_key_line(seed, who, out, n);
    memset(seed, 0, sizeof seed);
    return ok;
}

bool aos_backup_server_key_new(void)
{
    /* the old one is gone for good: whoever let it in lets nobody in now.
     * The SSH app reads the same preference, and writes its
     * id_ed25519.pub again the next time it shows the key. */
    uint8_t seed[32];
    lock();
    bool gone = aos_hal_pref_set_str(K_SSH_SEED, "");
    unlock();
    if (!gone) return false;
    bool ok = board_seed(seed);
    memset(seed, 0, sizeof seed);
    return ok;
}

void aos_backup_server_get(aos_backup_server_t *out)
{
    char *hk = BIG_ALLOC(HK_MAX);
    int32_t v = 0;
    memset(out, 0, sizeof *out);
    aos_hal_pref_get_str(K_SRV_HOST, out->host, sizeof out->host);
    aos_hal_pref_get_str(K_SRV_USER, out->user, sizeof out->user);
    if (!aos_hal_pref_get_str(K_SRV_DIR, out->dir, sizeof out->dir) || !out->dir[0]) snprintf(out->dir, sizeof out->dir, SRV_DIR_DEF);
    out->port = aos_hal_pref_get_i32(K_SRV_PORT, &v) && v > 0 && v < 65536 ? v : 22;
    v = 0;
    out->on = aos_hal_pref_get_i32(K_SRV_ON, &v) && v != 0;
    v = 0;
    aos_hal_pref_get_i32(K_SRV_LAST, &v);
    out->last = (uint32_t)v;
    if (hk && aos_hal_pref_get_str(K_SRV_HK, hk, HK_MAX) && hk[0]) out->pinned = aos_sshc_fingerprint_of(hk, out->fp, sizeof out->fp);
    free(hk);
}

void aos_backup_server_set(const char *host, int port, const char *user, const char *dir)
{
    aos_backup_server_t was;
    aos_backup_server_get(&was);
    if (port <= 0 || port > 65535) port = 22;
    /* another server, another key: the one kept was the other's */
    if (strcmp(was.host, host ? host : "") || was.port != port) aos_hal_pref_set_str(K_SRV_HK, "");
    aos_hal_pref_set_str(K_SRV_HOST, host ? host : "");
    aos_hal_pref_set_i32(K_SRV_PORT, port);
    aos_hal_pref_set_str(K_SRV_USER, user ? user : "");
    aos_hal_pref_set_str(K_SRV_DIR, dir && dir[0] ? dir : SRV_DIR_DEF);
}

void aos_backup_server_on(bool on) { aos_hal_pref_set_i32(K_SRV_ON, on ? 1 : 0); }

static int kind_of_ssh(int err)
{
    return err == AOS_SSHC_OK ? AOS_BACKUP_E_NONE : err == AOS_SSHC_E_HOSTKEY ? AOS_BACKUP_E_HOSTKEY :
           err == AOS_SSHC_E_AUTH ? AOS_BACKUP_E_LOGIN : err == AOS_SSHC_E_MEMORY ? AOS_BACKUP_E_MEMORY :
           err == AOS_SSHC_E_LOCAL ? AOS_BACKUP_E_CARD : AOS_BACKUP_E_SERVER;
}

/* The key the last look saw, until the person accepts it or not. */
static char *s_seen;
AOS_BSS_PSRAM static char s_seen_fp[80];        /* PSRAM: read by the screen, never from an interrupt */
static bool s_seen_in;

int aos_backup_server_look_now(char *why, size_t why_len)
{
    aos_backup_server_t sv;
    uint8_t seed[32];
    int err = 0;
    aos_backup_server_get(&sv);
    if (!s_seen) s_seen = BIG_ALLOC(HK_MAX);
    if (!s_seen) { snprintf(why, why_len, "no memory"); return AOS_BACKUP_E_MEMORY; }
    s_seen[0] = 0; s_seen_fp[0] = 0; s_seen_in = false;
    if (!sv.host[0] || !sv.user[0]) { snprintf(why, why_len, "no server set"); return AOS_BACKUP_E_SETUP; }
    if (!board_seed(seed)) { snprintf(why, why_len, "the board's key could not be kept"); return AOS_BACKUP_E_OTHER; }
    /* with no key kept: this is the look a person takes at whichever comes */
    aos_sshc_opts_t o = { sv.host, sv.port, sv.user, seed, NULL, &s_cancel };
    s_cancel = false;
    aos_sshc_t *c = aos_sshc_open(&o, &err, why, why_len, s_seen);
    memset(seed, 0, sizeof seed);
    if (s_seen[0]) aos_sshc_fingerprint_of(s_seen, s_seen_fp, sizeof s_seen_fp);
    if (c) {
        s_seen_in = true;
        snprintf(why, why_len, "%.60s shows %.70s, and lets the board in", sv.host, s_seen_fp);
        aos_sshc_close(c);
        return AOS_BACKUP_E_NONE;
    }
    return kind_of_ssh(err);
}

bool aos_backup_server_seen(char *fp, size_t n, bool *in)
{
    if (!s_seen || !s_seen[0]) return false;
    if (fp) snprintf(fp, n, "%s", s_seen_fp);
    if (in) *in = s_seen_in;
    return true;
}

bool aos_backup_server_trust(void)
{
    if (!s_seen || !s_seen[0]) return false;
    return aos_hal_pref_set_str(K_SRV_HK, s_seen);
}

int aos_backup_server_send_now(char *why, size_t why_len)
{
    err_t e = { "", 0 };
    aos_backup_server_t sv;
    uint8_t seed[32], stored[SALT_LEN + 32];
    char *hk = BIG_ALLOC(HK_MAX), *a = BIG_ALLOC(2 * 400), *b = a ? a + 400 : NULL;
    aos_backup_info_t *all = BIG_ALLOC(sizeof *all * PRUNE_MAX);
    aos_sshc_t *c = NULL;
    int err = 0, sent = 0, there = 0;
    aos_backup_server_get(&sv);
    if (!hk || !a || !all) { fail(&e, AOS_BACKUP_E_MEMORY, "no memory"); goto out; }
    if (!aos_hal_path_sd_root()) { fail(&e, AOS_BACKUP_E_CARD, "no card"); goto out; }
    if (!sv.host[0] || !sv.user[0]) { fail(&e, AOS_BACKUP_E_SETUP, "no server set"); goto out; }
    if (!aos_hal_pref_get_str(K_SRV_HK, hk, HK_MAX) || !hk[0]) { fail(&e, AOS_BACKUP_E_SETUP, "the server's key was not accepted at the board yet"); goto out; }
    /* nothing plain leaves the board: the copies that go are the sealed
     * ones, and there are none until a passphrase was chosen for them */
    bool sealed = auto_key(stored, stored + SALT_LEN);
    memset(stored, 0, sizeof stored);
    if (!sealed) { fail(&e, AOS_BACKUP_E_SETUP, "no passphrase for the automatic copies: nothing sealed to send"); goto out; }
    char w2[120];
    copy_if_changed(AOS_BACKUP_DAILY, true, NULL, w2, sizeof w2);

    if (!board_seed(seed)) { fail(&e, AOS_BACKUP_E_OTHER, "the board's key could not be read"); goto out; }
    aos_sshc_opts_t o = { sv.host, sv.port, sv.user, seed, hk, &s_cancel };
    s_cancel = false;
    c = aos_sshc_open(&o, &err, e.why, sizeof e.why, NULL);
    memset(seed, 0, sizeof seed);
    if (!c) { e.kind = kind_of_ssh(err); goto out; }

    /* <folder>/<the board's name>: several boards can share a server */
    char board[48];
    snprintf(board, sizeof board, "%.40s", aos_hal_device_name());
    for (char *p = board; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') *p = '_';
    snprintf(a, 400, "%s/%s", sv.dir, board[0] ? board : "p4os");
    err = aos_sshc_sftp(c);
    if (!err) err = aos_sshc_mkdirs(c, a);
    int n = err ? 0 : aos_backup_list(all, PRUNE_MAX);
    for (int i = n - 1; i >= 0 && !err; i--) {          /* the oldest first: a cut leaves the older ones whole */
        bool is = false;
        uint64_t size = 0;
        if (!all[i].sealed) continue;
        snprintf(a, 400, "%s/%s/%s", sv.dir, board[0] ? board : "p4os", all[i].name);
        err = aos_sshc_stat(c, a, &is, &size);
        if (err) break;
        if (is && size == all[i].size) { there++; continue; }
        char rel[160], f[FULL_MAX];
        snprintf(rel, sizeof rel, AOS_BACKUP_DIR "/%s", all[i].name);
        snprintf(b, 400, "%s", full(f, rel));
        err = aos_sshc_put(c, b, a);
        if (!err) sent++;
    }
    if (err) { fail(&e, kind_of_ssh(err), "%.110s", aos_sshc_why(c)); goto out; }
    if (aos_hal_time_is_valid()) aos_hal_pref_set_i32(K_SRV_LAST, (int32_t)time(NULL));
    snprintf(e.why, sizeof e.why, "%d sealed backups sent to %.50s; %d were there already", sent, sv.host, there);
out:
    aos_sshc_close(c);
    free(hk); free(a); free(all);
    if (why) snprintf(why, why_len, "%s", e.why);
    return e.kind;
}

/* ---- the thread ------------------------------------------------------------- */

enum { JOB_MAKE, JOB_RESTORE, JOB_DAILY, JOB_FW, JOB_KEY, JOB_TO_DRIVE, JOB_FROM_DRIVE, JOB_LOOK, JOB_SEND };
static const char *const JOB_NAME[] = { "make", "restore", "auto", "auto", "key", "to_drive", "from_drive", "look", "send" };

static void *s_lock;
static aos_backup_status_t s_st;
static struct { int kind; char name[AOS_BACKUP_NAME_MAX]; char pass[96]; } s_job;

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
    int written = 0, job = s_job.kind, kind = AOS_BACKUP_E_NONE;
    bool quiet = false;
    switch (job) {
    case JOB_RESTORE: kind = aos_backup_restore_now(s_job.name, s_job.pass, &written, why, sizeof why); break;
    case JOB_MAKE:    kind = aos_backup_make_now(s_job.pass, false, name, sizeof name, why, sizeof why); break;
    case JOB_TO_DRIVE:
    case JOB_FROM_DRIVE: kind = aos_backup_copy_now(job == JOB_TO_DRIVE, why, sizeof why); break;
    case JOB_LOOK:       kind = aos_backup_server_look_now(why, sizeof why); break;
    case JOB_SEND:       kind = aos_backup_server_send_now(why, sizeof why); break;
    case JOB_KEY:
        kind = aos_backup_auto_passphrase_now(s_job.pass) ? AOS_BACKUP_E_NONE : AOS_BACKUP_E_OTHER;
        snprintf(why, sizeof why, "%s", kind ? "the key could not be kept" : "the automatic copies are sealed from now on");
        /* the first sealed copy now, not tomorrow: what the card has until
         * then is the plain ones */
        if (!kind) {
            char w2[120];
            bool made = false;
            aos_hal_pref_set_str(K_SUM, "");
            aos_backup_auto_now(AOS_BACKUP_DAILY, &made, w2, sizeof w2);
            if (made) snprintf(why, sizeof why, "%s", w2);
        }
        break;
    default: {
        bool made = false;
        kind = aos_backup_auto_now(job == JOB_FW ? AOS_BACKUP_BEFORE_FW : AOS_BACKUP_DAILY, &made, why, sizeof why);
        quiet = !made && kind == AOS_BACKUP_E_NONE;      /* nothing to say: no copy was needed */
        break;
    }
    }
    memset(s_job.pass, 0, sizeof s_job.pass);
    aos_hal_log(TAG, "%s: %s", JOB_NAME[job], why);
    lock();
    s_st.busy = false;
    if (!quiet) {
        s_st.jobs_done++;
        s_st.last_ok = kind == AOS_BACKUP_E_NONE;
        s_st.last_kind = kind;
        snprintf(s_st.last_job, sizeof s_st.last_job, "%s", JOB_NAME[job]);
        snprintf(s_st.last, sizeof s_st.last, "%s", why);
        snprintf(s_st.name, sizeof s_st.name, "%s", job == JOB_RESTORE ? s_job.name : name);
        s_st.written = written;
        s_st.files = s_copy_files;
        s_st.kbytes = s_copy_kb;
        if (job == JOB_RESTORE && kind == AOS_BACKUP_E_NONE) s_st.restart = true;
    }
    unlock();
}

static bool start(int job, const char *name, const char *pass)
{
    lock();
    bool ok = !s_st.busy;
    if (ok) {
        s_job.kind = job;
        snprintf(s_job.name, sizeof s_job.name, "%s", name ? name : "");
        snprintf(s_job.pass, sizeof s_job.pass, "%s", pass ? pass : "");
        s_st.busy = true;
        snprintf(s_st.job, sizeof s_st.job, "%s", JOB_NAME[job]);
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

bool aos_backup_make(const char *passphrase) { return start(JOB_MAKE, NULL, passphrase); }

bool aos_backup_restore(const char *name, const char *passphrase)
{
    return name_ok(name) && start(JOB_RESTORE, name, passphrase);
}

bool aos_backup_server_look(void) { return start(JOB_LOOK, NULL, NULL); }
bool aos_backup_server_send(void) { return start(JOB_SEND, NULL, NULL); }
bool aos_backup_to_drive(void) { return start(JOB_TO_DRIVE, NULL, NULL); }
bool aos_backup_from_drive(void) { return start(JOB_FROM_DRIVE, NULL, NULL); }

bool aos_backup_auto_passphrase(const char *passphrase)
{
    /* taking it off is one write; setting it is PBKDF2's four seconds */
    if (!passphrase || !passphrase[0]) return aos_backup_auto_passphrase_now(NULL);
    return start(JOB_KEY, NULL, passphrase);
}

static bool busy(void)
{
    lock();
    bool b = s_st.busy;
    unlock();
    return b;
}

void aos_backup_before_firmware(void)
{
    if (!aos_backup_auto_on() || !aos_hal_path_sd_root()) return;
    /* in this module's thread, which has the stack for it, and waited for:
     * the image is not written until the copy is on the card. Half a
     * second; ten at most, and then the update goes on without it. */
    uint64_t until = aos_hal_uptime_ms() + 10000;
    while (busy() && aos_hal_uptime_ms() < until) aos_hal_sleep_ms(50);
    if (!start(JOB_FW, NULL, NULL)) return;
    while (busy() && aos_hal_uptime_ms() < until) aos_hal_sleep_ms(50);
}

static void tick_server(void);

void aos_backup_tick(void)
{
    /* called five times a second: a look a minute, and not in the first two
     * of a start, when the card and the clock may not be there yet */
    static uint32_t calls, wait;
    tick_server();
    if (++calls % 300 || aos_hal_uptime_ms() < 120000) return;
    if (wait) { wait--; return; }
    int32_t day = today(), last = 0;
    if (!day || !aos_backup_auto_on() || !aos_hal_path_sd_root()) return;
    aos_hal_pref_get_i32(K_DAY, &last);
    if (last == day) return;
    if (aos_backup_offer(NULL)) return;         /* a new board with a backup to look at first */
    if (start(JOB_DAILY, NULL, NULL)) wait = 60;        /* a card that fails is asked again in an hour */
}

/* The week's copy to the server, when one is set, its key accepted, the
 * copies sealed and the network up. Looked at once a minute, apart from the
 * day's copy; a server that is not there is asked again in an hour. */
static void tick_server(void)
{
    static uint32_t calls, wait;
    if (++calls % 300 != 150 || aos_hal_uptime_ms() < 180000) return;
    if (wait) { wait--; return; }
    if (!aos_hal_time_is_valid() || !aos_hal_path_sd_root() || aos_hal_net_state() != AOS_NET_CONNECTED) return;
    aos_backup_server_t sv;
    aos_backup_server_get(&sv);
    if (!sv.on || !sv.pinned || !sv.host[0] || !aos_backup_auto_sealed()) return;
    uint32_t now = (uint32_t)time(NULL);
    if (sv.last && now >= sv.last && now - sv.last < SRV_EVERY_S) return;
    if (start(JOB_SEND, NULL, NULL)) wait = 60;
}

void aos_backup_status(aos_backup_status_t *out)
{
    lock();
    *out = s_st;
    unlock();
    /* a copy in hand: how far it is */
    if (out->busy && (!strcmp(out->job, "to_drive") || !strcmp(out->job, "from_drive"))) {
        out->files = s_copy_files;
        out->kbytes = s_copy_kb;
    }
}
