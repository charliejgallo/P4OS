/*
 * Claudito - the two scenes (see cl_scene.h)
 *
 * The ornaments stay where the watch had them, on its 92x78 frame (the
 * buffer's origin places it). What fills the view whatever its size -wall,
 * wallpaper, parquet, sky, grass- is drawn from the view's visible edges
 * (cl_left() ... cl_bottom()), so the P4's taller stage gets more wall and
 * more floor instead of black bands.
 *
 * At x8 every art pixel is a visible block, so the second round of the art
 * works in steps and not in flat fills: each surface gets a lit side and a
 * shaded side, bands of colour meet through a row of checkerboard dither,
 * and the light comes from the top left (the living room's window). All of
 * it is drawn once into the cached background, so none of it costs a frame.
 */
#include "cl_scene.h"
#include "aos_i18n.h"
#include "cl_sprites.h"

/* Reproducible noise: the tufts of grass and the wood grain have to fall in
 * the same place every time, or the background shivers when redrawn. */
static uint32_t s_rng;

static void noise_seed(uint32_t seed)
{
    s_rng = seed | 1u;
}

static uint32_t noise(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int noise_range(int lo, int hi)
{
    return lo + (int)(noise() % (uint32_t)(hi - lo + 1));
}

/* The first value of start + k*step (any integer k) that is >= lo: the
 * wallpaper and the joints keep the watch's rhythm on the frame and carry it
 * on past its edges. */
static int first_from(int start, int step, int lo)
{
    int v = start;
    while (v - step >= lo) v -= step;
    while (v < lo)         v += step;
    return v;
}

/* A disc of checkerboard: a halo that lets what is behind show through */
static void dither_disc(cl_buf_t *b, int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r + r / 2 && ((cx + dx + cy + dy) & 1) == 0) {
                cl_px(b, cx + dx, cy + dy, c);
            }
        }
    }
}

/* Warm light falling off from a point: four rings, each pixel pulled
 * towards the lamp's colour, strongest in the middle and nothing at radius
 * r. The ellipse is squashed vertically by 'sy' quarters. */
static void glow(cl_buf_t *b, int cx, int cy, int r, int sy, int f)
{
    const uint16_t warm = cl_rgb(0xFFC872);
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int ey = dy * sy / 4;
            int d2 = dx * dx + ey * ey;
            if (d2 >= r * r) {
                continue;
            }
            int step = 4 - d2 * 4 / (r * r);    /* 4 in the middle, 1 at the rim */
            int k = f * step / 4;
            if (k > 0) {
                cl_mix(b, cx + dx, cy + dy, warm, k);
            }
        }
    }
}

static void mark(cl_rect_t *out, int *n, int max, int x, int y, int w, int h)
{
    if (*n < max) {
        out[*n].x = (int16_t)x;
        out[*n].y = (int16_t)y;
        out[*n].w = (int16_t)w;
        out[*n].h = (int16_t)h;
        (*n)++;
    }
}

/* --------------------------------------------------------------------------
 * Living room
 * -------------------------------------------------------------------------- */

#define WIN_X   8
#define WIN_Y   8
#define WIN_W   24
#define WIN_H   22

/* What is seen through the glass: sky and a green hill by day, a night sky
 * with the moon by night. It is drawn undimmed at night: the room is dark,
 * the sky outside is not darker for it. */
static void window_glass(cl_buf_t *b, int x, int y, int w, int h, bool night)
{
    if (night) {
        cl_rect(b, x, y, w, h, cl_rgb(0x1B2A4A));
        cl_dither(b, x, y + h / 2, w, 1, cl_rgb(0x24375C));
        cl_rect(b, x, y + h / 2 + 1, w, h - h / 2 - 1, cl_rgb(0x24375C));
        cl_blit(b, x + 12, y + 2, CL_SPRITE(cl_spr_moon), false);
        static const int8_t star[][2] = { { 4, 4 }, { 8, 13 }, { 3, 10 }, { 20, 16 }, { 9, 3 } };
        for (int i = 0; i < 5; i++) {
            cl_px(b, x + star[i][0], y + star[i][1], cl_rgb(0xFFF6D0));
        }
    } else {
        cl_rect(b, x, y, w, h, cl_rgb(0x8FD4F7));
        cl_rect(b, x, y, w, h / 3, cl_rgb(0x7CC6F2));
        cl_dither(b, x, y + h / 3, w, 1, cl_rgb(0x7CC6F2));
        cl_disc(b, x + w - 5, y + 4, 3, cl_rgb(0xFFE066));
        cl_px(b, x + w - 6, y + 3, cl_rgb(0xFFF6C0));
        /* a fixed little cloud */
        cl_rect(b, x + 3, y + 7, 6, 2, cl_rgb(0xFFFFFF));
        cl_rect(b, x + 4, y + 6, 3, 1, cl_rgb(0xFFFFFF));
        cl_hline(b, x + 3, y + 9, 6, cl_rgb(0xDCEEF8));
    }

    /* the yard's hill at the bottom of the window */
    for (int i = 0; i < w; i++) {
        int d = i - w / 3;
        int hh = 5 - d * d / 40;
        if (hh < 1) hh = 1;
        uint32_t c = night ? 0x16304A : 0x7FC77F;
        cl_vline(b, x + i, y + h - hh, hh, cl_rgb(c));
        cl_px(b, x + i, y + h - hh, cl_rgb(night ? 0x1E3C58 : 0x9BD89A));
    }

    /* a reflection in the corner of the top left pane */
    uint16_t glint = cl_rgb(night ? 0x3A4C74 : 0xBFE9FF);
    cl_px(b, x + 1, y + 3, glint);
    cl_px(b, x + 2, y + 2, glint);
    cl_px(b, x + 3, y + 1, glint);
}

