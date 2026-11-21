/*
 * P4OS - The Tienda's service. See aos_store.h and docs/plan/TIENDA.md.
 *
 * An install, in order (the same steps as tools/store_cli.py, whose header
 * spells them out):
 *
 *  1. The catalogue's line against this firmware: the ABI and fw_min.
 *  2. The package downloaded to apps/.store/tmp, its size and SHA-256 the
 *     catalogue's (aos_fetch does both).
 *  3. The package opened: a zip without compression, a p4app.json that
 *     agrees with the catalogue, every path one a package may write and not
 *     another package's, every file copied out to apps/.store/tmp with the
 *     hash p4app.json gives it. On the board, every .so asked whether it
 *     would load here (aos_dynapp_check), without loading it.
 *  4. The packs it needs and the extras asked for, to apps/.store/tmp too;
 *     a part the card already has with the right hash is not downloaded.
 *  Nothing of the installed app has been touched so far: any failure up to
 *  here leaves the card as it was.
 *  5. The app off the launcher, the files moved to their places, the old
 *     version's files and packs that the new one does not have deleted.
 *  6. The receipt, written last. Then the app registered again.
 *
 * Memory: the catalogue's text, the tables, the block and the thread's stack
 * are PSRAM. cJSON's nodes are the C library's malloc: a line of the
 * catalogue or a receipt at a time, a few kilobytes for a moment, never the
 * whole catalogue as a tree.
 */
#include "aos_store.h"
#include "aos_fetch.h"
#include "aos_hal.h"
#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef AOS_STORE_ABI
  #include "aos_app.h"
  #define AOS_STORE_ABI AOS_ABI_VERSION
#endif

#ifdef AOS_SIM
  /* the simulator's network is the Mac's: there is no "away" to tell from home */
  static bool net_trusted(void) { return true; }
  #define BIG_ALLOC(n)  calloc(1, (n))
  /* the simulator links its apps in: nothing to check, load or unload */
  static int  dyn_check(const char *path, char *missing, size_t n) { (void)path; if (missing && n) missing[0] = 0; return 0; }
  static void dyn_unload(const char *so) { (void)so; }
  static bool dyn_load(const char *so) { (void)so; return true; }
#else
  #include "esp_heap_caps.h"
  #include "aos_dynapp.h"
  #include "aos_access.h"
  static bool net_trusted(void) { return aos_access_here_trusted(); }
  #define BIG_ALLOC(n)  heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
  static int  dyn_check(const char *path, char *missing, size_t n) { return aos_dynapp_check(path, missing, n); }
  static void dyn_unload(const char *so) { aos_dynapp_unload_file(so); }
  static bool dyn_load(const char *so) { return aos_dynapp_load(so); }
#endif

#define TAG             "store"
#define SCHEMA          1
#define DIR_STORE       "apps/.store"
#define DIR_TMP         "apps/.store/tmp"
#define DIR_IN          "apps/.store/in"        /* a package handed in, until its turn */
#define PROBE_FILE      "apps/.store/probe.ndjson"
#define SRC_MAX         AOS_STORE_SOURCES_MAX
#define CATALOG_FILE    "apps/.store/catalog.ndjson"
#define CATALOG_MAX     (1024 * 1024)
#define APPS_MAX        256
#define JSON_MAX        (128 * 1024)    /* a receipt or a p4app.json */
#define ZIP_ENTRIES     96
#define PATH_MAX_       160             /* a path of the card, from its root */
#define FULL_MAX        224             /* with the mount point in front */
#define BLOCK           (64 * 1024)
#define QUEUE           8
#define STACK           (12 * 1024)

/* ---- state ----------------------------------------------------------------- */

typedef struct {
    aos_store_app_t pub;
    char pkg_sha[65];
    char so_sha[65];
} app_t;

enum { J_REFRESH = 1, J_INSTALL, J_REMOVE, J_ADOPT, J_PROBE, J_ADD, J_UNSOURCE, J_SIDELOAD };

typedef struct {
    int  kind;
    char file[32];
    char packs[208];        /* the packs asked for; or the job's own argument: an address, a path */
} job_t;

/* A source: an address a catalogue is read from. The first is the official
 * one; the others are the person's, added at the board (aos_store.h). */
typedef struct {
    char url[200];
    char name[40], home[160], base[200], release[28];
    int  apps, skipped;
    bool loaded;
} src_t;

static void   *s_lock;
static bool    s_inited;
static char   *s_cat;               /* the catalogue's text */
static size_t  s_cat_len;
static app_t  *s_apps;
static int     s_n;
static src_t   s_src[SRC_MAX];
static int     s_nsrc = 1;
static char    s_url[200];          /* the first source's address when not the official one */
static bool    s_dev;               /* developer mode: http:// and another first source */
static aos_store_probe_t s_probe;
static job_t   s_queue[QUEUE];
static int     s_qn;
static bool    s_running, s_cancel;
static aos_store_status_t s_st;
static int     s_shot_nbad;         /* pictures that could not be had (below) */

static void lock(void)   { aos_hal_mutex_lock(s_lock); }
static void unlock(void) { aos_hal_mutex_unlock(s_lock); }

/* ---- small things ---------------------------------------------------------- */

typedef struct { char why[160]; } err_t;

static bool fail(err_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool fail(err_t *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof e->why, fmt, ap);
    va_end(ap);
    return false;
}

static void step(const char *what)
{
    lock();
    snprintf(s_st.step, sizeof s_st.step, "%s", what);
    s_st.got = s_st.total = 0;
    unlock();
}

/* A path of the card as one of the file system. */
static const char *full(char out[FULL_MAX], const char *rel)
{
    const char *root = aos_hal_path_sd_root();
    snprintf(out, FULL_MAX, "%s/%s", root ? root : ".", rel);
    return out;
}

static void make_dirs(const char *rel)
{
    char f[FULL_MAX];
    full(f, rel);
    const char *root = aos_hal_path_sd_root();
    for (char *p = f + (root ? strlen(root) : 1) + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(f, 0755);
            *p = '/';
        }
    }
}

