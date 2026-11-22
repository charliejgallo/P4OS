/*
 * Estudio - Canción: the eight patterns, the song that chains them, the
 * project on the card, the export, USB MIDI and the audio settings.
 */
#include "es_ui.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define U es_ui

static lv_obj_t *g_pats, *g_chain, *g_name, *g_prog, *g_xlbl, *g_loops, *g_stats, *g_midi, *g_mode;
static bool g_exporting;

static int32_t col_w(void)
{
    return U->land ? (U->W - 24 - 12) / 2 : U->W - 24;
}

/* ---- patterns ---- */

static void pats_paint(void)
{
    if (!g_pats) return;
    int play = es_playing(U->e) ? es_play_pat(U->e) : -1;
    for (int i = 0; i < ES_PATS; i++) {
        lv_obj_t *b = lv_obj_get_child(g_pats, i);
        ui_btn_on(b, i == U->pat, aos_theme_accent_rgb());
        lv_obj_set_style_border_width(b, i == play ? 3 : 0, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_PLAY), 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        const es_pat_t *t = &U->p->pat[i];
        if (ui_pat_used(t)) lv_label_set_text_fmt(l, "%c\n%d", 'A' + i, t->len);
        else lv_label_set_text_fmt(l, "%c\n-", 'A' + i);
    }
}

static void pat_cb(lv_event_t *e)
{
    es_ui_select_pat((int)(intptr_t)lv_event_get_user_data(e));
    pats_paint();
}

static void copy_cb(lv_event_t *e)
{
    (void)e;
    if (!U->clip) U->clip = es_alloc(sizeof *U->clip);
    if (!U->clip) return;
    memcpy(U->clip, &U->p->pat[U->pat], sizeof *U->clip);
    U->clip_ok = true;
    char m[48];
    snprintf(m, sizeof m, _("Patrón %c copiado"), 'A' + U->pat);
    aos_ui_toast(m, 1200);
}

static void paste_cb(lv_event_t *e)
{
    (void)e;
    if (!U->clip_ok) { aos_ui_toast(_("Primero copiá un patrón"), 1400); return; }
    memcpy(&U->p->pat[U->pat], U->clip, sizeof *U->clip);
    es_ui_dirty();
    pats_paint();
}

static void clear_yes(void)
{
    es_pat_clear(&U->p->pat[U->pat]);
    es_ui_dirty();
    pats_paint();
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    char m[80];
    snprintf(m, sizeof m, _("Borrar todo el patrón %c."), 'A' + U->pat);
    ui_confirm(m, _("Borrar"), clear_yes);
}

/* ---- the song ---- */

static void chain_build(void);

static void slot_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        es_proj_t *p = U->p;
        memmove(p->chain + i, p->chain + i + 1, (size_t)(p->chain_n - i - 1));
        p->chain_n--;
        lv_indev_wait_release(lv_indev_active());
    } else {
        U->p->chain[i] = (uint8_t)((U->p->chain[i] + 1) % ES_PATS);
    }
    es_ui_dirty();
    chain_build();
}

