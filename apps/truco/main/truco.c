/* This app is deliberately NOT translated.
 *
 * The calls of truco -envido, quiero retruco, falta envido- and the vocabulary
 * of the table are not interface copy: they are the names of the plays.
 * Translating them would give a game that is no longer truco. That is why
 * there is not a single _() here and the aos.truco.lang catalogue does not
 * exist: the system may be in English and this screen stays in Spanish, which
 * is the right thing.
 *
 * The only thing translated is the menu's "Truco" name, which comes from the
 * system catalogue like any other app's. See docs/I18N.md. */

/*
 * P4OS - TRUCO (from AmoledOS)
 *
 * Argentine truco, two-handed against the machine, without flor and to 30
 * points. A green baize table, Spanish playing cards drawn in code and the
 * matchstick scoreboard, which is how it is really kept: a little square of
 * four sticks plus the diagonal for every five points, three squares of malas
 * and three of buenas.
 *
 * The file is split in three on purpose:
 *
 *   tr_game.c   the rules and the opponent. It knows nothing of LVGL, so a
 *               whole game can be played from tools/tr_harness.c: thousands of
 *               games in a row verifying that the score adds up and that
 *               nobody is left waiting for a turn that never comes.
 *   tr_cards.c  the drawing of the cards and of the scoreboard.
 *   truco.c     this one: the table, the animations and who touches what.
 *
 * The rules and the screen talk to each other through EVENTS. tr_apply()
 * leaves in a queue what happened ("said truco", "played the 7 of swords") and
 * here they are taken out one at a time and animated. Without that, every rule
 * would have to know how long an animation lasts.
 *
 * What the port changed:
 *
 *   - The cards are drawn at x2 upright (124 x 192) and x1.7 lying down, the
 *     scoreboard at x2 and x1.4; the layout comes from the root (geo_t).
 *     Upright it is the watch's table, stretched: scoreboard, their hand,
 *     the two rows of tricks, the calls and your hand at the bottom. Lying
 *     down the table goes right and the scoreboard, the calls and the menu
 *     make a column on the left.
 *   - There is a menu (the round button, or the back gesture): carry on, a
 *     new game, playing two-handed and leaving. On the watch leaving took a
 *     second back gesture; here the menu says what will happen.
 *   - Two players is the watches' link (tl_link.h), which rides on
 *     aos_hal_link_*. On a P4 without the radio link aos_hal_link_start()
 *     fails: then the option stays in the menu, greyed, saying why, and the
 *     game is against the machine, fully.
 *   - Turning the screen goes through resize(): the view is rebuilt in the
 *     new size around the same game, cards where they were.
 *   - A USB gamepad plays it through aos_pad_menu: the d-pad walks your
 *     cards still in hand and the calls showing; A does
 *     what a tap does (the first lifts a card, the second plays it). B puts
 *     a lifted card back, START opens and closes the menu, and at the end
 *     of a game A or START carries on. The outline only shows once the pad
 *     is used.
 *
 * Development switches (simulator; on the board getenv() returns NULL):
 *   TRUCO_AUTO=1        the machine plays both sides (for leaving it running)
 *   TRUCO_SHOWALL=1     the opponent's cards are visible
 *   TRUCO_FAST=1        no waits between plays
 *   TRUCO_SEED=n        reproducible deal
 *   TRUCO_SCORE=a,b     starts with that score (for testing the falta and the ending)
 *   TRUCO_LINK=1        straight to two players, without asking
 *   TRUCO_MENU=1        open the menu
 */
#include "aos_app.h"
#include "aos_theme.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "tr_game.h"
#include "tr_cards.h"
#include "tl_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_MAX     6
#define BTN_GAP     12
#define EDGE_TOP    48                  /* the notifications' strip           */
#define EDGE_BOT    40                  /* the home strip, with air            */
#define PAD         24

/* --- geometry of the table -------------------------------------------------
 * Every position is a card's top-left corner, as on the watch; the canvas
 * sits TR_PAD further out for the shadow. */
typedef struct {
    int   w, h;
    bool  land;
    float k;                            /* cards: watch pixels to these        */
    float sk;                           /* the scoreboard's                    */
    int   sc_x, sc_y, sc_w, sc_h;
    int   opp_y, opp_x[3];
    int   hand_y, hand_x[3];
    int   mesa_op_y, mesa_my_y, mesa_x[3], mesa_dx;
    int   win_dy;                       /* the trick's winner sits higher      */
    int   lift;                         /* how far the chosen card is lifted   */
    int   deck_x, deck_y;
    int   oval_x, oval_y, oval_w, oval_h;
    int   ban_x, ban_y, ban_w, ban_h;
    int   bar_x, bar_w, bar_y1;         /* the calls: bottom edge of the rows  */
    int   btn_h, btn_cols, btn_wmax;
    int   tantos_x, tantos_y;
    int   wait_x, wait_y, wait_w;
    int   menu_x, menu_y, menu_w, menu_h;
} geo_t;

typedef struct {
    geo_t     geo;
    lv_obj_t *root;
    lv_obj_t *oval;
    lv_obj_t *score_cv;
    void     *score_buf;

    lv_obj_t *card[2][TR_HAND_N];
    void     *card_buf[2][TR_HAND_N];
    int32_t   cx[2][TR_HAND_N], cy[2][TR_HAND_N];   /* current position */
    int32_t   tx[2][TR_HAND_N], ty[2][TR_HAND_N];   /* destination */
    int8_t    face[2][TR_HAND_N];   /* what each card shows: 0..39, -1 back  */
    bool      vis[2][TR_HAND_N];    /* dealt and on the table                */
    bool      gathered;             /* the hand is over, cards to the deck   */

    lv_obj_t *deck;
    void     *deck_buf;

    lv_obj_t *banner, *banner_lbl;
    char      banner_txt[64];       /* to put it back after a turn */
    lv_obj_t *btn[BTN_MAX], *btn_lbl[BTN_MAX + 2];   /* + the two of the chooser */
    tr_call_t btn_call[BTN_MAX];
    lv_obj_t *tantos;
    lv_obj_t *menu_btn;

    lv_timer_t *timer;
    tr_state_t  g;

    int      sel;                   /* card touched, -1 = none */
    uint32_t wait_until;
    uint32_t banner_until;
    int      deal_i;                /* -1 = not dealing */
    int      bar_n;                 /* buttons visible */
    bool     over;                  /* game over, waiting for a touch */
    bool     closing;
    bool     want_exit;             /* SALIR: left from the timer, not a callback */

    bool     auto_play, showall, fast;

    int32_t  won, lost;             /* games won and lost */

    /* The menu */
    lv_obj_t *menu;
    lv_obj_t *menu_stats, *menu_duo, *menu_duo_lbl, *menu_note, *menu_new;
    lv_obj_t *menu_resume, *menu_exit;
    bool      menu_open;

    /* The gamepad: pmenu holds whatever can be pressed right now */
    aos_pad_t      pad;
    aos_pad_menu_t pmenu;

    /* Two watches (tl_link.h). 'me' is my seat in the engine: 0 alone or as
     * the host, 1 as the guest. Everything the UI draws goes through ME/THEM
     * and never through TR_YO/TR_EL, so the guest also sees itself at the
     * bottom of the table. */
    int       me;
    bool      link;                 /* playing against the other watch */
    bool      link_up;              /* aos_hal_link_start() went through */
    bool      link_avail;           /* it can: the radio started when we asked */
    bool      have_partner;         /* and there is somebody paired in Enlace */
    int       lk;                   /* LK_* */
    bool      is_host;
    uint32_t  nonce, their_nonce;   /* this run of the app, and the partner's */
    uint32_t  my_seed;
    uint32_t  hello_ms;             /* last hello sent */
    uint32_t  unseen_ms;            /* since when the partner has been silent */
    char      pname[AOS_LINK_NAME_MAX + 1];
    tl_act_t  inbox[TL_INBOX];      /* what came in, applied when the table is quiet */
    uint8_t   in_head, in_tail;
    bool      await_echo;           /* guest: my move went to the host, not yet back */
    uint32_t  echo_ms;
    lv_obj_t *choose[2];            /* "the machine" / "<name>" */
    lv_obj_t *wait_lbl;
    bool      wait_on;
} truco_t;

static truco_t s_t;

#define ME    (s_t.me)
#define THEM  (s_t.me ^ 1)

enum { LK_OFF = 0, LK_CHOOSE, LK_HELLO, LK_PLAY, LK_LOST };

static void link_begin(void);
static void link_end(void);
static void solo_begin(void);
static void game_begin(uint32_t seed);
static void choose_cb(lv_event_t *e);
static void menu_show(bool on);
static void refresh_bar(void);

/* -------------------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static bool elapsed(uint32_t deadline)
{
    return (int32_t)(now_ms() - deadline) >= 0;
}

static int delay(int ms)
{
    return s_t.fast ? (ms > 120 ? 60 : 20) : ms;
}

static bool dev_flag(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && v[0] != '0';
}

/* -------------------------------------------------------------------------- */
/* Geometry                                                                    */

