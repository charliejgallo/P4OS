/*
 * P4OS - Doom.
 *
 * Chocolate Doom through doomgeneric, running in the app's worker on core 0
 * (port/, doomgeneric/). This file is only the OS's side: the picture,
 * blitted from LVGL's task, and the pad around it.
 *
 * Always in landscape: AOS_APP_FLAG_LANDSCAPE makes the runtime turn the
 * screen while Doom is in front and turn it back when it leaves. On the
 * 1280x720 screen:
 *
 *     x  160..1119, y 60..659   Doom, 320x200 x3 = 960x600, scaled and
 *                               turned by the PPA. Pressing it fires, and so
 *                               do the 60 px strips above and below it.
 *     x    0..159               MENU at the top, then the stick for the left
 *                               thumb, which appears where the thumb lands.
 *     x 1120..1279              MAP, WEAPON and STRAFE at the top, then USE
 *                               and FIRE for the right thumb.
 *
 * Two fingers (aos_touch_points, the GT911 reports both cleanly): each
 * finger owns whatever it landed on until it lifts - the stick, a button,
 * or the picture - so the left thumb walks while the right one shoots. The
 * stick keeps working when its thumb slides onto the picture. The stick and
 * FIRE sit level with each other, where the thumbs rest.
 *
 * Nothing LVGL draws may overlap the picture: LVGL's next redraw over that
 * area wins until the next frame, which would be a flicker. That is why
 * the knob travels less than the finger and the base stays inside its band.
 *
 * The picture goes up by page flipping (present_flip): the PPA scales it
 * into the panel's free buffer and that buffer is flipped to, so a frame is
 * never shown half old, half new. A flip shows a whole buffer, and LVGL
 * only ever draws the pad into the one on screen, so before each flip the
 * pad is brought up to date in the new one from the one shown: only the
 * boxes that changed (the stick's base, a lit button), and the whole bands
 * the first time a buffer is used or after anything of the system's was
 * drawn over the game. Under a panel, the switcher, a banner, the zoom or a
 * gesture the game pauses (no tics, the clock stopped); under a toast it
 * plays on, blitted into the buffer on screen as before, so the toast stays
 * where LVGL drew it.
 *
 * The WAD is not in the app: the card's doom/ folder is searched for the
 * full game first and the shareware DOOM1.WAD last (port/dg_system.c). The
 * config and the saves go in the same folder.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_gesture.h"
#include "lvgl.h"

#include "doom_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS     8
#define PIC_W       (DP_W * DP_SCALE)       /* 960 */
#define PIC_H       (DP_H * DP_SCALE)       /* 600 */

/* The CPU clock the mixer's cycles are divided by, for the log: the rev1.3
 * board runs at 360 MHz (a rev3 chip would say 400). */
#define CPU_MHZ     360

/* The stick, in the left band. The value is where the thumb is against
 * where it landed (landing is always neutral); the drawing is a base kept
 * whole inside the band and a knob that travels less than the thumb, so
 * neither ever reaches the picture. */
#define STICK_TOP       120         /* above: MENU                            */
#define STICK_R         60          /* the thumb this far away: full push     */
#define STICK_BASE      70          /* the base's radius, drawn               */
#define STICK_KNOB      28
#define STICK_TRAVEL    (STICK_BASE - STICK_KNOB - 2)
#define STICK_REST_Y    470         /* level with FIRE                        */
#define EDGE_KEEP       8

#define MIN_PRESS_MS    70          /* two of Doom's tics: a quick tap is seen */
#define MENU_HOLD_MS    2000        /* MENU held this long: out of Doom        */

/* A box of the pad that changed is copied into each buffer that comes up
 * for as long as it keeps changing and this much after: LVGL's last copy
 * into the buffer on screen is asynchronous, and a box read a moment too
 * early is read again on the buffer's next turn. */
#define PAD_SETTLE_MS   60
#define FB_N            3           /* the panel's buffers */

typedef struct {
    int x, y, w, h;                 /* screen box */
    bool round;
} box_t;

typedef struct {
    int x, y, w, h;
} rect_t;

/* One of the panel's buffers, as back() hands them out. */
typedef struct {
    uint16_t   *fb;
    bool        whole;              /* strips black, both bands as LVGL drew them */
    bool        dirty[2];           /* a box of the left / right band to bring over */
    rect_t      d[2];
} fbrec_t;

/* The layout, worked out from the screen's size (layout_compute). */
static struct {
    int   w, h;                     /* the screen                             */
    int   px, py;                   /* the picture's top-left corner          */
    int   band;                     /* each side band's width                 */
    bool  ok;                       /* landscape, and the picture fits        */
    box_t box[DP_BTN_N];
} L;

/* The right band, top to bottom: a finger belongs to the row it lands in,
 * not only to the drawn button, so a thumb that lands a little off still
 * gets what it was going for. */
