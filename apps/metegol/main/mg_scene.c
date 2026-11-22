/*
 * METEGOL - a frame of the match, as a list for the compositor
 *
 * The stadium's picture has the table in it. Over it, in order:
 *   the crowd's frame (the stands moving), the shadows on the field, the
 *   ball when it is under a rod, the rods (their shafts are bars the
 *   compositor need not redraw when they slide: mg_col), the far band again
 *   (it hides the rods going into it), the figures and the ball from the
 *   far side to the near one, the near band again (it hides what is behind
 *   it), the near ends of the rods (in front of the band), a ring where the
 *   ball is if the band hides it, and the television's scoreboard, the
 *   banners, the replay's label and the paper the crowd throws.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mg_app.h"
#include "mg_view.h"

#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define FY      340.0f
#define NEAR_OUT (FY + MG_WALL_T)       /* the near band's outer face */
#define FIG_FRAMES 32

static const float ROD_TRAVEL[MG_SIDE_RODS] = { 160.0f, 190.0f, 71.0f, 110.0f };

static int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

/* ---- the texts --------------------------------------------------------------------------------------- */

void mg_scene_strings(mg_app_t *a)
{
    mg_ui_t *u = a->ui;
    int lang = a->cfg.board_lang;
    bool en = lang == 2 || (lang == 0 && mg_lang() != 0);
    bool de = lang == 0 && mg_lang() == 2;
    /* the scoreboard speaks the language asked for; "as the system" goes
     * through the catalogue */
    if (lang == 0) {
        snprintf(u->s_goal, sizeof u->s_goal, "%s", _("¡GOL!"));
        snprintf(u->s_replay, sizeof u->s_replay, "%s", _("REPETICIÓN"));
        snprintf(u->s_half, sizeof u->s_half, "%s", _("ENTRETIEMPO"));
        snprintf(u->s_full, sizeof u->s_full, "%s", _("FINAL DEL PARTIDO"));
        snprintf(u->s_hint, sizeof u->s_hint, "%s", _("Deslizá para mover · tocá para patear · mantené para levantar"));
        snprintf(u->s_half_n[0], sizeof u->s_half_n[0], "%s", _("1T"));
        snprintf(u->s_half_n[1], sizeof u->s_half_n[1], "%s", _("2T"));
        snprintf(u->s_goals_to, sizeof u->s_goals_to, "%s", _("A %d"));
    } else if (en) {
        snprintf(u->s_goal, sizeof u->s_goal, "GOAL!");
        snprintf(u->s_replay, sizeof u->s_replay, "REPLAY");
        snprintf(u->s_half, sizeof u->s_half, "HALF TIME");
        snprintf(u->s_full, sizeof u->s_full, "FULL TIME");
        snprintf(u->s_hint, sizeof u->s_hint, "Slide to move · tap to kick · hold to raise");
        snprintf(u->s_half_n[0], sizeof u->s_half_n[0], "1H");
        snprintf(u->s_half_n[1], sizeof u->s_half_n[1], "2H");
        snprintf(u->s_goals_to, sizeof u->s_goals_to, "TO %%d");
    } else {
        snprintf(u->s_goal, sizeof u->s_goal, "¡GOL!");
        snprintf(u->s_replay, sizeof u->s_replay, "REPETICIÓN");
        snprintf(u->s_half, sizeof u->s_half, "ENTRETIEMPO");
        snprintf(u->s_full, sizeof u->s_full, "FINAL DEL PARTIDO");
        snprintf(u->s_hint, sizeof u->s_hint, "Deslizá para mover · tocá para patear · mantené para levantar");
        snprintf(u->s_half_n[0], sizeof u->s_half_n[0], "1T");
        snprintf(u->s_half_n[1], sizeof u->s_half_n[1], "2T");
        snprintf(u->s_goals_to, sizeof u->s_goals_to, "A %%d");
    }
    (void)de;
    for (int t = 0; t < 2; t++) {
        memcpy(u->abbr[t], a->tm[t].abbr, 4);
        snprintf(u->name[t], sizeof u->name[t], "%s", a->tm[t].name);
    }
}

/* ---- the table ------------------------------------------------------------------------------------- */

typedef struct {
    float y;
    int   kind;     /* 0 a figure, 1 the ball */
    int   rod, fig;
} thing_t;

