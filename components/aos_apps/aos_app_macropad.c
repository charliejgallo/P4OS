/*
 * P4OS - Macro pad: a Stream Deck of the bench. Pages of big buttons, each
 * one a key combo or a typed text sent to the computer by USB, a small
 * script of those, a Home Assistant service, an MQTT message or another
 * app of the board; and three fixed faces at the end of the top bar: a
 * trackpad with a keyboard, a gamepad and a MIDI keyboard (what the watch's
 * Control PC was, without the air mouse and the tilt: there is no IMU here,
 * so the gamepad's stick is on the glass and the bend is a strip).
 *
 * The gamepad and the piano read the two fingers themselves rather than
 * LVGL's pointer, so the stick and a button, or two keys, work at once; the
 * section that draws them tells how, and what is sent when.
 *
 * The layout lives on the card (macropad.json, aos_macropad.c) and is
 * edited here -a long press on a button opens its sheet- or, more
 * comfortably, from the portal's Macro pad page. 15 slots a page: 3x5
 * upright, 5x3 lying down, the same buttons in the same reading order.
 *
 * A button that has a state shows it: an HA entity's (a light on or off, a
 * sensor's value) and an MQTT button's "state" topic (a Tasmota plug's
 * stat/<x>/POWER). A toggle that is on is filled with its colour; one that
 * is off is dark with the colour in the glyph.
 *
 * What it sends is shown at the bottom ("Enviado: cmd+c"), and the button
 * flashes green or red. In the simulator nothing reaches the Mac: the HAL
 * only logs (sim/hal_sim.c, sim/usb_sim.c).
 */
#include "aos_apps.h"
#include "aos_macropad.h"
#include "aos_ha.h"
#include "aos_mqtt.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_text_safe.h"
#include "aos_gesture.h"
#include "aos_sys_glyphs.h"
#include "aos_fonts.h"
#include "aos_mono.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_MP     lv_color_hex(0x22D3EE)
#define C_SHEET  lv_color_hex(0x121216)
#define TOP_H    88
#define FOOT_H   60
#define GAP      14

/* what the body shows: a page of buttons or one of the three fixed faces */
typedef enum { FACE_PAGE, FACE_TRACKPAD, FACE_PAD, FACE_MIDI } face_t;

/* the top bar's chips: a page's index, or one of these */
#define TAB_TRACKPAD  (-1)
#define TAB_ADD       (-2)
#define TAB_PAD       (-3)
#define TAB_MIDI      (-4)

/* state kept across rotations (the UI is rebuilt, these are not) */
static struct {
    int    page;
    face_t face;
    int    speed;           /* 1..4 */
    int    octave;          /* MIDI: the lowest key is C<octave>, -1..7 */
    int    vel;             /* MIDI: 0 soft, 1 medium, 2 strong */
    int    mod;             /* MIDI: the modulation strip, 0..127 */
} S = { .speed = 2, .octave = 4, .vel = 1 };

typedef struct {
    lv_obj_t *obj, *glyph, *name, *state, *dot;
    uint32_t  color;
    bool      toggle, on, has_state;
} tile_t;

static struct {
    lv_obj_t *root, *tabs, *usb, *usb_dot, *usb_txt, *body, *foot_dot, *foot_txt;
    lv_obj_t *overlay, *ta, *kb, *sug;          /* text entry */
    lv_obj_t *sheet;                            /* editor, page actions, pickers */
    lv_obj_t *picker;                           /* over the sheet */
    tile_t    tiles[AOS_MP_SLOTS];
    int32_t   W, H;
    bool      land;
    lv_timer_t *timer;
    uint32_t  doc_ver, ha_ver, mq_ver, st_seq;
    int       usb_state;
    bool      dirty;                            /* the document changed under an overlay */
    int       tick;
    /* trackpad */
    lv_obj_t *drag_btn, *speed_lbl, *type_row;
    float     ax, ay, wheel;
    float     zlog;                             /* two fingers: the spread since the last zoom step */
    int       zmode, zooms;                     /* 0 undecided, 1 scrolling, 2 zooming */
    int       moved, notches;
    bool      drag_lock;
    lv_obj_t *live_ta;
    char      live_prev[256];
    uint32_t  ready_tick;
    /* editor */
    cJSON    *E;
    int       e_page, e_slot;
    bool      e_new;
    lv_obj_t *e_prev, *e_prev_glyph, *e_prev_name, *e_name, *e_params, *e_type_chips[6], *e_glyphs, *e_colors;
    int       confirm_delete;
} U;

static void build(void);
static void build_body(void);
static void e_params_build(void);
static void subscribe_states(void);

/* -------------------------------------------------------------------------- */
/* Deferred work                                                               */
/* -------------------------------------------------------------------------- */

/* Most taps here end up deleting the object that was tapped (a page chip
 * rebuilds the bar, a chip in the editor rebuilds its card, OK closes the
 * keyboard), and LVGL must not delete an object inside its own event. So
 * closing hides the object and puts it in the trash, a rebuild is asked for,
 * and both happen in one lv_async_call right after the event. Not
 * lv_obj_delete_async(): that one does not notice when the object dies
 * first in a rebuild, and deletes it twice. */
static struct {
    lv_obj_t *trash[8];
    int       ntrash;
    bool      pending, build, params;
} L;

static void later_cb(void *arg)
{
    (void)arg;
    L.pending = false;
    for (int i = 0; i < L.ntrash; i++) if (L.trash[i]) lv_obj_delete(L.trash[i]);
    L.ntrash = 0;
    if (L.build) { L.build = L.params = false; build(); }
    if (L.params) { L.params = false; if (U.E && U.e_params) e_params_build(); }
}

static void later(void)
{
    if (!L.pending) { L.pending = true; lv_async_call(later_cb, NULL); }
}

static void trash(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (L.ntrash < (int)(sizeof L.trash / sizeof L.trash[0])) L.trash[L.ntrash++] = o;
    later();
}

static void rebuild(void) { L.build = true; later(); }
static void params_rebuild(void) { L.params = true; later(); }

/* -------------------------------------------------------------------------- */
/* Small pieces                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, f, c);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    if (cb) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
        lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    }
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT);
    lv_obj_center(l);
    return c;
}

static void chip_set(lv_obj_t *c, bool on)
{
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(c, 0), on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, 76, 76);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_center(aos_label(b, glyph, &aos_sym_28, AOS_C_TEXT));
    return b;
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 10, 0);
    return t;
}

static void scpy(char *d, size_t n, const char *s)
{
    if (!n) return;
    size_t l = s ? strlen(s) : 0;
    if (l >= n) l = n - 1;
    if (l) memcpy(d, s, l);
    d[l] = 0;
}

/* dark text on a light colour, white on the rest */
static lv_color_t ink_for(uint32_t rgb)
{
    float r = (float)((rgb >> 16) & 0xFF), g = (float)((rgb >> 8) & 0xFF), b = (float)(rgb & 0xFF);
    return 0.299f * r + 0.587f * g + 0.114f * b > 170 ? lv_color_hex(0x111111) : lv_color_white();
}

static void grid_dims(int *cols, int *rows)
{
    *cols = U.land ? 5 : 3;
    *rows = U.land ? 3 : 5;
}

/* -------------------------------------------------------------------------- */
/* Text entry (the on-screen keyboard over everything)                         */
/* -------------------------------------------------------------------------- */

static void (*s_text_done)(const char *);
static char s_text[1200];

static void overlay_close(void)
{
    trash(U.overlay);
    U.overlay = U.ta = U.kb = U.sug = U.live_ta = NULL;
}

static void text_event_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    if (!U.ta) return;
    scpy(s_text, sizeof s_text, lv_textarea_get_text(U.ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    overlay_close();
    if (done) done(s_text);
}

static void clear_text_cb(lv_event_t *e) { if (U.ta) lv_textarea_set_text(U.ta, ""); }

static void sug_pick_cb(lv_event_t *e)
{
    lv_obj_t *l = lv_obj_get_child(lv_event_get_current_target(e), 0);
    if (U.ta && l) lv_textarea_set_text(U.ta, lv_label_get_text(l));
}

static void text_entry(const char *title, const char *hint, const char *value, int max, bool multiline,
                       const char *const *sug, int nsug, void (*done)(const char *))
{
    overlay_close();
    s_text_done = done;
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, C_SHEET, 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(U.overlay, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, AOS_UI_PAD, U.land ? 16 : 26);
    int32_t ty = U.land ? 70 : 96;
    int32_t kh = U.land ? U.H / 2 : U.H * 2 / 5;
    int32_t th = multiline ? (U.H - kh - ty - 30 > 400 ? 400 : U.H - kh - ty - 30) : 88;
    U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, !multiline);
    lv_textarea_set_max_length(U.ta, (uint32_t)max);
    lv_textarea_set_text(U.ta, value ? value : "");
    if (hint) lv_textarea_set_placeholder_text(U.ta, hint);
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD - 88, th);
    lv_obj_align(U.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, ty);
    lv_obj_set_style_text_font(U.ta, multiline ? &aos_mono_22 : aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_hor(U.ta, 24, 0);
    lv_obj_set_style_pad_ver(U.ta, multiline ? 16 : 22, 0);
    lv_obj_t *x = round_btn(U.overlay, AOS_SYM_BACKSPACE_OUTLINE, clear_text_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, ty + 6);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, kh);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, text_event_cb, LV_EVENT_ALL, NULL);
    if (!multiline) lv_obj_add_event_cb(U.ta, text_event_cb, LV_EVENT_READY, NULL);
    if (nsug > 0) {
        int32_t sy = ty + th + 18, sh = U.H - kh - sy - 10;
        if (sh >= 60) {
            U.sug = box(U.overlay, U.W - 2 * AOS_UI_PAD, sh);
            lv_obj_set_pos(U.sug, AOS_UI_PAD, sy);
            lv_obj_set_flex_flow(U.sug, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_gap(U.sug, 10, 0);
            lv_obj_add_flag(U.sug, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(U.sug, LV_DIR_VER);
            for (int i = 0; i < nsug; i++) chip(U.sug, sug[i], false, sug_pick_cb, NULL);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Top bar: the pages, the trackpad, the USB                                   */
/* -------------------------------------------------------------------------- */

static void page_sheet_open(int page);

static void tab_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == TAB_TRACKPAD) S.face = FACE_TRACKPAD;
    else if (i == TAB_PAD) S.face = FACE_PAD;
    else if (i == TAB_MIDI) S.face = FACE_MIDI;
    else { S.face = FACE_PAGE; S.page = i; }
    rebuild();
}

static void tab_long_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0) page_sheet_open(i);
}

static void rename_page_done(const char *v);
static int s_rename_page = -1;

static void add_page_cb(lv_event_t *e)
{
    aos_macropad_lock();
    int n = aos_macropad_page_count();
    if (n < AOS_MP_PAGES) {
        cJSON *p = cJSON_CreateObject();
        char name[24];
        snprintf(name, sizeof name, _("Página %d"), n + 1);
        cJSON_AddStringToObject(p, "name", name);
        cJSON_AddArrayToObject(p, "buttons");
        cJSON_AddItemToArray(cJSON_GetObjectItem(aos_macropad_doc(), "pages"), p);
        aos_macropad_changed();
        S.page = n;
        S.face = FACE_PAGE;
    }
    aos_macropad_unlock();
    if (n >= AOS_MP_PAGES) { aos_ui_toast(_("Hay 8 páginas como mucho"), 1600); return; }
    rebuild();
    s_rename_page = S.page;
    text_entry(_("Nombre de la página"), NULL, "", 20, false, NULL, 0, rename_page_done);
}

