/*
 * BASTION - the battle's scene into the frame's list (see bs_view.h)
 *
 * In this order: what lies on the ground (pads, the chosen tower's reach,
 * shadows, rings), then everything that stands, far to near (the fort,
 * towers, aliens, shots in the air), then light (beams, blasts, sparks),
 * then the numbers that float up.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "bs_view.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define MAX_SORT (BS_MAX_ALIENS + BS_PADS * (2 + BS_MAX_DRONES) + BS_MAX_SHOTS + 4)

enum { K_FORT, K_TOWER, K_ALIEN, K_SHOT, K_DRONE };

typedef struct {
    float   y;
    uint8_t kind;
    uint8_t sub;
    int16_t ix;
} sort_t;

static int sx(float x) { return (int)(360.0f + x * BS_PX + 0.5f); }
static int sy(float y, float z) { return (int)(1280.0f - y * BS_ROW_Y - z * BS_ROW_Z + 0.5f); }

static int by_y(const void *a, const void *b)
{
    float d = ((const sort_t *)b)->y - ((const sort_t *)a)->y;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void ground(bs_gfx_t *gf, const bs_ui_t *ui, const bs_game_t *g)
{
    const bs_world_t *w = g->w;
    bool playing = g->state == BS_BUILD || g->state == BS_WAVE;
    /* the free pads breathe while there is something to build */
    /* in ten steps a second: every step is a tile drawn again */
    int pulse = (int)(90.0f + 60.0f * sinf(floorf(ui->t * 10.0f) * 0.3f));
    for (int i = 0; i < w->npad && playing; i++) {
        int x = sx(w->pad_x[i]), y = sy(w->pad_y[i], 0);
        if (i == ui->sel) {
            bs_ring(gf, x, y, 44, 35, 4, 0xFFFF, 230, 0);
        } else if (!g->tw[i].on && g->state == BS_BUILD && ui->sel < 0) {
            bs_ring(gf, x, y, 40, 32, 2, bs_rgb(w->accent), pulse, BS_F_ADD);
        }
        if (ui->pad_on && ui->focus < 0 && i == ui->cursor) bs_ring(gf, x, y, 50, 40, 3, 0xFFFF, 255, 0);
    }
    /* how far the chosen tower reaches, or the one about to be built */
    if (ui->sel >= 0 && ui->sel < w->npad && playing) {
        const bs_tower_t *t = &g->tw[ui->sel];
        int type = t->on ? t->type : ui->pick;
        if (type >= 0) {
            float r = bs_game_range(g, type, t->on ? t->tier : 0);
            if (t->on && t->boost > 1.0f) r *= 1.08f;
            int x = sx(w->pad_x[ui->sel]), y = sy(w->pad_y[ui->sel], 0);
            if (r > 0) {
                uint16_t c = bs_rgb(BS_TOWER[type].accent);
                bs_disc(gf, x, y, (int)(r * BS_PX), (int)(r * BS_ROW_Y), c, 38);
                bs_ring(gf, x, y, (int)(r * BS_PX), (int)(r * BS_ROW_Y), 3, c, 200, 0);
                if (type == TW_MORTAR) bs_ring(gf, x, y, (int)(3.0f * BS_PX), (int)(3.0f * BS_ROW_Y), 2, 0xF800, 160, 0);
            }
        }
    }
    /* shadows: the towers', the aliens', a flier's well below it */
    for (int i = 0; i < w->npad; i++) {
        if (g->tw[i].on) bs_shadow(gf, sx(g->tw[i].x) + 6, sy(g->tw[i].y, 0) + 4, 40, 22, 120);
    }
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        const bs_alien_t *a = &g->al[i];
        if (!a->on) continue;
        const bs_alien_def_t *d = &BS_ALIEN[a->type];
        if (a->cloaked && !a->seen) continue;
        int rx = (int)(d->radius * 38.0f), ry = (int)(d->radius * 20.0f);
        bs_shadow(gf, sx(a->x) + 3, sy(a->y, 0) + 2, rx, ry, d->fly ? 80 : 130);
    }
    bs_shadow(gf, sx(w->fort_x) + 10, sy(w->fort_y, 0) + 8, 118, 78, 120);
    /* rings spread on the ground */
    for (int i = 0; i < BS_MAX_FX; i++) {
        const bs_fx_t *f = &g->fx[i];
        if (!f->on) continue;
        float u = f->t / f->life;
        if (f->kind == FX_RING) {
            float r = f->x1 + (f->r - f->x1) * u;
            int k = (int)(255.0f * (1.0f - u));
            if (r > 0.1f)
                bs_ring(gf, sx(f->x0), sy(f->y0, 0), (int)(r * BS_PX), (int)(r * BS_ROW_Y), r > 6 ? 6 : 4,
                        bs_rgb(f->color), k, BS_F_ADD);
        } else if (f->kind == FX_STRIKE && f->t < 0.55f) {
            /* the mark closing on where it will land */
            float r = f->r * (1.6f - u * 1.6f);
            if (r > 0.2f)
                bs_ring(gf, sx(f->x0), sy(f->y0, 0), (int)(r * BS_PX), (int)(r * BS_ROW_Y), 3, 0xFA00, 255, BS_F_ADD);
            bs_ring(gf, sx(f->x0), sy(f->y0, 0), (int)(f->r * BS_PX), (int)(f->r * BS_ROW_Y), 2, 0xF800, 200, 0);
        }
    }
}

