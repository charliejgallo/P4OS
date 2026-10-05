/*
 * DIBUJO - the document: layers of tiles, the history, the screen copy and
 * the file.
 *
 * No LVGL, no HAL: this file, dib_paint.c, dib_raster.c and dib_png.c build
 * with a bare `cc` on the Mac (tools/dib_harness.c), which is how the engine
 * is verified before the board sees it.
 *
 * A layer is a grid of 64x64 tiles of ARGB8888 (straight alpha, 0xAARRGGBB,
 * the byte order LVGL's ARGB8888 has on a little-endian CPU). A tile nobody
 * painted is NULL and costs nothing, so a layer with a signature in a corner
 * is 16 KB and not 5 MB. Everything that changes pixels works tile by tile:
 *
 *   - painting copies a tile into the history the first time a stroke
 *     touches it ("copy on write"), and that copy is also what the stroke
 *     paints over, so a translucent marker never darkens itself;
 *   - undo and redo SWAP the saved tiles with the layer's: the same
 *     function goes both ways, and nothing is copied twice;
 *   - the screen copy (comp, RGB565, the document's size) is recomposed only
 *     where something changed, from the tiles under that rectangle.
 *
 * The floating layer (flt) holds a shape, a text or a moved selection while
 * it is still being edited. It is drawn right above the active layer and is
 * never saved: committing it is one more step in the history.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIB_TILE        64
#define DIB_TILE_PX     (DIB_TILE * DIB_TILE)
#define DIB_TILE_BYTES  (DIB_TILE_PX * 4)
#define DIB_MAX_LAYERS  8
#define DIB_MAX_SIDE    1280
#define DIB_NAME_MAX    24
#define DIB_MAX_GUIDES  16
#define DIB_PREVIEW     160         /* the preview in the file, longest side */
#define DIB_HIST_MAX    96          /* steps, before the memory budget says less */

typedef uint32_t dib_px_t;          /* 0xAARRGGBB, straight alpha */

#define DIB_A(p)        ((uint32_t)(p) >> 24)
#define DIB_R(p)        (((uint32_t)(p) >> 16) & 0xFF)
#define DIB_G(p)        (((uint32_t)(p) >> 8) & 0xFF)
#define DIB_B(p)        ((uint32_t)(p) & 0xFF)
#define DIB_ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Allocation: everything big goes to PSRAM. */
void *dib_alloc(size_t n);
void *dib_calloc(size_t n);
void  dib_free(void *p);

typedef struct {
    int x0, y0, x1, y1;             /* x1, y1 exclusive; empty when x1 <= x0 */
} dib_rect_t;

static inline bool dib_rect_empty(const dib_rect_t *r) { return r->x1 <= r->x0 || r->y1 <= r->y0; }
void dib_rect_union(dib_rect_t *a, const dib_rect_t *b);
void dib_rect_clip(dib_rect_t *r, int w, int h);

typedef struct {
    char      name[DIB_NAME_MAX];
    uint8_t   opacity;              /* 0..255 */
    bool      visible;
    dib_px_t **tiles;               /* tw * th, NULL = transparent */
} dib_layer_t;

typedef struct {
    uint8_t vertical;               /* 1: a line at x = pos; 0: at y = pos */
    int16_t pos;
} dib_guide_t;

typedef struct dib_hent dib_hent_t;

typedef struct {
    int         w, h, tw, th, ntiles;
    int         nlayers, active;
    dib_layer_t layers[DIB_MAX_LAYERS];     /* 0 is the bottom one */
    dib_px_t    bg;                         /* may be transparent (alpha 0) */
    dib_guide_t guides[DIB_MAX_GUIDES];
    int         nguides;

    dib_px_t  **flt;                        /* the floating layer, ntiles */
    dib_rect_t  flt_box;                    /* where it has content */

    uint16_t   *comp;                       /* w * h RGB565, LVGL's order */

    /* the history: undo[0] is the oldest */
    dib_hent_t *undo[DIB_HIST_MAX];
    int         nundo;
    dib_hent_t *redo[DIB_HIST_MAX];
    int         nredo;
    size_t      hist_bytes;                 /* tiles held by both stacks */
    size_t      tile_bytes;                 /* tiles held by the layers and flt */
    size_t      budget;                     /* what the two may add up to */
    bool        oom;                        /* an allocation failed: tell the user */
} dib_doc_t;

/* ---- the document ---- */
dib_doc_t *dib_doc_new(int w, int h, dib_px_t bg, size_t budget);
void       dib_doc_free(dib_doc_t *d);
bool       dib_size_ok(int w, int h);

/* A pixel of one layer (0 where there is no tile). */
dib_px_t   dib_layer_px(const dib_doc_t *d, int layer, int x, int y);
/* The tile of a layer, allocated (transparent) if it was not there. NULL
 * when there is no memory; d->oom says so. */
dib_px_t  *dib_tile_get(dib_doc_t *d, dib_px_t **tiles, int t);
bool       dib_tile_empty(const dib_px_t *tile);
void       dib_tile_release(dib_doc_t *d, dib_px_t **slot);   /* frees, accounts */

