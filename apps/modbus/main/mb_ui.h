/*
 * P4OS - Modbus: the screen's shared parts (modbus.c) and the tabs
 * (mb_tabs.c). LVGL's thread only.
 */
#pragma once

#include "lvgl.h"
#include "mb.h"

#define C_TEAL   lv_color_hex(0x10B981)
#define C_AMBER  lv_color_hex(0xF59E0B)

enum { TAB_MASTER = 0, TAB_SLAVE, TAB_MON, TAB_TRAFFIC, TAB_WIRE, TAB_COUNT };
enum { FMT_U16, FMT_S16, FMT_HEX, FMT_BIN, FMT_U32, FMT_S32, FMT_FLOAT, FMT_COUNT };

typedef struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT], *overlay;
    lv_obj_t *bar_conn, *bar_state, *bar_btn, *bar_btn_l;
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    int tab;
    int last_tool;              /* the master's tool when the page was built */
    uint32_t seen, mseen, sseen, tseen, logseen;
    /* settings kept with the app */
    int fmt;
    bool swap;                  /* 32-bit values: low word first (CDAB) */
    bool paused, only_err;
    bool boot;                  /* the BOOT button was pressed */
    /* tab widgets, NULL when the tab is not shown */
    lv_obj_t *m_status, *m_stats, *m_grid, *m_text, *m_tool, *m_tool_bar, *m_found;
    lv_obj_t *s_stats, *s_last, *s_hr[16], *s_coil[16], *s_ir[8], *s_di[8];
    lv_obj_t *mon_tot, *mon_rows;
    lv_obj_t *t_list, *t_rows[60], *t_pause;
    /* the keypad and the text field */
    char kp_text[16];
    lv_obj_t *kp_value, *ta, *kb;
    void (*kp_done)(double v);
    int kp_dec;
    bool kp_fresh, kp_hex;
} mb_ui_t;

extern mb_ui_t MBU;

/* modbus.c: little widgets */
lv_obj_t *mb_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *mb_card(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *mb_column(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *mb_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *mb_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *mb_chips_row(lv_obj_t *parent, int32_t w);
lv_obj_t *mb_caption(lv_obj_t *parent, const char *text, int32_t w);
lv_obj_t *mb_section(lv_obj_t *parent, const char *text);
lv_obj_t *mb_switch(lv_obj_t *parent, bool on, lv_event_cb_t cb);
/* two columns side by side when lying down, one when standing */
void      mb_columns(lv_obj_t *parent, int32_t ch, int32_t left_w, lv_obj_t **l, int32_t *lw, lv_obj_t **r, int32_t *rw);
void      mb_keypad(const char *title, double value, int dec, const char *unit, void (*done)(double));
void      mb_text_entry(const char *title, const char *value, void (*done)(const char *));
void      mb_overlay_close(void);
void      mb_settings_save(void);
void      mb_rebuild_page(void);
void      mb_bar_refresh(void);

/* mb_tabs.c */
void tab_master_build(lv_obj_t *parent, int32_t ch);
void tab_master_refresh(bool values);
void tab_slave_build(lv_obj_t *parent, int32_t ch);
void tab_slave_refresh(void);
void tab_mon_build(lv_obj_t *parent, int32_t ch);
void tab_mon_refresh(void);
void tab_traffic_build(lv_obj_t *parent, int32_t ch);
void tab_traffic_refresh(void);
void tab_wire_build(lv_obj_t *parent, int32_t ch);

/* mb_ui_prof.c: device profiles */
void prof_init(void);
void prof_tick(void);
void prof_chip(lv_obj_t *parent, int32_t w);
void prof_points(lv_obj_t *parent, int32_t w, bool slave);
void prof_refresh(void);
void prof_forget(void);
void prof_sheet(void);