static void chain_build(void)
{
    if (!g_chain) return;
    lv_obj_clean(g_chain);
    es_proj_t *p = U->p;
    int now = es_playing(U->e) && p->song_mode ? es_chain_pos(U->e) : -1;
    for (int i = 0; i < p->chain_n; i++) {
        char b[2] = { (char)('A' + p->chain[i] % ES_PATS), 0 };
        lv_obj_t *s = ui_btn(g_chain, b, aos_font_small, 52, 52, NULL, NULL);
        lv_obj_add_event_cb(s, slot_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, slot_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        ui_btn_on(s, i == now, C_PLAY);
    }
    if (!p->chain_n) ui_label(g_chain, _("Vacía: agregá patrones abajo."), aos_font_caption, C_DIM);
}

static void add_cb(lv_event_t *e)
{
    es_proj_t *p = U->p;
    if (p->chain_n >= ES_CHAIN) return;
    p->chain[p->chain_n++] = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    es_ui_dirty();
    chain_build();
}

static void chain_back_cb(lv_event_t *e)
{
    (void)e;
    if (U->p->chain_n) U->p->chain_n--;
    es_ui_dirty();
    chain_build();
}

static void mode_cb(lv_event_t *e)
{
    U->p->song_mode = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    ui_seg_set(g_mode, U->p->song_mode, aos_theme_accent_rgb());
    es_ui_dirty();
}

/* ---- the project ---- */

static void name_paint(void)
{
    if (g_name) lv_label_set_text_fmt(g_name, "%s", U->p->name);
}

static void new_cb(lv_event_t *e)
{
    (void)e;
    es_ui_project_new();
    es_ui_show_view(V_SONG);
    aos_ui_toast(_("Proyecto nuevo"), 1200);
}

static void file_for(const char *name, char *out, size_t n)
{
    size_t j = 0;
    for (const char *s = name; *s && j + 6 < n; s++) {
        unsigned char c = (unsigned char)*s;
        out[j++] = (c == '/' || c == '\\' || c == ':' || c == '.' || c < 0x20) ? '_' : c == ' ' ? '_' : (char)c;
    }
    if (!j) out[j++] = '_';
    snprintf(out + j, n - j, ".json");
}

static void save_as_done(const char *name)
{
    snprintf(U->p->name, sizeof U->p->name, "%s", name);
    file_for(name, U->file, sizeof U->file);
    es_ui_project_save();
    name_paint();
    aos_ui_toast(_("Guardado"), 1200);
}

static void save_as_cb(lv_event_t *e)
{
    (void)e;
    ui_ask_text(_("Nombre del proyecto"), U->p->name, save_as_done);
}

static char s_files[40][ES_NAME + 8];
static int  s_del;

static void open_one_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ui_sheet_close();
    if (!es_ui_project_open(s_files[i])) { aos_ui_toast(_("No se pudo abrir"), 1600); return; }
    es_ui_build();
}

static void del_yes(void)
{
    char path[320];
    es_path(path, sizeof path, NULL, s_files[s_del]);
    unlink(path);
    aos_ui_toast(_("Borrado"), 1200);
}

static void del_one_cb(lv_event_t *e)
{
    s_del = (int)(intptr_t)lv_event_get_user_data(e);
    char m[96];
    snprintf(m, sizeof m, _("Borrar %s de la tarjeta. No se puede deshacer."), s_files[s_del]);
    ui_confirm(m, _("Borrar"), del_yes);
}

static void open_cb(lv_event_t *e)
{
    (void)e;
    if (U->dirty) es_ui_project_save();
    lv_obj_t *c = ui_sheet(_("Abrir un proyecto"));
    int32_t w = lv_obj_get_width(c);
    DIR *d = opendir(es_dir());
    int n = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n < 40) {
            const char *nm = de->d_name;
            size_t l = strlen(nm);
            if (nm[0] == '.' || l < 6 || strcasecmp(nm + l - 5, ".json") || l >= sizeof s_files[0]) continue;
            snprintf(s_files[n], sizeof s_files[n], "%.47s", nm);
            lv_obj_t *row = ui_box(c, w, 60);
            char lb[ES_NAME + 8];
            snprintf(lb, sizeof lb, "%.*s", (int)(l - 5), nm);
            for (char *q = lb; *q; q++) if (*q == '_') *q = ' ';
            lv_obj_t *b = ui_btn(row, lb, aos_font_small, w - 76, 60, open_one_cb, (void *)(intptr_t)n);
            lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
            ui_btn_on(b, !strcmp(nm, U->file), C_CELL2);
            if (strcmp(nm, U->file)) {
                lv_obj_t *x = ui_btn(row, AOS_SYM_DELETE, &aos_sym_28, 64, 60, del_one_cb, (void *)(intptr_t)n);
                lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
            }
            n++;
        }
        closedir(d);
    }
    if (!n) ui_label(c, _("No hay proyectos en la tarjeta."), aos_font_caption, C_DIM);
}

/* ---- the export ---- */

