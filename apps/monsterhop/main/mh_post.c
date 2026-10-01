/*
 * MONSTER HOP - the zones' airborne bits (see mh_post.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mh_post.h"
#include "mh_art.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sizes and speeds in screen pixels times the art's scale */
#define P MH_PX

enum { B_ASH = 1, B_MOTE, B_SAND, B_FIREFLY, B_LEAF, B_EMBER, B_PLANKTON, B_BUBBLE };

/* ---- the zone's bits ---- */

typedef struct {
    uint8_t kind, count;
} mix_t;

static int zone_mix(int zone, mix_t *m)
{
    switch (zone) {
    case ZONE_CITY: m[0] = (mix_t){ B_ASH, 45 }; return 1;
    case ZONE_CASTLE: m[0] = (mix_t){ B_MOTE, 36 }; return 1;
    case ZONE_DESERT: m[0] = (mix_t){ B_SAND, 55 }; return 1;
    case ZONE_FOREST: m[0] = (mix_t){ B_FIREFLY, 26 }; m[1] = (mix_t){ B_LEAF, 16 }; return 2;
    case ZONE_DINO: m[0] = (mix_t){ B_EMBER, 42 }; m[1] = (mix_t){ B_ASH, 22 }; return 2;
    case ZONE_BAY: m[0] = (mix_t){ B_PLANKTON, 44 }; m[1] = (mix_t){ B_BUBBLE, 12 }; return 2;
    default: return 0;
    }
}

static float frand(uint32_t *s)
{
    return mh_randf(s);
}

static void spawn(mh_post_t *p, int i, int kind, int cam_x, int cam_y, int w, int h, bool anywhere)
{
    uint32_t *r = &p->rnd;
    p->bit[i].kind = (uint8_t)kind;
    p->bit[i].x = cam_x - 40 * P + frand(r) * (w + 80 * P);
    p->bit[i].y = cam_y - 40 * P + frand(r) * (h + 80 * P);
    p->bit[i].t = anywhere ? frand(r) * 3.0f : 0;
    p->bit[i].phase = frand(r) * 6.28f;
    float life = 4.0f + frand(r) * 5.0f;
    float vx = 0, vy = 0;
    switch (kind) {
    case B_ASH: vx = 6 + frand(r) * 8; vy = 10 + frand(r) * 10; break;
    case B_MOTE: vx = -4 + frand(r) * 8; vy = -(8 + frand(r) * 10); break;
    case B_SAND: vx = 160 + frand(r) * 120; vy = 6 + frand(r) * 10; life = 1.2f + frand(r) * 1.5f; break;
    case B_FIREFLY: vx = -12 + frand(r) * 24; vy = -12 + frand(r) * 24; break;
    case B_LEAF: vx = 14 + frand(r) * 16; vy = 22 + frand(r) * 18; break;
    case B_EMBER: vx = -6 + frand(r) * 12; vy = -(22 + frand(r) * 26); life = 2.5f + frand(r) * 3.0f; break;
    case B_PLANKTON: vx = -4 + frand(r) * 8; vy = -3 + frand(r) * 6; life = 5.0f + frand(r) * 6.0f; break;
    case B_BUBBLE: vx = -2 + frand(r) * 4; vy = -(18 + frand(r) * 14); life = 2.0f + frand(r) * 2.0f; break;
    default: break;
    }
    p->bit[i].vx = vx * P;
    p->bit[i].vy = vy * P;
    p->bit[i].life = life;
}

/* light added around a point: a soft disc that saturates */
static void glow(mh_img_t *im, int cx, int cy, int r, uint32_t rgb, int a)
{
    if (a <= 0) return;
    uint16_t c = mh_hex(rgb);
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < im->cy0 || y >= im->cy1) continue;
        uint16_t *row = im->px + (size_t)y * im->w;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < im->cx0 || x >= im->cx1) continue;
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            int k = a * (r * r - d2) / (r * r);
            row[x] = mh_add(row[x], mh_scale(c, k));
        }
    }
}