static void geo_compute(geo_t *o, int w, int h)
{
    memset(o, 0, sizeof *o);
    o->w = w;
    o->h = h;
    o->land = w > h;

    if (!o->land) {
        /* Upright, top to bottom: scoreboard 48..180, their hand, the two
         * rows of tricks inside the oval, the banner, the calls, your hand
         * down at the thumb. */
        o->k  = 2.f;
        o->sk = 2.f;
        tr_cards_set_scale(o->k);
        o->sc_x = 0;
        o->sc_y = EDGE_TOP;
        o->sc_w = w;
        o->sc_h = 132;
        int cw = TR_CARD_W, ch = TR_CARD_H;
        int mid = w / 2;
        o->opp_y = 200;
        for (int i = 0; i < 3; i++) {
            o->opp_x[i]  = mid + (i - 1) * 96 - cw / 2;
            o->hand_x[i] = mid + (i - 1) * 210 - cw / 2;
            o->mesa_x[i] = mid + (i - 1) * 204 - cw / 2;
        }
        o->deck_x    = w - PAD - cw - TR_PAD;   /* the pile spreads TR_PAD right */
        o->deck_y    = o->opp_y;
        o->mesa_op_y = 430;
        o->mesa_my_y = o->mesa_op_y + 92;
        o->mesa_dx   = 20;
        o->win_dy    = 12;
        o->oval_x = PAD;
        o->oval_y = o->mesa_op_y - 26;
        o->oval_w = w - 2 * PAD;
        o->oval_h = o->mesa_my_y + ch - o->oval_y + 22;
        o->ban_x = PAD;
        o->ban_y = o->oval_y + o->oval_h + 8;
        o->ban_w = w - 2 * PAD;
        o->ban_h = 72;
        o->hand_y = h - EDGE_BOT - ch;
        o->lift   = 18;
        o->btn_h  = 88;
        o->btn_cols = 3;
        o->btn_wmax = 216;
        o->bar_x  = PAD;
        o->bar_w  = w - 2 * PAD;
        o->bar_y1 = o->hand_y - o->lift - 14;
        o->tantos_x = o->oval_x + 28;
        o->tantos_y = o->oval_y + 4;
        o->wait_x = 0;
        o->wait_w = w;
        o->wait_y = o->bar_y1 - 48;
        o->menu_x = PAD;
        o->menu_y = o->opp_y + (ch - 88) / 2;
        o->menu_w = 104;
        o->menu_h = 88;
    } else {
        /* Lying down: a column on the left with the scoreboard, the banner,
         * the calls in rows of two and the menu at the bottom; the table on
         * the right with their hand along the top edge and yours along the
         * bottom one. */
        o->k  = 1.7f;
        o->sk = 1.7f;
        tr_cards_set_scale(o->k);
        int col = 404;                  /* the column's right edge */
        o->sc_x = 8;
        o->sc_y = 44;
        o->sc_w = col - 8;
        o->sc_h = 112;
        int cw = TR_CARD_W, ch = TR_CARD_H;
        int tx0 = col + 16, tx1 = w - PAD;
        int mid = (tx0 + tx1) / 2;
        o->opp_y = 40;
        for (int i = 0; i < 3; i++) {
            o->opp_x[i]  = mid + (i - 1) * 82 - cw / 2;
            o->hand_x[i] = mid + (i - 1) * 170 - cw / 2;
            o->mesa_x[i] = mid + (i - 1) * 196 - cw / 2;
        }
        o->deck_x    = tx1 - cw - TR_PAD;
        o->deck_y    = o->opp_y;
        o->mesa_op_y = 222;
        o->mesa_my_y = o->mesa_op_y + 78;
        o->mesa_dx   = 17;
        o->win_dy    = 10;
        o->oval_x = tx0;
        o->oval_y = o->mesa_op_y - 18;
        o->oval_w = tx1 - tx0;
        o->oval_h = o->mesa_my_y + ch - o->oval_y + 14;
        o->hand_y = h - EDGE_BOT - ch;
        o->lift   = 14;
        o->ban_x = PAD;
        o->ban_y = o->sc_y + o->sc_h + 12;
        o->ban_w = col - PAD;
        o->ban_h = 108;                 /* two lines: the column is narrow */
        o->btn_h  = 84;
        o->btn_cols = 2;
        o->btn_wmax = 200;
        o->bar_x  = PAD;
        o->bar_w  = col - PAD;
        o->menu_x = PAD;
        o->menu_w = 104;
        o->menu_h = 84;
        o->menu_y = h - EDGE_BOT - o->menu_h;
        o->bar_y1 = o->menu_y - 16;
        o->tantos_x = o->oval_x + 24;
        o->tantos_y = o->oval_y + 2;
        o->wait_x = PAD;
        o->wait_w = col - PAD;
        o->wait_y = o->ban_y + o->ban_h + 12;
    }
}

/* -------------------------------------------------------------------------- */
/* Positions                                                                   */

static void card_target(int who, int idx, int32_t *x, int32_t *y)
{
    const tr_state_t *g = &s_t.g;
    const geo_t *o = &s_t.geo;

    if (!g->spent[who][idx]) {
        *x = who == ME ? o->hand_x[idx] : o->opp_x[idx];
        *y = who == ME ? o->hand_y : o->opp_y;
        if (who == ME && s_t.sel == idx) *y -= o->lift;
        return;
    }

    /* Already played: we have to find which trick it fell in. */
    for (int t = 0; t < TR_HAND_N; t++) {
        if (g->table[who][t] == g->hand[who][idx]) {
            *x = o->mesa_x[t] + (who == ME ? o->mesa_dx : -o->mesa_dx);
            *y = who == ME ? o->mesa_my_y : o->mesa_op_y;
            /* the one that won the trick sits a little higher */
            if (g->trick_win[t] == who) *y -= o->win_dy;
            return;
        }
    }
    *x = o->deck_x;
    *y = o->deck_y;
}

static void place(int who, int idx, int32_t x, int32_t y)
{
    s_t.cx[who][idx] = x;
    s_t.cy[who][idx] = y;
    s_t.tx[who][idx] = x;
    s_t.ty[who][idx] = y;
    if (s_t.card[who][idx]) {
        lv_obj_set_pos(s_t.card[who][idx], x - TR_PAD, y - TR_PAD);
    }
}

static void retarget_all(void)
{
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            card_target(p, i, &s_t.tx[p][i], &s_t.ty[p][i]);
        }
    }
}

/* A third of what remains per frame, with a floor of one pixel so it finishes
 * arriving. The two axes are treated separately on purpose: with a step common
 * to both, a card that only descends (dx = 0) is left oscillating around its
 * destination and the game never advances, because everything else waits for
 * nothing to be moving.
 *
 * Without lv_anim, which would have to be cancelled in destroy(): the timer
 * already exists. */
static int32_t approach(int32_t cur, int32_t tgt)
{
    int32_t d = tgt - cur;
    if (d == 0) return cur;
    int32_t step = d / 3;
    if (step == 0) step = d > 0 ? 1 : -1;
    return cur + step;
}

static bool tween(void)
{
    bool moving = false;

    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            lv_obj_t *o = s_t.card[p][i];
            if (!o) continue;
            if (s_t.cx[p][i] == s_t.tx[p][i] && s_t.cy[p][i] == s_t.ty[p][i]) continue;
            s_t.cx[p][i] = approach(s_t.cx[p][i], s_t.tx[p][i]);
            s_t.cy[p][i] = approach(s_t.cy[p][i], s_t.ty[p][i]);
            lv_obj_set_pos(o, s_t.cx[p][i] - TR_PAD, s_t.cy[p][i] - TR_PAD);
            moving = true;
        }
    }
    return moving;
}

static void render(int who, int idx, int card)
{
    s_t.face[who][idx] = (int8_t)card;
    tr_card_render(s_t.card[who][idx], card);
}

/* -------------------------------------------------------------------------- */
/* Panel                                                                       */

static void banner(const char *text, int ms)
{
    if (!s_t.banner) return;
    /* lv_label_set_text() copies the string, so it may come from the stack. */
    snprintf(s_t.banner_txt, sizeof s_t.banner_txt, "%s", text);
    lv_label_set_text(s_t.banner_lbl, text);
    /* the long ones in the smaller type; lying down the column is narrow
     * and they wrap to two lines instead */
    lv_obj_set_style_text_font(s_t.banner_lbl,
                               strlen(text) > 24 && !s_t.geo.land ? &aos_inter_32 : &aos_inter_36, 0);
    lv_obj_remove_flag(s_t.banner, LV_OBJ_FLAG_HIDDEN);
    s_t.banner_until = ms ? now_ms() + (uint32_t)ms : 0;
}

static void banner_hide(void)
{
    if (s_t.banner) lv_obj_add_flag(s_t.banner, LV_OBJ_FLAG_HIDDEN);
    s_t.banner_txt[0] = 0;
    s_t.banner_until  = 0;
}