static const int RIGHT_ROWS[] = {
    DP_BTN_MAP, DP_BTN_WEAPON, DP_BTN_STRAFE, DP_BTN_USE, DP_BTN_FIRE,
};
#define RIGHT_N ((int)(sizeof RIGHT_ROWS / sizeof RIGHT_ROWS[0]))

enum {
    OWN_NONE = -1,
    OWN_STICK = 100,
    OWN_PICTURE = 101,
};

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    lv_obj_t   *canvas;             /* the simulator's way to see a frame */
    uint16_t   *cv;                 /* its 960x600, only if a blit failed */
    lv_obj_t   *touch;
    lv_obj_t   *msg;                /* loading, errors, "turn the screen" */
    lv_obj_t   *btn[DP_BTN_N];
    lv_obj_t   *base, *knob;
    lv_timer_t *timer;

    bool        running;
    bool        sound;
    bool        shown_any;
    bool        want_exit;
    bool        exit_on_tap;
    bool        blocked;            /* the system's edge gestures are off */
    bool        paused_msg;         /* the "turn the screen" message is up */

    /* the stick: where its thumb landed, and where the base is drawn */
    int         cx, cy;
    int         bx, by;

    /* two fingers: what each slot of aos_touch_points() owns */
    uint8_t     fid[2];             /* the finger's id, 0 = none */
    int         fown[2];
    uint32_t    fdown[2];           /* when it landed */
    bool        strafe_on;          /* STRAFE is a latch: a tap on, a tap off */
    bool        menu_tap;           /* a MENU press from outside (back) */
    bool        btn_on[DP_BTN_N];   /* what Doom was last told */
    uint32_t    btn_ms[DP_BTN_N];   /* when it went down */

    int         dbx, dby;           /* where the base was last drawn, 0,0 none */

    /* page flipping */
    fbrec_t     fbr[FB_N];
    uint32_t    pad_ms[2];          /* the last change in each band */
    bool        pad_refr;           /* the pad changed: LVGL draws it before a flip */
    bool        redo;               /* every buffer needs its bands and strips again */
    uint32_t    over;               /* aos_ui_overlay(), read in LVGL's thread only */
    bool        held;               /* paused under the system's UI */

    uint32_t    fps_ms, fps_frames, fps_blits, blits;
    uint32_t    cyc_music, cyc_total;
    uint64_t    blit_us;            /* summed since the last log line */
    uint32_t    blit_n;
    uint32_t    flips, directs, wholes; /* since the last log line */
    uint64_t    pad_us;
    uint32_t    pad_kb;
} app_t;

#ifdef AOS_SIM
/* The engine's globals are initialised once per process: the board gets a
 * fresh copy with every dlopen, the simulator (where the app is built in)
 * does not. A second game in one simulator run would start on the first
 * one's leftovers, so it is refused instead. */
static bool s_sim_ran;
#endif

/* --------------------------------------------------------------------------
 * The layout
 * -------------------------------------------------------------------------- */

static void layout_compute(void)
{
    L.w = AOS_SCREEN_W;
    L.h = AOS_SCREEN_H;
    L.band = (L.w - PIC_W) / 2;
    L.px = L.band;
    L.py = (L.h - PIC_H) / 2;
    /* 140 px is the least a thumb's band can be: the stick's base is 140 */
    L.ok = aos_hal_landscape() && L.band >= 2 * STICK_BASE && L.py >= 0;

    int bw = L.band - 24;               /* the small buttons, 12 px from each side */
    int rx = L.w - L.band;              /* the right band's left edge */
    int rc = rx + L.band / 2;           /* and its middle */
    L.box[DP_BTN_MENU]   = (box_t){ 12,      52,  bw, 56, false };
    L.box[DP_BTN_MAP]    = (box_t){ rx + 12, 52,  bw, 56, false };
    L.box[DP_BTN_WEAPON] = (box_t){ rx + 12, 124, bw, 56, false };
    L.box[DP_BTN_STRAFE] = (box_t){ rx + 12, 196, bw, 56, false };
    L.box[DP_BTN_USE]    = (box_t){ rc - 56, 274, 112, 112, true };
    L.box[DP_BTN_FIRE]   = (box_t){ rc - 68, STICK_REST_Y - 68, 136, 136, true };
}

static int right_row(int y)
{
    for (int i = 0; i < RIGHT_N - 1; i++) {
        const box_t *b = &L.box[RIGHT_ROWS[i]], *n = &L.box[RIGHT_ROWS[i + 1]];
        if (y < (b->y + b->h + n->y) / 2) {
            return RIGHT_ROWS[i];
        }
    }
    return DP_BTN_FIRE;
}

/* --------------------------------------------------------------------------
 * The pad
 * -------------------------------------------------------------------------- */

static lv_obj_t *pad_button(lv_obj_t *root, const char *text)
{
    lv_obj_t *o = lv_obj_create(root);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x3A1010), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0x8A2A1A), 0);
    lv_obj_set_style_border_width(o, 3, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xF0D0B0), 0);
    lv_obj_set_style_text_font(l, aos_font_caption, 0);
    lv_obj_center(l);
    return o;
}

