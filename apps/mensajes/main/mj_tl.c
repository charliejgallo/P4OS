/*
 * P4OS - Mensajes: TL read and written by walking the generated tables
 * (mj_tl.h says what and why).
 */
#include "mj_tl.h"

#include <stdlib.h>
#include <string.h>

#include "mj_port.h"
#include "mj_tinfl.h"

/* ---- the arena ---- */

#define ARENA_BLOCK (64 * 1024)

struct mj_arena_blk {
    mj_arena_blk_t *next;
    size_t size, used;
    uint8_t data[];
};

void *mj_arena_alloc(mj_arena_t *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    mj_arena_blk_t *b = a->head;
    if (!b || b->used + n > b->size) {
        size_t sz = n > ARENA_BLOCK - sizeof *b ? n : ARENA_BLOCK - sizeof *b;
        b = (mj_arena_blk_t *)mj_malloc(sizeof *b + sz);
        if (!b) return NULL;
        b->size = sz;
        b->used = 0;
        /* a big one-off goes behind the current block, so the space left in
         * the current one is not wasted */
        if (a->head && n > ARENA_BLOCK / 2) {
            b->next = a->head->next;
            a->head->next = b;
        } else {
            b->next = a->head;
            a->head = b;
        }
        a->held += sizeof *b + sz;
    }
    void *p = b->data + b->used;
    b->used += n;
    a->used += n;
    memset(p, 0, n);
    return p;
}

