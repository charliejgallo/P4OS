/*
 * PLANEO - the worlds (see vu_world.h)
 *
 * Made in stages, each over rows and split between the two cores where it
 * can be: the heights and what each texel is (a beach, a road, lava...);
 * the trees, stamped one by one; the shadows, swept along each row from
 * the sun's side; the colours, lit; the far levels. The heights live in a
 * temporary array of 16-bit units (2 MB) and the kind of each texel in the
 * memory of level 1, which is only filled at the end.
 */
#include "vu_world.h"
#include "vu_mem.h"
#include "vu_render.h"

#include <math.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define N       VW_N
#define MASK    (VW_N - 1)
#define IDX(x, y) ((((y) & MASK) << VW_LG) | ((x) & MASK))     /* the generation's own arrays, by rows */
#define TIX(x, y) vw_idx((uint32_t)(x) & MASK, (uint32_t)(y) & MASK, VW_LG)   /* level 0, in tiles */

/* what a texel is: the high nibble of the kind byte (the low one is the
 * shadow, 0 dark .. 15 lit) */
enum {
    K_LAND = 0,     /* the biome decides by height and slope */
    K_TREE,
    K_PINE,
    K_ROAD,
    K_WALK,         /* the city's pavements */
    K_ROOF,
    K_PARK,
    K_LAVA,
    K_ICE,
    K_FIELD,        /* fields and meadows at the city's edge */
};

/* --------------------------------------------------------------------------
 * Noise that tiles
 * -------------------------------------------------------------------------- */

static inline uint32_t hash3(uint32_t x, uint32_t y, uint32_t s)
{
    uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ s * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

static inline float hash01(uint32_t x, uint32_t y, uint32_t s)
{
    return (float)(hash3(x, y, s) & 0xFFFFFF) * (1.0f / 16777216.0f);
}

static inline float grad(uint32_t h, float x, float y)
{
    switch (h & 7) {
    case 0: return x + y;
    case 1: return x - y;
    case 2: return -x + y;
    case 3: return -x - y;
    case 4: return x * 1.41f;
    case 5: return -x * 1.41f;
    case 6: return y * 1.41f;
    default: return -y * 1.41f;
    }
}

/* The lattice's hash: a shuffled table of 1024, made once per world. The
 * seed moves where each octave reads it. (Four multiplications a corner
 * made the heights take 4 s on the board.) */
static uint16_t s_perm[1024];

static void perm_make(uint32_t seed)
{
    for (int i = 0; i < 1024; i++) s_perm[i] = (uint16_t)i;
    uint32_t r = seed * 2654435761u + 1u;
    for (int i = 1023; i > 0; i--) {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        int j = (int)(r % (uint32_t)(i + 1));
        uint16_t t = s_perm[i];
        s_perm[i] = s_perm[j];
        s_perm[j] = t;
    }
}

static inline uint32_t lat(uint32_t x, uint32_t y, uint32_t s)
{
    return s_perm[(s_perm[(x + s) & 1023] + y + (s >> 10)) & 1023];
}

/* Perlin noise of period P (a power of two, up to 1024) lattice cells, about -1..1 */
static float perlin(float x, float y, int P, uint32_t s)
{
    int ix = (int)x, iy = (int)y;
    ix -= x < (float)ix;            /* floor, without the library's call */
    iy -= y < (float)iy;
    float fx = x - (float)ix, fy = y - (float)iy;
    uint32_t m = (uint32_t)P - 1;
    uint32_t x0 = (uint32_t)ix & m, y0 = (uint32_t)iy & m, x1 = (x0 + 1) & m, y1 = (y0 + 1) & m;
    s &= 0xFFFFF;
    float u = fx * fx * fx * (fx * (fx * 6 - 15) + 10);
    float v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    float a = grad(lat(x0, y0, s), fx, fy);
    float b = grad(lat(x1, y0, s), fx - 1, fy);
    float c = grad(lat(x0, y1, s), fx, fy - 1);
    float d = grad(lat(x1, y1, s), fx - 1, fy - 1);
    float ab = a + (b - a) * u, cd = c + (d - c) * u;
    return (ab + (cd - ab) * v) * 0.7f;
}

/* The smallest cell an octave may have: 2 at full resolution, 4 while the
 * heights are made at half of it (finer octaves would only alias there) */
static int s_mincell = 2;

/* Octaves from a cell of 'cell' texels down, each half the size */
static float fbm(float x, float y, int cell, int oct, float gain, uint32_t s)
{
    float sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < oct && cell >= s_mincell; o++) {
        float k = 1.0f / (float)cell;
        sum += amp * perlin(x * k, y * k, N / cell, s + (uint32_t)o * 7919u);
        norm += amp;
        amp *= gain;
        cell >>= 1;
    }
    return sum / norm;
}

/* Ridges: sharp crests where the noise crosses zero, 0..1 */
static float ridged(float x, float y, int cell, int oct, float gain, uint32_t s)
{
    float sum = 0, amp = 1, norm = 0, w = 1;
    for (int o = 0; o < oct && cell >= s_mincell; o++) {
        float k = 1.0f / (float)cell;
        float n = 1.0f - fabsf(perlin(x * k, y * k, N / cell, s + (uint32_t)o * 104729u) * 1.3f);
        if (n < 0) n = 0;
        n *= n;
        sum += amp * n * w;
        norm += amp;
        w = n * 1.6f;
        if (w > 1) w = 1;
        amp *= gain;
        cell >>= 1;
    }
    return sum / norm;
}

static inline float clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