static lv_obj_t *tab(lv_obj_t *parent, const char *glyph, const char *text, bool on, int idx)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, glyph && !text ? 20 : 24, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 8, 0);
    lv_color_t ink = on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT;
    if (glyph) aos_label(c, glyph, &aos_sym_28, ink);
    if (text) aos_label(c, text, aos_font_small, ink);
    aos_make_decorative(lv_obj_get_child(c, 0));
    if (idx == TAB_ADD) lv_obj_add_event_cb(c, add_page_cb, LV_EVENT_CLICKED, NULL);
    else {
        lv_obj_add_event_cb(c, tab_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_add_event_cb(c, tab_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
    }
    return c;
}

/* What this face needs from the port: the MIDI face wants the computer to
 * have opened the MIDI port too, the rest the keyboard (the gamepad goes out
 * on the same HID interface as the keyboard and the mouse). */
/* The keys go over Bluetooth when a computer took the board's keyboard mode
 * there (Settings, Bluetooth) and none has the cable's: the HAL picks the
 * way by itself, this only says which, on the chip. The gamepad and MIDI are
 * the cable's only. */
static bool via_bt(void)
{
    return aos_hal_bt_keyboard_ready() && !(aos_hal_usb_mode() == AOS_HAL_USB_KEYS && aos_hal_usb_connected());
}

static bool usb_ready(void)
{
    if (S.face == FACE_MIDI) return aos_hal_usb_midi_ready();
    if (S.face == FACE_PAD) return aos_hal_usb_keys_ready() && !via_bt();
    return aos_hal_usb_keys_ready();
}

/* Before sending: an idle port (CONSOLE) becomes the keyboard by itself, as
 * when the app opens; a DISK one is left alone, the computer has the card.
 * When it still is not ready, a toast says why, at most every 1.5 s (the
 * gamepad and the piano call this on every finger that lands). */
static bool usb_check(void)
{
    static uint32_t said;
    if (usb_ready()) return true;
    if (aos_hal_usb_mode() == AOS_HAL_USB_CONSOLE) aos_hal_usb_mode_set(AOS_HAL_USB_KEYS);
    if (usb_ready()) return true;
    if (said && lv_tick_elaps(said) < 1500) return false;
    said = lv_tick_get() | 1;
    const char *m = aos_hal_usb_mode() == AOS_HAL_USB_DISK ? _("El USB es un disco ahora (Ajustes → USB)")
                  : aos_hal_usb_busy()                     ? _("El USB se está preparando…")
                  : aos_hal_usb_keys_ready()               ? _("La computadora todavía no abrió el MIDI")
                                                           : _("Sin computadora por USB");
    aos_ui_toast(m, 1800);
    return false;
}

static void usb_cb(lv_event_t *e)
{
    const char *m;
    bool play = S.face == FACE_PAD || S.face == FACE_MIDI;
    static char bt_msg[96];
    if (usb_ready() && via_bt()) {
        char host[48];
        aos_text_safe(host, sizeof host, aos_hal_bt_keyboard_host());
        snprintf(bt_msg, sizeof bt_msg, _("%s usa la placa como teclado y mouse por Bluetooth"),
                 host[0] ? host : _("La computadora"));
        m = bt_msg;
    } else if (usb_ready()) m = S.face == FACE_PAD  ? _("La computadora tomó el mando del puerto OTG")
                       : S.face == FACE_MIDI ? _("La computadora tomó el MIDI del puerto OTG")
                                             : _("La computadora tomó el teclado y el mouse del puerto OTG");
    else if (aos_hal_usb_busy()) m = _("El USB se está preparando…");
    else if (S.face == FACE_MIDI && aos_hal_usb_keys_ready()) m = _("La computadora todavía no abrió el MIDI");
    else if (play) m = _("Conectá el puerto OTG de la placa a la computadora: la placa aparece como mando y como teclado MIDI");
    else m = _("Conectá el puerto OTG de la placa a la computadora: la placa aparece como teclado y mouse");
    aos_ui_toast(m, 2600);
}

static void usb_refresh(void)
{
    int st = usb_ready() ? (via_bt() ? 3 : 2) : aos_hal_usb_busy() ? 1 : 0;
    if (st == U.usb_state) return;
    U.usb_state = st;
    lv_obj_set_style_bg_color(U.usb_dot, st == 3 ? AOS_C_ACCENT : st == 2 ? AOS_C_GREEN : st == 1 ? AOS_C_ORANGE
                                                                                              : AOS_C_DIM, 0);
    lv_label_set_text(U.usb_txt, st == 3 ? "BLE" : st == 2 ? _("USB") : st == 1 ? _("USB…") : _("Sin USB"));
}

static void build_top(void)
{
    int32_t uw = 136;
    lv_obj_t *top = box(U.root, U.W, TOP_H);
    U.tabs = box(top, U.W - AOS_UI_PAD - uw - 12, TOP_H);
    lv_obj_set_pos(U.tabs, 0, 0);
    lv_obj_set_style_pad_left(U.tabs, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_right(U.tabs, 8, 0);
    lv_obj_set_flex_flow(U.tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(U.tabs, 10, 0);
    lv_obj_add_flag(U.tabs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.tabs, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(U.tabs, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *cur = NULL;
    aos_macropad_lock();
    int n = aos_macropad_page_count();
    for (int i = 0; i < n; i++) {
        bool on = S.face == FACE_PAGE && i == S.page;
        lv_obj_t *t = tab(U.tabs, NULL, aos_macropad_str(aos_macropad_page(i), "name"), on, i);
        if (on) cur = t;
    }
    aos_macropad_unlock();
    /* the fixed faces; upright there is no room for their names */
    static const struct { const char *glyph, *name; face_t face; int idx; } FACES[] = {
        { AOS_SYM_MOUSE, N_("Trackpad"), FACE_TRACKPAD, TAB_TRACKPAD },
        { AOS_SYM_GAMEPAD_VARIANT, N_("Mando"), FACE_PAD, TAB_PAD },
        { AOS_SYM_MUSIC, N_("MIDI"), FACE_MIDI, TAB_MIDI },
    };
    for (size_t i = 0; i < sizeof FACES / sizeof FACES[0]; i++) {
        lv_obj_t *t = tab(U.tabs, FACES[i].glyph, U.land ? _(FACES[i].name) : NULL, S.face == FACES[i].face, FACES[i].idx);
        if (S.face == FACES[i].face) cur = t;
    }
    if (n < AOS_MP_PAGES) tab(U.tabs, AOS_SYM_PLUS, NULL, false, TAB_ADD);
    if (cur) lv_obj_scroll_to_view(cur, LV_ANIM_OFF);

    U.usb = box(top, uw, 56);
    lv_obj_align(U.usb, LV_ALIGN_RIGHT_MID, -AOS_UI_PAD, 0);
    lv_obj_set_style_radius(U.usb, 28, 0);
    lv_obj_set_style_bg_color(U.usb, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(U.usb, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.usb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.usb, usb_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(U.usb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.usb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(U.usb, 10, 0);
    U.usb_dot = box(U.usb, 14, 14);
    aos_make_decorative(U.usb_dot);
    lv_obj_set_style_radius(U.usb_dot, 7, 0);
    lv_obj_set_style_bg_opa(U.usb_dot, LV_OPA_COVER, 0);
    U.usb_txt = aos_label(U.usb, "", aos_font_small, AOS_C_TEXT);
    U.usb_state = -1;
    usb_refresh();
}

/* -------------------------------------------------------------------------- */
/* The footer: what was sent                                                   */
/* -------------------------------------------------------------------------- */

static void foot_refresh(void)
{
    aos_mp_status_t st;
    aos_macropad_status(&st);
    if (st.seq == U.st_seq) return;
    U.st_seq = st.seq;
    if (!st.seq) {
        lv_label_set_text(U.foot_txt,
                          S.face == FACE_TRACKPAD ? _("Un dedo mueve · tocar = clic · dos dedos o la franja = rueda · pellizcar = zoom")
                          : S.face == FACE_PAD    ? _("Dos dedos a la vez: el stick y un botón")
                          : S.face == FACE_MIDI   ? _("Dos dedos, dos notas · deslizá para un glissando")
                                                  : _("Tocá un botón · mantenelo apretado para editarlo"));
        lv_obj_set_style_bg_color(U.foot_dot, AOS_C_DIM, 0);
        return;
    }
    lv_label_set_text(U.foot_txt, st.msg);
    lv_obj_set_style_bg_color(U.foot_dot, st.ok ? AOS_C_GREEN : AOS_C_RED, 0);
}

static void build_foot(void)
{
    lv_obj_t *f = box(U.root, U.W - 2 * AOS_UI_PAD, FOOT_H);
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f, 12, 0);
    U.foot_dot = box(f, 12, 12);
    lv_obj_set_style_radius(U.foot_dot, 6, 0);
    lv_obj_set_style_bg_opa(U.foot_dot, LV_OPA_COVER, 0);
    U.foot_txt = aos_label(f, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(U.foot_txt, U.W - 2 * AOS_UI_PAD - 24);
    lv_label_set_long_mode(U.foot_txt, LV_LABEL_LONG_MODE_DOTS);
    U.st_seq = (uint32_t)-1;
}

/* -------------------------------------------------------------------------- */
/* The buttons                                                                 */
/* -------------------------------------------------------------------------- */

static void editor_open(int page, int slot);

static bool word_on(const char *s)
{
    return !strcasecmp(s, "on") || !strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "open")
        || !strcasecmp(s, "playing") || !strcasecmp(s, "unlocked");
}

static bool word_off(const char *s)
{
    return !strcasecmp(s, "off") || !strcmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "closed")
        || !strcasecmp(s, "paused") || !strcasecmp(s, "idle") || !strcasecmp(s, "locked") || !strcasecmp(s, "standby");
}

/* An MQTT state payload: ON/OFF, or {"state":"ON"} / {"POWER":"OFF"}. 1 on, 0 off, -1 other. */
static int mqtt_onoff(const char *p)
{
    if (word_on(p)) return 1;
    if (word_off(p)) return 0;
    if (*p == '{') {
        cJSON *j = cJSON_Parse(p);
        int r = -1;
        static const char *const K[] = { "state", "POWER", "power", "value" };
        for (size_t i = 0; j && r < 0 && i < sizeof K / sizeof K[0]; i++) {
            cJSON *v = cJSON_GetObjectItem(j, K[i]);
            if (cJSON_IsString(v)) r = word_on(v->valuestring) ? 1 : word_off(v->valuestring) ? 0 : -1;
            else if (cJSON_IsBool(v)) r = cJSON_IsTrue(v);
        }
        cJSON_Delete(j);
        return r;
    }
    return -1;
}

static void number_comma(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

/* Paints a tile with what its entity or its state topic says now. */
static void tile_state(tile_t *t, const cJSON *b)
{
    aos_mp_type_t ty = aos_macropad_type(b);
    char txt[48] = "";
    int onoff = -1;
    bool pending = false;
    if (ty == AOS_MP_HA) {
        const char *id = aos_macropad_str(b, "entity");
        aos_ha_lock();
        int i = aos_ha_find(id);
        const aos_ha_entity_t *e = i >= 0 ? aos_ha_at(i) : NULL;
        if (e) {
            pending = e->pending;
            switch (e->domain) {
            case HA_LIGHT: case HA_SWITCH: case HA_INPUT_BOOLEAN: case HA_FAN:
                onoff = !strcmp(e->state, "on") ? 1 : !strcmp(e->state, "off") ? 0 : -1;
                break;
            case HA_COVER:  onoff = !strcmp(e->state, "open") ? 1 : !strcmp(e->state, "closed") ? 0 : -1; break;
            case HA_LOCK:   onoff = !strcmp(e->state, "unlocked") ? 1 : !strcmp(e->state, "locked") ? 0 : -1; break;
            case HA_MEDIA:  onoff = !strcmp(e->state, "playing") ? 1 : -1; break;
            case HA_SCENE: case HA_SCRIPT: case HA_BUTTON: break;
            default:
                snprintf(txt, sizeof txt, "%s%s%s", e->state, e->unit[0] ? " " : "", e->unit);
                number_comma(txt);
                break;
            }
            if (!strcmp(e->state, "unavailable")) { onoff = -1; scpy(txt, sizeof txt, _("no disponible")); }
            t->has_state = true;
        }
        aos_ha_unlock();
    } else if (ty == AOS_MP_MQTT && *aos_macropad_str(b, "state")) {
        aos_mqtt_lock();
        int slot = aos_mqtt_topic_find(aos_macropad_str(b, "state"));
        const aos_mqtt_topic_t *tp = slot >= 0 ? aos_mqtt_topic_at(slot) : NULL;
        if (tp) {
            onoff = mqtt_onoff(tp->payload);
            if (onoff < 0) { scpy(txt, sizeof txt, tp->payload); number_comma(txt); }
            t->has_state = true;
        }
        aos_mqtt_unlock();
    }
    t->toggle = onoff >= 0;
    t->on = onoff == 1;
    bool filled = !t->toggle || t->on;
    lv_obj_set_style_bg_color(t->obj, filled ? lv_color_hex(t->color) : AOS_C_CARD, 0);
    lv_color_t ink = filled ? ink_for(t->color) : AOS_C_TEXT;
    lv_obj_set_style_text_color(t->glyph, filled ? ink : lv_color_hex(t->color), 0);
    lv_obj_set_style_text_color(t->name, filled ? ink : AOS_C_DIM, 0);
    if (t->state) {
        lv_label_set_text(t->state, txt);
        lv_obj_set_style_text_color(t->state, ink, 0);
    }
    if (t->dot) {
        lv_obj_set_flag(t->dot, LV_OBJ_FLAG_HIDDEN, !t->toggle && !pending);
        lv_obj_set_style_bg_color(t->dot, pending ? AOS_C_ORANGE : t->on ? ink : lv_color_hex(0x48484A), 0);
    }
}

static void tile_click_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    aos_macropad_lock();
    bool empty = !aos_macropad_button(S.page, slot);
    aos_macropad_unlock();
    if (empty) { editor_open(S.page, slot); return; }
    aos_macropad_run(S.page, slot);
}

static void tile_long_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);          /* the release must not click it too */
    editor_open(S.page, slot);
}

static void tile_build(tile_t *t, lv_obj_t *parent, int32_t w, int32_t h, int slot, const cJSON *b)
{
    memset(t, 0, sizeof *t);
    lv_obj_t *o = box(parent, w, h);
    t->obj = o;
    lv_obj_set_style_radius(o, AOS_UI_RADIUS, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, tile_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)slot);
    lv_obj_add_event_cb(o, tile_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)slot);
    lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
    lv_obj_set_style_transform_scale(o, 236, LV_STATE_PRESSED);
    lv_obj_set_style_color_filter_dsc(o, &lv_color_filter_shade, LV_STATE_PRESSED);
    lv_obj_set_style_color_filter_opa(o, LV_OPA_30, LV_STATE_PRESSED);
    if (!b) {
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_set_style_border_color(o, AOS_C_CARD2, 0);
        lv_obj_center(aos_label(o, AOS_SYM_PLUS, &aos_sym_44, lv_color_hex(0x3A3A3C)));
        return;
    }
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    t->color = aos_macropad_color(b);
    bool big = h >= 150 && w >= 150;
    t->glyph = aos_label(o, aos_macropad_glyph(aos_macropad_str(b, "glyph")), big ? &aos_sym_72 : &aos_sym_44, AOS_C_TEXT);
    lv_obj_align(t->glyph, LV_ALIGN_CENTER, 0, big ? -18 : -14);
    t->name = aos_label(o, aos_macropad_str(b, "label"), aos_font_small, AOS_C_TEXT);
    lv_obj_set_size(t->name, w - 24, lv_font_get_line_height(aos_font_small));
    lv_obj_set_style_text_align(t->name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t->name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t->name, LV_ALIGN_BOTTOM_MID, 0, -12);
    aos_mp_type_t ty = aos_macropad_type(b);
    if (ty == AOS_MP_HA || (ty == AOS_MP_MQTT && *aos_macropad_str(b, "state"))) {
        t->state = aos_label(o, "", aos_font_caption, AOS_C_TEXT);
        lv_obj_align(t->state, LV_ALIGN_TOP_LEFT, 16, 12);
        t->dot = box(o, 16, 16);
        lv_obj_set_style_radius(t->dot, 8, 0);
        lv_obj_set_style_bg_opa(t->dot, LV_OPA_COVER, 0);
        lv_obj_align(t->dot, LV_ALIGN_TOP_RIGHT, -16, 16);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) aos_make_decorative(lv_obj_get_child(o, (int32_t)i));
    tile_state(t, b);
}

/* Paints the states again (HA or MQTT moved). */
static void tiles_refresh(void)
{
    if (S.face != FACE_PAGE) return;
    aos_macropad_lock();
    for (int i = 0; i < AOS_MP_SLOTS; i++) {
        tile_t *t = &U.tiles[i];
        if (!t->obj || !t->glyph) continue;
        const cJSON *b = aos_macropad_button(S.page, i);
        if (b) tile_state(t, b);
    }
    aos_macropad_unlock();
}

static void flash_off_cb(lv_timer_t *tm)
{
    int slot = (int)(intptr_t)lv_timer_get_user_data(tm);
    if (slot >= 0 && slot < AOS_MP_SLOTS && U.tiles[slot].obj)
        lv_obj_set_style_outline_width(U.tiles[slot].obj, 0, 0);
}

static void flash(int slot, bool ok)
{
    if (slot < 0 || slot >= AOS_MP_SLOTS || !U.tiles[slot].obj) return;
    lv_obj_t *o = U.tiles[slot].obj;
    lv_obj_set_style_outline_color(o, ok ? AOS_C_GREEN : AOS_C_RED, 0);
    lv_obj_set_style_outline_width(o, 6, 0);
    lv_obj_set_style_outline_pad(o, 3, 0);
    lv_obj_set_style_outline_opa(o, LV_OPA_90, 0);
    lv_timer_t *tm = lv_timer_create(flash_off_cb, 380, (void *)(intptr_t)slot);
    lv_timer_set_repeat_count(tm, 1);
}

static void build_pads(lv_obj_t *body, int32_t bw, int32_t bh)
{
    int cols, rows;
    grid_dims(&cols, &rows);
    int32_t tw = (bw - (cols - 1) * GAP) / cols, th = (bh - (rows - 1) * GAP) / rows;
    aos_macropad_lock();
    cJSON *page = aos_macropad_page(S.page);
    bool autoha = page && !strcmp(aos_macropad_str(page, "auto"), "ha")
                  && cJSON_GetArraySize(cJSON_GetObjectItem(page, "buttons")) == 0;
    if (autoha) {
        aos_macropad_unlock();
        lv_obj_t *c = card(body, bw, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 28, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 12, 0);
        lv_obj_t *r = box(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 14, 0);
        aos_label(r, AOS_SYM_HOME_ASSISTANT, &aos_sym_44, lv_color_hex(0x41BDF5));
        aos_label(r, _("Las escenas y luces de la casa"), aos_font_body, AOS_C_TEXT);
        aos_ha_state_t hs = aos_ha_state();
        const char *m = hs == AOS_HA_UNCONFIGURED
            ? _("Esta página se llena sola con las escenas, luces y enchufes de Home Assistant la primera vez que conecte. Configuralo desde el portal (Home Assistant) o con un ha.txt en la tarjeta.")
            : hs == AOS_HA_AUTH_FAILED ? _("Home Assistant rechazó el token: revisalo en el portal.")
            : _("Esperando a Home Assistant para llenar esta página…");
        wrap_label(c, m, aos_font_small, AOS_C_DIM, bw - 56);
        wrap_label(c, _("También podés agregar botones a mano: mantené apretada esta página arriba para vaciarla o renombrarla."),
                   aos_font_caption, AOS_C_DIM, bw - 56);
        return;
    }
    for (int i = 0; i < AOS_MP_SLOTS; i++) {
        int col = i % cols, row = i / cols;
        tile_build(&U.tiles[i], body, tw, th, i, aos_macropad_button(S.page, i));
        lv_obj_set_pos(U.tiles[i].obj, col * (tw + GAP), row * (th + GAP));
    }
    aos_macropad_unlock();
}

/* -------------------------------------------------------------------------- */
/* The trackpad page                                                           */
/* -------------------------------------------------------------------------- */

#define NOTCH_PX 26.0f

static void wheel_by(float dy)
{
    U.wheel += dy;
    /* fingers down, notch "towards you": with the Mac's natural scrolling
     * the page follows the fingers, as on a trackpad */
    while (U.wheel >= NOTCH_PX)  { aos_hal_usb_mouse(0, 0, -1); U.wheel -= NOTCH_PX; U.notches--; }
    while (U.wheel <= -NOTCH_PX) { aos_hal_usb_mouse(0, 0, 1);  U.wheel += NOTCH_PX; U.notches++; }
}

static void click(int button)
{
    if (!usb_check()) return;
    if (aos_hal_usb_click(button)) {
        aos_hal_beep(button == 2 ? 1000 : 1400, 10);
        aos_macropad_say(true, button == 2 ? _("Enviado: clic derecho") : _("Enviado: clic"));
    } else aos_macropad_say(false, _("No salió el clic"));
}

static void pad_gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    switch (ev->type) {
    case AOS_GESTURE_TAP: click(1); break;
    case AOS_GESTURE_DOUBLE_TAP: click(1); break;
    case AOS_GESTURE_LONG_PRESS: click(2); break;
    case AOS_GESTURE_DRAG_BEGIN: U.ax = U.ay = 0; U.moved = 0; break;
    case AOS_GESTURE_DRAG_END:
        if (U.moved) {
            char m[64];
            snprintf(m, sizeof m, _("Enviado: el puntero, %d px"), U.moved);
            aos_macropad_say(aos_hal_usb_keys_ready(), m);
        }
        break;
    case AOS_GESTURE_DRAG: {
        /* pointer speed grows with the finger's: slow for precision, fast
         * to cross a big screen */
        float d = sqrtf(ev->dx * ev->dx + ev->dy * ev->dy);
        float k = 0.45f * (float)S.speed * (1.0f + 0.08f * d);
        U.ax += ev->dx * k;
        U.ay += ev->dy * k;
        int mx = (int)U.ax, my = (int)U.ay;
        if (mx || my) {
            if (aos_hal_usb_mouse(mx, my, 0)) U.moved += abs(mx) + abs(my);
            U.ax -= (float)mx;
            U.ay -= (float)my;
        }
        break;
    }
    case AOS_GESTURE_PINCH_BEGIN: U.wheel = 0; U.notches = 0; U.zlog = 0; U.zmode = 0; U.zooms = 0; break;
    case AOS_GESTURE_PINCH:
        /* Two fingers scroll, or zoom when they spread or close: cmd+= and
         * cmd+-, what a Mac and a browser zoom with (the watch's Control PC
         * did the same). Whichever passes its threshold first owns the
         * gesture until the fingers lift, so a scroll never zooms by the
         * way. A step is a spread of 30 %: a whole pinch is four or five. */
        if (ev->scale > 0) U.zlog += logf(ev->scale);
        if (U.zmode != 2) wheel_by(ev->dy);
        if (!U.zmode && U.notches) U.zmode = 1;
        if (U.zmode != 1 && fabsf(U.zlog) > 0.262f) {
            if (!U.zmode) { U.zmode = 2; U.wheel = 0; }
            if (usb_check() && aos_hal_usb_key(U.zlog > 0 ? "cmd+=" : "cmd+-")) U.zooms += U.zlog > 0 ? 1 : -1;
            U.zlog = 0;
        }
        break;
    case AOS_GESTURE_PINCH_END:
        if (U.zooms) {
            char m[64];
            snprintf(m, sizeof m, _("Enviado: zoom %+d"), U.zooms);
            aos_macropad_say(aos_hal_usb_keys_ready(), m);
        } else if (U.notches) {
            char m[64];
            snprintf(m, sizeof m, _("Enviado: la rueda, %+d"), U.notches);
            aos_macropad_say(aos_hal_usb_keys_ready(), m);
        }
        break;
    default: break;
    }
}

static void strip_gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (ev->type == AOS_GESTURE_DRAG_BEGIN) { U.wheel = 0; U.notches = 0; }
    else if (ev->type == AOS_GESTURE_DRAG) wheel_by(ev->dy);
    else if (ev->type == AOS_GESTURE_DRAG_END && U.notches) {
        char m[64];
        snprintf(m, sizeof m, _("Enviado: la rueda, %+d"), U.notches);
        aos_macropad_say(aos_hal_usb_keys_ready(), m);
    }
}

