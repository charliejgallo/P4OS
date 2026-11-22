/*
 * HOCKEY - a frame of the match, as a list for the compositor
 *
 * The table's picture has everything that does not move. Over it, in
 * order: the shadows of the pieces, their glow on a neon table (and the
 * puck's trail), the light of a goal, the ring of a hard hit, a puck going
 * down the far goal's slot and the far end drawn again over it, the
 * mallets and the puck from far to near, the near end drawn again (it
 * hides the foot of a mallet against it, and a puck down the near slot),
 * the fingers, the scores and the clock in the rails' windows, the pause
 * button, and the banners. With two players the far player's texts are
 * turned half a turn, for them.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "hk_app.h"
#include "hk_view.h"

#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static uint32_t lighter(uint32_t c, int k)
{
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    r += (255 - r) * k / 256;
    g += (255 - g) * k / 256;
    b += (255 - b) * k / 256;
    return (uint32_t)((r << 16) | (g << 8) | b);
}

/* ---- the texts --------------------------------------------------------------------------------------- */

void hk_scene_strings(hk_app_t *a)
{
    hk_ui_t *u = a->ui;
    bool two = a->cfg.mode == MODE_TWO;
    snprintf(u->s_goal, sizeof u->s_goal, "%s", _("¡GOL!"));
    snprintf(u->s_replay, sizeof u->s_replay, "%s", _("REPETICIÓN"));
    snprintf(u->s_full, sizeof u->s_full, "%s", _("FINAL"));
    snprintf(u->s_golden, sizeof u->s_golden, "%s", _("GOL DE ORO"));
    snprintf(u->s_hint, sizeof u->s_hint, "%s", _("Mové la paleta con el dedo"));
    snprintf(u->name[0], sizeof u->name[0], "%s", two ? _("Jugador 1") : _("Vos"));
    snprintf(u->name[1], sizeof u->name[1], "%s", two ? _("Jugador 2") : _("La placa"));
    snprintf(u->s_win[0], sizeof u->s_win[0], "%s", two ? _("GANÓ EL JUGADOR 1") : _("¡GANASTE!"));
    snprintf(u->s_win[1], sizeof u->s_win[1], "%s", two ? _("GANÓ EL JUGADOR 2") : _("GANÓ LA PLACA"));
    u->two = two;
}

/* ---- the pieces ---------------------------------------------------------------------------------------- */

static void puck_draw(hk_app_t *a, const hk_game_t *g)
{
    hk_spr(&a->gfx, &a->art.puck, 0, iround(hk_sx(g->px)), iround(hk_sy(g->py, 0)), 0, 255);
}

static void mallet_draw(hk_app_t *a, const hk_game_t *g, int s)
{
    int k = a->cfg.paddle < 0 || a->cfg.paddle > 2 ? 1 : a->cfg.paddle;
    const hk_mallet_t *m = &g->m[s];
    hk_spr(&a->gfx, &a->art.mallet[s][k], 0, iround(hk_sx(m->x)), iround(hk_sy(m->y, 0)), 0, 255);
}

/* a text for one player, or for both: the far player's turned, mirrored about the centre */
static void say(hk_app_t *a, const hk_font_t *f, int y, const char *s, uint16_t col, int flags)
{
    hk_gfx_t *g = &a->gfx;
    if (!a->ui->two) {
        hk_text(g, f, HK_W / 2, y, s, col, flags | HK_F_CENTER);
        return;
    }
    /* the near player's below the centre line, the far one's above it, turned */
    int yn = y + 150;
    hk_text(g, f, HK_W / 2, yn, s, col, flags | HK_F_CENTER);
    hk_text(g, f, HK_W / 2, 2 * HK_CY - yn - f->line_h, s, col, flags | HK_F_CENTER | HK_F_ROT);
}

