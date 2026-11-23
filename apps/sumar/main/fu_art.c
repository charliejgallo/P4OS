/*
 * P4OS - Fusión: the themes and the tiles
 *
 * No bitmaps travel with the app. Each tile is drawn the first time it is
 * needed, at the size of a cell of the board in play, pixel by pixel into an
 * ARGB8888 canvas (the shape, its light, its glow or its shadow), then LVGL
 * writes the number on it, and the result is kept as an RGB565A8 sprite: the
 * one format with alpha the drawer blends without converting anything. A
 * game moves those sprites around as LVGL images, so a frame only repaints
 * what moved.
 *
 * The four themes differ in how a tile is lit, not only in colour:
 *
 *   Neón     dark glass with a bright rim and a halo around it
 *   Gemas    cut stones: bevelled edges lit from the top left, a polished
 *            table and a streak of shine, one stone per value
 *   Papel    flat cards of blue ink on cool white paper, with a pen line
 *            and a shadow offset like a cut-out
 *   Bosque   soft rounded pieces in moss going to gold, grained like wood
 */
#include "fu_app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void lv_image_cache_drop(const void *src);

static const fu_theme_t THEMES[FU_THEMES] = {
    { N_("Neón"), FU_ST_NEON,
      0x0D1233, 0x04050E, 0x0E1333, 0x171D4A, 0x121842, 0x2A3380,
      0xEAF2FF, 0x8A93C8, 0x22E5FF, 0xFF3FD8, 0x3A3F66,
      { 0x22E5FF, 0x2BB8FF, 0x3D8BFF, 0x6A63FF, 0x9B4DFF, 0xC63BFF,
        0xF033E6, 0xFF3FB0, 0xFF4F7A, 0xFFE14D, 0x7CFF6B, 0xFFFFFF } },
    { N_("Gemas"), FU_ST_GEM,
      0x22262E, 0x0F1115, 0x1F232A, 0x2C313B, 0x262B34, 0x3C4452,
      0xF2F4F8, 0x98A2B3, 0x3DDC97, 0xE8457B, 0x585E69,
      { 0xC9D3E0, 0x6CCBE6, 0x2E7BFF, 0x17B86C, 0x8A4DFF, 0xE02A48,
        0xF5B81C, 0x14A89C, 0xCC33BE, 0x3346D6, 0xEAF0FF, 0x26262C } },
    { N_("Papel"), FU_ST_PAPER,
      0xF5F7FA, 0xE4E9F0, 0xD5DCE7, 0xE8ECF2, 0xFFFFFF, 0xC8D0DD,
      0x1C2740, 0x66718A, 0x2557AE, 0xC2185B, 0x8E98AB,
      { 0xC4D6F2, 0xA2BDEA, 0x7FA4E0, 0x5D8AD4, 0x3F70C4, 0x2858AE,
        0x1B4492, 0x133374, 0x0E2557, 0x2E1D72, 0x5C1782, 0xB0123B } },
    { N_("Bosque"), FU_ST_FOREST,
      0x241B14, 0x110C08, 0x2A2019, 0x3A2D22, 0x2E241B, 0x4A3A2B,
      0xF3EBDC, 0xB3A48C, 0x8BD36A, 0xE8B84A, 0x77736B,
      { 0x5E7D3A, 0x6E9440, 0x7FAA45, 0x95BF4B, 0xAECC55, 0xC6D25E,
        0xDCCB56, 0xE8B84A, 0xC9833A, 0xB04A32, 0x8C2C50, 0xE9F3F7 } },
};

static struct {
    int         theme, cell;
    fu_sprite_t tile[FU_EXP_MAX + 1];       /* [0] is the stone */
} A = { .theme = -1 };

const fu_theme_t *fu_theme_at(int i)
{
    return &THEMES[(i >= 0 && i < FU_THEMES) ? i : 0];
}

const fu_theme_t *fu_theme(void)
{
    return fu_theme_at(fu_P.theme);
}