static inline float smooth(float e0, float e1, float v)
{
    float t = clampf((v - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

static inline float mixf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* distance on the torus, in texels */
static inline float tdist(float x, float y, float cx, float cy)
{
    float dx = fabsf(x - cx), dy = fabsf(y - cy);
    if (dx > N / 2) dx = N - dx;
    if (dy > N / 2) dy = N - dy;
    return sqrtf(dx * dx + dy * dy);
}

/* --------------------------------------------------------------------------
 * The generation's state
 * -------------------------------------------------------------------------- */

typedef struct {
    vw_world_t *w;
    uint16_t   *hq;     /* heights, units */
    uint8_t    *kind;   /* kind << 4 | shadow */
    uint32_t    seed;
    float       wl;     /* water level, metres (-1 none) */
    volatile int *progress;
    int         prog0, prog1;
    float      *hh;     /* the heights at half resolution, metres */
    uint8_t    *kh;     /* ... and the kinds */
    int8_t     *pat;    /* a tiling noise of 256 x 256 for the colours' patches */
    float      *warp;   /* the slow bending of the shapes, every 8 texels: x then y */
    float      *detail; /* the finest octave, 128 x 128 texels repeated (256 m: not seen) */
} gen_t;

/* x^1.5 and x^1.7 without powf (slow on the board), for 0..1 */
static inline float pow15(float x)
{
    return x * sqrtf(x);
}

static inline float pow17(float x)
{
    float r = sqrtf(x);
    return x * r * sqrtf(sqrtf(r)) * 0.94f + x * r * 0.06f;   /* x^1.6875, close enough */
}

#define WG      (N / 8)
/* The warp at (x, y), from the grid, between its points */
static inline void warp_at(const gen_t *g, float x, float y, float *wx, float *wy)
{
    float gx = x * 0.125f, gy = y * 0.125f;
    int ix = (int)gx, iy = (int)gy;
    float fx = gx - (float)ix, fy = gy - (float)iy;
    int x0 = ix & (WG - 1), y0 = iy & (WG - 1), x1 = (x0 + 1) & (WG - 1), y1 = (y0 + 1) & (WG - 1);
    for (int k = 0; k < 2; k++) {
        const float *w = g->warp + k * WG * WG;
        float a = w[y0 * WG + x0], b = w[y0 * WG + x1], c = w[y1 * WG + x0], d = w[y1 * WG + x1];
        float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
        float v = ab + (cd - ab) * fy;
        if (k) *wy = y + v;
        else *wx = x + v;
    }
}

static void prog(gen_t *g, int row, int part)
{
    /* the second core's half does not report: the first's is enough */
    if (!g->progress || part) return;
    *g->progress = g->prog0 + (g->prog1 - g->prog0) * row / (N / 2);
}

static inline uint16_t to_u(float m)
{
    float u = m * VW_HU;
    if (u < 0) u = 0;
    if (u > 32000) u = 32000;
    return (uint16_t)(u + 0.5f);
}

/* --------------------------------------------------------------------------
 * The heights of each kind of world
 * -------------------------------------------------------------------------- */

static float h_islas(gen_t *g, float x, float y, uint8_t *k)
{
    uint32_t s = g->seed;
    float wx, wy;
    warp_at(g, x, y, &wx, &wy);
    float e = fbm(wx, wy, 256, 7, 0.5f, s) * 1.7f + 0.02f;
    *k = K_LAND;
    if (e < 0) return g->wl + e * 70.0f;
    float m = ridged(wx, wy, 128, 5, 0.5f, s + 5);
    return g->wl + 0.8f + 190.0f * pow15(e) + 70.0f * e * m;
}

static float h_canones(gen_t *g, float x, float y, uint8_t *k)
{
    uint32_t s = g->seed;
    *k = K_LAND;
    float wx, wy;
    warp_at(g, x, y, &wx, &wy);
    /* the mesas: the noise in terraces with soft risers */
    float p = fbm(wx, wy, 256, 5, 0.55f, s) * 2.2f + 0.5f;
    float t = p * 5.0f;
    float fl = (float)(int)t, fr = t - fl;      /* t > 0 here */
    fr = smooth(0.70f, 0.95f, fr);
    float h = 50.0f + (fl + fr) * 32.0f + fbm(x, y, 16, 3, 0.5f, s + 3) * 3.0f;
    /* the river: where a second noise crosses zero, a gorge to the water */
    float r = fabsf(fbm(x, y, 512, 4, 0.45f, s + 9));
    float c = smooth(0.075f, 0.012f, r);
    float floor_h = g->wl - 3.0f;
    h = mixf(h, floor_h, c);
    if (h < 25.0f) h = 25.0f;
    return h;
}

static float h_nieve(gen_t *g, float x, float y, uint8_t *k)
{
    uint32_t s = g->seed;
    *k = K_LAND;
    float wx, wy;
    warp_at(g, x, y, &wx, &wy);
    float m = ridged(wx, wy, 256, 6, 0.45f, s);
    float base = fbm(x, y, 512, 2, 0.5f, s + 7) * 0.5f + 0.5f;
    float hills = fbm(x, y, 128, 4, 0.5f, s + 2) * 0.5f + 0.5f;
    float h = 30.0f + 330.0f * pow17(m) * (0.45f + 0.9f * base) + 60.0f * hills;
    /* the valleys' lakes stay; a frozen one now and then */
    if (h < g->wl && fbm(x, y, 256, 2, 0.5f, s + 8) > 0.18f) *k = K_ICE;
    return h;
}

static float h_volcan(gen_t *g, float x, float y, uint8_t *k)
{
    uint32_t s = g->seed;
    *k = K_LAND;
    float cx = N / 2, cy = N / 2;
    float d = tdist(x, y, cx, cy);
    float ang = atan2f(y - cy, x - cx);
    float R = 360.0f, rc = 42.0f;
    /* the island around it and the sea beyond */
    float e = fbm(x, y, 256, 6, 0.5f, s) * 1.4f + 0.55f - d / 560.0f;
    float h = e < 0 ? g->wl + e * 60.0f : g->wl + 0.6f + 60.0f * e;
    /* the cone, with gullies down its flanks */
    float q = 1.0f - d / R;
    if (q > 0) {
        float gul = ridged(ang * 40.0f + 500.0f, d * 0.6f, 32, 3, 0.5f, s + 4);
        float cone = 470.0f * pow17(q) * (0.92f + 0.12f * gul);
        float rim = 470.0f * pow17(1.0f - rc / R);
        if (d < rc) cone = rim - 70.0f * sqrtf(1.0f - d / rc);
        if (cone > h) h = cone;
        if (d < rc * 0.78f) {
            *k = K_LAVA;
            h = rim - 62.0f;
        }
        /* three flows of lava from the rim */
        for (int i = 0; i < 3; i++) {
            float a = hash01((uint32_t)i, 7, s) * 6.2831f;
            float wig = 0.18f * perlin(d / 40.0f, (float)i * 10.0f, 64, s + 40 + (uint32_t)i);
            float da = ang - a - wig;
            while (da > 3.14159f) da -= 6.28318f;
            while (da < -3.14159f) da += 6.28318f;
            float half = 4.5f / (d + 20.0f);
            if (d > rc && d < R * (0.55f + 0.25f * hash01((uint32_t)i, 9, s)) && fabsf(da) < half) {
                *k = K_LAVA;
                h -= 3.0f * (1.0f - fabsf(da) / half);
            }
        }
    }
    return h;
}

/* The city: blocks of 32 texels (64 m) between streets 5 wide, a river
 * winding across, low houses at the edges and towers in the middle. */
#define CITY_BLOCK 32
#define CITY_ROAD  5

static float river_y(float x, uint32_t s)
{
    float k = 6.2831853f / (float)N;
    return N * 0.3f + 70.0f * sinf(x * k + (float)(s & 7)) + 30.0f * sinf(x * k * 3.0f + 1.7f);
}

static float h_ciudad(gen_t *g, float x, float y, uint8_t *k)
{
    uint32_t s = g->seed;
    float gx, gy;
    warp_at(g, x, y, &gx, &gy);             /* the city's grid holds its gentle ground */
    float ground = 20.0f + (gx - x);
    /* the river */
    float ry = river_y(x, s);
    float dr = fabsf(y - ry);
    if (dr > N / 2) dr = N - dr;
    if (dr < 22.0f) {
        *k = K_LAND;
        if (dr < 16.0f) return g->wl - 4.0f;
        return mixf(g->wl - 1.0f, ground, (dr - 16.0f) / 6.0f);
    }
    int ix = (int)x, iy = (int)y;
    int bx = ix / CITY_BLOCK, by = iy / CITY_BLOCK;
    int lx = ix % CITY_BLOCK, ly = iy % CITY_BLOCK;
    if (lx < CITY_ROAD || ly < CITY_ROAD) {
        *k = K_ROAD;
        return ground;
    }
    /* how central the block is: towers in the middle */
    float dc = tdist((float)(bx * CITY_BLOCK + 16), (float)(by * CITY_BLOCK + 16), N * 0.5f, N * 0.62f);
    float central = clampf(1.0f - dc / 360.0f, 0, 1);
    float rb = hash01((uint32_t)bx, (uint32_t)by, s);
    if (central < 0.2f && rb < 0.55f) {
        /* the outskirts: fields */
        *k = K_FIELD;
        return ground;
    }
    if (rb < 0.10f) {
        *k = K_PARK;
        return ground;
    }
    /* the pavement round the block */
    int ex = lx - CITY_ROAD, ey = ly - CITY_ROAD, in = CITY_BLOCK - CITY_ROAD;
    if (ex < 1 || ey < 1 || ex >= in - 1 || ey >= in - 1) {
        *k = K_WALK;
        return ground + 0.2f;
    }
    /* lots: the block in two or four, each its own building */
    int split = rb < 0.4f ? 1 : 2;
    int cell = (in - 2) / split;
    int qx = (ex - 1) / cell, qy = (ey - 1) / cell;
    if (qx >= split) qx = split - 1;
    if (qy >= split) qy = split - 1;
    int ox = (ex - 1) - qx * cell, oy = (ey - 1) - qy * cell;
    if (split > 1 && (ox == 0 || oy == 0)) {
        *k = K_WALK;
        return ground + 0.2f;
    }
    uint32_t lot = hash3((uint32_t)(bx * 4 + qx), (uint32_t)(by * 4 + qy), s + 77);
    float lh = (float)(lot & 1023) / 1023.0f;
    float tall = 9.0f + 14.0f * lh + central * central * (40.0f + 150.0f * lh * lh);
    /* a tower steps back near its top */
    if (tall > 70.0f) {
        int m = ox < oy ? ox : oy;
        int m2 = (cell - 1 - ox) < (cell - 1 - oy) ? (cell - 1 - ox) : (cell - 1 - oy);
        if (m2 < m) m = m2;
        if (m < 2) tall *= 0.72f;
    }
    *k = K_ROOF;
    return ground + tall;
}

/* --------------------------------------------------------------------------
 * Stage 1: heights and kinds, by rows
 * -------------------------------------------------------------------------- */

static float biome_h(gen_t *g, float x, float y, uint8_t *k)
{
    switch (g->w->biome) {
    case VW_CANONES: return h_canones(g, x, y, k);
    case VW_NIEVE: return h_nieve(g, x, y, k);
    case VW_VOLCAN: return h_volcan(g, x, y, k);
    case VW_CIUDAD: return h_ciudad(g, x, y, k);
    default: return h_islas(g, x, y, k);
    }
}

#define NH (N / 2)

/* The shapes at half resolution: a quarter of the work, and the finest
 * octaves come back as a detail noise below */
static void stage_half(void *arg, int part)
{
    gen_t *g = (gen_t *)arg;
    int y0 = part ? NH / 2 : 0, y1 = part ? NH : NH / 2;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < NH; x++) {
            uint8_t k = K_LAND;
            g->hh[y * NH + x] = biome_h(g, (float)(x * 2), (float)(y * 2), &k);
            g->kh[y * NH + x] = k;
        }
        if ((y & 7) == 0) prog(g, (y - y0) * 2, part);
    }
}

static float detail_amp(int biome)
{
    switch (biome) {
    case VW_CANONES: return 1.2f;
    case VW_NIEVE: return 3.0f;
    case VW_VOLCAN: return 2.0f;
    default: return 1.4f;
    }
}

/* Full resolution: the half's heights between their texels, a detail noise
 * on the land, and the kinds from the nearest; the city straight (its
 * edges are sharp) */
static void stage_heights(void *arg, int part)
{
    gen_t *g = (gen_t *)arg;
    int y0 = part ? N / 2 : 0, y1 = part ? N : N / 2;
    int biome = g->w->biome;
    float amp = detail_amp(biome);
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < N; x++) {
            uint8_t k = K_LAND;
            float h;
            if (biome == VW_CIUDAD) {
                h = h_ciudad(g, (float)x, (float)y, &k);
            } else {
                int hx = x >> 1, hy = y >> 1;
                float fx = (float)(x & 1) * 0.5f, fy = (float)(y & 1) * 0.5f;
                int hx1 = (hx + 1) & (NH - 1), hy1 = (hy + 1) & (NH - 1);
                float a = g->hh[hy * NH + hx], b = g->hh[hy * NH + hx1];
                float c = g->hh[hy1 * NH + hx], d = g->hh[hy1 * NH + hx1];
                float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
                h = ab + (cd - ab) * fy;
                k = g->kh[hy * NH + hx];
                if (h > g->wl + 1.0f && k != K_LAVA && k != K_ICE)
                    h += amp * g->detail[((y & 127) << 7) | (x & 127)];
            }
            g->hq[IDX(x, y)] = to_u(h);
            g->kind[IDX(x, y)] = (uint8_t)(k << 4);
        }
        if ((y & 15) == 0) prog(g, y - y0, part);
    }
}

