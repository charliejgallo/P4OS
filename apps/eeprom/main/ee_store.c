/*
 * P4OS - EEPROM: the versions on the card.
 *
 * Every read of a chip, and the copy taken before every write, is a version:
 *
 *     <card>/eeprom/<chip>/20261005-140322-lectura.bin
 *     <card>/eeprom/<chip>/20261005-140322-lectura.json
 *
 * The .bin is the memory as it came off the wire (a 93xx at x16 with each
 * word high byte first). The .json beside it is small and flat, so the
 * portal's page reads it with JSON.parse and this side with a few strstr:
 *
 *     {"chip":"24LC256","size":32768,"date":"2026-10-05 14:03:22",
 *      "source":"lectura","note":"","crc32":"1a2b3c4d","md5":"...","sha256":"..."}
 *
 * Without a clock (no SNTP yet) the name starts with "sinhora-" and the
 * uptime instead of the date. A version the same as the newest one is not
 * written twice: the read says it matched that one.
 */
#include "ee.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif

void *ee_alloc(size_t n)
{
    if (!n) n = 1;
#if defined(AOS_SIM)
    return malloc(n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#endif
}

bool ee_card_ok(void)
{
    const char *r = aos_hal_path_sd_root();
    return r && r[0];
}

bool ee_ver_dir(const char *chip, char *out, size_t len)
{
    const char *r = aos_hal_path_sd_root();
    if (!r || !r[0]) return false;
    snprintf(out, len, "%s/%s", r, EE_DIR);
    mkdir(out, 0755);
    snprintf(out, len, "%s/%s/%s", r, EE_DIR, chip);
    mkdir(out, 0755);
    struct stat st;
    return stat(out, &st) == 0;
}

void ee_ver_path(const char *chip, const char *file, char *out, size_t len)
{
    const char *r = aos_hal_path_sd_root();
    snprintf(out, len, "%s/%s/%s/%s", r ? r : "", EE_DIR, chip, file);
}

static void json_path(const char *chip, const char *file, char *out, size_t len)
{
    ee_ver_path(chip, file, out, len);
    char *dot = strrchr(out, '.');
    if (dot && (size_t)(dot - out) + 6 <= len) strcpy(dot, ".json");
}

/* -------------------------------------------------------------------------- */
/* The .json                                                                   */
/* -------------------------------------------------------------------------- */

void ee_json_str(char *out, size_t len, const char *s)
{
    size_t k = 0;
    if (k + 1 < len) out[k++] = '"';
    for (; s && *s && k + 7 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (c < 0x20) k += (size_t)snprintf(out + k, len - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    if (k + 1 < len) out[k++] = '"';
    out[k] = 0;
}

/* The value of "key" in a flat object: a string (unescaped) or a number. */
bool ee_json_get(const char *js, const char *key, char *out, size_t len)
{
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    size_t k = 0;
    if (*p != '"') {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && k + 1 < len) out[k++] = *p++;
        out[k] = 0;
        return k > 0;
    }
    for (p++; *p && *p != '"' && k + 1 < len; p++) {
        if (*p != '\\') { out[k++] = *p; continue; }
        if (k + 4 >= len) break;
        p++;
        if (*p == 'n') out[k++] = '\n';
        else if (*p == 't') out[k++] = ' ';
        else if (*p == 'u') {
            unsigned v = (unsigned)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
            p += 4;
            if (v < 0x80) out[k++] = (char)(v < 0x20 ? ' ' : v);
            else if (v < 0x800) { out[k++] = (char)(0xC0 | v >> 6); out[k++] = (char)(0x80 | (v & 0x3F)); }
            else { out[k++] = (char)(0xE0 | v >> 12); out[k++] = (char)(0x80 | ((v >> 6) & 0x3F)); out[k++] = (char)(0x80 | (v & 0x3F)); }
        } else if (*p) out[k++] = *p;
        else break;
    }
    out[k] = 0;
    return true;
}

char *ee_read_small(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

static bool write_json(const char *chip, const char *file, const ee_ver_t *v)
{
    char path[200];
    json_path(chip, file, path, sizeof path);
    char note[220], src[40];
    ee_json_str(note, sizeof note, v->note);
    ee_json_str(src, sizeof src, v->source);
    char body[700];
    snprintf(body, sizeof body,
             "{\"chip\":\"%s\",\"size\":%u,\"date\":\"%s\",\"source\":%s,\"note\":%s,"
             "\"crc32\":\"%08x\",\"md5\":\"%s\",\"sha256\":\"%s\"}\n",
             chip, (unsigned)v->size, v->date, src, note,
             (unsigned)v->crc, v->md5, v->sha);
    char tmp[208];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fputs(body, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) unlink(tmp);
    return ok;
}

bool ee_ver_meta(const char *chip, const char *file, ee_ver_t *v)
{
    memset(v, 0, sizeof *v);
    snprintf(v->file, sizeof v->file, "%s", file);
    char path[200];
    ee_ver_path(chip, file, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) return false;
    v->size = (uint32_t)st.st_size;
    json_path(chip, file, path, sizeof path);
    char *js = ee_read_small(path, 2048);
    if (!js) return true;               /* a .bin dropped there by hand */
    char val[100];
    if (ee_json_get(js, "date", val, sizeof val)) snprintf(v->date, sizeof v->date, "%.19s", val);
    if (ee_json_get(js, "source", val, sizeof val)) snprintf(v->source, sizeof v->source, "%.15s", val);
    ee_json_get(js, "note", v->note, sizeof v->note);
    if (ee_json_get(js, "crc32", val, sizeof val)) v->crc = (uint32_t)strtoul(val, NULL, 16);
    ee_json_get(js, "md5", v->md5, sizeof v->md5);
    ee_json_get(js, "sha256", v->sha, sizeof v->sha);
    free(js);
    return true;
}

bool ee_ver_set_note(const char *chip, const char *file, const char *note)
{
    ee_ver_t v;
    if (!ee_ver_meta(chip, file, &v)) return false;
    snprintf(v.note, sizeof v.note, "%s", note ? note : "");
    return write_json(chip, file, &v);
}

/* -------------------------------------------------------------------------- */
/* The list                                                                    */
/* -------------------------------------------------------------------------- */

static bool is_bin(const char *n)
{
    size_t l = strlen(n);
    return n[0] != '.' && l > 4 && !strcmp(n + l - 4, ".bin");
}

static int by_name_desc(const void *a, const void *b)
{
    return strcmp(((const ee_ver_t *)b)->file, ((const ee_ver_t *)a)->file);
}

int ee_ver_list(const char *chip, ee_ver_t *out, int max)
{
    char dir[200];
    if (!ee_ver_dir(chip, dir, sizeof dir)) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!is_bin(e->d_name) || strlen(e->d_name) >= sizeof out[0].file) continue;
        if (ee_ver_meta(chip, e->d_name, &out[n])) n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], by_name_desc);
    return n;
}

