/*
 * TANQUES - the ground (see tq_terrain.h)
 */
#include "tq_terrain.h"

#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

/* --------------------------------------------------------------------------
 * Value noise
 * -------------------------------------------------------------------------- */

static uint32_t s_seed;

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
 * The map
 * -------------------------------------------------------------------------- */

/* The dirt road's centre line: y as a function of x */
static float road_y(float x)
{
    return 17.0f + 5.0f * sinf(x * 0.17f + 0.6f) + 1.5f * sinf(x * 0.41f);
}

void tq_ter_generate(tq_terrain_t *t, uint32_t seed)
{
    s_seed = seed;
    memset(t, 0, sizeof *t);
    for (int j = 0; j < TQ_NV; j++) {
        for (int i = 0; i < TQ_NV; i++) {
            float x = i * TQ_CELL, y = j * TQ_CELL;
            float h = 0.35f;
            /* rolling ground, then the hills the battle is about */
            h += 0.9f * noise2(x / 7.0f, y / 7.0f) + 0.30f * noise2(x / 2.6f + 7, y / 2.6f + 3);
            h += 2.6f * bump(x, y, 18.5f, 19.5f, 7.0f);      /* the middle hill */
            h += 1.7f * bump(x, y, 9.0f, 27.0f, 6.0f);
            h += 1.4f * bump(x, y, 28.0f, 9.0f, 5.5f);
            h -= 0.7f * bump(x, y, 26.0f, 21.0f, 4.5f);      /* a hollow */
            /* hills all round, so the arena reads as a valley */
            float e = fminf(fminf(x, TQ_MAP_M - x), fminf(y, TQ_MAP_M - y));
            float rim = 1 - smooth01(0.5f, 4.5f, e);
            h += rim * (1.6f + 1.4f * noise2(x / 4.0f + 11, y / 4.0f + 5));
            /* the road is flatter */
            float dr = fabsf(y - road_y(x));
            float on_road = 1 - smooth01(0.6f, 1.6f, dr);
            h -= on_road * 0.12f;
            t->h[j * TQ_NV + i] = h;
            t->tint[j * TQ_NV + i] = (uint8_t)(255.0f * noise2(x / 3.3f + 21, y / 3.3f + 13));
            t->mat[j * TQ_NV + i] = on_road > 0.5f ? TQ_MAT_DIRT : TQ_MAT_GRASS;
        }
    }
    float hm = 0;
    for (int j = 0; j < TQ_NV; j++) {
        for (int i = 0; i < TQ_NV; i++) {
            if (t->h[j * TQ_NV + i] > hm) hm = t->h[j * TQ_NV + i];
        }
    }
    t->hmax = hm;
    tq_vbox_t all = { 0, 0, TQ_NV - 1, TQ_NV - 1 };
    tq_ter_light(t, all);
}

