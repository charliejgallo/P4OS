/*
 * P4OS - Programmer: what there is to flash on the card (<sd>/firmware).
 *
 * Three shapes, the ones a bench actually has:
 *
 *   folder/flasher_args.json   ESP-IDF's build/ (or a project folder with its
 *                              build/ inside): flash_files gives every bin its
 *                              offset, flash_settings the mode/size/freq the
 *                              build was made for, extra_esptool_args.chip
 *                              the target. The bins are written as they are:
 *                              the build already put those settings in the
 *                              bootloader's header, and rewriting them here
 *                              would break the SHA-256 appended to the image,
 *                              which the ROM checks.
 *   name@0x10000.bin           one image at the offset in its name.
 *   name.bin                   a merged/factory image (esptool merge_bin, the
 *                              "factory" of a release) at 0x0 - unless the
 *                              file is a bare application image, recognisable
 *                              by the app descriptor right after its first
 *                              segment header, which goes to 0x10000.
 *
 * Every image header also says which chip it is for (chip_id, the same
 * numbering esp-serial-flasher uses), and an app image says its project name
 * and version: both are read here, so the list shows them and the job can
 * refuse an S3 build on a C3.
 *
 * flasher_args.json is read with a small scanner for just those three
 * objects, not a JSON library: it is always the same file written by the same
 * tool, and nothing here may depend on ESP-IDF (the simulator builds it).
 */
#include "aos_flasher.h"
#include "aos_hal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------- */
/* Chips                                                                       */
/* -------------------------------------------------------------------------- */

static const struct { int id; const char *name, *label; } CHIPS[] = {
    { 0,  "esp32",    "ESP32" },    { 2,  "esp32s2",  "ESP32-S2" }, { 5,  "esp32c3",  "ESP32-C3" },
    { 9,  "esp32s3",  "ESP32-S3" }, { 12, "esp32c2",  "ESP32-C2" }, { 13, "esp32c6",  "ESP32-C6" },
    { 16, "esp32h2",  "ESP32-H2" }, { 18, "esp32p4",  "ESP32-P4" }, { 20, "esp32c61", "ESP32-C61" },
    { 23, "esp32c5",  "ESP32-C5" }, { 25, "esp32h21", "ESP32-H21" },{ 28, "esp32h4",  "ESP32-H4" },
    { 32, "esp32s31", "ESP32-S31" },{ -1, "esp8266",  "ESP8266" },
};
#define N_CHIPS (sizeof CHIPS / sizeof CHIPS[0])

const char *aos_flasher_chip_label(const char *idf_name)
{
    for (size_t i = 0; idf_name && i < N_CHIPS; i++)
        if (!strcasecmp(CHIPS[i].name, idf_name)) return CHIPS[i].label;
    return idf_name ? idf_name : "";
}

static const char *chip_of_id(int id)
{
    for (size_t i = 0; i < N_CHIPS; i++) if (CHIPS[i].id == id) return CHIPS[i].name;
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Image headers                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool image;                 /* starts with an ESP image header */
    const char *chip;           /* from the extended header, NULL unknown */
    bool app;                   /* has the app descriptor */
    char app_name[32], app_version[32];
} head_t;

static void copy_field(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap && src[i]; i++) dst[i] = isprint(src[i]) ? (char)src[i] : '?';
    dst[i] = 0;
}

/* esp_image_header_t: magic 0xE9, segments, mode, size|freq, entry(4),
 * wp_pin (0xEE in every IDF image), drv[3], chip_id(2)... 24 bytes, then the
 * first segment header (8) and, in an app, esp_app_desc_t with its magic
 * 0xABCD5432, version[32] at +16 and project_name[32] at +48. An ESP8266
 * image has the same magic but no extended header: wp_pin tells them apart. */