static void rod_draw(mg_app_t *a, const mg_rod_t *r, int pass, int sx, int dy)
{
    mg_art_t *art = &a->art;
    mg_gfx_t *g = &a->gfx;
    float T = ROD_TRAVEL[r->role];
    float o = r->off;
    bool near_handle = r->side == 0;
    /* along the rod: its far end, its near end (game y), the handle's start */
    float y_far = near_handle ? -(NEAR_OUT + T + 22.0f) : -(NEAR_OUT + T + 14.0f);
    float y_near = near_handle ? NEAR_OUT + T + 14.0f : NEAR_OUT + T + 22.0f;
    y_far += o;
    y_near += o;
    int x = sx + art->rod_prof_x;
    int split = iround(mg_sy(NEAR_OUT, MG_ROD_Z)) + dy;
    int s_far = iround(mg_sy(y_far, MG_ROD_Z)) + dy, s_near = iround(mg_sy(y_near, MG_ROD_Z)) + dy;
    if (pass == 0) {
        mg_col(g, &art->rod_prof, x, s_far, split < s_near ? split : s_near);
        /* the stops, just inside the bands */
        for (int s = -1; s <= 1; s += 2) {
            float yb = (float)s * (FY - 7.0f - T) + o;
            mg_spr(g, &art->bumper, 0, sx, iround(mg_sy(yb, MG_ROD_Z)) + dy, 0, 255);
        }
        if (near_handle) mg_spr(g, &art->efar, 0, sx, s_far, 0, 255);
        else mg_spr(g, &art->hfar, 0, sx, s_far + iround(MG_S * MG_COS * 0.0f), 0, 255);
    } else {
        /* the part out of the near band, in front of it */
        if (s_near > split) mg_col(g, &art->rod_prof, x, split, s_near);
        if (near_handle) mg_spr(g, &art->hnear, 0, sx, s_near, 0, 255);
        else mg_spr(g, &art->enear, 0, sx, s_near, 0, 255);
    }
}

static int fig_frame(const mg_rod_t *r)
{
    float a = r->ang;
    int f = iround(a / (2.0f * 3.14159265f) * FIG_FRAMES);
    return ((f % FIG_FRAMES) + FIG_FRAMES) % FIG_FRAMES;
}

static void ball_draw(mg_app_t *a, const mg_game_t *gm, int dx, int dy)
{
    if (!gm->ball_in) return;
    int f = ((int)(gm->roll / 14.0f)) % (a->art.ball.nframes ? a->art.ball.nframes : 1);
    mg_spr(&a->gfx, &a->art.ball, f, iround(mg_sx(gm->bx)) + dx, iround(mg_sy(gm->by, MG_BALL_R)) + dy, 0, 255);
}

/* ---- the scoreboard and the rest --------------------------------------------------------------------- */

static void scoreboard(mg_app_t *a, const mg_game_t *gm)
{
    mg_gfx_t *g = &a->gfx;
    mg_ui_t *u = a->ui;
    const mg_font_t *f = &a->art.f_score;
    int x = 18, y = 14, h = 46;
    int s0 = gm->score[mg_side_of_team(a, 0)], s1 = gm->score[mg_side_of_team(a, 1)];
    char sc[16];
    snprintf(sc, sizeof sc, "%d - %d", s0, s1);
    int w_ab = 64, w_sc = mg_text_width(f, sc) + 30;
    int w = 12 + 40 + 8 + w_ab + w_sc + w_ab + 8 + 40 + 12;
    mg_rect(g, x, y, w, h, mg_rgb(0x10141C), 225, 10);
    mg_rect(g, x + 12 + 40 + 8 + w_ab, y, w_sc, h, mg_rgb(0xF4F4F4), 255, 0);
    int bx = x + 12;
    mg_spr(g, &a->art.badge[0], 0, bx, y + 10, 0, 255);
    mg_text(g, f, bx + 48, y + 8, u->abbr[0], mg_rgb(0xFFFFFF), 0);
    mg_text(g, f, x + 12 + 40 + 8 + w_ab + w_sc / 2, y + 8, sc, mg_rgb(0x10141C), MG_F_CENTER);
    int rx = x + 12 + 40 + 8 + w_ab + w_sc + 8;
    mg_text(g, f, rx, y + 8, u->abbr[1], mg_rgb(0xFFFFFF), 0);
    mg_spr(g, &a->art.badge[1], 0, rx + w_ab - 2, y + 10, 0, 255);
    /* the clock, or the goals it is played to */
    char ck[24];
    if (gm->halves_min > 0) {
        int s = (int)gm->clock;
        int left = gm->halves_min * 60 - s;
        if (left < 0) left = 0;
        snprintf(ck, sizeof ck, "%s %d:%02d", u->s_half_n[gm->half & 1], left / 60, left % 60);
    } else {
        snprintf(ck, sizeof ck, u->s_goals_to, gm->goals);
    }
    int cw = mg_text_width(f, ck) + 26;
    mg_rect(g, x + w + 6, y, cw, h, mg_rgb(0x0B6E3B), 230, 10);
    mg_text(g, f, x + w + 6 + cw / 2, y + 8, ck, mg_rgb(0xFFFFFF), MG_F_CENTER);
    /* the pause, top right */
    mg_disc(g, 1232, 38, 26, 26, mg_rgb(0x10141C), 190);
    mg_rect(g, 1222, 25, 7, 26, mg_rgb(0xFFFFFF), 255, 2);
    mg_rect(g, 1235, 25, 7, 26, mg_rgb(0xFFFFFF), 255, 2);
}

