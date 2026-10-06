/*
 * P4OS - VNC: see and use a computer from the board.
 *
 * Three screens:
 *
 *   list     the saved computers (vnc.txt, vnc_cfg.c): a tap connects, the
 *            pencil edits; the portal's page edits the same file, and the
 *            list follows it
 *   editor   name, address, port, password (kept on the card in the clear,
 *            and it says so), encoding, colours, look-only
 *   viewer   the remote screen, the whole board's screen, through an
 *            lv_canvas that points at the session's view buffer
 *            (vnc_rfb.c). A bar that hides by itself: close, keyboard,
 *            touch or trackpad, fit / fill / 1:1, turn the screen, refresh.
 *
 * Input in the viewer:
 *
 *   touch      the finger is the mouse: a tap clicks there, a drag drags
 *              with the button down, two fingers tapped (or a long press)
 *              click right. Two fingers pinch to zoom; moved together they
 *              pan the zoomed view, or turn the wheel when there is nothing
 *              to pan.
 *   trackpad   the pointer is relative, drawn as a ring: one finger moves
 *              it, a tap clicks, a long press then a drag drags, two
 *              fingers scroll, a pinch zooms. The view follows the pointer.
 *   USB mouse  the system's arrow (aos_hwmouse.c) is the remote pointer:
 *              its position and its left button. The right button reaches
 *              the app as "back", which the viewer turns into a right
 *              click while a mouse is in use. The wheel and the middle
 *              button do not reach apps yet (a request to the firmware).
 *   keyboard   the USB keyboard's keys go straight through
 *              (aos_ui_hwkbd_handler), and there is one on the screen with
 *              Esc, Tab, Ctrl, Alt, Cmd, arrows and F1-F12 (vnc_kbd.c).
 *
 * The viewer takes the system's back swipe away (NO_SWIPE while it is up):
 * a drag from the left edge is the remote mouse's. The bottom edge still
 * goes home; the app stays alive with its connection (KEEP).
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "vnc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMER_MS        15
#define BAR_MS          5000
#define STATS_MS        1000
#define CFG_POLL_MS     2000
#define KEY_MODE        "vnc_mode"
#define KEY_INPUT       "vnc_input"
#define MAX_ZOOM        4.0f
#define SCROLL_STEP_PX  36.0f
#define TAP2_MS         350         /* a two-finger touch shorter than this is a tap */
#define MOUSE_FRESH_MS  15000       /* "back" is the mouse's right button for this long */

enum { SCR_LIST = 0, SCR_EDIT, SCR_VIEW };
enum { MODE_FIT = 0, MODE_FILL, MODE_ONE, MODE_FREE };
enum { IN_TOUCH = 0, IN_TRACKPAD };
enum { G2_UNDECIDED = 0, G2_ZOOM, G2_PAN, G2_SCROLL };

/* A monitor on its stand, a desktop with a window on it, and a pointer. */
static const uint8_t VNC_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 30, 34, 6, 3, AIC_C_TEXT, 255),            /* base   */
    AIC_RECT(AIC_CENTER, 0, 22, 10, 12, 0, AIC_C_TEXT, 255),           /* neck   */
    AIC_RECT(AIC_CENTER, 0, -6, 70, 48, 8, AIC_C_TEXT, 255),           /* frame  */
    AIC_INTO,
    AIC_RECT(AIC_CENTER, 0, 0, 62, 40, 4, AIC_C_LIT(0x1E3A8A), 255),   /* screen */
    AIC_GRAD(AIC_C_LIT(0x0EA5E9), AIC_GRAD_VER),
    AIC_INTO,
    AIC_RECT(AIC_TOP_LEFT, 6, 6, 30, 20, 3, AIC_C_TEXT, 230),          /* window */
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, 0, 0, 30, 5, 0, AIC_C_LIT(0x93C5FD), 255),   /* its title bar */
    AIC_OUT,
    AIC_RECT(AIC_TOP_LEFT, 40, 18, 8, 14, 1, AIC_C_BG, 255),           /* pointer */
    AIC_ROT(-300),
    AIC_OUT,
    AIC_OUT,
    AIC_END
};

static struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    int32_t     W, H;
    bool        land;
    int         screen;
    lv_timer_t *timer;
    int         rot_restore;
    bool        hidden;

    vnc_cfg_t   cfg;
    uint32_t    cfg_stamp;
    uint64_t    cfg_poll;

    /* editor */
    struct {
        int           index;        /* -1: a new one */
        vnc_server_t  s;
        lv_obj_t     *page, *form, *kb;
        lv_obj_t     *name, *host, *port, *pass;
        lv_obj_t     *enc_row, *depth_row, *view_sw, *del_btn;
        uint64_t      del_armed;
    } ed;

    /* viewer */
    struct {
        bool          on;
        bool          leave;
        int           index;
        vnc_server_t  srv;
        vnc_sess_t   *sess;
        lv_obj_t     *obj, *canvas, *msg, *msg_btns, *bar, *name, *info, *handle, *ring;
        lv_obj_t     *in_g, *mode_g;
        lv_obj_t     *zpick, *mark;
        uint32_t      zone_gen;
        vnc_kbd_t    *kbd;
        bool          kbd_on;
        bool          bar_on;
        uint64_t      bar_until;
        int           last_state;
        uint64_t      stats_t;
        uint32_t      bells;
        const uint16_t *shown;
        int           shown_w, shown_h;

        int           mode;
        int           input;
        int           rw, rh;
        vnc_view_t    view;
        uint32_t      gen;

        /* the remote pointer */
        float         px, py;
        uint8_t       buttons;
        bool          dragging;
        bool          pending_right;

        /* two fingers */
        int           g2;
        float         g2_scale, g2_dx, g2_dy, g2_cx, g2_cy, g2_scroll_x, g2_scroll_y;
        uint32_t      g2_t0;

        /* the USB mouse */
        lv_indev_t   *mouse;
        int32_t       mx, my;
        bool          mdown;
        uint64_t      mouse_ms;
    } vw;
} A;

static void build_ui(void);
static void open_viewer(int index);
static void close_viewer(void);

/* ---- small pieces ------------------------------------------------------------ */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud, lv_obj_t **label)
{
    lv_obj_t *b = box(parent, AOS_UI_TAP_MIN, AOS_UI_TAP_MIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    if (label) *label = l;
    return b;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 24, 0);
    lv_obj_set_style_bg_color(c, on ? AOS_C_TEXT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return c;
}

static void set_text(lv_obj_t *l, const char *t)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t)) lv_label_set_text(l, t);
}

/* ---- the list ---------------------------------------------------------------- */

static void connect_cb(lv_event_t *e)
{
    open_viewer((int)(intptr_t)lv_event_get_user_data(e));
}

static void open_editor(int index);

static void edit_cb(lv_event_t *e)
{
    open_editor((int)(intptr_t)lv_event_get_user_data(e));
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    if (A.cfg.count >= VNC_MAX_SERVERS) {
        aos_ui_toast(_("No entran más computadoras"), 2000);
        return;
    }
    open_editor(-1);
}

