/*
 * P4OS - gemas (from AmoledOS): the jewels' art
 *
 * The jewels are not stored bitmaps: they are drawn in code when the app
 * opens, pixel by pixel, into RGB565A8 sprites that LVGL then paints like any
 * other image. It comes out cheaper than carrying bitmaps in the .so (an 86x86
 * sprite is 22 KB; seven jewels would be 155 KB of flash) and it also lets the
 * palette or the size be changed at run time: P4OS asks for the cell size the
 * current orientation leaves room for (86 px upright, 84 lying down).
 *
 * The format is RGB565A8 (colour and alpha in separate planes, 3 bytes per
 * pixel) because it is the only one with an alpha channel that LVGL's drawer
 * blends without converting anything: the screen is RGB565 too.
 *
 * Each jewel is a convex polygon with facets. For each pixel, which edge the
 * ray from the centre falls on is looked up, and from that comes the relative
 * distance to the edge (the cut's "depth") and which facet is being looked at.
 * From those two things come the central table, the crown, the dark fillet at
 * the edge and the specular highlight.
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/* The watch drew everything for a 44 px cell; the shapes are defined relative
 * to that, and every size here scales from it. */
#define GM_CELL_REF     44
#define GM_TYPES        7       /* jewel colours */

/* Types of special jewel (the ones formed by lining up 4 or more) */
typedef enum {
    GM_SP_NONE = 0,
    GM_SP_FLAME,        /* line up 4: it explodes in a 3x3          */
    GM_SP_STAR,         /* an L or T shape: it clears a row and a column */
    GM_SP_HYPER,        /* line up 5: it takes a whole colour        */
} gm_special_t;

typedef struct {
    lv_image_dsc_t dsc;
    uint8_t       *data;        /* RGB565 (2 bytes) per pixel + A8 plane */
} gm_sprite_t;

typedef struct {
    gm_sprite_t gem[GM_TYPES];  /* the ordinary jewels       */
    gm_sprite_t hyper;          /* the rainbow hypercube     */
    gm_sprite_t flame;          /* flame, drawn on top       */
    gm_sprite_t star;           /* four-pointed sparkle      */
    gm_sprite_t tile;           /* board background, 2x2 cells, opaque */
    int         cell;           /* side of the cell and of every sprite, px */
    bool        ready;
} gm_art_t;

/* Renders every sprite at 'cell' pixels a side. */
bool gm_art_init(gm_art_t *art, int cell);
void gm_art_free(gm_art_t *art);

/* Representative colour of each jewel: used by the sparks and the labels. */
uint32_t gm_art_color(int type);
uint32_t gm_art_color_light(int type);
