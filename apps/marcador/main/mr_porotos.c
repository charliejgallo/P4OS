/*
 * Marcador - beans on a wooden table.
 *
 * The way the score is kept in a kitchen: a heap of dry beans in the middle
 * and each player pulls his own in front of him. Here the table is planks
 * of oiled wood (rings that wander, the odd knot, the joints between
 * boards, a warm lamp over the middle), every player has a little woven
 * mat of his colour, and the count of each one is simply how many beans are
 * lying on his mat.
 *
 * Every bean is an object with a position and a velocity. A finger picks up
 * the bean under it (it rises: bigger, its shadow further away) and drags
 * it; let go and it falls with a tick, sliding on with the speed it had,
 * slowing on the wood and knocking the beans it meets, which slide in turn.
 * When everything is still, the beans on each mat are counted and every
 * change goes to the model as a move (from the heap, from another player,
 * back to the heap), so undo puts the bean back where it came from.
 *
 * Tapping a mat brings a bean from the heap by itself, holding it gives one
 * back; the buttons do the same. Anything the model changes (undo, the
 * keypad) is matched by beans hopping between heap and mats.
 */
#include "mr_app.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXB     170
#define ROTS     12
#define BTYPES   3
#define BEAN_L   44.0f      /* bean length, px                               */
#define BEAN_W   28.0f
#define BR       14.5f      /* collision radius                              */
#define LIFT     30.0f      /* a held bean rides above the finger            */
#define HOLD_MS  520
#define HOP_S    0.36f

typedef struct {
    float     x, y, vx, vy;
    uint8_t   type, rot;
    int8_t    zone;         /* -1 heap, -2 loose on the table, else player   */
    int8_t    finger;       /* held by, or -1                               */
    bool      moving;
    bool      hop;          /* flying by itself to (hx, hy)                  */
    float     hx0, hy0, hx, hy, ht;
    int8_t    from;         /* where it was picked up                        */
    lv_obj_t *o;
} bean_t;

typedef struct {
    mr_cv_t  bg;
    mr_img_t rest[BTYPES][ROTS], air[BTYPES][ROTS], ash;
    int      types[2];      /* the kinds in play (mixed: two)                */
    int      nb;
    bean_t   b[MAXB];
    float    pcx, pcy, pr;  /* the heap                                     */
    int      np;
    struct {
        float     x, y, r;
        lv_obj_t *name, *count;
        int       shown;    /* the count last told to the model             */
    } m[4];
    struct {
        int      bean;      /* held bean, or -1                             */
        int      mat;       /* a tap or hold on a mat                       */
        bool     held;
        float    lx, ly, vx, vy;
        uint32_t lt;
    } f[MR_FINGERS];
    lv_obj_t *sh[MR_FINGERS];  /* the shadow of a held bean                 */
    uint32_t  seed;
    bool      settling;
} por_t;

#define ST ((por_t *)s->st)

static void por_btn(mr_scene_t *s, int arg);

/* --- the table ------------------------------------------------------------------------- */

