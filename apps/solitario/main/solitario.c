/*
 * P4OS - Solitario
 *
 * The solitaires of always, on a felt: Klondike drawing one or three,
 * Spider with one, two or four suits, FreeCell, Pyramid, TriPeaks, Golf,
 * Yukon, and Klondike with the Spanish deck of forty (Truco's drawing).
 *
 * This file is the app's life and its hands: create/resize/destroy, the
 * frame timer, the finger (drag a card or a run, tap it to send it to its
 * best place, tap twice to send it home), the gamepad, and the flow of a
 * hand - looking for a deal with a solution while the cards gather, the
 * hint, the finish that plays itself, the win and what it unlocks.
 *
 *   so_game.c  the rules of every game (no LVGL: the bench uses them)
 *   so_solve.c the solver, over those same rules
 *   so_art.c   the French cards, backs and felts, so_court.c the courts
 *   so_es.c    the Spanish deck
 *   so_view.c  layout and motion, so_win.c the endings, so_ui.c the panels,
 *   so_data.c  settings, statistics, achievements, the hand in progress
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_ui.h"

#include "so_app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRAG_START   12         /* px before a press becomes a drag        */
#define DOUBLE_MS    420
#define SEARCH_MS    1600       /* how long to look for a deal on the board */
#define HINT_BUDGET  30000

extern uint32_t so_won_new_bits;

static uint32_t s_seq;          /* mixes the seeds of hands dealt in a row */

/* --- small things ------------------------------------------------------------------------ */

void so_beep(so_app_t *a, int kind)
{
    if (!a->set.sound) {
        return;
    }
    switch (kind) {
    case 0: aos_hal_beep(2600, 6); break;           /* a card down           */
    case 1: aos_hal_beep(1568, 30); break;          /* home                  */
    case 2: aos_hal_beep(2093, 70); break;          /* a foundation complete */
    case 3: aos_hal_beep(900 + (int)(aos_hal_uptime_ms() % 900), 25); break;
    case 4:                                          /* won                   */
        aos_hal_beep(1047, 90);
        aos_hal_beep(1319, 90);
        aos_hal_beep(1568, 90);
        aos_hal_beep(2093, 220);
        break;
    default: aos_hal_beep(330, 60); break;           /* no                    */
    }
}

static uint32_t new_seed(void)
{
    uint32_t s = (uint32_t)aos_hal_uptime_ms() * 2654435761u ^ (++s_seq * 0x9E3779B9u);
    so_rand(&s);
    return s ? s : 1;
}

static so_opts_t opts_now(const so_app_t *a)
{
    so_opts_t o = { a->set.scoring, a->set.limited != 0 };
    return o;
}

/* --- the touch layer --------------------------------------------------------------------- */

static void touch_point(lv_event_t *e, int *x, int *y)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    so_app_t *a = (so_app_t *)lv_event_get_user_data(e);
    lv_area_t c;
    lv_obj_get_coords(a->root, &c);
    *x = p.x - c.x1;
    *y = p.y - c.y1;
}

static bool input_blocked(so_app_t *a)
{
    return a->panel || a->dealing || a->autoplay || a->closing;
}

static void win_skip(so_app_t *a);

static void drag_begin(so_app_t *a)
{
    so_game_t *g = &a->g;
    a->dragging = true;
    a->vx = 0;
    so_cv_t *c = &a->v.cv[g->pc[a->pick_pile][a->pick_idx]];
    a->grab_dx = a->press_x - (int)c->x;
    a->grab_dy = a->press_y - (int)c->y;
    for (int p = 0; p < g->npiles; p++) {
        a->legal_to[p] = so_drop_move(g, a->pick_pile, a->pick_idx, p, NULL);
    }
    so_view_lift(a, a->pick_pile, a->pick_idx, true);
    so_view_hint(a, NULL, false);
}

static void spring_back(so_app_t *a, int pile, int idx)
{
    so_game_t *g = &a->g;
    so_view_sync(a, g->nops, 0);
    for (int k = idx; k < g->pn[pile]; k++) {
        so_cv_t *c = &a->v.cv[g->pc[pile][k]];
        if (c->moving) {
            c->ease = SO_EASE_SPRING;
            c->dur = 460;
        }
    }
}

/* a card that cannot go anywhere shivers */
static void shake(so_app_t *a, int pile, int idx)
{
    so_game_t *g = &a->g;
    for (int k = idx; k >= 0 && k < g->pn[pile]; k++) {
        so_cv_t *c = &a->v.cv[g->pc[pile][k]];
        c->x0 = c->x + 14.f;
        c->y0 = c->y;
        c->x1 = c->x;
        c->y1 = c->y;
        c->t = 0;
        c->dur = 380;
        c->delay = 0;
        c->ease = SO_EASE_SPRING;
        c->moving = true;
    }
    a->v.busy = true;
    so_beep(a, 5);
}

