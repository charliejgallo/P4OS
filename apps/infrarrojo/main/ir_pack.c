/*
 * P4OS - Infrarrojo: SmartIR's code library, from the card.
 *
 * infrarrojo_p4.pak in the apps folder, built by tools/pack_smartir.py (the
 * format is described there). The index - 400 devices, their brands and
 * models - is read once and stays; a device's blob (its metadata as JSON,
 * the table of pairs and its codes) is read when it is opened, and a code
 * is unpacked into durations only when it is sent or compared.
 *
 * SmartIR is MIT licensed: Copyright (c) 2019 Vassilis Panos, 2024 Li Tin
 * O've Weedle (https://github.com/litinoveweedle/SmartIR).
 */
#include "ir.h"
#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

static ir_pack_dev_t *s_dev;
static char          *s_str;
static int            s_count = -1;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

const char *ir_pack_path(char *out, size_t n)
{
    snprintf(out, n, "%s/infrarrojo_p4.pak", aos_hal_path_apps());
    return out;
}

bool ir_pack_open(void)
{
    if (s_count >= 0) return s_count > 0;
    s_count = 0;
    char path[128];
    FILE *f = fopen(ir_pack_path(path, sizeof path), "rb");
    if (!f) { aos_hal_log("ir", "no %s: no code library", path); return false; }
    uint8_t h[16];
    if (fread(h, 1, 16, f) != 16 || memcmp(h, "IRP1", 4)) { fclose(f); return false; }
    uint32_t n = rd32(h + 4), so = rd32(h + 8), sl = rd32(h + 12);
    if (n == 0 || n > 20000 || sl > 4 * 1024 * 1024) { fclose(f); return false; }
    uint8_t *idx = ir_alloc((size_t)n * 16);
    s_dev = ir_alloc((size_t)n * sizeof *s_dev);
    s_str = ir_alloc(sl + 1);
    bool ok = idx && s_dev && s_str && fread(idx, 16, n, f) == n &&
              fseek(f, (long)so, SEEK_SET) == 0 && fread(s_str, 1, sl, f) == sl;
    fclose(f);
    if (ok) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = idx + 16 * i;
            uint32_t b = rd32(e + 4), m = rd32(e + 8);
            s_dev[i].id = rd16(e);
            s_dev[i].cls = e[2];
            s_dev[i].brand = b < sl ? s_str + b : "";
            s_dev[i].models = m < sl ? s_str + m : "";
            s_dev[i].blob = rd32(e + 12);
        }
        s_count = (int)n;
        aos_hal_log("ir", "code library: %d devices", s_count);
    } else {
        ir_free(s_dev);
        ir_free(s_str);
        s_dev = NULL;
        s_str = NULL;
    }
    ir_free(idx);
    return s_count > 0;
}

void ir_pack_close(void)
{
    ir_free(s_dev);
    ir_free(s_str);
    s_dev = NULL;
    s_str = NULL;
    s_count = -1;
}

int ir_pack_count(void) { return s_count > 0 ? s_count : 0; }
const ir_pack_dev_t *ir_pack_at(int i) { return (i >= 0 && i < s_count) ? &s_dev[i] : NULL; }

int ir_pack_find(uint16_t id)
{
    for (int i = 0; i < s_count; i++) if (s_dev[i].id == id) return i;
    return -1;
}

bool ir_pack_parse_blob(uint8_t *buf, size_t len, ir_pack_blob_t *b, bool with_json)
{
    memset(b, 0, sizeof *b);
    if (len < 4) return false;
    uint32_t jl = rd32(buf);
    size_t at = 4 + jl;
    if (at + 2 > len) return false;
    if (with_json && !ij_parse(&b->js, (const char *)buf + 4, jl)) return false;
    b->npairs = rd16(buf + at);
    at += 2;
    b->pairs = buf + at;
    at += (size_t)b->npairs * 4;
    if (at + 3 > len) goto bad;
    b->bits = buf[at];
    b->ncodes = rd16(buf + at + 1);
    at += 3;
    b->offs = buf + at;
    at += (size_t)b->ncodes * 4;
    if (at > len || b->bits < 1 || b->bits > 16) goto bad;
    b->codes = buf + at;
    b->codes_len = len - at;
    return true;
bad:
    ij_free(&b->js);
    return false;
}

