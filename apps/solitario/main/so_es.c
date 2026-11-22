/*
 * SOLITARIO - the Spanish deck
 *
 * Truco's cards (apps/truco/main/tr_cards.c), the same drawing number for
 * number through S(), with what a solitaire needs on top: the number at the
 * top left is much larger and the suit sits beside it at the top right, so a
 * column fanned down to its strip still says which card each one is. Apps do
 * not share code, so this is a copy with names of our own.
 *
 * LVGL draws them, onto a canvas lent our ARGB8888 image for the moment: the
 * suits and figures are LVGL primitives (rectangles, arcs, triangles, lines),
 * and the numbers the system's fonts.
 */
#include "so_app.h"
#include "aos_fonts.h"

#include <stdlib.h>
#include <string.h>

/* Truco's order of suits, which its drawing uses */
enum { ES_ESPADA = 0, ES_BASTO, ES_ORO, ES_COPA };

/* --- palette -------------------------------------------------------------- */

#define C_PAPER_T   0xFBF6E8        /* the card stock, at the top */
#define C_PAPER_B   0xE7DAB9        /* and at the bottom: old cards yellow */
#define C_EDGE      0x8C7C5C
#define C_INK       0x2B2620
#define C_GOLD      0xD9A62B
#define C_GOLD_D    0x8E6510
#define C_GOLD_L    0xF3D874
#define C_STEEL     0xC8CED6
#define C_STEEL_D   0x7E8790
#define C_WOOD      0x8A5A2B
#define C_WOOD_D    0x54341A
#define C_RED       0xA6291F
#define C_BLUE      0x2E4E8C
#define C_SKIN      0xEBC7A1
#define C_HAIR      0x46311F
#define C_BACK      0x7C1D18
#define C_BACK_L    0x9C2E26
#define C_BACK_D    0x5A100D

/* --- scale ---------------------------------------------------------------- */

static float s_k = 1.f;

/* Watch pixels to panel pixels, rounded, never below one where it was one. */
static int S(float v)
{
    float r = v * s_k;
    int   i = (int)(r >= 0 ? r + 0.5f : r - 0.5f);
    return (v > 0 && i < 1) ? 1 : i;
}



/* The largest typeface whose line fits in 'h' pixels: the scoreboard's
 * captions sit in a strip above the matches and must not reach them. */
static const lv_font_t *font_fit(int h)
{
    static const lv_font_t *const F[] = {
        &aos_inter_48, &aos_inter_36, &aos_inter_32, &aos_inter_28,
        &aos_inter_24, &aos_inter_20, &aos_inter_16,
    };
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {
        if (lv_font_get_line_height(F[i]) <= h) return F[i];
    }
    return &aos_inter_14;
}

/* --- primitives ----------------------------------------------------------- */

static void rect(lv_layer_t *l, int x0, int y0, int x1, int y1,
                 uint32_t color, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa   = LV_OPA_COVER;
    d.radius   = radius;
    lv_area_t a = { x0, y0, x1, y1 };
    lv_draw_rect(l, &d, &a);
}


static void disc(lv_layer_t *l, int cx, int cy, int r, uint32_t color)
{
    rect(l, cx - r, cy - r, cx + r, cy + r, color, LV_RADIUS_CIRCLE);
}

static void ring(lv_layer_t *l, int cx, int cy, int r, int w, uint32_t color)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color       = lv_color_hex(color);
    d.width       = w;
    d.radius      = (uint16_t)r;
    d.center.x    = cx;
    d.center.y    = cy;
    d.start_angle = 0;
    d.end_angle   = 360;
    d.opa         = LV_OPA_COVER;
    lv_draw_arc(l, &d);
}

static void line(lv_layer_t *l, int x0, int y0, int x1, int y1, int w,
                 uint32_t color, bool round)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color       = lv_color_hex(color);
    d.width       = w;
    d.opa         = LV_OPA_COVER;
    d.round_start = round;
    d.round_end   = round;
    d.p1.x = x0; d.p1.y = y0;
    d.p2.x = x1; d.p2.y = y1;
    lv_draw_line(l, &d);
}

