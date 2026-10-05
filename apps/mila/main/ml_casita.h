/*
 * MILA - the casita: Mila's home, the hub (DESIGN.md section 6)
 *
 * A worker scene at 1.5 x the levels' scale (tools/blender/SPEC.md section
 * 6). The room is one picture drawn once into a full-screen cache (colour +
 * depth); Mila, the furniture and the toys are sprites depth-tested against
 * it. No bars, nothing to keep up: she wanders on her own, uses the toys the
 * player bought, chases what the finger drags, and answers a pet.
 *
 * Its buttons (play, shop, settings, a friend) are drawn in the frame.
 *
 * P4OS: the casita has a scale of its own, ML_CASITA_RES (the levels' is
 * ML_RES): its art is rendered from tools/blender with ML_RES=1.92, so the
 * room (3.4 m across, the watch's 368 px) is the screen's 720 px upright.
 * Lying down the same picture is the screen's height, less the top of the
 * wall and the front of the floor, which the watch's HUD covered too. The
 * cache is just the room's box on the screen; Mila's frames stay packed in
 * PSRAM (ML_ZIP) and are unpacked when they change (ml_zstream_t).
 */
#pragma once

#include "aos_pad.h"
#include "ml_gfx.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct app app_t;

#define ML_CASITA_RES   1.92f       /* tools/blender at ML_RES=1.92           */

bool mlc_open(app_t *a);            /* worker                                */
void mlc_close(app_t *a);           /* worker (or at exit)                   */
void mlc_step(app_t *a, float dt);  /* worker                                */
void mlc_band(app_t *a, ml_img_t *im, int y0, int y1);
/* what changed since the last frame (worker, after mlc_step) */
struct ml_dmg;
void mlc_damage(app_t *a, struct ml_dmg *d);
/* the screen turned (worker): the room again, where it goes now */
bool mlc_refit(app_t *a);
void mlc_touch(app_t *a, int code, int x, int y);   /* LVGL thread          */
/* a USB gamepad (LVGL thread): a cursor on the buttons, A presses, B pets
 * her, R sends her to a toy, L opens the day's present */
void mlc_gamepad(app_t *a, const aos_pad_t *p);
/* the friend's Mila is visiting (ml_link.c): her outfit, or NULL to leave */
void mlc_guest(app_t *a, const char *hat, uint32_t hat_col, const char *neck, uint32_t neck_col, bool on);