char *mj_arena_strdup(mj_arena_t *a, const char *s, size_t n)
{
    char *d = (char *)mj_arena_alloc(a, n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void mj_arena_free(mj_arena_t *a)
{
    mj_arena_blk_t *b = a->head;
    while (b) {
        mj_arena_blk_t *n = b->next;
        mj_free(b);
        b = n;
    }
    a->head = NULL;
    a->used = a->held = 0;
}

/* ---- names ---- */

int tl_ctor_index(uint32_t id)
{
    int lo = 0, hi = TL_NCTORS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t v = tl_ctors[tl_by_id[mid]].id;
        if (v == id) return tl_by_id[mid];
        if (v < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

const char *tl_name(uint32_t id)
{
    int ci = tl_ctor_index(id);
    return ci < 0 ? "?" : tl_names + tl_ctors[ci].name;
}

const char *tl_fname(uint16_t name)
{
    return name < TL_NNAMES ? tl_names + tl_fname_off[name] : "?";
}

/* ---- reading ---- */

typedef struct {
    mj_arena_t *a;
    const uint8_t *p, *end;
    int depth;
    bool bad;
    const char *why;
    uint32_t why_id;
} rd_t;

static void rd_fail(rd_t *r, const char *why, uint32_t id)
{
    if (!r->bad) {
        r->why = why;
        r->why_id = id;
    }
    r->bad = true;
}

static bool rd_need(rd_t *r, size_t n)
{
    if (r->bad) return false;
    if ((size_t)(r->end - r->p) < n) {
        rd_fail(r, "short", 0);
        return false;
    }
    return true;
}

static uint32_t rd_u32(rd_t *r)
{
    if (!rd_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
    r->p += 4;
    return v;
}

static uint64_t rd_u64(rd_t *r)
{
    uint32_t lo = rd_u32(r);
    uint32_t hi = rd_u32(r);
    return (uint64_t)hi << 32 | lo;
}

/* A TL string: one length byte (or 254 and three), the bytes, padding to 4. */
static const uint8_t *rd_bytes(rd_t *r, uint32_t *len)
{
    if (!rd_need(r, 1)) return NULL;
    uint32_t n = r->p[0];
    size_t head = 1;
    if (n == 254) {
        if (!rd_need(r, 4)) return NULL;
        n = (uint32_t)r->p[1] | (uint32_t)r->p[2] << 8 | (uint32_t)r->p[3] << 16;
        head = 4;
    } else if (n == 255) {
        rd_fail(r, "bad string length", 0);
        return NULL;
    }
    size_t total = (head + n + 3) & ~(size_t)3;
    if (!rd_need(r, total)) return NULL;
    const uint8_t *d = r->p + head;
    r->p += total;
    *len = n;
    return d;
}

static bool rd_value(rd_t *r, uint16_t type, tl_val_t *out);
static tl_obj_t *rd_fields(rd_t *r, int ci);

static bool rd_boxed(rd_t *r, tl_val_t *out);

static tl_vec_t *rd_vector_body(rd_t *r, uint16_t elem_type)
{
    uint32_t n = rd_u32(r);
    if (r->bad) return NULL;
    if (n > (uint32_t)(r->end - r->p)) {        /* every element is at least a byte... at least 4 */
        rd_fail(r, "vector too long", n);
        return NULL;
    }
    tl_vec_t *v = (tl_vec_t *)mj_arena_alloc(r->a, sizeof *v + n * sizeof(tl_val_t));
    if (!v) {
        rd_fail(r, "out of memory", 0);
        return NULL;
    }
    v->n = n;
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        if (elem_type == 0xffff) rd_boxed(r, &v->e[i]);
        else rd_value(r, elem_type, &v->e[i]);
    }
    return r->bad ? NULL : v;
}

/* gzip_packed: opened into the arena, and the object inside read from there. */
static bool rd_gzip(rd_t *r, tl_val_t *out)
{
    uint32_t len;
    const uint8_t *d = rd_bytes(r, &len);
    if (!d) return false;
    size_t on = 0;
    uint8_t *o = tl_gunzip(r->a, d, len, &on);
    if (!o) {
        rd_fail(r, "gzip", 0);
        return false;
    }
    rd_t in = {r->a, o, o + on, r->depth, false, NULL, 0};
    bool ok = rd_boxed(&in, out);
    if (!ok) rd_fail(r, in.why, in.why_id);
    return ok;
}

/* Any boxed value: an object of any type, a vector of boxed things, a Bool. */
static bool rd_boxed(rd_t *r, tl_val_t *out)
{
    uint32_t id = rd_u32(r);
    if (r->bad) return false;
    out->set = 1;
    if (id == TL_GZIP_PACKED) return rd_gzip(r, out);
    if (id == TL_BOOL_TRUE || id == TL_BOOL_FALSE) {
        out->kind = TLK_BOOL;
        out->u.i = id == TL_BOOL_TRUE;
        return true;
    }
    if (id == TL_VECTOR) {
        /* a vector whose type we do not know: of boxed objects, the only
         * kind an answer of !X can be (Vector<User>, Vector<long> is never
         * the answer of anything Mensajes calls) */
        out->kind = TLK_VEC;
        out->u.v = rd_vector_body(r, 0xffff);
        return !r->bad;
    }
    int ci = tl_ctor_index(id);
    if (ci < 0) {
        rd_fail(r, "unknown constructor", id);
        return false;
    }
    out->kind = TLK_OBJ;
    out->u.o = rd_fields(r, ci);
    return !r->bad;
}

static bool rd_value(rd_t *r, uint16_t type, tl_val_t *out)
{
    const tl_type_t *t = &tl_types[type];
    out->kind = t->kind;
    out->set = 1;
    switch (t->kind) {
    case TLK_FLAGS:
    case TLK_INT:
        out->u.i = (int32_t)rd_u32(r);
        break;
    case TLK_LONG:
    case TLK_DOUBLE:
        out->u.l = (int64_t)rd_u64(r);   /* a double is its 8 bytes, the same union */
        break;
    case TLK_STRING: {
        uint32_t n = 0;
        const uint8_t *d = rd_bytes(r, &n);
        if (!d) return false;
        out->u.s = mj_arena_strdup(r->a, (const char *)d, n);
        out->len = n;
        break;
    }
    case TLK_BYTES: {
        uint32_t n = 0;
        const uint8_t *d = rd_bytes(r, &n);
        if (!d) return false;
        out->u.s = mj_arena_strdup(r->a, (const char *)d, n);
        out->len = n;
        break;
    }
    case TLK_INT128:
    case TLK_INT256: {
        size_t n = t->kind == TLK_INT128 ? 16 : 32;
        if (!rd_need(r, n)) return false;
        out->u.b = (const uint8_t *)mj_arena_strdup(r->a, (const char *)r->p, n);
        out->len = (uint32_t)n;
        r->p += n;
        break;
    }
    case TLK_BOOL: {
        uint32_t id = rd_u32(r);
        out->u.i = id == TL_BOOL_TRUE;
        break;
    }
    case TLK_TRUE:
        out->u.i = 1;
        break;
    case TLK_OBJ:
    case TLK_ANY: {
        if (++r->depth > 64) {
            rd_fail(r, "too deep", 0);
            return false;
        }
        bool ok = rd_boxed(r, out);
        r->depth--;
        return ok;
    }
    case TLK_BARE:
        out->kind = TLK_OBJ;
        out->u.o = rd_fields(r, t->arg);
        break;
    case TLK_VEC: {
        uint32_t id = rd_u32(r);
        if (id != TL_VECTOR) {
            rd_fail(r, "expected a vector", id);
            return false;
        }
    }
    /* fall through */
    case TLK_BVEC: {
        out->kind = TLK_VEC;
        /* a vector of a boxed type reads its elements boxed through the
         * element type, whatever it is */
        out->u.v = rd_vector_body(r, t->arg);
        break;
    }
    default:
        rd_fail(r, "bad type", type);
        return false;
    }
    return !r->bad;
}

static tl_obj_t *rd_fields(rd_t *r, int ci)
{
    const tl_ctor_t *c = &tl_ctors[ci];
    tl_obj_t *o = (tl_obj_t *)mj_arena_alloc(r->a, sizeof *o + c->n * sizeof(tl_val_t));
    if (!o) {
        rd_fail(r, "out of memory", 0);
        return NULL;
    }
    o->id = c->id;
    o->ci = (uint16_t)ci;
    o->n = c->n;
    for (int i = 0; i < c->n && !r->bad; i++) {
        const tl_field_t *f = &tl_fields[c->first + i];
        if (f->flag_fi >= 0) {
            uint32_t flags = (uint32_t)o->f[f->flag_fi].u.i;
            if (!((flags >> f->bit) & 1)) {
                o->f[i].kind = tl_types[f->type].kind;
                continue;               /* absent: set = 0 */
            }
        }
        rd_value(r, f->type, &o->f[i]);
    }
    return r->bad ? NULL : o;
}

bool tl_read(mj_arena_t *a, const uint8_t *p, size_t n, tl_val_t *out, size_t *used)
{
    rd_t r = {a, p, p + n, 0, false, NULL, 0};
    memset(out, 0, sizeof *out);
    bool ok = rd_boxed(&r, out);
    if (!ok) {
        mj_log("tl: cannot read: %s %08x (%s) at %d of %d", r.why ? r.why : "?", (unsigned)r.why_id,
               r.why_id ? tl_name(r.why_id) : "-", (int)(r.p - p), (int)n);
        return false;
    }
    if (used) *used = (size_t)(r.p - p);
    return true;
}

tl_obj_t *tl_read_obj(mj_arena_t *a, const uint8_t *p, size_t n, size_t *used)
{
    tl_val_t v;
    if (!tl_read(a, p, n, &v, used)) return NULL;
    return v.kind == TLK_OBJ ? v.u.o : NULL;
}

uint8_t *tl_gunzip(mj_arena_t *a, const uint8_t *p, size_t n, size_t *out_n)
{
    /* gzip: 1f 8b 08 flags mtime(4) xfl os [extra] [name] [comment] [hcrc]
     * deflate ... crc32 isize */
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return NULL;
    uint8_t fl = p[3];
    size_t pos = 10;
    if (fl & 4) {
        if (pos + 2 > n) return NULL;
        pos += 2 + (size_t)(p[pos] | p[pos + 1] << 8);
    }
    if (fl & 8)
        while (pos < n && p[pos++]) {}
    if (fl & 16)
        while (pos < n && p[pos++]) {}
    if (fl & 2) pos += 2;
    if (pos + 8 > n) return NULL;
    uint32_t isize = (uint32_t)p[n - 4] | (uint32_t)p[n - 3] << 8 | (uint32_t)p[n - 2] << 16 | (uint32_t)p[n - 1] << 24;
    if (isize > 16 * 1024 * 1024) return NULL;
    uint8_t *out = (uint8_t *)mj_arena_alloc(a, isize + 1);
    tinfl_decompressor *d = (tinfl_decompressor *)mj_malloc(sizeof(tinfl_decompressor));
    if (!out || !d) {
        mj_free(d);
        return NULL;
    }
    tinfl_init(d);
    size_t in_n = n - 8 - pos, o_n = isize;
    tinfl_status st = tinfl_decompress(d, p + pos, &in_n, out, out, &o_n, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    mj_free(d);
    if (st != TINFL_STATUS_DONE || o_n != isize) return NULL;
    *out_n = isize;
    return out;
}

/* ---- asking ---- */

static int field_at(const tl_obj_t *o, uint16_t name)
{
    if (!o) return -1;
    const tl_ctor_t *c = &tl_ctors[o->ci];
    for (int i = 0; i < c->n; i++)
        if (tl_fields[c->first + i].name == name) return i;
    return -1;
}

const tl_val_t *tl_get(const tl_obj_t *o, uint16_t name)
{
    int i = field_at(o, name);
    if (i < 0 || !o->f[i].set) return NULL;
    return &o->f[i];
}

int32_t tl_i(const tl_obj_t *o, uint16_t name)
{
    const tl_val_t *v = tl_get(o, name);
    if (!v) return 0;
    return v->kind == TLK_LONG ? (int32_t)v->u.l : v->u.i;
}

int64_t tl_l(const tl_obj_t *o, uint16_t name)
{
    const tl_val_t *v = tl_get(o, name);
    if (!v) return 0;
    return v->kind == TLK_LONG ? v->u.l : v->u.i;
}

double tl_d(const tl_obj_t *o, uint16_t name)
{
    const tl_val_t *v = tl_get(o, name);
    return v && v->kind == TLK_DOUBLE ? v->u.d : 0;
}

const char *tl_s(const tl_obj_t *o, uint16_t name)
{
    const tl_val_t *v = tl_get(o, name);
    return v && (v->kind == TLK_STRING || v->kind == TLK_BYTES) && v->u.s ? v->u.s : "";
}

const uint8_t *tl_b(const tl_obj_t *o, uint16_t name, uint32_t *len)
{
    const tl_val_t *v = tl_get(o, name);
    if (!v || !(v->kind == TLK_STRING || v->kind == TLK_BYTES || v->kind == TLK_INT128 || v->kind == TLK_INT256)) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = v->len;
    return v->u.b;
}

bool tl_t(const tl_obj_t *o, uint16_t name)
{
    const tl_val_t *v = tl_get(o, name);
    return v && (v->kind == TLK_TRUE || v->kind == TLK_BOOL) && v->u.i;
}

tl_obj_t *tl_o(const tl_obj_t *o, uint16_t name)
{
    const tl_val_t *v = tl_get(o, name);
    return v && v->kind == TLK_OBJ ? v->u.o : NULL;
}

tl_vec_t *tl_v(const tl_obj_t *o, uint16_t name)
{
    const tl_val_t *v = tl_get(o, name);
    return v && v->kind == TLK_VEC ? v->u.v : NULL;
}

/* ---- writing ---- */

void mj_buf_put(mj_buf_t *b, const void *d, size_t n)
{
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = (uint8_t *)mj_realloc(b->p, cap);
        if (!p) {
            b->oom = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

void mj_buf_u32(mj_buf_t *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    mj_buf_put(b, x, 4);
}

void mj_buf_u64(mj_buf_t *b, uint64_t v)
{
    mj_buf_u32(b, (uint32_t)v);
    mj_buf_u32(b, (uint32_t)(v >> 32));
}

void mj_buf_bytes(mj_buf_t *b, const void *d, size_t n)
{
    uint8_t h[4];
    size_t hn;
    if (n < 254) {
        h[0] = (uint8_t)n;
        hn = 1;
    } else {
        h[0] = 254;
        h[1] = (uint8_t)n;
        h[2] = (uint8_t)(n >> 8);
        h[3] = (uint8_t)(n >> 16);
        hn = 4;
    }
    mj_buf_put(b, h, hn);
    mj_buf_put(b, d, n);
    static const uint8_t z[4] = {0};
    size_t pad = (4 - ((hn + n) & 3)) & 3;
    mj_buf_put(b, z, pad);
}

void mj_buf_free(mj_buf_t *b)
{
    mj_free(b->p);
    memset(b, 0, sizeof *b);
}

tl_obj_t *tl_new(mj_arena_t *a, uint32_t id)
{
    int ci = tl_ctor_index(id);
    if (ci < 0) {
        mj_log("tl: no constructor %08x to write", (unsigned)id);
        return NULL;
    }
    const tl_ctor_t *c = &tl_ctors[ci];
    tl_obj_t *o = (tl_obj_t *)mj_arena_alloc(a, sizeof *o + c->n * sizeof(tl_val_t));
    if (!o) return NULL;
    o->id = id;
    o->ci = (uint16_t)ci;
    o->n = c->n;
    for (int i = 0; i < c->n; i++) o->f[i].kind = tl_types[tl_fields[c->first + i].type].kind;
    return o;
}

tl_vec_t *tl_vec_new(mj_arena_t *a, uint32_t n)
{
    tl_vec_t *v = (tl_vec_t *)mj_arena_alloc(a, sizeof *v + n * sizeof(tl_val_t));
    if (v) v->n = n;
    return v;
}

static tl_val_t *set_slot(tl_obj_t *o, uint16_t name)
{
    int i = field_at(o, name);
    if (i < 0) {
        if (o) mj_log("tl: %s has no field %s", tl_name(o->id), tl_fname(name));
        return NULL;
    }
    o->f[i].set = 1;
    return &o->f[i];
}

bool tl_set_i(tl_obj_t *o, uint16_t name, int32_t v)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    if (s->kind == TLK_LONG) s->u.l = v;
    else s->u.i = v;
    return true;
}

bool tl_set_l(tl_obj_t *o, uint16_t name, int64_t v)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    if (s->kind == TLK_LONG) s->u.l = v;
    else s->u.i = (int32_t)v;
    return true;
}

bool tl_set_d(tl_obj_t *o, uint16_t name, double v)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->u.d = v;
    return true;
}

bool tl_set_s(tl_obj_t *o, uint16_t name, const char *str)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->u.s = str ? str : "";
    s->len = (uint32_t)strlen(s->u.s);
    return true;
}

bool tl_set_b(tl_obj_t *o, uint16_t name, const void *p, uint32_t len)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->u.b = (const uint8_t *)p;
    s->len = len;
    return true;
}

bool tl_set_t(tl_obj_t *o, uint16_t name)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->u.i = 1;
    return true;
}

bool tl_set_bool(tl_obj_t *o, uint16_t name, bool v)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->u.i = v ? 1 : 0;
    return true;
}