/* -------------------------------------------------------------------------- */
/* Button row                                                                  */

static const char *btn_text(tr_call_t c)
{
    switch (c) {
    case TR_C_ENVIDO:   return "ENVIDO";
    case TR_C_REAL:     return "REAL";
    case TR_C_FALTA:    return "FALTA";
    case TR_C_TRUCO:    return "TRUCO";
    case TR_C_RETRUCO:  return "RE TRUCO";
    case TR_C_VALE4:    return "VALE 4";
    case TR_C_QUIERO:   return "QUIERO";
    case TR_C_NOQUIERO: return "NO QUIERO";
    case TR_C_MAZO:     return "AL MAZO";
    default:            return "";
    }
}

/* Who is who on the banners: "VOS" for me, the other watch's name when there
 * is one, "ELLOS" for the machine. The scoreboard keeps NOS/ELLOS. */
static const char *who_name(int who)
{
    if (who == ME) return "VOS";
    return s_t.link && s_t.pname[0] ? s_t.pname : "ELLOS";
}

static const char *side_name(int who)
{
    return who == ME ? "NOS" : "ELLOS";
}

static void wait_show(bool on)
{
    s_t.wait_on = on;
    if (!s_t.wait_lbl) return;
    if (on) {
        static char t[48];
        lv_snprintf(t, sizeof(t), "ESPERANDO A %s", who_name(THEM));
        lv_label_set_text(s_t.wait_lbl, t);
        lv_obj_remove_flag(s_t.wait_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_t.wait_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void bar_hide(void)
{
    for (int i = 0; i < BTN_MAX; i++) {
        if (s_t.btn[i]) lv_obj_add_flag(s_t.btn[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_t.bar_n = 0;
    if (s_t.tantos) lv_obj_add_flag(s_t.tantos, LV_OBJ_FLAG_HIDDEN);
}

static void bar_show(const tr_call_t *opts, int n)
{
    const geo_t *o = &s_t.geo;
    if (n > BTN_MAX) n = BTN_MAX;

    /* Rows of btn_cols from the bottom up, against the bottom edge of the
     * bar: the first calls offered -QUIERO and NO QUIERO when answering-
     * are the ones nearest the thumb, and AL MAZO, always last, the
     * farthest. It never covers your own hand, which is the one thing that
     * cannot be covered. */
    int cols = o->btn_cols;
    for (int i = 0; i < BTN_MAX; i++) {
        if (i >= n) {
            lv_obj_add_flag(s_t.btn[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int row  = i / cols;                        /* 0 = the bottom one */
        int cnt  = n - row * cols < cols ? n - row * cols : cols;
        int k    = i - row * cols;
        int w    = (o->bar_w - BTN_GAP * (cnt - 1)) / cnt;
        if (w > o->btn_wmax) w = o->btn_wmax;
        int x0   = o->bar_x + (o->bar_w - (w * cnt + BTN_GAP * (cnt - 1))) / 2;
        int y    = o->bar_y1 - o->btn_h - row * (o->btn_h + BTN_GAP);

        lv_obj_set_size(s_t.btn[i], w, o->btn_h);
        lv_obj_set_pos(s_t.btn[i], x0 + k * (w + BTN_GAP), y);
        lv_label_set_text(s_t.btn_lbl[i], btn_text(opts[i]));
        s_t.btn_call[i] = opts[i];

        bool danger = opts[i] == TR_C_NOQUIERO || opts[i] == TR_C_MAZO;
        lv_obj_set_style_bg_color(s_t.btn[i],
                                  lv_color_hex(danger ? 0x53311E : 0x0E4A2A), 0);
        lv_obj_set_style_border_color(s_t.btn[i],
                                      lv_color_hex(danger ? 0xB07A45 : 0xC9A227), 0);
        lv_obj_remove_flag(s_t.btn[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_t.bar_n = n;
}

static void refresh_bar(void)
{
    const tr_state_t *g = &s_t.g;
    tr_call_t opts[BTN_MAX];
    int n = 0;

    if (g->turn != ME || s_t.over || s_t.auto_play || s_t.await_echo) {
        bar_hide();
        return;
    }

    if (g->phase == TR_P_ANSWER) {
        /* The call's panel is short-lived and the button row may appear
         * afterwards, so it is put back here WITHOUT a timeout: if something
         * has to be answered, it has to be written down what it is. */
        char q[48];
        lv_snprintf(q, sizeof(q), "%s: %s", who_name(THEM), tr_call_name(g->pending));
        banner(q, 0);

        opts[n++] = TR_C_QUIERO;
        opts[n++] = TR_C_NOQUIERO;
        static const tr_call_t UP[] = {
            TR_C_ENVIDO, TR_C_REAL, TR_C_FALTA,
            TR_C_TRUCO, TR_C_RETRUCO, TR_C_VALE4,
        };
        for (unsigned k = 0; k < sizeof(UP) / sizeof(UP[0]) && n < BTN_MAX; k++) {
            if (tr_can(g, ME, UP[k], 0)) opts[n++] = UP[k];
        }
    } else if (g->phase == TR_P_TURN) {
        static const tr_call_t CALLS[] = {
            TR_C_ENVIDO, TR_C_REAL, TR_C_FALTA,
            TR_C_TRUCO, TR_C_RETRUCO, TR_C_VALE4,
        };
        for (unsigned k = 0; k < sizeof(CALLS) / sizeof(CALLS[0]) && n < BTN_MAX - 1; k++) {
            if (tr_can(g, ME, CALLS[k], 0)) opts[n++] = CALLS[k];
        }
        opts[n++] = TR_C_MAZO;
    } else {
        bar_hide();
        return;
    }

    bar_show(opts, n);

    /* Your own points, while they can still be called: it is what you look
     * at. */
    if (s_t.tantos) {
        if (g->trick == 0 && !g->env_closed) {
            static char t[24];
            lv_snprintf(t, sizeof(t), "tengo %d", g->env_points[ME]);
            lv_label_set_text(s_t.tantos, t);
            lv_obj_remove_flag(s_t.tantos, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_t.tantos, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Scoreboard                                                                  */

static void score_refresh(void)
{
    tr_score_render(s_t.score_cv, s_t.geo.sk, s_t.g.score[ME], s_t.g.score[THEM],
                    s_t.g.mano == ME, tr_hand_value(&s_t.g));
}

static void save_prefs(void)
{
    /* The score that survives leaving is the game against the machine: a
     * game between two watches starts from zero every time. */
    if (!s_t.link) {
        aos_hal_pref_set_i32("truco_nos", s_t.g.score[ME]);
        aos_hal_pref_set_i32("truco_ellos", s_t.g.score[THEM]);
    }
    aos_hal_pref_set_i32("truco_won", s_t.won);
    aos_hal_pref_set_i32("truco_lost", s_t.lost);
}

/* -------------------------------------------------------------------------- */
/* Dealing                                                                     */

static void deal_start(void)
{
    const geo_t *o = &s_t.geo;
    s_t.sel = -1;
    s_t.gathered = false;
    bar_hide();
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            s_t.vis[p][i] = false;
            if (s_t.card[p][i]) lv_obj_add_flag(s_t.card[p][i], LV_OBJ_FLAG_HIDDEN);
            place(p, i, o->deck_x, o->deck_y);
        }
    }
    s_t.deal_i = 0;
    score_refresh();
}

static void deal_one(void)
{
    /* Dealt one at a time and alternating, starting with the one who is not
     * the mano. */
    int k   = s_t.deal_i;
    int who = (k % 2) ? s_t.g.mano : (s_t.g.mano ^ 1);
    int idx = k / 2;

    bool face = (who == ME) || s_t.showall;
    render(who, idx, face ? (int)s_t.g.hand[who][idx] : -1);
    s_t.vis[who][idx] = true;
    lv_obj_remove_flag(s_t.card[who][idx], LV_OBJ_FLAG_HIDDEN);
    card_target(who, idx, &s_t.tx[who][idx], &s_t.ty[who][idx]);

    aos_hal_beep(520 + idx * 40, 12);
    s_t.deal_i++;
    s_t.wait_until = now_ms() + (uint32_t)delay(95);
}

/* -------------------------------------------------------------------------- */
/* Presenting the game's events                                                */

static void present(const tr_ev_t *ev)
{
    char buf[64];

    switch (ev->kind) {
    case TR_EV_DEAL:
        deal_start();
        break;

    case TR_EV_SAY:
        lv_snprintf(buf, sizeof(buf), "%s: %s", who_name(ev->who),
                    tr_call_name((tr_call_t)ev->a));
        banner(buf, delay(1100));
        aos_hal_beep(ev->who == ME ? 700 : 480, 45);
        s_t.wait_until = now_ms() + (uint32_t)delay(750);
        score_refresh();
        break;

    case TR_EV_PLAY:
        if (ev->who == THEM && !s_t.showall) {
            render(THEM, ev->a, (int)ev->b);
        }
        retarget_all();
        aos_hal_beep(300, 18);
        s_t.wait_until = now_ms() + (uint32_t)delay(260);
        break;

    case TR_EV_TRICK:
        retarget_all();
        s_t.wait_until = now_ms() + (uint32_t)delay(ev->n == 2 ? 250 : 420);
        break;

    case TR_EV_ENVIDO:
        if (ev->a < 0) {
            lv_snprintf(buf, sizeof(buf), "ENVIDO NO QUERIDO: %d PARA %s",
                        ev->n, side_name(ev->who));
        } else {
            lv_snprintf(buf, sizeof(buf), "%d A %d: %d PARA %s",
                        ev->a, ev->b, ev->n, side_name(ev->who));
        }
        banner(buf, delay(1900));
        score_refresh();
        aos_hal_beep(ev->who == ME ? 880 : 330, 90);
        s_t.wait_until = now_ms() + (uint32_t)delay(1500);
        break;

    case TR_EV_HAND:
        lv_snprintf(buf, sizeof(buf), "LA MANO ES DE %s  (+%d)",
                    side_name(ev->who), ev->n);
        banner(buf, delay(1900));
        score_refresh();
        aos_hal_beep(ev->who == ME ? 780 : 260, 120);
        /* the cards are gathered up */
        s_t.gathered = true;
        for (int p = 0; p < 2; p++) {
            for (int i = 0; i < TR_HAND_N; i++) {
                s_t.tx[p][i] = s_t.geo.deck_x;
                s_t.ty[p][i] = s_t.geo.deck_y;
            }
        }
        s_t.wait_until = now_ms() + (uint32_t)delay(1600);
        break;

    case TR_EV_GAME:
        lv_snprintf(buf, sizeof(buf), "%s LA PARTIDA  %d-%d",
                    ev->who == ME ? "GANASTE" : "PERDISTE",
                    s_t.g.score[ME], s_t.g.score[THEM]);
        banner(buf, 0);
        if (ev->who == ME) s_t.won++; else s_t.lost++;
        s_t.over = true;
        bar_hide();
        wait_show(false);
        aos_hal_beep(ev->who == ME ? 990 : 200, 250);
        s_t.g.score[0] = 0;
        s_t.g.score[1] = 0;
        save_prefs();
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */

/* ---- two watches: the wire -------------------------------------------------
 *
 * Host-ordered lockstep. The engine is deterministic given its seed, so both
 * watches run the same tr_game.c: the host picks the seed, every move goes
 * through the host, and the host echoes the moves in the order it applied
 * them. The guest applies nothing on its own, not even its own taps: it sends
 * them and waits for the echo. Moves that arrive wait in the inbox until the
 * table is quiet (nothing moving, no event pending), exactly where the
 * machine would have thought its move, so the animations keep their pace on
 * both screens even when one of them is behind. */
static bool inbox_push(const tl_act_t *a)
{
    uint8_t next = (uint8_t)((s_t.in_head + 1) % TL_INBOX);
    if (next == s_t.in_tail) return false;
    s_t.inbox[s_t.in_head] = *a;
    s_t.in_head = next;
    return true;
}

static bool inbox_pop(tl_act_t *a)
{
    if (s_t.in_head == s_t.in_tail) return false;
    *a = s_t.inbox[s_t.in_tail];
    s_t.in_tail = (uint8_t)((s_t.in_tail + 1) % TL_INBOX);
    return true;
}

static void send_act(int who, tr_call_t c, int arg)
{
    tl_act_t a = { .type = TL_ACT, .proto = TL_PROTO,
                   .who = (uint8_t)who, .call = (uint8_t)c, .arg = (int8_t)arg };
    if (!aos_hal_link_send_reliable(&a, sizeof a)) {
        aos_hal_log("truco", "act %d/%d not sent", who, (int)c);
    }
}

static void send_hello(void)
{
    aos_link_stats_t st;
    memset(&st, 0, sizeof st);
    aos_hal_link_stats(&st);
    tl_hello_t h = { .type = TL_HELLO, .proto = TL_PROTO,
                     .seed = s_t.my_seed, .nonce = s_t.nonce };
    memcpy(h.mac, st.own_mac, 6);
    aos_hal_link_send_reliable(&h, sizeof h);
    s_t.hello_ms = now_ms();
}

static void link_lost(const char *why)
{
    if (s_t.lk == LK_LOST) return;
    aos_hal_log("truco", "link lost: %s", why);
    s_t.lk = LK_LOST;
    s_t.over = false;
    bar_hide();
    wait_show(false);
    banner(why, 0);
    aos_hal_beep(200, 200);
}

static void on_hello(const tl_hello_t *h)
{
    if (s_t.lk == LK_PLAY && h->nonce == s_t.their_nonce) return;   /* a repeat */
    aos_link_stats_t st;
    memset(&st, 0, sizeof st);
    aos_hal_link_stats(&st);
    s_t.their_nonce = h->nonce;
    s_t.is_host = memcmp(st.own_mac, h->mac, 6) < 0;
    s_t.me = s_t.is_host ? 0 : 1;
    if (s_t.lk == LK_PLAY) {
        aos_hal_log("truco", "partner restarted");   /* a new game, like them */
    }
    s_t.lk = LK_PLAY;
    send_hello();                       /* so the other side decides too */
    aos_hal_log("truco", "role: %s, seat %d", s_t.is_host ? "host" : "guest", s_t.me);
    game_begin(s_t.is_host ? s_t.my_seed : h->seed);
}

static void link_tick(void)
{
    aos_link_frame_t f;
    while (aos_hal_link_recv_reliable(&f) > 0) {
        if (f.len < 2 || f.data[1] != TL_PROTO) continue;
        if (f.data[0] == TL_HELLO && f.len >= sizeof(tl_hello_t)) {
            on_hello((const tl_hello_t *)f.data);
        } else if (f.data[0] == TL_ACT && f.len >= sizeof(tl_act_t)) {
            if (s_t.lk == LK_PLAY && !inbox_push((const tl_act_t *)f.data)) {
                link_lost("SE DESINCRONIZO");
            }
        }
    }
    if (s_t.lk == LK_HELLO) {
        aos_link_partner_t p;
        if (!aos_hal_link_partner(&p) || !p.valid) return;
        if (!s_t.pname[0]) {
            snprintf(s_t.pname, sizeof s_t.pname, "%s", p.name);
            for (char *c = s_t.pname; *c; c++) {
                if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
            }
        }
        if (aos_hal_link_reliable_lost()) aos_hal_link_reliable_reset();
        if (now_ms() - s_t.hello_ms >= 500) send_hello();
        return;
    }
    if (s_t.lk != LK_PLAY) return;
    if (aos_hal_link_reliable_lost()) {
        link_lost("SE PERDIO EL ENLACE");
        return;
    }
    /* The partner stopped beaconing (left the link) or offers another app
     * (left Truco): otherwise we would wait for a move for ever. */
    aos_link_partner_t p;
    uint32_t now = now_ms();
    bool gone = aos_hal_link_partner(&p) && p.valid && !p.seen;
    if (!gone) {
        aos_link_neighbour_t nb[AOS_LINK_NEIGHBOURS];
        int n = aos_hal_link_neighbours(nb, AOS_LINK_NEIGHBOURS);
        for (int i = 0; i < n; i++) {
            if (memcmp(nb[i].mac, p.mac, 6) == 0 && nb[i].app[0] && strcmp(nb[i].app, "truco") != 0) {
                gone = true;
            }
        }
    }
    if (!gone) {
        s_t.unseen_ms = now;
    } else if (now - s_t.unseen_ms > 6000) {
        char buf[48];
        lv_snprintf(buf, sizeof buf, "SE FUE %s", who_name(THEM));
        link_lost(buf);
    }
    if (s_t.await_echo && now - s_t.echo_ms > 4000) {
        s_t.await_echo = false;         /* the host did not take it: the bar comes back */
    }
}

/* One move that came over the link, once the table is quiet. Returns true if
 * something was applied (the caller then waits before the next). */
static bool link_apply_one(void)
{
    tl_act_t a;
    if (!inbox_pop(&a)) return false;
    if (s_t.is_host && a.who != THEM) {
        return false;                   /* the guest may only move for itself */
    }
    if (!tr_apply(&s_t.g, a.who, (tr_call_t)a.call, a.arg)) {
        if (s_t.is_host) {
            aos_hal_log("truco", "guest move refused: %d/%d", (int)a.call, (int)a.arg);
            return false;               /* not legal here: it is simply not echoed */
        }
        link_lost("SE DESINCRONIZO");   /* the host's stream must always apply */
        return false;
    }
    if (s_t.is_host) send_act(a.who, (tr_call_t)a.call, a.arg);
    if (a.who == ME) s_t.await_echo = false;
    s_t.sel = -1;
    bar_hide();
    wait_show(false);
    banner_hide();
    retarget_all();
    s_t.wait_until = now_ms() + (uint32_t)delay(a.who == ME ? 0 : 320);
    return true;
}

static void do_call(tr_call_t c, int arg)
{
    if (s_t.link) {
        if (s_t.lk != LK_PLAY || s_t.await_echo) return;
        if (!s_t.is_host) {
            /* the guest proposes; the move happens when the host echoes it */
            if (!tr_can(&s_t.g, ME, c, arg)) return;
            send_act(ME, c, arg);
            s_t.await_echo = true;
            s_t.echo_ms = now_ms();
            s_t.sel = -1;
            bar_hide();
            return;
        }
    }
    if (!tr_apply(&s_t.g, ME, c, arg)) return;
    if (s_t.link) send_act(ME, c, arg);
    s_t.sel = -1;
    bar_hide();
    banner_hide();
    retarget_all();
}

static void btn_cb(lv_event_t *e)
{
    if (s_t.closing) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_t.bar_n) return;
    do_call(s_t.btn_call[i], 0);
}

/* The game ended: the next touch starts another. */
static void continue_after_end(void)
{
    s_t.over = false;
    banner_hide();
    tr_new_hand(&s_t.g);
}

static void card_cb(lv_event_t *e)
{
    if (s_t.closing || s_t.auto_play) return;

    int i = (int)(intptr_t)lv_event_get_user_data(e);
    tr_state_t *g = &s_t.g;

    /* A click on a card does not reach the root, so the "touch to carry on" at
     * the end of a game has to be handled here as well. */
    if (s_t.lk == LK_LOST) { s_t.want_exit = true; return; }
    if (s_t.over) { continue_after_end(); return; }
    if (s_t.link && (s_t.lk != LK_PLAY || s_t.await_echo)) return;
    if (g->phase != TR_P_TURN || g->turn != ME) return;
    if (!tr_can(g, ME, TR_C_PLAY, i)) return;

    /* Two taps: the first lifts the card, the second plays it. The cards are
     * big enough here to hit, but a card played cannot be taken back, and a
     * stray tap on the wrong one costs the hand. */
    if (s_t.sel != i) {
        s_t.sel = i;
        retarget_all();
        aos_hal_beep(620, 10);
        return;
    }
    do_call(TR_C_PLAY, i);
}

static void root_cb(lv_event_t *e)
{
    (void)e;
    if (s_t.closing) return;
    if (s_t.lk == LK_LOST) { s_t.want_exit = true; return; }

    if (s_t.over) {
        continue_after_end();
        return;
    }
    /* touching the baize deselects */
    if (s_t.sel >= 0) {
        s_t.sel = -1;
        retarget_all();
    }
}

/* -------------------------------------------------------------------------- */
/* The gamepad                                                                 */

/* Takes the outline off everything but the selected one: aos_pad_menu skips
 * a button that was hidden while selected (a call once answered) and its
 * outline would come back with it the next time the bar shows. */
static void pad_unmark(lv_obj_t *o, const lv_obj_t *sel)
{
    if (o && o != sel && lv_obj_get_style_outline_width(o, 0) != 0) {
        lv_obj_set_style_outline_width(o, 0, 0);
        lv_obj_set_style_outline_pad(o, 0, 0);
    }
}

/* What the d-pad walks right now: the menu's buttons, the chooser, or your
 * cards still in hand and the calls showing. Handed to
 * pmenu only when it changed, keeping the selection where it was. */
static void pad_items(void)
{
    lv_obj_t *it[TR_HAND_N + BTN_MAX];
    int n = 0, def = 0;
    if (s_t.menu_open) {
        it[n++] = s_t.menu_resume;
        it[n++] = s_t.menu_new;
        it[n++] = s_t.menu_duo;
        it[n++] = s_t.menu_exit;
    } else if (s_t.lk == LK_CHOOSE) {
        for (int k = 0; k < 2; k++) {
            if (s_t.choose[k]) it[n++] = s_t.choose[k];
        }
    } else {
        for (int i = 0; i < TR_HAND_N; i++) {
            if (s_t.vis[ME][i] && !s_t.g.spent[ME][i] && s_t.card[ME][i]) {
                it[n++] = s_t.card[ME][i];
            }
        }
        /* answering a call, the outline starts on QUIERO: no card can be
         * played until it is answered */
        if (s_t.g.phase == TR_P_ANSWER && s_t.bar_n > 0) def = -n;
        for (int i = 0; i < s_t.bar_n; i++) {
            it[n++] = s_t.btn[i];
        }
        /* not the menu button: START is the menu, and between two hands it
         * would be all there is, for an A meant for the next card */
    }

    bool same = n == s_t.pmenu.n;
    for (int i = 0; same && i < n; i++) {
        same = it[i] == s_t.pmenu.item[i];
    }
    if (!same) {
        /* until the pad is used, the outline (unseen) stays on the default */
        lv_obj_t *was = s_t.pmenu.shown ? aos_pad_menu_selected(&s_t.pmenu) : NULL;
        bool answer = def < 0;
        int sel = answer ? -def : def;
        for (int i = answer ? sel : 0; i < n; i++) {
            if (it[i] == was) sel = i;
        }
        aos_pad_menu_set(&s_t.pmenu, it, n, sel);
    }

    if (!s_t.pmenu.shown) return;
    const lv_obj_t *sel = s_t.pmenu.sel < s_t.pmenu.n ? s_t.pmenu.item[s_t.pmenu.sel] : NULL;
    for (int p = 0; p < TR_HAND_N; p++) pad_unmark(s_t.card[ME][p], sel);
    for (int i = 0; i < BTN_MAX; i++) pad_unmark(s_t.btn[i], sel);
}

static void pad_step(void)
{
    aos_pad_update(&s_t.pad, lv_tick_get());
    pad_items();
    if (!s_t.pad.pressed && !s_t.pad.repeat) return;

    bool start = aos_pad_pressed(&s_t.pad, AOS_PAD_START);
    bool b     = aos_pad_pressed(&s_t.pad, AOS_PAD_B);
    if (s_t.menu_open) {
        if (start || b) menu_show(false);
        else aos_pad_menu_step(&s_t.pmenu, &s_t.pad);
        return;
    }
    /* the end of a game, or of the link: a touch anywhere, which is A here */
    if ((s_t.over || s_t.lk == LK_LOST) &&
        aos_pad_pressed(&s_t.pad, AOS_PAD_A | AOS_PAD_START)) {
        root_cb(NULL);
        return;
    }
    if (start) {
        if (s_t.lk != LK_CHOOSE) menu_show(true);
        return;
    }
    if (b) {
        /* B puts the lifted card back, as touching the baize does */
        if (s_t.sel >= 0) {
            s_t.sel = -1;
            retarget_all();
        }
        return;
    }
    aos_pad_menu_step(&s_t.pmenu, &s_t.pad);
}

/* -------------------------------------------------------------------------- */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_t.closing) return;
    if (s_t.want_exit) {
        /* aos_ui_back() destroys the app: nothing of s_t after this */
        s_t.want_exit = false;
        s_t.menu_open = false;
        aos_ui_back();
        return;
    }
    pad_step();
    if (s_t.want_exit) return;          /* SALIR from the pad: the next tick */
    if (s_t.lk == LK_CHOOSE) return;
    if (s_t.link) link_tick();
    if (s_t.lk == LK_HELLO || s_t.lk == LK_LOST) return;

    bool moving = tween();

    if (s_t.banner_until && elapsed(s_t.banner_until)) banner_hide();

    if (s_t.menu_open) return;          /* the table waits while it is open */

    if (s_t.deal_i >= 0) {
        if (s_t.deal_i < TR_HAND_N * 2) {
            if (elapsed(s_t.wait_until)) deal_one();
        } else if (!moving) {
            s_t.deal_i = -1;
            s_t.wait_until = now_ms() + (uint32_t)delay(200);
        }
        return;
    }

    if (moving || !elapsed(s_t.wait_until)) return;

    tr_ev_t ev;
    if (tr_pop_event(&s_t.g, &ev)) {
        present(&ev);
        return;
    }

    if (s_t.over) {
        /* In automatic mode there is nobody to touch the screen: another game
         * starts by itself, which is what makes leaving it running useful. */
        if (s_t.auto_play) continue_after_end();
        return;
    }

    if (s_t.g.phase == TR_P_HAND_END) {
        tr_new_hand(&s_t.g);
        return;
    }
    if (s_t.g.phase == TR_P_GAME_END) {
        return;                     /* the TR_EV_GAME event already went through present() */
    }

    int turn = s_t.g.turn;
    if (s_t.link) {
        if (link_apply_one()) return;
        if (turn == ME && !s_t.await_echo) {
            wait_show(false);
            if (s_t.bar_n == 0) refresh_bar();
        } else {
            wait_show(true);
        }
        return;
    }
    if (turn == THEM || s_t.auto_play) {
        int arg = 0;
        tr_call_t c = tr_ai_decide(&s_t.g, turn, &arg);
        if (c == TR_C_NONE) return;
        bar_hide();
        tr_apply(&s_t.g, turn, c, arg);
        retarget_all();
        s_t.wait_until = now_ms() + (uint32_t)delay(320);
        return;
    }

    if (s_t.bar_n == 0) refresh_bar();
}

/* -------------------------------------------------------------------------- */
/* The menu                                                                    */

static void menu_refresh(void)
{
    if (!s_t.menu) return;
    char buf[96];
    lv_snprintf(buf, sizeof buf, "NOS %d  -  ELLOS %d\nGANADAS %ld  -  PERDIDAS %ld",
                s_t.g.score[ME], s_t.g.score[THEM], (long)s_t.won, (long)s_t.lost);
    lv_label_set_text(s_t.menu_stats, buf);

    /* Two players: against the machine again when playing two-handed; the
     * partner's name when the link is there and somebody is paired; and
     * otherwise greyed, with the reason under it. */
    const char *note = "";
    bool        on   = true;
    if (s_t.link) {
        lv_label_set_text(s_t.menu_duo_lbl, "CONTRA LA MAQUINA");
    } else if (s_t.link_avail && s_t.have_partner) {
        lv_snprintf(buf, sizeof buf, "A DOS: CONTRA %s", s_t.pname);
        lv_label_set_text(s_t.menu_duo_lbl, buf);
    } else {
        lv_label_set_text(s_t.menu_duo_lbl, "A DOS JUGADORES");
        on   = false;
        note = s_t.link_avail
               ? "Para jugar de a dos, emparejá los dos equipos en Enlace."
               : "Este equipo todavía no tiene el enlace por radio: por ahora se juega contra la máquina.";
    }
    if (on) lv_obj_remove_state(s_t.menu_duo, LV_STATE_DISABLED);
    else    lv_obj_add_state(s_t.menu_duo, LV_STATE_DISABLED);
    lv_label_set_text(s_t.menu_note, note);

    /* a new game only against the machine: between two, the host decides */
    if (s_t.link) lv_obj_add_state(s_t.menu_new, LV_STATE_DISABLED);
    else          lv_obj_remove_state(s_t.menu_new, LV_STATE_DISABLED);
}

static void menu_show(bool on)
{
    s_t.menu_open = on;
    if (!s_t.menu) return;
    if (on) {
        menu_refresh();
        lv_obj_remove_flag(s_t.menu, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_t.menu, LV_OBJ_FLAG_HIDDEN);
    }
}

static void menu_open_cb(lv_event_t *e)   { (void)e; if (!s_t.closing) menu_show(true); }
static void menu_resume_cb(lv_event_t *e) { (void)e; if (!s_t.closing) menu_show(false); }

static void menu_exit_cb(lv_event_t *e)
{
    (void)e;
    if (!s_t.closing) s_t.want_exit = true;     /* from the timer: it destroys the app */
}

static void menu_new_cb(lv_event_t *e)
{
    (void)e;
    if (s_t.closing || s_t.link) return;
    menu_show(false);
    s_t.g.score[0] = s_t.g.score[1] = 0;
    s_t.my_seed = s_t.my_seed * 2654435761u + 12345u;
    game_begin(s_t.my_seed);
    save_prefs();
}

static void menu_duo_cb(lv_event_t *e)
{
    (void)e;
    if (s_t.closing) return;
    if (s_t.link) {
        link_end();
        menu_show(false);
        solo_begin();
    } else if (s_t.link_avail && s_t.have_partner) {
        menu_show(false);
        save_prefs();                   /* the game against the machine waits */
        link_begin();
    }
}

static lv_obj_t *menu_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                             uint32_t fill, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xF3EBD2), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xC9A227), 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &aos_inter_32, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xF3EBD2), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x0E2418), LV_STATE_PRESSED);
    lv_obj_set_width(l, w - 24);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void build_menu(lv_obj_t *root)
{
    const geo_t *o = &s_t.geo;
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, o->w, o->h);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x03120A), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);          /* touches do not pass */
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    s_t.menu = p;

    /* A card: one column upright; lying down the title and the numbers on
     * the left and the buttons on the right. */
    int cw = o->land ? 1040 : 624, chh = o->land ? 580 : 840;
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, cw, chh);
    lv_obj_center(c);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x0B3A20), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(0x072A16), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 36, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0xC9A227), 0);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    const int in = 48;
    int lw = o->land ? cw / 2 - in - in / 2 : cw - 2 * in;
    int rx = o->land ? cw / 2 + in / 2 : in;
    int rw = o->land ? cw - rx - in : lw;

    lv_obj_t *t = lv_label_create(c);
    lv_label_set_text(t, "TRUCO");
    lv_obj_set_style_text_font(t, &aos_inter_64, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xF3D874), 0);
    lv_obj_set_style_text_letter_space(t, 8, 0);
    lv_obj_set_width(t, lw);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, in, 44);

    s_t.menu_stats = lv_label_create(c);
    lv_obj_set_style_text_font(s_t.menu_stats, &aos_inter_28, 0);
    lv_obj_set_style_text_color(s_t.menu_stats, lv_color_hex(0xE9E4D2), 0);
    lv_obj_set_style_text_line_space(s_t.menu_stats, 12, 0);
    lv_obj_set_width(s_t.menu_stats, lw);
    lv_obj_set_style_text_align(s_t.menu_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_t.menu_stats, in, 148);

    const int bh = 96, gap = 18;
    int y = o->land ? (chh - (4 * bh + 3 * gap)) / 2 : 272;
    s_t.menu_resume = menu_button(c, "SEGUIR", rx, y, rw, bh, 0x0E4A2A, menu_resume_cb, NULL);
    y += bh + gap;
    s_t.menu_new = menu_button(c, "PARTIDA NUEVA", rx, y, rw, bh, 0x0E4A2A, menu_new_cb, NULL);
    y += bh + gap;
    s_t.menu_duo = menu_button(c, "A DOS JUGADORES", rx, y, rw, bh, 0x16365E, menu_duo_cb,
                               &s_t.menu_duo_lbl);
    y += bh + gap;
    s_t.menu_exit = menu_button(c, "SALIR", rx, y, rw, bh, 0x53311E, menu_exit_cb, NULL);
    y += bh + 28;

    s_t.menu_note = lv_label_create(c);
    lv_label_set_text(s_t.menu_note, "");
    lv_obj_set_style_text_font(s_t.menu_note, &aos_inter_24, 0);
    lv_obj_set_style_text_color(s_t.menu_note, lv_color_hex(0x9FD3AE), 0);
    lv_obj_set_width(s_t.menu_note, o->land ? lw : rw);
    lv_label_set_long_mode(s_t.menu_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_t.menu_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_t.menu_note, o->land ? in : rx, o->land ? 300 : y);
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */

