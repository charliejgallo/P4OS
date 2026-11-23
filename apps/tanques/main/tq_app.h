/*
 * TANQUES - the app's state, shared by its screens: the core (tanques.c:
 * life cycle, the worker, frames, touch), the battle (tq_battle.c), the
 * menus (tq_menu.c) and the hangar (tq_hangar.c).
 *
 * Everything runs in LVGL's task but the frames, which the worker draws
 * from a copy of the battle (tq_req_t).
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_pad.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "tq_snd.h"
#include "tq_art.h"
#include "tq_data.h"
#include "tq_game.h"
#include "tq_mem.h"
#include "tq_scene.h"

enum { SC_MENU = 0, SC_STORY, SC_FREE, SC_HANGAR, SC_BATTLE, SC_RESULT };

enum {
    B_MOVE = 0,     /* a person picks where to drive */
    B_DRIVE,        /* a tank drives its path */
    B_AIM,          /* a person aims, first person */
    B_AI_THINK,
    B_AI_AIM,       /* the board's turret turns to its aim */
    B_FLY,          /* shells in the air */
    B_BOOM,         /* the blast settles */
    B_HANDOVER,     /* two players: "pass the board" */
    B_LEAVE,        /* "leave the battle?" */
};

#define TQ_LABELS 6

typedef struct app_s {
    aos_app_t *self;
    lv_obj_t  *root;
    int        W, H;
    bool       closing;

    /* pixels */
    uint16_t  *fb[2];
    int        shown;
    lv_obj_t  *canvas, *touch, *ui;
    tq_req_t  *req;
    volatile int job;
    int        target;
    bool       worker_ok;
    bool       dirty;
    tq_prof_t  prof, prof_acc;
    uint32_t   perf_t0, perf_frames, perf_render_us, perf_world_us;
    int        perf_mode;
    lv_timer_t *timer;
    uint32_t   last_ms;
    /* the frame shown, for redrawing only where things move */
    uint32_t   shown_key, req_key;
    int        shown_wx, shown_wy, req_wx, req_wy;   /* their windows */
    tq_rects_t shown_rects, req_rects;
    uint32_t   part_frames, part_us, part_px, scroll_frames;
    bool       part_counted;
    /* the first person through the PPA, straight to the panel */
    int        blit_ok;                 /* -1 not known yet, 0 no (the simulator), 1 yes */
    bool       fp_shown, fp_dark;
    int        fp_y0, fp_y1;            /* the picture's rows between the bars */

    /* the world */
    tq_terrain_t *ter;
    tq_state_t st;
    tq_lut_t  *lut;                 /* TQ_MAX_TANKS paints + the wreck's */
    uint32_t   rgb[TQ_MAX_TANKS];
    tq_save_t  save;
    bool       art_ok;

    int        screen;

    /* the battle */
    int        bstate, bstate_before;
    float      st_t;
    bool       story;
    int        mission;
    int        humans;
    tq_setup_t setup;
    float      cam_x, cam_y, cam_tx, cam_ty;
    bool       cam_follow;
    int        drive_seg, drive_cost;
    float      ai_yaw, ai_pitch;
    int        ai_slot;
    int        coins_won, stars_won, winner;
    int        waves_done;                  /* the waves: lived through */
    int        chest_coins;                 /* the chests the person took this battle */

    /* the hangar */
    int        h_tank, h_tab;
    float      h_spin;

    /* touch */
    bool       pressing, dragging;
    int        px0, py0, plx, ply;

    /* the battle's HUD */
    lv_obj_t  *top_bar, *chip[TQ_MAX_TANKS], *chip_hp[TQ_MAX_TANKS], *turn_lbl, *wind_lbl, *mode_lbl;
    lv_obj_t  *move_bar, *ap_lbl, *ap_dot[14], *btn_aim, *hint;
    lv_obj_t  *aim_bar, *btn_w[TQ_SLOTS_MAX], *btn_fire, *btn_back, *aim_lbl;
    lv_obj_t  *banner, *leave;
    lv_obj_t  *lbl[TQ_LABELS];
    float      lbl_t[TQ_LABELS];
    tq_v3_t    lbl_p[TQ_LABELS];
    /* the hangar's and menus' bits that change */
    lv_obj_t  *coins_lbl, *h_name, *h_role, *h_list, *h_buy, *h_tabs[3];
} app_t;

/* tanques.c */
void tqa_screen(app_t *a, int screen);
void tqa_save(app_t *a);
lv_obj_t *tqa_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius);
lv_obj_t *tqa_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t rgb);
lv_obj_t *tqa_button(lv_obj_t *parent, const char *txt, uint32_t rgb, int w, int h, lv_event_cb_t cb, void *ud);
void tqa_btn_text(lv_obj_t *b, const char *txt);
void tqa_show(lv_obj_t *o, bool on);
void tqa_wait_worker(app_t *a);
/* the paints of the battle's tanks (and the wreck's) from the setup */
void tqa_paints(app_t *a);

/* tq_battle.c */
void tqb_start(app_t *a, const tq_setup_t *su, bool story, int mission);
void tqb_build(app_t *a);              /* the HUD, on the current ui */
void tqb_tick(app_t *a, float dt);
void tqb_fill(app_t *a, tq_req_t *r);
void tqb_touch(app_t *a, int ev, int x, int y, int dx, int dy);
bool tqb_back(app_t *a);
bool tqb_animating(app_t *a);

/* tq_menu.c */
void tqm_build_menu(app_t *a);
void tqm_play(app_t *a, int mission);
void tqm_build_story(app_t *a);
void tqm_build_free(app_t *a);
void tqm_build_result(app_t *a);
void tqm_menu_tick(app_t *a, float dt);
void tqm_demo(app_t *a);               /* the battle behind the title */

/* tq_hangar.c */
void tqh_build(app_t *a);
void tqh_fill(app_t *a, tq_req_t *r);
void tqh_touch(app_t *a, int ev, int x, int y, int dx, int dy);
/* A player's tank as a setup entry */
void tqh_player_tank(app_t *a, int slot, int type, bool full);
void tqh_free(void);
void tqh_cycle(app_t *a, int d);