static void tap(so_app_t *a, int pile, int idx)
{
    so_game_t *g = &a->g;
    so_move_t m;
    uint32_t now = lv_tick_get();
    if (pile == g->stock) {
        if (so_stock_move(g, &m)) {
            so_play(a, &m, m.kind == SO_MV_REDEAL ? 22 : (g->kind == SO_K_SPIDER ? 45 : 60));
        } else if (g->kind == SO_K_SPIDER && g->pn[g->stock]) {
            so_ui_status(a, _("Para repartir no puede haber columnas vacías."), 2500);
            so_beep(a, 5);
        } else {
            so_beep(a, 5);
        }
        return;
    }
    if (idx < 0) {
        return;
    }
    int card = g->pc[pile][idx];
    bool twice = card == a->tap_card && now - a->tap_ms < DOUBLE_MS;
    if (so_tap_move(g, pile, idx, twice, &m) || (twice && so_tap_move(g, pile, idx, false, &m))) {
        so_play(a, &m, 0);
        a->tap_card = card;
        a->tap_ms = now;
        return;
    }
    a->tap_card = card;
    a->tap_ms = now;
    if (so_can_pick(g, pile, idx)) {
        shake(a, pile, idx);
    }
}

static void touch_cb(lv_event_t *e)
{
    so_app_t *a = (so_app_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    so_game_t *g = &a->g;
    int x, y;
    touch_point(e, &x, &y);

    if (a->win.box) {
        if (code == LV_EVENT_PRESSED) {
            win_skip(a);
        }
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        if (input_blocked(a)) return;
        a->down = true;
        a->dragging = false;
        a->press_x = a->last_x = x;
        a->press_y = a->last_y = y;
        a->press_ms = lv_tick_get();
        int idx = -1;
        int p = so_view_hit(a, x, y, &idx);
        a->pick_pile = p;
        a->pick_idx = idx;
        if (a->pad_shown) {
            a->pad_shown = false;
            so_view_cursor(a, -1, 0, false);
        }
        return;
    }
    if (!a->down) {
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        int dx = x - a->press_x, dy = y - a->press_y;
        if (!a->dragging) {
            if (dx * dx + dy * dy < DRAG_START * DRAG_START) return;
            if (a->pick_pile < 0 || a->pick_idx < 0 || !so_can_pick(g, a->pick_pile, a->pick_idx) ||
                g->ptype[a->pick_pile] == SO_P_STOCK) {
                a->down = false;
                return;
            }
            drag_begin(a);
        }
        float v = (float)(x - a->last_x);
        a->vx = a->vx * 0.7f + v * 0.3f;
        a->last_x = x;
        a->last_y = y;
        float tilt = a->vx * 0.45f;
        if (tilt > 6.f) tilt = 6.f;
        if (tilt < -6.f) tilt = -6.f;
        so_view_drag(a, a->pick_pile, a->pick_idx, x, y, tilt);
        so_view_highlight(a, a->legal_to, so_view_drop_pile(a, x, y));
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        a->down = false;
        if (a->dragging) {
            a->dragging = false;
            so_view_highlight(a, NULL, -1);
            int to = so_view_drop_pile(a, x, y);
            so_view_drag(a, a->pick_pile, a->pick_idx, x, y, 0.f);
            so_view_lift(a, a->pick_pile, a->pick_idx, false);
            so_move_t m;
            if (to >= 0 && so_drop_move(g, a->pick_pile, a->pick_idx, to, &m)) {
                so_play(a, &m, 0);
            } else {
                spring_back(a, a->pick_pile, a->pick_idx);
            }
            return;
        }
        if (code == LV_EVENT_RELEASED && a->pick_pile >= 0) {
            tap(a, a->pick_pile, a->pick_idx);
        }
    }
}

/* --- the flow of a hand --------------------------------------------------------------------- */

void so_play(so_app_t *a, const so_move_t *m, int stagger)
{
    so_game_t *g = &a->g;
    uint32_t from = g->nops;
    so_apply(g, m);
    if (!a->started) {
        a->started = true;
        a->data.st[g->mode].played++;
    }
    so_view_sync(a, from, stagger);
    so_view_hint(a, NULL, false);
    a->hint_ms = 0;
    so_beep(a, 0);
    so_ui_refresh(a);
}

void so_do_undo(so_app_t *a)
{
    if (!a->set.undo || a->finished || input_blocked(a)) {
        return;
    }
    if (!so_undo(&a->g)) {
        return;
    }
    a->used_help = true;
    so_view_sync(a, a->g.nops, 0);
    so_view_hint(a, NULL, false);
    so_ui_refresh(a);
}

void so_do_hint(so_app_t *a)
{
    if (!a->set.hints || a->finished || input_blocked(a) || a->hinting) {
        return;
    }
    a->used_help = true;
    if (a->solver) {
        so_solver_start(a->solver, &a->g, HINT_BUDGET);
        a->hinting = true;
    } else {
        so_move_t m;
        if (so_hint_quick(&a->g, &m)) {
            a->hint_mv = m;
            a->hint_ms = 2400;
            so_view_hint(a, &m, true);
        }
    }
}

static void hint_step(so_app_t *a)
{
    uint64_t t0 = aos_hal_uptime_ms();
    so_solve_res_t r = SO_SOLVE_RUNNING;
    while (r == SO_SOLVE_RUNNING && aos_hal_uptime_ms() - t0 < 10) {
        r = so_solver_run(a->solver, 400);
    }
    if (r == SO_SOLVE_RUNNING) {
        return;
    }
    a->hinting = false;
    so_move_t m;
    const so_move_t *path;
    bool ok = false;
    if (r == SO_SOLVE_SOLVED && so_solver_path(a->solver, &path) > 0) {
        m = path[0];
        ok = true;
    } else {
        ok = so_hint_quick(&a->g, &m);
    }
    if (!ok) {
        so_ui_status(a, _("No hay jugadas."), 2000);
        return;
    }
    if (r == SO_SOLVE_FAILED && (m.kind == SO_MV_DRAW || m.kind == SO_MV_REDEAL)) {
        so_ui_status(a, _("Sólo queda el mazo."), 1800);
    }
    a->hint_mv = m;
    a->hint_ms = 2600;
    so_view_hint(a, &m, true);
}

/* the deal itself, once its seed is chosen */
static void deal(so_app_t *a, int mode, uint32_t seed)
{
    so_opts_t o = opts_now(a);
    int old_mode = a->g.mode, old_n = a->g.ncards;
    so_new(&a->g, mode, seed, &o);
    a->elapsed_ms = 0;
    a->started = false;
    a->finished = false;
    a->used_help = false;
    a->autoplay = 0;
    a->hint_ms = 0;
    a->tap_card = -1;
    a->pad_hold_pile = -1;
    a->pad_pile = -1;
    so_view_hint(a, NULL, false);
    if (old_mode != mode || old_n != a->g.ncards || !a->v.layer) {
        so_rebuild(a);
        so_view_reset(a, true);
    } else {
        so_view_reset(a, true);
    }
    so_ui_refresh(a);
    so_game_forget();
    aos_hal_log(SO_TAG, "dealt %s, seed %u", so_mode_name[mode], (unsigned)seed);
}

static void win_stop_restore(so_app_t *a)
{
    if (a->win.box) {
        so_win_stop(a);
        so_view_hide_cards(a, false);
    }
}

void so_new_hand(so_app_t *a, int mode, bool daily)
{
    win_stop_restore(a);
    so_ui_panel(a, SO_PANEL_NONE);
    so_data_lost(a);
    a->daily = daily;
    a->daily_day = daily ? so_today() : 0;
    if (!daily && mode != a->set.mode) {
        a->set.mode = (uint8_t)mode;
        so_set_save(&a->set);
    }
    if (daily) {
        deal(a, mode, so_daily_seed(a->daily_day, mode));
        return;
    }
    if (a->set.solvable && a->solver) {
        /* look for a deal with a solution while the old cards gather */
        a->dealing = true;
        a->deal_mode = mode;
        a->deal_seed = new_seed();
        a->deal_t = 0;
        a->deal_tries = 0;
        so_game_t probe;
        memset(&probe, 0, sizeof probe);
        so_opts_t o = opts_now(a);
        so_new(&probe, mode, a->deal_seed, &o);
        so_solver_start(a->solver, &probe, 60000);
        so_free(&probe);
        so_ui_status(a, _("Buscando una mano con solución..."), 30000);
        /* the cards go back to the stock meanwhile */
        so_view_t *v = &a->v;
        for (int i = 0; i < a->g.ncards; i++) {
            so_cv_t *c = &v->cv[i];
            c->x0 = c->x;
            c->y0 = c->y;
            c->x1 = (float)(v->W / 2 - v->cw / 2);
            c->y1 = (float)(v->play.y1 + 10);
            if (a->g.stock >= 0) {
                c->x1 = v->pl[a->g.stock].x;
                c->y1 = v->pl[a->g.stock].y;
            }
            c->t = 0;
            c->dur = 300;
            c->delay = (int16_t)((a->g.ncards - i) * 4);
            c->ease = SO_EASE_INOUT;
            c->moving = true;
            if (c->up) {
                c->flipping = true;
                c->flip_to = false;
                c->ft = 0;
                c->fdelay = c->delay;
            }
        }
        v->busy = true;
        return;
    }
    deal(a, mode, new_seed());
}

static void deal_step(so_app_t *a, int dt)
{
    a->deal_t += (uint32_t)dt;
    uint64_t t0 = aos_hal_uptime_ms();
    so_solve_res_t r = SO_SOLVE_RUNNING;
    while (r == SO_SOLVE_RUNNING && aos_hal_uptime_ms() - t0 < 12) {
        r = so_solver_run(a->solver, 500);
    }
    if (r == SO_SOLVE_SOLVED) {
        a->dealing = false;
        so_ui_status(a, "", 0);
        aos_hal_log(SO_TAG, "deal with a solution after %d tries, %u ms", a->deal_tries + 1,
                    (unsigned)a->deal_t);
        deal(a, a->deal_mode, a->deal_seed);
        return;
    }
    if (r == SO_SOLVE_RUNNING) {
        return;
    }
    a->deal_tries++;
    if (a->deal_t > SEARCH_MS || !so_seed_n[a->deal_mode]) {
        /* no time to look further: one the bench already solved */
        a->dealing = false;
        so_ui_status(a, "", 0);
        uint32_t n = so_seed_n[a->deal_mode];
        uint32_t seed = n ? so_seed_tab[a->deal_mode][new_seed() % n] : new_seed();
        aos_hal_log(SO_TAG, "deal from the table after %d tries", a->deal_tries);
        deal(a, a->deal_mode, seed);
        return;
    }
    a->deal_seed = new_seed();
    so_game_t probe;
    memset(&probe, 0, sizeof probe);
    so_opts_t o = opts_now(a);
    so_new(&probe, a->deal_mode, a->deal_seed, &o);
    so_solver_start(a->solver, &probe, 60000);
    so_free(&probe);
}

void so_restart_hand(so_app_t *a)
{
    win_stop_restore(a);
    so_data_lost(a);
    bool daily = a->daily;
    uint32_t day = a->daily_day;
    deal(a, a->g.mode, a->g.seed);
    a->daily = daily;
    a->daily_day = day;
    so_ui_refresh(a);
}

static void win_skip(so_app_t *a)
{
    win_stop_restore(a);
    so_ui_panel(a, SO_PANEL_WON);
}

/* after the cards of a move have landed: won, finishing, or stuck */
static void check_end(so_app_t *a)
{
    so_game_t *g = &a->g;
    if (a->finished) {
        return;
    }
    if (so_won(g)) {
        a->autoplay = 0;
        so_won_new_bits = so_data_won(a);
        so_beep(a, 4);
        so_ui_refresh(a);
        so_win_start(a);
        return;
    }
    if (!a->autoplay && a->set.autocomplete && so_can_autocomplete(g)) {
        a->autoplay = 1;
        a->auto_wait = 200;
        return;
    }
    if (!a->autoplay && so_stuck(g)) {
        so_ui_panel(a, SO_PANEL_STUCK);
    }
}

/* --- the gamepad ----------------------------------------------------------------------------- */

/* the point a pile shows to the cursor */
static void pile_point(so_app_t *a, int p, int idx, float *x, float *y)
{
    so_game_t *g = &a->g;
    if (g->pn[p]) {
        if (idx < 0 || idx >= g->pn[p]) idx = g->pn[p] - 1;
        so_view_card_pos(a, p, idx, x, y);
    } else {
        *x = a->v.pl[p].x;
        *y = a->v.pl[p].y;
    }
    *x += (float)a->v.cw * 0.5f;
    *y += (float)a->v.ch * 0.3f;
}

static bool pad_usable(so_app_t *a, int p)
{
    so_game_t *g = &a->g;
    if (a->v.pl[p].pad_skip || a->v.pl[p].off) return false;
    if (g->ptype[p] == SO_P_SLOT) return g->pn[p] > 0;
    if (g->ptype[p] == SO_P_FOUND && g->kind == SO_K_SPIDER) return false;
    if (g->ptype[p] == SO_P_WASTE && !g->pn[p]) return false;
    return true;
}

static int pad_lowest(so_app_t *a, int p)
{
    so_game_t *g = &a->g;
    int i = g->pn[p] - 1;
    while (i > 0 && so_can_pick(g, p, i - 1)) i--;
    return i;
}

/* the nearest pile from the cursor in a direction */
static int pad_next(so_app_t *a, int dx, int dy)
{
    so_game_t *g = &a->g;
    float cx, cy;
    pile_point(a, a->pad_pile, a->pad_idx, &cx, &cy);
    int best = -1;
    float bestd = 1e9f;
    for (int p = 0; p < g->npiles; p++) {
        if (p == a->pad_pile || !pad_usable(a, p)) continue;
        float x, y;
        pile_point(a, p, -1, &x, &y);
        if (g->ptype[p] == SO_P_TABLEAU) {
            y = a->v.pl[p].y + (float)a->v.ch * 0.3f;
        }
        float ex = x - cx, ey = y - cy;
        float along = ex * (float)dx + ey * (float)dy;
        if (along <= 4.f) continue;
        float across = fabsf(ex * (float)dy - ey * (float)dx);
        float d = along + across * 2.5f;
        if (d < bestd) {
            bestd = d;
            best = p;
        }
    }
    return best;
}

static void pad_show(so_app_t *a)
{
    so_game_t *g = &a->g;
    if (a->pad_pile < 0) return;
    int idx = a->pad_idx;
    if (g->pn[a->pad_pile] == 0) idx = -1;
    so_view_cursor(a, a->pad_pile, idx, a->pad_hold_pile >= 0);
}

static void pad_game(so_app_t *a)
{
    so_game_t *g = &a->g;
    aos_pad_t *pd = &a->pad;
    if (!pd->pressed && !pd->repeat) return;
    if (aos_pad_pressed(pd, AOS_PAD_START)) {
        so_ui_panel(a, SO_PANEL_MENU);
        return;
    }
    if (aos_pad_pressed(pd, AOS_PAD_L)) {
        so_do_undo(a);
        return;
    }
    if (aos_pad_pressed(pd, AOS_PAD_R)) {
        so_do_hint(a);
        return;
    }
    if (input_blocked(a)) return;
    if (!a->pad_shown || a->pad_pile < 0 || a->pad_pile >= g->npiles) {
        /* the first press only shows where the cursor is */
        a->pad_shown = true;
        a->pad_pile = g->ntab ? g->tab0 : (g->nslot ? g->slot0 + g->nslot - 1 : 0);
        for (int p = 0; p < g->npiles && !pad_usable(a, a->pad_pile); p++) a->pad_pile = p;
        a->pad_idx = g->pn[a->pad_pile] ? g->pn[a->pad_pile] - 1 : 0;
        pad_show(a);
        return;
    }
    int p = a->pad_pile;
    bool col = g->ptype[p] == SO_P_TABLEAU && g->kind != SO_K_GOLF;
    if (aos_pad_repeat(pd, AOS_PAD_UP)) {
        if (col && a->pad_hold_pile < 0 && a->pad_idx > 0 && a->pad_idx > pad_lowest(a, p)) {
            a->pad_idx--;
        } else {
            int q = pad_next(a, 0, -1);
            if (q >= 0) {
                a->pad_pile = q;
                a->pad_idx = g->pn[q] ? g->pn[q] - 1 : 0;
            }
        }
    } else if (aos_pad_repeat(pd, AOS_PAD_DOWN)) {
        if (col && a->pad_idx < g->pn[p] - 1) {
            a->pad_idx++;
        } else {
            int q = pad_next(a, 0, 1);
            if (q >= 0) {
                a->pad_pile = q;
                a->pad_idx = g->pn[q] ? g->pn[q] - 1 : 0;
            }
        }
    } else if (aos_pad_repeat(pd, AOS_PAD_LEFT) || aos_pad_repeat(pd, AOS_PAD_RIGHT)) {
        int q = pad_next(a, aos_pad_repeat(pd, AOS_PAD_LEFT) ? -1 : 1, 0);
        if (q >= 0) {
            a->pad_pile = q;
            a->pad_idx = g->pn[q] ? g->pn[q] - 1 : 0;
            if (g->ptype[q] == SO_P_TABLEAU && g->pn[q] && a->pad_hold_pile < 0) {
                a->pad_idx = g->pn[q] - 1;
            }
        }
    } else if (aos_pad_pressed(pd, AOS_PAD_B)) {
        a->pad_hold_pile = -1;
        so_view_hint(a, NULL, false);
        so_view_highlight(a, NULL, -1);
    } else if (aos_pad_pressed(pd, AOS_PAD_A)) {
        so_move_t m;
        if (a->pad_hold_pile >= 0) {
            int hp = a->pad_hold_pile, hi = a->pad_hold_idx;
            a->pad_hold_pile = -1;
            so_view_highlight(a, NULL, -1);
            so_view_hint(a, NULL, false);
            if (hp == p) {
                tap(a, hp, hi);
            } else if (so_drop_move(g, hp, hi, p, &m)) {
                so_play(a, &m, 0);
            } else {
                shake(a, hp, hi);
            }
        } else if (p == g->stock) {
            tap(a, p, -1);
        } else if (g->pn[p] && so_can_pick(g, p, a->pad_idx)) {
            bool lone = g->kind == SO_K_TRIPEAKS || g->kind == SO_K_GOLF ||
                        (g->kind == SO_K_PYRAMID && g->rank[g->pc[p][a->pad_idx]] == 12);
            if (lone) {
                tap(a, p, a->pad_idx);
            } else {
                a->pad_hold_pile = p;
                a->pad_hold_idx = a->pad_idx;
                so_move_t show = { SO_MV_MOVE, (uint8_t)p, (uint8_t)p, (uint8_t)(g->pn[p] - a->pad_idx) };
                so_view_hint(a, &show, true);
                for (int q = 0; q < g->npiles; q++) a->legal_to[q] = so_drop_move(g, p, a->pad_idx, q, NULL);
                so_view_highlight(a, a->legal_to, -1);
            }
        } else {
            so_beep(a, 5);
        }
    }
    if (a->pad_pile >= 0 && a->pad_pile < g->npiles) {
        int n = g->pn[a->pad_pile];
        if (a->pad_idx >= n) a->pad_idx = n ? n - 1 : 0;
    }
    pad_show(a);
}

/* --- the frame ------------------------------------------------------------------------------- */

static void frame(lv_timer_t *timer)
{
    so_app_t *a = (so_app_t *)lv_timer_get_user_data(timer);
    if (a->closing) {
        return;
    }
    if (a->want_exit) {
        a->want_exit = false;
        a->leaving = true;
        aos_ui_back();              /* destroys the app: 'a' is gone after this */
        return;
    }
    uint64_t now = aos_hal_uptime_ms();
    int dt = (int)(uint32_t)(now - a->prev_ms);
    a->prev_ms = now;
    if (dt < 0 || dt > 100) {
        dt = SO_FRAME_MS;
    }

    aos_pad_update(&a->pad, lv_tick_get());
    if (a->win.box) {
        if (a->pad.pressed) {
            win_skip(a);
        } else if (!so_win_frame(a, dt)) {
            win_skip(a);
        }
    } else if (a->panel) {
        so_ui_pad(a);
    } else {
        pad_game(a);
    }
    if (a->want_exit) {
        return;
    }

    so_view_frame(a, dt);

    if (a->dealing && a->solver) {
        deal_step(a, dt);
    }
    if (a->hinting && a->solver) {
        hint_step(a);
    }
    if (a->hint_ms > 0) {
        a->hint_ms = (int16_t)(a->hint_ms - dt);
        if (a->hint_ms <= 0) so_view_hint(a, NULL, false);
    }
    if (a->autoplay && !a->dealing) {
        a->auto_wait -= dt;
        if (a->auto_wait <= 0) {
            so_move_t m;
            if (so_auto_step(&a->g, &m)) {
                so_play(a, &m, 0);
                a->auto_wait = 95;
            } else {
                a->autoplay = 0;
            }
        }
    }
    if (a->started && !a->finished && !a->panel && !a->win.box) {
        uint32_t before = a->elapsed_ms / 1000;
        a->elapsed_ms += (uint32_t)dt;
        if (a->elapsed_ms / 1000 != before) so_ui_refresh(a);
    }
    so_ui_frame(a, dt);

    if (!a->v.busy && !a->dragging && !a->dealing && !a->win.box && !a->panel &&
        a->last_ops == a->g.nops && !a->finished) {
        static uint32_t checked_ops = 0xFFFFFFFF;
        static const so_app_t *checked_app;
        if (checked_ops != a->g.nops || checked_app != a || a->autoplay) {
            checked_ops = a->g.nops;
            checked_app = a;
            check_end(a);
        }
    }
}

/* --- building --------------------------------------------------------------------------------- */

static void touch_layer(so_app_t *a)
{
    lv_obj_t *t = lv_obj_create(a->root);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, a->v.W, a->v.H);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(t, touch_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(t, touch_cb, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(t, touch_cb, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(t, touch_cb, LV_EVENT_PRESS_LOST, a);
}

static bool build_all(so_app_t *a)
{
    lv_obj_t *root = a->root;
    lv_obj_update_layout(root);
    int w = (int)lv_obj_get_width(root), h = (int)lv_obj_get_height(root);
    if (w <= 0 || h <= 0) {
        int32_t aw, ah;
        aos_ui_app_area(&aw, &ah);
        w = (int)aw;
        h = (int)ah;
    }
    if (a->v.W != w || a->v.H != h) {
        free(a->v.feltmem);
        free(a->v.feltclean);
        a->v.feltmem = a->v.feltclean = NULL;
    }
    a->v.W = w;
    a->v.H = h;
    lv_obj_set_style_bg_color(root, lv_color_hex(so_felt_tone(a->set.felt)), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    if (!so_view_build(a)) {
        return false;
    }
    touch_layer(a);
    so_ui_build(a);
    return true;
}

/* The game or the size changed: everything is made again around the same
 * hand (the felt is kept if the size is the same). */
void so_rebuild(so_app_t *a)
{
    so_panel_t open = a->panel_kind;
    bool closing = a->closing;
    a->closing = true;
    win_stop_restore(a);
    if (a->panel) {
        so_ui_panel(a, SO_PANEL_NONE);
    }
    lv_obj_clean(a->root);
    a->panel = NULL;
    a->lbl_time = NULL;
    a->v.layer = NULL;
    for (int i = 0; i < SO_MAXCARDS; i++) a->v.card[i] = NULL;
    for (int i = 0; i < SO_MAXPILES; i++) a->v.slots[i] = NULL;
    a->closing = closing;
    build_all(a);
    so_view_reset(a, false);
    if (open != SO_PANEL_NONE && open != SO_PANEL_WON) {
        so_ui_panel(a, open);
    }
    a->pad_hold_pile = -1;
    aos_pad_menu_clear(&a->pmenu);
}

void so_exit(so_app_t *a)
{
    a->want_exit = true;
}

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    so_app_t *a = (so_app_t *)inst;
    if (!a || a->leaving) {
        return false;
    }
    if (a->win.box) {
        win_skip(a);
        return true;
    }
    if (a->panel) {
        if (a->panel_kind == SO_PANEL_MENU) {
            return false;                   /* back from the menu leaves */
        }
        so_ui_panel(a, a->panel_kind == SO_PANEL_WON || a->panel_kind == SO_PANEL_STUCK ? SO_PANEL_NONE
                                                                                      : SO_PANEL_MENU);
        return true;
    }
    so_ui_panel(a, SO_PANEL_MENU);
    return true;
}

static void *solitario_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    so_app_t *a = (so_app_t *)malloc(sizeof(so_app_t));
    if (!a) {
        return NULL;
    }
    memset(a, 0, sizeof *a);
    uint32_t heap_int = 0, heap_psram = 0;
    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log(SO_TAG, "opening | internal %u B, psram %u B", (unsigned)heap_int, (unsigned)heap_psram);
    uint64_t t0 = aos_hal_uptime_ms();

    a->root = root;
    a->prev_ms = t0;
    a->tap_card = -1;
    a->pad_pile = -1;
    a->pad_hold_pile = -1;
    a->pick_pile = -1;
    so_set_load(&a->set);
    so_data_load(a);
    a->solver = so_solver_new(17, 600);

    bool resumed = so_game_load(a);
    if (!resumed) {
        so_opts_t o = opts_now(a);
        uint32_t seed = so_seed_n[a->set.mode] ? so_seed_tab[a->set.mode][new_seed() % so_seed_n[a->set.mode]]
                                               : new_seed();
        so_new(&a->g, a->set.mode, seed, &o);
    } else {
        a->set.mode = a->g.mode;
    }
    if (!build_all(a)) {
        so_view_free(a);
        so_solver_free(a->solver);
        so_free(&a->g);
        free(a);
        return NULL;
    }
    so_view_reset(a, !resumed);
    so_ui_refresh(a);
    aos_pad_reset(&a->pad, lv_tick_get());
    a->timer = lv_timer_create(frame, SO_FRAME_MS, a);

#ifdef AOS_SIM_BUILTIN
    /* Development switches. On the board getenv() always returns NULL.
     *
     *   SO_MODE=3         a new hand of that game (0..10, so_mode_t)
     *   SO_SEED=42        with this deal
     *   SO_PLAY=40        play that many moves of the solution (or hints)
     *   SO_WIN=1          finish the hand by itself and win (the ending)
     *   SO_WINANIM=2      which ending: 0 bounce, 1 fireworks, 2 spiral
     *   SO_PANEL=menu     open a panel: menu games options looks stats ach help
     *   SO_FELT, SO_BACK, SO_DECK   the looks
     *   SO_ALL=1          every achievement, for the looks
     */
    {
        const char *env;
        bool redeal = false;
        int mode = a->g.mode;
        uint32_t seed = a->g.seed;
        if ((env = getenv("SO_ALL")) && env[0]) a->data.ach = (1u << SO_NACH) - 1;
        if ((env = getenv("SO_FELT")) && env[0]) a->set.felt = (uint8_t)atoi(env);
        if ((env = getenv("SO_BACK")) && env[0]) a->set.back = (uint8_t)atoi(env);
        if ((env = getenv("SO_DECK")) && env[0]) a->set.deck = (uint8_t)atoi(env);
        if ((env = getenv("SO_WINANIM")) && env[0]) a->set.win_anim = (uint8_t)atoi(env);
        if ((env = getenv("SO_MODE")) && env[0]) {
            mode = atoi(env);
            redeal = true;
            a->set.mode = (uint8_t)mode;
        }
        if ((env = getenv("SO_SEED")) && env[0]) {
            seed = (uint32_t)strtoul(env, NULL, 0);
            redeal = true;
        }
        so_view_set_looks(a);
        if (redeal) {
            if (!getenv("SO_SEED") && so_seed_n[mode]) seed = so_seed_tab[mode][0];
            deal(a, mode, seed);
            so_view_reset(a, false);
        }
        int play = (env = getenv("SO_PLAY")) && env[0] ? atoi(env) : 0;
        bool win = (env = getenv("SO_WIN")) && env[0];
        if ((play || win) && a->solver) {
            so_solve_res_t r = so_solve(a->solver, &a->g, 2000000);
            const so_move_t *path;
            int n = r == SO_SOLVE_SOLVED ? so_solver_path(a->solver, &path) : 0;
            if (win) play = n;
            if (!win && play > n - 3) play = n > 3 ? n - 3 : 0;
            for (int i = 0; i < play && i < n; i++) {
                so_apply(&a->g, &path[i]);
            }
            a->started = play > 0;
            if (a->started) a->data.st[a->g.mode].played++;
            a->elapsed_ms = (uint32_t)play * 4100u;
            so_view_reset(a, false);
            so_ui_refresh(a);
            if (win) {
                /* the last move again, so the win plays out as it would */
                so_undo(&a->g);
                so_view_reset(a, false);
                so_play(a, &path[n - 1], 0);
            }
        }
        if ((env = getenv("SO_PANEL")) && env[0]) {
            static const char *const P[] = { "menu", "games", "options", "looks", "stats", "ach", "help" };
            static const so_panel_t K[] = { SO_PANEL_MENU, SO_PANEL_GAMES, SO_PANEL_OPTIONS, SO_PANEL_LOOKS,
                                            SO_PANEL_STATS, SO_PANEL_ACH, SO_PANEL_HELP };
            for (int i = 0; i < 7; i++) {
                if (!strcmp(env, P[i])) so_ui_panel(a, K[i]);
            }
        }
    }
#endif

    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log(SO_TAG, "ready in %u ms | %s | internal %u B, psram %u B",
                (unsigned)(aos_hal_uptime_ms() - t0), so_mode_name[a->g.mode], (unsigned)heap_int,
                (unsigned)heap_psram);
    return a;
}

static bool solitario_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    so_app_t *a = (so_app_t *)inst;
    if (!a) {
        return false;
    }
    a->root = root;
    a->down = a->dragging = false;
    so_rebuild(a);
    a->prev_ms = aos_hal_uptime_ms();
    return true;
}

static void solitario_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    so_app_t *a = (so_app_t *)inst;
    if (!a) {
        return;
    }
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* the objects go HERE, before the memory they point at */
    a->closing = true;
    so_win_stop(a);
    so_ui_panel(a, SO_PANEL_NONE);
    if (a->root) {
        lv_obj_clean(a->root);
    }
    if (!a->dealing) {
        so_game_save(a);
    }
    so_data_save(a);
    so_set_save(&a->set);
    aos_hal_log(SO_TAG, "closing | %s, %lu moves", so_mode_name[a->g.mode], (unsigned long)a->g.moves);
    so_view_free(a);
    so_solver_free(a->solver);
    so_free(&a->g);
    free(a);
}

