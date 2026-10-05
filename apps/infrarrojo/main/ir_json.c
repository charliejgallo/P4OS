/*
 * P4OS - Infrarrojo: a small JSON reader.
 *
 * The firmware lends the apps no JSON library, and the app reads three kinds
 * of it: its own device files, the portal's requests, and the metadata of
 * each SmartIR device in the pack. All are small and trusted enough; this is
 * a plain recursive descent over a copy of the text, unescaping strings in
 * place (an unescaped string is never longer than its source), with the
 * nodes in one growing array.
 */
#include "ir.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    ij_doc_t *d;
    char     *p, *end;
    int       depth;
} ij_ps_t;

static int node_new(ij_ps_t *s, int type)
{
    ij_doc_t *d = s->d;
    if (d->len == d->cap) {
        int cap = d->cap ? d->cap * 2 : 64;
        ij_node_t *n = ir_realloc(d->n, (size_t)cap * sizeof *n);
        if (!n) return -1;
        d->n = n;
        d->cap = cap;
    }
    ij_node_t *n = &d->n[d->len];
    memset(n, 0, sizeof *n);
    n->type = (uint8_t)type;
    n->kid = n->next = -1;
    return d->len++;
}

static void ws(ij_ps_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *put_utf8(char *o, uint32_t u)
{
    if (u < 0x80) *o++ = (char)u;
    else if (u < 0x800) { *o++ = (char)(0xC0 | u >> 6); *o++ = (char)(0x80 | (u & 0x3F)); }
    else if (u < 0x10000) { *o++ = (char)(0xE0 | u >> 12); *o++ = (char)(0x80 | (u >> 6 & 0x3F)); *o++ = (char)(0x80 | (u & 0x3F)); }
    else { *o++ = (char)(0xF0 | u >> 18); *o++ = (char)(0x80 | (u >> 12 & 0x3F)); *o++ = (char)(0x80 | (u >> 6 & 0x3F)); *o++ = (char)(0x80 | (u & 0x3F)); }
    return o;
}

/* s->p is on the opening quote; returns the string, NUL-terminated in place */
static const char *str_in_place(ij_ps_t *s)
{
    char *start = ++s->p, *o = start;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c != '\\') { *o++ = c; continue; }
        if (s->p >= s->end) return NULL;
        c = *s->p++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            if (s->end - s->p < 4) return NULL;
            uint32_t u = 0;
            for (int i = 0; i < 4; i++) {
                int h = hexv(s->p[i]);
                if (h < 0) return NULL;
                u = u << 4 | (uint32_t)h;
            }
            s->p += 4;
            /* a surrogate pair */
            if (u >= 0xD800 && u < 0xDC00 && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                uint32_t lo = 0;
                bool ok = true;
                for (int i = 0; i < 4; i++) {
                    int h = hexv(s->p[2 + i]);
                    if (h < 0) { ok = false; break; }
                    lo = lo << 4 | (uint32_t)h;
                }
                if (ok && lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    s->p += 6;
                }
            }
            o = put_utf8(o, u);
            break;
        }
        default: *o++ = c; break;     /* \" \\ \/ */
        }
    }
    if (s->p >= s->end) return NULL;
    s->p++;         /* the closing quote */
    *o = 0;
    return start;
}

static int value(ij_ps_t *s);

static int container(ij_ps_t *s, bool obj)
{
    int me = node_new(s, obj ? IJ_OBJ : IJ_ARR);
    if (me < 0) return -1;
    s->p++;
    int last = -1;
    ws(s);
    if (s->p < s->end && *s->p == (obj ? '}' : ']')) { s->p++; return me; }
    for (;;) {
        const char *key = NULL;
        ws(s);
        if (obj) {
            if (s->p >= s->end || *s->p != '"') return -1;
            key = str_in_place(s);
            if (!key) return -1;
            ws(s);
            if (s->p >= s->end || *s->p != ':') return -1;
            s->p++;
        }
        int v = value(s);
        if (v < 0) return -1;
        s->d->n[v].key = key;
        if (last < 0) s->d->n[me].kid = v;
        else s->d->n[last].next = v;
        last = v;
        s->d->n[me].count++;
        ws(s);
        if (s->p >= s->end) return -1;
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == (obj ? '}' : ']')) { s->p++; return me; }
        return -1;
    }
}

static int value(ij_ps_t *s)
{
    ws(s);
    if (s->p >= s->end || ++s->depth > 64) return -1;
    int r = -1;
    char c = *s->p;
    if (c == '{' || c == '[') r = container(s, c == '{');
    else if (c == '"') {
        r = node_new(s, IJ_STR);
        if (r >= 0) {
            const char *str = str_in_place(s);
            if (!str) r = -1;
            else s->d->n[r].str = str;
        }
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *w = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t l = strlen(w);
        if ((size_t)(s->end - s->p) >= l && !strncmp(s->p, w, l)) {
            r = node_new(s, c == 'n' ? IJ_NULL : IJ_BOOL);
            if (r >= 0) s->d->n[r].num = c == 't';
            s->p += l;
        }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *e;
        double v = strtod(s->p, &e);
        if (e > s->p) {
            r = node_new(s, IJ_NUM);
            if (r >= 0) s->d->n[r].num = v;
            s->p = e;
        }
    }
    s->depth--;
    return r;
}

bool ij_parse(ij_doc_t *d, const char *text, size_t len)
{
    memset(d, 0, sizeof *d);
    d->text = ir_alloc(len + 1);
    if (!d->text) return false;
    memcpy(d->text, text, len);
    /* strtod stops at the NUL: a number at the very end still parses */
    ij_ps_t s = { d, d->text, d->text + len, 0 };
    if (value(&s) != 0) { ij_free(d); return false; }
    return true;
}

void ij_free(ij_doc_t *d)
{
    if (!d) return;
    ir_free(d->n);
    ir_free(d->text);
    memset(d, 0, sizeof *d);
}

int ij_get(const ij_doc_t *d, int obj, const char *key)
{
    if (obj < 0 || obj >= d->len || d->n[obj].type != IJ_OBJ) return -1;
    IJ_EACH(d, obj, c) if (d->n[c].key && !strcmp(d->n[c].key, key)) return c;
    return -1;
}

int ij_at(const ij_doc_t *d, int arr, int i)
{
    if (arr < 0 || arr >= d->len) return -1;
    IJ_EACH(d, arr, c) if (i-- == 0) return c;
    return -1;
}

const char *ij_str(const ij_doc_t *d, int i, const char *def)
{
    return (i >= 0 && i < d->len && d->n[i].type == IJ_STR) ? d->n[i].str : def;
}

double ij_num(const ij_doc_t *d, int i, double def)
{
    if (i < 0 || i >= d->len) return def;
    if (d->n[i].type == IJ_NUM || d->n[i].type == IJ_BOOL) return d->n[i].num;
    if (d->n[i].type == IJ_STR) {
        char *e;
        double v = strtod(d->n[i].str, &e);
        return e > d->n[i].str ? v : def;
    }
    return def;
}