static void window_bars(cl_buf_t *b, int x, int y, int w, int h)
{
    cl_vline(b, x + w / 2, y, h, cl_rgb(0xF4EDE0));
    cl_hline(b, x, y + h / 2, w, cl_rgb(0xF4EDE0));
}

/* A curtain in pleats: light, mid, dark, over and over, with a scalloped hem
 * and a gold tie-back. */
static void draw_curtain(cl_buf_t *b, int x, int y, int w, int h)
{
    static const uint32_t pleat[3] = { 0xE07A80, 0xC8505A, 0x96343E };
    for (int i = 0; i < w; i++) {
        cl_vline(b, x + i, y, h, cl_rgb(pleat[i % 3]));
    }
    for (int i = 0; i < w; i += 3) {
        cl_px(b, x + i, y + h, cl_rgb(pleat[0]));
        cl_px(b, x + i + 1, y + h, cl_rgb(pleat[1]));
    }
    int ty = y + h * 2 / 3;
    cl_hline(b, x, ty, w, cl_rgb(0xFFD166));
    cl_hline(b, x, ty + 1, w, cl_rgb(0xC9A400));
}

static void draw_window(cl_buf_t *b, bool night)
{
    const int x = WIN_X, y = WIN_Y, w = WIN_W, h = WIN_H;

    int dim = b->dim;
    b->dim = 0;
    window_glass(b, x, y, w, h, night);
    b->dim = (int8_t)dim;

    /* frame: lit on the top and left, shaded on the bottom and right */
    cl_frame(b, x - 2, y - 2, w + 4, h + 4, cl_rgb(0xF4EDE0));
    cl_frame(b, x - 1, y - 1, w + 2, h + 2, cl_rgb(0xF4EDE0));
    cl_frame(b, x - 3, y - 3, w + 6, h + 6, cl_rgb(0xC9A87E));
    cl_hline(b, x - 1, y + h, w + 2, cl_rgb(0xDCD0BC));
    cl_vline(b, x + w, y - 1, h + 2, cl_rgb(0xDCD0BC));
    window_bars(b, x, y, w, h);

    /* sill, with its shadow on the wall */
    cl_rect(b, x - 5, y + h + 3, w + 10, 2, cl_rgb(0xE0C49B));
    cl_hline(b, x - 5, y + h + 3, w + 10, cl_rgb(0xF0DCB8));
    cl_hline(b, x - 5, y + h + 5, w + 10, cl_rgb(0xB98A5C));
    cl_shade(b, x - 4, y + h + 6, w + 8, 1, -2);

    /* the curtain rod and the curtains, in front of the frame's edges */
    cl_hline(b, x - 7, y - 5, w + 14, cl_rgb(0x6E4423));
    cl_px(b, x - 8, y - 5, cl_rgb(0xFFD166));
    cl_px(b, x + w + 7, y - 5, cl_rgb(0xFFD166));
    draw_curtain(b, x - 7, y - 4, 6, h + 10);
    draw_curtain(b, x + w + 1, y - 4, 6, h + 10);
}

/* A round clock between the window and the picture: a dark rim, a cream
 * face, the four quarter marks and two hands, at three o'clock. */
static void draw_clock(cl_buf_t *b, int cx, int cy)
{
    const uint16_t hand = cl_rgb(0x3A2A22);
    const uint16_t mark = cl_rgb(0xB9A27E);

    cl_shade(b, cx - 3, cy + 6, 8, 1, -2);          /* its shadow on the wall */
    cl_shade(b, cx + 6, cy - 3, 1, 8, -2);
    cl_disc(b, cx, cy, 5, cl_rgb(0x6E4423));
    cl_disc(b, cx, cy, 4, cl_rgb(0xFBF3E2));
    cl_px(b, cx - 2, cy - 3, cl_rgb(0xFFFFFF));     /* the glass's shine */
    cl_px(b, cx, cy - 3, mark);
    cl_px(b, cx + 3, cy, mark);
    cl_px(b, cx, cy + 3, mark);
    cl_px(b, cx - 3, cy, mark);
    cl_vline(b, cx, cy - 2, 3, hand);               /* the minute hand, on 12 */
    cl_hline(b, cx, cy, 3, hand);                   /* the hour hand, on 3    */
}

