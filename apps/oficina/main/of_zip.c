/*
 * P4OS - Office: ZIP packages, read and written.
 *
 * Every modern office file is a ZIP of XML parts. Reading follows the
 * Emulador's em_zip.c: the central directory at the end, stored or deflated
 * entries. The file is in memory whole (they are a few MB at most), so a
 * part is inflated straight from it.
 *
 * Writing builds a new package in memory, entry by entry, in the original
 * order (ODF wants "mimetype" first and stored, and it already is): an
 * entry nobody touched is copied with its compressed bytes as they were,
 * under a fresh local header that carries its CRC and sizes (so a data
 * descriptor of the original is not needed); an edited part is deflated
 * again. Then the central directory, and the file goes onto the card in one
 * piece through of_file_write().
 *
 * No ZIP64 (no office file comes near 4 GB) and no encryption.
 */
#include "of.h"

#include <string.h>
#include <time.h>

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static void wr16(of_sb_t *b, uint32_t v)
{
    char c[2] = { (char)(v & 0xFF), (char)(v >> 8 & 0xFF) };
    of_sb_put(b, c, 2);
}

static void wr32(of_sb_t *b, uint32_t v)
{
    wr16(b, v & 0xFFFF);
    wr16(b, v >> 16);
}

bool of_zip_open(of_zip_t *z, const uint8_t *buf, size_t len)
{
    memset(z, 0, sizeof *z);
    if (len < 22) return false;
    size_t from = len > 65557 ? len - 65557 : 0;
    long eocd = -1;
    for (size_t i = len - 22 + 1; i-- > from;) {
        if (rd32(buf + i) == 0x06054b50u) {
            eocd = (long)i;
            break;
        }
    }
    if (eocd < 0) return false;
    uint32_t count = rd16(buf + eocd + 10);
    uint32_t cd = rd32(buf + eocd + 16);
    if (cd >= len) return false;
    z->buf = buf;
    z->len = len;
    z->e = of_alloc((count ? count : 1) * sizeof(of_zentry_t));
    if (!z->e) return false;
    size_t p = cd;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > len || rd32(buf + p) != 0x02014b50u) break;
        const uint8_t *h = buf + p;
        uint32_t nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        if (p + 46 + nl > len) break;
        of_zentry_t *e = &z->e[z->n];
        e->ver_made = (uint16_t)rd16(h + 4);
        e->flags = (uint16_t)rd16(h + 8);
        e->method = (uint16_t)rd16(h + 10);
        e->mtime = (uint16_t)rd16(h + 12);
        e->mdate = (uint16_t)rd16(h + 14);
        e->crc = rd32(h + 16);
        e->csize = rd32(h + 20);
        e->usize = rd32(h + 24);
        e->ext_attr = rd32(h + 38);
        e->local = rd32(h + 42);
        e->name = of_strndup((const char *)h + 46, nl);
        p += 46 + nl + xl + cl;
        /* where the data starts: after the local header's own name and extra */
        if (e->local + 30 > len || rd32(buf + e->local) != 0x04034b50u) {
            of_free(e->name);
            continue;
        }
        uint32_t lnl = rd16(buf + e->local + 26), lxl = rd16(buf + e->local + 28);
        e->data = e->local + 30 + lnl + lxl;
        if ((size_t)e->data + e->csize > len || e->csize == 0xFFFFFFFFu) {
            of_free(e->name);
            continue;
        }
        z->n++;
    }
    return z->n > 0;
}

void of_zip_close(of_zip_t *z)
{
    for (int i = 0; i < z->n; i++) of_free(z->e[i].name);
    of_free(z->e);
    memset(z, 0, sizeof *z);
}

int of_zip_find(const of_zip_t *z, const char *name)
{
    if (name[0] == '/') name++;
    for (int i = 0; i < z->n; i++)
        if (strcmp(z->e[i].name, name) == 0) return i;
    /* some writers differ in case */
    for (int i = 0; i < z->n; i++)
        if (strcasecmp(z->e[i].name, name) == 0) return i;
    return -1;
}

uint8_t *of_zip_read(const of_zip_t *z, int i, size_t *len)
{
    if (i < 0 || i >= z->n) return NULL;
    const of_zentry_t *e = &z->e[i];
    if (e->method == 0) {
        uint8_t *o = of_alloc(e->csize + 1);
        if (!o) return NULL;
        memcpy(o, z->buf + e->data, e->csize);
        if (len) *len = e->csize;
        return o;
    }
    if (e->method != 8) return NULL;
    size_t n = 0;
    uint8_t *o = of_inflate(z->buf + e->data, e->csize, e->usize, &n);
    if (o && len) *len = n;
    return o;
}

/* -------------------------------------------------------------------------- */
/* Writing                                                                     */
/* -------------------------------------------------------------------------- */

void of_zipw_init(of_zipw_t *w)
{
    memset(w, 0, sizeof *w);
    w->ok = true;
}

