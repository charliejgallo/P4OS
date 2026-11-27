/*
 * P4OS - Mensajes: TL, the language of Telegram's API, read and written.
 *
 * Nothing here knows a constructor by hand. tools/tl_gen.py turns the subset
 * of the schema into tables (tl_schema.c) and this is one walk over them in
 * each direction:
 *
 *   reading  bytes -> a tree of tl_obj_t in an arena. Every field of every
 *            constructor is read, the ones the app never looks at too,
 *            because TL does not say how long anything is: to step over a
 *            field one has to know its type. gzip_packed is opened on the
 *            way (tinfl) wherever it appears.
 *   writing  a tl_obj_t the app filled by field name -> bytes. The flags
 *            words are worked out from the optional fields that were set,
 *            so a request never carries a bit without its value.
 *
 * Fields are asked for by name (TLN_id, TLN_message), and the number is the
 * same in every constructor: tl_i(msg, TLN_id) is a message's id and
 * tl_l(user, TLN_id) a user's, without knowing which kind of message or
 * user came.
 *
 * The arena: one pool per answer (or per request), freed in one go. All of
 * it is PSRAM on the board (malloc of blocks of 64 KB).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tl_schema.h"

/* ---- the generated tables ---- */

enum {
    TLK_NONE = 0,
    TLK_FLAGS, TLK_INT, TLK_LONG, TLK_DOUBLE, TLK_STRING, TLK_BYTES,
    TLK_INT128, TLK_INT256, TLK_BOOL, TLK_TRUE,
    TLK_OBJ,                    /* a boxed object of one type (arg: TLT_)      */
    TLK_BARE,                   /* a bare constructor (arg: tl_ctors index)   */
    TLK_VEC,                    /* Vector<t> (arg: tl_types index of t)       */
    TLK_BVEC,                   /* vector<t>, bare                            */
    TLK_ANY,                    /* !X: any boxed object                       */
};

typedef struct {
    uint8_t  kind;
    uint16_t arg;
} tl_type_t;

typedef struct {
    uint16_t name;              /* TLN_                                       */
    uint16_t type;              /* tl_types index                             */
    int8_t   flag_fi;           /* the flags field it hangs on (index in the constructor), -1 none */
    uint8_t  bit;
} tl_field_t;

typedef struct {
    uint32_t id;
    uint32_t name;              /* offset in tl_names                         */
    uint16_t result;            /* TLT_, 0xffff for a function                */
    uint16_t first;             /* its fields in tl_fields                    */
    uint8_t  n;
    uint8_t  func;
} tl_ctor_t;

extern const tl_type_t  tl_types[TL_NTYPES];
extern const tl_field_t tl_fields[TL_NFIELDS];
extern const tl_ctor_t  tl_ctors[TL_NCTORS];
extern const uint16_t   tl_by_id[TL_NCTORS];
extern const uint32_t   tl_fname_off[TL_NNAMES];
extern const char       tl_names[];

#define TL_VECTOR      0x1cb5c415u
#define TL_BOOL_TRUE   0x997275b5u
#define TL_BOOL_FALSE  0xbc799737u
#define TL_GZIP_PACKED 0x3072cfa1u
#define TL_RPC_RESULT  0xf35c6d01u
#define TL_CONTAINER   0x73f1f8dcu

/* ---- the arena ---- */

typedef struct mj_arena_blk mj_arena_blk_t;
typedef struct {
    mj_arena_blk_t *head;
    size_t used;                /* bytes handed out, for the measure          */
    size_t held;                /* bytes of blocks                            */
} mj_arena_t;

void *mj_arena_alloc(mj_arena_t *a, size_t n);        /* zeroed, 8-aligned  */
char *mj_arena_strdup(mj_arena_t *a, const char *s, size_t n);
void  mj_arena_free(mj_arena_t *a);

/* ---- the tree ---- */

typedef struct tl_obj tl_obj_t;
typedef struct tl_vec tl_vec_t;

typedef struct {
    uint8_t  kind;              /* TLK_ of the field's type; TLK_OBJ for any object */
    uint8_t  set;               /* an optional field that came (or was set)   */
    uint16_t pad;
    uint32_t len;               /* strings and bytes                          */
    union {
        int32_t        i;
        int64_t        l;
        double         d;
        const char    *s;       /* strings: NUL-terminated copies             */
        const uint8_t *b;       /* bytes, int128, int256                      */
        tl_obj_t      *o;
        tl_vec_t      *v;
    } u;
} tl_val_t;

struct tl_obj {
    uint32_t id;
    uint16_t ci;                /* index in tl_ctors                          */
    uint16_t n;
    tl_val_t f[];
};

struct tl_vec {
    uint32_t n;
    uint32_t pad;
    tl_val_t e[];
};

/* ---- reading ---- */

/* One boxed value from p: an object, a vector or a Bool. *used, if given,
 * gets the bytes it took. false on anything malformed or unknown (the
 * reason goes to the log once per call). */
