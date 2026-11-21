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
  #define BIG_ALLOC(n)  calloc(1, (n))
  /* the simulator links its apps in: nothing to check, load or unload */
  static int  dyn_check(const char *path, char *missing, size_t n) { (void)path; if (missing && n) missing[0] = 0; return 0; }
  static void dyn_unload(const char *so) { (void)so; }
  static bool dyn_load(const char *so) { (void)so; return true; }
#else
  #include "esp_heap_caps.h"
  #include "aos_dynapp.h"
  #define BIG_ALLOC(n)  heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
  static int  dyn_check(const char *path, char *missing, size_t n) { return aos_dynapp_check(path, missing, n); }
  static void dyn_unload(const char *so) { aos_dynapp_unload_file(so); }
  static bool dyn_load(const char *so) { return aos_dynapp_load(so); }
#endif

#define TAG             "store"
#define SCHEMA          1
#define DIR_STORE       "apps/.store"
#define DIR_TMP         "apps/.store/tmp"
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

enum { J_REFRESH = 1, J_INSTALL, J_REMOVE, J_ADOPT };

typedef struct {
    int  kind;
    char file[32];
    char packs[160];
} job_t;

static void   *s_lock;
static bool    s_inited;
static char   *s_cat;               /* the catalogue's text */
static size_t  s_cat_len;
static app_t  *s_apps;
static int     s_n;
static char    s_base[200];
static char    s_source_name[40], s_source_url[160];
static char    s_url[200];
static job_t   s_queue[QUEUE];
static int     s_qn;
static bool    s_running, s_cancel;
static aos_store_status_t s_st;

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

/* Reads apps/.store/catalog.ndjson into memory. false, with why, when there
 * is none or it is not one this firmware reads; what was loaded stays. */
static bool catalog_load(err_t *e)
{
    size_t len = 0;
    char *text = slurp(CATALOG_FILE, CATALOG_MAX, &len);
    if (!text) return fail(e, "no catalogue yet");
    app_t *apps = BIG_ALLOC(sizeof(app_t) * APPS_MAX);
    if (!apps) { free(text); return fail(e, "no memory for the catalogue"); }

    int n = 0;
    bool head = false;
    char base[sizeof s_base] = "", name[sizeof s_source_name] = "", url[sizeof s_source_url] = "", rel[28] = "";
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        if (end > at) {
            cJSON *o = cJSON_ParseWithLength(text + at, end - at);
            if (!o) { free(apps); free(text); return fail(e, "the catalogue does not read at byte %u", (unsigned)at); }
            if (!head) {
                if (jnum(o, "schema") != SCHEMA) {
                    cJSON_Delete(o); free(apps); free(text);
                    return fail(e, "a catalogue of another schema: update P4OS to read it");
                }
                snprintf(base, sizeof base, "%s", jstr(o, "base"));
                snprintf(rel, sizeof rel, "%s", jstr(o, "release"));
                const cJSON *src = cJSON_GetObjectItem(o, "source");
                snprintf(name, sizeof name, "%s", jstr(src, "name"));
                snprintf(url, sizeof url, "%s", jstr(src, "url"));
                head = true;
            } else if (n < APPS_MAX && file_name_ok(jstr(o, "file"))) {
                app_t *a = &apps[n];
                const cJSON *pkg = cJSON_GetObjectItem(o, "pkg");
                snprintf(a->pub.file, sizeof a->pub.file, "%s", jstr(o, "file"));
                snprintf(a->pub.id, sizeof a->pub.id, "%s", jstr(o, "id"));
                snprintf(a->pub.ver, sizeof a->pub.ver, "%s", jstr(o, "ver"));
                snprintf(a->pub.fw_min, sizeof a->pub.fw_min, "%s", jstr(o, "fw_min"));
                snprintf(a->pkg_sha, sizeof a->pkg_sha, "%s", jstr(pkg, "sha256"));
                snprintf(a->so_sha, sizeof a->so_sha, "%s", jstr(pkg, "so_sha256"));
                a->pub.fits = jnum(o, "abi") == AOS_STORE_ABI && fw_fits(a->pub.fw_min);
                a->pub.line_off = (uint32_t)at;
                a->pub.line_len = (uint32_t)(end - at);
                n++;
            }
            cJSON_Delete(o);
        }
        at = end + 1;
    }
    if (!head) { free(apps); free(text); return fail(e, "an empty catalogue"); }

    lock();
    free(s_cat);
    free(s_apps);
    s_cat = text;
    s_cat_len = len;
    s_apps = apps;
    s_n = n;
    snprintf(s_base, sizeof s_base, "%s", base);
    snprintf(s_source_name, sizeof s_source_name, "%s", name);
    snprintf(s_source_url, sizeof s_source_url, "%s", url);
    snprintf(s_st.release, sizeof s_st.release, "%s", rel);
    snprintf(s_st.source, sizeof s_st.source, "%s", name);
    s_st.apps = (uint32_t)n;
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

/* "v0.29.0/x.p4app" as an address. */
static void url_of(char *out, size_t n, const char *rel)
{
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) snprintf(out, n, "%s", rel);
    else snprintf(out, n, "%s%s", s_base, rel);
}

/* ---- downloads ------------------------------------------------------------- */

static bool download(const char *url, const char *rel, const char *sha, uint32_t size, err_t *e)
{
    char f[FULL_MAX];
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

static bool install(const char *file, const char *packs_arg, bool adopt, err_t *e)
{
    app_t app;
    cJSON *entry = entry_of(file, &app);
    if (!entry) return fail(e, "%.40s is not in the catalogue", file);
    file = app.pub.file;
    const cJSON *pkg = cJSON_GetObjectItem(entry, "pkg");
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
    url_of(url, sizeof url, jstr(pkg, "url"));
    if (!download(url, tmp, app.pkg_sha, jnum(pkg, "size"), e)) goto out;

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
                    url_of(url, sizeof url, jstr(part, "url"));
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
    cJSON_AddStringToObject(src, "name", s_source_name);
    cJSON_AddStringToObject(src, "url", s_source_url);
    cJSON_AddStringToObject(src, "catalog", aos_store_url());
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

/* ---- the worker ------------------------------------------------------------ */

static const char *job_name(int kind)
{
    return kind == J_REFRESH ? "refresh" : kind == J_INSTALL ? "install" : kind == J_REMOVE ? "remove" : "adopt";
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
        else if (j.kind == J_REFRESH) {
            step("downloading");
            make_dirs(CATALOG_FILE);
            /* to a name of its own first: a catalogue that does not read
             * must not take the place of the one that does */
            ok = download(aos_store_url(), DIR_STORE "/catalog.new", NULL, 0, &e);
            if (ok) {
                char *keep = slurp(CATALOG_FILE, CATALOG_MAX, NULL);
                ok = move(DIR_STORE "/catalog.new", CATALOG_FILE) && catalog_load(&e);
                if (!ok && keep) {
                    spit(CATALOG_FILE, keep);
                    err_t back;
                    catalog_load(&back);
                }
                free(keep);
            }
        } else if (j.kind == J_INSTALL) ok = install(j.file, j.packs, false, &e);
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
    if (!aos_hal_pref_get_str("store_url", s_url, sizeof s_url)) s_url[0] = 0;
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
    if (url && url[0] && strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) return false;
    if (url && strlen(url) >= sizeof s_url) return false;
    lock();
    snprintf(s_url, sizeof s_url, "%s", url ? url : "");
    unlock();
    return aos_hal_pref_set_str("store_url", s_url);
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