static void dos_now(uint16_t *t, uint16_t *d)
{
    time_t now = time(NULL);
    struct tm tm;
    if (now < 1600000000 || !localtime_r(&now, &tm)) {
        *t = 0;
        *d = (0 << 9) | (1 << 5) | 1;       /* 1980-01-01 */
        return;
    }
    *t = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *d = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

static void entry(of_zipw_t *w, const char *name, uint16_t method, uint16_t mtime, uint16_t mdate,
                  uint32_t crc, uint32_t csize, uint32_t usize, const uint8_t *data,
                  uint16_t flags, uint32_t ext_attr)
{
    size_t nl = strlen(name);
    uint32_t local = (uint32_t)w->out.n;
    flags &= 0x0800;            /* keep only "the name is UTF-8" */
    /* local header */
    wr32(&w->out, 0x04034b50u);
    wr16(&w->out, method == 8 ? 20 : 10);
    wr16(&w->out, flags);
    wr16(&w->out, method);
    wr16(&w->out, mtime);
    wr16(&w->out, mdate);
    wr32(&w->out, crc);
    wr32(&w->out, csize);
    wr32(&w->out, usize);
    wr16(&w->out, (uint32_t)nl);
    wr16(&w->out, 0);
    of_sb_put(&w->out, name, nl);
    of_sb_put(&w->out, (const char *)data, csize);
    /* its central directory record */
    wr32(&w->cd, 0x02014b50u);
    wr16(&w->cd, 20);
    wr16(&w->cd, method == 8 ? 20 : 10);
    wr16(&w->cd, flags);
    wr16(&w->cd, method);
    wr16(&w->cd, mtime);
    wr16(&w->cd, mdate);
    wr32(&w->cd, crc);
    wr32(&w->cd, csize);
    wr32(&w->cd, usize);
    wr16(&w->cd, (uint32_t)nl);
    wr16(&w->cd, 0);
    wr16(&w->cd, 0);
    wr16(&w->cd, 0);
    wr16(&w->cd, 0);
    wr32(&w->cd, ext_attr);
    wr32(&w->cd, local);
    of_sb_put(&w->cd, name, nl);
    w->count++;
    if (w->out.oom || w->cd.oom) w->ok = false;
}

bool of_zipw_copy(of_zipw_t *w, const of_zip_t *z, int i)
{
    if (!w->ok || i < 0 || i >= z->n) return false;
    const of_zentry_t *e = &z->e[i];
    entry(w, e->name, e->method, e->mtime, e->mdate, e->crc, e->csize, e->usize,
          z->buf + e->data, e->flags, (e->ver_made >> 8) == 0 ? e->ext_attr : 0);
    return w->ok;
}

bool of_zipw_add(of_zipw_t *w, const char *name, const void *data, size_t n, bool deflate)
{
    if (!w->ok) return false;
    uint16_t t, d;
    dos_now(&t, &d);
    uint32_t crc = of_crc32(0, data, n);
    of_sb_t c = { 0 };
    if (deflate && n > 64 && of_deflate(data, n, &c) && c.n < n) {
        entry(w, name, 8, t, d, crc, (uint32_t)c.n, (uint32_t)n, (const uint8_t *)c.s, 0x0800, 0);
    } else {
        entry(w, name, 0, t, d, crc, (uint32_t)n, (uint32_t)n, data, 0x0800, 0);
    }
    of_sb_free(&c);
    return w->ok;
}

bool of_zipw_finish(of_zipw_t *w, const char *path)
{
    if (!w->ok) return false;
    uint32_t cd_off = (uint32_t)w->out.n;
    of_sb_put(&w->out, w->cd.s, w->cd.n);
    wr32(&w->out, 0x06054b50u);
    wr16(&w->out, 0);
    wr16(&w->out, 0);
    wr16(&w->out, (uint32_t)w->count);
    wr16(&w->out, (uint32_t)w->count);
    wr32(&w->out, (uint32_t)w->cd.n);
    wr32(&w->out, cd_off);
    wr16(&w->out, 0);
    if (w->out.oom) return false;
    return of_file_write(path, w->out.s, w->out.n);
}

void of_zipw_free(of_zipw_t *w)
{
    of_sb_free(&w->out);
    of_sb_free(&w->cd);
}

/* -------------------------------------------------------------------------- */
/* An encrypted OOXML file is not a ZIP but a CFB (OLE2) compound file with an
 * "EncryptedPackage" stream: the name appears in UTF-16 in its directory.   */
/* -------------------------------------------------------------------------- */

bool of_is_encrypted_ooxml(const uint8_t *buf, size_t len)
{
    static const uint8_t cfb[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    if (len < 512 || memcmp(buf, cfb, 8) != 0) return false;
    static const char want[] = "EncryptedPackage";
    size_t wl = sizeof want - 1;
    for (size_t i = 0; i + wl * 2 <= len; i++) {
        size_t k = 0;
        while (k < wl && buf[i + 2 * k] == (uint8_t)want[k] && buf[i + 2 * k + 1] == 0) k++;
        if (k == wl) return true;
    }
    return false;
}
