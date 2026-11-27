/*
 * Marcador - chalk on a slate.
 *
 * The board of a club or a bar: a green-black slate in a wooden frame, the
 * ledge at the bottom with a stick of chalk and the felt eraser, and the
 * ghosts of games before, wiped but never quite gone. Two ways of keeping a
 * score on it:
 *
 *   - tally marks: four strokes and the fifth across them;
 *   - running totals: every new total written under the last, the last one
 *     struck through, as darts players and card players do (adding, or
 *     counting down from 501).
 *
 * Chalk is not paint: it catches on the slate's grain and leaves gaps. All
 * the chalk on the board lives in one coverage layer, and a pixel shows it
 * through a fixed grain of the slate, so every stroke has the same rough
 * edge and the same little holes. Strokes are written, not stamped: the tip
 * moves along the line at a hand's speed with the squeak of the chalk, so a
 * new number appears digit by digit.
 *
 * Taking something back is the eraser: it rubs over the marks, and when it
 * lifts they are gone, leaving a pale haze where they were.
 *
 * Fingers: tap a column for a mark (tally) or to write a score (totals:
 * the keypad), hold it to rub the last mark out.
 */
#include "mr_app.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXMK   160         /* marks per player shown                     */
#define MAXPATH 6
#define SPEED   950.0f      /* px/s of the chalk tip                      */
#define HOLD_MS 520
#define ERASE_S 0.55f
#define JOBS    64

typedef struct {
    int         n;
    mr_stroke_t s[MAXPATH];     /* screen coordinates                       */
} path_t;

typedef struct {
    int      kind;          /* 1 write, 2 erase                              */
    int      p, from;       /* player, first mark                            */
} job_t;

typedef struct {
    mr_cv_t   bg, base;
    uint8_t  *chalk, *names, *dust;
    lv_obj_t *cvo;
    mr_img_t  eraser;
    lv_obj_t *ero;
    int       np;
    bool      totals;
    struct {
        int x, w;           /* the column                                 */
        int y0;             /* where the marks begin                       */
        int drawn;          /* marks on the board                          */
        int32_t val[MAXMK]; /* totals written                              */
        int cap;
    } c[MR_MAXP];
    float     rowh, gw, gh;
    int       gpr;          /* tally groups in a row                       */
    float     lw;           /* chalk width                                 */
    /* the job in hand */
    job_t     q[JOBS];
    int       nq;
    path_t    cur;
    float     dist, done;
    int       seg_s, seg_k;
    float     et;           /* the eraser's time                           */
    int       ex0, ey0, ex1, ey1;
    bool      erased;
    uint32_t  squeak;
    struct {
        int  p;
        bool held;
    } f[MR_FINGERS];
} tiza_t;

#define ST ((tiza_t *)s->st)

static void tz_btn(mr_scene_t *s, int arg);

/* --- the board ------------------------------------------------------------------------------- */

static float grain(int x, int y)
{
    float h = mr_hashf(x, y, 1234);
    float g = 0.45f + h * 0.75f;
    if (h < 0.10f) g = 0.12f;
    return mr_clampf(g, 0, 1);
}

static void compose(tiza_t *T, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > T->bg.w) x1 = T->bg.w;
    if (y1 > T->bg.h) y1 = T->bg.h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int i = y * T->bg.w + x;
            uint32_t c = mr_888(T->base.px[i]);
            if (T->dust[i]) c = mr_rgb_mix(c, 0xC8D0C8, T->dust[i] / 255.0f * 0.22f);
            int a = T->chalk[i] > T->names[i] ? T->chalk[i] : T->names[i];
            if (a) c = mr_rgb_mix(c, 0xEEF0E8, a / 255.0f * grain(x, y));
            T->bg.px[i] = mr_565(c);
        }
}

static void invalidate(mr_scene_t *s, int x0, int y0, int x1, int y1)
{
    tiza_t *T = ST;
    lv_area_t o, a;
    lv_obj_get_coords(T->cvo, &o);
    a.x1 = o.x1 + x0;
    a.y1 = o.y1 + y0;
    a.x2 = o.x1 + x1;
    a.y2 = o.y1 + y1;
    lv_obj_invalidate_area(T->cvo, &a);
}