static void stem(char *out, size_t n)
{
    snprintf(out, n, "%s", U->file);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static void loops_paint(void)
{
    if (g_loops) lv_label_set_text_fmt(g_loops, _("%d vueltas"), U->export_loops);
}

static void loops_minus(lv_event_t *e) { (void)e; if (U->export_loops > 1) U->export_loops--; loops_paint(); }
static void loops_plus(lv_event_t *e)  { (void)e; if (U->export_loops < 32) U->export_loops++; loops_paint(); }

static void wav_cb(lv_event_t *e)
{
    (void)e;
    if (es_export_busy()) { es_export_cancel(); return; }
    char s[ES_NAME + 8], f[ES_NAME + 16], path[320];
    stem(s, sizeof s);
    snprintf(f, sizeof f, "%s.wav", s);
    es_mkdirs();
    es_path(path, sizeof path, "export", f);
    if (!es_export_wav_start(U->p, path, U->pat, U->export_loops)) {
        aos_ui_toast(_("No se pudo empezar a exportar"), 1600);
        return;
    }
    g_exporting = true;
}

static void mid_cb(lv_event_t *e)
{
    (void)e;
    char s[ES_NAME + 8], f[ES_NAME + 16], path[320];
    stem(s, sizeof s);
    snprintf(f, sizeof f, "%s.mid", s);
    es_mkdirs();
    es_path(path, sizeof path, "export", f);
    char m[120];
    if (es_midi_export(U->p, path, U->pat, U->export_loops)) snprintf(m, sizeof m, _("Listo: estudio/export/%s"), f);
    else snprintf(m, sizeof m, "%s", _("No se pudo exportar el MIDI"));
    if (g_xlbl) lv_label_set_text(g_xlbl, m);
    aos_ui_toast(m, 2000);
}

/* ---- MIDI and audio ---- */

static void mout_cb(lv_event_t *e)
{
    U->mo_dest = (int)(intptr_t)lv_event_get_user_data(e);
    ui_seg_set(lv_obj_get_parent(lv_event_get_target_obj(e)), U->mo_dest, aos_theme_accent_rgb());
    es_ui_apply_settings();
}

static void clock_cb(lv_event_t *e)
{
    U->mo_clock = !U->mo_clock;
    ui_btn_on(lv_event_get_target_obj(e), U->mo_clock, aos_theme_accent_rgb());
    es_ui_apply_settings();
}

static void pc_paint(lv_obj_t *b)
{
    char t[48];
    if (U->mo_pc < ES_PARTS) snprintf(t, sizeof t, _("A la compu: parte %d"), U->mo_pc + 1);
    else if (U->mo_pc == ES_PARTS) snprintf(t, sizeof t, "%s", _("A la compu: batería"));
    else snprintf(t, sizeof t, "%s", _("A la compu: todo"));
    ui_btn_text(b, t);
}

static void pc_cb(lv_event_t *e)
{
    U->mo_pc = (U->mo_pc + 1) % (ES_PC_ALL + 1);
    pc_paint(lv_event_get_target_obj(e));
    es_ui_apply_settings();
}

static void lat_cb(lv_event_t *e)
{
    U->latency = (int)(intptr_t)lv_event_get_user_data(e);
    ui_seg_set(lv_obj_get_parent(lv_event_get_target_obj(e)), U->latency, aos_theme_accent_rgb());
    es_ui_apply_settings();
}

static void voices_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    U->voices = i ? 8 : 6;
    ui_seg_set(lv_obj_get_parent(lv_event_get_target_obj(e)), i, aos_theme_accent_rgb());
    es_ui_apply_settings();
}

static void test_cb(lv_event_t *e)
{
    (void)e;
    es_ui_load_test();
}

static void midi_paint(void)
{
    if (!g_midi) return;
    char names[4][48];
    int n = aos_hal_midi_devices(names, 4);
    char b[220];
    if (n <= 0) snprintf(b, sizeof b, "%s", _("Ningún aparato MIDI en el USB."));
    else {
        int o = snprintf(b, sizeof b, "%s ", _("Entrada:"));
        for (int i = 0; i < n && o < (int)sizeof b - 50; i++) o += snprintf(b + o, sizeof b - (size_t)o, "%s%.40s", i ? ", " : "", names[i]);
    }
    lv_label_set_text(g_midi, b);
}

