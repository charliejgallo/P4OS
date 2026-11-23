/*
 * Tortuga - the board on the screen: an LVGL canvas whose buffer tg_gfx
 * (Bastion's compositor) draws. The board is toy blocks drawn row by row
 * from the back (DESIGN.md, 9): the ground, what stands on it and the
 * turtle; only the 16 x 16 tiles that changed are drawn again. On the
 * sheet of paper the finished strokes are painted into the background, so
 * a long drawing costs nothing more per frame.
 */
#include "tg_view.h"

#include <math.h>
#include <string.h>

#define TILE_W  64
#define TILE_D  48
#define FRONT   26      /* the soil under the front row */
#define HEAD    64      /* room over the back row for what stands up */

static struct {
    lv_obj_t *canvas;
    uint16_t *fb, *bg;
    uint32_t *shown;
    tg_gfx_t gfx;
    bool gfx_ok, full;
    int w, h;
    bool paper;
    int mw, mh;             /* the map, in tiles */
    int ox, oy;             /* the back left corner of tile (0, 0) */
    float scale, cx, cy;    /* the paper */
    int baked;              /* strokes painted into bg */
    const tg_ev_t *ev;
} V;

/* ------------------------------------------------------------ backgrounds */

static uint16_t rgb(uint32_t c)
{
    return tg_rgb(c);
}

static void sky(void)
{
    /* a soft sky, warm low and blue high, with a dither so 565 shows no bands */
    for (int y = 0; y < V.h; y++) {
        float t = (float)y / (float)(V.h > 1 ? V.h - 1 : 1);
        float r = 190 + (255 - 190) * t, g = 228 + (238 - 228) * t, b = 246 + (205 - 246) * t;
        uint16_t *row = V.bg + (size_t)y * V.w;
        for (int x = 0; x < V.w; x++) {
            int d = ((x ^ y) & 3) - 1;
            int R = (int)r + d * 2, G = (int)g + d, B = (int)b + d * 2;
            R = R < 0 ? 0 : R > 255 ? 255 : R;
            G = G < 0 ? 0 : G > 255 ? 255 : G;
            B = B < 0 ? 0 : B > 255 ? 255 : B;
            row[x] = rgb((uint32_t)(R << 16 | G << 8 | B));
        }
    }
}

static void put_px(int x, int y, uint16_t c, int a)
{
    if (x < 0 || y < 0 || x >= V.w || y >= V.h) return;
    uint16_t *p = V.bg + (size_t)y * V.w + x;
    if (a >= 255) {
        *p = c;
        return;
    }
    uint32_t d = *p;
    uint32_t r = ((c >> 11) * (uint32_t)a + (d >> 11) * (uint32_t)(255 - a)) / 255;
    uint32_t g = (((c >> 5) & 63) * (uint32_t)a + ((d >> 5) & 63) * (uint32_t)(255 - a)) / 255;
    uint32_t b = ((c & 31) * (uint32_t)a + (d & 31) * (uint32_t)(255 - a)) / 255;
    *p = (uint16_t)(r << 11 | g << 5 | b);
}

/* a round-ended line of half width hw into the background */
static void bg_line(float x0, float y0, float x1, float y1, float hw, uint16_t c, int alpha)
{
    int xa = (int)floorf(fminf(x0, x1) - hw - 1), xb = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int ya = (int)floorf(fminf(y0, y1) - hw - 1), yb = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = ya; y <= yb; y++) {
        for (int x = xa; x <= xb; x++) {
            float t = l2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / l2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float ex = x - (x0 + t * dx), ey = y - (y0 + t * dy);
            float d = sqrtf(ex * ex + ey * ey) - hw;
            if (d >= 1) continue;
            int a = d <= 0 ? alpha : (int)((1 - d) * (float)alpha);
            put_px(x, y, c, a);
        }
    }
}

static const uint32_t INK[10] = { 0x23202a, 0xe8443a, 0x2f7fe0, 0x2e9e45, 0xf0b400, 0xf07a1a, 0x8e4ad8,
                                  0xf06aa8, 0x8a5631, 0xffffff };

void tg_view_paper_point(float x, float y, float *sx, float *sy)
{
    *sx = V.cx + x * V.scale;
    *sy = V.cy - y * V.scale;
}

