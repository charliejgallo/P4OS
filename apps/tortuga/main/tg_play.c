/*
 * Tortuga - a level (DESIGN.md, 6): the board, the controls, the program,
 * and the run. The program runs whole first, in a thread of its own, on
 * every map of the level (tg_core: a trace of everything the turtle did),
 * and then the trace is played on the board: at a speed, or a step at a
 * time with its line marked, and backwards. The result explains in plain
 * words what happened and where.
 */
#include "tg_app.h"

#include "aos_fonts.h"
#include "tg_edit.h"
#include "tg_blocks.h"
#include "tg_lua.h"
#include "tg_view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN_STACK (128 * 1024)

enum { P_IDLE, P_PLAY, P_STEP, P_DONE };
enum { JOB_NONE, JOB_RUN, JOB_DONE };

struct tg_play {
    tg_level_t free_lv;
    const tg_level_t *lv;
    bool paper, blocks;
    int lang;                       /* TG_LANG_* this level is played in */
    int idx;                        /* the level's index, -1 in free mode */

    lv_obj_t *scr, *head, *title, *hint_btn, *view_btn, *stars, *dots;  /* stars: the header's row */
    lv_obj_t *ctrl, *b_run, *b_step, *b_stop, *b_speed, *speed_lbl, *step_lbl;
    lv_obj_t *edit_box, *bubble, *panel, *note;
    int bx, by, bw, bh;
    bool editing;

    /* the run */
    volatile int job;
    char *src;
    tg_ev_t *ev_a, *ev_b;
    tg_world_t wa, wb;
    tg_world_t *show;               /* the run played: the first map, or the first that failed */
    int shown_variant;
    int8_t vres[TG_MAXVAR];         /* per map: 1 won, 0 failed, -1 not run */
    tg_ev_t *fev;
    tg_world_t fig;
    bool have_fig;
    bool same_drawing;
    int pieces;

    /* the playback */
    int mode;
    int at;                         /* the event being played */
    float t;                        /* how far into it, 0..1 */
    int last_line;
    uint32_t last_ms;
    int sound_at;
    tg_world_t start;               /* the map before the run */
    tg_scene_t sc;
    int hints;                      /* hints opened in this level */
};

static tg_play_t *P;

static const float SPEED[5] = { 0.5f, 1.0f, 2.0f, 4.0f, 40.0f };

/* ---------------------------------------------------------------- helpers */

static const tg_level_t *lvl(void)
{
    return P->lv;
}

static int world_no(void)
{
    return tg->free_mode ? 9 : lvl()->world;
}

static bool edit_is_text(void)
{
    return !P->blocks;
}

static void mark_line(int line, bool error)
{
    if (P->blocks) tg_blocks_mark(line, error);
    else tg_edit_mark(line, error);
}

static char *program_text(void)
{
    if (P->blocks) return tg_blocks_logo();
    const char *t = tg_edit_text();
    size_t n = strlen(t);
    char *c = tg_malloc(n + 1);
    if (c) memcpy(c, t, n + 1);
    return c;
}

static void save_program(void)
{
    char *t = program_text();
    if (!t) return;
    tg_program_save(tg->free_mode ? NULL : lvl(), P->lang, t);
    tg_free(t);
}

/* ------------------------------------------------------------------ run */

static void run_variant(int v, tg_world_t *w, tg_ev_t *ev)
{
    tg_world_init(w, lvl(), v, tg->prefs.hard, P->paper, ev, TG_EV_CAP);
    if (P->lang == TG_LANG_LUA) {
        tg_lua_run(P->src, w);
        return;
    }
    tg_prog_t pr;
    if (!tg_prog_init(&pr, 4096)) {
        tg_world_fail(w, TG_F_RUNTIME, 0, TG_TR("no hay memoria"));
        return;
    }
    if (!tg_logo_parse(&pr, P->src)) {
        tg_world_fail(w, TG_F_SYNTAX, pr.err_line, pr.err);
    } else {
        tg_logo_run(&pr, w);
    }
    tg_prog_free(&pr);
}

static void run_figure(void)
{
    if (P->have_fig || !P->paper || !lvl()->figure) return;
    tg_prog_t pr;
    if (!tg_prog_init(&pr, 4096)) return;
    if (tg_logo_parse(&pr, lvl()->figure)) {
        tg_level_t any = *lvl();
        any.orders = 0;
        tg_world_init(&P->fig, &any, 0, false, true, P->fev, TG_EV_CAP);
        tg_logo_run(&pr, &P->fig);
        P->fig.lv = lvl();
        P->have_fig = true;
    }
    tg_prog_free(&pr);
}

static void run_thread(void *arg)
{
    (void)arg;
    tg_play_t *p = P;
    int n = p->paper ? 1 : (lvl()->nmap ? lvl()->nmap : 1);
    for (int v = 0; v < TG_MAXVAR; v++) p->vres[v] = -1;
    run_figure();
    run_variant(0, &p->wa, p->ev_a);
    p->show = &p->wa;
    p->shown_variant = 0;
    p->vres[0] = p->wa.state == TG_ST_WIN;
    if (p->paper && p->wa.state == TG_ST_WIN && p->have_fig) {
        p->same_drawing = tg_world_same_drawing(&p->wa, &p->fig);
        p->vres[0] = p->same_drawing;
        if (!p->same_drawing && p->wa.nev) {
            /* no cheering for a drawing that is not the figure */
            tg_ev_t *last = &p->wa.ev[p->wa.nev - 1];
            last->kind = TG_EV_FAIL;
            last->a = TG_F_DRAWING;
            p->wa.state = TG_ST_FAIL;
            p->wa.reason = TG_F_DRAWING;
        }
    }
    for (int v = 1; v < n; v++) {
        run_variant(v, &p->wb, p->ev_b);
        p->vres[v] = p->wb.state == TG_ST_WIN;
        if (!p->vres[v] && p->vres[0]) {
            /* the first map won and this one did not: play this one */
            tg_world_t tw = p->wa;
            tg_ev_t *te = p->ev_a;
            p->wa = p->wb;
            p->ev_a = p->ev_b;
            p->wa.ev = p->ev_a;
            p->wb = tw;
            p->ev_b = te;
            p->show = &p->wa;
            p->shown_variant = v;
            break;
        }
        if (!p->vres[0]) break;
    }
    p->job = JOB_DONE;
}