static void table_draw(mr_scene_t *s)
{
    por_t *P = ST;
    mr_cv_t *cv = &P->bg;
    int w = cv->w, h = cv->h;
    bool vert = !s->land;           /* planks run along the long side */
    int along = vert ? h : w, across = vert ? w : h;
    int nplank = across / 170 + 1;
    float pw = (float)across / nplank;
    mr_field_t warp, fine;
    mr_field_new(&warp, w, h, 8, 1.0f / 160.0f, 3, 911);
    mr_field_new(&fine, w, h, 4, 1.0f / 22.0f, 2, 913);
    float lcx = w * 0.5f, lcy = h * 0.47f, lr = sqrtf((float)w * w + (float)h * h) * 0.55f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float a = vert ? (float)x : (float)y, l = vert ? (float)y : (float)x;
            int pi = (int)(a / pw);
            float pa = a - pi * pw;
            uint32_t ph = mr_hash((uint32_t)pi * 7919u + 13u);
            float shift = (float)(ph & 1023);
            float wv = mr_vnoise(l * 0.0016f + (float)pi * 3.7f, pa * 0.010f, 917) * 0.8f + mr_field_at(&warp, x, y) * 0.25f;
            float fn = mr_field_at(&fine, x, y);
            /* rings: a slow wave across the board, bent by the warp */
            /* growth rings cut lengthwise: long lines that drift slowly across */
            float ring = sinf((pa * 0.045f + wv * 2.4f + 0.35f * sinf((l + shift) * 0.0025f)) * 6.2831853f);
            float band = 0.5f + 0.5f * ring;
            band = band * band * band;
            /* a knot now and then */
            float kx = (float)((ph >> 10) % 1000) / 1000.0f * along, ky = pw * (0.3f + 0.4f * (float)((ph >> 20) & 15) / 15.0f);
            float dk = sqrtf((l - kx) * (l - kx) * 0.35f + (pa - ky) * (pa - ky));
            float knot = (ph & 0x30000) ? 0.0f : expf(-dk * dk / 260.0f);
            band = band * (1 - knot) + knot * 0.95f;
            float fibre = (mr_hashf((int)(l * 0.02f), (int)(pa * 1.5f), pi) - 0.5f) * 0.09f +
                          (mr_hashf((int)(l * 0.3f), (int)(pa * 3.0f), pi + 9) - 0.5f) * 0.05f;
            float k = 0.78f + 0.20f * band - 0.12f * knot + fibre + (fn - 0.5f) * 0.10f;
            k *= 0.90f + 0.14f * (float)((ph >> 4) & 15) / 15.0f;
            uint32_t base = mr_rgb_mix(0x9B6638, 0x7A4A28, (float)((ph >> 8) & 7) / 7.0f);
            /* the lamp over the middle */
            float dx = x - lcx, dy = y - lcy;
            float light = 1.10f - 0.62f * mr_smooth(0.0f, 1.0f, sqrtf(dx * dx + dy * dy) / lr);
            uint32_t c = mr_rgb_scale(base, k * light);
            /* the joint between planks */
            if (pa < 2.5f) c = mr_rgb_scale(c, 0.35f + 0.2f * pa);
            else if (pa < 4.0f) c = mr_rgb_scale(c, 1.12f);
            cv->px[y * w + x] = mr_565(c);
        }
    }
    mr_field_free(&warp);
    mr_field_free(&fine);
}

/* a woven mat: two sets of threads over and under, a fringe at the edge */
static void mat_draw(mr_cv_t *cv, float cx, float cy, float r, uint32_t col)
{
    uint32_t base = mr_rgb_mix(0xE9DCC2, col, 0.45f);
    for (int y = (int)(cy - r - 14); y <= (int)(cy + r + 14); y++) {
        for (int x = (int)(cx - r - 14); x <= (int)(cx + r + 14); x++) {
            float dx = x - cx, dy = y - cy, d = sqrtf(dx * dx + dy * dy);
            /* its shadow on the wood */
            if (d < r + 10 && d > r - 6) mr_cv_mix(cv, x + 3, y + 4, 0x000000, 0.18f * (1 - mr_smooth(r - 6, r + 10, d)));
            if (d > r + 9) continue;
            if (d > r) {
                /* the fringe: loose threads pointing out */
                float ang = atan2f(dy, dx);
                float th = fabsf(sinf(ang * r * 0.5f));
                if (th < 0.45f) mr_cv_mix(cv, x, y, mr_rgb_scale(base, 0.85f), 0.85f * (1 - (d - r) / 9.0f));
                continue;
            }
            int tx = (x + 2000) / 6, ty = (y + 2000) / 6;
            bool over = ((tx + ty) & 1) != 0;
            float fx = (float)((x + 2000) % 6) / 6.0f, fy = (float)((y + 2000) % 6) / 6.0f;
            float thread = over ? sinf(fy * 3.14159f) : sinf(fx * 3.14159f);
            float k = 0.72f + 0.30f * thread + (mr_hashf(x, y, 3) - 0.5f) * 0.08f;
            if (d > r - 7) k *= 0.82f;     /* the hem */
            mr_cv_mix(cv, x, y, mr_rgb_scale(base, k), mr_clampf(r + 0.5f - d, 0, 1));
        }
    }
}

/* --- beans ------------------------------------------------------------------------------ */

