/*
 * P4OS - PWM generator (aos.pwm), a workshop tool on the rear header.
 *
 * Up to seven channels of the LEDC, each on a pin of the header: free PWM,
 * a servo, a LED with its gamma curve, or an analog level through an RC
 * filter; a big knob, patterns (breathe, sweep, strobe, ramp, sine, steps),
 * the signal drawn to scale, presets on the card and a page in the portal.
 * The API is aos_io_pwm_* and aos_io_dac_* (docs/MODULES.md, "PWM, an
 * analog level, infrared and CAN").
 *
 * It keeps running in the background (AOS_APP_FLAG_KEEP): a servo holds its
 * angle and a pattern goes on while another app is in front. Closing the
 * app stops every output and releases the pins.
 *
 *     tools/build_apps.sh pwm
 *     tools/install_apps.sh p4os.local pwm
 */
#include "pw.h"
#include "aos_icon_ops.h"

#include <stdlib.h>

/* A square wave: two periods, the second one with a shorter high time. */
static const uint8_t PW_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -27,  12, 12,  7, 3, AIC_C_TEXT, 255),    /* low   */
    AIC_RECT(AIC_CENTER, -21,   0,  7, 31, 3, AIC_C_TEXT, 255),    /* rise  */
    AIC_RECT(AIC_CENTER, -10, -12, 26,  7, 3, AIC_C_TEXT, 255),    /* high  */
    AIC_RECT(AIC_CENTER,   0,   0,  7, 31, 3, AIC_C_TEXT, 255),    /* fall  */
    AIC_RECT(AIC_CENTER,   7,  12, 20,  7, 3, AIC_C_TEXT, 255),    /* low   */
    AIC_RECT(AIC_CENTER,  14,   0,  7, 31, 3, AIC_C_TEXT, 255),    /* rise  */
    AIC_RECT(AIC_CENTER,  20, -12, 13,  7, 3, AIC_C_TEXT, 255),    /* high  */
    AIC_RECT(AIC_CENTER,  25,   0,  7, 31, 3, AIC_C_TEXT, 255),    /* fall  */
    AIC_RECT(AIC_CENTER,  29,  12,  8,  7, 3, AIC_C_TEXT, 255),    /* low   */
    AIC_END
};

static void *pw_create(aos_app_t *self, lv_obj_t *root)
{
    /* a few KB: malloc puts anything of 1 KB or more in PSRAM */
    pw_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    pw_g = g;
    g->app = self;
    g->root = root;
    g->target = PW_T_VALUE;
    pw_store_restore();
    if (!g->n) pw_add(PW_M_PWM);
    pw_ui_build();
    pw_engine_start();
    return g;
}

static void pw_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    if (!inst) return;
    pw_engine_stop();
    pw_sheet_forget();
    pw_close_all();
    pw_store_final();
    aos_io_release_owner(PW_OWNER);
    free(inst);
    pw_g = NULL;
}

static bool pw_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    pw_g->root = root;
    pw_ui_build();
    return true;
}

static bool pw_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (pw_g && pw_g->sheet) {
        pw_sheet_close();
        pw_ui_refresh();
        return true;
    }
    return false;
}

static bool pw_init(aos_app_t *app)
{
    app->desc.id       = "aos.pwm";              /* before the icon: it is keyed by id */
    app->desc.name     = "PWM";
    app->desc.icon     = LV_SYMBOL_SHUFFLE;      /* the fallback, if the blob were refused */
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xFF9F0A;
    app->desc.color_b  = 0xB34700;
    app->desc.flags    = AOS_APP_FLAG_KEEP;
    app->desc.order    = 160;
    aos_icon_set_ops(app, PW_ICON, sizeof PW_ICON);

    app->create  = pw_create;
    app->destroy = pw_destroy;
    app->resize  = pw_resize;
    app->back    = pw_back;
    return true;
}

AOS_APP_ENTRY(pw_init);
