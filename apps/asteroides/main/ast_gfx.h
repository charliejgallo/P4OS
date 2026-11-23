/*
 * DERIVA - the picture: Bastion's tile compositor with neon strokes
 *
 * A frame is a list of things to draw (ast_line, ast_glow, ast_ring ...)
 * over the sector's background picture. Nobody says what changed: the
 * screen is cut in 16 x 16 tiles and each tile's list of things is
 * hashed; a tile whose hash is the one the target already shows is left
 * alone, the rest get the background's pixels back and their things drawn
 * again, clipped to them, half the rows of tiles on each core. Each of the
 * panel's three buffers remembers its own hashes.
 *
 * Everything is light: a stroke is a capsule whose brightness falls off
 * with the distance to the segment (a thin white-hot core and a coloured
 * halo), added to what is under it, so the order of the list does not
 * matter and crossings glow brighter, as on a vector screen.
 *
 * The screen is 720 x 1280 upright or 1280 x 720 lying down, as the player
 * sees it. Upright the panel's buffer is drawn straight in; lying down each
 * row of tiles is drawn into a strip and written into the upright panel's
 * buffer turned (Metegol's way), so no frame goes through the PPA. The
 * copy LVGL shows under its panels is always the screen as seen.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AST_TILE      16
#define AST_WMAX      1280
#define AST_NT        3600          /* 45 x 80 tiles either way              */
#define AST_THMAX     80
#define AST_MAX_ITEMS 2600

enum {
    AST_F_CAP0   = 1,       /* line: round end at its start                   */
    AST_F_CAP1   = 2,       /* line: round end at its end                     */
    AST_F_CAPS   = 3,
    AST_F_ADD    = 4,       /* ring/disc/rect: light added instead of blended */
    AST_F_CENTER = 16,      /* text: x is its middle                          */
    AST_F_RIGHT  = 32,      /* text: x is its right end                       */
};

/* how the target holds the screen */
enum {
    AST_OUT_SCREEN = 0,     /* as seen                                        */
    AST_OUT_ROT90  = 90,    /* the panel's buffer, screen (x, y) at (719 - y, x) */
    AST_OUT_ROT270 = 270    /* the panel's buffer, screen (x, y) at (y, 1279 - x) */
};

typedef struct {
    int16_t x0, y0, x1, y1;
} ast_rect_t;

typedef struct {
    uint8_t  kind, flags, a, aux;
    int16_t  x0, y0, x1, y1;        /* what it covers, inside the screen      */
    int16_t  p[6];
    uint16_t color, color2;
    uint32_t hash;
} ast_item_t;

typedef struct {
    int        W, H, TW, TH;        /* the screen as seen, and in tiles       */
    const uint16_t *bg;             /* W x H, or NULL (black)                 */
    uint32_t   salt;                /* mixed into every tile's hash           */
    ast_rect_t clip;                /* what ast_gfx_clip() keeps things inside */
    bool       clip_on;

    ast_item_t *item;
    int        nitem;
    bool       overflow;

    uint32_t  *cur;                 /* this frame's hash of every tile        */
    uint8_t   *need;
    uint16_t  *band_ix;
    int        band_cap;
    int        band_at[AST_THMAX + 1];
    uint16_t  *strip[2];
    uint8_t    glow[64 * 64];
    uint8_t    lut_h[256], lut_c[256];  /* a stroke's halo and core by 4 d^2  */

    int        drawn_tiles;

    /* one frame's job, for the two halves */
    uint16_t  *fb;
    int        out;
    int        split;
    bool       one_core;
} ast_gfx_t;

static inline uint16_t ast_rgb(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

/* the app's own allocator: PSRAM or nothing, and what it still holds */
void  *ast_malloc(size_t n);
void  *ast_calloc(size_t n);
void   ast_free(void *p);
size_t ast_mem_used(void);

bool ast_gfx_init(ast_gfx_t *g);
void ast_gfx_free(ast_gfx_t *g);
void ast_gfx_size(ast_gfx_t *g, int w, int h);
void ast_gfx_world(ast_gfx_t *g, const uint16_t *bg);

void ast_gfx_begin(ast_gfx_t *g);
/* what comes next stays inside this rectangle (the field), or not; one
 * rectangle a frame (off leaves it as it was for what is already listed) */
void ast_gfx_clip(ast_gfx_t *g, bool on, int x0, int y0, int x1, int y1);
/* a neon stroke; ends in pixels (fractions count); w is the thickness
 * (1 = the usual), k its brightness 0..255. Long strokes are cut in pieces
 * that meet flat, so the hashes stay local and the joints do not glow. */
void ast_line(ast_gfx_t *g, float ax, float ay, float bx, float by, uint32_t rgb, float w, int k, int caps);
/* a closed outline through n points, each joint lit once */
void ast_poly(ast_gfx_t *g, const float *xy, int n, uint32_t rgb, float w, int k);
void ast_glow(ast_gfx_t *g, float x, float y, int r, uint32_t rgb, int k);
void ast_ring(ast_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint32_t rgb, int a, int flags);
void ast_disc(ast_gfx_t *g, int cx, int cy, int rx, int ry, uint32_t rgb, int a, int flags);
void ast_rect(ast_gfx_t *g, int x, int y, int w, int h, uint32_t rgb, int a, int flags);

/* the vector font (ast_font.c): UTF-8, upper case, the Latin letters with
 * their accents; u is the size of a unit (a capital is 4 x 6 units).
 * Returns the width in pixels. */
float ast_text(ast_gfx_t *g, float x, float y, const char *s, float u, uint32_t rgb, int k, int flags);
float ast_text_width(const char *s, float u);

/* Draws the frame into fb, whose tiles' hashes are in 'shown' (AST_NT of
 * them, updated); full: draw it all. Returns the tiles drawn. */
int  ast_gfx_end(ast_gfx_t *g, uint16_t *fb, int out, uint32_t *shown, bool full);
