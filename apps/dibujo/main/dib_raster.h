/*
 * DIBUJO - shapes: paths, an antialiased rasterizer, and the floating
 * object that a shape, a text or a moved selection is while it is still
 * being edited.
 *
 * The rasterizer fills any set of closed contours with the non-zero rule,
 * four sub-rows per pixel and exact coverage across, so edges come out
 * smooth at any angle. A stroke is built as geometry, not as a second
 * pass: a ring (the outline grown and shrunk by half the width, the inner
 * one wound the other way) for rectangles and ellipses, and for lines and
 * polygons a quad per segment plus a round cap at every vertex, all wound
 * the same way so that their union is filled once.
 *
 * An object lives in a box: centre, width, height and angle, in document
 * pixels. Moving, scaling and turning it only change those; it is drawn
 * again into the floating layer each time, so nothing blurs until it is
 * committed. A text or a selection is a bitmap drawn through the inverse of
 * the box's transform, bilinear.
 */
#pragma once

#include "dib_doc.h"

typedef struct {
    float x, y;
} dib_pt_t;

typedef struct {
    dib_pt_t *pts;
    int       n, cap;
    int      *start;                /* where each contour begins */
    int       nc, ccap;
    bool      oom;
} dib_path_t;

void dib_path_init(dib_path_t *p);
void dib_path_free(dib_path_t *p);
void dib_path_reset(dib_path_t *p);
void dib_path_move(dib_path_t *p, float x, float y);   /* starts a contour */
void dib_path_line(dib_path_t *p, float x, float y);
/* Closed shapes, turned by 'ang' radians about (cx, cy); 'rev' winds them
 * the other way (the hole of a ring). */
void dib_path_ellipse(dib_path_t *p, float cx, float cy, float rx, float ry, float ang, bool rev);
void dib_path_rrect(dib_path_t *p, float cx, float cy, float w, float h, float r, float ang, bool rev);
void dib_path_circle(dib_path_t *p, float cx, float cy, float r);
/* A thick polyline with round joins and caps. */
void dib_path_polyline(dib_path_t *p, const dib_pt_t *pts, int n, bool closed, float width);

/* Row callback: coverage 0..255 of pixels x0 .. x0 + n - 1 on row y. */
typedef void (*dib_span_fn)(void *user, int y, int x0, int n, const uint8_t *cov);
/* Fills the path inside the clip; scratch is allocated per call. */
bool dib_raster_fill(const dib_path_t *p, dib_rect_t clip, dib_span_fn fn, void *user);

/* ---- the floating object ---- */

typedef enum {
    DIB_OBJ_LINE = 0,
    DIB_OBJ_RECT,
    DIB_OBJ_ELLIPSE,
    DIB_OBJ_POLY,
    DIB_OBJ_ARROW,
    DIB_OBJ_BITMAP,                 /* a text, a selection, an imported picture */
    DIB_OBJ_COUNT
} dib_obj_kind_t;

#define DIB_POLY_MAX    64

typedef struct {
    int       kind;
    float     cx, cy, w, h, ang;    /* the box; a line runs along w, h unused */
    float     stroke_w, radius;
    bool      fill, stroke;
    dib_px_t  stroke_c, fill_c;
    dib_pt_t  poly[DIB_POLY_MAX];   /* in the box, -0.5 .. 0.5 */
    int       npoly;
    dib_px_t *bmp;                  /* DIB_OBJ_BITMAP, owned by the object */
    int       bw, bh;
} dib_obj_t;

/* The box's four corners (0 TL, 1 TR, 2 BR, 3 BL) in document pixels. */
void dib_obj_corners(const dib_obj_t *o, dib_pt_t out[4]);
/* A line's two ends. */
void dib_obj_ends(const dib_obj_t *o, dib_pt_t *a, dib_pt_t *b);
void dib_obj_set_ends(dib_obj_t *o, dib_pt_t a, dib_pt_t b);
/* Where it draws, stroke included. */
dib_rect_t dib_obj_bounds(const dib_obj_t *o);
/* Whether a document point is inside the box (with a margin in px). */
bool dib_obj_hit(const dib_obj_t *o, float x, float y, float margin);
/* Clears the floating layer and draws the object into it. */
void dib_obj_render(dib_doc_t *d, const dib_obj_t *o, dib_rect_t *changed);
void dib_obj_free(dib_obj_t *o);
