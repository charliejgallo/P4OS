/*
 * P4OS - Logic: captures on the card, as sigrok sessions (.sr).
 *
 * A .sr is a zip: "version" ("2"), "metadata" (an INI file with the rate,
 * the channels and their names) and the samples in "logic-1-1",
 * "logic-1-2"... (unitsize bytes per sample). PulseView and sigrok-cli open
 * what the board saves, and the board opens what they save. The board
 * writes the members stored (no compression: a capture of 16 MB goes to
 * the card at its speed instead of waiting for a deflate), and reads
 * stored or deflated ones (PulseView deflates) with tinfl. One more member,
 * "p4os", keeps what sigrok has no place for: where the trigger was.
 *
 * The decoded table goes beside it as a .csv when asked (la_csv_save).
 */
#include "la.h"
#include "la_tinfl.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK (4u * 1024 * 1024)

/* ---- CRC-32 (zip's) ---- */

static uint32_t s_crc_tab[256];

uint32_t la_crc32(uint32_t crc, const void *p, size_t n)
{
    if (!s_crc_tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            s_crc_tab[i] = c;
        }
    }
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) crc = s_crc_tab[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- writing ---- */

typedef struct {
    char name[16];
    uint32_t crc, size, off;
} member_t;

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static bool put_member(FILE *f, member_t *m, const char *name, const void *data, uint32_t len)
{
    snprintf(m->name, sizeof m->name, "%s", name);
    m->crc = la_crc32(0, data, len);
    m->size = len;
    m->off = (uint32_t)ftell(f);
    uint8_t h[30] = { 0 };
    le32(h, 0x04034B50);
    le16(h + 4, 20);
    le16(h + 10, 0);            /* 00:00 */
    le16(h + 12, 0x21);         /* 1980-01-01 */
    le32(h + 14, m->crc);
    le32(h + 18, len);
    le32(h + 22, len);
    le16(h + 26, (uint32_t)strlen(m->name));
    if (fwrite(h, 1, 30, f) != 30 || fwrite(m->name, 1, strlen(m->name), f) != strlen(m->name)) return false;
    const uint8_t *p = data;
    uint32_t left = len;
    while (left) {
        uint32_t c = left > 32768 ? 32768 : left;
        if (fwrite(p, 1, c, f) != c) return false;
        p += c;
        left -= c;
        la_lock();
        LA.prog += c;
        la_unlock();
    }
    return true;
}

static void rate_str(uint32_t r, char *out, size_t n)
{
    if (r % 1000000 == 0) snprintf(out, n, "%u MHz", (unsigned)(r / 1000000));
    else if (r % 1000 == 0) snprintf(out, n, "%u kHz", (unsigned)(r / 1000));
    else snprintf(out, n, "%u Hz", (unsigned)r);
}

const char *la_sr_save(const char *path)
{
    const la_cap_t *c = &LA.cap;
    if (!c->buf || !c->n) return _("No hay captura para guardar");
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return _("No se pudo escribir en la tarjeta");
    la_lock();
    LA.state = LA_ST_FILE;
    LA.prog = 0;
    LA.prog_total = c->n;
    la_unlock();

    member_t m[2 + 16 + 1];
    int nm = 0;
    bool ok = put_member(f, &m[nm++], "version", "2", 1);
    char meta[1024], rs[24];
    rate_str(c->rate, rs, sizeof rs);
    int k = snprintf(meta, sizeof meta,
                     "[global]\nsigrok version=0.5.2\n\n[device 1]\ncapturefile=logic-1\ntotal probes=%d\n"
                     "samplerate=%s\ntotal analog=0\n", LA_NCH, rs);
    for (int i = 0; i < LA_NCH && k < (int)sizeof meta - 40; i++)
        k += snprintf(meta + k, sizeof meta - k, "probe%d=%s\n", i + 1, LA.chname[i][0] ? LA.chname[i] : "D");
    if (k < (int)sizeof meta - 16) snprintf(meta + k, sizeof meta - k, "unitsize=1\n");
    if (ok) ok = put_member(f, &m[nm++], "metadata", meta, (uint32_t)strlen(meta));
    uint32_t chunks = (c->n + CHUNK - 1) / CHUNK;
    for (uint32_t i = 0; ok && i < chunks && nm < 18; i++) {
        char nmb[16];
        snprintf(nmb, sizeof nmb, "logic-1-%u", (unsigned)(i + 1));
        uint32_t len = c->n - i * CHUNK > CHUNK ? CHUNK : c->n - i * CHUNK;
        ok = put_member(f, &m[nm++], nmb, c->buf + i * CHUNK, len);
    }
    char extra[160];
    snprintf(extra, sizeof extra, "trigger=%d\nsource=%s\n", (int)c->trig, c->src);
    if (ok) ok = put_member(f, &m[nm++], "p4os", extra, (uint32_t)strlen(extra));
    /* the central directory */
    uint32_t cd = (uint32_t)ftell(f), cdlen = 0;
    for (int i = 0; ok && i < nm; i++) {
        uint8_t h[46] = { 0 };
        le32(h, 0x02014B50);
        le16(h + 4, 20);
        le16(h + 6, 20);
        le16(h + 14, 0x21);
        le32(h + 16, m[i].crc);
        le32(h + 20, m[i].size);
        le32(h + 24, m[i].size);
        le16(h + 28, (uint32_t)strlen(m[i].name));
        le32(h + 42, m[i].off);
        ok = fwrite(h, 1, 46, f) == 46 && fwrite(m[i].name, 1, strlen(m[i].name), f) == strlen(m[i].name);
        cdlen += 46 + (uint32_t)strlen(m[i].name);
    }
    uint8_t e[22] = { 0 };
    le32(e, 0x06054B50);
    le16(e + 8, (uint32_t)nm);
    le16(e + 10, (uint32_t)nm);
    le32(e + 12, cdlen);
    le32(e + 16, cd);
    if (ok) ok = fwrite(e, 1, 22, f) == 22;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return _("No se pudo escribir en la tarjeta (¿llena?)");
    }
    unlink(path);
    if (rename(tmp, path) != 0) return _("No se pudo escribir en la tarjeta");
    aos_hal_log("logica", "saved %s: %u samples", path, (unsigned)c->n);
    return NULL;
}

