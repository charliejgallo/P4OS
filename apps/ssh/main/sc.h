/*
 * P4OS - SSH client: what the app's files share.
 *
 *   sc_app.c    the app: the list of computers, the editor, the questions
 *               (host key, password) and the terminal screen (LVGL's task)
 *   sc_store.c  ssh/hosts.txt and ssh/known_hosts in the app's data folder,
 *               and the board's own key (an Ed25519 seed in preferences)
 *   sc_conn.c   the session: SSH-2 over aos_hal_tcp_* in a thread of its
 *               own, key exchange, authentication, one shell channel
 *   sc_term.c   the terminal: an xterm-like screen fed by what comes in
 *   sc_view.c   the terminal drawn with LVGL, and its scrolling
 *   sc_kbd.c    the keyboard on the screen (Esc, Tab, Ctrl, Alt, arrows,
 *               F1-F12) and the USB keyboard's keys, both as the bytes a
 *               terminal sends
 *
 * The crypto is the firmware's, lent to the apps: Monocypher (X25519,
 * Ed25519), chacha20-poly1305@openssh.com (aos_ssh_chacha.h), SSH's wire
 * types and the host key check (aos_ssh_wire.h), SHA-256 (aos_hal).
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SC_TAG "sshc"

void *sc_psram(size_t n);           /* zeroed; PSRAM first */

static inline void sc_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memmove(dst, src, n);
    dst[n] = '\0';
}

/* ---- the saved computers and what is known of them (sc_store.c) ---- */

typedef struct {
    char name[40];
    char host[96];
    int  port;
    char user[40];
} sc_host_t;

#define SC_MAX_HOSTS 24

typedef struct {
    sc_host_t h[SC_MAX_HOSTS];
    int count;
} sc_hosts_t;

void sc_hosts_load(sc_hosts_t *hs);
bool sc_hosts_save(const sc_hosts_t *hs);

/* known_hosts: 1 the key is the one kept for host:port, 0 none is kept,
 * -1 another one is (the computer changed, or someone is in the middle) */
int  sc_known_check(const char *host, int port, const uint8_t *blob, size_t n);
bool sc_known_add(const char *host, int port, const uint8_t *blob, size_t n);
void sc_known_forget(const char *host, int port);

/* The board's key for logging in: Ed25519, made the first time. sk is
 * Monocypher's 64-byte secret (seed and public key), pub the public key. */
bool sc_key(uint8_t sk[64], uint8_t pub[32]);
/* "ssh-ed25519 AAAA... p4os@<name>", the line for authorized_keys; it is
 * also written to ssh/id_ed25519.pub in the data folder */
bool sc_key_line(char *out, size_t n);
/* "SHA256:..." of a key's wire form, as ssh-keygen -l prints it */
void sc_fingerprint(const uint8_t *blob, size_t n, char *out, size_t on);

/* ---- the terminal (sc_term.c) ---- */

#define SC_COLS_MAX   160
#define SC_ROWS_MAX   80
#define SC_SCROLLBACK 1000

/* colours: 0-255 xterm's, SC_DEF the default of fg or bg */
#define SC_DEF 0xFFFF
enum { SC_A_BOLD = 1, SC_A_UNDER = 2, SC_A_REV = 4, SC_A_DIM = 8, SC_A_INVIS = 16 };

typedef struct {
    uint32_t ch;                /* a code point; 0 is a blank */
    uint16_t fg, bg;
    uint8_t  attr;
} sc_cell_t;

typedef struct sc_term sc_term_t;

sc_term_t *sc_term_new(int cols, int rows);
void       sc_term_free(sc_term_t *t);
void       sc_term_lock(sc_term_t *t);
void       sc_term_unlock(sc_term_t *t);
/* everything below but new/free under the lock */
void       sc_term_feed(sc_term_t *t, const uint8_t *d, size_t n);
void       sc_term_resize(sc_term_t *t, int cols, int rows);
void       sc_term_size(sc_term_t *t, int *cols, int *rows);
/* a row as shown with the view 'back' lines up in the scrollback (0: the
 * screen); NULL past what there is */
const sc_cell_t *sc_term_row(sc_term_t *t, int y, int back);
int        sc_term_scrollback(sc_term_t *t);     /* lines kept above the screen */
bool       sc_term_cursor(sc_term_t *t, int *x, int *y);    /* false: hidden */
/* rows changed since the last call, as a bitmask over 0..rows-1 (all: -1
 * in every word), cleared by it; true if any */
bool       sc_term_take_dirty(sc_term_t *t, uint32_t dirty[(SC_ROWS_MAX + 31) / 32]);
/* what the terminal answers the computer (cursor position, who it is) */
size_t     sc_term_take_reply(sc_term_t *t, uint8_t *out, size_t cap);
bool       sc_term_app_cursor(sc_term_t *t);     /* arrows as ESC O A */
bool       sc_term_bracketed_paste(sc_term_t *t);
uint32_t   sc_term_bells(sc_term_t *t);
void       sc_term_title(sc_term_t *t, char *out, size_t n);

