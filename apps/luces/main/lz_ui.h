/*
 * P4OS - Luces: what the screen's files share (luces.c, lz_stage.c,
 * lz_tabs.c, lz_remote.c).
 */
#pragma once

#include "lz.h"

#include "lvgl.h"
#include "aos_io.h"

#include <stdbool.h>
#include <stdint.h>

#define LZ_ACCENT lv_color_hex(0xEC4899)

enum { LZ_TAB_STAGE = 0, LZ_TAB_SCENES, LZ_TAB_PATCH, LZ_TAB_OUT, LZ_TAB_COUNT };

/* Art-Net out (lz_net.c) */
typedef struct {
    bool     on;
    char     dest[40];          /* "" = the broadcast 255.255.255.255 */
    uint16_t base;              /* the port-address of universe 1 */
    uint32_t packets;
    char     err[48];
} lz_net_t;

extern lz_net_t LZN;
void lz_net_start(void);
void lz_net_stop(void);

/* what a page builds and its refresh looks at; cleared on every rebuild */
typedef struct {
    /* Escenario */
    lv_obj_t *tile[LZ_FIX], *tile_dot[LZ_FIX];
    lv_obj_t *dim_s, *dim_l, *master_s, *master_l, *speed_s;
    lv_obj_t *fx_chip[LZ_FX_COUNT], *strobe_chip[4];
    lv_obj_t *pad, *pad_dot, *sel_l, *group[LZ_GROUPS];
    /* Escenas */
    lv_obj_t *scene[LZ_SCENES], *chase_btn, *bpm_l, *fade_chip[6], *cfade_chip[3];
    /* Salida */
    lv_obj_t *out_map, *out_stats, *an_stats;
} lz_page_t;

typedef struct {
    lv_obj_t *root, *content, *tabs[LZ_TAB_COUNT], *overlay;
    lv_obj_t *bar_state, *bar_btn, *bar_bo;
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    int tab;
    lz_page_t pg;
    /* the output */
    char port[16];
    bool want_open;
    aos_io_dmx_t *h;
    char err[80];
    aos_dmx_stats_t st;
    /* the overlays */
    char kp_text[16];
    lv_obj_t *kp_value, *ta;
    void (*kp_done)(int v);
    bool kp_fresh;
    int edit;                   /* the fixture or scene an overlay is about */
    int add_type, add_n, add_univ;  /* Patch, adding */
    int out_univ;               /* Salida: the universe the map shows */
    uint32_t seen_changed;      /* LZ.changed when the page was built */
} lz_ui_t;

extern lz_ui_t LZU;

/* luces.c */
lv_obj_t *lz_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *lz_card(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *lz_column(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *lz_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *lz_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
void      lz_chip_set(lv_obj_t *chip, bool on);
lv_obj_t *lz_chips_row(lv_obj_t *parent, int32_t w);
lv_obj_t *lz_caption(lv_obj_t *parent, const char *text, int32_t w);
lv_obj_t *lz_section(lv_obj_t *parent, const char *text);
lv_obj_t *lz_slider(lv_obj_t *parent, int32_t w, int min, int max, int value, lv_event_cb_t cb, void *ud);
lv_obj_t *lz_sheet(const char *title);           /* a card over the screen, for choices */
void      lz_keypad(const char *title, int value, void (*done)(int v));
void      lz_text(const char *title, const char *value, void (*done)(const char *v));
void      lz_overlay_close(void);
void      lz_rebuild(void);
void      lz_save(void);
void      lz_output_tick(void);
void      lz_bar_refresh(void);
void      lz_set_port(const char *port);

/* lz_stage.c */
void lz_stage_build(lv_obj_t *parent, int32_t ch);
void lz_stage_refresh(void);

/* lz_tabs.c */
void lz_scenes_build(lv_obj_t *parent, int32_t ch);
void lz_scenes_refresh(void);
void lz_patch_build(lv_obj_t *parent, int32_t ch);
void lz_out_build(lv_obj_t *parent, int32_t ch);
void lz_out_refresh(void);
void lz_chase_set(bool on);

/* lz_remote.c */
void lz_remote_tick(void);