static void paper(const tg_world_t *figure)
{
    /* the sheet: cream with a soft grid every 50 steps, and the figure to
     * draw, dotted */
    uint16_t desk = rgb(0x3a3f47), sheet = rgb(0xfbf6e9), grid = rgb(0xe6dcc3), axis = rgb(0xd9ccad);
    for (size_t i = 0; i < (size_t)V.w * V.h; i++) V.bg[i] = desk;
    float x0, y0, x1, y1;
    tg_view_paper_point(-TG_PAPER, TG_PAPER, &x0, &y0);
    tg_view_paper_point(TG_PAPER, -TG_PAPER, &x1, &y1);
    for (int y = (int)y0; y < (int)y1; y++)
        for (int x = (int)x0; x < (int)x1; x++) put_px(x, y, sheet, 255);
    for (int k = -TG_PAPER; k <= TG_PAPER; k += 50) {
        float a, b, c, d;
        tg_view_paper_point((float)k, TG_PAPER, &a, &b);
        tg_view_paper_point((float)k, -TG_PAPER, &c, &d);
        bg_line(a, b, c, d, 0.5f, k ? grid : axis, 255);
        tg_view_paper_point(-TG_PAPER, (float)k, &a, &b);
        tg_view_paper_point(TG_PAPER, (float)k, &c, &d);
        bg_line(a, b, c, d, 0.5f, k ? grid : axis, 255);
    }
    if (!figure) return;
    uint16_t dot = rgb(0x9fb7d9);
    for (int i = 0; i < figure->nev; i++) {
        const tg_ev_t *e = &figure->ev[i];
        if (e->kind != TG_EV_MOVE || !e->pen) continue;
        float ax, ay, bx, by;
        tg_view_paper_point(e->x0, e->y0, &ax, &ay);
        tg_view_paper_point(e->x1, e->y1, &bx, &by);
        float len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        int n = (int)(len / 9.0f);
        for (int k = 0; k <= n; k++) {
            float t = n ? (float)k / (float)n : 0;
            float px = ax + (bx - ax) * t, py = ay + (by - ay) * t;
            bg_line(px, py, px, py, 2.2f, dot, 255);
        }
    }
}

static void bake(const tg_ev_t *ev, int upto)
{
    for (int i = V.baked; i < upto; i++) {
        const tg_ev_t *e = &ev[i];
        if (e->kind != TG_EV_MOVE || !e->pen) continue;
        float ax, ay, bx, by;
        tg_view_paper_point(e->x0, e->y0, &ax, &ay);
        tg_view_paper_point(e->x1, e->y1, &bx, &by);
        bg_line(ax, ay, bx, by, 2.4f, rgb(INK[e->color % 10]), 255);
    }
    V.baked = upto;
}

/* ------------------------------------------------------------------ setup */

bool tg_view_open(lv_obj_t *parent, int x, int y, int w, int h)
{
    tg_view_close();
    V.w = w;
    V.h = h;
    V.fb = tg_alloc((size_t)w * h * 2);
    V.bg = tg_alloc((size_t)w * h * 2);
    V.shown = tg_malloc(sizeof(uint32_t) * TGX_MAX_TILES);
    V.gfx_ok = V.fb && V.bg && V.shown && tg_gfx_init(&V.gfx);
    if (!V.gfx_ok) {
        tg_view_close();
        return false;
    }
    V.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(V.canvas, V.fb, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(V.canvas, x, y);
    lv_obj_set_size(V.canvas, w, h);
    lv_obj_remove_flag(V.canvas, LV_OBJ_FLAG_CLICKABLE);
    sky();
    memcpy(V.fb, V.bg, (size_t)w * h * 2);
    tg_gfx_size(&V.gfx, w, h, V.bg);
    V.full = true;
    return true;
}

void tg_view_close(void)
{
    if (V.canvas) lv_obj_delete(V.canvas);
    tg_free(V.fb);
    tg_free(V.bg);
    tg_free(V.shown);
    if (V.gfx_ok) tg_gfx_free(&V.gfx);
    memset(&V, 0, sizeof V);
}

lv_obj_t *tg_view_obj(void)
{
    return V.canvas;
}

void tg_view_level(const tg_world_t *start, const tg_world_t *figure)
{
    if (!V.gfx_ok) return;
    V.paper = start->paper;
    V.baked = 0;
    if (V.paper) {
        float side = (float)(V.w < V.h ? V.w : V.h) - 24.0f;
        V.scale = side / (2.0f * TG_PAPER);
        V.cx = V.w / 2.0f;
        V.cy = V.h / 2.0f;
        paper(figure);
    } else {
        V.mw = start->w;
        V.mh = start->h;
        V.ox = (V.w - V.mw * TILE_W) / 2;
        int content = V.mh * TILE_D + FRONT + HEAD;
        V.oy = (V.h - content) / 2 + HEAD;
        if (V.oy < HEAD / 2) V.oy = HEAD / 2;
        sky();
    }
    tg_gfx_size(&V.gfx, V.w, V.h, V.bg);
    V.full = true;
}

void tg_view_tile_point(float x, float y, float *sx, float *sy)
{
    *sx = (float)V.ox + x * TILE_W + TILE_W / 2.0f;
    *sy = (float)V.oy + y * TILE_D + TILE_D / 2.0f;
}

/* ------------------------------------------------------------------ frame */

static int grass_variant(int x, int y)
{
    unsigned h = ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u);
    h &= 0xffff;
    return h % 7 == 0 ? 2 + (int)((h >> 4) % 2) : (int)(h % 2);
}

