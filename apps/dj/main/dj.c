/*
 * DJ - two decks and a mixer (docs/internal/PEDIDOS-APPS-2026-11.md, G22).
 *
 * The app's life: create() makes the engine (the caches, ~6 MB of
 * PSRAM), starts the audio, I/O and analysis threads and loads the
 * tracks that were on the decks last time. Leaving it (hide)
 * stops the audio and lets go of the speaker, so the music and the other
 * apps can sound; the decks stay where they were. destroy() waits for the
 * three threads to return - the loader unloads the .so right after - and
 * frees everything.
 */
#include "dj_ui.h"

#include "aos_icon_ops.h"

#include <stdio.h>
#include <string.h>

#define U dj_ui

/* two platters and a crossfader */
static const uint8_t DJ_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -19, -8, 34, 34, AIC_CIRCLE, AIC_C_LIT(0x202024), 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, 0, 0, 12, 12, AIC_CIRCLE, AIC_C_LIT(0x3DA5FF), 255),
    AIC_RECT(AIC_CENTER, 0, -11, 3, 8, 1, AIC_C_TEXT, 230),
    AIC_OUT,
    AIC_RECT(AIC_CENTER, 19, -8, 34, 34, AIC_CIRCLE, AIC_C_LIT(0x202024), 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, 0, 0, 12, 12, AIC_CIRCLE, AIC_C_LIT(0xFF7A3D), 255),
    AIC_RECT(AIC_CENTER, 7, 7, 3, 8, 1, AIC_C_TEXT, 230),
    AIC_OUT,
    AIC_RECT(AIC_CENTER, 0, 22, 46, 4, 2, AIC_C_DIM, 255),
    AIC_RECT(AIC_CENTER, 6, 22, 9, 14, 3, AIC_C_TEXT, 255),
    AIC_END
};

static void audio_on(void)
{
    if (U->audio_ok) return;
    U->audio_ok = dj_engine_start(U->e);
    if (!U->audio_ok) aos_ui_toast(_("El parlante está ocupado"), 2000);
}

static void audio_off(void)
{
    if (U->audio_ok) dj_engine_stop(U->e);
    U->audio_ok = false;
}

static void restore_decks(void)
{
    for (int d = 0; d < DJ_NDECK; d++) {
        char key[12], p[256];
        snprintf(key, sizeof key, "dj_deck%d", d);
        if (aos_hal_pref_get_str(key, p, sizeof p) && p[0]) {
            FILE *f = fopen(p, "rb");
            if (f) { fclose(f); dj_load(d, p); }
        }
    }
}

static void remember_decks(void)
{
    for (int d = 0; d < DJ_NDECK; d++) {
        dj_track_t *t = dj_deck_track(U->e, d);
        char key[12];
        snprintf(key, sizeof key, "dj_deck%d", d);
        aos_hal_pref_set_str(key, t->state == DJ_TS_READY ? t->path : "");
    }
}

static void *dj_create(aos_app_t *self, lv_obj_t *root)
{
    U = lv_malloc_zeroed(sizeof *U);
    if (!U) return NULL;
    U->self = self;
    U->root = root;
    U->learn_ctl = -1;
    dj_mkdirs();
    U->e = dj_engine_new();
    if (!U->e) {
        aos_hal_log(DJ_TAG, "no memory for the engine");
        lv_free(U);
        U = NULL;
        return NULL;
    }
    U->m = dj_mix(U->e);
    dj_ui_load_settings();
    dj_midi_load();
    if (!dj_io_start(U->e) || !dj_ana_start(U->e)) {
        dj_io_stop();
        dj_ana_stop();
        dj_engine_free(U->e);
        lv_free(U);
        U = NULL;
        return NULL;
    }
    audio_on();
    restore_decks();
    dj_ui_build();
    dj_ui_timers(true);
    U->log_ms = (uint32_t)aos_hal_uptime_ms();
    aos_hal_log(DJ_TAG, "open: %d Hz, block %d, latency %d ms", DJ_SR, DJ_BLK, aos_hal_spk_latency_ms());
    return U;
}

static void dj_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!U) return;
    dj_ui_timers(false);
    dj_ui_release_all();
    dj_ui_meta_flush(true);
    remember_decks();
    dj_lib_forget();
    audio_off();
    dj_ana_stop();
    dj_io_stop();
    dj_wave_destroy();
    dj_engine_free(U->e);
    lv_free(U);
    U = NULL;
}

static void dj_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    dj_ui_release_all();
    dj_ui_meta_flush(true);
    remember_decks();
    audio_off();
}

static void dj_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    audio_on();
}

static bool dj_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    U->root = root;
    dj_ui_build();
    return true;
}

static bool dj_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U && U->sheet) { dj_sheet_close(); return true; }
    if (U && U->learn) { U->learn = 0; U->learn_ctl = -1; return true; }
    return false;
}

static void dj_tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    dj_ui_tick();
}

static bool dj_init(aos_app_t *app)
{
    app->desc.id       = "aos.dj";
    app->desc.name     = "DJ";
    app->desc.icon     = LV_SYMBOL_AUDIO;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x3DA5FF;
    app->desc.color_b  = 0xFF7A3D;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_KEEP | AOS_APP_FLAG_LONG_DRAG | AOS_APP_FLAG_FULLSCREEN;
    app->desc.order    = 335;
    aos_icon_set_ops(app, DJ_ICON, sizeof DJ_ICON);

    app->create  = dj_create;
    app->destroy = dj_destroy;
    app->show    = dj_show;
    app->hide    = dj_hide;
    app->resize  = dj_resize;
    app->back    = dj_back;
    app->tick    = dj_tick;
    return true;
}

AOS_APP_ENTRY(dj_init);
