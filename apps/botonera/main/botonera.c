/*
 * Botonera - the app: big buttons that play the sounds of <card>/sonidos,
 * several at once (bn.h has the pieces).
 *
 *     tools/build_apps.sh botonera
 *
 * The first time it opens it makes its example sounds (bn_synth.c) and a
 * grid with two pages of them; the page in the portal (web/botonera.js)
 * uploads more, arranges the grid and plays it from the phone.
 */
#include "bn.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"

/* A board of four pads, one of them lit and sounding: two arcs of sound
 * coming out of it. Percent of the icon's size. */
static const uint8_t BN_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -16, -16, 28, 28, 7, AIC_C_PINK,   255),
    AIC_RECT(AIC_CENTER,  16, -16, 28, 28, 7, AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER, -16,  16, 28, 28, 7, AIC_C_TEAL,   255),
    AIC_RECT(AIC_CENTER,  16,  16, 28, 28, 7, AIC_C_TEXT,   255),
    AIC_BORDER(3, AIC_C_PINK, 255),
    AIC_ARC(AIC_CENTER, 16, 16, 44, 0, 4, 0, 360, 300, 340, 0, AIC_C_TEXT, 0, AIC_C_TEXT, 200),
    AIC_END
};

static void *bn_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    return bn_ui_new(root);
}

static void bn_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    bn_ui_free(inst);
}

static void bn_show(aos_app_t *self, void *inst)
{
    (void)self;
    if (inst) bn_ui_show(inst);
}

static void bn_hide(aos_app_t *self, void *inst)
{
    (void)self;
    if (inst) bn_ui_hide(inst);
}

static void bn_tick(aos_app_t *self, void *inst)
{
    (void)self;
    if (inst) bn_ui_tick(inst);
}

static bool bn_back(aos_app_t *self, void *inst)
{
    (void)self;
    return inst && bn_ui_back(inst);
}

static bool bn_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    if (!inst) return false;
    bn_ui_relayout(inst, root);
    return true;
}

static bool bn_init(aos_app_t *app)
{
    app->desc.id       = "aos.botonera";        /* before the icon: it is keyed by id */
    app->desc.name     = "Botonera";
    app->desc.icon     = LV_SYMBOL_AUDIO;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x5E5CE6;
    app->desc.color_b  = 0x2A1F7A;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 200;
    aos_icon_set_ops(app, BN_ICON, sizeof BN_ICON);

    app->create  = bn_create;
    app->destroy = bn_destroy;
    app->show    = bn_show;
    app->hide    = bn_hide;
    app->tick    = bn_tick;
    app->back    = bn_back;
    app->resize  = bn_resize;
    return true;
}

AOS_APP_ENTRY(bn_init);
