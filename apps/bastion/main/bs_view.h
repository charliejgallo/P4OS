/*
 * BASTION - a battle on the screen: the scene (bs_scene.c) and the HUD
 * with what the player does on it (bs_hud.c). Both only fill the frame's
 * list (bs_gfx.h); both run on the battle's thread.
 *
 * The HUD's words are copied when the battle starts and when the app
 * comes back to the front (bs_hud_strings, on LVGL's thread): the
 * catalogue belongs to the app in front and may go while the battle's
 * thread still draws.
 */
#pragma once

#include "bs_art.h"
#include "bs_game.h"
#include "bs_gfx.h"

#include <stdbool.h>
#include <stdint.h>

#define BS_TOP_H    88          /* the bar on top                          */
#define BS_PANEL_Y  1092        /* where the panel below starts            */
#define BS_MAX_BTN  40

enum {
    ACT_NONE, ACT_SPEED, ACT_PAUSE, ACT_WAVE, ACT_POWER, ACT_PICK, ACT_UPGRADE, ACT_SELL, ACT_MODE, ACT_CLOSE
};

enum {
    STR_WAVE, STR_CALL, STR_UPGRADE, STR_SELL, STR_MAX, STR_FIRST, STR_STRONG, STR_WEAK, STR_NEAR, STR_DMG,
    STR_RANGE, STR_KILLS, STR_LEFT, STR_HINT_PAD, STR_HINT_BUILD, STR_HINT_AIM, STR_HINT_WAVE, STR_NO_CREDITS,
    STR_BOSS, STR_JAMMED, STR_PER_WAVE, STR_PER_SEC, STR_LAST, STR_AIR, STR_GROUND, STR_BOOST, STR_FINAL, STR_HINT_AIR,
    STR_MILE, STR_MILE_PAY, STR_N
};

typedef struct {
    int16_t x, y, w, h;
    uint8_t act, arg, enabled, pad;
} bs_btn_t;

typedef struct {
    int      sel;               /* the pad chosen, -1 none                 */
    int      pick;              /* the tower chosen in the build grid      */
    int      aim;               /* the power waiting for a place           */
    int      speed;             /* 1..3                                    */
    bool     pad_on;            /* a gamepad drives: its frame shows       */
    int      cursor;            /* the pad its frame is on                 */
    int      focus;             /* -1: the field; else a button            */
    bs_btn_t btn[BS_MAX_BTN];
    int      nbtn;
    bool     want_pause;
    float    t;
    char     hint[120];
    float    hint_t;
    int      seen_wave;         /* for the "wave N" banner                 */
    float    banner_t;
    int      seen_miles;        /* and for a goal's, in the mode without end */
    float    mile_t;
    /* the words, copied (see above) */
    char     str[STR_N][72];
    char     tw_name[TW_N][40], tw_desc[TW_N][96];
    char     al_name[AL_N][28];
    char     pw_name[PW_N][32];
} bs_ui_t;

void bs_ui_init(bs_ui_t *ui);
void bs_hud_strings(bs_ui_t *ui);
/* the buttons of this frame, from what is selected */
void bs_hud_layout(bs_ui_t *ui, const bs_game_t *g);
void bs_hud_tap(bs_ui_t *ui, bs_game_t *g, int x, int y);
/* the gamepad: what went down (AOS_PAD_* bits, with the directions' repeats) */
void bs_hud_pad(bs_ui_t *ui, bs_game_t *g, uint32_t pressed);
void bs_hud_draw(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g);
void bs_scene_draw(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g);