/* --------------------------------------------------------------------------
 * Stage 2: trees
 * -------------------------------------------------------------------------- */

static inline float hm(const gen_t *g, int x, int y)
{
    return (float)g->hq[IDX(x, y)] * (1.0f / VW_HU);
}

static float slope_at(const gen_t *g, int x, int y)
{
    float dx = (hm(g, x + 1, y) - hm(g, x - 1, y)) / (2 * VW_TEXM);
    float dy = (hm(g, x, y + 1) - hm(g, x, y - 1)) / (2 * VW_TEXM);
    return sqrtf(dx * dx + dy * dy);
}

static void stamp_tree(gen_t *g, int cx, int cy, float r, float ht, bool pine)
{
    float base = hm(g, cx, cy);
    int ir = (int)ceilf(r);
    for (int dy = -ir; dy <= ir; dy++) {
        for (int dx = -ir; dx <= ir; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy)) / r;
            if (d > 1) continue;
            float prof = pine ? (1.0f - d) * 0.85f + 0.15f : sqrtf(1.0f - d * d) * 0.55f + 0.45f;
            float top = base + ht * prof;
            int i = IDX(cx + dx, cy + dy);
            uint16_t u = to_u(top);
            if (u > g->hq[i]) {
                g->hq[i] = u;
                g->kind[i] = (uint8_t)((pine ? K_PINE : K_TREE) << 4);
            }
        }
    }
}

