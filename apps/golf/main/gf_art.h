/*
 * GOLF - the art rendered in Blender: the golfer and the props
 *
 * Everything comes from one file, golf.pak (tools/pack_assets.py), read when
 * the app opens:
 *
 *   - the TREES and the flag, baked in colour: seen from the side for the 3D
 *     view and from above (with their shadow) for the map, each with mip
 *     levels so a tree 10 px wide is filtered and not sampled;
 *   - the GOLFER, as a lighting pass plus a region id per pixel
 *     (tools/blender/SPEC.md). gf_art colours each frame with the outfit's
 *     palette the first time it is needed and keeps it, until the outfit
 *     changes. Hats are layers of their own composited on top.
 *
 * Frames are positioned in the swing camera's frame (the watch's 368x448 at
 * GF_ART_SCALE times the pixels): drawing one where that frame's centre
 * falls on the camera's principal point puts the golfer exactly beside the
 * ball the 3D view draws.
 */
#pragma once

#include "gf_gfx.h"
#include "gf_outfit.h"
#include "gf_world.h"

#include <stdbool.h>
#include <stdint.h>

enum { SEQ_SWING = 0, SEQ_IDLE, SEQ_CHEER, SEQ_SAD, SEQ_TURN, SEQ_N };

#define GF_SWING_TOP     10     /* last frame of the backswing               */
#define GF_SWING_IMPACT  14

typedef struct gf_tree_art gf_tree_art_t;

/* Reads the pack. false if the file is missing or broken: the game still
 * runs, with trees drawn by code and no golfer. */
bool gf_art_load(const char *path);
void gf_art_free(void);
bool gf_art_have_golfer(void);

/* trees */
const gf_tree_art_t *gf_art_trees(void);
bool  gf_art_tree_top(const gf_tree_art_t *a, int kind);
/* ppm: screen pixels per metre; size: the tree's scale (1 = normal) */
void  gf_art_tree_draw_top(const gf_tree_art_t *a, gf_img_t *im, int kind, float cx, float cy, float ppm, float size, int tint);
void  gf_art_tree_shadow(const gf_tree_art_t *a, gf_img_t *im, int kind, float cx, float cy, float ppm, float size);
const gf_mip_t *gf_art_tree_side(int kind);
const gf_mip_t *gf_art_flag(int frame);         /* 4 frames, NULL if none  */
/* the art's own size of a kind, metres: the world uses it for collisions */
float gf_art_tree_height(int kind);
float gf_art_tree_radius(int kind);

/* the golfer */
int   gf_art_frames(int seq);
/* A new outfit: its colours, and which hat (the old hat's frames go). */
void  gf_art_outfit(const uint8_t eq[CAT_N]);
/* Draws a frame, coloured with the outfit, its ground shadow first if
 * 'shadow': the frame's rectangle in the frame it was rendered in (the
 * swing camera's, GF_ART_PPX / PPY at its centre; the turntable's) goes to
 * (dx, dy) + its own offset. box = x0, y0, x1, y1 of what it covered.
 * false if the frame is not prepared (gf_art_prepare). From LVGL's task. */
bool  gf_art_draw(gf_img_t *im, int seq, int frame, int dx, int dy, bool shadow, int box[4]);
/* where a frame's body sits in its frame, and its size */
bool  gf_art_frame_box(int seq, int frame, int *x0, int *y0, int *w, int *h);
/* Reads into PSRAM what the sequences in mask (1 << SEQ_*) need to be drawn.
 * From the worker only: it reads the card. Returns the bytes read. */
size_t gf_art_prepare(unsigned mask);
/* the sequences prepared so far (1 << SEQ_*): the UI may draw those while
 * the worker is still reading the rest */
unsigned gf_art_ready(void);
/* From LVGL's task, before a new outfit is handed to the worker: nothing is
 * drawn until the worker has prepared the sequences again, so it may free
 * the old hat's frames while LVGL is not looking at them */
void  gf_art_hold(void);
/* frees one sequence (the shop's turntable, the card's reactions) */
void  gf_art_release(int seq);