/* ---------------------------------------------------------------- scene */

static void scene_reset(void)
{
    tg_scene_t *s = &P->sc;
    tg_world_t *w = &P->start;
    tg_world_init(w, lvl(), P->show ? P->shown_variant : 0, tg->prefs.hard, P->paper, NULL, 0);
    memset(s, 0, sizeof(*s));
    memcpy(s->t, w->t, sizeof(s->t));
    s->w = w->w;
    s->h = w->h;
    s->x = P->paper ? w->x : (float)w->tx;
    s->y = P->paper ? w->y : (float)w->ty;
    s->hd = P->paper ? w->hd : 90.0f * (float)w->dir;
    s->anim = SH_WALK;
    s->figure = P->have_fig ? &P->fig : NULL;
    s->ev = P->show && P->mode != P_IDLE ? P->show->ev : NULL;
}

static void apply(const tg_ev_t *e)
{
    tg_scene_t *s = &P->sc;
    switch (e->kind) {
    case TG_EV_MOVE: s->x = e->x1; s->y = e->y1; break;
    case TG_EV_TURN: s->hd = e->h1; break;
    case TG_EV_EAT: s->t[(int)e->y0][(int)e->x0] = '.'; break;
    case TG_EV_KEY: s->t[(int)e->y1][(int)e->x1] = '.'; break;
    case TG_EV_DOOR: s->t[(int)e->y1][(int)e->x1] = 'p'; break;
    default: break;
    }
}

/* the scene with events [0, at) done */
static void scene_at(int at)
{
    scene_reset();
    const tg_world_t *w = P->show;
    if (!w) return;
    for (int i = 0; i < at && i < w->nev; i++) apply(&w->ev[i]);
    P->sc.strokes = at;
}

static float duration(const tg_ev_t *e)
{
    switch (e->kind) {
    case TG_EV_MOVE: {
        float d = sqrtf((e->x1 - e->x0) * (e->x1 - e->x0) + (e->y1 - e->y0) * (e->y1 - e->y0));
        return P->paper ? 120.0f + d * 5.0f : 420.0f * d;
    }
    case TG_EV_TURN: {
        float d = fabsf(e->h1 - e->h0);
        return P->paper ? 80.0f + d * 2.2f : 75.0f * d / 22.5f;
    }
    case TG_EV_EAT: return 650;
    case TG_EV_CHEW: return 500;
    case TG_EV_BUMP: return 750;
    case TG_EV_SPLASH: return 900;
    case TG_EV_SAY: return 1000;
    case TG_EV_SENSE: return 260;
    case TG_EV_KEY: return 180;
    case TG_EV_DOOR: return 260;
    case TG_EV_WIN: return 1600;
    case TG_EV_FAIL: return 300;
    default: return 60;
    }
}

