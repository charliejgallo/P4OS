/*
 * VITRAL - drawing on the retro canvas (see vt_gfx.h)
 */
#include "vt_gfx.h"
#include "vt_game.h"

#include <math.h>
#include <string.h>

const int8_t vt_sin[256] = {
       0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

#define LEAD_DARK   0x1C1D23
#define LEAD_MID    0x2E3038
#define LEAD_RIDGE  0x454852

uint32_t vt_glass_rgb(int color)
{
    switch (color) {
    case VT_RUBY:     return 0xC8203C;
    case VT_AMBER:    return 0xF0A01E;
    case VT_EMERALD:  return 0x1EAA5A;
    case VT_SAPPHIRE: return 0x2A5CE6;
    case VT_CLEAR:    return 0xC4DCEC;
    case VT_FROST:    return 0x6A7082;
    case VT_STONE:    return 0x3E3A40;
    default:          return 0x101222;
    }
}

void vt_surf_init(vt_surf_t *s, uint16_t *px, int cw, int ch, int ox, int oy, int w, int h, bool flip)
{
    s->px = px;
    s->cw = (int16_t)cw;
    s->ch = (int16_t)ch;
    s->ox = (int16_t)ox;
    s->oy = (int16_t)oy;
    s->w = (int16_t)w;
    s->h = (int16_t)h;
    s->flip = flip;
}

vt_surf_t vt_sub(const vt_surf_t *s, int x, int y, int w, int h)
{
    vt_surf_t o = *s;
    o.w = (int16_t)w;
    o.h = (int16_t)h;
    if (s->flip) {
        o.ox = (int16_t)(s->ox + s->w - x - w);
        o.oy = (int16_t)(s->oy + s->h - y - h);
    } else {
        o.ox = (int16_t)(s->ox + x);
        o.oy = (int16_t)(s->oy + y);
    }
    return o;
}

vt_rect_t vt_surf_rect(const vt_surf_t *s, int x, int y, int w, int h)
{
    vt_rect_t r = { 0, 0, 0, 0 };
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return r;
    int X = s->flip ? s->ox + s->w - (x + w) : s->ox + x;
    int Y = s->flip ? s->oy + s->h - (y + h) : s->oy + y;
    if (X < 0) { w += X; X = 0; }
    if (Y < 0) { h += Y; Y = 0; }
    if (X + w > s->cw) w = s->cw - X;
    if (Y + h > s->ch) h = s->ch - Y;
    if (w <= 0 || h <= 0) return r;
    r.x = (int16_t)X;
    r.y = (int16_t)Y;
    r.w = (int16_t)w;
    r.h = (int16_t)h;
    return r;
}

vt_rect_t vt_fill(const vt_surf_t *s, int x, int y, int w, int h, uint16_t c)
{
    vt_rect_t r = vt_surf_rect(s, x, y, w, h);
    for (int yy = 0; yy < r.h; yy++) {
        uint16_t *p = s->px + (r.y + yy) * s->cw + r.x;
        for (int xx = 0; xx < r.w; xx++) p[xx] = c;
    }
    return r;
}

vt_rect_t vt_tint(const vt_surf_t *s, int x, int y, int w, int h, uint16_t c, int f)
{
    vt_rect_t r = vt_surf_rect(s, x, y, w, h);
    for (int yy = 0; yy < r.h; yy++) {
        uint16_t *p = s->px + (r.y + yy) * s->cw + r.x;
        for (int xx = 0; xx < r.w; xx++) p[xx] = vt_mix(p[xx], c, f);
    }
    return r;
}

vt_rect_t vt_glow(const vt_surf_t *s, int x, int y, int w, int h, uint16_t c, int f)
{
    vt_rect_t r = vt_surf_rect(s, x, y, w, h);
    for (int yy = 0; yy < r.h; yy++) {
        uint16_t *p = s->px + (r.y + yy) * s->cw + r.x;
        for (int xx = 0; xx < r.w; xx++) p[xx] = vt_add(p[xx], c, f);
    }
    return r;
}

void vt_copy(uint16_t *dst, const uint16_t *src, int cw, vt_rect_t r)
{
    for (int yy = 0; yy < r.h; yy++) {
        size_t o = (size_t)(r.y + yy) * (size_t)cw + (size_t)r.x;
        memcpy(dst + o, src + o, (size_t)r.w * 2);
    }
}

/* -------------------------------------------------------------------------- */
/* Glass                                                                       */

/* One pixel of glass at (px, py) of the well: the colour, waves that run on
 * across the piece, a streak now and then, a grain. 'shade' is extra light. */
static uint16_t glass_px(uint32_t rgb, int color, int px, int py, uint32_t seed, int shade)
{
    int s = 208 + shade;
    if (color != VT_STONE) {
        s += vt_sin[(px * 3 + py * 5 + (int)seed) & 255] / 5;
        s += vt_sin[(px * 7 - py * 2 + (int)seed * 3) & 255] / 12;
    }
    if (color != VT_FROST && color != VT_STONE) {
        int k = (px - py + (int)seed) & 31;
        if (k == 0) s += 52;
        else if (k == 1 || k == 31) s += 22;
    }
    int grain = (int)(vt_hash((uint32_t)(px * 131 + py * 977) + seed) & 15) - 8;
    s += color == VT_FROST || color == VT_STONE ? grain * 2 : grain;
    int r = (int)((rgb >> 16) & 255) * s / 208;
    int g = (int)((rgb >> 8) & 255) * s / 208;
    int b = (int)(rgb & 255) * s / 208;
    if (s > 236) {
        /* past full colour the light goes white, as through thin glass */
        int w = (s - 236) * 2;
        r += w;
        g += w;
        b += w;
    }
    return vt_rgb(r, g, b);
}

void vt_glass_cell(const vt_surf_t *s, int x, int y, int c, int color, uint8_t lead,
                   int gx, int gy, uint32_t seed, int light)
{
    uint32_t rgb = vt_glass_rgb(color);
    const int L = c >= 18 ? 2 : 1;
    const uint16_t ldark = vt_hex(LEAD_DARK), lmid = vt_hex(LEAD_MID), lridge = vt_hex(LEAD_RIDGE);
    seed = vt_hash(seed) & 255;
    for (int ly = 0; ly < c; ly++) {
        for (int lx = 0; lx < c; lx++) {
            int depth = 99;
            if ((lead & VT_L_TOP) && ly < L && ly < depth) depth = ly;
            if ((lead & VT_L_BOTTOM) && ly >= c - L && c - 1 - ly < depth) depth = c - 1 - ly;
            if ((lead & VT_L_LEFT) && lx < L && lx < depth) depth = lx;
            if ((lead & VT_L_RIGHT) && lx >= c - L && c - 1 - lx < depth) depth = c - 1 - lx;
            if ((lead & VT_L_TL) && lx < L && ly < L) depth = lx < ly ? lx : ly;
            if ((lead & VT_L_TR) && lx >= c - L && ly < L) depth = 0;
            if ((lead & VT_L_BR) && lx >= c - L && ly >= c - L) depth = 0;
            if ((lead & VT_L_BL) && lx < L && ly >= c - L) depth = 0;
            uint16_t px;
            if (depth < L) {
                if (color == VT_STONE) px = vt_hex(0x2A2622);
                else px = L == 1 ? lmid : (depth == 0 ? ldark : lridge);
                if (light > 40) px = vt_add(px, vt_hex(0x605848), light - 40);
            } else {
                int sh = light;
                /* a bevel along the lead: lit above and left, shaded below
                 * and right, so each piece is a cushion of glass */
                if (lead & VT_L_TOP) sh += ly == L ? 44 : ly == L + 1 ? 18 : 0;
                if (lead & VT_L_LEFT) sh += lx == L ? 34 : lx == L + 1 ? 14 : 0;
                if (lead & VT_L_BOTTOM) sh -= ly == c - L - 1 ? 44 : ly == c - L - 2 ? 18 : 0;
                if (lead & VT_L_RIGHT) sh -= lx == c - L - 1 ? 34 : lx == c - L - 2 ? 14 : 0;
                if (color == VT_STONE) {
                    /* carved stone: the joints at every cell */
                    if (lx == 0 || ly == 0) sh -= 70;
                    else if (lx == 1 || ly == 1) sh += 26;
                    else if (lx == c - 1 || ly == c - 1) sh -= 30;
                }
                px = glass_px(rgb, color, gx + lx, gy + ly, seed, sh);
            }
            vt_put(s, x + lx, y + ly, px);
        }
    }
}

void vt_empty_cell(const vt_surf_t *s, int x, int y, int c, int gx, int gy, bool target, uint8_t edge)
{
    const int P = c * 2;
    for (int ly = 0; ly < c; ly++) {
        for (int lx = 0; lx < c; lx++) {
            int px = gx + lx, py = gy + ly;
            /* the night through clear quarries: a diamond lattice */
            int a = (px + py) % P, b = ((px - py) % P + P) % P;
            bool line = a == 0 || b == 0;
            int lift = 14 - py / 40;            /* a little moonlight at the top */
            if (lift < 0) lift = 0;
            int r, g, bl;
            if (target) {
                r = 40; g = 42; bl = 78;
                if (line) { r += 22; g += 22; bl += 34; }
                int k = ((px * 5 + py * 3) & 63) < 2 ? 10 : 0;
                r += k; g += k; bl += k;
            } else {
                r = 11 + lift / 2; g = 13 + lift / 2; bl = 27 + lift;
                if (line) { r += 11; g += 12; bl += 22; }
            }
            int n = (int)(vt_hash((uint32_t)(px * 31 + py * 1013)) & 3);
            r += n; g += n; bl += n;
            uint16_t col = vt_rgb(r, g, bl);
            if (target && edge) {
                bool e = ((edge & VT_L_TOP) && ly == 0) || ((edge & VT_L_BOTTOM) && ly == c - 1) ||
                         ((edge & VT_L_LEFT) && lx == 0) || ((edge & VT_L_RIGHT) && lx == c - 1);
                if (e) col = vt_hex(0xB89A52);
            }
            vt_put(s, x + lx, y + ly, col);
        }
    }
}

void vt_ghost_cell(const vt_surf_t *s, int x, int y, int c, int color, uint8_t lead)
{
    uint32_t rgb = vt_glass_rgb(color);
    uint16_t edge = vt_rgb((int)((rgb >> 16) & 255) * 2 / 3 + 90, (int)((rgb >> 8) & 255) * 2 / 3 + 90,
                           (int)(rgb & 255) * 2 / 3 + 90);
    uint16_t fill = vt_hex(rgb);
    for (int ly = 0; ly < c; ly++) {
        for (int lx = 0; lx < c; lx++) {
            bool e = ((lead & VT_L_TOP) && ly < 2) || ((lead & VT_L_BOTTOM) && ly >= c - 2) ||
                     ((lead & VT_L_LEFT) && lx < 2) || ((lead & VT_L_RIGHT) && lx >= c - 2);
            uint16_t under = vt_get(s, x + lx, y + ly);
            vt_put(s, x + lx, y + ly, e ? vt_mix(under, edge, 215) : vt_mix(under, fill, 56));
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Stone                                                                       */

void vt_wall(const vt_surf_t *s)
{
    const int BH = 18, BW = 34;
    int cx = s->w / 2, cy = s->h / 2;
    for (int y = 0; y < s->h; y++) {
        int row = y / BH, ly = y % BH;
        for (int x = 0; x < s->w; x++) {
            int xo = x + (row & 1) * (BW / 2) + 7;
            int col = xo / BW, lx = xo % BW;
            int r, g, b;
            if (ly == 0 || lx == 0) {
                r = 12; g = 11; b = 16;
            } else {
                uint32_t h = vt_hash((uint32_t)(row * 977 + col * 131 + 7));
                int v = 30 + (int)(h & 15);
                r = v; g = v - 2; b = v + 9;
                if (ly == 1) { r += 12; g += 12; b += 12; }
                else if (ly == BH - 1) { r -= 8; g -= 8; b -= 8; }
                if (lx == 1) { r += 6; g += 6; b += 6; }
                int n = (int)(vt_hash((uint32_t)(x * 73 + y * 1031)) & 7) - 3;
                r += n; g += n; b += n;
            }
            /* darker towards the corners */
            int dx = x - cx, dy = y - cy;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            int v = dx * 256 / (cx ? cx : 1) + dy * 256 / (cy ? cy : 1);
            if (v > 200) {
                int k = (v - 200) / 6;
                if (k > 20) k = 20;
                r -= k; g -= k; b -= k;
            }
            vt_put(s, x, y, vt_rgb(r, g, b));
        }
    }
}

static uint16_t frame_stone(int x, int y, int lit)
{
    int v = 84 + (int)(vt_hash((uint32_t)(x * 59 + y * 701)) & 7) + lit;
    return vt_rgb(v, v - 7, v + 6);
}

/* inside a pointed arch of span w over the springing line, height h:
 * (u, v) from its left foot, v upwards */
static bool in_arch(int u, int v, int w, int h)
{
    if (u < 0 || u > w || v < 0) return false;
    /* an equilateral arch squashed to the height asked */
    float fv = (float)v * (0.866f * (float)w) / (float)(h > 0 ? h : 1);
    float fu = (float)u, fw = (float)w;
    return (fu - fw) * (fu - fw) + fv * fv <= fw * fw && fu * fu + fv * fv <= fw * fw;
}

void vt_frame(const vt_surf_t *s, int x, int y, int w, int h, int arch, bool oculus)
{
    const int F = 5;
    int top = y - arch - F - 2;
    for (int py = top; py < y + h + F; py++) {
        for (int px = x - F; px < x + w + F; px++) {
            if (px >= x && px < x + w && py >= y && py < y + h) continue;
            if (py < y) {
                bool inner = in_arch(px - x, y - py, w, arch);
                bool outer = in_arch(px - (x - F), y - py, w + 2 * F, arch + F);
                if (inner) {
                    /* the arch's glass: the night, and a little round window */
                    int ocx = x + w / 2, ocy = y - arch * 45 / 100;
                    int orad = arch * 30 / 100;
                    if (orad > w / 8) orad = w / 8;
                    int dx = px - ocx, dy = py - ocy, d2 = dx * dx + dy * dy;
                    if (oculus && orad >= 6 && d2 <= orad * orad) {
                        int rr = orad - 2;
                        bool lead = d2 >= rr * rr || dx == 0 || dy == 0;
                        if (lead) vt_put(s, px, py, vt_hex(LEAD_MID));
                        else {
                            int q = (dx > 0) ^ (dy > 0);
                            vt_put(s, px, py, glass_px(q ? 0xC8203C : 0xF0A01E, VT_RUBY, px, py, 17, 20));
                        }
                    } else {
                        int P = 24;
                        int a = (px + py) % P, b = ((px - py) % P + P) % P;
                        bool line = a == 0 || b == 0;
                        vt_put(s, px, py, line ? vt_rgb(22, 25, 49) : vt_rgb(12, 14, 30));
                    }
                } else if (outer) {
                    /* the arch's stone, lit along its inner edge */
                    bool edge = !in_arch(px - x + 1, y - py - 1, w, arch) && in_arch(px - x, y - py - 2, w, arch);
                    vt_put(s, px, py, frame_stone(px, py, edge ? 24 : 0));
                }
                continue;
            }
            int lit = 0;
            if (px == x - 1 || py == y + h) lit = -18;      /* the joint along the glass */
            else if (px == x - F || px == x + w + F - 1 || py == y + h + F - 1) lit = -10;
            else if (px == x + w) lit = 20;
            vt_put(s, px, py, frame_stone(px, py, lit));
        }
    }
}

void vt_box(const vt_surf_t *s, int x, int y, int w, int h)
{
    for (int py = y - 3; py < y + h + 3; py++) {
        for (int px = x - 3; px < x + w + 3; px++) {
            bool in = px >= x && px < x + w && py >= y && py < y + h;
            if (in) vt_put(s, px, py, vt_rgb(10, 11, 22));
            else vt_put(s, px, py, frame_stone(px, py, (py == y - 3 || px == x - 3) ? 16 : -6));
        }
    }
}

/* -------------------------------------------------------------------------- */
/* The rose window                                                             */

void vt_rose(const vt_surf_t *s, int cx, int cy, int r)
{
    static const uint32_t C[4] = { 0xC8203C, 0x2A5CE6, 0x1EAA5A, 0xF0A01E };
    const float R = (float)r;
    const float TAU = 6.2831853f;
    for (int py = cy - r - 8; py <= cy + r + 8; py++) {
        for (int px = cx - r - 8; px <= cx + r + 8; px++) {
            float dx = (float)(px - cx) + 0.5f, dy = (float)(py - cy) + 0.5f;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > R + 8.0f) continue;
            if (d > R) {
                int lit = d < R + 2.0f ? -16 : (dy < 0 ? 18 : -6);
                vt_put(s, px, py, frame_stone(px, py, lit));
                continue;
            }
            float a = atan2f(dy, dx) / TAU + 0.5f;     /* 0..1 */
            float q = d / R;
            uint32_t rgb = C[3];
            int seed = 0, color = VT_RUBY, light = (int)(48.0f * (1.0f - q));
            bool lead = false;
            float ringw = 1.6f;
            if (q < 0.16f) {
                /* the heart: a six-petalled star of amber on sapphire */
                float t = a * 6.0f;
                float f = t - (float)(int)t;
                float petal = 0.16f * R * (0.55f + 0.45f * (1.0f - fabsf(f - 0.5f) * 2.0f));
                rgb = d < petal ? C[3] : C[1];
                seed = d < petal ? 3 : 4;
                lead = fabsf(d - petal) < 1.2f || d > 0.16f * R - ringw;
            } else if (q < 0.48f) {
                int sct = (int)(a * 8.0f);
                float f = a * 8.0f - (float)sct;
                rgb = C[sct & 1 ? 1 : 0];
                seed = 10 + sct;
                float arc = fabsf(f - 0.5f) * 2.0f;     /* 1 at the sector's edges */
                /* a medallion in each */
                float mr = 0.32f * R, ma = ((float)sct + 0.5f) / 8.0f * TAU - TAU / 2.0f;
                float mx = cosf(ma) * mr, my = sinf(ma) * mr;
                float md = sqrtf((dx - mx) * (dx - mx) + (dy - my) * (dy - my));
                if (md < 0.085f * R) {
                    rgb = C[3];
                    seed = 30 + sct;
                }
                lead = (1.0f - arc) * d * TAU / 16.0f < 1.3f || fabsf(md - 0.085f * R) < 1.2f ||
                       d < 0.16f * R + ringw || d > 0.48f * R - ringw;
            } else if (q < 0.80f) {
                int sct = (int)(a * 16.0f);
                float f = a * 16.0f - (float)sct;
                rgb = C[(sct + 2) & 3];
                seed = 50 + sct;
                /* each a lancet: a pointed top towards the rim */
                float qq = (q - 0.48f) / 0.32f;
                float half = qq < 0.6f ? 0.42f : 0.42f * (1.0f - (qq - 0.6f) / 0.4f);
                bool in = fabsf(f - 0.5f) < half;
                if (!in) { rgb = 0x16182E; seed = 90; }
                float edge = (half - fabsf(f - 0.5f)) * d * TAU / 16.0f;
                lead = (in && edge < 1.3f) || d < 0.48f * R + ringw || d > 0.80f * R - ringw;
            } else {
                int sct = (int)(a * 24.0f);
                float ma = ((float)sct + 0.5f) / 24.0f * TAU - TAU / 2.0f;
                float mr = 0.90f * R;
                float mx = cosf(ma) * mr, my = sinf(ma) * mr;
                float md = sqrtf((dx - mx) * (dx - mx) + (dy - my) * (dy - my));
                float fr = 0.075f * R;
                if (md < fr) {
                    rgb = C[sct & 1 ? 0 : 1];
                    seed = 70 + sct;
                    lead = md > fr - 1.4f;
                } else {
                    /* the stone tracery between the roundels */
                    vt_put(s, px, py, frame_stone(px, py, -20));
                    continue;
                }
                if (d < 0.80f * R + ringw) lead = true;
            }
            uint16_t col = lead ? vt_hex(LEAD_MID) : glass_px(rgb, color, px, py, (uint32_t)seed * 37u, light);
            vt_put(s, px, py, col);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Keys                                                                        */

void vt_key(const vt_surf_t *s, int x, int y, int w, int h, int glyph, bool down)
{
    int cx = w / 2, cy = h / 2;
    int g = (w < h ? w : h) * 3 / 10;
    for (int ly = 0; ly < h; ly++) {
        for (int lx = 0; lx < w; lx++) {
            /* rounded corners */
            int rx = lx < 6 ? 6 - lx : lx > w - 7 ? lx - (w - 7) : 0;
            int ry = ly < 6 ? 6 - ly : ly > h - 7 ? ly - (h - 7) : 0;
            if (rx * rx + ry * ry > 36) continue;
            int lit = ly < 2 ? 22 : ly > h - 3 ? -22 : 0;
            if (down) lit += 26;
            uint16_t c = frame_stone(x + lx, y + ly, lit - 26);
            /* the glyph, glass in the middle */
            int u = lx - cx, v = ly - cy;
            bool in = false;
            switch (glyph) {
            case VT_GLYPH_LEFT: in = u >= -g && u <= g && (v < 0 ? -v : v) <= (u + g) / 2 + 1; break;
            case VT_GLYPH_RIGHT: in = u >= -g && u <= g && (v < 0 ? -v : v) <= (g - u) / 2 + 1; break;
            case VT_GLYPH_DOWN: in = v >= -g && v <= g && (u < 0 ? -u : u) <= (g - v) / 2 + 1; break;
            case VT_GLYPH_DROP:
                in = (v >= -g && v <= g / 2 && (u < 0 ? -u : u) <= (g / 2 - v) / 2 + 1) ||
                     (v > g / 2 + 2 && v <= g && (u < 0 ? -u : u) <= g);
                break;
            case VT_GLYPH_CW:
            case VT_GLYPH_CCW: {
                int d2 = u * u + v * v;
                int ro = g, ri = g * 6 / 10;
                bool ring = d2 <= ro * ro && d2 >= ri * ri;
                /* open at the top right (cw) or top left (ccw), with a head */
                bool gap = glyph == VT_GLYPH_CW ? (u > 0 && v < 0 && u > -v / 2) : (u < 0 && v < 0 && -u > -v / 2);
                in = ring && !gap;
                int hx = glyph == VT_GLYPH_CW ? g * 8 / 10 : -g * 8 / 10, hy = -g / 4;
                int ux = u - hx, vy = v - hy;
                if (vy >= -g / 2 && vy <= 0 && (ux < 0 ? -ux : ux) <= (vy + g / 2)) in = true;
                break;
            }
            default: break;
            }
            if (in) c = glass_px(down ? 0xFFD060 : 0xE8C878, VT_AMBER, x + lx, y + ly, 5, down ? 40 : 0);
            vt_put(s, x + lx, y + ly, c);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Dirty tiles                                                                 */

void vt_dirty_init(vt_dirty_t *d, int cw, int ch)
{
    memset(d, 0, sizeof *d);
    d->cw = (int16_t)cw;
    d->ch = (int16_t)ch;
}

void vt_dirty_clear(vt_dirty_t *d)
{
    memset(d->t, 0, sizeof d->t);
    d->any = false;
}

void vt_dirty_add(vt_dirty_t *d, vt_rect_t r)
{
    if (r.w <= 0 || r.h <= 0) return;
    int x0 = r.x / VT_TILE, x1 = (r.x + r.w - 1) / VT_TILE;
    int y0 = r.y / VT_TILE, y1 = (r.y + r.h - 1) / VT_TILE;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= VT_TILES_X) x1 = VT_TILES_X - 1;
    if (y1 >= VT_TILES_Y) y1 = VT_TILES_Y - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) d->t[y][x >> 3] |= (uint8_t)(1u << (x & 7));
    d->any = true;
}

void vt_dirty_or(vt_dirty_t *d, const vt_dirty_t *o)
{
    if (!o->any) return;
    for (int y = 0; y < VT_TILES_Y; y++)
        for (int x = 0; x < VT_TILES_X / 8; x++) d->t[y][x] |= o->t[y][x];
    d->any = true;
}

int vt_dirty_rects(const vt_dirty_t *d, vt_rect_t *out, int max)
{
    if (!d->any) return 0;
    const int BAND = 48 / VT_TILE;
    int tx = (d->cw + VT_TILE - 1) / VT_TILE, ty = (d->ch + VT_TILE - 1) / VT_TILE;
    int n = 0;
    for (int b0 = 0; b0 < ty; b0 += BAND) {
        int b1 = b0 + BAND > ty ? ty : b0 + BAND;
        /* which columns of tiles are marked in this band, and its rows */
        uint8_t colm[VT_TILES_X];
        int r0 = -1, r1 = -1;
        for (int x = 0; x < tx; x++) colm[x] = 0;
        for (int y = b0; y < b1; y++) {
            bool row = false;
            for (int x = 0; x < tx; x++) {
                if (d->t[y][x >> 3] & (1u << (x & 7))) {
                    colm[x] = 1;
                    row = true;
                }
            }
            if (row) {
                if (r0 < 0) r0 = y;
                r1 = y;
            }
        }
        if (r0 < 0) continue;
        /* the runs of marked columns; keep the two with the widest gap
         * between them apart, merge the rest */
        int first = -1, last = -1, gap0 = -1, gap1 = -1, gapw = 0;
        for (int x = 0; x < tx; x++) {
            if (!colm[x]) continue;
            if (first < 0) first = x;
            else if (x - last - 1 > gapw) {
                gapw = x - last - 1;
                gap0 = last;
                gap1 = x;
            }
            last = x;
        }
        int segs = gapw >= 3 ? 2 : 1;
        for (int k = 0; k < segs && n < max; k++) {
            int a = k == 0 ? first : gap1, z = (segs == 2 && k == 0) ? gap0 : last;
            vt_rect_t r;
            r.x = (int16_t)(a * VT_TILE);
            r.y = (int16_t)(r0 * VT_TILE);
            r.w = (int16_t)((z - a + 1) * VT_TILE);
            r.h = (int16_t)((r1 - r0 + 1) * VT_TILE);
            if (r.x + r.w > d->cw) r.w = (int16_t)(d->cw - r.x);
            if (r.y + r.h > d->ch) r.h = (int16_t)(d->ch - r.y);
            out[n++] = r;
        }
    }
    return n;
}
