/*
 * SOLITARIO - the art, drawn in code
 *
 * Plain C, no LVGL: it paints into ARGB8888 buffers (0xAARRGGBB in a
 * uint32_t, straight alpha, which is LVGL's ARGB8888 in memory), so the
 * bench lays the whole deck out on a sheet without the board.
 *
 * Everything is drawn at the size it is shown - a card is redrawn for each
 * layout, never scaled - which is what keeps a 66 px Spider card as sharp
 * as a 120 px Klondike one:
 *   - the suits are implicit curves sampled 4 x 4 per pixel into a mask,
 *     once per size, then tinted (the method of Blackjack's art);
 *   - the indices are a stroke font, segments and arcs with round caps,
 *     antialiased by distance;
 *   - the court cards are small vector pictures of our own (so_court.c):
 *     ellipses, polygons and strokes, supersampled, the half figure drawn
 *     twice turned like a real double-headed card;
 *   - backs, felts and the marks of empty piles are functions of the pixel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t *px;
    int       w, h;
} so_img_t;

so_img_t so_img_new(int w, int h);          /* zeroed; px NULL if no memory */
void     so_img_free(so_img_t *img);
void     so_img_clear(so_img_t *img);

/* The faces a player can choose. */
enum {
    SO_DECK_CLASSIC = 0,
    SO_DECK_FOUR,           /* four colours: spades black, hearts red,
                             * diamonds blue, clubs green               */
    SO_DECK_LARGE,          /* huge indices and one big suit            */
    SO_NDECKS
};

#define SO_NBACKS 8
#define SO_NFELTS 8

/* The marks drawn where a pile is empty. */
enum {
    SO_SLOT_PLAIN = 0,
    SO_SLOT_HOME,           /* a foundation: an ace's place             */
    SO_SLOT_STOCK,          /* the stock, which turns over again        */
    SO_SLOT_DONE,           /* the stock, spent                         */
    SO_SLOT_KING,           /* a column only a king may take            */
    SO_NSLOTS
};

typedef struct {
    uint8_t *a;
    int      size, suit;
} so_mask_t;

#define SO_MAX_MASKS 24

typedef struct {
    int cw, ch;             /* the card                                  */
    int pad;                /* room for its shadow on each side          */
    int cv_w, cv_h;         /* the whole image                           */
    int deck;
    int strip;              /* how much of a card shows in a fanned column
                             * for its index to read                    */
    so_mask_t masks[SO_MAX_MASKS];
    int       nmasks;
} so_art_t;

/* the card's height for a width, in the usual 5:7 */
int  so_card_h_for(int cw);
void so_art_setup(so_art_t *a, int cw, int ch, int deck, int flags);
void so_art_free(so_art_t *a);

/* A face (suit 0 spades, 1 hearts, 2 diamonds, 3 clubs; rank 0 ace ..
 * 12 king) into an image of cv_w x cv_h, shadow included. */
void so_card_draw(so_art_t *a, so_img_t *img, int suit, int rank);
void so_back_draw(so_art_t *a, so_img_t *img, int back);
void so_slot_draw(so_art_t *a, so_img_t *img, int kind);
/* A suit alone, centred in a size x size square of img at (x, y). */
void so_suit_draw(so_art_t *a, so_img_t *img, int suit, int size, int x, int y, uint32_t rgb);
uint32_t so_suit_color(const so_art_t *a, int suit);

/* The felt, opaque, w x h. */
void so_felt_draw(so_img_t *img, int felt, int ox, int oy);
/* The same, straight into RGB565 with an ordered dither, row by row. */
void so_felt_draw565(uint16_t *dst, int w, int h, int felt);
/* Its main colour, for the app's own widgets on top of it. */
uint32_t so_felt_tone(int felt);

/* ARGB over RGB565: a card stamped onto the felt (the trails of the win).
 * Clipped to the destination. */
void so_stamp565(uint16_t *dst, int dw, int dh, const so_img_t *src, int x, int y);
/* The same turned by 'deg' and scaled by 'k' about the image's centre,
 * which lands at (cx, cy). */
void so_stamp565_rot(uint16_t *dst, int dw, int dh, const so_img_t *src, float cx, float cy,
                     float deg, float k);

/* --- inside the art --------------------------------------------------------- */

/* straight alpha 'rgb' at coverage a (0..255) over the pixel */
void so_plot(so_img_t *m, int x, int y, uint32_t rgb, int a);
/* is (nx, ny), from -1 to 1 with y down, inside the suit's shape */
bool so_suit_test(int suit, float nx, float ny);
/* the court figure, inside the frame (x, y, w, h) of a card */
void so_court_draw(so_img_t *img, int suit, int rank, int x, int y, int w, int h, bool four);