static void tower(bs_gfx_t *gf, const bs_art_t *art, const bs_game_t *g, int i)
{
    const bs_tower_t *t = &g->tw[i];
    const bs_tower_def_t *d = &BS_TOWER[t->type];
    int x = sx(t->x), y = sy(t->y, 0);
    uint16_t z = bs_z_of(t->y);
    int frame;
    if (d->spin) {
        /* a loop: faster while it works */
        float fps = t->type == TW_BANK ? 8.0f : (t->firing || t->flash_t > 0 ? 20.0f : 9.0f);
        if (t->off_t > 0) fps = 0;
        frame = (int)(t->anim * fps) & 15;
    } else {
        frame = (int)(t->yaw / (2 * PI / 16) + 0.5f) & 15;
    }
    bs_spr(gf, &art->tw_base[t->type][t->tier], 0, x, y, BS_F_DEPTH, 255, z, y);
    bs_spr(gf, &art->tw_head[t->type][t->tier], frame, x, y, BS_F_DEPTH, 255, z, y);
}

static void alien(bs_gfx_t *gf, const bs_art_t *art, const bs_game_t *g, int i)
{
    const bs_alien_t *a = &g->al[i];
    const bs_alien_def_t *d = &BS_ALIEN[a->type];
    int x = sx(a->x), foot = sy(a->y, 0), y = foot - (int)(d->alt * BS_ROW_Z);
    /* a flier bobs */
    if (d->alt > 0) y += (int)(3.0f * sinf(a->anim * 1.5f));
    int alpha = 255;
    if (a->cloaked) alpha = a->seen ? 150 : 0;
    else if (a->type == AL_PHASE && a->cloak_t < 0.3f) alpha = 150;
    if (!alpha) return;
    int flags = BS_F_DEPTH | (a->flash_t > 0 ? BS_F_FLASH : 0);
    const bs_sheet_t *s = &art->alien[a->type];
    int frame = a->dir8 + s->cols * ((int)a->anim & 3);
    bs_spr(gf, s, frame, x, y, flags, alpha, bs_z_of(a->y), foot);
}