uint32_t fu_art_color(uint8_t exp)
{
    const fu_theme_t *t = fu_theme();
    if (exp == FU_BLOCK || exp == 0) return t->block;
    int i = exp - 1;
    if (i > 11) i = 11;
    return t->ramp[i];
}

void fu_value_text(uint8_t exp, char *out, int len)
{
    snprintf(out, (size_t)len, "%lu", (unsigned long)(1ul << exp));
}

/* ---- colour ---- */

typedef struct { float r, g, b; } rgbf_t;

static rgbf_t C(uint32_t hex)
{
    rgbf_t c = { (float)((hex >> 16) & 0xFF), (float)((hex >> 8) & 0xFF), (float)(hex & 0xFF) };
    return c;
}

static rgbf_t mix(rgbf_t a, rgbf_t b, float f)
{
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    rgbf_t c = { a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f };
    return c;
}

static float luma(rgbf_t c)
{
    return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Signed distance to a rounded square centred on the origin: negative inside. */
static float sd_box(float px, float py, float h, float rad)
{
    float qx = fabsf(px) - (h - rad), qy = fabsf(py) - (h - rad);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rad;
}

/* A cheap repeatable noise in -1..1, for the paper and the wood. */
static float hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 32767.5f - 1.0f;
}

/* ---- one pixel of a tile, per style ---- */

typedef struct {
    int   S;                /* sprite side */
    float h, rad, margin;   /* half side of the body, its corner, the space around */
    rgbf_t base;
    float k;                /* 0 for the 2 .. 1 for the top of the ramp */
    bool  stone;
    uint8_t style;
} shape_t;