static void lclick_cb(lv_event_t *e) { click(1); }
static void rclick_cb(lv_event_t *e) { click(2); }

static void drag_cb(lv_event_t *e)
{
    U.drag_lock = !U.drag_lock;
    if (!aos_hal_usb_mouse_hold(U.drag_lock ? 1 : 0) && U.drag_lock) {
        U.drag_lock = false;
        aos_ui_toast(_("Sin computadora por USB"), 1400);
    }
    chip_set(U.drag_btn, U.drag_lock);
}

static void speed_cb(lv_event_t *e)
{
    S.speed = S.speed % 4 + 1;
    lv_label_set_text_fmt(U.speed_lbl, _("Vel. %d"), S.speed);
}

static void key_cb(lv_event_t *e)
{
    aos_macropad_queue_key(lv_event_get_user_data(e));
}

/* live typing: whatever changes in the field goes to the computer */
static void live_changed_cb(lv_event_t *e)
{
    if (!U.live_ta) return;
    const char *now = lv_textarea_get_text(U.live_ta);
    size_t p = 0, lp = strlen(U.live_prev), ln = strlen(now);
    while (p < lp && p < ln && U.live_prev[p] == now[p]) p++;
    for (size_t i = p; i < lp; i++) aos_macropad_queue_key("backspace");
    if (ln > p) aos_macropad_queue_text(now + p);
    scpy(U.live_prev, sizeof U.live_prev, now);
    if (ln > 200) {                     /* a long session: keep the field short */
        lv_textarea_set_text(U.live_ta, "");
        U.live_prev[0] = 0;
    }
}

static void live_close(void) { overlay_close(); }

static void live_event_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_CANCEL) { live_close(); return; }
    if (c != LV_EVENT_READY || !U.live_ta) return;
    if (U.ready_tick == lv_tick_get()) return;      /* OK reaches the keyboard and the field */
    U.ready_tick = lv_tick_get();
    aos_macropad_queue_key("enter");
    U.live_prev[0] = 0;
    lv_textarea_set_text(U.live_ta, "");
}

static void live_close_cb(lv_event_t *e) { live_close(); }

static const struct { const char *label, *glyph, *key; } SPECIAL[] = {
    { "Esc", NULL, "esc" }, { "Tab", NULL, "tab" },
    { NULL, AOS_SYM_ARROW_LEFT_BOLD, "left" }, { NULL, AOS_SYM_ARROW_DOWN_BOLD, "down" },
    { NULL, AOS_SYM_ARROW_UP_BOLD, "up" }, { NULL, AOS_SYM_ARROW_RIGHT_BOLD, "right" },
    { NULL, AOS_SYM_BACKSPACE_OUTLINE, "backspace" }, { "Enter", NULL, "enter" },
};
#define N_SPECIAL ((int)(sizeof SPECIAL / sizeof SPECIAL[0]))

static lv_obj_t *key_btn(lv_obj_t *parent, int i, int32_t w, int32_t h)
{
    lv_obj_t *k = box(parent, w, h);
    lv_obj_set_style_radius(k, 18, 0);
    lv_obj_set_style_bg_color(k, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(k, key_cb, LV_EVENT_CLICKED, (void *)SPECIAL[i].key);
    lv_obj_center(SPECIAL[i].glyph ? aos_label(k, SPECIAL[i].glyph, &aos_sym_28, AOS_C_TEXT)
                                   : aos_label(k, SPECIAL[i].label, aos_font_small, AOS_C_TEXT));
    return k;
}

static void keys_row(lv_obj_t *parent, int32_t w, int32_t h, int per_row)
{
    lv_obj_t *r = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    int32_t kw = (w - (per_row - 1) * 10) / per_row;
    for (int i = 0; i < N_SPECIAL; i++) key_btn(r, i, kw, h);
}

static void live_open_cb(lv_event_t *e)
{
    overlay_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, C_SHEET, 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(U.overlay, _("Escribir en la computadora"), aos_font_title, AOS_C_TEXT),
                 LV_ALIGN_TOP_LEFT, AOS_UI_PAD, U.land ? 16 : 26);
    lv_obj_t *x = round_btn(U.overlay, AOS_SYM_CLOSE, live_close_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, U.land ? 8 : 18);
    int32_t ty = U.land ? 84 : 110, kh = U.land ? U.H / 2 : U.H * 2 / 5;
    U.live_ta = U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, true);
    lv_textarea_set_placeholder_text(U.ta, _("Lo que escribas va directo a la computadora"));
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(U.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, ty);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_hor(U.ta, 24, 0);
    lv_obj_set_style_pad_ver(U.ta, 22, 0);
    lv_obj_add_event_cb(U.ta, live_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(U.ta, live_event_cb, LV_EVENT_READY, NULL);
    U.live_prev[0] = 0;
    lv_obj_t *keys = box(U.overlay, U.W - 2 * AOS_UI_PAD, LV_SIZE_CONTENT);
    lv_obj_align(keys, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, ty + 88 + 18);
    keys_row(keys, U.W - 2 * AOS_UI_PAD, 70, U.land ? 8 : 4);
    wrap_label(U.overlay, _("Teclado de EE. UU.: los acentos y la ñ no llegan. Enter manda Enter."),
               aos_font_caption, AOS_C_DIM, U.W - 2 * AOS_UI_PAD);
    lv_obj_align(lv_obj_get_child(U.overlay, -1), LV_ALIGN_BOTTOM_LEFT, AOS_UI_PAD, -kh - 12);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, kh);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, live_event_cb, LV_EVENT_ALL, NULL);
}

static lv_obj_t *big_btn(lv_obj_t *parent, const char *glyph, const char *text, int32_t w, int32_t h, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent, w, h);
    lv_obj_set_style_radius(b, AOS_UI_RADIUS, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    if (glyph) aos_label(b, glyph, &aos_sym_28, AOS_C_TEXT);
    if (text) aos_label(b, text, aos_font_small, AOS_C_TEXT);
    return b;
}