/* The visible result at one point, background included, opaque. */
dib_px_t   dib_pick(const dib_doc_t *d, int x, int y, bool all_layers);

/* ---- composing ---- */
/* Recomposes comp under r (document pixels) from the layers, the floating
 * layer and the background; a transparent background shows a checkerboard. */
void dib_compose(dib_doc_t *d, dib_rect_t r);
/* One row of the picture as it would be exported: RGBA8888 bytes, the
 * background included (transparent where it is). */
void dib_compose_row_rgba(const dib_doc_t *d, int y, uint8_t *rgba);

static inline uint16_t dib_565(dib_px_t p)
{
    return (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
}

/* Straight-alpha "over": src with an extra coverage 0..255 over dst. */
dib_px_t dib_over(dib_px_t dst, dib_px_t src, unsigned cov);

/* ---- layers (each one a step in the history) ---- */
int  dib_layer_add(dib_doc_t *d, int at, const char *name);    /* index or -1 */
int  dib_layer_dup(dib_doc_t *d, int i);
bool dib_layer_delete(dib_doc_t *d, int i);                     /* not the last */
bool dib_layer_move(dib_doc_t *d, int from, int to);
bool dib_layer_merge_down(dib_doc_t *d, int i);
void dib_layer_props(dib_doc_t *d, int i, uint8_t opacity, bool visible);
void dib_set_bg(dib_doc_t *d, dib_px_t bg);
dib_rect_t dib_layer_bounds(const dib_doc_t *d, int i);        /* where it has tiles */
/* Small picture of one layer (or of the whole when layer < 0), RGB565 on a
 * checkerboard, fitted in pw x ph. */
void dib_thumb(const dib_doc_t *d, int layer, uint16_t *out, int pw, int ph);

/* ---- the history ----
 *
 * A step that changes pixels:
 *
 *     dib_hent_t *h = dib_hist_begin(d);
 *     ... dib_hist_save(d, h, layer, t) before touching tile t ...
 *     dib_hist_end(d, h);           // pushed if it saved anything
 *
 * dib_hist_save() returns the saved copy (NULL if the tile was empty),
 * which is the tile as it was before the step began: the stroke paints
 * over it. */
dib_hent_t     *dib_hist_begin(dib_doc_t *d);
bool            dib_hist_save(dib_doc_t *d, dib_hent_t *h, int layer, int t);
const dib_px_t *dib_hist_before(const dib_doc_t *d, const dib_hent_t *h, int layer, int t, bool *found);
void            dib_hist_end(dib_doc_t *d, dib_hent_t *h);
void            dib_hist_abort(dib_doc_t *d, dib_hent_t *h);   /* put the tiles back, drop it */
bool            dib_undo(dib_doc_t *d, dib_rect_t *changed);
bool            dib_redo(dib_doc_t *d, dib_rect_t *changed);
void            dib_hist_clear(dib_doc_t *d);
/* Frees the oldest steps until 'need' more bytes fit in the budget, or there
 * is nothing left to free. */
bool            dib_mem_make_room(dib_doc_t *d, size_t need);

/* ---- the floating layer ---- */
void dib_flt_clear(dib_doc_t *d, dib_rect_t *changed);
dib_px_t *dib_flt_tile(dib_doc_t *d, int t);                    /* allocated */
/* Into the active layer, as one step. */
void dib_flt_commit(dib_doc_t *d, dib_rect_t *changed);

/* ---- selections ---- */
/* Copies r of the active layer into a new ARGB buffer (w*h). With cut, the
 * pixels go away from the layer as one step. */
dib_px_t *dib_copy_rect(dib_doc_t *d, dib_rect_t r, bool cut);
void dib_clear_rect(dib_doc_t *d, dib_rect_t r);                 /* one step */

/* ---- the file (.dib) ----
 *
 *     0   "DIB1"
 *     4   u16 width, u16 height
 *     8   u8 layers, u8 active, u8 guides, u8 reserved
 *    12   u32 background ARGB
 *    16   u16 preview width, u16 preview height
 *    20   preview, RGB565 little endian, pw * ph * 2
 *    ..   guides: u8 vertical, u8 reserved, i16 position, each
 *    ..   per layer: name[24], u8 opacity, u8 visible, u16 reserved,
 *         u32 tiles stored; per tile: u16 index, u32 bytes, the tile
 *         packed (below)
 *
 * All little endian. A tile is packed as runs of 32-bit pixels: a byte n,
 * then n < 128: n + 1 pixels as they are; n >= 128: one pixel repeated
 * n - 126 times. Drawings are long runs of one colour and of transparency,
 * so a tile is often a few dozen bytes. */
bool dib_save(const dib_doc_t *d, const char *path);
dib_doc_t *dib_load(const char *path, size_t budget);
/* Reads only the header and the preview: pw, ph <= DIB_PREVIEW; out holds
 * DIB_PREVIEW * DIB_PREVIEW pixels. */
bool dib_peek(const char *path, int *w, int *h, uint16_t *out, int *pw, int *ph);