static void stage_trees(gen_t *g)
{
    int biome = g->w->biome;
    uint32_t s = g->seed + 991;
    int step = biome == VW_CIUDAD ? 4 : 3;
    for (int gy = 0; gy < N; gy += step) {
        for (int gx = 0; gx < N; gx += step) {
            uint32_t hsh = hash3((uint32_t)gx, (uint32_t)gy, s);
            int x = gx + (int)(hsh & 3) - 1, y = gy + (int)((hsh >> 2) & 3) - 1;
            int i = IDX(x, y);
            int k = g->kind[i] >> 4;
            float h = hm(g, x, y);
            if (g->wl >= 0 && h < g->wl + 1.5f) continue;
            float dens = fbm((float)x, (float)y, 64, 3, 0.5f, s) + 0.5f * hash01((uint32_t)x, (uint32_t)y, s + 1) - 0.25f;
            float sl = slope_at(g, x, y);
            bool pine = false;
            float r = 1.4f + (float)((hsh >> 4) & 7) * 0.12f, ht = 6.0f + (float)((hsh >> 8) & 7) * 0.7f;
            switch (biome) {
            case VW_ISLAS:
                if (k != K_LAND || sl > 0.65f || h < g->wl + 4.0f || h > g->wl + 150.0f || dens < 0.05f) continue;
                break;
            case VW_NIEVE:
                if (k != K_LAND || sl > 0.8f || h > 230.0f || dens < -0.02f) continue;
                pine = true;
                r = 1.1f + (float)((hsh >> 4) & 7) * 0.1f;
                ht = 9.0f + (float)((hsh >> 8) & 7) * 1.0f;
                break;
            case VW_CANONES:
                /* only along the river, and a few on the mesas */
                if (k != K_LAND || sl > 0.5f) continue;
                if (!(h < g->wl + 8.0f && dens > -0.1f) && !(dens > 0.38f)) continue;
                r *= 0.8f;
                ht *= 0.6f;
                break;
            case VW_VOLCAN:
                if (k != K_LAND || sl > 0.5f || h > g->wl + 45.0f || dens < 0.12f) continue;
                break;
            case VW_CIUDAD:
                if (k == K_PARK) {
                    if (dens < -0.3f) continue;
                } else if (k == K_FIELD) {
                    if (dens < 0.25f) continue;
                } else {
                    continue;
                }
                break;
            default: continue;
            }
            stamp_tree(g, x, y, r, ht, pine);
        }
    }
}

/* --------------------------------------------------------------------------
 * Stage 3: shadows, swept along each row from the sun's side
 * -------------------------------------------------------------------------- */