static void build_trackpad(lv_obj_t *body, int32_t bw, int32_t bh)
{
    int32_t side = U.land ? bw * 42 / 100 : bw;          /* the controls' column */
    int32_t pad_w = U.land ? bw - side - GAP : bw;
    /* the controls: the field, the special keys, the clicks */
    lv_obj_t *ctl = box(body, side, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ctl, 12, 0);
    lv_obj_t *field = box(ctl, side, 80);
    lv_obj_set_style_radius(field, 20, 0);
    lv_obj_set_style_bg_color(field, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(field, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(field, live_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(aos_label(field, AOS_SYM_KEYBOARD, &aos_sym_28, C_MP), LV_ALIGN_LEFT_MID, 22, 0);
    lv_obj_align(aos_label(field, _("Escribir en la computadora…"), aos_font_small, AOS_C_DIM), LV_ALIGN_LEFT_MID, 70, 0);
    aos_make_decorative(lv_obj_get_child(field, 0));
    aos_make_decorative(lv_obj_get_child(field, 1));
    keys_row(ctl, side, 70, U.land ? 4 : 8);
    lv_obj_t *clicks = box(ctl, side, 96);
    lv_obj_set_flex_flow(clicks, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(clicks, 10, 0);
    int32_t cw = (side - 20) / 3;
    big_btn(clicks, AOS_SYM_CURSOR_DEFAULT_CLICK, _("Clic"), cw, 96, lclick_cb);
    lv_obj_t *d = box(clicks, cw, 96);
    lv_obj_set_style_radius(d, AOS_UI_RADIUS, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d, drag_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(aos_label(d, _("Arrastrar"), aos_font_small, AOS_C_TEXT));
    U.drag_btn = d;
    chip_set(d, U.drag_lock);
    big_btn(clicks, NULL, _("Clic derecho"), cw, 96, rclick_cb);
    lv_obj_update_layout(ctl);
    int32_t ctl_h = lv_obj_get_height(ctl);

    /* the pad and its wheel strip */
    int32_t pad_h = U.land ? bh : bh - ctl_h - GAP;
    int32_t strip_w = 84;
    lv_obj_t *pad = card(body, pad_w - strip_w - GAP, pad_h);
    lv_obj_set_style_bg_color(pad, lv_color_hex(0x161618), 0);
    lv_obj_set_style_border_width(pad, 2, 0);
    lv_obj_set_style_border_color(pad, AOS_C_CARD2, 0);
    lv_obj_t *hint = aos_label(pad, AOS_SYM_GESTURE_TAP_BUTTON, &aos_sym_72, lv_color_hex(0x2C2C2E));
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t *hint2 = aos_label(pad, _("Trackpad"), aos_font_body, lv_color_hex(0x3A3A3C));
    lv_obj_align(hint2, LV_ALIGN_CENTER, 0, 40);
    aos_make_decorative(hint);
    aos_make_decorative(hint2);
    lv_obj_t *sp = box(pad, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_radius(sp, 26, 0);
    lv_obj_set_style_pad_hor(sp, 18, 0);
    lv_obj_set_style_bg_color(sp, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(sp, LV_OPA_COVER, 0);
    lv_obj_add_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sp, speed_cb, LV_EVENT_CLICKED, NULL);
    U.speed_lbl = aos_label(sp, "", aos_font_caption, AOS_C_TEXT);
    lv_label_set_text_fmt(U.speed_lbl, _("Vel. %d"), S.speed);
    lv_obj_center(U.speed_lbl);
    lv_obj_align(sp, LV_ALIGN_TOP_RIGHT, -14, 14);
    /* the speed chip keeps its own clicks: the recogniser only hears
     * touches that start on the pad itself */
    aos_gesture_attach(pad, AOS_GESTURE_FLAG_FAST_TAP, pad_gesture_cb, NULL);

    lv_obj_t *strip = card(body, strip_w, pad_h);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0x161618), 0);
    lv_obj_set_style_border_width(strip, 2, 0);
    lv_obj_set_style_border_color(strip, AOS_C_CARD2, 0);
    lv_obj_align(aos_label(strip, AOS_SYM_CHEVRON_UP, &aos_sym_44, AOS_C_DIM), LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_align(aos_label(strip, AOS_SYM_CHEVRON_DOWN, &aos_sym_44, AOS_C_DIM), LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_t *wl = aos_label(strip, _("Rueda"), aos_font_caption, AOS_C_DIM);
    lv_obj_center(wl);
    for (uint32_t i = 0; i < lv_obj_get_child_count(strip); i++) aos_make_decorative(lv_obj_get_child(strip, (int32_t)i));
    aos_gesture_attach(strip, 0, strip_gesture_cb, NULL);

    if (U.land) {
        lv_obj_set_pos(pad, 0, 0);
        lv_obj_set_pos(strip, pad_w - strip_w, 0);
        lv_obj_set_pos(ctl, pad_w + GAP, 0);
    } else {
        lv_obj_set_pos(ctl, 0, 0);
        lv_obj_set_pos(pad, 0, ctl_h + GAP);
        lv_obj_set_pos(strip, pad_w - strip_w, ctl_h + GAP);
    }
}

/* -------------------------------------------------------------------------- */
/* The gamepad and the MIDI keyboard: the fingers one by one                   */
/* -------------------------------------------------------------------------- */

/* These two faces do not use LVGL's pointer, which is only ever the first
 * finger: a timer reads the fingers every PLAY_MS (aos_touch_points(), the
 * same filtered pair the recogniser uses) and hit-tests them against the
 * controls itself, in body coordinates. The controls are plain boxes that
 * only get painted; what LVGL does with the first finger over them is
 * nothing. The touch gives two fingers, so the left thumb can hold the stick
 * while the right one presses A, and the piano plays two notes at once.
 *
 * Each finger owns what it landed on until it lifts. On the stick and the
 * cross it keeps steering when it slides off them (a thumb drifts); on the
 * buttons and the keys it presses whatever is under it now, so sliding from
 * A to B moves the press and sliding along the keys is a glissando (note off
 * the old one, note on the new one), and sliding off the keyboard silences
 * it. A finger that was already down when the face was built (the rebuild
 * the portal can cause) is ignored until it lifts.
 *
 * The gamepad sends a report when something changed, from the timer, so at
 * most every PLAY_MS; while a thumb is on the stick it also sends every
 * GP_STREAM_MS even when nothing moved, and when every finger is up it sends
 * nothing at all. Leaving the face, the app or the foreground sends one
 * neutral report (centred, nothing pressed), and only if the last one was
 * not neutral already. The MIDI face sends note on and off as fingers land,
 * slide and lift; the pitch bend and the modulation on change, the bend in
 * steps of BEND_STEP; and leaving it releases every note, sends All Notes
 * Off (CC 123) and puts the bend back in the centre.
 *
 * The velocity is one of three, picked with chips above the keys, not taken
 * from where the finger lands: on keys this narrow the finger's height is
 * noise, and a player wants the same key to sound the same. */

#define PLAY_MS       10    /* the fingers are read this often */
#define GP_STREAM_MS  20    /* the stick's report while a thumb holds it */
#define BEND_STEP     64    /* of 16384: finer is noise from the finger */
#define KEYS_MAX      25    /* two octaves and the top C */

enum { OWN_NONE, OWN_STICK, OWN_DPAD, OWN_BTNS, OWN_KEYS, OWN_BEND, OWN_MOD };

typedef struct { int32_t x1, y1, x2, y2; } rect_t;

static rect_t rect_at(int32_t x, int32_t y, int32_t w, int32_t h)
{
    return (rect_t){ x, y, x + w, y + h };
}

static bool in_rect(const rect_t *r, int32_t x, int32_t y, int32_t slop)
{
    return x >= r->x1 - slop && x < r->x2 + slop && y >= r->y1 - slop && y < r->y2 + slop;
}

/* The buttons, with TinyUSB's bits (GAMEPAD_BUTTON_*): A 0, B 1, X 3, Y 4,
 * TL 6, TR 7, TL2 8, TR2 9, SELECT 10, START 11, MODE 12. The face buttons
 * wear the colours they have on most pads. */
enum { GB_A, GB_B, GB_X, GB_Y, GB_L, GB_R, GB_L2, GB_R2, GB_SELECT, GB_START, GB_HOME, GB_N };
static const struct { const char *label, *glyph; uint8_t bit; uint32_t color; } GB[GB_N] = {
    [GB_A] = { "A", NULL, 0, 0x30D158 },  [GB_B] = { "B", NULL, 1, 0xFF453A },
    [GB_X] = { "X", NULL, 3, 0x0A84FF },  [GB_Y] = { "Y", NULL, 4, 0xFFD60A },
    [GB_L] = { "L", NULL, 6, 0 },         [GB_R] = { "R", NULL, 7, 0 },
    [GB_L2] = { "L2", NULL, 8, 0 },       [GB_R2] = { "R2", NULL, 9, 0 },
    [GB_SELECT] = { "Select", NULL, 10, 0 }, [GB_START] = { "Start", NULL, 11, 0 },
    [GB_HOME] = { NULL, AOS_SYM_GAMEPAD_VARIANT, 12, 0 },
};

/* the cross, as TinyUSB's hat: 0 centred, 1 up, then clockwise to 8 up-left */
static const char *const HAT_TXT[9] = { "", "↑", "↑→", "→", "↓→", "↓", "↓←", "←", "↑←" };

static const char *const NOTE_TXT[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const uint8_t WHITE_OFF[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const uint8_t VEL[3] = { 48, 88, 120 };
static const char *const VEL_TXT[3] = { N_("Suave"), N_("Medio"), N_("Fuerte") };

typedef struct {
    rect_t    r;
    int       off;                  /* semitones above the base */
    bool      black, lit;
    lv_obj_t *o, *name;             /* name: "C4" on the C keys */
} pkey_t;

static struct {
    lv_timer_t *timer;
    struct { uint8_t id; int own, cur; int32_t x0, y0; } f[2];
    uint8_t  stale[2];          /* fingers down when the face was built */
    bool     covered;           /* a sheet or the keyboard is over the face */
    /* gamepad */
    lv_obj_t *base, *knob, *arm[4], *btn[GB_N], *readout;
    int32_t  scx, scy, sr, kr, travel;      /* the stick */
    int32_t  dcx, dcy, dr;                  /* the cross */
    rect_t   brc[GB_N];
    int      x, y, hat;
    unsigned buttons;
    int      sx, sy, shat;                  /* the last report sent */
    unsigned sbuttons;
    uint32_t sent_ms;
    unsigned lit_btns;
    int      lit_hat;
    bool     lit_knob;
    /* MIDI */
    pkey_t   key[KEYS_MAX];
    int      nkeys;
    lv_obj_t *oct_lbl, *vel_chip[3], *bend_knob, *mod_fill;
    rect_t   bend_rc, mod_rc;
    bool     vertical;              /* lying down the strips stand up */
    bool     lit_bend;
    uint8_t  held[128];             /* fingers on each note */
    int      bend;                  /* the last bend sent */
    bool     midi_used;             /* a note went out since the face opened */
} P;

static bool play_face(void) { return S.face == FACE_PAD || S.face == FACE_MIDI; }

static int base_note(void) { return 12 * (S.octave + 1); }

/* ---- the gamepad ---- */

static int hat_of(float dx, float dy, float dead)
{
    if (dx * dx + dy * dy < dead * dead) return 0;
    float a = atan2f(dx, -dy);                  /* 0 up, clockwise */
    int s = (int)lroundf(a / 0.78539816f);     /* eighths of a turn */
    return (s + 8) % 8 + 1;
}

static void gp_stick(int32_t x, int32_t y)
{
    float dx = (float)(x - P.scx), dy = (float)(y - P.scy), lim = (float)P.travel;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > lim) { dx *= lim / d; dy *= lim / d; }
    if (d < lim * 0.06f) dx = dy = 0;           /* a thumb resting in the middle is the middle */
    P.x = (int)lroundf(dx / lim * 127.0f);
    P.y = (int)lroundf(dy / lim * 127.0f);      /* HID: +y is down, as on the screen */
}

static int gp_button_at(int32_t x, int32_t y)
{
    for (int b = 0; b < GB_N; b++) if (P.btn[b] && in_rect(&P.brc[b], x, y, 10)) return b;
    return -1;
}

static void gp_paint(void)
{
    bool held = false;
    for (int i = 0; i < 2; i++) held |= P.f[i].own == OWN_STICK;
    if (P.knob) {
        lv_obj_set_pos(P.knob, P.scx - P.kr + P.x * P.travel / 127, P.scy - P.kr + P.y * P.travel / 127);
        if (held != P.lit_knob) {
            P.lit_knob = held;
            lv_obj_set_style_bg_color(P.knob, held ? C_MP : lv_color_hex(0x3A3A3C), 0);
        }
    }
    if (P.hat != P.lit_hat) {
        /* which arms a hat lights: up for 8 1 2, right for 2 3 4... */
        for (int a = 0; a < 4; a++) {
            int h1 = a * 2 + 1, h0 = h1 == 1 ? 8 : h1 - 1, h2 = h1 + 1;
            bool on = P.hat && (P.hat == h0 || P.hat == h1 || P.hat == h2);
            if (!P.arm[a]) continue;
            lv_obj_set_style_bg_color(P.arm[a], on ? C_MP : AOS_C_CARD2, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(P.arm[a], 0), on ? lv_color_hex(0x111111) : AOS_C_DIM, 0);
        }
        P.lit_hat = P.hat;
    }
    unsigned now = 0;
    for (int b = 0; b < GB_N; b++) if (P.buttons & (1u << GB[b].bit)) now |= 1u << b;
    for (int b = 0; b < GB_N; b++) {
        bool on = now & (1u << b);
        if (!P.btn[b] || on == !!(P.lit_btns & (1u << b))) continue;
        lv_obj_t *l = lv_obj_get_child(P.btn[b], 0);
        if (GB[b].color) {
            lv_obj_set_style_bg_color(P.btn[b], on ? lv_color_hex(GB[b].color) : AOS_C_CARD, 0);
            lv_obj_set_style_text_color(l, on ? lv_color_hex(0x111111) : lv_color_hex(GB[b].color), 0);
        } else {
            lv_obj_set_style_bg_color(P.btn[b], on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
            lv_obj_set_style_text_color(l, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
        }
    }
    P.lit_btns = now;
    if (P.readout) {
        char t[48];
        snprintf(t, sizeof t, "X %+d · Y %+d%s%s", P.x, P.y, P.hat ? " · " : "", HAT_TXT[P.hat]);
        if (strcmp(t, lv_label_get_text(P.readout))) lv_label_set_text(P.readout, t);
    }
}

/* One report if anything changed, or the steady one while the stick is held.
 * The HAL drops a report while the previous one is still in flight (false
 * with the port ready): then it is not counted as sent, and the next tick
 * tries again, or a released button would stay down on the computer. With
 * no computer it is dropped for good, so nothing repeats every tick. */
static void gp_send(void)
{
    bool held = false;
    for (int i = 0; i < 2; i++) held |= P.f[i].own == OWN_STICK;
    bool changed = P.x != P.sx || P.y != P.sy || P.hat != P.shat || P.buttons != P.sbuttons;
    if (!changed && !(held && lv_tick_elaps(P.sent_ms) >= GP_STREAM_MS)) return;
    bool ok = aos_hal_usb_gamepad(P.x, P.y, P.hat, P.buttons);
    if (!ok && aos_hal_usb_keys_ready()) return;
    bool more = (P.buttons & ~P.sbuttons) || (P.hat && P.hat != P.shat);
    if (ok && more) {
        /* the footer names what is pressed, not every step of the stick */
        char names[64] = "";
        size_t n = 0;
        for (int b = 0; b < GB_N; b++) {
            if (!(P.buttons & (1u << GB[b].bit))) continue;
            const char *nm = GB[b].label ? GB[b].label : "Home";
            n += (size_t)snprintf(names + n, sizeof names - n, "%s%s", n ? " + " : "", nm);
            if (n >= sizeof names) n = sizeof names - 1;
        }
        if (P.hat) snprintf(names + n, sizeof names - n, "%s%s", n ? " + " : "", HAT_TXT[P.hat]);
        char m[96];
        snprintf(m, sizeof m, _("Enviado: %s"), names);
        aos_macropad_say(true, m);
    }
    P.sx = P.x; P.sy = P.y; P.shat = P.hat; P.sbuttons = P.buttons;
    P.sent_ms = lv_tick_get();
}

/* The report that lets go of everything. It has no next tick to be retried
 * on, so a report in flight is waited out here, 10 ms at the most. */
static void gp_neutral(void)
{
    P.x = P.y = P.hat = 0;
    P.buttons = 0;
    if (P.sx || P.sy || P.shat || P.sbuttons)
        for (int i = 0; i < 10 && !aos_hal_usb_gamepad(0, 0, 0, 0) && aos_hal_usb_keys_ready(); i++)
            aos_hal_sleep_ms(1);
    P.sx = P.sy = P.shat = 0;
    P.sbuttons = 0;
}

/* ---- the MIDI keyboard ---- */

static void key_paint(void)
{
    int base = base_note();
    for (int k = 0; k < P.nkeys; k++) {
        int n = base + P.key[k].off;
        bool on = n >= 0 && n < 128 && P.held[n];
        if (on == P.key[k].lit) continue;
        P.key[k].lit = on;
        lv_obj_set_style_bg_color(P.key[k].o, on ? C_MP : P.key[k].black ? lv_color_hex(0x1C1C1E)
                                                                         : lv_color_hex(0xE5E5EA), 0);
    }
}

static void note_on(int n)
{
    if (n < 0 || n > 127) return;
    if (P.held[n]++) return;                    /* the other finger has it already */
    if (!aos_hal_usb_midi_note(n, VEL[S.vel], true)) return;
    P.midi_used = true;
    char m[48];
    snprintf(m, sizeof m, _("Enviado: %s%d"), NOTE_TXT[n % 12], n / 12 - 1);
    aos_macropad_say(true, m);
}

static void note_off(int n)
{
    if (n < 0 || n > 127 || !P.held[n]) return;
    if (--P.held[n]) return;
    aos_hal_usb_midi_note(n, 0, false);
}

static int key_at(int32_t x, int32_t y)
{
    /* the black keys lie on top of the white ones */
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < P.nkeys; k++)
            if (P.key[k].black == (pass == 0) && in_rect(&P.key[k].r, x, y, 0)) {
                int n = base_note() + P.key[k].off;
                return n <= 127 ? n : -1;
            }
    return -1;
}

static void bend_send(int v, bool force)
{
    if (v < -8192) v = -8192;
    if (v > 8191) v = 8191;
    if (v == P.bend) return;
    if (!force && abs(v - P.bend) < BEND_STEP && v != 0) return;
    aos_hal_usb_midi_bend(v);
    P.bend = v;
}

static void strips_paint(void)
{
    if (P.bend_knob) {
        int32_t kw = lv_obj_get_width(P.bend_knob), kh = lv_obj_get_height(P.bend_knob);
        int32_t w = P.bend_rc.x2 - P.bend_rc.x1, h = P.bend_rc.y2 - P.bend_rc.y1;
        if (P.vertical)
            lv_obj_set_pos(P.bend_knob, (w - kw) / 2, (h - kh) / 2 - P.bend * (h - kh - 16) / 2 / 8192);
        else
            lv_obj_set_pos(P.bend_knob, (w - kw) / 2 + P.bend * (w - kw - 16) / 2 / 8192, (h - kh) / 2);
        if (P.lit_bend != (P.bend != 0)) {
            P.lit_bend = P.bend != 0;
            lv_obj_set_style_bg_color(P.bend_knob, P.lit_bend ? C_MP : AOS_C_CARD2, 0);
        }
    }
    if (P.mod_fill) {
        int32_t w = P.mod_rc.x2 - P.mod_rc.x1, h = P.mod_rc.y2 - P.mod_rc.y1;
        if (P.vertical) {
            int32_t fh = h * S.mod / 127;
            lv_obj_set_size(P.mod_fill, w, fh);
            lv_obj_set_pos(P.mod_fill, 0, h - fh);
        } else {
            lv_obj_set_size(P.mod_fill, w * S.mod / 127, h);
            lv_obj_set_pos(P.mod_fill, 0, 0);
        }
        lv_obj_set_flag(P.mod_fill, LV_OBJ_FLAG_HIDDEN, S.mod == 0);
    }
}

static void midi_release(void)
{
    for (int i = 0; i < 2; i++) if (P.f[i].own == OWN_KEYS) P.f[i].cur = -1;
    for (int n = 0; n < 128; n++) if (P.held[n]) { P.held[n] = 1; note_off(n); }
    if (P.midi_used) aos_hal_usb_midi_cc(123, 0);       /* All Notes Off */
    if (P.bend) bend_send(0, true);
    P.midi_used = false;
}

/* ---- the fingers ---- */

static void finger_move(int i, int32_t x, int32_t y)
{
    switch (P.f[i].own) {
    case OWN_STICK: gp_stick(x, y); break;
    case OWN_DPAD:  P.hat = hat_of((float)(x - P.dcx), (float)(y - P.dcy), (float)P.dr * 0.22f); break;
    case OWN_BTNS:  P.f[i].cur = gp_button_at(x, y); break;
    case OWN_KEYS: {
        int n = key_at(x, y);
        if (n != P.f[i].cur) {                  /* a glissando, or off the keys */
            note_off(P.f[i].cur);
            note_on(n);
            P.f[i].cur = n;
        }
        break;
    }
    case OWN_BEND: {
        /* from where the finger landed, so touching it does not jump */
        float v = P.vertical ? (float)(P.f[i].y0 - y) / (float)((P.bend_rc.y2 - P.bend_rc.y1) / 2)
                             : (float)(x - P.f[i].x0) / (float)((P.bend_rc.x2 - P.bend_rc.x1) / 2);
        bend_send((int)lroundf(v * 8192.0f), false);
        break;
    }
    case OWN_MOD: {
        float v = P.vertical ? (float)(P.mod_rc.y2 - y) / (float)(P.mod_rc.y2 - P.mod_rc.y1)
                             : (float)(x - P.mod_rc.x1) / (float)(P.mod_rc.x2 - P.mod_rc.x1);
        int m = (int)lroundf(v * 127.0f);
        m = m < 0 ? 0 : m > 127 ? 127 : m;
        if (m != S.mod) { S.mod = m; aos_hal_usb_midi_cc(1, m); }
        break;
    }
    default: break;
    }
}

static void finger_down(int i, int32_t x, int32_t y)
{
    int own = OWN_NONE;
    if (S.face == FACE_PAD) {
        float dx = (float)(x - P.scx), dy = (float)(y - P.scy);
        float ex = (float)(x - P.dcx), ey = (float)(y - P.dcy);
        bool stick_busy = P.f[1 - i].own == OWN_STICK, dpad_busy = P.f[1 - i].own == OWN_DPAD;
        if (P.knob && !stick_busy && dx * dx + dy * dy <= (float)(P.sr * P.sr) * 1.3f) own = OWN_STICK;
        else if (P.arm[0] && !dpad_busy && ex * ex + ey * ey <= (float)(P.dr * P.dr) * 1.2f) own = OWN_DPAD;
        else if (gp_button_at(x, y) >= 0) own = OWN_BTNS;
    } else if (S.face == FACE_MIDI) {
        if (key_at(x, y) >= 0) own = OWN_KEYS;
        else if (P.bend_knob && in_rect(&P.bend_rc, x, y, 0) && P.f[1 - i].own != OWN_BEND) own = OWN_BEND;
        else if (P.mod_fill && in_rect(&P.mod_rc, x, y, 0)) own = OWN_MOD;
    }
    P.f[i].own = own;
    P.f[i].cur = -1;
    P.f[i].x0 = x;
    P.f[i].y0 = y;
    if (own != OWN_NONE) usb_check();
    finger_move(i, x, y);
}

static void finger_up(int i)
{
    switch (P.f[i].own) {
    case OWN_STICK: P.x = P.y = 0; break;       /* the spring */
    case OWN_DPAD:  P.hat = 0; break;
    case OWN_KEYS:  note_off(P.f[i].cur); break;
    case OWN_BEND:  bend_send(0, true); break;
    default: break;
    }
    P.f[i].own = OWN_NONE;
    P.f[i].cur = -1;
    P.f[i].id = 0;
}

static void play_paint(void)
{
    if (S.face == FACE_PAD) gp_paint();
    else if (S.face == FACE_MIDI) { key_paint(); strips_paint(); }
}

static void play_release(bool paint);
static void play_mark_stale(void);

static void play_poll(lv_timer_t *t)
{
    (void)t;
    if (!U.body || !play_face()) return;
    /* under a sheet or the text entry the fingers are theirs; and the ones
     * still down when it goes started on it */
    bool covered = U.sheet || U.overlay || U.picker;
    if (covered != P.covered) {
        P.covered = covered;
        if (covered) play_release(true);
        else play_mark_stale();
    }
    if (covered) return;
    aos_touch_point_t pts[2];
    aos_touch_points(pts);
    lv_area_t a;
    lv_obj_get_coords(U.body, &a);      /* the runtime may slide the root */
    for (int s = 0; s < 2; s++) {
        if (P.stale[s] && !(pts[0].down && pts[0].id == P.stale[s]) && !(pts[1].down && pts[1].id == P.stale[s]))
            P.stale[s] = 0;
    }
    for (int i = 0; i < 2; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        if (id && (id == P.stale[0] || id == P.stale[1])) id = 0;
        int32_t x = (int32_t)pts[i].x - a.x1, y = (int32_t)pts[i].y - a.y1;
        if (id != P.f[i].id) {
            if (P.f[i].id) finger_up(i);
            P.f[i].id = id;
            if (id) finger_down(i, x, y);
        } else if (id) finger_move(i, x, y);
    }
    if (S.face == FACE_PAD) {
        P.buttons = 0;
        for (int i = 0; i < 2; i++)
            if (P.f[i].own == OWN_BTNS && P.f[i].cur >= 0) P.buttons |= 1u << GB[P.f[i].cur].bit;
        gp_send();
    }
    play_paint();
}

/* Everything up and silent: leaving the face, the app, the foreground, or a
 * rebuild. 'paint' = the objects are still there to show it. */
static void play_release(bool paint)
{
    for (int i = 0; i < 2; i++) if (P.f[i].id) finger_up(i);
    gp_neutral();
    midi_release();
    if (paint && U.body) play_paint();
}

/* The fingers down now are not ours: they started on something else. */
static void play_mark_stale(void)
{
    aos_touch_point_t pts[2];
    aos_touch_points(pts);
    for (int i = 0; i < 2; i++) P.stale[i] = pts[i].down ? pts[i].id : 0;
}

/* The objects of the face that is going; the sounding state is released
 * first by the caller. */
static void play_forget(void)
{
    P.base = P.knob = P.readout = NULL;
    memset(P.arm, 0, sizeof P.arm);
    memset(P.btn, 0, sizeof P.btn);
    P.lit_btns = 0;
    P.lit_hat = 0;
    P.lit_knob = false;
    P.nkeys = 0;
    P.oct_lbl = P.bend_knob = P.mod_fill = NULL;
    P.lit_bend = false;
    memset(P.vel_chip, 0, sizeof P.vel_chip);
}

/* ---- the gamepad face ---- */

static lv_obj_t *gp_btn(lv_obj_t *body, int b, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius)
{
    lv_obj_t *o = box(body, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (GB[b].color) {
        lv_obj_set_style_bg_color(o, AOS_C_CARD, 0);
        lv_obj_set_style_border_width(o, 3, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(GB[b].color), 0);
        lv_obj_set_style_border_opa(o, LV_OPA_50, 0);
        lv_obj_center(aos_label(o, GB[b].label, aos_font_title, lv_color_hex(GB[b].color)));
    } else {
        lv_obj_set_style_bg_color(o, AOS_C_CARD2, 0);
        lv_obj_center(GB[b].glyph ? aos_label(o, GB[b].glyph, &aos_sym_28, AOS_C_TEXT)
                                  : aos_label(o, GB[b].label, h >= 70 ? aos_font_body : aos_font_small, AOS_C_TEXT));
    }
    aos_make_decorative(lv_obj_get_child(o, 0));
    P.btn[b] = o;
    P.brc[b] = rect_at(x, y, w, h);
    return o;
}

static void gp_stick_build(lv_obj_t *body, int32_t cx, int32_t cy, int32_t r)
{
    P.scx = cx; P.scy = cy; P.sr = r;
    P.kr = r * 38 / 100;
    P.travel = r - P.kr;
    lv_obj_t *b = box(body, 2 * r, 2 * r);
    lv_obj_set_pos(b, cx - r, cy - r);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x161618), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, AOS_C_CARD2, 0);
    /* the ring the knob's centre travels in */
    lv_obj_t *ring = box(b, 2 * P.travel, 2 * P.travel);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x242426), 0);
    aos_make_decorative(ring);
    P.base = b;
    P.knob = box(body, 2 * P.kr, 2 * P.kr);
    lv_obj_set_style_radius(P.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(P.knob, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(P.knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(P.knob, 3, 0);
    lv_obj_set_style_border_color(P.knob, lv_color_hex(0x48484A), 0);
    lv_obj_set_pos(P.knob, cx - P.kr, cy - P.kr);
}

static void gp_dpad_build(lv_obj_t *body, int32_t cx, int32_t cy, int32_t r)
{
    P.dcx = cx; P.dcy = cy; P.dr = r;
    int32_t aw = r * 64 / 100, al = r - aw / 2 + 20;    /* the arm reaches under the centre */
    static const char *const G[4] = { AOS_SYM_CHEVRON_UP, AOS_SYM_CHEVRON_RIGHT, AOS_SYM_CHEVRON_DOWN, AOS_SYM_CHEVRON_LEFT };
    for (int a = 0; a < 4; a++) {
        bool vert = a == 0 || a == 2;
        lv_obj_t *o = box(body, vert ? aw : al, vert ? al : aw);
        int32_t x = a == 1 ? cx + aw / 2 - 20 : a == 3 ? cx - r : cx - aw / 2;
        int32_t y = a == 0 ? cy - r : a == 2 ? cy + aw / 2 - 20 : cy - aw / 2;
        lv_obj_set_pos(o, x, y);
        lv_obj_set_style_radius(o, 18, 0);
        lv_obj_set_style_bg_color(o, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_t *g = aos_label(o, G[a], &aos_sym_44, AOS_C_DIM);
        /* the chevron towards the tip, clear of the centre */
        lv_obj_align(g, a == 0 ? LV_ALIGN_TOP_MID : a == 1 ? LV_ALIGN_RIGHT_MID : a == 2 ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_LEFT_MID,
                     0, 0);
        lv_obj_set_style_pad_all(g, 10, 0);
        aos_make_decorative(g);
        P.arm[a] = o;
    }
    lv_obj_t *c = box(body, aw, aw);
    lv_obj_set_pos(c, cx - aw / 2, cy - aw / 2);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_t *dot = box(c, 24, 24);
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, 12, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
}

static void gp_face_build(lv_obj_t *body, int32_t cx, int32_t cy, int32_t size)
{
    int32_t d = size * 34 / 100, o = size / 2 - d / 2;
    gp_btn(body, GB_Y, cx - d / 2, cy - o - d / 2, d, d, LV_RADIUS_CIRCLE);
    gp_btn(body, GB_X, cx - o - d / 2, cy - d / 2, d, d, LV_RADIUS_CIRCLE);
    gp_btn(body, GB_B, cx + o - d / 2, cy - d / 2, d, d, LV_RADIUS_CIRCLE);
    gp_btn(body, GB_A, cx - d / 2, cy + o - d / 2, d, d, LV_RADIUS_CIRCLE);
}

static void build_gamepad(lv_obj_t *body, int32_t bw, int32_t bh)
{
    /* 8 px in from the body's sides: the back swipe starts at the screen's
     * left 28 px, and a thumb there would take the app away */
    const int32_t M = 8;
    int32_t sh = U.land ? 72 : 84;              /* the shoulder row */
    int32_t sw = U.land ? 132 : (bw - 2 * M - 3 * GAP - 36) / 4;
    gp_btn(body, GB_L2, M, 0, sw, sh, 20);
    gp_btn(body, GB_L, M + sw + GAP, 0, sw, sh, 20);
    gp_btn(body, GB_R, bw - M - 2 * sw - GAP, 0, sw, sh, 20);
    gp_btn(body, GB_R2, bw - M - sw, 0, sw, sh, 20);
    /* Select, Home, Start: in the shoulder row lying down, a row of their
     * own upright */
    int32_t ph = 60, pw = 136, hw = 76;
    int32_t py = U.land ? (sh - ph) / 2 : sh + GAP;
    int32_t px = (bw - (2 * pw + hw + 2 * GAP)) / 2;
    gp_btn(body, GB_SELECT, px, py, pw, ph, ph / 2);
    gp_btn(body, GB_HOME, px + pw + GAP, py, hw, ph, ph / 2);
    gp_btn(body, GB_START, px + pw + hw + 2 * GAP, py, pw, ph, ph / 2);

    int32_t y0 = U.land ? sh + GAP : py + ph + GAP;
    P.readout = aos_label(body, "", aos_font_caption, AOS_C_DIM);
    if (U.land) {
        /* stick, cross, buttons in a line, the stick and A level with each other */
        int32_t h = bh - y0, size = LV_MIN(h - 8, 380);
        int32_t cy = y0 + h / 2;
        gp_stick_build(body, M + size / 2, cy, size / 2);
        gp_face_build(body, bw - M - size / 2, cy, size);
        int32_t dr = LV_MIN(size * 40 / 100, 150);
        int32_t dcx = (M + size + bw - M - size) / 2;
        gp_dpad_build(body, dcx, cy - 20, dr);
        lv_obj_align(P.readout, LV_ALIGN_TOP_MID, dcx - bw / 2, cy - 20 + dr + 14);
    } else {
        /* the thumbs' row at the bottom: the stick and the face buttons;
         * the cross above the stick, the readout above the buttons */
        int32_t size = LV_MIN((bw - 2 * M - GAP) / 2, 340);
        int32_t cy = bh - M - size / 2;
        gp_stick_build(body, M + size / 2, cy, size / 2);
        gp_face_build(body, bw - M - size / 2, cy, size);
        int32_t mid_top = y0, mid_bot = cy - size / 2 - GAP;
        int32_t dr = LV_MIN((mid_bot - mid_top) / 2, 150);
        int32_t dcy = (mid_top + mid_bot) / 2;
        gp_dpad_build(body, M + size / 2, dcy, dr);
        lv_obj_align(P.readout, LV_ALIGN_TOP_MID, bw - M - size / 2 - bw / 2, dcy - 12);
    }
    aos_make_decorative(P.readout);
    P.lit_hat = -1;         /* paint the cross once */
    gp_paint();
}

/* ---- the MIDI face ---- */

static void oct_paint(void)
{
    if (P.oct_lbl) lv_label_set_text_fmt(P.oct_lbl, "C%d", S.octave);
    int base = base_note();
    for (int k = 0; k < P.nkeys; k++)
        if (P.key[k].name) lv_label_set_text_fmt(P.key[k].name, "C%d", (base + P.key[k].off) / 12 - 1);
}

static void oct_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int top = 0;
    for (int k = 0; k < P.nkeys; k++) if (P.key[k].off > top) top = P.key[k].off;
    int o = S.octave + d;
    if (o < -1 || 12 * (o + 1) + top > 127) return;
    /* the notes that sound stop; a finger still on a key plays it again
     * an octave away on its next sample */
    for (int i = 0; i < 2; i++) if (P.f[i].own == OWN_KEYS) { note_off(P.f[i].cur); P.f[i].cur = -2; }
    S.octave = o;
    oct_paint();
    key_paint();
}

static void vel_cb(lv_event_t *e)
{
    S.vel = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < 3; i++) if (P.vel_chip[i]) chip_set(P.vel_chip[i], i == S.vel);
}

/* One row of keys: 'nwhite' white keys from C, the first at note 'off0'
 * above the base. The black ones go on top, so they are made after. */
static void kb_row(lv_obj_t *body, int32_t x, int32_t y, int32_t w, int32_t h, int nwhite, int off0)
{
    const int32_t g = 4;
    int32_t ww = (w - (nwhite - 1) * g) / nwhite;
    for (int i = 0; i < nwhite && P.nkeys < KEYS_MAX; i++) {
        int off = off0 + 12 * (i / 7) + WHITE_OFF[i % 7];
        int32_t kx = x + i * (ww + g);
        lv_obj_t *o = box(body, ww, h);
        lv_obj_set_pos(o, kx, y);
        lv_obj_set_style_radius(o, 10, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(0xE5E5EA), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_t *name = NULL;
        if (off % 12 == 0) {
            name = aos_label(o, "", aos_font_caption, lv_color_hex(0x636366));
            lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -12);
            aos_make_decorative(name);
        }
        P.key[P.nkeys++] = (pkey_t){ .r = rect_at(kx, y, ww, h), .off = off, .o = o, .name = name };
    }
    int32_t bkw = ww * 60 / 100, bkh = h * 58 / 100;
    for (int i = 0; i < nwhite - 1 && P.nkeys < KEYS_MAX; i++) {
        int w7 = i % 7;
        if (w7 == 2 || w7 == 6) continue;       /* no black key after E and B */
        int off = off0 + 12 * (i / 7) + WHITE_OFF[w7] + 1;
        int32_t kx = x + (i + 1) * (ww + g) - g / 2 - bkw / 2;
        lv_obj_t *o = box(body, bkw, bkh);
        lv_obj_set_pos(o, kx, y);
        lv_obj_set_style_radius(o, 8, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x1C1C1E), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 3, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(0x000000), 0);
        P.key[P.nkeys++] = (pkey_t){ .r = rect_at(kx, y, bkw, bkh), .off = off, .black = true, .o = o };
    }
}

static lv_obj_t *strip_build(lv_obj_t *body, rect_t r, const char *name)
{
    lv_obj_t *s = card(body, r.x2 - r.x1, r.y2 - r.y1);
    lv_obj_set_pos(s, r.x1, r.y1);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x161618), 0);
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_border_color(s, AOS_C_CARD2, 0);
    lv_obj_set_style_border_post(s, true, 0);   /* over the modulation's fill */
    lv_obj_set_style_clip_corner(s, true, 0);
    lv_obj_t *l = aos_label(s, name, aos_font_caption, AOS_C_DIM);
    lv_obj_align(l, P.vertical ? LV_ALIGN_TOP_MID : LV_ALIGN_LEFT_MID, P.vertical ? 0 : 18, P.vertical ? 14 : 0);
    aos_make_decorative(l);
    return s;
}

static void build_midi(lv_obj_t *body, int32_t bw, int32_t bh)
{
    P.vertical = U.land;
    int32_t ch = 76;
    /* the octave, and the velocity */
    lv_obj_t *oct = box(body, LV_SIZE_CONTENT, ch);
    lv_obj_set_flex_flow(oct, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(oct, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(oct, 8, 0);
    round_btn(oct, AOS_SYM_MINUS, oct_cb, (void *)(intptr_t)-1);
    P.oct_lbl = aos_label(oct, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(P.oct_lbl, 96);
    lv_obj_set_style_text_align(P.oct_lbl, LV_TEXT_ALIGN_CENTER, 0);
    round_btn(oct, AOS_SYM_PLUS, oct_cb, (void *)(intptr_t)1);
    lv_obj_set_pos(oct, 0, 0);
    lv_obj_t *vel = box(body, LV_SIZE_CONTENT, ch);
    lv_obj_set_flex_flow(vel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vel, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vel, 8, 0);
    for (int i = 0; i < 3; i++)
        P.vel_chip[i] = chip(vel, _(VEL_TXT[i]), i == S.vel, vel_cb, (void *)(intptr_t)i);
    lv_obj_align(vel, LV_ALIGN_TOP_RIGHT, 0, 0);

    int32_t top = ch + GAP;
    if (U.land) {
        /* the wheels to the left of the keys, as on a keyboard */
        int32_t sw = 92, h = bh - top;
        P.bend_rc = rect_at(0, top, sw, h);
        P.mod_rc = rect_at(sw + GAP, top, sw, h);
        int32_t kx = 2 * (sw + GAP);
        kb_row(body, kx, top, bw - kx, h, 15, 0);
    } else {
        /* the ribbons above, and two rows of keys: the upper one is the
         * octave above, as on an organ */
        int32_t rh = 92;
        P.bend_rc = rect_at(0, top, bw, rh);
        P.mod_rc = rect_at(0, top + rh + GAP, bw, rh);
        int32_t ky = top + 2 * (rh + GAP), kh = (bh - ky - GAP) / 2;
        kb_row(body, 0, ky + kh + GAP, bw, kh, 7, 0);
        kb_row(body, 0, ky, bw, kh, 7, 12);
    }
    lv_obj_t *bs = strip_build(body, P.bend_rc, _("Bend"));
    int32_t bsw = P.bend_rc.x2 - P.bend_rc.x1, bsh = P.bend_rc.y2 - P.bend_rc.y1;
    lv_obj_t *mid = box(bs, P.vertical ? bsw - 24 : 2, P.vertical ? 2 : bsh - 24);  /* the centre */
    lv_obj_center(mid);
    lv_obj_set_style_bg_color(mid, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, 0);
    P.bend_knob = box(bs, P.vertical ? bsw - 20 : 44, P.vertical ? 44 : bsh - 20);
    lv_obj_set_style_radius(P.bend_knob, 14, 0);
    lv_obj_set_style_bg_color(P.bend_knob, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(P.bend_knob, LV_OPA_COVER, 0);
    lv_obj_t *ms = strip_build(body, P.mod_rc, _("Mod."));
    P.mod_fill = box(ms, 1, 1);
    lv_obj_set_style_bg_color(P.mod_fill, C_MP, 0);
    lv_obj_set_style_bg_opa(P.mod_fill, LV_OPA_40, 0);
    lv_obj_move_to_index(P.mod_fill, 0);        /* under the label */
    oct_paint();
    key_paint();
    strips_paint();
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static void build_body(void)
{
    if (U.body) lv_obj_delete(U.body);
    memset(U.tiles, 0, sizeof U.tiles);
    int32_t bw = U.W - 2 * AOS_UI_PAD, bh = U.H - TOP_H - FOOT_H - 4;
    U.body = box(U.root, bw, bh);
    lv_obj_set_pos(U.body, AOS_UI_PAD, TOP_H);
    aos_macropad_lock();
    int n = aos_macropad_page_count();
    aos_macropad_unlock();
    if (S.page >= n) S.page = n - 1;
    if (S.page < 0) S.page = 0;
    switch (S.face) {
    case FACE_TRACKPAD: build_trackpad(U.body, bw, bh); break;
    case FACE_PAD:      build_gamepad(U.body, bw, bh); break;
    case FACE_MIDI:     build_midi(U.body, bw, bh); break;
    default:            build_pads(U.body, bw, bh); break;
    }
}

static void build(void)
{
    if (U.drag_lock && S.face != FACE_TRACKPAD) { aos_hal_usb_mouse_hold(0); U.drag_lock = false; }
    play_release(false);            /* whatever face it was: nothing held across a rebuild */
    play_forget();
    lv_obj_clean(U.root);
    L.ntrash = 0;                   /* the trash went with the rest */
    if (U.E) { cJSON_Delete(U.E); U.E = NULL; }
    U.e_params = U.e_name = NULL;
    U.body = NULL;
    U.overlay = U.ta = U.kb = U.sug = U.live_ta = U.sheet = U.picker = NULL;
    memset(U.tiles, 0, sizeof U.tiles);
    build_top();
    build_body();
    build_foot();
    U.doc_ver = aos_macropad_version();
    foot_refresh();
    if (play_face()) {
        if (!P.timer) P.timer = lv_timer_create(play_poll, PLAY_MS, NULL);
        play_mark_stale();
    } else if (P.timer) {
        lv_timer_delete(P.timer);
        P.timer = NULL;
    }
}

/* -------------------------------------------------------------------------- */
/* Page actions (long press on a page's chip)                                  */
/* -------------------------------------------------------------------------- */

static void sheet_close(void)
{
    trash(U.picker);
    trash(U.sheet);
    U.sheet = U.picker = U.e_params = U.e_name = NULL;
    if (U.E) { cJSON_Delete(U.E); U.E = NULL; }
    if (U.dirty) { U.dirty = false; rebuild(); }
}

static void sheet_close_cb(lv_event_t *e) { sheet_close(); }

static lv_obj_t *sheet_new(const char *title)
{
    U.sheet = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.sheet, C_SHEET, 0);
    lv_obj_set_style_bg_opa(U.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *col = box(U.sheet, U.W, U.H);
    lv_obj_set_style_pad_hor(col, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(col, 16, 0);
    lv_obj_set_style_pad_bottom(col, 40, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 14, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_t *head = box(col, U.W - 2 * AOS_UI_PAD, 76);
    lv_obj_align(aos_label(head, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    return col;
}

static void rename_page_done(const char *v)
{
    if (s_rename_page < 0 || !v[0]) return;
    aos_macropad_lock();
    cJSON *p = aos_macropad_page(s_rename_page);
    if (p) {
        cJSON_ReplaceItemInObject(p, "name", cJSON_CreateString(v));
        aos_macropad_changed();
    }
    aos_macropad_unlock();
    s_rename_page = -1;
    rebuild();
}

static void page_rename_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char name[48] = "";
    aos_macropad_lock();
    scpy(name, sizeof name, aos_macropad_str(aos_macropad_page(i), "name"));
    aos_macropad_unlock();
    sheet_close();
    s_rename_page = i;
    text_entry(_("Nombre de la página"), NULL, name, 20, false, NULL, 0, rename_page_done);
}

static void page_move_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int i = (int)(v >> 4), dir = (v & 1) ? 1 : -1;
    aos_macropad_lock();
    cJSON *pages = cJSON_GetObjectItem(aos_macropad_doc(), "pages");
    int n = cJSON_GetArraySize(pages), j = i + dir;
    if (j >= 0 && j < n) {
        cJSON *p = cJSON_DetachItemFromArray(pages, i);
        cJSON_InsertItemInArray(pages, j, p);
        aos_macropad_changed();
        S.page = j;
    }
    aos_macropad_unlock();
    sheet_close();
    rebuild();
}

static void page_clear_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    aos_macropad_lock();
    cJSON *p = aos_macropad_page(i);
    if (p) {
        cJSON_DeleteItemFromObject(p, "auto");
        cJSON_ReplaceItemInObject(p, "buttons", cJSON_CreateArray());
        aos_macropad_changed();
    }
    aos_macropad_unlock();
    sheet_close();
    rebuild();
}

static void page_delete_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (U.confirm_delete != i + 1) {
        U.confirm_delete = i + 1;
        lv_label_set_text(lv_obj_get_child(lv_event_get_current_target(e), 0), _("Tocá otra vez para borrarla"));
        return;
    }
    U.confirm_delete = 0;
    aos_macropad_lock();
    cJSON *pages = cJSON_GetObjectItem(aos_macropad_doc(), "pages");
    if (cJSON_GetArraySize(pages) > 1) {
        cJSON_DeleteItemFromArray(pages, i);
        aos_macropad_changed();
    }
    aos_macropad_unlock();
    if (S.page >= i && S.page > 0) S.page--;
    sheet_close();
    rebuild();
}

static void page_sheet_open(int page)
{
    sheet_close();
    U.confirm_delete = 0;
    char name[48];
    aos_macropad_lock();
    scpy(name, sizeof name, aos_macropad_str(aos_macropad_page(page), "name"));
    int n = aos_macropad_page_count();
    aos_macropad_unlock();
    lv_obj_t *col = sheet_new(name);
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *r = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 12, 0);
    chip(r, _("Renombrar"), false, page_rename_cb, (void *)(intptr_t)page);
    if (page > 0) chip(r, _("Mover a la izquierda"), false, page_move_cb, (void *)(intptr_t)(page << 4));
    if (page < n - 1) chip(r, _("Mover a la derecha"), false, page_move_cb, (void *)(intptr_t)((page << 4) | 1));
    chip(r, _("Vaciar"), false, page_clear_cb, (void *)(intptr_t)page);
    if (n > 1) {
        lv_obj_t *d = chip(r, _("Borrar la página"), false, page_delete_cb, (void *)(intptr_t)page);
        lv_obj_set_style_bg_color(d, AOS_C_RED, 0);
    }
    chip(r, _("Cancelar"), false, sheet_close_cb, NULL);
    wrap_label(col, _("Desde el portal (Macro pad) se ordenan los botones arrastrándolos y se edita todo más cómodo."),
               aos_font_caption, AOS_C_DIM, w);
}

/* -------------------------------------------------------------------------- */
/* The button editor                                                           */
/* -------------------------------------------------------------------------- */

static const uint32_t SWATCHES[] = {
    0x0A84FF, 0x5E5CE6, 0xBF5AF2, 0xFF375F, 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158,
    0x10B981, 0x40C8E0, 0x0E7490, 0xA2845E, 0xD97757, 0x8E8E93, 0x3A3A3C, 0xF2F2F7,
};
#define N_SWATCHES ((int)(sizeof SWATCHES / sizeof SWATCHES[0]))

static const struct { aos_mp_type_t t; const char *name; } TYPES[] = {
    { AOS_MP_KEY, N_("Atajo") }, { AOS_MP_TEXT, N_("Texto") }, { AOS_MP_SEQ, N_("Secuencia") },
    { AOS_MP_HA, "Home Assistant" }, { AOS_MP_MQTT, "MQTT" }, { AOS_MP_APP, N_("App") },
};

static void e_set(const char *k, const char *v)
{
    if (cJSON_GetObjectItem(U.E, k)) cJSON_ReplaceItemInObject(U.E, k, cJSON_CreateString(v));
    else cJSON_AddStringToObject(U.E, k, v);
}

static void e_preview(void)
{
    uint32_t c = aos_macropad_color(U.E);
    lv_obj_set_style_bg_color(U.e_prev, lv_color_hex(c), 0);
    lv_label_set_text(U.e_prev_glyph, aos_macropad_glyph(aos_macropad_str(U.E, "glyph")));
    lv_obj_set_style_text_color(U.e_prev_glyph, ink_for(c), 0);
    const char *l = aos_macropad_str(U.E, "label");
    lv_label_set_text(U.e_prev_name, *l ? l : _("(sin nombre)"));
    if (U.e_name) lv_label_set_text(U.e_name, l);
    lv_obj_set_style_text_color(U.e_prev_name, ink_for(c), 0);
}

/* rows of the parameters' card: a label and the value, tap to change */
static lv_obj_t *e_row(lv_obj_t *parent, const char *label, const char *value, const char *empty,
                       lv_event_cb_t cb, void *ud, bool mono)
{
    int32_t w = U.W - 2 * AOS_UI_PAD - 40;
    lv_obj_t *r = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, 14, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, 4, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 14, 0);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    aos_label(r, label, aos_font_caption, AOS_C_DIM);
    lv_obj_t *v = wrap_label(r, value && *value ? value : empty, mono ? &aos_mono_22 : aos_font_body,
                             value && *value ? AOS_C_TEXT : AOS_C_DIM, w);
    if (mono) lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++) aos_make_decorative(lv_obj_get_child(r, (int32_t)i));
    return r;
}

static const char *s_edit_key;      /* which field the text entry fills */

static void e_text_done(const char *v)
{
    if (!U.E || !s_edit_key) return;
    e_set(s_edit_key, v);
    e_preview();
    params_rebuild();
}

static void e_label_done(const char *v)
{
    if (!U.E) return;
    e_set("label", v);
    e_preview();
}

static void e_label_cb(lv_event_t *e)
{
    text_entry(_("Nombre del botón"), _("Copiar"), aos_macropad_str(U.E, "label"), 30, false, NULL, 0, e_label_done);
}

static const char *const KEY_SUG[] = {
    "cmd+c", "cmd+v", "cmd+x", "cmd+z", "cmd+shift+z", "cmd+s", "cmd+space", "cmd+tab",
    "cmd+shift+4", "cmd+shift+3", "ctrl+cmd+q", "cmd+w", "cmd+t", "cmd+q", "ctrl+alt+t",
    "ctrl+c", "ctrl+v", "alt+tab", "alt+f4", "win+l", "win+d",
    "play", "next", "prev", "mute", "volup", "voldown", "brightup", "brightdown",
    "f5", "enter", "esc", "tab", "up", "down", "pgup", "pgdn",
};

static void e_field_cb(lv_event_t *e)
{
    s_edit_key = lv_event_get_user_data(e);
    const char *k = s_edit_key, *cur = aos_macropad_str(U.E, k);
    if (!strcmp(k, "key"))
        text_entry(_("Combinación de teclas"), "cmd+shift+4", cur, 48, false, KEY_SUG, (int)(sizeof KEY_SUG / sizeof KEY_SUG[0]), e_text_done);
    else if (!strcmp(k, "text")) text_entry(_("Texto a escribir"), _("Hola"), cur, 250, false, NULL, 0, e_text_done);
    else if (!strcmp(k, "seq")) text_entry(_("Secuencia"), "KEY cmd+space", cur, 1000, true, NULL, 0, e_text_done);
    else if (!strcmp(k, "service")) text_entry(_("Servicio (vacío = tocar)"), "light.toggle", cur, 60, false, NULL, 0, e_text_done);
    else if (!strcmp(k, "data")) text_entry(_("Datos del servicio (JSON)"), "{\"brightness_pct\":40}", cur, 250, false, NULL, 0, e_text_done);
    else if (!strcmp(k, "topic")) text_entry(_("Tópico"), "taller/luz/set", cur, AOS_MQTT_TOPIC_MAX - 1, false, NULL, 0, e_text_done);
    else if (!strcmp(k, "payload")) text_entry(_("Mensaje"), "TOGGLE", cur, 250, false, NULL, 0, e_text_done);
    else if (!strcmp(k, "state")) text_entry(_("Tópico de estado (opcional)"), "stat/enchufe/POWER", cur, AOS_MQTT_TOPIC_MAX - 1, false, NULL, 0, e_text_done);
    else if (!strcmp(k, "entity")) text_entry(_("Entidad"), "light.taller", cur, 63, false, NULL, 0, e_text_done);
}

static void e_service_chip_cb(lv_event_t *e)
{
    e_set("service", lv_event_get_user_data(e));
    params_rebuild();
}

/* the entity picker: a list of HA's entities, the ones a tap does something with first */
static void picker_close(void)
{
    trash(U.picker);
    U.picker = NULL;
}

static void picker_close_cb(lv_event_t *e) { picker_close(); }

static const char *domain_glyph(int d)
{
    switch (d) {
    case HA_LIGHT: return "LIGHTBULB";
    case HA_SWITCH: case HA_INPUT_BOOLEAN: return "TOGGLE_SWITCH";
    case HA_FAN: return "FAN";
    case HA_COVER: return "BLINDS";
    case HA_CLIMATE: return "THERMOMETER";
    case HA_LOCK: return "LOCK";
    case HA_MEDIA: return "PLAY_PAUSE";
    case HA_SCENE: return "AUTO_FIX";
    case HA_SCRIPT: return "SCRIPT_TEXT";
    case HA_BUTTON: return "GESTURE_TAP_BUTTON";
    case HA_SENSOR: return "GAUGE";
    default: return "HOME_ASSISTANT";
    }
}

static void entity_pick_cb(lv_event_t *e)
{
    const char *id = lv_label_get_text(lv_obj_get_child(lv_event_get_current_target(e), 2));
    char name[48] = "";
    int dom = -1;
    aos_ha_lock();
    int i = aos_ha_find(id);
    if (i >= 0) { scpy(name, sizeof name, aos_ha_at(i)->name); dom = aos_ha_at(i)->domain; }
    aos_ha_unlock();
    e_set("entity", id);
    if (!*aos_macropad_str(U.E, "label") && name[0]) e_set("label", name);
    if (dom >= 0) e_set("glyph", domain_glyph(dom));
    picker_close();
    e_preview();
    params_rebuild();
}

static void entity_picker_cb(lv_event_t *e)
{
    if (aos_ha_state() != AOS_HA_READY) {
        s_edit_key = "entity";
        text_entry(_("Entidad"), "light.taller", aos_macropad_str(U.E, "entity"), 63, false, NULL, 0, e_text_done);
        return;
    }
    picker_close();
    U.picker = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.picker, C_SHEET, 0);
    lv_obj_set_style_bg_opa(U.picker, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(U.picker, _("Elegí una entidad"), aos_font_title, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 22);
    lv_obj_t *x = round_btn(U.picker, AOS_SYM_CLOSE, picker_close_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 12);
    lv_obj_t *list = box(U.picker, U.W, U.H - 100);
    lv_obj_set_pos(list, 0, 100);
    lv_obj_set_style_pad_hor(list, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(list, 30, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    static const uint8_t ORDER[] = { HA_SCENE, HA_LIGHT, HA_SWITCH, HA_FAN, HA_COVER, HA_SCRIPT, HA_BUTTON,
                                     HA_INPUT_BOOLEAN, HA_LOCK, HA_MEDIA, HA_CLIMATE, HA_SENSOR, HA_BINARY_SENSOR };
    int shown = 0;
    int32_t w = U.W - 2 * AOS_UI_PAD;
    aos_ha_lock();
    for (size_t o = 0; o < sizeof ORDER && shown < 250; o++) {
        for (int i = 0; i < aos_ha_count() && shown < 250; i++) {
            const aos_ha_entity_t *en = aos_ha_at(i);
            if (!en || en->domain != ORDER[o]) continue;
            lv_obj_t *r = box(list, w, 84);
            lv_obj_set_style_radius(r, 18, 0);
            lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(r, entity_pick_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_align(aos_label(r, aos_macropad_glyph(domain_glyph(en->domain)), &aos_sym_28, lv_color_hex(0x41BDF5)), LV_ALIGN_LEFT_MID, 20, 0);
            lv_obj_t *n = aos_label(r, en->name[0] ? en->name : en->id, aos_font_small, AOS_C_TEXT);
            lv_obj_set_width(n, w - 90);
            lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(n, LV_ALIGN_LEFT_MID, 70, -14);
            lv_obj_t *id = aos_label(r, en->id, aos_font_caption, AOS_C_DIM);
            lv_obj_set_width(id, w - 90);
            lv_label_set_long_mode(id, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(id, LV_ALIGN_LEFT_MID, 70, 18);
            for (int k = 0; k < 3; k++) aos_make_decorative(lv_obj_get_child(r, k));
            shown++;
        }
    }
    aos_ha_unlock();
    if (!shown) wrap_label(list, _("Home Assistant no tiene entidades todavía."), aos_font_small, AOS_C_DIM, w);
}

static void app_pick_cb(lv_event_t *e)
{
    const aos_app_t *a = aos_ui_app_find(lv_event_get_user_data(e));
    if (!a) return;
    e_set("app", a->desc.id);
    if (!*aos_macropad_str(U.E, "label")) e_set("label", aos_tr(a->desc.name));
    e_preview();
    params_rebuild();
}

static void e_toggle_cb(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    if (!strcmp(k, "retain")) {
        bool on = !cJSON_IsTrue(cJSON_GetObjectItem(U.E, "retain"));
        cJSON_DeleteItemFromObject(U.E, "retain");
        cJSON_AddBoolToObject(U.E, "retain", on);
    } else {
        const cJSON *q = cJSON_GetObjectItem(U.E, "qos");
        int v = cJSON_IsNumber(q) && q->valueint == 1 ? 0 : 1;
        cJSON_DeleteItemFromObject(U.E, "qos");
        cJSON_AddNumberToObject(U.E, "qos", v);
    }
    params_rebuild();
}

static void e_params_build(void)
{
    lv_obj_t *p = U.e_params;
    lv_obj_clean(p);
    int32_t w = U.W - 2 * AOS_UI_PAD - 40;
    aos_mp_type_t t = aos_macropad_type(U.E);
    for (int i = 0; i < 6; i++) chip_set(U.e_type_chips[i], TYPES[i].t == t);
    switch (t) {
    case AOS_MP_KEY: {
        e_row(p, _("Combinación de teclas"), aos_macropad_str(U.E, "key"), _("tocá para elegir"), e_field_cb, (void *)"key", true);
        const char *k = aos_macropad_str(U.E, "key");
        bool ok = *k && aos_hal_usb_key_valid(k);
        wrap_label(p, !*k ? _("Modificadores cmd, ctrl, alt, shift unidos con + y una tecla: cmd+shift+4, ctrl+alt+t, f5. Teclas de medios: play, next, prev, mute, volup, voldown.")
                          : ok ? _("Tecla válida.") : _("No conozco esa tecla: revisá el nombre."),
                   aos_font_caption, !*k ? AOS_C_DIM : ok ? AOS_C_GREEN : AOS_C_RED, w);
        break;
    }
    case AOS_MP_TEXT:
        e_row(p, _("Texto"), aos_macropad_str(U.E, "text"), _("tocá para escribirlo"), e_field_cb, (void *)"text", false);
        wrap_label(p, _("Se escribe como un teclado de EE. UU.: los acentos y la ñ no llegan."), aos_font_caption, AOS_C_DIM, w);
        break;
    case AOS_MP_SEQ:
        e_row(p, _("Pasos"), aos_macropad_str(U.E, "seq"), _("tocá para escribirlos"), e_field_cb, (void *)"seq", true);
        wrap_label(p, _("Una orden por línea: STRING texto · KEY cmd+c · DELAY 300 · MOUSE dx dy · SCROLL n · CLICK 1|2 · REPEAT n · HA entidad · MQTT tópico mensaje · OPEN app"),
                   aos_font_caption, AOS_C_DIM, w);
        break;
    case AOS_MP_HA: {
        const char *ent = aos_macropad_str(U.E, "entity");
        e_row(p, _("Entidad"), ent, _("tocá para elegirla"), entity_picker_cb, NULL, false);
        e_row(p, _("Servicio"), aos_macropad_str(U.E, "service"), _("ninguno: lo de siempre (prender/apagar, abrir, ejecutar)"),
              e_field_cb, (void *)"service", true);
        char dom[32] = "";
        const char *dot = strchr(ent, '.');
        if (dot && dot - ent < (long)sizeof dom) { memcpy(dom, ent, (size_t)(dot - ent)); dom[dot - ent] = 0; }
        if (dom[0]) {
            lv_obj_t *r = box(p, w, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_gap(r, 10, 0);
            static char svc[4][48];
            static const char *const SV[] = { "toggle", "turn_on", "turn_off" };
            const char *cur = aos_macropad_str(U.E, "service");
            chip(r, _("Tocar"), !*cur, e_service_chip_cb, (void *)"");
            for (int i = 0; i < 3; i++) {
                snprintf(svc[i], sizeof svc[i], "%s.%s", dom, SV[i]);
                chip(r, svc[i], !strcmp(cur, svc[i]), e_service_chip_cb, svc[i]);
            }
        }
        e_row(p, _("Datos (JSON, opcional)"), aos_macropad_str(U.E, "data"), "{}", e_field_cb, (void *)"data", true);
        if (aos_ha_state() != AOS_HA_READY)
            wrap_label(p, _("Home Assistant no está conectado: la entidad se escribe a mano."), aos_font_caption, AOS_C_ORANGE, w);
        break;
    }
    case AOS_MP_MQTT: {
        e_row(p, _("Tópico"), aos_macropad_str(U.E, "topic"), _("tocá para escribirlo"), e_field_cb, (void *)"topic", true);
        e_row(p, _("Mensaje"), aos_macropad_str(U.E, "payload"), _("(vacío)"), e_field_cb, (void *)"payload", true);
        e_row(p, _("Tópico de estado"), aos_macropad_str(U.E, "state"), _("ninguno: el botón no muestra estado"), e_field_cb, (void *)"state", true);
        lv_obj_t *r = box(p, w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(r, 10, 0);
        chip(r, _("Retenido"), cJSON_IsTrue(cJSON_GetObjectItem(U.E, "retain")), e_toggle_cb, (void *)"retain");
        const cJSON *q = cJSON_GetObjectItem(U.E, "qos");
        chip(r, "QoS 1", cJSON_IsNumber(q) && q->valueint == 1, e_toggle_cb, (void *)"qos");
        break;
    }
    case AOS_MP_APP: {
        const char *cur = aos_macropad_str(U.E, "app");
        lv_obj_t *r = box(p, w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(r, 10, 0);
        for (int i = 0; i < aos_ui_app_count(); i++) {
            const aos_app_t *a = aos_ui_app_at(i);
            if (!a || !a->desc.id || !strcmp(a->desc.id, "aos.macropad")) continue;
            chip(r, aos_tr(a->desc.name), !strcmp(cur, a->desc.id), app_pick_cb, (void *)a->desc.id);
        }
        break;
    }
    default: break;
    }
}

static void e_type_cb(lv_event_t *e)
{
    aos_mp_type_t t = (aos_mp_type_t)(intptr_t)lv_event_get_user_data(e);
    e_set("type", aos_macropad_type_name(t));
    params_rebuild();
}

static void e_glyph_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    e_set("glyph", aos_macropad_glyph_name(i));
    for (uint32_t k = 0; k < lv_obj_get_child_count(U.e_glyphs); k++)
        lv_obj_set_style_bg_color(lv_obj_get_child(U.e_glyphs, (int32_t)k), (int)k == i ? C_MP : AOS_C_CARD, 0);
    e_preview();
}

static void e_color_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char c[8];
    snprintf(c, sizeof c, "#%06X", (unsigned)SWATCHES[i]);
    e_set("color", c);
    for (int k = 0; k < N_SWATCHES; k++)
        lv_obj_set_style_border_width(lv_obj_get_child(U.e_colors, k), k == i ? 5 : 0, 0);
    e_preview();
}

static void e_save_cb(lv_event_t *e)
{
    const char *why = aos_macropad_check(U.E);
    if (why) { aos_ui_toast(why, 1800); return; }
    aos_macropad_lock();
    aos_macropad_set_button(U.e_page, U.e_slot, U.E);
    U.E = NULL;
    aos_macropad_changed();
    aos_macropad_unlock();
    U.dirty = true;
    sheet_close();
    subscribe_states();
}

static void e_try_cb(lv_event_t *e)
{
    aos_macropad_run_button(U.E);
    aos_mp_status_t st;
    aos_macropad_status(&st);
    aos_ui_toast(st.msg[0] ? st.msg : _("Probando…"), 1600);
}

static void e_delete_cb(lv_event_t *e)
{
    if (U.confirm_delete != 99) {
        U.confirm_delete = 99;
        lv_label_set_text(lv_obj_get_child(lv_event_get_current_target(e), 0), _("Tocá otra vez para borrarlo"));
        return;
    }
    aos_macropad_lock();
    aos_macropad_set_button(U.e_page, U.e_slot, NULL);
    aos_macropad_changed();
    aos_macropad_unlock();
    U.dirty = true;
    sheet_close();
}

static void editor_open(int page, int slot)
{
    sheet_close();
    U.confirm_delete = 0;
    aos_macropad_lock();
    const cJSON *b = aos_macropad_button(page, slot);
    U.E = b ? cJSON_Duplicate(b, true) : NULL;
    aos_macropad_unlock();
    U.e_new = !U.E;
    if (!U.E) {
        U.E = cJSON_CreateObject();
        cJSON_AddStringToObject(U.E, "label", "");
        cJSON_AddStringToObject(U.E, "glyph", "KEYBOARD");
        cJSON_AddStringToObject(U.E, "color", "#0A84FF");
        cJSON_AddStringToObject(U.E, "type", "key");
        cJSON_AddStringToObject(U.E, "key", "");
    }
    U.e_page = page;
    U.e_slot = slot;
    lv_obj_t *col = sheet_new(U.e_new ? _("Botón nuevo") : _("Editar botón"));
    lv_obj_t *head = lv_obj_get_child(col, 0);
    lv_obj_t *save = chip(head, _("Guardar"), false, e_save_cb, NULL);
    lv_obj_set_style_bg_color(save, AOS_C_ACCENT, 0);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *cancel = chip(head, _("Cancelar"), false, sheet_close_cb, NULL);
    lv_obj_align_to(cancel, save, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    int32_t w = U.W - 2 * AOS_UI_PAD;

    /* the preview and the name */
    lv_obj_t *top = box(col, w, 190);
    U.e_prev = box(top, 200, 180);
    lv_obj_set_style_radius(U.e_prev, AOS_UI_RADIUS, 0);
    lv_obj_set_style_bg_opa(U.e_prev, LV_OPA_COVER, 0);
    U.e_prev_glyph = aos_label(U.e_prev, "", &aos_sym_72, AOS_C_TEXT);
    lv_obj_align(U.e_prev_glyph, LV_ALIGN_CENTER, 0, -18);
    U.e_prev_name = aos_label(U.e_prev, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(U.e_prev_name, 176);
    lv_obj_set_style_text_align(U.e_prev_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(U.e_prev_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.e_prev_name, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_t *side = box(top, w - 224, 180);
    lv_obj_align(side, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 12, 0);
    lv_obj_t *nm = card(side, w - 224, 100);
    lv_obj_add_flag(nm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(nm, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(nm, e_label_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(aos_label(nm, _("Nombre"), aos_font_caption, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 20, 14);
    lv_obj_t *nv = U.e_name = aos_label(nm, aos_macropad_str(U.E, "label"), aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(nv, w - 270);
    lv_label_set_long_mode(nv, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(nv, LV_ALIGN_BOTTOM_LEFT, 20, -14);
    aos_make_decorative(lv_obj_get_child(nm, 0));
    aos_make_decorative(nv);
    lv_obj_t *tr = box(side, w - 224, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tr, 10, 0);
    chip(tr, _("Probar"), false, e_try_cb, NULL);
    if (!U.e_new) {
        lv_obj_t *d = chip(tr, _("Borrar"), false, e_delete_cb, NULL);
        lv_obj_set_style_bg_color(d, AOS_C_RED, 0);
    }

    section(col, _("ACCIÓN"));
    lv_obj_t *types = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(types, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(types, 10, 0);
    for (int i = 0; i < 6; i++)
        U.e_type_chips[i] = chip(types, aos_tr(TYPES[i].name), false, e_type_cb, (void *)(intptr_t)TYPES[i].t);
    U.e_params = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(U.e_params, 20, 0);
    lv_obj_set_flex_flow(U.e_params, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.e_params, 8, 0);

    section(col, _("ÍCONO"));
    U.e_glyphs = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.e_glyphs, LV_FLEX_FLOW_ROW_WRAP);
    int per = U.land ? 13 : 7;
    int32_t cell = (w - (per - 1) * 10) / per;
    lv_obj_set_style_pad_gap(U.e_glyphs, 10, 0);
    const char *g = aos_macropad_str(U.E, "glyph");
    for (int i = 0; i < aos_macropad_glyph_count(); i++) {
        lv_obj_t *c = box(U.e_glyphs, cell, cell);
        lv_obj_set_style_radius(c, 18, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, !strcmp(g, aos_macropad_glyph_name(i)) ? C_MP : AOS_C_CARD, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, e_glyph_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = aos_label(c, aos_macropad_glyph(aos_macropad_glyph_name(i)), &aos_sym_44, AOS_C_TEXT);
        lv_obj_center(l);
        aos_make_decorative(l);
    }

    section(col, _("COLOR"));
    U.e_colors = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.e_colors, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(U.e_colors, 14, 0);
    uint32_t cur = aos_macropad_color(U.E);
    for (int i = 0; i < N_SWATCHES; i++) {
        lv_obj_t *s = box(U.e_colors, 70, 70);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(SWATCHES[i]), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s, lv_color_white(), 0);
        lv_obj_set_style_border_width(s, SWATCHES[i] == cur ? 5 : 0, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s, e_color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    e_preview();
    e_params_build();
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static void subscribe_states(void)
{
    /* the "state" topics may fall outside the base subscription */
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    if (!c.enabled || !c.host[0]) return;
    aos_mqtt_start();
    if (!strcmp(c.sub, "#")) return;
    int n = 0;
    aos_macropad_lock();
    for (int p = 0; p < aos_macropad_page_count() && n < 6; p++)
        for (int s = 0; s < AOS_MP_SLOTS && n < 6; s++) {
            const cJSON *b = aos_macropad_button(p, s);
            if (b && aos_macropad_type(b) == AOS_MP_MQTT && *aos_macropad_str(b, "state")) {
                aos_mqtt_subscribe(aos_macropad_str(b, "state"), 0);
                n++;
            }
        }
    aos_macropad_unlock();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    U.tick++;
    usb_refresh();
    if (aos_macropad_version() != U.doc_ver) {
        if (U.overlay || U.sheet) U.dirty = true;       /* the portal changed it: after the sheet */
        else rebuild();
        U.doc_ver = aos_macropad_version();
    }
    aos_mp_status_t st;
    aos_macropad_status(&st);
    if (st.seq != U.st_seq) {
        if (S.face == FACE_PAGE && st.page == S.page) flash(st.slot, st.ok);
        foot_refresh();
    }
    uint32_t hv = aos_ha_version(), mv = aos_mqtt_version();
    if (hv != U.ha_ver || mv != U.mq_ver) {
        U.ha_ver = hv;
        U.mq_ver = mv;
        tiles_refresh();
    }
    if (U.tick % 8 == 0 && aos_macropad_autofill() && !U.overlay && !U.sheet) rebuild();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    aos_macropad_load();
    if (aos_ha_configured()) aos_ha_start();
    subscribe_states();
    /* the port becomes the keyboard as soon as the pad is open: on the P4 it
     * costs nothing, the console is on the UART. Not when it is a disk: the
     * computer has the card, and taking it back is for Ajustes to do */
    if (aos_hal_usb_mode() == AOS_HAL_USB_CONSOLE) aos_hal_usb_mode_set(AOS_HAL_USB_KEYS);
    memset(&U, 0, sizeof U);
    memset(&L, 0, sizeof L);
    memset(&P, 0, sizeof P);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    build();
    U.timer = lv_timer_create(timer_cb, 250, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.drag_lock) aos_hal_usb_mouse_hold(0);
    play_release(false);
    if (P.timer) lv_timer_delete(P.timer);
    memset(&P, 0, sizeof P);
    aos_macropad_stop();
    if (U.timer) lv_timer_delete(U.timer);
    if (U.E) cJSON_Delete(U.E);
    if (L.pending) lv_async_call_cancel(later_cb, NULL);
    memset(&L, 0, sizeof L);        /* the trash goes with the root */
    memset(&U, 0, sizeof U);
}

static void hide(aos_app_t *self, void *inst)
{
    if (U.drag_lock) { aos_hal_usb_mouse_hold(0); U.drag_lock = false; if (U.drag_btn) chip_set(U.drag_btn, false); }
    play_release(true);
    if (P.timer) lv_timer_pause(P.timer);
    if (U.timer) lv_timer_pause(U.timer);
}

static void show(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_resume(U.timer);
    if (P.timer) { play_mark_stale(); lv_timer_resume(P.timer); }
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.picker) { picker_close(); return true; }
    if (U.overlay) { overlay_close(); return true; }
    if (U.sheet) { sheet_close(); return true; }
    return false;
}

void aos_app_macropad_get(aos_app_t *app)
{
    aos_macropad_init();        /* the lock, before the portal's threads can ask */
    *app = (aos_app_t){
        .desc = {
            .id = "aos.macropad", .name = "Macro pad", .icon = AOS_SYM_VIEW_GRID_OUTLINE,
            .color_a = 0x22D3EE, .color_b = 0x0E7490,
            .flags = AOS_APP_FLAG_KEEP,
            .order = 170,
        },
        .create = create, .destroy = destroy, .show = show, .hide = hide, .back = back,
    };
}
