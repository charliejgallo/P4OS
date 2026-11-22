/*
 * Juegos de mesa - a game being played.
 *
 * The game is its options, its seed and the list of what was done (moves
 * and rolls, hist[0..n)). The position is always init() and that list
 * played again, so undo is the list one shorter played again and redo the
 * next entry of what was undone (hist[n..top)); a new move drops the
 * entries that redo still had.
 *
 * The board thinks on a thread of its own, on a copy of the position, and
 * leaves its move in ai_move; LVGL's timer plays it once the pieces have
 * landed and a short pause has passed, so a fast answer does not look like
 * a reflex. Undo, a new game or leaving stop it (ai.stop) and wait for it.
 *
 * Who is at the bottom: a person against the board sits at the bottom
 * whatever side they play; two people face to face keep the first player
 * at the bottom; passing the board turns it to whoever moves.
 */
#include "jm_app.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P (&jm->play)
#define AI_STACK (128 * 1024)

static void begin_turn(void);

bool jm_play_person_to_move(void)
{
    int t = P->g && P->st ? P->g->turn(P->st) : -1;
    return t >= 0 && P->human[t] && !P->over;
}

int jm_play_turn(void)
{
    return P->g && P->st && !P->over ? P->g->turn(P->st) : -1;
}

/* ---- the board thinking --------------------------------------------------------------- */

