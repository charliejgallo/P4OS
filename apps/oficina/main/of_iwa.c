/*
 * P4OS - Office: the text inside Apple's Pages, Numbers and Keynote files
 * (IWA: Snappy-compressed chunks of protobuf messages, Apple's own).
 *
 * No schema is needed for the text: every archive in an .iwa file says its
 * messages' types and lengths; the text storages (TSWP.StorageArchive,
 * type 2001) keep their text in field 3, and the tables' string lists
 * (TST.TableDataList, type 6005) their strings in field 3 of each entry.
 * Pages: the document's storages, in order. Keynote: each slide file's.
 * Numbers: the strings of its tables. Formatting and places are not read.
 */
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))

static bool varint(const uint8_t *p, size_t n, size_t *pos, uint64_t *v)
{
    *v = 0;
    for (int sh = 0; *pos < n && sh < 64; sh += 7) {
        uint8_t b = p[(*pos)++];
        *v |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80)) return true;
    }
    return false;
}

/* raw Snappy (no framing) */
static bool snappy(const uint8_t *in, size_t n, of_sb_t *out)
{
    size_t p = 0;
    uint64_t total;
    if (!varint(in, n, &p, &total) || total > 64u * 1024 * 1024) return false;
    size_t base = out->n;
    while (p < n) {
        uint8_t tag = in[p++];
        size_t len, off;
        switch (tag & 3) {
        case 0:
            len = (size_t)(tag >> 2) + 1;
            if (len > 60) {
                int k = (int)len - 60;
                if (p + (size_t)k > n) return false;
                len = 0;
                for (int i = 0; i < k; i++) len |= (size_t)in[p + i] << (8 * i);
                len++;
                p += (size_t)k;
            }
            if (p + len > n) return false;
            of_sb_put(out, (const char *)in + p, len);
            p += len;
            continue;
        case 1:
            if (p >= n) return false;
            len = ((tag >> 2) & 7) + 4;
            off = ((size_t)(tag >> 5) << 8) | in[p++];
            break;
        case 2:
            if (p + 2 > n) return false;
            len = (size_t)(tag >> 2) + 1;
            off = (size_t)in[p] | (size_t)in[p + 1] << 8;
            p += 2;
            break;
        default:
            if (p + 4 > n) return false;
            len = (size_t)(tag >> 2) + 1;
            off = (size_t)in[p] | (size_t)in[p + 1] << 8 | (size_t)in[p + 2] << 16 | (size_t)in[p + 3] << 24;
            p += 4;
            break;
        }
        if (!off || off > out->n - base) return false;
        for (size_t i = 0; i < len; i++) of_sb_putc(out, out->s[out->n - off]);
        if (out->oom) return false;
    }
    return true;
}

/* an .iwa file's chunks, decompressed */
static bool iwa_inflate(const uint8_t *d, size_t n, of_sb_t *out)
{
    size_t p = 0;
    while (p + 4 <= n) {
        size_t len = (size_t)d[p + 1] | (size_t)d[p + 2] << 8 | (size_t)d[p + 3] << 16;
        p += 4;
        if (p + len > n) len = n - p;
        if (d[p - 4] == 0 && !snappy(d + p, len, out)) return out->n > 0;
        p += len;
    }
    return true;
}

typedef void (*str_cb_t)(void *ud, int type, const char *s, size_t n);

/* the strings of field 3 of a message (and, for string lists, of their entries) */
static void fields3(const uint8_t *m, size_t n, int type, int depth, str_cb_t cb, void *ud)
{
    size_t p = 0;
    while (p < n) {
        uint64_t key;
        if (!varint(m, n, &p, &key)) return;
        int f = (int)(key >> 3), wt = (int)(key & 7);
        uint64_t v;
        if (wt == 0) { if (!varint(m, n, &p, &v)) return; }
        else if (wt == 1) p += 8;
        else if (wt == 5) p += 4;
        else if (wt == 2) {
            if (!varint(m, n, &p, &v) || p + v > n) return;
            if (f == 3 && type == 2001) cb(ud, type, (const char *)m + p, (size_t)v);
            else if (type == 6005 && depth == 0 && f == 3) fields3(m + p, (size_t)v, 60051, 1, cb, ud);
            else if (type == 60051 && f == 3) cb(ud, 6005, (const char *)m + p, (size_t)v);
            p += (size_t)v;
        } else {
            return;
        }
    }
}

/* every archive of a decompressed .iwa: its messages by type */
static void archives(const uint8_t *d, size_t n, str_cb_t cb, void *ud)
{
    size_t p = 0;
    while (p < n) {
        uint64_t alen;
        if (!varint(d, n, &p, &alen) || p + alen > n) return;
        const uint8_t *a = d + p;
        size_t ap = 0, an = (size_t)alen;
        p += (size_t)alen;
        /* ArchiveInfo: 2 = MessageInfo { 1 type, 3 length } */
        while (ap < an) {
            uint64_t key, v;
            if (!varint(a, an, &ap, &key)) return;
            if ((key & 7) == 0) { if (!varint(a, an, &ap, &v)) return; continue; }
            if ((key & 7) != 2) return;
            if (!varint(a, an, &ap, &v) || ap + v > an) return;
            if ((key >> 3) == 2) {
                const uint8_t *mi = a + ap;
                size_t mp = 0, mn = (size_t)v;
                uint64_t type = 0, mlen = 0;
                while (mp < mn) {
                    uint64_t k2, v2;
                    if (!varint(mi, mn, &mp, &k2)) break;
                    if ((k2 & 7) == 0) {
                        if (!varint(mi, mn, &mp, &v2)) break;
                        if ((k2 >> 3) == 1) type = v2;
                        else if ((k2 >> 3) == 3) mlen = v2;
                    } else if ((k2 & 7) == 2) {
                        if (!varint(mi, mn, &mp, &v2) || mp + v2 > mn) break;
                        mp += (size_t)v2;
                    } else {
                        break;
                    }
                }
                if (p + mlen > n) return;
                if (type == 2001 || type == 6005) fields3(d + p, (size_t)mlen, (int)type, 0, cb, ud);
                p += (size_t)mlen;
            }
            ap += (size_t)v;
        }
    }
}