static void lights(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g)
{
    (void)art;
    /* what the aliens carry: a shield, the cold, the bar of what is left */
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        const bs_alien_t *a = &g->al[i];
        if (!a->on || (a->cloaked && !a->seen)) continue;
        const bs_alien_def_t *d = &BS_ALIEN[a->type];
        int x = sx(a->x), y = sy(a->y, d->alt + d->radius * 0.9f);
        int r = (int)(d->radius * 46.0f);
        if (a->shield > 0) {
            int k = 110 + (int)(100.0f * a->shield / a->maxshield);
            bs_ring(gf, x, y, r + 6, r + 4, 2, 0x47FF, k, BS_F_ADD);
            bs_glow(gf, x, y, r + 6, 0x21DF, 60);
        }
        if (a->stun_t > 0 || g->freeze_t > 0) bs_glow(gf, x, y, r + 4, 0x653F, 130);
        else if (a->slow > 0) bs_glow(gf, x, y + r / 2, r, 0x4D3F, 70);
        if (a->hp < a->maxhp || d->boss) {
            int w = d->boss ? 110 : (int)(d->radius * 60.0f) + 12, h = d->boss ? 8 : 4;
            int top = sy(a->y, d->alt + d->radius * 2.2f + 0.25f) - h;
            float k = a->hp / a->maxhp;
            if (k < 0) k = 0;
            int fill = (int)((float)w * k);
            bs_rect(gf, x - w / 2 - 1, top - 1, w + 2, h + 2, 0x0000, 200, 0);
            uint16_t c = k > 0.5f ? 0x47E8 : k > 0.25f ? 0xFE40 : 0xF986;
            if (fill > 0) bs_rect(gf, x - w / 2, top, fill, h, c, 255, 0);
            if (a->maxshield > 0 && a->shield > 0)
                bs_rect(gf, x - w / 2, top - 4, (int)((float)w * a->shield / a->maxshield), 2, 0x47FF, 255, 0);
        }
    }
    /* the towers' own light */
    for (int i = 0; i < g->w->npad; i++) {
        const bs_tower_t *t = &g->tw[i];
        if (!t->on) continue;
        const bs_tower_def_t *d = &BS_TOWER[t->type];
        uint16_t c = bs_rgb(d->accent);
        float m = (float)d->muzzle * 0.1f, h = (float)d->height * 0.1f + 0.12f * (float)t->tier;
        int mx = sx(t->x + cosf(t->yaw) * m), my = sy(t->y + sinf(t->yaw) * m, h);
        if (t->off_t > 0) {
            /* silenced: it sparks */
            if (((int)(ui->t * 12.0f) & 1)) bs_glow(gf, sx(t->x), sy(t->y, 1.2f), 22, 0xF81F, 150);
            continue;
        }
        if (t->flash_t > 0 && !d->spin) {
            bs_glow(gf, mx, my, t->type == TW_GATLING ? 12 : 26, 0xFFFF, 220);
            bs_glow(gf, mx, my, t->type == TW_GATLING ? 20 : 40, c, 160);
        }
        if (t->flash_t > 0 && d->spin) bs_glow(gf, sx(t->x), sy(t->y, h), 46, c, (int)(t->flash_t * 600.0f) > 200 ? 200 : (int)(t->flash_t * 600.0f));
        if (t->beam) {
            int ex = sx(t->bx), ey = sy(t->by, t->bz);
            int wide = 3 + (int)(t->heat * 4.0f) + t->tier;
            bs_beam(gf, mx, my, ex, ey, wide + 3, c, 150);
            bs_beam(gf, mx, my, ex, ey, wide > 4 ? 3 : 2, 0xFFFF, 255);
            bs_glow(gf, ex, ey, 14 + (int)(t->heat * 12.0f), c, 220);
            bs_glow(gf, mx, my, 12, 0xFFFF, 200);
        }
        if (t->boost > 1.0f) bs_glow(gf, sx(t->x), sy(t->y, 0.2f), 30, 0x1FF3, 50);
        if (t->type == TW_DRONE) {
            for (int k = 0; k < 2 + t->tier && k < BS_MAX_DRONES; k++) {
                const bs_drone_t *dr = &t->drone[k];
                bs_shadow(gf, sx(dr->x), sy(dr->y, 0), 12, 7, 90);
            }
        }
    }
    for (int i = 0; i < BS_MAX_SHOTS; i++) {
        const bs_shot_t *s = &g->shot[i];
        if (!s->on) continue;
        int x = sx(s->x), y = sy(s->y, s->z);
        if (s->kind == SH_SHELL) {
            bs_glow(gf, x, y, 9, 0xFD00, 255);
            bs_glow(gf, x, y, 5, 0xFFFF, 255);
        } else if (s->kind == SH_MORTAR) {
            bs_glow(gf, x, y, 20, bs_rgb(BS_TOWER[TW_MORTAR].accent), 255);
            bs_glow(gf, x, y, 9, 0xFFFF, 255);
            bs_shadow(gf, sx(s->x), sy(s->y, 0), 10, 6, 100);
        } else {
            bs_glow(gf, x - (int)(s->vx * 1.5f), y + (int)(s->vy * 1.2f), 9, 0xFD20, 230);
        }
    }
    for (int i = 0; i < BS_MAX_FX; i++) {
        const bs_fx_t *f = &g->fx[i];
        if (!f->on) continue;
        float u = f->t / f->life;
        uint16_t c = bs_rgb(f->color);
        switch (f->kind) {
        case FX_TRACER:
            bs_beam(gf, sx(f->x0), sy(f->y0, f->z0), sx(f->x1), sy(f->y1, f->z1), 2, c, (int)(255.0f * (1.0f - u * 0.7f)));
            bs_glow(gf, sx(f->x1), sy(f->y1, f->z1), 8, c, 200);
            break;
        case FX_BOLT: {
            /* a broken line that changes every other frame */
            uint32_t seed = (uint32_t)(f->t * 30.0f) * 2654435761u + (uint32_t)i * 40503u;
            int k = (int)(255.0f * (1.0f - u * 0.6f));
            for (int p = 0; p + 1 < f->n; p++) {
                int x0 = sx(f->px[p]), y0 = sy(f->py[p], f->pz[p]);
                int x1 = sx(f->px[p + 1]), y1 = sy(f->py[p + 1], f->pz[p + 1]);
                int px = x0, py = y0;
                for (int q = 1; q <= 3; q++) {
                    seed = seed * 1664525u + 1013904223u;
                    int jx = q == 3 ? 0 : (int)((seed >> 16) % 23) - 11;
                    int jy = q == 3 ? 0 : (int)((seed >> 8) % 23) - 11;
                    int nx = x0 + (x1 - x0) * q / 3 + jx, ny = y0 + (y1 - y0) * q / 3 + jy;
                    bs_beam(gf, px, py, nx, ny, 4, c, k);
                    bs_beam(gf, px, py, nx, ny, 1, 0xFFFF, k);
                    px = nx;
                    py = ny;
                }
                bs_glow(gf, x1, y1, 16, c, k);
            }
            break;
        }
        case FX_BLAST: {
            int r = (int)(f->r * BS_PX * (0.45f + 0.75f * u));
            int k = (int)(255.0f * (1.0f - u) * (1.0f - u));
            int x = sx(f->x0), y = sy(f->y0, f->z0);
            bs_glow(gf, x, y, r, c, k);
            if (u < 0.5f) bs_glow(gf, x, y, r / 2 + 2, 0xFFFF, (int)(255.0f * (1.0f - u * 2.0f)));
            break;
        }
        case FX_RAIL: {
            int k = (int)(255.0f * (1.0f - u));
            int x0 = sx(f->x0), y0 = sy(f->y0, f->z0), x1 = sx(f->x1), y1 = sy(f->y1, f->z1);
            bs_beam(gf, x0, y0, x1, y1, 9 - (int)(u * 5.0f), c, k);
            if (u < 0.6f) bs_beam(gf, x0, y0, x1, y1, 3, 0xFFFF, k);
            bs_glow(gf, x0, y0, 30, c, k);
            break;
        }
        case FX_STRIKE:
            if (f->t >= 0.5f) {
                float v = (f->t - 0.5f) / (f->life - 0.5f);
                int k = (int)(255.0f * (1.0f - v));
                int x = sx(f->x0), y = sy(f->y0, 0);
                bs_beam(gf, x + 60, 0, x, y, 22 - (int)(v * 14.0f), c, k);
                bs_beam(gf, x + 60, 0, x, y, 7, 0xFFFF, k);
                bs_glow(gf, x, y, 90, 0xFFFF, k);
            }
            break;
        default: break;
        }
    }
    /* sparks */
    for (int i = 0; i < g->npart; i++) {
        const bs_part_t *p = &g->part[i];
        float u = p->life / p->max;
        int k = u > 0.5f ? 255 : (int)(u * 510.0f);
        bs_glow(gf, (int)p->x, (int)p->y, p->r + 1, p->color, k);
    }
    /* the fort's own light: it flashes when hit, burns when nearly lost */
    int fx = sx(g->w->fort_x), fy = sy(g->w->fort_y, 0);
    if (g->fort_flash > 0) bs_glow(gf, fx, fy - 60, 150, 0xF980, (int)(g->fort_flash * 500.0f) > 255 ? 255 : (int)(g->fort_flash * 500.0f));
    if (g->lives * 4 <= g->max_lives && g->state != BS_WON) {
        int k = 90 + (int)(70.0f * sinf(ui->t * 9.0f));
        bs_glow(gf, fx - 30, fy - 70, 36, 0xFB00, k);
        bs_glow(gf, fx + 36, fy - 110, 28, 0xFC40, 160 - k / 2);
    }
    if (g->freeze_t > 0) bs_rect(gf, 0, BS_TOP_H, BS_W, 6, 0x9F7F, 200, 0);
    for (int i = 0; i < BS_MAX_FLOATS; i++) {
        const bs_float_t *f = &g->fl[i];
        if (f->t <= 0) continue;
        bs_text(gf, &art->font[FONT_M], sx(f->x), sy(f->y, 1.0f), f->text, f->color, BS_F_CENTER | BS_F_SHADOW);
    }
}