static bool exists(const char *rel, uint32_t *size)
{
    char f[FULL_MAX];
    struct stat st;
    if (stat(full(f, rel), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (size) *size = (uint32_t)st.st_size;
    return true;
}

static void del(const char *rel)
{
    char f[FULL_MAX];
    remove(full(f, rel));
}

static bool move(const char *from, const char *to)
{
    char a[FULL_MAX], b[FULL_MAX];
    make_dirs(to);
    remove(full(b, to));            /* FAT's rename does not replace */
    return rename(full(a, from), b) == 0;
}

static void hex(const uint8_t in[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
}

/* The SHA-256 of bytes [off, off+size) of a file, or of all of it with
 * size 0 and off 0. 'copy_to', when not NULL, gets the same bytes. */
static bool hash_range(const char *rel, uint32_t off, uint32_t size, bool whole, const char *copy_to, char out[65])
{
    char f[FULL_MAX];
    int in = open(full(f, rel), O_RDONLY);
    if (in < 0) return false;
    int dst = -1;
    if (copy_to) {
        make_dirs(copy_to);
        dst = open(full(f, copy_to), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst < 0) { close(in); return false; }
    }
    uint8_t *buf = aos_hal_io_alloc(BLOCK);
    aos_hal_sha256_ctx_t *h = buf ? aos_hal_sha256_begin() : NULL;
    bool ok = h != NULL && lseek(in, (off_t)off, SEEK_SET) == (off_t)off;
    uint32_t left = size;
    while (ok && (whole || left)) {
        int want = whole || left > BLOCK ? BLOCK : (int)left;
        int n = (int)read(in, buf, (size_t)want);
        if (n < 0 || (n == 0 && !whole)) { ok = false; break; }
        if (n == 0) break;
        aos_hal_sha256_update(h, buf, (size_t)n);
        if (dst >= 0 && write(dst, buf, (size_t)n) != n) { ok = false; break; }
        left -= whole ? 0 : (uint32_t)n;
    }
    uint8_t d[32];
    if (h) aos_hal_sha256_end(h, d);
    if (ok) hex(d, out);
    if (buf) aos_hal_io_free(buf);
    close(in);
    if (dst >= 0) close(dst);
    return ok;
}

static bool file_sha(const char *rel, char out[65])
{
    return hash_range(rel, 0, 0, true, NULL, out);
}

/* A whole small file, NUL-terminated, in PSRAM; NULL when it is not there
 * or is larger than max. */
static char *slurp(const char *rel, size_t max, size_t *len)
{
    char f[FULL_MAX];
    FILE *fp = fopen(full(f, rel), "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (n >= 0 && (size_t)n <= max) ? BIG_ALLOC((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); buf = NULL; }
    fclose(fp);
    if (buf && len) *len = (size_t)n;
    return buf;
}

static bool spit(const char *rel, const char *text)
{
    char tmp[PATH_MAX_ + 8], f[FULL_MAX];
    snprintf(tmp, sizeof tmp, "%.*s.part", PATH_MAX_ - 1, rel);
    make_dirs(tmp);
    FILE *fp = fopen(full(f, tmp), "wb");
    if (!fp) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, fp) == n;
    ok = fclose(fp) == 0 && ok;
    return ok && move(tmp, rel);
}

static const char *jstr(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
    return s ? s : "";
}

static uint32_t jnum(const cJSON *o, const char *key)
{
    const cJSON *n = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(n) && n->valuedouble >= 0 ? (uint32_t)n->valuedouble : 0;
}

/* "v0.29.0-5-gefef5a5", "0.31.0" -> 0x001D00 and such, to compare; -1 when
 * there is no version in it (the simulator's "sim"): not one to refuse by. */
static int32_t version_of(const char *v)
{
    while (*v && !isdigit((unsigned char)*v)) v++;
    int a = 0, b = 0, c = 0;
    if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return (a & 0xFF) << 16 | (b & 0xFF) << 8 | (c & 0xFF);
}

static bool fw_fits(const char *fw_min)
{
    int32_t have = version_of(aos_hal_firmware_version()), need = version_of(fw_min);
    return have < 0 || need < 0 || have >= need;
}

/* ---- what a package may write ---------------------------------------------- */

static bool name_ok(const char *s, size_t n, const char *also)
{
    if (!n) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr(also, s[i])) return false;
    return true;
}

static bool ends(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a > b && !strcmp(s + a - b, tail);
}

/* apps/<file>.so, apps/<folder>/<module>.so, web/<page>.js|.mjs,
 * lang/<code>/<name>.lang - and nothing else. */
static bool allowed(const char *path, const char *file)
{
    if (strlen(path) >= PATH_MAX_ - 8 || strstr(path, "..") || strchr(path, '\\') || strstr(path, "//")) return false;
    const char *a = strchr(path, '/');
    if (!a) return false;
    const char *b = strchr(a + 1, '/');
    if (b && strchr(b + 1, '/')) return false;                  /* three levels at most */
    if (!strncmp(path, "apps/", 5)) {
        if (!b) {
            char own[48];
            snprintf(own, sizeof own, "%.31s.so", file);
            return !strcmp(a + 1, own);
        }
        return a[1] != '.' && name_ok(a + 1, (size_t)(b - a - 1), "_-") && ends(b + 1, ".so") &&
               name_ok(b + 1, strlen(b + 1), "_-.");
    }
    if (!strncmp(path, "web/", 4))
        return !b && (ends(a + 1, ".js") || ends(a + 1, ".mjs")) && name_ok(a + 1, strlen(a + 1), "_-.");
    if (!strncmp(path, "lang/", 5))
        return b && b - a - 1 == 2 && islower((unsigned char)a[1]) && islower((unsigned char)a[2]) &&
               ends(b + 1, ".lang") && name_ok(b + 1, strlen(b + 1), "_-.");
    return false;
}

static bool pack_name_ok(const char *s)
{
    return ends(s, ".pak") && name_ok(s, strlen(s), "_.") && strlen(s) < 64 && s[0] != '.';
}

static bool file_name_ok(const char *s)
{
    return s[0] && strlen(s) < 32 && name_ok(s, strlen(s), "_-");
}

/* ---- receipts -------------------------------------------------------------- */

static cJSON *receipt_read(const char *file)
{
    char rel[PATH_MAX_];
    snprintf(rel, sizeof rel, DIR_STORE "/%.31s.json", file);
    char *text = slurp(rel, JSON_MAX, NULL);
    cJSON *r = text ? cJSON_Parse(text) : NULL;
    free(text);
    return r;
}

/* Whether 'path' is a file (or a pack's part) of a package other than
 * 'mine': its name goes to 'owner'. */
static bool owned_by_other(const char *path, const char *mine, char owner[32])
{
    char d[FULL_MAX];
    DIR *dir = opendir(full(d, DIR_STORE));
    if (!dir) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(dir)) != NULL) {
        if (!ends(de->d_name, ".json")) continue;
        char file[32];
        snprintf(file, sizeof file, "%.*s", (int)(strlen(de->d_name) - 5 < 31 ? strlen(de->d_name) - 5 : 31), de->d_name);
        if (!strcmp(file, mine)) continue;
        cJSON *r = receipt_read(file);
        const cJSON *f, *p, *part;
        cJSON_ArrayForEach(f, cJSON_GetObjectItem(r, "files"))
            if (!strcmp(jstr(f, "path"), path)) found = true;
        if (!strncmp(path, "apps/", 5))
            cJSON_ArrayForEach(p, cJSON_GetObjectItem(r, "packs"))
                cJSON_ArrayForEach(part, cJSON_GetObjectItem(p, "parts"))
                    if (!strcmp(jstr(part, "name"), path + 5)) found = true;
        if (found) snprintf(owner, 32, "%s", file);
        cJSON_Delete(r);
    }
    closedir(dir);
    return found;
}

/* ---- the catalogue --------------------------------------------------------- */

/* Every app's state from the receipts and the card, with the lock held. */
static void states(void)
{
    for (int i = 0; i < s_n; i++) {
        app_t *a = &s_apps[i];
        a->pub.have[0] = 0;
        char so[PATH_MAX_], off[PATH_MAX_];
        snprintf(so, sizeof so, "apps/%s.so", a->pub.file);
        snprintf(off, sizeof off, "apps/%s.so.off", a->pub.file);
        bool there = exists(so, NULL);
        cJSON *r = receipt_read(a->pub.file);
        if (r) {
            snprintf(a->pub.have, sizeof a->pub.have, "%s", jstr(r, "ver"));
            a->pub.state = !strcmp(jstr(r, "pkg_sha256"), a->pkg_sha) ? AOS_STORE_INSTALLED : AOS_STORE_UPDATE;
            cJSON_Delete(r);
        } else {
            a->pub.state = there ? AOS_STORE_BY_HAND : AOS_STORE_ABSENT;
        }
        /* the loader set it aside: whatever the receipt says, it is not running */
        if (!there && exists(off, NULL)) a->pub.state = AOS_STORE_SET_ASIDE;
    }
}

/* Where a source's catalogue is kept on the card. */
static void src_file(int i, char out[PATH_MAX_])
{
    if (i == 0) {
        snprintf(out, PATH_MAX_, "%s", CATALOG_FILE);
        return;
    }
    uint32_t h = 2166136261u;
    for (const char *c = s_src[i].url; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    snprintf(out, PATH_MAX_, DIR_STORE "/cat-%08x.ndjson", (unsigned)h);
}

/* Whether the file is a catalogue this firmware reads: its first line the
 * source's, of this schema, and every other line an object. How many apps
 * it lists, or -1 with why. 'source', when not NULL, gets the first line's
 * "source" printed (900 bytes). */
static int catalog_check(const char *rel, char *source, err_t *e)
{
    size_t len = 0;
    char *text = slurp(rel, CATALOG_MAX, &len);
    if (!text) { fail(e, "no catalogue there, or one over %d KB", CATALOG_MAX / 1024); return -1; }
    int n = -1;
    bool head = false, bad = false;
    for (size_t at = 0; at < len && !bad;) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        if (end > at) {
            cJSON *o = cJSON_ParseWithLength(text + at, end - at);
            if (!cJSON_IsObject(o)) { fail(e, "the catalogue does not read at byte %u", (unsigned)at); bad = true; }
            else if (!head) {
                head = true;
                n = 0;
                const cJSON *src = cJSON_GetObjectItem(o, "source");
                if (jnum(o, "schema") != SCHEMA) { fail(e, "a catalogue of another schema: update P4OS to read it"); bad = true; }
                else if (!jstr(src, "name")[0] || !jstr(o, "base")[0]) { fail(e, "a catalogue that does not say whose it is"); bad = true; }
                else if (source) {
                    char *js = cJSON_PrintUnformatted(src);
                    snprintf(source, 900, "%s", js ? js : "{}");
                    free(js);
                }
            } else n++;
            cJSON_Delete(o);
        }
        at = end + 1;
    }
    free(text);
    if (!bad && !head) { fail(e, "an empty catalogue"); bad = true; }
    return bad ? -1 : n;
}