typedef struct {
    of_bld_t *b;
    int       chars;
    int       cells;
    bool      numbers;
} ctx_t;

static void text_cb(void *ud, int type, const char *s, size_t n)
{
    ctx_t *X = ud;
    if (!of_utf8_valid(s, n)) return;
    if (type == 6005) {
        /* a table's string: one line each */
        if (!X->numbers || X->cells > 5000 || !n) return;
        X->b->cur = -1;
        X->b->list = OF_LIST_BULLET;
        of_bld_para(X->b);
        of_bld_text(X->b, s, n);
        X->b->list = 0;
        X->cells++;
        return;
    }
    /* paragraphs by line ends; the objects' marks out */
    size_t from = 0;
    X->b->cur = -1;
    for (size_t i = 0; i <= n;) {
        int l = 1;
        uint32_t cp = i < n ? of_utf8_get(s + i, &l) : '\n';
        bool sep = cp == '\n' || cp == '\r' || cp == 0x2029 || cp == 0x2028 || cp == 0x0B;
        bool drop = cp == 0xFFFC || (cp < 32 && cp != '\t' && !sep);
        if (sep || drop) {
            if (i > from) {
                if (X->b->cur < 0) of_bld_para(X->b);
                of_bld_text(X->b, s + from, i - from);
                X->chars += (int)(i - from);
            }
            if (sep) X->b->cur = -1;
            from = i + (size_t)l;
        }
        if (i >= n) break;
        i += (size_t)l;
    }
}

static void one_iwa(const uint8_t *d, size_t n, ctx_t *X)
{
    of_sb_t raw = { 0 };
    if (iwa_inflate(d, n, &raw) && raw.n) archives((const uint8_t *)raw.s, raw.n, text_cb, X);
    of_sb_free(&raw);
}

static bool is_slide(const char *name)
{
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    return !strncmp(b, "Slide", 5) && strstr(b, ".iwa");
}

/* the archives of a package (a zip of Index/ files, or one holding Index.zip) */
static void from_zip(const of_zip_t *z, ctx_t *X, bool keynote, int depth)
{
    int idx = of_zip_find(z, "Index.zip");
    if (idx >= 0 && depth == 0) {
        size_t n;
        uint8_t *inner = of_zip_read(z, idx, &n);
        of_zip_t z2;
        if (inner && of_zip_open(&z2, inner, n)) { from_zip(&z2, X, keynote, 1); of_zip_close(&z2); }
        of_free(inner);
        return;
    }
    const char *first = keynote ? NULL : "Index/Document.iwa";
    if (first) {
        int i = of_zip_find(z, first);
        if (i < 0) i = of_zip_find(z, "Document.iwa");
        if (i >= 0) {
            size_t n;
            uint8_t *d = of_zip_read(z, i, &n);
            if (d) { one_iwa(d, n, X); of_free(d); }
        }
    }
    int slide = 0;
    for (int i = 0; i < z->n; i++) {
        const char *nm = z->e[i].name;
        if (!strstr(nm, ".iwa")) continue;
        bool sl = is_slide(nm);
        bool tables = strstr(nm, "Tables/") || strstr(nm, "DataList") || strstr(nm, "CalculationEngine") == NULL;
        if (keynote && sl) {
            X->b->heading = 2;
            X->b->cur = -1;
            of_bld_para(X->b);
            char h[48];
            snprintf(h, sizeof h, "%s %d", _("Diapositiva"), ++slide);
            of_bld_text(X->b, h, strlen(h));
            X->b->heading = 0;
            X->b->cur = -1;
            size_t n;
            uint8_t *d = of_zip_read(z, i, &n);
            if (d) { one_iwa(d, n, X); of_free(d); }
        } else if (X->numbers && tables && !strstr(nm, "Document.iwa")) {
            size_t n;
            uint8_t *d = of_zip_read(z, i, &n);
            if (d) { one_iwa(d, n, X); of_free(d); }
        }
    }
}

/* The text of an iWork file into the builder; false if none was found. */
bool of_iwork_text(of_bld_t *b, const of_zip_t *z, const char *folder, const char *ext)
{
    ctx_t X = { .b = b, .numbers = !strcmp(ext, "numbers") };
    bool keynote = !strcmp(ext, "key");
    if (z && z->n) {
        from_zip(z, &X, keynote, 0);
    } else if (folder) {
        /* a package copied as a folder: Index.zip, or Index/ */
        char p[400];
        snprintf(p, sizeof p, "%.300s/Index.zip", folder);
        size_t n;
        uint8_t *iz = of_file_read(p, &n, 64u * 1024 * 1024);
        of_zip_t zz;
        if (iz && of_zip_open(&zz, iz, n)) { from_zip(&zz, &X, keynote, 1); of_zip_close(&zz); }
        of_free(iz);
        if (!X.chars && !X.cells) {
            snprintf(p, sizeof p, "%.300s/Index/Document.iwa", folder);
            uint8_t *d = keynote ? NULL : of_file_read(p, &n, 64u * 1024 * 1024);
            if (d) { one_iwa(d, n, &X); of_free(d); }
        }
    }
    return X.chars > 0 || X.cells > 0;
}