/* Writes colour and alpha (0..1) of pixel (x, y). */
static void tile_px(const shape_t *s, int x, int y, rgbf_t *out, float *alpha)
{
    float px = (float)x + 0.5f - (float)s->S * 0.5f;
    float py = (float)y + 0.5f - (float)s->S * 0.5f;
    float d = sd_box(px, py, s->h, s->rad);
    float cov = clampf(0.5f - d, 0, 1);
    rgbf_t body = s->base, halo = s->base;
    float halo_a = 0;
    float S = (float)s->S;
    float t = clampf((py + s->h) / (2 * s->h), 0, 1);    /* 0 at the top */

    switch (s->style) {
    case FU_ST_NEON: {
        float bw = S * 0.036f < 3 ? 3 : S * 0.036f;
        rgbf_t rim = mix(s->base, C(0xFFFFFF), 0.30f);
        rgbf_t fill = mix(C(0x080B22), s->base, s->stone ? 0.30f : 0.16f + 0.16f * s->k);
        fill = mix(fill, s->base, 0.10f * (1 - t));
        if (s->stone) {
            float hatch = fmodf(px - py + 1000.0f, S * 0.14f);
            if (hatch < 2.5f) fill = mix(fill, s->base, 0.5f);
            rim = s->base;
        }
        float inner = clampf(-d - bw + 0.5f, 0, 1);     /* 1 well inside the rim */
        body = mix(rim, fill, inner);
        if (!s->stone && d > 0) {
            float f = 1 - d / s->margin;
            halo_a = f > 0 ? (0.35f + 0.35f * s->k) * f * f : 0;
        }
        break;
    }
    case FU_ST_GEM: {
        float bw = S * 0.095f;
        if (s->stone) {
            float n = hash2(x / 3, y / 3) * 0.06f + hash2(x, y) * 0.03f;
            body = mix(s->base, C(n > 0 ? 0xFFFFFF : 0x000000), fabsf(n));
            float crack = fabsf(py - 0.35f * px - S * 0.05f * sinf(px * 0.09f));
            if (crack < 1.6f && fabsf(px) < s->h * 0.7f) body = mix(body, C(0x15171B), 0.7f);
        } else if (d > -bw) {
            /* the bevel: the slope faces outwards; lit from the top left */
            float e = 1.0f;
            float gx = sd_box(px + e, py, s->h, s->rad) - sd_box(px - e, py, s->h, s->rad);
            float gy = sd_box(px, py + e, s->h, s->rad) - sd_box(px, py - e, s->h, s->rad);
            float gl = sqrtf(gx * gx + gy * gy);
            if (gl > 0.0001f) { gx /= gl; gy /= gl; }
            float lit = -0.55f * gx - 0.83f * gy;
            body = lit > 0 ? mix(s->base, C(0xFFFFFF), lit * 0.55f)
                           : mix(s->base, C(0x000000), -lit * 0.50f);
        } else {
            body = mix(mix(s->base, C(0xFFFFFF), 0.18f), mix(s->base, C(0x000000), 0.12f), t);
            float band = (px + py) / S + 0.22f;
            if (band > -0.05f && band < 0.05f) body = mix(body, C(0xFFFFFF), 0.28f * (1 - fabsf(band) / 0.05f));
            float band2 = (px + py) / S + 0.36f;
            if (band2 > -0.02f && band2 < 0.02f) body = mix(body, C(0xFFFFFF), 0.18f);
        }
        if (d > -1.6f && d <= 0) body = mix(body, C(0x000000), 0.35f);
        if (d > 0) {
            float ds = sd_box(px, py - S * 0.025f, s->h, s->rad);
            halo = C(0x000000);
            halo_a = ds < 0 ? 0.40f : clampf(0.40f * (1 - ds / s->margin), 0, 0.40f);
        }
        break;
    }
    case FU_ST_PAPER: {
        float n = hash2(x, y) * 0.025f;
        body = mix(s->base, C(n > 0 ? 0xFFFFFF : 0x000000), fabsf(n));
        if (s->stone) {
            float hatch = fmodf(px + py + 1000.0f, S * 0.11f);
            if (hatch < 2.0f) body = mix(body, C(0x2A3140), 0.55f);
        }
        if (d > -3.0f && d <= 0) body = mix(s->base, C(0x0B1530), 0.45f);
        if (d > 0) {
            float ds = sd_box(px - S * 0.022f, py - S * 0.03f, s->h, s->rad);
            halo = C(0x1C2740);
            halo_a = ds < 0 ? 0.20f : 0;
        }
        break;
    }
    default: {   /* FU_ST_FOREST */
        float grain = sinf(py * 0.35f + 2.2f * sinf(px * 0.045f + (float)(s->k * 7))) * 0.035f
                    + hash2(x, y / 4) * 0.015f;
        body = mix(mix(s->base, C(0xFFFFFF), 0.16f), mix(s->base, C(0x000000), 0.18f), t);
        body = mix(body, C(grain > 0 ? 0xFFFFFF : 0x000000), fabsf(grain));
        if (s->stone) {
            float n = hash2(x / 2, y / 2) * 0.08f;
            body = mix(mix(s->base, C(0xFFFFFF), 0.10f * (1 - t)), C(n > 0 ? 0xFFFFFF : 0), fabsf(n));
        }
        if (d > -4.0f && d <= 0 && py < 0) body = mix(body, C(0xFFFFFF), 0.18f * (1 + d / 4.0f));
        if (d > -2.0f && d <= 0 && py > 0) body = mix(body, C(0x000000), 0.25f);
        if (d > 0) {
            float ds = sd_box(px, py - S * 0.03f, s->h, s->rad);
            halo = C(0x000000);
            halo_a = ds < 0 ? 0.45f : clampf(0.45f * (1 - ds / s->margin), 0, 0.45f);
        }
        break;
    }
    }

    float a = cov + halo_a * (1 - cov);
    if (a <= 0.001f) {
        *alpha = 0;
        out->r = out->g = out->b = 0;
        return;
    }
    out->r = (body.r * cov + halo.r * halo_a * (1 - cov)) / a;
    out->g = (body.g * cov + halo.g * halo_a * (1 - cov)) / a;
    out->b = (body.b * cov + halo.b * halo_a * (1 - cov)) / a;
    *alpha = a;
}