static void slate_draw(mr_scene_t *s, int ledge_y)
{
    tiza_t *T = ST;
    mr_cv_t *cv = &T->base;
    int w = cv->w, h = cv->h, fr = 26;
    mr_field_t m, ghost;
    mr_field_new(&m, w, h, 8, 1.0f / 140.0f, 4, 501);
    mr_field_new(&ghost, w, h, 4, 1.0f / 45.0f, 3, 502);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float n = mr_field_at(&m, x, y), gh = mr_field_at(&ghost, x, y);
            float k = 0.92f + (n - 0.5f) * 0.25f + (mr_hashf(x, y, 9) - 0.5f) * 0.06f;
            uint32_t c = mr_rgb_scale(0x26362E, k);
            /* old chalk, wiped in arcs */
            float wipe = mr_smooth(0.55f, 0.85f, gh) * (0.5f + 0.5f * sinf(x * 0.02f + y * 0.006f + n * 6));
            c = mr_rgb_mix(c, 0x9AA89E, wipe * 0.10f);
            cv->px[y * w + x] = mr_565(c);
        }
    mr_field_free(&m);
    mr_field_free(&ghost);
    /* the frame: oak with rounded edges */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = x < fr ? x : x >= w - fr ? w - 1 - x : y < s->top - 30 + fr && y >= s->top - 30 ? y - (s->top - 30) : 999;
            bool ledge = y >= ledge_y;
            if (y < s->top - 30) d = 999;
            if (d >= fr && !ledge) continue;
            float t = ledge ? (float)(y - ledge_y) / (h - ledge_y) : (float)d / fr;
            float g = 0.85f + 0.10f * sinf((ledge ? y : x + y) * 0.5f + mr_vnoise(x * 0.02f, y * 0.2f, 33) * 6);
            float k = (0.75f + 0.45f * sinf(t * 3.14159f)) * g;
            if (ledge && y < ledge_y + 4) k *= 1.3f;
            cv->px[y * w + x] = mr_565(mr_rgb_scale(0x9A6A3A, k));
        }
    /* the ledge's lip throws a shadow on the slate */
    for (int x = fr; x < w - fr; x++)
        for (int i = 1; i < 8; i++) mr_cv_mix(cv, x, ledge_y - i, 0x000000, 0.30f - i * 0.035f);
    /* chalk dust lying on the ledge */
    for (int i = 0; i < 1400; i++) {
        uint32_t hs = mr_hash((uint32_t)i * 2654435761u);
        int x = fr + (int)(hs % (uint32_t)(w - 2 * fr)), y = ledge_y + 3 + (int)((hs >> 12) % 10);
        mr_cv_mix(cv, x, y, 0xE8EAE2, 0.25f + ((hs >> 24) & 63) / 160.0f);
    }
}

/* the eraser seen from above: a block of wood with the felt under it */
static void eraser_draw(mr_img_t *im)
{
    float w = im->w - 10, h = im->h - 12;
    mr_img_rrect(im, 2, h * 0.55f, w, h * 0.45f, 6, 0x4A4E54, 1.0f);
    for (int y = 0; y < (int)(h * 0.62f); y++)
        for (int x = 2; x < (int)w + 2; x++) {
            float d = mr_rrect_sd(x + 0.5f, y + 0.5f, 2, 0, w, h * 0.62f, 8);
            float c = mr_clampf(0.5f - d, 0, 1);
            if (c <= 0) continue;
            float g = 0.85f + 0.12f * sinf(y * 0.9f + mr_vnoise(x * 0.03f, y * 0.3f, 3) * 5);
            float k = g * (1.05f - 0.25f * (float)y / (h * 0.62f));
            mr_img_over(im, x, y, mr_rgb_scale(0xC89A60, k), c);
        }
    /* chalk caught in the felt */
    for (int i = 0; i < 300; i++) {
        uint32_t hs = mr_hash((uint32_t)i * 977u);
        mr_img_over(im, 4 + (int)(hs % (uint32_t)(w - 4)), (int)(h * 0.66f) + (int)((hs >> 10) % (uint32_t)(h * 0.3f)), 0xE0E2DA, 0.5f);
    }
    mr_img_shadow(im, 4, 6, 4, 0.5f);
}

/* --- marks as strokes ------------------------------------------------------------------------- */

static float jit(uint32_t seed, int k)
{
    return (float)(mr_hash(seed * 31u + (uint32_t)k * 7919u) & 1023) / 1023.0f - 0.5f;
}

static void line(path_t *P, float ax, float ay, float bx, float by, float bend, uint32_t seed)
{
    if (P->n >= MAXPATH) return;
    mr_stroke_t *st = &P->s[P->n++];
    st->n = 0;
    float nx = -(by - ay), ny = bx - ax, L = sqrtf(nx * nx + ny * ny) + 1e-3f;
    nx /= L, ny /= L;
    for (int i = 0; i <= 8; i++) {
        float t = i / 8.0f, b = sinf(t * 3.14159f) * bend + jit(seed, i) * 0.8f;
        st->x[st->n] = ax + (bx - ax) * t + nx * b;
        st->y[st->n] = ay + (by - ay) * t + ny * b;
        st->n++;
    }
}

