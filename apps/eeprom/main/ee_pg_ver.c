/*
 * P4OS - EEPROM: the Versions page.
 *
 * The versions of the chosen chip, newest first: every read, every copy
 * taken before a write and every write, plus what the portal uploaded. A
 * version opens in the Memory page (to look at it, edit it, write it),
 * becomes the one the image is compared with, or is restored: written to
 * the chip whole and verified, after the usual copy of what was there.
 */
#include "ee.h"

static int s_sel;

static const char *source_name(const char *s)
{
    if (!strcmp(s, "lectura")) return _("lectura");
    if (!strcmp(s, "antes")) return _("antes de escribir");
    if (!strcmp(s, "escrita")) return _("escrita");
    if (!strcmp(s, "portal")) return _("del portal");
    if (!strcmp(s, "vista")) return _("guardada");
    return s[0] ? s : _("archivo");
}

static lv_color_t source_color(const char *s)
{
    if (!strcmp(s, "antes")) return AOS_C_ORANGE;
    if (!strcmp(s, "escrita")) return AOS_C_GREEN;
    if (!strcmp(s, "portal")) return AOS_C_TEAL;
    if (!strcmp(s, "vista")) return AOS_C_PURPLE;
    return AOS_C_ACCENT;
}

/* "2026-10-05 14:03:22", or the stamp in the name without a clock */
static void ver_when(const ee_ver_t *v, char *out, size_t len)
{
    if (v->date[0]) { snprintf(out, len, "%s", v->date); return; }
    const char *dot = strrchr(v->file, '.');
    snprintf(out, len, "%.*s", dot ? (int)(dot - v->file) : (int)strlen(v->file), v->file);
}

/* -------------------------------------------------------------------------- */
/* Actions                                                                     */
/* -------------------------------------------------------------------------- */

static bool open_version(const ee_ver_t *v)
{
    uint32_t size = 0;
    uint8_t *b = ee_ver_load(ee_cur()->name, v->file, &size);
    if (!b) { aos_ui_toast(_("No se pudo leer la versión"), 1600); return false; }
    uint8_t *d = ee_alloc((size + 7) / 8);
    if (!d) { free(b); aos_ui_toast(_("No hay memoria para la imagen"), 1600); return false; }
    ee_img_free();
    S.img = b;
    S.dirty = d;
    memset(d, 0, (size + 7) / 8);
    S.size = size;
    S.valid = true;
    snprintf(S.img_chip, sizeof S.img_chip, "%s", ee_cur()->name);
    char w[24];
    ver_when(v, w, sizeof w);
    snprintf(S.img_from, sizeof S.img_from, "%s %s", _("versión"), w);
    ee_sums_of(S.img, S.size, &S.sums);
    S.cursor = 0;
    S.mark_len = 0;
    if (S.ref && !strcmp(S.ref_name, v->file)) ee_ref_clear();
    ee_diff_count();
    return true;
}

static void open_go(void *ud)
{
    (void)ud;
    if (!open_version(&S.vers[s_sel])) return;
    S.tab = TAB_MEM;
    ee_rebuild();
}

static void restore_go(void *ud)
{
    (void)ud;
    if (!open_version(&S.vers[s_sel])) return;
    ee_start_write(WR_FULL, false);
    ee_rebuild();
}

static void delete_go(void *ud)
{
    (void)ud;
    if (!ee_ver_delete(ee_cur()->name, S.vers[s_sel].file)) aos_ui_toast(_("No se pudo borrar"), 1600);
    if (!strcmp(S.ref_name, S.vers[s_sel].file)) ee_ref_clear();
    ee_vers_reload();
    ee_rebuild();
}

static void note_done(const char *t, void *ud)
{
    (void)ud;
    if (!ee_ver_set_note(ee_cur()->name, S.vers[s_sel].file, t)) aos_ui_toast(_("No se pudo guardar la nota"), 1600);
    ee_vers_reload();
    ee_rebuild();
}

static void show_sums(const ee_ver_t *v)
{
    int32_t cw;
    lv_obj_t *k = ee_sheet_custom(v->file, &cw);
    char t[200];
    snprintf(t, sizeof t, "CRC32  %08x\nMD5    %s\nSHA-256", (unsigned)v->crc, v->md5[0] ? v->md5 : "-");
    lv_obj_t *l = aos_label(k, t, &aos_mono_22, AOS_C_TEXT);
    lv_obj_set_style_pad_hor(l, 12, 0);
    l = aos_label(k, v->sha[0] ? v->sha : "-", &aos_mono_22, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, cw - 24);
    lv_obj_set_style_pad_hor(l, 12, 0);
    lv_obj_set_style_pad_bottom(l, 12, 0);
}

enum { A_OPEN, A_CMP, A_RESTORE, A_NOTE, A_SUMS, A_DELETE };
static int s_act[8];