bool la_csv_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs("t_s,end_s,decoder,error,text\n", f);
    float r = LA.cap.rate ? (float)LA.cap.rate : 1;
    for (uint32_t i = 0; i < LA.an.npkt; i++) {
        const la_pkt_t *p = &LA.an.pkt[i];
        char t[200];
        size_t k = 0;
        for (const char *s = p->t; *s && k + 2 < sizeof t; s++) {
            if (*s == '"') t[k++] = '"';
            t[k++] = *s;
        }
        t[k] = 0;
        char line[320];
        snprintf(line, sizeof line, "%.9f,%.9f,%s,%d,\"%s\"\n", (double)((float)p->s0 / r), (double)((float)p->s1 / r),
                 la_dec_name(LA.dec[p->row].type), p->err, t);
        fputs(line, f);
    }
    return fclose(f) == 0;
}

/* ---- reading ---- */

typedef struct {
    char name[24];
    uint16_t method;
    uint32_t csize, usize, off;
} entry_t;

#define ENTRIES_MAX 64

static int read_dir(FILE *f, entry_t *e, int max)
{
    if (fseek(f, 0, SEEK_END)) return -1;
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;
    uint8_t *b = la_big_alloc((size_t)tail);
    if (!b) return -1;
    fseek(f, size - tail, SEEK_SET);
    if (fread(b, 1, (size_t)tail, f) != (size_t)tail) {
        la_big_free(b);
        return -1;
    }
    long eo = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(b + i) == 0x06054B50) {
            eo = i;
            break;
        }
    if (eo < 0) {
        la_big_free(b);
        return -1;
    }
    uint32_t n = rd16(b + eo + 10), cdlen = rd32(b + eo + 12), cdoff = rd32(b + eo + 16);
    la_big_free(b);
    if (cdlen > 1024 * 1024) return -1;
    uint8_t *cd = la_big_alloc(cdlen + 1);
    if (!cd) return -1;
    fseek(f, (long)cdoff, SEEK_SET);
    if (fread(cd, 1, cdlen, f) != cdlen) {
        la_big_free(cd);
        return -1;
    }
    int k = 0;
    uint32_t p = 0;
    for (uint32_t i = 0; i < n && p + 46 <= cdlen && k < max; i++) {
        if (rd32(cd + p) != 0x02014B50) break;
        uint32_t nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        if (p + 46 + nl > cdlen) break;
        entry_t *o = &e[k++];
        memset(o, 0, sizeof *o);
        memcpy(o->name, cd + p + 46, nl < sizeof o->name - 1 ? nl : sizeof o->name - 1);
        o->method = (uint16_t)rd16(cd + p + 10);
        o->csize = rd32(cd + p + 20);
        o->usize = rd32(cd + p + 24);
        o->off = rd32(cd + p + 42);
        p += 46 + nl + xl + cl;
    }
    la_big_free(cd);
    return k;
}