/* ---- the session (sc_conn.c) ---- */

typedef enum {
    SC_ST_CONNECTING = 0,
    SC_ST_ASK_HOSTKEY,          /* the UI shows the key and answers */
    SC_ST_ASK_PASSWORD,         /* the UI asks for it and answers */
    SC_ST_LIVE,
    SC_ST_CLOSED,               /* detail says why */
} sc_state_t;

typedef struct sc_sess sc_sess_t;

sc_sess_t *sc_sess_start(const sc_host_t *h, sc_term_t *term);
/* Stops the thread (and waits for it), closes the socket, frees it all. */
void       sc_sess_stop(sc_sess_t *s);
sc_state_t sc_sess_state(sc_sess_t *s, char *detail, size_t n);
/* changes with every question, so the UI asks again even when the state
 * looks the same (a wrong password asked for again in a few ms) */
uint32_t   sc_sess_ask_seq(sc_sess_t *s);

typedef struct {
    char type[24];              /* "ED25519", "RSA 3072" */
    char fp[56];
    int  known;                 /* sc_known_check: 0 new, -1 changed */
} sc_hostkey_q_t;
void sc_sess_hostkey(sc_sess_t *s, sc_hostkey_q_t *q);
void sc_sess_answer_hostkey(sc_sess_t *s, bool yes);
/* user@host, or the question the computer wrote (a code, say), for the
 * sheet; *retry: the password before did not do */
void sc_sess_password_prompt(sc_sess_t *s, char *out, size_t n, bool *retry);
void sc_sess_answer_password(sc_sess_t *s, const char *pw);     /* NULL: cancel */

/* typed bytes, as a terminal sends them */
void sc_sess_send(sc_sess_t *s, const void *d, size_t n);
void sc_sess_resize(sc_sess_t *s, int cols, int rows);
/* Puts the board's key in the computer's ~/.ssh/authorized_keys, over a
 * second channel; the result comes as a line in the terminal. */
void sc_sess_install_key(sc_sess_t *s);
bool sc_sess_key_used(sc_sess_t *s);    /* logged in with the board's key */
/* a line for the terminal (a key for _()), once; NULL if none */
const char *sc_sess_take_note(sc_sess_t *s);

/* ---- the terminal on the screen (sc_view.c) ---- */

typedef struct sc_view sc_view_t;
sc_view_t *sc_view_create(lv_obj_t *parent, sc_term_t *term);
void       sc_view_delete(sc_view_t *v);
lv_obj_t  *sc_view_obj(sc_view_t *v);
/* sets the view's size; the terminal's columns and rows that fit in it */
void       sc_view_set_size(sc_view_t *v, int32_t w, int32_t h, int *cols, int *rows);
void       sc_view_refresh(sc_view_t *v);       /* from a timer: redraw what changed */
void       sc_view_to_bottom(sc_view_t *v);     /* typing goes back to the live screen */
void       sc_view_on_tap(sc_view_t *v, void (*cb)(void *), void *user);

/* ---- keys (sc_kbd.c) ---- */

/* The bytes a key makes, as xterm sends them. key: a code point, or one of
 * these; mods SC_M_*. app_cursor: the computer asked for ESC O arrows. */
enum {
    SC_K_UP = 0x110001, SC_K_DOWN, SC_K_RIGHT, SC_K_LEFT, SC_K_HOME, SC_K_END, SC_K_PGUP, SC_K_PGDN,
    SC_K_INS, SC_K_DEL, SC_K_ESC, SC_K_TAB, SC_K_BTAB, SC_K_ENTER, SC_K_BS, SC_K_F1,   /* F1..F12 follow */
};
enum { SC_M_SHIFT = 1, SC_M_CTRL = 2, SC_M_ALT = 4 };
size_t sc_key_bytes(uint32_t key, uint8_t mods, bool app_cursor, char out[16]);

typedef struct sc_kbd sc_kbd_t;
/* A keyboard at the bottom of 'parent'; each key goes to send(user, key, mods). */
sc_kbd_t *sc_kbd_create(lv_obj_t *parent, int32_t w, bool land,
                        void (*send)(void *user, uint32_t key, uint8_t mods),
                        void (*on_hide)(void *user), void *user);
int32_t   sc_kbd_height(const sc_kbd_t *k);
void      sc_kbd_delete(sc_kbd_t *k);
/* A key of the USB keyboard (aos_ui_hwkbd_handler) as a key and mods. */
bool      sc_hwkey(uint32_t key, uint8_t hid_mods, uint32_t *out_key, uint8_t *out_mods);
