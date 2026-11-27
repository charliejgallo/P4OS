/*
 * CIELO - a frame of the sky (see cielo.h).
 *
 * The projection is stereographic about the direction looked at: circles
 * on the sky stay circles on the screen and shapes keep their look near the
 * edges, so the same projection serves the whole dome (the zenith in the
 * middle and the horizon a circle, at ~190 degrees) and a view of the
 * horizon zoomed to a degree. A direction of camera coordinates (x right,
 * y up, z ahead) lands at (x, y) / (1 + z) times S.
 *
 * A frame is made in two steps. cl_frame_prepare(), once, on the worker:
 * the sky of the moment (cl_sky_update), every star and object projected, the
 * Milky Way sampled on a grid of 16 px, where each label goes (decided once
 * so the two halves agree) and where each object landed, for the taps.
 * Then cl_render() draws, once per half of the screen, each half on its
 * own core (aos_hal_worker_split), clipped to it.
 *
 * The ground is not a polygon: the horizon of a stereographic projection
 * is a circle (or a line), so each row's ground is found by solving a
 * quadratic and filled as one or two spans.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "cielo.h"
#include "cl_dmath.h"
#include "cl_mem.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PAD     24
#define BTN_R   38
#define BAR_H   92
#define PI_F    3.14159265f
#define D2R_F   0.017453293f

#define INK     0xE8EAEE
#define DIM     0x9AA1AD
#define CARD    0x141A24
#define HALO    0x000000
#define LINE_C  0x3A6EA5
#define ACCENT  0x7FC8FF

/* ---------------------------------------------------------------------------
 * The layout
 * ------------------------------------------------------------------------- */

void cl_layout(app_t *a)
{
    cl_lay_t *L = &a->L;
    int sw = a->sw, sh = a->sh;
    L->sw = sw;
    L->sh = sh;
    L->land = sw > sh;
    L->set = (cl_disc_t){ sw - PAD - BTN_R, PAD + BAR_H / 2, BTN_R };
    L->sats = (cl_disc_t){ L->set.x - 2 * BTN_R - 16, L->set.y, BTN_R };
    L->bar_x = PAD;
    L->bar_y = PAD;
    L->bar_h = BAR_H;
    L->bar_w = L->sats.x - BTN_R - 16 - PAD;
    if (L->bar_w > 560) L->bar_w = 560;
    L->mode = (cl_disc_t){ sw - PAD - BTN_R, PAD + BAR_H + 24 + BTN_R, BTN_R };
    L->night = (cl_disc_t){ L->mode.x, L->mode.y + 2 * BTN_R + 20, BTN_R };

    /* the time bar along the bottom (lying down: the right part) */
    L->time_y = sh - PAD - BTN_R;
    int tw = L->land ? 600 : sw - 2 * PAD;
    int tx = L->land ? sw - PAD - tw : PAD;
    int step = (tw - 2 * BTN_R) / 4;
    L->t_back2 = (cl_disc_t){ tx + BTN_R, L->time_y, BTN_R };
    L->t_back = (cl_disc_t){ tx + BTN_R + step, L->time_y, BTN_R };
    L->t_now = (cl_disc_t){ tx + BTN_R + 2 * step, L->time_y, BTN_R + 30 };
    L->t_fwd = (cl_disc_t){ tx + BTN_R + 3 * step, L->time_y, BTN_R };
    L->t_fwd2 = (cl_disc_t){ tx + BTN_R + 4 * step, L->time_y, BTN_R };

    /* the card: across the bottom upright, a column on the left lying down */
    L->card_h = L->land ? 384 : 330;
    if (L->land) {
        L->card_x = PAD;
        L->card_w = 480;
        L->card_y = sh - PAD - L->card_h;
    } else {
        L->card_x = PAD;
        L->card_w = sw - 2 * PAD;
        L->card_y = L->time_y - BTN_R - 24 - L->card_h;
    }
    L->card_close = (cl_disc_t){ L->card_x + L->card_w - 44, L->card_y + 44, 30 };
}

bool cl_hit_disc(const cl_disc_t *d, float x, float y)
{
    float dx = x - (float)d->x, dy = y - (float)d->y, r = (float)d->r + 12;
    return dx * dx + dy * dy < r * r;
}

bool cl_hit_card(const app_t *a, float x, float y)
{
    const cl_lay_t *L = &a->L;
    return x >= L->card_x && x < L->card_x + L->card_w && y >= L->card_y && y < L->card_y + L->card_h;
}

/* ---------------------------------------------------------------------------
 * The projection
 * ------------------------------------------------------------------------- */

typedef struct {
    float cam[3][3];        /* east, north, up -> camera (rows: right, up, ahead) */
    float S, cx, cy;
} proj_t;

static void proj_init(proj_t *p, const cl_scene_t *s)
{
    float sa = sinf(s->az), ca = cosf(s->az), se = sinf(s->alt), ce = cosf(s->alt);
    float f[3] = { ce * sa, ce * ca, se };
    float r[3] = { ca, -sa, 0 };
    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    for (int i = 0; i < 3; i++) {
        p->cam[0][i] = r[i];
        p->cam[1][i] = u[i];
        p->cam[2][i] = f[i];
    }
    int m = s->sw < s->sh ? s->sw : s->sh;
    p->S = (float)m * 0.5f / tanf(s->fov * 0.25f);
    p->cx = (float)s->sw * 0.5f;
    p->cy = (float)s->sh * 0.5f;
}

static inline bool proj_cam(const proj_t *p, const float c[3], float *x, float *y)
{
    if (c[2] < -0.95f) return false;
    float k = p->S / (1.0f + c[2]);
    *x = p->cx + c[0] * k;
    *y = p->cy - c[1] * k;
    return true;
}

bool cl_project(const cl_scene_t *s, const float enu[3], float *x, float *y)
{
    proj_t p;
    proj_init(&p, s);
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = p.cam[i][0] * enu[0] + p.cam[i][1] * enu[1] + p.cam[i][2] * enu[2];
    return proj_cam(&p, c, x, y);
}

static void unproj(const proj_t *p, float x, float y, float enu[3])
{
    float X = (x - p->cx) / p->S, Y = (p->cy - y) / p->S, R2 = X * X + Y * Y;
    float k = 1.0f / (1.0f + R2);
    float c[3] = { 2 * X * k, 2 * Y * k, (1 - R2) * k };
    for (int i = 0; i < 3; i++) enu[i] = p->cam[0][i] * c[0] + p->cam[1][i] * c[1] + p->cam[2][i] * c[2];
}

