/*
 * DERIVA - each sector's sky, painted once when the sector starts: a
 * dark gradient, a nebula in the sector's colours (value noise, four
 * octaves, worked out on a grid of 8 px and blended), stars, now and then
 * a planet far away, and the deck where the thumbs go. Dithered into
 * RGB565 so the gradients do not band. The compositor copies its pixels
 * back under whatever moved.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "ast_app.h"

#include <math.h>
#include <string.h>

#define CELL 8

static uint32_t hash(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float vnoise(float x, float y, uint32_t seed)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - (float)xi, fy = y - (float)yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = (float)(hash(xi, yi, seed) & 0xFFFF) / 65535.0f;
    float b = (float)(hash(xi + 1, yi, seed) & 0xFFFF) / 65535.0f;
    float c = (float)(hash(xi, yi + 1, seed) & 0xFFFF) / 65535.0f;
    float d = (float)(hash(xi + 1, yi + 1, seed) & 0xFFFF) / 65535.0f;
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float fbm(float x, float y, uint32_t seed)
{
    float v = 0, amp = 0.5f;
    for (int o = 0; o < 4; o++) {
        v += vnoise(x, y, seed + (uint32_t)o * 17u) * amp;
        x *= 2.03f;
        y *= 2.03f;
        amp *= 0.5f;
    }
    return v;
}

static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static inline uint16_t px565(float r, float g, float b, int x, int y)
{
    float d = ((float)BAYER[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f;
    int ri = (int)(r * 31.0f + d), gi = (int)(g * 63.0f + d), bi = (int)(b * 31.0f + d);
    ri = ri < 0 ? 0 : ri > 31 ? 31 : ri;
    gi = gi < 0 ? 0 : gi > 63 ? 63 : gi;
    bi = bi < 0 ? 0 : bi > 31 ? 31 : bi;
    return (uint16_t)((ri << 11) | (gi << 5) | bi);
}

static void rgbf(uint32_t c, float *r, float *g, float *b)
{
    *r = (float)((c >> 16) & 255) / 255.0f;
    *g = (float)((c >> 8) & 255) / 255.0f;
    *b = (float)(c & 255) / 255.0f;
}

static void add565(uint16_t *p, float r, float g, float b)
{
    int ri = ((*p >> 11) & 31) + (int)(r * 31.0f), gi = ((*p >> 5) & 63) + (int)(g * 63.0f), bi = (*p & 31) + (int)(b * 31.0f);
    ri = ri > 31 ? 31 : ri;
    gi = gi > 63 ? 63 : gi;
    bi = bi > 31 ? 31 : bi;
    *p = (uint16_t)((ri << 11) | (gi << 5) | bi);
}

void ast_bg_make(ast_app_t *a, int sector)
{
    uint16_t *bg = a->bg;
    if (!bg) return;
    const ast_layout_t *L = &a->lay;
    int W = L->W, H = L->H;
    uint32_t seed = 0xA5u + (uint32_t)sector * 7919u;
    float nr, ng, nb, ar, ag, ab, sr, sg, sb;
    rgbf(ast_sector_color(sector, 2), &nr, &ng, &nb);
    rgbf(ast_sector_color(sector, 1), &ar, &ag, &ab);
    rgbf(ast_sector_color(sector, 0), &sr, &sg, &sb);

    /* the nebula on a coarse grid, then blended to every pixel */
    int gw = L->fw / CELL + 2, gh = L->fh / CELL + 2;
    float *n1 = (float *)ast_malloc(sizeof(float) * (size_t)gw * (size_t)gh);
    float *n2 = (float *)ast_malloc(sizeof(float) * (size_t)gw * (size_t)gh);
    if (!n1 || !n2) {
        ast_free(n1);
        ast_free(n2);
        memset(bg, 0, (size_t)W * H * 2);
        return;
    }
    float sc = 1.0f / 260.0f;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            float px = (float)(x * CELL) * sc, py = (float)(y * CELL) * sc;
            float w = fbm(px * 0.7f + 3.1f, py * 0.7f + 1.7f, seed + 99);
            float v = fbm(px + w * 1.6f, py + w * 1.2f, seed);
            float v2 = fbm(px * 1.8f + 7.0f, py * 1.8f - 2.0f, seed + 5);
            v = (v - 0.42f) * 2.2f;
            v2 = (v2 - 0.5f) * 2.4f;
            n1[y * gw + x] = v < 0 ? 0 : v > 1 ? 1 : v;
            n2[y * gw + x] = v2 < 0 ? 0 : v2 > 1 ? 1 : v2;
        }
    }
    for (int y = 0; y < L->fh; y++) {
        uint16_t *row = bg + (size_t)(y + L->fy) * W + L->fx;
        int gy = y / CELL;
        float fy = (float)(y % CELL) / (float)CELL;
        float vert = (float)y / (float)L->fh;
        for (int x = 0; x < L->fw; x++) {
            int gx = x / CELL;
            float fx = (float)(x % CELL) / (float)CELL;
            const float *p = n1 + gy * gw + gx;
            float a1 = p[0] + (p[1] - p[0]) * fx, b1 = p[gw] + (p[gw + 1] - p[gw]) * fx;
            float v = a1 + (b1 - a1) * fy;
            p = n2 + gy * gw + gx;
            a1 = p[0] + (p[1] - p[0]) * fx;
            b1 = p[gw] + (p[gw + 1] - p[gw]) * fx;
            float v2 = a1 + (b1 - a1) * fy;
            float k = v * v * 0.55f, k2 = v2 * v2 * v2 * 0.22f;
            float base = 0.018f + 0.02f * vert;
            float r = base * 0.6f + nr * k + ar * k2;
            float g = base * 0.7f + ng * k + ag * k2;
            float b = base * 1.4f + nb * k + ab * k2;
            row[x] = px565(r, g, b, x, y);
        }
    }
    ast_free(n1);
    ast_free(n2);

    /* a planet far off, in some sectors */
    uint32_t s = seed;
    if (sector % 3 == 1 || sector % 4 == 2) {
        float pr = (float)(L->fw < L->fh ? L->fw : L->fh) * (0.22f + 0.12f * ag_rand01(&s));
        float cx = ag_rand01(&s) < 0.5f ? pr * 0.35f : (float)L->fw - pr * 0.35f;
        float cy = (float)L->fh * (0.15f + 0.7f * ag_rand01(&s));
        float lx = -0.6f, ly = -0.7f;
        float pe = pr * 1.08f + 2.0f;
        int y0 = (int)(cy - pe), y1 = (int)(cy + pe);
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < L->fh; y++) {
            uint16_t *row = bg + (size_t)(y + L->fy) * W + L->fx;
            for (int x = (int)(cx - pe) < 0 ? 0 : (int)(cx - pe); x < (int)(cx + pe) && x < L->fw; x++) {
                float dx = ((float)x - cx) / pr, dy = ((float)y - cy) / pr;
                float d2 = dx * dx + dy * dy;
                if (d2 > 1.0f) {
                    /* the haze around it */
                    float h = 1.0f - (sqrtf(d2) - 1.0f) * 14.0f;
                    if (h > 0) add565(&row[x], ar * h * 0.18f, ag * h * 0.18f, ab * h * 0.18f);
                    continue;
                }
                float dz = sqrtf(1.0f - d2);
                float lit = -(dx * lx + dy * ly) * 0.8f + dz * 0.2f;
                lit = lit < 0 ? 0 : lit;
                float band = 0.5f + 0.5f * sinf((dy * 9.0f + fbm(dx * 3.0f, dy * 3.0f, seed + 3) * 3.0f));
                float rim = d2 > 0.9f ? (d2 - 0.9f) * 10.0f * (lit > 0.1f ? 1.0f : 0.3f) : 0;
                float r = 0.02f + (sr * 0.35f + nr * 0.4f * band) * lit * 0.5f + ar * rim * 0.5f;
                float g = 0.02f + (sg * 0.35f + ng * 0.4f * band) * lit * 0.5f + ag * rim * 0.5f;
                float b = 0.03f + (sb * 0.35f + nb * 0.4f * band) * lit * 0.5f + ab * rim * 0.5f;
                row[x] = px565(r, g, b, x, y);
            }
        }
    }

    /* stars: most faint, a few with a glint */
    int nstars = L->fw * L->fh / 1500;
    for (int i = 0; i < nstars; i++) {
        int x = (int)(ag_rand01(&s) * (float)(L->fw - 4)) + 2, y = (int)(ag_rand01(&s) * (float)(L->fh - 4)) + 2;
        float b = ag_rand01(&s);
        b = b * b * b * 0.85f + 0.06f;
        float tr = 0.8f + 0.2f * ag_rand01(&s), tb = 0.85f + 0.15f * ag_rand01(&s);
        uint16_t *p = bg + (size_t)(y + L->fy) * W + L->fx + x;
        add565(p, b * tr, b * 0.95f, b * tb);
        if (b > 0.35f) {
            float h = b * 0.35f;
            add565(p - 1, h, h, h);
            add565(p + 1, h, h, h);
            add565(p - W, h, h, h);
            add565(p + W, h, h, h);
        }
        if (b > 0.6f) {
            for (int k = 2; k < 6; k++) {
                float h = b * 0.25f / (float)(k - 1);
                if (x - k >= 0) add565(p - k, h, h, h);
                if (x + k < L->fw) add565(p + k, h, h, h);
                if (y - k >= 0) add565(p - (ptrdiff_t)k * W, h, h, h);
                if (y + k < L->fh) add565(p + (ptrdiff_t)k * W, h, h, h);
            }
        }
    }

    /* the deck: dark, with a thin line of the sector's colour along the field */
    for (int y = 0; y < H; y++) {
        uint16_t *row = bg + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            bool in_field = x >= L->fx && x < L->fx + L->fw && y >= L->fy && y < L->fy + L->fh;
            if (in_field) continue;
            float d;
            if (L->land) d = x < L->fx ? (float)(L->fx - x) / (float)L->fx : (float)(x - (L->fx + L->fw)) / (float)(W - L->fx - L->fw);
            else d = (float)(y - (L->fy + L->fh)) / (float)(H - L->fy - L->fh);
            float k = 0.035f - 0.02f * d;
            float r = k * 0.7f + nr * 0.05f, g = k * 0.8f + ng * 0.05f, b = k * 1.1f + nb * 0.05f;
            row[x] = px565(r, g, b, x, y);
        }
    }
    if (L->land) {
        for (int y = 0; y < H; y++) {
            for (int j = 0; j < 2; j++) {
                int xs[2] = { L->fx - 1 - j, L->fx + L->fw + j };
                for (int q = 0; q < 2; q++) {
                    uint16_t *p = bg + (size_t)y * W + xs[q];
                    float k = j ? 0.12f : 0.35f;
                    add565(p, sr * k, sg * k, sb * k);
                }
            }
        }
    } else {
        for (int x = 0; x < W; x++) {
            for (int j = 0; j < 2; j++) {
                uint16_t *p = bg + (size_t)(L->fy + L->fh + j) * W + x;
                float k = j ? 0.12f : 0.35f;
                add565(p, sr * k, sg * k, sb * k);
            }
        }
    }
    a->bg_sector = sector;
}