static void refresh(void)
{
    static uint32_t last, lastm;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (g_exporting && g_prog) {
        float pr = es_export_progress();
        if (pr >= 2.0f || pr < 0.0f || !es_export_busy()) {
            g_exporting = false;
            lv_bar_set_value(g_prog, pr >= 2.0f ? 100 : 0, LV_ANIM_OFF);
            char s[ES_NAME + 8], m[120];
            stem(s, sizeof s);
            if (pr >= 2.0f) snprintf(m, sizeof m, _("Listo: estudio/export/%s.wav"), s);
            else snprintf(m, sizeof m, "%s", _("La exportación no terminó"));
            if (g_xlbl) lv_label_set_text(g_xlbl, m);
            aos_ui_toast(m, 2000);
        } else {
            lv_bar_set_value(g_prog, (int32_t)(pr * 100.0f), LV_ANIM_OFF);
            if (g_xlbl) lv_label_set_text_fmt(g_xlbl, _("Exportando… %d %% (tocá WAV para cortar)"), (int)(pr * 100.0f));
        }
    }
    if (now - last >= 250) {
        last = now;
        pats_paint();
        static int lastpos = -2;
        int pos = es_playing(U->e) && U->p->song_mode ? es_chain_pos(U->e) : -1;
        if (pos != lastpos) { lastpos = pos; chain_build(); }
    }
    if (g_stats && now - lastm >= 700) {
        lastm = now;
        es_stats_t s;
        es_stats(U->e, &s);
        lv_label_set_text_fmt(g_stats, _("Bloque de %d muestras (%d us): promedio %d us, pico %d us, %d %% del tiempo. Voces: %d. Faltas: %u. Cola: %d muestras."),
                              ES_BLK, ES_BLK * 1000000 / ES_SR, (int)s.blk_us_avg, (int)s.blk_us_max, (int)s.load_pct,
                              s.voices, (unsigned)s.underruns, s.queued);
        midi_paint();
    }
}

static void forget(void)
{
    g_pats = g_chain = g_name = g_prog = g_xlbl = g_loops = g_stats = g_midi = g_mode = NULL;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *t, int32_t w)
{
    lv_obj_t *l = ui_label(parent, t, aos_font_caption, C_DIM);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, w);
    return l;
}