static const entry_t *find(const entry_t *e, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, name)) return &e[i];
    return NULL;
}

/* a member's bytes, inflated if they were deflated, into out (usize bytes) */
static bool member_read(FILE *f, const entry_t *e, uint8_t *out)
{
    uint8_t h[30];
    if (fseek(f, (long)e->off, SEEK_SET) || fread(h, 1, 30, f) != 30 || rd32(h) != 0x04034B50) return false;
    fseek(f, (long)(e->off + 30 + rd16(h + 26) + rd16(h + 28)), SEEK_SET);
    if (e->method == 0) {
        uint32_t left = e->usize;
        uint8_t *p = out;
        while (left) {
            uint32_t c = left > 65536 ? 65536 : left;
            if (fread(p, 1, c, f) != c) return false;
            p += c;
            left -= c;
            la_lock();
            LA.prog += c;
            la_unlock();
        }
        return true;
    }
    if (e->method != 8) return false;
    tinfl_decompressor *r = la_big_alloc(sizeof *r);
    uint8_t *in = la_big_alloc(32768);
    if (!r || !in) {
        la_big_free(r);
        la_big_free(in);
        return false;
    }
    tinfl_init(r);
    uint32_t cleft = e->csize;
    size_t have = 0, at = 0, outp = 0;
    bool ok = false;
    for (;;) {
        if (at == have && cleft) {
            uint32_t c = cleft > 32768 ? 32768 : cleft;
            if (fread(in, 1, c, f) != c) break;
            have = c;
            at = 0;
            cleft -= c;
        }
        size_t isz = have - at, osz = e->usize - outp;
        tinfl_status st = tinfl_decompress(r, in + at, &isz, out, out + outp, &osz,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                                           (cleft ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        at += isz;
        outp += osz;
        la_lock();
        LA.prog += (uint32_t)osz;
        la_unlock();
        if (st == TINFL_STATUS_DONE) {
            ok = outp == e->usize;
            break;
        }
        if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && !cleft && at == have)) break;
        if (LA.stop) break;
    }
    la_big_free(r);
    la_big_free(in);
    return ok;
}

static void ini_get(const char *ini, const char *sect, const char *key, char *out, size_t n)
{
    out[0] = 0;
    char hdr[32];
    snprintf(hdr, sizeof hdr, "[%s]", sect);
    const char *s = strstr(ini, hdr);
    if (!s) return;
    s += strlen(hdr);
    size_t kl = strlen(key);
    while (*s) {
        while (*s == '\n' || *s == '\r') s++;
        if (*s == '[') return;
        if (!strncmp(s, key, kl) && s[kl] == '=') {
            s += kl + 1;
            size_t k = 0;
            while (*s && *s != '\n' && *s != '\r' && k + 1 < n) out[k++] = *s++;
            out[k] = 0;
            return;
        }
        while (*s && *s != '\n') s++;
    }
}

static uint32_t parse_rate(const char *s)
{
    float v = strtof(s, NULL);
    const char *u = s;
    while (*u && (*u == ' ' || *u == '.' || (*u >= '0' && *u <= '9'))) u++;
    if (*u == 'G' || *u == 'g') v *= 1e9f;
    else if (*u == 'M') v *= 1e6f;
    else if (*u == 'k' || *u == 'K') v *= 1e3f;
    return v > 0 ? (uint32_t)(v + 0.5f) : 0;
}

bool la_sr_nomem;