bool ir_pack_load(int i, ir_pack_blob_t *b)
{
    memset(b, 0, sizeof *b);
    const ir_pack_dev_t *d = ir_pack_at(i);
    if (!d) return false;
    char path[128];
    FILE *f = fopen(ir_pack_path(path, sizeof path), "rb");
    if (!f) return false;
    uint8_t h[4];
    bool ok = fseek(f, (long)d->blob, SEEK_SET) == 0 && fread(h, 1, 4, f) == 4;
    uint32_t jl = ok ? rd32(h) : 0;
    /* the length of the blob is not stored: the next one's start, or the end */
    uint32_t end = 0;
    if (ok) {
        if (i + 1 < s_count) end = s_dev[i + 1].blob;
        else { fseek(f, 0, SEEK_END); end = (uint32_t)ftell(f); }
    }
    size_t len = end > d->blob ? end - d->blob : 0;
    uint8_t *buf = (ok && len > 4 + jl && len < 4 * 1024 * 1024) ? ir_alloc(len) : NULL;
    ok = buf && fseek(f, (long)d->blob, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
    fclose(f);
    if (ok && ir_pack_parse_blob(buf, len, b, true)) {
        b->buf = buf;
        return true;
    }
    ir_free(buf);
    memset(b, 0, sizeof *b);
    return false;
}

void ir_pack_blob_free(ir_pack_blob_t *b)
{
    ij_free(&b->js);
    ir_free(b->buf);
    memset(b, 0, sizeof *b);
}

int ir_pack_code(const ir_pack_blob_t *b, int code, uint32_t *out, int max)
{
    if (code < 0 || code >= b->ncodes) return 0;
    uint32_t off = rd32(b->offs + 4 * code);
    if (off + 2 > b->codes_len) return 0;
    const uint8_t *p = b->codes + off;
    int npairs = rd16(p);
    p += 2;
    size_t bytes = ((size_t)npairs * (size_t)b->bits + 7) / 8;
    if (off + 2 + bytes > b->codes_len) return 0;
    uint32_t acc = 0;
    int nb = 0, n = 0;
    uint32_t mask = (1u << b->bits) - 1;
    for (int i = 0; i < npairs; i++) {
        while (nb < b->bits) { acc |= (uint32_t)*p++ << nb; nb += 8; }
        uint32_t k = acc & mask;
        acc >>= b->bits;
        nb -= b->bits;
        if ((int)k >= b->npairs) return 0;
        uint16_t mark = rd16(b->pairs + 4 * k), space = rd16(b->pairs + 4 * k + 2);
        if (n < max) out[n++] = mark;
        if (space && n < max) out[n++] = space;
    }
    return n;
}

/* the path to a code index, depth first */
static bool name_of(const ij_doc_t *j, int node, int code, char *out, size_t n, size_t at)
{
    if (j->n[node].type == IJ_NUM) return (int)j->n[node].num == code;
    if (j->n[node].type != IJ_OBJ && j->n[node].type != IJ_ARR) return false;
    IJ_EACH(j, node, c) {
        size_t l = at;
        if (j->n[c].key) {
            snprintf(out + at, n - at, "%s%s", at ? " · " : "", j->n[c].key);
            l = strlen(out);
        }
        if (name_of(j, c, code, out, n, l)) return true;
        out[at] = 0;
    }
    return false;
}

bool ir_pack_code_name(const ir_pack_blob_t *b, int code, char *out, size_t n)
{
    out[0] = 0;
    int cmds = ij_get(&b->js, 0, "commands");
    return cmds >= 0 && name_of(&b->js, cmds, code, out, n, 0);
}
