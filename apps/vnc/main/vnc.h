/*
 * P4OS - VNC viewer: what the app's files share.
 *
 *   vnc.c        the app: the list of computers, the editor, the viewer and
 *                its gestures (LVGL's task)
 *   vnc_cfg.c    vnc.txt, the saved computers
 *   vnc_rfb.c    the session: RFB over aos_hal_tcp_* in the app's worker,
 *                the handshake, the encodings, the input going out
 *   vnc_fb.c     the remote screen in PSRAM, and the view of it scaled to
 *                the board's screen
 *   vnc_kbd.c    the on-screen keyboard (Ctrl, Alt, Cmd, Esc, F1-F12...)
 *                and the keysyms of the USB keyboard's keys
 *   vnc_des.c    DES for the VNC password
 *   tinfl.c      inflate (miniz, MIT), for ZRLE and Tight
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VNC_TAG "vnc"

/* PSRAM first (docs/MEMORY.md); plain malloc in the simulator */
void *vnc_psram(size_t n);

/* src into dst, cut to fit (snprintf("%s") warns on every possible cut) */
static inline void vnc_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memmove(dst, src, n);
    dst[n] = '\0';
}

/* ---- the saved computers (vnc_cfg.c) ------------------------------------- */

/* A rectangle of the server's screen, in its pixels */
typedef struct {
    int x, y, w, h;
} vnc_rect_t;

enum { VNC_ENC_AUTO = 0, VNC_ENC_TIGHT, VNC_ENC_ZRLE, VNC_ENC_HEXTILE, VNC_ENC_RAW, VNC_ENC_COUNT };

typedef struct {
    char name[48];
    char host[96];
    int  port;
    char pass[64];              /* VNC uses the first 8 characters */
    int  enc;                   /* VNC_ENC_* */
    int  depth;                 /* 16 or 24 */
    int  quality;               /* Tight's JPEG, 0-9 */
    bool view_only;
    vnc_rect_t zone;            /* the part shown (one monitor of several); w 0: all of it */
} vnc_server_t;

#define VNC_MAX_SERVERS 16

typedef struct {
    vnc_server_t s[VNC_MAX_SERVERS];
    int          count;
} vnc_cfg_t;

const char *vnc_cfg_path(void);
uint32_t    vnc_cfg_stamp(void);        /* changes when the file does */
void        vnc_cfg_load(vnc_cfg_t *cfg);
bool        vnc_cfg_save(const vnc_cfg_t *cfg);
void        vnc_server_defaults(vnc_server_t *s);
const char *vnc_enc_name(int enc);

/* ---- the remote screen and the view of it (vnc_fb.c) ---------------------- */

/* The remote screen as it is kept: RGB565 in LVGL's byte order, in PSRAM.
 * A screen too big for the budget (a Retina Mac's 2880 x 1800, a 5K) is kept
 * at a half or a quarter: every write skips the pixels in between, so the
 * decoders never know. Coordinates outside are always the server's: the
 * writes move them by (zx, zy) and cut what falls outside the zone. */
typedef struct {
    uint16_t *px;
    int       zx, zy;           /* the server pixel kept at (0, 0): a zone's corner */
    int       rw, rh;           /* the size kept, before the shift: the zone's */
    int       w, h;             /* what is kept: rw >> shift */
    int       shift;
    /* what changed since the last present, in kept pixels: x0 < x1 if any */
    int       dx0, dy0, dx1, dy1;
} vnc_fb_t;

bool vnc_fb_alloc(vnc_fb_t *fb, int rw, int rh);
void vnc_fb_free(vnc_fb_t *fb);
void vnc_fb_fill(vnc_fb_t *fb, int x, int y, int w, int h, uint16_t c);
/* one row of w pixels at (x, y) */
void vnc_fb_row(vnc_fb_t *fb, int x, int y, int w, const uint16_t *src);
/* w x h pixels, rows stride_px apart */
void vnc_fb_rect(vnc_fb_t *fb, int x, int y, int w, int h, const uint16_t *src, int stride_px);
/* false: the source is not all in the zone; the caller asks for dx..dy again */
bool vnc_fb_copy(vnc_fb_t *fb, int sx, int sy, int dx, int dy, int w, int h);
static inline bool vnc_fb_skip_row(const vnc_fb_t *fb, int y)
{
    return ((y - fb->zy) & ((1 << fb->shift) - 1)) != 0;
}

/* Where the view looks: scale is view pixels per server pixel, (ox, oy)
 * the server pixel at the view's top left corner. */
typedef struct {
    int      vw, vh;
    float    scale;
    float    ox, oy;
    uint32_t gen;
} vnc_view_t;

typedef struct vnc_render vnc_render_t;
vnc_render_t *vnc_render_new(void);
void          vnc_render_free(vnc_render_t *r);
/* Renders the view pixels [x0,x1) x [y0,y1) of 'v' into dst (vw x vh). */
void          vnc_render(vnc_render_t *r, const vnc_fb_t *fb, const vnc_view_t *v,
                         uint16_t *dst, int x0, int y0, int x1, int y1);
/* The view rectangle that kept pixels [x0,x1) x [y0,y1) land on; false if
 * none of it is in view. */