static void hud(hk_app_t *a, const hk_game_t *g)
{
    hk_gfx_t *gx = &a->gfx;
    hk_ui_t *u = a->ui;
    const hk_table_t *t = &a->art.tb[a->art.cur];
    const hk_font_t *fs = &a->art.f_score, *fm = &a->art.f_clock;
    char s[16];
    for (int side = 0; side < 2; side++) {
        int cy = side ? HK_WIN_Y1 : HK_WIN_Y0;
        snprintf(s, sizeof s, "%d", g->score[side]);
        uint16_t col = hk_rgb(lighter(t->side[side], 90));
        int flags = HK_F_CENTER | (u->two && side == 1 ? HK_F_ROT : 0);
        hk_glow(gx, HK_WIN_X, cy, 30, hk_rgb(t->side[side]), 70);
        hk_text(gx, fs, HK_WIN_X, cy - fs->line_h / 2, s, col, flags);
    }
    /* the clock: what is left of the time, or the time played when it goes to goals */
    int secs;
    if (g->minutes > 0) secs = g->minutes * 60 - (int)g->clock;
    else secs = (int)g->clock;
    if (secs < 0) secs = 0;
    bool blink = g->golden && ((int)(u->t * 2.0f) & 1);
    if (!blink) {
        snprintf(s, sizeof s, "%d:%02d", secs / 60, secs % 60);
        uint16_t col = hk_rgb(g->golden ? 0xFFD040 : 0xFFB040);
        hk_text(gx, fm, HK_CLOCK_X, HK_CLOCK_Y - fm->line_h / 2, s, col, HK_F_CENTER);
    }
    /* the pause: two bars in the bezel */
    hk_disc(gx, HK_PAUSE_X, HK_PAUSE_Y, 17, 16, hk_rgb(0x15181E), 230);
    hk_rect(gx, HK_PAUSE_X - 8, HK_PAUSE_Y - 9, 6, 18, hk_rgb(0xFFFFFF), 255, 2);
    hk_rect(gx, HK_PAUSE_X + 2, HK_PAUSE_Y - 9, 6, 18, hk_rgb(0xFFFFFF), 255, 2);
}