bool tl_set_o(tl_obj_t *o, uint16_t name, tl_obj_t *child)
{
    if (!child) return false;
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->u.o = child;
    return true;
}

bool tl_set_v(tl_obj_t *o, uint16_t name, tl_vec_t *v)
{
    if (!v) return false;
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->u.v = v;
    return true;
}

bool tl_set_raw(tl_obj_t *o, uint16_t name, const void *p, uint32_t len)
{
    tl_val_t *s = set_slot(o, name);
    if (!s) return false;
    s->set = 2;                 /* 2: raw bytes, see wr_value */
    s->u.b = (const uint8_t *)p;
    s->len = len;
    return true;
}

static bool wr_fields(mj_buf_t *b, const tl_obj_t *o);

static bool wr_value(mj_buf_t *b, uint16_t type, const tl_val_t *v)
{
    const tl_type_t *t = &tl_types[type];
    switch (t->kind) {
    case TLK_INT:
    case TLK_FLAGS:
        mj_buf_u32(b, (uint32_t)(v->kind == TLK_LONG ? (int32_t)v->u.l : v->u.i));
        return true;
    case TLK_LONG:
        mj_buf_u64(b, (uint64_t)(v->kind == TLK_INT ? (int64_t)v->u.i : v->u.l));
        return true;
    case TLK_DOUBLE:
        mj_buf_put(b, &v->u.d, 8);
        return true;
    case TLK_STRING:
    case TLK_BYTES:
        mj_buf_bytes(b, v->u.b ? v->u.b : (const uint8_t *)"", v->u.b ? v->len : 0);
        return true;
    case TLK_INT128:
    case TLK_INT256: {
        size_t n = t->kind == TLK_INT128 ? 16 : 32;
        if (!v->u.b) return false;
        mj_buf_put(b, v->u.b, n);
        return true;
    }
    case TLK_BOOL:
        mj_buf_u32(b, v->u.i ? TL_BOOL_TRUE : TL_BOOL_FALSE);
        return true;
    case TLK_TRUE:
        return true;
    case TLK_OBJ:
    case TLK_ANY:
        if (v->set == 2) {
            mj_buf_put(b, v->u.b, v->len);
            return true;
        }
        if (!v->u.o) return false;
        return tl_write(b, v->u.o);
    case TLK_BARE:
        if (!v->u.o) return false;
        return wr_fields(b, v->u.o);
    case TLK_VEC:
        mj_buf_u32(b, TL_VECTOR);
        /* fall through */
    case TLK_BVEC: {
        const tl_vec_t *vec = v->u.v;
        uint32_t n = vec ? vec->n : 0;
        mj_buf_u32(b, n);
        for (uint32_t i = 0; i < n; i++)
            if (!wr_value(b, t->arg, &vec->e[i])) return false;
        return true;
    }
    }
    return false;
}