static void bean_draw(mr_img_t *im, int type, float rot, uint32_t seed, bool shadow)
{
    float ca = cosf(rot), sa = sinf(rot);
    float cx = im->w * 0.5f, cy = im->h * 0.5f;
    float a = BEAN_L * 0.5f, b = BEAN_W * 0.5f;
    const float lx = -0.45f, ly = -0.6f, lz = 0.66f;
    uint32_t base = type == 0 ? 0xF1E9D6 : type == 1 ? 0xD8B48C : 0x23201F;
    float gloss = type == 2 ? 0.9f : 0.45f;
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            float u = px * ca + py * sa, v = -px * sa + py * ca;
            /* a kidney: an ellipse with a dent in the middle of one side */
            float d = sqrtf((u * u) / (a * a) + (v * v) / (b * b));
            float dent = expf(-(u * u) / (a * a * 0.12f)) * 0.22f;
            float dd = sqrtf((u * u) / (a * a) + ((v - b * dent * 1.2f) * (v - b * dent * 1.2f)) / (b * b));
            if (v > 0) d = dd * (1.0f + dent * 0.4f);
            if (d > 1.06f) continue;
            float cov = mr_clampf((1.0f - d) * b + 0.5f, 0, 1);
            float hz = sqrtf(mr_clampf(1 - d * d, 0, 1));
            float nu = u / (a * a) * 2.0f, nv = v / (b * b) * 2.0f;
            float nx = nu * ca - nv * sa, ny = nu * sa + nv * ca, nz = hz * 0.12f + 0.02f;
            float ln = sqrtf(nx * nx + ny * ny + nz * nz);
            nx /= ln, ny /= ln, nz /= ln;
            float dif = mr_clampf(nx * lx + ny * ly + nz * lz, 0, 1);
            float hx = lx, hy = ly, hzz = lz + 1.0f;
            float hl = sqrtf(hx * hx + hy * hy + hzz * hzz);
            float spec = powf(mr_clampf((nx * hx + ny * hy + nz * hzz) / hl, 0, 1), 28.0f) * gloss;
            uint32_t c = base;
            if (type == 1) {
                /* pinto: a cream ground marbled with brown */
                float m = mr_vnoise(u * 0.32f + 3.1f * (float)(seed & 7), v * 0.55f, seed);
                float m2 = mr_vnoise(u * 0.9f, v * 1.3f, seed + 5);
                c = mr_rgb_mix(0xE6D2B4, 0x7B4A2E, mr_smooth(0.45f, 0.62f, m * 0.7f + m2 * 0.4f));
            }
            /* the hilum: the little eye in the dent */
            float he = ((u * u) / 16.0f + ((v - b * 0.62f) * (v - b * 0.62f)) / 5.0f);
            if (v > 0 && he < 1.0f) c = type == 2 ? 0xEDE6DA : mr_rgb_scale(base, type == 0 ? 0.86f : 0.7f);
            float l = 0.38f + 0.78f * dif;
            uint32_t col = mr_rgb_scale(c, l);
            col = mr_rgb_mix(col, 0xFFFFFF, spec);
            mr_img_over(im, x, y, col, cov);
        }
    }
    if (shadow) mr_img_shadow(im, 3, 4, 3, 0.55f);
}

static void make_sprites(por_t *P)
{
    int sz = (int)(BEAN_L + 16);
    for (int t = 0; t < BTYPES; t++) {
        for (int r = 0; r < ROTS; r++) {
            float rot = (float)r / ROTS * 3.14159265f * 2.0f;
            mr_img_new(&P->rest[t][r], sz, sz);
            bean_draw(&P->rest[t][r], t, rot, 31u + (uint32_t)r * 17u, true);
            mr_img_new(&P->air[t][r], sz, sz);
            bean_draw(&P->air[t][r], t, rot, 31u + (uint32_t)r * 17u, false);
        }
    }
    mr_img_new(&P->ash, sz + 10, sz + 10);
    mr_img_disc(&P->ash, P->ash.w * 0.5f, P->ash.h * 0.5f, BEAN_W * 0.62f, 0x000000, 0.5f);
    mr_img_blur(&P->ash, 4);
}

static void bean_show(por_t *P, bean_t *b, float lift)
{
    mr_img_t *im = lift > 0.05f ? &P->air[b->type][b->rot] : &P->rest[b->type][b->rot];
    if (lv_image_get_src(b->o) != &im->dsc) lv_image_set_src(b->o, &im->dsc);
    if (lift > 0.05f) lv_image_set_scale(b->o, (uint32_t)(256 + lift * 52));
    else lv_image_set_scale(b->o, 256);
    lv_obj_set_pos(b->o, (int)(b->x - im->w * 0.5f), (int)(b->y - im->h * 0.5f - lift * 6));
}

static int zone_at(por_t *P, float x, float y)
{
    for (int p = 0; p < P->np; p++) {
        float dx = x - P->m[p].x, dy = y - P->m[p].y;
        if (dx * dx + dy * dy < (P->m[p].r - 4) * (P->m[p].r - 4)) return p;
    }
    float dx = x - P->pcx, dy = y - P->pcy;
    if (dx * dx + dy * dy < (P->pr + 20) * (P->pr + 20)) return -1;
    return -2;
}

