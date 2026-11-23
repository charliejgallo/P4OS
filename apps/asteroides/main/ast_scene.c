/*
 * DERIVA - a frame of the game: the game's things as neon strokes in
 * the field (each one drawn again across the edge it is crossing), the
 * sparks, shards and rings the game's events leave, the banners, the
 * score and lives, and the deck with the stick and the buttons. Runs on
 * the worker; the texts come translated from LVGL's thread
 * (ast_scene_strings).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "ast_app.h"

#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI  3.14159265f
#define TAU 6.28318531f
#define GLOWPAD 10.0f

/* each sector's colours: its rocks, its accent, its sky */
static const uint32_t SEC_COL[AG_SECTORS][3] = {
    { 0x5CF2FF, 0x9AFFEA, 0x0E3B5A },   /* aurora    */
    { 0xC79BFF, 0xFF8AE2, 0x2A1450 },   /* quartz    */
    { 0xFFC46B, 0xFF8F3A, 0x4A2608 },   /* amber     */
    { 0xFF8A66, 0xFFC0A0, 0x4A0E0A },   /* forge     */
    { 0x6BFFD8, 0xFF7A8A, 0x0A3A3A },   /* coral     */
    { 0x8C9BFF, 0x5CC8FF, 0x14185A },   /* indigo    */
    { 0xD8E4F0, 0x8AFFB0, 0x22262E },   /* ash       */
    { 0xFFE45C, 0x9CFF5C, 0x3A3208 },   /* honeycomb */
    { 0x7CFFA0, 0x3AE0C8, 0x0A3A22 },   /* jade      */
    { 0xFFA25A, 0xFF4A3A, 0x501404 },   /* magma     */
    { 0xA8D8FF, 0xFFFFFF, 0x060A1A },   /* void      */
    { 0xFF6BE0, 0xB06BFF, 0x3A0A3A },   /* abyss     */
};

static const char *const SEC_NAME[AG_SECTORS] = {
    N_("Aurora"), N_("Cuarzo"), N_("Ámbar"), N_("Forja"), N_("Coral"), N_("Índigo"),
    N_("Ceniza"), N_("Panal"), N_("Jade"), N_("Magma"), N_("Vacío"), N_("Abismo"),
};
static const char *const BOSS_NAME[4] = { "", N_("El Coloso"), N_("La Colmena"), N_("La Serpiente") };
static const char *const POWER_NAME[PW_N] = { N_("Disparo triple"), N_("Escudo"), N_("Rayo"), N_("Imán"), N_("Una nave más") };

static const uint32_t TYPE_COL[RK_N] = { 0, 0xFF5FD2, 0xA8BCD8, 0xFF8A2A, 0x7CFF6B };
static const uint32_t PW_COL[PW_N] = { 0xFFD93A, 0x3AD8FF, 0xFF4AF0, 0x6CFF8E, 0xFFFFFF };
static const uint32_t SHIP_COL[2] = { 0x8AF8FF, 0xFFB347 };
static const uint32_t EN_COL[EN_N] = { 0xC8FF3A, 0xB07CFF, 0xFF3B5C, 0xFF6FB5 };

uint32_t ast_sector_color(int sector, int which)
{
    int i = (sector - 1) % AG_SECTORS;
    if (i < 0) i = 0;
    return SEC_COL[i][which < 0 || which > 2 ? 0 : which];
}

int ast_palette(const ag_game_t *g)
{
    /* the arcade goes through the sectors' colours, two waves each */
    return g->mode == AG_ARCADE ? ((g->sector - 1) / 2) % AG_SECTORS + 1 : g->sector;
}

void ast_scene_strings(ast_app_t *a)
{
    ast_ui_t *u = a->ui;
    snprintf(u->s_sector, sizeof u->s_sector, "%s", _("Sector"));
    snprintf(u->s_wave, sizeof u->s_wave, "%s", _("Oleada"));
    snprintf(u->s_clear, sizeof u->s_clear, "%s", _("Sector limpio"));
    snprintf(u->s_boss, sizeof u->s_boss, "%s", _("¡Cuidado!"));
    snprintf(u->s_over, sizeof u->s_over, "%s", _("Sin naves"));
    snprintf(u->s_won, sizeof u->s_won, "%s", _("¡Campaña completa!"));
    snprintf(u->s_ready, sizeof u->s_ready, "%s", _("¡Listos!"));
    snprintf(u->s_fire, sizeof u->s_fire, "%s", _("Disparo"));
    snprintf(u->s_jump, sizeof u->s_jump, "%s", _("Salto"));
    snprintf(u->s_record, sizeof u->s_record, "%s", _("Récord"));
    snprintf(u->s_paused, sizeof u->s_paused, "%s", _("Pausa"));
    for (int i = 0; i < AG_SECTORS; i++) snprintf(u->s_sector_name[i], sizeof u->s_sector_name[i], "%s", _(SEC_NAME[i]));
    for (int i = 1; i < 4; i++) snprintf(u->s_boss_name[i], sizeof u->s_boss_name[i], "%s", _(BOSS_NAME[i]));
    for (int i = 0; i < PW_N; i++) snprintf(u->s_power[i], sizeof u->s_power[i], "%s", _(POWER_NAME[i]));
}

void ast_scene_reset(ast_app_t *a)
{
    ast_ui_t *u = a->ui;
    u->npart = 0;
    memset(u->text, 0, sizeof u->text);
    u->shake = 0;
    u->flash = 0;
    u->seen_state = -1;
    u->banner_t = 0;
}

/* ---- particles ------------------------------------------------------------------------------ */

static uint32_t s_rnd = 0x51A7u;

static float frand(void)
{
    return ag_rand01(&s_rnd);
}

static ast_part_t *part(ast_ui_t *u, int kind, float x, float y, float life, uint32_t color)
{
    if (u->npart >= AST_PARTS) return NULL;
    ast_part_t *p = &u->part[u->npart++];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    p->x = x;
    p->y = y;
    p->life = p->max = life;
    p->color = color;
    return p;
}

static void sparks(ast_ui_t *u, float x, float y, int n, float v0, float v1, float life, uint32_t color)
{
    for (int i = 0; i < n; i++) {
        float a = frand() * TAU, v = v0 + (v1 - v0) * frand();
        ast_part_t *p = part(u, 0, x, y, life * (0.5f + 0.7f * frand()), color);
        if (!p) return;
        p->vx = cosf(a) * v;
        p->vy = sinf(a) * v;
    }
}

static void shards(ast_ui_t *u, float x, float y, int n, float len, uint32_t color)
{
    for (int i = 0; i < n; i++) {
        float a = frand() * TAU, v = 40.0f + 120.0f * frand();
        ast_part_t *p = part(u, 3, x + cosf(a) * len * 0.5f, y + sinf(a) * len * 0.5f, 0.6f + 0.6f * frand(), color);
        if (!p) return;
        p->vx = cosf(a) * v;
        p->vy = sinf(a) * v;
        p->a = frand() * TAU;
        p->spin = (frand() - 0.5f) * 12.0f;
        p->size = len * (0.4f + 0.5f * frand());
    }
}

static void ring(ast_ui_t *u, float x, float y, float r0, float r1, float life, uint32_t color)
{
    ast_part_t *p = part(u, 2, x, y, life, color);
    if (!p) return;
    p->a = r0;
    p->size = r1;
}

static void puff(ast_ui_t *u, float x, float y, float r, float life, uint32_t color)
{
    ast_part_t *p = part(u, 1, x, y, life, color);
    if (p) p->size = r;
}