static bool wr_fields(mj_buf_t *b, const tl_obj_t *o)
{
    const tl_ctor_t *c = &tl_ctors[o->ci];
    uint32_t flags[8] = {0};    /* the flags words, by field index (up to 8 of them is plenty) */
    int8_t fpos[8];
    int nf = 0;
    for (int i = 0; i < c->n; i++) {
        const tl_field_t *f = &tl_fields[c->first + i];
        if (tl_types[f->type].kind == TLK_FLAGS && nf < 8) {
            fpos[nf] = (int8_t)i;
            flags[nf++] = o->f[i].set ? (uint32_t)o->f[i].u.i : 0;
        }
    }
    for (int i = 0; i < c->n; i++) {
        const tl_field_t *f = &tl_fields[c->first + i];
        if (f->flag_fi < 0 || !o->f[i].set) continue;
        if (tl_types[f->type].kind == TLK_TRUE && !o->f[i].u.i) continue;
        for (int k = 0; k < nf; k++)
            if (fpos[k] == f->flag_fi) flags[k] |= 1u << f->bit;
    }
    for (int i = 0; i < c->n; i++) {
        const tl_field_t *f = &tl_fields[c->first + i];
        const tl_type_t *t = &tl_types[f->type];
        if (t->kind == TLK_FLAGS) {
            for (int k = 0; k < nf; k++)
                if (fpos[k] == i) mj_buf_u32(b, flags[k]);
            continue;
        }
        if (f->flag_fi >= 0) {
            uint32_t fl = 0;
            for (int k = 0; k < nf; k++)
                if (fpos[k] == f->flag_fi) fl = flags[k];
            if (!((fl >> f->bit) & 1)) continue;
            if (!o->f[i].set) {
                if (t->kind == TLK_TRUE) continue;
                mj_log("tl: %s.%s shares a bit that is set, and has no value", tl_name(o->id), tl_fname(f->name));
                return false;
            }
        } else if (!o->f[i].set) {
            mj_log("tl: %s needs %s", tl_name(o->id), tl_fname(f->name));
            return false;
        }
        if (!wr_value(b, f->type, &o->f[i])) {
            mj_log("tl: %s.%s could not be written", tl_name(o->id), tl_fname(f->name));
            return false;
        }
    }
    return !b->oom;
}