/* Reads every source's catalogue from the card into one text: each source's
 * own first line, then the apps it lists that no source before it has. An
 * app's file and id are its own across sources - what the official source
 * lists cannot be replaced by another's, and between two others the first
 * added wins - and the one left out is said in the log. false, with why,
 * when no source has a catalogue yet; what was loaded stays. */
static bool catalog_load(err_t *e)
{
    char  *texts[SRC_MAX] = { 0 };
    size_t lens[SRC_MAX] = { 0 }, total = 0;
    char   rel[PATH_MAX_];
    int    nsrc;
    lock();
    nsrc = s_nsrc;
    unlock();
    for (int i = 0; i < nsrc; i++) {
        src_file(i, rel);
        texts[i] = slurp(rel, CATALOG_MAX, &lens[i]);
        total += lens[i] + 2;
    }
    app_t *apps = BIG_ALLOC(sizeof(app_t) * APPS_MAX);
    char *all = BIG_ALLOC(total + 1);
    src_t found[SRC_MAX];
    memset(found, 0, sizeof found);
    int n = 0, loaded = 0;
    size_t w = 0;
    for (int i = 0; i < nsrc && apps && all; i++) {
        bool head = false;
        for (size_t at = 0; texts[i] && at < lens[i];) {
            size_t end = at;
            while (end < lens[i] && texts[i][end] != '\n') end++;
            cJSON *o = end > at ? cJSON_ParseWithLength(texts[i] + at, end - at) : NULL;
            bool keep = false;
            if (o && !head) {
                if (jnum(o, "schema") != SCHEMA) { cJSON_Delete(o); break; }
                const cJSON *src = cJSON_GetObjectItem(o, "source");
                snprintf(found[i].name, sizeof found[i].name, "%s", jstr(src, "name"));
                snprintf(found[i].home, sizeof found[i].home, "%s", jstr(src, "url"));
                snprintf(found[i].base, sizeof found[i].base, "%s", jstr(o, "base"));
                snprintf(found[i].release, sizeof found[i].release, "%s", jstr(o, "release"));
                found[i].loaded = head = keep = true;
                loaded++;
            } else if (o && n < APPS_MAX && file_name_ok(jstr(o, "file"))) {
                const char *file = jstr(o, "file"), *id = jstr(o, "id");
                int theirs = -1;
                for (int k = 0; k < n && theirs < 0; k++)
                    if (!strcmp(apps[k].pub.file, file) || (id[0] && !strcmp(apps[k].pub.id, id))) theirs = apps[k].pub.src;
                if (theirs >= 0) {
                    found[i].skipped++;
                    aos_hal_log(TAG, "%s of %s is not offered: %s already lists it", file, found[i].name, found[theirs].name);
                } else {
                    app_t *a = &apps[n++];
                    const cJSON *pkg = cJSON_GetObjectItem(o, "pkg");
                    snprintf(a->pub.file, sizeof a->pub.file, "%s", file);
                    snprintf(a->pub.id, sizeof a->pub.id, "%s", id);
                    snprintf(a->pub.ver, sizeof a->pub.ver, "%s", jstr(o, "ver"));
                    snprintf(a->pub.fw_min, sizeof a->pub.fw_min, "%s", jstr(o, "fw_min"));
                    snprintf(a->pkg_sha, sizeof a->pkg_sha, "%s", jstr(pkg, "sha256"));
                    snprintf(a->so_sha, sizeof a->so_sha, "%s", jstr(pkg, "so_sha256"));
                    a->pub.fits = jnum(o, "abi") == AOS_STORE_ABI && fw_fits(a->pub.fw_min);
                    a->pub.src = i;
                    a->pub.line_off = (uint32_t)w;
                    a->pub.line_len = (uint32_t)(end - at);
                    found[i].apps++;
                    keep = true;
                }
            }
            if (keep) {
                memcpy(all + w, texts[i] + at, end - at);
                w += end - at;
                all[w++] = '\n';
            }
            cJSON_Delete(o);
            at = end + 1;
        }
    }
    for (int i = 0; i < nsrc; i++) free(texts[i]);
    if (!apps || !all || !loaded) {
        free(apps);
        free(all);
        return fail(e, apps && all ? "no catalogue yet" : "no memory for the catalogue");
    }
    all[w] = 0;

    lock();
    free(s_cat);
    free(s_apps);
    s_cat = all;
    s_cat_len = w;
    s_apps = apps;
    s_n = n;
    for (int i = 0; i < nsrc && i < s_nsrc; i++) {
        snprintf(found[i].url, sizeof found[i].url, "%.199s", s_src[i].url);
        s_src[i] = found[i];
    }
    snprintf(s_st.release, sizeof s_st.release, "%s", s_src[0].release);
    snprintf(s_st.source, sizeof s_st.source, "%s", s_src[0].name);
    s_st.apps = (uint32_t)n;
    s_shot_nbad = 0;
    states();
    unlock();
    return true;
}

/* The catalogue's line of a package, parsed; NULL when it is not there. */
static cJSON *entry_of(const char *file, app_t *copy)
{
    cJSON *o = NULL;
    lock();
    for (int i = 0; i < s_n && !o; i++) {
        if (!strcmp(s_apps[i].pub.file, file) || !strcmp(s_apps[i].pub.id, file)) {
            o = cJSON_ParseWithLength(s_cat + s_apps[i].pub.line_off, s_apps[i].pub.line_len);
            if (copy) *copy = s_apps[i];
        }
    }
    unlock();
    return o;
}

/* "v0.29.0/x.p4app" as an address, from where its source keeps its files. */
static void url_of(char *out, size_t n, const char *base, const char *rel)
{
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) snprintf(out, n, "%s", rel);
    else snprintf(out, n, "%s%s", base, rel);
}

/* https always; http only for a developer, at home: a catalogue or a
 * package over http can be anybody's on a network that is not one's own. */
static bool url_allowed(const char *url, err_t *e)
{
    if (!strncmp(url, "https://", 8)) return true;
    if (strncmp(url, "http://", 7)) return fail(e, "not an http(s) address");
    if (!s_dev) return fail(e, "http:// is for developer mode (Tienda, Sources)");
    if (!net_trusted()) return fail(e, "http:// only on a trusted network");
    return true;
}

/* ---- downloads ------------------------------------------------------------- */

static bool download(const char *url, const char *rel, const char *sha, uint32_t size, err_t *e)
{
    char f[FULL_MAX];
    if (!url_allowed(url, e)) return false;
    int id = aos_fetch_start(url, full(f, rel), sha, size);
    if (id <= 0) return fail(e, "could not start the download (%d)", id);
    aos_fetch_info_t in;
    for (;;) {
        aos_hal_sleep_ms(100);
        if (!aos_fetch_info(id, &in)) return fail(e, "the download went missing");
        if (in.state != AOS_FETCH_BUSY) break;
        lock();
        s_st.got = in.got;
        s_st.total = in.total ? in.total : size;
        unlock();
        if (s_cancel) aos_fetch_cancel(id);
    }
    aos_fetch_release(id);
    if (in.state == AOS_FETCH_DONE) return true;
    if (in.state == AOS_FETCH_CANCELLED) return fail(e, "cancelled");
    return fail(e, "%.150s", in.why[0] ? in.why : "the download failed");
}

/* ---- the pictures of an app's page ----------------------------------------- */

#define DIR_SHOTS   "apps/.store/shots"
#define SHOTS_LIVE  2
#define SHOTS_BAD   16