static void tally_path(tiza_t *T, int p, int k, path_t *P)
{
    P->n = 0;
    int g = k / 5, m = k % 5;
    float gx = T->c[p].x + 24 + (g % T->gpr) * T->gw, gy = T->c[p].y0 + (g / T->gpr) * T->gh;
    float sp = (T->gw - 26) / 4.0f, h = T->gh * 0.72f;
    uint32_t sd = (uint32_t)(p * 1000 + k + 17);
    if (m < 4) {
        float x = gx + m * sp + jit(sd, 1) * 4;
        line(P, x + jit(sd, 2) * 6, gy + jit(sd, 3) * 6, x + jit(sd, 4) * 8, gy + h + jit(sd, 5) * 6, jit(sd, 6) * 5, sd);
    } else {
        line(P, gx - 10 + jit(sd, 7) * 6, gy + h * 0.82f + jit(sd, 8) * 6, gx + 3 * sp + 12 + jit(sd, 9) * 6,
             gy + h * 0.14f + jit(sd, 10) * 6, jit(sd, 11) * 6, sd);
    }
}

/* a number as strokes, hand-wobbled, centred at cx on a row */
static void number_path(tiza_t *T, const char *txt, float cx, float y, float h, uint32_t seed, path_t *P, bool strike,
                        float *x0o, float *x1o)
{
    P->n = 0;
    float tw = mr_text_w(txt, h), x = cx - tw * 0.5f;
    if (x0o) *x0o = x;
    if (x1o) *x1o = x + tw;
    int ci = 0;
    for (const char *c = txt; *c && P->n < MAXPATH - 1; c++, ci++) {
        const mr_stroke_t *gs;
        int ns = mr_glyph(*c, &gs);
        float adv = (*c == '1' ? 0.56f : 0.70f) * h, gw = 0.56f * h;
        float gx = x + (adv - gw) * 0.5f, slant = 0.10f * h;
        float sx = 1.0f + jit(seed, ci) * 0.10f, dy = jit(seed, ci + 9) * h * 0.06f;
        for (int i = 0; i < ns && P->n < MAXPATH - 1; i++) {
            mr_stroke_t *st = &P->s[P->n++];
            st->n = 0;
            for (int k = 0; k < gs[i].n; k++) {
                float u = gs[i].x[k], v = gs[i].y[k];
                st->x[st->n] = gx + u * gw * sx + (1 - v) * slant + jit(seed, ci * 50 + k) * h * 0.03f;
                st->y[st->n] = y + dy + v * h + jit(seed, ci * 70 + k) * h * 0.03f;
                st->n++;
            }
        }
        x += adv;
    }
    (void)T;
    (void)strike;
}

/* every stroke of mark k of player p (and the strike through the one before it) */
static void mark_path(mr_scene_t *s, int p, int k, path_t *P, bool with_strike_of_prev)
{
    tiza_t *T = ST;
    if (!T->totals) {
        tally_path(T, p, k, P);
        return;
    }
    char txt[12];
    snprintf(txt, sizeof txt, "%ld", (long)T->c[p].val[k]);
    float h = T->rowh * 0.62f;
    float cy = T->c[p].y0 + k * T->rowh;
    number_path(T, txt, T->c[p].x + T->c[p].w * 0.5f, cy, h, (uint32_t)(p * 7777 + k * 13), P, false, NULL, NULL);
    if (with_strike_of_prev && k > 0 && P->n < MAXPATH) {
        char pt[12];
        snprintf(pt, sizeof pt, "%ld", (long)T->c[p].val[k - 1]);
        float x0, x1;
        path_t tmp;
        float py = T->c[p].y0 + (k - 1) * T->rowh;
        number_path(T, pt, T->c[p].x + T->c[p].w * 0.5f, py, h, 1, &tmp, false, &x0, &x1);
        /* the strike goes first: shift the number's strokes along */
        memmove(&P->s[1], &P->s[0], sizeof(mr_stroke_t) * (MAXPATH - 1));
        if (P->n < MAXPATH) P->n++;
        path_t one;
        one.n = 0;
        uint32_t sd = (uint32_t)(p * 31 + k);
        line(&one, x0 - 12, py + h * 0.58f + jit(sd, 1) * 6, x1 + 12, py + h * 0.42f + jit(sd, 2) * 6, jit(sd, 3) * 4, sd);
        P->s[0] = one.s[0];
    }
}

/* --- chalk ----------------------------------------------------------------------------------------- */

