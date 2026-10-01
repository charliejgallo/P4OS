/*
 * BLACKJACK - the art, drawn in code
 *
 * Nothing here knows about LVGL or the HAL: it paints into ARGB8888 buffers
 * (0xAARRGGBB in a uint32_t, which is LVGL's ARGB8888 in memory, not
 * premultiplied), so tools/bj_harness.c compiles it with plain cc and lays
 * the whole deck out on a sheet to look at without the board.
 *
 * How each thing is made:
 *   - the four suits are implicit curves (the heart is the classic sextic),
 *     sampled 4x4 per pixel ONCE per size into an alpha mask and then tinted;
 *   - the indices are a stroke font: segments with round caps, antialiased
 *     by distance, so one drawing serves any size;
 *   - the rounded shapes (card, frames, shadow) by signed distance;
 *   - the court figures are pixel art at x2 (bj_court.c), mirrored top to
 *     bottom like a real deck.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* P4OS: everything is drawn at an integer scale of the watch's art, 'bj_k'
 * pixels per watch pixel (2 on the 5" panel: a 152 x 212 card). An integer
 * and not a float so the court figures, which are pixel art, stay pixel
 * art: at x2 each of their source pixels is a clean 4 x 4 block. The masks
 * are made per size, so changing the scale frees them. */
extern int bj_k;
void bj_art_set_scale(int k);

#define BJ_CARD_W   (76 * bj_k)
#define BJ_CARD_H   (106 * bj_k)
#define BJ_CARD_PAD (5 * bj_k)              /* room for the shadow            */
#define BJ_CV_W     (BJ_CARD_W + BJ_CARD_PAD * 2)
#define BJ_CV_H     (BJ_CARD_H + BJ_CARD_PAD * 2)

#define BJ_CHIP_R   (25 * bj_k)             /* the chips you bet with         */
#define BJ_CHIP_CV  (BJ_CHIP_R * 2 + 6 * bj_k)

#define BJ_STACK_W  (60 * bj_k)             /* the pile on the betting circle */
#define BJ_STACK_H  (96 * bj_k)

/* The table's layout, which the app decides (it is not the same upright and
 * lying down). All in canvas pixels. */
typedef struct {
    int   w, h;             /* the whole canvas                                 */
    int   rail_h;           /* the leather rail along the top, 0 = none         */
    int   side_x;           /* a leather panel from here to the right edge, 0 = none */
    float lx, ly, lrx, lry; /* where the cloth is lit, and how far it falls off */
    float ax, ay;           /* centre of the printed arcs, above the dealer     */
    float r1, r2;           /* their radii                                      */
    float arc_y0;           /* the arcs are only printed below this row         */
    float bx, by, br;       /* the betting circle                               */
} bj_felt_t;

typedef struct {
    uint32_t *px;
    int       w, h;
} bj_img_t;

/* card: 0..51 = suit * 13 + rank; rank 0 = ace .. 12 = king;
 * suit 0 spades, 1 hearts, 2 diamonds, 3 clubs. */
#define BJ_SUIT(c)  ((c) / 13)
#define BJ_RANK(c)  ((c) % 13)

bool bj_art_init(void);             /* the suit masks; idempotent            */
void bj_art_free(void);

void bj_img_clear(bj_img_t *img);   /* all transparent                       */

/* A whole card in a BJ_CV_W x BJ_CV_H image, shadow included; card < 0 is
 * the back. */
void bj_card_draw(bj_img_t *img, int card);

/* A chip seen from above, centred in a BJ_CHIP_CV square. */
void bj_chip_draw(bj_img_t *img, int value);

/* The pile of chips for 'amount', seen from the side, in BJ_STACK_W x
 * BJ_STACK_H. Greedy by denomination, tallest pile to the left. */
void bj_stack_draw(bj_img_t *img, int amount);

/* The table, opaque, f->w x f->h. The printed words go on top from the app,
 * with the real fonts: they have to be translated. */
void bj_felt_draw(bj_img_t *img, const bj_felt_t *f);

/* The same table straight into RGB565 with the ordered dither, row by row:
 * no 32-bit copy of a whole screen (3.7 MB) on the way. */
void bj_felt_draw565(uint16_t *dst, const bj_felt_t *f);

/* ARGB8888 -> RGB565 with an ordered dither, so the felt's gradient does not
 * come out in bands. */
void bj_to_rgb565(const bj_img_t *src, uint16_t *dst);

/* The chips there are, and their colours (the app's buttons use them). */
#define BJ_NCHIPS 4
extern const int      bj_chip_value[BJ_NCHIPS];
extern const uint32_t bj_chip_color[BJ_NCHIPS];

/* --- for bj_court.c ---------------------------------------------------- */
void bj_court_draw(bj_img_t *img, int suit, int rank, int x0, int y0);
#define BJ_COURT_W  23                      /* source pixels of HALF a figure */
#define BJ_COURT_PX (2 * bj_k)              /* screen pixels per source pixel  */
#define BJ_COURT_H  20