static uint32_t ai_now(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static void ai_fn(void *arg)
{
    jm_play_t *p = (jm_play_t *)arg;
    int t = p->g->turn(p->ai_st);
    int level = t >= 0 ? p->level[t] : 1;
    if (p->hint_busy) level = p->g->levels > 4 ? 5 : p->g->levels;
    p->ai_ok = p->g->ai_move(p->ai_st, level, &p->ai, &p->ai_move);
    p->ai_done = true;
    p->ai_busy = false;
}

static bool ai_start(bool hint)
{
    if (!P->g->ai_move || P->ai_busy) return false;
    memcpy(P->ai_st, P->st, P->g->state_size);
    memset(&P->ai, 0, sizeof P->ai);
    P->ai.now_ms = ai_now;
    P->ai.seed = jm_rand();
    /* with a clock, a part of what is left */
    int t = P->g->turn(P->st);
    if (P->clock.mode != CLK_NONE && t >= 0 && t < 2) {
        int32_t left = P->clock.left[t];
        int32_t budget = left / 30 + (P->clock.mode == CLK_FISCHER ? P->clock.inc_ms * 3 / 4 : 0);
        if (P->clock.mode == CLK_MOVE) budget = P->clock.base_ms * 2 / 3;
        if (budget < 100) budget = 100;
        P->ai.ms = (uint32_t)budget;
    }
    P->hint_busy = hint;
    P->ai_done = false;
    P->ai_ok = false;
    P->ai_busy = true;
    P->ai_t0 = jm_now();
    P->ai_thread = true;
    if (!aos_hal_thread_start("mesa_ai", ai_fn, P, AI_STACK, 2)) {
        /* no thread: think here, briefly */
        P->ai.ms = 300;
        ai_fn(P);
    }
    return true;
}

void jm_play_ai_cancel(void)
{
    if (!P->ai_thread) return;
    P->ai.stop = true;
    for (int i = 0; i < 400 && P->ai_busy; i++) aos_hal_sleep_ms(10);
    if (P->ai_busy) aos_hal_log("mesa", "the board's thought did not come back");
    P->ai_thread = P->ai_busy;
    P->ai_done = false;
    P->hint_busy = false;
}

/* ---- the clock ------------------------------------------------------------------------- */

static void clock_run(void)
{
    jm_clock_t *c = &P->clock;
    if (c->mode == CLK_NONE) return;
    int t = jm_play_turn();
    if (t != c->running) {
        c->running = t >= 0 && t < 2 ? t : -1;
        c->spent = 0;
        c->last = jm_now();
    }
}

static void clock_count(void)
{
    jm_clock_t *c = &P->clock;
    if (c->mode == CLK_NONE || c->running < 0 || P->over) return;
    uint32_t now = jm_now();
    int32_t d = (int32_t)(now - c->last);
    c->last = now;
    if (d <= 0) return;
    /* nothing counts while a panel is over the game */
    if (jm->panel && jm->panel_kind != PANEL_CHOICE) return;
    c->left[c->running] -= d;
    c->spent += d;
    if (c->left[c->running] <= 0) {
        c->left[c->running] = 0;
        c->flagged = c->running;
        P->over = true;
        jm_play_ai_cancel();
        jm_audio_play(SND_LOSE);
        jm_hud_refresh();
        jm_hud_end();
        jm_save_game();
    }
}

static void clock_moved(int who)
{
    jm_clock_t *c = &P->clock;
    if (c->mode == CLK_NONE || who < 0 || who > 1) return;
    if (c->mode == CLK_FISCHER) c->left[who] += c->inc_ms;
    else if (c->mode == CLK_BRONSTEIN) c->left[who] += c->spent < c->inc_ms ? c->spent : c->inc_ms;
    else if (c->mode == CLK_MOVE) c->left[who] = c->base_ms;
    c->spent = 0;
}

/* ---- the list --------------------------------------------------------------------------- */

static void fresh_position(void)
{
    P->g->init(P->st, P->opts, P->seed);
    if (P->fen[0] && P->g->setup) P->g->setup(P->st, P->opts, P->fen);
}

void jm_play_replay(int n)
{
    fresh_position();
    for (int i = 0; i < n; i++) {
        const jm_hist_t *h = &P->hist[i];
        if (h->roll) P->g->roll(P->st, h->rnd);
        else P->g->apply(P->st, &h->m);
    }
    P->n = n;
    if (P->clock.mode != CLK_NONE) {
        if (n > 0) {
            P->clock.left[0] = P->hist[n - 1].clock[0];
            P->clock.left[1] = P->hist[n - 1].clock[1];
        } else {
            P->clock.left[0] = P->clock.left[1] = P->clock.base_ms;
        }
        P->clock.flagged = -1;
        P->clock.running = -1;
    }
    P->over = P->g->turn(P->st) < 0;
    P->resigned = -1;
}

static void hist_push(const jm_hist_t *h)
{
    if (P->n >= JM_HIST_MAX) return;
    P->hist[P->n] = *h;
    P->hist[P->n].clock[0] = P->clock.left[0];
    P->hist[P->n].clock[1] = P->clock.left[1];
    P->n++;
    P->top = P->n;
}

static void after_change(bool animate)
{
    P->sel = 0;
    P->nmoves = 0;
    jm_view_sync(animate);
    jm_hud_refresh();
    jm_hud_sheet_refresh();
}

static int bottom_for_turn(void)
{
    int t = jm_play_turn();
    int people = 0, first = -1;
    for (int i = 0; i < P->np; i++) {
        if (P->human[i]) {
            people++;
            if (first < 0) first = i;
        }
    }
    if (P->np > 2) return 0;
    if (people == 1) return first;
    if (people == 2 && P->two == TWO_PASS && t >= 0 && (P->g->flags & MESA_GF_ROTATE)) return t;
    return 0;
}

static void apply(const mesa_move_t *m)
{
    int who = P->g->turn(P->st);
    jm_hist_t h;
    memset(&h, 0, sizeof h);
    h.m = *m;
    uint32_t ev = P->g->apply(P->st, m);
    clock_moved(who);
    hist_push(&h);
    if (ev & MESA_EV_CAPTURE) jm_audio_play(SND_CAPTURE);
    if (ev & MESA_EV_CHECK) jm_audio_play(SND_CHECK);
    if (ev & MESA_EV_FLIP) jm_audio_play(SND_FLIP);
    after_change(true);
    P->over = P->g->turn(P->st) < 0;
    jm_save_game();
    if (P->over) {
        jm_play_ai_cancel();
    }
}

static void roll_now(void)
{
    jm_hist_t h;
    memset(&h, 0, sizeof h);
    h.roll = 1;
    h.rnd = jm_rand();
    P->g->roll(P->st, h.rnd);
    hist_push(&h);
    P->rolling = false;
    after_change(true);
    jm_save_game();
}

void jm_play_roll(void)
{
    if (P->over || P->rolling || !P->g->need_roll || P->g->need_roll(P->st) <= 0) return;
    P->rolling = true;
    P->roll_t0 = jm_now();
    for (int i = 0; i < 4; i++) P->roll_shown[i] = 1 + (int)(jm_rand() % 6);
    jm_audio_play(SND_DICE);
    jm_hud_refresh();
}

/* ---- whose turn it is ---------------------------------------------------------------------- */

static bool s_pass_wait;
static uint32_t s_next_check;

static void begin_turn(void)
{
    P->sel = 0;
    P->nmoves = 0;
    s_pass_wait = false;
    if (P->over || P->g->turn(P->st) < 0) {
        P->over = true;
        if (!P->saved_result) {
            P->saved_result = true;
            const char *why;
            int r = P->g->result(P->st, &why);
            int people = 0;
            for (int i = 0; i < P->np; i++) people += P->human[i] ? 1 : 0;
            if (people == 1 && r >= 0) jm_audio_play(P->human[r] ? SND_WIN : SND_LOSE);
            else jm_audio_play(SND_WIN);
            jm_hud_end();
        }
        return;
    }
    int b = bottom_for_turn();
    if (b != P->bottom) {
        P->bottom = b;
        jm_view_layout();
        jm_hud_build();
    }
    clock_run();
    int t = P->g->turn(P->st);
    if (P->g->need_roll && P->g->need_roll(P->st) > 0) {
        /* a person rolls with the button or a tap; the board by itself */
        if (!P->human[t]) P->wait_until = jm_now() + 450;
        jm_hud_refresh();
        return;
    }
    P->nmoves = P->g->moves(P->st, P->moves, MESA_MAX_MOVES);
    if (P->nmoves == 1 && P->moves[0].n == 0) {
        /* nothing to do but pass */
        s_pass_wait = true;
        P->wait_until = jm_now() + 1300;
        jm_hud_status(_("No hay jugada: pasa el turno"), 1300);
        return;
    }
    if (!P->human[t]) {
        ai_start(false);
        P->wait_until = jm_now() + 350;
    }
    jm_hud_refresh();
}

void jm_play_relayout(void)
{
    jm_art_load(&jm->art, P->g, P->g->board(P->opts, &(float){ 0 }, &(float){ 0 }), jm->prefs.set, jm->prefs.flat,
                jm->portrait);
    jm_view_layout();
    jm_hud_build();
}

/* ---- starting and stopping ------------------------------------------------------------------ */

bool jm_play_start(const jm_new_t *n)
{
    jm_play_stop(false);
    if (n->gi < 0 || n->gi >= jm_ngames) return false;
    const mesa_game_t *g = jm_games[n->gi].g;
    jm_play_t *p = P;
    memset(p, 0, sizeof *p);
    p->g = g;
    p->gi = n->gi;
    memcpy(p->opts, n->opts, sizeof p->opts);
    p->seed = jm_rand();
    p->np = g->players(p->opts);
    if (p->np < 1) p->np = 1;
    if (p->np > MESA_MAX_PLAYERS) p->np = MESA_MAX_PLAYERS;
    memcpy(p->human, n->human, sizeof p->human);
    memcpy(p->level, n->level, sizeof p->level);
    p->two = n->two;
    p->st = jm_calloc(g->state_size);
    p->tmp = jm_calloc(g->state_size);
    p->ai_st = jm_calloc(g->state_size);
    p->hist = (jm_hist_t *)jm_calloc(sizeof(jm_hist_t) * JM_HIST_MAX);
    p->moves = (mesa_move_t *)jm_calloc(sizeof(mesa_move_t) * MESA_MAX_MOVES);
    if (!p->st || !p->tmp || !p->ai_st || !p->hist || !p->moves) {
        jm_play_stop(false);
        aos_ui_toast(_("No hay memoria"), 2000);
        return false;
    }
    if (n->fen && n->fen[0]) snprintf(p->fen, sizeof p->fen, "%s", n->fen);
    p->clock.mode = (g->flags & MESA_GF_CLOCK) && p->np == 2 ? n->clock_mode : CLK_NONE;
    p->clock.base_ms = n->clock_base;
    p->clock.inc_ms = n->clock_inc;
    p->clock.left[0] = p->clock.left[1] = n->clock_base;
    p->clock.running = -1;
    p->clock.flagged = -1;
    p->resigned = -1;
    fresh_position();
    if (p->fen[0] && !(g->setup && g->setup(p->st, p->opts, p->fen))) p->fen[0] = 0;
    p->bottom = bottom_for_turn();
    jm->playing = true;
    jm->screen = SCR_PLAY;
    jm_play_relayout();
    begin_turn();
    return true;
}

void jm_play_stop(bool keep)
{
    jm_play_t *p = P;
    jm_play_ai_cancel();
    if (keep && p->g && p->st) jm_save_game();
    jm_hud_free();
    jm_free(p->st);
    jm_free(p->tmp);
    jm_free(p->ai_st);
    jm_free(p->hist);
    jm_free(p->moves);
    bool thread = p->ai_thread && p->ai_busy;
    memset(p, 0, sizeof *p);
    p->ai_thread = thread;
    jm->playing = false;
}

/* ---- every frame ------------------------------------------------------------------------------- */

void jm_play_tick(void)
{
    if (!jm->playing || !P->g) return;
    uint32_t now = jm_now();
    clock_count();
    if (P->over) return;
    bool busy = jm_view_busy() || (int32_t)(now - jm->view.anim_until) < 0;
    if (P->rolling) {
        if (now - P->roll_t0 > 650) {
            roll_now();
            begin_turn();
        } else if ((now / 70) % 2 == 0) {
            for (int i = 0; i < 4; i++) P->roll_shown[i] = 1 + (int)(jm_rand() % 6);
        }
        return;
    }
    if (busy || (int32_t)(now - P->wait_until) < 0) return;
    if (jm->panel) return;
    int t = P->g->turn(P->st);
    if (t < 0) {
        begin_turn();
        return;
    }
    if (s_pass_wait && P->nmoves == 1) {
        s_pass_wait = false;
        mesa_move_t m = P->moves[0];
        apply(&m);
        begin_turn();
        return;
    }
    if (!P->human[t]) {
        if (P->g->need_roll && P->g->need_roll(P->st) > 0) {
            jm_play_roll();
            return;
        }
        if (P->ai_done && !P->ai_busy && !P->hint_busy) {
            P->ai_done = false;
            P->ai_thread = false;
            if (P->ai_ok) {
                mesa_move_t m = P->ai_move;
                aos_hal_log("mesa", "%s: %u nodes, depth %d, %u ms", P->g->word, (unsigned)P->ai.nodes,
                            P->ai.depth, (unsigned)(now - P->ai_t0));
                apply(&m);
            } else if (P->nmoves > 0) {
                mesa_move_t m = P->moves[jm_rand() % (uint32_t)P->nmoves];
                apply(&m);
            }
            begin_turn();
        } else if (!P->ai_busy && !P->ai_done && (int32_t)(now - s_next_check) >= 0) {
            /* nothing thinking for the board: start it */
            s_next_check = now + 500;
            if (!P->nmoves) P->nmoves = P->g->moves(P->st, P->moves, MESA_MAX_MOVES);
            ai_start(false);
        }
        return;
    }
    /* a hint came back */
    if (P->hint_busy && P->ai_done && !P->ai_busy) {
        P->hint_busy = false;
        P->ai_done = false;
        P->ai_thread = false;
        if (P->ai_ok && P->ai_move.n) {
            P->sel = P->ai_move.n > 1 ? P->ai_move.n - 1 : 1;
            memcpy(P->chosen, P->ai_move.step, (size_t)P->sel);
            jm_hud_status(_("Esta es la que haría la placa"), 2500);
        }
    }
    if (!P->nmoves && !(P->g->need_roll && P->g->need_roll(P->st) > 0)) {
        P->nmoves = P->g->moves(P->st, P->moves, MESA_MAX_MOVES);
    }
}

/* ---- a person's move ------------------------------------------------------------------------------ */

static bool prefix_ok(int k, int c)
{
    for (int i = 0; i < P->nmoves; i++) {
        const mesa_move_t *m = &P->moves[i];
        if (m->n > k && !memcmp(m->step, P->chosen, (size_t)k) && m->step[k] == c) return true;
    }
    return false;
}

void jm_play_choose(const mesa_move_t *m)
{
    mesa_move_t c = *m;
    jm_hud_panel_close();
    apply(&c);
    begin_turn();
}

/* the moves the steps chosen so far complete: one is played, several ask */
static void complete(void)
{
    mesa_move_t done[8];
    int nd = 0;
    bool longer = false;
    for (int i = 0; i < P->nmoves; i++) {
        const mesa_move_t *m = &P->moves[i];
        if (memcmp(m->step, P->chosen, (size_t)P->sel)) continue;
        if (m->n == P->sel) {
            if (nd < 8) done[nd++] = *m;
        } else if (m->n > P->sel) {
            longer = true;
        }
    }
    if (!nd) return;
    if (nd == 1 && !longer) {
        mesa_move_t m = done[0];
        apply(&m);
        begin_turn();
        return;
    }
    if (nd > 1) jm_hud_choice(done, nd);
}

static void tap_cell(int plane, int piece)
{
    if (P->sel == 0) {
        int c = piece != MESA_NO_CELL && prefix_ok(0, piece) ? piece : plane;
        if (c == MESA_NO_CELL || !prefix_ok(0, c)) {
            if (plane != MESA_NO_CELL || piece != MESA_NO_CELL) jm_audio_play(SND_DENY);
            return;
        }
        P->chosen[0] = (uint8_t)c;
        P->sel = 1;
        jm_audio_play(SND_TAP);
        complete();
        return;
    }
    int last = P->chosen[P->sel - 1];
    if (plane == last || (piece == last && plane == MESA_NO_CELL)) {
        P->sel = 0;     /* put back */
        return;
    }
    int c = plane != MESA_NO_CELL && prefix_ok(P->sel, plane) ? plane
          : piece != MESA_NO_CELL && prefix_ok(P->sel, piece) ? piece : MESA_NO_CELL;
    if (c != MESA_NO_CELL) {
        P->chosen[P->sel++] = (uint8_t)c;
        complete();
        return;
    }
    /* straight to where a jump ends, skipping the cells between */
    int found = -1, count = 0;
    int target = plane != MESA_NO_CELL ? plane : piece;
    for (int i = 0; i < P->nmoves && target != MESA_NO_CELL; i++) {
        const mesa_move_t *m = &P->moves[i];
        if (m->n > P->sel && !memcmp(m->step, P->chosen, (size_t)P->sel) && m->step[m->n - 1] == target) {
            if (found < 0 || memcmp(P->moves[found].step, m->step, m->n)) count++;
            found = i;
        }
    }
    if (count == 1) {
        memcpy(P->chosen, P->moves[found].step, P->moves[found].n);
        P->sel = P->moves[found].n;
        complete();
        return;
    }
    /* another piece of one's own */
    P->sel = 0;
    int s = piece != MESA_NO_CELL && prefix_ok(0, piece) ? piece : plane;
    if (s != MESA_NO_CELL && prefix_ok(0, s)) {
        P->chosen[0] = (uint8_t)s;
        P->sel = 1;
        jm_audio_play(SND_TAP);
        complete();
    }
}

void jm_play_tap(int x, int y)
{
    if (!jm->playing || P->over || jm->panel) return;
    if (!jm_play_person_to_move()) {
        if (P->ai_busy) jm_hud_status(_("Piensa la placa..."), 1200);
        return;
    }
    if (P->rolling) return;
    if (P->g->need_roll && P->g->need_roll(P->st) > 0) {
        jm_play_roll();
        return;
    }
    if (jm_view_busy() || s_pass_wait) return;
    if (!P->nmoves) P->nmoves = P->g->moves(P->st, P->moves, MESA_MAX_MOVES);
    tap_cell(jm_view_hit(x, y), jm_view_hit_piece(x, y));
}

/* ---- the gamepad on the board ---------------------------------------------------------------------- */

void jm_play_pad(void)
{
    aos_pad_t *pad = &jm->pad;
    jm_view_t *v = &jm->view;
    if (aos_pad_pressed(pad, AOS_PAD_L)) jm_play_undo();
    if (aos_pad_pressed(pad, AOS_PAD_R)) jm_play_redo();
    if (!jm_play_person_to_move()) return;
    uint32_t d = pad->repeat & AOS_PAD_DIRS;
    int ncell = P->g->cells ? P->g->cells(P->st) : 64;
    if (d && ncell > 0) {
        if (v->cursor < 0) {
            /* it appears on the piece last chosen, or the middle */
            v->cursor = P->sel ? P->chosen[P->sel - 1] : (P->nmoves ? P->moves[0].step[0] : 0);
        } else {
            float cx, cy;
            jm_view_cell_xy(v->cursor, 0, &cx, &cy);
            float dx = d & AOS_PAD_LEFT ? -1.f : d & AOS_PAD_RIGHT ? 1.f : 0.f;
            float dy = d & AOS_PAD_UP ? -1.f : d & AOS_PAD_DOWN ? 1.f : 0.f;
            int best = -1;
            float bs = 1e9f;
            for (int c = 0; c < ncell; c++) {
                if (c == v->cursor) continue;
                float x, y;
                jm_view_cell_xy(c, 0, &x, &y);
                float ex = x - cx, ey = y - cy;
                float along = ex * dx + ey * dy;
                if (along < 2) continue;
                float across = fabsf(ex * dy - ey * dx);
                float score = along + across * 2.5f;
                if (score < bs) {
                    bs = score;
                    best = c;
                }
            }
            if (best >= 0) v->cursor = best;
        }
    }
    if (aos_pad_pressed(pad, AOS_PAD_A)) {
        if (P->g->need_roll && P->g->need_roll(P->st) > 0) {
            jm_play_roll();
        } else if (v->cursor >= 0 && !jm_view_busy()) {
            if (!P->nmoves) P->nmoves = P->g->moves(P->st, P->moves, MESA_MAX_MOVES);
            tap_cell(v->cursor, v->cursor);
        } else if (v->cursor < 0) {
            v->cursor = P->nmoves ? P->moves[0].step[0] : 0;
        }
    }
    if (aos_pad_pressed(pad, AOS_PAD_B)) P->sel = 0;
}

/* ---- undo, redo, giving up, a hint ---------------------------------------------------------------------- */

/* after n entries, is it a person's turn to choose a move (not to roll)? */
static bool person_at(int n)
{
    jm_play_replay(n);
    int t = P->g->turn(P->st);
    if (t < 0 || !P->human[t]) return false;
    return !(P->g->need_roll && P->g->need_roll(P->st) > 0);
}

bool jm_play_can_undo(void)
{
    return jm->playing && P->n > 0;
}

bool jm_play_can_redo(void)
{
    return jm->playing && P->n < P->top;
}

void jm_play_undo(void)
{
    if (!jm_play_can_undo() || P->rolling) return;
    jm_play_ai_cancel();
    int n = P->n, top = P->top;
    /* back to the last time a person was to move; a roll stays rolled */
    int k = n - 1;
    while (k >= 0 && !person_at(k)) k--;
    if (k < 0) {
        jm_play_replay(n);
        P->top = top;
        begin_turn();
        return;
    }
    jm_play_replay(k);
    P->top = top;
    P->saved_result = false;
    after_change(true);
    jm_save_game();
    begin_turn();
}

void jm_play_redo(void)
{
    if (!jm_play_can_redo() || P->rolling) return;
    jm_play_ai_cancel();
    int top = P->top, k = P->n + 1;
    while (k < top && !person_at(k)) k++;
    jm_play_replay(k);
    P->top = top;
    after_change(true);
    jm_save_game();
    begin_turn();
}

void jm_play_resign(void)
{
    if (!jm->playing || P->over) return;
    int t = P->g->turn(P->st);
    P->resigned = t;
    P->over = true;
    jm_play_ai_cancel();
    jm_save_game();
    jm_hud_refresh();
    jm_hud_end();
}

void jm_play_hint(void)
{
    if (!jm_play_person_to_move() || P->ai_busy) return;
    if (P->g->need_roll && P->g->need_roll(P->st) > 0) return;
    jm_hud_status(_("Pensando..."), 3000);
    ai_start(true);
}