static void stamp(tiza_t *T, float x, float y, float pressure, int *bx0, int *by0, int *bx1, int *by1)
{
    float r = T->lw * 0.5f;
    int x0 = (int)(x - r - 1), x1 = (int)(x + r + 2), y0 = (int)(y - r - 1), y1 = (int)(y + r + 2);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            if ((unsigned)px >= (unsigned)T->bg.w || (unsigned)py >= (unsigned)T->bg.h) continue;
            float d = sqrtf((px + 0.5f - x) * (px + 0.5f - x) + (py + 0.5f - y) * (py + 0.5f - y));
            float c = mr_clampf(r + 0.5f - d, 0, 1) * pressure;
            int v = (int)(c * 255);
            uint8_t *a = &T->chalk[py * T->bg.w + px];
            if (v > *a) *a = (uint8_t)v;
        }
    if (x0 < *bx0) *bx0 = x0;
    if (y0 < *by0) *by0 = y0;
    if (x1 > *bx1) *bx1 = x1;
    if (y1 > *by1) *by1 = y1;
}

static float path_len(const path_t *P)
{
    float L = 0;
    for (int i = 0; i < P->n; i++)
        for (int k = 0; k + 1 < P->s[i].n; k++) {
            float dx = P->s[i].x[k + 1] - P->s[i].x[k], dy = P->s[i].y[k + 1] - P->s[i].y[k];
            L += sqrtf(dx * dx + dy * dy);
        }
    return L;
}

/* stamp the path from distance a to b; a pen lift between strokes is free */
static void draw_range(mr_scene_t *s, const path_t *P, float a, float b)
{
    tiza_t *T = ST;
    int bx0 = 1 << 20, by0 = 1 << 20, bx1 = -1, by1 = -1;
    float L = 0;
    for (int i = 0; i < P->n; i++)
        for (int k = 0; k + 1 < P->s[i].n; k++) {
            float ax = P->s[i].x[k], ay = P->s[i].y[k], cx = P->s[i].x[k + 1], cy = P->s[i].y[k + 1];
            float dx = cx - ax, dy = cy - ay, sl = sqrtf(dx * dx + dy * dy);
            float s0 = L, s1 = L + sl;
            L = s1;
            if (s1 < a || s0 > b || sl < 0.01f) continue;
            float t0 = mr_clampf((a - s0) / sl, 0, 1), t1 = mr_clampf((b - s0) / sl, 0, 1);
            for (float t = t0; t <= t1; t += 1.2f / sl) {
                float pr = 0.80f + 0.2f * sinf((s0 + t * sl) * 0.05f + i);
                stamp(T, ax + dx * t, ay + dy * t, pr, &bx0, &by0, &bx1, &by1);
            }
        }
    if (bx1 >= 0) {
        compose(T, bx0, by0, bx1, by1);
        invalidate(s, bx0, by0, bx1, by1);
    }
}

static void redraw_all(mr_scene_t *s)
{
    tiza_t *T = ST;
    memset(T->chalk, 0, (size_t)T->bg.w * T->bg.h);
    for (int p = 0; p < T->np; p++)
        for (int k = 0; k < T->c[p].drawn; k++) {
            path_t P;
            mark_path(s, p, k, &P, true);
            /* all at once: no compose per stroke */
            int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
            for (int i = 0; i < P.n; i++)
                for (int j = 0; j + 1 < P.s[i].n; j++) {
                    float ax = P.s[i].x[j], ay = P.s[i].y[j], dx = P.s[i].x[j + 1] - ax, dy = P.s[i].y[j + 1] - ay;
                    float sl = sqrtf(dx * dx + dy * dy);
                    for (float t = 0; t <= 1; t += 1.2f / (sl + 0.01f))
                        stamp(T, ax + dx * t, ay + dy * t, 0.80f + 0.2f * sinf(t * 3 + i), &bx0, &by0, &bx1, &by1);
                }
        }
    compose(T, 0, 0, T->bg.w, T->bg.h);
    lv_obj_invalidate(T->cvo);
}

