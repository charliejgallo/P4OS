/*
 * P4OS - Grabador: the app (gr.h says what each piece does).
 *
 * It stays alive in the background (AOS_APP_FLAG_BACKGROUND) and, armed,
 * starts with the board: a security camera that keeps watching after a power
 * cut. Leaving it does not stop the camera; closing it from the switcher
 * does.
 */
#include "gr.h"
#include "aos_icon_ops.h"

#include <stdlib.h>

gr_app_t gr_app;

static const uint8_t GR_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, -6, 58, 34, 8, AIC_C_TEXT, 255),                   /* the body */
    AIC_RECT(AIC_CENTER, 22, -6, 14, 14, AIC_CIRCLE, AIC_C_LIT(0x1C1C1E), 255),  /* the lens */
    AIC_RECT(AIC_CENTER, -20, 22, 8, 26, 3, AIC_C_TEXT, 255),                   /* the arm */
    AIC_RECT(AIC_CENTER, -16, -16, 8, 8, AIC_CIRCLE, AIC_C_RED, 255),           /* recording */
    AIC_END
};

static lv_timer_t *s_timer;
static bool s_rtsp_on;

static void apply_rtsp(void)
{
    if (gr_app.cfg.rtsp && !s_rtsp_on) s_rtsp_on = gr_rtsp_start(gr_app.cfg.rtsp_port);
    else if (!gr_app.cfg.rtsp && s_rtsp_on) { gr_rtsp_stop(); s_rtsp_on = false; }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    apply_rtsp();
    if (!gr_app.hidden) gr_ui_tick();
    gr_live_tick();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&gr_app, 0, sizeof gr_app);
    gr_app.self = self;
    gr_app.root = root;
    gr_cfg_load(&gr_app.cfg);
    gr_engine_cfg(&gr_app.cfg);
    if (!gr_engine_start()) aos_ui_toast(_("No hay memoria para la cámara"), 2500);
    apply_rtsp();
    gr_ui_build();
    s_timer = lv_timer_create(timer_cb, 150, NULL);
    return &gr_app;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    gr_ui_free();
    lv_obj_clean(gr_app.root);
    /* the loader unloads the .so on the next tick: no thread may be left in it */
    if (s_rtsp_on) { gr_rtsp_stop(); s_rtsp_on = false; }
    gr_engine_stop(8000);
    aos_hal_live_clear(GR_APP_ID);
    aos_ui_status_icon_set(GR_APP_ID, NULL, 0);
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    return gr_ui_back();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    gr_app.hidden = true;
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    gr_app.hidden = false;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    gr_app.root = root;
    gr_ui_free();
    gr_ui_build();
    return true;
}

static bool gr_init(aos_app_t *app)
{
    app->desc.id       = "aos.grabador";    /* literal: gen_lang.py looks for it here */
    app->desc.name     = "Grabador";
    app->desc.icon     = AOS_SYM_CCTV;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x64748B;
    app->desc.color_b  = 0x1E293B;
    app->desc.order    = 136;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, GR_ICON, sizeof GR_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(gr_init);