bool tl_write(mj_buf_t *b, const tl_obj_t *o)
{
    if (!o) return false;
    mj_buf_u32(b, o->id);
    return wr_fields(b, o);
}

/* ---- cloning ---- */

static bool clone_val(mj_arena_t *a, tl_val_t *d, const tl_val_t *s);

static tl_vec_t *clone_vec(mj_arena_t *a, const tl_vec_t *v)
{
    if (!v) return NULL;
    tl_vec_t *n = tl_vec_new(a, v->n);
    if (!n) return NULL;
    for (uint32_t i = 0; i < v->n; i++) clone_val(a, &n->e[i], &v->e[i]);
    return n;
}

static bool clone_val(mj_arena_t *a, tl_val_t *d, const tl_val_t *s)
{
    *d = *s;
    if (!s->set) return true;
    switch (s->kind) {
    case TLK_STRING:
    case TLK_BYTES:
    case TLK_INT128:
    case TLK_INT256:
        if (s->u.s) d->u.s = mj_arena_strdup(a, s->u.s, s->len);
        break;
    case TLK_OBJ:
    case TLK_ANY:
    case TLK_BARE:
        d->u.o = tl_clone(a, s->u.o);
        break;
    case TLK_VEC:
    case TLK_BVEC:
        d->u.v = clone_vec(a, s->u.v);
        break;
    default:
        break;
    }
    return true;
}

tl_obj_t *tl_clone(mj_arena_t *a, const tl_obj_t *o)
{
    if (!o) return NULL;
    tl_obj_t *n = (tl_obj_t *)mj_arena_alloc(a, sizeof *n + o->n * sizeof(tl_val_t));
    if (!n) return NULL;
    n->id = o->id;
    n->ci = o->ci;
    n->n = o->n;
    for (int i = 0; i < o->n; i++) clone_val(a, &n->f[i], &o->f[i]);
    return n;
}