const char *la_sr_load(const char *path, la_cap_t *c)
{
    la_sr_nomem = false;
    FILE *f = fopen(path, "rb");
    if (!f) return _("No se pudo abrir el archivo");
    entry_t *e = la_big_alloc(ENTRIES_MAX * sizeof *e);
    if (!e) {
        fclose(f);
        return _("No hay memoria");
    }
    const char *why = NULL;
    char *meta = NULL;
    int n = read_dir(f, e, ENTRIES_MAX);
    const entry_t *me = n > 0 ? find(e, n, "metadata") : NULL;
    if (!me || me->usize > 64 * 1024) {
        why = _("No es una sesión de sigrok (.sr)");
        goto out;
    }
    meta = la_big_alloc(me->usize + 1);
    if (!meta || !member_read(f, me, (uint8_t *)meta)) {
        why = _("No se pudo leer el archivo");
        goto out;
    }
    meta[me->usize] = 0;
    char v[64], cf[32];
    ini_get(meta, "device 1", "samplerate", v, sizeof v);
    uint32_t rate = parse_rate(v);
    ini_get(meta, "device 1", "unitsize", v, sizeof v);
    int unit = atoi(v);
    if (unit < 1) unit = 1;
    ini_get(meta, "device 1", "total probes", v, sizeof v);
    int probes = atoi(v);
    ini_get(meta, "device 1", "capturefile", cf, sizeof cf);
    if (!cf[0] || !rate || unit > 8) {
        why = _("La sesión no tiene canales lógicos");
        goto out;
    }
    /* the chunks: logic-1-1, -2... or a single "logic-1" (format 1) */
    uint32_t total = 0;
    int nchunks = 0;
    const entry_t *ch[64];
    for (int i = 1; i <= 64; i++) {
        char nmb[40];
        snprintf(nmb, sizeof nmb, "%s-%d", cf, i);
        const entry_t *x = find(e, n, nmb);
        if (!x) break;
        ch[nchunks++] = x;
        total += x->usize;
    }
    if (!nchunks) {
        const entry_t *x = find(e, n, cf);
        if (x) {
            ch[nchunks++] = x;
            total = x->usize;
        }
    }
    uint32_t samples = total / (uint32_t)unit;
    if (!samples) {
        why = _("La sesión está vacía");
        goto out;
    }
    uint32_t maxs = la_max_samples();
    if (samples > maxs) {
        why = _("La captura no entra en la memoria");
        goto out;
    }
    la_lock();
    LA.state = LA_ST_FILE;
    LA.prog = 0;
    LA.prog_total = total;
    la_unlock();
    if (!la_cap_alloc(c, samples)) {
        la_sr_nomem = true;
        why = _("No hay memoria para tantas muestras");
        goto out;
    }
    uint32_t at = 0;
    for (int i = 0; i < nchunks && !why; i++) {
        const entry_t *x = ch[i];
        if (unit == 1) {
            if (!member_read(f, x, c->buf + at)) why = _("No se pudo leer el archivo");
            at += x->usize;
        } else {
            /* wider samples: keep the first byte (channels 1 to 8) */
            uint8_t *tmp = la_big_alloc(x->usize);
            if (!tmp) why = _("No hay memoria");
            else if (!member_read(f, x, tmp)) why = _("No se pudo leer el archivo");
            else
                for (uint32_t k = 0; k + (uint32_t)unit <= x->usize; k += (uint32_t)unit) c->buf[at++] = tmp[k];
            la_big_free(tmp);
        }
    }
    if (why) {
        c->n = 0;
        goto out;
    }
    c->n = samples;
    c->rate = rate;
    c->nch = (uint8_t)(probes > 8 || probes < 1 ? 8 : probes);
    c->trig = -1;
    c->lost = 0;
    c->stalled = false;
    for (int i = 0; i < LA_NCH; i++) {
        char key[12];
        snprintf(key, sizeof key, "probe%d", i + 1);
        ini_get(meta, "device 1", key, v, sizeof v);
        snprintf(LA.chname[i], sizeof LA.chname[i], "%.11s", v[0] ? v : "");
        LA.chshow[i] = i < c->nch;
    }
    const entry_t *px = find(e, n, "p4os");
    if (px && px->usize < 512) {
        char extra[512];
        if (member_read(f, px, (uint8_t *)extra)) {
            extra[px->usize] = 0;
            const char *t = strstr(extra, "trigger=");
            if (t) {
                int tv = atoi(t + 8);
                c->trig = tv >= 0 && (uint32_t)tv < samples ? tv : -1;
            }
        }
    }
    const char *base = strrchr(path, '/');
    snprintf(c->src, sizeof c->src, "%.47s", base ? base + 1 : path);
    if (probes > 8) la_set_msg(false, _("La sesión tiene %d canales: se ven los primeros 8"), probes);
out:
    la_big_free(meta);
    la_big_free(e);
    fclose(f);
    return why;
}
