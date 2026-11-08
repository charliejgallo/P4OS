/*
 * P4OS - SSH's data types on the wire (RFC 4251, section 5): the writer
 * stops at its buffer's end and the reader at its message's, each marking
 * itself bad, so a message that does not add up is noticed once, at the
 * end, instead of after every field.
 */
#include "aos_ssh_int.h"

#include <string.h>

static bool room(wbuf_t *b, size_t n)
{
    if (b->bad || b->cap - b->len < n) {
        b->bad = true;
        return false;
    }
    return true;
}

void wb_byte(wbuf_t *b, uint8_t v)
{
    if (room(b, 1)) b->p[b->len++] = v;
}

void wb_u32(wbuf_t *b, uint32_t v)
{
    if (!room(b, 4)) return;
    b->p[b->len++] = (uint8_t)(v >> 24);
    b->p[b->len++] = (uint8_t)(v >> 16);
    b->p[b->len++] = (uint8_t)(v >> 8);
    b->p[b->len++] = (uint8_t)v;
}

void wb_bytes(wbuf_t *b, const void *d, size_t n)
{
    if (!room(b, n)) return;
    if (n) memcpy(b->p + b->len, d, n);
    b->len += n;
}

void wb_string(wbuf_t *b, const void *d, size_t n)
{
    wb_u32(b, (uint32_t)n);
    wb_bytes(b, d, n);
}

void wb_cstr(wbuf_t *b, const char *s) { wb_string(b, s, strlen(s)); }

void wb_mpint(wbuf_t *b, const uint8_t *be, size_t n)
{
    while (n && !be[0]) {
        be++;
        n--;
    }
    bool pad = n && (be[0] & 0x80);     /* positive: a zero byte before a high bit */
    wb_u32(b, (uint32_t)(n + pad));
    if (pad) wb_byte(b, 0);
    wb_bytes(b, be, n);
}

uint8_t rb_byte(rbuf_t *b)
{
    if (b->bad || b->off + 1 > b->len) {
        b->bad = true;
        return 0;
    }
    return b->p[b->off++];
}

uint32_t rb_u32(rbuf_t *b)
{
    if (b->bad || b->len - b->off < 4) {
        b->bad = true;
        return 0;
    }
    const uint8_t *q = b->p + b->off;
    b->off += 4;
    return (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
}

bool rb_bool(rbuf_t *b) { return rb_byte(b) != 0; }

const uint8_t *rb_string(rbuf_t *b, size_t *n)
{
    *n = 0;
    uint32_t l = rb_u32(b);
    if (b->bad || l > b->len - b->off) {
        b->bad = true;
        return NULL;
    }
    const uint8_t *q = b->p + b->off;
    b->off += l;
    *n = l;
    return q;
}

bool rb_cstr(rbuf_t *b, char *out, size_t n)
{
    size_t l;
    const uint8_t *q = rb_string(b, &l);
    if (!n) return false;
    if (!q) {
        out[0] = 0;
        return false;
    }
    size_t c = l < n - 1 ? l : n - 1;
    memcpy(out, q, c);
    out[c] = 0;
    for (size_t i = 0; i < c; i++)
        if (!out[i]) out[i] = '?';      /* a NUL inside would cut it short silently */
    return true;
}

const uint8_t *rb_mpint(rbuf_t *b, size_t *n)
{
    const uint8_t *q = rb_string(b, n);
    while (q && *n && !q[0]) {
        q++;
        (*n)--;
    }
    return q;
}

bool namelist_has(const uint8_t *list, size_t n, const char *name)
{
    size_t nl = strlen(name), i = 0;
    while (i <= n && list) {
        size_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == nl && !memcmp(list + i, name, nl)) return true;
        i = j + 1;
    }
    return false;
}