static void draw_picture(cl_buf_t *b, int x, int y)
{
    /* the string from the nail to the frame's corners */
    for (int i = 0; i < 7; i++) {
        cl_px(b, x + 3 + i, y - 1 - i * 4 / 7, cl_rgb(0x8A6A4A));
        cl_px(b, x + 16 - i, y - 1 - i * 4 / 7, cl_rgb(0x8A6A4A));
    }
    cl_px(b, x + 10, y - 5, cl_rgb(0x4A4A4E));
    cl_shade(b, x + 1, y + 16, 20, 1, -2);          /* shadow on the wall */
    cl_shade(b, x + 20, y + 1, 1, 16, -2);

    cl_rect(b, x, y, 20, 16, cl_rgb(0xD9A441));
    cl_hline(b, x, y, 20, cl_rgb(0xF0C866));
    cl_vline(b, x, y, 16, cl_rgb(0xF0C866));
    cl_hline(b, x, y + 15, 20, cl_rgb(0xA87A20));
    cl_vline(b, x + 19, y, 16, cl_rgb(0xA87A20));
    cl_rect(b, x + 2, y + 2, 16, 12, cl_rgb(0xFBF3E2));
    /* little hills and sun: a picture within the picture */
    cl_rect(b, x + 2, y + 2, 16, 4, cl_rgb(0xDDEFF7));
    cl_rect(b, x + 2, y + 9, 16, 5, cl_rgb(0x9BD3A0));
    for (int i = 0; i < 5; i++) {
        cl_hline(b, x + 5 - i, y + 9 - i, 1 + i * 2, cl_rgb(0x7FBF8C));
        cl_hline(b, x + 12 - i, y + 10 - i, 1 + i * 2, cl_rgb(0x6FAE7C));
    }
    cl_px(b, x + 5, y + 5, cl_rgb(0xFFFFFF));       /* snow on the peaks */
    cl_px(b, x + 12, y + 6, cl_rgb(0xFFFFFF));
    cl_disc(b, x + 14, y + 5, 2, cl_rgb(0xFFD166));
    cl_hline(b, x + 2, y + 13, 16, cl_rgb(0x5E9A6A));
}

/* A flattened oval rug done by hand: rows of decreasing widths, a cream
 * border, a band of diamonds and a fringe at both ends. */
static void draw_rug(cl_buf_t *b, int cx, int cy)
{
    static const int8_t half[] = { 22, 26, 28, 29, 28, 26, 22, 16 };
    const int rows = (int)(sizeof(half) / sizeof(half[0]));

    cl_shade(b, cx - 26, cy + rows, 52, 1, -3);     /* its edge on the floor */
    for (int i = 0; i < rows; i++) {
        int hw = half[i];
        cl_hline(b, cx - hw, cy + i, hw * 2, cl_rgb(0x3E8F92));
        /* the border: cream at the ends of each row */
        cl_rect(b, cx - hw, cy + i, 2, 1, cl_rgb(0xEFE0C4));
        cl_rect(b, cx + hw - 2, cy + i, 2, 1, cl_rgb(0xEFE0C4));
    }
    cl_hline(b, cx - 22, cy, 44, cl_rgb(0xEFE0C4));
    cl_hline(b, cx - 16, cy + 7, 32, cl_rgb(0xEFE0C4));
    /* the lighter band in the middle, and its diamonds */
    for (int i = 2; i <= 5; i++) {
        int hw = half[i] - 4;
        cl_hline(b, cx - hw, cy + i, hw * 2, cl_rgb(0x4EA8A0));
    }
    for (int k = -3; k <= 3; k++) {
        int x = cx + k * 7;
        uint16_t c = cl_rgb((k & 1) ? 0xE07A5F : 0xEFE0C4);
        cl_px(b, x, cy + 2, c);
        cl_px(b, x - 1, cy + 3, c);
        cl_px(b, x + 1, cy + 3, c);
        cl_px(b, x - 1, cy + 4, c);
        cl_px(b, x + 1, cy + 4, c);
        cl_px(b, x, cy + 5, c);
    }
    /* fringe */
    for (int i = 2; i <= 5; i++) {
        cl_px(b, cx - half[i] - 1, cy + i, cl_rgb(0xEFE0C4));
        cl_px(b, cx + half[i], cy + i, cl_rgb(0xEFE0C4));
    }
    cl_shade(b, cx - 28, cy + 6, 56, 2, -1);        /* the near side, in shadow */
}

