/*
 * P4OS - Sudoku
 *
 * Classic 9x9 in five levels, 6x6 to start, Killer and Diagonal, and a
 * daily challenge that is the same board for everybody that day. The
 * levels are measured by the techniques a board needs, not by how many
 * clues it has: the generator (sd_core.c) solves each candidate like a
 * person and keeps it only if its hardest step is the level's. The hints
 * come from the same solver, so they explain the technique.
 *
 * Solving pays coins (more without help), and the coins buy themes in the
 * app's own shop: fifteen of them besides the free one, each with its
 * picture, board, digits, sounds and something that moves in the back. The
 * art comes from one pack, sudoku_p4.pak (tools/pack_p4.py), all of it
 * painted by code.
 *
 * Both orientations: turning the screen re-lays out the screen (resize())
 * without destroying the app, so the art stays loaded. A USB gamepad and a
 * keyboard play it too (sd_ui.c has the mapping).
 *
 * The files are described in sd_app.h.
 */
#include "sd_app.h"
#include "aos_icon_ops.h"

#include <string.h>

/* A 3x3 of cells, the middle one lit and two in light blue. */
static const uint8_t SUDOKU_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -19, -19, 17, 17, 4, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 0, -19, 17, 17, 4, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 19, -19, 17, 17, 4, AIC_C_LIT(0x9CC4FF), 255),
    AIC_RECT(AIC_CENTER, -19, 0, 17, 17, 4, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 0, 0, 17, 17, 4, AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER, 19, 0, 17, 17, 4, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -19, 19, 17, 17, 4, AIC_C_LIT(0x9CC4FF), 255),
    AIC_RECT(AIC_CENTER, 0, 19, 17, 17, 4, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 19, 19, 17, 17, 4, AIC_C_TEXT, 255),
    AIC_END
};

static bool s_hidden;

static bool key_cb(uint32_t key, uint8_t mods)
{
    if (s_hidden) return false;
    return sd_ui_key(key, mods);
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    if (s_hidden) {
        sd_U.last_ms = lv_tick_get();
        return;
    }
    sd_ui_frame();
}

static lv_obj_t *layer(lv_obj_t *root)
{
    lv_obj_t *o = lv_obj_create(root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, sd_U.W, sd_U.H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void layout(lv_obj_t *root)
{
    sd_U.W = (int)lv_obj_get_width(root);
    sd_U.H = (int)lv_obj_get_height(root);
    sd_U.land = sd_U.W > sd_U.H;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    int screen = sd_U.screen, preview = sd_U.preview;
    bool first = !sd_D.loaded;
    memset(&sd_U, 0, sizeof sd_U);
    sd_U.self = self;
    sd_U.root = root;
    if (first) {
        sd_data_load();
        sd_game_load();
    } else {
        sd_U.screen = screen;
        sd_U.preview = preview;
    }
    if (sd_U.screen == SD_SCR_GAME && !sd_G.active) sd_U.screen = SD_SCR_MENU;
    if (sd_U.screen == SD_SCR_PREVIEW) sd_U.screen = SD_SCR_SHOP;
    s_hidden = false;

    lv_obj_update_layout(root);
    layout(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(root, 255, 0);
    sd_U.bgimg = lv_image_create(root);
    lv_obj_remove_flag(sd_U.bgimg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sd_U.bgimg, LV_OBJ_FLAG_HIDDEN);
    sd_U.decor = layer(root);
    sd_U.scr = layer(root);

    aos_pad_reset(&sd_U.pad, lv_tick_get());
    sd_theme_show(sd_D.theme);
    sd_gen_start();
    sd_snd_open();
    aos_ui_hwkbd_handler(key_cb);

    sd_ui_build();
    sd_U.last_ms = lv_tick_get();
    sd_U.timer = lv_timer_create(frame_cb, 30, NULL);
    return &sd_U;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    lv_obj_update_layout(root);
    layout(root);
    lv_obj_set_size(sd_U.decor, sd_U.W, sd_U.H);
    lv_obj_set_size(sd_U.scr, sd_U.W, sd_U.H);
    sd_ui_build();
    return true;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (sd_G.active) sd_game_save();
    if (sd_U.timer) lv_timer_delete(sd_U.timer);
    sd_U.timer = NULL;
    aos_ui_hwkbd_handler(NULL);
    sd_gen_stop();
    sd_snd_close();
    sd_board_destroy();
    sd_decor_clear();
    if (self && self->root) lv_obj_clean(self->root);
    /* the objects are gone: now the art they pointed at can go */
    sd_theme_stop();
    extern void sd_snd_release(void);
    sd_snd_release();
    int screen = sd_U.screen, preview = sd_U.preview;
    memset(&sd_U, 0, sizeof sd_U);
    sd_U.screen = screen;
    sd_U.preview = preview;
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    s_hidden = true;
    if (sd_G.active) sd_game_save();
    sd_snd_close();
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    s_hidden = false;
    sd_U.last_ms = lv_tick_get();
    aos_pad_reset(&sd_U.pad, lv_tick_get());
    aos_ui_hwkbd_handler(key_cb);
    sd_snd_open();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    return sd_ui_back();
}

static bool sudoku_init(aos_app_t *app)
{
    app->desc.id = "demo.sudoku";
    app->desc.name = "Sudoku";
    app->desc.icon = "9";
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a = 0x2F6BFF;
    app->desc.color_b = 0x16205E;
    app->desc.flags = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order = 156;
    aos_icon_set_ops(app, SUDOKU_ICON, sizeof SUDOKU_ICON);

    app->create = create;
    app->destroy = destroy;
    app->show = show;
    app->hide = hide;
    app->back = back;
    app->resize = resize;
    return true;
}

AOS_APP_ENTRY(sudoku_init);