static void stage_shadows(void *arg, int part)
{
    gen_t *g = (gen_t *)arg;
    const float *sun = g->w->sun;
    float horiz = fabsf(sun[0]);
    if (horiz < 0.05f) horiz = 0.05f;
    /* how far the shadow line drops a texel, in units */
    int drop = (int)(VW_TEXM * sun[2] / horiz * VW_HU + 0.5f);
    if (drop < 1) drop = 1;
    int dir = sun[0] > 0 ? -1 : 1;      /* light travels away from the sun */
    int y0 = part ? N / 2 : 0, y1 = part ? N : N / 2;
    for (int y = y0; y < y1; y++) {
        uint16_t *row = g->hq + ((size_t)y << VW_LG);
        uint8_t *kr = g->kind + ((size_t)y << VW_LG);
        int s = 0;
        /* twice round: the first lap only to know the shadow at the start */
        for (int lap = 0; lap < 2; lap++) {
            for (int n = 0; n < N; n++) {
                int x = dir < 0 ? N - 1 - n : n;
                int h = row[x];
                s -= drop;
                if (h > s) s = h;
                if (lap) {
                    int under = s - h;          /* units below the shadow line */
                    int lit = 15 - under / 12;  /* soft over the first ~11 m */
                    if (lit < 0) lit = 0;
                    kr[x] = (uint8_t)((kr[x] & 0xF0) | lit);
                }
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Stage 4: colours
 * -------------------------------------------------------------------------- */

typedef struct {
    float r, g, b;
} rgb_t;

static inline rgb_t RGB(uint32_t c)
{
    rgb_t o = { (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f };
    return o;
}

static inline rgb_t mixc(rgb_t a, rgb_t b, float t)
{
    rgb_t o = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
    return o;
}

static inline uint16_t to565(float r, float g, float b)
{
    int R = (int)(clampf(r, 0, 1) * 31.0f + 0.5f);
    int G = (int)(clampf(g, 0, 1) * 63.0f + 0.5f);
    int B = (int)(clampf(b, 0, 1) * 31.0f + 0.5f);
    return (uint16_t)((R << 11) | (G << 5) | B);
}

static const uint32_t STRATA[] = { 0xC8643A, 0xDB8C50, 0xB5502F, 0xE8B47E, 0xA8462B, 0xD47A42, 0xE0A266 };
static const uint32_t ROOFS[] = { 0xB9B5AB, 0x8E8A84, 0xC2603C, 0x6F7A86, 0xD8D2C4, 0x9E7B5C, 0x5E6670, 0xA8A098 };

/* The albedo of a texel; *emit: lit by itself (lava) */
static rgb_t albedo(gen_t *g, int x, int y, float h, float sl, int k, bool water, bool *emit)
{
    uint32_t s = g->seed;
    float n1 = hash01((uint32_t)x, (uint32_t)y, s + 5) - 0.5f;   /* grain */
    float n2 = (float)g->pat[((y & MASK) >> 2 & 255) * 256 + ((x & MASK) >> 2 & 255)] * (1.0f / 160.0f);   /* patches */
    *emit = false;
    float wl = g->wl;
    if (water) {
        float depth = clampf((wl - h) / 18.0f, 0, 1);
        rgb_t shallow, deep;
        switch (g->w->biome) {
        case VW_CANONES: shallow = RGB(0x4FA88E); deep = RGB(0x2E6A66); break;
        case VW_NIEVE: shallow = RGB(0x4A7F96); deep = RGB(0x1E4660); break;
        case VW_VOLCAN: shallow = RGB(0x3D8FA0); deep = RGB(0x173E66); break;
        case VW_CIUDAD: shallow = RGB(0x4C7F8E); deep = RGB(0x2A5770); break;
        default: shallow = RGB(0x45D0C8); deep = RGB(0x15488A); break;
        }
        return mixc(shallow, deep, sqrtf(depth));
    }
    switch (k) {
    case K_TREE: {
        rgb_t c = g->w->biome == VW_CIUDAD ? RGB(0x3F7A34) : RGB(0x2E6B2A);
        return mixc(c, RGB(0x4C8A38), clampf(n1 + 0.5f + n2, 0, 1) * 0.5f);
    }
    case K_PINE: return mixc(RGB(0x1C4430), RGB(0x2C5A3A), n1 + 0.5f);
    case K_ROAD: return mixc(RGB(0x4E5157), RGB(0x5A5D63), n1 + 0.5f);
    case K_WALK: return RGB(0x9C988F);
    case K_PARK: return mixc(RGB(0x5C9A42), RGB(0x6FAE4E), n2 + 0.5f);
    case K_FIELD: {
        /* strips of crops, a colour a field */
        uint32_t f = hash3((uint32_t)(x / 32), (uint32_t)(y / 32), s + 3);
        static const uint32_t C[] = { 0x8DB04C, 0xC9B865, 0x6E9A44, 0xB59D5A, 0x9CB85A };
        rgb_t c = RGB(C[f % 5]);
        if (((x + (int)(f >> 8)) & 3) == 0) c = mixc(c, RGB(0x5E7A38), 0.3f);
        return c;
    }
    case K_ROOF: {
        int bx = x / CITY_BLOCK, by = y / CITY_BLOCK;
        int ex = (x % CITY_BLOCK) - CITY_ROAD, ey = (y % CITY_BLOCK) - CITY_ROAD;
        uint32_t lot = hash3((uint32_t)(bx * 4 + ex / 13), (uint32_t)(by * 4 + ey / 13), s + 78);
        rgb_t c = RGB(ROOFS[lot % 8]);
        return mixc(c, RGB(0xFFFFFF), n1 * 0.08f + 0.04f);
    }
    case K_LAVA: {
        *emit = true;
        float hot = clampf(0.5f + n2 * 1.4f + n1 * 0.3f, 0, 1);
        return mixc(RGB(0xD8380E), RGB(0xFFC43C), hot);
    }
    case K_ICE: return mixc(RGB(0xBFDDEA), RGB(0xE4F2F8), n2 + 0.5f);
    default: break;
    }
    /* the land, by world */
    switch (g->w->biome) {
    case VW_CANONES: {
        if (h < wl + 2.5f) return mixc(RGB(0xD8C293), RGB(0xC9AE7C), n1 + 0.5f);
        int band = (int)(h / 7.0f + n2 * 1.5f);
        rgb_t c = RGB(STRATA[(unsigned)band % 7]);
        if (sl < 0.25f) c = mixc(c, RGB(0xB7A06A), 0.45f + n2 * 0.5f);   /* the mesa's tops: scrub */
        return mixc(c, RGB(0x000000), n1 * 0.08f);
    }
    case VW_NIEVE: {
        float snowline = 215.0f + n2 * 60.0f - (0.7f - clampf(sl, 0, 0.7f)) * 70.0f;
        if (h > snowline && sl < 1.4f) return mixc(RGB(0xE8EEF5), RGB(0xFAFCFF), n1 + 0.5f);
        if (sl > 0.9f || h > 260.0f) return mixc(RGB(0x5F5B5E), RGB(0x7C7676), n1 + 0.5f);
        if (h < wl + 2.0f) return RGB(0x8A8A78);
        return mixc(RGB(0x6E8E52), RGB(0x8A9A62), clampf(n2 + 0.5f + (h - 60.0f) / 300.0f, 0, 1));
    }
    case VW_VOLCAN: {
        if (h < wl + 2.0f) return RGB(0x3A3636);
        if (h < wl + 40.0f && sl < 0.45f) return mixc(RGB(0x55703A), RGB(0x6E8444), n2 + 0.5f);
        rgb_t ash = mixc(RGB(0x3A3432), RGB(0x5A524C), n2 + 0.5f);
        if (sl > 1.0f) ash = mixc(ash, RGB(0x2A2626), 0.5f);
        return mixc(ash, RGB(0x6A3A2A), clampf((h - 300.0f) / 200.0f, 0, 0.35f));
    }
    case VW_CIUDAD:
        if (h < wl + 1.5f) return RGB(0x6E6A5E);
        return mixc(RGB(0x6E9A4A), RGB(0x84A658), n2 + 0.5f);
    default: {
        if (h < wl + 2.2f + n2 * 1.5f) return mixc(RGB(0xE6D6A2), RGB(0xF0E4BC), n1 + 0.5f);
        if (sl > 0.85f || h > wl + 165.0f) return mixc(RGB(0x7E776C), RGB(0x958D80), n1 + 0.5f);
        rgb_t grass = mixc(RGB(0x5FA03E), RGB(0x8DB850), clampf(n2 + 0.5f, 0, 1));
        return mixc(grass, RGB(0x8A8A5A), clampf((h - wl - 60.0f) / 120.0f, 0, 0.6f));
    }
    }
}

typedef struct {
    rgb_t sun, amb;
} light_t;

static void stage_colours(void *arg, int part)
{
    gen_t *g = (gen_t *)arg;
    vw_world_t *w = g->w;
    light_t L;
    L.sun = RGB(w->sun_col);
    rgb_t sky = RGB(w->sky_top), hor = RGB(w->sky_hor);
    L.amb = mixc(sky, hor, 0.5f);
    float ambk = w->hour == VW_NIGHT ? 0.55f : 0.55f;
    float sunk = w->hour == VW_NIGHT ? 0.85f : 1.0f;
    uint16_t wl_u = g->wl >= 0 ? to_u(g->wl) : 0;
    int y0 = part ? N / 2 : 0, y1 = part ? N : N / 2;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < N; x++) {
            int i = IDX(x, y);
            int hu = g->hq[i];
            int k = g->kind[i] >> 4, shadow = g->kind[i] & 15;
            bool water = g->wl >= 0 && hu < wl_u && k != K_ICE;
            float h = (float)hu / VW_HU;
            float dx = (hm(g, x + 1, y) - hm(g, x - 1, y)) / (2 * VW_TEXM);
            float dy = (hm(g, x, y + 1) - hm(g, x, y - 1)) / (2 * VW_TEXM);
            float sl = sqrtf(dx * dx + dy * dy);
            bool emit;
            rgb_t a = albedo(g, x, y, h, sl, k, water, &emit);
            float r, gg, b;
            if (emit) {
                r = a.r;
                gg = a.g;
                b = a.b;
            } else if (water) {
                /* the water: its own colour and the sky's, the renderer adds the shine */
                float lit = ambk + 0.45f * sunk * w->sun[2];
                r = a.r * lit * (0.6f + 0.4f * L.sun.r) + 0.10f * L.amb.r;
                gg = a.g * lit * (0.6f + 0.4f * L.sun.g) + 0.10f * L.amb.g;
                b = a.b * lit * (0.6f + 0.4f * L.sun.b) + 0.10f * L.amb.b;
            } else {
                float il = 1.0f / sqrtf(dx * dx + dy * dy + 1.0f);
                float nx = -dx * il, ny = -dy * il, nz = il;
                float dif = nx * w->sun[0] + ny * w->sun[1] + nz * w->sun[2];
                if (dif < 0) dif = 0;
                dif *= (float)shadow * (1.0f / 15.0f);
                /* the hollows a little darker, the crests a little lighter */
                float around = (hm(g, x + 4, y) + hm(g, x - 4, y) + hm(g, x, y + 4) + hm(g, x, y - 4)) * 0.25f;
                float ao = clampf(1.0f - (around - h) * 0.035f, 0.55f, 1.08f);
                float am = ambk * ao * (0.75f + 0.25f * nz);
                r = a.r * (L.amb.r * am + L.sun.r * dif * sunk);
                gg = a.g * (L.amb.g * am + L.sun.g * dif * sunk);
                b = a.b * (L.amb.b * am + L.sun.b * dif * sunk);
                if (w->hour == VW_NIGHT && k == K_ROOF && (hash3((uint32_t)x, (uint32_t)y, g->seed) & 15) == 0) {
                    /* a lit window on a roof */
                    r = 1.0f;
                    gg = 0.85f;
                    b = 0.5f;
                }
            }
            uint32_t hh = water ? wl_u : (uint32_t)hu;
            w->lev[0][TIX(x, y)] = (uint32_t)to565(r, gg, b) | (hh << 16) | (water ? VW_WATER : 0);
        }
        if ((y & 15) == 0) prog(g, y - y0, part);
    }
}

/* --------------------------------------------------------------------------
 * Stage 5: the far levels
 * -------------------------------------------------------------------------- */

static void make_level(vw_world_t *w, int l)
{
    int n = N >> l, lg = VW_LG - l;
    const uint32_t *src = w->lev[l - 1];
    uint32_t *dst = w->lev[l];
    int slg = lg + 1;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            uint32_t X = (uint32_t)(2 * x), Y = (uint32_t)(2 * y);
            uint32_t t[4] = { src[vw_idx(X, Y, slg)], src[vw_idx(X + 1, Y, slg)], src[vw_idx(X, Y + 1, slg)],
                              src[vw_idx(X + 1, Y + 1, slg)] };
            int r = 0, g = 0, b = 0, hs = 0, hx = 0, wet = 0;
            for (int i = 0; i < 4; i++) {
                r += (t[i] >> 11) & 31;
                g += (t[i] >> 5) & 63;
                b += t[i] & 31;
                int h = vw_h(t[i]);
                hs += h;
                if (h > hx) hx = h;
                wet += (t[i] & VW_WATER) ? 1 : 0;
            }
            /* between the mean and the highest: towers and crests keep some of their skyline */
            uint32_t h = (uint32_t)((hs / 4 + hx) / 2);
            dst[vw_idx((uint32_t)x, (uint32_t)y, lg)] = (uint32_t)(((r / 4) << 11) | ((g / 4) << 5) | (b / 4)) | (h << 16) |
                                 (wet >= 2 ? VW_WATER : 0);
        }
    }
}

/* --------------------------------------------------------------------------
 * The hours
 * -------------------------------------------------------------------------- */

static void set_hour(vw_world_t *w, int hour)
{
    float el, az;
    switch (hour) {
    case VW_MORNING:
        el = 0.30f, az = 0.25f;
        w->sun_col = 0xFFD8B0;
        w->sky_top = 0x4A82C8;
        w->sky_hor = 0xF4CFA6;
        w->fog = 0xE6D0BC;
        w->cloud_lit = 0xFFE8D2;
        w->cloud_dark = 0xA8A0B4;
        break;
    case VW_EVENING:
        el = 0.17f, az = 3.4f;
        w->sun_col = 0xFFA060;
        w->sky_top = 0x34488A;
        w->sky_hor = 0xF8A060;
        w->fog = 0xD89878;
        w->cloud_lit = 0xFFB880;
        w->cloud_dark = 0x7A5A78;
        break;
    case VW_NIGHT:
        el = 0.75f, az = 3.6f;
        w->sun_col = 0x9CB0E0;
        w->sky_top = 0x0A1028;
        w->sky_hor = 0x2A3A64;
        w->fog = 0x26345A;
        w->cloud_lit = 0x4A5472;
        w->cloud_dark = 0x1A2034;
        break;
    default:
        el = 1.05f, az = 0.6f;
        w->sun_col = 0xFFF8EC;
        w->sky_top = 0x2C6CC4;
        w->sky_hor = 0xC4DCF0;
        w->fog = 0xC8DCEE;
        w->cloud_lit = 0xFFFFFF;
        w->cloud_dark = 0xAAB4C4;
        break;
    }
    w->hour = hour;
    w->stars = hour == VW_NIGHT;
    w->sun[0] = cosf(el) * cosf(az);
    w->sun[1] = cosf(el) * sinf(az);
    w->sun[2] = sinf(el);
}

/* --------------------------------------------------------------------------
 * The clouds
 * -------------------------------------------------------------------------- */

static uint8_t *s_cloud;

const uint8_t *vw_clouds(void)
{
    return s_cloud;
}

void vw_clouds_make(uint32_t seed, float cover)
{
    int n = 1 << VW_CLOUD_LG;
    if (!s_cloud) s_cloud = (uint8_t *)vu_malloc((size_t)n * n);
    if (!s_cloud) return;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            /* the noise's period is the texture's: cells of 64, 32... of 256 */
            float v = 0, amp = 1, norm = 0;
            int cell = 64;
            for (int o = 0; o < 5; o++) {
                v += amp * perlin((float)x / (float)cell, (float)y / (float)cell, n / cell, seed + (uint32_t)o * 31u);
                norm += amp;
                amp *= 0.55f;
                cell >>= 1;
            }
            v = v / norm + 0.5f;
            float d = (v - (1.0f - cover)) * 3.2f;
            s_cloud[y * n + x] = (uint8_t)(clampf(d, 0, 1) * 255.0f);
        }
    }
}