static void scene_home(cl_buf_t *b, bool night)
{
    const uint16_t wall  = cl_rgb(0xF0D8B8);
    const uint16_t strip = cl_rgb(0xE3C69C);
    const int L = cl_left(b), R = cl_right(b), T = cl_top(b), B = cl_bottom(b);

    b->dim = night ? -10 : 0;

    cl_rect(b, L, T, R - L, 54 - T, wall);
    for (int x = first_from(3, 9, L - 2); x < R; x += 9) {
        cl_vline(b, x, T, 54 - T, strip);
        cl_vline(b, x + 1, T, 54 - T, strip);
        cl_vline(b, x + 2, T, 54 - T, cl_rgb(0xF6E4CA));   /* its lit edge */
    }
    /* the wallpaper's little diamonds, between the stripes */
    for (int y = first_from(4, 9, T + 6); y < 45; y += 9) {
        for (int x = first_from(7, 9, L - 2); x < R; x += 9) {
            uint16_t c = cl_rgb(0xDCBB8C);
            cl_px(b, x + 1, y, c);
            cl_px(b, x, y + 1, c);
            cl_px(b, x + 2, y + 1, c);
            cl_px(b, x + 1, y + 2, c);
        }
    }
    /* A cornice where the wall meets the ceiling, when the stage is taller
     * than the watch's, and the wall darkening towards it: without it the
     * wallpaper just stops under the HUD. */
    if (T < 0) {
        cl_shade(b, L, T + 5, R - L, 2, -2);
        cl_shade(b, L, T + 7, R - L, 2, -1);
        cl_rect(b, L, T, R - L, 3, cl_rgb(0xE8DCC8));
        cl_hline(b, L, T, R - L, cl_rgb(0xF6EEE0));
        cl_hline(b, L, T + 3, R - L, cl_rgb(0xC9B79A));
        cl_hline(b, L, T + 4, R - L, cl_rgb(0xDCC49E));
    }
    /* With a whole band of wall above the window (portrait), a string of
     * party pennants: the tall wall reads as a room and not as filler. */
    if (T <= -12) {
        uint16_t line = cl_rgb(0x8A6A4A);
        static const uint32_t col[4] = { 0xE5484D, 0xFFD60A, 0x4A9DF5, 0x45C463 };
        static const uint32_t lit[4] = { 0xFF8A8A, 0xFFF1A8, 0x9FCBFA, 0x8BE39B };
        static const int8_t   half[5] = { 2, 2, 1, 1, 0 };
        int y = T + 6, mid = (L + R) / 2, span = (R - L) / 2;

        for (int x = L; x < R; x++) {
            int d = x - mid;
            cl_px(b, x, y + 3 - 3 * d * d / (span * span), line);
        }
        for (int i = 0, x = L + 4; x < R - 2; x += 8, i++) {
            int d = x - mid;
            int top = y + 4 - 3 * d * d / (span * span);
            for (int r = 0; r < 5; r++) {
                cl_hline(b, x - half[r], top + r, 2 * half[r] + 1, cl_rgb(col[i % 4]));
            }
            cl_px(b, x - 2, top, cl_rgb(lit[i % 4]));
            cl_px(b, x - 1, top + 1, cl_rgb(lit[i % 4]));
            cl_shade(b, x + 1, top + 5, 2, 1, -2);          /* its shadow */
        }
    }
    /* the wall darkening towards the skirting */
    cl_shade(b, L, 46, R - L, 2, -1);

    draw_window(b, night);
    draw_clock(b, 49, 16);
    draw_picture(b, 60, 10);

    /* skirting board */
    cl_rect(b, L, 48, R - L, 6, cl_rgb(0xE8DCC8));
    cl_hline(b, L, 48, R - L, cl_rgb(0xC9B79A));
    cl_hline(b, L, 49, R - L, cl_rgb(0xF6EEE0));
    cl_hline(b, L, 53, R - L, cl_rgb(0xA8916F));

    /* parquet: 4-row planks with staggered joints, each plank its own tone */
    static const uint32_t wood[4] = { 0xC08A53, 0xB77F49, 0xC7935C, 0xBB844E };
    noise_seed(0xC0FFEE);
    for (int band = 0; band * 4 + 54 < B; band++) {
        int y = 54 + band * 4;
        for (int x = first_from((band % 2) ? 0 : 11, 23, L - 22); x < R; x += 23) {
            cl_rect(b, x, y, 23, 4, cl_rgb(wood[noise() % 4]));
            cl_hline(b, x + 1, y + 1, 5, cl_rgb(0xD29C62));     /* the lit end */
            cl_vline(b, x, y, 4, cl_rgb(0x9C6A3A));
        }
        cl_hline(b, L, y, R - L, cl_rgb(0x9C6A3A));
        /* grain */
        for (int i = 0; i < 6; i++) {
            int vx = noise_range(L, R - 5);
            cl_hline(b, vx, y + 1 + (int)(noise() % 3), 3 + (int)(noise() % 3),
                     cl_rgb(0xA97747));
        }
    }
    cl_shade(b, L, 54, R - L, 1, -3);           /* under the skirting */
    cl_shade(b, L, 55, R - L, 1, -1);
    /* the floor darkens towards the viewer: depth without perspective */
    if (B > 80) {
        cl_shade(b, L, B - 10, R - L, 5, -1);
        cl_shade(b, L, B - 5, R - L, 5, -2);
    }

    /* the window's light on the floor, a slanted patch (by day) */
    if (!night) {
        for (int r = 0; r < 7; r++) {
            cl_shade(b, WIN_X + 2 + r, 56 + r, WIN_W - 2 + r, 1, 2);
        }
    }

    draw_rug(b, 46, 63);

    cl_blit(b, 2, 38, CL_SPRITE(cl_spr_plant), false);
    cl_shade(b, 3, 54, 11, 1, -3);              /* the pot's shadow */

    cl_blit(b, 6, 70, CL_SPRITE(cl_spr_bone), false);
    cl_shade(b, 6, 75, 9, 1, -3);
    cl_blit(b, 70, 68, CL_SPRITE(cl_spr_bowl), false);
    cl_shade(b, 71, 75, 11, 1, -3);

    if (night) {
        /* the lamp is lit: it is drawn undimmed and throws a pool of light
           over the wall and the floor around it */
        b->dim = 0;
        cl_blit(b, 78, 41, CL_SPRITE(cl_spr_lamp), false);
        glow(b, 83, 48, 22, 5, 7);
        cl_rect(b, 81, 46, 4, 1, cl_rgb(0xFFF6C0));     /* the bulb */
    } else {
        cl_blit(b, 78, 41, CL_SPRITE(cl_spr_lamp), false);
        /* the lamp's halo on the wall */
        cl_shade(b, 74, 36, 19, 8, 2);
    }
    cl_shade(b, 80, 54, 9, 1, -3);              /* the lamp's foot */
    b->dim = 0;
}

/* --------------------------------------------------------------------------
 * Yard
 * -------------------------------------------------------------------------- */

/* Sky, top to bottom, one band of 8 rows each; the watch's frame starts at
 * band 4 (row 0). Night has the same bands in blues that lighten towards the
 * horizon. */
static const uint32_t SKY_DAY[10] = {
    0x2A87D5, 0x3393DD, 0x3E9FE4, 0x4AACEA,
    0x59B7EE, 0x6BC2F1, 0x7ECDF4, 0x92D8F7, 0xA8E2FA, 0xBEEBFC,
};
static const uint32_t SKY_NIGHT[10] = {
    0x0A0F24, 0x0D132C, 0x101834, 0x131D3D,
    0x172347, 0x1B2A52, 0x21325C, 0x283B66, 0x304571, 0x3A507B,
};

