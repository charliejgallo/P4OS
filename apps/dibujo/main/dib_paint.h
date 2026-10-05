/*
 * DIBUJO - painting: brushes, the stroke, the fill.
 *
 * A stroke is a row of round "dabs" along the (smoothed) path of the finger.
 * Each dab does not paint the layer directly: it raises a mask of the
 * stroke (16 bits a pixel, only on the tiles it touches), and the pixel is
 * recomputed from the tile as it was before the stroke plus the colour at
 * mask x opacity. So:
 *
 *   - opacity caps the stroke: a 50 % marker going over itself stays 50 %
 *     (the marker keeps the mask's maximum; the others accumulate it);
 *   - the airbrush's low flow builds up while the finger lingers;
 *   - the tile "before" is the copy the history already keeps, so undoing a
 *     stroke and painting it cost the same single copy.
 *
 * Symmetry repeats every dab mirrored about the canvas's middle: across,
 * up and down, or both.
 */
#pragma once

#include "dib_doc.h"

typedef enum {
    DIB_BR_PENCIL = 0,          /* hard edge                                 */
    DIB_BR_SOFT,                /* soft edge                                 */
    DIB_BR_AIR,                 /* soft, low flow, builds up while held      */
    DIB_BR_MARKER,              /* translucent, does not darken itself       */
    DIB_BR_ERASER,
    DIB_BR_COUNT
} dib_brush_kind_t;

enum { DIB_SYM_NONE = 0, DIB_SYM_V, DIB_SYM_H, DIB_SYM_4 };

typedef struct {
    int      kind;
    float    size;              /* diameter, document px                     */
    float    opacity;           /* 0..1                                      */
    float    smooth;            /* 0..1: how much the line lags to calm down */
    dib_px_t color;             /* opaque                                    */
    int      sym;
} dib_brush_t;

/* What each kind is made of; the UI shows the defaults. */
typedef struct {
    float hardness;             /* 1: hard edge; 0: soft all the way         */
    float flow;                 /* how much one dab adds                     */
    float spacing;              /* between dabs, x diameter                  */
    bool  max_mode;             /* the marker                                */
    float def_size, def_opacity;
} dib_brush_def_t;
extern const dib_brush_def_t dib_brush_defs[DIB_BR_COUNT];

typedef struct {
    dib_doc_t  *doc;
    dib_hent_t *hist;
    dib_brush_t br;
    int         layer;
    uint16_t  **mask;           /* per tile, NULL: untouched                 */
    float       fx, fy;         /* the finger                                */
    float       sx, sy;         /* the smoothed point                        */
    float       lx, ly;         /* the last dab                              */
    bool        active;
    bool        any;            /* something painted                         */
    dib_rect_t  dirty;          /* changed since the last take               */
    uint32_t    seed;
} dib_stroke_t;

bool dib_stroke_begin(dib_stroke_t *s, dib_doc_t *d, const dib_brush_t *br, float x, float y);
void dib_stroke_to(dib_stroke_t *s, float x, float y);
/* The finger rests: the airbrush keeps spraying (ms since the last call). */
void dib_stroke_hold(dib_stroke_t *s, float ms);
/* Catches up with the finger and pushes the step. */
void dib_stroke_end(dib_stroke_t *s);
/* Takes it all back (a pinch's first finger), nothing in the history. */
void dib_stroke_cancel(dib_stroke_t *s);
/* What changed since the last call; false if nothing. */
bool dib_stroke_take_dirty(dib_stroke_t *s, dib_rect_t *r);

/* Flood fill of the active layer from (x, y), with the colour at the given
 * opacity. tolerance 0..255 per channel; all_layers: the region is decided
 * by what is seen, not by the layer alone. One step; false if nothing
 * changed. The region grows by one pixel to cover the soft edge of a
 * line. */
bool dib_fill(dib_doc_t *d, int x, int y, dib_px_t color, float opacity, int tolerance,
              bool all_layers, dib_rect_t *changed);
