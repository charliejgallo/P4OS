/*
 * Estudio - Sonido (the synth of each part, its presets) and Mezcla (the
 * mixer, the effects, the metronome).
 *
 * Every control is a slider or a row of options bound to a float of the
 * project (ui_param, ui_slider): the audio thread reads the same float on
 * its next block, so a move is heard within a millisecond.
 *
 * The presets: the factory ones are in the code (es_patch.c); the user's
 * are one JSON file each in <card>/estudio/presets, so the portal can list
 * and carry them.
 */
#include "es_ui.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define U es_ui

static lv_obj_t *s_scroll, *s_env[2], *s_preset;

static lv_obj_t *scroller(lv_obj_t *body, int32_t top)
{
    int32_t W = lv_obj_get_width(body), H = lv_obj_get_height(body);
    lv_obj_t *s = lv_obj_create(body);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, W, H - top);
    lv_obj_set_pos(s, 0, top);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_left(s, 12, 0);
    lv_obj_set_style_pad_right(s, 12, 0);
    lv_obj_set_style_pad_top(s, 4, 0);
    lv_obj_set_style_pad_bottom(s, 60, 0);
    lv_obj_set_style_pad_column(s, 12, 0);
    lv_obj_set_style_pad_row(s, 12, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    return s;
}

static int32_t card_w(void)
{
    int cols = U->land ? 3 : 1;
    return (U->W - 24 - 12 * (cols - 1)) / cols;
}

/* ---- the envelope's picture ---- */

static void env_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    const float *P = U->p->part[U->part].p;
    float a = P[which ? P_A2 : P_A1], d = P[which ? P_D2 : P_D1], s = P[which ? P_S2 : P_S1],
          r = P[which ? P_R2 : P_R1];
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    float w = (float)lv_area_get_width(&c), h = (float)lv_area_get_height(&c) - 8;
    /* the times on a square-root axis, so a 5 ms attack and a 5 s release
     * both show; the sustain holds a fixed stretch */
    float ta = sqrtf(a), td = sqrtf(d), tr = sqrtf(r), hold = 0.6f;
    float tot = ta + td + hold + tr;
    float k = w / tot;
    lv_point_precise_t p[5] = {
        { c.x1, c.y2 - 4 },
        { c.x1 + ta * k, c.y1 + 4 },
        { c.x1 + (ta + td) * k, c.y1 + 4 + (1 - s) * h },
        { c.x1 + (ta + td + hold) * k, c.y1 + 4 + (1 - s) * h },
        { c.x2, c.y2 - 4 },
    };
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = lv_color_hex(ui_part_color(U->part));
    ld.width = 4;
    ld.round_start = ld.round_end = 1;
    lv_layer_t *layer = lv_event_get_layer(e);
    for (int i = 0; i < 4; i++) {
        ld.p1 = p[i];
        ld.p2 = p[i + 1];
        lv_draw_line(layer, &ld);
    }
}

static void env_changed(void)
{
    for (int i = 0; i < 2; i++) if (s_env[i]) lv_obj_invalidate(s_env[i]);
}

static lv_obj_t *env_card(lv_obj_t *parent, int which, int32_t w)
{
    float *P = U->p->part[U->part].p;
    lv_obj_t *c = ui_card(parent, which ? _("Envolvente 2: filtro y FM") : _("Envolvente 1: volumen"), w);
    lv_obj_t *pic = ui_box(c, w - 28, 80);
    lv_obj_add_event_cb(pic, env_draw, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)which);
    s_env[which] = pic;
    int base = which ? P_A2 : P_A1;
    for (int i = 0; i < 4; i++) ui_param(c, base + i, &P[base + i], w - 28, env_changed);
    return c;
}

/* ---- the presets ---- */

static void preset_text(void)
{
    if (!s_preset) return;
    char b[ES_NAME + 8];
    snprintf(b, sizeof b, "%s  " LV_SYMBOL_DOWN, _(U->p->part[U->part].name));
    ui_btn_text(s_preset, b);
}

static void factory_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    U->p->part[U->part] = *es_patch_factory(i);
    ui_sheet_close();
    es_ui_dirty();
    es_ui_show_view(V_SOUND);
}

static void own_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target_obj(e);
    const char *file = lv_obj_get_user_data(b);
    char path[320];
    es_path(path, sizeof path, "presets", file);
    es_patch_t tmp;
    if (!es_patch_load(&tmp, path)) { aos_ui_toast(_("No se pudo leer ese sonido"), 1800); return; }
    U->p->part[U->part] = tmp;
    ui_sheet_close();
    es_ui_dirty();
    es_ui_show_view(V_SOUND);
}