static void act_pick(int i, void *ud)
{
    (void)ud;
    const ee_ver_t *v = &S.vers[s_sel];
    const ee_chip_t *c = ee_cur();
    char t[300];
    switch (s_act[i]) {
    case A_OPEN:
        if (S.ndirty) {
            snprintf(t, sizeof t, _("Hay %u bytes cambiados sin escribir en la memoria: abrir la versión los pisa."), (unsigned)S.ndirty);
            ee_confirm(_("Abrir"), t, _("Abrir igual"), true, open_go, NULL);
        } else open_go(NULL);
        break;
    case A_CMP: {
        uint32_t size = 0;
        uint8_t *b = ee_ver_load(c->name, v->file, &size);
        if (!b) { aos_ui_toast(_("No se pudo leer la versión"), 1600); break; }
        ee_ref_set(b, size, v->file);
        S.tab = TAB_MEM;
        ee_rebuild();
        if (!S.valid) aos_ui_toast(_("Leé el chip para ver las diferencias"), 2000);
        break;
    }
    case A_RESTORE:
        if (v->size != c->size) { aos_ui_toast(_("La versión no es del tamaño de este chip"), 2000); break; }
        if (J.busy) { aos_ui_toast(_("Hay un trabajo en curso"), 1600); break; }
        snprintf(t, sizeof t, _("Escribir %s en el %s y verificarlo. Antes se guarda una copia de lo que tiene ahora; sólo se escriben las páginas que cambian.%s"),
                 v->file, c->name, S.ndirty ? _("\nLos cambios sin escribir de la memoria se pierden.") : "");
        ee_confirm(_("Restaurar"), t, _("Restaurar"), true, restore_go, NULL);
        break;
    case A_NOTE:
        ee_ask_text(_("Nota"), v->note, 90, note_done, NULL);
        break;
    case A_SUMS:
        show_sums(v);
        break;
    case A_DELETE:
        snprintf(t, sizeof t, _("Borrar %s de la tarjeta. No se puede deshacer."), v->file);
        ee_confirm(_("Borrar la versión"), t, _("Borrar"), true, delete_go, NULL);
        break;
    }
}

static void row_cb(lv_event_t *e)
{
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_sel >= S.nvers) return;
    const ee_ver_t *v = &S.vers[s_sel];
    static ee_item_t it[8];
    int n = 0;
    s_act[n] = A_OPEN;    it[n++] = (ee_item_t){ _("Abrir en Memoria"), _("para verla, editarla o escribirla"), false, false };
    s_act[n] = A_CMP;     it[n++] = (ee_item_t){ _("Comparar con la memoria"), _("las diferencias en rojo"), false, false };
    s_act[n] = A_RESTORE; it[n++] = (ee_item_t){ _("Restaurar en el chip"), _("escribir y verificar"), true, false };
    s_act[n] = A_NOTE;    it[n++] = (ee_item_t){ _("Nota"), v->note[0] ? v->note : NULL, false, false };
    s_act[n] = A_SUMS;    it[n++] = (ee_item_t){ _("Sumas"), "CRC32 · MD5 · SHA-256", false, false };
    s_act[n] = A_DELETE;  it[n++] = (ee_item_t){ _("Borrar"), NULL, true, false };
    ee_sheet(v->file, it, n, act_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* The list                                                                    */
/* -------------------------------------------------------------------------- */

static void reload_cb(lv_event_t *e)
{
    (void)e;
    ee_vers_reload();
    ee_rebuild();
}

void ee_pg_ver(lv_obj_t *parent)
{
    int32_t w = U.CW;
    lv_obj_t *col = ee_column(parent, w, U.CH);
    const ee_chip_t *c = ee_cur();

    lv_obj_t *head = ee_box(col, w, LV_SIZE_CONTENT);
    lv_obj_t *t = aos_label(head, "", aos_font_title, AOS_C_TEXT);
    lv_label_set_text_fmt(t, _("Versiones del %s"), c->name);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_t *r = ee_pill(head, AOS_SYM_UPDATE, NULL, AOS_C_CARD2, reload_cb, NULL);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, 0, 0);
    char sub[96];
    if (!ee_card_ok()) snprintf(sub, sizeof sub, "%s", _("Sin tarjeta: no hay versiones."));
    else snprintf(sub, sizeof sub, "/%s/%s · %d", EE_DIR, c->name, S.nvers);
    lv_obj_t *s = aos_label(head, sub, aos_font_small, AOS_C_DIM);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 8, 60);
    lv_obj_set_height(head, 100);

    if (!S.nvers) {
        lv_obj_t *k = ee_card(col, w);
        ee_caption(k, _("Todavía no hay versiones de este chip. Cada lectura guarda una, y cada escritura guarda lo que había antes y lo que quedó. Desde el portal se pueden subir .bin."), w - 40);
        return;
    }
    lv_obj_t *k = ee_card(col, w);
    lv_obj_set_style_pad_all(k, 8, 0);
    lv_obj_set_style_pad_row(k, 0, 0);
    for (int i = 0; i < S.nvers; i++) {
        const ee_ver_t *v = &S.vers[i];
        lv_obj_t *row = ee_box(k, w - 16, 104);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        bool cmp = S.ref && !strcmp(S.ref_name, v->file);
        if (cmp) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0x3A1512), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }
        char when[32];
        ver_when(v, when, sizeof when);
        lv_obj_t *l = aos_label(row, when, aos_font_body, AOS_C_TEXT);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 16, 12);
        lv_obj_t *b = aos_label(row, source_name(v->source), aos_font_small, source_color(v->source));
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -16, 14);
        char st[16], line[160];
        ee_size_text(v->size, st, sizeof st);
        if (v->sha[0] || v->crc) snprintf(line, sizeof line, "%s · CRC32 %08x%s%s%s", st, (unsigned)v->crc,
                                          cmp ? _(" · comparando") : "", v->note[0] ? " · " : "", v->note);
        else snprintf(line, sizeof line, "%s · %s", st, v->file);
        lv_obj_t *d = aos_label(row, line, aos_font_small, AOS_C_DIM);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(d, w - 16 - 32);
        lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 16, -12);
        aos_make_decorative(l);
        aos_make_decorative(b);
        aos_make_decorative(d);
    }
}
