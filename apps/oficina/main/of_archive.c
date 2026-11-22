/*
 * P4OS - Office: archives (ZIP, CBZ, TAR, TAR.GZ, GZ), listed and extracted.
 *
 * A ZIP is read whole (up to ARC_MAX) and its entries inflated one at a
 * time. A TAR is walked on the card, header after header, and its files
 * copied out without loading them; a compressed TAR is inflated whole first
 * (up to ARC_MAX). Names: ustar's prefix and name, GNU long names ('L') and
 * pax paths ('x'). Encrypted ZIP entries are listed but not extracted.
 */
#include "of_arc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define ARC_MAX (64u * 1024 * 1024)

static void add(of_arc_t *a, const char *name, size_t nl, uint64_t size, uint64_t off, int idx, bool dir, uint32_t mtime)
{
    if (!nl) return;
    of_aent_t *n = of_realloc(a->e, (size_t)(a->n + 1) * sizeof *n);
    if (!n) return;
    a->e = n;
    of_aent_t *e = &a->e[a->n];
    memset(e, 0, sizeof *e);
    /* names in UTF-8: old ZIPs write them in a DOS code page */
    if (of_utf8_valid(name, nl)) {
        e->name = of_alloc(nl + 1);
        if (e->name) memcpy(e->name, name, nl);
    } else {
        e->name = of_latin1_to_utf8(name, nl, NULL);
    }
    if (!e->name) return;
    for (char *p = e->name; *p; p++) if (*p == '\\') *p = '/';
    size_t l = strlen(e->name);
    if (l && e->name[l - 1] == '/') { e->name[l - 1] = 0; dir = true; }
    /* "./a.txt" and "/a.txt" are a.txt */
    while (!strncmp(e->name, "./", 2)) memmove(e->name, e->name + 2, strlen(e->name + 2) + 1);
    while (e->name[0] == '/') memmove(e->name, e->name + 1, strlen(e->name));
    if (!e->name[0] || strstr(e->name, "../")) { of_free(e->name); return; }
    e->size = size;
    e->off = off;
    e->idx = idx;
    e->dir = dir;
    e->mtime = mtime;
    a->n++;
}

static uint64_t octal(const char *s, size_t n)
{
    /* base-256 for big sizes */
    if ((uint8_t)s[0] & 0x80) {
        uint64_t v = (uint8_t)s[0] & 0x7F;
        for (size_t i = 1; i < n; i++) v = v << 8 | (uint8_t)s[i];
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] == ' ') continue;
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (uint64_t)(s[i] - '0');
    }
    return v;
}

/* The TAR headers, from a buffer (inflated) or a file (f). */
static bool tar_walk(of_arc_t *a, const uint8_t *buf, size_t len, FILE *f)
{
    uint8_t h[512];
    uint64_t pos = 0;
    char *longname = NULL;
    int zeros = 0;
    for (;;) {
        if (buf) {
            if (pos + 512 > len) break;
            memcpy(h, buf + pos, 512);
        } else {
            if (fseek(f, (long)pos, SEEK_SET) || fread(h, 1, 512, f) != 512) break;
        }
        bool empty = true;
        for (int i = 0; i < 512 && empty; i++) if (h[i]) empty = false;
        if (empty) { if (++zeros == 2) break; pos += 512; continue; }
        zeros = 0;
        /* the checksum, or this is no TAR */
        uint64_t sum = octal((const char *)h + 148, 8), calc = 0;
        for (int i = 0; i < 512; i++) calc += (i >= 148 && i < 156) ? ' ' : h[i];
        if (sum != calc) { of_free(longname); return a->n > 0; }
        uint64_t size = octal((const char *)h + 124, 12);
        uint32_t mtime = (uint32_t)octal((const char *)h + 136, 12);
        char type = (char)h[156];
        uint64_t data = pos + 512;
        uint64_t next = data + (size + 511) / 512 * 512;
        if (type == 'L' || type == 'x') {
            /* a long name or pax records for the next entry */
            size_t n = size > 4096 ? 4096 : (size_t)size;
            char *t = of_alloc(n + 1);
            if (t) {
                if (buf) memcpy(t, buf + data, data + n <= len ? n : 0);
                else { fseek(f, (long)data, SEEK_SET); n = fread(t, 1, n, f); }
                t[n] = 0;
                of_free(longname);
                longname = NULL;
                if (type == 'L') longname = t;
                else {
                    /* "len path=value\n" */
                    char *p = strstr(t, " path=");
                    if (p) {
                        p += 6;
                        char *e = strchr(p, '\n');
                        if (e) *e = 0;
                        longname = of_strdup(p);
                    }
                    of_free(t);
                }
            }
            pos = next;
            continue;
        }
        char name[260];
        if (longname) {
            snprintf(name, sizeof name, "%s", longname);
            of_free(longname);
            longname = NULL;
        } else if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h + 0);
        } else {
            snprintf(name, sizeof name, "%.100s", (const char *)h);
        }
        if (type == '0' || type == 0 || type == '7') add(a, name, strlen(name), size, data, -1, false, mtime);
        else if (type == '5') add(a, name, strlen(name), 0, 0, -1, true, mtime);
        pos = next;
    }
    of_free(longname);
    return true;
}

