/*
 * P4OS - Luces: Escenas, Patch and Salida (Escenario is lz_stage.c).
 */
#include "lz_ui.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

/* ======================================================================== */
/* Escenas                                                                   */
/* ======================================================================== */

static const uint16_t FADE_MS[6] = { 0, 500, 1000, 2000, 3000, 5000 };
static const uint8_t CFADE[3] = { 0, 50, 100 };
static const char *const CFADE_NAME[3] = { N_("Corte"), N_("Mitad"), N_("Todo fundido") };

void lz_chase_set(bool on)
{
    LZ.chase = on && LZ.nscenes > 0;
    if (LZ.chase && LZ.bpm) LZ.chase_t = now_ms() - 60000u / LZ.bpm;      /* the first step now */
}

static void store_done(const char *name)
{
    lz_scene_store(name);
    lz_rebuild();
}

static void store_cb(lv_event_t *e)
{
    (void)e;
    char n[24];
    snprintf(n, sizeof n, _("Escena %d"), LZ.nscenes + 1);
    if (LZ.nscenes >= LZ_SCENES) return;
    lz_text(_("Nombre de la escena"), n, store_done);
}

static void scene_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lz_scene_call(i);
    lz_scenes_refresh();
}

static void rerecord_cb(lv_event_t *e)
{
    (void)e;
    int i = LZU.edit;
    if (i < 0 || i >= LZ.nscenes) return;
    memcpy(LZ.scenes[i].look, LZ.look, sizeof LZ.look);
    lz_scenes_save();
    LZ.cur_scene = i;
    lz_overlay_close();
    lz_scenes_refresh();
}

static void rename_done(const char *name)
{
    int i = LZU.edit;
    if (i < 0 || i >= LZ.nscenes || !name[0]) return;
    snprintf(LZ.scenes[i].name, sizeof LZ.scenes[i].name, "%.23s", name);
    lz_scenes_save();
    LZ.changed++;
}

static void rename_cb(lv_event_t *e) { (void)e; if (LZU.edit < LZ.nscenes) lz_text(_("Nombre de la escena"), LZ.scenes[LZU.edit].name, rename_done); }

static void delete_scene_cb(lv_event_t *e)
{
    (void)e;
    lz_scene_delete(LZU.edit);
    if (!LZ.nscenes) LZ.chase = false;
    lz_overlay_close();
    lz_rebuild();
}

static void scene_fade_cb(lv_event_t *e)
{
    int i = LZU.edit;
    if (i < 0 || i >= LZ.nscenes) return;
    LZ.scenes[i].fade_ms = FADE_MS[(int)(intptr_t)lv_event_get_user_data(e)];
    lz_scenes_save();
    lz_overlay_close();
    lz_rebuild();
}

static void scene_menu_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    LZU.edit = i;
    lv_obj_t *c = lz_sheet(LZ.scenes[i].name);
    lz_section(c, _("SU FUNDIDO AL LLAMARLA"));
    lv_obj_t *fr = lz_chips_row(c, LZU.W - 2 * AOS_UI_PAD - 40);
    for (int k = 0; k < 6; k++) {
        char t[16];
        if (FADE_MS[k] == 0) snprintf(t, sizeof t, "%s", _("Corte"));
        else if (FADE_MS[k] % 1000) snprintf(t, sizeof t, "%u,%u s", FADE_MS[k] / 1000, FADE_MS[k] % 1000 / 100);
        else snprintf(t, sizeof t, "%u s", FADE_MS[k] / 1000);
        lz_chip(fr, t, LZ.scenes[i].fade_ms == FADE_MS[k], scene_fade_cb, (void *)(intptr_t)k);
    }
    lz_pill(c, AOS_SYM_RECORD, _("Volver a grabar con lo que hay ahora"), LZ_ACCENT, rerecord_cb, NULL);
    lz_pill(c, AOS_SYM_PENCIL, _("Cambiar el nombre"), AOS_C_CARD2, rename_cb, NULL);
    lz_pill(c, AOS_SYM_DELETE, _("Borrar"), AOS_C_RED, delete_scene_cb, NULL);
}