static char at(const tg_scene_t *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return ' ';
    return s->t[y][x];
}

/* the turtle's sprite for a heading (0 = north, clockwise): the sheets
 * count yaw counter clockwise from looking right */
static int turtle_col(const tg_sheet_t *sh, float hd)
{
    float yaw = fmodf(90.0f - hd + 720.0f, 360.0f);
    if (sh->cols >= 16) return (int)roundf(yaw / 22.5f) % 16;
    return (int)roundf(yaw / 90.0f) % 4;
}

static void draw_turtle(const tg_scene_t *s, float px, float py)
{
    if (!tg->have_pak) {
        tg_disc(&V.gfx, (int)px, (int)py - 8, 22, 18, rgb(0xc7772a), 255);
        return;
    }
    tg_shadow(&V.gfx, (int)px, (int)py + 2, 24, 10, 90);
    const tg_sheet_t *sh = &tg->sheet[s->anim];
    int col = turtle_col(sh, s->hd);
    int frame = s->frame * sh->cols + col;
    if (frame >= sh->nframes) frame = col;
    tg_spr(&V.gfx, sh, frame, (int)px, (int)(py - s->lift), 0, 255);
}

void tg_view_draw(const tg_scene_t *s)
{
    if (!V.gfx_ok) return;
    tg_gfx_t *g = &V.gfx;
    if (V.paper && s->ev) {
        if (s->strokes < V.baked || s->ev != V.ev) {
            V.ev = s->ev;
            paper(s->figure);
            V.baked = 0;
            V.full = true;
        }
        if (s->strokes > V.baked) {
            bake(s->ev, s->strokes);
            V.full = true;
        }
    }
    tg_gfx_begin(g);
    if (V.paper) {
        if (s->partial) {
            float ax, ay, bx, by;
            tg_view_paper_point(s->px0, s->py0, &ax, &ay);
            tg_view_paper_point(s->px1, s->py1, &bx, &by);
            /* the stroke being drawn, a thin quadrilateral (the beam adds light,
             * which a cream sheet would swallow) */
            float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
            if (l > 0.5f) {
                float nx = -dy / l * 2.4f, ny = dx / l * 2.4f;
                int16_t q[8] = { (int16_t)(ax + nx), (int16_t)(ay + ny), (int16_t)(bx + nx), (int16_t)(by + ny),
                                 (int16_t)(bx - nx), (int16_t)(by - ny), (int16_t)(ax - nx), (int16_t)(ay - ny) };
                tg_quad(g, q, rgb(INK[s->pcolor % 10]), 255);
            }
        }
        float px, py;
        tg_view_paper_point(s->x, s->y, &px, &py);
        draw_turtle(s, px, py + 10);
    } else {
        int trow = (int)ceilf(s->y - 0.001f);
        int water = (s->tick / 6) % 4;
        for (int y = 0; y < s->h; y++) {
            for (int x = 0; x < s->w; x++) {
                char c = at(s, x, y);
                if (c == ' ') continue;
                float cx, cy;
                tg_view_tile_point((float)x, (float)y, &cx, &cy);
                const tg_sheet_t *gs = &tg->sheet[SH_GROUND];
                if (!tg->have_pak) {
                    int16_t q[8] = { (int16_t)(cx - 32), (int16_t)(cy - 24), (int16_t)(cx + 32), (int16_t)(cy - 24),
                                     (int16_t)(cx + 32), (int16_t)(cy + 24), (int16_t)(cx - 32), (int16_t)(cy + 24) };
                    tg_quad(g, q, c == '~' ? rgb(0x2d8fd6) : rgb(0x6fbf4a), 255);
                    continue;
                }
                tg_spr(g, gs, c == '~' || c == '=' ? 4 + water : grass_variant(x, y), (int)cx, (int)cy, 0, 255);
            }
            for (int x = 0; x < s->w && tg->have_pak; x++) {
                char c = at(s, x, y);
                float cx, cy;
                tg_view_tile_point((float)x, (float)y, &cx, &cy);
                int X = (int)cx, Y = (int)cy;
                switch (c) {
                case '#': tg_spr(g, &tg->sheet[SH_ROCK], (x + y) & 1, X, Y, 0, 255); break;
                case 'L': tg_spr(g, &tg->sheet[SH_LETTUCE], 0, X, Y, 0, 255); break;
                case 'k': tg_spr(g, &tg->sheet[SH_KEY], (s->tick / 5) % 4, X, Y, 0, 255); break;
                case 'P':
                case 'p': {
                    bool ns = at(s, x - 1, y) == '#' || at(s, x + 1, y) == '#';
                    tg_spr(g, &tg->sheet[SH_DOOR], (ns ? 0 : 2) + (c == 'p'), X, Y, 0, 255);
                    break;
                }
                case 'M': tg_spr(g, &tg->sheet[SH_GOAL], (s->tick / 4) % 4, X, Y, 0, 255); break;
                case '=': {
                    bool ns = at(s, x - 1, y) == '~' || at(s, x + 1, y) == '~';
                    tg_spr(g, &tg->sheet[SH_BRIDGE], ns ? 1 : 0, X, Y, 0, 255);
                    break;
                }
                default: break;
                }
            }
            if (y == trow || (y == s->h - 1 && trow >= s->h)) {
                float px, py;
                tg_view_tile_point(s->x, s->y, &px, &py);
                draw_turtle(s, px, py + s->sink);
            }
        }
        if (trow < 0) {
            float px, py;
            tg_view_tile_point(s->x, s->y, &px, &py);
            draw_turtle(s, px, py);
        }
    }
    tg_gfx_end(g, V.fb, V.shown, V.full);
    V.full = false;
    if (g->dirty.x1 > g->dirty.x0) {
        lv_area_t a = { g->dirty.x0, g->dirty.y0, g->dirty.x1 - 1, g->dirty.y1 - 1 };
        lv_obj_invalidate_area(V.canvas, &a);
    }
}

