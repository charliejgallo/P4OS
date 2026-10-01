/*
 * TURBO - the pseudo-3D renderer
 *
 * The camera is 2 m up, 3.2 m behind the car, with no pitch: the horizon of
 * a flat road is row TB_HOR and vertical lines stay vertical. The road is
 * drawn front to back, segment by segment, one screen row at a time: each
 * row is a run of ground, rumble, asphalt, rumble, ground, so every pixel
 * below the horizon is written once. A segment hidden by a hill in front is
 * skipped and remembers how far down it is clipped; the sprites standing on
 * it (the scenery, the traffic) are then drawn back to front under that
 * clip. The player's car is a fixed sprite on top, then the HUD.
 *
 * P4OS: tb_render_prepare() does the geometry once per frame, down to every
 * row's spans in whole pixels; tb_render_band() then fills any rectangle of
 * the screen: a band of rows upright, a strip of columns lying down (the
 * frame is drawn turned for the panel there, turbo.c). Bands only read what
 * prepare wrote, so two cores draw two bands at once.
 */
#pragma once

#include "tb_art.h"
#include "tb_game.h"
#include "tb_gfx.h"
#include "tb_track.h"

#define TB_DRAW         170         /* segments drawn: 850 m                   */

typedef struct tb_render tb_render_t;

tb_render_t *tb_render_new(void);
void tb_render_free(tb_render_t *r);
/* a stage's colours and textures (after tb_art_load_stage) */
void tb_render_stage(tb_render_t *r, const tb_track_t *t);
/* frees the player's coloured car; the next frame colours it again */
void tb_render_car_drop(tb_render_t *r);
/* the player's paint (the garage) */
void tb_render_paint(tb_render_t *r, const tb_paint_t *p, const tb_paint_t *rival);
/* PSRAM held by the renderer (the coloured car and the panorama), bytes */
uint32_t tb_render_bytes(const tb_render_t *r);
/* once per frame, before the bands (for the current tb_view) */
void tb_render_prepare(tb_render_t *r, const tb_game_t *g, float dt);
/* the pixels of im's clip rectangle; only reads what prepare wrote */
void tb_render_band(const tb_render_t *r, tb_img_t *im, const tb_game_t *g);
/* both, over the whole of im */
void tb_render_world(tb_render_t *r, tb_img_t *im, const tb_game_t *g, float dt);