/* The launcher icon: a column of cards fanned down on the felt, the last
 * an ace of hearts. Percent coordinates. */
static const uint8_t SOLITARIO_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -16, -20, 34, 46, 5, AIC_C_LIT(0xB0283A), 255),
    AIC_BORDER(2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -4, -6, 34, 46, 5, AIC_C_LIT(0x2B4180), 255),
    AIC_BORDER(2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 8, 10, 36, 48, 5, AIC_C_TEXT, 255),
    AIC_INTO,
    /* the heart: a square on its corner and two lobes */
    AIC_RECT(AIC_CENTER, 0, 3, 14, 14, 1, AIC_C_LIT(0xD0142C), 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, -5, -3, 12, 12, AIC_CIRCLE, AIC_C_LIT(0xD0142C), 255),
    AIC_RECT(AIC_CENTER, 5, -3, 12, 12, AIC_CIRCLE, AIC_C_LIT(0xD0142C), 255),
    AIC_OUT,
    AIC_END
};

static bool solitario_init(aos_app_t *app)
{
    app->desc.id       = "demo.solitario";
    app->desc.name     = "Solitario";
    app->desc.icon     = LV_SYMBOL_SHUFFLE;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, SOLITARIO_ICON, sizeof SOLITARIO_ICON);
    app->desc.color_a  = 0x23784A;
    app->desc.color_b  = 0x0C3A22;
    app->desc.order    = 148;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN;

    app->create  = solitario_create;
    app->destroy = solitario_destroy;
    app->back    = app_back;
    app->resize  = solitario_resize;
    return true;
}

AOS_APP_ENTRY(solitario_init);