void cl_unproject(const cl_scene_t *s, float x, float y, float enu[3])
{
    proj_t p;
    proj_init(&p, s);
    unproj(&p, x, y, enu);
}

/* ---------------------------------------------------------------------------
 * The frame's preparation
 * ------------------------------------------------------------------------- */

#define MW_STEP     16
#define MW_SHIFT    8           /* log2(MW_STEP * MW_STEP) */
#define MAX_LABELS  160
#define TRACK_N     CL_TRACK_N

typedef struct {
    int16_t x, y;           /* top-left */
    uint8_t font;
    uint32_t rgb;
    char    txt[40];
} label_t;

typedef struct {
    float x, y, r;
    bool  on, up;
    float mag;
    float k, ang;           /* the Moon's phase and the Sun's way on the screen */
} bodyp_t;

typedef struct {
    float x, y;
    bool  on, up, lit;
    float mag;
} satp_t;

typedef struct {
    proj_t   p;
    cl_sky_t sky;
    float    M[3][3];       /* J2000 -> camera */
    float    mlim;          /* stars to this magnitude */
    float    sun_alt;
    float    dark;          /* 0 day .. 1 night */
    uint32_t bg, ground;
    int      n;             /* stars projected */
    bodyp_t  body[CL_NBODY];
    satp_t   sat[CL_MAX_SATS];
    int      mw_w, mw_h;
    float    mw_gain;
    label_t  lab[MAX_LABELS];
    int      nlab;
    float    gz[3];         /* the zenith in camera coordinates */
    /* the traced pass, cached: it is costly and does not change while the
     * view moves */
    int      tr_sat;
    double   tr_t0, tr_t1;
    uint32_t tr_gen;
    float    tr_enu[TRACK_N][3];
    int      tr_n;
    float    tr_xy[TRACK_N][2];
    bool     tr_on[TRACK_N];
    float    cardinal[8][2];
    bool     card_on[8];
    /* kept from frame to frame (see where each is used) */
    cl_body_t bp[CL_NBODY];
    double   b_jd;
    bool     b_ok, b_planets;
    cl_look_t sl[CL_MAX_SATS];
    bool     sl_ok[CL_MAX_SATS];
    double   sl_jd[CL_MAX_SATS];
    uint32_t sl_gen;
    int      sl_n;
    bool     mw_ok, mw_ground, mw_atmos;
    float    mw_az, mw_alt, mw_fov;
    int      mw_sw, mw_sh;
    double   mw_jd;
} frame_t;

static frame_t *s_f;

static void mat_vec(const float m[3][3], const float v[3], float o[3])
{
    o[0] = m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2];
    o[1] = m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2];
    o[2] = m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2];
}

/* label placement: a coarse grid of what is taken */
#define OCC_CELL 12
static uint8_t s_occ[(1280 / OCC_CELL + 2) * (1280 / OCC_CELL + 2)];
static int s_occ_w, s_occ_h;

static bool occ_take(int x0, int y0, int x1, int y1, bool force)
{
    int a0 = x0 / OCC_CELL, a1 = x1 / OCC_CELL, b0 = y0 / OCC_CELL, b1 = y1 / OCC_CELL;
    if (a0 < 0) a0 = 0;
    if (b0 < 0) b0 = 0;
    if (a1 >= s_occ_w) a1 = s_occ_w - 1;
    if (b1 >= s_occ_h) b1 = s_occ_h - 1;
    if (a0 > a1 || b0 > b1) return false;
    if (!force)
        for (int b = b0; b <= b1; b++)
            for (int x = a0; x <= a1; x++)
                if (s_occ[b * s_occ_w + x]) return false;
    for (int b = b0; b <= b1; b++)
        for (int x = a0; x <= a1; x++) s_occ[b * s_occ_w + x] = 1;
    return true;
}

static void add_label(app_t *a, float x, float y, float off, const char *t, int font, uint32_t rgb, bool force)
{
    frame_t *f = s_f;
    if (f->nlab >= MAX_LABELS || !t || !t[0]) return;
    const cl_font_t *fn = &a->font[font];
    int w = cl_text_width(fn, t), h = fn->line_h;
    /* to the right, else to the left, above, below */
    const int cand[4][2] = {
        { (int)(x + off), (int)(y - h / 2) },
        { (int)(x - off) - w, (int)(y - h / 2) },
        { (int)x - w / 2, (int)(y - off) - h },
        { (int)x - w / 2, (int)(y + off) },
    };
    int lx = 0, ly = 0, k = 0;
    for (; k < (off > 0 ? 4 : 1); k++) {
        lx = cand[k][0];
        ly = cand[k][1];
        if (lx < 0 || ly < 0 || ly + h > a->sh || lx + w > a->sw) continue;
        if (occ_take(lx, ly, lx + w, ly + h, force)) break;
    }
    if (k == (off > 0 ? 4 : 1)) return;
    label_t *l = &f->lab[f->nlab++];
    l->x = (int16_t)lx;
    l->y = (int16_t)ly;
    l->font = (uint8_t)font;
    l->rgb = rgb;
    snprintf(l->txt, sizeof l->txt, "%s", t);
}

static void add_hit(cl_hit_t *hits, int *n, float x, float y, int type, int idx, float r)
{
    if (*n >= CL_MAX_HITS) return;
    cl_hit_t *h = &hits[(*n)++];
    h->x = (int16_t)x;
    h->y = (int16_t)y;
    h->type = (uint8_t)type;
    h->idx = (int16_t)idx;
    h->r = (uint8_t)(r > 255 ? 255 : r);
}

static uint32_t sky_colour(float sun_alt_deg, bool atmos)
{
    const uint32_t NIGHT = 0x02050C;
    if (!atmos || sun_alt_deg < -18) return NIGHT;
    if (sun_alt_deg < -6) return cl_mix(NIGHT, 0x0C1C38, (sun_alt_deg + 18) / 12);
    if (sun_alt_deg < 0) return cl_mix(0x0C1C38, 0x2C5284, (sun_alt_deg + 6) / 6);
    if (sun_alt_deg < 12) return cl_mix(0x2C5284, 0x4F86C6, sun_alt_deg / 12);
    return 0x4F86C6;
}

static float star_radius(float mag, float mlim)
{
    float d = mlim - mag;
    if (d < 0) return 0;
    float r = 0.55f + 0.42f * d + 0.03f * d * d;
    return r > 9 ? 9 : r;
}