static void draw_sky(cl_buf_t *b, int L, int R, int T, bool night)
{
    const uint32_t *sky = night ? SKY_NIGHT : SKY_DAY;
    cl_rect(b, L, T, R - L, 48 - T, cl_rgb(sky[0]));
    for (int k = 0; k < 10; k++) {
        int y = (k - 4) * 8;
        if (y + 8 <= T) {
            continue;
        }
        cl_rect(b, L, y, R - L, 8, cl_rgb(sky[k]));
        /* the band above bleeds into this one through a dithered row */
        if (k > 0) {
            cl_dither(b, L, y, R - L, 1, cl_rgb(sky[k - 1]));
        }
    }
    cl_rect(b, L, 48, R - L, 2, cl_rgb(night ? 0x46607F : 0xCDF0FD));
    cl_dither(b, L, 47, R - L, 1, cl_rgb(night ? 0x46607F : 0xCDF0FD));
}

static void draw_tree(cl_buf_t *b, int x, int base_y)
{
    const uint16_t trunk = cl_rgb(0x8A5A32);
    const uint16_t bark  = cl_rgb(0x6B4222);
    const uint16_t lit   = cl_rgb(0xA87244);
    const uint16_t leaf  = cl_rgb(0x3FA353);
    const uint16_t leaf2 = cl_rgb(0x59C06B);
    const uint16_t leaf3 = cl_rgb(0x2C7C40);
    const uint16_t leaf4 = cl_rgb(0x1F5F30);

    /* the canopy's shadow on the grass, off to the right (the sun is there) */
    for (int r = 0; r < 3; r++) {
        cl_shade(b, x - 9 + r * 2, base_y - 1 + r, 22 - r * 3, 1, -3);
    }

    cl_rect(b, x - 3, base_y - 22, 7, 22, trunk);
    cl_vline(b, x - 3, base_y - 22, 22, bark);
    cl_vline(b, x - 2, base_y - 20, 20, lit);
    cl_vline(b, x + 2, base_y - 18, 18, bark);
    cl_vline(b, x + 3, base_y - 22, 22, bark);
    /* knots and bark marks */
    cl_px(b, x, base_y - 17, bark);
    cl_px(b, x - 1, base_y - 11, bark);
    cl_px(b, x, base_y - 10, bark);
    cl_px(b, x + 1, base_y - 5, bark);
    /* roots */
    cl_rect(b, x - 5, base_y - 2, 11, 2, bark);
    cl_px(b, x - 6, base_y - 1, bark);
    cl_px(b, x + 6, base_y - 1, bark);
    cl_px(b, x - 4, base_y - 3, trunk);

    /* canopy: overlapping discs, the dark tone first and then the light one
     * shifted upwards, which is how volume reads in pixel art */
    cl_disc(b, x,      base_y - 33, 12, leaf4);
    cl_disc(b, x,      base_y - 34, 11, leaf3);
    cl_disc(b, x - 9,  base_y - 28,  8, leaf3);
    cl_disc(b, x + 9,  base_y - 29,  8, leaf3);
    cl_disc(b, x,      base_y - 36,  9, leaf);
    cl_disc(b, x - 8,  base_y - 30,  6, leaf);
    cl_disc(b, x + 8,  base_y - 31,  6, leaf);
    cl_disc(b, x - 2,  base_y - 39,  5, leaf2);
    cl_disc(b, x + 6,  base_y - 35,  3, leaf2);
    cl_disc(b, x - 9,  base_y - 32,  2, leaf2);
    cl_disc(b, x - 3,  base_y - 41,  2, cl_rgb(0x8BE39B));

    /* leaves: light flecks up top, dark ones underneath */
    noise_seed(0x7EAF);
    for (int i = 0; i < 26; i++) {
        int dx = noise_range(-10, 10), dy = noise_range(-9, 9);
        if (dx * dx + dy * dy > 81) {
            continue;
        }
        cl_px(b, x + dx, base_y - 33 + dy, dy < 0 ? leaf2 : leaf3);
    }

    /* little apples, each with its shine */
    static const int8_t apple[3][2] = { { -6, -31 }, { 5, -27 }, { 1, -24 } };
    for (int i = 0; i < 3; i++) {
        int ax = x + apple[i][0], ay = base_y + apple[i][1];
        cl_disc(b, ax, ay, 1, cl_rgb(0xE5484D));
        cl_px(b, ax + 1, ay + 1, cl_rgb(0xA32A2E));
        cl_px(b, ax - 1, ay - 1, cl_rgb(0xFF8A8A));
    }
}

