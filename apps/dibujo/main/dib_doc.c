/*
 * DIBUJO - the document. See dib_doc.h.
 */
#include "dib_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AOS_SIM_BUILTIN) || defined(DIB_HOST)
void *dib_alloc(size_t n) { return malloc(n); }
void *dib_calloc(size_t n) { return calloc(1, n); }
void  dib_free(void *p) { free(p); }
#else
#include "esp_heap_caps.h"
void *dib_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
void *dib_calloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) memset(p, 0, n);
    return p;
}
void  dib_free(void *p) { heap_caps_free(p); }
#endif

/* --------------------------------------------------------------------------
 * Rectangles and pixels
 * -------------------------------------------------------------------------- */

void dib_rect_union(dib_rect_t *a, const dib_rect_t *b)
{
    if (dib_rect_empty(b)) return;
    if (dib_rect_empty(a)) { *a = *b; return; }
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

void dib_rect_clip(dib_rect_t *r, int w, int h)
{
    if (r->x0 < 0) r->x0 = 0;
    if (r->y0 < 0) r->y0 = 0;
    if (r->x1 > w) r->x1 = w;
    if (r->y1 > h) r->y1 = h;
}

static dib_rect_t tile_rect(const dib_doc_t *d, int t)
{
    int tx = t % d->tw, ty = t / d->tw;
    dib_rect_t r = { tx * DIB_TILE, ty * DIB_TILE, (tx + 1) * DIB_TILE, (ty + 1) * DIB_TILE };
    dib_rect_clip(&r, d->w, d->h);
    return r;
}

static inline unsigned div255(unsigned v)
{
    return (v + 128 + ((v + 128) >> 8)) >> 8;
}

dib_px_t dib_over(dib_px_t dst, dib_px_t src, unsigned cov)
{
    unsigned sa = div255(DIB_A(src) * cov);
    if (sa == 0) return dst;
    unsigned da = DIB_A(dst);
    if (sa == 255 || da == 0) return (src & 0x00FFFFFF) | ((dib_px_t)sa << 24);
    unsigned t = da * (255 - sa);               /* dst's share, x255 */
    unsigned o = sa * 255 + t;                  /* out alpha, x255   */
    unsigned r = (DIB_R(src) * sa * 255 + DIB_R(dst) * t + o / 2) / o;
    unsigned g = (DIB_G(src) * sa * 255 + DIB_G(dst) * t + o / 2) / o;
    unsigned b = (DIB_B(src) * sa * 255 + DIB_B(dst) * t + o / 2) / o;
    return DIB_ARGB(div255(o), r, g, b);
}

/* --------------------------------------------------------------------------
 * Tiles and memory
 * -------------------------------------------------------------------------- */

bool dib_size_ok(int w, int h)
{
    return w >= 16 && h >= 16 && w <= DIB_MAX_SIDE && h <= DIB_MAX_SIDE;
}

static dib_px_t *tile_new(dib_doc_t *d, bool clear)
{
    if (d->tile_bytes + d->hist_bytes + DIB_TILE_BYTES > d->budget &&
        !dib_mem_make_room(d, DIB_TILE_BYTES)) {
        d->oom = true;
        return NULL;
    }
    dib_px_t *t = clear ? dib_calloc(DIB_TILE_BYTES) : dib_alloc(DIB_TILE_BYTES);
    if (!t) {
        /* the budget said yes and the heap said no: let history go */
        if (dib_mem_make_room(d, d->budget)) {
            t = clear ? dib_calloc(DIB_TILE_BYTES) : dib_alloc(DIB_TILE_BYTES);
        }
        if (!t) {
            d->oom = true;
            return NULL;
        }
    }
    d->tile_bytes += DIB_TILE_BYTES;
    return t;
}

dib_px_t *dib_tile_get(dib_doc_t *d, dib_px_t **tiles, int t)
{
    if (!tiles[t]) tiles[t] = tile_new(d, true);
    return tiles[t];
}

void dib_tile_release(dib_doc_t *d, dib_px_t **slot)
{
    if (*slot) {
        dib_free(*slot);
        *slot = NULL;
        d->tile_bytes -= DIB_TILE_BYTES;
    }
}

bool dib_tile_empty(const dib_px_t *tile)
{
    if (!tile) return true;
    for (int i = 0; i < DIB_TILE_PX; i++) {
        if (tile[i] >> 24) return false;
    }
    return true;
}

dib_px_t dib_layer_px(const dib_doc_t *d, int layer, int x, int y)
{
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return 0;
    const dib_px_t *t = d->layers[layer].tiles[(y / DIB_TILE) * d->tw + x / DIB_TILE];
    return t ? t[(y % DIB_TILE) * DIB_TILE + x % DIB_TILE] : 0;
}

static void layer_free_tiles(dib_doc_t *d, dib_layer_t *l, bool account)
{
    if (!l->tiles) return;
    for (int t = 0; t < d->ntiles; t++) {
        if (l->tiles[t]) {
            dib_free(l->tiles[t]);
            if (account) d->tile_bytes -= DIB_TILE_BYTES;
        }
    }
    dib_free(l->tiles);
    l->tiles = NULL;
}

static size_t layer_bytes(const dib_doc_t *d, const dib_layer_t *l)
{
    size_t n = 0;
    if (!l->tiles) return 0;
    for (int t = 0; t < d->ntiles; t++) {
        if (l->tiles[t]) n += DIB_TILE_BYTES;
    }
    return n;
}

/* --------------------------------------------------------------------------
 * The document
 * -------------------------------------------------------------------------- */

dib_doc_t *dib_doc_new(int w, int h, dib_px_t bg, size_t budget)
{
    if (!dib_size_ok(w, h)) return NULL;
    dib_doc_t *d = dib_calloc(sizeof(dib_doc_t));
    if (!d) return NULL;
    d->w = w;
    d->h = h;
    d->tw = (w + DIB_TILE - 1) / DIB_TILE;
    d->th = (h + DIB_TILE - 1) / DIB_TILE;
    d->ntiles = d->tw * d->th;
    d->bg = bg;
    d->budget = budget;
    d->comp = dib_alloc((size_t)w * h * 2);
    d->flt = dib_calloc(sizeof(dib_px_t *) * d->ntiles);
    d->layers[0].tiles = dib_calloc(sizeof(dib_px_t *) * d->ntiles);
    if (!d->comp || !d->flt || !d->layers[0].tiles) {
        dib_doc_free(d);
        return NULL;
    }
    snprintf(d->layers[0].name, DIB_NAME_MAX, "1");
    d->layers[0].opacity = 255;
    d->layers[0].visible = true;
    d->nlayers = 1;
    d->active = 0;
    dib_compose(d, (dib_rect_t){ 0, 0, w, h });
    return d;
}

void dib_doc_free(dib_doc_t *d)
{
    if (!d) return;
    dib_hist_clear(d);
    for (int i = 0; i < DIB_MAX_LAYERS; i++) layer_free_tiles(d, &d->layers[i], true);
    if (d->flt) {
        for (int t = 0; t < d->ntiles; t++) dib_tile_release(d, &d->flt[t]);
        dib_free(d->flt);
    }
    dib_free(d->comp);
    dib_free(d);
}

/* --------------------------------------------------------------------------
 * Composing
 * -------------------------------------------------------------------------- */

#define CHECK_SQ    16
#define CHECK_A     0xFFFFFF
#define CHECK_B     0xCFCFD4

static inline dib_px_t base_px(const dib_doc_t *d, int x, int y)
{
    unsigned a = DIB_A(d->bg);
    if (a == 255) return d->bg;
    dib_px_t c = (((x / CHECK_SQ) ^ (y / CHECK_SQ)) & 1) ? CHECK_B : CHECK_A;
    c |= 0xFF000000u;
    return a ? dib_over(c, d->bg, 255) : c;
}

typedef struct {
    const dib_px_t *px;
    unsigned op;
} srcl_t;

/* The tiles that make up tile t, bottom to top, the floating one right above
 * the active layer. */
static int tile_sources(const dib_doc_t *d, int t, srcl_t *out)
{
    int n = 0;
    for (int i = 0; i < d->nlayers; i++) {
        const dib_layer_t *l = &d->layers[i];
        if (l->visible && l->opacity && l->tiles[t]) {
            out[n].px = l->tiles[t];
            out[n].op = l->opacity;
            n++;
        }
        if (i == d->active && d->flt[t]) {
            out[n].px = d->flt[t];
            out[n].op = l->visible ? l->opacity : 255;
            n++;
        }
    }
    return n;
}

void dib_compose(dib_doc_t *d, dib_rect_t r)
{
    dib_rect_clip(&r, d->w, d->h);
    if (dib_rect_empty(&r)) return;
    srcl_t src[DIB_MAX_LAYERS + 1];
    for (int ty = r.y0 / DIB_TILE; ty <= (r.y1 - 1) / DIB_TILE; ty++) {
        for (int tx = r.x0 / DIB_TILE; tx <= (r.x1 - 1) / DIB_TILE; tx++) {
            int t = ty * d->tw + tx;
            int n = tile_sources(d, t, src);
            int x0 = tx * DIB_TILE, y0 = ty * DIB_TILE;
            int ax = x0 > r.x0 ? x0 : r.x0, bx = x0 + DIB_TILE < r.x1 ? x0 + DIB_TILE : r.x1;
            int ay = y0 > r.y0 ? y0 : r.y0, by = y0 + DIB_TILE < r.y1 ? y0 + DIB_TILE : r.y1;
            for (int y = ay; y < by; y++) {
                uint16_t *o = d->comp + (size_t)y * d->w;
                int row = (y - y0) * DIB_TILE - x0;
                for (int x = ax; x < bx; x++) {
                    dib_px_t b = base_px(d, x, y);
                    unsigned cr = DIB_R(b), cg = DIB_G(b), cb = DIB_B(b);
                    for (int k = 0; k < n; k++) {
                        dib_px_t s = src[k].px[row + x];
                        unsigned a = s >> 24;
                        if (!a) continue;
                        if (src[k].op != 255) a = div255(a * src[k].op);
                        if (a == 255) {
                            cr = DIB_R(s); cg = DIB_G(s); cb = DIB_B(s);
                        } else {
                            unsigned ia = 255 - a;
                            cr = div255(DIB_R(s) * a + cr * ia);
                            cg = div255(DIB_G(s) * a + cg * ia);
                            cb = div255(DIB_B(s) * a + cb * ia);
                        }
                    }
                    o[x] = (uint16_t)(((cr & 0xF8) << 8) | ((cg & 0xFC) << 3) | (cb >> 3));
                }
            }
        }
    }
}

void dib_compose_row_rgba(const dib_doc_t *d, int y, uint8_t *rgba)
{
    srcl_t src[DIB_MAX_LAYERS + 1];
    int ty = y / DIB_TILE;
    for (int tx = 0; tx < d->tw; tx++) {
        int t = ty * d->tw + tx;
        int n = tile_sources(d, t, src);
        int x0 = tx * DIB_TILE, x1 = x0 + DIB_TILE < d->w ? x0 + DIB_TILE : d->w;
        int row = (y - ty * DIB_TILE) * DIB_TILE - x0;
        for (int x = x0; x < x1; x++) {
            dib_px_t c = d->bg;
            for (int k = 0; k < n; k++) c = dib_over(c, src[k].px[row + x], src[k].op);
            uint8_t *o = rgba + (size_t)x * 4;
            o[0] = DIB_R(c); o[1] = DIB_G(c); o[2] = DIB_B(c); o[3] = DIB_A(c);
        }
    }
}

dib_px_t dib_pick(const dib_doc_t *d, int x, int y, bool all_layers)
{
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return 0;
    if (!all_layers) return dib_layer_px(d, d->active, x, y);
    srcl_t src[DIB_MAX_LAYERS + 1];
    int t = (y / DIB_TILE) * d->tw + x / DIB_TILE;
    int n = tile_sources(d, t, src);
    int i = (y % DIB_TILE) * DIB_TILE + x % DIB_TILE;
    dib_px_t c = DIB_A(d->bg) ? dib_over(0xFFFFFFFFu, d->bg, 255) : 0xFFFFFFFFu;
    for (int k = 0; k < n; k++) c = dib_over(c, src[k].px[i], src[k].op);
    return c | 0xFF000000u;
}

void dib_thumb(const dib_doc_t *d, int layer, uint16_t *out, int pw, int ph)
{
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            int sx = (int)(((long)x * 2 + 1) * d->w / (2L * pw));
            int sy = (int)(((long)y * 2 + 1) * d->h / (2L * ph));
            if (layer < 0) {
                out[y * pw + x] = d->comp[(size_t)sy * d->w + sx];
            } else {
                dib_px_t c = (((x / 6) ^ (y / 6)) & 1) ? (0xFF000000u | CHECK_B) : (0xFF000000u | CHECK_A);
                c = dib_over(c, dib_layer_px(d, layer, sx, sy), 255);
                out[y * pw + x] = dib_565(c);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * The history
 * -------------------------------------------------------------------------- */

typedef struct {
    int16_t   layer;
    int32_t   t;
    dib_px_t *px;               /* NULL: the tile was empty */
} tsave_t;

enum { LOP_NONE = 0, LOP_INSERTED, LOP_REMOVED, LOP_MOVED, LOP_PROPS, LOP_BG };

struct dib_hent {
    tsave_t    *s;
    int         n, cap;
    int32_t    *map;            /* while building: layer * ntiles + t -> index */
    int         lop, la, lb;
    dib_layer_t layer;          /* LOP_REMOVED: the layer; LOP_PROPS: old props */
    dib_px_t    bg;
    int         active;         /* the active layer on the other side */
    size_t      bytes;
};

static size_t hent_bytes(const dib_doc_t *d, const dib_hent_t *h)
{
    size_t n = 0;
    for (int i = 0; i < h->n; i++) {
        if (h->s[i].px) n += DIB_TILE_BYTES;
    }
    if (h->lop == LOP_REMOVED) n += layer_bytes(d, &h->layer);
    return n;
}

static void hent_free(dib_doc_t *d, dib_hent_t *h)
{
    if (!h) return;
    for (int i = 0; i < h->n; i++) dib_free(h->s[i].px);
    if (h->lop == LOP_REMOVED) layer_free_tiles(d, &h->layer, false);
    dib_free(h->s);
    dib_free(h->map);
    dib_free(h);
}

dib_hent_t *dib_hist_begin(dib_doc_t *d)
{
    dib_hent_t *h = dib_calloc(sizeof(dib_hent_t));
    if (!h) {
        d->oom = true;
        return NULL;
    }
    h->active = d->active;
    return h;
}

static bool hent_map(const dib_doc_t *d, dib_hent_t *h)
{
    if (h->map) return true;
    size_t n = (size_t)DIB_MAX_LAYERS * d->ntiles;
    h->map = dib_alloc(n * sizeof(int32_t));
    if (!h->map) return false;
    for (size_t i = 0; i < n; i++) h->map[i] = -1;
    for (int i = 0; i < h->n; i++) h->map[h->s[i].layer * d->ntiles + h->s[i].t] = i;
    return true;
}

bool dib_hist_save(dib_doc_t *d, dib_hent_t *h, int layer, int t)
{
    if (!h || !hent_map(d, h)) {
        d->oom = true;
        return false;
    }
    int32_t *m = &h->map[layer * d->ntiles + t];
    if (*m >= 0) return true;
    if (h->n == h->cap) {
        int cap = h->cap ? h->cap * 2 : 32;
        tsave_t *s = dib_alloc(sizeof(tsave_t) * cap);
        if (!s) {
            d->oom = true;
            return false;
        }
        if (h->n) memcpy(s, h->s, sizeof(tsave_t) * h->n);
        dib_free(h->s);
        h->s = s;
        h->cap = cap;
    }
    dib_px_t *cur = d->layers[layer].tiles[t], *cp = NULL;
    if (cur) {
        /* the copy is the history's: counted there once the step is pushed */
        cp = tile_new(d, false);
        if (!cp) return false;
        memcpy(cp, cur, DIB_TILE_BYTES);
        d->tile_bytes -= DIB_TILE_BYTES;
        d->hist_bytes += DIB_TILE_BYTES;
        h->bytes += DIB_TILE_BYTES;
    }
    h->s[h->n] = (tsave_t){ (int16_t)layer, t, cp };
    *m = h->n++;
    return true;
}

const dib_px_t *dib_hist_before(const dib_doc_t *d, const dib_hent_t *h, int layer, int t, bool *found)
{
    *found = false;
    if (!h) return NULL;
    if (h->map) {
        int32_t i = h->map[layer * d->ntiles + t];
        if (i < 0) return NULL;
        *found = true;
        return h->s[i].px;
    }
    for (int i = h->n - 1; i >= 0; i--) {
        if (h->s[i].layer == layer && h->s[i].t == t) {
            *found = true;
            return h->s[i].px;
        }
    }
    return NULL;
}

static void hist_drop_oldest(dib_doc_t *d)
{
    if (!d->nundo) return;
    dib_hent_t *h = d->undo[0];
    d->hist_bytes -= h->bytes;
    hent_free(d, h);
    memmove(d->undo, d->undo + 1, sizeof(d->undo[0]) * (d->nundo - 1));
    d->nundo--;
}

static void redo_clear(dib_doc_t *d)
{
    while (d->nredo) {
        dib_hent_t *h = d->redo[--d->nredo];
        d->hist_bytes -= h->bytes;
        hent_free(d, h);
    }
}

void dib_hist_end(dib_doc_t *d, dib_hent_t *h)
{
    if (!h) return;
    dib_free(h->map);
    h->map = NULL;
    if (!h->n && h->lop == LOP_NONE) {
        hent_free(d, h);
        return;
    }
    redo_clear(d);
    if (d->nundo == DIB_HIST_MAX) hist_drop_oldest(d);
    d->undo[d->nundo++] = h;
}

static void layer_insert(dib_doc_t *d, int at, dib_layer_t *l)
{
    memmove(&d->layers[at + 1], &d->layers[at], sizeof(dib_layer_t) * (d->nlayers - at));
    d->layers[at] = *l;
    memset(l, 0, sizeof(*l));
    d->nlayers++;
}

static void layer_remove(dib_doc_t *d, int at, dib_layer_t *into)
{
    *into = d->layers[at];
    memmove(&d->layers[at], &d->layers[at + 1], sizeof(dib_layer_t) * (d->nlayers - at - 1));
    memset(&d->layers[d->nlayers - 1], 0, sizeof(dib_layer_t));
    d->nlayers--;
}

static void layer_move(dib_doc_t *d, int from, int to)
{
    dib_layer_t l = d->layers[from];
    if (from < to) {
        memmove(&d->layers[from], &d->layers[from + 1], sizeof(dib_layer_t) * (to - from));
    } else {
        memmove(&d->layers[to + 1], &d->layers[to], sizeof(dib_layer_t) * (from - to));
    }
    d->layers[to] = l;
}

/* Goes from one side of the step to the other: undo and redo alike. */
static void hent_toggle(dib_doc_t *d, dib_hent_t *h, dib_rect_t *changed)
{
    size_t total = d->tile_bytes + d->hist_bytes;
    d->hist_bytes -= h->bytes;
    dib_rect_t ch = { 0, 0, 0, 0 };
    bool whole = false;

    switch (h->lop) {
    case LOP_INSERTED:
        layer_remove(d, h->la, &h->layer);
        h->lop = LOP_REMOVED;
        whole = true;
        break;
    case LOP_REMOVED:
        layer_insert(d, h->la, &h->layer);
        h->lop = LOP_INSERTED;
        whole = true;
        break;
    case LOP_MOVED: {
        layer_move(d, h->lb, h->la);
        int t = h->la; h->la = h->lb; h->lb = t;
        whole = true;
        break;
    }
    case LOP_PROPS: {
        dib_layer_t *l = &d->layers[h->la];
        uint8_t op = l->opacity; bool v = l->visible;
        l->opacity = h->layer.opacity; l->visible = h->layer.visible;
        h->layer.opacity = op; h->layer.visible = v;
        whole = true;
        break;
    }
    case LOP_BG: {
        dib_px_t b = d->bg; d->bg = h->bg; h->bg = b;
        whole = true;
        break;
    }
    default:
        break;
    }
    for (int i = 0; i < h->n; i++) {
        tsave_t *s = &h->s[i];
        if (s->layer >= d->nlayers) continue;       /* cannot happen; be safe */
        dib_px_t **slot = &d->layers[s->layer].tiles[s->t];
        dib_px_t *t = *slot; *slot = s->px; s->px = t;
        dib_rect_t r = tile_rect(d, s->t);
        dib_rect_union(&ch, &r);
    }
    int a = d->active; d->active = h->active; h->active = a;
    if (d->active >= d->nlayers) d->active = d->nlayers - 1;
    if (d->active < 0) d->active = 0;

    h->bytes = hent_bytes(d, h);
    d->hist_bytes += h->bytes;
    d->tile_bytes = total - d->hist_bytes;
    if (whole) ch = (dib_rect_t){ 0, 0, d->w, d->h };
    if (changed) dib_rect_union(changed, &ch);
}

void dib_hist_abort(dib_doc_t *d, dib_hent_t *h)
{
    if (!h) return;
    /* the tiles go back; the layers' new tiles go to the step and die with it */
    size_t held = h->bytes;
    for (int i = 0; i < h->n; i++) {
        tsave_t *s = &h->s[i];
        dib_px_t **slot = &d->layers[s->layer].tiles[s->t];
        if (*slot) {
            dib_free(*slot);
            d->tile_bytes -= DIB_TILE_BYTES;
        }
        *slot = s->px;
        s->px = NULL;
    }
    /* the saved copies were counted as history: they are layer tiles again */
    d->hist_bytes -= held;
    d->tile_bytes += held;
    h->n = 0;
    hent_free(d, h);
}

bool dib_undo(dib_doc_t *d, dib_rect_t *changed)
{
    if (!d->nundo) return false;
    dib_hent_t *h = d->undo[--d->nundo];
    hent_toggle(d, h, changed);
    d->redo[d->nredo++] = h;
    return true;
}

bool dib_redo(dib_doc_t *d, dib_rect_t *changed)
{
    if (!d->nredo) return false;
    dib_hent_t *h = d->redo[--d->nredo];
    hent_toggle(d, h, changed);
    d->undo[d->nundo++] = h;
    return true;
}

void dib_hist_clear(dib_doc_t *d)
{
    redo_clear(d);
    while (d->nundo) hist_drop_oldest(d);
}

bool dib_mem_make_room(dib_doc_t *d, size_t need)
{
    while (d->tile_bytes + d->hist_bytes + need > d->budget) {
        if (d->nredo) {
            dib_hent_t *h = d->redo[0];
            d->hist_bytes -= h->bytes;
            hent_free(d, h);
            memmove(d->redo, d->redo + 1, sizeof(d->redo[0]) * (d->nredo - 1));
            d->nredo--;
        } else if (d->nundo) {
            hist_drop_oldest(d);
        } else {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Layers
 * -------------------------------------------------------------------------- */

static void push_layer_step(dib_doc_t *d, int lop, int la, int lb, int active_before)
{
    dib_hent_t *h = dib_hist_begin(d);
    if (!h) return;
    h->lop = lop;
    h->la = la;
    h->lb = lb;
    h->active = active_before;
    dib_hist_end(d, h);
}

int dib_layer_add(dib_doc_t *d, int at, const char *name)
{
    if (d->nlayers >= DIB_MAX_LAYERS) return -1;
    if (at < 0 || at > d->nlayers) at = d->nlayers;
    dib_layer_t l = { 0 };
    l.tiles = dib_calloc(sizeof(dib_px_t *) * d->ntiles);
    if (!l.tiles) {
        d->oom = true;
        return -1;
    }
    snprintf(l.name, DIB_NAME_MAX, "%s", name ? name : "");
    l.opacity = 255;
    l.visible = true;
    int before = d->active;
    layer_insert(d, at, &l);
    d->active = at;
    push_layer_step(d, LOP_INSERTED, at, 0, before);
    return at;
}

int dib_layer_dup(dib_doc_t *d, int i)
{
    if (d->nlayers >= DIB_MAX_LAYERS || i < 0 || i >= d->nlayers) return -1;
    dib_layer_t l = d->layers[i];
    l.tiles = dib_calloc(sizeof(dib_px_t *) * d->ntiles);
    if (!l.tiles) {
        d->oom = true;
        return -1;
    }
    for (int t = 0; t < d->ntiles; t++) {
        if (!d->layers[i].tiles[t]) continue;
        l.tiles[t] = tile_new(d, false);
        if (!l.tiles[t]) {
            layer_free_tiles(d, &l, true);
            return -1;
        }
        memcpy(l.tiles[t], d->layers[i].tiles[t], DIB_TILE_BYTES);
    }
    int before = d->active;
    layer_insert(d, i + 1, &l);
    d->active = i + 1;
    push_layer_step(d, LOP_INSERTED, i + 1, 0, before);
    return i + 1;
}

bool dib_layer_delete(dib_doc_t *d, int i)
{
    if (d->nlayers <= 1 || i < 0 || i >= d->nlayers) return false;
    dib_hent_t *h = dib_hist_begin(d);
    if (!h) return false;
    size_t bytes = layer_bytes(d, &d->layers[i]);
    layer_remove(d, i, &h->layer);
    h->lop = LOP_REMOVED;
    h->la = i;
    h->active = d->active;
    if (d->active >= d->nlayers) d->active = d->nlayers - 1;
    else if (d->active > i) d->active--;
    d->tile_bytes -= bytes;
    d->hist_bytes += bytes;
    h->bytes = bytes;
    dib_hist_end(d, h);
    return true;
}

bool dib_layer_move(dib_doc_t *d, int from, int to)
{
    if (from < 0 || to < 0 || from >= d->nlayers || to >= d->nlayers || from == to) return false;
    int before = d->active;
    layer_move(d, from, to);
    if (d->active == from) d->active = to;
    else if (from < d->active && to >= d->active) d->active--;
    else if (from > d->active && to <= d->active) d->active++;
    push_layer_step(d, LOP_MOVED, from, to, before);
    return true;
}

void dib_layer_props(dib_doc_t *d, int i, uint8_t opacity, bool visible)
{
    if (i < 0 || i >= d->nlayers) return;
    dib_layer_t *l = &d->layers[i];
    if (l->opacity == opacity && l->visible == visible) return;
    /* dragging the opacity slider is one step, not fifty */
    if (d->nundo && !d->nredo) {
        dib_hent_t *top = d->undo[d->nundo - 1];
        if (top->lop == LOP_PROPS && top->la == i && !top->n && top->layer.visible == l->visible &&
            l->visible == visible) {
            l->opacity = opacity;
            return;
        }
    }
    dib_hent_t *h = dib_hist_begin(d);
    if (!h) return;
    h->lop = LOP_PROPS;
    h->la = i;
    h->layer.opacity = l->opacity;
    h->layer.visible = l->visible;
    l->opacity = opacity;
    l->visible = visible;
    dib_hist_end(d, h);
}

void dib_set_bg(dib_doc_t *d, dib_px_t bg)
{
    if (d->bg == bg) return;
    dib_hent_t *h = dib_hist_begin(d);
    if (!h) return;
    h->lop = LOP_BG;
    h->bg = d->bg;
    d->bg = bg;
    dib_hist_end(d, h);
}

bool dib_layer_merge_down(dib_doc_t *d, int i)
{
    if (i <= 0 || i >= d->nlayers) return false;
    dib_hent_t *h = dib_hist_begin(d);
    if (!h) return false;
    dib_layer_t *up = &d->layers[i];
    int lo = i - 1;
    for (int t = 0; t < d->ntiles; t++) {
        const dib_px_t *s = up->tiles[t];
        if (!s || !up->visible || !up->opacity) continue;
        if (!dib_hist_save(d, h, lo, t)) {
            dib_hist_abort(d, h);
            return false;
        }
        dib_px_t *o = dib_tile_get(d, d->layers[lo].tiles, t);
        if (!o) {
            dib_hist_abort(d, h);
            return false;
        }
        for (int k = 0; k < DIB_TILE_PX; k++) {
            if (s[k] >> 24) o[k] = dib_over(o[k], s[k], up->opacity);
        }
    }
    dib_free(h->map);
    h->map = NULL;
    size_t bytes = layer_bytes(d, up);
    layer_remove(d, i, &h->layer);
    h->lop = LOP_REMOVED;
    h->la = i;
    h->active = d->active;
    d->active = lo;
    d->tile_bytes -= bytes;
    d->hist_bytes += bytes;
    h->bytes += bytes;
    dib_hist_end(d, h);
    return true;
}

dib_rect_t dib_layer_bounds(const dib_doc_t *d, int i)
{
    dib_rect_t r = { 0, 0, 0, 0 };
    for (int t = 0; t < d->ntiles; t++) {
        if (d->layers[i].tiles[t]) {
            dib_rect_t tr = tile_rect(d, t);
            dib_rect_union(&r, &tr);
        }
    }
    return r;
}

/* --------------------------------------------------------------------------
 * The floating layer and selections
 * -------------------------------------------------------------------------- */

dib_px_t *dib_flt_tile(dib_doc_t *d, int t)
{
    return dib_tile_get(d, d->flt, t);
}

void dib_flt_clear(dib_doc_t *d, dib_rect_t *changed)
{
    for (int t = 0; t < d->ntiles; t++) {
        if (d->flt[t]) {
            dib_tile_release(d, &d->flt[t]);
            if (changed) {
                dib_rect_t r = tile_rect(d, t);
                dib_rect_union(changed, &r);
            }
        }
    }
    d->flt_box = (dib_rect_t){ 0, 0, 0, 0 };
}

void dib_flt_commit(dib_doc_t *d, dib_rect_t *changed)
{
    dib_hent_t *h = dib_hist_begin(d);
    int a = d->active;
    for (int t = 0; t < d->ntiles && h; t++) {
        const dib_px_t *s = d->flt[t];
        if (!s) continue;
        if (!dib_hist_save(d, h, a, t)) break;
        dib_px_t *o = dib_tile_get(d, d->layers[a].tiles, t);
        if (!o) break;
        for (int k = 0; k < DIB_TILE_PX; k++) {
            if (s[k] >> 24) o[k] = dib_over(o[k], s[k], 255);
        }
    }
    dib_hist_end(d, h);
    dib_flt_clear(d, changed);
}

dib_px_t *dib_copy_rect(dib_doc_t *d, dib_rect_t r, bool cut)
{
    dib_rect_clip(&r, d->w, d->h);
    if (dib_rect_empty(&r)) return NULL;
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    dib_px_t *out = dib_alloc((size_t)w * h * 4);
    if (!out) {
        d->oom = true;
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) out[y * w + x] = dib_layer_px(d, d->active, r.x0 + x, r.y0 + y);
    }
    if (cut) dib_clear_rect(d, r);
    return out;
}

void dib_clear_rect(dib_doc_t *d, dib_rect_t r)
{
    dib_rect_clip(&r, d->w, d->h);
    if (dib_rect_empty(&r)) return;
    dib_hent_t *h = dib_hist_begin(d);
    int a = d->active;
    for (int ty = r.y0 / DIB_TILE; ty <= (r.y1 - 1) / DIB_TILE && h; ty++) {
        for (int tx = r.x0 / DIB_TILE; tx <= (r.x1 - 1) / DIB_TILE; tx++) {
            int t = ty * d->tw + tx;
            if (!d->layers[a].tiles[t]) continue;
            if (!dib_hist_save(d, h, a, t)) break;
            dib_px_t *o = d->layers[a].tiles[t];
            int x0 = tx * DIB_TILE, y0 = ty * DIB_TILE;
            for (int y = (r.y0 > y0 ? r.y0 : y0); y < r.y1 && y < y0 + DIB_TILE; y++) {
                for (int x = (r.x0 > x0 ? r.x0 : x0); x < r.x1 && x < x0 + DIB_TILE; x++) {
                    o[(y - y0) * DIB_TILE + x - x0] = 0;
                }
            }
            if (dib_tile_empty(o)) dib_tile_release(d, &d->layers[a].tiles[t]);
        }
    }
    dib_hist_end(d, h);
}

/* --------------------------------------------------------------------------
 * The file
 * -------------------------------------------------------------------------- */

static void put16(uint8_t *p, unsigned v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static unsigned get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

/* PackBits over 32-bit pixels; 'out' holds at least DIB_TILE_PX * 4 + 64. */
static size_t pack_tile(const dib_px_t *t, uint8_t *out)
{
    size_t o = 0;
    int i = 0;
    while (i < DIB_TILE_PX) {
        int run = 1;
        while (i + run < DIB_TILE_PX && run < 129 && t[i + run] == t[i]) run++;
        if (run >= 2) {
            out[o++] = (uint8_t)(run + 126);
            put32(out + o, t[i]);
            o += 4;
            i += run;
            continue;
        }
        int lit = 1;
        while (i + lit < DIB_TILE_PX && lit < 128 &&
               !(i + lit + 1 < DIB_TILE_PX && t[i + lit] == t[i + lit + 1])) {
            lit++;
        }
        out[o++] = (uint8_t)(lit - 1);
        for (int k = 0; k < lit; k++) {
            put32(out + o, t[i + k]);
            o += 4;
        }
        i += lit;
    }
    return o;
}

static bool unpack_tile(const uint8_t *in, size_t n, dib_px_t *t)
{
    size_t p = 0;
    int i = 0;
    while (p < n && i < DIB_TILE_PX) {
        unsigned c = in[p++];
        if (c < 128) {
            int lit = c + 1;
            if (i + lit > DIB_TILE_PX || p + (size_t)lit * 4 > n) return false;
            for (int k = 0; k < lit; k++, p += 4) t[i++] = get32(in + p);
        } else {
            int run = c - 126;
            if (i + run > DIB_TILE_PX || p + 4 > n) return false;
            dib_px_t v = get32(in + p);
            p += 4;
            while (run--) t[i++] = v;
        }
    }
    return i == DIB_TILE_PX && p == n;
}

static void preview_size(int w, int h, int *pw, int *ph)
{
    if (w >= h) {
        *pw = DIB_PREVIEW;
        *ph = (h * DIB_PREVIEW + w / 2) / w;
    } else {
        *ph = DIB_PREVIEW;
        *pw = (w * DIB_PREVIEW + h / 2) / h;
    }
    if (*pw < 1) *pw = 1;
    if (*ph < 1) *ph = 1;
}

/* The preview, every pixel of its source box averaged: a thin line still
 * shows, faint, instead of breaking into dashes. */
static void make_preview(const dib_doc_t *d, uint16_t *out, int pw, int ph)
{
    for (int y = 0; y < ph; y++) {
        int sy0 = y * d->h / ph, sy1 = (y + 1) * d->h / ph;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < pw; x++) {
            int sx0 = x * d->w / pw, sx1 = (x + 1) * d->w / pw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    uint16_t c = d->comp[(size_t)sy * d->w + sx];
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31; n++;
                }
            }
            out[y * pw + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
}

bool dib_save(const dib_doc_t *d, const char *path)
{
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint8_t *buf = dib_alloc(DIB_TILE_BYTES + DIB_TILE_PX / 64 + 128);
    int pw, ph;
    preview_size(d->w, d->h, &pw, &ph);
    uint16_t *prev = dib_alloc((size_t)pw * ph * 2);
    bool ok = buf && prev;
    if (ok) {
        uint8_t hd[20];
        memcpy(hd, "DIB1", 4);
        put16(hd + 4, d->w);
        put16(hd + 6, d->h);
        hd[8] = (uint8_t)d->nlayers;
        hd[9] = (uint8_t)d->active;
        hd[10] = (uint8_t)d->nguides;
        hd[11] = 0;
        put32(hd + 12, d->bg);
        put16(hd + 16, pw);
        put16(hd + 18, ph);
        ok = fwrite(hd, 1, 20, f) == 20;
        make_preview(d, prev, pw, ph);
        for (int i = 0; ok && i < pw * ph; i++) {
            uint8_t b2[2];
            put16(b2, prev[i]);
            ok = fwrite(b2, 1, 2, f) == 2;
        }
        for (int i = 0; ok && i < d->nguides; i++) {
            uint8_t g[4] = { d->guides[i].vertical, 0 };
            put16(g + 2, (uint16_t)d->guides[i].pos);
            ok = fwrite(g, 1, 4, f) == 4;
        }
    }
    for (int i = 0; ok && i < d->nlayers; i++) {
        const dib_layer_t *l = &d->layers[i];
        uint8_t lh[DIB_NAME_MAX + 8] = { 0 };
        memcpy(lh, l->name, strnlen(l->name, DIB_NAME_MAX - 1));
        lh[DIB_NAME_MAX] = l->opacity;
        lh[DIB_NAME_MAX + 1] = l->visible;
        uint32_t n = 0;
        for (int t = 0; t < d->ntiles; t++) {
            if (l->tiles[t] && !dib_tile_empty(l->tiles[t])) n++;
        }
        put32(lh + DIB_NAME_MAX + 4, n);
        ok = fwrite(lh, 1, sizeof lh, f) == sizeof lh;
        for (int t = 0; ok && t < d->ntiles; t++) {
            if (!l->tiles[t] || dib_tile_empty(l->tiles[t])) continue;
            size_t sz = pack_tile(l->tiles[t], buf + 6);
            put16(buf, t);
            put32(buf + 2, (uint32_t)sz);
            ok = fwrite(buf, 1, sz + 6, f) == sz + 6;
        }
    }
    ok = (fclose(f) == 0) && ok;
    dib_free(buf);
    dib_free(prev);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

bool dib_peek(const char *path, int *w, int *h, uint16_t *out, int *pw, int *ph)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hd[20];
    bool ok = fread(hd, 1, 20, f) == 20 && !memcmp(hd, "DIB1", 4);
    if (ok) {
        *w = get16(hd + 4);
        *h = get16(hd + 6);
        *pw = get16(hd + 16);
        *ph = get16(hd + 18);
        ok = dib_size_ok(*w, *h) && *pw >= 1 && *ph >= 1 && *pw <= DIB_PREVIEW && *ph <= DIB_PREVIEW;
    }
    if (ok && out) {
        size_t n = (size_t)*pw * *ph;
        ok = fread(out, 2, n, f) == n;
        /* the file is little endian, and so are the P4 and the Mac */
    }
    fclose(f);
    return ok;
}

dib_doc_t *dib_load(const char *path, size_t budget)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    dib_doc_t *d = NULL;
    uint8_t *buf = dib_alloc(DIB_TILE_BYTES + DIB_TILE_PX / 64 + 128);
    uint8_t hd[20];
    bool ok = buf && fread(hd, 1, 20, f) == 20 && !memcmp(hd, "DIB1", 4);
    int nl = 0;
    if (ok) {
        int w = get16(hd + 4), h = get16(hd + 6);
        nl = hd[8];
        ok = nl >= 1 && nl <= DIB_MAX_LAYERS && hd[10] <= DIB_MAX_GUIDES;
        if (ok) d = dib_doc_new(w, h, get32(hd + 12), budget);
        ok = d != NULL;
        if (ok) {
            d->active = hd[9] < nl ? hd[9] : nl - 1;
            d->nguides = hd[10];
            long skip = (long)get16(hd + 16) * get16(hd + 18) * 2;
            ok = fseek(f, 20 + skip, SEEK_SET) == 0;
        }
    }
    for (int i = 0; ok && i < d->nguides; i++) {
        uint8_t g[4];
        ok = fread(g, 1, 4, f) == 4;
        d->guides[i].vertical = g[0] ? 1 : 0;
        d->guides[i].pos = (int16_t)get16(g + 2);
    }
    for (int i = 0; ok && i < nl; i++) {
        dib_layer_t *l = &d->layers[i];
        if (!l->tiles) {
            l->tiles = dib_calloc(sizeof(dib_px_t *) * d->ntiles);
            if (!l->tiles) { ok = false; break; }
        }
        uint8_t lh[DIB_NAME_MAX + 8];
        ok = fread(lh, 1, sizeof lh, f) == sizeof lh;
        if (!ok) break;
        memcpy(l->name, lh, DIB_NAME_MAX);
        l->name[DIB_NAME_MAX - 1] = 0;
        l->opacity = lh[DIB_NAME_MAX];
        l->visible = lh[DIB_NAME_MAX + 1] != 0;
        uint32_t n = get32(lh + DIB_NAME_MAX + 4);
        for (uint32_t k = 0; ok && k < n; k++) {
            uint8_t th[6];
            ok = fread(th, 1, 6, f) == 6;
            if (!ok) break;
            unsigned t = get16(th);
            uint32_t sz = get32(th + 2);
            ok = t < (unsigned)d->ntiles && sz <= DIB_TILE_BYTES + DIB_TILE_PX / 64 + 64 && !l->tiles[t];
            if (!ok) break;
            ok = fread(buf, 1, sz, f) == sz;
            dib_px_t *tile = ok ? tile_new(d, false) : NULL;
            ok = tile && unpack_tile(buf, sz, tile);
            if (tile) l->tiles[t] = tile;
        }
        d->nlayers = i + 1;
    }
    fclose(f);
    dib_free(buf);
    if (!ok) {
        dib_doc_free(d);
        return NULL;
    }
    dib_compose(d, (dib_rect_t){ 0, 0, d->w, d->h });
    return d;
}
