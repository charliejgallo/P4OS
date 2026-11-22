/*
 * Estudio - a synthesizer, a drum machine and a sequencer, for the big
 * screen and five fingers (docs/internal/PEDIDOS-APPS-2026-11.md, G9).
 *
 * The app's life: create() loads the last project from the card (or makes
 * one with a beat in it), starts the audio thread and the MIDI one, and
 * builds the screen. Leaving it (hide) saves and lets go of the speaker, so
 * the music and the other apps can sound; coming back (show) takes it
 * again. destroy() saves, waits for both threads to return - the loader
 * unloads the .so right after - and frees everything.
 */
#include "es_ui.h"

#include "aos_icon_ops.h"

#include <stdio.h>
#include <string.h>

#define U es_ui

/* a keyboard with three steps lit over it */
static const uint8_t ESTUDIO_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -24, -22, 13, 13, 3, AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER,  -8, -22, 13, 13, 3, AIC_C_TEXT, 90),
    AIC_RECT(AIC_CENTER,   8, -22, 13, 13, 3, AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER,  24, -22, 13, 13, 3, AIC_C_TEXT, 90),
    AIC_RECT(AIC_CENTER, 0, 14, 66, 36, 6, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_LEFT, 16, 0, 2, 36, 0, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_LEFT, 32, 0, 2, 36, 0, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_LEFT, 48, 0, 2, 36, 0, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_LEFT, 11, 0, 10, 21, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_TOP_LEFT, 27, 0, 10, 21, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_TOP_LEFT, 53, 0, 10, 21, 2, AIC_C_BG, 255),
    AIC_OUT,
    AIC_END
};

void es_ui_timers(bool on);
void es_ui_log_stats(void);

static void audio_on(void)
{
    if (U->audio_ok) return;
    U->audio_ok = es_engine_start(U->e);
    if (!U->audio_ok) aos_ui_toast(_("El parlante está ocupado"), 2000);
    es_midi_start(U->e, U->p);
}

static void audio_off(void)
{
    es_midi_stop();
    if (U->audio_ok) es_engine_stop(U->e);
    U->audio_ok = false;
}

static void *es_create(aos_app_t *self, lv_obj_t *root)
{
    U = lv_malloc_zeroed(sizeof *U);
    if (!U) return NULL;
    U->self = self;
    U->root = root;
    U->p = es_alloc(sizeof *U->p);
    if (!U->p) { lv_free(U); U = NULL; return NULL; }

    int32_t v;
    U->octave = 3;
    U->vel_lvl = 1;
    U->export_loops = 4;
    U->latency = aos_hal_pref_get_i32("es_lat", &v) ? (int)v : 1;
    U->voices = aos_hal_pref_get_i32("es_voices", &v) ? (int)v : 8;
    U->mo_pc = ES_PC_ALL;
    if (aos_hal_pref_get_i32("es_mout", &v)) {
        U->mo_dest = v & 3;
        U->mo_clock = (v & 16) != 0;
        U->mo_pc = (v >> 8) & 7;
        if (U->mo_pc > ES_PC_ALL) U->mo_pc = ES_PC_ALL;
    }
    if (aos_hal_pref_get_i32("es_pmode", &v)) U->play_mode = (int)v % 3;
    if (aos_hal_pref_get_i32("es_scale", &v)) { U->scale = (int)(v & 0xFF) % 6; U->key_root = (int)(v >> 8) % 12; }
    for (int i = 0; i < ES_PARTS; i++) U->roll_low[i] = i == 0 ? 28 : 48;

    es_mkdirs();
    U->e = es_engine_new(U->p, true);
    if (!U->e) { es_free(U->p); lv_free(U); U = NULL; return NULL; }

    char f[ES_NAME + 8] = "";
    bool ok = aos_hal_pref_get_str("es_proj", f, sizeof f) && f[0] && es_ui_project_open(f);
    if (!ok) {
        es_proj_default(U->p);
        es_pat_demo(U->p);
        snprintf(U->p->name, sizeof U->p->name, "%s", _("Primer proyecto"));
        snprintf(U->file, sizeof U->file, "%s", "primer_proyecto.json");
        es_ui_project_save();
    }
    es_ui_preload_samples();
    es_ui_apply_settings();
    audio_on();

    lv_obj_update_layout(root);
    bool land = lv_obj_get_width(root) > lv_obj_get_height(root);
    U->view = aos_hal_pref_get_i32(land ? "es_view_l" : "es_view_p", &v) ? (int)v : (land ? V_PLAY : V_DRUMS);
    es_ui_build();
    es_ui_timers(true);
    U->log_ms = (uint32_t)aos_hal_uptime_ms();
    aos_hal_log(ES_TAG, "open: %d Hz, block %d, latency level %d, %d voices, project %s",
                ES_SR, ES_BLK, U->latency, U->voices, U->file);
    return U;
}

static void es_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!U) return;
    es_ui_timers(false);
    if (U->forget) U->forget();
    es_play_release_all();
    es_ui_project_save();
    audio_off();
    es_export_cancel();
    es_engine_free(U->e);
    es_bank_free_all();
    es_free(U->clip);
    es_free(U->p);
    lv_free(U);
    U = NULL;
}

static void es_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    es_play_release_all();
    if (U->dirty) es_ui_project_save();
    audio_off();
}

static void es_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    audio_on();
    es_ui_apply_settings();
}

static bool es_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    U->root = root;
    int32_t v;
    lv_obj_update_layout(root);
    bool land = lv_obj_get_width(root) > lv_obj_get_height(root);
    U->view = aos_hal_pref_get_i32(land ? "es_view_l" : "es_view_p", &v) ? (int)v : (land ? V_PLAY : V_DRUMS);
    es_ui_build();
    return true;
}

static bool es_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U && U->sheet) { ui_sheet_close(); return true; }
    return false;
}

static void es_tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!U) return;
    es_ui_log_stats();
    /* the project goes to the card a few seconds after the last change */
    if (U->dirty && (uint32_t)aos_hal_uptime_ms() - U->dirty_ms > 4000 && !es_recording(U->e))
        es_ui_project_save();
}

static bool es_init(aos_app_t *app)
{
    app->desc.id       = "aos.estudio";
    app->desc.name     = "Estudio";
    app->desc.icon     = LV_SYMBOL_AUDIO;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xFF6A3D;
    app->desc.color_b  = 0x7A2BD8;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_KEEP | AOS_APP_FLAG_LONG_DRAG;
    app->desc.order    = 330;
    aos_icon_set_ops(app, ESTUDIO_ICON, sizeof ESTUDIO_ICON);

    app->create  = es_create;
    app->destroy = es_destroy;
    app->show    = es_show;
    app->hide    = es_hide;
    app->resize  = es_resize;
    app->back    = es_back;
    app->tick    = es_tick;
    return true;
}

AOS_APP_ENTRY(es_init);