void tg_view_turtle_point(const tg_scene_t *s, int *x, int *y)
{
    float px, py;
    if (V.paper) tg_view_paper_point(s->x, s->y, &px, &py);
    else tg_view_tile_point(s->x, s->y, &px, &py);
    *x = (int)px;
    *y = (int)py;
}

/* ------------------------------------------------------------ one sprite */

void tg_sheet_blit(const tg_sheet_t *sh, int frame, uint16_t *buf, int w, int h, int x, int y)
{
    if (!sh->data || frame < 0 || frame >= sh->nframes) return;
    const tg_frame_t *f = &sh->fr[frame];
    for (int r = 0; r < f->h; r++) {
        int py = y + f->oy + r;
        if (py < 0 || py >= h) continue;
        uint32_t ro;
        memcpy(&ro, sh->data + f->off + 4u * (uint32_t)r, 4);
        const uint8_t *p = sh->data + ro;
        int px = x + f->ox;
        for (;;) {
            uint16_t op;
            memcpy(&op, p, 2);
            p += 2;
            int kind = op >> 14, n = op & 0x3FFF;
            if (kind == 0) break;
            if (kind == 1) {
                px += n;
                continue;
            }
            for (int i = 0; i < n; i++, px++) {
                uint16_t c, a = 255;
                memcpy(&c, p, 2);
                p += 2;
                if (kind == 3) {
                    memcpy(&a, p, 2);
                    p += 2;
                }
                if (px < 0 || px >= w) continue;
                uint16_t *d = buf + (size_t)py * w + px;
                if (a >= 250) {
                    *d = c;
                } else {
                    uint32_t A = a, B = 255 - a, D = *d;
                    uint32_t R = ((c >> 11) * A + (D >> 11) * B) / 255;
                    uint32_t G = (((c >> 5) & 63) * A + ((D >> 5) & 63) * B) / 255;
                    uint32_t Bl = ((c & 31) * A + (D & 31) * B) / 255;
                    *d = (uint16_t)(R << 11 | G << 5 | Bl);
                }
            }
        }
    }
}