/* ---- the number ---- */

static int text_w(const lv_font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) w += (int)lv_font_get_glyph_width(f, (uint32_t)(uint8_t)*s, (uint32_t)(uint8_t)s[1]);
    return w;
}

static const lv_font_t *fit_font(const char *txt, int box_w, int box_h)
{
    const lv_font_t *fonts[] = {
        &aos_inter_num_144, &aos_inter_num_96, &aos_inter_64, &aos_inter_48,
        &aos_inter_36, &aos_inter_28, &aos_inter_24, &aos_inter_20,
    };
    for (unsigned i = 0; i < sizeof fonts / sizeof fonts[0]; i++) {
        /* the digits' height is about 0.73 of the line */
        int h = (int)lv_font_get_line_height(fonts[i]) * 73 / 100;
        if (text_w(fonts[i], txt) <= box_w && h <= box_h) return fonts[i];
    }
    return &aos_inter_20;
}

static void draw_text(lv_layer_t *layer, const char *txt, const lv_font_t *f, int S,
                      int dx, int dy, uint32_t color, lv_opa_t opa)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.text_local = 1;
    d.font = f;
    d.color = lv_color_hex(color);
    d.opa = opa;
    d.align = LV_TEXT_ALIGN_CENTER;
    int lh = (int)lv_font_get_line_height(f);
    lv_area_t a = { 0, (S - lh) / 2 + dy, S - 1, (S - lh) / 2 + dy + lh - 1 };
    a.x1 += dx;
    a.x2 += dx;
    lv_draw_label(layer, &d, &a);
}

static uint32_t hex_of(rgbf_t c)
{
    return ((uint32_t)clampf(c.r, 0, 255) << 16) | ((uint32_t)clampf(c.g, 0, 255) << 8) |
           (uint32_t)clampf(c.b, 0, 255);
}

/* ---- rendering one sprite ---- */

