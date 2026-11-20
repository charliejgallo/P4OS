/*
 * BASTION - the app's own state, shared by bastion.c (life cycle, the
 * battle's thread, its panels) and bs_menu.c (title, campaign,
 * laboratory).
 */
#pragma once

#include "bs_art.h"
#include "bs_audio.h"
#include "bs_game.h"
#include "bs_gfx.h"
#include "bs_pak.h"
#include "bs_save.h"
#include "bs_view.h"
#include "bs_world.h"

#include "aos_app.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "lvgl.h"

enum { ST_TITLE, ST_MAP, ST_LAB, ST_LOADING, ST_BATTLE };
enum { PN_NONE, PN_PAUSE, PN_RESULT };
enum { IN_TAP, IN_PAD };

#define BS_ACCENT 0x38D8FF

typedef struct {
    char            path[160];
    bs_world_t      head;
    uint8_t        *thumb;
    lv_image_dsc_t  thumb_dsc;
} bs_card_t;

typedef struct {
    uint16_t *buf;
    uint32_t  flip;             /* our flip that showed it last, 0 none    */
    uint32_t *hash;             /* what its tiles show                     */
} bs_pbuf_t;

typedef struct {
    uint8_t  kind;
    int16_t  x, y;
    uint32_t bits;
} bs_input_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    int         state;
    bs_save_t   save;
    bs_art_t    art;
    bool        art_ok;
    lv_timer_t *timer, *load_timer;
    aos_pad_t   pad;
    aos_pad_menu_t menu;

    /* the menus */
    bs_card_t   card[BS_WORLDS_MAX];
    int         ncard, sel_world, sel_sector;
    lv_obj_t   *screen;
    int         lab_tab, lab_back;
    uint8_t    *title_img;
    lv_image_dsc_t title_dsc;
    uint8_t    *img[40];        /* portraits and icons LVGL shows          */
    lv_image_dsc_t img_dsc[40];
    lv_obj_t   *m_scroll, *m_name, *m_desc, *m_sector[BS_SECTORS], *m_cores;
    lv_obj_t   *m_card[BS_WORLDS_MAX];

    /* the battle */
    bs_pak_t    wpak;
    bs_world_t  world;
    bool        loaded;
    uint16_t   *bg, *depth;
    bs_gfx_t    gfx;
    bs_game_t  *game;
    bs_ui_t    *ui;
    uint16_t   *fb;             /* the frame LVGL shows under its panels   */
    uint32_t   *fb_hash;
    bool        fb_full;
    lv_obj_t   *canvas, *touch, *panel, *loading;
    int         panel_kind;
    bool        over_seen, button_pause, no_memory, result_done;
    int         res_cores, res_new, res_rank;

    /* the battle's thread */
    void       *mx;
    volatile bool run;          /* frames may be drawn                     */
    bool        worker_on;
    bool        clock_reset;
    uint32_t    last_us;
    float       acc;
    bool        flip_ok;
    uint32_t    flips;
    bs_pbuf_t   pb[3];
    volatile uint32_t in_w, in_r;
    bs_input_t  in[32];
    volatile uint32_t snd_w, snd_r;
    uint8_t     snd[64];
    volatile uint32_t frame_gen;
    uint32_t    frame_seen;

    /* the log's line */
    uint32_t    perf_frames, perf_t0, perf_tiles, perf_items, perf_steps;
    uint32_t    perf_sim_us, perf_list_us, perf_draw_us;
} bs_app_t;

/* bs_menu.c */
void bs_menu_show(bs_app_t *a, int state);
void bs_menu_free(bs_app_t *a);
void bs_menu_pad(bs_app_t *a);
bool bs_menu_back(bs_app_t *a);
void bs_cards_load(bs_app_t *a);
void bs_cards_free(bs_app_t *a);
lv_obj_t *bs_button(lv_obj_t *parent, const char *text, uint32_t accent, bool primary, int w, int h, lv_event_cb_t cb,
                    void *ud);
lv_obj_t *bs_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col);
uint8_t *bs_pak_image(bs_pak_t *pak, const char *name, lv_image_dsc_t *d);
int  bs_lang(void);

/* bastion.c */
void bs_battle_start(bs_app_t *a, int world, int sector);
void bs_battle_quit(bs_app_t *a);