static lv_obj_t *make_button(lv_obj_t *parent, int i)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0E4A2A), 0);
    lv_obj_set_style_bg_opa(b, 235, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xC9A227), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xC9A227), 0);
    lv_obj_set_style_border_opa(b, 200, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    s_t.btn_lbl[i] = lv_label_create(b);
    lv_obj_set_style_text_font(s_t.btn_lbl[i], &aos_inter_28, 0);
    lv_obj_set_style_text_color(s_t.btn_lbl[i], lv_color_hex(0xF3EBD2), 0);
    lv_obj_center(s_t.btn_lbl[i]);
    lv_obj_remove_flag(s_t.btn_lbl[i], LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* The chooser at the start, when somebody is paired: against the machine,
 * or against them. */
static void build_chooser(lv_obj_t *root)
{
    const geo_t *o = &s_t.geo;
    int w = o->land ? 560 : 560, h = 104;
    int x = o->land ? o->oval_x + (o->oval_w - w) / 2 : (o->w - w) / 2;
    int y = o->oval_y + o->oval_h / 2 - h - 12;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = make_button(root, BTN_MAX + i);   /* out of the bar: its own callback */
        lv_obj_remove_event_cb(b, btn_cb);
        lv_obj_add_event_cb(b, choose_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_size(b, w, h);
        lv_obj_set_pos(b, x, y + i * (h + 24));
        lv_obj_set_style_text_font(s_t.btn_lbl[BTN_MAX + i], &aos_inter_36, 0);
        lv_label_set_text(s_t.btn_lbl[BTN_MAX + i], i == 0 ? "LA MAQUINA" : s_t.pname);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_t.choose[i] = b;
    }
}

static void view_free(void)
{
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            free(s_t.card_buf[p][i]);
            s_t.card_buf[p][i] = NULL;
            s_t.card[p][i] = NULL;
        }
    }
    free(s_t.deck_buf);
    free(s_t.score_buf);
    s_t.deck_buf = s_t.score_buf = NULL;
    s_t.deck = s_t.score_cv = s_t.oval = s_t.banner = s_t.banner_lbl = NULL;
    for (int i = 0; i < BTN_MAX + 2; i++) {
        if (i < BTN_MAX) s_t.btn[i] = NULL;
        s_t.btn_lbl[i] = NULL;
    }
    s_t.choose[0] = s_t.choose[1] = NULL;
    s_t.tantos = s_t.wait_lbl = s_t.menu = s_t.menu_btn = NULL;
    s_t.menu_stats = s_t.menu_duo = s_t.menu_duo_lbl = s_t.menu_note = s_t.menu_new = NULL;
}