static void tri(lv_layer_t *l, int x0, int y0, int x1, int y1, int x2, int y2,
                uint32_t color)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.opa   = LV_OPA_COVER;
    d.p[0].x = x0; d.p[0].y = y0;
    d.p[1].x = x1; d.p[1].y = y1;
    d.p[2].x = x2; d.p[2].y = y2;
    lv_draw_triangle(l, &d);
}

static void text(lv_layer_t *l, int x0, int y0, int x1, int y1, const char *s,
                 const lv_font_t *font, uint32_t color, lv_text_align_t align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text  = s;                /* NOTE: the pointer is stored, not the string */
    d.font  = font;
    d.color = lv_color_hex(color);
    d.align = align;
    d.opa   = LV_OPA_COVER;
    lv_area_t a = { x0, y0, x1, y1 };
    lv_draw_label(l, &d, &a);
}

/* --- the four suits -------------------------------------------------------
 * Each one is drawn centred on (cx, cy) and 's' tall. The measurements go in
 * fractions of 's' so the same code serves both for the large symbol of an ace
 * and for the little ones on a seven.
 * ------------------------------------------------------------------------- */

static void suit_oro(lv_layer_t *l, int cx, int cy, int s)
{
    int r = s / 2;
    disc(l, cx, cy, r, C_GOLD_D);
    disc(l, cx, cy, r - S(1), C_GOLD);
    ring(l, cx, cy, r - S(2), S(1), C_GOLD_L);

    if (r < S(8)) {
        disc(l, cx, cy, LV_MAX(1, r / 3), C_GOLD_D);
        return;
    }

    /* A minted coin: two rings, four cardinal marks between them and the dot
     * in the centre. No stars and no triangles: at 20 px they smudge together
     * and leave a blob. */
    ring(l, cx, cy, r - S(4), S(1), C_GOLD_D);
    disc(l, cx, cy, r - S(6), C_GOLD_L);
    int a = r - S(5), b = r - S(3), t = S(1);
    rect(l, cx - t, cy - b, cx + t, cy - a, C_GOLD_D, 0);
    rect(l, cx - t, cy + a, cx + t, cy + b, C_GOLD_D, 0);
    rect(l, cx - b, cy - t, cx - a, cy + t, C_GOLD_D, 0);
    rect(l, cx + a, cy - t, cx + b, cy + t, C_GOLD_D, 0);
    disc(l, cx, cy, LV_MAX(1, r / 5), C_GOLD_D);
}

static void suit_copa(lv_layer_t *l, int cx, int cy, int s)
{
    int w  = (s * 7) / 10;              /* width of the mouth */
    int y0 = cy - s / 2;
    int bowl = (s * 9) / 20;

    /* the cup: a straight mouth at the top and a rounded belly below */
    rect(l, cx - w / 2, y0, cx + w / 2, y0 + bowl / 2, C_GOLD, S(1));
    rect(l, cx - w / 2, y0 + bowl / 4, cx + w / 2, y0 + bowl, C_GOLD, w / 2);
    rect(l, cx - w / 2, y0, cx + w / 2, y0 + LV_MAX(1, s / 12), C_GOLD_L, 0);

    /* foot */
    int st = LV_MAX(2, s / 8);
    rect(l, cx - st / 2, y0 + bowl, cx + st / 2, cy + s / 3, C_GOLD_D, 0);
    if (s >= 16) disc(l, cx, y0 + bowl + s / 8, LV_MAX(2, s / 10), C_GOLD);
    rect(l, cx - w / 2, cy + s / 3, cx + w / 2, cy + s / 2, C_GOLD, LV_MAX(1, s / 12));
    rect(l, cx - w / 2, cy + s / 3, cx + w / 2, cy + s / 3 + S(1), C_GOLD_D, 0);
}

