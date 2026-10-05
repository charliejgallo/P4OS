/*
 * P4OS - Desktop simulator (SDL2).
 *
 *     cd sim && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
 *     ./build/p4os_sim
 *
 * A window the size of the 720x1280 panel, scaled to fit a desktop screen
 * (P4_SIM_ZOOM, default 0.6). The mouse is the finger. It runs from sim/, so
 * the fake SD card is sim/sim_fs.
 *
 * Keys
 *     r        portrait <-> landscape (what the Control Centre button does)
 *     h / b    home / back
 *     c / n    open the Control Centre / the notifications
 *     x        a fake notification arrives
 *     e        edit the home screen
 *     s        screenshot to sim_shot.png
 *     q        quit
 *
 * A USB gamepad, faked (P4_SIM_PAD=1, what aos_hal_hid_gamepad_get reads):
 *     arrows   the d-pad          z / c    buttons 1 / 2 (A)
 *     x / v    buttons 3 / 4 (B)  a / d    buttons 5 / 6 (L / R)
 *     Return   button 10 (Start)  Shift    button 9 (Select)
 * With the pad on, those letters are the pad's and not the commands above.
 *
 * Scripts, for looking at the UI without anybody at the mouse:
 *
 *     P4_SIM_SCRIPT="wait 800; shot home.png; tap 360 700; wait 600; shot app.png; quit"
 *
 * Commands, separated by ';':  wait <ms> | tap <x> <y> [ms] |
 * drag <x1> <y1> <x2> <y2> [ms] [rest_ms] | pinch <cx> <cy> <from> <to> [ms] |
 * key <k> | type <text> | enter | open <app id> | rotate | shot <file.png> |
 * pad <buttons> <x> <y> [ms] [hat] | quit
 * (pad: a gamepad plugged in, holding <buttons> - hex, bit 0 = button 1 -
 * and the left stick at <x> <y> in -32767..32767 for ms, 150 by default,
 * and the hat (the d-pad: 0 up, 2 right, 4 down, 6 left; -1 none); then
 * it lets go and stays plugged in. "pad off" unplugs it)
 * (type and enter go to the text field of the on-screen keyboard showing)
 * Coordinates are logical (the ones the UI sees in the current orientation).
 */
#include "lvgl.h"
#include <SDL2/SDL.h>
#include "aos_ui.h"
#include "aos_apps.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_portal.h"
#include "lvgl/src/libs/lodepng/lodepng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void aos_hal_sim_set_display(lv_display_t *disp);
void aos_hal_sim_idle_tick(void);
void aos_hal_sim_notificacion(void);
void aos_hal_sim_link_tick(void);
void aos_hal_sim_pad(bool connected, uint32_t buttons, int16_t ax, int16_t ay, int8_t hat);

/* --------------------------------------------------------------------------
 * Apps from apps/ compiled in (AOS_SIM_BUILTIN), as on the watch's simulator
 * -------------------------------------------------------------------------- */
typedef struct {
    bool     (*init)(aos_app_t *);
    uint32_t (*count)(void);
    bool     (*init_at)(aos_app_t *, uint32_t);
} sim_reg_t;
static sim_reg_t s_regs[AOS_MAX_APPS];
static int s_nregs;

void aos_sim_register_app(bool (*init)(aos_app_t *app))
{
    if (s_nregs < AOS_MAX_APPS) s_regs[s_nregs++] = (sim_reg_t){ .init = init };
}

void aos_sim_register_app_many(uint32_t (*count)(void), bool (*init_at)(aos_app_t *app, uint32_t index))
{
    if (s_nregs < AOS_MAX_APPS) s_regs[s_nregs++] = (sim_reg_t){ .count = count, .init_at = init_at };
}

