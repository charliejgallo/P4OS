/*
 * P4OS - Fusión
 *
 * A board of numbered tiles: a swipe slides every tile that way, two equal
 * tiles that meet become one of double the value, and after each move a new
 * one appears. The game ends when no move is left.
 *
 * Boards from 3x3 to 6x6 and four modes: Clásico (a goal per level, each
 * one higher, each worth an extra undo), Contra reloj (two minutes, goals
 * add time, undo costs time), Bloques (stones that never move and cut the
 * lines) and De a dos (two players taking turns on one board, passing it
 * along). Twelve achievements, records per size and mode, four themes drawn
 * in code (fu_art.c) and sounds synthesised as they play (fu_snd.c).
 *
 * Touch, a USB gamepad (aos_pad.h: the d-pad moves, B or L undoes, START
 * pauses; the panels go through aos_pad_menu.h) or a keyboard (arrows or
 * WASD, U undoes, Esc pauses). Both orientations: turning the screen lays
 * it out again (resize()) without touching the game.
 *
 * The files are described in fu_app.h.
 */
#include "fu_app.h"
#include "aos_icon_ops.h"

#include <string.h>

/* Two small tiles and the bigger one they make. */
static const uint8_t FUSION_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -16, -14, 25, 25, 7, AIC_C_LIT(0x0D1233), 255),
    AIC_BORDER(AIC_DIV(22), AIC_C_LIT(0x22E5FF), 255),
    AIC_RECT(AIC_CENTER, -16, 14, 25, 25, 7, AIC_C_LIT(0x0D1233), 255),
    AIC_BORDER(AIC_DIV(22), AIC_C_LIT(0x22E5FF), 255),
    AIC_RECT(AIC_CENTER, 15, 0, 34, 34, 9, AIC_C_LIT(0xFF3FD8), 255),
    AIC_GRAD(AIC_C_LIT(0x9B4DFF), AIC_GRAD_VER),
    AIC_BORDER(AIC_DIV(26), AIC_C_LIT(0xFFC6F4), 255),
    AIC_END
};

static bool s_hidden;

static bool key_cb(uint32_t key, uint8_t mods)
{
    if (s_hidden) return false;
    return fu_ui_key(key, mods);
}

static lv_timer_t *s_timer;

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    fu_ui_frame();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    static bool loaded;
    if (!loaded) {
        fu_prefs_load();
        fu_game_load();
        loaded = true;
    }
    s_hidden = false;
    fu_snd_open();
    fu_ui_create(root, self);
    aos_ui_hwkbd_handler(key_cb);
    /* 16 ms: as close to the panel's 60 Hz as an LVGL timer gets */
    s_timer = lv_timer_create(frame_cb, 16, NULL);
    return &fu_G;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    fu_ui_rebuild(root);
    return true;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    aos_ui_hwkbd_handler(NULL);
    fu_ui_destroy();
    fu_snd_free();
    /* the images are gone with the root's children: the sprites can go */
    fu_art_free();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    s_hidden = true;
    fu_ui_hide();
    fu_snd_close();
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    s_hidden = false;
    fu_ui_show();
    aos_ui_hwkbd_handler(key_cb);
    fu_snd_open();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    return fu_ui_back();
}

static bool fusion_init(aos_app_t *app)
{
    app->desc.id = "demo.sumar";
    app->desc.name = "Fusión";
    app->desc.icon = "+";
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a = 0x3B2C9E;
    app->desc.color_b = 0x0D1233;
    /* No orientation flag: both. NO_SWIPE: a swipe is a move here, so the
     * back gesture is the app's (the menu leaves with a swipe right). */
    app->desc.flags = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_NO_SWIPE |
                      AOS_APP_FLAG_LONG_DRAG;
    app->desc.order = 163;
    aos_icon_set_ops(app, FUSION_ICON, sizeof FUSION_ICON);

    app->create = create;
    app->destroy = destroy;
    app->show = show;
    app->hide = hide;
    app->back = back;
    app->resize = resize;
    return true;
}

AOS_APP_ENTRY(fusion_init);