static void suit_espada(lv_layer_t *l, int cx, int cy, int s)
{
    int y0 = cy - s / 2;                        /* point */
    int gy = cy + s / 5;                        /* the crossguard */
    int bw = LV_MAX(5, (s * 22) / 100);         /* width of the blade */
    int ty = y0 + LV_MAX(3, s / 5);             /* where the point ends */

    /* Blade: dark edge first and steel on top, which is the only thing that
     * makes it read as metal when it is 17 px. */
    int e = S(1);                               /* the dark edge's width */
    tri(l, cx, y0, cx - bw / 2 - e, ty, cx + bw / 2 + e, ty, C_STEEL_D);
    tri(l, cx, y0 + 2 * e, cx - bw / 2, ty, cx + bw / 2, ty, C_STEEL);
    rect(l, cx - bw / 2 - e, ty - e, cx + bw / 2 + e, gy, C_STEEL_D, 0);
    rect(l, cx - bw / 2, ty - e, cx + bw / 2, gy, C_STEEL, 0);
    rect(l, cx - e, ty, cx + e - 1, gy - e, 0xEDF1F5, 0);   /* the edge */

    int gw = LV_MAX(9, (s * 62) / 100);
    int gh = LV_MAX(3, s / 9);
    rect(l, cx - gw / 2, gy, cx + gw / 2, gy + gh, C_GOLD_D, e);
    rect(l, cx - gw / 2, gy, cx + gw / 2, gy + gh - e, C_GOLD, e);

    int hw = LV_MAX(3, s / 8);
    rect(l, cx - hw / 2, gy + gh, cx + hw / 2, cy + s / 2 - s / 8, C_WOOD_D, e);
    disc(l, cx, cy + s / 2 - s / 10, LV_MAX(2, (s * 11) / 100), C_GOLD_D);
    disc(l, cx, cy + s / 2 - s / 10, LV_MAX(1, (s * 11) / 100 - e), C_GOLD);
}

static void suit_basto(lv_layer_t *l, int cx, int cy, int s)
{
    int y0 = cy - s / 2, y1 = cy + s / 2;
    int w  = LV_MAX(5, (s * 32) / 100);         /* thick at the bottom */

    /* The cut branches go FIRST, so the trunk covers them on the inside and
     * they come out of the club instead of crossing it. */
    int br = LV_MAX(2, w / 3);
    int bl = (w * 3) / 4;
    int e  = S(1);                              /* the dark rim */
    line(l, cx - e, cy + s / 6, cx - bl, cy + s / 6 - s / 12, br, C_WOOD_D, true);
    line(l, cx + e, cy - s / 8, cx + bl, cy - s / 8 - s / 10, br, C_WOOD_D, true);

    /* The club tapers upwards: three sections with rounded ends come out
     * cheaper than a polygon and read just the same. */
    line(l, cx, y1, cx, cy + s / 8, w, C_WOOD_D, true);
    line(l, cx, y1 - e, cx, cy + s / 8, w - 2 * e, C_WOOD, true);
    int w2 = LV_MAX(4, (w * 8) / 10);
    line(l, cx, cy + s / 6, cx, cy - s / 8, w2, C_WOOD_D, true);
    line(l, cx, cy + s / 6 - e, cx, cy - s / 8, w2 - 2 * e, C_WOOD, true);
    int w3 = LV_MAX(3, (w * 6) / 10);
    line(l, cx, cy - s / 12, cx, y0, w3, C_WOOD_D, true);
    line(l, cx, cy - s / 12, cx, y0 + e, w3 - 2 * e > 0 ? w3 - 2 * e : 1, C_WOOD, true);

    /* the knots, alternating: it is what makes it read as a trunk */
    if (s >= 14) {
        int k = LV_MAX(1, s / 12);
        disc(l, cx - w / 3, cy + s / 4, k, C_WOOD_D);
        disc(l, cx + w / 4, cy + s / 30, k, C_WOOD_D);
        disc(l, cx - w / 4, cy - s / 4, k, C_WOOD_D);
    }
}

static void suit(lv_layer_t *l, int s_id, int cx, int cy, int size)
{
    switch (s_id) {
    case ES_ORO:    suit_oro(l, cx, cy, size);    break;
    case ES_COPA:   suit_copa(l, cx, cy, size);   break;
    case ES_ESPADA: suit_espada(l, cx, cy, size); break;
    default:        suit_basto(l, cx, cy, size);  break;
    }
}

/* --- the face cards -------------------------------------------------------
 * Knave, knight and king in a box of ~44x64. At that size no portrait is worth
 * it: what is wanted is for each one to be told from the others at a glance,
 * and that is given by the crown, the horse in profile and the standing
 * figure.
 * ------------------------------------------------------------------------- */