static void ftext(ast_ui_t *u, float x, float y, const char *s, uint32_t color)
{
    int best = 0;
    for (int i = 0; i < AST_TEXTS; i++) {
        if (u->text[i].t <= 0) {
            best = i;
            break;
        }
        if (u->text[i].t < u->text[best].t) best = i;
    }
    ast_ftext_t *t = &u->text[best];
    t->x = x;
    t->y = y;
    t->t = 1.6f;
    t->color = color;
    snprintf(t->s, sizeof t->s, "%s", s);
}

void ast_scene_events(ast_app_t *a)
{
    ag_game_t *g = a->game;
    ast_ui_t *u = a->ui;
    uint32_t pri = ast_sector_color(ast_palette(g), 0);
    for (int i = 0; i < g->nev; i++) {
        const ag_event_t *e = &g->ev[i];
        switch (e->kind) {
        case EV_BOOM: {
            int sz = (int)e->a, ty = (int)e->b;
            uint32_t c = ty == RK_STONE ? pri : TYPE_COL[ty];
            sparks(u, e->x, e->y, 8 + sz * 9, 60.0f, 260.0f + 60.0f * sz, 0.7f, c);
            shards(u, e->x, e->y, 3 + sz * 2, 10.0f + 9.0f * sz, c);
            puff(u, e->x, e->y, 22.0f + 18.0f * sz, 0.35f, c);
            break;
        }
        case EV_SPARK:
            sparks(u, e->x, e->y, 6, 80.0f, 260.0f, 0.35f, 0xDDEEFF);
            break;
        case EV_BLAST:
            ring(u, e->x, e->y, 8.0f, e->a, 0.45f, 0xFFA040);
            ring(u, e->x, e->y, 4.0f, e->a * 0.7f, 0.35f, 0xFFE0A0);
            sparks(u, e->x, e->y, 34, 120.0f, 420.0f, 0.8f, 0xFF9A3A);
            puff(u, e->x, e->y, e->a * 0.8f, 0.4f, 0xFF7A2A);
            u->flash = 0.12f;
            break;
        case EV_SHIP_DIE: {
            uint32_t c = SHIP_COL[(int)e->a & 1];
            sparks(u, e->x, e->y, 46, 80.0f, 380.0f, 1.1f, c);
            shards(u, e->x, e->y, 7, 16.0f, c);
            ring(u, e->x, e->y, 6.0f, 90.0f, 0.6f, c);
            puff(u, e->x, e->y, 60.0f, 0.5f, 0xFFFFFF);
            u->flash = 0.2f;
            break;
        }
        case EV_JUMP_OUT:
            ring(u, e->x, e->y, 46.0f, 2.0f, 0.3f, 0x9AF0FF);
            sparks(u, e->x, e->y, 14, 20.0f, 120.0f, 0.4f, 0x9AF0FF);
            break;
        case EV_JUMP_IN:
            ring(u, e->x, e->y, 2.0f, 56.0f, 0.35f, 0x9AF0FF);
            sparks(u, e->x, e->y, 18, 60.0f, 220.0f, 0.45f, 0xFFFFFF);
            break;
        case EV_PICK: {
            int k = (int)e->a;
            if (k < 0 || k >= PW_N) break;
            ring(u, e->x, e->y, 10.0f, 60.0f, 0.4f, PW_COL[k]);
            ftext(u, e->x, e->y - 30, u->s_power[k], PW_COL[k]);
            break;
        }
        case EV_CRYSTAL:
            sparks(u, e->x, e->y, 4, 30.0f, 110.0f, 0.3f, 0xB8FBFF);
            break;
        case EV_SHIELD:
            ring(u, e->x, e->y, 22.0f, 60.0f, 0.35f, 0x3AD8FF);
            sparks(u, e->x, e->y, 12, 80.0f, 240.0f, 0.4f, 0x3AD8FF);
            break;
        case EV_STICK:
            sparks(u, e->x, e->y, 10, 40.0f, 160.0f, 0.45f, 0x7CFF6B);
            break;
        case EV_ENEMY_DIE: {
            int k = (int)e->a;
            uint32_t c = EN_COL[k < 0 || k >= EN_N ? 0 : k];
            sparks(u, e->x, e->y, 30, 80.0f, 320.0f, 0.8f, c);
            shards(u, e->x, e->y, 5, 12.0f, c);
            ring(u, e->x, e->y, 6.0f, 54.0f, 0.4f, c);
            break;
        }
        case EV_BOSS_PART:
            sparks(u, e->x, e->y, 30, 80.0f, 340.0f, 0.8f, 0xFFD0A0);
            shards(u, e->x, e->y, 6, 18.0f, 0xFFD0A0);
            ring(u, e->x, e->y, 6.0f, 70.0f, 0.4f, 0xFFFFFF);
            break;
        case EV_BOSS_DIE:
            for (int k = 0; k < 3; k++) ring(u, e->x, e->y, 10.0f + 20.0f * k, e->a * (1.6f + 0.6f * k), 0.6f + 0.25f * k, k == 1 ? 0xFFFFFF : pri);
            sparks(u, e->x, e->y, 140, 100.0f, 520.0f, 1.4f, 0xFFE0B0);
            shards(u, e->x, e->y, 16, 30.0f, pri);
            puff(u, e->x, e->y, e->a * 1.5f, 0.7f, 0xFFFFFF);
            u->flash = 0.3f;
            break;
        case EV_MUZZLE: {
            const ag_ship_t *sh = &g->ship[(int)e->a & 1];
            float x = sh->x + cosf(sh->ang) * 18.0f, y = sh->y + sinf(sh->ang) * 18.0f;
            puff(u, x, y, 9.0f, 0.08f, SHIP_COL[(int)e->a & 1]);
            break;
        }
        case EV_LIFE:
            ftext(u, e->x, e->y - 30, "+1", 0xFFFFFF);
            break;
        case EV_DASH:
            sparks(u, e->x, e->y, 10, 60.0f, 180.0f, 0.4f, EN_COL[EN_LANCER]);
            break;
        default: break;
        }
    }
    if (g->shake > u->shake) u->shake = g->shake;
    g->shake = 0;
    g->nev = 0;
}

static void parts_step(ast_app_t *a, float dt)
{
    ast_ui_t *u = a->ui;
    const ag_game_t *g = a->game;
    int n = 0;
    for (int i = 0; i < u->npart; i++) {
        ast_part_t *p = &u->part[i];
        p->life -= dt;
        if (p->life <= 0) continue;
        float fr = p->kind == 0 ? 1.0f - 2.2f * dt : 1.0f - 1.0f * dt;
        p->vx *= fr;
        p->vy *= fr;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if (p->x < 0) p->x += g->W;
        if (p->x >= g->W) p->x -= g->W;
        if (p->y < 0) p->y += g->H;
        if (p->y >= g->H) p->y -= g->H;
        p->a += p->spin * dt;
        u->part[n++] = *p;
    }
    u->npart = n;
    for (int i = 0; i < AST_TEXTS; i++) {
        if (u->text[i].t > 0) {
            u->text[i].t -= dt;
            u->text[i].y -= 26.0f * dt;
        }
    }
}

/* ---- the field --------------------------------------------------------------------------- */

typedef struct {
    ast_gfx_t *g;
    float ox, oy, W, H;
} view_t;