bool tl_read(mj_arena_t *a, const uint8_t *p, size_t n, tl_val_t *out, size_t *used);
/* The same for an object only: NULL when it is not one. */
tl_obj_t *tl_read_obj(mj_arena_t *a, const uint8_t *p, size_t n, size_t *used);
/* Bytes that inflate (gzip_packed's packed_data, also used by the MTProto
 * layer for a container's body). NULL on error; *out_n its length. */
uint8_t *tl_gunzip(mj_arena_t *a, const uint8_t *p, size_t n, size_t *out_n);

/* ---- asking a tree ---- */

const tl_val_t *tl_get(const tl_obj_t *o, uint16_t name);   /* NULL: no such field, or not set */
static inline bool tl_has(const tl_obj_t *o, uint16_t name) { return tl_get(o, name) != NULL; }
int32_t     tl_i(const tl_obj_t *o, uint16_t name);          /* 0 when absent  */
int64_t     tl_l(const tl_obj_t *o, uint16_t name);          /* int or long    */
double      tl_d(const tl_obj_t *o, uint16_t name);
const char *tl_s(const tl_obj_t *o, uint16_t name);          /* "" when absent */
const uint8_t *tl_b(const tl_obj_t *o, uint16_t name, uint32_t *len);
bool        tl_t(const tl_obj_t *o, uint16_t name);          /* a true flag, a Bool */
tl_obj_t   *tl_o(const tl_obj_t *o, uint16_t name);
tl_vec_t   *tl_v(const tl_obj_t *o, uint16_t name);
static inline uint32_t tl_vn(const tl_vec_t *v) { return v ? v->n : 0; }
static inline tl_obj_t *tl_vo(const tl_vec_t *v, uint32_t i)
{
    return (v && i < v->n && (v->e[i].kind == TLK_OBJ || v->e[i].kind == TLK_BARE)) ? v->e[i].u.o : NULL;
}
static inline int64_t tl_vl(const tl_vec_t *v, uint32_t i)
{
    if (!v || i >= v->n) return 0;
    return v->e[i].kind == TLK_LONG ? v->e[i].u.l : v->e[i].u.i;
}
/* An object's constructor name ("message"), for the log. */
const char *tl_name(uint32_t id);
const char *tl_fname(uint16_t name);
int tl_ctor_index(uint32_t id);                             /* -1: unknown    */

/* ---- writing ---- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    bool oom;
} mj_buf_t;

void mj_buf_put(mj_buf_t *b, const void *d, size_t n);
void mj_buf_u32(mj_buf_t *b, uint32_t v);
void mj_buf_u64(mj_buf_t *b, uint64_t v);
void mj_buf_bytes(mj_buf_t *b, const void *d, size_t n);   /* TL string/bytes */
void mj_buf_free(mj_buf_t *b);

/* A constructor or function to fill: every field unset. NULL for an id the
 * tables do not have. */
tl_obj_t *tl_new(mj_arena_t *a, uint32_t id);
tl_vec_t *tl_vec_new(mj_arena_t *a, uint32_t n);

/* Setters by name; false when the constructor has no such field (a mistake
 * the log names). The value's kind is the field's own. Strings and bytes
 * are not copied: they must live until tl_write. */
bool tl_set_i(tl_obj_t *o, uint16_t name, int32_t v);
bool tl_set_l(tl_obj_t *o, uint16_t name, int64_t v);
bool tl_set_d(tl_obj_t *o, uint16_t name, double v);
bool tl_set_s(tl_obj_t *o, uint16_t name, const char *s);
bool tl_set_b(tl_obj_t *o, uint16_t name, const void *p, uint32_t len);
bool tl_set_t(tl_obj_t *o, uint16_t name);                  /* a true flag    */
bool tl_set_bool(tl_obj_t *o, uint16_t name, bool v);
bool tl_set_o(tl_obj_t *o, uint16_t name, tl_obj_t *child);
bool tl_set_v(tl_obj_t *o, uint16_t name, tl_vec_t *v);
static inline void tl_vset_o(tl_vec_t *v, uint32_t i, tl_obj_t *o) { v->e[i].kind = TLK_OBJ; v->e[i].set = 1; v->e[i].u.o = o; }
static inline void tl_vset_l(tl_vec_t *v, uint32_t i, int64_t x) { v->e[i].kind = TLK_LONG; v->e[i].set = 1; v->e[i].u.l = x; }
static inline void tl_vset_i(tl_vec_t *v, uint32_t i, int32_t x) { v->e[i].kind = TLK_INT; v->e[i].set = 1; v->e[i].u.i = x; }
static inline void tl_vset_s(tl_vec_t *v, uint32_t i, const char *s)
{
    v->e[i].kind = TLK_STRING; v->e[i].set = 1; v->e[i].u.s = s; v->e[i].len = 0;
}

/* Appends the boxed object (its id and its fields) to b. false on a field
 * that is required and was not set. */
bool tl_write(mj_buf_t *b, const tl_obj_t *o);

/* A copy of an object into another arena (what the store keeps of an answer
 * whose arena goes away). Strings and bytes are copied too. */
tl_obj_t *tl_clone(mj_arena_t *a, const tl_obj_t *o);
