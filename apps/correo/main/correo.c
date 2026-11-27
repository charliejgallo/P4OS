/*
 * P4OS - Correo: the app's life cycle (cr.h, apps/correo/README.md).
 *
 * The network thread starts with the app and stays while it lives. With
 * "notices with the app in the background" on (the default) the app keeps
 * AOS_APP_FLAG_BACKGROUND, so the system does not close it on leaving and
 * the INBOXes stay in IDLE: a new message shows as a notice, a sound and
 * the number on the icon. With it off, leaving closes the connections
 * (cr_sync_bg) and the app is only kept like any other.
 *
 * destroy() waits for the thread (cr_sync_stop): the loader unloads the
 * .so right after, and the thread's waits are sliced short for that.
 */
#include "cr.h"
#include "cr_ui.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_icon_ops.h"

#include <stdio.h>

/* An envelope with its flap, and the dot of something unread. */
static const uint8_t CR_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 6, 66, 46, 8, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, -16, 12, 42, 5, 2, AIC_C_LIT(0x1A6FD6), 255),
    AIC_ROT(330),
    AIC_RECT(AIC_TOP_MID, 16, 12, 42, 5, 2, AIC_C_LIT(0x1A6FD6), 255),
    AIC_ROT(-330),
    AIC_OUT,
    AIC_RECT(AIC_CENTER, 31, -20, 24, 24, AIC_CIRCLE, AIC_C_RED, 255),
    AIC_END
};

static struct {
    aos_app_t  *self;
    lv_timer_t *timer;
    bool        hidden;
} S;

static void set_flags(void)
{
    if (!S.self) return;
    uint32_t f = S.self->desc.flags & ~(uint32_t)(AOS_APP_FLAG_BACKGROUND | AOS_APP_FLAG_KEEP);
    f |= CR.notify ? AOS_APP_FLAG_BACKGROUND : AOS_APP_FLAG_KEEP;
    S.self->desc.flags = f;
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    set_flags();
    cr_ui_tick();
    cr_portal_tick();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&S, 0, sizeof S);
    S.self = self;
    cr_store_load();
    set_flags();
    cr_sync_start();
    cr_ui_create(root);
    S.timer = lv_timer_create(timer_cb, 100, NULL);
    aos_hal_log(CR_TAG, "%d account(s), cache %d MB", CR.na, CR.cache_mb);
    return &S;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (S.timer) {
        lv_timer_delete(S.timer);
        S.timer = NULL;
    }
    cr_portal_stop();
    cr_sync_stop();
    cr_ui_destroy();
    cr_contacts_save();
    for (int i = 0; i < CR_ACCT_MAX; i++) {
        if (!CR.a[i].f) continue;
        for (int k = 0; k < CR.a[i].nf; k++) cr_free(CR.a[i].f[k].h);
        cr_free(CR.a[i].f);
        CR.a[i].f = NULL;
        CR.a[i].nf = 0;
    }
    CR.na = 0;
    aos_ui_set_badge(CR_APP_ID, 0);
    aos_hal_live_clear(CR_APP_ID);
    S.self = NULL;
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    S.hidden = false;
    cr_sync_bg(false);
    cr_ui_shown(true);
    const char *arg = aos_ui_take_open_arg(CR_APP_ID);
    if (arg && arg[0]) cr_ui_open_arg(arg);
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    S.hidden = true;
    cr_ui_shown(false);
    cr_sync_bg(true);
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    return cr_ui_back();
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    cr_ui_resize(root);
    return true;
}

static bool cr_init(aos_app_t *app)
{
    app->desc.id       = "aos.correo";
    app->desc.name     = "Correo";
    app->desc.icon     = AOS_SYM_SEND;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x3AA0FF;
    app->desc.color_b  = 0x0A4FB8;
    app->desc.order    = 40;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, CR_ICON, sizeof CR_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->show    = show;
    app->hide    = hide;
    app->back    = back;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(cr_init);