static void register_sim_apps(void)
{
    for (int i = 0; i < s_nregs; i++) {
        aos_app_t app;
        if (s_regs[i].init) {
            memset(&app, 0, sizeof app);
            if (s_regs[i].init(&app)) aos_ui_register_app(&app);
        } else {
            uint32_t n = s_regs[i].count();
            for (uint32_t k = 0; k < n; k++) {
                memset(&app, 0, sizeof app);
                if (s_regs[i].init_at(&app, k)) aos_ui_register_app(&app);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Screenshot: the same capture the portal's /api/captura uses (screen plus
 * the top and system layers), written as PNG.
 * -------------------------------------------------------------------------- */
static char s_shot_path[256];
static bool s_shot_pending;

static void shot_request(const char *path)
{
    snprintf(s_shot_path, sizeof s_shot_path, "%s", path);
    s_shot_pending = aos_ui_request_snapshot(false);
}

static void shot_tick(void)
{
    if (!s_shot_pending) return;
    aos_ui_snapshot_t s;
    aos_snapshot_state_t st = aos_ui_snapshot_peek(&s);
    if (st == AOS_SNAPSHOT_PENDING) return;
    s_shot_pending = false;
    if (st == AOS_SNAPSHOT_READY) {
        unsigned char *rgb = malloc((size_t)s.w * s.h * 3);
        for (uint32_t y = 0; y < s.h; y++) {
            const uint16_t *row = (const uint16_t *)(s.data + y * s.stride);
            for (uint32_t x = 0; x < s.w; x++) {
                uint16_t p = row[x];
                uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                unsigned char *d = rgb + (y * s.w + x) * 3;
                d[0] = (r << 3) | (r >> 2);
                d[1] = (g << 2) | (g >> 4);
                d[2] = (b << 3) | (b >> 2);
            }
        }
        /* LVGL builds lodepng without its file I/O: encode to memory */
        unsigned char *png = NULL;
        size_t png_len = 0;
        unsigned err = lodepng_encode24(&png, &png_len, rgb, s.w, s.h);
        if (!err) {
            FILE *f = fopen(s_shot_path, "wb");
            if (f) { fwrite(png, 1, png_len, f); fclose(f); } else err = 1;
        }
        free(png);
        free(rgb);
        printf("[shot] %s %ux%u%s\n", s_shot_path, s.w, s.h, err ? " (write failed)" : "");
    } else {
        printf("[shot] failed\n");
    }
    aos_ui_snapshot_release();
}

/* --------------------------------------------------------------------------
 * Keys: noted down in SDL's event watch, done in the main loop (the watch
 * runs inside lv_timer_handler()).
 * -------------------------------------------------------------------------- */
static volatile char s_key;
static bool s_quit;
static bool s_pad_keys;             /* P4_SIM_PAD: the keyboard is a gamepad */

static bool pad_key(SDL_Keycode k)
{
    return s_pad_keys && (k == 'z' || k == 'c' || k == 'x' || k == 'v' || k == 'a' || k == 'd');
}

static int event_watch(void *ud, SDL_Event *e)
{
    if (e->type == SDL_QUIT) s_quit = true;
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        SDL_Keycode k = e->key.keysym.sym;
        if (k < 128 && !pad_key(k)) s_key = (char)k;
        if (k == SDLK_ESCAPE) s_key = 'b';
    }
    return 1;
}

static void do_key(char k)
{
    switch (k) {
    case 'r': aos_ui_request_landscape(-1); break;
    case 'h': aos_ui_home(); break;
    case 'b': aos_ui_back(); break;
    case 'c': aos_ui_home(); break;
    case 'x': aos_hal_sim_notificacion(); break;
    case 'e': aos_ui_edit_home(); break;
    case 's': shot_request("sim_shot.png"); break;
    case 'q': s_quit = true; break;
    default: break;
    }
}

/* The fake gamepad: the keyboard's state every pass of the loop, unless a
 * script's 'pad' is holding it. */
static uint32_t s_pad_hold_until;
static bool s_pad_scripted;

static void pad_tick(void)
{
    if (s_pad_scripted) {
        if ((int32_t)(lv_tick_get() - s_pad_hold_until) < 0) return;
        s_pad_scripted = false;
        aos_hal_sim_pad(true, 0, 0, 0, -1);
    }
    if (!s_pad_keys) return;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint32_t b = 0;
    if (k[SDL_SCANCODE_Z]) b |= 1u << 0;
    if (k[SDL_SCANCODE_C]) b |= 1u << 1;
    if (k[SDL_SCANCODE_X]) b |= 1u << 2;
    if (k[SDL_SCANCODE_V]) b |= 1u << 3;
    if (k[SDL_SCANCODE_A]) b |= 1u << 4;
    if (k[SDL_SCANCODE_D]) b |= 1u << 5;
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) b |= 1u << 8;
    if (k[SDL_SCANCODE_RETURN]) b |= 1u << 9;
    int dx = k[SDL_SCANCODE_RIGHT] - k[SDL_SCANCODE_LEFT];
    int dy = k[SDL_SCANCODE_DOWN] - k[SDL_SCANCODE_UP];
    /* the hat, 0 up then clockwise, as a real pad reports its d-pad */
    static const int8_t HAT[3][3] = { { 7, 0, 1 }, { 6, -1, 2 }, { 5, 4, 3 } };
    aos_hal_sim_pad(true, b, 0, 0, HAT[dy + 1][dx + 1]);
}

/* --------------------------------------------------------------------------
 * Scripts
 * -------------------------------------------------------------------------- */
/* Two-finger frames for aos_gesture (aos_hal_touch_frames). The pointer LVGL
 * sees - the mouse or a scripted tap/drag - is one finger; with Option held a
 * second one mirrors it around the centre of the screen; a scripted pinch
 * drives both fingers itself. */
void aos_hal_sim_set_touch(int count, int x1, int y1, int x2, int y2);

static struct { bool on; int cx, cy, d0, d1; uint32_t t0, ms; } s_pinch;

static void touch_frames_tick(void)
{
    if (s_pinch.on) {
        uint32_t el = lv_tick_get() - s_pinch.t0;
        if (el >= s_pinch.ms + 150) { s_pinch.on = false; aos_hal_sim_set_touch(0, 0, 0, 0, 0); return; }
        if (el > s_pinch.ms) el = s_pinch.ms;
        int d = s_pinch.d0 + (s_pinch.d1 - s_pinch.d0) * (int)el / (int)(s_pinch.ms ? s_pinch.ms : 1);
        aos_hal_sim_set_touch(2, s_pinch.cx - d / 2, s_pinch.cy, s_pinch.cx + d / 2, s_pinch.cy);
        return;
    }
    static lv_indev_t *ptr;
    for (lv_indev_t *in = ptr ? NULL : lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) { ptr = in; break; }
    if (!ptr) return;
    if (lv_indev_get_state(ptr) != LV_INDEV_STATE_PRESSED) { aos_hal_sim_set_touch(0, 0, 0, 0, 0); return; }
    lv_point_t p;
    lv_indev_get_point(ptr, &p);
    if (SDL_GetModState() & KMOD_ALT)
        aos_hal_sim_set_touch(2, p.x, p.y, aos_hal_screen_w() - 1 - p.x, aos_hal_screen_h() - 1 - p.y);
    else
        aos_hal_sim_set_touch(1, p.x, p.y, 0, 0);
}

/* The on-screen keyboard that is showing, if any: 'type' and 'enter' drive
 * whatever text field it is bound to, as fingers on its keys would. */
static lv_obj_t *find_keyboard(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(o, &lv_keyboard_class)) return o;
    for (uint32_t i = lv_obj_get_child_count(o); i-- > 0; ) {
        lv_obj_t *k = find_keyboard(lv_obj_get_child(o, (int32_t)i));
        if (k) return k;
    }
    return NULL;
}