bool cl_frame_init(app_t *a)
{
    if (!s_f) s_f = (frame_t *)cl_calloc(sizeof(frame_t));
    if (!s_f) return false;
    s_f->tr_sat = -2;
    (void)a;
    return true;
}

void cl_frame_free(void)
{
    cl_free(s_f);
    s_f = NULL;
}

void cl_frame_prepare(app_t *a, const cl_scene_t *s, cl_hit_t *hits, int *nhits)
{
    frame_t *f = s_f;
    *nhits = 0;
    f->nlab = 0;
    proj_init(&f->p, s);
    uint64_t t0 = aos_hal_uptime_us();
    cl_sky_update(&f->sky, s->jd, s->lat, s->lon);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            f->M[i][j] = f->p.cam[i][0] * f->sky.m_j2000_enu[0][j] + f->p.cam[i][1] * f->sky.m_j2000_enu[1][j] +
                         f->p.cam[i][2] * f->sky.m_j2000_enu[2][j];
    for (int i = 0; i < 3; i++) f->gz[i] = f->p.cam[i][2];

    s_occ_w = s->sw / OCC_CELL + 1;
    s_occ_h = s->sh / OCC_CELL + 1;
    memset(s_occ, 0, (size_t)(s_occ_w * s_occ_h));
    /* nothing goes under the buttons and bars */
    const cl_lay_t *L = &a->L;
    occ_take(L->bar_x, L->bar_y, L->set.x + BTN_R, L->bar_y + L->bar_h, true);
    occ_take(L->mode.x - BTN_R, L->mode.y - BTN_R, L->mode.x + BTN_R, L->night.y + BTN_R, true);
    occ_take(L->t_back2.x - BTN_R, L->time_y - BTN_R, L->t_fwd2.x + BTN_R, L->time_y + BTN_R, true);
    if (s->card) occ_take(L->card_x, L->card_y, L->card_x + L->card_w, L->card_y + L->card_h, true);

    /* the bodies first: the Sun sets the sky. Their places are worked out
     * again only when the shown time moved 20 s (the Moon, the fastest,
     * moves 10 arcseconds in that): the theories are soft double on the
     * board, thousands of operations a frame for nothing */
    bool want_pl = s->planets;
    if (!f->b_ok || cl_dabs(s->jd - f->b_jd) > 20.0 / 86400 || f->b_planets != want_pl) {
        for (int b = 0; b < CL_NBODY; b++)
            if (want_pl || b <= CL_MOON) cl_body(b, s->jd, &f->bp[b]);
        f->b_jd = s->jd;
        f->b_planets = want_pl;
        f->b_ok = true;
    }
    cl_body_t *bp = f->bp;
    float benu[CL_NBODY][3];
    for (int b = 0; b < CL_NBODY; b++) {
        if (!want_pl && b >= CL_MERCURY) {
            f->body[b].on = false;
            continue;
        }
        cl_radec_to_enu(&f->sky, bp[b].ra, bp[b].dec, b == CL_MOON ? bp[b].dist_au : 0, benu[b]);
    }
    f->sun_alt = asinf(benu[CL_SUN][2]) * 57.29578f;
    f->bg = sky_colour(f->sun_alt, s->atmos);
    f->dark = s->atmos ? (f->sun_alt < -18 ? 1 : f->sun_alt > -6 ? 0 : (-6 - f->sun_alt) / 12) : 1;
    float day = s->atmos ? (f->sun_alt > 0 ? 1 : f->sun_alt < -6 ? 0 : (f->sun_alt + 6) / 6) : 0;
    f->ground = cl_mix(0x0B140D, 0x2A3A24, day);

    /* the limiting magnitude: wider fields show fewer, the twilight less */
    float fovd = s->fov * 57.29578f;
    float mlim = 4.9f + 1.25f * log2f(150.0f / (fovd < 2 ? 2 : fovd));
    if (mlim < 4.9f) mlim = 4.9f;
    if (mlim > 7.9f) mlim = 7.9f;
    mlim += s->mag_extra;
    if (s->atmos && f->sun_alt > -18) mlim -= (f->sun_alt + 18) / 18 * 4.5f;
    if (mlim < -1) mlim = -1;
    f->mlim = mlim;

    /* the stars */
    const cl_cat_t *c = &a->cat;
    int n = 0;
    float *pr = a->proj;
    float margin = 12;
    for (int i = 0; a->have_cat && i < c->n; i++) {
        if (c->mag[i] > mlim) break;
        float cc[3], x, y;
        mat_vec(f->M, &c->xyz[3 * i], cc);
        float up = f->sky.m_j2000_enu[2][0] * c->xyz[3 * i] + f->sky.m_j2000_enu[2][1] * c->xyz[3 * i + 1] +
                   f->sky.m_j2000_enu[2][2] * c->xyz[3 * i + 2];
        pr[3 * i + 2] = up;
        if (!proj_cam(&f->p, cc, &x, &y) || x < -margin || x > s->sw + margin || y < -margin || y > s->sh + margin) {
            pr[3 * i] = -1e9f;
            pr[3 * i + 1] = -1e9f;
        } else {
            pr[3 * i] = x;
            pr[3 * i + 1] = y;
        }
        n = i + 1;
    }
    f->n = n;

    /* the bodies on the screen */
    for (int b = 0; b < CL_NBODY; b++) {
        bodyp_t *q = &f->body[b];
        q->on = false;
        if (!s->planets && b != CL_SUN && b != CL_MOON) continue;
        float cc[3];
        mat_vec(f->p.cam, benu[b], cc);
        float x, y;
        if (!proj_cam(&f->p, cc, &x, &y)) continue;
        q->x = x;
        q->y = y;
        q->up = benu[b][2] > -0.01f;
        q->mag = bp[b].mag;
        /* the true size on the screen, at least a dot that says what it is */
        float k = f->p.S / (1 + cc[2]);
        float r = bp[b].semidiam * k;
        float rmin = b == CL_SUN || b == CL_MOON ? 9.0f : 3.0f + (2.0f - (q->mag > 2 ? 2 : q->mag)) * 0.9f;
        if (b >= CL_MERCURY && rmin < 3) rmin = 3;
        q->r = r > rmin ? r : rmin;
        q->on = x > -q->r - 40 && x < s->sw + q->r + 40 && y > -q->r - 40 && y < s->sh + q->r + 40;
        if (b == CL_MOON) {
            q->k = bp[b].phase;
            /* towards the Sun on the screen: a point a little that way */
            float t[3], cs[3];
            for (int i = 0; i < 3; i++) t[i] = benu[CL_MOON][i] + 0.02f * (benu[CL_SUN][i] - benu[CL_MOON][i]);
            mat_vec(f->p.cam, t, cs);
            float sx, sy;
            q->ang = proj_cam(&f->p, cs, &sx, &sy) ? atan2f(sy - y, sx - x) : 0;
        }
    }

    /* the satellites */
    for (int k = 0; k < s->nsat; k++) {
        satp_t *q = &f->sat[k];
        q->on = false;
        if (!s->sats) continue;
        /* again when the shown time moved a fifth of a second: a frame at
         * thirty a second reuses the look of the frame before */
        cl_look_t *lkp = &f->sl[k];
        if (f->sl_gen != s->gen || k >= f->sl_n || cl_dabs(s->jd - f->sl_jd[k]) > 0.2 / 86400) {
            f->sl_ok[k] = cl_sat_look(&s->sat[k].el, s->jd, s->lat, s->lon, 0.02, s->sat[k].std_mag, lkp);
            f->sl_jd[k] = s->jd;
        }
        if (!f->sl_ok[k]) continue;
        cl_look_t lk = *lkp;
        float ce = cosf((float)lk.el);
        float e[3] = { ce * sinf((float)lk.az), ce * cosf((float)lk.az), sinf((float)lk.el) }, cc[3];
        mat_vec(f->p.cam, e, cc);
        float x, y;
        if (!proj_cam(&f->p, cc, &x, &y)) continue;
        q->x = x;
        q->y = y;
        q->up = lk.el > 0;
        q->lit = lk.sunlit;
        q->mag = lk.mag;
        q->on = q->up && x > -20 && x < s->sw + 20 && y > -20 && y < s->sh + 20;
    }
    f->sl_gen = s->gen;
    f->sl_n = s->nsat;
    uint64_t t1 = aos_hal_uptime_us();
    /* the traced pass */
    if (s->track_sat >= 0 && s->track_sat < s->nsat) {
        if (f->tr_sat != s->track_sat || f->tr_t0 != s->track_t0 || f->tr_t1 != s->track_t1 || f->tr_gen != s->gen) {
            f->tr_sat = s->track_sat;
            f->tr_t0 = s->track_t0;
            f->tr_t1 = s->track_t1;
            f->tr_gen = s->gen;
            f->tr_n = 0;
            for (int i = 0; i < TRACK_N; i++) {
                double t = s->track_t0 + (s->track_t1 - s->track_t0) * i / (TRACK_N - 1);
                cl_look_t lk;
                if (!cl_sat_look(&s->sat[s->track_sat].el, t, s->lat, s->lon, 0.02, 99, &lk)) break;
                float ce = cosf((float)lk.el);
                f->tr_enu[i][0] = ce * sinf((float)lk.az);
                f->tr_enu[i][1] = ce * cosf((float)lk.az);
                f->tr_enu[i][2] = sinf((float)lk.el);
                f->tr_n = i + 1;
            }
        }
        for (int i = 0; i < f->tr_n; i++) {
            float cc[3];
            mat_vec(f->p.cam, f->tr_enu[i], cc);
            f->tr_on[i] = proj_cam(&f->p, cc, &f->tr_xy[i][0], &f->tr_xy[i][1]) && f->tr_enu[i][2] > -0.02f;
        }
    } else {
        f->tr_n = 0;
        f->tr_sat = -2;
    }

    /* the Milky Way on a grid */
    f->mw_gain = s->mw && a->cat.mw ? f->dark * (s->night ? 0.5f : 0.75f) : 0;
    f->mw_w = s->sw / MW_STEP + 2;
    f->mw_h = s->sh / MW_STEP + 2;
    /* the grid again only when the view or the ground changed, or the sky
     * turned a quarter of a degree (a minute): it is 4000 inverse
     * projections and the float atan2 and asin of each */
    bool mw_same = f->mw_ok && f->mw_az == s->az && f->mw_alt == s->alt && f->mw_fov == s->fov &&
                   f->mw_sw == s->sw && f->mw_sh == s->sh && f->mw_ground == s->ground && f->mw_atmos == s->atmos &&
                   cl_dabs(s->jd - f->mw_jd) < 60.0 / 86400;
    if (f->mw_gain > 0.01f && a->mwgrid && !mw_same) {
        f->mw_ok = true;
        f->mw_az = s->az;
        f->mw_alt = s->alt;
        f->mw_fov = s->fov;
        f->mw_sw = s->sw;
        f->mw_sh = s->sh;
        f->mw_ground = s->ground;
        f->mw_atmos = s->atmos;
        f->mw_jd = s->jd;
        float gal_from_cam[3][3];
        /* camera -> ENU is cam^T; ENU -> J2000 is m^T; J2000 -> galactic */
        for (int i = 0; i < f->mw_h; i++) {
            for (int j = 0; j < f->mw_w; j++) {
                float enu[3], eq[3], g[3];
                unproj(&f->p, (float)(j * MW_STEP), (float)(i * MW_STEP), enu);
                int v = 0;
                if (!(s->ground && enu[2] < -0.05f)) {
                    cl_enu_to_j2000(&f->sky, enu, eq);
                    cl_eq_to_gal(eq, g);
                    float l = atan2f(g[1], g[0]), b = asinf(g[2] > 1 ? 1 : g[2] < -1 ? -1 : g[2]);
                    v = cl_cat_mw(&a->cat, l, b);
                    /* fainter towards the horizon */
                    if (s->atmos && enu[2] < 0.25f) v = (int)((float)v * (enu[2] < 0 ? 0 : enu[2] * 4));
                }
                a->mwgrid[i * f->mw_w + j] = (uint8_t)v;
            }
        }
        (void)gal_from_cam;
    }

    uint64_t t2 = aos_hal_uptime_us();
    /* the cardinal points */
    for (int k = 0; k < 8; k++) {
        float az = (float)k * PI_F / 4;
        float e[3] = { sinf(az) * 0.9986f, cosf(az) * 0.9986f, 0.052f }, cc[3];
        mat_vec(f->p.cam, e, cc);
        f->card_on[k] = proj_cam(&f->p, cc, &f->cardinal[k][0], &f->cardinal[k][1]) && cc[2] > -0.5f;
        if (f->card_on[k]) {
            const cl_font_t *fn = &a->font[k % 2 ? F_S : F_M];
            int w = cl_text_width(fn, s->labels[k]);
            int x = (int)f->cardinal[k][0] - w / 2, y = (int)f->cardinal[k][1] - fn->line_h;
            if (x < 0 || x + w > s->sw || y < 0 || y > s->sh) f->card_on[k] = false;
            else occ_take(x, y, x + w, y + fn->line_h, true);
        }
    }

    /* labels and hits: what matters first */
    for (int b = 0; b < CL_NBODY; b++) {
        bodyp_t *q = &f->body[b];
        if (!q->on || (s->ground && !q->up)) continue;
        add_hit(hits, nhits, q->x, q->y, OBJ_BODY, b, q->r + 10);
        add_label(a, q->x, q->y, q->r + 8, cl_body_name(b), F_M, b == CL_SUN ? 0xFFD66B : b == CL_MOON ? 0xE6E6E6 : 0xFFC98A,
                  false);
    }
    for (int k = 0; k < s->nsat; k++) {
        satp_t *q = &f->sat[k];
        if (!q->on) continue;
        add_hit(hits, nhits, q->x, q->y, OBJ_SAT, k, 14);
        add_label(a, q->x, q->y, 12, s->sat[k].el.name, F_S, q->lit ? 0x9FE7FF : 0x6C8C9A, false);
    }
    if (s->sel_type == OBJ_SAT && s->sel_idx >= 0 && s->sel_idx < s->nsat && f->tr_n > 0) {
        /* the selected one's track hits too, so a tap there keeps it */
    }
    float name_lim = fovd > 120 ? 1.6f : fovd > 60 ? 2.4f : fovd > 25 ? 3.5f : 5.0f;
    for (int i = 0; i < n; i++) {
        float x = pr[3 * i], y = pr[3 * i + 1];
        if (x < -1e8f || (s->ground && pr[3 * i + 2] < 0)) continue;
        float m = c->mag[i];
        if (m < 5.8f || fovd < 40) add_hit(hits, nhits, x, y, OBJ_STAR, i, star_radius(m, mlim) + 6);
        if (s->snames && m < name_lim && c->name[i] != 0xFFFF && !s->moving) {
            const char *nm = cl_star_name(cl_cat_star_name(c, i));
            add_label(a, x, y, star_radius(m, mlim) + 6, nm, F_S, 0xC8D2E0, false);
        }
    }
    if (s->messier) {
        for (int i = 0; i < c->nmess; i++) {
            const struct cl_mess *m = &c->mess[i];
            float cc[3], x, y;
            mat_vec(f->M, m->xyz, cc);
            if (!proj_cam(&f->p, cc, &x, &y) || x < 0 || y < 0 || x > s->sw || y > s->sh) continue;
            float up = f->sky.m_j2000_enu[2][0] * m->xyz[0] + f->sky.m_j2000_enu[2][1] * m->xyz[1] +
                       f->sky.m_j2000_enu[2][2] * m->xyz[2];
            if (s->ground && up < 0) continue;
            if (m->mag > mlim + 2.5f && fovd > 50) continue;
            add_hit(hits, nhits, x, y, OBJ_MESS, i, 14);
            if (fovd < 70 || m->mag < 5) {
                char t[12];
                snprintf(t, sizeof t, "M%d", m->m);
                add_label(a, x, y, 12, t, F_S, 0xD7A6FF, false);
            }
        }
    }
    if (s->cnames && fovd < 200 && f->dark > 0.15f) {
        for (int i = 0; i < c->ncons; i++) {
            const struct cl_cons *k = &c->cons[i];
            float cc[3], x, y;
            mat_vec(f->M, k->xyz, cc);
            if (!proj_cam(&f->p, cc, &x, &y)) continue;
            float up = f->sky.m_j2000_enu[2][0] * k->xyz[0] + f->sky.m_j2000_enu[2][1] * k->xyz[1] +
                       f->sky.m_j2000_enu[2][2] * k->xyz[2];
            if (s->ground && up < 0.02f) continue;
            const char *nm = cl_cons_name(k->abbr);
            int w = cl_text_width(&a->font[F_S], nm);
            add_label(a, x - (float)w / 2, y, 0, nm, F_S, 0x6F93C0, false);
        }
    }
    uint64_t t3 = aos_hal_uptime_us();
    a->us_sky += (uint32_t)(t1 - t0);
    a->us_mw += (uint32_t)(t2 - t1);
    a->us_lab += (uint32_t)(t3 - t2);
}

