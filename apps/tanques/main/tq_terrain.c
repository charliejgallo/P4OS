/*
 * TANQUES - the ground (see tq_terrain.h)
 */
#include "tq_terrain.h"
#include "tq_data.h"

#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

/* --------------------------------------------------------------------------
 * Value noise
 * -------------------------------------------------------------------------- */

static uint32_t s_seed, s_rng;

static inline float hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s_seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFF) * (1.0f / 65535.0f);
}

static float noise2(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float rnd(float a, float b)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return a + (b - a) * (float)(s_rng & 0xFFFFFF) / 16777216.0f;
}

static float bump(float x, float y, float cx, float cy, float r)
{
    float d2 = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / (r * r);
    return d2 >= 1 ? 0 : (1 - d2) * (1 - d2);
}

static float smooth01(float e0, float e1, float v)
{
    float t = tq_clampf((v - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

/* --------------------------------------------------------------------------
 * The biomes
 * -------------------------------------------------------------------------- */

typedef struct {
    float mat[4][3];        /* base, path, rock, special */
    float patch[3];         /* how the patches shift the base */
    bool  special_glows;    /* lava: lit by itself */
    uint32_t side_top, sky_top, sky_low, far;
} biome_look_t;

static const biome_look_t LOOK[TQ_BIOMES] = {
    { { { 0.44f, 0.70f, 0.30f }, { 0.70f, 0.55f, 0.37f }, { 0.62f, 0.58f, 0.54f }, { 0.86f, 0.78f, 0.56f } },
      { 0.16f, 0.06f, -0.05f }, false, 0x8C5E3C, 0x5C9AD6, 0xD2E6F0, 0xA9C49A },
    { { { 0.90f, 0.75f, 0.50f }, { 0.80f, 0.62f, 0.42f }, { 0.76f, 0.50f, 0.36f }, { 0.36f, 0.70f, 0.70f } },
      { 0.08f, 0.06f, 0.02f }, false, 0xB0784A, 0x6AA6DA, 0xF0E2C8, 0xE0C89A },
    { { { 0.93f, 0.96f, 0.99f }, { 0.74f, 0.80f, 0.87f }, { 0.50f, 0.52f, 0.57f }, { 0.66f, 0.86f, 0.95f } },
      { -0.05f, -0.03f, 0.0f }, false, 0x6E6A70, 0x8FB3D6, 0xE6EEF4, 0xDCE6EE },
    { { { 0.52f, 0.62f, 0.38f }, { 0.36f, 0.37f, 0.41f }, { 0.60f, 0.58f, 0.55f }, { 0.74f, 0.71f, 0.66f } },
      { 0.10f, 0.08f, -0.02f }, false, 0x6E5A48, 0x7C98B0, 0xCED6DC, 0x9CA690 },
    { { { 0.33f, 0.29f, 0.28f }, { 0.42f, 0.34f, 0.28f }, { 0.21f, 0.20f, 0.22f }, { 1.00f, 0.46f, 0.12f } },
      { 0.06f, 0.03f, 0.03f }, true, 0x3E3430, 0x5A4A58, 0xC89070, 0x6A5450 },
};

uint32_t tq_ter_far_colour(int biome, int which)
{
    const biome_look_t *L = &LOOK[biome % TQ_BIOMES];
    return which == 0 ? L->sky_top : (which == 1 ? L->sky_low : L->far);
}

/* --------------------------------------------------------------------------
 * The map
 * -------------------------------------------------------------------------- */

void tq_ter_generate(tq_terrain_t *t, uint32_t seed, float size, int biome)
{
    s_seed = seed;
    s_rng = seed * 2654435761u + 7u;
    memset(t, 0, sizeof *t);
    t->size = size;
    t->n = (int)(size / TQ_CELL + 0.5f);
    if (t->n > TQ_NMAX) t->n = TQ_NMAX;
    t->nv = t->n + 1;
    t->biome = biome;
    float M = t->n * TQ_CELL;
    /* the hills and hollows the battle is about, placed by the seed */
    float hx[6], hy[6], hr[6], hh[6];
    int nh = 3 + (int)rnd(0, 2.99f);
    for (int k = 0; k < nh; k++) {
        hx[k] = rnd(0.2f, 0.8f) * M;
        hy[k] = rnd(0.2f, 0.8f) * M;
        hr[k] = rnd(4.0f, 7.5f) * M / 36.0f + 1.0f;
        hh[k] = rnd(1.0f, 2.6f) * (k == nh - 1 ? -0.35f : 1.0f);
    }
    if (biome == TQ_B_RUINAS)
        for (int k = 0; k < nh; k++) hh[k] *= 0.45f;
    float rp = rnd(0, 6.28f), rq = rnd(0, 6.28f), ra = rnd(0.12f, 0.2f), ryy = rnd(0.35f, 0.65f) * M;
    float dune = rnd(0, 3.14f);
    float vx = rnd(0.35f, 0.65f) * M, vy = rnd(0.35f, 0.65f) * M;    /* the volcano */
    float pool_x = rnd(0.2f, 0.8f) * M, pool_y = rnd(0.2f, 0.8f) * M;
    for (int j = 0; j < t->nv; j++) {
        for (int i = 0; i < t->nv; i++) {
            float x = i * TQ_CELL, y = j * TQ_CELL;
            float h = 0.35f;
            h += 0.9f * noise2(x / 7.0f, y / 7.0f) + 0.30f * noise2(x / 2.6f + 7, y / 2.6f + 3);
            for (int k = 0; k < nh; k++) h += hh[k] * bump(x, y, hx[k], hy[k], hr[k]);
            uint8_t mat = TQ_MAT_BASE;
            /* a path across */
            float py = ryy + 5.0f * sinf(x * ra + rp) + 1.5f * sinf(x * 0.41f + rq);
            float on_path = 1 - smooth01(0.6f, 1.6f, fabsf(y - py));
            switch (biome) {
            case TQ_B_DESIERTO: {
                float d = x * cosf(dune) + y * sinf(dune);
                h += 0.55f * fabsf(sinf(d / 2.4f + 2.0f * noise2(x / 5, y / 5)));
                break;
            }
            case TQ_B_NIEVE: {
                float lake = bump(x, y, pool_x, pool_y, 4.5f);
                if (lake > 0.25f) {
                    h = h * (1 - lake) + 0.15f * lake;
                    if (lake > 0.45f) mat = TQ_MAT_SPECIAL;
                }
                break;
            }
            case TQ_B_RUINAS: {
                /* streets on a grid, plazas paved */
                float gx = fmodf(x + 3.0f, 9.0f), gy = fmodf(y + 5.0f, 9.0f);
                if (gx < 1.5f || gy < 1.5f) on_path = 1;
                else if (noise2(x / 9.0f + 3, y / 9.0f + 1) > 0.62f) mat = TQ_MAT_SPECIAL;
                break;
            }
            case TQ_B_VOLCAN: {
                float cone = bump(x, y, vx, vy, 8.0f * M / 36.0f);
                h += 4.0f * cone - 3.2f * bump(x, y, vx, vy, 2.6f * M / 36.0f);
                float pool = bump(x, y, pool_x, pool_y, 3.5f);
                if (pool > 0.3f) {
                    h = h * (1 - pool) - 0.25f * pool;
                    if (pool > 0.5f) mat = TQ_MAT_SPECIAL;
                }
                break;
            }
            default:
                break;
            }
            /* hills all round, so the arena reads as a valley */
            float e = fminf(fminf(x, M - x), fminf(y, M - y));
            float rim = 1 - smooth01(0.5f, 4.5f, e);
            h += rim * (1.6f + 1.4f * noise2(x / 4.0f + 11, y / 4.0f + 5));
            if (on_path > 0.5f && mat == TQ_MAT_BASE) mat = TQ_MAT_PATH;
            h -= on_path * 0.12f;
            int k = j * t->nv + i;
            t->h[k] = h;
            t->tint[k] = (uint8_t)(255.0f * noise2(x / 3.3f + 21, y / 3.3f + 13));
            t->mat[k] = mat;
        }
    }
    tq_ter_relight(t);
}

void tq_ter_flatten(tq_terrain_t *t, float x, float y, float r)
{
    float h0 = tq_ter_height(t, x, y);
    for (int j = 0; j < t->nv; j++) {
        for (int i = 0; i < t->nv; i++) {
            float dx = i * TQ_CELL - x, dy = j * TQ_CELL - y;
            float d = sqrtf(dx * dx + dy * dy) / r;
            if (d >= 1.6f) continue;
            float f = d < 1 ? 1 : 1 - smooth01(1, 1.6f, d);
            int k = j * t->nv + i;
            t->h[k] += (h0 - t->h[k]) * f;
            if (t->mat[k] == TQ_MAT_SPECIAL) t->mat[k] = TQ_MAT_BASE;
        }
    }
}

void tq_ter_relight(tq_terrain_t *t)
{
    float hm = 0;
    for (int k = 0; k < t->nv * t->nv; k++)
        if (t->h[k] > hm) hm = t->h[k];
    t->hmax = hm;
    tq_vbox_t all = { 0, 0, t->nv - 1, t->nv - 1 };
    tq_ter_light(t, all);
}

float tq_ter_height(const tq_terrain_t *t, float x, float y)
{
    float fx = tq_clampf(x / TQ_CELL, 0, t->n - 0.001f);
    float fy = tq_clampf(y / TQ_CELL, 0, t->n - 0.001f);
    int i = (int)fx, j = (int)fy;
    fx -= (float)i;
    fy -= (float)j;
    const float *r0 = &t->h[j * t->nv + i], *r1 = r0 + t->nv;
    /* the two triangles the renderers draw (split along i,j - i+1,j+1) */
    if (fx > fy) return r0[0] + (r0[1] - r0[0]) * fx + (r1[1] - r0[1]) * fy;
    return r0[0] + (r1[1] - r1[0]) * fx + (r1[0] - r0[0]) * fy;
}

void tq_ter_normal(const tq_terrain_t *t, float x, float y, tq_v3_t *n)
{
    float e = TQ_CELL;
    float gx = (tq_ter_height(t, x + e, y) - tq_ter_height(t, x - e, y)) / (2 * e);
    float gy = (tq_ter_height(t, x, y + e) - tq_ter_height(t, x, y - e)) / (2 * e);
    float l = 1.0f / sqrtf(gx * gx + gy * gy + 1);
    *n = tq_v3(-gx * l, -gy * l, l);
}

/* --------------------------------------------------------------------------
 * Light
 * -------------------------------------------------------------------------- */

static inline uint8_t to8(float v)
{
    v = v * 255.0f + 0.5f;
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void tq_ter_light(tq_terrain_t *t, tq_vbox_t b)
{
    const biome_look_t *LK = &LOOK[t->biome % TQ_BIOMES];
    int nv = t->nv;
    float M = t->n * TQ_CELL;
    tq_v3_t L = tq_sun();
    float lh = sqrtf(L.x * L.x + L.y * L.y);
    float sx = L.x / lh * TQ_CELL, sy = L.y / lh * TQ_CELL;     /* one step towards the sun */
    float rise = tanf(TQ_SUN_ELEV * TQ_DEG) * TQ_CELL;          /* the ray's climb per step */
    for (int j = b.y0; j <= b.y1; j++) {
        for (int i = b.x0; i <= b.x1; i++) {
            int k = j * nv + i;
            float x = i * TQ_CELL, y = j * TQ_CELL, h = t->h[k];
            int i0 = i > 0 ? i - 1 : i, i1 = i < nv - 1 ? i + 1 : i;
            int j0 = j > 0 ? j - 1 : j, j1 = j < nv - 1 ? j + 1 : j;
            float gx = (tq_ter_vh(t, i1, j) - tq_ter_vh(t, i0, j)) / ((i1 - i0) * TQ_CELL);
            float gy = (tq_ter_vh(t, i, j1) - tq_ter_vh(t, i, j0)) / ((j1 - j0) * TQ_CELL);
            float nl = 1.0f / sqrtf(gx * gx + gy * gy + 1);
            tq_v3_t n = tq_v3(-gx * nl, -gy * nl, nl);
            float dif = tq_dot(n, L);
            if (dif < 0) dif = 0;
            /* the hills' shadows: march towards the sun, softly */
            float lit = 1;
            float px = x, py = y, rh = h + 0.04f;
            for (int s = 0; s < 28 && lit > 0; s++) {
                px += sx;
                py += sy;
                rh += rise;
                if (px < 0 || py < 0 || px > M || py > M) break;
                float over = tq_ter_height(t, px, py) - rh;
                if (over > 0) lit -= over * 3.0f;
            }
            if (lit < 0) lit = 0;
            /* hollows are a little darker: the height against the ring 1.5 m round */
            float ring = 0;
            static const int8_t RX[8] = { 3, 2, 0, -2, -3, -2, 0, 2 }, RY[8] = { 0, 2, 3, 2, 0, -2, -3, -2 };
            for (int q = 0; q < 8; q++) {
                int ii = i + RX[q], jj = j + RY[q];
                ii = ii < 0 ? 0 : (ii >= nv ? nv - 1 : ii);
                jj = jj < 0 ? 0 : (jj >= nv ? nv - 1 : jj);
                ring += t->h[jj * nv + ii];
            }
            float cav = ring * 0.125f - h;
            float ao = tq_clampf(1.0f - 0.30f * cav, 0.62f, 1.05f);

            int m = t->mat[k];
            const float *base = LK->mat[m];
            float r = base[0], g = base[1], bl = base[2];
            if (m == TQ_MAT_BASE) {
                float p = t->tint[k] * (1.0f / 255.0f) - 0.5f;
                r += p * LK->patch[0];
                g += p * LK->patch[1];
                bl += p * LK->patch[2];
            }
            /* rock shows through where it is steep, gradually */
            float rock = smooth01(0.55f, 1.05f, sqrtf(gx * gx + gy * gy)) * 0.85f;
            if (m == TQ_MAT_SPECIAL && LK->special_glows) rock = 0;
            r += (LK->mat[TQ_MAT_ROCK][0] - r) * rock;
            g += (LK->mat[TQ_MAT_ROCK][1] - g) * rock;
            bl += (LK->mat[TQ_MAT_ROCK][2] - bl) * rock;
            float burn = t->burn[k];
            r += (0.20f - r) * burn;
            g += (0.17f - g) * burn;
            bl += (0.15f - bl) * burn;

            float sky = 0.48f + 0.14f * n.z;
            float sun = 0.72f * dif * lit;
            float li = sky * ao + sun;
            float cr = r * li, cg = g * li, cb = bl * li;
            if (m == TQ_MAT_SPECIAL && LK->special_glows) {
                /* lava: its own light, a little noisy */
                float f = 0.85f + 0.3f * (t->tint[k] / 255.0f);
                cr = r * f;
                cg = g * f;
                cb = bl * f;
            } else {
                /* cool shadows, warm light: the toy look of the sprites */
                float sh = 1 - lit * (dif > 0.2f ? 1.0f : dif * 5);
                cb += 0.05f * sh * ao;
                cr += 0.03f * sun;
            }
            t->col[k] = ((uint32_t)to8(cr) << 16) | ((uint32_t)to8(cg) << 8) | to8(cb);
        }
    }
}

tq_vbox_t tq_ter_crater(tq_terrain_t *t, float x, float y, float r, float d, bool scorch)
{
    int nv = t->nv;
    float rr = r * 1.35f;
    int i0 = (int)floorf((x - rr) / TQ_CELL), i1 = (int)ceilf((x + rr) / TQ_CELL);
    int j0 = (int)floorf((y - rr) / TQ_CELL), j1 = (int)ceilf((y + rr) / TQ_CELL);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > nv - 1) i1 = nv - 1;
    if (j1 > nv - 1) j1 = nv - 1;
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            float dx = i * TQ_CELL - x, dy = j * TQ_CELL - y;
            float q = sqrtf(dx * dx + dy * dy) / r;
            int k = j * nv + i;
            if (q < 1) {
                float f = 1 - q * q;
                t->h[k] -= d * f * f * 1.15f;
                if (t->h[k] < TQ_BASE_Z + 0.3f) t->h[k] = TQ_BASE_Z + 0.3f;
            } else if (q < 1.35f && d > 0) {
                float f = sinf((q - 1) / 0.35f * TQ_PI);
                t->h[k] += d * 0.16f * f;              /* the rim */
            }
            if (scorch) {
                float bq = q / 1.3f;
                if (bq < 1) {
                    float b = t->burn[k] + 0.85f * (1 - bq * bq);
                    t->burn[k] = b > 0.9f ? 0.9f : b;
                }
                if (q < 1.1f && t->mat[k] == TQ_MAT_BASE) t->mat[k] = TQ_MAT_PATH;
            }
        }
    }
    for (int k = 0; k < nv * nv; k++)
        if (t->h[k] > t->hmax) t->hmax = t->h[k];
    /* light again: the crater, and downhill of it the ground whose sun it
     * may now see or lose (the shadow march is 14 m) */
    tq_v3_t L = tq_sun();
    int reach = (int)(14.0f / TQ_CELL);
    tq_vbox_t b = { i0 - 1, j0 - 1, i1 + 1, j1 + 1 };
    if (L.x > 0) b.x0 -= reach;
    else b.x1 += reach;
    if (L.y > 0) b.y0 -= reach;
    else b.y1 += reach;
    if (b.x0 < 0) b.x0 = 0;
    if (b.y0 < 0) b.y0 = 0;
    if (b.x1 > nv - 1) b.x1 = nv - 1;
    if (b.y1 > nv - 1) b.y1 = nv - 1;
    tq_ter_light(t, b);
    return b;
}

uint32_t tq_ter_side_colour(const tq_terrain_t *t, float z, float shade)
{
    /* strata of earth, darker going down */
    uint32_t top = LOOK[t->biome % TQ_BIOMES].side_top;
    float k = (z - TQ_BASE_Z) * 2.2f;
    float band = 0.5f + 0.5f * sinf(k * 2.1f) * sinf(k * 0.7f + 1.3f);
    float r = ((top >> 16) & 255) / 255.0f, g = ((top >> 8) & 255) / 255.0f, b = (top & 255) / 255.0f;
    r *= 1.0f - 0.15f * band;
    g *= 1.0f - 0.15f * band;
    b *= 1.0f - 0.12f * band;
    float dark = 0.65f + 0.35f * tq_clampf((z - TQ_BASE_Z) / 2.5f, 0, 1);
    r *= dark * shade;
    g *= dark * shade;
    b *= dark * shade;
    return ((uint32_t)to8(r) << 16) | ((uint32_t)to8(g) << 8) | to8(b);
}