float tq_ter_height(const tq_terrain_t *t, float x, float y)
{
    float fx = tq_clampf(x / TQ_CELL, 0, TQ_N - 0.001f);
    float fy = tq_clampf(y / TQ_CELL, 0, TQ_N - 0.001f);
    int i = (int)fx, j = (int)fy;
    fx -= (float)i;
    fy -= (float)j;
    const float *r0 = &t->h[j * TQ_NV + i], *r1 = r0 + TQ_NV;
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

static const float MAT_RGB[4][3] = {
    { 0.44f, 0.70f, 0.30f },     /* grass */
    { 0.70f, 0.55f, 0.37f },     /* dirt  */
    { 0.62f, 0.58f, 0.54f },     /* rock  */
    { 0.86f, 0.78f, 0.56f },     /* sand  */
};

static inline uint8_t to8(float v)
{
    v = v * 255.0f + 0.5f;
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void tq_ter_light(tq_terrain_t *t, tq_vbox_t b)
{
    tq_v3_t L = tq_sun();
    float lh = sqrtf(L.x * L.x + L.y * L.y);
    float sx = L.x / lh * TQ_CELL, sy = L.y / lh * TQ_CELL;     /* one step towards the sun */
    float rise = tanf(TQ_SUN_ELEV * TQ_DEG) * TQ_CELL;          /* the ray's climb per step */
    for (int j = b.y0; j <= b.y1; j++) {
        for (int i = b.x0; i <= b.x1; i++) {
            int k = j * TQ_NV + i;
            float x = i * TQ_CELL, y = j * TQ_CELL, h = t->h[k];
            int i0 = i > 0 ? i - 1 : i, i1 = i < TQ_NV - 1 ? i + 1 : i;
            int j0 = j > 0 ? j - 1 : j, j1 = j < TQ_NV - 1 ? j + 1 : j;
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
                if (px < 0 || py < 0 || px > TQ_MAP_M || py > TQ_MAP_M) break;
                float over = tq_ter_height(t, px, py) - rh;
                if (over > 0) lit -= over * 3.0f;
            }
            if (lit < 0) lit = 0;
            /* hollows are a little darker: the height against the ring 1.5 m round */
            float ring = 0;
            static const int8_t RX[8] = { 3, 2, 0, -2, -3, -2, 0, 2 }, RY[8] = { 0, 2, 3, 2, 0, -2, -3, -2 };
            for (int q = 0; q < 8; q++) {
                int ii = i + RX[q], jj = j + RY[q];
                ii = ii < 0 ? 0 : (ii >= TQ_NV ? TQ_NV - 1 : ii);
                jj = jj < 0 ? 0 : (jj >= TQ_NV ? TQ_NV - 1 : jj);
                ring += t->h[jj * TQ_NV + ii];
            }
            float cav = ring * 0.125f - h;
            float ao = tq_clampf(1.0f - 0.30f * cav, 0.62f, 1.05f);

            const float *base = MAT_RGB[t->mat[k]];
            float r = base[0], g = base[1], bl = base[2];
            if (t->mat[k] == TQ_MAT_GRASS) {
                /* patches: yellower and darker greens */
                float p = t->tint[k] * (1.0f / 255.0f);
                r += (p - 0.5f) * 0.16f;
                g += (p - 0.5f) * 0.06f;
                bl -= (p - 0.5f) * 0.05f;
            }
            /* rock shows through where it is steep, gradually */
            float rock = smooth01(0.55f, 1.05f, sqrtf(gx * gx + gy * gy)) * 0.85f;
            r += (MAT_RGB[TQ_MAT_ROCK][0] - r) * rock;
            g += (MAT_RGB[TQ_MAT_ROCK][1] - g) * rock;
            bl += (MAT_RGB[TQ_MAT_ROCK][2] - bl) * rock;
            float burn = t->burn[k];
            r += (0.20f - r) * burn;
            g += (0.17f - g) * burn;
            bl += (0.15f - bl) * burn;

            float sky = 0.48f + 0.14f * n.z;
            float sun = 0.72f * dif * lit;
            float li = sky * ao + sun;
            float cr = r * li, cg = g * li, cb = bl * li;
            /* cool shadows, warm light: the toy look of the sprites */
            float sh = 1 - lit * (dif > 0.2f ? 1.0f : dif * 5);
            cb += 0.05f * sh * ao;
            cr += 0.03f * sun;
            t->col[k] = ((uint32_t)to8(cr) << 16) | ((uint32_t)to8(cg) << 8) | to8(cb);
        }
    }
}

tq_vbox_t tq_ter_crater(tq_terrain_t *t, float x, float y, float r, float d)
{
    float rr = r * 1.35f;
    int i0 = (int)floorf((x - rr) / TQ_CELL), i1 = (int)ceilf((x + rr) / TQ_CELL);
    int j0 = (int)floorf((y - rr) / TQ_CELL), j1 = (int)ceilf((y + rr) / TQ_CELL);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > TQ_NV - 1) i1 = TQ_NV - 1;
    if (j1 > TQ_NV - 1) j1 = TQ_NV - 1;
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            float dx = i * TQ_CELL - x, dy = j * TQ_CELL - y;
            float q = sqrtf(dx * dx + dy * dy) / r;
            int k = j * TQ_NV + i;
            if (q < 1) {
                float f = 1 - q * q;
                t->h[k] -= d * f * f * 1.15f;
                if (t->h[k] < TQ_BASE_Z + 0.3f) t->h[k] = TQ_BASE_Z + 0.3f;
            } else if (q < 1.35f) {
                float f = sinf((q - 1) / 0.35f * TQ_PI);
                t->h[k] += d * 0.16f * f;              /* the rim */
            }
            float bq = q / 1.3f;
            if (bq < 1) {
                float b = t->burn[k] + 0.85f * (1 - bq * bq);
                t->burn[k] = b > 0.9f ? 0.9f : b;
            }
            if (q < 1.1f && t->mat[k] == TQ_MAT_GRASS) t->mat[k] = TQ_MAT_DIRT;
        }
    }
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
    if (b.x1 > TQ_NV - 1) b.x1 = TQ_NV - 1;
    if (b.y1 > TQ_NV - 1) b.y1 = TQ_NV - 1;
    tq_ter_light(t, b);
    return b;
}

uint32_t tq_ter_side_colour(float z, float shade)
{
    /* strata of earth, darker going down */
    float k = (z - TQ_BASE_Z) * 2.2f;
    float band = 0.5f + 0.5f * sinf(k * 2.1f) * sinf(k * 0.7f + 1.3f);
    float r = 0.55f - 0.08f * band, g = 0.38f - 0.06f * band, b = 0.25f - 0.03f * band;
    float dark = 0.65f + 0.35f * tq_clampf((z - TQ_BASE_Z) / 2.5f, 0, 1);
    r *= dark * shade;
    g *= dark * shade;
    b *= dark * shade;
    return ((uint32_t)to8(r) << 16) | ((uint32_t)to8(g) << 8) | to8(b);
}