static void draw_fence(cl_buf_t *b, int x0, int x1, int top, int bottom)
{
    const uint16_t slat = cl_rgb(0xF2EDE0);
    const uint16_t edge = cl_rgb(0xCFC6B2);
    const uint16_t lit  = cl_rgb(0xFFFFFF);

    cl_shade(b, x0, bottom, x1 - x0, 2, -3);        /* its shadow on the grass */

    for (int x = x0; x < x1; x += 7) {
        cl_rect(b, x, top + 2, 4, bottom - top - 2, slat);
        cl_vline(b, x, top + 2, bottom - top - 2, lit);
        cl_vline(b, x + 3, top + 2, bottom - top - 2, edge);
        /* pointed tip */
        cl_hline(b, x + 1, top, 2, slat);
        cl_hline(b, x, top + 1, 4, slat);
        cl_px(b, x + 1, top, lit);
        /* the gap shows the slat's shadow on the rail behind */
        cl_shade(b, x + 4, top + 5, 1, 2, -2);
        cl_shade(b, x + 4, bottom - 6, 1, 2, -2);
    }
    cl_rect(b, x0, top + 5, x1 - x0, 2, slat);
    cl_rect(b, x0, bottom - 6, x1 - x0, 2, slat);
    cl_hline(b, x0, top + 5, x1 - x0, lit);
    cl_hline(b, x0, bottom - 6, x1 - x0, lit);
    cl_hline(b, x0, top + 6, x1 - x0, edge);
    cl_hline(b, x0, bottom - 5, x1 - x0, edge);
    /* nails where the rails cross the slats */
    for (int x = x0 + 1; x < x1; x += 7) {
        cl_px(b, x + 1, top + 5, cl_rgb(0x8A8A90));
        cl_px(b, x + 1, bottom - 6, cl_rgb(0x8A8A90));
    }
}

/* A bush in front of the fence, with a couple of flowers in it */
static void draw_bush(cl_buf_t *b, int x, int base_y)
{
    cl_shade(b, x - 10, base_y, 20, 1, -3);
    cl_disc(b, x,     base_y - 4, 5, cl_rgb(0x2C7C40));
    cl_disc(b, x - 6, base_y - 3, 4, cl_rgb(0x2C7C40));
    cl_disc(b, x + 6, base_y - 3, 4, cl_rgb(0x2C7C40));
    cl_disc(b, x,     base_y - 5, 4, cl_rgb(0x3FA353));
    cl_disc(b, x - 6, base_y - 4, 3, cl_rgb(0x3FA353));
    cl_disc(b, x + 6, base_y - 4, 2, cl_rgb(0x3FA353));
    cl_disc(b, x - 1, base_y - 7, 2, cl_rgb(0x59C06B));
    cl_px(b, x - 7, base_y - 6, cl_rgb(0x59C06B));
    cl_px(b, x + 4, base_y - 6, cl_rgb(0xFFFFFF));
    cl_px(b, x - 4, base_y - 3, cl_rgb(0xFF6FAE));
    cl_px(b, x + 2, base_y - 2, cl_rgb(0xFFD60A));
}

/* A small flower drawn by hand: four petals, a yellow middle, a stem */
static void tiny_flower(cl_buf_t *b, int x, int y, uint32_t petal)
{
    uint16_t c = cl_rgb(petal);
    cl_px(b, x, y + 2, cl_rgb(0x21823E));
    cl_px(b, x, y - 1, c);
    cl_px(b, x - 1, y, c);
    cl_px(b, x + 1, y, c);
    cl_px(b, x, y + 1, c);
    cl_px(b, x, y, cl_rgb(0xFFD60A));
}