/* the places a thing of radius r shows: itself and its copies across the edges */
static int copies(const view_t *v, float x, float y, float r, float *xs, float *ys)
{
    float rr = r + GLOWPAD;
    int nx = 1, ny = 1;
    float dx[2] = { 0, 0 }, dy[2] = { 0, 0 };
    if (x < rr) dx[nx++] = v->W;
    else if (x > v->W - rr) dx[nx++] = -v->W;
    if (y < rr) dy[ny++] = v->H;
    else if (y > v->H - rr) dy[ny++] = -v->H;
    int n = 0;
    for (int i = 0; i < nx; i++) {
        for (int j = 0; j < ny; j++) {
            xs[n] = v->ox + x + dx[i];
            ys[n] = v->oy + y + dy[j];
            n++;
        }
    }
    return n;
}

/* a closed shape of n points (local, unit), turned by ang and scaled by r */
static void shape(const view_t *v, float x, float y, float ang, float r, const float *pts, int n, uint32_t col, float w, int k)
{
    float xs[4], ys[4];
    int nc = copies(v, x, y, r, xs, ys);
    float c = cosf(ang), s = sinf(ang);
    float xy[64];
    if (n > 32) n = 32;
    for (int q = 0; q < nc; q++) {
        for (int i = 0; i < n; i++) {
            float px = pts[i * 2] * r, py = pts[i * 2 + 1] * r;
            xy[i * 2] = xs[q] + px * c - py * s;
            xy[i * 2 + 1] = ys[q] + px * s + py * c;
        }
        ast_poly(v->g, xy, n, col, w, k);
    }
}

/* an open line between two local points of a turned thing */
static void segment(const view_t *v, float x, float y, float ang, float r, float ax, float ay, float bx, float by, uint32_t col,
                    float w, int k)
{
    float xs[4], ys[4];
    int nc = copies(v, x, y, r, xs, ys);
    float c = cosf(ang), s = sinf(ang);
    for (int q = 0; q < nc; q++) {
        ast_line(v->g, xs[q] + (ax * c - ay * s) * r, ys[q] + (ax * s + ay * c) * r, xs[q] + (bx * c - by * s) * r,
                 ys[q] + (bx * s + by * c) * r, col, w, k, AST_F_CAPS);
    }
}

static void circle(const view_t *v, float x, float y, float r, int n, uint32_t col, float w, int k)
{
    float pts[64];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        pts[i * 2] = cosf(TAU * (float)i / (float)n);
        pts[i * 2 + 1] = sinf(TAU * (float)i / (float)n);
    }
    shape(v, x, y, 0, r, pts, n, col, w, k);
}

static void dot(const view_t *v, float x, float y, int r, uint32_t col, int k)
{
    float xs[4], ys[4];
    int nc = copies(v, x, y, (float)r, xs, ys);
    for (int q = 0; q < nc; q++) ast_glow(v->g, xs[q], ys[q], r, col, k);
}