static void figure_rey(lv_layer_t *l, int x0, int y0, int x1, int y1, int s_id)
{
    int cx = (x0 + x1) / 2;
    int w  = x1 - x0;
    int hy = y0 + (y1 - y0) / 3;            /* chin */

    /* cloak */
    tri(l, cx, hy, x0 + S(1), y1, x1 - S(1), y1, C_RED);
    rect(l, cx - w / 6, hy, cx + w / 6, y1, C_RED, 0);
    rect(l, cx - w / 8, hy + S(4), cx + w / 8, y1, C_GOLD, 0);

    /* face and beard */
    disc(l, cx, hy - S(6), S(8), C_SKIN);
    rect(l, cx - S(7), hy - S(4), cx + S(7), hy + S(5), 0xDCD6C6, S(4));      /* the beard */
    disc(l, cx, hy - S(7), S(7), C_SKIN);
    disc(l, cx - S(3), hy - S(8), S(1), C_INK);
    disc(l, cx + S(3), hy - S(8), S(1), C_INK);

    /* crown */
    int cy0 = hy - S(14);
    rect(l, cx - S(9), cy0, cx + S(9), cy0 + S(4), C_GOLD, S(1));
    tri(l, cx - S(9), cy0, cx - S(4), cy0 - S(7), cx + S(1), cy0, C_GOLD);
    tri(l, cx - S(1), cy0, cx + S(4), cy0 - S(7), cx + S(9), cy0, C_GOLD);
    disc(l, cx - S(4), cy0 - S(7), S(1), C_RED);
    disc(l, cx + S(4), cy0 - S(7), S(1), C_RED);

    suit(l, s_id, x0 + S(7), y1 - S(9), S(14));
}

static void figure_caballo(lv_layer_t *l, int x0, int y0, int x1, int y1, int s_id)
{
    int cx = (x0 + x1) / 2;
    int by = y0 + ((y1 - y0) * 2) / 3;       /* line of the back */

    /* body and legs */
    rect(l, x0 + S(4), by - S(8), x1 - S(6), by + S(4), C_WOOD, S(5));
    rect(l, x0 + S(7), by + S(2), x0 + S(10), y1, C_WOOD_D, S(1));
    rect(l, x1 - S(12), by + S(2), x1 - S(9), y1, C_WOOD_D, S(1));

    /* neck and head in profile, facing left */
    tri(l, cx - S(2), by - S(6), cx + S(8), by - S(6), cx - S(4), y0 + S(10), C_WOOD);
    rect(l, x0 + S(4), y0 + S(6), cx + S(1), y0 + S(15), C_WOOD, S(4));
    rect(l, x0 + S(3), y0 + S(10), x0 + S(10), y0 + S(15), C_WOOD_D, S(2));
    disc(l, cx - S(4), y0 + S(10), S(1), C_INK);
    tri(l, cx - S(3), y0 + S(6), cx, y0 + S(1), cx + S(2), y0 + S(7), C_WOOD_D);

    /* mane and tail */
    for (int i = 0; i < 3; i++) {
        line(l, cx + S(-1 + i * 3), y0 + S(6 + i), cx + S(3 + i * 3), y0 + S(13 + i * 2), S(2),
             C_HAIR, true);
    }
    line(l, x1 - S(6), by - S(6), x1 - S(3), by + S(6), S(3), C_HAIR, true);

    suit(l, s_id, x0 + S(7), y1 - S(9), S(14));
}

static void figure_sota(lv_layer_t *l, int x0, int y0, int x1, int y1, int s_id)
{
    int cx = (x0 + x1) / 2;
    int w  = x1 - x0;
    int hy = y0 + (y1 - y0) / 3;

    /* tunic */
    tri(l, cx, hy + S(2), x0 + S(3), y1, x1 - S(3), y1, C_BLUE);
    rect(l, cx - w / 7, hy, cx + w / 7, y1, C_BLUE, 0);
    line(l, cx - w / 5, hy + S(6), cx - w / 3 + S(2), hy + S(16), S(3), C_BLUE, true);

    /* head with hair */
    disc(l, cx, hy - S(7), S(8), C_HAIR);
    disc(l, cx, hy - S(8), S(6), C_SKIN);
    rect(l, cx - S(7), hy - S(15), cx + S(7), hy - S(10), C_HAIR, S(3));
    disc(l, cx - S(2), hy - S(9), S(1), C_INK);
    disc(l, cx + S(2), hy - S(9), S(1), C_INK);

    /* cap */
    rect(l, cx - S(8), hy - S(17), cx + S(8), hy - S(13), C_RED, S(2));
    tri(l, cx + S(4), hy - S(17), cx + S(11), hy - S(22), cx + S(8), hy - S(13), C_RED);

    suit(l, s_id, x0 + S(7), y1 - S(9), S(14));
}

