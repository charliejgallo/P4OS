/*
 * TRUCO - the cards, drawn in code
 *
 * There are no bitmaps: each card is drawn with LVGL's primitives onto a
 * canvas of its own, once, when it is dealt. After that the card is an object
 * that moves; LVGL redraws only the area it passed over.
 *
 * The canvas is ARGB8888 and not RGB565 for a concrete reason: the cards have
 * rounded corners and a shadow, and both need the green baize underneath to
 * show through. With RGB565 the background would have to be painted inside
 * each card and made to match the baize's gradient, which is exactly the kind
 * of seam that shows. It costs 4 bytes per pixel in PSRAM (134 KB per card at
 * x2) and there is plenty of that.
 *
 * P4OS: the watch's drawing is kept number for number and multiplied by a
 * scale (tr_cards_set_scale), so a card is REDRAWN at 124 x 192 upright
 * rather than enlarged: every line stays one line, only thicker.
 */
#pragma once

#include "lvgl.h"

/* Sizes at the current scale; tr_cards_set_scale() sets them. */
extern int tr_card_w, tr_card_h, tr_pad;

#define TR_CARD_W   tr_card_w               /* 62 x 96 on the watch   */
#define TR_CARD_H   tr_card_h
#define TR_PAD      tr_pad                  /* room for the shadow    */
#define TR_CV_W     (TR_CARD_W + TR_PAD * 2)
#define TR_CV_H     (TR_CARD_H + TR_PAD * 2)

/* Watch pixels to panel pixels, for the cards. Before any canvas is made:
 * the canvases take their size from it. */
void tr_cards_set_scale(float k);

/* Creates a card's canvas (with its buffer in PSRAM) inside 'parent'. */
lv_obj_t *tr_card_canvas(lv_obj_t *parent, void **buf_out);

/* Draws card 'card' (0..39) or the back if card < 0. */
void tr_card_render(lv_obj_t *canvas, int card);

/* The matchstick scoreboard, into a canvas of any size: two cloth cards,
 * NOS on the left and ELLOS on the right, each with its matches, and the
 * hand's value and who is mano in the middle. 'k' scales the matches (1 on
 * the watch, where the canvas was 368 x 66). It draws both scores, from 0
 * to 30. */
void tr_score_render(lv_obj_t *canvas, float k, int nos, int ellos, int mano_yo, int valor);