static struct { char name[72]; int id; } s_shot[SHOTS_LIVE];
static char s_shot_bad[SHOTS_BAD][72];

int aos_store_shot(const char *url, char *path, size_t path_len)
{
    aos_store_init();
    if (!url || !aos_hal_path_sd_root()) return -1;
    const char *name = strrchr(url, '/');
    name = name ? name + 1 : url;
    if (!ends(name, ".jpg") || strlen(name) >= sizeof s_shot[0].name || !name_ok(name, strlen(name), "_-.") || name[0] == '.')
        return -1;
    char rel[PATH_MAX_], f[FULL_MAX], full_url[512];
    snprintf(rel, sizeof rel, DIR_SHOTS "/%s", name);
    full(f, rel);
    int result = 0;
    lock();
    int slot = -1, free_slot = -1;
    for (int i = 0; i < SHOTS_LIVE; i++) {
        if (s_shot[i].id && !strcmp(s_shot[i].name, name)) slot = i;
        if (!s_shot[i].id && free_slot < 0) free_slot = i;
    }
    if (slot >= 0) {
        aos_fetch_info_t in;
        bool known = aos_fetch_info(s_shot[slot].id, &in);
        if (known && in.state == AOS_FETCH_BUSY) result = 0;
        else {
            result = known && in.state == AOS_FETCH_DONE ? 1 : -1;
            aos_fetch_release(s_shot[slot].id);
            s_shot[slot].id = 0;
            if (result < 0 && s_shot_nbad < SHOTS_BAD) snprintf(s_shot_bad[s_shot_nbad++], sizeof s_shot_bad[0], "%s", name);
        }
    } else if (exists(rel, NULL)) result = 1;
    else {
        for (int i = 0; i < s_shot_nbad; i++)
            if (!strcmp(s_shot_bad[i], name)) result = -1;
        if (result == 0 && free_slot >= 0) {
            err_t no;
            url_of(full_url, sizeof full_url, s_src[0].base, url);
            int id = url_allowed(full_url, &no) ? aos_fetch_start(full_url, f, NULL, 0) : -1;
            if (id <= 0) result = -1;
            if (id > 0) {
                s_shot[free_slot].id = id;
                snprintf(s_shot[free_slot].name, sizeof s_shot[0].name, "%s", name);
            }
        }
    }
    unlock();
    if (result == 1 && path) snprintf(path, path_len, "%s", f);
    return result;
}

/* ---- a zip without compression --------------------------------------------- */

typedef struct {
    char     name[PATH_MAX_];
    uint32_t off, size;         /* its bytes in the zip */
} zent_t;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The entries of the zip at 'rel' into 'ents' (ZIP_ENTRIES of them): how
 * many, or -1 with why. Every offset is checked against the file's size. */
static int zip_list(const char *rel, zent_t *ents, err_t *e)
{
    char f[FULL_MAX];
    FILE *fp = fopen(full(f, rel), "rb");
    if (!fp) { fail(e, "the package cannot be opened"); return -1; }
    int n = -1;
    uint8_t *cd = NULL;
    fseek(fp, 0, SEEK_END);
    long flen = ftell(fp);
    uint8_t tail[1024];
    long tl = flen < (long)sizeof tail ? flen : (long)sizeof tail;
    if (tl < 22 || fseek(fp, flen - tl, SEEK_SET) != 0 || fread(tail, 1, (size_t)tl, fp) != (size_t)tl) {
        fail(e, "not a package: too short");
        goto out;
    }
    long at = tl - 22;
    while (at >= 0 && le32(tail + at) != 0x06054b50) at--;
    if (at < 0) { fail(e, "not a package: no zip directory"); goto out; }
    uint32_t count = le16(tail + at + 10), cdsize = le32(tail + at + 12), cdoff = le32(tail + at + 16);
    if (count == 0 || count > ZIP_ENTRIES || cdsize > 64 * 1024 || cdoff > (uint32_t)flen || cdsize > (uint32_t)flen - cdoff) {
        fail(e, "not a package: %u files in it, %d at most", (unsigned)count, ZIP_ENTRIES);
        goto out;
    }
    cd = BIG_ALLOC(cdsize + 1);
    if (!cd || fseek(fp, (long)cdoff, SEEK_SET) != 0 || fread(cd, 1, cdsize, fp) != cdsize) {
        fail(e, "the package's directory does not read");
        goto out;
    }
    uint32_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > cdsize || le32(cd + p) != 0x02014b50) { fail(e, "the package's directory is cut short"); goto out; }
        uint16_t flags = le16(cd + p + 8), method = le16(cd + p + 10);
        uint32_t csize = le32(cd + p + 20), usize = le32(cd + p + 24), local = le32(cd + p + 42);
        uint16_t nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (p + 46 + nl > cdsize || nl == 0 || nl >= PATH_MAX_) { fail(e, "a name in the package is too long"); goto out; }
        zent_t *z = &ents[i];
        memcpy(z->name, cd + p + 46, nl);
        z->name[nl] = 0;
        if (method != 0 || csize != usize || (flags & 1)) {
            fail(e, "%.80s is compressed: a package is a zip without compression", z->name);
            goto out;
        }
        uint8_t lh[30];
        if (local > cdoff || fseek(fp, (long)local, SEEK_SET) != 0 || fread(lh, 1, 30, fp) != 30 || le32(lh) != 0x04034b50) {
            fail(e, "%.80s is not where the package says", z->name);
            goto out;
        }
        z->off = local + 30 + le16(lh + 26) + le16(lh + 28);
        z->size = usize;
        if (z->off > cdoff || z->size > cdoff - z->off) { fail(e, "%.80s runs past the package's end", z->name); goto out; }
        p += 46u + nl + xl + cl;
    }
    n = (int)count;
out:
    free(cd);
    fclose(fp);
    return n;
}

/* ---- install --------------------------------------------------------------- */

typedef struct {
    char     name[72];          /* "x_p4.pak.1" */
    char     sha[65];
    uint32_t size;
    bool     staged;            /* downloaded to tmp/, to be moved in */
} part_t;

#define PARTS_MAX 48

/* Whether 'name' is in the comma-separated list. */
static bool listed(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == n && !strncmp(p, name, n)) return true;
        p = c ? c + 1 : NULL;
    }
    return false;
}

static void tmp_clean(bool keep_parts)
{
    char d[FULL_MAX], rel[PATH_MAX_];
    DIR *dir = opendir(full(d, DIR_TMP));
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        /* a pack cut short is worth keeping: the next try goes on from it */
        if (keep_parts && ends(de->d_name, ".part")) continue;
        snprintf(rel, sizeof rel, DIR_TMP "/%.100s", de->d_name);
        del(rel);
    }
    closedir(dir);
}

/* 'entry' is the app's line of a catalogue (deleted here); 'local' the
 * package itself, on the card, when it was handed in and not listed. */
