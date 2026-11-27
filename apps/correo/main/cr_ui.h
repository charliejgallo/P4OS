/*
 * P4OS - Correo: what the screens share (cr_ui*.c).
 *
 * A stack of screens, each built from scratch into the page it is given
 * (the whole root, or the right half of it in landscape when a message is
 * read beside its list). Their state lives in U so a rebuild after turning
 * the screen or a change of the model shows the same thing.
 */
#pragma once

#include "cr.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "lvgl.h"

/* Every app is linked together in the simulator: the short names below
 * are Correo's own. */
#define ui_acct_name crui_acct_name
#define ui_bar crui_bar
#define ui_bar_right crui_bar_right
#define ui_box crui_box
#define ui_btn crui_btn
#define ui_card crui_card
#define ui_chip crui_chip
#define ui_col crui_col
#define ui_confirm crui_confirm
#define ui_err_text crui_err_text
#define ui_field crui_field
#define ui_go crui_go
#define ui_icon_btn crui_icon_btn
#define ui_kb_attach crui_kb_attach
#define ui_kb_hide crui_kb_hide
#define ui_kb_visible crui_kb_visible
#define ui_kb_watch crui_kb_watch
#define ui_label crui_label
#define ui_label_dots crui_label_dots
#define ui_label_wrap crui_label_wrap
#define ui_pop crui_pop
#define ui_qr_sheet crui_qr_sheet
#define ui_rebuild crui_rebuild
#define ui_sheet crui_sheet
#define ui_sheet_close crui_sheet_close
#define ui_sheet_open crui_sheet_open
#define ui_size crui_size
#define ui_state_text crui_state_text
#define ui_switch_row crui_switch_row
#define ui_top crui_top
#define ui_token crui_token
#define ui_page_w crui_page_w
#define ui_page_h crui_page_h

enum { SCR_WELCOME, SCR_HOME, SCR_LIST, SCR_MSG, SCR_COMPOSE, SCR_SETTINGS, SCR_ACCT, SCR_PICK };

/* Who asked for a job: the top byte of its token. */
enum { OWN_NONE, OWN_LIST, OWN_MSG, OWN_COMPOSE, OWN_ACCT, OWN_PORTAL, OWN_IMG, OWN_PART };

#define TOKEN(own, n) (((uint32_t)(own) << 24) | ((n) & 0xFFFFFF))
#define TOKEN_OWN(t)  ((int)((t) >> 24))

/* A row of a list: a message, or the newest of a conversation. */
typedef struct {
    uint32_t uid;
    uint32_t date;
    uint32_t key;               /* the conversation */
    uint16_t flags;
    uint8_t  acct, folder;
    uint16_t count, unread;
    bool     attach;
    char     who[96];
    char     subject[124];
} cr_row_t;

typedef struct {
    lv_obj_t *root;
    int32_t   W, H;
    bool      land;
    int       stack[12];
    int       depth;
    lv_obj_t *page;             /* where the top screen is built */
    lv_obj_t *left;             /* the list beside a message, in landscape */
    uint32_t  ver_seen;
    uint32_t  gen_seen;
    bool      hidden;
    uint32_t  seq;              /* for tokens */
    int       badge;
    bool      building;

    /* the list */
    int       l_acct, l_folder; /* l_acct -1: every INBOX */
    uint32_t  l_key;            /* a conversation, 0 none */
    char      l_query[120];
    bool      l_server;         /* l_res holds the server's answer */
    cr_hdr_t *l_res;
    int       l_nres;
    bool      l_searching;
    cr_row_t *rows;
    int       nrows, caprows;
    int       l_shown;          /* rows built, more on scrolling */
    int32_t   l_scroll;
    lv_obj_t *l_list, *l_more, *l_search, *l_kb;
    uint32_t  l_token;
    bool      l_loading;

    /* the message */
    int       m_acct, m_folder;
    uint32_t  m_uid;
    cr_msg_t *msg;
    uint32_t  m_token;
    char      m_why[200];
} cr_ui_t;