void mhp_step(mh_post_t *p, int zone, int cam_x, int cam_y, int w, int h, float dt)
{
    mix_t mix[2];
    int nm = zone_mix(zone, mix);
    if (!p->rnd) p->rnd = 0x51ED270Bu;
    p->clock += dt;
    int want = 0;
    for (int k = 0; k < nm; k++) want += mix[k].count;
    if (want > MHP_BITS) want = MHP_BITS;
    /* the population of the zone: the first ones of each kind in order */
    if (p->n != want) {
        int i = 0;
        for (int k = 0; k < nm; k++)
            for (int c = 0; c < mix[k].count && i < want; c++, i++) spawn(p, i, mix[k].kind, cam_x, cam_y, w, h, true);
        p->n = want;
    }
    for (int i = 0; i < p->n; i++) {
        float *bx = &p->bit[i].x, *by = &p->bit[i].y;
        p->bit[i].t += dt;
        float wob = sinf(p->clock * 1.7f + p->bit[i].phase);
        int kind = p->bit[i].kind;
        *bx += (p->bit[i].vx + (kind == B_FIREFLY || kind == B_PLANKTON ? wob * 10 : kind == B_EMBER ? wob * 14 : 0) * P) * dt;
        *by += p->bit[i].vy * dt;
        bool out = *bx < cam_x - 60 * P || *bx > cam_x + w + 60 * P || *by < cam_y - 60 * P || *by > cam_y + h + 60 * P;
        if (p->bit[i].t > p->bit[i].life || out) {
            spawn(p, i, kind, cam_x, cam_y, w, h, false);
            continue;
        }
    }
}

void mhp_draw(const mh_post_t *p, int zone, mh_img_t *im, int cam_x, int cam_y, int w, int h)
{
    for (int i = 0; i < p->n; i++) {
        float bx = p->bit[i].x, by = p->bit[i].y;
        float wob = sinf(p->clock * 1.7f + p->bit[i].phase);
        int kind = p->bit[i].kind;
        /* fade in and out over its life */
        float f = p->bit[i].t / p->bit[i].life;
        float fade = f < 0.15f ? f / 0.15f : f > 0.8f ? (1 - f) / 0.2f : 1;
        int x = (int)(bx - cam_x), y = (int)(by - cam_y);
        if (x < -8 * P || x > w + 8 * P || y < -8 * P || y > h + 8 * P) continue;
        if (y + 8 * P < im->cy0 || y - 8 * P >= im->cy1) continue;     /* not in this band */
        switch (kind) {
        case B_ASH:
            mh_rect_blend(im, x, y, 2 * P, 2 * P, mh_hex(zone == ZONE_DINO ? 0x6A5A50 : 0xB8BCC8), (int)(110 * fade));
            break;
        case B_MOTE:
            glow(im, x, y, 3 * P, 0xB070FF, (int)(170 * fade * (0.6f + 0.4f * wob)));
            break;
        case B_SAND:
            mh_rect_blend(im, x, y, 7 * P, P, mh_hex(0xF0D8A8), (int)(90 * fade));
            break;
        case B_FIREFLY: {
            /* they blink */
            float on = sinf(p->clock * 3.1f + p->bit[i].phase * 3.0f);
            if (on > 0) glow(im, x, y, 4 * P, 0xC8FF60, (int)(230 * fade * on));
            break;
        }
        case B_LEAF: {
            bool wide = ((int)(p->clock * 5 + p->bit[i].phase * 4)) & 1;
            mh_rect_blend(im, x, y, (wide ? 4 : 2) * P, (wide ? 2 : 3) * P, mh_hex(0xC8702A), (int)(200 * fade));
            break;
        }
        case B_EMBER:
            glow(im, x, y, 3 * P, 0xFF7A20, (int)(240 * fade));
            mh_rect_blend(im, x, y, P, P, mh_hex(0xFFE0A0), (int)(255 * fade));
            break;
        case B_PLANKTON:
            glow(im, x, y, 3 * P, (p->bit[i].phase > 3.14f) ? 0x40E8FF : 0xFF60D0, (int)(180 * fade * (0.7f + 0.3f * wob)));
            break;
        case B_BUBBLE:
            mh_disc(im, x * 16, y * 16, 40 * P, mh_hex(0xC8F0FF), (int)(90 * fade));
            break;
        default:
            break;
        }
    }
}

void mhp_free(mh_post_t *p)
{
    memset(p, 0, sizeof(*p));
}