static void build_list(void)
{
    lv_obj_t *page = box(A.root, A.W, A.H);
    lv_obj_set_style_bg_color(page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

    lv_obj_t *title = aos_label(page, "VNC", aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 36);
    aos_make_decorative(title);
    lv_obj_t *add = round_btn(page, AOS_SYM_PLUS, add_cb, NULL, NULL);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 22);

    const int32_t top = 130;
    lv_obj_t *list = box(page, A.W, A.H - top);
    lv_obj_set_pos(list, 0, top);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(list, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(list, 40, 0);
    lv_obj_set_style_pad_gap(list, 16, 0);

    int cols = A.land ? 2 : 1;
    int32_t cw = (A.W - 2 * AOS_UI_PAD - (cols - 1) * 16) / cols;
    for (int i = 0; i < A.cfg.count; i++) {
        const vnc_server_t *s = &A.cfg.s[i];
        lv_obj_t *card = box(list, cw, 132);
        lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, connect_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = aos_label(card, AOS_SYM_MONITOR, &aos_sym_44, AOS_C_ACCENT);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 24, 0);
        aos_make_decorative(ic);

        char safe[96];
        aos_text_safe(safe, sizeof safe, s->name);
        lv_obj_t *nm = aos_label(card, safe, aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(nm, cw - 96 - 110, lv_font_get_line_height(aos_font_body));
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 92, 26);
        aos_make_decorative(nm);

        char where[160];
        snprintf(where, sizeof where, "%s:%d%s%s", s->host, s->port, s->view_only ? "  ·  " : "",
                 s->view_only ? _("sólo mirar") : "");
        lv_obj_t *hl = aos_label(card, where, aos_font_caption, AOS_C_DIM);
        lv_label_set_long_mode(hl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(hl, cw - 96 - 110, lv_font_get_line_height(aos_font_caption));
        lv_obj_align(hl, LV_ALIGN_TOP_LEFT, 92, 76);
        aos_make_decorative(hl);

        lv_obj_t *ed = round_btn(card, AOS_SYM_PENCIL, edit_cb, (void *)(intptr_t)i, NULL);
        lv_obj_align(ed, LV_ALIGN_RIGHT_MID, -20, 0);
    }
    if (!A.cfg.count) {
        lv_obj_t *l = aos_label(list, _("Ninguna computadora todavía.\nAgregala con + o desde la página VNC del portal.\n\nEn la Mac: Compartir pantalla, con «Los visores VNC pueden controlar la pantalla con contraseña»."),
                                aos_font_body, AOS_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, A.W - 2 * AOS_UI_PAD);
        aos_make_decorative(l);
    }
}

/* ---- the editor ---------------------------------------------------------------- */

static const char *ENC_LABELS[VNC_ENC_COUNT] = { N_("Automática"), "Tight", "ZRLE", "Hextile", "Raw" };

static void ed_chips(void);

static void enc_cb(lv_event_t *e)
{
    A.ed.s.enc = (int)(intptr_t)lv_event_get_user_data(e);
    ed_chips();
}

static void depth_cb(lv_event_t *e)
{
    A.ed.s.depth = (int)(intptr_t)lv_event_get_user_data(e);
    ed_chips();
}

static void ed_chips(void)
{
    lv_obj_clean(A.ed.enc_row);
    for (int i = 0; i < VNC_ENC_COUNT; i++) {
        chip(A.ed.enc_row, i ? ENC_LABELS[i] : _(ENC_LABELS[i]), A.ed.s.enc == i, enc_cb, (void *)(intptr_t)i);
    }
    lv_obj_clean(A.ed.depth_row);
    chip(A.ed.depth_row, _("16 bits (rápido)"), A.ed.s.depth != 24, depth_cb, (void *)(intptr_t)16);
    chip(A.ed.depth_row, _("24 bits"), A.ed.s.depth == 24, depth_cb, (void *)(intptr_t)24);
}

static void ed_kb_show(lv_obj_t *ta)
{
    if (!A.ed.kb) return;
    if (ta) {
        lv_keyboard_set_textarea(A.ed.kb, ta);
        lv_keyboard_set_mode(A.ed.kb, ta == A.ed.port ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(A.ed.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(A.ed.form, A.H - lv_obj_get_height(A.ed.kb));
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(A.ed.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(A.ed.form, A.H);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    ed_kb_show(lv_event_get_target(e));
}

static void kb_cb(lv_event_t *e)
{
    (void)e;
    ed_kb_show(NULL);
}

static lv_obj_t *field(lv_obj_t *form, const char *label, const char *value, int max)
{
    lv_obj_t *l = aos_label(form, label, aos_font_caption, AOS_C_DIM);
    aos_make_decorative(l);
    lv_obj_t *ta = lv_textarea_create(form);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_text(ta, value);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 20, 0);
    lv_obj_set_style_pad_hor(ta, 24, 0);
    lv_obj_set_style_pad_ver(ta, 16, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static lv_obj_t *row_box(lv_obj_t *form)
{
    lv_obj_t *r = box(form, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 12, 0);
    return r;
}

static void ed_close(void)
{
    A.screen = SCR_LIST;
    build_ui();
}

static void ed_back_cb(lv_event_t *e)
{
    (void)e;
    ed_close();
}

static void ed_save_cb(lv_event_t *e)
{
    (void)e;
    vnc_server_t s = A.ed.s;
    snprintf(s.name, sizeof s.name, "%s", lv_textarea_get_text(A.ed.name));
    snprintf(s.host, sizeof s.host, "%s", lv_textarea_get_text(A.ed.host));
    snprintf(s.pass, sizeof s.pass, "%s", lv_textarea_get_text(A.ed.pass));
    s.port = atoi(lv_textarea_get_text(A.ed.port));
    s.view_only = lv_obj_has_state(A.ed.view_sw, LV_STATE_CHECKED);
    /* trim the host; a name with brackets would break the file's blocks */
    char *h = s.host;
    while (*h == ' ') h++;
    memmove(s.host, h, strlen(h) + 1);
    for (char *p = s.host + strlen(s.host); p > s.host && p[-1] == ' ';) *--p = '\0';
    for (char *p = s.name; *p; p++) if (*p == '[' || *p == ']') *p = ' ';
    if (!s.host[0]) {
        aos_ui_toast(_("Falta la dirección"), 2000);
        return;
    }
    if (s.port <= 0 || s.port > 65535) s.port = 5900;
    if (!s.name[0]) vnc_copy(s.name, sizeof s.name, s.host);
    if (A.ed.index < 0) {
        if (A.cfg.count >= VNC_MAX_SERVERS) return;
        A.cfg.s[A.cfg.count++] = s;
    } else {
        A.cfg.s[A.ed.index] = s;
    }
    if (!vnc_cfg_save(&A.cfg)) {
        aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2500);
        return;
    }
    A.cfg_stamp = vnc_cfg_stamp();
    aos_ui_toast(_("Guardada"), 1200);
    ed_close();
}

static void ed_del_cb(lv_event_t *e)
{
    (void)e;
    uint64_t now = aos_hal_uptime_ms();
    if (now - A.ed.del_armed > 3000) {
        A.ed.del_armed = now;
        lv_obj_t *l = lv_obj_get_child(A.ed.del_btn, 0);
        if (l) lv_label_set_text(l, _("Tocá otra vez para borrar"));
        return;
    }
    for (int i = A.ed.index; i < A.cfg.count - 1; i++) A.cfg.s[i] = A.cfg.s[i + 1];
    A.cfg.count--;
    vnc_cfg_save(&A.cfg);
    A.cfg_stamp = vnc_cfg_stamp();
    ed_close();
}

static void build_editor(void)
{
    lv_obj_t *page = box(A.root, A.W, A.H);
    A.ed.page = page;
    lv_obj_set_style_bg_color(page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

    lv_obj_t *form = box(page, A.W, A.H);
    A.ed.form = form;
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(form, LV_DIR_VER);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    int32_t side = A.land ? A.W / 6 : AOS_UI_PAD;
    lv_obj_set_style_pad_hor(form, side, 0);
    lv_obj_set_style_pad_top(form, 130, 0);
    lv_obj_set_style_pad_bottom(form, 60, 0);
    lv_obj_set_style_pad_row(form, 10, 0);

    lv_obj_t *bk = round_btn(page, AOS_SYM_CHEVRON_LEFT, ed_back_cb, NULL, NULL);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 22);
    lv_obj_t *t = aos_label(page, A.ed.index < 0 ? _("Nueva computadora") : _("Computadora"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 20, 36);
    aos_make_decorative(t);

    char port[8];
    snprintf(port, sizeof port, "%d", A.ed.s.port);
    A.ed.name = field(form, _("Nombre"), A.ed.s.name, sizeof A.ed.s.name - 1);
    A.ed.host = field(form, _("Dirección (IP o nombre)"), A.ed.s.host, sizeof A.ed.s.host - 1);
    lv_textarea_set_placeholder_text(A.ed.host, "192.168.0.20");
    A.ed.port = field(form, _("Puerto"), port, 5);
    lv_textarea_set_accepted_chars(A.ed.port, "0123456789");
    A.ed.pass = field(form, _("Contraseña VNC"), A.ed.s.pass, sizeof A.ed.s.pass - 1);
    lv_textarea_set_password_mode(A.ed.pass, true);
    lv_obj_t *warn = aos_label(form, _("Queda guardada en la tarjeta (vnc.txt) y se puede leer. VNC usa sólo los primeros 8 caracteres."),
                               aos_font_caption, AOS_C_ORANGE);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(warn, lv_pct(100));
    aos_make_decorative(warn);

    lv_obj_t *l = aos_label(form, _("Codificación"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_margin_top(l, 14, 0);
    aos_make_decorative(l);
    A.ed.enc_row = row_box(form);
    l = aos_label(form, _("Colores"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_margin_top(l, 14, 0);
    aos_make_decorative(l);
    A.ed.depth_row = row_box(form);
    ed_chips();

    lv_obj_t *vr = box(form, lv_pct(100), 80);
    lv_obj_set_style_margin_top(vr, 14, 0);
    l = aos_label(vr, _("Sólo mirar (no manda mouse ni teclado)"), aos_font_body, AOS_C_TEXT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, A.W - 2 * side - 120);
    aos_make_decorative(l);
    A.ed.view_sw = lv_switch_create(vr);
    lv_obj_set_size(A.ed.view_sw, 96, 54);
    lv_obj_align(A.ed.view_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (A.ed.s.view_only) lv_obj_add_state(A.ed.view_sw, LV_STATE_CHECKED);

    lv_obj_t *btns = row_box(form);
    lv_obj_set_style_margin_top(btns, 20, 0);
    lv_obj_t *sv = aos_button(btns, _("Guardar"), AOS_C_ACCENT, ed_save_cb, NULL);
    (void)sv;
    if (A.ed.index >= 0) {
        A.ed.del_btn = aos_button(btns, _("Borrar"), AOS_C_RED, ed_del_cb, NULL);
    } else {
        A.ed.del_btn = NULL;
    }

    A.ed.kb = lv_keyboard_create(page);
    aos_keyboard_style(A.ed.kb, aos_font_body);
    lv_obj_set_size(A.ed.kb, A.W, A.land ? A.H * 45 / 100 : A.H * 34 / 100);
    lv_obj_align(A.ed.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(A.ed.kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(A.ed.kb, kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(A.ed.kb, LV_OBJ_FLAG_HIDDEN);
}

static void open_editor(int index)
{
    memset(&A.ed, 0, sizeof A.ed);
    A.ed.index = index;
    if (index >= 0 && index < A.cfg.count) {
        A.ed.s = A.cfg.s[index];
    } else {
        A.ed.index = -1;
        vnc_server_defaults(&A.ed.s);
    }
    A.screen = SCR_EDIT;
    build_ui();
}

/* ---- the viewer: geometry ------------------------------------------------------ */

static int32_t view_h(void)
{
    return A.vw.kbd_on ? A.H - vnc_kbd_height(A.vw.kbd) : A.H;
}

static float fit_scale(void)
{
    if (A.vw.rw <= 0 || A.vw.rh <= 0) return 1.0f;
    float sx = (float)A.W / (float)A.vw.rw, sy = (float)view_h() / (float)A.vw.rh;
    return sx < sy ? sx : sy;
}

/* Keeps the view on the remote screen: centred on an axis where it is
 * smaller than the view, inside it where it is bigger. */
static void clamp_offset(vnc_view_t *v)
{
    float vw = (float)v->vw / v->scale, vh = (float)v->vh / v->scale;
    if (vw >= (float)A.vw.rw) v->ox = ((float)A.vw.rw - vw) / 2.0f;
    else v->ox = v->ox < 0 ? 0 : v->ox > (float)A.vw.rw - vw ? (float)A.vw.rw - vw : v->ox;
    if (vh >= (float)A.vw.rh) v->oy = ((float)A.vw.rh - vh) / 2.0f;
    else v->oy = v->oy < 0 ? 0 : v->oy > (float)A.vw.rh - vh ? (float)A.vw.rh - vh : v->oy;
}

static void view_push(void)
{
    vnc_view_t *v = &A.vw.view;
    clamp_offset(v);
    v->gen = ++A.vw.gen;
    if (A.vw.sess) vnc_sess_set_view(A.vw.sess, v);
}

/* The view for the mode; 'recentre' puts the middle of the remote screen
 * in the middle (else the current centre stays). */
static void view_mode(bool recentre)
{
    vnc_view_t *v = &A.vw.view;
    float cx = v->scale > 0 ? v->ox + (float)v->vw / v->scale / 2.0f : (float)A.vw.rw / 2.0f;
    float cy = v->scale > 0 ? v->oy + (float)v->vh / v->scale / 2.0f : (float)A.vw.rh / 2.0f;
    if (recentre) {
        cx = (float)A.vw.rw / 2.0f;
        cy = (float)A.vw.rh / 2.0f;
    }
    v->vw = A.W & ~1;
    v->vh = view_h() & ~1;
    float fit = fit_scale();
    switch (A.vw.mode) {
    case MODE_FIT: v->scale = fit; break;
    case MODE_FILL: {
        float sx = (float)v->vw / (float)A.vw.rw, sy = (float)v->vh / (float)A.vw.rh;
        v->scale = sx > sy ? sx : sy;
        break;
    }
    case MODE_ONE: v->scale = 1.0f; break;
    default:
        if (v->scale < fit) v->scale = fit;
        break;
    }
    v->ox = cx - (float)v->vw / v->scale / 2.0f;
    v->oy = cy - (float)v->vh / v->scale / 2.0f;
    view_push();
}

static bool can_pan(void)
{
    const vnc_view_t *v = &A.vw.view;
    return (float)v->vw / v->scale < (float)A.vw.rw - 1.0f || (float)v->vh / v->scale < (float)A.vw.rh - 1.0f;
}

/* screen -> remote */
static void to_remote(float sx, float sy, float *rx, float *ry)
{
    lv_area_t a;
    lv_obj_get_coords(A.vw.canvas, &a);
    *rx = A.vw.view.ox + (sx - (float)a.x1) / A.vw.view.scale;
    *ry = A.vw.view.oy + (sy - (float)a.y1) / A.vw.view.scale;
}

static void pointer_send(void)
{
    A.vw.px = A.vw.px < 0 ? 0 : A.vw.px > (float)(A.vw.rw - 1) ? (float)(A.vw.rw - 1) : A.vw.px;
    A.vw.py = A.vw.py < 0 ? 0 : A.vw.py > (float)(A.vw.rh - 1) ? (float)(A.vw.rh - 1) : A.vw.py;
    vnc_sess_pointer(A.vw.sess, (int)A.vw.px, (int)A.vw.py, A.vw.buttons);
}

static void click(uint8_t button)
{
    A.vw.buttons = 0;
    pointer_send();
    A.vw.buttons = button;
    pointer_send();
    A.vw.buttons = 0;
    pointer_send();
}

static void wheel(uint8_t bit, int steps)
{
    for (int i = 0; i < steps; i++) {
        A.vw.buttons = bit;
        pointer_send();
        A.vw.buttons = 0;
        pointer_send();
    }
}

/* The trackpad's ring where the pointer is; the view follows the pointer
 * when it would leave it. */
static void ring_update(void)
{
    if (!A.vw.ring) return;
    bool show = A.vw.input == IN_TRACKPAD && A.vw.rw > 0 && A.vw.shown;
    lv_obj_set_flag(A.vw.ring, LV_OBJ_FLAG_HIDDEN, !show);
    if (!show) return;
    vnc_view_t *v = &A.vw.view;
    float x = (A.vw.px - v->ox) * v->scale, y = (A.vw.py - v->oy) * v->scale;
    const float m = 40.0f;
    bool moved = false;
    if (can_pan()) {
        if (x < m) { v->ox -= (m - x) / v->scale; moved = true; }
        if (x > (float)v->vw - m) { v->ox += (x - ((float)v->vw - m)) / v->scale; moved = true; }
        if (y < m) { v->oy -= (m - y) / v->scale; moved = true; }
        if (y > (float)v->vh - m) { v->oy += (y - ((float)v->vh - m)) / v->scale; moved = true; }
        if (moved) {
            view_push();
            x = (A.vw.px - v->ox) * v->scale;
            y = (A.vw.py - v->oy) * v->scale;
        }
    }
    lv_obj_set_pos(A.vw.ring, (int32_t)x - 14, (int32_t)y - 14);
}

/* ---- the viewer: the bar ---------------------------------------------------------- */

static void bar_show(bool on)
{
    A.vw.bar_on = on;
    A.vw.bar_until = on ? aos_hal_uptime_ms() + BAR_MS : 0;
    if (A.vw.bar) lv_obj_set_flag(A.vw.bar, LV_OBJ_FLAG_HIDDEN, !on);
    if (A.vw.handle) lv_obj_set_flag(A.vw.handle, LV_OBJ_FLAG_HIDDEN, on);
}

static void bar_touch(void)
{
    if (A.vw.bar_on) A.vw.bar_until = aos_hal_uptime_ms() + BAR_MS;
}

static void handle_cb(lv_event_t *e)
{
    (void)e;
    bar_show(true);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    A.vw.leave = true;
}

static void build_viewer(void);

static void kbd_hide(void *user)
{
    (void)user;
    A.vw.kbd_on = false;
    build_viewer();
    view_mode(false);
}

static void kbd_cb(lv_event_t *e)
{
    (void)e;
    A.vw.kbd_on = !A.vw.kbd_on;
    build_viewer();
    view_mode(false);
}

/* ---- the viewer: the keyboard bubble ------------------------------------------------
 *
 * A round button that floats over the remote screen, always there (the bar
 * hides): a tap opens or closes the keyboard, a drag puts it somewhere
 * else, and it stays there (preferences "vnc_bx", "vnc_by"). With no USB
 * keyboard it is the way to type a login. Above the keyboard while that
 * is open. */
#define BUBBLE 92
static int32_t s_bx = -1, s_by = -1;
static int32_t s_b_x0, s_b_y0;
static bool s_b_moved;

static void bubble_place(lv_obj_t *b)
{
    int32_t maxy = (A.vw.kbd_on && A.vw.kbd ? A.H - vnc_kbd_height(A.vw.kbd) : A.H) - BUBBLE - 12;
    if (s_bx < 0) {
        int32_t v;
        s_bx = aos_hal_pref_get_i32("vnc_bx", &v) ? v : A.W - BUBBLE - 24;
        s_by = aos_hal_pref_get_i32("vnc_by", &v) ? v : A.H - BUBBLE - 160;
    }
    int32_t x = s_bx < 8 ? 8 : s_bx > A.W - BUBBLE - 8 ? A.W - BUBBLE - 8 : s_bx;
    int32_t y = s_by < 8 ? 8 : s_by > maxy ? maxy : s_by;
    lv_obj_set_pos(b, x, y);
}

static void bubble_toggle(void *ud)
{
    (void)ud;
    kbd_cb(NULL);
}

static void bubble_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        s_b_x0 = lv_obj_get_x(b);
        s_b_y0 = lv_obj_get_y(b);
        s_b_moved = false;
    } else if (c == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        if (!v.x && !v.y) return;
        int32_t x = lv_obj_get_x(b) + v.x, y = lv_obj_get_y(b) + v.y;
        int32_t dx = x - s_b_x0, dy = y - s_b_y0;
        if (dx * dx + dy * dy > 14 * 14) s_b_moved = true;
        if (s_b_moved) {
            s_bx = x;
            s_by = y;
            bubble_place(b);
        }
    } else if (c == LV_EVENT_RELEASED) {
        if (s_b_moved) {
            s_bx = lv_obj_get_x(b);
            s_by = lv_obj_get_y(b);
            aos_hal_pref_set_i32("vnc_bx", s_bx);
            aos_hal_pref_set_i32("vnc_by", s_by);
        } else {
            /* the viewer is rebuilt: not from inside this button's own event */
            lv_async_call(bubble_toggle, NULL);
        }
    }
}

static void bubble_create(lv_obj_t *parent)
{
    lv_obj_t *b = box(parent, BUBBLE, BUBBLE);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, A.vw.kbd_on ? AOS_C_ACCENT : lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(b, 18, 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_50, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE |
                          LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(b, bubble_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(b, bubble_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(b, bubble_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *l = aos_label(b, AOS_SYM_KEYBOARD, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    bubble_place(b);
}

static const char *input_glyph(void)
{
    return A.vw.input == IN_TRACKPAD ? AOS_SYM_MOUSE : AOS_SYM_GESTURE_TAP_BUTTON;
}

static const char *mode_glyph(void)
{
    return A.vw.mode == MODE_FIT ? AOS_SYM_FULLSCREEN
         : A.vw.mode == MODE_FILL ? AOS_SYM_ARROW_EXPAND_HORIZONTAL
         : A.vw.mode == MODE_ONE ? AOS_SYM_MAGNIFY : AOS_SYM_VECTOR_SQUARE;
}

static void input_cb(lv_event_t *e)
{
    (void)e;
    A.vw.input = A.vw.input == IN_TOUCH ? IN_TRACKPAD : IN_TOUCH;
    aos_hal_pref_set_i32(KEY_INPUT, A.vw.input);
    if (A.vw.in_g) lv_label_set_text(A.vw.in_g, input_glyph());
    aos_ui_toast(A.vw.input == IN_TRACKPAD ? _("Trackpad: el dedo mueve el puntero") : _("Toque: el dedo es el mouse"), 1800);
    bar_touch();
    ring_update();
}

static void mode_cb(lv_event_t *e)
{
    (void)e;
    A.vw.mode = A.vw.mode == MODE_FIT ? MODE_FILL : A.vw.mode == MODE_FILL ? MODE_ONE : MODE_FIT;
    aos_hal_pref_set_i32(KEY_MODE, A.vw.mode);
    if (A.vw.mode_g) lv_label_set_text(A.vw.mode_g, mode_glyph());
    aos_ui_toast(A.vw.mode == MODE_FIT ? _("Ajustar") : A.vw.mode == MODE_FILL ? _("Llenar") : "1:1", 1000);
    view_mode(A.vw.mode != MODE_ONE);
    bar_touch();
}

static void rotate_cb(lv_event_t *e)
{
    (void)e;
    if (A.rot_restore < 0) A.rot_restore = aos_ui_landscape();
    aos_ui_request_landscape(-1);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    vnc_sess_refresh(A.vw.sess);
    bar_touch();
}

/* ---- the viewer: which part ---------------------------------------------------------
 *
 * A computer with several monitors may send them as one wide screen (a Mac
 * does). The monitor button picks a part: a monitor from the list the
 * server sends (ExtendedDesktopSize), when it sends one, or a rectangle
 * marked with a finger (zoomed in first, for precision). The session then
 * keeps, asks for and points at only that; it is saved with the computer
 * ("zone"). Guessing the monitors from the black between them was tried
 * and dropped: a real desktop, with a dark wallpaper or a lock screen, cut
 * into eight. */
#define ZONE_MAX (VNC_MAX_SCREENS + 3)
static vnc_rect_t s_zones[ZONE_MAX];
#define ZONE_MARK (-2)                  /* the picker's row that starts marking */
static void mark_open(void);

static void zpick_close(void)
{
    if (A.vw.zpick) lv_obj_delete(A.vw.zpick);
    A.vw.zpick = NULL;
}

static void zpick_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) zpick_close();
}

static void zone_save(const vnc_rect_t *z)
{
    A.vw.srv.zone = *z;
    int i = A.vw.index;
    if (i < 0 || i >= A.cfg.count || strcmp(A.cfg.s[i].host, A.vw.srv.host) || A.cfg.s[i].port != A.vw.srv.port) {
        return;
    }
    A.cfg.s[i].zone = *z;
    if (vnc_cfg_save(&A.cfg)) A.cfg_stamp = vnc_cfg_stamp();
}

static void zone_pick(int k)
{
    vnc_rect_t z = s_zones[k];
    int fw = 0, fh = 0;
    vnc_sess_screens(A.vw.sess, &fw, &fh, NULL, NULL, 0);
    if (z.x == 0 && z.y == 0 && z.w == fw && z.h == fh) z.w = z.h = 0;     /* all of it */
    zone_save(&z);
    vnc_sess_set_zone(A.vw.sess, &z);
}

static void zpick_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    zpick_close();
    if (k == ZONE_MARK) {
        mark_open();
        return;
    }
    if (k >= 0 && k < ZONE_MAX) zone_pick(k);
    bar_touch();
}

/* Marking a part with a finger: a drag over the remote screen draws the
 * rectangle, and lifting it shows only that. Zoomed in first, it is as
 * precise as one wants; it works the same upright or turned. */
static lv_obj_t *s_mark_r;
static lv_point_t s_mark_p0;

static void mark_close(void)
{
    if (A.vw.mark) lv_obj_delete(A.vw.mark);
    A.vw.mark = NULL;
    s_mark_r = NULL;
}

static void mark_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (code == LV_EVENT_PRESSED) {
        s_mark_p0 = p;
        lv_obj_set_pos(s_mark_r, p.x, p.y);
        lv_obj_set_size(s_mark_r, 1, 1);
        lv_obj_remove_flag(s_mark_r, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int32_t x0 = LV_MIN(s_mark_p0.x, p.x), y0 = LV_MIN(s_mark_p0.y, p.y);
    int32_t x1 = LV_MAX(s_mark_p0.x, p.x), y1 = LV_MAX(s_mark_p0.y, p.y);
    if (code == LV_EVENT_PRESSING) {
        lv_obj_set_pos(s_mark_r, x0, y0);
        lv_obj_set_size(s_mark_r, x1 - x0 + 1, y1 - y0 + 1);
        return;
    }
    if (code != LV_EVENT_RELEASED) return;
    float rx0, ry0, rx1, ry1;
    to_remote((float)x0, (float)y0, &rx0, &ry0);
    to_remote((float)x1, (float)y1, &rx1, &ry1);
    int ix0 = rx0 < 0 ? 0 : (int)(rx0 + 0.5f), iy0 = ry0 < 0 ? 0 : (int)(ry0 + 0.5f);
    int ix1 = rx1 > (float)A.vw.rw ? A.vw.rw : (int)(rx1 + 0.5f);
    int iy1 = ry1 > (float)A.vw.rh ? A.vw.rh : (int)(ry1 + 0.5f);
    mark_close();
    if (ix1 - ix0 < 64 || iy1 - iy0 < 64) {
        aos_ui_toast(_("Muy chico: arrastrá un rectángulo más grande"), 2000);
        return;
    }
    vnc_rect_t cur;
    vnc_sess_screens(A.vw.sess, NULL, NULL, &cur, NULL, 0);
    vnc_rect_t z = { cur.x + ix0, cur.y + iy0, ix1 - ix0, iy1 - iy0 };
    zone_save(&z);
    vnc_sess_set_zone(A.vw.sess, &z);
}

static void mark_open(void)
{
    bar_show(false);
    lv_obj_t *o = box(A.vw.obj, A.W, view_h());
    A.vw.mark = o;
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, mark_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(o, mark_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(o, mark_cb, LV_EVENT_RELEASED, NULL);
    s_mark_r = box(o, 1, 1);
    lv_obj_set_style_border_width(s_mark_r, 4, 0);
    lv_obj_set_style_border_color(s_mark_r, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_color(s_mark_r, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_mark_r, LV_OPA_20, 0);
    lv_obj_remove_flag(s_mark_r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_mark_r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *tip = aos_label(o, _("Arrastrá un rectángulo sobre la parte que querés ver"), aos_font_body,
                              AOS_C_TEXT);
    lv_obj_set_width(tip, A.W - 2 * AOS_UI_PAD);
    lv_label_set_long_mode(tip, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(tip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(tip, 14, 0);
    lv_obj_set_style_radius(tip, 16, 0);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 64);         /* below the handle */
    lv_obj_remove_flag(tip, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *zpick_row(lv_obj_t *card, const char *text, bool on, int k)
{
    lv_obj_t *b = aos_button(card, text, on ? AOS_C_ACCENT : AOS_C_CARD2, zpick_cb, (void *)(intptr_t)k);
    lv_obj_set_width(b, LV_PCT(100));
    return b;
}

static void zone_cb(lv_event_t *e)
{
    (void)e;
    if (A.vw.zpick || !A.vw.sess) return;
    int fw = 0, fh = 0;
    vnc_rect_t cur, scr[VNC_MAX_SCREENS];
    int ns = vnc_sess_screens(A.vw.sess, &fw, &fh, &cur, scr, VNC_MAX_SCREENS);
    if (fw <= 0) return;

    lv_obj_t *bg = box(A.vw.obj, A.W, A.H);
    A.vw.zpick = bg;
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bg, zpick_bg_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *card = box(bg, A.land ? 620 : A.W - 2 * AOS_UI_PAD, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);       /* a tap between rows does not close */
    lv_obj_center(card);
    lv_obj_t *title = aos_label(card, _("Qué pantalla ver"), aos_font_body, AOS_C_TEXT);
    aos_make_decorative(title);

    char line[96];
    int k = 0;
    bool all = cur.x == 0 && cur.y == 0 && cur.w == fw && cur.h == fh;
    s_zones[k] = (vnc_rect_t){ 0, 0, fw, fh };
    snprintf(line, sizeof line, _("Todas · %d×%d"), fw, fh);
    zpick_row(card, line, all, k++);
    bool listed = all;
    for (int i = 0; i < ns && k < ZONE_MAX - 2; i++) {
        bool on = !memcmp(&scr[i], &cur, sizeof cur);
        listed |= on;
        s_zones[k] = scr[i];
        snprintf(line, sizeof line, _("Monitor %d · %d×%d"), i + 1, scr[i].w, scr[i].h);
        zpick_row(card, line, on, k++);
    }
    if (!listed) {
        s_zones[k] = cur;
        snprintf(line, sizeof line, _("Zona elegida · %d×%d"), cur.w, cur.h);
        zpick_row(card, line, true, k++);
    }
    zpick_row(card, _("Marcar con el dedo"), false, ZONE_MARK);
    const char *why = ns ? _("La lista la manda la computadora.")
                         : _("Para ver una sola parte (un monitor de varios), marcala con el dedo; "
                             "acercá antes con dos dedos si querés más precisión.");
    lv_obj_t *hint = aos_label(card, why,
                               aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    aos_make_decorative(hint);
    bar_touch();
}

static void retry_cb(lv_event_t *e)
{
    (void)e;
    int i = A.vw.index;
    close_viewer();
    open_viewer(i);
}

/* ---- the viewer: gestures ------------------------------------------------------------- */

static void zoom_at(float factor, float sx, float sy)
{
    vnc_view_t *v = &A.vw.view;
    float fit = fit_scale(), lo = fit < 1.0f ? fit : 1.0f;
    if (lo > fit) lo = fit;
    float ns = v->scale * factor;
    if (ns < fit) ns = fit;
    if (ns > MAX_ZOOM) ns = MAX_ZOOM;
    float rx, ry;
    to_remote(sx, sy, &rx, &ry);
    lv_area_t a;
    lv_obj_get_coords(A.vw.canvas, &a);
    v->scale = ns;
    v->ox = rx - (sx - (float)a.x1) / ns;
    v->oy = ry - (sy - (float)a.y1) / ns;
    A.vw.mode = MODE_FREE;
    if (A.vw.mode_g) lv_label_set_text(A.vw.mode_g, mode_glyph());
}

static void pan_by(float dx, float dy)
{
    A.vw.view.ox -= dx / A.vw.view.scale;
    A.vw.view.oy -= dy / A.vw.view.scale;
}

static void two_fingers(const aos_gesture_event_t *ev)
{
    switch (ev->type) {
    case AOS_GESTURE_PINCH_BEGIN:
        A.vw.g2 = G2_UNDECIDED;
        A.vw.g2_scale = 1.0f;
        A.vw.g2_dx = A.vw.g2_dy = 0;
        A.vw.g2_scroll_x = A.vw.g2_scroll_y = 0;
        A.vw.g2_cx = ev->x;
        A.vw.g2_cy = ev->y;
        A.vw.g2_t0 = ev->t_ms;
        break;
    case AOS_GESTURE_PINCH: {
        A.vw.g2_scale *= ev->scale;
        A.vw.g2_dx += ev->dx;
        A.vw.g2_dy += ev->dy;
        if (A.vw.g2 == G2_UNDECIDED) {
            float ls = fabsf(logf(A.vw.g2_scale));
            float moved = fabsf(A.vw.g2_dx) + fabsf(A.vw.g2_dy);
            if (ls > 0.08f) {
                A.vw.g2 = G2_ZOOM;
                zoom_at(A.vw.g2_scale, ev->x, ev->y);
                pan_by(A.vw.g2_dx, A.vw.g2_dy);
                view_push();
            } else if (moved > 24.0f) {
                A.vw.g2 = A.vw.input == IN_TOUCH && can_pan() ? G2_PAN : G2_SCROLL;
            }
            break;
        }
        if (A.vw.g2 == G2_ZOOM) {
            zoom_at(ev->scale, ev->x, ev->y);
            pan_by(ev->dx, ev->dy);
            view_push();
        } else if (A.vw.g2 == G2_PAN) {
            pan_by(ev->dx, ev->dy);
            if (A.vw.mode != MODE_FREE) {
                A.vw.mode = MODE_FREE;
                if (A.vw.mode_g) lv_label_set_text(A.vw.mode_g, mode_glyph());
            }
            view_push();
        } else {
            /* fingers going up scroll the page down, as on a phone */
            A.vw.g2_scroll_y += ev->dy;
            A.vw.g2_scroll_x += ev->dx;
            if (A.vw.input == IN_TOUCH) {
                float rx, ry;
                to_remote(A.vw.g2_cx, A.vw.g2_cy, &rx, &ry);
                A.vw.px = rx;
                A.vw.py = ry;
            }
            while (A.vw.g2_scroll_y <= -SCROLL_STEP_PX) { wheel(1 << 4, 1); A.vw.g2_scroll_y += SCROLL_STEP_PX; }
            while (A.vw.g2_scroll_y >= SCROLL_STEP_PX) { wheel(1 << 3, 1); A.vw.g2_scroll_y -= SCROLL_STEP_PX; }
            while (A.vw.g2_scroll_x <= -SCROLL_STEP_PX) { wheel(1 << 6, 1); A.vw.g2_scroll_x += SCROLL_STEP_PX; }
            while (A.vw.g2_scroll_x >= SCROLL_STEP_PX) { wheel(1 << 5, 1); A.vw.g2_scroll_x -= SCROLL_STEP_PX; }
        }
        break;
    }
    case AOS_GESTURE_PINCH_END:
        if (A.vw.g2 == G2_UNDECIDED && ev->t_ms - A.vw.g2_t0 < TAP2_MS) {
            /* two fingers tapped: a right click */
            if (A.vw.input == IN_TOUCH) to_remote(A.vw.g2_cx, A.vw.g2_cy, &A.vw.px, &A.vw.py);
            click(1 << 2);
        }
        A.vw.g2 = G2_UNDECIDED;
        break;
    default:
        break;
    }
}

static void on_gesture(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (!A.vw.sess || A.vw.rw <= 0 || !A.vw.shown) return;
    if (A.vw.bar_on && ev->type != AOS_GESTURE_DRAG && ev->type != AOS_GESTURE_PINCH) bar_show(false);
    if (ev->type >= AOS_GESTURE_PINCH_BEGIN) {
        two_fingers(ev);
        ring_update();
        return;
    }
    bool touch = A.vw.input == IN_TOUCH;
    switch (ev->type) {
    case AOS_GESTURE_TAP:
        A.vw.pending_right = false;
        if (touch) to_remote(ev->x, ev->y, &A.vw.px, &A.vw.py);
        click(1);
        break;
    case AOS_GESTURE_LONG_PRESS:
        /* a right click when the finger lifts without dragging */
        if (touch) to_remote(ev->x, ev->y, &A.vw.px, &A.vw.py);
        A.vw.pending_right = true;
        if (touch) pointer_send();
        break;
    case AOS_GESTURE_DRAG_BEGIN:
        A.vw.pending_right = false;
        if (touch) {
            to_remote(ev->x, ev->y, &A.vw.px, &A.vw.py);
            A.vw.buttons = 0;
            pointer_send();
            A.vw.buttons = 1;
            A.vw.dragging = true;
            pointer_send();
        } else {
            A.vw.dragging = ev->after_long;
            A.vw.buttons = A.vw.dragging ? 1 : 0;
            pointer_send();
        }
        break;
    case AOS_GESTURE_DRAG:
        if (touch) {
            to_remote(ev->x, ev->y, &A.vw.px, &A.vw.py);
        } else {
            float d = fabsf(ev->dx) + fabsf(ev->dy);
            float k = 1.0f + (d > 30.0f ? 1.0f : d / 30.0f);     /* faster moves go further, twice at most */
            A.vw.px += ev->dx * k / A.vw.view.scale;
            A.vw.py += ev->dy * k / A.vw.view.scale;
        }
        pointer_send();
        break;
    case AOS_GESTURE_DRAG_END:
        A.vw.buttons = 0;
        A.vw.dragging = false;
        pointer_send();
        break;
    default:
        break;
    }
    ring_update();
}

/* ---- the viewer: the USB mouse and keyboard ------------------------------------------ */

static lv_indev_t *find_mouse(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER && lv_indev_get_cursor(i)) return i;
    }
    return NULL;
}

/* Is the screen point over the remote screen, not over the bar, the
 * handle or the keyboard? */
static bool over_canvas(int32_t x, int32_t y)
{
    lv_point_t p = { x, y };
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);
    if (o) return false;
    o = lv_indev_search_obj(lv_screen_active(), &p);
    return o == A.vw.canvas;
}

static void mouse_poll(void)
{
    if (!A.vw.mouse) {
        if (!aos_hal_hid_mouse_present()) return;
        A.vw.mouse = find_mouse();
        if (!A.vw.mouse) return;
    }
    lv_point_t p;
    lv_indev_get_point(A.vw.mouse, &p);
    bool down = lv_indev_get_state(A.vw.mouse) == LV_INDEV_STATE_PRESSED;
    if (p.x == A.vw.mx && p.y == A.vw.my && down == A.vw.mdown) return;
    A.vw.mx = p.x;
    A.vw.my = p.y;
    A.vw.mouse_ms = aos_hal_uptime_ms();
    if (!A.vw.sess || A.vw.rw <= 0 || !A.vw.shown) {
        A.vw.mdown = down;
        return;
    }
    /* a press that started on the bar is the bar's; one on the screen keeps
     * going to the remote even if the arrow leaves it */
    if (!over_canvas(p.x, p.y) && !(A.vw.mdown && A.vw.buttons)) {
        A.vw.mdown = down;
        return;
    }
    A.vw.mdown = down;
    to_remote((float)p.x, (float)p.y, &A.vw.px, &A.vw.py);
    A.vw.buttons = down ? 1 : 0;
    pointer_send();
    if (A.vw.input == IN_TRACKPAD) ring_update();
}

static bool hwkey_cb(uint32_t key, uint8_t mods)
{
    if (!A.vw.on || !A.vw.sess || A.vw.srv.view_only) return false;
    return vnc_hwkey(A.vw.sess, key, mods);
}

/* ---- the viewer: building, opening, closing ------------------------------------------- */

static lv_obj_t *bar_btn(lv_obj_t *row, const char *glyph, lv_event_cb_t cb, lv_obj_t **g)
{
    return round_btn(row, glyph, cb, NULL, g);
}

static void build_viewer(void)
{
    if (A.vw.kbd) {
        vnc_kbd_delete(A.vw.kbd);
        A.vw.kbd = NULL;
    }
    if (A.vw.obj) {
        lv_obj_delete(A.vw.obj);
        A.vw.obj = NULL;
    }
    A.vw.zpick = NULL;
    A.vw.mark = NULL;
    s_mark_r = NULL;
    lv_obj_t *o = box(A.root, A.W, A.H);
    A.vw.obj = o;
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    A.vw.canvas = lv_canvas_create(o);
    lv_obj_set_pos(A.vw.canvas, 0, 0);
    aos_gesture_attach(A.vw.canvas, AOS_GESTURE_FLAG_FAST_TAP, on_gesture, NULL);
    A.vw.shown = NULL;
    lv_obj_add_flag(A.vw.canvas, LV_OBJ_FLAG_HIDDEN);

    A.vw.ring = box(o, 28, 28);
    lv_obj_set_style_radius(A.vw.ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(A.vw.ring, 3, 0);
    lv_obj_set_style_border_color(A.vw.ring, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_color(A.vw.ring, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(A.vw.ring, LV_OPA_30, 0);
    lv_obj_remove_flag(A.vw.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(A.vw.ring, LV_OBJ_FLAG_HIDDEN);

    /* the state, while there is no picture */
    A.vw.msg = aos_label(o, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_width(A.vw.msg, A.W - 120);
    lv_label_set_long_mode(A.vw.msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(A.vw.msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(A.vw.msg, LV_ALIGN_CENTER, 0, -60);
    aos_make_decorative(A.vw.msg);
    A.vw.msg_btns = box(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(A.vw.msg_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(A.vw.msg_btns, 20, 0);
    lv_obj_align(A.vw.msg_btns, LV_ALIGN_CENTER, 0, 120);
    aos_button(A.vw.msg_btns, _("Reintentar"), AOS_C_ACCENT, retry_cb, NULL);
    aos_button(A.vw.msg_btns, _("Volver"), AOS_C_CARD2, close_cb, NULL);
    lv_obj_add_flag(A.vw.msg_btns, LV_OBJ_FLAG_HIDDEN);

    /* the bar: close, name and numbers; the buttons */
    int32_t bar_h = A.land ? 120 : 220;
    A.vw.bar = box(o, A.W, bar_h);
    lv_obj_set_style_bg_color(A.vw.bar, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(A.vw.bar, lv_color_black(), 0);
    lv_obj_set_style_bg_main_opa(A.vw.bar, LV_OPA_80, 0);
    lv_obj_set_style_bg_grad_opa(A.vw.bar, LV_OPA_40, 0);
    lv_obj_set_style_bg_grad_dir(A.vw.bar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(A.vw.bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(A.vw.bar, LV_OBJ_FLAG_CLICKABLE);       /* touches on it stay on it */
    lv_obj_t *x = round_btn(A.vw.bar, AOS_SYM_CLOSE, close_cb, NULL, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 16);
    int32_t text_w = A.land ? A.W / 2 - AOS_UI_TAP_MIN - 60 : A.W - 2 * AOS_UI_PAD - AOS_UI_TAP_MIN - 24;
    char safe[96];
    aos_text_safe(safe, sizeof safe, A.vw.srv.name);
    A.vw.name = aos_label(A.vw.bar, safe, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(A.vw.name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(A.vw.name, text_w, lv_font_get_line_height(aos_font_body));
    lv_obj_align(A.vw.name, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 20, 20);
    A.vw.info = aos_label(A.vw.bar, "", aos_font_caption, lv_color_hex(0xD1D1D6));
    lv_label_set_long_mode(A.vw.info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(A.vw.info, text_w, lv_font_get_line_height(aos_font_caption));
    lv_obj_align(A.vw.info, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 20, 62);
    aos_make_decorative(A.vw.name);
    aos_make_decorative(A.vw.info);

    lv_obj_t *row = box(A.vw.bar, LV_SIZE_CONTENT, AOS_UI_TAP_MIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, A.land ? 16 : 22, 0);
    if (A.land) lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 16);
    else lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 116);
    if (!A.vw.srv.view_only) bar_btn(row, AOS_SYM_KEYBOARD, kbd_cb, NULL);
    if (!A.vw.srv.view_only) bar_btn(row, input_glyph(), input_cb, &A.vw.in_g);
    else A.vw.in_g = NULL;
    bar_btn(row, mode_glyph(), mode_cb, &A.vw.mode_g);
    bar_btn(row, AOS_SYM_MONITOR, zone_cb, NULL);
    bar_btn(row, A.land ? AOS_SYM_PHONE_ROTATE_PORTRAIT : AOS_SYM_PHONE_ROTATE_LANDSCAPE, rotate_cb, NULL);
    bar_btn(row, AOS_SYM_RESTART, refresh_cb, NULL);

    /* the handle that brings the bar back */
    A.vw.handle = box(o, 120, 44);
    lv_obj_align(A.vw.handle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(A.vw.handle, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(A.vw.handle, LV_OPA_50, 0);
    lv_obj_set_style_radius(A.vw.handle, 22, 0);
    lv_obj_add_flag(A.vw.handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(A.vw.handle, 16);
    lv_obj_add_event_cb(A.vw.handle, handle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hg = aos_label(A.vw.handle, AOS_SYM_CHEVRON_DOWN, &aos_sym_28, AOS_C_TEXT);
    lv_obj_center(hg);
    aos_make_decorative(hg);

    if (A.vw.kbd_on) {
        A.vw.kbd = vnc_kbd_create(o, A.W, A.land, &A.vw.sess, kbd_hide, NULL);
        if (!A.vw.kbd) A.vw.kbd_on = false;
    }
    if (!A.vw.srv.view_only) bubble_create(o);
    A.vw.last_state = -1;
    bar_show(A.vw.bar_on);
}

static void open_viewer(int index)
{
    if (index < 0 || index >= A.cfg.count) return;
    memset(&A.vw, 0, sizeof A.vw);
    A.vw.on = true;
    A.vw.index = index;
    A.vw.srv = A.cfg.s[index];
    int32_t v;
    A.vw.mode = aos_hal_pref_get_i32(KEY_MODE, &v) && v >= MODE_FIT && v <= MODE_ONE ? v : MODE_FIT;
    A.vw.input = aos_hal_pref_get_i32(KEY_INPUT, &v) && v == IN_TRACKPAD ? IN_TRACKPAD : IN_TOUCH;
    A.vw.bar_on = true;
    A.screen = SCR_VIEW;
    /* a drag from the left edge is the remote mouse's, not "back" */
    A.self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    lv_obj_clean(A.root);
    build_viewer();
    A.vw.sess = vnc_sess_start(&A.vw.srv);
    if (!A.vw.sess) {
        set_text(A.vw.msg, _("No se pudo empezar: la conexión anterior todavía se está cerrando"));
        lv_obj_remove_flag(A.vw.msg_btns, LV_OBJ_FLAG_HIDDEN);
    }
    aos_hal_net_low_latency(true);
    aos_ui_hwkbd_handler(hwkey_cb);
    aos_hal_log(VNC_TAG, "connecting to %s (%s:%d, %s, %d bits)", A.vw.srv.name, A.vw.srv.host, A.vw.srv.port,
                vnc_enc_name(A.vw.srv.enc), A.vw.srv.depth);
}

static void close_viewer(void)
{
    if (!A.vw.on) return;
    aos_ui_hwkbd_handler(NULL);
    if (A.vw.kbd) {
        vnc_kbd_delete(A.vw.kbd);
        A.vw.kbd = NULL;
    }
    /* the canvas goes before the buffer it points at */
    if (A.vw.obj) lv_obj_delete(A.vw.obj);
    A.vw.obj = NULL;
    vnc_sess_stop(A.vw.sess);
    A.vw.sess = NULL;
    aos_hal_net_low_latency(false);
    A.self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
    memset(&A.vw, 0, sizeof A.vw);
}

/* ---- the viewer: every frame ------------------------------------------------------------ */

static void viewer_frame(void)
{
    vnc_sess_t *s = A.vw.sess;
    if (!s) return;

    char detail[160];
    vnc_state_t st = vnc_sess_state(s, detail, sizeof detail);
    if ((int)st != A.vw.last_state) {
        A.vw.last_state = (int)st;
        const char *t = st == VNC_ST_CONNECTING ? _("Conectando…")
                      : st == VNC_ST_AUTH ? _("Entrando…")
                      : st == VNC_ST_ERROR ? detail : "";
        set_text(A.vw.msg, t);
        lv_obj_set_flag(A.vw.msg, LV_OBJ_FLAG_HIDDEN, st == VNC_ST_LIVE);
        lv_obj_set_flag(A.vw.msg_btns, LV_OBJ_FLAG_HIDDEN, st != VNC_ST_ERROR);
        if (st == VNC_ST_ERROR) {
            lv_obj_add_flag(A.vw.canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(A.vw.ring, LV_OBJ_FLAG_HIDDEN);
            A.vw.shown = NULL;
            bar_show(true);
            A.vw.bar_until = 0;     /* stays */
        }
    }
    if (st == VNC_ST_ERROR) return;

    int rw, rh;
    uint32_t zg = vnc_sess_zone_gen(s);
    if (vnc_sess_desktop(s, &rw, &rh, NULL, 0) && (rw != A.vw.rw || rh != A.vw.rh || zg != A.vw.zone_gen)) {
        bool first = A.vw.rw == 0 || zg != A.vw.zone_gen;
        A.vw.zone_gen = zg;
        A.vw.rw = rw;
        A.vw.rh = rh;
        if (first) {
            A.vw.px = (float)rw / 2;
            A.vw.py = (float)rh / 2;
        }
        if (A.vw.mode == MODE_FREE) A.vw.mode = MODE_FIT;
        view_mode(true);
    }

    const uint16_t *px;
    int w, h;
    bool changed;
    lv_area_t area;
    if (vnc_sess_take(s, &px, &w, &h, &changed, &area)) {
        if (changed || px != A.vw.shown || w != A.vw.shown_w || h != A.vw.shown_h) {
            lv_canvas_set_buffer(A.vw.canvas, (void *)px, w, h, LV_COLOR_FORMAT_RGB565);
            lv_obj_remove_flag(A.vw.canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(A.vw.canvas);
            bool first = !A.vw.shown;
            A.vw.shown = px;
            A.vw.shown_w = w;
            A.vw.shown_h = h;
            if (first) ring_update();
        } else {
            lv_area_t c;
            lv_obj_get_coords(A.vw.canvas, &c);
            lv_area_move(&area, c.x1, c.y1);
            lv_obj_invalidate_area(A.vw.canvas, &area);
        }
    }

    uint32_t bells = vnc_sess_bells(s);
    if (bells != A.vw.bells) {
        A.vw.bells = bells;
        aos_hal_beep(880, 60);
    }

    uint64_t now = aos_hal_uptime_ms();
    if (now - A.vw.stats_t >= STATS_MS) {
        A.vw.stats_t = now;
        char line[96];
        vnc_sess_stats(s, line, sizeof line);
        if (!line[0] && st == VNC_ST_LIVE) snprintf(line, sizeof line, "%dx%d", A.vw.rw, A.vw.rh);
        set_text(A.vw.info, line);
    }

    /* a long press lifted without a drag: the right click it promised */
    if (A.vw.pending_right) {
        aos_touch_point_t tp[2];
        if (aos_touch_points(tp) == 0) {
            A.vw.pending_right = false;
            click(1 << 2);
        }
    }
    mouse_poll();
    if (A.vw.bar_on && A.vw.bar_until && now > A.vw.bar_until && st == VNC_ST_LIVE && A.vw.shown) {
        bar_show(false);
    }
}

/* ---- the app --------------------------------------------------------------------------- */

static void build_ui(void)
{
    lv_obj_clean(A.root);
    A.W = lv_obj_get_width(A.root);
    A.H = lv_obj_get_height(A.root);
    A.land = A.W > A.H;
    lv_obj_set_style_bg_color(A.root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(A.root, LV_OPA_COVER, 0);
    if (A.screen == SCR_EDIT) build_editor();
    else build_list();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (A.vw.leave) {
        close_viewer();
        A.screen = SCR_LIST;
        build_ui();
        return;
    }
    if (A.screen == SCR_VIEW) {
        if (!A.hidden) viewer_frame();
        return;
    }
    uint64_t now = aos_hal_uptime_ms();
    if (A.screen == SCR_LIST && now - A.cfg_poll >= CFG_POLL_MS) {
        /* the portal's page writes the same file */
        A.cfg_poll = now;
        uint32_t stamp = vnc_cfg_stamp();
        if (stamp != A.cfg_stamp) {
            A.cfg_stamp = stamp;
            vnc_cfg_load(&A.cfg);
            build_ui();
        }
    }
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (A.screen == SCR_VIEW) {
        if (A.vw.zpick) {
            zpick_close();
            return true;
        }
        if (A.vw.mark) {
            mark_close();
            return true;
        }
        /* with a mouse in use, "back" is its right button */
        if (A.vw.mouse && aos_hal_hid_mouse_present() && aos_hal_uptime_ms() - A.vw.mouse_ms < MOUSE_FRESH_MS &&
            A.vw.sess && A.vw.shown && over_canvas(A.vw.mx, A.vw.my)) {
            to_remote((float)A.vw.mx, (float)A.vw.my, &A.vw.px, &A.vw.py);
            click(1 << 2);
            return true;
        }
        A.vw.leave = true;              /* on the next timer call, not inside the gesture */
        return true;
    }
    if (A.screen == SCR_EDIT) {
        if (A.ed.kb && !lv_obj_has_flag(A.ed.kb, LV_OBJ_FLAG_HIDDEN)) ed_kb_show(NULL);
        else ed_close();
        return true;
    }
    return false;
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A.hidden = true;
    vnc_sess_pause(A.vw.sess, true);
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A.hidden = false;
    vnc_sess_pause(A.vw.sess, false);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    A.root = root;
    if (A.screen == SCR_VIEW) {
        A.W = lv_obj_get_width(root);
        A.H = lv_obj_get_height(root);
        A.land = A.W > A.H;
        build_viewer();                 /* the session and its picture carry on */
        if (A.vw.rw > 0) view_mode(false);
        return true;
    }
    if (A.screen == SCR_EDIT) {
        /* keep what was typed */
        snprintf(A.ed.s.name, sizeof A.ed.s.name, "%s", lv_textarea_get_text(A.ed.name));
        snprintf(A.ed.s.host, sizeof A.ed.s.host, "%s", lv_textarea_get_text(A.ed.host));
        snprintf(A.ed.s.pass, sizeof A.ed.s.pass, "%s", lv_textarea_get_text(A.ed.pass));
        A.ed.s.port = atoi(lv_textarea_get_text(A.ed.port));
        A.ed.s.view_only = lv_obj_has_state(A.ed.view_sw, LV_STATE_CHECKED);
    }
    build_ui();
    return true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&A, 0, sizeof A);
    A.self = self;
    A.root = root;
    A.rot_restore = -1;
    vnc_cfg_load(&A.cfg);
    A.cfg_stamp = vnc_cfg_stamp();
    A.cfg_poll = aos_hal_uptime_ms();
    A.screen = SCR_LIST;
    build_ui();
    A.timer = lv_timer_create(timer_cb, TIMER_MS, NULL);

    /* Development: VNC_OPEN=<n> connects to the n-th computer at once. */
    const char *open = getenv("VNC_OPEN");
    if (open && open[0] >= '0' && open[0] <= '9') open_viewer(open[0] - '0');
    return &A;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (A.timer) {
        lv_timer_delete(A.timer);
        A.timer = NULL;
    }
    close_viewer();                     /* the canvas goes before its buffer */
    lv_obj_clean(A.root);
    if (A.rot_restore >= 0 && aos_ui_landscape() != (bool)A.rot_restore) {
        aos_ui_request_landscape(A.rot_restore);
    }
}

static bool vnc_init(aos_app_t *app)
{
    app->desc.id       = "aos.vnc";
    app->desc.name     = "VNC";
    app->desc.icon     = AOS_SYM_MONITOR;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x2563EB;
    app->desc.color_b  = 0x1E3A8A;
    app->desc.order    = 128;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP;
    aos_icon_set_ops(app, VNC_ICON, sizeof VNC_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(vnc_init);