#define U cr_ui_state
extern cr_ui_t U;

/* navigation */
void      ui_go(int scr);
void      ui_pop(void);
void      ui_rebuild(void);
int       ui_top(void);
uint32_t  ui_token(int own);

/* building blocks */
lv_obj_t *ui_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *ui_label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c);
lv_obj_t *ui_label_dots(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c, int32_t w);
lv_obj_t *ui_label_wrap(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c);
lv_obj_t *ui_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *ui_icon_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud);
lv_obj_t *ui_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_col(lv_obj_t *parent, int32_t w, int32_t h);         /* a scrolling column */
lv_obj_t *ui_bar(lv_obj_t *page, const char *back, const char *title, lv_event_cb_t back_cb);
lv_obj_t *ui_bar_right(lv_obj_t *bar);                             /* where its buttons go */
lv_obj_t *ui_field(lv_obj_t *parent, const char *label, const char *text, const char *hint, bool one_line);
lv_obj_t *ui_switch_row(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb);
int32_t   ui_page_w(void);
int32_t   ui_page_h(void);

/* sheets: a list of choices, a question */
typedef void (*ui_pick_cb_t)(int i, void *ud);
void ui_sheet(const char *title, const char *const *items, int n, ui_pick_cb_t cb, void *ud);
void ui_confirm(const char *text, const char *yes, bool danger, ui_pick_cb_t cb, void *ud);
void ui_sheet_close(void);
bool ui_sheet_open(void);
void ui_qr_sheet(const char *title, const char *url);

/* the keyboard, for the text areas of a page; content is what shrinks */
void ui_kb_attach(lv_obj_t *page, lv_obj_t *content);
void ui_kb_watch(lv_obj_t *ta);
void ui_kb_hide(void);
bool ui_kb_visible(void);

/* words */
const char *ui_state_text(const cr_acct_t *a);
const char *ui_err_text(int kind);
void        ui_size(char *out, size_t n, uint32_t bytes);
const char *ui_acct_name(int i);

/* screens */
void cr_home_build(lv_obj_t *page);
void cr_welcome_build(lv_obj_t *page);
void cr_list_build(lv_obj_t *page);
void cr_list_open(int acct, int folder);
void cr_list_job(cr_job_t *j);
void cr_list_free(void);
void cr_list_refresh(void);

void cr_msg_open(int acct, int folder, uint32_t uid);
void cr_msg_build(lv_obj_t *page);
void cr_msg_job(cr_job_t *j);
void cr_img_job(cr_job_t *j);
void cr_part_job(cr_job_t *j);
void cr_msg_close(void);
void cr_msg_refresh(void);

enum { CR_REPLY, CR_REPLY_ALL, CR_FORWARD };
void cr_compose_new(int acct, const char *to);
void cr_compose_reply(const cr_msg_t *m, int mode);
void cr_compose_draft(const cr_msg_t *m);
void cr_compose_build(lv_obj_t *page);
void cr_compose_job(cr_job_t *j);
bool cr_compose_back(void);
void cr_compose_free(void);

void cr_settings_build(lv_obj_t *page);
void cr_acct_edit(int i);                      /* -1: a new one */
void cr_acct_build(lv_obj_t *page);
void cr_acct_job(cr_job_t *j);
bool cr_acct_back(void);

typedef void (*cr_pick_file_cb_t)(const char *path);
void cr_pick_file(cr_pick_file_cb_t cb);
void cr_pick_build(lv_obj_t *page);
bool cr_pick_back(void);

/* actions on a message, from the list or the message itself */
void cr_act_flags(int acct, int folder, uint32_t uid, uint16_t add, uint16_t del);
void cr_act_move(int acct, int folder, uint32_t uid, int dest);
void cr_act_archive(int acct, int folder, uint32_t uid);
void cr_act_delete(int acct, int folder, uint32_t uid);
void cr_act_move_pick(int acct, int folder, uint32_t uid);