static void script_type(const char *text, bool enter)
{
    lv_obj_t *kb = find_keyboard(lv_screen_active());
    if (!kb) kb = find_keyboard(lv_layer_top());
    lv_obj_t *ta = kb ? lv_keyboard_get_textarea(kb) : NULL;
    if (!ta) { printf("[script] no keyboard showing\n"); return; }
    if (enter) lv_obj_send_event(kb, LV_EVENT_READY, NULL);
    else lv_textarea_add_text(ta, text);
}

static char *s_script;
static char *s_script_pos;
static uint32_t s_script_wait_until;

static void script_step(void)
{
    if (!s_script_pos || !*s_script_pos) return;
    if (lv_tick_get() < s_script_wait_until || s_shot_pending) return;
    char *end = strchr(s_script_pos, ';');
    char cmd[256];
    size_t n = end ? (size_t)(end - s_script_pos) : strlen(s_script_pos);
    if (n >= sizeof cmd) n = sizeof cmd - 1;
    memcpy(cmd, s_script_pos, n);
    cmd[n] = 0;
    s_script_pos = end ? end + 1 : s_script_pos + strlen(s_script_pos);

    char a[200] = "";
    int x, y, x2, y2, ms;
    char *c = cmd;
    while (*c == ' ') c++;
    printf("[script] %s\n", c);
    if (sscanf(c, "wait %d", &ms) == 1) s_script_wait_until = lv_tick_get() + ms;
    else if (sscanf(c, "tap %d %d %d", &x, &y, &ms) >= 2) {
        if (sscanf(c, "tap %d %d %d", &x, &y, &ms) < 3) ms = 80;
        aos_ui_inject_tap(x, y, ms);
        s_script_wait_until = lv_tick_get() + ms + 120;
    } else if (sscanf(c, "drag %d %d %d %d %d", &x, &y, &x2, &y2, &ms) >= 4) {
        int rest = 0;
        int got = sscanf(c, "drag %d %d %d %d %d %d", &x, &y, &x2, &y2, &ms, &rest);
        if (got < 5) ms = 300;
        aos_ui_inject_drag_rest(x, y, x2, y2, ms, rest);
        s_script_wait_until = lv_tick_get() + ms + rest + 150;
    } else if (sscanf(c, "pinch %d %d %d %d %d", &x, &y, &x2, &y2, &ms) >= 4) {
        /* pinch <cx> <cy> <from> <to> [ms]: two fingers on a horizontal line
         * through the centre, 'from' px apart going to 'to' */
        if (sscanf(c, "pinch %d %d %d %d %d", &x, &y, &x2, &y2, &ms) < 5) ms = 500;
        s_pinch = (typeof(s_pinch)){ .on = true, .cx = x, .cy = y, .d0 = x2, .d1 = y2, .t0 = lv_tick_get(), .ms = (uint32_t)ms };
        /* the first finger also presses LVGL, as on the board: aos_gesture
         * only arms on a press of the object it is attached to */
        aos_ui_inject_drag_rest(x - x2 / 2, y, x - y2 / 2, y, ms, 150);
        s_script_wait_until = lv_tick_get() + ms + 300;
    } else if (!strncmp(c, "type ", 5)) script_type(c + 5, false);
    else if (!strcmp(c, "enter")) script_type(NULL, true);
    else if (sscanf(c, "key %1s", a) == 1) do_key(a[0]);
    else if (sscanf(c, "open %199s", a) == 1) aos_ui_open(a);
    else if (!strncmp(c, "rotate", 6)) { aos_ui_request_landscape(-1); s_script_wait_until = lv_tick_get() + 300; }
    else if (sscanf(c, "shot %199s", a) == 1) shot_request(a);
    else if (!strcmp(c, "pad off")) { s_pad_scripted = false; aos_hal_sim_pad(false, 0, 0, 0, -1); }
    else if (sscanf(c, "pad %x %d %d %d", (unsigned *)&x, &y, &x2, &ms) >= 3) {
        int hat = -1;
        int got = sscanf(c, "pad %x %d %d %d %d", (unsigned *)&x, &y, &x2, &ms, &hat);
        if (got < 4) ms = 150;
        aos_hal_sim_pad(true, (uint32_t)x, (int16_t)y, (int16_t)x2, (int8_t)hat);
        s_pad_scripted = true;
        s_pad_hold_until = lv_tick_get() + ms;
        s_script_wait_until = lv_tick_get() + ms + 50;
    }
    else if (!strncmp(c, "quit", 4)) s_quit = true;
    else printf("[script] ?? %s\n", c);
}

