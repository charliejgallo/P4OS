/*
 * P4OS - Mensajes: what the screen's files share.
 *
 *   mensajes.c     the app: life cycle, icon, the 100 ms timer that looks at
 *                  the store, notifications, the badge, the settings
 *   mj_ui_login.c  signing in: the app's keys, the number, the code, the
 *                  two-step password
 *   mj_ui_list.c   the chat list, the search, the settings sheet
 *   mj_ui_chat.c   the open chat: bubbles, the composer, the message's
 *                  sheet (reply, forward, edit, delete, reactions), photos
 *   mj_ui_util.c   small widgets, times, names, avatars, sheets, emoji
 *   mj_img.c       pictures decoded off the screen's thread, kept in PSRAM
 *   mj_web.c       the portal's page, over the live channel
 *
 * Upright the list and the chat take the whole screen in turn; lying down
 * the list is a column on the left and the chat the rest.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "lvgl.h"
#include "mj_api.h"
#include "mj_mtp.h"
#include "mj_port.h"

#define MJ_APP_ID "aos.mensajes"

typedef struct mj_row mj_row_t;

typedef struct {
    lv_obj_t *root;
    int32_t W, H;
    bool land;
    bool visible;               /* in front (for notifications and reading)  */

    /* the two halves (lying down) or the one in front (upright) */
    lv_obj_t *list_pane, *chat_pane;
    int32_t list_w;

    /* signing in */
    lv_obj_t *login;
    int login_state;
    lv_obj_t *ta1, *ta2, *err_lbl, *go_btn;

    /* the list */
    lv_obj_t *list, *list_title, *net_lbl, *search_ta;
    char filter[64];
    bool show_found;
    int list_scroll;

    /* the chat */
    mj_pkey_t open;
    lv_obj_t *chat_list, *chat_title, *chat_status, *chat_avatar, *composer, *ta, *send_btn;
    lv_obj_t *reply_bar, *reply_lbl, *emoji_panel, *older_spin;
    mj_row_t *rows;
    int nrows, rows_cap;
    int32_t reply_to, editing;
    bool want_bottom;
    uint32_t last_typing_ms;

    /* over it */
    lv_obj_t *kb, *kb_ta;
    lv_obj_t *sheet;
    lv_obj_t *viewer;

    /* what was drawn last */
    uint32_t v_auth, v_dialogs, v_peers, v_chat, v_media, v_img, v_toast, v_net, v_search;
    int badge;

    /* settings */
    bool notif_on;
    char data_dir[160], dl_dir[160];
} mj_ui_t;

/* "U" in the code; mj_U to the linker: the simulator links every app together
 * and CAN, Datalog and EEPROM have a global U of their own. */
#define U mj_U
extern mj_ui_t U;

/* ---- mensajes.c ---- */
void mj_ui_rebuild(void);                 /* the whole screen again           */
void mj_ui_open_chat(mj_pkey_t k);
void mj_ui_close_chat(void);

/* ---- mj_ui_login.c ---- */
void mj_login_build(lv_obj_t *parent, int state);
void mj_login_update(void);               /* the store's auth moved           */

/* ---- mj_ui_list.c ---- */
void mj_list_build(lv_obj_t *pane);
void mj_list_refresh(void);
void mj_list_net(void);
void mj_settings_open(void);
void mj_forward_pick(mj_pkey_t from, int32_t id);
void mj_list_free(void);

/* ---- mj_ui_chat.c ---- */
void mj_chat_build(lv_obj_t *pane);
void mj_chat_refresh(bool force);
void mj_chat_media(void);                 /* files or pictures arrived         */
void mj_chat_free_rows(void);
void mj_chat_status(void);
bool mj_chat_back(void);                  /* true: handled                     */

/* ---- mj_ui_util.c ---- */
lv_obj_t *mj_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *mj_label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c);
void      mj_label_safe(lv_obj_t *l, const char *t);    /* aos_text_safe first  */
lv_obj_t *mj_icon_btn(lv_obj_t *parent, const char *glyph, int32_t d, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *mj_pill_btn(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *mj_avatar(lv_obj_t *parent, mj_pkey_t k, int32_t d);
void      mj_avatars_refresh(lv_obj_t *root);          /* pictures that arrived */
lv_color_t mj_peer_color(int64_t id);
void      mj_time_short(int32_t t, char *out, size_t n);          /* "14:05", "ayer", "lun", "27/11" */
void      mj_day_label(int32_t t, char *out, size_t n);           /* "Hoy", "Ayer", "27 de noviembre" */
void      mj_status_line(mj_pkey_t k, char *out, size_t n);       /* "en línea", "visto a las 14:05" */
void      mj_media_words(const mj_msg_t *m, char *out, size_t n); /* "📷 Foto", "🎤 Nota de voz 0:07" */
void      mj_action_words(const mj_msg_t *m, const char *who, char *out, size_t n);
void      mj_size_words(int64_t b, char *out, size_t n);
lv_obj_t *mj_sheet_open(const char *title, int32_t h);          /* the sheet's body */
void      mj_sheet_close(void);
void      mj_toast_code(int code, const char *arg);
void      mj_kb_show(lv_obj_t *ta, lv_keyboard_mode_t mode, lv_event_cb_t ready);
void      mj_kb_hide(void);
extern const char *const MJ_REACTIONS[];
extern const int MJ_NREACTIONS;
extern const char *const MJ_EMOJI[];
extern const int MJ_NEMOJI;

/* ---- mj_img.c ---- */
/* A picture of a file on the card, decoded to fit (or to fill and crop)
 * w x h. NULL while it is being decoded (it is queued); later calls give
 * it. mj_img_ver() moves whenever one is ready. */
const lv_image_dsc_t *mj_img_get(const char *path, int w, int h, bool fill);
uint32_t mj_img_ver(void);
/* Puts the picture in an lv_image, held until the image is deleted. */
void     mj_img_attach(lv_obj_t *img, const lv_image_dsc_t *d);
void     mj_img_start(void);
void     mj_img_stop(void);
void     mj_img_forget(const char *path);
size_t   mj_img_bytes(void);

/* ---- mj_web.c ---- */
void mj_web_tick(void);
void mj_web_stop(void);