/* gzip: the deflate stream after its header; the name it carries */
static uint8_t *gunzip(const uint8_t *g, size_t n, size_t *out, char *name, size_t nlen)
{
    if (n < 18 || g[0] != 0x1F || g[1] != 0x8B || g[2] != 8) return NULL;
    uint8_t fl = g[3];
    size_t p = 10;
    if (fl & 4) { if (p + 2 > n) return NULL; p += 2 + (size_t)(g[p] | g[p + 1] << 8); }
    if (fl & 8) {
        size_t s = p;
        while (p < n && g[p]) p++;
        if (name) snprintf(name, nlen, "%.*s", (int)(p - s), (const char *)g + s);
        p++;
    }
    if (fl & 16) { while (p < n && g[p]) p++; p++; }
    if (fl & 2) p += 2;
    if (p >= n) return NULL;
    size_t isize = (size_t)(g[n - 4] | g[n - 3] << 8 | g[n - 2] << 16 | (uint32_t)g[n - 1] << 24);
    if (isize > ARC_MAX) return NULL;
    return of_inflate(g + p, n - 8 - p, isize, out);
}

of_arc_t *of_arc_open(const char *path, char *err, size_t errlen)
{
    of_arc_t *a = of_alloc(sizeof *a);
    if (!a) { snprintf(err, errlen, "memory"); return NULL; }
    snprintf(a->path, sizeof a->path, "%s", path);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "read"); of_free(a); return NULL; }
    uint8_t head[512];
    size_t hn = fread(head, 1, sizeof head, f);
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    if (hn >= 4 && !memcmp(head, "PK", 2)) {
        fclose(f);
        if ((unsigned long)flen > ARC_MAX) { snprintf(err, errlen, "big"); of_free(a); return NULL; }
        size_t n;
        a->buf = of_file_read(path, &n, ARC_MAX);
        if (!a->buf || !of_zip_open(&a->zip, a->buf, n)) { snprintf(err, errlen, "damaged"); of_arc_free(a); return NULL; }
        a->kind = OF_ARC_ZIP;
        for (int i = 0; i < a->zip.n; i++) {
            const of_zentry_t *e = &a->zip.e[i];
            add(a, e->name, strlen(e->name), e->usize, 0, i, false, 0);
            if (a->n && a->e[a->n - 1].idx == i) a->e[a->n - 1].locked = (e->flags & 1) != 0;
        }
        return a;
    }
    if (hn >= 3 && head[0] == 0x1F && head[1] == 0x8B) {
        fclose(f);
        if ((unsigned long)flen > ARC_MAX) { snprintf(err, errlen, "big"); of_free(a); return NULL; }
        size_t n;
        uint8_t *g = of_file_read(path, &n, ARC_MAX);
        if (!g) { snprintf(err, errlen, "read"); of_free(a); return NULL; }
        char inner[200] = "";
        a->buf = gunzip(g, n, &a->len, inner, sizeof inner);
        of_free(g);
        if (!a->buf) { snprintf(err, errlen, "damaged"); of_free(a); return NULL; }
        if (a->len > 262 && !memcmp(a->buf + 257, "ustar", 5)) {
            a->kind = OF_ARC_TGZ;
            tar_walk(a, a->buf, a->len, NULL);
        } else {
            /* one file: its own name, or the archive's without .gz */
            a->kind = OF_ARC_GZ;
            if (!inner[0]) {
                snprintf(inner, sizeof inner, "%s", of_basename(path));
                char *dot = strrchr(inner, '.');
                if (dot && !strcasecmp(dot, ".gz")) *dot = 0;
            }
            add(a, of_basename(inner), strlen(of_basename(inner)), a->len, 0, -1, false, 0);
        }
        return a;
    }
    if (hn >= 262 && !memcmp(head + 257, "ustar", 5)) {
        a->kind = OF_ARC_TAR;
        tar_walk(a, NULL, 0, f);
        fclose(f);
        return a;
    }
    /* an old TAR without "ustar": its checksum tells */
    if (hn == 512) {
        a->kind = OF_ARC_TAR;
        tar_walk(a, NULL, 0, f);
        fclose(f);
        if (a->n) return a;
    } else {
        fclose(f);
    }
    snprintf(err, errlen, "format");
    of_arc_free(a);
    return NULL;
}