static uint32_t mixc(uint32_t a, uint32_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    int r = (int)(((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t));
    int gg = (int)(((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t));
    int bb = (int)((a & 255) + (((int)(b & 255) - (int)(a & 255)) * t));
    return (uint32_t)((r << 16) | (gg << 8) | bb);
}

static void draw_rock(const view_t *v, const ag_game_t *g, const ag_rock_t *r, uint32_t pri, float t)
{
    uint32_t col = r->type == RK_STONE ? pri : TYPE_COL[r->type];
    if (r->flash > 0) col = mixc(col, 0xFFFFFF, 0.8f);
    float pts[64];
    int n = r->nv;
    for (int i = 0; i < n; i++) {
        float k = (float)r->shape[i] / 128.0f;
        if (r->type == RK_LAPA) k *= 1.0f + 0.07f * sinf(t * 5.0f + (float)i * 1.7f);
        float a = TAU * (float)i / (float)n;
        pts[i * 2] = cosf(a) * k;
        pts[i * 2 + 1] = sinf(a) * k;
    }
    int bright = r->type == RK_LAPA && r->stuck >= 0 ? 255 : 220;
    shape(v, r->x, r->y, r->ang, r->r, pts, n, col, 1.0f, bright);
    switch (r->type) {
    case RK_TRIO: {
        static const float TRI[6] = { 0.42f, 0.0f, -0.21f, 0.36f, -0.21f, -0.36f };
        shape(v, r->x, r->y, -r->ang * 1.5f, r->r, TRI, 3, col, 0.8f, 150);
        break;
    }
    case RK_METAL: {
        for (int i = 0; i < n; i++) {
            pts[i * 2] *= 0.7f;
            pts[i * 2 + 1] *= 0.7f;
        }
        shape(v, r->x, r->y, r->ang, r->r, pts, n, col, 0.7f, 120);
        for (int i = 0; i < 3; i++) {
            float a = r->ang + TAU * (float)i / 3.0f;
            dot(v, r->x + cosf(a) * r->r * 0.45f, r->y + sinf(a) * r->r * 0.45f, 4, col, 200);
        }
        break;
    }
    case RK_VOLATILE: {
        float pulse = 0.5f + 0.5f * sinf(t * (r->fuse > 0 ? 40.0f : 6.0f));
        int k = (int)(120 + 135 * pulse);
        segment(v, r->x, r->y, r->ang, r->r, -0.35f, 0, 0.35f, 0, 0xFFE070, 0.9f, k);
        segment(v, r->x, r->y, r->ang, r->r, 0, -0.35f, 0, 0.35f, 0xFFE070, 0.9f, k);
        dot(v, r->x, r->y, (int)(r->r * 0.5f), 0xFF7A20, (int)(60 + 80 * pulse));
        break;
    }
    case RK_LAPA:
        for (int i = 0; i < 3; i++) {
            float a = r->ang * 0.6f + TAU * (float)i / 3.0f + t;
            dot(v, r->x + cosf(a) * r->r * 0.35f, r->y + sinf(a) * r->r * 0.35f, 4, col, 170);
        }
        break;
    default: break;
    }
}

static const float SHIP_PTS[] = {
    1.0f, 0.0f, -0.55f, -0.66f, -0.28f, -0.24f, -0.62f, 0.0f, -0.28f, 0.24f, -0.55f, 0.66f,
};

static void draw_ship(const view_t *v, ast_app_t *a, int s, float t, float dt)
{
    const ag_game_t *g = a->game;
    const ag_ship_t *sh = &g->ship[s];
    ast_ui_t *u = a->ui;
    if (!sh->alive || sh->jump_t > 0) return;
    uint32_t col = SHIP_COL[s];
    int k = 255;
    if (sh->inv_t > 0 && fmodf(sh->inv_t, 0.2f) < 0.09f) k = 90;
    float R = 24.0f;
    shape(v, sh->x, sh->y, sh->ang, R, SHIP_PTS, 6, col, 1.1f, k);
    segment(v, sh->x, sh->y, sh->ang, R, 0.45f, 0, -0.05f, 0, col, 0.7f, k * 2 / 3);
    /* the flame and its trail */
    if (sh->thrust > 0.05f) {
        float fl = 0.5f + sh->thrust * (0.7f + 0.3f * frand());
        segment(v, sh->x, sh->y, sh->ang, R, -0.62f, -0.18f, -0.62f - fl, 0, 0xFFB040, 0.9f, 230);
        segment(v, sh->x, sh->y, sh->ang, R, -0.62f, 0.18f, -0.62f - fl, 0, 0xFFB040, 0.9f, 230);
        u->trail_acc[s] += dt * (40.0f + 60.0f * sh->thrust);
        while (u->trail_acc[s] >= 1.0f) {
            u->trail_acc[s] -= 1.0f;
            float c = cosf(sh->ang), sn = sinf(sh->ang);
            ast_part_t *p = part(u, 0, sh->x - c * 14.0f, sh->y - sn * 14.0f, 0.35f + 0.2f * frand(), frand() < 0.5f ? 0xFF8A2A : 0xFFD060);
            if (p) {
                float sp = 120.0f + 120.0f * frand(), da = (frand() - 0.5f) * 0.6f;
                p->vx = sh->vx - cosf(sh->ang + da) * sp;
                p->vy = sh->vy - sinf(sh->ang + da) * sp;
            }
        }
    }
    if (sh->shield > 0) {
        float pulse = 0.6f + 0.4f * sinf(t * 5.0f);
        circle(v, sh->x, sh->y, 26.0f + (sh->shield_t > 0 ? 6.0f : 0), 18, 0x3AD8FF, 0.8f, (int)(110 + 80 * pulse));
        if (sh->shield > 1) circle(v, sh->x, sh->y, 31.0f, 18, 0x3AD8FF, 0.6f, 80);
    }
    if (sh->pw_t[PW_MAGNET] > 0) circle(v, sh->x, sh->y, 40.0f + 6.0f * sinf(t * 7.0f), 16, 0x6CFF8E, 0.5f, 60);
    if (sh->beam_on) {
        float c = cosf(sh->ang), sn = sinf(sh->ang);
        float x0 = v->ox + sh->x + c * 18.0f, y0 = v->oy + sh->y + sn * 18.0f;
        float x1 = v->ox + sh->bx, y1 = v->oy + sh->by;
        float fl = 0.85f + 0.15f * frand();
        ast_line(v->g, x0, y0, x1, y1, 0xFF4AF0, 2.2f * fl, 200, AST_F_CAPS);
        ast_line(v->g, x0, y0, x1, y1, 0xFFFFFF, 0.6f, 200, AST_F_CAPS);
        if (frand() < 0.6f) sparks(u, sh->bx, sh->by, 2, 60.0f, 200.0f, 0.25f, 0xFF8AF8);
    }
}

static void draw_enemy(const view_t *v, const ag_enemy_t *e, float t)
{
    uint32_t col = EN_COL[e->kind];
    if (e->flash > 0) col = 0xFFFFFF;
    switch (e->kind) {
    case EN_SEEKER: {
        static const float P[] = { 1.0f, 0.0f, -0.6f, -0.55f, -0.25f, 0.0f, -0.6f, 0.55f };
        shape(v, e->x, e->y, e->ang, 15.0f, P, 4, col, 0.9f, 240);
        dot(v, e->x, e->y, 5, col, (int)(140 + 100 * sinf(t * 18.0f)));
        break;
    }
    case EN_LAYER: {
        float P[12], Q[12];
        for (int i = 0; i < 6; i++) {
            P[i * 2] = cosf(TAU * i / 6.0f);
            P[i * 2 + 1] = sinf(TAU * i / 6.0f);
            Q[i * 2] = P[i * 2] * 0.45f;
            Q[i * 2 + 1] = P[i * 2 + 1] * 0.45f;
        }
        shape(v, e->x, e->y, e->ang, 22.0f, P, 6, col, 1.0f, 230);
        shape(v, e->x, e->y, -e->ang * 2.0f, 22.0f, Q, 6, col, 0.7f, 170);
        for (int i = 0; i < 3; i++) {
            float a = TAU * (float)i / 3.0f;
            segment(v, e->x, e->y, e->ang, 22.0f, cosf(a) * 0.45f, sinf(a) * 0.45f, cosf(a), sinf(a), col, 0.6f, 120);
        }
        break;
    }
    case EN_LANCER: {
        static const float P[] = { 1.0f, 0.0f, 0.0f, -0.26f, -0.75f, 0.0f, 0.0f, 0.26f };
        shape(v, e->x, e->y, e->ang, 24.0f, P, 4, col, 1.0f, 240);
        if (e->state == 1) {
            /* aiming: a dashed line where it will go */
            float c = cosf(e->ang), s = sinf(e->ang);
            int k = (int)(70 + 90 * (e->t / 1.15f));
            for (int i = 0; i < 9; i++) {
                float d0 = 40.0f + (float)i * 46.0f + fmodf(t * 120.0f, 46.0f);
                ast_line(v->g, v->ox + e->x + c * d0, v->oy + e->y + s * d0, v->ox + e->x + c * (d0 + 20.0f),
                         v->oy + e->y + s * (d0 + 20.0f), col, 0.6f, k, AST_F_CAPS);
            }
        } else if (e->state == 2) {
            float c = cosf(e->ang), s = sinf(e->ang);
            ast_line(v->g, v->ox + e->x, v->oy + e->y, v->ox + e->x - c * 110.0f, v->oy + e->y - s * 110.0f, col, 1.4f, 160,
                     AST_F_CAP0);
        }
        break;
    }
    case EN_WEAVER: {
        circle(v, e->x, e->y, 11.0f, 12, col, 1.0f, 230);
        static const float T[] = { 1.0f, 0.0f, -0.5f, 0.6f, -0.5f, -0.6f };
        for (int i = 0; i < 3; i++) {
            float a = e->ang + TAU * (float)i / 3.0f;
            shape(v, e->x + cosf(a) * 22.0f, e->y + sinf(a) * 22.0f, a + PI / 2, 7.0f, T, 3, col, 0.7f, 200);
        }
        break;
    }
    default: break;
    }
}

static void draw_boss(const view_t *v, const ag_game_t *g, float t, uint32_t pri)
{
    const ag_boss_t *b = &g->boss;
    if (!b->on) return;
    if (b->kind == BOSS_COLOSO) {
        static float core[28];
        static bool made;
        if (!made) {
            uint32_t s = 99;
            for (int i = 0; i < 14; i++) {
                float k = 0.82f + 0.18f * ag_rand01(&s);
                core[i * 2] = cosf(TAU * i / 14.0f) * k;
                core[i * 2 + 1] = sinf(TAU * i / 14.0f) * k;
            }
            made = true;
        }
        uint32_t cc = b->flash > 0 ? 0xFFFFFF : 0xFF6A3A;
        shape(v, b->x, b->y, b->ang * 0.5f, 88.0f, core, 14, cc, 1.4f, 240);
        float life = b->hp / (b->hp_max > 0 ? b->hp_max : 1);
        dot(v, b->x, b->y, 40, 0xFF4A20, (int)(60 + 120 * life * (0.7f + 0.3f * sinf(t * 4.0f))));
        for (int p = 0; p < 6; p++) {
            if (b->part_hp[p] <= 0) continue;
            float a0 = (float)p * TAU / 6.0f + 0.05f, a1 = (float)(p + 1) * TAU / 6.0f - 0.05f;
            float P[8] = { cosf(a0) * 0.76f, sinf(a0) * 0.76f, cosf(a0), sinf(a0), cosf(a1), sinf(a1), cosf(a1) * 0.76f,
                           sinf(a1) * 0.76f };
            uint32_t pc = b->part_flash[p] > 0 ? 0xFFFFFF : 0xA8BCD8;
            shape(v, b->x, b->y, b->ang, 124.0f, P, 4, pc, 1.2f, 230);
        }
    } else if (b->kind == BOSS_COLMENA) {
        float P[12], Q[12];
        bool open = b->part_hp[0] <= 0 && b->part_hp[1] <= 0 && b->part_hp[2] <= 0;
        for (int i = 0; i < 6; i++) {
            P[i * 2] = cosf(TAU * i / 6.0f + PI / 6);
            P[i * 2 + 1] = sinf(TAU * i / 6.0f + PI / 6);
            Q[i * 2] = P[i * 2] * 0.58f;
            Q[i * 2 + 1] = P[i * 2 + 1] * 0.58f;
        }
        uint32_t hc = 0xFFD23A;
        shape(v, b->x, b->y, b->ang, b->r * 0.92f, P, 6, hc, 1.4f, 230);
        if (!open) {
            shape(v, b->x, b->y, b->ang, b->r * 0.92f, Q, 6, hc, 1.0f, 170);
        } else {
            /* opened: the inner shell in pieces, the core glowing */
            for (int i = 0; i < 6; i++) {
                float a = b->ang + TAU * i / 6.0f + PI / 6;
                float o = 10.0f + 6.0f * sinf(t * 3.0f);
                float c = cosf(a), s = sinf(a);
                float cx = b->x + c * (b->r * 0.53f + o), cy = b->y + s * (b->r * 0.53f + o);
                segment(v, cx, cy, a + PI / 2, 26.0f, -1.0f, 0, 1.0f, 0, hc, 0.9f, 170);
            }
            uint32_t cc = b->flash > 0 ? 0xFFFFFF : 0xFF4A3A;
            circle(v, b->x, b->y, 30.0f, 14, cc, 1.3f, 255);
            dot(v, b->x, b->y, 46, 0xFF3020, (int)(90 + 80 * sinf(t * 6.0f)));
        }
        for (int p = 0; p < 3; p++) {
            if (b->part_hp[p] <= 0) continue;
            float pa = b->ang + (float)p * TAU / 3.0f;
            float tx = b->x + cosf(pa) * b->r, ty = b->y + sinf(pa) * b->r;
            uint32_t tc = b->part_flash[p] > 0 ? 0xFFFFFF : 0xFFF0A0;
            circle(v, tx, ty, 19.0f, 10, tc, 1.0f, 240);
            float dx = 0, dy = 1;
            for (int s = 0; s < g->players; s++) {
                if (g->ship[s].alive) {
                    dx = ag_wrapd(g->ship[s].x - tx, g->W);
                    dy = ag_wrapd(g->ship[s].y - ty, g->H);
                    break;
                }
            }
            float l = sqrtf(dx * dx + dy * dy) + 1e-3f;
            ast_line(v->g, v->ox + tx, v->oy + ty, v->ox + tx + dx / l * 30.0f, v->oy + ty + dy / l * 30.0f, tc, 1.2f, 230,
                     AST_F_CAPS);
        }
    } else {
        /* the serpent: rings along its trail, then the head */
        float px = 0, py = 0;
        bool have = false;
        for (int k = AG_SEGS - 1; k >= 0; k--) {
            int i = (b->trail_i - (k + 1) * 22) & (AG_TRAIL - 1);
            float sx = b->trail_x[i], sy = b->trail_y[i];
            if (b->seg_hp[k] > 0) {
                uint32_t c = b->seg_flash[k] > 0 ? 0xFFFFFF : (k & 1 ? 0xFF6BE0 : 0xB06BFF);
                circle(v, sx, sy, 15.0f, 10, c, 1.0f, 230);
                dot(v, sx, sy, 6, c, 150);
                if (have && fabsf(sx - px) < 80 && fabsf(sy - py) < 80)
                    ast_line(v->g, v->ox + px, v->oy + py, v->ox + sx, v->oy + sy, 0xB06BFF, 0.5f, 90, 0);
                have = true;
                px = sx;
                py = sy;
            }
        }
        static const float HEAD[] = { 1.2f, 0.0f, 0.3f, -0.8f, -0.8f, -0.6f, -0.5f, 0.0f, -0.8f, 0.6f, 0.3f, 0.8f };
        int tail = -1;
        for (int k = 0; k < AG_SEGS; k++) {
            if (b->seg_hp[k] > 0) tail = k;
        }
        uint32_t hc = b->flash > 0 ? 0xFFFFFF : tail < 0 ? 0xFF3A6A : 0xFF6BE0;
        shape(v, b->x, b->y, b->ang, b->r, HEAD, 6, hc, 1.4f, 255);
        float c = cosf(b->ang), s = sinf(b->ang);
        dot(v, b->x + c * 8.0f - s * 9.0f, b->y + s * 8.0f + c * 9.0f, 5, 0xFFFFFF, 220);
        dot(v, b->x + c * 8.0f + s * 9.0f, b->y + s * 8.0f - c * 9.0f, 5, 0xFFFFFF, 220);
    }
    (void)pri;
}

static void draw_pick(const view_t *v, const ag_pick_t *p, float t)
{
    if (p->life < 2.0f && fmodf(p->life, 0.25f) < 0.1f) return;
    uint32_t c = PW_COL[p->kind];
    float P[12];
    for (int i = 0; i < 6; i++) {
        P[i * 2] = cosf(TAU * i / 6.0f);
        P[i * 2 + 1] = sinf(TAU * i / 6.0f);
    }
    shape(v, p->x, p->y, p->ang, 17.0f, P, 6, c, 0.9f, 220);
    float x = p->x, y = p->y;
    switch (p->kind) {
    case PW_TRIPLE:
        for (int i = -1; i <= 1; i++) segment(v, x, y, -PI / 2 + i * 0.45f, 9.0f, 0.0f, 0.0f, 1.0f, 0.0f, c, 0.7f, 230);
        break;
    case PW_SHIELD: circle(v, x, y, 7.0f, 10, c, 0.7f, 230); break;
    case PW_BEAM:
        segment(v, x, y, 0, 10.0f, -0.9f, 0.4f, 0.9f, -0.4f, c, 0.9f, 240);
        break;
    case PW_MAGNET: {
        static const float U[] = { -0.6f, -0.7f, -0.6f, 0.2f, 0.0f, 0.7f, 0.6f, 0.2f, 0.6f, -0.7f };
        for (int i = 0; i < 4; i++) segment(v, x, y, 0, 10.0f, U[i * 2], U[i * 2 + 1], U[i * 2 + 2], U[i * 2 + 3], c, 0.7f, 230);
        break;
    }
    case PW_LIFE: shape(v, x, y, -PI / 2, 9.0f, SHIP_PTS, 6, c, 0.6f, 230); break;
    default: break;
    }
    (void)t;
}

/* ---- the deck and the score -------------------------------------------------------------------- */

static void arc(ast_gfx_t *g, float cx, float cy, float r, float a0, float a1, uint32_t col, float w, int k)
{
    int n = (int)(fabsf(a1 - a0) / (TAU / 48.0f)) + 1;
    bool closed = fabsf(a1 - a0) >= TAU - 1e-3f;
    float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
    for (int i = 1; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        float qx = cx + cosf(a) * r, qy = cy + sinf(a) * r;
        /* pieces of a curve meet flat, so the joints do not bead */
        int caps = closed ? 0 : (i == 1 ? AST_F_CAP0 : 0) | (i == n ? AST_F_CAP1 : 0);
        ast_line(g, px, py, qx, qy, col, w, k, caps);
        px = qx;
        py = qy;
    }
}

static void button(ast_gfx_t *g, int x, int y, int r, uint32_t col, bool lit)
{
    if (lit) ast_disc(g, x, y, r - 4, r - 4, col, 70, AST_F_ADD);
    arc(g, (float)x, (float)y, (float)r, 0, TAU, col, 1.0f, lit ? 255 : 120);
}

static void draw_deck(ast_app_t *a, ast_gfx_t *g, float t)
{
    const ast_layout_t *L = &a->lay;
    const ast_in_t *in = &a->in[0];
    const ag_game_t *gm = a->game;
    const ag_ship_t *sh = &gm->ship[0];
    ast_ui_t *u = a->ui;
    uint32_t acc = ast_sector_color(ast_palette(gm), 0);
    uint32_t dim = 0x6A8AA0;
    float ts = L->land ? 3.0f : 3.4f;
    if (a->cfg.controls == CTL_STICK) {
        if (in->stick) {
            arc(g, in->cx, in->cy, (float)L->stick_r, 0, TAU, acc, 0.8f, 110);
            float kx = in->cx + in->sx * (float)L->stick_r, ky = in->cy + in->sy * (float)L->stick_r;
            ast_disc(g, (int)kx, (int)ky, 34, 34, acc, 60, AST_F_ADD);
            arc(g, kx, ky, 34.0f, 0, TAU, acc, 1.0f, 240);
        } else {
            arc(g, (float)L->stick_x, (float)L->stick_y, (float)L->stick_r, 0, TAU, dim, 0.7f, 80);
            arc(g, (float)L->stick_x, (float)L->stick_y, 34.0f, 0, TAU, dim, 0.9f, 140);
        }
    } else {
        button(g, L->tl_x, L->tl_y, L->btn_r, acc, in->turn_l);
        button(g, L->tr_x, L->tr_y, L->btn_r, acc, in->turn_r);
        button(g, L->push_x, L->push_y, L->btn_r, acc, in->push);
        float r = (float)L->btn_r * 0.45f;
        arc(g, (float)L->tl_x, (float)L->tl_y, r, -PI * 0.15f, PI * 1.1f, 0xFFFFFF, 0.8f, 200);
        ast_line(g, L->tl_x - r * 0.95f, L->tl_y - r * 0.1f, L->tl_x - r * 1.0f, L->tl_y + r * 0.55f, 0xFFFFFF, 0.8f, 200, AST_F_CAPS);
        arc(g, (float)L->tr_x, (float)L->tr_y, r, -PI * 0.1f, PI * 1.15f, 0xFFFFFF, 0.8f, 200);
        ast_line(g, L->tr_x + r * 0.95f, L->tr_y - r * 0.1f, L->tr_x + r * 1.0f, L->tr_y + r * 0.55f, 0xFFFFFF, 0.8f, 200, AST_F_CAPS);
        float px = (float)L->push_x, py = (float)L->push_y;
        ast_line(g, px - r, py + r * 0.5f, px, py - r * 0.6f, 0xFFFFFF, 0.9f, 200, AST_F_CAPS);
        ast_line(g, px, py - r * 0.6f, px + r, py + r * 0.5f, 0xFFFFFF, 0.9f, 200, AST_F_CAPS);
    }
    /* fire: three chevrons; jump: a ring that fills while it charges */
    button(g, L->fire_x, L->fire_y, L->fire_r, 0xFF5FA8, in->fire);
    for (int i = -1; i <= 1; i++) {
        float y = (float)L->fire_y + (float)i * 16.0f + 4.0f;
        ast_line(g, L->fire_x - 18.0f, y + 9.0f, (float)L->fire_x, y - 6.0f, 0xFFFFFF, 0.9f, 220, AST_F_CAPS);
        ast_line(g, (float)L->fire_x, y - 6.0f, L->fire_x + 18.0f, y + 9.0f, 0xFFFFFF, 0.9f, 220, AST_F_CAPS);
    }
    ast_text(g, (float)L->fire_x, (float)(L->fire_y + L->fire_r + 10), u->s_fire, ts, 0xFF8AC0, 170, AST_F_CENTER);
    bool ready = sh->alive && sh->jump_cd <= 0;
    button(g, L->jump_x, L->jump_y, L->jump_r, 0x9AF0FF, in->jump);
    if (!ready && sh->alive) {
        static const float CD[3] = { 5.0f, 3.9f, 2.8f };
        float f = 1.0f - sh->jump_cd / CD[gm->upg[UP_JUMP]];
        arc(g, (float)L->jump_x, (float)L->jump_y, L->jump_r - 9.0f, -PI / 2, -PI / 2 + TAU * f, 0x9AF0FF, 0.8f, 160);
    }
    int jk = ready ? 230 : 90;
    float jr = (float)L->jump_r * 0.42f;
    for (int i = 0; i < 4; i++) {
        float a0 = PI / 4 + i * PI / 2;
        float x0 = L->jump_x + cosf(a0) * jr, y0 = L->jump_y + sinf(a0) * jr;
        ast_line(g, x0, y0, x0 + cosf(a0) * jr * 0.7f, y0 + sinf(a0) * jr * 0.7f, 0xFFFFFF, 0.8f, jk, AST_F_CAPS);
    }
    ast_glow(g, (float)L->jump_x, (float)L->jump_y, 6, 0xFFFFFF, jk);
    ast_text(g, (float)L->jump_x, (float)(L->jump_y + L->jump_r + 10), u->s_jump, ts, 0x9AF0FF, 150, AST_F_CENTER);
    /* pause */
    arc(g, (float)L->pause_x, (float)L->pause_y, (float)L->pause_r, 0, TAU, dim, 0.8f, 120);
    ast_line(g, L->pause_x - 7.0f, L->pause_y - 10.0f, L->pause_x - 7.0f, L->pause_y + 10.0f, 0xFFFFFF, 0.9f, 190, AST_F_CAPS);
    ast_line(g, L->pause_x + 7.0f, L->pause_y - 10.0f, L->pause_x + 7.0f, L->pause_y + 10.0f, 0xFFFFFF, 0.9f, 190, AST_F_CAPS);
    (void)t;
}

static void mini_ship(ast_gfx_t *g, float x, float y, float r, uint32_t c, int k)
{
    float xy[12];
    for (int i = 0; i < 6; i++) {
        float px = SHIP_PTS[i * 2], py = SHIP_PTS[i * 2 + 1];
        xy[i * 2] = x + py * r;
        xy[i * 2 + 1] = y - px * r;
    }
    ast_poly(g, xy, 6, c, 0.6f, k);
}

static void diamond(ast_gfx_t *g, float x, float y, float r, uint32_t c, int k)
{
    float xy[8] = { x, y - r * 1.3f, x + r, y, x, y + r * 1.3f, x - r, y };
    ast_poly(g, xy, 4, c, 0.6f, k);
}

static void power_bars(ast_gfx_t *g, const ag_ship_t *sh, float x, float y, float w)
{
    static const float MAXT[PW_N] = { 12.0f, 1.0f, 7.0f, 15.0f, 1.0f };
    for (int p = 0; p < PW_N; p++) {
        if (p == PW_SHIELD || p == PW_LIFE || sh->pw_t[p] <= 0) continue;
        float f = sh->pw_t[p] / MAXT[p];
        ast_line(g, x, y, x + w, y, PW_COL[p], 0.5f, 60, AST_F_CAPS);
        ast_line(g, x, y, x + w * f, y, PW_COL[p], 0.8f, 220, AST_F_CAPS);
        y += 14.0f;
    }
}

static void draw_hud(ast_app_t *a, ast_gfx_t *g)
{
    const ast_layout_t *L = &a->lay;
    const ag_game_t *gm = a->game;
    ast_ui_t *u = a->ui;
    char buf[64];
    uint32_t acc = ast_sector_color(ast_palette(gm), 0);
    float lx, ly, rx, ry, us;
    if (L->land) {
        bool lefty = a->cfg.lefty != 0;
        lx = lefty ? (float)(L->W - 196) : 16.0f;
        ly = 22.0f;
        rx = lefty ? 184.0f : (float)(L->W - 76);
        ry = 22.0f;
        us = 3.6f;
    } else {
        lx = (float)L->fx + 16.0f;
        ly = (float)L->fy + 16.0f;
        rx = (float)(L->fx + L->fw - 80);
        ry = ly;
        us = 4.0f;
    }
    snprintf(buf, sizeof buf, "%07d", gm->score);
    ast_text(g, lx, ly, buf, us, 0xFFFFFF, 230, 0);
    /* lives, as small ships */
    float sy = ly + us * 6.0f + 20.0f;
    for (int s = 0; s < gm->players; s++) {
        const ag_ship_t *sh = &gm->ship[s];
        int n = sh->lives - (sh->alive ? 1 : 0);
        if (n < 0) n = 0;
        for (int i = 0; i < n && i < 6; i++) mini_ship(g, lx + 9.0f + (float)i * 22.0f, sy, 10.0f, SHIP_COL[s], 220);
        sy += 26.0f;
    }
    /* where we are */
    if (gm->mode == AG_CAMPAIGN) snprintf(buf, sizeof buf, "%s %d  %d/%d", u->s_sector, gm->sector, gm->wave, gm->waves);
    else snprintf(buf, sizeof buf, "%s %d", u->s_wave, gm->sector);
    float lab_u = L->land ? 2.4f : 2.6f;
    if (L->land) {
        ast_text(g, lx, sy + 4.0f, buf, lab_u, acc, 170, 0);
        if (gm->mode == AG_ARCADE && a->cfg.best[0] > 0) {
            snprintf(buf, sizeof buf, "%s %d", u->s_record, a->cfg.best[0] > gm->score ? a->cfg.best[0] : gm->score);
            ast_text(g, lx, sy + 30.0f, buf, 2.2f, 0x9AA8B8, 150, 0);
        }
    } else {
        ast_text(g, rx, ly + us * 6.0f + 14.0f, buf, lab_u, acc, 170, AST_F_RIGHT);
        if (gm->mode == AG_ARCADE && a->cfg.best[0] > 0) {
            snprintf(buf, sizeof buf, "%s %d", u->s_record, a->cfg.best[0] > gm->score ? a->cfg.best[0] : gm->score);
            ast_text(g, rx, ly + us * 6.0f + 40.0f, buf, 2.2f, 0x9AA8B8, 150, AST_F_RIGHT);
        }
    }
    /* crystals */
    snprintf(buf, sizeof buf, "%d", gm->crystals);
    float cw = ast_text_width(buf, us);
    ast_text(g, rx, ry, buf, us, 0xB8FBFF, 230, AST_F_RIGHT);
    diamond(g, rx - cw - 16.0f, ry + us * 3.0f, 7.0f, 0xB8FBFF, 230);
    /* the powers' time left */
    for (int s = 0; s < gm->players; s++) {
        const ag_ship_t *sh = &gm->ship[s];
        float px = L->land ? rx - 150.0f : rx - 120.0f, py = ry + us * 6.0f + (L->land ? 24.0f : 60.0f) + s * 44.0f;
        power_bars(g, sh, px, py, L->land ? 150.0f : 120.0f);
    }
    /* the boss's life */
    if (gm->boss.on && gm->boss.state) {
        const ag_boss_t *b = &gm->boss;
        float tot = b->hp_max, now = b->hp > 0 ? b->hp : 0;
        for (int p = 0; p < b->nparts; p++) {
            now += b->part_hp[p] > 0 ? b->part_hp[p] : 0;
        }
        if (b->kind == BOSS_SERPIENTE) {
            for (int k = 0; k < AG_SEGS; k++) now += b->seg_hp[k] > 0 ? b->seg_hp[k] : 0;
        }
        static float s_max;
        if (now > s_max || b->t < 0.1f) s_max = now;
        tot = s_max > 0 ? s_max : 1;
        float x0 = (float)L->fx + 60.0f, x1 = (float)(L->fx + L->fw) - 60.0f;
        float y = (float)L->fy + (L->land ? 24.0f : 104.0f);
        ast_line(g, x0, y, x1, y, 0xFF5050, 0.6f, 70, AST_F_CAPS);
        ast_line(g, x0, y, x0 + (x1 - x0) * (now / tot), y, 0xFF5050, 1.0f, 230, AST_F_CAPS);
    }
}

static void banner(ast_app_t *a, ast_gfx_t *g, const char *small, const char *big, uint32_t col, int k)
{
    const ast_layout_t *L = &a->lay;
    float cx = (float)L->fx + (float)L->fw / 2, cy = (float)L->fy + (float)L->fh * 0.42f;
    float maxw = (float)L->fw - 70.0f;
    if (small && small[0]) {
        float us = 5.0f, w = ast_text_width(small, 1.0f);
        if (w * us > maxw) us = maxw / w;
        ast_text(g, cx, cy - 64.0f, small, us, 0xFFFFFF, k * 3 / 4, AST_F_CENTER);
    }
    if (big && big[0]) {
        float ub = 10.0f, w = ast_text_width(big, 1.0f);
        if (w * ub > maxw) ub = maxw / w;
        ast_text(g, cx, cy - ub * 3.0f + 26.0f, big, ub, col, k, AST_F_CENTER);
    }
}

static int fade(float t, float len)
{
    float k = t < 0.25f ? t / 0.25f : t > len - 0.4f ? (len - t) / 0.4f : 1.0f;
    if (k < 0) k = 0;
    return (int)(255.0f * k);
}

static void draw_banners(ast_app_t *a, ast_gfx_t *g)
{
    const ag_game_t *gm = a->game;
    ast_ui_t *u = a->ui;
    char buf[48];
    uint32_t acc = ast_sector_color(ast_palette(gm), 0);
    switch (gm->state) {
    case AG_ST_INTRO:
        if (gm->mode == AG_CAMPAIGN && gm->wave == 1) {
            snprintf(buf, sizeof buf, "%s %d", u->s_sector, gm->sector);
            banner(a, g, buf, u->s_sector_name[gm->sector - 1], acc, fade(gm->state_t, 1.8f));
        } else {
            if (gm->mode == AG_CAMPAIGN) snprintf(buf, sizeof buf, "%s %d/%d", u->s_wave, gm->wave, gm->waves);
            else snprintf(buf, sizeof buf, "%s %d", u->s_wave, gm->sector);
            banner(a, g, NULL, buf, acc, fade(gm->state_t, 1.8f));
        }
        break;
    case AG_ST_BOSS_IN: {
        int k = fmodf(gm->state_t, 0.5f) < 0.32f ? 255 : 120;
        banner(a, g, u->s_boss, u->s_boss_name[gm->boss_kind & 3], 0xFF4A4A, k * fade(gm->state_t, 2.6f) / 255);
        break;
    }
    case AG_ST_CLEAR:
        if (gm->mode == AG_CAMPAIGN && gm->wave == gm->waves) banner(a, g, NULL, u->s_clear, acc, fade(gm->state_t, 2.6f));
        break;
    case AG_ST_DONE: banner(a, g, NULL, u->s_clear, acc, 255); break;
    case AG_ST_WON: banner(a, g, NULL, u->s_won, 0xFFE45C, 255); break;
    case AG_ST_OVER: banner(a, g, NULL, u->s_over, 0xFF5A5A, gm->state_t < 0.3f ? (int)(gm->state_t * 850) : 255); break;
    default: break;
    }
}

void ast_scene_draw(ast_app_t *a, float dt)
{
    ast_gfx_t *g = &a->gfx;
    ag_game_t *gm = a->game;
    ast_ui_t *u = a->ui;
    const ast_layout_t *L = &a->lay;
    u->t += dt;
    parts_step(a, dt);
    if (u->shake > 0) {
        u->shake -= dt * (8.0f + u->shake * 5.0f);
        if (u->shake < 0) u->shake = 0;
    }
    if (u->flash > 0) u->flash -= dt;
    float k = a->cfg.shake ? u->shake : 0;
    u->sx = k > 0.3f ? (frand() - 0.5f) * k : 0;
    u->sy = k > 0.3f ? (frand() - 0.5f) * k : 0;
    uint32_t pri = ast_sector_color(ast_palette(gm), 0);
    float t = u->t;

    ast_gfx_begin(g);
    view_t v = { g, (float)L->fx + u->sx, (float)L->fy + u->sy, gm->W, gm->H };
    ast_gfx_clip(g, true, L->fx, L->fy, L->fx + L->fw, L->fy + L->fh);

    for (int i = 0; i < AG_CRYST; i++) {
        const ag_cryst_t *c = &gm->cryst[i];
        if (!c->on || (c->life < 2.0f && fmodf(c->life, 0.2f) < 0.08f)) continue;
        float r = 6.0f;
        float sp = sinf(t * 4.0f + (float)i);
        float pts[8] = { 0, -1.3f, 1.0f * sp, 0, 0, 1.3f, -1.0f * sp, 0 };
        shape(&v, c->x, c->y, 0, r, pts, 4, 0xB8FBFF, 0.6f, 220);
    }
    for (int i = 0; i < AG_PICKS; i++) {
        if (gm->pick[i].on) draw_pick(&v, &gm->pick[i], t);
    }
    for (int i = 0; i < AG_ROCKS; i++) {
        if (gm->rock[i].on) draw_rock(&v, gm, &gm->rock[i], pri, t);
    }
    for (int i = 0; i < AG_EBULS; i++) {
        const ag_ebul_t *b = &gm->ebul[i];
        if (!b->on) continue;
        if (b->kind == EB_SHOT) {
            float xs[4], ys[4];
            int n = copies(&v, b->x, b->y, 10, xs, ys);
            for (int q = 0; q < n; q++)
                ast_line(g, xs[q], ys[q], xs[q] - b->vx * 0.03f, ys[q] - b->vy * 0.03f, 0xFF5040, 1.0f, 240, AST_F_CAPS);
        } else if (b->kind == EB_ORB) {
            dot(&v, b->x, b->y, 12, 0xFF6FB5, 200);
            circle(&v, b->x, b->y, 6.0f, 8, 0xFFC0E0, 0.7f, 220);
        } else {
            bool armed = b->arm <= 0;
            uint32_t c = armed && fmodf(t, 0.5f) < 0.25f ? 0xFF4A4A : 0xB07CFF;
            float P[12];
            for (int j = 0; j < 6; j++) {
                float rr = j & 1 ? 0.45f : 1.0f;
                P[j * 2] = cosf(TAU * j / 6.0f) * rr;
                P[j * 2 + 1] = sinf(TAU * j / 6.0f) * rr;
            }
            shape(&v, b->x, b->y, t * 1.5f + (float)i, 12.0f, P, 6, c, 0.8f, armed ? 240 : 140);
        }
    }
    for (int i = 0; i < AG_ENEMIES; i++) {
        if (gm->en[i].on) draw_enemy(&v, &gm->en[i], t);
    }
    draw_boss(&v, gm, t, pri);
    for (int i = 0; i < AG_SHOTS; i++) {
        const ag_shot_t *b = &gm->shot[i];
        if (!b->on) continue;
        float xs[4], ys[4];
        int n = copies(&v, b->x, b->y, 16, xs, ys);
        for (int q = 0; q < n; q++)
            ast_line(g, xs[q], ys[q], xs[q] - b->vx * 0.022f, ys[q] - b->vy * 0.022f, SHIP_COL[b->owner & 1], 0.9f, 255,
                     AST_F_CAPS);
    }
    for (int s = 0; s < gm->players; s++) draw_ship(&v, a, s, t, dt);
    for (int i = 0; i < u->npart; i++) {
        const ast_part_t *p = &u->part[i];
        float f = p->life / p->max;
        float x = v.ox + p->x, y = v.oy + p->y;
        switch (p->kind) {
        case 0:
            ast_line(g, x, y, x - p->vx * 0.035f, y - p->vy * 0.035f, p->color, 0.6f, (int)(250 * f), AST_F_CAPS);
            break;
        case 1:
            ast_glow(g, x, y, (int)(p->size * (1.2f - 0.5f * f)), p->color, (int)(200 * f));
            break;
        case 2: {
            float r = p->a + (p->size - p->a) * (1.0f - f);
            int n = r > 40 ? 28 : 16;
            float px = x + r, py = y;
            for (int j = 1; j <= n; j++) {
                float aa = TAU * (float)j / (float)n;
                float qx = x + cosf(aa) * r, qy = y + sinf(aa) * r;
                ast_line(g, px, py, qx, qy, p->color, 0.8f, (int)(230 * f), AST_F_CAP0);
                px = qx;
                py = qy;
            }
            break;
        }
        case 3: {
            float c = cosf(p->a) * p->size * 0.5f, sn = sinf(p->a) * p->size * 0.5f;
            ast_line(g, x - c, y - sn, x + c, y + sn, p->color, 0.7f, (int)(230 * f), AST_F_CAPS);
            break;
        }
        default: break;
        }
    }
    for (int i = 0; i < AST_TEXTS; i++) {
        const ast_ftext_t *tx = &u->text[i];
        if (tx->t <= 0) continue;
        int kk = tx->t > 0.5f ? 230 : (int)(460 * tx->t);
        ast_text(g, v.ox + tx->x, v.oy + tx->y, tx->s, 2.8f, tx->color, kk, AST_F_CENTER);
    }
    if (u->flash > 0 && a->cfg.shake) {
        ast_rect(g, L->fx, L->fy, L->fw, L->fh, 0xFFFFFF, (int)(u->flash * 300.0f), AST_F_ADD);
    }
    if (u->title) {
        /* the menus' still: the name over the field */
        float tw = ast_text_width(u->s_title, 1.0f);
        float us = ((float)(L->land ? 760 : L->fw) - 80.0f) / tw;
        if (us > 16.0f) us = 16.0f;
        float ty = L->land ? 50.0f : 200.0f;
        ast_text(g, (float)L->fx + (float)L->fw / 2, ty, u->s_title, us, pri, 255, AST_F_CENTER);
        ast_gfx_clip(g, false, 0, 0, 0, 0);
        return;
    }
    draw_banners(a, g);
    ast_gfx_clip(g, false, 0, 0, 0, 0);
    draw_hud(a, g);
    draw_deck(a, g, t);
}