void bs_scene_draw(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g)
{
    static sort_t order[MAX_SORT];
    int n = 0;
    ground(gf, ui, g);
    order[n++] = (sort_t){ g->w->fort_y, K_FORT, 0, 0 };
    for (int i = 0; i < g->w->npad; i++) {
        const bs_tower_t *t = &g->tw[i];
        if (!t->on) continue;
        order[n++] = (sort_t){ t->y, K_TOWER, 0, (int16_t)i };
        if (t->type == TW_DRONE) {
            for (int k = 0; k < 2 + t->tier && k < BS_MAX_DRONES; k++)
                order[n++] = (sort_t){ t->drone[k].y - 0.05f, K_DRONE, (uint8_t)k, (int16_t)i };
        }
    }
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        if (g->al[i].on) order[n++] = (sort_t){ g->al[i].y, K_ALIEN, 0, (int16_t)i };
    }
    for (int i = 0; i < BS_MAX_SHOTS; i++) {
        if (g->shot[i].on && g->shot[i].kind == SH_MISSILE) order[n++] = (sort_t){ g->shot[i].y, K_SHOT, 0, (int16_t)i };
    }
    qsort(order, (size_t)n, sizeof(sort_t), by_y);
    for (int i = 0; i < n; i++) {
        const sort_t *o = &order[i];
        switch (o->kind) {
        case K_FORT: {
            int state = g->state == BS_LOST ? 2 : (g->lives * 5 > g->max_lives * 3 ? 0 : (g->lives * 4 > g->max_lives ? 1 : 2));
            int y = sy(g->w->fort_y, 0);
            bs_spr(gf, &art->fort, state, sx(g->w->fort_x), y, BS_F_DEPTH | (g->fort_flash > 0.38f ? BS_F_FLASH : 0), 255,
                   bs_z_of(g->w->fort_y), y);
            break;
        }
        case K_TOWER: tower(gf, art, g, o->ix); break;
        case K_ALIEN: alien(gf, art, g, o->ix); break;
        case K_SHOT: {
            const bs_shot_t *s = &g->shot[o->ix];
            float a = atan2f(s->vy, s->vx);
            if (a < 0) a += 2 * PI;
            bs_spr(gf, &art->missile, (int)(a / (2 * PI / 16) + 0.5f) & 15, sx(s->x), sy(s->y, s->z), 0, 255, 0, 0);
            break;
        }
        case K_DRONE: {
            const bs_tower_t *t = &g->tw[o->ix];
            const bs_drone_t *dr = &t->drone[o->sub];
            float a = dr->a;
            if (a < 0) a += 2 * PI;
            int fr = ((int)(a / (PI / 4) + 0.5f) & 7) + art->drone.cols * ((int)(t->anim * 20.0f) & 1);
            bs_spr(gf, &art->drone, fr, sx(dr->x), sy(dr->y, 1.8f) + (int)(2.0f * sinf(t->anim * 5.0f + (float)o->sub)), 0,
                   255, 0, 0);
            break;
        }
        default: break;
        }
    }
    lights(gf, art, ui, g);
}