/* the names: LVGL's text, turned into chalk */
static void names_draw(mr_scene_t *s)
{
    tiza_t *T = ST;
    for (int p = 0; p < T->np; p++) {
        int tw = T->c[p].w - 20, th = 56;
        uint32_t *buf = (uint32_t *)mr_alloc((size_t)tw * th * 4);
        if (!buf) continue;
        lv_obj_t *cv = lv_canvas_create(s->obj);
        lv_canvas_set_buffer(cv, buf, tw, th, LV_COLOR_FORMAT_ARGB8888);
        lv_canvas_fill_bg(cv, lv_color_hex(0), LV_OPA_TRANSP);
        lv_layer_t layer;
        lv_canvas_init_layer(cv, &layer);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = aos_font_title;
        d.color = lv_color_hex(0xFFFFFF);
        d.text = s->g->p[p].name;
        d.align = LV_TEXT_ALIGN_CENTER;
        d.letter_space = 2;
        lv_area_t a = { 0, 4, tw - 1, th - 1 };
        lv_draw_label(&layer, &d, &a);
        lv_canvas_finish_layer(cv, &layer);
        int ox = T->c[p].x + 10, oy = s->top + 22;
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++) {
                int al = (int)(buf[y * tw + x] >> 24);
                /* a slight slant, as written by hand */
                int X = ox + x, Y = oy + y + (int)((x - tw * 0.5f) * -0.02f);
                if (al && (unsigned)X < (unsigned)T->bg.w && (unsigned)Y < (unsigned)T->bg.h) T->names[Y * T->bg.w + X] = (uint8_t)(al * 0.95f);
            }
        lv_obj_delete(cv);
        mr_free(buf);
        /* the underline, in chalk too */
        path_t P;
        P.n = 0;
        line(&P, T->c[p].x + 30, s->top + 86, T->c[p].x + T->c[p].w - 30, s->top + 84, 3, (uint32_t)p + 5);
        for (int j = 0; j + 1 < P.s[0].n; j++) {
            float ax = P.s[0].x[j], ay = P.s[0].y[j], dx = P.s[0].x[j + 1] - ax, dy = P.s[0].y[j + 1] - ay;
            float sl = sqrtf(dx * dx + dy * dy);
            for (float t = 0; t <= 1; t += 1.0f / (sl + 0.01f)) {
                int X = (int)(ax + dx * t), Y = (int)(ay + dy * t);
                for (int k = -1; k <= 1; k++)
                    if ((unsigned)(Y + k) < (unsigned)T->bg.h) T->names[(Y + k) * T->bg.w + X] = 210;
            }
        }
        /* a dash of the player's colour, as coloured chalk */
        uint32_t col = s->g->p[p].color;
        for (int y = s->top + 92; y < s->top + 98; y++)
            for (int x = T->c[p].x + T->c[p].w / 2 - 26; x < T->c[p].x + T->c[p].w / 2 + 26; x++)
                if (grain(x, y) > 0.4f) mr_cv_mix(&T->base, x, y, col, 0.75f);
    }
    /* a line between columns */
    for (int p = 1; p < T->np; p++) {
        path_t P;
        P.n = 0;
        line(&P, T->c[p].x, s->top + 26, T->c[p].x + 3, T->bg.h - 130, 4, (uint32_t)p * 3);
        for (int j = 0; j + 1 < P.s[0].n; j++) {
            float ax = P.s[0].x[j], ay = P.s[0].y[j], dx = P.s[0].x[j + 1] - ax, dy = P.s[0].y[j + 1] - ay;
            float sl = sqrtf(dx * dx + dy * dy);
            for (float t = 0; t <= 1; t += 1.0f / (sl + 0.01f)) {
                int X = (int)(ax + dx * t), Y = (int)(ay + dy * t);
                for (int k = -1; k <= 1; k++)
                    if ((unsigned)(X + k) < (unsigned)T->bg.w && (unsigned)Y < (unsigned)T->bg.h) T->names[Y * T->bg.w + X + k] = 150;
            }
        }
    }
}

/* --- build ---------------------------------------------------------------------------------------- */