/* --------------------------------------------------------------------------
 * The whole
 * -------------------------------------------------------------------------- */

bool vw_alloc(vw_world_t *w)
{
    for (int l = 0; l < VW_LEVELS; l++) {
        if (w->lev[l]) continue;
        size_t n = (size_t)(N >> l) * (size_t)(N >> l) * 4;
        w->lev[l] = (uint32_t *)vu_malloc(n);
        if (!w->lev[l]) {
            vw_free(w);
            return false;
        }
    }
    return true;
}

void vw_free(vw_world_t *w)
{
    for (int l = 0; l < VW_LEVELS; l++) {
        vu_free(w->lev[l]);
        w->lev[l] = NULL;
    }
}

static void find_start(gen_t *g)
{
    vw_world_t *w = g->w;
    float best = 1e9f;
    float bx = N / 2, by = N / 2;
    /* a spot low and clear, near where the world shows itself best */
    float tx = N * 0.5f, ty = N * 0.5f;
    if (w->biome == VW_CIUDAD) ty = N * 0.62f;
    for (int i = 0; i < 64; i++) {
        float a = (float)i * 0.618f * 6.2831f;
        float r = 260.0f + (float)(i % 8) * 20.0f;
        float x = tx + cosf(a) * r, y = ty + sinf(a) * r;
        float h = vw_height(w, x * VW_TEXM, y * VW_TEXM);
        if (h < best) {
            best = h;
            bx = x;
            by = y;
        }
    }
    w->start[0] = vw_wrap(bx * VW_TEXM);
    w->start[1] = vw_wrap(by * VW_TEXM);
    /* above whatever stands round it */
    float top = 0;
    for (int i = 0; i < 16; i++) {
        float a = (float)i * 0.3927f;
        for (int r = 0; r < 300; r += 50) {
            float h = vw_height(w, w->start[0] + cosf(a) * (float)r, w->start[1] + sinf(a) * (float)r);
            if (h > top) top = h;
        }
    }
    w->start[2] = top + 60.0f;
    w->start_yaw = atan2f(ty - by, tx - bx);
}