static bool install_core(cJSON *entry, app_t app, const char *local, const char *packs_arg, bool adopt, err_t *e)
{
    const char *file = app.pub.file;
    const cJSON *pkg = cJSON_GetObjectItem(entry, "pkg");
    src_t from = { .name = "local" };
    lock();
    if (app.pub.src >= 0 && app.pub.src < s_nsrc) from = s_src[app.pub.src];
    unlock();
    cJSON *old = receipt_read(file), *inside = NULL, *receipt = NULL;
    zent_t *ents = BIG_ALLOC(sizeof(zent_t) * ZIP_ENTRIES);
    part_t *parts = BIG_ALLOC(sizeof(part_t) * PARTS_MAX);
    char *missing = BIG_ALLOC(1024);
    char rel[PATH_MAX_], tmp[PATH_MAX_], url[512], sha[65], owner[32];
    int nents = 0, nparts = 0;
    bool ok = false;

    if (!ents || !parts || !missing) { fail(e, "no memory"); goto out; }
    if (jnum(entry, "abi") != AOS_STORE_ABI) { fail(e, "built for ABI %u; this firmware's is %d", (unsigned)jnum(entry, "abi"), AOS_STORE_ABI); goto out; }
    if (!fw_fits(app.pub.fw_min)) { fail(e, "needs P4OS %s or newer", app.pub.fw_min); goto out; }

    /* 2. the package */
    step("downloading");
    tmp_clean(true);
    snprintf(tmp, sizeof tmp, DIR_TMP "/%s.p4app", file);
    make_dirs(tmp);
    if (local) {
        /* a copy: the file handed in is the person's */
        if (!hash_range(local, 0, 0, true, tmp, sha)) { fail(e, "the package cannot be read"); goto out; }
    } else {
        url_of(url, sizeof url, from.base, jstr(pkg, "url"));
        if (!download(url, tmp, app.pkg_sha, jnum(pkg, "size"), e)) goto out;
    }

    /* 3. opened, and every file out to tmp/ with its hash */
    step("checking");
    nents = zip_list(tmp, ents, e);
    if (nents < 0) goto out;
    int mi = -1;
    for (int i = 0; i < nents; i++)
        if (!strcmp(ents[i].name, "p4app.json")) mi = i;
    if (mi < 0 || ents[mi].size > JSON_MAX) { fail(e, "no p4app.json in the package"); goto out; }
    {
        char f[FULL_MAX];
        char *text = BIG_ALLOC(ents[mi].size + 1);
        FILE *fp = text ? fopen(full(f, tmp), "rb") : NULL;
        if (fp && fseek(fp, (long)ents[mi].off, SEEK_SET) == 0 && fread(text, 1, ents[mi].size, fp) == ents[mi].size)
            inside = cJSON_Parse(text);
        if (fp) fclose(fp);
        free(text);
    }
    if (!inside) { fail(e, "p4app.json does not read"); goto out; }
    if (strcmp(jstr(inside, "file"), file) || strcmp(jstr(inside, "id"), app.pub.id) || jnum(inside, "abi") != AOS_STORE_ABI) {
        fail(e, "the package says it is %.40s (%.40s), not what the catalogue lists", jstr(inside, "file"), jstr(inside, "id"));
        goto out;
    }
    const cJSON *files = cJSON_GetObjectItem(inside, "files"), *f;
    if (cJSON_GetArraySize(files) != nents - 1) { fail(e, "the package's files are not the ones its p4app.json lists"); goto out; }
    bool has_so = false;
    int fi = 0;
    cJSON_ArrayForEach(f, files) {
        const char *path = jstr(f, "path");
        int zi = -1;
        for (int i = 0; i < nents; i++)
            if (!strcmp(ents[i].name, path)) zi = i;
        if (zi < 0) { fail(e, "the package's files are not the ones its p4app.json lists"); goto out; }
        if (!allowed(path, file)) { fail(e, "a package may not write %.100s", path); goto out; }
        if (owned_by_other(path, file, owner)) { fail(e, "%.90s belongs to %s, installed already", path, owner); goto out; }
        snprintf(tmp, sizeof tmp, DIR_TMP "/f%d", fi);
        snprintf(rel, sizeof rel, DIR_TMP "/%s.p4app", file);
        if (ents[zi].size != jnum(f, "size") || !hash_range(rel, ents[zi].off, ents[zi].size, false, tmp, sha) ||
            strcmp(sha, jstr(f, "sha256"))) {
            fail(e, "%.100s is not what p4app.json says", path);
            goto out;
        }
        if (ends(path, ".so")) {
            char ff[FULL_MAX];
            int miss = dyn_check(full(ff, tmp), missing, 1024);
            bool main_so = !strchr(path + 5, '/');
            has_so = has_so || main_so;
            if (miss > 0) { fail(e, "needs a newer P4OS: this one lacks %.100s", missing); goto out; }
            if (miss == -1 || (miss == -2 && main_so)) { fail(e, "%.60s: %.80s", path, missing); goto out; }
        }
        fi++;
        if (s_cancel) { fail(e, "cancelled"); goto out; }
    }
    if (!has_so) { fail(e, "the package has no apps/%s.so", file); goto out; }

    /* 4. the packs */
    step("packs");
    const cJSON *p, *part;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(entry, "packs")) {
        const char *pf = jstr(p, "file");
        if (!pack_name_ok(pf)) { fail(e, "a pack with a name that is not one"); goto out; }
        bool had = false;
        const cJSON *op;
        cJSON_ArrayForEach(op, cJSON_GetObjectItem(old, "packs"))
            if (!strcmp(jstr(op, "file"), pf)) had = true;
        bool need = cJSON_IsTrue(cJSON_GetObjectItem(p, "need"));
        const char *choice = packs_arg && packs_arg[0] ? packs_arg : "need";
        bool want = had || !strcmp(choice, "all") || (strcmp(choice, "none") && (need || listed(choice, pf)));
        if (!want && !adopt) continue;
        int first = nparts, i = 0;
        bool all_here = true;
        cJSON_ArrayForEach(part, cJSON_GetObjectItem(p, "parts")) {
            if (nparts >= PARTS_MAX) { fail(e, "too many pack parts"); goto out; }
            part_t *t = &parts[nparts];
            if (i) snprintf(t->name, sizeof t->name, "%s.%d", pf, i);
            else snprintf(t->name, sizeof t->name, "%s", pf);
            snprintf(t->sha, sizeof t->sha, "%s", jstr(part, "sha256"));
            t->size = jnum(part, "size");
            t->staged = false;
            snprintf(rel, sizeof rel, "apps/%s", t->name);
            if (owned_by_other(rel, file, owner)) { fail(e, "%.90s belongs to %s, installed already", rel, owner); goto out; }
            uint32_t have = 0;
            /* the bytes the card has already are not downloaded again */
            bool here = exists(rel, &have) && have == t->size && file_sha(rel, sha) && !strcmp(sha, t->sha);
            if (!here) {
                all_here = false;
                if (!adopt) {
                    lock();
                    snprintf(s_st.step, sizeof s_st.step, "pack %.30s", t->name);
                    unlock();
                    snprintf(tmp, sizeof tmp, DIR_TMP "/%s", t->name);
                    url_of(url, sizeof url, from.base, jstr(part, "url"));
                    if (!download(url, tmp, t->sha, t->size, e)) goto out;
                    t->staged = true;
                }
            }
            nparts++;
            i++;
        }
        /* adopting: a pack counts as the app's only when all of it is there */
        if (adopt && !all_here) nparts = first;
    }

    /* 5. from here on the card changes */
    step("installing");
    char so_name[40];
    snprintf(so_name, sizeof so_name, "%s.so", file);
    if (!adopt) dyn_unload(so_name);
    fi = 0;
    cJSON_ArrayForEach(f, files) {
        const char *path = jstr(f, "path");
        snprintf(tmp, sizeof tmp, DIR_TMP "/f%d", fi++);
        /* the same bytes there already (an adoption, a reinstall): left alone */
        if (exists(path, NULL) && file_sha(path, sha) && !strcmp(sha, jstr(f, "sha256"))) continue;
        if (!move(tmp, path)) { fail(e, "could not write %.100s (errno %d)", path, errno); goto out; }
    }
    for (int i = 0; i < nparts; i++) {
        if (!parts[i].staged) continue;
        snprintf(tmp, sizeof tmp, DIR_TMP "/%s", parts[i].name);
        snprintf(rel, sizeof rel, "apps/%s", parts[i].name);
        if (!move(tmp, rel)) { fail(e, "could not write %.100s (errno %d)", rel, errno); goto out; }
    }
    /* what the old version had and this one does not */
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(old, "files")) {
        bool still = false;
        const cJSON *nf;
        cJSON_ArrayForEach(nf, files)
            if (!strcmp(jstr(nf, "path"), jstr(f, "path"))) still = true;
        if (!still && allowed(jstr(f, "path"), file)) del(jstr(f, "path"));
    }
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(old, "packs"))
        cJSON_ArrayForEach(part, cJSON_GetObjectItem(p, "parts")) {
            bool still = false;
            for (int i = 0; i < nparts; i++)
                if (!strcmp(parts[i].name, jstr(part, "name"))) still = true;
            snprintf(rel, sizeof rel, "apps/%.80s", jstr(part, "name"));
            if (!still && strstr(rel, ".pak") && !strstr(rel, "..") && !strchr(rel + 5, '/')) del(rel);
        }

    /* 6. the receipt, last */
    receipt = cJSON_CreateObject();
    cJSON_AddNumberToObject(receipt, "schema", SCHEMA);
    cJSON_AddStringToObject(receipt, "file", file);
    cJSON_AddStringToObject(receipt, "id", app.pub.id);
    cJSON_AddItemToObject(receipt, "name", cJSON_Duplicate(cJSON_GetObjectItem(entry, "name"), true));
    cJSON_AddStringToObject(receipt, "ver", app.pub.ver);
    cJSON_AddStringToObject(receipt, "pkg_sha256", app.pkg_sha);
    cJSON *src = cJSON_AddObjectToObject(receipt, "source");
    cJSON_AddStringToObject(src, "name", from.name);
    cJSON_AddStringToObject(src, "url", from.home);
    cJSON_AddStringToObject(src, "catalog", from.url);
    cJSON_AddNumberToObject(receipt, "installed", aos_hal_time_is_valid() ? (double)time(NULL) : 0);
    if (adopt) cJSON_AddTrueToObject(receipt, "adopted");
    cJSON_AddItemToObject(receipt, "files", cJSON_Duplicate(files, true));
    cJSON *rp = cJSON_AddArrayToObject(receipt, "packs"), *cur = NULL, *cur_parts = NULL;
    for (int i = 0; i < nparts; i++) {
        /* "x.pak.1" belongs to the pack "x.pak" */
        const char *dot = strstr(parts[i].name, ".pak");
        if (!dot[4]) {
            cur = cJSON_CreateObject();
            cJSON_AddStringToObject(cur, "file", parts[i].name);
            cur_parts = cJSON_AddArrayToObject(cur, "parts");
            cJSON_AddItemToArray(rp, cur);
        }
        if (!cur_parts) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", parts[i].name);
        cJSON_AddNumberToObject(o, "size", parts[i].size);
        cJSON_AddStringToObject(o, "sha256", parts[i].sha);
        cJSON_AddItemToArray(cur_parts, o);
    }
    {
        char *text = cJSON_Print(receipt);
        snprintf(rel, sizeof rel, DIR_STORE "/%s.json", file);
        bool wrote = text && spit(rel, text);
        free(text);
        if (!wrote) { fail(e, "could not write the receipt"); goto out; }
    }
    /* a copy the loader had set aside: this install is the way back */
    snprintf(rel, sizeof rel, "apps/%s.so.off", file);
    del(rel);
    if (!adopt && !dyn_load(so_name)) {
        /* the files and the receipt are right: a restart registers it */
        aos_hal_log(TAG, "%s is installed and did not register now: at the next start", file);
    }
    ok = true;