static void bubble(const char *text, lv_color_t color)
{
    if (!text) {
        lv_obj_add_flag(P->bubble, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t *l = lv_obj_get_child(P->bubble, 0);
    lv_label_set_text(l, text);
    lv_obj_set_style_bg_color(P->bubble, color, 0);
    lv_obj_remove_flag(P->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(P->bubble);
    int x, y;
    tg_view_turtle_point(&P->sc, &x, &y);
    int bw = lv_obj_get_width(P->bubble), bh = lv_obj_get_height(P->bubble);
    int px = P->bx + x - bw / 2, py = P->by + y - 70 - bh;
    if (px < P->bx + 4) px = P->bx + 4;
    if (px + bw > P->bx + P->bw - 4) px = P->bx + P->bw - 4 - bw;
    if (py < P->by + 4) py = P->by + 4;
    lv_obj_set_pos(P->bubble, px, py);
}

static void pose(const tg_ev_t *e, float t)
{
    tg_scene_t *s = &P->sc;
    s->anim = SH_WALK;
    s->frame = 0;
    s->lift = 0;
    s->sink = 0;
    s->partial = false;
    switch (e->kind) {
    case TG_EV_MOVE: {
        s->x = e->x0 + (e->x1 - e->x0) * t;
        s->y = e->y0 + (e->y1 - e->y0) * t;
        float dist = P->paper ? 3.0f : sqrtf((e->x1 - e->x0) * (e->x1 - e->x0) + (e->y1 - e->y0) * (e->y1 - e->y0));
        s->frame = 1 + (int)(t * 6.0f * (dist < 1 ? 1 : dist)) % 5;
        if (P->paper && e->pen) {
            s->partial = true;
            s->px0 = e->x0;
            s->py0 = e->y0;
            s->px1 = s->x;
            s->py1 = s->y;
            s->pcolor = e->color;
        }
        break;
    }
    case TG_EV_TURN:
        s->hd = e->h0 + (e->h1 - e->h0) * t;
        break;
    case TG_EV_EAT:
    case TG_EV_CHEW:
        s->anim = SH_EAT;
        s->frame = (int)(t * 4.0f);
        if (s->frame > 3) s->frame = 3;
        if (e->kind == TG_EV_EAT && t > 0.45f) s->t[(int)e->y0][(int)e->x0] = '.';
        break;
    case TG_EV_BUMP: {
        float k = t < 0.3f ? t / 0.3f : 1.0f - (t - 0.3f) / 0.7f;
        s->x = e->x0 + (e->x1 - e->x0) * 0.22f * k;
        s->y = e->y0 + (e->y1 - e->y0) * 0.22f * k;
        s->anim = SH_BUMP;
        s->frame = (int)(t * 4.0f);
        if (s->frame > 3) s->frame = 3;
        break;
    }
    case TG_EV_SPLASH: {
        float k = t < 0.5f ? t / 0.5f : 1.0f;
        s->x = e->x0 + (e->x1 - e->x0) * 0.55f * k;
        s->y = e->y0 + (e->y1 - e->y0) * 0.55f * k;
        s->sink = t > 0.4f ? (t - 0.4f) * 40.0f : 0;
        s->anim = SH_BUMP;
        s->frame = 1;
        break;
    }
    case TG_EV_WIN: {
        s->anim = SH_PARTY;
        s->hd = 180.0f;     /* it turns to us */
        int f = (int)(t * 12.0f) % 6;
        s->frame = f;
        static const float LIFT[6] = { 0, 8, 16, 14, 6, 0 };
        s->lift = LIFT[f];
        break;
    }
    default:
        break;
    }
}

static void enter_event(int i)
{
    const tg_ev_t *e = &P->show->ev[i];
    if (e->line > 0 && e->line != P->last_line) {
        P->last_line = e->line;
        mark_line(e->line, false);
    }
    if (P->sound_at == i) return;
    P->sound_at = i;
    static const int8_t SND[] = {
        [TG_EV_MOVE] = SND_STEP, [TG_EV_TURN] = SND_TURN, [TG_EV_EAT] = SND_EAT, [TG_EV_CHEW] = SND_EAT,
        [TG_EV_SAY] = -1, [TG_EV_PEN] = -1, [TG_EV_COLOR] = -1, [TG_EV_SENSE] = -1, [TG_EV_KEY] = SND_KEY,
        [TG_EV_DOOR] = SND_DOOR, [TG_EV_BUMP] = SND_BUMP, [TG_EV_SPLASH] = SND_SPLASH, [TG_EV_WIN] = SND_WIN,
        [TG_EV_FAIL] = -1,
    };
    if (P->mode != P_PLAY || tg->prefs.speed < 4) {
        if (e->kind < sizeof(SND) && SND[e->kind] >= 0 && !(e->kind == TG_EV_MOVE && P->paper)) {
            tg_audio_play(SND[e->kind]);
        }
    }
    if (e->kind == TG_EV_SAY) {
        char buf[48];
        if (e->str) snprintf(buf, sizeof buf, "%.40s", e->str);
        else if (e->val == floorf(e->val)) snprintf(buf, sizeof buf, "%d", (int)e->val);
        else snprintf(buf, sizeof buf, "%.2f", (double)e->val);
        bubble(buf, lv_color_hex(0xfff7e0));
        lv_obj_set_style_text_color(lv_obj_get_child(P->bubble, 0), lv_color_hex(0x2a2a2a), 0);
    } else if (e->kind == TG_EV_SENSE && P->mode == P_STEP) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s? %s", tg->tlang == TG_ES ? tg_orders[e->a].es : tg_orders[e->a].en,
                 e->val ? _("sí") : _("no"));
        bubble(buf, e->val ? lv_color_hex(0x2e7d32) : lv_color_hex(0x8e2c2c));
        lv_obj_set_style_text_color(lv_obj_get_child(P->bubble, 0), AOS_C_TEXT, 0);
    } else if (e->kind != TG_EV_SENSE) {
        bubble(NULL, AOS_C_TEXT);
    }
}

/* ---------------------------------------------------------------- result */

static void close_panel(void)
{
    if (P->panel) lv_obj_delete(P->panel);
    P->panel = NULL;
}

static void reset_run(void)
{
    close_panel();
    P->mode = P_IDLE;
    P->at = 0;
    P->t = 0;
    P->last_line = -1;
    P->sound_at = -1;
    bubble(NULL, AOS_C_TEXT);
    mark_line(0, false);
    scene_reset();
    P->sc.ev = NULL;
    lv_label_set_text(P->step_lbl, "");
    lv_obj_add_flag(P->step_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void retry_cb(lv_event_t *e)
{
    (void)e;
    reset_run();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    save_program();
    int i = tg->level + 1;
    if (i < tg->levels.n && tg_world_open(tg->levels.lv[i].world)) {
        tg->level = i;
        tg->world = tg->levels.lv[i].world;
        tg_go(SCR_PLAY);
    } else {
        tg_go(SCR_WORLDS);
    }
}

static void fail_text(const tg_world_t *w, char *out, size_t n)
{
    char m[200] = "";
    int L = w->fail_line;
    switch (w->reason) {
    case TG_F_ROCK: snprintf(m, sizeof m, _("En la línea %d, la tortuga chocó con una piedra."), L); break;
    case TG_F_EDGE: snprintf(m, sizeof m, _("En la línea %d, la tortuga chocó con el borde."), L); break;
    case TG_F_WATER:
        snprintf(m, sizeof m, _("En la línea %d, la tortuga se metió al agua. ¿Había un puente?"), L);
        break;
    case TG_F_DOOR: snprintf(m, sizeof m, _("En la línea %d, la puerta estaba cerrada: falta la llave."), L); break;
    case TG_F_NOT_YET: snprintf(m, sizeof m, _("En este nivel todavía no está «%s»."), w->msg); break;
    case TG_F_ARGS: snprintf(m, sizeof m, _("Línea %d: «%s» no lleva esos datos."), L, w->msg); break;
    case TG_F_TURN90: snprintf(m, sizeof m, _("Línea %d: en el mapa la tortuga gira de a 90 grados."), L); break;
    case TG_F_WHOLE: snprintf(m, sizeof m, _("Línea %d: en el mapa se avanza de a casillas enteras."), L); break;
    case TG_F_OFF_PAPER: snprintf(m, sizeof m, _("Línea %d: la tortuga se salió de la hoja."), L); break;
    case TG_F_NOT_HERE: snprintf(m, sizeof m, _("Línea %d: «%s» no sirve en la hoja."), L, w->msg); break;
    case TG_F_SHORT:
        if (w->arg > 0) snprintf(m, sizeof m, _("El programa terminó y la tortuga quedó a %d casillas de la meta."),
                                 w->arg);
        else snprintf(m, sizeof m, "%s", _("El programa terminó y la tortuga no llegó a la meta."));
        break;
    case TG_F_LETTUCE_LEFT: snprintf(m, sizeof m, _("Llegó, pero quedan %d lechugas sin comer."), w->arg); break;
    case TG_F_SAID_WRONG:
        snprintf(m, sizeof m, "%s", _("La tortuga tenía que decir cuántas lechugas había."));
        break;
    case TG_F_PASSED:
        snprintf(m, sizeof m, "%s",
                 _("La tortuga pasó por la meta pero no se quedó ahí: en el modo difícil tiene que terminar justo encima."));
        break;
    case TG_F_LOOP:
        snprintf(m, sizeof m, "%s", _("La tortuga se quedó dando vueltas: ¿el mientras tiene cómo terminar?"));
        break;
    case TG_F_DRAWING:
        snprintf(m, sizeof m, "%s", _("El dibujo no es igual a la figura. Mirá la de puntitos."));
        break;
    case TG_F_SYNTAX:
    case TG_F_RUNTIME:
        if (L > 0) snprintf(m, sizeof m, _("Línea %d: %s"), L, _(w->msg));
        else snprintf(m, sizeof m, "%s", _(w->msg));
        break;
    default: snprintf(m, sizeof m, "%s", _("No anduvo."));
    }
    snprintf(out, n, "%s", m);
}

static int stars_for(bool win)
{
    if (!win) return 0;
    const tg_level_t *l = lvl();
    int s = P->pieces <= l->star3 ? 3 : P->pieces <= l->star2 ? 2 : 1;
    if (P->hints >= 3 && s > 2) s = 2;
    return s;
}

static lv_obj_t *panel(lv_color_t accent)
{
    close_panel();
    lv_obj_t *p = lv_obj_create(P->scr);
    int w = P->bw - 40;
    if (w > 600) w = 600;
    lv_obj_set_width(p, w);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x1b2027), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_90, 0);
    lv_obj_set_style_border_color(p, accent, 0);
    lv_obj_set_style_border_width(p, 3, 0);
    lv_obj_set_style_radius(p, 22, 0);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, P->bx + (P->bw - w) / 2, P->by + 24);
    P->panel = p;
    return p;
}