/* a free spot inside a circle: the best of a few tries */
static void free_spot(por_t *P, float cx, float cy, float r, float *ox, float *oy, int skip)
{
    float best = -1, bx = cx, by = cy;
    for (int t = 0; t < 24; t++) {
        float a = mr_rnd(&P->seed) * 6.2831853f, d = sqrtf(mr_rnd(&P->seed)) * (r - BR * 1.4f);
        float x = cx + cosf(a) * d, y = cy + sinf(a) * d;
        float m = 1e9f;
        for (int i = 0; i < P->nb; i++) {
            if (i == skip) continue;
            bean_t *b = &P->b[i];
            float tx = b->hop ? b->hx : b->x, ty = b->hop ? b->hy : b->y;
            float dd = (tx - x) * (tx - x) + (ty - y) * (ty - y);
            if (dd < m) m = dd;
        }
        if (m > best) {
            best = m;
            bx = x;
            by = y;
        }
        if (best > (BR * 2.6f) * (BR * 2.6f)) break;
    }
    *ox = bx;
    *oy = by;
}

/* --- build -------------------------------------------------------------------------------- */

static void build(mr_scene_t *s)
{
    por_t *P = (por_t *)mr_alloc(sizeof(por_t));
    s->st = P;
    if (!P) return;
    mr_game_t *g = s->g;
    P->np = g->np;
    P->seed = 4242;
    int kind = g->opt[MR_POR_KIND];
    P->types[0] = kind == 3 ? 0 : kind;
    P->types[1] = kind == 3 ? 1 : kind;
    /* how many beans: enough for everybody to reach the target */
    int target = g->target > 0 ? g->target : 30;
    int total = g->opt[MR_POR_TOTAL] > 0 ? g->opt[MR_POR_TOTAL] : target * P->np;
    int need = 0;
    for (int p = 0; p < P->np; p++) need += g->p[p].score;
    if (total < need + 6) total = need + 6;
    if (total > MAXB) total = MAXB;
    P->nb = total;

    /* the mats round the heap: player 0 at the bottom, the others round */
    int top = s->top;
    float W = s->w, H = s->h - top;
    float cx = W * 0.5f, cy = top + H * 0.5f;
    float mr = sqrtf((float)target * 2300.0f / 3.14159f);
    if (mr < 95) mr = 95;
    static const float ANG2[2] = { 90, 270 }, ANG3[3] = { 90, 210, 330 }, ANG4[4] = { 90, 180, 270, 0 };
    const float *ang = P->np == 2 ? ANG2 : P->np == 3 ? ANG3 : ANG4;
    float ex = W * 0.5f, ey = H * 0.5f;
    float lim = P->np == 2 ? (s->land ? ex * 0.45f : ey * 0.42f) : mr_minf(ex, ey) * 0.50f;
    if (mr > lim) mr = lim;
    for (int p = 0; p < P->np; p++) {
        float a = ang[p] * 3.14159f / 180.0f;
        float rx = ex - mr - 34, ry = ey - mr - 78;
        if (P->np == 2 && s->land) {
            a = p ? 3.14159f : 0.0f;
        }
        P->m[p].x = cx + cosf(a) * rx;
        P->m[p].y = cy + sinf(a) * ry;
        P->m[p].r = mr;
    }
    if (P->np == 2 && s->land) {
        /* lying down: the two mats left and right, player 0 on the left */
        P->m[0].x = 34 + mr + 10;
        P->m[1].x = W - 34 - mr - 10;
        P->m[0].y = P->m[1].y = cy + 10;
    }
    P->pcx = cx;
    P->pcy = cy;
    P->pr = sqrtf((float)total * 640.0f / 3.14159f);
    float pmax = P->np == 2 ? (s->land ? (P->m[1].x - P->m[0].x) * 0.5f - mr - 20 : (P->m[0].y - P->m[1].y) * 0.5f - mr - 20)
                            : mr_minf(ex, ey) * 0.34f;
    if (P->pr > pmax) P->pr = pmax;
    if (P->pr < 50) P->pr = 50;

    if (!mr_cv_new(&P->bg, s->w, s->h)) return;
    table_draw(s);
    for (int p = 0; p < P->np; p++) mat_draw(&P->bg, P->m[p].x, P->m[p].y, P->m[p].r, g->p[p].color);
    mr_cv_obj(s->obj, &P->bg);
    make_sprites(P);

    /* the names, turned to face whoever sits on that side */
    for (int p = 0; p < P->np; p++) {
        float a = atan2f(P->m[p].y - cy, P->m[p].x - cx);
        int deg = (int)(a * 180.0f / 3.14159f);
        int rot = 0;
        float off = P->m[p].r + 40;
        float lx = P->m[p].x + cosf(a) * off, ly = P->m[p].y + sinf(a) * off;
        if (deg > 45 && deg < 135) rot = 0;
        else if (deg >= 135 || deg <= -135) rot = 900;
        else if (deg < -45) rot = 1800;
        else rot = 2700;
        if (P->np == 2 && s->land) {
            rot = 0;
            lx = P->m[p].x;
            ly = P->m[p].y + P->m[p].r + 40;
        }
        /* keep it on the table */
        if (ly > s->h - 34) ly = s->h - 34;
        P->m[p].name = mr_label(s->obj, aos_font_title, 0xFFF4E0, "");
        lv_label_set_text_fmt(P->m[p].name, "%s  %d", g->p[p].name, 0);
        lv_obj_set_style_text_font(P->m[p].name, aos_font_body, 0);
        lv_obj_set_style_bg_color(P->m[p].name, lv_color_hex(0x2A1A0E), 0);
        lv_obj_set_style_bg_opa(P->m[p].name, 150, 0);
        lv_obj_set_style_radius(P->m[p].name, 18, 0);
        lv_obj_set_style_pad_hor(P->m[p].name, 16, 0);
        lv_obj_set_style_pad_ver(P->m[p].name, 4, 0);
        lv_obj_set_style_border_color(P->m[p].name, lv_color_hex(g->p[p].color), 0);
        lv_obj_set_style_border_width(P->m[p].name, 3, 0);
        lv_obj_update_layout(P->m[p].name);
        int lw = lv_obj_get_width(P->m[p].name), lh = lv_obj_get_height(P->m[p].name);
        lv_obj_set_style_transform_pivot_x(P->m[p].name, lw / 2, 0);
        lv_obj_set_style_transform_pivot_y(P->m[p].name, lh / 2, 0);
        lv_obj_set_style_transform_rotation(P->m[p].name, rot, 0);
        if (ly < s->top + 30 && rot == 1800) ly = s->top + 30;
        lv_obj_set_pos(P->m[p].name, (int)(lx - lw * 0.5f), (int)(ly - lh * 0.5f));
    }
    /* the buttons, by each mat, on the side away from the heap */
    for (int p = 0; p < P->np; p++) {
        float a = atan2f(P->m[p].y - cy, P->m[p].x - cx) + 1.5708f;
        float bx = P->m[p].x + cosf(a) * (P->m[p].r + 52), by = P->m[p].y + sinf(a) * (P->m[p].r + 52);
        if (P->np == 2 && s->land) {
            bx = P->m[p].x + (p ? -1 : 1) * (P->m[p].r + 60);
            by = P->m[p].y - 70;
        }
        if (bx < 50) bx = 50;
        if (bx > s->w - 50) bx = s->w - 50;
        if (by < s->top + 40) by = s->top + 40;
        if (by > s->h - 100) by = s->h - 100;
        mr_rbtn(s, (int)bx - 34, (int)by - 70, 68, 60, LV_SYMBOL_PLUS, 0xB03A2412, 0xFFF4E0, por_btn, p + 1);
        mr_rbtn(s, (int)bx - 34, (int)by + 8, 68, 60, LV_SYMBOL_MINUS, 0xB03A2412, 0xFFF4E0, por_btn, -(p + 1));
    }

    /* the beans: those already won on their mats, the rest heaped */
    int k = 0;
    for (int p = 0; p < P->np; p++) {
        for (int i = 0; i < g->p[p].score && k < P->nb; i++, k++) {
            bean_t *b = &P->b[k];
            free_spot(P, P->m[p].x, P->m[p].y, P->m[p].r, &b->x, &b->y, k);
            b->zone = (int8_t)p;
            P->m[p].shown++;
        }
    }
    for (; k < P->nb; k++) {
        bean_t *b = &P->b[k];
        float a = mr_rnd(&P->seed) * 6.2831853f, d = powf(mr_rnd(&P->seed), 0.7f) * P->pr;
        b->x = P->pcx + cosf(a) * d;
        b->y = P->pcy + sinf(a) * d * 0.9f;
        b->zone = -1;
    }
    for (int i = 0; i < P->nb; i++) {
        bean_t *b = &P->b[i];
        b->type = (uint8_t)P->types[mr_rnd(&P->seed) < 0.5f ? 0 : 1];
        b->rot = (uint8_t)(mr_rnd(&P->seed) * ROTS) % ROTS;
        b->finger = -1;
        b->o = mr_img_obj(s->obj, &P->rest[b->type][b->rot]);
        lv_image_set_pivot(b->o, P->rest[0][0].w / 2, P->rest[0][0].h / 2);
        bean_show(P, b, 0);
    }
    for (int i = 0; i < MR_FINGERS; i++) {
        P->f[i].bean = -1;
        P->f[i].mat = -9;
        P->sh[i] = mr_img_obj(s->obj, &P->ash);
        lv_obj_add_flag(P->sh[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void sfree(mr_scene_t *s)
{
    por_t *P = ST;
    if (!P) return;
    mr_cv_free(&P->bg);
    for (int t = 0; t < BTYPES; t++)
        for (int r = 0; r < ROTS; r++) {
            mr_img_free(&P->rest[t][r]);
            mr_img_free(&P->air[t][r]);
        }
    mr_img_free(&P->ash);
}

/* --- the count ---------------------------------------------------------------------------- */

static void labels(mr_scene_t *s)
{
    por_t *P = ST;
    for (int p = 0; p < P->np; p++) {
        int n = 0;
        for (int i = 0; i < P->nb; i++)
            if (P->b[i].zone == p) n++;
        lv_label_set_text_fmt(P->m[p].name, "%s  %d", s->g->p[p].name, (int)s->g->p[p].score);
        (void)n;
    }
}

/* everything still: tell the model what moved */
static void settle(mr_scene_t *s)
{
    por_t *P = ST;
    int cnt[4] = { 0 };
    for (int i = 0; i < P->nb; i++) {
        bean_t *b = &P->b[i];
        b->zone = (int8_t)zone_at(P, b->x, b->y);
        if (b->zone >= 0) cnt[b->zone]++;
    }
    /* each bean that changed hands is one move: losers to winners, the rest from or to the heap */
    for (;;) {
        int gain = -1, lose = -1;
        for (int p = 0; p < P->np; p++) {
            if (cnt[p] > s->g->p[p].score && gain < 0) gain = p;
            if (cnt[p] < s->g->p[p].score && lose < 0) lose = p;
        }
        if (gain < 0 && lose < 0) break;
        mr_move(s->g, lose, gain);
    }
    labels(s);
}

/* send one bean flying by itself */
static void hop(por_t *P, int i, float x, float y)
{
    bean_t *b = &P->b[i];
    b->hop = true;
    b->hx0 = b->x;
    b->hy0 = b->y;
    b->hx = x;
    b->hy = y;
    b->ht = 0;
    b->vx = b->vy = 0;
    b->moving = false;
    lv_obj_move_to_index(b->o, -1);
}

/* the model changed by itself (undo, keypad, buttons): beans hop to match */
static void reconcile(mr_scene_t *s, bool animate)
{
    por_t *P = ST;
    mr_game_t *g = s->g;
    for (int p = 0; p < P->np; p++) {
        int have = 0;
        for (int i = 0; i < P->nb; i++)
            if ((P->b[i].hop ? zone_at(P, P->b[i].hx, P->b[i].hy) : P->b[i].zone) == p) have++;
        int want = g->p[p].score;
        while (have > want) {
            /* the last one in goes back to the heap */
            int pick = -1;
            for (int i = P->nb - 1; i >= 0 && pick < 0; i--) {
                bean_t *b = &P->b[i];
                if (b->finger < 0 && (b->hop ? zone_at(P, b->hx, b->hy) : b->zone) == p) pick = i;
            }
            if (pick < 0) break;
            float x, y;
            free_spot(P, P->pcx, P->pcy, P->pr, &x, &y, pick);
            P->b[pick].zone = -1;
            if (animate) hop(P, pick, x, y);
            else {
                P->b[pick].x = x;
                P->b[pick].y = y;
                bean_show(P, &P->b[pick], 0);
            }
            have--;
        }
        while (have < want) {
            /* the heap's bean nearest the mat */
            int pick = -1;
            float best = 1e12f;
            for (int i = 0; i < P->nb; i++) {
                bean_t *b = &P->b[i];
                if (b->finger >= 0 || b->hop || b->zone >= 0) continue;
                float d = (b->x - P->m[p].x) * (b->x - P->m[p].x) + (b->y - P->m[p].y) * (b->y - P->m[p].y);
                if (b->zone == -2) d *= 0.5f;   /* loose ones first */
                if (d < best) {
                    best = d;
                    pick = i;
                }
            }
            if (pick < 0) break;
            float x, y;
            free_spot(P, P->m[p].x, P->m[p].y, P->m[p].r, &x, &y, pick);
            P->b[pick].zone = (int8_t)p;
            if (animate) hop(P, pick, x, y);
            else {
                P->b[pick].x = x;
                P->b[pick].y = y;
                bean_show(P, &P->b[pick], 0);
            }
            have++;
        }
    }
    labels(s);
}

static void sync(mr_scene_t *s, int why, int who)
{
    (void)who;
    if (!ST) return;
    reconcile(s, why != MR_SYNC_LOAD);
}

/* --- motion ------------------------------------------------------------------------------- */

static void frame(mr_scene_t *s, float dt)
{
    por_t *P = ST;
    if (!P) return;
    bool any = false;
    float x0 = 30, y0 = s->top + 10, x1 = s->w - 30, y1 = s->h - 20;
    for (int i = 0; i < P->nb; i++) {
        bean_t *b = &P->b[i];
        if (b->hop) {
            any = true;
            b->ht += dt / HOP_S;
            float t = mr_clampf(b->ht, 0, 1), e = 1 - (1 - t) * (1 - t);
            float lift = sinf(3.14159f * t);
            b->x = b->hx0 + (b->hx - b->hx0) * e;
            b->y = b->hy0 + (b->hy - b->hy0) * e;
            bean_show(P, b, lift);
            if (t >= 1) {
                b->hop = false;
                bean_show(P, b, 0);
                mr_snd_play(b->zone == -1 ? MR_S_BEAN_SOFT : MR_S_BEAN, 0.8f, 0.85f + 0.3f * mr_rnd(&P->seed));
            }
            continue;
        }
        if (!b->moving || b->finger >= 0) continue;
        any = true;
        float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
        float dec = 1500.0f * dt;
        if (sp <= dec) {
            b->vx = b->vy = 0;
            b->moving = false;
        } else {
            b->vx -= b->vx / sp * dec;
            b->vy -= b->vy / sp * dec;
        }
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        /* the table's edge */
        if (b->x < x0) { b->x = x0; b->vx = fabsf(b->vx) * 0.4f; }
        if (b->x > x1) { b->x = x1; b->vx = -fabsf(b->vx) * 0.4f; }
        if (b->y < y0) { b->y = y0; b->vy = fabsf(b->vy) * 0.4f; }
        if (b->y > y1) { b->y = y1; b->vy = -fabsf(b->vy) * 0.4f; }
        /* knocking the others: a moving bean pushes what it meets */
        for (int j = 0; j < P->nb; j++) {
            if (j == i) continue;
            bean_t *o = &P->b[j];
            if (o->finger >= 0 || o->hop) continue;
            float dx = o->x - b->x, dy = o->y - b->y, d2 = dx * dx + dy * dy;
            if (d2 >= (BR * 2) * (BR * 2) || d2 < 0.01f) continue;
            float d = sqrtf(d2), nx = dx / d, ny = dy / d;
            float rel = (b->vx - o->vx) * nx + (b->vy - o->vy) * ny;
            if (rel > 0) {
                float imp = rel * 0.85f;
                o->vx += nx * imp;
                o->vy += ny * imp;
                b->vx -= nx * imp;
                b->vy -= ny * imp;
                o->moving = true;
                if (rel > 120) mr_snd_play(MR_S_BEAN_SOFT, mr_clampf(rel / 900.0f, 0.15f, 0.7f), 1.1f + 0.3f * mr_rnd(&P->seed));
            }
            /* out of each other */
            float push = (BR * 2 - d) * 0.5f;
            o->x += nx * push;
            o->y += ny * push;
            b->x -= nx * push;
            b->y -= ny * push;
            bean_show(P, o, 0);
        }
        bean_show(P, b, 0);
    }
    for (int f = 0; f < MR_FINGERS; f++) {
        int i = P->f[f].bean;
        if (i >= 0 && P->b[i].finger == f) any = true;
    }
    if (any) P->settling = true;
    else if (P->settling) {
        P->settling = false;
        settle(s);
    }
    /* a finger held still on a mat gives a bean back */
    for (int f = 0; f < MR_FINGERS; f++) {
        mr_finger_t *F = &mr_A->fing[f];
        if (!F->id || P->f[f].mat < 0 || P->f[f].held || F->moved) continue;
        if (mr_ms() - F->t0 > HOLD_MS) {
            P->f[f].held = true;
            mr_add(s->g, P->f[f].mat, -1);
        }
    }
}

/* --- fingers ------------------------------------------------------------------------------- */

static void por_btn(mr_scene_t *s, int arg)
{
    if (arg > 0) mr_add(s->g, arg - 1, 1);
    else mr_add(s->g, -arg - 1, -1);
}

static void down(mr_scene_t *s, int f, float x, float y)
{
    por_t *P = ST;
    if (!P) return;
    P->f[f].bean = -1;
    P->f[f].mat = -9;
    P->f[f].held = false;
    /* the bean under the finger: the nearest, the one on top if they overlap */
    int pick = -1;
    float best = 40.0f * 40.0f;
    int best_z = -1;
    for (int i = 0; i < P->nb; i++) {
        bean_t *b = &P->b[i];
        if (b->finger >= 0 || b->hop) continue;
        float d = (b->x - x) * (b->x - x) + (b->y - y) * (b->y - y);
        if (d > 40.0f * 40.0f) continue;
        int z = (int)lv_obj_get_index(b->o);
        if (d < best * 0.6f || (d < best * 1.6f && z > best_z)) {
            best = d;
            best_z = z;
            pick = i;
        }
    }
    if (pick >= 0) {
        bean_t *b = &P->b[pick];
        b->finger = (int8_t)f;
        b->from = b->zone;
        b->moving = false;
        b->vx = b->vy = 0;
        P->f[f].bean = pick;
        P->f[f].lx = x;
        P->f[f].ly = y;
        P->f[f].vx = P->f[f].vy = 0;
        P->f[f].lt = mr_ms();
        lv_obj_remove_flag(P->sh[f], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(P->sh[f], -1);
        lv_obj_move_to_index(b->o, -1);
        mr_snd_play(MR_S_BEAN_GRAB, 0.6f, 0.9f + 0.2f * mr_rnd(&P->seed));
        b->x = x;
        b->y = y - LIFT;
        bean_show(P, b, 1);
        lv_obj_set_pos(P->sh[f], (int)(b->x - P->ash.w * 0.5f + 12), (int)(b->y - P->ash.h * 0.5f + 22));
        return;
    }
    int z = zone_at(P, x, y);
    if (z >= 0) P->f[f].mat = z;
}

static void move(mr_scene_t *s, int f, float x, float y)
{
    por_t *P = ST;
    if (!P) return;
    int i = P->f[f].bean;
    if (i < 0) return;
    bean_t *b = &P->b[i];
    uint32_t now = mr_ms();
    float dt = (float)(now - P->f[f].lt) / 1000.0f;
    if (dt > 0.001f) {
        float k = mr_clampf(dt / 0.05f, 0, 1);
        P->f[f].vx += ((x - P->f[f].lx) / dt - P->f[f].vx) * k;
        P->f[f].vy += ((y - P->f[f].ly) / dt - P->f[f].vy) * k;
    }
    P->f[f].lx = x;
    P->f[f].ly = y;
    P->f[f].lt = now;
    b->x = mr_clampf(x, 30, s->w - 30);
    b->y = mr_clampf(y - LIFT, s->top + 10, s->h - 20);
    bean_show(P, b, 1);
    lv_obj_set_pos(P->sh[f], (int)(b->x - P->ash.w * 0.5f + 12), (int)(b->y - P->ash.h * 0.5f + 22));
}

static void up(mr_scene_t *s, int f, float x, float y)
{
    por_t *P = ST;
    if (!P) return;
    int i = P->f[f].bean;
    int mat = P->f[f].mat;
    bool held = P->f[f].held;
    P->f[f].bean = -1;
    P->f[f].mat = -9;
    lv_obj_add_flag(P->sh[f], LV_OBJ_FLAG_HIDDEN);
    if (i >= 0) {
        bean_t *b = &P->b[i];
        b->finger = -1;
        float vx = P->f[f].vx, vy = P->f[f].vy;
        /* a stale speed (the finger stopped before lifting) does not throw */
        if (mr_ms() - P->f[f].lt > 80) vx = vy = 0;
        float sp = sqrtf(vx * vx + vy * vy);
        if (sp > 2200) {
            vx *= 2200 / sp;
            vy *= 2200 / sp;
        }
        b->vx = vx * 0.8f;
        b->vy = vy * 0.8f;
        b->moving = true;
        bean_show(P, b, 0);
        int z = zone_at(P, b->x, b->y);
        mr_snd_play(z == -1 ? MR_S_BEAN_SOFT : MR_S_BEAN, 0.9f, 0.85f + 0.3f * mr_rnd(&P->seed));
        P->settling = true;
        return;
    }
    if (mat >= 0 && !held && x > -1000 && !mr_A->fing[f].moved && zone_at(P, x, y) == mat) mr_add(s->g, mat, 1);
}

const mr_scene_ops_t mr_porotos_ops = { build, sfree, sync, frame, down, move, up, NULL };