/* --- the whole card ------------------------------------------------------- */

static void draw_frame(lv_layer_t *l, int x0, int y0, int x1, int y1, int cuts)
{
    const int t = S(2);

    /* whole horizontals */
    rect(l, x0, y0, x1, y0 + t - 1, C_INK, 0);
    rect(l, x0, y1 - t + 1, x1, y1, C_INK, 0);

    /* verticals, cut in the middle according to the suit */
    int h = y1 - y0;
    if (cuts == 0) {
        rect(l, x0, y0, x0 + t - 1, y1, C_INK, 0);
        rect(l, x1 - t + 1, y0, x1, y1, C_INK, 0);
        return;
    }

    const int gap = S(5), sep = S(4);
    int span = cuts * gap + (cuts - 1) * sep;
    int gy0  = y0 + h / 2 - span / 2;

    for (int side = 0; side < 2; side++) {
        int sx0 = side ? x1 - t + 1 : x0;
        int sx1 = side ? x1 : x0 + t - 1;
        int y   = y0;
        for (int i = 0; i < cuts; i++) {
            int cut0 = gy0 + i * (gap + sep);
            if (cut0 > y) rect(l, sx0, y, sx1, cut0 - 1, C_INK, 0);
            y = cut0 + gap;
        }
        if (y <= y1) rect(l, sx0, y, sx1, y1, C_INK, 0);
    }
}

static void draw_pips(lv_layer_t *l, int s_id, int n, int x0, int y0, int x1, int y1)
{
    if (n == 1) {
        /* The ace goes large: it is half a card. Coins is the only one taking
         * up as much across as down, so that one has to be limited by the
         * width or it runs off the frame. */
        int big = s_id == ES_ORO ? LV_MIN(x1 - x0, ((y1 - y0) * 4) / 5)
                                 : ((y1 - y0) * 17) / 20;
        suit(l, s_id, (x0 + x1) / 2, (y0 + y1) / 2, big);
        return;
    }

    /* The sizes come from measuring the overlap: with three rows and a symbol
     * of 25 the points collide. The middle one on the 5 and the 7 goes smaller
     * still, which is also what real decks do. */
    int size = S(n == 2 ? 26 : n == 3 ? 21 : n <= 5 ? 20 : 17);
    int mid  = (size * 3) / 4;
    int lx = x0 + (x1 - x0) / 4 + S(1);
    int rx = x1 - (x1 - x0) / 4 - S(1);
    int cx = (x0 + x1) / 2;
    int h  = y1 - y0;
    int r2a = y0 + h / 4,     r2b = y1 - h / 4;
    int r3a = y0 + h / 6, r3b = y0 + h / 2, r3c = y1 - h / 6;

    switch (n) {
    case 2:
        suit(l, s_id, cx, r2a, size); suit(l, s_id, cx, r2b, size);
        break;
    case 3:
        suit(l, s_id, cx, r3a, size); suit(l, s_id, cx, r3b, size);
        suit(l, s_id, cx, r3c, size);
        break;
    case 4:
        suit(l, s_id, lx, r2a, size); suit(l, s_id, rx, r2a, size);
        suit(l, s_id, lx, r2b, size); suit(l, s_id, rx, r2b, size);
        break;
    case 5:
        suit(l, s_id, lx, r2a, size); suit(l, s_id, rx, r2a, size);
        suit(l, s_id, lx, r2b, size); suit(l, s_id, rx, r2b, size);
        suit(l, s_id, cx, (y0 + y1) / 2, mid);
        break;
    case 6:
        suit(l, s_id, lx, r3a, size); suit(l, s_id, rx, r3a, size);
        suit(l, s_id, lx, r3b, size); suit(l, s_id, rx, r3b, size);
        suit(l, s_id, lx, r3c, size); suit(l, s_id, rx, r3c, size);
        break;
    default:
        suit(l, s_id, lx, r3a, size); suit(l, s_id, rx, r3a, size);
        suit(l, s_id, lx, r3b, size); suit(l, s_id, rx, r3b, size);
        suit(l, s_id, lx, r3c, size); suit(l, s_id, rx, r3c, size);
        suit(l, s_id, cx, (y0 + y1) / 2, mid);
        break;
    }
}