bool          vnc_view_rect(const vnc_fb_t *fb, const vnc_view_t *v, int x0, int y0, int x1, int y1,
                            int *vx0, int *vy0, int *vx1, int *vy1);

/* ---- the session (vnc_rfb.c) ---------------------------------------------- */

typedef enum {
    VNC_ST_CONNECTING = 0,
    VNC_ST_AUTH,
    VNC_ST_LIVE,
    VNC_ST_ERROR,               /* detail says why; the session is over */
} vnc_state_t;

typedef struct vnc_sess vnc_sess_t;

/* Starts the session in the app's worker. NULL: no memory, or the worker is
 * busy (a session not yet stopped). */
vnc_sess_t *vnc_sess_start(const vnc_server_t *srv);
/* Stops the worker, closes the socket and frees everything. */
void        vnc_sess_stop(vnc_sess_t *s);

vnc_state_t vnc_sess_state(vnc_sess_t *s, char *detail, size_t n);
/* The screen shown (the zone, or all of it) and the server's name; false
 * before ServerInit. Every coordinate the UI uses is the zone's. */
bool        vnc_sess_desktop(vnc_sess_t *s, int *w, int *h, char *name, size_t n);

#define VNC_MAX_SCREENS 8
/* The server's whole screen, the zone shown in it and the monitors the
 * server listed (ExtendedDesktopSize; one that does not list them gives
 * 0). Returns how many went into scr. */
int         vnc_sess_screens(vnc_sess_t *s, int *full_w, int *full_h, vnc_rect_t *zone,
                             vnc_rect_t *scr, int max);
/* Shows only z of the server's screen (w 0: all of it). Takes effect when
 * the worker next looks, with a whole update of the new zone. */
void        vnc_sess_set_zone(vnc_sess_t *s, const vnc_rect_t *z);
/* A number that changes with every new zone. */
uint32_t    vnc_sess_zone_gen(vnc_sess_t *s);
/* A line of numbers for the overlay: encoding, fps, kbit/s. */
void        vnc_sess_stats(vnc_sess_t *s, char *out, size_t n);
/* Bells rung by the server so far. */
uint32_t    vnc_sess_bells(vnc_sess_t *s);
/* The remote screen kept at a half or a quarter (0 = whole). */
int         vnc_sess_shift(vnc_sess_t *s);

/* What the UI asks the view to be; the worker renders it. */
void        vnc_sess_set_view(vnc_sess_t *s, const vnc_view_t *v);
/* The newest picture for the canvas. *changed: a new buffer (point the
 * canvas at it and redraw it all); otherwise the area in *area changed.
 * false: nothing new. */
bool        vnc_sess_take(vnc_sess_t *s, const uint16_t **px, int *w, int *h, bool *changed,
                          lv_area_t *area);

/* Input going out; coordinates are the server's. buttons: bit 0 left,
 * 1 middle, 2 right, 3/4 wheel up/down, 5/6 wheel left/right. */
void        vnc_sess_pointer(vnc_sess_t *s, int x, int y, uint8_t buttons);
void        vnc_sess_key(vnc_sess_t *s, uint32_t keysym, bool down);
void        vnc_sess_refresh(vnc_sess_t *s);    /* the whole screen again */
/* In the background: no more updates asked for; back, the whole screen. */
void        vnc_sess_pause(vnc_sess_t *s, bool paused);

/* ---- the keyboard (vnc_kbd.c) --------------------------------------------- */

/* X11 keysyms (RFB sends these) */
enum {
    XK_BackSpace = 0xff08, XK_Tab = 0xff09, XK_Return = 0xff0d, XK_Escape = 0xff1b,
    XK_Home = 0xff50, XK_Left = 0xff51, XK_Up = 0xff52, XK_Right = 0xff53, XK_Down = 0xff54,
    XK_Page_Up = 0xff55, XK_Page_Down = 0xff56, XK_End = 0xff57, XK_Insert = 0xff63,
    XK_F1 = 0xffbe, XK_Shift_L = 0xffe1, XK_Control_L = 0xffe3, XK_Meta_L = 0xffe7,
    XK_Alt_L = 0xffe9, XK_Super_L = 0xffeb, XK_Delete = 0xffff,
};

/* The keysym of a Unicode character. */
uint32_t vnc_keysym_char(uint32_t cp);

typedef struct vnc_kbd vnc_kbd_t;
/* A keyboard at the bottom of 'parent', w wide; it sends to *sess (read at
 * every key, so the pointer may change under it). Its own key to hide it
 * calls on_hide(user). */
vnc_kbd_t *vnc_kbd_create(lv_obj_t *parent, int32_t w, bool land, vnc_sess_t **sess,
                          void (*on_hide)(void *), void *user);
int32_t    vnc_kbd_height(const vnc_kbd_t *k);
lv_obj_t  *vnc_kbd_obj(const vnc_kbd_t *k);
void       vnc_kbd_delete(vnc_kbd_t *k);

/* A key of the USB keyboard (aos_ui_hwkbd_handler): true if it was sent. */
bool vnc_hwkey(vnc_sess_t *s, uint32_t key, uint8_t mods);