void of_arc_free(of_arc_t *a)
{
    if (!a) return;
    for (int i = 0; i < a->n; i++) of_free(a->e[i].name);
    of_free(a->e);
    of_zip_close(&a->zip);
    of_free(a->buf);
    of_free(a);
}

static bool mkdirs(const char *path)
{
    char p[400];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        mkdir(p, 0777);
        *s = '/';
    }
    return true;
}

bool of_arc_extract(of_arc_t *a, int i, const char *out, char *err, size_t errlen)
{
    if (i < 0 || i >= a->n || a->e[i].dir) { snprintf(err, errlen, "entry"); return false; }
    of_aent_t *e = &a->e[i];
    if (e->locked) { snprintf(err, errlen, "encrypted"); return false; }
    mkdirs(out);
    if (a->kind == OF_ARC_ZIP) {
        size_t n;
        uint8_t *d = of_zip_read(&a->zip, e->idx, &n);
        if (!d) { snprintf(err, errlen, "damaged"); return false; }
        bool ok = of_file_write(out, d, n);
        of_free(d);
        if (!ok) snprintf(err, errlen, "write");
        return ok;
    }
    if (a->kind == OF_ARC_GZ || a->kind == OF_ARC_TGZ) {
        if (e->off + e->size > a->len) { snprintf(err, errlen, "damaged"); return false; }
        bool ok = of_file_write(out, a->buf + e->off, (size_t)e->size);
        if (!ok) snprintf(err, errlen, "write");
        return ok;
    }
    /* TAR on the card: copied in pieces */
    FILE *f = fopen(a->path, "rb");
    if (!f) { snprintf(err, errlen, "read"); return false; }
    char tmp[420];
    snprintf(tmp, sizeof tmp, "%s.part", out);
    FILE *o = fopen(tmp, "wb");
    if (!o) { fclose(f); snprintf(err, errlen, "write"); return false; }
    fseek(f, (long)e->off, SEEK_SET);
    uint8_t *buf = of_alloc(32768);
    uint64_t left = e->size;
    bool ok = buf != NULL;
    while (ok && left) {
        size_t k = left > 32768 ? 32768 : (size_t)left;
        if (fread(buf, 1, k, f) != k || fwrite(buf, 1, k, o) != k) ok = false;
        left -= k;
    }
    of_free(buf);
    fclose(f);
    if (fclose(o) != 0) ok = false;
    if (ok) { remove(out); ok = rename(tmp, out) == 0; }
    if (!ok) { remove(tmp); snprintf(err, errlen, "write"); }
    return ok;
}

bool of_is_arc_ext(const char *e)
{
    return !strcmp(e, "zip") || !strcmp(e, "cbz") || !strcmp(e, "tar") || !strcmp(e, "gz") || !strcmp(e, "tgz");
}