out:
    tmp_clean(!ok);
    cJSON_Delete(entry);
    cJSON_Delete(old);
    cJSON_Delete(inside);
    cJSON_Delete(receipt);
    free(ents);
    free(parts);
    free(missing);
    return ok;
}

static bool install(const char *file, const char *packs_arg, bool adopt, err_t *e)
{
    app_t app;
    cJSON *entry = entry_of(file, &app);
    if (!entry) return fail(e, "%.40s is not in the catalogue", file);
    return install_core(entry, app, NULL, packs_arg, adopt, e);
}

/* A package handed in - dropped on the portal, or a file of the card or of a
 * pendrive - installed by the same steps. It may not take the place of what
 * a source offers or installed: that is updated from its source. */
static bool sideload(const char *rel, err_t *e)
{
    zent_t *ents = BIG_ALLOC(sizeof(zent_t) * ZIP_ENTRIES);
    cJSON *inside = NULL, *entry = NULL, *r = NULL;
    app_t app;
    char f[FULL_MAX], owner[40] = "";
    bool ok = false;
    memset(&app, 0, sizeof app);
    if (!ents) { fail(e, "no memory"); goto out; }
    step("checking");
    int n = zip_list(rel, ents, e), mi = -1;
    if (n < 0) goto out;
    for (int i = 0; i < n; i++)
        if (!strcmp(ents[i].name, "p4app.json")) mi = i;
    if (mi < 0 || ents[mi].size > JSON_MAX) { fail(e, "no p4app.json in the package"); goto out; }
    {
        char *text = BIG_ALLOC(ents[mi].size + 1);
        FILE *fp = text ? fopen(full(f, rel), "rb") : NULL;
        if (fp && fseek(fp, (long)ents[mi].off, SEEK_SET) == 0 && fread(text, 1, ents[mi].size, fp) == ents[mi].size)
            inside = cJSON_Parse(text);
        if (fp) fclose(fp);
        free(text);
    }
    if (!inside || !file_name_ok(jstr(inside, "file")) || !jstr(inside, "id")[0]) { fail(e, "p4app.json does not read"); goto out; }
    snprintf(app.pub.file, sizeof app.pub.file, "%s", jstr(inside, "file"));
    snprintf(app.pub.id, sizeof app.pub.id, "%s", jstr(inside, "id"));
    snprintf(app.pub.ver, sizeof app.pub.ver, "local");
    snprintf(app.pub.fw_min, sizeof app.pub.fw_min, "%s", jstr(inside, "fw_min"));
    app.pub.src = -1;
    lock();
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_apps[i].pub.file, app.pub.file) || !strcmp(s_apps[i].pub.id, app.pub.id))
            snprintf(owner, sizeof owner, "%s", s_src[s_apps[i].pub.src].name);
    unlock();
    if (owner[0]) { fail(e, "%s is in the Tienda (%s): install it from there", app.pub.file, owner); goto out; }
    r = receipt_read(app.pub.file);
    if (r && jstr(cJSON_GetObjectItem(r, "source"), "catalog")[0]) {
        fail(e, "%s was installed from %.40s: remove it first", app.pub.file, jstr(cJSON_GetObjectItem(r, "source"), "name"));
        goto out;
    }
    if (!file_sha(rel, app.pkg_sha)) { fail(e, "the package cannot be read"); goto out; }
    entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "file", app.pub.file);
    cJSON_AddStringToObject(entry, "id", app.pub.id);
    cJSON_AddItemToObject(entry, "name", cJSON_Duplicate(cJSON_GetObjectItem(inside, "name"), true));
    cJSON_AddNumberToObject(entry, "abi", jnum(inside, "abi"));
    ok = install_core(entry, app, rel, "none", false, e);
    entry = NULL;
out:
    cJSON_Delete(inside);
    cJSON_Delete(entry);
    cJSON_Delete(r);
    free(ents);
    if (!strncmp(rel, DIR_IN "/", sizeof DIR_IN)) del(rel);
    return ok;
}

static bool uninstall(const char *file, err_t *e)
{
    if (!file_name_ok(file)) return fail(e, "not a package's name");
    cJSON *r = receipt_read(file);
    if (!r) return fail(e, "%.40s was not installed by the store: no receipt", file);
    char so[40], rel[PATH_MAX_];
    snprintf(so, sizeof so, "%s.so", file);
    dyn_unload(so);
    const cJSON *f, *p, *part;
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(r, "files"))
        if (allowed(jstr(f, "path"), file)) del(jstr(f, "path"));
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(r, "packs"))
        cJSON_ArrayForEach(part, cJSON_GetObjectItem(p, "parts")) {
            snprintf(rel, sizeof rel, "apps/%.80s", jstr(part, "name"));
            if (strstr(rel, ".pak") && !strstr(rel, "..") && !strchr(rel + 5, '/')) del(rel);
        }
    snprintf(rel, sizeof rel, DIR_STORE "/%s.json", file);
    del(rel);
    snprintf(rel, sizeof rel, "apps/%s.so.off", file);
    del(rel);
    cJSON_Delete(r);
    return true;
}

static bool adopt_all(err_t *e)
{
    int done = 0, left = 0;
    for (int i = 0;; i++) {
        app_t a;
        lock();
        bool more = i < s_n;
        if (more) a = s_apps[i];
        unlock();
        if (!more || s_cancel) break;
        if (a.pub.state != AOS_STORE_BY_HAND) continue;
        char rel[PATH_MAX_], sha[65];
        snprintf(rel, sizeof rel, "apps/%s.so", a.pub.file);
        if (!file_sha(rel, sha) || strcmp(sha, a.so_sha)) { left++; continue; }
        lock();
        snprintf(s_st.file, sizeof s_st.file, "%s", a.pub.file);
        unlock();
        err_t one = { "" };
        if (install(a.pub.file, "none", true, &one)) done++;
        else {
            left++;
            aos_hal_log(TAG, "%s not adopted: %s", a.pub.file, one.why);
        }
    }
    snprintf(e->why, sizeof e->why, "%d adopted, %d left as they are", done, left);
    return true;
}