static void confetti_start(mg_app_t *a, int team)
{
    mg_ui_t *u = a->ui;
    const mg_team_t *t = &a->tm[team];
    uint32_t cols[4] = { t->c1, t->c2, 0xFFFFFF, t->shorts };
    uint32_t s = (uint32_t)(u->t * 1000.0f) | 1u;
    u->nconf = MG_CONFETTI;
    for (int i = 0; i < MG_CONFETTI; i++) {
        s = s * 1664525u + 1013904223u;
        u->conf_x[i] = (float)(s >> 8 & 1023) * 1.25f;
        s = s * 1664525u + 1013904223u;
        u->conf_y[i] = -10.0f - (float)(s >> 8 & 255) * 1.2f;
        s = s * 1664525u + 1013904223u;
        u->conf_vx[i] = ((float)(s >> 8 & 255) - 128.0f) * 0.6f;
        u->conf_vy[i] = 120.0f + (float)(s >> 16 & 127);
        u->conf_c[i] = mg_rgb(cols[i & 3]);
    }
}

void mg_scene_draw(mg_app_t *a, float dt)
{
    mg_game_t *gm = a->game;
    mg_ui_t *u = a->ui;
    mg_art_t *art = &a->art;
    mg_gfx_t *g = &a->gfx;
    u->t += dt;
    if (u->hint_t > 0) u->hint_t -= dt;

    /* what happened since the last frame */
    int goals = gm->score[0] + gm->score[1];
    if (goals != u->seen_goals) {
        u->seen_goals = goals;
        if (gm->state == MG_ST_GOAL) {
            u->banner = 1;
            u->banner_t = 2.2f;
            u->banner_team = mg_team_of_side(a, gm->last_goal);
            if (u->shake_on) u->shake_t = 0.3f;
            confetti_start(a, u->banner_team);
        }
    }
    if (gm->state != u->seen_state) {
        if (gm->state == MG_ST_HALF) {
            u->banner = 2;
            u->banner_t = 2.6f;
        } else if (gm->state == MG_ST_END) {
            u->banner = 3;
            u->banner_t = 9.0f;
        }
        u->seen_state = gm->state;
    }
    if (u->banner_t > 0) u->banner_t -= dt;
    else u->banner = 0;
    if (u->shake_t > 0) u->shake_t -= dt;

    /* the crowd: faster when the ball nears a goal, wild after one */
    float excite = gm->near_goal;
    if (gm->state == MG_ST_GOAL || (u->banner == 1)) excite = 2.0f;
    /* each strip of the stands at its own pace: a change redraws only that
     * strip, and never more than a few a second (the board measured a whole
     * band 16 times a second at 25 fps) */
    int ns = art->crowd_strips > 0 && art->crowd_strips <= 8 ? art->crowd_strips : 1;
    for (int s = 0; s < ns; s++) {
        float rate = (0.8f + 3.2f * (excite > 1.0f ? 1.0f : excite)) * (0.8f + 0.06f * (float)((s * 5) % 8));
        u->crowd_acc[s] += dt * rate;
        if (u->crowd_acc[s] >= 1.0f) {
            u->crowd_acc[s] -= floorf(u->crowd_acc[s]);
            if (art->crowd_n > 1) u->crowd_f[s] = (u->crowd_f[s] + 1) % art->crowd_n;
        }
    }

    mg_gfx_begin(g);
    int dx = 0, dy = 0;
    if (u->shake_t > 0) {
        float k = u->shake_t / 0.3f;
        dx = iround(sinf(u->t * 71.0f) * 7.0f * k);
        dy = iround(cosf(u->t * 53.0f) * 5.0f * k);
    }
    g->salt = (uint32_t)((dx & 0xFF) | ((dy & 0xFF) << 8));
    g->bg_dx = dx;
    g->bg_dy = dy;

    if (u->crowd_on && art->crowd_n) {
        for (int s = 0; s < ns; s++) mg_spr(g, &art->crowd, s * art->crowd_n + u->crowd_f[s], dx, dy, 0, 255);
    }
    if (u->crowd_on && u->banner == 1) {
        /* the stands' flashes */
        for (int i = 0; i < art->nflash; i++) {
            if (((int)(u->t * 9.0f) + i * 3) % 5 == 0)
                mg_glow(g, art->flash[i][0] + dx, art->flash[i][1] + dy, 22, mg_rgb(0xFFFFFF), 255);
        }
    }

    /* the shadows: the light is above and a little behind the camera */
    for (int i = 0; i < MG_RODS; i++) {
        const mg_rod_t *r = &gm->rod[i];
        int sx = iround(mg_sx(r->x)) + dx;
        for (int k = 0; k < r->n; k++) {
            float y = r->base[k] + r->off;
            int sy = iround(mg_sy(y, 0)) + dy;
            float s = sinf(r->ang);
            int ox = iround(s * 24.0f * (r->side ? -1.0f : 1.0f));
            mg_shadow(g, sx + ox / 2, sy - 6, 15 + (ox < 0 ? -ox : ox) / 3, 8, 110);
        }
    }
    if (gm->ball_in) mg_shadow(g, iround(mg_sx(gm->bx)) + dx + 3, iround(mg_sy(gm->by, 0)) + dy - 2, 14, 7, 130);

    /* the ball under a rod goes in before it */
    bool under = false;
    for (int i = 0; i < MG_RODS; i++) {
        if (fabsf(gm->bx - gm->rod[i].x) < MG_BALL_R + 9.0f) under = true;
    }
    if (under) ball_draw(a, gm, dx, dy);
    for (int i = 0; i < MG_RODS; i++) rod_draw(a, &gm->rod[i], 0, iround(mg_sx(gm->rod[i].x)) + dx, dy);
    mg_spr(g, &art->far, 0, dx, dy, 0, 255);

    /* the figures and the ball, far to near */
    thing_t th[24];
    int n = 0;
    for (int i = 0; i < MG_RODS; i++) {
        for (int k = 0; k < gm->rod[i].n && n < 23; k++) {
            th[n].y = gm->rod[i].base[k] + gm->rod[i].off;
            th[n].kind = 0;
            th[n].rod = i;
            th[n].fig = k;
            n++;
        }
    }
    if (!under) {
        th[n].y = gm->by + 6.0f;
        th[n].kind = 1;
        n++;
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && th[j].y < th[j - 1].y; j--) {
            thing_t t = th[j];
            th[j] = th[j - 1];
            th[j - 1] = t;
        }
    }
    for (int i = 0; i < n; i++) {
        if (th[i].kind == 1) {
            ball_draw(a, gm, dx, dy);
            continue;
        }
        const mg_rod_t *r = &gm->rod[th[i].rod];
        int team = mg_team_of_side(a, r->side);
        int sx = iround(mg_sx(r->x)) + dx, sy = iround(mg_sy(th[i].y, 0)) + dy;
        mg_spr(g, &art->fig[team][r->side], fig_frame(r), sx, sy, 0, 255);
    }
    mg_spr(g, &art->near, 0, dx, dy, 0, 255);
    for (int i = 0; i < MG_RODS; i++) rod_draw(a, &gm->rod[i], 1, iround(mg_sx(gm->rod[i].x)) + dx, dy);
    /* a ball behind the near band: its outline */
    if (gm->ball_in && gm->by > FY - 52.0f)
        mg_ring(g, iround(mg_sx(gm->bx)) + dx, iround(mg_sy(gm->by, MG_BALL_R)) + dy, 15, 15, 3, mg_rgb(0xFFFFFF), 150, 0);

    /* the fingers: a soft ring where each one is */
    for (int i = 0; i < 5; i++) {
        if (u->touch_on[i]) mg_ring(g, u->touch_x[i], u->touch_y[i], 34, 34, 3, mg_rgb(0xFFFFFF), 70, 0);
    }

    if (!u->hud_off) scoreboard(a, gm);

    /* the banners */
    const mg_font_t *fb = &art->f_big, *fs = &art->f_score;
    if (u->banner == 1) {
        float k = 2.2f - u->banner_t;
        int alpha = k < 0.15f ? (int)(k / 0.15f * 255) : u->banner_t < 0.3f ? (int)(u->banner_t / 0.3f * 255) : 255;
        mg_rect(g, 0, 250, MG_W, 150, mg_rgb(0x0A0E16), alpha * 170 / 255, 0);
        mg_text(g, fb, MG_W / 2, 262, u->s_goal, mg_rgb(0xFFE14A), MG_F_CENTER | MG_F_SHADOW);
        mg_text(g, fs, MG_W / 2, 356, u->name[u->banner_team], mg_rgb(0xFFFFFF), MG_F_CENTER | MG_F_SHADOW);
    } else if (u->banner == 2 || u->banner == 3) {
        const char *s = u->banner == 2 ? u->s_half : u->s_full;
        mg_rect(g, 0, 280, MG_W, 120, mg_rgb(0x0A0E16), 180, 0);
        mg_text(g, fs, MG_W / 2, 296, s, mg_rgb(0xFFFFFF), MG_F_CENTER | MG_F_SHADOW);
        char sc[80];
        snprintf(sc, sizeof sc, "%s  %d - %d  %s", u->name[0], gm->score[mg_side_of_team(a, 0)],
                 gm->score[mg_side_of_team(a, 1)], u->name[1]);
        mg_text(g, fs, MG_W / 2, 344, sc, mg_rgb(0xFFE14A), MG_F_CENTER | MG_F_SHADOW);
    }
    if (gm->state == MG_ST_REPLAY) {
        mg_rect(g, MG_W / 2 - 130, 76, 260, 42, mg_rgb(0x10141C), 210, 10);
        if (((int)(u->t * 2.0f)) & 1) mg_disc(g, MG_W / 2 - 104, 97, 8, 8, mg_rgb(0xFF3030), 255);
        mg_text(g, fs, MG_W / 2 + 12, 81, u->s_replay, mg_rgb(0xFFFFFF), MG_F_CENTER);
    }
    if (u->hint_t > 0 && gm->state != MG_ST_REPLAY) {
        int al = u->hint_t < 0.6f ? (int)(u->hint_t / 0.6f * 220) : 220;
        int w = mg_text_width(&art->f_small, u->s_hint) + 40;
        mg_rect(g, MG_W / 2 - w / 2, 664, w, 40, mg_rgb(0x10141C), al, 12);
        mg_text(g, &art->f_small, MG_W / 2, 672, u->s_hint, mg_rgb(0xFFFFFF), MG_F_CENTER);
    }
    /* the paper */
    if (u->nconf) {
        int live = 0;
        for (int i = 0; i < u->nconf; i++) {
            u->conf_x[i] += u->conf_vx[i] * dt;
            u->conf_y[i] += u->conf_vy[i] * dt;
            u->conf_vx[i] += sinf(u->t * 3.0f + (float)i) * 40.0f * dt;
            if (u->conf_y[i] > MG_H + 10) continue;
            live++;
            if (u->conf_y[i] > 0)
                mg_rect(g, iround(u->conf_x[i]), iround(u->conf_y[i]), 5 + (i & 1) * 2, 7 - (i & 1) * 2, u->conf_c[i], 255, 0);
        }
        if (!live) u->nconf = 0;
    }
}