/* --- the whole card ------------------------------------------------------- */

static const char *const NUMTXT[10] = { "1", "2", "3", "4", "5", "6", "7", "10", "11", "12" };

/* The "pinta": the card's frame carries as many cuts as the suit says, so you
 * can tell which suit it is by looking only at the edge of a closed fan. Coins
 * none, cups one, swords two and clubs three. */
static const uint8_t PINTA_CUTS[4] = { 2, 3, 0, 1 };    /* swords, clubs, coins, cups */

/* our suits (coins, cups, swords, clubs) to Truco's */
static const uint8_t ES_OF[4] = { ES_ORO, ES_COPA, ES_ESPADA, ES_BASTO };

int so_es_height(int cw)
{
    return (cw * 96 + 31) / 62;
}

bool so_es_render(lv_obj_t *parent, so_img_t *img, int cw, int ch, int pad, int our_suit, int ri)
{
    (void)ch;
    s_k = (float)cw / 62.f;
    lv_obj_t *cv = lv_canvas_create(parent);
    if (!cv) return false;
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_buffer(cv, img->px, img->w, img->h, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(cv, lv_color_hex(0), LV_OPA_TRANSP);

    lv_layer_t l;
    lv_canvas_init_layer(cv, &l);

    const int x0 = pad, y0 = pad;
    const int x1 = pad + cw - 1, y1 = pad + so_es_height(cw) - 1;

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius        = S(5);
    d.bg_grad.dir   = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(C_PAPER_T);
    d.bg_grad.stops[0].frac  = 0;
    d.bg_grad.stops[0].opa   = LV_OPA_COVER;
    d.bg_grad.stops[1].color = lv_color_hex(C_PAPER_B);
    d.bg_grad.stops[1].frac  = 255;
    d.bg_grad.stops[1].opa   = LV_OPA_COVER;
    d.bg_opa        = LV_OPA_COVER;
    d.border_color  = lv_color_hex(C_EDGE);
    d.border_width  = S(1);
    d.border_opa    = LV_OPA_COVER;
    d.shadow_color  = lv_color_hex(0x000000);
    d.shadow_width  = pad;
    d.shadow_offset_x = S(1);
    d.shadow_offset_y = S(2);
    d.shadow_opa    = 110;
    lv_area_t body = { x0, y0, x1, y1 };
    lv_draw_rect(&l, &d, &body);

    int s_id = ES_OF[our_suit & 3];
    int rank = ri < 7 ? ri + 1 : ri + 3;            /* 1..7, 10, 11, 12 */

    draw_frame(&l, x0 + S(4), y0 + S(4), x1 - S(4), y1 - S(4), PINTA_CUTS[s_id]);

    int ax0 = x0 + S(8), ay0 = y0 + S(27), ax1 = x1 - S(8), ay1 = y1 - S(7);
    if (rank >= 10) {
        if (rank == 12)      figure_rey(&l, ax0, ay0, ax1, ay1, s_id);
        else if (rank == 11) figure_caballo(&l, ax0, ay0, ax1, ay1, s_id);
        else                 figure_sota(&l, ax0, ay0, ax1, ay1, s_id);
    } else {
        draw_pips(&l, s_id, rank, ax0, ay0, ax1, ay1);
    }

    /* the strip: a big number on a plate of card stock, the suit beside it */
    rect(&l, x0 + S(5), y0 + S(5), x1 - S(5), y0 + S(25), C_PAPER_T, S(3));
    const lv_font_t *nf = font_fit(S(22));
    text(&l, x0 + S(7), y0 + S(4), x0 + S(44), y0 + S(4) + lv_font_get_line_height(nf),
         NUMTXT[ri], nf, (s_id == ES_ORO || s_id == ES_COPA) ? C_RED : C_INK, LV_TEXT_ALIGN_LEFT);
    suit(&l, s_id, x1 - S(13), y0 + S(15), S(17));

    lv_canvas_finish_layer(cv, &l);
    lv_obj_delete(cv);
    return true;
}
