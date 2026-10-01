/*
 * P4OS - Radio (from AmoledOS): internet stations on a front panel from the
 * sixties.
 *
 * The sound is the firmware's: aos_hal_radio_play() hands a list of stations
 * to the player, which keeps the station playing with the app closed (and
 * the control centre drives it). This app is the panel: the dial with the
 * station, the song and its cover, the needle over the numbered scale, the
 * transport, the volume, nine keys the portal's /radio page fills, and - new
 * on the 5" screen - the whole list of stations beside them.
 *
 * The keys live in NVS as rad0..rad8 = "name \x1F url", written by the
 * portal (or by holding a key down while a station plays, like a real
 * preset); rad_gen goes up on every save and the app rebuilds its keys when
 * it moves. The key's logo, if the portal could make one, is
 * radio/logoN.jpg on the card. The list is radio/library.json, the portal's
 * "My list", with the 33 stations the portal starts with as the fallback.
 */
#pragma once

#include "lvgl.h"
#include "aos_hal.h"
#include "radio_art.h"
#include "radio_lib.h"

#define RADIO_KEYS      9

typedef struct {
    lv_obj_t *root;
    bool      closing;
    bool      shown;            /* show() after the first one reloads the list */

    /* the geometry of this orientation */
    int32_t   W, H;
    bool      land;
    int32_t   dial_w, cover_sz, scale_w, scale_h, scale_x, scale_y;

    /* the dial */
    lv_obj_t *dial;
    lv_obj_t *cover;            /* canvas cover_sz square                   */
    uint16_t *cover_px;
    lv_obj_t *station, *title, *artist, *meta, *onair;
    lv_obj_t *scale;            /* canvas, ARGB8888, drawn once             */
    uint8_t  *scale_px;
    lv_obj_t *scale_num[RADIO_KEYS];
    lv_obj_t *needle;
    int       needle_x, needle_to;      /* in 1/16 px */

    /* the panel */
    lv_obj_t *b_mute, *b_prev, *b_play, *b_next, *b_info;
    lv_obj_t *i_mute, *i_play;
    lv_obj_t *vol, *vol_lbl;
    lv_obj_t *key[RADIO_KEYS], *key_led[RADIO_KEYS], *key_lbl[RADIO_KEYS];
    lv_obj_t *status;

    /* the list of stations */
    lv_obj_t *list, *list_count;
    lv_obj_t *row[RADIO_LIB_MAX], *row_air[RADIO_LIB_MAX];
    int       s_row_on;

    /* the info card */
    lv_obj_t *info, *info_cover, *info_title, *info_text;
    uint16_t *info_px;

    lv_timer_t *anim;

    /* the keys */
    aos_radio_station_t st[RADIO_KEYS];
    int32_t   gen;

    /* all the stations */
    radio_lib_t lib;

    /* what is on screen, to write only what changed (LVGL repaints the same
     * text as if it were new) */
    char      s_station[48], s_title[96], s_artist[96], s_meta[96], s_status[96];
    int       s_active, s_state, s_vol, s_muted, s_onair;
    uint32_t  s_title_gen;
    int       s_cover_kind;     /* 0 drawn, 1 logo, 2 art                   */
    int       s_cover_slot;
    uint32_t  info_refresh;

    radio_art_t art;
} radio_t;

/* radio_ui.c */
void radio_ui_build(radio_t *r, lv_obj_t *root);
void radio_ui_free(radio_t *r);                 /* the buffers of the canvases */
void radio_ui_keys(radio_t *r);                 /* labels and LEDs of the keys */
void radio_ui_list(radio_t *r);                 /* the rows of the list        */
void radio_ui_list_on(radio_t *r, int index);   /* the row on air, -1 none     */
void radio_ui_cover(radio_t *r, const uint16_t *px); /* ART_PX square, NULL: drawn */
void radio_ui_info_open(radio_t *r);
void radio_ui_info_close(radio_t *r);
void radio_ui_needle_to(radio_t *r, int key);   /* -1: rest at the left */
void radio_ui_anim(lv_timer_t *t);

/* radio.c */
void radio_on_key(radio_t *r, int i);
void radio_on_key_store(radio_t *r, int i);     /* held down: store what plays */
void radio_on_station(radio_t *r, int index);   /* a row of the list */
void radio_on_play(radio_t *r);
void radio_on_step(radio_t *r, int step);
void radio_on_mute(radio_t *r);
void radio_on_volume(radio_t *r, int v);
void radio_info_text(radio_t *r, char *out, int cap);
int  radio_key_of_url(const radio_t *r, const char *url);  /* -1 if on no key */