static void fade_cb(lv_event_t *e)
{
    LZ.scene_fade_ms = FADE_MS[(int)(intptr_t)lv_event_get_user_data(e)];
    lz_save();
    lz_scenes_refresh();
}

static void chase_cb(lv_event_t *e) { (void)e; lz_chase_set(!LZ.chase); lz_scenes_refresh(); }
static void demo_cb(lv_event_t *e) { (void)e; lz_scenes_demo(); lz_rebuild(); }
static void tap_cb(lv_event_t *e) { (void)e; lz_tap(); lz_save(); lz_scenes_refresh(); }

static void bpm_done(int v)
{
    if (v >= 20 && v <= 300) LZ.bpm = (uint16_t)v;
    lz_save();
    lz_scenes_refresh();
}

static void bpm_cb(lv_event_t *e) { (void)e; lz_keypad(_("Tiempo (golpes por minuto)"), LZ.bpm, bpm_done); }

static void cfade_cb(lv_event_t *e)
{
    LZ.chase_fade = CFADE[(int)(intptr_t)lv_event_get_user_data(e)];
    lz_save();
    lz_scenes_refresh();
}

void lz_scenes_build(lv_obj_t *parent, int32_t ch)
{
    lz_page_t *P = &LZU.pg;
    int32_t w = LZU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = lz_column(parent, w, ch);

    lv_obj_t *top = lz_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(top, 10, 0);
    lz_pill(top, AOS_SYM_PLUS, _("Guardar lo que hay"), LZ_ACCENT, store_cb, NULL);
    lz_pill(top, AOS_SYM_AUTO_FIX, _("Escenas de muestra"), AOS_C_CARD2, demo_cb, NULL);
    lz_section(col, _("FUNDIDO DE LAS ESCENAS NUEVAS (CADA UNA GUARDA EL SUYO)"));
    lv_obj_t *r = lz_chips_row(col, w);
    for (int k = 0; k < 6; k++) {
        char t[16];
        if (FADE_MS[k] == 0) snprintf(t, sizeof t, "%s", _("Corte"));
        else if (FADE_MS[k] % 1000) snprintf(t, sizeof t, "%u,%u s", FADE_MS[k] / 1000, FADE_MS[k] % 1000 / 100);
        else snprintf(t, sizeof t, "%u s", FADE_MS[k] / 1000);
        P->fade_chip[k] = lz_chip(r, t, false, fade_cb, (void *)(intptr_t)k);
    }

    lz_section(col, _("ESCENAS · MANTENÉ UNA PARA GRABARLA O BORRARLA"));
    if (!LZ.nscenes) lz_caption(col, _("Todavía no hay escenas. Armá un look en Escenario (intensidad, color, posición) y guardalo acá."), w);
    int cols = LZU.land ? 5 : 3;
    int32_t tw = (w - (cols - 1) * 10) / cols;
    lv_obj_t *g = lz_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, 10, 0);
    for (int i = 0; i < LZ.nscenes; i++) {
        lv_obj_t *t = lz_card(g, tw, 110);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(t, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_add_event_cb(t, scene_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, scene_menu_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        char n[8];
        snprintf(n, sizeof n, "%d", i + 1);
        lv_obj_align(aos_label(t, n, aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 14, 10);
        char fd[16];
        uint16_t fm = LZ.scenes[i].fade_ms;
        if (!fm) snprintf(fd, sizeof fd, "%s", _("corte"));
        else if (fm % 1000) snprintf(fd, sizeof fd, "%u,%u s", fm / 1000, fm % 1000 / 100);
        else snprintf(fd, sizeof fd, "%u s", fm / 1000);
        lv_obj_align(aos_label(t, fd, aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_RIGHT, -14, 10);
        lv_obj_t *l = aos_label(t, LZ.scenes[i].name, aos_font_small, AOS_C_TEXT);
        lv_obj_set_width(l, tw - 28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 14, -14);
        P->scene[i] = t;
    }

    lz_section(col, _("SECUENCIA: UNA ESCENA TRAS OTRA, AL TIEMPO"));
    lv_obj_t *c = lz_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_t *row = lz_box(c, w - 36, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    P->chase_btn = lz_pill(row, AOS_SYM_PLAY, _("Andar"), LZ_ACCENT, chase_cb, NULL);
    lv_obj_t *bb = lz_box(row, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_pad_hor(bb, 16, 0);
    lv_obj_set_style_radius(bb, 20, 0);
    lv_obj_set_style_bg_color(bb, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_add_flag(bb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bb, bpm_cb, LV_EVENT_CLICKED, NULL);
    P->bpm_l = aos_label(bb, "", aos_font_body, AOS_C_TEXT);
    lv_obj_center(P->bpm_l);
    lz_pill(row, AOS_SYM_GESTURE_TAP_BUTTON, "Tap", AOS_C_CARD2, tap_cb, NULL);
    r = lz_chips_row(c, w - 36);
    for (int k = 0; k < 3; k++) P->cfade_chip[k] = lz_chip(r, aos_tr(CFADE_NAME[k]), false, cfade_cb, (void *)(intptr_t)k);
    if (LZ.nscenes < 2) {
        lv_obj_t *wl = lz_caption(c, _("Hacen falta al menos dos escenas distintas: con una sola, la secuencia llama siempre a la misma. \"Escenas de muestra\" agrega cinco."), w - 36);
        lv_obj_set_style_text_color(wl, AOS_C_ORANGE, 0);
    }
    lz_caption(c, _("Pasa por todas las escenas en orden, una por golpe. Tap: tocalo al ritmo de la música, tres o cuatro veces. Corte, mitad o todo fundido: cuánto de cada paso es fundido."), w - 36);
    lz_scenes_refresh();
}

void lz_scenes_refresh(void)
{
    lz_page_t *P = &LZU.pg;
    if (!P->bpm_l) return;
    for (int i = 0; i < LZ.nscenes; i++) {
        if (!P->scene[i]) continue;
        bool cur = i == LZ.cur_scene;
        lv_obj_set_style_bg_color(P->scene[i], cur ? LZ_ACCENT : AOS_C_CARD, 0);
    }
    for (int k = 0; k < 6; k++) lz_chip_set(P->fade_chip[k], LZ.scene_fade_ms == FADE_MS[k]);
    for (int k = 0; k < 3; k++) lz_chip_set(P->cfade_chip[k], LZ.chase_fade == CFADE[k]);
    lv_label_set_text_fmt(P->bpm_l, "%u BPM", LZ.bpm);
    lv_obj_set_style_bg_color(P->chase_btn, LZ.chase ? AOS_C_RED : LZ_ACCENT, 0);
    lv_label_set_text(lv_obj_get_child(P->chase_btn, 0), LZ.chase ? AOS_SYM_STOP : AOS_SYM_PLAY);
    lv_label_set_text(lv_obj_get_child(P->chase_btn, 1), LZ.chase ? _("Parar") : _("Andar"));
}

/* ======================================================================== */
/* Patch                                                                     */
/* ======================================================================== */

static void patch_changed(void)
{
    lz_patch_save();
    lz_scenes_save();
    LZ.changed++;
}

static void add_type_cb(lv_event_t *e);
static void add_n_cb(lv_event_t *e);
static void add_univ_cb(lv_event_t *e);
static void add_go_cb(lv_event_t *e);

static void add_sheet(void)
{
    lv_obj_t *c = lz_sheet(_("Agregar aparatos"));
    int32_t w = LZU.W - 2 * AOS_UI_PAD - 40;
    lz_section(c, _("TIPO"));
    lv_obj_t *r = lz_chips_row(c, w);
    for (int t = 0; t < LZ.ntypes; t++) {
        char s[56];
        snprintf(s, sizeof s, "%.40s · %u", LZ.types[t].name, LZ.types[t].n);
        lz_chip(r, s, t == LZU.add_type, add_type_cb, (void *)(intptr_t)t);
    }
    lz_section(c, _("CUÁNTOS"));
    r = lz_chips_row(c, w);
    static const int N[] = { 1, 2, 4, 8, 12, 16 };
    for (int k = 0; k < 6; k++) {
        char s[8];
        snprintf(s, sizeof s, "%d", N[k]);
        lz_chip(r, s, N[k] == LZU.add_n, add_n_cb, (void *)(intptr_t)N[k]);
    }
    lz_section(c, _("UNIVERSO (EL 1 SALE POR RS485; LOS DEMÁS, SÓLO POR ART-NET)"));
    r = lz_chips_row(c, w);
    for (int u = 0; u < LZ_UNIV; u++) {
        char s[8];
        snprintf(s, sizeof s, "U%d", u + 1);
        lz_chip(r, s, u == LZU.add_univ, add_univ_cb, (void *)(intptr_t)u);
    }
    const lz_type_t *t = &LZ.types[LZU.add_type];
    int a = lz_next_free(LZU.add_univ, t->n);
    char s[120];
    if (a > 0) snprintf(s, sizeof s, _("Desde la dirección %d, uno tras otro, %u canales cada uno."), a, t->n);
    else snprintf(s, sizeof s, "%s", _("No quedan canales libres para ese tipo."));
    lz_caption(c, s, w);
    lz_pill(c, AOS_SYM_PLUS, _("Agregar"), LZ_ACCENT, add_go_cb, NULL);
}

static void add_type_cb(lv_event_t *e) { LZU.add_type = (int)(intptr_t)lv_event_get_user_data(e); add_sheet(); }
static void add_n_cb(lv_event_t *e) { LZU.add_n = (int)(intptr_t)lv_event_get_user_data(e); add_sheet(); }
static void add_univ_cb(lv_event_t *e) { LZU.add_univ = (int)(intptr_t)lv_event_get_user_data(e); add_sheet(); }

static void add_go_cb(lv_event_t *e)
{
    (void)e;
    const lz_type_t *t = &LZ.types[LZU.add_type];
    /* "PAR RGB de 7 canales (x)" -> "PAR RGB de 7 canales", then a number */
    char base[24];
    snprintf(base, sizeof base, "%.20s", t->name);
    char *p = strstr(base, " (");
    if (p) *p = 0;
    int have = 0;
    for (int i = 0; i < LZ.nfix; i++) have += LZ.fix[i].type == LZU.add_type;
    for (int k = 0; k < LZU.add_n; k++) {
        int a = lz_next_free(LZU.add_univ, t->n);
        if (a < 0) break;
        char n[24];
        snprintf(n, sizeof n, "%.18s %u", base, (unsigned)(uint8_t)(have + k + 1));
        int fi = lz_fix_add(LZU.add_type, (uint16_t)a, n);
        if (fi < 0) break;
        LZ.fix[fi].univ = (uint8_t)LZU.add_univ;
    }
    patch_changed();
    lz_overlay_close();
    lz_rebuild();
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    if (LZU.add_type < 0 || LZU.add_type >= LZ.ntypes) LZU.add_type = 1;
    if (LZU.add_n < 1) LZU.add_n = 1;
    add_sheet();
}

static void fix_sheet(void);

static void fix_name_done(const char *v)
{
    if (LZU.edit < LZ.nfix && v[0]) snprintf(LZ.fix[LZU.edit].name, sizeof LZ.fix[LZU.edit].name, "%.23s", v);
    patch_changed();
    lz_rebuild();
}

static void fix_name_cb(lv_event_t *e) { (void)e; lz_text(_("Nombre del aparato"), LZ.fix[LZU.edit].name, fix_name_done); }

static void fix_addr_done(int v)
{
    if (LZU.edit < LZ.nfix && v >= 1 && v <= 512) LZ.fix[LZU.edit].addr = (uint16_t)v;
    patch_changed();
    lz_rebuild();
}

static void fix_addr_cb(lv_event_t *e) { (void)e; lz_keypad(_("Dirección DMX (1 a 512)"), LZ.fix[LZU.edit].addr, fix_addr_done); }

static void fix_type_cb(lv_event_t *e)
{
    LZ.fix[LZU.edit].type = (int)(intptr_t)lv_event_get_user_data(e);
    patch_changed();
    fix_sheet();
}

static void fix_univ_cb(lv_event_t *e)
{
    LZ.fix[LZU.edit].univ = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    patch_changed();
    fix_sheet();
}

static void fix_del_cb(lv_event_t *e)
{
    (void)e;
    lz_fix_remove(LZU.edit);
    patch_changed();
    lz_overlay_close();
    lz_rebuild();
}

static void fix_sheet(void)
{
    int i = LZU.edit;
    const lz_fix_t *f = &LZ.fix[i];
    lv_obj_t *c = lz_sheet(f->name);
    int32_t w = LZU.W - 2 * AOS_UI_PAD - 40;
    lv_obj_t *r = lz_chips_row(c, w);
    lz_pill(r, AOS_SYM_PENCIL, _("Nombre"), AOS_C_CARD2, fix_name_cb, NULL);
    char a[32];
    snprintf(a, sizeof a, _("Dirección %u"), f->addr);
    lz_pill(r, AOS_SYM_POUND, a, AOS_C_CARD2, fix_addr_cb, NULL);
    lz_section(c, _("TIPO"));
    r = lz_chips_row(c, w);
    for (int t = 0; t < LZ.ntypes; t++) {
        char s[56];
        snprintf(s, sizeof s, "%.40s · %u", LZ.types[t].name, LZ.types[t].n);
        lz_chip(r, s, t == f->type, fix_type_cb, (void *)(intptr_t)t);
    }
    lz_section(c, _("UNIVERSO"));
    r = lz_chips_row(c, w);
    for (int u = 0; u < LZ_UNIV; u++) {
        char s[8];
        snprintf(s, sizeof s, "U%d", u + 1);
        lz_chip(r, s, u == f->univ, fix_univ_cb, (void *)(intptr_t)u);
    }
    const lz_type_t *t = &LZ.types[f->type];
    char s[200];
    size_t k = (size_t)snprintf(s, sizeof s, _("Canales %u a %u: "), f->addr, f->addr + t->n - 1);
    for (int ch = 0; ch < t->n && k < sizeof s - 24; ch++) {
        if (t->attr[ch] == LZ_A_CONST) k += (size_t)snprintf(s + k, sizeof s - k, "%s%u", ch ? ", " : "", t->def[ch]);
        else k += (size_t)snprintf(s + k, sizeof s - k, "%s%s", ch ? ", " : "", lz_attr_name(t->attr[ch]));
    }
    lz_caption(c, s, w);
    if (lz_fix_overlaps(i)) {
        lv_obj_t *wl = lz_caption(c, _("Se pisa con otro aparato o se pasa del canal 512."), w);
        lv_obj_set_style_text_color(wl, AOS_C_ORANGE, 0);
    }
    lz_pill(c, AOS_SYM_DELETE, _("Sacar del patch"), AOS_C_RED, fix_del_cb, NULL);
}

static void fix_cb(lv_event_t *e)
{
    LZU.edit = (int)(intptr_t)lv_event_get_user_data(e);
    if (LZU.edit < LZ.nfix) fix_sheet();
}

static void reload_cb(lv_event_t *e)
{
    (void)e;
    if (lz_types_reload()) snprintf(LZU.err, sizeof LZU.err, "%s", _("faltan tipos: esos aparatos quedaron como dimmer"));
    lz_rebuild();
}

void lz_patch_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = LZU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = lz_column(parent, w, ch);
    lv_obj_t *top = lz_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(top, 10, 0);
    lz_pill(top, AOS_SYM_PLUS, _("Agregar aparatos"), LZ_ACCENT, add_cb, NULL);
    int used = 0;
    for (int i = 0; i < LZ.nfix; i++) used += LZ.types[LZ.fix[i].type].n;
    char s[96];
    snprintf(s, sizeof s, _("%d aparatos, %d canales de 512"), LZ.nfix, used);
    lz_section(col, s);
    for (int i = 0; i < LZ.nfix; i++) {
        const lz_fix_t *f = &LZ.fix[i];
        const lz_type_t *t = &LZ.types[f->type];
        lv_obj_t *c = lz_card(col, w, 96);
        lv_obj_set_style_pad_hor(c, 20, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, fix_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *n = aos_label(c, f->name, aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(n, w - 260);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 14);
        lv_obj_t *d = aos_label(c, t->name, aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(d, w - 260);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 0, -14);
        bool bad = lz_fix_overlaps(i);
        char a[32], u[6] = "";
        if (f->univ) snprintf(u, sizeof u, "U%u ", f->univ + 1);
        if (t->n > 1) snprintf(a, sizeof a, "%s%u-%u", u, f->addr, f->addr + t->n - 1);
        else snprintf(a, sizeof a, "%s%u", u, f->addr);
        lv_obj_align(aos_label(c, a, aos_font_title, bad ? AOS_C_ORANGE : LZ_ACCENT), LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lz_section(col, _("TIPOS DE APARATO"));
    for (int k = 0; k < LZ.ntypes; k++) {
        const lz_type_t *t = &LZ.types[k];
        lv_obj_t *c = lz_card(col, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 6, 0);
        char h[96];
        snprintf(h, sizeof h, "%.40s · %u %s", t->name, t->n, t->file[0] ? t->file : _("incluido"));
        aos_label(c, h, aos_font_small, AOS_C_TEXT);
        char cs[200];
        size_t q = 0;
        cs[0] = 0;
        for (int i = 0; i < t->n && q < sizeof cs - 24; i++) {
            if (t->attr[i] == LZ_A_CONST) q += (size_t)snprintf(cs + q, sizeof cs - q, "%s%u", i ? ", " : "", t->def[i]);
            else q += (size_t)snprintf(cs + q, sizeof cs - q, "%s%s", i ? ", " : "", lz_attr_name(t->attr[i]));
        }
        lz_caption(c, cs, w - 32);
    }
    lz_pill(col, AOS_SYM_RESTART, _("Volver a leer los tipos de la tarjeta"), AOS_C_CARD2, reload_cb, NULL);
    char p[200];
    snprintf(p, sizeof p, _("Un tipo nuevo es un archivo .csv en %s/aparatos, con una línea nombre y una línea canales (ahí hay un ejemplo). Desde el portal también se pueden escribir."), lz_dir());
    lz_caption(col, p, w);
}

/* ======================================================================== */
/* Salida                                                                    */
/* ======================================================================== */

static int s_pick = 1;          /* the channel tapped on the map */

static int fixture_of(int chn, int *k)
{
    for (int i = 0; i < LZ.nfix; i++) {
        if (LZ.fix[i].univ != LZU.out_univ) continue;
        int a = LZ.fix[i].addr, n = LZ.types[LZ.fix[i].type].n;
        if (chn >= a && chn < a + n) { if (k) *k = chn - a; return i; }
    }
    return -1;
}

static void out_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t cw = lv_area_get_width(&a) / 32, chh = lv_area_get_height(&a) / 16;
    static bool patched[513];
    memset(patched, 0, sizeof patched);
    for (int i = 0; i < LZ.nfix; i++) {
        if (LZ.fix[i].univ != LZU.out_univ) continue;
        int b = LZ.fix[i].addr, n = LZ.types[LZ.fix[i].type].n;
        for (int c = b; c < b + n && c <= 512; c++) patched[c] = true;
    }
    const uint8_t *uni = LZ.uni[LZU.out_univ];
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 2;
    for (int i = 0; i < 512; i++) {
        int chn = i + 1, v = uni[chn];
        if (!patched[chn]) d.bg_color = lv_color_hex(0x161618);
        else d.bg_color = lv_color_make((uint8_t)(40 + v * 196 / 255), (uint8_t)(30 + v * 42 / 255), (uint8_t)(50 + v * 103 / 255));
        d.border_width = chn == s_pick ? 2 : 0;
        d.border_color = lv_color_white();
        d.border_opa = LV_OPA_COVER;
        int32_t x = a.x1 + (i % 32) * cw, y = a.y1 + (i / 32) * chh;
        lv_area_t r = { x + 1, y + 1, x + cw - 2, y + chh - 2 };
        lv_draw_rect(layer, &d, &r);
    }
}

static void out_click_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int col = (p.x - a.x1) * 32 / lv_area_get_width(&a), row = (p.y - a.y1) * 16 / lv_area_get_height(&a);
    if (col < 0 || col > 31 || row < 0 || row > 15) return;
    s_pick = row * 32 + col + 1;
    lz_out_refresh();
}

static void univ_view_cb(lv_event_t *e)
{
    LZU.out_univ = (int)(intptr_t)lv_event_get_user_data(e);
    lz_rebuild();
}

static void an_on_cb(lv_event_t *e)
{
    LZN.on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lz_save();
    lz_out_refresh();
}

static void an_base_done(int v)
{
    if (v >= 0 && v < 32768) LZN.base = (uint16_t)v;
    lz_save();
    lz_rebuild();
}

static void an_base_cb(lv_event_t *e) { (void)e; lz_keypad(_("Universo Art-Net del U1 (0 a 32767)"), LZN.base, an_base_done); }

static void an_dest_done(const char *v)
{
    snprintf(LZN.dest, sizeof LZN.dest, "%.39s", v);
    lz_save();
    lz_rebuild();
}

static void an_dest_cb(lv_event_t *e)
{
    if ((int)(intptr_t)lv_event_get_user_data(e) == 0) { LZN.dest[0] = 0; lz_save(); lz_rebuild(); return; }
    lz_text(_("Dirección IP del nodo Art-Net"), LZN.dest, an_dest_done);
}

static void port_cb(lv_event_t *e)
{
    lz_set_port((const char *)lv_event_get_user_data(e));
    lz_rebuild();
}

void lz_out_build(lv_obj_t *parent, int32_t ch)
{
    lz_page_t *P = &LZU.pg;
    int32_t w = LZU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = lz_column(parent, w, ch);
    lz_section(col, _("PUERTO"));
    lv_obj_t *r = lz_chips_row(col, w);
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind != AOS_PORT_UART) continue;
        char t[40];
        snprintf(t, sizeof t, p->pins[2] >= 0 ? "%s · DE %d" : "%s", p->name, p->pins[2]);
        lz_chip(r, t, !strcmp(p->name, LZU.port), port_cb, (void *)p->name);
    }
    lv_obj_t *c = lz_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    P->out_stats = aos_label(c, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(P->out_stats, w - 36);
    lv_label_set_long_mode(P->out_stats, LV_LABEL_LONG_MODE_WRAP);
    /* Art-Net */
    lz_section(col, "ART-NET");
    lv_obj_t *ac = lz_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(ac, 18, 0);
    lv_obj_set_flex_flow(ac, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ac, 12, 0);
    lv_obj_t *ar = lz_box(ac, w - 36, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(ar, 60, 0);
    lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ar, 14, 0);
    lv_obj_t *swl = aos_label(ar, _("Mandar por la red"), aos_font_body, AOS_C_TEXT);
    lv_obj_set_flex_grow(swl, 1);
    lv_label_set_long_mode(swl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *sw = lv_switch_create(ar);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_set_style_bg_color(sw, LZ_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (LZN.on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, an_on_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = lz_chips_row(ac, w - 36);
    lz_chip(r, _("A toda la red"), !LZN.dest[0], an_dest_cb, (void *)0);
    char dt[56];
    snprintf(dt, sizeof dt, LZN.dest[0] ? "%s" : "%s", LZN.dest[0] ? LZN.dest : _("A un nodo…"));
    lz_chip(r, dt, LZN.dest[0] != 0, an_dest_cb, (void *)1);
    char bt[40];
    snprintf(bt, sizeof bt, _("U1 = universo %u"), LZN.base);
    lz_chip(r, bt, false, an_base_cb, NULL);
    P->an_stats = aos_label(ac, "", aos_font_caption, AOS_C_DIM);
    lz_caption(ac, _("Cada universo con algún aparato sale 40 veces por segundo como Art-Net (UDP 6454): a toda la red, o a la IP de un nodo. U1 lleva el universo elegido, U2 el siguiente, y así. Para un nodo Art-Net a DMX, consolas y programas de la PC."), w - 36);

    lz_section(col, _("LOS 512 CANALES COMO SALEN (OSCUROS: SIN APARATO)"));
    r = lz_chips_row(col, w);
    for (int u = 0; u < LZ_UNIV; u++) {
        char s[16];
        snprintf(s, sizeof s, "U%d%s", u + 1, LZ.used_univ >> u & 1 ? " ·" : "");
        lz_chip(r, s, u == LZU.out_univ, univ_view_cb, (void *)(intptr_t)u);
    }
    P->out_map = lz_card(col, w, (w / 32) * 16);
    lv_obj_add_flag(P->out_map, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(P->out_map, out_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(P->out_map, out_click_cb, LV_EVENT_CLICKED, NULL);
    lz_caption(col, _("La salida arranca cerrada cada vez que se abre la app: ▶ arriba la abre. Sigue saliendo con la app en segundo plano. 40 paquetes por segundo, 512 canales, BREAK de 176 µs. Para ver qué llega del otro lado, la app DMX en Analizador."), w);
    lz_out_refresh();
}

void lz_out_refresh(void)
{
    lz_page_t *P = &LZU.pg;
    if (!P->out_map) return;
    lv_obj_invalidate(P->out_map);
    char t[240];
    int k = 0, i = fixture_of(s_pick, &k);
    size_t n = (size_t)snprintf(t, sizeof t, _("Canal %d: %u"), s_pick, LZ.uni[LZU.out_univ][s_pick]);
    if (i >= 0) {
        const lz_type_t *ty = &LZ.types[LZ.fix[i].type];
        n += (size_t)snprintf(t + n, sizeof t - n, " · %s, %s", LZ.fix[i].name, ty->attr[k] == LZ_A_CONST ? _("fijo") : lz_attr_name(ty->attr[k]));
    } else n += (size_t)snprintf(t + n, sizeof t - n, " · %s", _("sin aparato"));
    if (LZU.h)
        snprintf(t + n, sizeof t - n, _("\nSaliendo por %s: %.1f paquetes/s, %lu mandados."), LZU.port, (double)LZU.st.hz, (unsigned long)LZU.st.packets);
    else snprintf(t + n, sizeof t - n, _("\nSalida cerrada (%s)."), LZU.port);
    lv_label_set_text(P->out_stats, t);
    if (P->an_stats) {
        if (!LZN.on) lv_label_set_text(P->an_stats, _("Apagado."));
        else if (LZN.err[0]) lv_label_set_text(P->an_stats, LZN.err);
        else lv_label_set_text_fmt(P->an_stats, _("%lu paquetes mandados a %s"), (unsigned long)LZN.packets, LZN.dest[0] ? LZN.dest : "255.255.255.255");
    }
}
