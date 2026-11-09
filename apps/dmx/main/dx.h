/*
 * P4OS - DMX: what the app's files share.
 *
 * A bench tool for DMX512 on RS485, on the firmware's aos_io_dmx_* (the
 * BREAK, the timing and the interrupt that receives are the firmware's).
 * Four tabs:
 *
 *   Consola     the app sends: 512 faders a page of 16, all to a level,
 *               patterns (a chase channel by channel, a ramp, a rainbow on
 *               eight RGB pixels), and the packet's shape (slots, BREAK,
 *               MAB, rate) to try what a fixture puts up with
 *   Aparato     the app is a fixture: an address and a kind (dimmer, RGB,
 *               RGBW, a 7-channel PAR, a moving head, a bar of 8 pixels),
 *               and what it would do drawn, with its channels
 *   Analizador  the app listens: rate, slots, start code, BREAK and MAB as
 *               measured, bad packets, and the 512 channels as a map, the
 *               ones moving lit
 *   Cableado    XLR, the terminator, what not to do
 *
 * No thread of its own: sending and receiving run in the firmware, and the
 * app's timer reads the stats and writes the universe. The link opens
 * closed: sending on somebody's rig is the user's call.
 */
#pragma once

#include "lvgl.h"
#include "aos_io.h"

#include <stdbool.h>
#include <stdint.h>

#define DX_TEAL  lv_color_hex(0x7C3AED)
#define DX_AMBER lv_color_hex(0xF59E0B)

enum { DX_TAB_CONSOLE = 0, DX_TAB_FIXTURE, DX_TAB_ANALYZER, DX_TAB_WIRE, DX_TAB_COUNT };
enum { DX_PAT_NONE = 0, DX_PAT_CHASE, DX_PAT_RAMP, DX_PAT_RAINBOW, DX_PAT_COUNT };
enum { DX_FX_DIMMER = 0, DX_FX_RGB, DX_FX_RGBW, DX_FX_PAR7, DX_FX_HEAD, DX_FX_BAR8, DX_FX_COUNT };

typedef struct {
    /* the link */
    char port[16];
    bool want_open;
    aos_io_dmx_t *h;
    aos_dmx_dir_t dir;          /* of h */
    char err[80];
    aos_dmx_cfg_t cfg;          /* what the console sends */
    /* console */
    uint8_t uni[513];           /* [0] start code, [1..512] the levels */
    int page;                   /* 16 channels a page */
    int pattern;
    uint32_t pat_t0;
    /* fixture */
    int fx_kind;
    int fx_addr;
    /* what came */
    uint8_t rx[513];
    int rx_slots;
    aos_dmx_stats_t st;
    uint8_t prev[513];
    uint32_t chg_ms[513];
    int an_page;                /* 32 channels shown as numbers */
} dx_state_t;

typedef struct {
    lv_obj_t *root, *content, *tabs[DX_TAB_COUNT], *overlay;
    lv_obj_t *bar_port, *bar_state, *bar_btn;
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    int tab;
    /* console */
    lv_obj_t *fader[16], *fader_v[16], *page_l;
    /* fixture */
    lv_obj_t *fx_draw, *fx_list, *fx_state;
    /* analyzer */
    lv_obj_t *an_stats, *an_map, *an_nums, *an_page_l;
    /* keypad */
    char kp_text[16];
    lv_obj_t *kp_value;
    void (*kp_done)(int v);
    bool kp_fresh;
} dx_ui_t;

extern dx_state_t DX;
extern dx_ui_t DXU;

/* dmx.c */
lv_obj_t *dx_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *dx_card(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *dx_column(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *dx_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *dx_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *dx_chips_row(lv_obj_t *parent, int32_t w);
lv_obj_t *dx_caption(lv_obj_t *parent, const char *text, int32_t w);
lv_obj_t *dx_section(lv_obj_t *parent, const char *text);
void      dx_keypad(const char *title, int value, void (*done)(int v));
void      dx_overlay_close(void);
void      dx_rebuild(void);
void      dx_save(void);
void      dx_send_universe(void);

/* dx_tabs.c */
void dx_console_build(lv_obj_t *parent, int32_t ch);
void dx_console_tick(void);
void dx_fixture_build(lv_obj_t *parent, int32_t ch);
void dx_fixture_refresh(void);
void dx_analyzer_build(lv_obj_t *parent, int32_t ch);
void dx_analyzer_refresh(void);
void dx_wire_build(lv_obj_t *parent, int32_t ch);
const char *dx_fx_name(int kind);
int dx_fx_slots(int kind);