static void parse_head(const uint8_t *b, size_t n, head_t *h)
{
    memset(h, 0, sizeof *h);
    if (n < 24 || b[0] != 0xE9) return;
    h->image = true;
    if (b[8] == 0xEE) h->chip = chip_of_id(b[12] | (b[13] << 8));
    if (n >= 112 && b[32] == 0x32 && b[33] == 0x54 && b[34] == 0xCD && b[35] == 0xAB) {
        h->app = true;
        copy_field(h->app_version, sizeof h->app_version, b + 48, 32);
        copy_field(h->app_name, sizeof h->app_name, b + 80, 32);
    }
}

static size_t read_at(const char *path, uint32_t off, uint8_t *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = 0;
    if (fseek(f, (long)off, SEEK_SET) == 0) got = fread(buf, 1, n, f);
    fclose(f);
    return got;
}

static bool file_size(const char *path, uint32_t *size)
{
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return false;
    *size = (uint32_t)st.st_size;
    return true;
}

/* -------------------------------------------------------------------------- */
/* flasher_args.json                                                           */
/* -------------------------------------------------------------------------- */

typedef struct { const char *p, *end; } scan_t;

static void ws(scan_t *s) { while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++; }

/* A JSON string into out (escapes resolved; \u kept as '?'). */
static bool str(scan_t *s, char *out, size_t cap)
{
    ws(s);
    if (s->p >= s->end || *s->p != '"') return false;
    s->p++;
    size_t n = 0;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\' && s->p < s->end) {
            c = *s->p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') { c = '?'; s->p += (s->end - s->p >= 4) ? 4 : s->end - s->p; }
        }
        if (out && n + 1 < cap) out[n++] = c;
    }
    if (out && cap) out[n] = 0;
    if (s->p >= s->end) return false;
    s->p++;
    return true;
}

static bool skip_value(scan_t *s)
{
    ws(s);
    if (s->p >= s->end) return false;
    char c = *s->p;
    if (c == '"') return str(s, NULL, 0);
    if (c == '{' || c == '[') {
        int depth = 0;
        while (s->p < s->end) {
            char d = *s->p;
            if (d == '"') { if (!str(s, NULL, 0)) return false; continue; }
            s->p++;
            if (d == '{' || d == '[') depth++;
            else if ((d == '}' || d == ']') && --depth == 0) return true;
        }
        return false;
    }
    while (s->p < s->end && *s->p != ',' && *s->p != '}' && *s->p != ']') s->p++;
    return true;
}

/* Walks an object's members; for each one calls fn(key, s) positioned on the
 * value, which must consume it (or leave it to skip_value, returning false). */
typedef bool (*member_fn)(const char *key, scan_t *s, void *ud);

static bool object(scan_t *s, member_fn fn, void *ud)
{
    ws(s);
    if (s->p >= s->end || *s->p != '{') return false;
    s->p++;
    for (;;) {
        ws(s);
        if (s->p < s->end && *s->p == '}') { s->p++; return true; }
        char key[64];
        if (!str(s, key, sizeof key)) return false;
        ws(s);
        if (s->p >= s->end || *s->p != ':') return false;
        s->p++;
        if (!fn(key, s, ud) && !skip_value(s)) return false;
        ws(s);
        if (s->p < s->end && *s->p == ',') s->p++;
    }
}

static bool files_member(const char *key, scan_t *s, void *ud)
{
    aos_flasher_source_t *src = ud;
    ws(s);
    if (s->p >= s->end || *s->p != '"') return false;
    char name[64];
    if (!str(s, name, sizeof name)) return false;
    if (src->nfiles < AOS_FLASHER_FILES_MAX) {
        aos_flasher_file_t *f = &src->files[src->nfiles++];
        f->offset = (uint32_t)strtoul(key, NULL, 0);
        snprintf(f->name, sizeof f->name, "%s", name);
    }
    return true;
}