/* The six cards' owners are only known once the seat is: mine get the tap
 * and sit on top of the other's when they overlap on the table. */
static void cards_wire(void)
{
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            lv_obj_t *c = s_t.card[p][i];
            lv_obj_remove_event_cb(c, card_cb);
            if (p == ME) {
                lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(c, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            } else {
                lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
    /* the cards were created 0 then 1; if I am 1 mine are already on top */
    if (ME == 0) {
        for (int i = 0; i < TR_HAND_N; i++) {
            if (lv_obj_get_index(s_t.card[0][i]) < lv_obj_get_index(s_t.card[1][i])) {
                lv_obj_swap(s_t.card[0][i], s_t.card[1][i]);
            }
        }
    }
}

static bool view_build(lv_obj_t *root)
{
    lv_obj_update_layout(root);
    int w = (int)lv_obj_get_width(root), h = (int)lv_obj_get_height(root);
    if (w <= 0 || h <= 0) {
        int32_t aw, ah;
        aos_ui_app_area(&aw, &ah);
        w = (int)aw;
        h = (int)ah;
    }
    geo_compute(&s_t.geo, w, h);
    const geo_t *o = &s_t.geo;

    /* The baize: a vertical gradient and a lighter oval where the cards fall.
     * All with styles, no canvas: it is a still background and it is not worth
     * spending a full-screen buffer on it. */
    /* Beware lv_obj_remove_style_all(root): in LVGL 9 the width and the height
     * ARE local style properties, so deleting them leaves the root at content
     * size and the table appears clipped to a small rectangle in the top left,
     * with no error at all. The runtime has already cleaned it before calling
     * us: here it only has to be painted. */
    lv_obj_set_style_bg_color(root, lv_color_hex(0x14522F), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(0x072A16), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(root, root_cb);
    lv_obj_add_event_cb(root, root_cb, LV_EVENT_CLICKED, NULL);

    s_t.oval = lv_obj_create(root);
    lv_obj_remove_style_all(s_t.oval);
    lv_obj_set_size(s_t.oval, o->oval_w, o->oval_h);
    lv_obj_set_pos(s_t.oval, o->oval_x, o->oval_y);
    lv_obj_set_style_radius(s_t.oval, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_t.oval, lv_color_hex(0x1C7A44), 0);
    lv_obj_set_style_bg_opa(s_t.oval, 50, 0);
    lv_obj_set_style_border_width(s_t.oval, 3, 0);
    lv_obj_set_style_border_color(s_t.oval, lv_color_hex(0xC9A227), 0);
    lv_obj_set_style_border_opa(s_t.oval, 55, 0);
    aos_make_decorative(s_t.oval);

    /* Scoreboard */
    s_t.score_buf = malloc((size_t)o->sc_w * o->sc_h * 4);
    if (!s_t.score_buf) return false;
    s_t.score_cv = lv_canvas_create(root);
    lv_canvas_set_buffer(s_t.score_cv, s_t.score_buf, o->sc_w, o->sc_h,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_size(s_t.score_cv, o->sc_w, o->sc_h);
    lv_obj_set_pos(s_t.score_cv, o->sc_x, o->sc_y);
    lv_obj_set_style_radius(s_t.score_cv, 0, 0);
    lv_image_set_antialias(s_t.score_cv, false);
    lv_obj_remove_flag(s_t.score_cv, LV_OBJ_FLAG_CLICKABLE);

    /* The six cards. The opponent's first so that yours end up on top when
     * they overlap on the table. */
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            s_t.card[p][i] = tr_card_canvas(root, &s_t.card_buf[p][i]);
            if (!s_t.card[p][i]) return false;
            lv_obj_add_flag(s_t.card[p][i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* The deck, for show: it is where the cards come from and go back to.
     * Three backs, a little apart, so it reads as a pile and not as a
     * fourth card of theirs, next to which it sits; the two below show the
     * top one's buffer, drawn once. */
    s_t.deck = tr_card_canvas(root, &s_t.deck_buf);
    if (!s_t.deck) return false;
    tr_card_render(s_t.deck, -1);
    int step = TR_PAD / 2;
    for (int j = 2; j >= 1; j--) {
        lv_obj_t *u = lv_canvas_create(root);
        lv_canvas_set_buffer(u, s_t.deck_buf, TR_CV_W, TR_CV_H, LV_COLOR_FORMAT_ARGB8888);
        lv_obj_set_size(u, TR_CV_W, TR_CV_H);
        lv_obj_set_style_radius(u, 0, 0);
        lv_obj_remove_flag(u, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(u, o->deck_x - TR_PAD + j * step, o->deck_y - TR_PAD + j * step);
        lv_obj_move_to_index(u, lv_obj_get_index(s_t.card[0][0]));
    }
    lv_obj_set_pos(s_t.deck, o->deck_x - TR_PAD, o->deck_y - TR_PAD);
    /* under the cards: they fly out of it and back into it */
    lv_obj_move_to_index(s_t.deck, lv_obj_get_index(s_t.card[0][0]));

    /* Panel */
    s_t.banner = lv_obj_create(root);
    lv_obj_remove_style_all(s_t.banner);
    lv_obj_set_size(s_t.banner, o->ban_w, o->ban_h);
    lv_obj_set_pos(s_t.banner, o->ban_x, o->ban_y);
    lv_obj_set_style_bg_color(s_t.banner, lv_color_hex(0x08240F), 0);
    lv_obj_set_style_bg_opa(s_t.banner, 225, 0);
    lv_obj_set_style_radius(s_t.banner, 22, 0);
    lv_obj_set_style_border_width(s_t.banner, 3, 0);
    lv_obj_set_style_border_color(s_t.banner, lv_color_hex(0xC9A227), 0);
    lv_obj_add_flag(s_t.banner, LV_OBJ_FLAG_HIDDEN);
    s_t.banner_lbl = lv_label_create(s_t.banner);
    lv_obj_set_style_text_font(s_t.banner_lbl, &aos_inter_36, 0);
    lv_obj_set_style_text_color(s_t.banner_lbl, lv_color_hex(0xF6EFD8), 0);
    lv_obj_set_width(s_t.banner_lbl, o->ban_w - 32);
    lv_obj_set_style_text_align(s_t.banner_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_t.banner_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_center(s_t.banner_lbl);
    aos_make_decorative(s_t.banner);

    /* Button row */
    for (int i = 0; i < BTN_MAX; i++) s_t.btn[i] = make_button(root, i);

    s_t.tantos = lv_label_create(root);
    lv_obj_set_style_text_font(s_t.tantos, &aos_inter_28, 0);
    lv_obj_set_style_text_color(s_t.tantos, lv_color_hex(0x9FD3AE), 0);
    lv_obj_set_pos(s_t.tantos, o->tantos_x, o->tantos_y);
    lv_obj_add_flag(s_t.tantos, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_t.tantos, LV_OBJ_FLAG_CLICKABLE);

    s_t.wait_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(s_t.wait_lbl, &aos_inter_28, 0);
    lv_obj_set_style_text_color(s_t.wait_lbl, lv_color_hex(0x9FD3AE), 0);
    lv_obj_set_width(s_t.wait_lbl, o->wait_w);
    lv_obj_set_style_text_align(s_t.wait_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_t.wait_lbl, o->wait_x, o->wait_y);
    lv_obj_add_flag(s_t.wait_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_t.wait_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* The menu's button: round, like the rest of the system's */
    s_t.menu_btn = lv_obj_create(root);
    lv_obj_t *mb = s_t.menu_btn;
    lv_obj_remove_style_all(mb);
    lv_obj_set_size(mb, o->menu_w, o->menu_h);
    lv_obj_set_pos(mb, o->menu_x, o->menu_y);
    lv_obj_set_style_bg_color(mb, lv_color_hex(0x08240F), 0);
    lv_obj_set_style_bg_opa(mb, 225, 0);
    lv_obj_set_style_bg_color(mb, lv_color_hex(0xC9A227), LV_STATE_PRESSED);
    lv_obj_set_style_radius(mb, 22, 0);
    lv_obj_set_style_border_width(mb, 3, 0);
    lv_obj_set_style_border_color(mb, lv_color_hex(0xC9A227), 0);
    lv_obj_remove_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mb, menu_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(mb);
    lv_label_set_text(ml, LV_SYMBOL_BARS);
    lv_obj_set_style_text_font(ml, &aos_inter_36, 0);
    lv_obj_set_style_text_color(ml, lv_color_hex(0xF3EBD2), 0);
    lv_obj_center(ml);
    lv_obj_remove_flag(ml, LV_OBJ_FLAG_CLICKABLE);

    build_menu(root);
    cards_wire();
    return true;
}

/* A new view around a game already in course: every card that was on the
 * table is drawn again with the face it showed and put straight where it
 * belongs; the calls, the banner and the menu come back as they were. */
static void view_restore(void)
{
    const geo_t *o = &s_t.geo;
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            /* not dealt yet, or gathered at the end of the hand: at the deck */
            int32_t x = o->deck_x, y = o->deck_y;
            if (!s_t.gathered && s_t.vis[p][i]) card_target(p, i, &x, &y);
            place(p, i, x, y);
            if (s_t.vis[p][i]) {
                tr_card_render(s_t.card[p][i], s_t.face[p][i]);
                lv_obj_remove_flag(s_t.card[p][i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    score_refresh();
    if (s_t.banner_txt[0]) {
        uint32_t left = s_t.banner_until;
        char txt[sizeof s_t.banner_txt];
        memcpy(txt, s_t.banner_txt, sizeof txt);
        banner(txt, 0);
        s_t.banner_until = left;
    }
    if (s_t.bar_n > 0) {
        s_t.bar_n = 0;
        refresh_bar();
    }
    wait_show(s_t.wait_on);
    if (s_t.lk == LK_CHOOSE) build_chooser(s_t.root);
    menu_show(s_t.menu_open);
}

static void *truco_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&s_t, 0, sizeof(s_t));
    s_t.root   = root;
    s_t.sel    = -1;
    s_t.deal_i = -1;

    s_t.auto_play = dev_flag("TRUCO_AUTO");
    s_t.showall   = dev_flag("TRUCO_SHOWALL");
    s_t.fast      = dev_flag("TRUCO_FAST");

    if (!view_build(root)) {
        aos_ui_toast("Sin memoria", 2000);
        view_free();
        return NULL;
    }
    score_refresh();                /* the canvas comes from malloc: paint it before asking */

    int32_t v = 0;
    if (aos_hal_pref_get_i32("truco_won", &v))  s_t.won  = v;
    if (aos_hal_pref_get_i32("truco_lost", &v)) s_t.lost = v;
    s_t.my_seed = (uint32_t)aos_hal_uptime_ms() * 2654435761u + 1u;
    const char *sd = getenv("TRUCO_SEED");
    if (sd && sd[0]) s_t.my_seed = (uint32_t)atoi(sd);
    s_t.nonce = s_t.my_seed ^ 0xA5A5F00Du;
    aos_pad_reset(&s_t.pad, lv_tick_get());
    s_t.timer = lv_timer_create(tick_cb, 33, NULL);

    /* Can this device play two-handed at all? Asking the radio is the only
     * honest test: on a P4 without the link aos_hal_link_start() just says
     * no, at once. If it works it is left running only while somebody is
     * paired to be asked; otherwise it goes quiet again until the menu
     * wants it. */
    s_t.link_avail = aos_hal_link_start();
    aos_link_partner_t p;
    memset(&p, 0, sizeof p);
    s_t.have_partner = s_t.link_avail && aos_hal_link_partner(&p) && p.valid;
    if (s_t.have_partner) {
        snprintf(s_t.pname, sizeof s_t.pname, "%s", p.name);
        for (char *c = s_t.pname; *c; c++) {
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        }
    }
    if (s_t.link_avail) {
        aos_hal_link_stop();
    }
    aos_hal_log("truco", "link %s, partner %s", s_t.link_avail ? "available" : "not available",
                s_t.have_partner ? s_t.pname : "none");

    /* Against whom: with a partner paired in Enlace the table asks first.
     * TRUCO_LINK=1 skips the question (the simulator has no partner in its
     * preferences until the link is up). */
    if (dev_flag("TRUCO_LINK") && s_t.link_avail) {
        link_begin();
    } else if (s_t.have_partner && !s_t.auto_play) {
        s_t.lk = LK_CHOOSE;
        banner("CONTRA QUIEN?", 0);
        build_chooser(root);
    } else {
        solo_begin();
    }
    if (dev_flag("TRUCO_MENU")) menu_show(true);
    return &s_t;
}

static void game_begin(uint32_t seed)
{
    cards_wire();
    s_t.in_head = s_t.in_tail = 0;
    s_t.await_echo = false;
    s_t.over = false;
    s_t.sel  = -1;
    s_t.deal_i = -1;
    s_t.gathered = false;
    s_t.unseen_ms = now_ms();
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < TR_HAND_N; i++) {
            s_t.vis[p][i] = false;
            if (s_t.card[p][i]) lv_obj_add_flag(s_t.card[p][i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    bar_hide();
    wait_show(false);
    banner_hide();
    tr_new_game(&s_t.g, seed);
    score_refresh();
}

static void solo_begin(void)
{
    s_t.me = TR_YO;
    s_t.lk = LK_OFF;
    game_begin(s_t.my_seed);
    int32_t v = 0;
    if (aos_hal_pref_get_i32("truco_nos", &v) && v > 0 && v < TR_TARGET) {
        s_t.g.score[TR_YO] = v;
    }
    if (aos_hal_pref_get_i32("truco_ellos", &v) && v > 0 && v < TR_TARGET) {
        s_t.g.score[TR_EL] = v;
    }
    const char *sc = getenv("TRUCO_SCORE");
    if (sc && sc[0]) {
        int a = 0, b = 0;
        if (sscanf(sc, "%d,%d", &a, &b) == 2) {
            s_t.g.score[TR_YO] = a < 0 ? 0 : (a > 29 ? 29 : a);
            s_t.g.score[TR_EL] = b < 0 ? 0 : (b > 29 ? 29 : b);
        }
    }
    score_refresh();
}

static void link_begin(void)
{
    s_t.link_up = aos_hal_link_start();
    if (!s_t.link_up) {
        /* the radio said yes at the start and no now: say so, and play on */
        aos_ui_toast("Sin enlace: jugás contra la máquina", 2500);
        s_t.link       = false;
        s_t.link_avail = false;
        solo_begin();
        return;
    }
    s_t.link = true;
    aos_hal_link_offer("truco");
    aos_hal_link_reliable_reset();
    s_t.lk = LK_HELLO;
    s_t.hello_ms = 0;
    bar_hide();
    char buf[48];
    lv_snprintf(buf, sizeof buf, "BUSCANDO A %s", s_t.pname[0] ? s_t.pname : "LA PAREJA");
    banner(buf, 0);
}

/* Back to the machine: the radio off, my seat the first again. */
static void link_end(void)
{
    if (s_t.link_up) {
        aos_hal_link_offer("");
        aos_hal_link_stop();
    }
    s_t.link_up = false;
    s_t.link    = false;
    s_t.lk      = LK_OFF;
    s_t.is_host = false;
    wait_show(false);
}

static void choose_cb(lv_event_t *e)
{
    if (s_t.closing || s_t.lk != LK_CHOOSE) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    for (int k = 0; k < 2; k++) {
        if (s_t.choose[k]) {
            lv_obj_delete_async(s_t.choose[k]);     /* we are inside its callback */
            s_t.choose[k] = NULL;
            s_t.btn_lbl[BTN_MAX + k] = NULL;
        }
    }
    banner_hide();
    s_t.lk = LK_OFF;
    aos_hal_beep(620, 15);
    if (i == 0) solo_begin(); else link_begin();
}

/* The screen turned: the old view goes (objects first, then the buffers
 * they point at) and a new one is built in the new size around the same
 * game. */
static bool truco_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self; (void)inst;
    s_t.closing = true;
    lv_obj_clean(root);
    aos_pad_menu_clear(&s_t.pmenu);     /* its buttons went with the view */
    view_free();
    s_t.root = root;
    s_t.closing = false;
    if (!view_build(root)) {
        view_free();
        return false;                   /* the runtime makes the app again */
    }
    view_restore();
    return true;
}

static void truco_destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    s_t.closing = true;

    if (s_t.timer) {
        lv_timer_delete(s_t.timer);
        s_t.timer = NULL;
    }
    save_prefs();
    link_end();

    /* The objects first, with the context still standing: each canvas points
     * at a buffer we are about to free. */
    if (self && self->root) lv_obj_clean(self->root);

    view_free();
    memset(&s_t, 0, sizeof(s_t));
}

/* Back opens the menu, which says what leaving does (the hand in course is
 * lost; the score stays). From the menu, back leaves. While looking for
 * the partner or after losing it there is no game to keep: back leaves. */
static bool truco_back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;

    if (s_t.menu_open) return false;
    if (s_t.lk == LK_CHOOSE || s_t.lk == LK_LOST) return false;
    menu_show(true);
    return true;
}

static bool truco_init(aos_app_t *app)
{
    app->desc.id      = "aos.truco";
    app->desc.name    = "Truco";
    app->desc.icon    = "Tr";
    app->desc.icon_vec = AOS_ICON_CARDS;
    app->desc.color_a = 0x1E7A45;
    app->desc.color_b = 0x0A3C21;
    app->desc.order   = 152;
    app->desc.flags   = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP_AWAKE;

    app->create  = truco_create;
    app->destroy = truco_destroy;
    app->back    = truco_back;
    app->resize  = truco_resize;
    return true;
}

AOS_APP_ENTRY(truco_init);