static void build(mr_scene_t *s)
{
    tiza_t *T = (tiza_t *)mr_alloc(sizeof(tiza_t));
    s->st = T;
    if (!T) return;
    mr_game_t *g = s->g;
    T->np = g->np;
    T->totals = g->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS;
    int fr = 26, ledge_y = s->h - 70;
    int cw = (s->w - 2 * fr) / T->np;
    T->lw = 7.0f;
    for (int p = 0; p < T->np; p++) {
        T->c[p].x = fr + p * cw;
        T->c[p].w = cw;
        T->c[p].y0 = s->top + 124;
    }
    float area = ledge_y - (s->top + 124) - 20;
    if (T->totals) {
        T->rowh = mr_minf(84.0f, mr_maxf(54.0f, cw / 4.2f));
        int rows = (int)(area / T->rowh);
        for (int p = 0; p < T->np; p++) T->c[p].cap = rows;
    } else {
        T->gw = mr_minf(150.0f, (cw - 30) / 2.0f);
        T->gpr = (int)((cw - 30) / T->gw);
        if (T->gpr < 1) T->gpr = 1;
        T->gh = mr_minf(140.0f, T->gw * 0.95f);
        int rows = (int)(area / T->gh);
        for (int p = 0; p < T->np; p++) T->c[p].cap = mr_minf(MAXMK, (float)(rows * T->gpr * 5));
        T->lw = mr_clampf(T->gw / 14.0f, 5, 10);
    }
    size_t n = (size_t)s->w * s->h;
    if (!mr_cv_new(&T->bg, s->w, s->h) || !mr_cv_new(&T->base, s->w, s->h)) return;
    T->chalk = (uint8_t *)mr_alloc(n);
    T->names = (uint8_t *)mr_alloc(n);
    T->dust = (uint8_t *)mr_alloc(n);
    if (!T->chalk || !T->names || !T->dust) return;
    slate_draw(s, ledge_y);
    T->cvo = mr_cv_obj(s->obj, &T->bg);
    names_draw(s);
    compose(T, 0, 0, s->w, s->h);
    /* the chalk on the ledge */
    lv_obj_t *stick = lv_obj_create(s->obj);
    mr_obj_plain(stick);
    lv_obj_set_size(stick, 74, 16);
    lv_obj_set_style_radius(stick, 8, 0);
    lv_obj_set_style_bg_color(stick, lv_color_hex(0xF4F4EE), 0);
    lv_obj_set_style_bg_grad_color(stick, lv_color_hex(0xB8BAB2), 0);
    lv_obj_set_style_bg_grad_dir(stick, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(stick, 255, 0);
    lv_obj_set_style_shadow_width(stick, 8, 0);
    lv_obj_set_style_shadow_offset_y(stick, 4, 0);
    lv_obj_set_style_shadow_opa(stick, 140, 0);
    lv_obj_set_pos(stick, 70, ledge_y + 18);
    lv_obj_set_style_transform_rotation(stick, -40, 0);
    mr_img_new(&T->eraser, 170, 74);
    eraser_draw(&T->eraser);
    T->ero = mr_img_obj(s->obj, &T->eraser);
    lv_obj_set_pos(T->ero, s->w - 230, ledge_y - 8);
    lv_image_set_pivot(T->ero, T->eraser.w / 2, T->eraser.h / 2);
    for (int p = 0; p < T->np; p++) {
        if (T->totals) continue;
        int cx = T->c[p].x + T->c[p].w / 2;
        int y = ledge_y - 78;
        mr_rbtn(s, cx - 104, y, 96, 62, LV_SYMBOL_MINUS, 0x9024322A, 0xEEF0E8, tz_btn, -(p + 1));
        mr_rbtn(s, cx + 8, y, 96, 62, LV_SYMBOL_PLUS, 0x9024322A, 0xEEF0E8, tz_btn, p + 1);
    }
    for (int i = 0; i < MR_FINGERS; i++) T->f[i].p = -1;
}

static void sfree(mr_scene_t *s)
{
    tiza_t *T = ST;
    if (!T) return;
    mr_cv_free(&T->bg);
    mr_cv_free(&T->base);
    mr_free(T->chalk);
    mr_free(T->names);
    mr_free(T->dust);
    mr_img_free(&T->eraser);
}

/* --- following the model --------------------------------------------------------------------------- */

/* what each column should show: a count of tally marks, or the totals */
static int wanted(mr_scene_t *s, int p, int32_t *vals)
{
    tiza_t *T = ST;
    mr_game_t *g = s->g;
    if (!T->totals) {
        int n = g->p[p].score;
        if (n < 0) n = 0;
        return n > T->c[p].cap ? T->c[p].cap : n;
    }
    /* the totals since the last reset: the start, then after each score */
    int first = 0;
    for (int k = 0; k < g->nhist; k++)
        if (g->hist[k].kind == MR_H_RESET) first = k + 1;
    int n = 0;
    int32_t start = g->opt[MR_TIZA_SUB] ? g->opt[MR_TIZA_START] : 0;
    int32_t tmp[MR_HIST / 2 + 2];
    int m = 0;
    if (g->opt[MR_TIZA_SUB]) tmp[m++] = start;
    for (int k = first; k < g->nhist && m < (int)(sizeof tmp / sizeof tmp[0]); k++) {
        if (g->hist[k].who != p || g->hist[k].kind != MR_H_POINTS) continue;
        int32_t after = k + 1 < g->nhist ? g->hist[k + 1].before[p] : g->p[p].score;
        tmp[m++] = after;
    }
    /* only the last ones fit */
    int cap = T->c[p].cap;
    int from = m > cap ? m - cap : 0;
    for (int k = from; k < m; k++) vals[n++] = tmp[k];
    return n;
}

static void push_job(tiza_t *T, int kind, int p, int from)
{
    if (T->nq >= JOBS) return;
    T->q[T->nq].kind = kind;
    T->q[T->nq].p = p;
    T->q[T->nq].from = from;
    T->nq++;
}

static void erase_apply(mr_scene_t *s, int p, int from);

/* the job in hand is finished at once (the model moved on under it) */
static void finish_current(mr_scene_t *s)
{
    tiza_t *T = ST;
    if (!T->nq) return;
    job_t *j = &T->q[0];
    if (j->kind == 1 && T->done > 0) {
        draw_range(s, &T->cur, T->dist, T->done + 1);
        T->c[j->p].drawn = j->from + 1;
    } else if (j->kind == 2 && T->ex1 > 0) {
        if (!T->erased) erase_apply(s, j->p, j->from);
        lv_obj_set_pos(T->ero, s->w - 230, s->h - 70 - 8);
        lv_image_set_rotation(T->ero, 0);
    }
    T->dist = T->done = 0;
    T->et = 0;
    T->erased = false;
    T->ex1 = 0;
}

static void sync(mr_scene_t *s, int why, int who)
{
    tiza_t *T = ST;
    if (!T || !T->chalk) return;
    (void)who;
    /* what is queued is dropped: the board catches up from what is drawn */
    finish_current(s);
    T->nq = 0;
    for (int p = 0; p < T->np; p++) {
        int32_t vals[MAXMK];
        int n = wanted(s, p, vals);
        if (why == MR_SYNC_LOAD) {
            memcpy(T->c[p].val, vals, sizeof(int32_t) * (size_t)n);
            T->c[p].drawn = n;
            continue;
        }
        /* the first mark that differs */
        int same = 0;
        if (T->totals) {
            while (same < n && same < T->c[p].drawn && vals[same] == T->c[p].val[same]) same++;
        } else {
            same = n < T->c[p].drawn ? n : T->c[p].drawn;
        }
        if (same < T->c[p].drawn) push_job(T, 2, p, same);
        memcpy(T->c[p].val, vals, sizeof(int32_t) * (size_t)n);
        for (int k = same; k < n; k++) push_job(T, 1, p, k);
    }
    if (why == MR_SYNC_LOAD) redraw_all(s);
}

static void job_next(mr_scene_t *s)
{
    tiza_t *T = ST;
    while (T->nq) {
        job_t *j = &T->q[0];
        if (j->kind == 1) {
            /* write mark j->from, if the column is up to it */
            if (T->c[j->p].drawn != j->from) {
                memmove(T->q, T->q + 1, sizeof(job_t) * (size_t)--T->nq);
                continue;
            }
            mark_path(s, j->p, j->from, &T->cur, T->totals);
            T->dist = 0;
            T->done = path_len(&T->cur);
            mr_snd_play(MR_S_CHALK_TAP, 0.7f, 0.9f + 0.2f * mr_hashf(j->from, j->p, 1));
            return;
        }
        /* erase from j->from on: the eraser's path over those marks */
        int p = j->p, from = j->from;
        if (from >= T->c[p].drawn) {
            memmove(T->q, T->q + 1, sizeof(job_t) * (size_t)--T->nq);
            continue;
        }
        int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
        for (int k = (T->totals && from > 0) ? from - 1 : from; k < T->c[p].drawn; k++) {
            path_t P;
            mark_path(s, p, k, &P, false);
            for (int i = 0; i < P.n; i++)
                for (int q = 0; q < P.s[i].n; q++) {
                    if (P.s[i].x[q] < x0) x0 = (int)P.s[i].x[q];
                    if (P.s[i].x[q] > x1) x1 = (int)P.s[i].x[q];
                    if (P.s[i].y[q] < y0) y0 = (int)P.s[i].y[q];
                    if (P.s[i].y[q] > y1) y1 = (int)P.s[i].y[q];
                }
        }
        T->ex0 = x0 - 16;
        T->ey0 = y0 - 16;
        T->ex1 = x1 + 16;
        T->ey1 = y1 + 16;
        T->et = 0;
        T->erased = false;
        mr_snd_play(MR_S_ERASE, 0.8f, 1.0f);
        return;
    }
}

static void frame(mr_scene_t *s, float dt)
{
    tiza_t *T = ST;
    if (!T || !T->chalk) return;
    if (!T->nq) return;
    job_t *j = &T->q[0];
    if (j->kind == 1) {
        if (T->done <= 0 && T->dist == 0) job_next(s);
        if (!T->nq || T->q[0].kind != 1) return;
        float a = T->dist;
        T->dist += SPEED * dt;
        draw_range(s, &T->cur, a, T->dist);
        uint32_t now = mr_ms();
        if (now - T->squeak > 240) {
            T->squeak = now;
            mr_snd_play(MR_S_CHALK, 0.55f, 0.9f + 0.25f * mr_hashf((int)now, 3, 2));
        }
        if (T->dist >= T->done) {
            T->c[j->p].drawn = j->from + 1;
            memmove(T->q, T->q + 1, sizeof(job_t) * (size_t)--T->nq);
            T->dist = 0;
            T->done = 0;
            if (T->nq) job_next(s);
        }
        return;
    }
    /* the eraser at work */
    if (T->et == 0 && !T->erased && T->ex1 <= 0) job_next(s);
    T->et += dt / ERASE_S;
    float t = mr_clampf(T->et, 0, 1);
    float cx = (T->ex0 + T->ex1) * 0.5f, w = (float)(T->ex1 - T->ex0);
    float x = cx + sinf(t * 3.14159f * 5) * w * 0.40f;
    float y = T->ey0 + (T->ey1 - T->ey0) * t;
    lv_obj_set_pos(T->ero, (int)(x - T->eraser.w * 0.5f), (int)(y - T->eraser.h * 0.5f));
    lv_image_set_rotation(T->ero, (int)(sinf(t * 18) * 60));
    if (t > 0.6f && !T->erased) erase_apply(s, j->p, j->from);
    if (T->et >= 1) {
        lv_obj_set_pos(T->ero, s->w - 230, s->h - 70 - 8);
        lv_image_set_rotation(T->ero, 0);
        memmove(T->q, T->q + 1, sizeof(job_t) * (size_t)--T->nq);
        T->et = 0;
        T->erased = false;
        T->ex1 = 0;
        if (T->nq) job_next(s);
    }
}

static void erase_apply(mr_scene_t *s, int p, int from)
{
    tiza_t *T = ST;
    T->erased = true;
    T->c[p].drawn = from;
    /* the haze it leaves */
    for (int yy = T->ey0; yy < T->ey1; yy++)
        for (int xx = T->ex0; xx < T->ex1; xx++) {
            if ((unsigned)xx >= (unsigned)T->bg.w || (unsigned)yy >= (unsigned)T->bg.h) continue;
            int i = yy * T->bg.w + xx;
            int d = T->chalk[i] / 3 + (int)(mr_vnoise(xx * 0.05f, yy * 0.2f, 9) * 40);
            if (d > T->dust[i]) T->dust[i] = (uint8_t)(d > 255 ? 255 : d);
        }
    redraw_all(s);
}

/* --- fingers ---------------------------------------------------------------------------------------- */

static void tz_btn(mr_scene_t *s, int arg)
{
    if (arg > 0) mr_add(s->g, arg - 1, 1);
    else mr_add(s->g, -arg - 1, -1);
}

static int col_at(tiza_t *T, float x)
{
    for (int p = 0; p < T->np; p++)
        if (x >= T->c[p].x && x < T->c[p].x + T->c[p].w) return p;
    return -1;
}

static void down(mr_scene_t *s, int f, float x, float y)
{
    tiza_t *T = ST;
    if (!T) return;
    T->f[f].p = y < s->h - 70 ? col_at(T, x) : -1;
    T->f[f].held = false;
}

static void tick(mr_scene_t *s)
{
    tiza_t *T = ST;
    if (!T) return;
    for (int f = 0; f < MR_FINGERS; f++) {
        mr_finger_t *F = &mr_A->fing[f];
        if (!F->id || T->f[f].p < 0 || T->f[f].held || F->moved) continue;
        if (mr_ms() - F->t0 > HOLD_MS) {
            T->f[f].held = true;
            int p = T->f[f].p;
            if (T->totals) {
                /* the last total of that column goes, if it is the last thing written */
                mr_game_t *g = s->g;
                if (g->nhist && g->hist[g->nhist - 1].who == p) mr_undo(g);
            } else {
                mr_add(s->g, p, -1);
            }
        }
    }
}

static void up(mr_scene_t *s, int f, float x, float y)
{
    tiza_t *T = ST;
    if (!T) return;
    int p = T->f[f].p;
    T->f[f].p = -1;
    if (p < 0 || T->f[f].held || x < -1000 || mr_A->fing[f].moved || col_at(T, x) != p) return;
    (void)y;
    if (T->totals) mr_ui_keypad(p);
    else mr_add(s->g, p, 1);
}

static void fr(mr_scene_t *s, float dt)
{
    frame(s, dt);
    tick(s);
}

const mr_scene_ops_t mr_tiza_ops = { build, sfree, sync, fr, down, NULL, up, NULL };