static void save_as_done(const char *name)
{
    char file[ES_NAME + 8], path[320];
    size_t j = 0;
    for (const char *s = name; *s && j < sizeof file - 6; s++) {
        unsigned char ch = (unsigned char)*s;
        file[j++] = (ch == '/' || ch == '\\' || ch == ':' || ch < 0x20) ? '_' : (char)ch;
    }
    snprintf(file + j, sizeof file - j, ".json");
    es_path(path, sizeof path, "presets", file);
    es_patch_t *p = &U->p->part[U->part];
    snprintf(p->name, sizeof p->name, "%s", name);
    if (es_patch_save(p, path)) aos_ui_toast(_("Sonido guardado"), 1500);
    else aos_ui_toast(_("No se pudo guardar"), 1800);
    es_ui_dirty();
    preset_text();
}

static void save_as_cb(lv_event_t *e)
{
    (void)e;
    ui_ask_text(_("Nombre del sonido"), U->p->part[U->part].name, save_as_done);
}

/* the file names of the user's presets live as long as the sheet */
static char s_files[48][ES_NAME + 8];

static void presets_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *c = ui_sheet(_("Sonidos"));
    int32_t w = lv_obj_get_width(c);
    lv_obj_t *sv = ui_btn(c, _("Guardar este sonido como…"), aos_font_small, w, 60, NULL, NULL);
    lv_obj_add_event_cb(sv, save_as_cb, LV_EVENT_CLICKED, NULL);
    ui_btn_on(sv, true, aos_theme_accent_rgb());
    ui_label(c, _("De fábrica"), aos_font_small, C_DIM);
    lv_obj_t *g = ui_row(c, w, 8);
    int32_t bw = U->land ? (w - 16) / 3 : (w - 8) / 2;
    for (int i = 0; i < es_patch_factory_n(); i++)
        ui_btn(g, _(es_patch_factory(i)->name), aos_font_caption, bw, 58, factory_cb, (void *)(intptr_t)i);
    ui_label(c, _("Míos (estudio/presets)"), aos_font_small, C_DIM);
    char dir[300];
    es_path(dir, sizeof dir, "presets", "");
    DIR *d = opendir(dir);
    int n = 0;
    if (d) {
        lv_obj_t *g2 = ui_row(c, w, 8);
        struct dirent *de;
        while ((de = readdir(d)) && n < 48) {
            const char *nm = de->d_name;
            size_t l = strlen(nm);
            if (nm[0] == '.' || l < 6 || strcasecmp(nm + l - 5, ".json") || l >= sizeof s_files[0]) continue;
            snprintf(s_files[n], sizeof s_files[n], "%.47s", nm);
            char label[ES_NAME + 8];
            snprintf(label, sizeof label, "%.*s", (int)(l - 5), nm);
            lv_obj_t *b = ui_btn(g2, label, aos_font_caption, bw, 58, NULL, NULL);
            lv_obj_set_user_data(b, s_files[n]);
            lv_obj_add_event_cb(b, own_cb, LV_EVENT_CLICKED, NULL);
            n++;
        }
        closedir(d);
    }
    if (!n) ui_label(c, _("Todavía ninguno."), aos_font_caption, C_DIM);
}

/* ---- Sonido ---- */

static void part_cb(lv_event_t *e)
{
    U->part = (int)(intptr_t)lv_event_get_user_data(e);
    es_set_live_part(U->e, U->part);
    es_ui_show_view(V_SOUND);
}

static void engine_changed(void)
{
    es_ui_show_view(V_SOUND);
}

static void try_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    int n = 12 * (U->octave + 1) + 12;
    if (c == LV_EVENT_PRESSED) es_cmd(U->e, EC_NOTE_ON, U->part, n, 100, SRC_LIVE);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) es_cmd(U->e, EC_NOTE_OFF, U->part, n, 0, SRC_LIVE);
}

static void s_forget(void)
{
    s_scroll = s_preset = NULL;
    s_env[0] = s_env[1] = NULL;
}