/* ---- sources --------------------------------------------------------------- */

/* The addresses the person added, a line each, kept in the board's own
 * memory and not on the card: a card from elsewhere brings no sources. */
static void sources_save(void)      /* with the lock held */
{
    char all[SRC_MAX * 201] = "";
    size_t w = 0;
    for (int i = 1; i < s_nsrc; i++) w += (size_t)snprintf(all + w, sizeof all - w, "%s\n", s_src[i].url);
    aos_hal_pref_set_str("store_srcs", all);
}

static void sources_read(void)
{
    static char all[SRC_MAX * 201];
    s_nsrc = 1;
    memset(s_src, 0, sizeof s_src);
    snprintf(s_src[0].url, sizeof s_src[0].url, "%s", aos_store_url());
    if (!aos_hal_pref_get_str("store_srcs", all, sizeof all)) return;
    for (char *line = strtok(all, "\n"); line && s_nsrc < SRC_MAX; line = strtok(NULL, "\n"))
        if (line[0] && strlen(line) < sizeof s_src[0].url) snprintf(s_src[s_nsrc++].url, sizeof s_src[0].url, "%s", line);
}

/* Every source's catalogue again. One that fails keeps the one it had; the
 * whole is a failure only when the first, the official one, is. */
static bool refresh_all(err_t *e)
{
    char rel[PATH_MAX_], url[200], names[120] = "";
    bool first_ok = false;
    int nsrc;
    lock();
    nsrc = s_nsrc;
    unlock();
    for (int i = 0; i < nsrc && !s_cancel; i++) {
        lock();
        snprintf(url, sizeof url, "%.199s", s_src[i].url);
        snprintf(s_st.file, sizeof s_st.file, "%.31s", s_src[i].name);
        unlock();
        src_file(i, rel);
        step("downloading");
        make_dirs(rel);
        err_t one = { "" };
        /* to a name of its own first: a catalogue that does not read must
         * not take the place of the one that does */
        bool ok = download(url, DIR_STORE "/catalog.new", NULL, 0, &one) &&
                  catalog_check(DIR_STORE "/catalog.new", NULL, &one) >= 0 && move(DIR_STORE "/catalog.new", rel);
        del(DIR_STORE "/catalog.new");
        if (i == 0) {
            first_ok = ok;
            if (!ok) *e = one;
        } else if (!ok) {
            size_t w = strlen(names);
            snprintf(names + w, sizeof names - w, "%s%.30s", w ? ", " : "", url + (strlen(url) > 30 ? strlen(url) - 30 : 0));
            aos_hal_log(TAG, "%s: %s", url, one.why);
        }
    }
    err_t load;
    bool loaded = catalog_load(&load);
    if (first_ok && names[0]) snprintf(e->why, sizeof e->why, "done, but not %.110s", names);
    return first_ok && loaded;
}

/* A look at a catalogue before its source is added: who it says it is and
 * how much it lists, for the person to read with the warning. */
static bool probe(const char *url, err_t *e)
{
    lock();
    memset(&s_probe, 0, sizeof s_probe);
    snprintf(s_probe.url, sizeof s_probe.url, "%.199s", url);
    bool have = false;
    for (int i = 0; i < s_nsrc; i++) have = have || !strcmp(s_src[i].url, url);
    bool full_list = s_nsrc >= SRC_MAX;
    unlock();
    if (have) return fail(e, "that source is there already");
    if (full_list) return fail(e, "%d sources at most", SRC_MAX);
    step("downloading");
    make_dirs(PROBE_FILE);
    del(PROBE_FILE);
    if (!download(url, PROBE_FILE, NULL, 0, e)) return false;
    char source[900];
    int n = catalog_check(PROBE_FILE, source, e);
    if (n < 0) { del(PROBE_FILE); return false; }
    lock();
    s_probe.ok = true;
    s_probe.apps = n;
    snprintf(s_probe.source_json, sizeof s_probe.source_json, "%s", source);
    unlock();
    return true;
}

static bool source_add(const char *url, err_t *e)
{
    char rel[PATH_MAX_];
    lock();
    bool ok = s_probe.ok && !strcmp(s_probe.url, url) && s_nsrc < SRC_MAX;
    if (ok) {
        memset(&s_src[s_nsrc], 0, sizeof s_src[0]);
        snprintf(s_src[s_nsrc].url, sizeof s_src[0].url, "%.199s", url);
        src_file(s_nsrc, rel);
        s_nsrc++;
        sources_save();
        memset(&s_probe, 0, sizeof s_probe);
    }
    unlock();
    if (!ok) return fail(e, "look at the source first");
    move(PROBE_FILE, rel);
    err_t load;
    catalog_load(&load);
    return true;
}

/* Takes a source off, and with 'apps' what was installed from it. */
static bool source_remove(int index, bool apps, err_t *e)
{
    char url[200], rel[PATH_MAX_], d[FULL_MAX];
    lock();
    bool ok = index > 0 && index < s_nsrc;
    if (ok) {
        snprintf(url, sizeof url, "%s", s_src[index].url);
        src_file(index, rel);
    }
    unlock();
    if (!ok) return fail(e, "no such source (the official one stays)");
    int gone = 0;
    if (apps) {
        /* the names first: removing changes the folder being read */
        static char names[64][32];
        int n = 0;
        DIR *dir = opendir(full(d, DIR_STORE));
        struct dirent *de;
        while (dir && (de = readdir(dir)) != NULL && n < 64) {
            size_t l = strlen(de->d_name);
            if (l < 6 || l > 36 || strcmp(de->d_name + l - 5, ".json")) continue;
            char file[32];
            snprintf(file, sizeof file, "%.*s", (int)(l - 5), de->d_name);
            cJSON *r = receipt_read(file);
            if (r && !strcmp(jstr(cJSON_GetObjectItem(r, "source"), "catalog"), url)) snprintf(names[n++], sizeof names[0], "%s", file);
            cJSON_Delete(r);
        }
        if (dir) closedir(dir);
        for (int i = 0; i < n; i++) {
            err_t one;
            gone += uninstall(names[i], &one);
        }
    }
    del(rel);
    lock();
    memmove(&s_src[index], &s_src[index + 1], sizeof(src_t) * (size_t)(s_nsrc - index - 1));
    s_nsrc--;
    sources_save();
    unlock();
    err_t load;
    catalog_load(&load);
    if (apps) snprintf(e->why, sizeof e->why, "done, with %d of its apps", gone);
    return true;
}

/* ---- the worker ------------------------------------------------------------ */

static const char *job_name(int kind)
{
    static const char *names[] = { "", "refresh", "install", "remove", "adopt", "look", "add", "unsource", "sideload" };
    return names[kind >= J_REFRESH && kind <= J_SIDELOAD ? kind : 0];
}

static void work(void *arg)
{
    (void)arg;
    for (;;) {
        job_t j;
        lock();
        if (s_qn == 0 || s_cancel) {
            s_qn = 0;
            s_running = false;
            s_cancel = false;
            s_st.busy = false;
            s_st.job[0] = s_st.file[0] = s_st.step[0] = 0;
            s_st.queued = 0;
            unlock();
            return;
        }
        j = s_queue[0];
        memmove(&s_queue[0], &s_queue[1], sizeof(job_t) * (size_t)(--s_qn));
        s_st.busy = true;
        s_st.queued = s_qn;
        snprintf(s_st.job, sizeof s_st.job, "%s", job_name(j.kind));
        snprintf(s_st.file, sizeof s_st.file, "%s", j.file);
        s_st.step[0] = 0;
        unlock();

        err_t e = { "" };
        bool ok = false;
        if (!aos_hal_path_sd_root()) fail(&e, "no card");
        else if (j.kind == J_REFRESH) ok = refresh_all(&e);
        else if (j.kind == J_PROBE) ok = probe(j.packs, &e);
        else if (j.kind == J_ADD) ok = source_add(j.packs, &e);
        else if (j.kind == J_UNSOURCE) ok = source_remove(atoi(j.file), j.packs[0] == '1', &e);
        else if (j.kind == J_SIDELOAD) ok = sideload(j.packs, &e);
        else if (j.kind == J_INSTALL) ok = install(j.file, j.packs, false, &e);
        else if (j.kind == J_REMOVE) ok = uninstall(j.file, &e);
        else if (j.kind == J_ADOPT) ok = adopt_all(&e);

        lock();
        states();
        s_st.jobs_done++;
        s_st.last_ok = ok;
        if (ok && !e.why[0]) snprintf(s_st.last, sizeof s_st.last, "%s%s%s: done", job_name(j.kind), j.file[0] ? " " : "", j.file);
        else snprintf(s_st.last, sizeof s_st.last, "%s%s%s: %.110s", job_name(j.kind), j.file[0] ? " " : "", j.file, e.why);
        aos_hal_log(TAG, "%s", s_st.last);
        unlock();
    }
}

