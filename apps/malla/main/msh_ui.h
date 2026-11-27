/*
 * Malla - the screens.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Four tabs (Chats, Nodos, Mapa, Nodo) under a header with the link's
 * state, and two views that take the whole body: a conversation and a
 * node. Everything is rebuilt from the model when its sequence numbers
 * move (msh_ui_refresh, from the app's timer), with the scroll kept.
 * Sheets (connection, PIN, share, edit) go over everything, one at a time.
 */
#pragma once

#include "msh.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "lvgl.h"

typedef enum { MSH_TAB_CHATS = 0, MSH_TAB_NODES, MSH_TAB_MAP, MSH_TAB_NODE, MSH_TAB_N } msh_tab_t;
typedef enum { MSH_V_TAB = 0, MSH_V_CHAT, MSH_V_NODE } msh_view_t;

typedef struct {
    aos_app_t *self;
    lv_obj_t *root;
    int W, H;
    bool land, hidden;
    msh_tab_t tab;
    msh_view_t view;
    msh_view_t back_to;             /* where a node opened from a chat goes back */

    lv_obj_t *head, *title, *chip, *chip_dot, *chip_txt;
    lv_obj_t *body;                 /* what the tab or view draws in */
    lv_obj_t *bar, *tab_btn[MSH_TAB_N], *badge;
    lv_obj_t *sheet;                /* the one over everything, or NULL */
    int sheet_kind;

    /* the conversation open */
    uint32_t chat_peer;
    int chat_ch;
    lv_obj_t *chat_list, *chat_ta, *chat_kb, *chat_send;
    uint32_t reply_to;              /* the message a reaction goes to */

    /* the node open */
    uint32_t node_num;

    /* the map: centre in 1e-7 degrees, metres a pixel */
    int32_t map_lat, map_lon;
    float map_mpp;
    bool map_fitted;
    lv_obj_t *map_obj;
    lv_point_t map_press;
    bool map_dragged;

    /* what the screen shows, to know when to build again */
    uint32_t seen_nodes, seen_msgs, seen_cfg, seen_state, seen_trace, seen_notice;
    uint32_t seen_rx;
    msh_ask_t seen_ask;
    bool dirty;                     /* build the body again on the next refresh */
    uint32_t last_age_redraw;       /* the "hace 3 min" texts age by themselves */

    /* the BLE scan of the connection sheet */
    bool scanning;
    lv_obj_t *scan_list;
    int n_found;
    struct { uint8_t addr[6], type; int8_t rssi; char name[32]; uint32_t seen; } found[16];
} msh_ui_t;

extern msh_ui_t UI;

/* msh_ui_main.c */
void msh_ui_build(void);                    /* the whole screen, from nothing */
void msh_ui_refresh(void);                  /* from the timer: what changed */
void msh_ui_show_tab(msh_tab_t t);
void msh_ui_open_chat(uint32_t peer, int ch);
void msh_ui_open_node(uint32_t num);
bool msh_ui_back(void);                     /* true when it went back somewhere */
void msh_ui_header(void);                   /* the title and the chip again */
/* a sheet over everything: a panel at the bottom (or centred acostada) on a
 * dimmed backdrop; tapping the backdrop closes it */
lv_obj_t *msh_ui_sheet(int kind, const char *title);
void msh_ui_sheet_close(void);
/* small builders shared by the screens */
lv_obj_t *msh_ui_col(lv_obj_t *parent);     /* a scrolling column filling the parent */
lv_obj_t *msh_ui_card(lv_obj_t *parent);
lv_obj_t *msh_ui_row(lv_obj_t *parent);     /* a row of content-sized items */
lv_obj_t *msh_ui_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c);
lv_obj_t *msh_ui_btn(lv_obj_t *parent, const char *text, lv_color_t c, lv_event_cb_t cb, void *ud);
lv_obj_t *msh_ui_avatar(lv_obj_t *parent, uint32_t num, int size);
void msh_ui_age(int32_t s, char *out, size_t cap);          /* "hace 3 min" */
void msh_ui_when(const msh_msg_t *m, char *out, size_t cap);/* "14:32", "ayer 9:05" */
void msh_ui_safe(char *out, size_t cap, const char *in);    /* text from the mesh, emoji drawn */
void msh_ui_num(char *out, size_t cap, float v, int dec);   /* "6,5": the decimal comma */
void msh_ui_dist(char *out, size_t cap, float m);           /* "850 m", "12,3 km" */

/* msh_ui_chats.c */
void msh_ui_chats_build(lv_obj_t *body);
void msh_ui_chat_build(lv_obj_t *body);
void msh_ui_chat_refresh(void);
void msh_ui_chat_close(void);

/* msh_ui_nodes.c */
void msh_ui_nodes_build(lv_obj_t *body);
void msh_ui_node_build(lv_obj_t *body);

/* msh_ui_map.c */
void msh_ui_map_build(lv_obj_t *body);

/* msh_ui_cfg.c */
void msh_ui_cfg_build(lv_obj_t *body);
void msh_ui_conn_sheet(void);               /* where the node is */
void msh_ui_pin_sheet(void);                /* the PIN it asks for */
void msh_ui_scan_tick(void);                /* the BLE scan while the sheet is open */
void msh_ui_scan_stop(void);

/* msh_emoji.c */
void msh_emoji_sheet(lv_obj_t *ta);         /* picks one into the text area */
void msh_reaction_sheet(uint32_t msg_id);   /* puts one on a message */
/* the text area's text as it goes to the mesh: the emoji drawn as
 * private-use code points turned back into their UTF-8 */
void msh_emoji_to_utf8(const char *in, char *out, size_t cap);
/* A short text from the screen's emoji, as it should travel. */

/* msh_portal.c */
void msh_portal_tick(void);
void msh_portal_stop(void);

/* malla.c */
void msh_app_connect(const msh_target_t *t);    /* remember it and start */
void msh_app_disconnect(void);                  /* stop and forget */

enum { MSH_SHEET_NONE = 0, MSH_SHEET_CONN, MSH_SHEET_PIN, MSH_SHEET_SHARE, MSH_SHEET_IMPORT, MSH_SHEET_EDIT,
       MSH_SHEET_EMOJI, MSH_SHEET_REACT, MSH_SHEET_CONFIRM, MSH_SHEET_CHANNEL };