void es_view_sound(lv_obj_t *body)
{
    int32_t W = lv_obj_get_width(body);
    float *P = U->p->part[U->part].p;
    lv_obj_t *hdr = ui_row(body, W - 24, 8);
    lv_obj_set_pos(hdr, 12, 8);
    for (int i = 0; i < ES_PARTS; i++) {
        char b[4];
        snprintf(b, sizeof b, "%d", i + 1);
        lv_obj_t *pb = ui_btn(hdr, b, aos_font_small, 54, 58, part_cb, (void *)(intptr_t)i);
        ui_btn_on(pb, i == U->part, ui_part_color(i));
    }
    int32_t pw = U->land ? 330 : W - 24 - 3 * 62 - 8;
    s_preset = ui_btn(hdr, "", aos_font_small, pw, 58, presets_cb, NULL);
    lv_obj_t *pl = lv_obj_get_child(s_preset, 0);
    lv_label_set_long_mode(pl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(pl, pw - 16);
    lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_CENTER, 0);
    preset_text();
    lv_obj_t *eng = ui_param(hdr, P_ENGINE, &P[P_ENGINE], U->land ? 330 : W - 24 - 168, engine_changed);
    (void)eng;
    lv_obj_t *tr = ui_btn(hdr, _("Probar"), aos_font_small, 150, 58, NULL, NULL);
    lv_obj_add_event_cb(tr, try_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(tr, try_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(tr, try_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_update_layout(body);
    s_scroll = scroller(body, 8 + lv_obj_get_height(hdr) + 10);
    int32_t cw = card_w(), iw = cw - 28;
    bool fm = P[P_ENGINE] >= 0.5f;
    if (fm) {
        lv_obj_t *c = ui_card(s_scroll, _("FM de dos operadores"), cw);
        static const int FM[] = { P_O1OCT, P_FMRAT, P_FMIDX, P_FMENV, P_FMFB, P_NOISE };
        for (size_t i = 0; i < sizeof FM / sizeof FM[0]; i++) ui_param(c, FM[i], &P[FM[i]], iw, NULL);
    } else {
        lv_obj_t *c = ui_card(s_scroll, _("Oscilador 1"), cw);
        static const int O1[] = { P_O1W, P_O1OCT, P_O1PW, P_NOISE };
        for (size_t i = 0; i < sizeof O1 / sizeof O1[0]; i++) ui_param(c, O1[i], &P[O1[i]], iw, NULL);
        c = ui_card(s_scroll, _("Oscilador 2"), cw);
        static const int O2[] = { P_O2W, P_O2SEMI, P_O2FINE, P_O2MIX };
        for (size_t i = 0; i < sizeof O2 / sizeof O2[0]; i++) ui_param(c, O2[i], &P[O2[i]], iw, NULL);
    }
    lv_obj_t *c = ui_card(s_scroll, _("Filtro"), cw);
    static const int FL[] = { P_FTYPE, P_CUT, P_RES, P_FENV, P_FKEY, P_FVEL };
    for (size_t i = 0; i < sizeof FL / sizeof FL[0]; i++) ui_param(c, FL[i], &P[FL[i]], iw, NULL);
    env_card(s_scroll, 0, cw);
    env_card(s_scroll, 1, cw);
    c = ui_card(s_scroll, "LFO", cw);
    static const int LF[] = { P_LFOW, P_LFOR, P_LFODST, P_LFODEP };
    for (size_t i = 0; i < sizeof LF / sizeof LF[0]; i++) ui_param(c, LF[i], &P[LF[i]], iw, NULL);
    c = ui_card(s_scroll, _("Voz"), cw);
    static const int VO[] = { P_MONO, P_GLIDE, P_VOL, P_VELAMP };
    for (size_t i = 0; i < sizeof VO / sizeof VO[0]; i++) ui_param(c, VO[i], &P[VO[i]], iw, NULL);
    U->forget = s_forget;
}

/* ======================================================================
 * Mezcla
 * ====================================================================== */

static lv_obj_t *m_div, *m_mute[ES_CH], *m_met, *m_beats, *m_stats;

static void mute_cb(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    U->p->ch[c].mute = !U->p->ch[c].mute;
    ui_btn_on(m_mute[c], U->p->ch[c].mute, C_REC);
    es_ui_dirty();
}

static void div_paint(void)
{
    char b[32];
    snprintf(b, sizeof b, "%s %s", _("División"), ES_DLY_NAMES[U->p->fx.dly_div % DD_COUNT]);
    ui_btn_text(m_div, b);
}

static void div_cb(lv_event_t *e)
{
    (void)e;
    U->p->fx.dly_div = (uint8_t)((U->p->fx.dly_div + 1) % DD_COUNT);
    div_paint();
    es_ui_dirty();
}

static void met_paint(void)
{
    ui_btn_on(m_met, U->p->met_on, 0xFFD60A);
    ui_btn_on(U->b_met, U->p->met_on, 0xFFD60A);
}

static void met_cb(lv_event_t *e)
{
    (void)e;
    U->p->met_on = !U->p->met_on;
    met_paint();
    es_ui_dirty();
}

static void beats_step(int d)
{
    int b = U->p->beats + d;
    U->p->beats = (uint8_t)(b < 2 ? 2 : b > 7 ? 7 : b);
    lv_label_set_text_fmt(m_beats, "%d/4", U->p->beats);
    es_ui_dirty();
}

static void beats_minus(lv_event_t *e) { (void)e; beats_step(-1); }
static void beats_plus(lv_event_t *e)  { (void)e; beats_step(1); }

static void m_refresh(void)
{
    static uint32_t last;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!m_stats || now - last < 500) return;
    last = now;
    es_stats_t s;
    es_stats(U->e, &s);
    lv_label_set_text_fmt(m_stats, _("Audio: %d %% del tiempo · %d voces · faltas %u"),
                          (int)s.load_pct, s.voices, (unsigned)s.underruns);
}

static void m_forget(void)
{
    m_div = m_met = m_beats = m_stats = NULL;
    memset(m_mute, 0, sizeof m_mute);
}

void es_view_mix(lv_obj_t *body)
{
    es_proj_t *p = U->p;
    lv_obj_t *sc = scroller(body, 8);
    int32_t cw = U->land ? (U->W - 24 - 36) / 4 : (U->W - 36) / 2, iw = cw - 28;
    for (int c = 0; c < ES_CH; c++) {
        char t[ES_NAME + 16];
        if (c < ES_PARTS) snprintf(t, sizeof t, "%d · %s", c + 1, _(p->part[c].name));
        else snprintf(t, sizeof t, "%s", _("Batería"));
        lv_obj_t *k = ui_card(sc, NULL, cw);
        lv_obj_t *tl = ui_label(k, t, aos_font_small, c < ES_PARTS ? ui_part_color(c) : 0xFF9F0A);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(tl, iw);
        es_chan_t *ch = &p->ch[c];
        ui_slider(k, _("Volumen"), &ch->vol, 0, 1, U_PCT, iw, NULL);
        ui_slider(k, _("Distorsión"), &ch->drive, 0, 1, U_PCT, iw, NULL);
        ui_slider(k, _("Al delay"), &ch->dly, 0, 1, U_PCT, iw, NULL);
        ui_slider(k, _("A la reverb"), &ch->rev, 0, 1, U_PCT, iw, NULL);
        m_mute[c] = ui_btn(k, _("Silenciar"), aos_font_caption, iw, 52, mute_cb, (void *)(intptr_t)c);
        ui_btn_on(m_mute[c], ch->mute, C_REC);
    }
    int32_t fw = U->land ? (U->W - 24 - 36) / 4 : (U->W - 36) / 2, fi = fw - 28;
    es_fx_t *fx = &p->fx;
    lv_obj_t *k = ui_card(sc, _("Delay"), fw);
    m_div = ui_btn(k, "", aos_font_caption, fi, 52, div_cb, NULL);
    div_paint();
    ui_slider(k, _("Repeticiones"), &fx->dly_fb, 0, 1, U_PCT, fi, NULL);
    ui_slider(k, _("Brillo"), &fx->dly_tone, 0, 1, U_PCT, fi, NULL);
    ui_slider(k, _("Nivel"), &fx->dly_ret, 0, 1, U_PCT, fi, NULL);
    k = ui_card(sc, _("Reverb"), fw);
    ui_slider(k, _("Tamaño"), &fx->rev_size, 0, 1, U_PCT, fi, NULL);
    ui_slider(k, _("Apagado"), &fx->rev_damp, 0, 1, U_PCT, fi, NULL);
    ui_slider(k, _("Nivel"), &fx->rev_ret, 0, 1, U_PCT, fi, NULL);
    k = ui_card(sc, _("Máster"), fw);
    ui_slider(k, _("Filtro"), &fx->mfilt, -1, 1, U_MFILT, fi, NULL);
    lv_obj_t *nl0 = ui_label(k, _("A la izquierda corta los agudos; a la derecha, los graves."), aos_font_tiny, C_DIM);
    lv_label_set_long_mode(nl0, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(nl0, fi);
    ui_slider(k, _("Resonancia"), &fx->mres, 0, 1, U_PCT, fi, NULL);
    ui_slider(k, _("Volumen"), &fx->master, 0, 1, U_PCT, fi, NULL);
    k = ui_card(sc, _("Metrónomo"), fw);
    m_met = ui_btn(k, _("Metrónomo"), aos_font_caption, fi, 52, met_cb, NULL);
    met_paint();
    ui_slider(k, _("Volumen"), &p->met_vol, 0, 1, U_PCT, fi, NULL);
    char b[8];
    snprintf(b, sizeof b, "%d/4", p->beats);
    ui_label(k, _("Compás"), aos_font_caption, C_TEXT);
    ui_stepper(k, b, fi, 52, beats_minus, beats_plus, &m_beats);
    lv_obj_t *nl = ui_label(k, _("Cuenta un compás antes de grabar."), aos_font_tiny, C_DIM);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(nl, fi);
    m_stats = ui_label(sc, "", aos_font_caption, C_DIM);
    lv_obj_set_width(m_stats, U->W - 24);
    U->refresh = m_refresh;
    U->forget = m_forget;
}