static bool settings_member(const char *key, scan_t *s, void *ud)
{
    aos_flasher_source_t *src = ud;
    char v[16];
    ws(s);
    if (s->p >= s->end || *s->p != '"') return false;
    if (!str(s, v, sizeof v)) return false;
    if (!strcmp(key, "flash_mode")) snprintf(src->flash_mode, sizeof src->flash_mode, "%.7s", v);
    else if (!strcmp(key, "flash_size")) snprintf(src->flash_size, sizeof src->flash_size, "%.7s", v);
    else if (!strcmp(key, "flash_freq")) snprintf(src->flash_freq, sizeof src->flash_freq, "%.7s", v);
    return true;
}

static bool extra_member(const char *key, scan_t *s, void *ud)
{
    aos_flasher_source_t *src = ud;
    if (strcmp(key, "chip")) return false;
    ws(s);
    if (s->p >= s->end || *s->p != '"') return false;
    return str(s, src->chip, sizeof src->chip);
}

static bool top_member(const char *key, scan_t *s, void *ud)
{
    if (!strcmp(key, "flash_files")) return object(s, files_member, ud);
    if (!strcmp(key, "flash_settings")) return object(s, settings_member, ud);
    if (!strcmp(key, "extra_esptool_args")) return object(s, extra_member, ud);
    return false;
}