void hk_scene_draw(hk_app_t *a, float dt)
{
    hk_game_t *g = a->game;
    hk_ui_t *u = a->ui;
    hk_art_t *art = &a->art;
    hk_gfx_t *gx = &a->gfx;
    const hk_table_t *t = &art->tb[art->cur >= 0 ? art->cur : 0];
    u->t += dt;
    if (u->hint_t > 0) u->hint_t -= dt;

    /* what happened since the last frame */
    int goals = g->score[0] + g->score[1];
    if (goals != u->seen_goals) {
        u->seen_goals = goals;
        if (g->state == HK_ST_GOAL) {
            u->banner = 1;
            u->banner_t = 2.0f;
            u->banner_side = g->last_goal;
            u->goal_t = 1.8f;
            u->goal_side = g->last_goal ^ 1;
        }
    }
    if (g->state != u->seen_state) {
        if (g->state == HK_ST_END) {
            u->banner = 2;
            u->banner_t = 30.0f;
        }
        u->seen_state = g->state;
    }
    if (g->golden && !u->seen_golden) {
        u->seen_golden = true;
        u->banner = 3;
        u->banner_t = 2.2f;
    }
    if (u->banner_t > 0) u->banner_t -= dt;
    else u->banner = 0;
    if (u->goal_t > 0) u->goal_t -= dt;
    if (g->hits != u->hits_seen) {
        u->hits_seen = g->hits;
        if (g->hit_v > 500.0f) {
            u->ring_t = 0.22f;
            u->ring_x = (int16_t)iround(hk_sx(g->hit_x));
            u->ring_y = (int16_t)iround(hk_sy(g->hit_y, 3.0f));
        }
    }
    if (u->ring_t > 0) u->ring_t -= dt;
    if (t->glow) {
        u->trail_acc += dt;
        if (u->trail_acc >= 1.0f / 50.0f) {
            u->trail_acc = 0;
            u->trail_at = (u->trail_at + 1) % HK_TRAIL;
            u->trail_x[u->trail_at] = g->px;
            u->trail_y[u->trail_at] = g->py;
        }
    }

    hk_gfx_begin(gx);
    int k = a->cfg.paddle < 0 || a->cfg.paddle > 2 ? 1 : a->cfg.paddle;
    float mr = g->m[0].r;
    (void)k;

    /* the shadows: the lamp is over the table, a little towards the far end */
    for (int s = 0; s < 2; s++) {
        const hk_mallet_t *m = &g->m[s];
        int sx = iround(hk_sx(m->x)) + 5, sy = iround(hk_sy(m->y, 0)) + 6;
        hk_shadow(gx, sx, sy, iround(mr + 5), iround((mr + 5) * HK_COS), 120);
    }
    if (g->puck_in && fabsf(g->py) < HK_FY + 4.0f)
        hk_shadow(gx, iround(hk_sx(g->px)) + 3, iround(hk_sy(g->py, 0)) + 4, 28, 26, 100);

    /* a neon table: everything shines, the puck leaves a trail */
    if (t->glow) {
        for (int s = 0; s < 2; s++) {
            const hk_mallet_t *m = &g->m[s];
            hk_glow(gx, iround(hk_sx(m->x)), iround(hk_sy(m->y, 10)), iround(mr + 34), hk_rgb(t->side[s]), 150);
        }
        if (g->puck_in) {
            for (int i = 1; i < HK_TRAIL; i++) {
                int j = (u->trail_at - i + HK_TRAIL) % HK_TRAIL;
                float dx = u->trail_x[j] - g->px, dy = u->trail_y[j] - g->py;
                if (dx * dx + dy * dy < 30.0f * 30.0f) continue;
                hk_glow(gx, iround(hk_sx(u->trail_x[j])), iround(hk_sy(u->trail_y[j], 3)), 30 - i * 2,
                        hk_rgb(t->puck), 150 - i * 13);
            }
            hk_glow(gx, iround(hk_sx(g->px)), iround(hk_sy(g->py, 3)), 54, hk_rgb(t->puck), 170);
        }
    }

    /* a goal lights its mouth */
    if (u->goal_t > 0) {
        float d = hk_dir(u->goal_side);
        int pulse = (int)(160.0f + 90.0f * sinf(u->t * 22.0f));
        hk_glow(gx, HK_CX, iround(hk_sy(d * HK_FY, 16)), 150, hk_rgb(t->side[u->banner_side]), pulse);
    }
    if (u->ring_t > 0) {
        float k2 = 1.0f - u->ring_t / 0.22f;
        int r = 20 + (int)(k2 * 34.0f);
        hk_ring(gx, u->ring_x, u->ring_y, r, iround((float)r * HK_COS), 3, hk_rgb(0xFFFFFF), (int)(200 * (1.0f - k2)),
                HK_F_ADD);
    }

    /* a puck down the far slot: behind the far end */
    bool puck_done = false;
    if (g->puck_in && g->py < -HK_FY + 1.0f) {
        puck_draw(a, g);
        hk_spr(gx, &art->far, 0, 0, 0, 0, 255);
        puck_done = true;
    }
    /* the pieces, far to near */
    float ys[3] = { g->m[1].y, g->py, g->m[0].y };
    int order[3] = { 1, 2, 0 };     /* 1: far mallet, 2: the puck, 0: near mallet */
    for (int i = 1; i < 3; i++) {
        for (int j = i; j > 0 && ys[j] < ys[j - 1]; j--) {
            float ty = ys[j];
            ys[j] = ys[j - 1];
            ys[j - 1] = ty;
            int to = order[j];
            order[j] = order[j - 1];
            order[j - 1] = to;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (order[i] == 2) {
            if (g->puck_in && !puck_done) puck_draw(a, g);
        } else {
            mallet_draw(a, g, order[i]);
        }
    }
    hk_spr(gx, &art->near, 0, 0, 0, 0, 255);

    /* the fingers: a soft ring where each one is */
    for (int i = 0; i < 5; i++) {
        if (u->touch_on[i]) hk_ring(gx, u->touch_x[i], u->touch_y[i], 30, 30, 3, hk_rgb(0xFFFFFF), 60, 0);
    }

    if (!u->hud_off) hud(a, g);

    /* the banners */
    const hk_font_t *fb = &art->f_big, *fs = &art->f_score, *fm = &art->f_small;
    if (u->banner == 1) {
        float kk = 2.0f - u->banner_t;
        int alpha = kk < 0.15f ? (int)(kk / 0.15f * 255) : u->banner_t < 0.3f ? (int)(u->banner_t / 0.3f * 255) : 255;
        uint16_t col = hk_rgb(lighter(t->side[u->banner_side], 110));
        if (!u->two) {
            hk_rect(gx, 0, 520, HK_W, 200, hk_rgb(0x0A0E16), alpha * 170 / 255, 0);
            hk_text(gx, fb, HK_W / 2, 530, u->s_goal, hk_rgb(0xFFE14A), HK_F_CENTER | HK_F_SHADOW);
            hk_text(gx, fs, HK_W / 2, 650, u->name[u->banner_side], col, HK_F_CENTER | HK_F_SHADOW);
        } else {
            hk_rect(gx, 0, 380, HK_W, 500, hk_rgb(0x0A0E16), alpha * 150 / 255, 0);
            hk_text(gx, fb, HK_W / 2, 660, u->s_goal, hk_rgb(0xFFE14A), HK_F_CENTER | HK_F_SHADOW);
            hk_text(gx, fs, HK_W / 2, 776, u->name[u->banner_side], col, HK_F_CENTER | HK_F_SHADOW);
            hk_text(gx, fb, HK_W / 2, 2 * HK_CY - 660 - fb->line_h, u->s_goal, hk_rgb(0xFFE14A),
                    HK_F_CENTER | HK_F_SHADOW | HK_F_ROT);
            hk_text(gx, fs, HK_W / 2, 2 * HK_CY - 776 - fs->line_h, u->name[u->banner_side], col,
                    HK_F_CENTER | HK_F_SHADOW | HK_F_ROT);
        }
    } else if (u->banner == 2) {
        int w = g->score[0] >= g->score[1] ? 0 : 1;
        if (!u->two) {
            hk_rect(gx, 0, 540, HK_W, 150, hk_rgb(0x0A0E16), 180, 0);
            hk_text(gx, fs, HK_W / 2, 556, u->s_full, hk_rgb(0xFFFFFF), HK_F_CENTER | HK_F_SHADOW);
            hk_text(gx, fs, HK_W / 2, 612, u->s_win[w], hk_rgb(0xFFE14A), HK_F_CENTER | HK_F_SHADOW);
        } else {
            hk_rect(gx, 0, 420, HK_W, 420, hk_rgb(0x0A0E16), 170, 0);
            say(a, fs, 546, u->s_full, hk_rgb(0xFFFFFF), HK_F_SHADOW);
            say(a, fs, 602, u->s_win[w], hk_rgb(0xFFE14A), HK_F_SHADOW);
        }
    } else if (u->banner == 3) {
        if (!u->two) {
            hk_rect(gx, 0, 560, HK_W, 70, hk_rgb(0x0A0E16), 170, 0);
        } else {
            hk_rect(gx, 0, 710, HK_W, 70, hk_rgb(0x0A0E16), 170, 0);
            hk_rect(gx, 0, 2 * HK_CY - 780, HK_W, 70, hk_rgb(0x0A0E16), 170, 0);
        }
        say(a, fs, 572, u->s_golden, hk_rgb(0xFFD040), HK_F_SHADOW);
    }
    if (g->state == HK_ST_REPLAY) {
        int w = hk_text_width(fm, u->s_replay) + 56;
        int ry = HK_CY - 21;
        hk_rect(gx, HK_W / 2 - w / 2, ry, w, 42, hk_rgb(0x10141C), 210, 12);
        if (((int)(u->t * 2.0f)) & 1) hk_disc(gx, HK_W / 2 - w / 2 + 22, ry + 21, 8, 8, hk_rgb(0xFF3030), 255);
        hk_text(gx, fm, HK_W / 2 + 12, ry + 8, u->s_replay, hk_rgb(0xFFFFFF), HK_F_CENTER);
    }
    if (u->hint_t > 0 && g->state != HK_ST_REPLAY && !u->hud_off) {
        int al = u->hint_t < 0.6f ? (int)(u->hint_t / 0.6f * 220) : 220;
        int w = hk_text_width(fm, u->s_hint) + 40;
        int y = 1010;
        hk_rect(gx, HK_W / 2 - w / 2, y - 8, w, 44, hk_rgb(0x10141C), al, 12);
        hk_text(gx, fm, HK_W / 2, y, u->s_hint, hk_rgb(0xFFFFFF), HK_F_CENTER);
        if (u->two) {
            int yr = 2 * HK_CY - y - fm->line_h;
            hk_rect(gx, HK_W / 2 - w / 2, yr - 8, w, 44, hk_rgb(0x10141C), al, 12);
            hk_text(gx, fm, HK_W / 2, yr, u->s_hint, hk_rgb(0xFFFFFF), HK_F_CENTER | HK_F_ROT);
        }
    }
}