/* A box of the pad (screen pixels) LVGL is about to draw again: every
 * buffer gets it from the one on screen before it is flipped to. */
static void pad_mark(app_t *a, int x, int y, int w, int h)
{
    int band = x + w / 2 < L.w / 2 ? 0 : 1;
    int bx0 = band ? L.w - L.band : 0, bx1 = band ? L.w : L.band;
    x -= 2; y -= 2; w += 4; h += 4;             /* the border's antialiasing */
    if (x < bx0) { w -= bx0 - x; x = bx0; }
    if (x + w > bx1) w = bx1 - x;
    if (y < 0) { h += y; y = 0; }
    if (y + h > L.h) h = L.h - y;
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < FB_N; i++) {
        fbrec_t *r = &a->fbr[i];
        if (!r->dirty[band]) {
            r->d[band] = (rect_t){ x, y, w, h };
            r->dirty[band] = true;
            continue;
        }
        rect_t *d = &r->d[band];
        int x1 = d->x + d->w > x + w ? d->x + d->w : x + w;
        int y1 = d->y + d->h > y + h ? d->y + d->h : y + h;
        d->x = d->x < x ? d->x : x;
        d->y = d->y < y ? d->y : y;
        d->w = x1 - d->x;
        d->h = y1 - d->y;
    }
    a->pad_ms[band] = (uint32_t)aos_hal_uptime_ms();
    a->pad_refr = true;
}

static void pad_lit(app_t *a, int btn, bool on)
{
    if (btn < 0 || btn >= DP_BTN_N || !a->btn[btn]) return;
    lv_obj_set_style_bg_color(a->btn[btn], lv_color_hex(on ? 0xB03018 : 0x3A1010), 0);
    const box_t *b = &L.box[btn];
    pad_mark(a, b->x, b->y, b->w, b->h);
}

static void stick_draw(app_t *a, int kx, int ky)
{
    /* the knob never leaves the base's box: the base, where it was and
     * where it is, is all that changes */
    if (a->dbx || a->dby) pad_mark(a, a->dbx - STICK_BASE, a->dby - STICK_BASE, 2 * STICK_BASE, 2 * STICK_BASE);
    if (a->dbx != a->bx || a->dby != a->by) pad_mark(a, a->bx - STICK_BASE, a->by - STICK_BASE, 2 * STICK_BASE, 2 * STICK_BASE);
    a->dbx = a->bx;
    a->dby = a->by;
    lv_obj_set_pos(a->base, a->bx - STICK_BASE, a->by - STICK_BASE);
    lv_obj_set_pos(a->knob, a->bx + kx - STICK_KNOB, a->by + ky - STICK_KNOB);
}

static void stick_show(app_t *a, bool on)
{
    a->bx = L.band / 2;
    if (on) {
        /* the base goes to the thumb's height, kept whole in the band and
         * clear of MENU; the neutral point is where the thumb landed anyway */
        int by = a->cy;
        int lo = L.box[DP_BTN_MENU].y + L.box[DP_BTN_MENU].h + 4 + STICK_BASE;
        int hi = L.h - EDGE_KEEP - STICK_BASE;
        a->by = by < lo ? lo : by > hi ? hi : by;
    } else {
        a->by = STICK_REST_Y;
    }
    stick_draw(a, 0, 0);
}

static void stick_update(app_t *a, int x, int y)
{
    int dx = x - a->cx, dy = y - a->cy;
    int d2 = dx * dx + dy * dy;
    if (d2 > STICK_R * STICK_R) {
        /* past the rim: full deflection, in the thumb's direction */
        int d = 1;
        while (d * d < d2) d++;
        dx = dx * STICK_R / d;
        dy = dy * STICK_R / d;
    }
    stick_draw(a, dx * STICK_TRAVEL / STICK_R, dy * STICK_TRAVEL / STICK_R);
    dp_stick(dx * 100 / STICK_R, dy * 100 / STICK_R);
}

static void pad_place(app_t *a)
{
    for (int i = 0; i < DP_BTN_N; i++) {
        const box_t *b = &L.box[i];
        if (!a->btn[i]) continue;
        lv_obj_set_pos(a->btn[i], b->x, b->y);
        lv_obj_set_size(a->btn[i], b->w, b->h);
        lv_obj_set_style_radius(a->btn[i], b->round ? LV_RADIUS_CIRCLE : 12, 0);
    }
    stick_show(a, false);
    a->redo = true;                     /* a new layout: every buffer from scratch */
    if (a->canvas) lv_obj_set_pos(a->canvas, L.px, L.py);
    lv_obj_set_width(a->msg, PIC_W - 80);
    lv_obj_set_pos(a->msg, L.px + 40, L.py + 60);
    lv_obj_set_size(a->touch, L.w, L.h);
}