static bool render(fu_sprite_t *sp, uint8_t exp)
{
    const fu_theme_t *th = fu_theme();
    int S = A.cell;
    bool stone = exp == FU_BLOCK;
    shape_t s;
    memset(&s, 0, sizeof s);
    s.S = S;
    s.style = th->style;
    s.margin = (float)S * 0.045f;
    if (s.margin < 4) s.margin = 4;
    s.h = (float)S * 0.5f - s.margin;
    static const float radk[4] = { 0.16f, 0.12f, 0.09f, 0.24f };
    s.rad = (float)S * radk[th->style];
    s.stone = stone;
    s.base = C(fu_art_color(exp));
    s.k = stone ? 0 : clampf((float)(exp - 1) / 11.0f, 0, 1);

    lv_draw_buf_t *buf = lv_draw_buf_create((uint32_t)S, (uint32_t)S, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!buf) return false;
    uint32_t stride = buf->header.stride;
    for (int y = 0; y < S; y++) {
        uint8_t *row = (uint8_t *)buf->data + (size_t)y * stride;
        for (int x = 0; x < S; x++) {
            rgbf_t c;
            float a;
            tile_px(&s, x, y, &c, &a);
            row[x * 4 + 0] = (uint8_t)clampf(c.b, 0, 255);
            row[x * 4 + 1] = (uint8_t)clampf(c.g, 0, 255);
            row[x * 4 + 2] = (uint8_t)clampf(c.r, 0, 255);
            row[x * 4 + 3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }

    if (!stone) {
        char txt[12];
        fu_value_text(exp, txt, sizeof txt);
        int body = (int)(s.h * 2);
        /* fitted on as many 8s as the value has digits, so every value of
         * the same length gets the same size (a 1 is narrower than a 2) */
        char probe[12];
        int nd = (int)strlen(txt);
        memset(probe, '8', (size_t)nd);
        probe[nd] = 0;
        const lv_font_t *f = fit_font(probe, body * 80 / 100, body * 52 / 100);
        uint32_t ink;
        bool shadow = false;
        lv_opa_t sh_opa = 110;
        switch (th->style) {
        case FU_ST_NEON:
            ink = hex_of(mix(s.base, C(0xFFFFFF), 0.55f));
            shadow = true;          /* here the "shadow" is a halo in the tile's colour */
            break;
        case FU_ST_PAPER:
            ink = luma(s.base) > 0.55f ? 0x16213A : 0xFFFFFF;
            break;
        case FU_ST_GEM:
            ink = luma(s.base) > 0.62f ? 0x1A1D24 : 0xFFFFFF;
            shadow = ink == 0xFFFFFF;
            break;
        default:
            ink = luma(s.base) > 0.62f ? 0x2B2119 : 0xFFF8E6;
            shadow = ink != 0x2B2119;
            break;
        }

        lv_obj_t *cv = lv_canvas_create(lv_layer_top());
        lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
        lv_canvas_set_draw_buf(cv, buf);
        lv_layer_t layer;
        lv_canvas_init_layer(cv, &layer);
        if (th->style == FU_ST_NEON) {
            uint32_t glow = hex_of(s.base);
            draw_text(&layer, txt, f, S, -2, 0, glow, 90);
            draw_text(&layer, txt, f, S, 2, 0, glow, 90);
            draw_text(&layer, txt, f, S, 0, -2, glow, 90);
            draw_text(&layer, txt, f, S, 0, 2, glow, 90);
        } else if (shadow) {
            int o = S / 70 + 1;
            draw_text(&layer, txt, f, S, 0, o, 0x000000, sh_opa);
        }
        /* twice, a pixel apart: Inter Medium reads bolder that way */
        draw_text(&layer, txt, f, S, 0, 0, ink, LV_OPA_COVER);
        draw_text(&layer, txt, f, S, 1, 0, ink, LV_OPA_COVER);
        lv_canvas_finish_layer(cv, &layer);
        lv_obj_delete(cv);
    }

    size_t bytes = (size_t)S * S * 3;
    sp->data = (uint8_t *)malloc(bytes);
    if (!sp->data) {
        lv_draw_buf_destroy(buf);
        return false;
    }
    uint16_t *cp = (uint16_t *)sp->data;
    uint8_t *ap = sp->data + (size_t)S * S * 2;
    for (int y = 0; y < S; y++) {
        const uint8_t *row = (const uint8_t *)buf->data + (size_t)y * stride;
        for (int x = 0; x < S; x++) {
            int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            cp[y * S + x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            ap[y * S + x] = row[x * 4 + 3];
        }
    }
    lv_draw_buf_destroy(buf);

    memset(&sp->dsc, 0, sizeof sp->dsc);
    sp->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    sp->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    sp->dsc.header.w = (uint32_t)S;
    sp->dsc.header.h = (uint32_t)S;
    sp->dsc.header.stride = (uint32_t)(S * 2);
    sp->dsc.data_size = (uint32_t)bytes;
    sp->dsc.data = sp->data;
    return true;
}

void fu_art_free(void)
{
    for (int i = 0; i <= FU_EXP_MAX; i++) {
        fu_sprite_t *sp = &A.tile[i];
        if (sp->data) {
            lv_image_cache_drop(&sp->dsc);
            free(sp->data);
        }
        memset(sp, 0, sizeof *sp);
    }
    A.theme = -1;
    A.cell = 0;
}

void fu_art_setup(int theme, int cell)
{
    if (theme == A.theme && cell == A.cell) return;
    fu_art_free();
    A.theme = theme;
    A.cell = cell;
}

const lv_image_dsc_t *fu_art_tile(uint8_t exp)
{
    int i = exp == FU_BLOCK ? 0 : exp;
    if (exp == 0 || i > FU_EXP_MAX || A.cell <= 0) return NULL;
    fu_sprite_t *sp = &A.tile[i];
    if (!sp->data && !render(sp, exp)) return NULL;
    return &sp->dsc;
}