/* ---------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */

static void milky_way(app_t *a, const cl_scene_t *s, cl_fb_t *fb)
{
    frame_t *f = s_f;
    if (f->mw_gain <= 0.01f || !a->mwgrid) return;
    uint32_t col = cl_tint(fb, 0xB8C6E8);
    int cr = (int)(col >> 16), cg = (int)(col >> 8) & 255, cb = (int)col & 255;
    int gain = (int)(f->mw_gain * 256 * 0.55f);
    (void)s;
    for (int i = 0; i + 1 < f->mw_h; i++) {
        int y0 = i * MW_STEP, y1 = y0 + MW_STEP;
        if (y1 <= fb->cy0 || y0 >= fb->cy1) continue;
        for (int j = 0; j + 1 < f->mw_w; j++) {
            int x0 = j * MW_STEP, x1 = x0 + MW_STEP;
            if (x1 <= fb->cx0 || x0 >= fb->cx1) continue;
            const uint8_t *g = a->mwgrid + i * f->mw_w + j;
            int v00 = g[0], v01 = g[1], v10 = g[f->mw_w], v11 = g[f->mw_w + 1];
            if (!(v00 | v01 | v10 | v11)) continue;
            int ya = y0 < fb->cy0 ? fb->cy0 : y0, yb = y1 > fb->cy1 ? fb->cy1 : y1;
            int xa = x0 < fb->cx0 ? fb->cx0 : x0, xb = x1 > fb->cx1 ? fb->cx1 : x1;
            for (int y = ya; y < yb; y++) {
                int fy = y - y0;
                int l = v00 * (MW_STEP - fy) + v10 * fy, r = v01 * (MW_STEP - fy) + v11 * fy;  /* * MW_STEP */
                uint16_t *p = cl_px(fb, xa, y);
                for (int x = xa; x < xb; x++, p += fb->sx) {
                    int fx = x - x0;
                    int v = (l * (MW_STEP - fx) + r * fx) >> MW_SHIFT;  /* 0..255 */
                    int amt = (v * gain) >> 8;
                    if (amt < 3) continue;
                    uint16_t px = *p;
                    int R = ((px >> 11) << 3) + (cr * amt >> 8);
                    int G = (((px >> 5) & 63) << 2) + (cg * amt >> 8);
                    int B = ((px & 31) << 3) + (cb * amt >> 8);
                    if (R > 255) R = 255;
                    if (G > 255) G = 255;
                    if (B > 255) B = 255;
                    *p = (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
                }
            }
        }
    }
}

/* a great circle's arc between two J2000 directions, as short pieces */
static void arc(cl_fb_t *fb, const float *a3, const float *b3, int pieces, uint32_t rgb, int alpha, float w)
{
    frame_t *f = s_f;
    float px = 0, py = 0;
    bool have = false;
    for (int k = 0; k <= pieces; k++) {
        float t = (float)k / (float)pieces, v[3], cc[3];
        for (int i = 0; i < 3; i++) v[i] = a3[i] + (b3[i] - a3[i]) * t;
        mat_vec(f->M, v, cc);
        float n = sqrtf(cc[0] * cc[0] + cc[1] * cc[1] + cc[2] * cc[2]);
        for (int i = 0; i < 3; i++) cc[i] /= n;
        float x = 0, y = 0;
        bool ok = cc[2] > -0.6f && proj_cam(&f->p, cc, &x, &y);
        if (ok && have) cl_line(fb, px, py, x, y, w, rgb, alpha);
        have = ok;
        px = x;
        py = y;
    }
}

/* a circle of the local sky (constant altitude) or a meridian, in ENU */
static void enu_curve(cl_fb_t *fb, float alt, float az, bool is_alt, uint32_t rgb, int alpha)
{
    frame_t *f = s_f;
    float px = 0, py = 0;
    bool have = false;
    int N = 72;
    for (int k = 0; k <= N; k++) {
        float e[3], cc[3];
        if (is_alt) {
            float A = (float)k * 2 * PI_F / N;
            e[0] = cosf(alt) * sinf(A);
            e[1] = cosf(alt) * cosf(A);
            e[2] = sinf(alt);
        } else {
            float h = -0.1f + (PI_F / 2 + 0.1f) * (float)k / N;
            e[0] = cosf(h) * sinf(az);
            e[1] = cosf(h) * cosf(az);
            e[2] = sinf(h);
        }
        mat_vec(f->p.cam, e, cc);
        float x = 0, y = 0;
        bool ok = cc[2] > -0.6f && proj_cam(&f->p, cc, &x, &y);
        if (ok && have && fabsf(x - px) < 600 && fabsf(y - py) < 600) cl_line(fb, px, py, x, y, 1.2f, rgb, alpha);
        have = ok;
        px = x;
        py = y;
    }
}

static void ground(const cl_scene_t *s, cl_fb_t *fb)
{
    frame_t *f = s_f;
    float nx = f->gz[0], ny = f->gz[1], nz = f->gz[2];
    float S = f->p.S, cx = f->p.cx, cy = f->p.cy;
    uint32_t col = f->ground;
    for (int y = fb->cy0; y < fb->cy1; y++) {
        float Y = (cy - ((float)y + 0.5f)) / S;
        float cc = 2 * ny * Y + nz * (1 - Y * Y);
        float x0s = ((float)fb->cx0 - cx) / S, x1s = ((float)fb->cx1 - cx) / S;
        /* q(X) = -nz X^2 + 2 nx X + cc < 0 is ground */
        float ivs[2][2];
        int ni = 0;
        if (fabsf(nz) < 1e-5f) {
            if (fabsf(nx) < 1e-6f) {
                if (cc < 0) ivs[ni][0] = x0s, ivs[ni][1] = x1s, ni++;
            } else {
                float r = -cc / (2 * nx);
                if (nx > 0) ivs[ni][0] = x0s, ivs[ni][1] = r, ni++;
                else ivs[ni][0] = r, ivs[ni][1] = x1s, ni++;
            }
        } else {
            float A = -nz, D = 4 * nx * nx + 4 * nz * cc;
            if (D <= 0) {
                if (A < 0) ivs[ni][0] = x0s, ivs[ni][1] = x1s, ni++;
            } else {
                float sq = sqrtf(D), r1 = (-2 * nx - sq) / (2 * A), r2 = (-2 * nx + sq) / (2 * A);
                if (r1 > r2) {
                    float t = r1;
                    r1 = r2;
                    r2 = t;
                }
                if (A < 0) {
                    ivs[ni][0] = x0s, ivs[ni][1] = r1, ni++;
                    ivs[ni][0] = r2, ivs[ni][1] = x1s, ni++;
                } else {
                    ivs[ni][0] = r1, ivs[ni][1] = r2, ni++;
                }
            }
        }
        for (int k = 0; k < ni; k++) {
            float a0 = ivs[k][0] < x0s ? x0s : ivs[k][0], a1 = ivs[k][1] > x1s ? x1s : ivs[k][1];
            if (a1 <= a0) continue;
            cl_span(fb, y, cx + a0 * S, cx + a1 * S, col, 255);
        }
    }
    (void)s;
}

static void button(app_t *a, cl_fb_t *fb, const cl_disc_t *d, bool on)
{
    (void)a;
    cl_disc(fb, (float)d->x, (float)d->y, (float)d->r, on ? 0x2A5D8F : 0x1A2230, 220);
    cl_ring(fb, (float)d->x, (float)d->y, (float)d->r - 1, 2, 0x33445A, 200);
}

static void glyph_c(app_t *a, cl_fb_t *fb, const cl_disc_t *d, const char *g, uint32_t rgb)
{
    const cl_font_t *fn = &a->font[F_M];
    int w = cl_text_width(fn, g);
    cl_text(fb, fn, d->x - w / 2, d->y - fn->line_h / 2, g, rgb, 0, false);
}

#define SYM_COG     "\xF3\xB0\x92\x93"
#define SYM_CLOSE   "\xF3\xB0\x85\x96"
#define SYM_LEFT    "\xF3\xB0\x85\x81"
#define SYM_RIGHT   "\xF3\xB0\x85\x82"
#define SYM_MOON    "\xF3\xB0\xBD\xA5"
#define SYM_PAUSE   "\xF3\xB0\x8F\xA4"

static void ui(app_t *a, const cl_scene_t *s, cl_fb_t *fb)
{
    const cl_lay_t *L = &a->L;
    /* the top bar */
    cl_rbox(fb, L->bar_x, L->bar_y, L->bar_w, L->bar_h, 24, 0x10161F, 215);
    cl_text(fb, &a->font[F_M], L->bar_x + 22, L->bar_y + 12, s->top1, INK, 0, false);
    uint32_t tc = s->live ? DIM : 0xFFC94A;
    cl_text(fb, &a->font[F_S], L->bar_x + 22, L->bar_y + 52, s->top2, tc, 0, false);
    if (s->rate[0]) {
        int w = cl_text_width(&a->font[F_S], s->rate);
        cl_rbox(fb, L->bar_x + L->bar_w - w - 34, L->bar_y + 50, w + 20, 30, 15, 0x5A4610, 230);
        cl_text(fb, &a->font[F_S], L->bar_x + L->bar_w - w - 24, L->bar_y + 52, s->rate, 0xFFD66B, 0, false);
    }
    /* the buttons */
    button(a, fb, &L->set, false);
    glyph_c(a, fb, &L->set, SYM_COG, INK);
    button(a, fb, &L->sats, false);
    {
        /* a small station: a body and two panels */
        float x = (float)L->sats.x, y = (float)L->sats.y;
        cl_line(fb, x - 20, y, x + 20, y, 2, INK, 255);
        cl_rbox(fb, (int)x - 22, (int)y - 11, 9, 22, 2, INK, 255);
        cl_rbox(fb, (int)x + 13, (int)y - 11, 9, 22, 2, INK, 255);
        cl_rbox(fb, (int)x - 5, (int)y - 6, 10, 12, 3, INK, 255);
    }
    bool dome = s->alt > 1.4f && s->fov > 2.6f;
    button(a, fb, &L->mode, false);
    {
        float x = (float)L->mode.x, y = (float)L->mode.y;
        if (dome) {
            /* go to the horizon: a line and an arc over it */
            cl_line(fb, x - 20, y + 9, x + 20, y + 9, 2.5f, INK, 255);
            cl_ring(fb, x, y + 9, 14, 2.5f, INK, 255);
            cl_fill_rect(fb, (int)x - 22, (int)y + 11, 44, 16, 0x1A2230);
        } else {
            /* go to the dome: a circle with the zenith */
            cl_ring(fb, x, y, 17, 2.5f, INK, 255);
            cl_disc(fb, x, y, 3.5f, INK, 255);
        }
    }
    button(a, fb, &L->night, s->night);
    glyph_c(a, fb, &L->night, SYM_MOON, s->night ? 0xFF5040 : INK);

    /* the time bar */
    button(a, fb, &L->t_back2, false);
    glyph_c(a, fb, &L->t_back2, "\xC2\xAB", INK);
    button(a, fb, &L->t_back, false);
    glyph_c(a, fb, &L->t_back, SYM_LEFT, INK);
    button(a, fb, &L->t_fwd, false);
    glyph_c(a, fb, &L->t_fwd, SYM_RIGHT, INK);
    button(a, fb, &L->t_fwd2, false);
    glyph_c(a, fb, &L->t_fwd2, "\xC2\xBB", INK);
    {
        const cl_disc_t *d = &L->t_now;
        const char *t = s->labels[8];
        int w = cl_text_width(&a->font[F_M], t) + 40;
        if (w < 140) w = 140;
        cl_rbox(fb, d->x - w / 2, d->y - BTN_R, w, 2 * BTN_R, BTN_R, s->live ? 0x1A2230 : 0x6B5212, 225);
        cl_text_c(fb, &a->font[F_M], d->x, d->y - a->font[F_M].line_h / 2, t, s->live ? DIM : 0xFFE08A, 0, false);
    }
    if (s->hint[0]) {
        int y = L->time_y - BTN_R - 16 - a->font[F_S].line_h;
        if (s->card) y = L->card_y - 12 - a->font[F_S].line_h;
        cl_text_c(fb, &a->font[F_S], s->sw / 2, y, s->hint, 0xFFD66B, HALO, true);
    }

    /* the card */
    if (s->card) {
        int x = L->card_x, y = L->card_y, w = L->card_w;
        cl_rbox(fb, x, y, w, L->card_h, 26, CARD, 235);
        cl_text(fb, &a->font[F_L], x + 26, y + 18, s->card_title, INK, 0, false);
        cl_text(fb, &a->font[F_S], x + 26, y + 18 + a->font[F_L].line_h + 2, s->card_sub, DIM, 0, false);
        cl_disc(fb, (float)L->card_close.x, (float)L->card_close.y, 26, 0x2A3240, 255);
        glyph_c(a, fb, &L->card_close, SYM_CLOSE, INK);
        int cols = L->land ? 2 : 3;
        int top = y + 18 + a->font[F_L].line_h + a->font[F_S].line_h + 22;
        int cw = (w - 52) / cols;
        for (int i = 0; i < s->card_nrows; i++) {
            int cx = x + 26 + (i % cols) * cw, cy = top + (i / cols) * (a->font[F_S].line_h + a->font[F_M].line_h + 12);
            cl_text(fb, &a->font[F_S], cx, cy, s->card_rows[i][0], DIM, 0, false);
            cl_text(fb, &a->font[F_M], cx, cy + a->font[F_S].line_h, s->card_rows[i][1], INK, 0, false);
        }
        cl_text(fb, &a->font[F_S], x + 26, y + L->card_h - 20 - a->font[F_S].line_h, s->card_foot, 0xBFD4EA, 0, false);
    }
}

void cl_render(app_t *a, const cl_scene_t *s, cl_fb_t *fb, int part, int parts, cl_hit_t *hits, int *nhits)
{
    (void)part;
    (void)parts;
    (void)hits;
    (void)nhits;
    frame_t *f = s_f;
    const cl_cat_t *c = &a->cat;
    fb->night = s->night;
    cl_fill_rect(fb, fb->cx0, fb->cy0, fb->cx1 - fb->cx0, fb->cy1 - fb->cy0, f->bg);
    milky_way(a, s, fb);

    float fovd = s->fov * 57.29578f;
    if (s->grid) {
        for (int k = 1; k <= 5; k++) enu_curve(fb, (float)k * 15 * D2R_F, 0, true, 0x3A5A40, 110);
        for (int k = 0; k < 12; k++) enu_curve(fb, 0, (float)k * 30 * D2R_F, false, 0x3A5A40, 90);
    }

    /* the figures */
    if (s->lines && a->have_cat) {
        int pieces = fovd > 100 ? 4 : fovd > 50 ? 2 : 1;
        int al = (int)(140 * (0.35f + 0.65f * f->dark));
        for (int i = 0; i < c->nsegs; i++) {
            int p0 = c->segs[2 * i], p1 = c->segs[2 * i + 1];
            arc(fb, &c->xyz[3 * p0], &c->xyz[3 * p1], pieces, LINE_C, al, 1.6f);
        }
    }

    /* the Messier objects */
    if (s->messier && a->have_cat) {
        for (int i = 0; i < c->nmess; i++) {
            const struct cl_mess *m = &c->mess[i];
            if (m->mag > f->mlim + 2.5f && fovd > 50) continue;
            float cc[3], x, y;
            mat_vec(f->M, m->xyz, cc);
            if (!proj_cam(&f->p, cc, &x, &y)) continue;
            if (x < fb->cx0 - 40 || x > fb->cx1 + 40 || y < fb->cy0 - 40 || y > fb->cy1 + 40) continue;
            float r = m->size / 60.0f * D2R_F * f->p.S / (1 + cc[2]) * 0.5f;
            if (r < 5) r = 5;
            uint32_t col = m->type == CL_M_SP || m->type == CL_M_EL || m->type == CL_M_IR ? 0xD7A6FF
                         : m->type == CL_M_GB ? 0xFFD9A0 : m->type == CL_M_OC ? 0xFFF2A8 : 0x9FE8C8;
            cl_ring(fb, x, y, r, 1.6f, col, 170);
            if (r > 12) cl_glow(fb, x, y, r, col, 40);
        }
    }

    /* the stars */
    const float *pr = a->proj;
    float alpha_k = 0.55f + 0.45f * f->dark;
    for (int i = 0; i < f->n; i++) {
        float x = pr[3 * i], y = pr[3 * i + 1];
        if (x < -1e8f) continue;
        float r = star_radius(c->mag[i], f->mlim);
        if (r <= 0 || x + r + 6 < fb->cx0 || x - r - 6 > fb->cx1 || y + r + 6 < fb->cy0 || y - r - 6 > fb->cy1) continue;
        uint32_t col = c->rgb[i];
        if (c->mag[i] < 1.5f) cl_glow(fb, x, y, r * 3.2f, col, (int)(70 * alpha_k));
        cl_disc(fb, x, y, r, col, (int)(255 * alpha_k));
    }

    /* the planets, the Moon, the Sun */
    for (int b = CL_NBODY - 1; b >= 0; b--) {
        const bodyp_t *q = &f->body[b];
        if (!q->on) continue;
        if (b == CL_SUN) {
            cl_glow(fb, q->x, q->y, q->r * 5, 0xFFE9A0, 120);
            cl_disc(fb, q->x, q->y, q->r, 0xFFF3C4, 255);
        } else if (b == CL_MOON) {
            cl_glow(fb, q->x, q->y, q->r * 3, 0xD8DCE6, (int)(50 * (0.3f + q->k)));
            cl_phase_disc(fb, q->x, q->y, q->r, q->k, q->ang, 0xF0F0EA, 0x2A2E36);
        } else {
            static const uint32_t PC[CL_NBODY] = { 0, 0, 0xC9C3B8, 0xFFF6DC, 0xFF8A5C, 0xF0D6B0, 0xF2DFA0, 0xB8E6F0, 0x8FB0FF };
            cl_glow(fb, q->x, q->y, q->r * 3, PC[b], 60);
            cl_disc(fb, q->x, q->y, q->r, PC[b], 255);
            if (b == CL_SATURN && q->r > 6) cl_ring(fb, q->x, q->y, q->r * 1.9f, 1.4f, PC[b], 160);
        }
    }

    /* the satellites and the traced pass */
    if (f->tr_n > 1) {
        for (int i = 1; i < f->tr_n; i++) {
            if (!f->tr_on[i] || !f->tr_on[i - 1]) continue;
            cl_line(fb, f->tr_xy[i - 1][0], f->tr_xy[i - 1][1], f->tr_xy[i][0], f->tr_xy[i][1], 2.2f, 0x5FD0FF, 190);
            if (i % 12 == 0) cl_disc(fb, f->tr_xy[i][0], f->tr_xy[i][1], 3.5f, 0x5FD0FF, 230);
        }
    }
    for (int k = 0; k < s->nsat; k++) {
        const satp_t *q = &f->sat[k];
        if (!q->on) continue;
        uint32_t col = q->lit ? 0xFFFFFF : 0x6C8C9A;
        cl_line(fb, q->x - 7, q->y, q->x + 7, q->y, 2, col, 255);
        cl_rbox(fb, (int)q->x - 3, (int)q->y - 3, 6, 6, 1, col, 255);
        cl_ring(fb, q->x, q->y, 10, 1.4f, 0x5FD0FF, 160);
    }

    /* the ground, and the horizon over it */
    if (s->ground) ground(s, fb);
    enu_curve(fb, 0, 0, true, s->ground ? 0x4E6B4A : 0x3E5A44, 200);

    /* the selection */
    if (s->sel_type != OBJ_NONE && a->nhits >= 0) {
        float x = -1, y = -1, r = 16;
        if (s->sel_type == OBJ_STAR && s->sel_idx < f->n && pr[3 * s->sel_idx] > -1e8f) {
            x = pr[3 * s->sel_idx];
            y = pr[3 * s->sel_idx + 1];
            r = star_radius(c->mag[s->sel_idx], f->mlim) + 12;
        } else if (s->sel_type == OBJ_BODY && f->body[s->sel_idx].on) {
            x = f->body[s->sel_idx].x;
            y = f->body[s->sel_idx].y;
            r = f->body[s->sel_idx].r + 12;
        } else if (s->sel_type == OBJ_SAT && s->sel_idx < s->nsat && f->sat[s->sel_idx].on) {
            x = f->sat[s->sel_idx].x;
            y = f->sat[s->sel_idx].y;
            r = 18;
        } else if (s->sel_type == OBJ_MESS && s->sel_idx < c->nmess) {
            float cc[3];
            mat_vec(f->M, c->mess[s->sel_idx].xyz, cc);
            if (!proj_cam(&f->p, cc, &x, &y)) x = -1;
            r = 20;
        }
        if (x >= 0) cl_ring(fb, x, y, r, 2.5f, 0xFFD66B, 230);
    }

    /* the labels */
    for (int k = 0; k < 8; k++) {
        if (!f->card_on[k]) continue;
        const cl_font_t *fn = &a->font[k % 2 ? F_S : F_M];
        cl_text_c(fb, fn, (int)f->cardinal[k][0], (int)f->cardinal[k][1] - fn->line_h, s->labels[k],
                  k == 0 ? 0xFF8A7A : 0xD9C27A, HALO, true);
    }
    for (int i = 0; i < f->nlab; i++) {
        const label_t *l = &f->lab[i];
        cl_text(fb, &a->font[l->font], l->x, l->y, l->txt, l->rgb, HALO, true);
    }

    ui(a, s, fb);
}