bool vw_generate(vw_world_t *w, int biome, uint32_t seed, int hour, vw_split_fn split, volatile int *progress)
{
    if (!w->lev[0]) return false;
    gen_t g;
    memset(&g, 0, sizeof g);
    g.w = w;
    g.seed = seed * 2654435761u + 12345u;
    g.progress = progress;
    w->biome = biome % VW_BIOMES;
    w->seed = seed;
    set_hour(w, hour % VW_HOURS);
    switch (w->biome) {
    case VW_CANONES: g.wl = 33.0f; w->fog_near = 300; w->fog_far = 1100; w->clouds = 0.25f; break;
    case VW_NIEVE: g.wl = 46.0f; w->fog_near = 250; w->fog_far = 1000; w->clouds = 0.55f; break;
    case VW_VOLCAN: g.wl = 20.0f; w->fog_near = 220; w->fog_far = 1000; w->clouds = 0.45f; break;
    case VW_CIUDAD: g.wl = 17.0f; w->fog_near = 250; w->fog_far = 1000; w->clouds = 0.35f; break;
    default: g.wl = 24.0f; w->fog_near = 300; w->fog_far = 1100; w->clouds = 0.40f; break;
    }
    if (hour == VW_NIGHT) {
        w->fog_near *= 0.6f;
        w->fog_far *= 0.8f;
    }
    w->water = g.wl;
    perm_make(g.seed);
    g.hq = (uint16_t *)vu_malloc((size_t)N * N * 2);
    if (!g.hq) return false;
    g.kind = (uint8_t *)w->lev[1];          /* 1 MB: level 1 is made last */
    g.prog0 = 0;
    g.prog1 = 45;
    uint32_t tq0 = vr_now_us();
    g.hh = (float *)vu_malloc((size_t)NH * NH * 4);
    g.kh = (uint8_t *)vu_malloc((size_t)NH * NH);
    g.pat = (int8_t *)vu_malloc(256 * 256);
    g.warp = (float *)vu_malloc(sizeof(float) * WG * WG * 2);
    g.detail = (float *)vu_malloc(sizeof(float) * 128 * 128);
    if (!g.hh || !g.kh || !g.pat || !g.warp || !g.detail) {
        vu_free(g.warp);
        vu_free(g.detail);
        vu_free(g.hh);
        vu_free(g.kh);
        vu_free(g.pat);
        vu_free(g.hq);
        return false;
    }
    /* the patches: two octaves of 32 and 16 texels (8 and 4 of the table) */
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            float v = perlin((float)x / 8.0f, (float)y / 8.0f, 32, g.seed + 6) +
                      0.5f * perlin((float)x / 4.0f, (float)y / 4.0f, 64, g.seed + 7);
            int q = (int)(v / 1.5f * 160.0f);
            g.pat[y * 256 + x] = (int8_t)(q < -127 ? -127 : q > 127 ? 127 : q);
        }
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++)
            g.detail[y * 128 + x] = perlin((float)x * 0.25f, (float)y * 0.25f, 32, g.seed + 999);
    /* the warp: as each world bends its shapes */
    {
        float amp = 0, xo = 0, yo = 0;
        int cell = 256;
        uint32_t s1 = 0, s2 = 0;
        switch (w->biome) {
        case VW_ISLAS: amp = 60, cell = 256, s1 = 11, s2 = 12, xo = 300; break;
        case VW_CANONES: amp = 30, cell = 128, s1 = 21, s2 = 22, yo = 200; break;
        case VW_NIEVE: amp = 40, cell = 256, s1 = 31, s2 = 32, yo = 500; break;
        case VW_CIUDAD: amp = 7, cell = 256, s1 = 1, s2 = 2; break;
        default: break;
        }
        for (int y = 0; y < WG; y++)
            for (int x = 0; x < WG; x++) {
                float px = (float)(x * 8), py = (float)(y * 8);
                g.warp[y * WG + x] = amp ? amp * fbm(px + xo, py, cell, 3, 0.5f, g.seed + s1) : 0;
                g.warp[WG * WG + y * WG + x] = amp ? amp * fbm(px, py + yo, cell, 3, 0.5f, g.seed + s2) : 0;
            }
    }
    if (w->biome != VW_CIUDAD) {
        s_mincell = 4;
        g.prog1 = 35;
        if (!split || !split(stage_half, &g)) {
            stage_half(&g, 0);
            stage_half(&g, 1);
        }
        s_mincell = 2;
        g.prog0 = 35;
        g.prog1 = 45;
    }
    if (!split || !split(stage_heights, &g)) {
        stage_heights(&g, 0);
        stage_heights(&g, 1);
    }
    uint32_t tq1 = vr_now_us();
    stage_trees(&g);
    if (progress) *progress = 55;
    if (!split || !split(stage_shadows, &g)) {
        stage_shadows(&g, 0);
        stage_shadows(&g, 1);
    }
    g.prog0 = 60;
    g.prog1 = 95;
    uint32_t tq2 = vr_now_us();
    if (!split || !split(stage_colours, &g)) {
        stage_colours(&g, 0);
        stage_colours(&g, 1);
    }
    uint32_t tq3 = vr_now_us();
    vu_free(g.hq);
    vu_free(g.hh);
    vu_free(g.kh);
    vu_free(g.pat);
    vu_free(g.warp);
    vu_free(g.detail);
    for (int l = 1; l < VW_LEVELS; l++) make_level(w, l);
    uint32_t tq4 = vr_now_us();
    w->gen_ms[0] = (tq1 - tq0) / 1000;
    w->gen_ms[1] = (tq2 - tq1) / 1000;
    w->gen_ms[2] = (tq3 - tq2) / 1000;
    w->gen_ms[3] = (tq4 - tq3) / 1000;
    int hx = 0;
    for (int i = 0; i < N * N; i++) {
        int h = vw_h(w->lev[0][i]);
        if (h > hx) hx = h;
    }
    w->hmax = (float)hx / VW_HU;
    vw_clouds_make(seed + 3, w->clouds);
    find_start(&g);
    if (progress) *progress = 100;
    return true;
}

float vw_height(const vw_world_t *w, float x, float y)
{
    float fx = x / VW_TEXM - 0.5f, fy = y / VW_TEXM - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float ax = fx - (float)ix, ay = fy - (float)iy;
    const uint32_t *m = w->lev[0];
    float a = (float)vw_h(m[TIX(ix, iy)]), b = (float)vw_h(m[TIX(ix + 1, iy)]);
    float c = (float)vw_h(m[TIX(ix, iy + 1)]), d = (float)vw_h(m[TIX(ix + 1, iy + 1)]);
    float ab = a + (b - a) * ax, cd = c + (d - c) * ax;
    return (ab + (cd - ab) * ay) / VW_HU;
}

bool vw_is_water(const vw_world_t *w, float x, float y)
{
    int ix = (int)floorf(x / VW_TEXM), iy = (int)floorf(y / VW_TEXM);
    return (w->lev[0][TIX(ix, iy)] & VW_WATER) != 0;
}