static void pad_visible(app_t *a, bool on)
{
    lv_obj_t *objs[DP_BTN_N + 2];
    int n = 0;
    for (int i = 0; i < DP_BTN_N; i++) objs[n++] = a->btn[i];
    objs[n++] = a->base;
    objs[n++] = a->knob;
    for (int i = 0; i < n; i++) {
        if (on) lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    a->redo = true;
}

static void build_pad(app_t *a)
{
    lv_obj_t *r = a->root;
    a->base = lv_obj_create(r);
    lv_obj_remove_style_all(a->base);
    lv_obj_set_size(a->base, STICK_BASE * 2, STICK_BASE * 2);
    lv_obj_set_style_radius(a->base, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(a->base, lv_color_hex(0x8A2A1A), 0);
    lv_obj_set_style_border_width(a->base, 3, 0);
    lv_obj_set_style_bg_color(a->base, lv_color_hex(0x200808), 0);
    lv_obj_set_style_bg_opa(a->base, LV_OPA_COVER, 0);
    lv_obj_remove_flag(a->base, LV_OBJ_FLAG_CLICKABLE);

    a->knob = lv_obj_create(r);
    lv_obj_remove_style_all(a->knob);
    lv_obj_set_size(a->knob, STICK_KNOB * 2, STICK_KNOB * 2);
    lv_obj_set_style_radius(a->knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(a->knob, lv_color_hex(0x9A3420), 0);
    lv_obj_set_style_bg_opa(a->knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(a->knob, LV_OBJ_FLAG_CLICKABLE);

    a->btn[DP_BTN_FIRE]   = pad_button(r, _("FUEGO"));
    a->btn[DP_BTN_USE]    = pad_button(r, _("USAR"));
    a->btn[DP_BTN_MENU]   = pad_button(r, _("MENÚ"));
    a->btn[DP_BTN_MAP]    = pad_button(r, _("MAPA"));
    a->btn[DP_BTN_WEAPON] = pad_button(r, _("ARMA"));
    a->btn[DP_BTN_STRAFE] = pad_button(r, _("LATERAL"));
}

/* --------------------------------------------------------------------------
 * Two fingers
 *
 * Polled on every frame (8 ms) instead of LVGL's events, which only know
 * one finger. Each finger claims what it landed on; Doom is then told the
 * union: a button is down while any finger holds it. STRAFE is the one
 * exception, a latch that the finger landing on it flips.
 * -------------------------------------------------------------------------- */

static int claim(app_t *a, int x, int y, int other_owner)
{
    if (x >= L.px && x < L.px + PIC_W) {
        return OWN_PICTURE;             /* the strips above and below too */
    }
    if (x >= L.px + PIC_W) {
        int b = right_row(y);
        if (b == DP_BTN_STRAFE) a->strafe_on = !a->strafe_on;
        return b;
    }
    if (y < STICK_TOP) {
        return DP_BTN_MENU;
    }
    if (other_owner == OWN_STICK) {
        return OWN_NONE;                /* one stick, one thumb */
    }
    a->cx = x;
    a->cy = y;
    stick_show(a, true);
    return OWN_STICK;
}

static void pad_apply(app_t *a)
{
    bool want[DP_BTN_N] = { false };
    for (int i = 0; i < 2; i++) {
        if (a->fown[i] == OWN_PICTURE) {
            want[DP_BTN_FIRE] = true;
        } else if (a->fown[i] >= 0 && a->fown[i] < DP_BTN_N) {
            want[a->fown[i]] = true;
        }
    }
    want[DP_BTN_STRAFE] = a->strafe_on;
    if (a->menu_tap) {
        a->menu_tap = false;
        want[DP_BTN_MENU] = true;       /* MIN_PRESS_MS lets it go below */
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int b = 0; b < DP_BTN_N; b++) {
        if (!want[b] && a->btn_on[b] && now - a->btn_ms[b] < MIN_PRESS_MS) {
            want[b] = true;         /* held a moment longer; pad_poll comes back in 8 ms */
        }
        if (want[b] != a->btn_on[b]) {
            a->btn_on[b] = want[b];
            if (want[b]) a->btn_ms[b] = now;
            dp_button(b, want[b]);
            pad_lit(a, b, want[b]);
        }
    }
}

static void pad_release_all(app_t *a)
{
    for (int i = 0; i < 2; i++) {
        if (a->fown[i] == OWN_STICK) {
            dp_stick(0, 0);
            stick_show(a, false);
        }
        a->fown[i] = OWN_NONE;
        a->fid[i] = 0;
    }
    a->strafe_on = false;
    pad_apply(a);
}

static void pad_poll(app_t *a)
{
    aos_touch_point_t pts[2];
    aos_touch_points(pts);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);    /* the runtime may slide the root */
    uint32_t now = (uint32_t)aos_hal_uptime_ms();

    for (int i = 0; i < 2; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        int x = (int)pts[i].x - rc.x1, y = (int)pts[i].y - rc.y1;
        if (id != a->fid[i]) {
            if (a->fown[i] == OWN_STICK) {      /* the finger that had it left */
                dp_stick(0, 0);
                stick_show(a, false);
            }
            a->fown[i] = id ? claim(a, x, y, a->fown[1 - i]) : OWN_NONE;
            a->fid[i] = id;
            a->fdown[i] = now;
        }
        if (id && a->fown[i] == OWN_STICK) {
            stick_update(a, x, y);
        }
        /* the way out that does not depend on the engine answering: MENU
         * held down. Doom's own Quit Game is the usual one */
        if (id && a->fown[i] == DP_BTN_MENU && now - a->fdown[i] >= MENU_HOLD_MS) {
            a->want_exit = true;
        }
    }
    pad_apply(a);
}

/* --------------------------------------------------------------------------
 * Messages over the picture
 * -------------------------------------------------------------------------- */

static void show_msg(app_t *a, const char *text, bool tap_to_exit)
{
    lv_label_set_text(a->msg, text);
    lv_obj_remove_flag(a->msg, LV_OBJ_FLAG_HIDDEN);
    a->exit_on_tap = tap_to_exit;
}

static void no_wad(app_t *a)
{
    char text[400];
    snprintf(text, sizeof text, "%s\n\n%s\n%s\n\n%s",
             _("No hay ningún WAD"),
             _("Copiá doom1.wad (shareware) o doom.wad a la carpeta"),
             dp_data_dir(),
             _("Tocá para salir"));
    show_msg(a, text, true);
}

static void gestures_block(app_t *a, bool on)
{
    /* While playing, the system's edge gestures are off: a thumb pushing
     * the stick from the bottom edge would go home, one near the top would
     * pull a panel down under the blitted picture, which hides it. Out of
     * a game they are back, and MENU held is always a way out. */
    if (a->blocked != on) {
        a->blocked = on;
        aos_ui_block_gestures(on);
    }
}

static void stopped(app_t *a)
{
    a->running = false;
    pad_release_all(a);
    gestures_block(a, false);
    lv_obj_invalidate(a->root);         /* the last frame goes: black behind the text */
}

/* --------------------------------------------------------------------------
 * The frame loop
 * -------------------------------------------------------------------------- */

/* The simulator (or a blit the HAL refused): through an LVGL canvas, the
 * picture scaled x3 on the CPU. Never used on a board that blits: a canvas
 * over the picture would be redrawn by LVGL on top of the blits. */
static void canvas_frame(app_t *a, const uint16_t *f)
{
    if (!a->cv) {
        a->cv = (uint16_t *)malloc((size_t)PIC_W * PIC_H * 2);
        if (!a->cv) return;
        a->canvas = lv_canvas_create(a->root);
        lv_canvas_set_buffer(a->canvas, a->cv, PIC_W, PIC_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(a->canvas, L.px, L.py);
        lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(a->canvas, 0);             /* behind everything */
    }
    uint16_t *out = a->cv;
    for (int y = 0; y < DP_H; y++) {
        const uint16_t *src = f + y * DP_W;
        uint16_t *row = out;
        for (int x = 0; x < DP_W; x++) {
            uint16_t v = src[x];
            *out++ = v;
            *out++ = v;
            *out++ = v;
        }
        memcpy(out, row, PIC_W * 2);
        out += PIC_W;
        memcpy(out, row, PIC_W * 2);
        out += PIC_W;
    }
    lv_obj_invalidate(a->canvas);
}

/* A box of the screen as the panel's rectangle, for the rotation the screen
 * has (the mapping of aos_hal_display_blit_native). Lying down a column of
 * the screen is a row of the panel, so each side band is whole rows. */
static rect_t to_panel(rect_t r, int rot)
{
    switch (rot) {
    case 90:  return (rect_t){ AOS_PANEL_W - (r.y + r.h), r.x, r.h, r.w };
    case 180: return (rect_t){ AOS_PANEL_W - (r.x + r.w), AOS_PANEL_H - (r.y + r.h), r.w, r.h };
    case 270: return (rect_t){ r.y, AOS_PANEL_H - (r.x + r.w), r.h, r.w };
    default:  return r;
    }
}

static uint32_t copy_box(uint16_t *dst, const uint16_t *src, rect_t s, int rot)
{
    rect_t p = to_panel(s, rot);
    if (p.w <= 0 || p.h <= 0) return 0;
    for (int y = 0; y < p.h; y++) {
        size_t o = (size_t)(p.y + y) * AOS_PANEL_W + p.x;
        memcpy(dst + o, src + o, (size_t)p.w * 2);
    }
    return (uint32_t)p.w * p.h * 2;
}

static void clear_box(uint16_t *dst, rect_t s, int rot)
{
    rect_t p = to_panel(s, rot);
    if (p.w <= 0 || p.h <= 0) return;
    for (int y = 0; y < p.h; y++) memset(dst + (size_t)(p.y + y) * AOS_PANEL_W + p.x, 0, (size_t)p.w * 2);
}

static fbrec_t *fb_rec(app_t *a, uint16_t *fb)
{
    for (int i = 0; i < FB_N; i++)
        if (a->fbr[i].fb == fb) return &a->fbr[i];
    for (int i = 0; i < FB_N; i++) {
        if (!a->fbr[i].fb) {
            a->fbr[i].fb = fb;
            a->fbr[i].whole = false;
            return &a->fbr[i];
        }
    }
    return NULL;
}

/* The frame into the panel's free buffer, the pad brought up to date in it,
 * and the buffer flipped to. false: no free buffer here (the simulator, or
 * pref "fbs" = 1), and the caller blits the old way. From LVGL's thread,
 * with nothing of the system's over the game. */
static bool present_flip(app_t *a, const uint16_t *f)
{
    uint16_t *fb = aos_hal_display_back();
    fbrec_t *r = fb ? fb_rec(a, fb) : NULL;
    uint32_t age = 0;
    if (!r || !aos_hal_display_back_age(fb, &age, NULL)) return false;
    if (a->redo) {
        a->redo = false;
        for (int i = 0; i < FB_N; i++) a->fbr[i].whole = false;
    }
    if (age == UINT32_MAX) r->whole = false;    /* never one of our frames */
    /* What LVGL still owes the buffer on screen (a knob that moved, the
     * place a panel was) is drawn there now, so it is there to be copied. */
    if (a->pad_refr || !r->whole) {
        a->pad_refr = false;
        lv_refr_now(NULL);
    }
    int rot = aos_hal_display_get_rotation();
    /* The PPA first: before it runs it drops the cache over the rows it
     * writes, which would lose whatever the CPU had written there and not
     * yet written back (the strips share those rows). */
    if (!aos_hal_display_blit_into_fit(fb, L.px, L.py, PIC_W, PIC_H, f, DP_W, DP_H, DP_W, false)) return false;

    uint64_t t0 = aos_hal_uptime_us();
    const uint16_t *front = NULL;
    uint32_t bytes = 0;
    if (!r->whole || r->dirty[0] || r->dirty[1]) {
        /* the one on screen, with the cache over it dropped: LVGL's DMA2D
         * wrote into it behind the CPU's back */
        int fw, fh;
        if (!aos_hal_display_fb(&front, &fw, &fh)) front = NULL;
    }
    if (!r->whole && front) {
        clear_box(fb, (rect_t){ L.px, 0, PIC_W, L.py }, rot);
        clear_box(fb, (rect_t){ L.px, L.py + PIC_H, PIC_W, L.h - L.py - PIC_H }, rot);
        bytes += copy_box(fb, front, (rect_t){ 0, 0, L.band, L.h }, rot);
        bytes += copy_box(fb, front, (rect_t){ L.w - L.band, 0, L.band, L.h }, rot);
        r->whole = true;
        r->dirty[0] = r->dirty[1] = false;
        a->wholes++;
    } else if (front) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        for (int b = 0; b < 2; b++) {
            if (!r->dirty[b]) continue;
            bytes += copy_box(fb, front, r->d[b], rot);
            if (now - a->pad_ms[b] >= PAD_SETTLE_MS) r->dirty[b] = false;
        }
    }
    a->pad_us += aos_hal_uptime_us() - t0;
    a->pad_kb += bytes / 1024;
    if (!aos_hal_display_flip(fb)) {
        r->whole = false;
        return false;
    }
    a->flips++;
    return true;
}

static void push_frame(app_t *a)
{
    const uint16_t *f = dp_frame_take();
    if (!f) return;
    if (!a->shown_any) {
        /* the "loading" label goes before the first blit, drawn away now:
         * LVGL's own redraw of that area would land on top of the frame */
        a->shown_any = true;
        lv_obj_add_flag(a->msg, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        a->redo = true;
    }
    uint64_t t0 = aos_hal_uptime_us();
    /* Under a toast the frame goes into the buffer on screen, where LVGL
     * drew the toast: a flip would hide it. The other buffers are made
     * whole again afterwards, the toast's place included. */
    bool flip = !(a->over & AOS_UI_OVER_TOAST) && present_flip(a, f);
    if (flip || aos_hal_display_blit_scaled(L.px, L.py, DP_W, DP_H, f, DP_SCALE, false)) {
        a->blit_us += aos_hal_uptime_us() - t0;
        a->blit_n++;
        if (!flip) {
            a->directs++;
            a->redo = true;
        }
    } else {
        canvas_frame(a, f);
    }
    dp_frame_blitted();
    a->blits++;
}

static void log_rates(app_t *a, uint32_t now)
{
    if (!a->fps_ms) {
        a->fps_ms = now;
        a->fps_frames = dp_frames();
        a->fps_blits = a->blits;
        return;
    }
    if (now - a->fps_ms < 5000) return;
    uint32_t dt = now - a->fps_ms;
    uint32_t fr = dp_frames() - a->fps_frames, bl = a->blits - a->fps_blits;
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    /* the mixer's share of core 0: its cycles over the clock */
    uint32_t cm, ct;
    dp_audio_cycles(&cm, &ct);
    unsigned pm = (unsigned)((uint64_t)(cm - a->cyc_music) * 1000 / (CPU_MHZ * 1000) / dt);
    unsigned pt = (unsigned)((uint64_t)(ct - a->cyc_total) * 1000 / (CPU_MHZ * 1000) / dt);
    unsigned bu = a->blit_n ? (unsigned)(a->blit_us / a->blit_n) : 0;
    unsigned pu = a->flips ? (unsigned)(a->pad_us / a->flips) : 0;
    unsigned pk = a->flips ? (unsigned)(a->pad_kb / a->flips) : 0;
    a->cyc_music = cm;
    a->cyc_total = ct;
    /* blit: the whole present (PPA + pad + flip); pad: bringing the pad
     * into the new buffer, per flip, and its KB; whole: buffers redone */
    aos_hal_log("doom", "%s%s: %u.%u fps rendered, %u.%u shown, blit %u us (%u flips, %u direct), "
                "pad %u us %u KB, whole %u | audio %u.%u%% of core 0 (music %u.%u%%) | internal %u B, psram %u B",
                dp_where(), a->held ? " PAUSED" : "",
                (unsigned)(fr * 10000 / dt / 10), (unsigned)(fr * 10000 / dt % 10),
                (unsigned)(bl * 10000 / dt / 10), (unsigned)(bl * 10000 / dt % 10),
                bu, (unsigned)a->flips, (unsigned)a->directs, pu, pk, (unsigned)a->wholes,
                pt / 10, pt % 10, pm / 10, pm % 10,
                (unsigned)fi, (unsigned)fp);
    a->flips = a->directs = a->wholes = 0;
    a->pad_us = 0;
    a->pad_kb = 0;
    a->fps_ms = now;
    a->fps_frames = dp_frames();
    a->fps_blits = a->blits;
    a->blit_us = 0;
    a->blit_n = 0;
}

static void frame(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->want_exit) {
        a->want_exit = false;
        aos_ui_home();                  /* destroy() stops the engine */
        return;
    }
    if (!a->running) return;

    dp_state_t st = dp_state();
    if (st == DP_QUIT) {
        stopped(a);
        aos_ui_home();
        return;
    }
    if (st == DP_ERROR || st == DP_STOPPED) {
        stopped(a);
        char text[300];
        snprintf(text, sizeof text, "%s\n\n%s\n\n%s", _("Doom se detuvo"), dp_error(),
                 _("Tocá para salir"));
        show_msg(a, text, true);
        return;
    }
    if (a->paused_msg) {
        return;                         /* not in landscape: nothing to blit to */
    }
    /* A panel, the switcher, a banner, the zoom or a gesture over the game:
     * Doom pauses (dp_pause: no tics, its clock stopped) and nothing is
     * presented over them. Read here, in LVGL's thread; the engine only
     * sees the flag. A toast is not a reason: the game plays on under it. */
    a->over = aos_ui_overlay();
    const char *cur = aos_ui_current_app();
    if (!cur || strcmp(cur, a->self->desc.id) != 0) a->over |= AOS_UI_OVER_NOT_FRONT;
    bool hold = (a->over & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    if (hold != a->held) {
        a->held = hold;
        dp_pause(hold);
        if (hold) pad_release_all(a);   /* the fingers are the system's now */
        a->redo = true;                 /* LVGL draws over the game meanwhile */
        aos_hal_log("doom", hold ? "paused: 0x%x over the game" : "playing again (0x%x)",
                    (unsigned)a->over);
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!hold) {
        pad_poll(a);
        push_frame(a);
    }
    log_rates(a, now);
}

static void touch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    /* the pad is polled (pad_poll); this only takes the tap that leaves */
    if (a->exit_on_tap) a->want_exit = true;
}

/* Portrait: not a place for Doom. The runtime turns the screen for it, so
 * this is only for a screen that turned anyway; the engine keeps going and
 * the picture comes back when the screen does. */
static void orientation_check(app_t *a)
{
    bool pause = !L.ok;
    if (pause == a->paused_msg) return;
    a->paused_msg = pause;
    pad_visible(a, !pause);
    if (pause) {
        if (a->running) pad_release_all(a);
        gestures_block(a, false);
        char text[200];
        snprintf(text, sizeof text, "%s\n\n%s", _("Doom se juega con la pantalla horizontal"),
                 _("Girá la pantalla para seguir"));
        show_msg(a, text, !a->running);
    } else {
        if (a->running) gestures_block(a, true);
        if (a->shown_any || !a->running) lv_obj_add_flag(a->msg, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(a->root);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static bool doom_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    /* the edge swipe is off (NO_SWIPE); a back from elsewhere (the portal,
     * the simulator's key) is Doom's MENU while playing */
    if (!a || !a->running) return false;
    a->menu_tap = true;
    return true;
}

static bool doom_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)root;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    layout_compute();
    pad_place(a);
    orientation_check(a);
    return true;
}

static void *doom_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->fown[0] = a->fown[1] = OWN_NONE;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("doom", "opening | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    layout_compute();
    build_pad(a);

    a->msg = lv_label_create(root);
    lv_label_set_long_mode(a->msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(a->msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(a->msg, lv_color_hex(0xF0D0B0), 0);
    lv_obj_set_style_text_font(a->msg, aos_font_body, 0);
    lv_obj_remove_flag(a->msg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->msg, LV_OBJ_FLAG_HIDDEN);

    a->touch = lv_obj_create(root);
    lv_obj_remove_style_all(a->touch);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSED, a);

    pad_place(a);
    orientation_check(a);

    a->timer = lv_timer_create(frame, TICK_MS, a);

    char wad[128];
#ifdef AOS_SIM
    if (s_sim_ran) {
        show_msg(a, _("Doom ya corrió en este simulador: reinicialo para jugar otra vez"), true);
        return a;
    }
#endif
    if (!dp_find_wad(wad, sizeof wad)) {
        aos_hal_log("doom", "no WAD in %s", dp_data_dir());
        no_wad(a);
        return a;
    }
    aos_hal_log("doom", "WAD %s", wad);

    a->sound = aos_hal_spk_open(16000);
    dp_sound_enable(a->sound);
    if (!a->sound) aos_hal_log("doom", "no speaker: playing silent");

    if (!a->paused_msg) show_msg(a, _("Cargando..."), false);
#ifdef AOS_SIM
    s_sim_ran = true;
#endif
    if (!dp_start(wad)) {
        char text[300];
        snprintf(text, sizeof text, "%s\n\n%s\n\n%s", _("Doom no pudo arrancar"),
                 dp_error()[0] ? dp_error() : _("Falta memoria"), _("Tocá para salir"));
        show_msg(a, text, true);
        return a;
    }
    a->running = true;
    if (!a->paused_msg) gestures_block(a, true);

    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("doom", "started | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);
    return a;
}

static void doom_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->timer = NULL;
    gestures_block(a, false);
    dp_stop();                          /* waits for the engine, frees its frames */
    if (a->sound) aos_hal_spk_close();
    dp_sound_enable(false);
    if (a->root) lv_obj_clean(a->root);
    free(a->cv);
    lv_free(a);
}

/* The launcher icon: a cacodemon, red, one green eye, a mouthful of teeth. */
static const uint8_t DOOM_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -20, -24, 10, 18, 4,          AIC_C_LIT(0xE8DCC0), 255),
    AIC_ROT(-250),
    AIC_RECT(AIC_CENTER,  20, -24, 10, 18, 4,          AIC_C_LIT(0xE8DCC0), 255),
    AIC_ROT(250),
    AIC_RECT(AIC_CENTER,   0,   4, 62, 58, AIC_CIRCLE, AIC_C_LIT(0xC4221A), 255),
    AIC_GRAD(AIC_C_LIT(0x6A0C08), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER,   0,  -6, 24, 20, AIC_CIRCLE, AIC_C_LIT(0xF4F0D8), 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER,   0,   0, 11, 13, AIC_CIRCLE, AIC_C_LIT(0x28B040), 255),
    AIC_OUT,
    AIC_RECT(AIC_CENTER,   0,  20, 34, 11, 5,          AIC_C_LIT(0x2A0404), 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID,  -9,   0,  5,  5, 1,          AIC_C_LIT(0xF4F0D8), 255),
    AIC_RECT(AIC_TOP_MID,   0,   0,  5,  5, 1,          AIC_C_LIT(0xF4F0D8), 255),
    AIC_RECT(AIC_TOP_MID,   9,   0,  5,  5, 1,          AIC_C_LIT(0xF4F0D8), 255),
    AIC_OUT,
    AIC_END
};

static bool doom_init(aos_app_t *app)
{
    app->desc.id       = "demo.doom";
    app->desc.name     = "Doom";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, DOOM_ICON, sizeof DOOM_ICON);
    app->desc.color_a  = 0x5A1208;
    app->desc.color_b  = 0x140404;
    app->desc.order    = 162;
    /* LANDSCAPE alone: the runtime turns the screen for Doom and back.
     * KEEP_AWAKE keeps the screen on for as long as Doom is in front, the
     * demos included, with nobody touching the glass. */
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG |
                         AOS_APP_FLAG_LANDSCAPE;

    app->create  = doom_create;
    app->destroy = doom_destroy;
    app->back    = doom_back;
    app->resize  = doom_resize;
    return true;
}

AOS_APP_ENTRY(doom_init);