static int by_offset(const void *a, const void *b)
{
    const aos_flasher_file_t *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

static bool fail(char *err, size_t n, const char *fmt, const char *arg)
{
    if (err && n) snprintf(err, n, fmt, arg);
    return false;
}

static const char *base_name(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static bool load_project(const char *dir, aos_flasher_source_t *src, char *err, size_t err_len)
{
    char path[320];
    snprintf(path, sizeof path, "%s/flasher_args.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return fail(err, err_len, "no flasher_args.json in %s", dir);
    char *buf = malloc(16384);
    size_t n = buf ? fread(buf, 1, 16384, f) : 0;
    fclose(f);
    if (!buf) return fail(err, err_len, "%s", "no memory");
    scan_t s = { buf, buf + n };
    bool ok = object(&s, top_member, src);
    free(buf);
    if (!ok) return fail(err, err_len, "%s: not valid JSON", "flasher_args.json");
    if (!src->nfiles) return fail(err, err_len, "%s: no flash_files", "flasher_args.json");
    qsort(src->files, (size_t)src->nfiles, sizeof src->files[0], by_offset);
    src->project = true;
    src->kind = AOS_FLASHER_PROJECT;
    for (int i = 0; i < src->nfiles; i++) {
        aos_flasher_file_t *fl = &src->files[i];
        snprintf(path, sizeof path, "%s/%s", dir, fl->name);
        if (!file_size(path, &fl->size)) return fail(err, err_len, "missing %s", fl->name);
        src->total += fl->size;
        uint8_t h[128];
        head_t hd;
        parse_head(h, read_at(path, 0, h, sizeof h), &hd);
        if (!src->chip[0] && hd.chip) snprintf(src->chip, sizeof src->chip, "%s", hd.chip);
        if (hd.app && !src->app_name[0]) {
            snprintf(src->app_name, sizeof src->app_name, "%s", hd.app_name);
            snprintf(src->app_version, sizeof src->app_version, "%s", hd.app_version);
        }
    }
    return true;
}

/* "name@0x10000.bin" -> 0x10000; -1 without one. */
static long offset_in_name(const char *name)
{
    const char *at = strrchr(name, '@');
    if (!at || strncasecmp(at + 1, "0x", 2)) return -1;
    char *end;
    unsigned long v = strtoul(at + 1, &end, 16);
    if (strcasecmp(end, ".bin")) return -1;
    return (long)v;
}

static bool load_bin(const char *path, aos_flasher_source_t *src, char *err, size_t err_len)
{
    aos_flasher_file_t *f = &src->files[0];
    if (!file_size(path, &f->size)) return fail(err, err_len, "cannot read %s", path);
    src->nfiles = 1;
    src->total = f->size;
    snprintf(f->name, sizeof f->name, "%.63s", base_name(path));
    uint8_t h[128];
    head_t hd;
    parse_head(h, read_at(path, 0, h, sizeof h), &hd);
    long off = offset_in_name(f->name);
    if (off >= 0) {
        f->offset = (uint32_t)off;
        src->kind = AOS_FLASHER_BIN_AT;
    } else if (hd.app) {
        f->offset = 0x10000;
        src->kind = AOS_FLASHER_BIN_APP;
    } else {
        f->offset = 0;
        src->kind = AOS_FLASHER_BIN_MERGED;
        /* an ESP32's merged image starts with 4 KB of padding before the
         * bootloader; the others start with it */
        if (!hd.image) parse_head(h, read_at(path, 0x1000, h, sizeof h), &hd);
    }
    if (hd.chip) snprintf(src->chip, sizeof src->chip, "%s", hd.chip);
    if (hd.app) {
        snprintf(src->app_name, sizeof src->app_name, "%s", hd.app_name);
        snprintf(src->app_version, sizeof src->app_version, "%s", hd.app_version);
    } else if (f->offset == 0 && f->size > 0x10000 + 128) {
        head_t app;
        parse_head(h, read_at(path, 0x10000, h, sizeof h), &app);
        if (app.app) {
            snprintf(src->app_name, sizeof src->app_name, "%s", app.app_name);
            snprintf(src->app_version, sizeof src->app_version, "%s", app.app_version);
        }
    }
    return true;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static bool has_args(const char *dir)
{
    char p[320];
    snprintf(p, sizeof p, "%s/flasher_args.json", dir);
    uint32_t sz;
    return file_size(p, &sz);
}

bool aos_flasher_source_load(const char *path, aos_flasher_source_t *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof *out);
    if (!path || !*path) return fail(err, err_len, "%s", "no path");
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    size_t l = strlen(dir);
    while (l > 1 && dir[l - 1] == '/') dir[--l] = 0;
    snprintf(out->name, sizeof out->name, "%.47s", base_name(dir));
    if (is_dir(dir)) {
        /* the project folder with its build/ inside also counts */
        if (!has_args(dir)) {
            char b[256];
            snprintf(b, sizeof b, "%.249s/build", dir);
            if (has_args(b)) snprintf(dir, sizeof dir, "%s", b);
        }
        snprintf(out->path, sizeof out->path, "%s", dir);
        if (!strcmp(out->name, "build")) {
            /* ".../hello_world/build": call it by the project */
            char up[256];
            snprintf(up, sizeof up, "%s", dir);
            char *s = strrchr(up, '/');
            if (s) { *s = 0; snprintf(out->name, sizeof out->name, "%.47s", base_name(up)); }
        }
        return load_project(dir, out, err, err_len);
    }
    snprintf(out->path, sizeof out->path, "%s", dir);
    return load_bin(dir, out, err, err_len);
}

const char *aos_flasher_dir(void)
{
    static char dir[192];
    const char *root = aos_hal_path_sd_root();
    if (!root || !*root) return NULL;
    snprintf(dir, sizeof dir, "%s/firmware", root);
    return dir;
}

static int by_kind_name(const void *a, const void *b)
{
    const aos_flasher_source_t *x = a, *y = b;
    if (x->project != y->project) return x->project ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

int aos_flasher_scan(aos_flasher_source_t *out, int max)
{
    const char *dir = aos_flasher_dir();
    if (!dir) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        char p[256];
        snprintf(p, sizeof p, "%.190s/%.60s", dir, e->d_name);
        size_t l = strlen(e->d_name);
        bool bin = l > 4 && !strcasecmp(e->d_name + l - 4, ".bin");
        if (!bin && !is_dir(p)) continue;
        if (!bin) {
            char b[256];
            snprintf(b, sizeof b, "%.249s/build", p);
            if (!has_args(p) && !has_args(b)) continue;
        }
        char err[64];
        if (aos_flasher_source_load(p, &out[n], err, sizeof err)) n++;
        else aos_hal_log("flasher", "%s skipped: %s", e->d_name, err);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], by_kind_name);
    return n;
}