/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    lv_init();

    float zoom = getenv("P4_SIM_ZOOM") ? (float)atof(getenv("P4_SIM_ZOOM")) : 0.6f;
    lv_display_t *disp = lv_sdl_window_create(AOS_PANEL_W, AOS_PANEL_H);
    lv_sdl_window_set_zoom(disp, zoom);
    lv_sdl_window_set_title(disp, "P4OS - ESP32-P4-WIFI6-Touch-LCD-5");
    aos_hal_sim_set_display(disp);
    SDL_AddEventWatch(event_watch, NULL);
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    lv_sdl_keyboard_create();

    aos_hal_init();
    aos_apps_register_builtin();
    register_sim_apps();
    aos_ui_init();
    aos_portal_start(getenv("P4_SIM_PORTAL_PORT") ? atoi(getenv("P4_SIM_PORTAL_PORT")) : 8080);

    s_pad_keys = getenv("P4_SIM_PAD") && atoi(getenv("P4_SIM_PAD"));
    if (s_pad_keys) aos_hal_sim_pad(true, 0, 0, 0, -1);

    if (getenv("P4_SIM_SCRIPT")) {
        s_script = strdup(getenv("P4_SIM_SCRIPT"));
        s_script_pos = s_script;
    }

    uint32_t last_tick = 0;
    while (!s_quit) {
        uint32_t wait = lv_timer_handler();
        touch_frames_tick();
        pad_tick();
        if (s_key) { char k = s_key; s_key = 0; do_key(k); }
        uint32_t now = lv_tick_get();
        if (now - last_tick >= 200) {
            last_tick = now;
            aos_ui_tick();
            aos_apps_service_tick();
            aos_hal_sim_idle_tick();
        }
        shot_tick();
        script_step();
        if (s_script_pos && !*s_script_pos && !s_shot_pending && s_script) {
            /* a script that does not quit leaves the window open */
            free(s_script);
            s_script = NULL;
            s_script_pos = NULL;
        }
        usleep((wait > 10 ? 10 : wait) * 1000);
    }
    return 0;
}