/* The newest version's name but 'except', "" if none. */
static void newest(const char *chip, const char *except, char *out, size_t len)
{
    out[0] = 0;
    char dir[200];
    if (!ee_ver_dir(chip, dir, sizeof dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_bin(e->d_name) || !strcmp(e->d_name, except)) continue;
        if (strcmp(e->d_name, out) > 0) snprintf(out, len, "%s", e->d_name);
    }
    closedir(d);
}

/* -------------------------------------------------------------------------- */
/* Writing one                                                                 */
/* -------------------------------------------------------------------------- */

static void stamp(char *out, size_t len, char *date, size_t dlen)
{
    if (aos_hal_time_is_valid()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(out, len, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        snprintf(date, dlen, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(out, len, "sinhora-%06u", (unsigned)(aos_hal_uptime_ms() / 1000));
        date[0] = 0;
    }
}

bool ee_vw_open(ee_vw_t *w, const char *chip, const char *source)
{
    memset(w, 0, sizeof *w);
    char dir[200];
    if (!ee_ver_dir(chip, dir, sizeof dir)) return false;
    snprintf(w->chip, sizeof w->chip, "%s", chip);
    char st[32], date[20];
    stamp(st, sizeof st, date, sizeof date);
    struct stat sb;
    for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(w->path, sizeof w->path, "%.140s/%.24s-%.15s.bin", dir, st, source);
        else snprintf(w->path, sizeof w->path, "%.140s/%.24s-%.15s-%d.bin", dir, st, source, k);
        if (stat(w->path, &sb) != 0) break;
    }
    w->f = fopen(w->path, "wb");
    if (!w->f) return false;
    ee_hash_init(&w->h);
    return true;
}

bool ee_vw_put(ee_vw_t *w, const uint8_t *p, size_t n)
{
    if (!w->f) return false;
    ee_hash_update(&w->h, p, n);
    w->n += (uint32_t)n;
    return fwrite(p, 1, n, (FILE *)w->f) == n;
}

void ee_vw_abort(ee_vw_t *w)
{
    if (w->f) fclose((FILE *)w->f);
    w->f = NULL;
    if (w->path[0]) unlink(w->path);
}

bool ee_vw_close(ee_vw_t *w, const char *source, const char *note, char *file, size_t len, bool *same)
{
    *same = false;
    if (!w->f) return false;
    bool ok = fclose((FILE *)w->f) == 0;
    w->f = NULL;
    if (!ok) { unlink(w->path); return false; }
    ee_sums_t s;
    ee_hash_final(&w->h, &s);
    const char *base = strrchr(w->path, '/');
    base = base ? base + 1 : w->path;

    char last[64];
    newest(w->chip, base, last, sizeof last);
    ee_ver_t prev;
    if (last[0] && ee_ver_meta(w->chip, last, &prev) && prev.size == w->n && !strcmp(prev.sha, s.sha)) {
        unlink(w->path);
        snprintf(file, len, "%s", last);
        *same = true;
        return true;
    }
    ee_ver_t v;
    memset(&v, 0, sizeof v);
    char st[32];
    stamp(st, sizeof st, v.date, sizeof v.date);
    snprintf(v.source, sizeof v.source, "%s", source);
    snprintf(v.note, sizeof v.note, "%s", note ? note : "");
    v.size = w->n;
    v.crc = s.crc;
    memcpy(v.md5, s.md5, sizeof v.md5);
    memcpy(v.sha, s.sha, sizeof v.sha);
    snprintf(file, len, "%s", base);
    if (!write_json(w->chip, base, &v)) return false;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Reading and deleting one                                                    */
/* -------------------------------------------------------------------------- */

uint8_t *ee_ver_load(const char *chip, const char *file, uint32_t *size)
{
    char path[200];
    ee_ver_path(chip, file, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 && n <= 32L * 1024 * 1024 ? ee_alloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *size = (uint32_t)n;
    return b;
}

bool ee_ver_delete(const char *chip, const char *file)
{
    char path[200];
    ee_ver_path(chip, file, path, sizeof path);
    bool ok = unlink(path) == 0;
    json_path(chip, file, path, sizeof path);
    unlink(path);
    return ok;
}