void es_view_song(lv_obj_t *body)
{
    int32_t W = lv_obj_get_width(body), H = lv_obj_get_height(body);
    lv_obj_t *sc = lv_obj_create(body);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, W, H);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_left(sc, 12, 0);
    lv_obj_set_style_pad_top(sc, 8, 0);
    lv_obj_set_style_pad_bottom(sc, 60, 0);
    lv_obj_set_style_pad_column(sc, 12, 0);
    lv_obj_set_style_pad_row(sc, 12, 0);
    lv_obj_add_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    int32_t cw = col_w(), iw = cw - 28;
    es_proj_t *p = U->p;

    /* the two columns lying down: the music on the left, the rest right */
    lv_obj_t *left = ui_col(sc, cw, 12), *right = U->land ? ui_col(sc, cw, 12) : left;

    lv_obj_t *k = ui_card(left, _("Patrones"), cw);
    g_pats = ui_row(k, iw, 8);
    int32_t pw = (iw - 3 * 8) / 4;
    for (int i = 0; i < ES_PATS; i++) {
        lv_obj_t *b = ui_btn(g_pats, "", aos_font_small, pw, 84, pat_cb, (void *)(intptr_t)i);
        lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_CENTER, 0);
    }
    pats_paint();
    lv_obj_t *r = ui_row(k, iw, 8);
    int32_t bw = (iw - 16) / 3;
    ui_btn(r, _("Copiar"), aos_font_caption, bw, 54, copy_cb, NULL);
    ui_btn(r, _("Pegar"), aos_font_caption, bw, 54, paste_cb, NULL);
    ui_btn(r, _("Borrar"), aos_font_caption, bw, 54, clear_cb, NULL);

    k = ui_card(left, _("Canción"), cw);
    static const char *MODES[2];
    MODES[0] = _("Un patrón"); MODES[1] = _("La canción");
    g_mode = ui_seg(k, MODES, 2, p->song_mode, iw, 54, mode_cb, aos_theme_accent_rgb());
    g_chain = ui_row(k, iw, 6);
    chain_build();
    wrap_label(k, _("Tocá un casillero para cambiarle el patrón; mantenelo apretado para sacarlo."), iw);
    lv_obj_t *add = ui_row(k, iw, 6);
    for (int i = 0; i < ES_PATS; i++) {
        char b[4];
        snprintf(b, sizeof b, "+%c", 'A' + i);
        ui_btn(add, b, aos_font_caption, (iw - 7 * 6 - 70) / 8, 52, add_cb, (void *)(intptr_t)i);
    }
    ui_btn(add, AOS_SYM_BACKSPACE_OUTLINE, &aos_sym_28, 64, 52, chain_back_cb, NULL);

    k = ui_card(right, _("Proyecto"), cw);
    g_name = ui_label(k, "", aos_font_body, C_TEXT);
    lv_label_set_long_mode(g_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_name, iw);
    name_paint();
    r = ui_row(k, iw, 8);
    ui_btn(r, _("Nuevo"), aos_font_caption, bw, 54, new_cb, NULL);
    ui_btn(r, _("Abrir…"), aos_font_caption, bw, 54, open_cb, NULL);
    ui_btn(r, _("Guardar como…"), aos_font_caption, bw, 54, save_as_cb, NULL);
    wrap_label(k, _("Se guarda solo en estudio/ de la tarjeta."), iw);

    k = ui_card(right, _("Exportar"), cw);
    wrap_label(k, _("Con La canción, la cadena entera; con Un patrón, el patrón elegido las vueltas que digas."), iw);
    r = ui_row(k, iw, 8);
    ui_btn(r, "WAV", aos_font_small, bw, 54, wav_cb, NULL);
    ui_btn(r, "MIDI", aos_font_small, bw, 54, mid_cb, NULL);
    ui_stepper(r, "", bw, 54, loops_minus, loops_plus, &g_loops);
    lv_obj_set_style_text_font(g_loops, aos_font_caption, 0);
    loops_paint();
    g_prog = lv_bar_create(k);
    lv_obj_set_size(g_prog, iw, 12);
    lv_bar_set_range(g_prog, 0, 100);
    g_xlbl = wrap_label(k, "", iw);

    k = ui_card(right, "MIDI (USB)", cw);
    g_midi = wrap_label(k, "", iw);
    static const char *OUTS[4];
    OUTS[0] = _("Sin salida"); OUTS[1] = _("Compu"); OUTS[2] = _("Aparato"); OUTS[3] = _("Las dos");
    ui_label(k, _("Salida"), aos_font_caption, C_TEXT);
    ui_seg(k, OUTS, 4, U->mo_dest, iw, 54, mout_cb, aos_theme_accent_rgb());
    r = ui_row(k, iw, 8);
    lv_obj_t *cb = ui_btn(r, _("Reloj"), aos_font_caption, (iw - 8) / 3, 54, clock_cb, NULL);
    ui_btn_on(cb, U->mo_clock, aos_theme_accent_rgb());
    lv_obj_t *pcb = ui_btn(r, "", aos_font_caption, (iw - 8) * 2 / 3, 54, pc_cb, NULL);
    pc_paint(pcb);
    wrap_label(k, _("Entra por el canal 1 la parte elegida, por el 2 al 4 las partes 1 a 3 y por el 10 la batería. Al aparato salen las partes por 1 a 3, la batería por 10 y el reloj; a la compu, lo mismo, o una sola pista por el canal 1."), iw);

    k = ui_card(right, _("Audio"), cw);
    static const char *LAT[3];
    static char lat_txt[3][32];
    static const char *const LATN[3] = { N_("Baja"), N_("Normal"), N_("Segura") };
    for (int i = 0; i < 3; i++) {
        snprintf(lat_txt[i], sizeof lat_txt[i], "%s %d ms", _(LATN[i]), es_latency_ms(i));
        LAT[i] = lat_txt[i];
    }
    ui_label(k, _("Latencia (lo que espera en la cola)"), aos_font_caption, C_TEXT);
    ui_seg(k, LAT, 3, U->latency, iw, 54, lat_cb, aos_theme_accent_rgb());
    static const char *VOI[2] = { "6", "8" };
    ui_label(k, _("Voces del sintetizador"), aos_font_caption, C_TEXT);
    ui_seg(k, VOI, 2, U->voices >= 8, iw, 54, voices_cb, aos_theme_accent_rgb());
    ui_btn(k, _("Prueba de carga"), aos_font_caption, iw, 54, test_cb, NULL);
    g_stats = wrap_label(k, "", iw);

    U->refresh = refresh;
    U->forget = forget;
    g_exporting = es_export_busy();
    refresh();
}