static void show_result(void)
{
    const tg_world_t *w = P->show;
    int n = P->paper ? 1 : (lvl()->nmap ? lvl()->nmap : 1);
    bool win = true;
    for (int v = 0; v < n; v++) win = win && P->vres[v] == 1;
    if (tg->free_mode) {
        if (w->state != TG_ST_WIN) {
            lv_obj_t *p = panel(AOS_C_ORANGE);
            char m[220];
            fail_text(w, m, sizeof m);
            lv_obj_t *l = tg_label(p, m, aos_font_body, AOS_C_TEXT);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, LV_PCT(100));
            tg_button(p, _("Volver a intentar"), lv_color_hex(0x2c3440), retry_cb, NULL);
            tg_audio_play(SND_FAIL);
        }
        return;
    }
    if (win) {
        int s = stars_for(true);
        tg_score_t *sc = &tg->score[P->idx];
        bool better = s > sc->stars[P->lang];
        if (better) {
            sc->stars[P->lang] = (uint8_t)s;
            /* the header's stars follow */
            for (int k = 0; k < 3 && P->stars; k++) {
                lv_obj_set_style_text_color(lv_obj_get_child(P->stars, k),
                                            k < s ? AOS_C_YELLOW : lv_color_hex(0x48484A), 0);
            }
        }
        if (tg->prefs.hard) sc->medal |= (uint8_t)(1u << P->lang);
        tg_save_scores();
        save_program();
        lv_obj_t *p = panel(AOS_C_YELLOW);
        tg_label(p, _("¡Llegó!"), aos_font_title, AOS_C_YELLOW);
        tg_stars_draw(p, s, 44);
        char buf[160];
        if (s == 3) snprintf(buf, sizeof buf, _("%d piezas: ¡perfecto!"), P->pieces);
        else snprintf(buf, sizeof buf, _("%d piezas. Para tres estrellas: %d o menos."), P->pieces, lvl()->star3);
        lv_obj_t *l = tg_label(p, buf, aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        if (P->hints >= 3 && P->pieces <= lvl()->star3) {
            lv_obj_t *h = tg_label(p, _("Con la tercera pista, dos estrellas como mucho."), aos_font_small, AOS_C_DIM);
            lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(h, LV_PCT(100));
        }
        if (n > 1) {
            snprintf(buf, sizeof buf, _("Anduvo en los %d caminos."), n);
            tg_label(p, buf, aos_font_small, AOS_C_GREEN);
        }
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_t *a = tg_button(row, _("Otra vez"), lv_color_hex(0x2c3440), retry_cb, NULL);
        lv_obj_set_flex_grow(a, 1);
        lv_obj_t *b = tg_button(row, _("Siguiente"), lv_color_hex(0xFF9F0A), next_cb, NULL);
        lv_obj_set_flex_grow(b, 1);
        return;
    }
    char m[260];
    fail_text(w, m, sizeof m);
    if (n > 1 && P->shown_variant > 0) {
        char t[300];
        snprintf(t, sizeof t, _("En el primer camino anduvo, en el %d no: %s"), P->shown_variant + 1, m);
        snprintf(m, sizeof m, "%s", t);
        if (P->shown_variant > 0 && w->reason != TG_F_SYNTAX) {
            size_t k = strlen(m);
            snprintf(m + k, sizeof m - k, " %s", _("Probá preguntar en vez de contar."));
        }
    }
    lv_obj_t *p = panel(AOS_C_ORANGE);
    lv_obj_t *l = tg_label(p, m, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    tg_button(p, _("Volver a intentar"), lv_color_hex(0x2c3440), retry_cb, NULL);
    if (w->fail_line > 0) mark_line(w->fail_line, true);
    tg_audio_play(SND_FAIL);
}

/* ------------------------------------------------------------ controls */

static bool start_job(void)
{
    if (P->job == JOB_RUN) return false;
    tg_free(P->src);
    P->src = program_text();
    if (!P->src) return false;
    bool empty = true;
    for (const char *c = P->src; *c && empty; c++) empty = *c == ' ' || *c == '\n' || *c == '\t' || *c == '\r';
    if (empty) {
        lv_obj_t *p = panel(AOS_C_ORANGE);
        lv_obj_t *l = tg_label(p, P->blocks ? _("Todavía no hay bloques: tocá las piezas de abajo.")
                                            : _("Todavía no hay programa: escribilo o tocá las órdenes de abajo."),
                               aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return false;
    }
    save_program();
    if (P->blocks || P->lang == TG_LANG_LOGO) {
        tg_prog_t pr;
        if (tg_prog_init(&pr, 4096)) {
            if (tg_logo_parse(&pr, P->src)) P->pieces = tg_logo_pieces(&pr);
            tg_prog_free(&pr);
        }
    } else {
        P->pieces = tg_text_pieces(P->src, P->lang == TG_LANG_PY);
    }
    P->job = JOB_RUN;
    P->show = NULL;
    if (!aos_hal_thread_start("tortuga", run_thread, NULL, RUN_STACK, 4)) {
        P->job = JOB_NONE;
        return false;
    }
    return true;
}

static void run_cb(lv_event_t *e)
{
    (void)e;
    if (P->mode == P_STEP && P->show) {
        P->mode = P_PLAY;
        P->last_ms = lv_tick_get();
        return;
    }
    if (P->job == JOB_RUN) return;
    reset_run();
    tg_edit_keyboard(false);
    if (start_job()) P->mode = P_PLAY;
}

static void step_cb(lv_event_t *e)
{
    (void)e;
    if (P->job == JOB_RUN) return;
    if (P->mode == P_IDLE || P->mode == P_DONE) {
        reset_run();
        tg_edit_keyboard(false);
        if (start_job()) {
            P->mode = P_STEP;
            P->at = 0;
            P->t = 0;
        }
        return;
    }
    if (P->mode == P_PLAY) {
        P->mode = P_STEP;
        return;
    }
    /* step: finish this event and go to the next */
    if (P->show && P->at < P->show->nev) {
        apply(&P->show->ev[P->at]);
        P->sc.strokes = P->at + 1;
        P->at++;
        P->t = 0;
        if (P->at < P->show->nev) enter_event(P->at);
    }
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    if (P->job == JOB_RUN) return;
    reset_run();
}

static void speed_text(void)
{
    static const char *const N[5] = { N_("Lento"), N_("Normal"), N_("Rápido"), N_("Muy rápido"), N_("Al toque") };
    lv_label_set_text(P->speed_lbl, _(N[tg->prefs.speed]));
}

static void speed_cb(lv_event_t *e)
{
    (void)e;
    tg->prefs.speed = (uint8_t)((tg->prefs.speed + 1) % 5);
    speed_text();
    tg_save_prefs();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    tg_play_back();
}

/* ----------------------------------------------------------------- hints */

static void hint_close(lv_event_t *e)
{
    (void)e;
    close_panel();
}

static void hint_more(lv_event_t *e);

static void hint_show(void)
{
    const tg_level_t *l = lvl();
    lv_obj_t *p = panel(AOS_C_YELLOW);
    char buf[64];
    snprintf(buf, sizeof buf, _("Pista %d de %d"), P->hints, 3);
    tg_label(p, buf, aos_font_small, AOS_C_YELLOW);
    if (P->hints <= 2) {
        const char *h = NULL;
        if (P->hints - 1 < l->nhint) h = tg_level_text(l->hint[P->hints - 1], tg->tlang);
        if (!h || !h[0]) h = tg_world_idea(l->world);
        lv_obj_t *t = tg_label(p, h, aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, LV_PCT(100));
    } else {
        /* the skeleton: the reference, with its numbers hidden */
        lv_obj_t *t = tg_label(p, _("Así empieza (cambiá los ?):"), aos_font_small, AOS_C_DIM);
        (void)t;
        /* the reference is Logo; the other languages are written from its
         * tree, in the player's words */
        static char src[1400];
        src[0] = 0;
        tg_prog_t pr;
        if (l->sol[TG_SOL_LOGO] && tg_prog_init(&pr, 2048)) {
            if (tg_logo_parse(&pr, l->sol[TG_SOL_LOGO])) {
                int kind = P->lang == TG_LANG_LUA ? TG_GEN_LUA : P->lang == TG_LANG_PY ? TG_GEN_PY : TG_GEN_LOGO;
                tg_logo_gen(&pr, kind, tg->tlang == TG_ES ? TG_ES : TG_EN, src, sizeof src);
            }
            tg_prog_free(&pr);
        }
        static char sk[1400];
        int k = 0;
        for (const char *c = src; *c && k < (int)sizeof(sk) - 2; c++) {
            if (*c >= '0' && *c <= '9') {
                if (k == 0 || sk[k - 1] != '?') sk[k++] = '?';
            } else {
                sk[k++] = *c;
            }
        }
        sk[k] = 0;
        lv_obj_t *code = tg_label(p, sk, &aos_mono_22, AOS_C_TEXT);
        (void)code;
    }
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *a = tg_button(row, _("Cerrar"), lv_color_hex(0x2c3440), hint_close, NULL);
    lv_obj_set_flex_grow(a, 1);
    if (P->hints < 3) {
        lv_obj_t *b = tg_button(row, P->hints == 2 ? _("Ver el esqueleto") : _("Otra pista"), lv_color_hex(0x5a4a10),
                                hint_more, NULL);
        lv_obj_set_flex_grow(b, 1);
    }
}

static void hint_more(lv_event_t *e)
{
    (void)e;
    if (P->hints < 3) P->hints++;
    hint_show();
}

static void hint_cb(lv_event_t *e)
{
    (void)e;
    if (tg->free_mode) return;
    if (!P->hints) P->hints = 1;
    hint_show();
}

/* "see it in": the blocks written in another language */
static void view_lang_cb(lv_event_t *e)
{
    int kind = (int)(intptr_t)lv_event_get_user_data(e);
    char *t = tg_blocks_logo();
    if (!t) return;
    tg_prog_t pr;
    static char out[4096];
    out[0] = 0;
    if (tg_prog_init(&pr, 4096)) {
        if (tg_logo_parse(&pr, t)) tg_logo_gen(&pr, kind, tg->tlang == TG_ES ? TG_ES : TG_EN, out, sizeof out);
        tg_prog_free(&pr);
    }
    tg_free(t);
    lv_obj_t *p = panel(lv_color_hex(0x0A84FF));
    lv_obj_t *code = tg_label(p, out[0] ? out : _("(vacío)"), &aos_mono_22, AOS_C_TEXT);
    (void)code;
    tg_button(p, _("Cerrar"), lv_color_hex(0x2c3440), hint_close, NULL);
}

static void view_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = panel(lv_color_hex(0x0A84FF));
    tg_label(p, _("Ver estos bloques escritos en..."), aos_font_body, AOS_C_TEXT);
    static const char *const N[3] = { "Logo", "Lua", "Python" };
    for (int k = 0; k < 3; k++) {
        lv_obj_t *b = tg_button(p, N[k], tg_lang_color(k + 1), view_lang_cb, (void *)(intptr_t)k);
        lv_obj_set_width(b, LV_PCT(100));
    }
    tg_button(p, _("Cerrar"), lv_color_hex(0x2c3440), hint_close, NULL);
}

/* ----------------------------------------------------------------- cards */

/* A level's cards (@carta): what a world is about, or a story's chapter,
 * shown before the level until it has stars in this language. */
static int s_card;

static void card_next(lv_event_t *e);

static void show_card(int i)
{
    const tg_level_t *l = lvl();
    s_card = i;
    lv_obj_t *p = panel(lv_color_hex(0x64B5FF));
    if (l->ncard > 1) {
        char buf[24];
        snprintf(buf, sizeof buf, "%d / %d", i + 1, l->ncard);
        tg_label(p, buf, aos_font_small, AOS_C_DIM);
    }
    lv_obj_t *t = tg_label(p, tg_level_text(l->card[i], tg->tlang), aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_t *b = tg_button(p, i + 1 < l->ncard ? _("Siguiente") : _("¡A jugar!"), lv_color_hex(0x0A84FF),
                            card_next, NULL);
    lv_obj_set_width(b, LV_PCT(100));
}

static void card_next(lv_event_t *e)
{
    (void)e;
    tg_audio_play(SND_TAP);
    if (s_card + 1 < lvl()->ncard) show_card(s_card + 1);
    else close_panel();
}

/* ---------------------------------------------------------------- layout */

static lv_obj_t *ctl_button(lv_obj_t *parent, const char *sym, const char *text, lv_color_t bg, lv_color_t fg,
                            lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 64);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    tg_label(b, sym, &aos_sym_28, fg);
    if (text) tg_label(b, text, aos_font_small, fg);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void layout(void);

static void edit_focus(bool editing)
{
    P->editing = editing;
    layout();
}

static void layout(void)
{
    int W = tg->W, H = tg->H;
    int head_h = 72;
    if (tg->land) {
        int bw = W * 52 / 100;
        P->bx = 0;
        P->by = head_h;
        P->bw = bw;
        P->bh = H - head_h;
        int rx = bw + 12, rw = W - rx - 12;
        lv_obj_set_pos(P->ctrl, rx, head_h);
        lv_obj_set_size(P->ctrl, rw, 72);
        int ey = head_h + 84;
        if (P->editing && edit_is_text()) {
            /* the keyboard covers the bottom: the program takes the whole top */
            lv_obj_add_flag(P->ctrl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tg_view_obj(), LV_OBJ_FLAG_HIDDEN);
            tg_edit_place(12, head_h, W - 24, H - head_h - tg_edit_keyboard_height());
        } else {
            lv_obj_remove_flag(P->ctrl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(tg_view_obj(), LV_OBJ_FLAG_HIDDEN);
            if (P->blocks) tg_blocks_place(rx, ey, rw, H - ey - 8);
            else tg_edit_place(rx, ey, rw, H - ey - 8);
        }
    } else {
        int bh = H * 40 / 100;
        if (P->paper) bh = W - 40 < H * 44 / 100 ? W - 40 : H * 44 / 100;
        P->bx = 0;
        P->by = head_h;
        P->bw = W;
        P->bh = bh;
        int cy = head_h + bh + 8;
        lv_obj_set_pos(P->ctrl, 12, cy);
        lv_obj_set_size(P->ctrl, W - 24, 72);
        int ey = cy + 80;
        if (P->editing && edit_is_text()) {
            lv_obj_add_flag(P->ctrl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tg_view_obj(), LV_OBJ_FLAG_HIDDEN);
            tg_edit_place(12, head_h, W - 24, H - head_h - tg_edit_keyboard_height());
        } else {
            lv_obj_remove_flag(P->ctrl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(tg_view_obj(), LV_OBJ_FLAG_HIDDEN);
            if (P->blocks) tg_blocks_place(12, ey, W - 24, H - ey - 8);
            else tg_edit_place(12, ey, W - 24, H - ey - 8);
        }
    }
}

static void build_header(void)
{
    lv_obj_t *h = lv_obj_create(P->scr);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, tg->W, 72);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    P->head = h;
    lv_obj_t *b = lv_button_create(h);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 64, 64);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *bl = tg_label(b, AOS_SYM_ARROW_LEFT, &aos_sym_44, AOS_C_ORANGE);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);

    char buf[96];
    if (tg->free_mode) snprintf(buf, sizeof buf, "%s", _("Hoja libre"));
    else snprintf(buf, sizeof buf, "%s  %s", lvl()->id, tg_level_text(lvl()->title, tg->tlang));
    P->title = tg_label(h, buf, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(P->title, LV_LABEL_LONG_DOT);
    lv_obj_align(P->title, LV_ALIGN_LEFT_MID, 72, 0);

    /* on the right: the hint, "see in" (blocks), the stars, the maps */
    lv_obj_t *r = lv_obj_create(h);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, 64);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -8, 0);
    if (!tg->free_mode) {
        int n = lvl()->nmap > 1 ? lvl()->nmap : 0;
        if (n) {
            lv_obj_t *d = tg_label(r, "", aos_font_small, AOS_C_DIM);
            snprintf(buf, sizeof buf, _("%d mapas"), n);
            lv_label_set_text(d, buf);
        }
        P->stars = tg_stars_draw(r, tg->score[P->idx].stars[P->lang], 28);
        P->hint_btn = lv_button_create(r);
        lv_obj_remove_style_all(P->hint_btn);
        lv_obj_set_size(P->hint_btn, 56, 56);
        lv_obj_t *hl = tg_label(P->hint_btn, AOS_SYM_LIGHTBULB_OUTLINE, &aos_sym_44, AOS_C_YELLOW);
        lv_obj_center(hl);
        lv_obj_add_event_cb(P->hint_btn, hint_cb, LV_EVENT_CLICKED, NULL);
    }
    if (P->blocks) {
        P->view_btn = lv_button_create(r);
        lv_obj_remove_style_all(P->view_btn);
        lv_obj_set_size(P->view_btn, 56, 56);
        lv_obj_t *vl = tg_label(P->view_btn, AOS_SYM_EYE_OUTLINE, &aos_sym_44, lv_color_hex(0x64B5FF));
        lv_obj_center(vl);
        lv_obj_add_event_cb(P->view_btn, view_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_update_layout(h);
    lv_obj_set_width(P->title, lv_obj_get_x(r) - 80);
}

void tg_play_open(lv_obj_t *scr)
{
    tg_play_close();
    P = tg_malloc(sizeof(tg_play_t));
    if (!P) return;
    tg->play = P;
    P->scr = scr;
    P->idx = tg->free_mode ? -1 : tg->level;
    if (tg->free_mode) {
        tg_level_t *f = &P->free_lv;
        memset(f, 0, sizeof(*f));
        f->id = "libre";
        f->world = 9;
        f->goals = TG_G_DRAW;
        f->title[TG_ES] = "Hoja libre";
        P->lv = f;
    } else {
        P->lv = &tg->levels.lv[tg->level];
    }
    P->paper = (lvl()->goals & TG_G_DRAW) != 0;
    P->lang = tg->prefs.lang;
    if (!tg_lang_available(P->lang)) P->lang = TG_LANG_LOGO;
    P->blocks = P->lang == TG_LANG_BLOCKS;
    P->ev_a = tg_malloc(sizeof(tg_ev_t) * TG_EV_CAP);
    P->ev_b = tg_malloc(sizeof(tg_ev_t) * TG_EV_CAP);
    P->fev = tg_malloc(sizeof(tg_ev_t) * TG_EV_CAP);

    build_header();
    /* the board */
    int W = tg->W, H = tg->H;
    int bw = tg->land ? W * 52 / 100 : W;
    int bh = tg->land ? H - 72 : H * 40 / 100;
    if (!tg->land && P->paper) bh = W - 40 < H * 44 / 100 ? W - 40 : H * 44 / 100;
    tg_view_open(scr, 0, 72, bw, bh);
    run_figure();
    P->bx = 0;
    P->by = 72;
    P->bw = bw;
    P->bh = bh;

    /* the controls */
    P->ctrl = lv_obj_create(scr);
    lv_obj_remove_style_all(P->ctrl);
    lv_obj_set_flex_flow(P->ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(P->ctrl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(P->ctrl, 10, 0);
    lv_obj_remove_flag(P->ctrl, LV_OBJ_FLAG_SCROLLABLE);
    P->b_run = ctl_button(P->ctrl, AOS_SYM_PLAY, _("Correr"), lv_color_hex(0xFF9F0A), lv_color_hex(0x2a1a05),
                          run_cb);
    P->b_step = ctl_button(P->ctrl, AOS_SYM_STEP_FORWARD, _("Paso"), lv_color_hex(0x2c3440), AOS_C_TEXT, step_cb);
    P->b_stop = ctl_button(P->ctrl, AOS_SYM_STOP, NULL, lv_color_hex(0x2c3440), AOS_C_TEXT, stop_cb);
    lv_obj_set_flex_grow(P->b_stop, 0);
    lv_obj_set_width(P->b_stop, 72);
    P->b_speed = lv_button_create(P->ctrl);
    lv_obj_set_size(P->b_speed, tg->land ? 150 : 170, 64);
    lv_obj_set_style_bg_color(P->b_speed, lv_color_hex(0x2c3440), 0);
    lv_obj_set_style_radius(P->b_speed, 16, 0);
    lv_obj_set_style_shadow_width(P->b_speed, 0, 0);
    lv_obj_add_event_cb(P->b_speed, speed_cb, LV_EVENT_CLICKED, NULL);
    P->speed_lbl = tg_label(P->b_speed, "", aos_font_small, AOS_C_TEXT);
    lv_obj_center(P->speed_lbl);
    speed_text();

    /* the step counter, over the board's corner */
    P->step_lbl = tg_label(scr, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_style_bg_color(P->step_lbl, lv_color_hex(0x101318), 0);
    lv_obj_set_style_bg_opa(P->step_lbl, LV_OPA_60, 0);
    lv_obj_set_style_radius(P->step_lbl, 12, 0);
    lv_obj_set_style_pad_hor(P->step_lbl, 10, 0);
    lv_obj_set_style_pad_ver(P->step_lbl, 2, 0);
    lv_obj_set_pos(P->step_lbl, 10, 80);

    /* the program */
    char *prog = tg_program_load(tg->free_mode ? NULL : lvl(), P->lang);
    if (P->blocks) tg_blocks_open(scr, lvl(), world_no(), prog);
    else tg_edit_open(scr, P->lang, lvl(), world_no(), prog ? prog : "", edit_focus);
    tg_free(prog);

    /* the bubble over the turtle */
    P->bubble = lv_obj_create(scr);
    lv_obj_remove_style_all(P->bubble);
    lv_obj_set_size(P->bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(P->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(P->bubble, 16, 0);
    lv_obj_set_style_pad_hor(P->bubble, 14, 0);
    lv_obj_set_style_pad_ver(P->bubble, 6, 0);
    lv_obj_remove_flag(P->bubble, LV_OBJ_FLAG_CLICKABLE);
    tg_label(P->bubble, "", aos_font_body, lv_color_hex(0x2a2a2a));
    lv_obj_add_flag(P->bubble, LV_OBJ_FLAG_HIDDEN);

    layout();
    reset_run();
    tg_world_t first;
    tg_world_init(&first, lvl(), 0, false, P->paper, NULL, 0);
    tg_view_level(&first, P->have_fig ? &P->fig : NULL);
    P->sc.tick = 0;
    tg_view_draw(&P->sc);
    P->last_ms = lv_tick_get();
    if (!tg->free_mode && lvl()->ncard && !tg->score[P->idx].stars[P->lang]) show_card(0);
}

void tg_play_close(void)
{
    if (!P) return;
    /* the run's thread uses P: it ends on its own within its budget */
    for (int i = 0; i < 300 && P->job == JOB_RUN; i++) aos_hal_sleep_ms(10);
    save_program();
    if (P->blocks) tg_blocks_close();
    else tg_edit_close();
    tg_view_close();
    tg_free(P->src);
    tg_free(P->ev_a);
    tg_free(P->ev_b);
    tg_free(P->fev);
    tg_free(P);
    P = NULL;
    tg->play = NULL;
}

bool tg_play_back(void)
{
    if (!P) return false;
    if (P->panel) {
        close_panel();
        return true;
    }
    if (P->editing) {
        tg_edit_keyboard(false);
        return true;
    }
    save_program();
    tg_go(tg->free_mode ? SCR_HOME : SCR_LEVELS);
    return true;
}

/* ------------------------------------------------------------------ frame */

void tg_play_frame(void)
{
    if (!P) return;
    uint32_t now = lv_tick_get();
    float dt = (float)(now - P->last_ms);
    P->last_ms = now;
    if (dt > 100) dt = 100;
    P->sc.tick++;
    if (P->job == JOB_DONE) {
        P->job = JOB_NONE;
        P->at = 0;
        P->t = 0;
        P->sound_at = -1;
        scene_at(0);
        P->sc.ev = P->show->ev;
        if (P->show->nev) enter_event(0);
    }
    if (P->show && P->job == JOB_NONE && (P->mode == P_PLAY || P->mode == P_STEP)) {
        const tg_world_t *w = P->show;
        if (P->mode == P_PLAY && tg->prefs.speed == 4) {
            /* "right away": straight to the end */
            scene_at(w->nev);
            P->at = w->nev;
        }
        if (P->at < w->nev) {
            const tg_ev_t *e = &w->ev[P->at];
            float d = duration(e) / SPEED[tg->prefs.speed];
            if (P->mode == P_STEP) d = duration(e) / 1.5f;
            P->t += dt / (d < 1 ? 1 : d);
            if (P->t >= 1.0f) {
                if (P->mode == P_PLAY || e->kind == TG_EV_WIN) {
                    apply(e);
                    P->sc.strokes = P->at + 1;
                    P->at++;
                    P->t = 0;
                    if (P->at < w->nev) enter_event(P->at);
                } else {
                    P->t = 1.0f;     /* a step waits at the end of its event */
                }
            }
            if (P->at < w->nev) pose(&w->ev[P->at], P->t > 1 ? 1 : P->t);
        }
        char buf[48];
        snprintf(buf, sizeof buf, _("paso %d de %d"), P->at < w->nev ? P->at + 1 : w->nev, w->nev);
        lv_label_set_text(P->step_lbl, buf);
        lv_obj_remove_flag(P->step_lbl, LV_OBJ_FLAG_HIDDEN);
        if (P->at >= w->nev) {
            P->mode = P_DONE;
            if (w->nev && w->ev[w->nev - 1].kind == TG_EV_WIN) {
                P->sc.anim = SH_PARTY;
                P->sc.hd = 180.0f;
            }
            show_result();
        }
    } else if (P->mode == P_DONE && P->sc.anim == SH_PARTY) {
        /* it keeps cheering */
        int f = (P->sc.tick / 3) % 6;
        static const float LIFT[6] = { 0, 8, 16, 14, 6, 0 };
        P->sc.frame = f;
        P->sc.lift = LIFT[f];
    }
    tg_view_draw(&P->sc);
}

/* ---------------------------------------------------------------- gamepad */

void tg_play_pad(void)
{
    aos_pad_t *p = &tg->pad;
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        if (P->mode == P_PLAY) stop_cb(NULL);
        else run_cb(NULL);
    }
    if (P->panel && aos_pad_pressed(p, AOS_PAD_A)) {
        if (P->mode == P_DONE) retry_cb(NULL);
        else close_panel();
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_R)) step_cb(NULL);
    if (aos_pad_pressed(p, AOS_PAD_L)) speed_cb(NULL);
    if (P->blocks) tg_blocks_pad(p);
}