static void scene_park(cl_buf_t *b, bool night)
{
    const int L = cl_left(b), R = cl_right(b), T = cl_top(b), B = cl_bottom(b);

    b->dim = 0;
    draw_sky(b, L, R, T, night);

    if (night) {
        /* The still stars, drawn before anything that stands in front of the
         * sky. A few are bigger; the twinkling ones are cl_scene_anim()'s. */
        noise_seed(0x5EED);
        int stars = 26 * (44 - T) / 44;
        for (int i = 0; i < stars; i++) {
            int x = noise_range(L + 1, R - 2);
            int y = noise_range(T + 1, 44);
            uint32_t c = (i % 3) ? 0xC8D0E8 : 0xFFF6D0;
            cl_px(b, x, y, cl_rgb(c));
            if (i % 9 == 0) {
                cl_px(b, x - 1, y, cl_rgb(0x6A78A0));
                cl_px(b, x + 1, y, cl_rgb(0x6A78A0));
                cl_px(b, x, y - 1, cl_rgb(0x6A78A0));
                cl_px(b, x, y + 1, cl_rgb(0x6A78A0));
            }
        }
        /* the moon, with its halo */
        dither_disc(b, 76, 9, 7, cl_rgb(0x2A3868));
        cl_blit(b, 72, 5, CL_SPRITE(cl_spr_moon), false);
        b->dim = -7;
    }

    /* hills far away, pale with the distance, with a few trees on them */
    for (int x = L; x < R; x++) {
        int d1 = x - 44, d2 = x - 96, d3 = x + 4;
        int h = 8 - d1 * d1 / 160;
        int h2 = 7 - d2 * d2 / 110;
        int h3 = 6 - d3 * d3 / 90;
        if (h2 > h) h = h2;
        if (h3 > h) h = h3;
        if (h > 0) {
            cl_vline(b, x, 50 - h, h, cl_rgb(0xA3D4B0));
            cl_px(b, x, 50 - h, cl_rgb(0xBEE2C6));
        }
    }
    static const int8_t far_tree[4][2] = { { 38, 42 }, { 44, 41 }, { 52, 43 }, { 88, 44 } };
    for (int i = 0; i < 4; i++) {
        cl_disc(b, far_tree[i][0], far_tree[i][1], 1, cl_rgb(0x7FB68E));
        cl_px(b, far_tree[i][0], far_tree[i][1] + 2, cl_rgb(0x8A7A60));
    }

    /* hills nearer */
    for (int x = L; x < R; x++) {
        int h1 = 6 - ((x - 20) * (x - 20)) / 90;
        int h2 = 8 - ((x - 68) * (x - 68)) / 70;
        int h = h1 > h2 ? h1 : h2;
        if (h > 0) {
            cl_vline(b, x, 50 - h, h, cl_rgb(0x6FBF74));
            cl_px(b, x, 50 - h, cl_rgb(0x8AD48C));
            if (h > 2 && ((x * 7) % 5) == 0) {
                cl_px(b, x, 50 - h + 2, cl_rgb(0x5FAE66));
            }
        }
    }

    /* grass: darker far, lighter near, the two meeting through a dither */
    cl_rect(b, L, 50, R - L, B - 50, cl_rgb(0x6CC24A));
    cl_rect(b, L, 50, R - L, 3, cl_rgb(0x57A83B));
    cl_dither(b, L, 53, R - L, 1, cl_rgb(0x57A83B));
    cl_rect(b, L, 68, R - L, B - 68, cl_rgb(0x79CE55));
    cl_dither(b, L, 67, R - L, 1, cl_rgb(0x79CE55));

    /* the tufts, as dense as on the watch's 26 rows of grass: little Vs */
    noise_seed(0xA11CE);
    int tufts = 90 * (B - 52) / 26;
    for (int i = 0; i < tufts; i++) {
        int x = noise_range(L, R - 1);
        int y = noise_range(52, B - 2);
        uint16_t c = (y > 68) ? cl_rgb(0x63BC43) : cl_rgb(0x54A63A);
        cl_px(b, x, y, c);
        cl_px(b, x - 1, y - 1, c);
        cl_px(b, x + 1, y - 1, c);
        if ((i & 3) == 0) {
            cl_px(b, x, y - 2, cl_rgb(0x9BDC73));   /* a blade catching the sun */
        }
    }

    draw_fence(b, 58, R, 36, 50);
    draw_tree(b, 14, 52);
    draw_bush(b, 84, 50);

    /* scattered flowers */
    cl_blit(b, 33, 71, CL_SPRITE(cl_spr_flower), false);
    cl_blit(b, 63, 73, CL_SPRITE(cl_spr_flower), true);
    cl_blit(b, 82, 60, CL_SPRITE(cl_spr_flower), false);
    cl_blit(b, 4, 63, CL_SPRITE(cl_spr_flower), true);
    /* and small ones in the near grass, more of them the more grass there is */
    static const uint32_t petal[4] = { 0xFFFFFF, 0xFFD60A, 0xB072F0, 0xFFB3D4 };
    noise_seed(0xF10E5);
    int flowers = (B - 70) / 2;
    for (int i = 0; i < flowers; i++) {
        int x = noise_range(L + 2, R - 3);
        int y = noise_range(72, B - 4);
        tiny_flower(b, x, y, petal[noise() % 4]);
    }

    /* dirt path where the critter stands, with pebbles and a shaded rim */
    static const int8_t path[] = { 14, 18, 20, 21, 20, 17 };
    const int rows = (int)(sizeof(path) / sizeof(path[0]));
    for (int i = 0; i < rows; i++) {
        cl_hline(b, 46 - path[i], 63 + i, path[i] * 2, cl_rgb(0xCBA96F));
        cl_px(b, 46 - path[i], 63 + i, cl_rgb(0xB08F58));
        cl_px(b, 46 + path[i] - 1, 63 + i, cl_rgb(0xB08F58));
    }
    cl_hline(b, 46 - 14, 63, 28, cl_rgb(0xDDBE88));
    cl_hline(b, 46 - 16, 63 + rows, 32, cl_rgb(0x5FA83F));
    noise_seed(0xBEEF);
    for (int i = 0; i < 14; i++) {
        int x = noise_range(28, 64), y = noise_range(64, 68);
        cl_px(b, x, y, cl_rgb(0xB08F58));
        if (i % 3 == 0) {
            cl_px(b, x + 1, y, cl_rgb(0xE8D2A2));   /* a pebble's lit side */
        }
    }

    /* long grass along the bottom of the view */
    noise_seed(0x6A55);
    for (int x = L; x < R; x += 2) {
        int h = noise_range(2, 4);
        cl_vline(b, x, B - h, h, cl_rgb(0x4FA83A));
        cl_px(b, x, B - h, cl_rgb(0x8BD86A));
    }

    b->dim = 0;
}

/* --------------------------------------------------------------------------
 * Moving parts
 * -------------------------------------------------------------------------- */

/* A cloud: a flat bottom, two puffs on top and a blue-grey belly */
static void draw_cloud(cl_buf_t *b, int x, int y, int w)
{
    const uint16_t white = cl_rgb(0xFFFFFF);
    cl_rect(b, x, y, w, 3, white);
    cl_rect(b, x + 1, y - 1, w - 2, 1, white);
    cl_rect(b, x + 2, y - 2, w / 2 - 1, 1, white);
    cl_rect(b, x + w / 2, y - 3, w / 3 > 1 ? w / 3 : 2, 2, white);
    cl_hline(b, x + 1, y + 3, w - 2, cl_rgb(0xD4E9F6));
    cl_hline(b, x, y + 2, w, cl_rgb(0xEEF7FC));
}

void cl_scene_draw(cl_buf_t *b, cl_scene_id_t scene, bool night)
{
    if (scene == CL_SCENE_PARK) {
        scene_park(b, night);
    } else {
        scene_home(b, night);
    }
}