static bool enqueue(int kind, const char *file, const char *packs)
{
    aos_store_init();
    lock();
    bool ok = s_qn < QUEUE;
    if (ok) {
        job_t *j = &s_queue[s_qn++];
        memset(j, 0, sizeof *j);
        j->kind = kind;
        snprintf(j->file, sizeof j->file, "%s", file ? file : "");
        snprintf(j->packs, sizeof j->packs, "%s", packs ? packs : "");
        s_st.queued = s_qn;
        if (!s_running) {
            s_running = true;
            s_cancel = false;
            s_st.busy = true;
            /* below the interface: it waits on the network and the card */
            if (!aos_hal_thread_start("aos_store", work, NULL, STACK, 2)) {
                s_running = false;
                s_st.busy = false;
                s_qn = 0;
                ok = false;
            }
        }
    }
    unlock();
    return ok;
}

/* ---- the calls ------------------------------------------------------------- */

void aos_store_init(void)
{
    if (s_inited) return;
    s_lock = aos_hal_mutex_create();
    s_inited = true;
    int32_t dev = 0;
    s_dev = aos_hal_pref_get_i32("store_dev", &dev) && dev;
    /* another first source is a developer's: without the mode, the official one */
    if (!s_dev || !aos_hal_pref_get_str("store_url", s_url, sizeof s_url)) s_url[0] = 0;
    sources_read();
    err_t e;
    if (!aos_hal_path_sd_root()) return;
    /* what an install cut short by a restart left staged; a pack's .part
     * stays, for the next try to go on from */
    tmp_clean(true);
    if (!catalog_load(&e)) aos_hal_log(TAG, "%s", e.why);
}

const char *aos_store_url(void)
{
    return s_url[0] ? s_url : AOS_STORE_URL_DEFAULT;
}

bool aos_store_set_url(const char *url)
{
    aos_store_init();
    bool to_official = !url || !url[0];
    if (!to_official && (!s_dev || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) || strlen(url) >= sizeof s_url))
        return false;
    lock();
    snprintf(s_url, sizeof s_url, "%s", to_official ? "" : url);
    snprintf(s_src[0].url, sizeof s_src[0].url, "%s", aos_store_url());
    unlock();
    return aos_hal_pref_set_str("store_url", s_url);
}

bool aos_store_dev(void)
{
    aos_store_init();
    return s_dev;
}

void aos_store_set_dev(bool on)
{
    aos_store_init();
    lock();
    s_dev = on;
    unlock();
    aos_hal_pref_set_i32("store_dev", on ? 1 : 0);
    if (!on) aos_store_set_url(NULL);       /* and back to the official catalogue */
}

bool aos_store_source_url(const char *text, char *out, size_t n)
{
    static const char TAIL[] = "/releases/latest/download/p4os-store.ndjson";
    if (!text || !out) return false;
    while (*text == ' ') text++;
    char t[200];
    snprintf(t, sizeof t, "%s", text);
    size_t l = strlen(t);
    while (l && (t[l - 1] == ' ' || t[l - 1] == '/' || t[l - 1] == '\n')) t[--l] = 0;
    if (!l || strchr(t, ' ')) return false;
    const char *repo = !strncmp(t, "https://github.com/", 19) ? t + 19 : !strncmp(t, "github.com/", 11) ? t + 11 : t;
    const char *slash = strchr(repo, '/');
    /* "user/repo", with or without github.com in front: its latest release's catalogue */
    if (slash && slash > repo && slash[1] && !strchr(slash + 1, '/') && !strstr(repo, "://") &&
        name_ok(repo, (size_t)(slash - repo), "_-.") && name_ok(slash + 1, strlen(slash + 1), "_-."))
        return snprintf(out, n, "https://github.com/%s%s", repo, TAIL) < (int)n;
    if (strncmp(t, "https://", 8) && strncmp(t, "http://", 7)) return false;
    return snprintf(out, n, "%s", t) < (int)n;
}

int aos_store_sources(void)
{
    aos_store_init();
    return s_nsrc;
}

bool aos_store_source(int index, aos_store_source_t *out)
{
    aos_store_init();
    lock();
    bool ok = index >= 0 && index < s_nsrc;
    if (ok) {
        const src_t *sp = &s_src[index];
        memset(out, 0, sizeof *out);
        snprintf(out->url, sizeof out->url, "%s", sp->url);
        snprintf(out->name, sizeof out->name, "%s", sp->name);
        snprintf(out->home, sizeof out->home, "%s", sp->home);
        snprintf(out->base, sizeof out->base, "%s", sp->base);
        snprintf(out->release, sizeof out->release, "%s", sp->release);
        out->apps = sp->apps;
        out->skipped = sp->skipped;
        out->loaded = sp->loaded;
    }
    unlock();
    return ok;
}

bool aos_store_probe(const char *url)
{
    return url && strlen(url) < sizeof s_probe.url && enqueue(J_PROBE, NULL, url);
}

void aos_store_probe_result(aos_store_probe_t *out)
{
    aos_store_init();
    lock();
    *out = s_probe;
    unlock();
}

bool aos_store_source_add(const char *url)
{
    return url && strlen(url) < sizeof s_probe.url && enqueue(J_ADD, NULL, url);
}

bool aos_store_source_remove(int index, bool with_apps)
{
    char n[8];
    snprintf(n, sizeof n, "%d", index);
    return index > 0 && enqueue(J_UNSOURCE, n, with_apps ? "1" : "0");
}

bool aos_store_sideload(const char *card_path)
{
    if (!card_path) return false;
    while (*card_path == '/') card_path++;
    return card_path[0] && !strstr(card_path, "..") && strlen(card_path) < PATH_MAX_ && enqueue(J_SIDELOAD, NULL, card_path);
}

bool aos_store_refresh(void) { return enqueue(J_REFRESH, NULL, NULL); }
bool aos_store_adopt(void)   { return enqueue(J_ADOPT, NULL, NULL); }

bool aos_store_install(const char *file, const char *packs)
{
    if (!file || !file[0] || strlen(file) >= 40 || (packs && strlen(packs) >= sizeof s_queue[0].packs)) return false;
    return enqueue(J_INSTALL, file, packs);
}

bool aos_store_remove(const char *file)
{
    return file && file_name_ok(file) && enqueue(J_REMOVE, file, NULL);
}

void aos_store_cancel(void)
{
    if (!s_inited) return;
    lock();
    if (s_running) s_cancel = true;
    unlock();
}

void aos_store_status(aos_store_status_t *out)
{
    aos_store_init();
    lock();
    *out = s_st;
    unlock();
}

int aos_store_count(void)
{
    aos_store_init();
    return s_n;
}

bool aos_store_app(int index, aos_store_app_t *out)
{
    aos_store_init();
    lock();
    bool ok = index >= 0 && index < s_n;
    if (ok) *out = s_apps[index].pub;
    unlock();
    return ok;
}

char *aos_store_catalog(size_t *len)
{
    aos_store_init();
    lock();
    char *copy = s_cat ? BIG_ALLOC(s_cat_len + 1) : NULL;
    if (copy) {
        memcpy(copy, s_cat, s_cat_len);
        if (len) *len = s_cat_len;
    }
    unlock();
    return copy;
}