int cl_scene_anim(cl_buf_t *b, cl_scene_id_t scene, int frame, bool night,
                  cl_rect_t *out, int max)
{
    const int L = cl_left(b), R = cl_right(b), T = cl_top(b);
    int n = 0;

    if (scene == CL_SCENE_PARK && night) {
        /* A handful of stars twinkle; they change every 4 frames, so only
         * then are they marked. The rest of the sky is baked. */
        noise_seed(0x7717);
        for (int i = 0; i < 8; i++) {
            int x = noise_range(L + 2, R - 3);
            int y = noise_range(T + 1, 40);
            int phase = ((frame / 4) + i * 3) % 6;
            if (phase < 4) {
                cl_px(b, x, y, cl_rgb(phase == 1 ? 0xFFFFFF : 0xFFF6D0));
            }
            if (phase == 1) {
                cl_px(b, x - 1, y, cl_rgb(0x8A96BE));
                cl_px(b, x + 1, y, cl_rgb(0x8A96BE));
                cl_px(b, x, y - 1, cl_rgb(0x8A96BE));
                cl_px(b, x, y + 1, cl_rgb(0x8A96BE));
            }
            if ((frame % 4) == 0) {
                mark(out, &n, max, x - 1, y - 1, 3, 3);
            }
        }
        /* fireflies over the grass, blinking */
        for (int i = 0; i < 4; i++) {
            int t = frame + i * 23;
            int x = 24 + i * 16 + ((t / 3) % 12) - 6 + ((t / 7) % 3);
            int y = 44 + i * 5 + ((t / 5) % 6) - 3;
            if (((t / 6) % 4) != 0) {
                cl_px(b, x, y, cl_rgb(0xE8FF7A));
                cl_px(b, x + 1, y, cl_rgb(0x6E8A3A));
            }
            mark(out, &n, max, x - 1, y - 1, 4, 3);
        }
        return n;
    }

    if (scene == CL_SCENE_PARK) {
        /* The sun is drawn per frame and not into the cached background:
         * the rays pulse. */
        dither_disc(b, 76, 9, 9, cl_rgb(0xFFF6C0));
        cl_disc(b, 76, 9, 7, cl_rgb(0xFFE066));
        cl_disc(b, 76, 9, 5, cl_rgb(0xFFF0A0));
        cl_disc(b, 75, 8, 2, cl_rgb(0xFFFBE0));
        for (int i = 0; i < 8; i++) {
            static const int8_t dx[] = { 0, 7, 10, 7, 0, -7, -10, -7 };
            static const int8_t dy[] = { -10, -7, 0, 7, 10, 7, 0, -7 };
            int beat = ((frame / 6) + i) % 4 ? 5 : 6;
            cl_px(b, 76 + dx[i], 9 + dy[i], cl_rgb(0xFFE066));
            cl_px(b, 76 + dx[i] * (beat + 1) / beat,
                     9 + dy[i] * (beat + 1) / beat, cl_rgb(0xFFEC99));
        }
        mark(out, &n, max, 76 - 13, 9 - 13, 27, 27);

        /* The clouds travel along the strip of clear sky between the tree and
         * the sun, so they can be drawn over the copied background without
         * covering anything. */
        static const int8_t cloud_y[3] = { 6, 15, 3 };
        static const int8_t cloud_w[3] = { 11, 8, 6 };
        for (int i = 0; i < 3; i++) {
            int span = 40;
            int x = 28 + ((frame / (3 + i * 2) + i * 17) % span);
            draw_cloud(b, x, cloud_y[i], cloud_w[i]);
            mark(out, &n, max, x - 1, cloud_y[i] - 3, cloud_w[i] + 2, 7);
        }
        /* With sky above the watch's frame (portrait), two slow clouds cross
         * the whole width up there, over nothing but sky. */
        if (T <= -12) {
            for (int i = 0; i < 2; i++) {
                int w = 12 - i * 3;
                int span = R - L + w + 2;
                int x = L - w + ((frame / (7 + i * 5) + i * 41) % span);
                int y = T + 5 + i * 7;
                draw_cloud(b, x, y, w);
                mark(out, &n, max, x - 1, y - 3, w + 2, 7);
            }
        }

        /* butterfly: it rises and falls as it crosses the grass */
        int bx = 20 + ((frame / 2) % 60);
        int by = 40 + (frame / 3) % 6;
        cl_blit(b, bx, by, CL_SPRITE(cl_spr_butterfly), (frame & 4) != 0);
        mark(out, &n, max, bx - 1, by - 1, 11, 9);
        return n;
    }

    if (night) {
        return 0;                   /* the room at night is still */
    }

    /* indoors the window's clouds move, which is pure sky; the bars are
     * drawn again over them so the cloud passes behind the glass */
    int x = WIN_X + 1 + ((frame / 6) % 17);
    cl_rect(b, x, 22, 6, 2, cl_rgb(0xFFFFFF));
    cl_rect(b, x + 1, 21, 4, 1, cl_rgb(0xFFFFFF));
    cl_hline(b, x, 24, 6, cl_rgb(0xDCEEF8));
    window_bars(b, WIN_X, WIN_Y, WIN_W, WIN_H);
    mark(out, &n, max, WIN_X, 21, WIN_W, 4);

    /* dust motes floating in the window's light */
    for (int i = 0; i < 4; i++) {
        int px = 34 + i * 13 + ((frame / (4 + i)) % 7);
        int py = 20 + i * 6 + ((frame / (5 + i)) % 5);
        cl_px(b, px, py, cl_rgb(0xFBEFD8));
        mark(out, &n, max, px, py, 1, 1);
    }
    return n;
}

const char *cl_scene_name(cl_scene_id_t scene)
{
    return (scene == CL_SCENE_PARK) ? _("PATIO") : _("CASA");
}
