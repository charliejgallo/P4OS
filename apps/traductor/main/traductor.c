/*
 * P4OS - Traductor: text from one language to another, offline with the
 * language packs from the Store, or online (MyMemory, or the user's own
 * LibreTranslate server).
 *
 * The screen, upright: the two languages and the swap between them, the
 * text (the board's keyboard or a USB/Bluetooth one), the translation with
 * its engine and time, the dictionary for a word or two, and below the
 * history and the favourites. Lying down the lists go to the right.
 *
 * Everything slow happens elsewhere (td_job.c): this file only asks, and
 * draws what comes back from its 100 ms timer.
 */
#include "td.h"
#include "aos_icon_ops.h"
#include "aos_text_safe.h"

/* Two speech bubbles, an A behind and an Ñ in front */
static const uint8_t TRADUCTOR_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -13, -12, 54, 44, 10, AIC_C_TEAL, 255),
    AIC_INTO,
    AIC_TEXT(AIC_FONT_TITLE, 1), 'A',
    AIC_OUT,
    AIC_RECT(AIC_CENTER, -26, 9, 10, 10, 2, AIC_C_TEAL, 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, 14, 15, 54, 44, 10, AIC_C_ORANGE, 255),
    AIC_INTO,
    AIC_TEXT(AIC_FONT_TITLE, 2), 0xC3, 0x91,
    AIC_OUT,
    AIC_RECT(AIC_CENTER, 28, 36, 10, 10, 2, AIC_C_ORANGE, 255),
    AIC_ROT(450),
    AIC_END
};

typedef struct {
    lv_obj_t *root;
    int32_t W, H;
    bool land;
    char src[8], dst[8];
    /* widgets */
    lv_obj_t *src_pill, *dst_pill, *src_lbl, *dst_lbl;
    lv_obj_t *ta, *kb, *go_btn, *go_lbl, *src_info;
    lv_obj_t *res_card, *res_col, *res_lbl, *res_info, *res_star, *spinner, *dict_box;
    lv_obj_t *tab_hist, *tab_fav, *list;
    bool show_fav;
    /* the translation on screen */
    char *res_text;
    char res_info_text[120];
    char res_src[8], res_dst[8];
    int res_hist;               /* its line in the history, -1 none           */
    bool busy;
    char last_stage[48];
    /* sheets */
    lv_obj_t *sheet;
    int pick;                   /* 0 source, 1 target                        */
    uint32_t last_files;
} td_ui_t;

static td_ui_t U;
static lv_timer_t *s_timer;

static void build(void);
static void list_fill(void);
static void result_show(const char *text, const char *info, bool ok);

/* ---- small widgets ---- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = box(parent, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, AOS_UI_RADIUS, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, int32_t d, lv_color_t bg, lv_event_cb_t cb,
                           void *ud)
{
    lv_obj_t *b = box(parent, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = label(b, glyph, &aos_sym_28, AOS_C_TEXT);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *pill(lv_obj_t *parent, int32_t w, int32_t h, lv_color_t bg, lv_event_cb_t cb, void *ud,
                      lv_obj_t **lbl)
{
    lv_obj_t *b = box(parent, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    *lbl = label(b, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(*lbl, w - 36);
    lv_label_set_long_mode(*lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(*lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(*lbl);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void set_text_safe(lv_obj_t *l, const char *t)
{
    size_t n = strlen(t) * 2 + 16;
    char *s = (char *)malloc(n);
    if (!s) {
        lv_label_set_text(l, t);
        return;
    }
    aos_text_safe(s, n, t);
    lv_label_set_text(l, s);
    free(s);
}

static void upper2(char *out, const char *code)
{
    out[0] = (char)(code[0] >= 'a' && code[0] <= 'z' ? code[0] - 32 : code[0]);
    out[1] = (char)(code[1] >= 'a' && code[1] <= 'z' ? code[1] - 32 : code[1]);
    out[2] = 0;
}

/* ---- the languages ---- */

static void langs_save(void)
{
    aos_hal_pref_set_str("trad_src", U.src);
    aos_hal_pref_set_str("trad_dst", U.dst);
}

static void langs_show(void)
{
    if (!U.src_lbl) return;
    lv_label_set_text(U.src_lbl, td_lang_name(U.src));
    lv_label_set_text(U.dst_lbl, td_lang_name(U.dst));
}

static void swap_cb(lv_event_t *e)
{
    (void)e;
    if (td_job_busy()) return;
    char a[8];
    /* "Detectar" swaps as what it found, or as the language of the result */
    td_scpy(a, sizeof a, strcmp(U.src, "auto") ? U.src : U.res_src[0] && strcmp(U.res_src, "auto") ? U.res_src : "es");
    td_scpy(U.src, sizeof U.src, U.dst);
    td_scpy(U.dst, sizeof U.dst, a);
    langs_save();
    langs_show();
    if (U.res_text && U.res_text[0] && U.ta) {
        lv_textarea_set_text(U.ta, U.res_text);
        result_show("", "", true);
    }
}

/* ---- sheets ---- */

static void sheet_del_cb(void *o) { lv_obj_delete((lv_obj_t *)o); }

static void sheet_close(void)
{
    if (!U.sheet) return;
    lv_obj_t *o = U.sheet;
    U.sheet = NULL;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_async_call(sheet_del_cb, o);     /* maybe one of its own buttons' events */
}

static void sheet_x_cb(lv_event_t *e)
{
    (void)e;
    sheet_close();
}

static lv_obj_t *sheet_open(const char *title)
{
    sheet_close();
    U.sheet = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.sheet, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(U.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = label(U.sheet, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, AOS_UI_PAD + 4, 18);
    lv_obj_set_width(t, U.W - 2 * AOS_UI_PAD - 100);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *x = round_btn(U.sheet, AOS_SYM_CLOSE, 72, AOS_C_CARD2, sheet_x_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 10);
    lv_obj_t *body = box(U.sheet, U.W - 2 * AOS_UI_PAD, U.H - 104);
    lv_obj_set_pos(body, AOS_UI_PAD, 100);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_style_pad_bottom(body, 24, 0);
    lv_obj_update_layout(body);
    return body;
}

static lv_obj_t *row(lv_obj_t *body, const char *title, const char *sub, bool on, lv_event_cb_t cb, void *ud)
{
    int32_t w = lv_obj_get_width(body);
    lv_obj_t *r = box(body, w, sub ? 116 : AOS_UI_ROW_H);
    lv_obj_set_style_radius(r, 22, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *t = label(r, title, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(t, w - 130);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, sub ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 24, sub ? 16 : 0);
    if (sub) {
        lv_obj_t *s = label(r, sub, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(s, w - 130);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 24, -16);
    }
    if (on) {
        lv_obj_t *c = label(r, AOS_SYM_CHECK, &aos_sym_28, AOS_C_ACCENT);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -24, 0);
    }
    return r;
}

static lv_obj_t *section(lv_obj_t *body, const char *t)
{
    lv_obj_t *l = label(body, t, aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 18, 0);
    lv_obj_set_style_pad_left(l, 8, 0);
    return l;
}

static void lang_pick_cb(lv_event_t *e)
{
    const char *code = (const char *)lv_event_get_user_data(e);
    if (U.pick == 0) {
        if (!strcmp(code, U.dst)) td_scpy(U.dst, sizeof U.dst, U.src[0] && strcmp(U.src, "auto") ? U.src : "en");
        td_scpy(U.src, sizeof U.src, code);
    } else {
        if (!strcmp(code, U.src)) td_scpy(U.src, sizeof U.src, U.dst);
        td_scpy(U.dst, sizeof U.dst, code);
    }
    if (!strcmp(U.src, U.dst)) td_scpy(U.dst, sizeof U.dst, strcmp(U.src, "en") ? "en" : "es");
    langs_save();
    langs_show();
    sheet_close();
}

static void lang_sheet(int which)
{
    U.pick = which;
    lv_obj_t *body = sheet_open(which == 0 ? _("Traducir del") : _("Traducir al"));
    const char *cur = which == 0 ? U.src : U.dst;
    const char *other = which == 0 ? U.dst : U.src;
    if (which == 0)
        row(body, _("Detectar"), _("La placa reconoce español, inglés, portugués, italiano, francés y alemán"),
            !strcmp(cur, "auto"), lang_pick_cb, (void *)"auto");
    /* first the ones the board can do without a network, then the rest */
    for (int pass = 0; pass < 2; pass++) {
        bool any = false;
        for (int i = 0; i < td_lang_count(); i++) {
            const td_lang_t *L = td_lang_at(i);
            if (!strcmp(L->code, other)) continue;
            int steps = which == 0 ? td_local_steps(L->code, other) : td_local_steps(other, L->code);
            bool dict = which == 0 ? td_pack_dict(L->code, other) != NULL : td_pack_dict(other, L->code) != NULL;
            bool local = steps > 0 || dict;
            if ((pass == 0) != local) continue;
            if (!any) {
                section(body, pass == 0 ? _("Sin red, en la placa") : _("Con red"));
                any = true;
            }
            const char *sub = NULL;
            if (pass == 0)
                sub = steps == 1 ? _("Paquete de idioma") : steps == 2 ? _("Pasando por el inglés, tarda el doble")
                                                                       : _("Sólo el diccionario");
            row(body, _(L->name), sub, !strcmp(cur, L->code), lang_pick_cb, (void *)L->code);
        }
    }
}

static void src_pill_cb(lv_event_t *e)
{
    (void)e;
    lang_sheet(0);
}

static void dst_pill_cb(lv_event_t *e)
{
    (void)e;
    lang_sheet(1);
}

/* the settings: the engine, the packs, the history */

static void settings_sheet(void);

static void engine_cb(lv_event_t *e)
{
    td_cfg()->engine = (int)(intptr_t)lv_event_get_user_data(e);
    td_cfg_save();
    settings_sheet();
}

static uint32_t s_clear_ms;

static void hist_clear_cb(lv_event_t *e)
{
    (void)e;
    /* a second tap within three seconds: a slip of the finger keeps it */
    uint32_t n = td_now_ms();
    if (!s_clear_ms || n - s_clear_ms > 3000) {
        s_clear_ms = n;
        aos_ui_toast(_("Tocá de nuevo para borrar el historial"), 2500);
        return;
    }
    s_clear_ms = 0;
    for (int i = td_hist_count() - 1; i >= 0; i--)
        if (!td_hist_at(i)->fav) td_hist_del(i);
    td_hist_save();
    U.res_hist = -1;
    list_fill();
    aos_ui_toast(_("Historial borrado; los favoritos quedan"), 1600);
}

static void settings_sheet(void)
{
    lv_obj_t *body = sheet_open(_("Ajustes"));
    td_cfg_t *c = td_cfg();
    section(body, _("Motor"));
    char sub[160];
    for (int i = 0; i < TD_E_COUNT; i++) {
        switch (i) {
        case TD_E_AUTO:
            snprintf(sub, sizeof sub, "%s", _("La placa si hay paquete; si no, tu servidor o MyMemory"));
            break;
        case TD_E_LOCAL:
            snprintf(sub, sizeof sub, "%s", _("Sin red, con los paquetes de idioma de la tarjeta"));
            break;
        case TD_E_SERVER:
            if (c->server[0]) snprintf(sub, sizeof sub, "LibreTranslate: %.100s", c->server);
            else snprintf(sub, sizeof sub, "%s", _("LibreTranslate; la dirección va en la página del portal"));
            break;
        case TD_E_ONLINE:
            snprintf(sub, sizeof sub, _("MyMemory, gratis: %d de %d caracteres hoy"), td_quota_used(),
                     td_quota_limit());
            break;
        }
        row(body, td_engine_name(i), sub, c->engine == i, engine_cb, (void *)(intptr_t)i);
    }
    section(body, _("Paquetes de idioma"));
    if (!td_packs_count()) {
        lv_obj_t *l = label(body, _("No hay paquetes en la tarjeta. Se bajan de la Tienda, en los extras del Traductor: cada par trae los dos modelos y los diccionarios."),
                            aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, lv_obj_get_width(body) - 16);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_left(l, 8, 0);
    }
    for (int i = 0; i < td_packs_count(); i++) {
        const td_pack_t *k = td_pack_at(i);
        char t[96], a[3], b[3];
        upper2(a, k->pair);
        upper2(b, k->pair + 3);
        snprintf(t, sizeof t, "%s - %s", td_lang_name((char[]){k->pair[0], k->pair[1], 0}),
                 td_lang_name((char[]){k->pair[3], k->pair[4], 0}));
        unsigned long mb = (unsigned long)((k->bytes + 500000) / 1000000);
        if (k->n_nmt && k->n_dict) snprintf(sub, sizeof sub, _("Traducción y diccionario, %lu MB"), mb);
        else if (k->n_nmt) snprintf(sub, sizeof sub, _("Traducción, %lu MB"), mb);
        else snprintf(sub, sizeof sub, _("Diccionario, %lu MB"), mb);
        row(body, t, sub, false, NULL, NULL);
    }
    section(body, _("Historial"));
    row(body, _("Borrar el historial"), _("Los favoritos no se borran"), false, hist_clear_cb, NULL);
    lv_obj_t *l = label(body, _("La dirección del servidor, su clave y el correo para MyMemory se escriben en la página Traductor del portal."),
                        aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, lv_obj_get_width(body) - 16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_left(l, 8, 0);
    lv_obj_set_style_pad_top(l, 12, 0);
}

static void gear_cb(lv_event_t *e)
{
    (void)e;
    settings_sheet();
}

/* ---- the keyboard ---- */

static void kb_hide(void)
{
    if (!U.kb) return;
    lv_obj_t *k = U.kb;
    U.kb = NULL;
    lv_keyboard_set_textarea(k, NULL);
    lv_obj_delete(k);
    if (U.ta) lv_obj_remove_state(U.ta, LV_STATE_FOCUSED);
}

static void translate(void);

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        lv_async_call((lv_async_cb_t)(void (*)(void *))kb_hide, NULL);
        translate();
    } else if (code == LV_EVENT_CANCEL) {
        lv_async_call((lv_async_cb_t)(void (*)(void *))kb_hide, NULL);
    }
}

static void kb_show(void)
{
    if (U.kb || !U.ta) return;
    int32_t h = U.land ? U.H * 46 / 100 : 430;
    U.kb = lv_keyboard_create(U.root);
    lv_obj_set_size(U.kb, U.W, h);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(U.kb, kb_cb, LV_EVENT_CANCEL, NULL);
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        kb_show();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        char t[48];
        size_t n = strlen(lv_textarea_get_text(U.ta));
        snprintf(t, sizeof t, "%u / %d", (unsigned)n, TD_TEXT_MAX);
        if (U.src_info) lv_label_set_text(U.src_info, n ? t : "");
    }
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    if (!U.ta) return;
    lv_textarea_set_text(U.ta, "");
    result_show("", "", true);
}

static void paste_cb(lv_event_t *e)
{
    (void)e;
    if (!U.ta) return;
    char *b = (char *)malloc(TD_TEXT_MAX + 1);
    if (!b) return;
    if (aos_ui_clip_get(b, TD_TEXT_MAX + 1)) lv_textarea_set_text(U.ta, b);
    else aos_ui_toast(_("El portapapeles está vacío"), 1200);
    free(b);
}

/* ---- the result ---- */

static void dict_show(const char *src, const char *dst, const char *word);

static void result_star_paint(void)
{
    if (!U.res_star) return;
    td_hist_t *h = td_hist_at(U.res_hist);
    bool on = h && h->fav;
    lv_obj_t *l = lv_obj_get_child(U.res_star, 0);
    lv_label_set_text(l, on ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE);
    lv_obj_set_style_text_color(l, on ? AOS_C_YELLOW : AOS_C_TEXT, 0);
}

static void result_show(const char *text, const char *info, bool ok)
{
    free(U.res_text);
    U.res_text = NULL;
    if (text) {
        size_t l = strlen(text);
        U.res_text = (char *)malloc(l + 1);
        if (U.res_text) memcpy(U.res_text, text, l + 1);
    }
    if (!U.res_lbl) return;
    if (U.spinner) lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
    if (!text || !text[0]) {
        lv_label_set_text(U.res_lbl, ok ? _("La traducción aparece acá.") : info);
        lv_obj_set_style_text_color(U.res_lbl, ok ? AOS_C_DIM : AOS_C_ORANGE, 0);
        lv_label_set_text(U.res_info, "");
        U.res_hist = -1;
    } else {
        set_text_safe(U.res_lbl, text);
        lv_obj_set_style_text_color(U.res_lbl, AOS_C_TEXT, 0);
        if (info != U.res_info_text) td_scpy(U.res_info_text, sizeof U.res_info_text, info);
        lv_label_set_text(U.res_info, info);
    }
    bool have = text && text[0];
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_obj_get_child(lv_obj_get_parent(U.res_star), i + 1);
        if (b) {
            if (have) lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }
    result_star_paint();
    if (U.dict_box) {
        lv_obj_clean(U.dict_box);
        lv_obj_add_flag(U.dict_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (U.res_col) lv_obj_scroll_to_y(U.res_col, 0, LV_ANIM_OFF);
}

static const char *pos_name(const char *en)
{
    static const struct { const char *en, *es; } P[] = {
        {"noun", N_("sustantivo")}, {"verb", N_("verbo")}, {"adjective", N_("adjetivo")},
        {"adverb", N_("adverbio")}, {"pronoun", N_("pronombre")}, {"preposition", N_("preposición")},
        {"conjunction", N_("conjunción")}, {"interjection", N_("interjección")},
        {"phrase", N_("frase")}, {"proper noun", N_("nombre propio")}, {"numeral", N_("numeral")},
        {"article", N_("artículo")}, {"determiner", N_("determinante")}, {"prefix", N_("prefijo")},
        {"suffix", N_("sufijo")}, {"abbreviation", N_("abreviatura")}, {"idiom", N_("modismo")},
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (!strcmp(P[i].en, en)) return _(P[i].es);
    return en;
}

static void dict_show(const char *src, const char *dst, const char *word)
{
    if (!U.dict_box || !strcmp(src, "auto")) return;
    /* a word or two: three spaces at most */
    int sp = 0;
    for (const char *p = word; *p; p++) {
        if (*p == ' ') sp++;
        if (*p == '\n') return;
    }
    if (sp > 2 || strlen(word) > 40) return;
    char *buf = (char *)malloc(4096);
    if (!buf) return;
    int n = td_dict_lookup(src, dst, word, buf, 4096);
    if (n < 2) {
        free(buf);
        return;
    }
    lv_obj_remove_flag(U.dict_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(U.res_col);
    int32_t w = lv_obj_get_width(U.dict_box);
    char *nl = strchr(buf, '\n');
    *nl = 0;
    label(U.dict_box, buf, aos_font_body, AOS_C_ACCENT);     /* the word, as the dictionary has it */
    char last_pos[32] = "";
    int shown = 0;
    for (char *l = nl + 1; l && *l && shown < 6;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char *g = strchr(l, '\t');
        char *tr = g ? strchr(g + 1, '\t') : NULL;
        if (g && tr) {
            *g = 0;
            *tr = 0;
            if (strcmp(last_pos, l)) {
                td_scpy(last_pos, sizeof last_pos, l);
                lv_obj_t *p = label(U.dict_box, pos_name(l), aos_font_caption, AOS_C_GREEN);
                lv_obj_set_style_pad_top(p, 6, 0);
            }
            lv_obj_t *x = label(U.dict_box, tr + 1, aos_font_small, AOS_C_TEXT);
            lv_obj_set_width(x, w);
            lv_label_set_long_mode(x, LV_LABEL_LONG_MODE_WRAP);
            if (g[1]) {
                lv_obj_t *y = label(U.dict_box, g + 1, aos_font_caption, AOS_C_DIM);
                lv_obj_set_width(y, w);
                lv_label_set_long_mode(y, LV_LABEL_LONG_MODE_WRAP);
            }
            shown++;
        }
        l = e ? e + 1 : NULL;
    }
    free(buf);
}


/* One word, translated by the model, is a guess without context: the model
 * gave "Corr--s de ejecución" for "run". When the dictionary knows the
 * word, the model's answer is none of its translations and looks broken,
 * the dictionary's first one goes on top (the whole entry is below it). Not
 * whenever they differ: WikDict's Spanish side is thin ("correr" has no
 * "run" in it) and the model's "Running" is the better answer there. */
static bool one_word(const char *t)
{
    bool in = false;
    int words = 0;
    for (const char *p = t; *p; p++) {
        bool sp = *p == ' ' || *p == '\n' || *p == '\t';
        if (!sp && !in) words++;
        in = !sp;
    }
    return words == 1;
}

static void fold_simple(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) {
        unsigned char c = *p;
        if (c == '.' || c == '!' || c == '?' || c == ',' || c == '"') continue;
        if (c == 0xC2 && (p[1] == 0xBF || p[1] == 0xA1)) {
            p++;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && o + 2 < n) {
            out[o++] = (char)c;
            out[o++] = (char)(p[1] + 0x20);
            p++;
            continue;
        }
        out[o++] = (char)c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static bool dict_override(const char *src, const char *dst, const char *word, const char *nmt, char *out, size_t n)
{
    if (!one_word(word)) return false;
    char *buf = (char *)malloc(4096);
    if (!buf) return false;
    bool ok = false;
    if (td_dict_lookup(src, dst, word, buf, 4096) >= 2) {
        char want[128];
        fold_simple(nmt, want, sizeof want);
        bool found = false;
        char first[96] = "";
        for (char *l = strchr(buf, '\n'); l && *l && !found;) {
            l++;
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            char *tr = strrchr(l, '\t');
            if (tr) {
                tr++;
                for (char *t = tr; *t;) {
                    char *c = strstr(t, ", ");
                    size_t len = c ? (size_t)(c - t) : strlen(t);
                    char one[96], f[96];
                    snprintf(one, sizeof one, "%.*s", (int)(len < 95 ? len : 95), t);
                    if (!first[0]) td_scpy(first, sizeof first, one);
                    fold_simple(one, f, sizeof f);
                    if (!strcmp(f, want)) found = true;
                    if (!c) break;
                    t = c + 2;
                }
            }
            if (!e) break;
            *e = '\n';
            l = e;
        }
        /* and only when the model's answer looks broken: more than two
         * words for one, or marks a word does not have */
        int nw = 0;
        bool in = false;
        for (const char *q = nmt; *q; q++) {
            bool sp = *q == ' ';
            if (!sp && !in) nw++;
            in = !sp;
        }
        bool broken = nw > 2 || strstr(nmt, "--") != NULL;
        if (!found && broken && first[0]) {
            td_scpy(out, n, first);
            ok = true;
        }
    }
    free(buf);
    return ok;
}

static void copy_cb(lv_event_t *e)
{
    (void)e;
    if (!U.res_text || !U.res_text[0]) return;
    aos_ui_clip_set(U.res_text);
    aos_ui_toast(_("Copiado"), 1000);
}

static void star_cb(lv_event_t *e)
{
    (void)e;
    td_hist_t *h = td_hist_at(U.res_hist);
    if (!h && U.res_text && U.res_text[0] && U.ta) {
        /* with the history off there is no line yet: a favourite makes one */
        if (td_hist_add(U.res_src, U.res_dst, lv_textarea_get_text(U.ta), U.res_text) == 0) U.res_hist = 0;
        h = td_hist_at(U.res_hist);
    }
    if (!h) return;
    h->fav = !h->fav;
    td_hist_save();
    result_star_paint();
    list_fill();
}

static void use_cb(lv_event_t *e)
{
    swap_cb(e);
}

static const char *err_text(const td_result_t *r)
{
    switch (r->err) {
    case TD_OK: return "";
    case TD_ERR_NO_ROUTE:
        if (!strcmp(r->src, "auto")) return _("No reconocí el idioma: elegilo arriba.");
        return _("No hay paquete de idioma para este par. Bajalo de la Tienda o elegí un motor en línea en Ajustes.");
    case TD_ERR_NO_NET: return _("Sin red. Sin red sólo traducen los paquetes de idioma.");
    case TD_ERR_NET: return _("No se pudo llegar al servicio.");
    case TD_ERR_SERVER: return _("El servicio no pudo traducir.");
    case TD_ERR_QUOTA:
        return r->engine == TD_E_ONLINE ? _("Se terminaron los caracteres gratis de MyMemory por hoy. Un correo en la página del portal da diez veces más.")
                                        : _("El servidor pidió esperar un poco.");
    case TD_ERR_KEY: return _("El servidor pide una clave, o la clave no es la suya: va en la página del portal.");
    case TD_ERR_MEM: return _("No hay memoria para el modelo: cerrá otras apps.");
    case TD_ERR_CANCEL: return _("Cancelado.");
    case TD_ERR_NO_SERVER: return _("Falta la dirección del servidor: se escribe en la página del portal.");
    }
    return "?";
}

static void busy_paint(bool busy)
{
    U.busy = busy;
    if (U.go_lbl) lv_label_set_text(U.go_lbl, busy ? _("Cancelar") : _("Traducir"));
    if (U.go_btn) lv_obj_set_style_bg_color(U.go_btn, busy ? AOS_C_CARD2 : AOS_C_ACCENT, 0);
    if (U.spinner) {
        if (busy) lv_obj_remove_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void done_cb(const td_result_t *r)
{
    busy_paint(false);
    td_scpy(U.res_src, sizeof U.res_src, r->src);
    td_scpy(U.res_dst, sizeof U.res_dst, r->dst);
    const char *text = U.ta ? lv_textarea_get_text(U.ta) : "";
    if (r->err != TD_OK) {
        /* no model, but the dictionary knows the word: that is the answer */
        if (r->err == TD_ERR_NO_ROUTE && strcmp(r->src, "auto") && td_pack_dict(r->src, r->dst)) {
            result_show("", "", true);
            dict_show(r->src, r->dst, text);
            if (U.dict_box && !lv_obj_has_flag(U.dict_box, LV_OBJ_FLAG_HIDDEN)) {
                lv_label_set_text(U.res_lbl, _("Sin modelo para este par: lo que dice el diccionario."));
                lv_obj_set_style_text_color(U.res_lbl, AOS_C_DIM, 0);
                return;
            }
        }
        result_show("", err_text(r), false);
        if (r->msg[0] && (r->err == TD_ERR_SERVER || r->err == TD_ERR_KEY))
            lv_label_set_text_fmt(U.res_info, "%.100s", r->msg);
        return;
    }
    char info[120], a[3], b[3];
    upper2(a, r->src);
    upper2(b, r->dst);
    char secs[16];
    snprintf(secs, sizeof secs, "%lu,%lu s", (unsigned long)(r->ms / 1000), (unsigned long)(r->ms % 1000 / 100));
    snprintf(info, sizeof info, "%s %s %s  ·  %s%s%s  ·  %s", a, LV_SYMBOL_RIGHT, b, td_engine_name(r->engine),
             r->pivot ? " " : "", r->pivot ? _("por inglés") : "", secs);
    const char *shown = r->text;
    char word[96];
    if (r->engine == TD_E_LOCAL && dict_override(r->src, r->dst, text, r->text, word, sizeof word)) {
        shown = word;
        snprintf(info, sizeof info, "%s %s %s  ·  %s", a, LV_SYMBOL_RIGHT, b, _("Diccionario"));
    }
    if (!td_cfg()->hist_off && text[0]) {
        U.res_hist = td_hist_add(r->src, r->dst, text, shown) == 0 ? 0 : -1;
        td_hist_save();
    } else {
        U.res_hist = -1;
    }
    result_show(shown, info, true);
    dict_show(r->src, r->dst, text);
    if (strcmp(U.src, "auto") == 0 && U.src_info) {
        char t[64];
        snprintf(t, sizeof t, _("Detectado: %s"), td_lang_name(r->src));
        lv_label_set_text(U.src_info, t);
    }
    list_fill();
}

static void translate(void)
{
    if (td_job_busy()) {
        td_job_cancel();
        return;
    }
    if (!U.ta) return;
    const char *t = lv_textarea_get_text(U.ta);
    bool any = false;
    for (const char *p = t; *p; p++)
        if (*p != ' ' && *p != '\n') any = true;
    if (!any) return;
    if (!td_job_start(t, U.src, U.dst, done_cb)) {
        aos_ui_toast(_("No se pudo empezar"), 1200);
        return;
    }
    busy_paint(true);
    lv_label_set_text(U.res_lbl, "");
    lv_label_set_text(U.res_info, td_job_stage());
    U.last_stage[0] = 0;
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    kb_hide();
    translate();
}

/* ---- the lists ---- */

static void hist_open_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    td_hist_t *h = td_hist_at(i);
    if (!h || td_job_busy()) return;
    kb_hide();
    td_scpy(U.src, sizeof U.src, h->src);
    td_scpy(U.dst, sizeof U.dst, h->dst);
    langs_save();
    langs_show();
    lv_textarea_set_text(U.ta, h->a);
    td_scpy(U.res_src, sizeof U.res_src, h->src);
    td_scpy(U.res_dst, sizeof U.res_dst, h->dst);
    char info[64], a[3], b[3];
    upper2(a, h->src);
    upper2(b, h->dst);
    snprintf(info, sizeof info, "%s %s %s  ·  %s", a, LV_SYMBOL_RIGHT, b, h->fav ? _("Favorito") : _("Del historial"));
    result_show(h->b, info, true);
    U.res_hist = i;
    result_star_paint();
    dict_show(h->src, h->dst, h->a);
}

static void hist_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    td_hist_t *h = td_hist_at(i);
    if (!h) return;
    lv_indev_wait_release(lv_indev_active());
    if (h->fav) {
        h->fav = false;
        aos_ui_toast(_("Ya no es favorito"), 1000);
    } else {
        td_hist_del(i);
        aos_ui_toast(_("Borrado del historial"), 1000);
    }
    if (U.res_hist == i) U.res_hist = -1;
    else if (U.res_hist > i) U.res_hist--;
    td_hist_save();
    result_star_paint();
    list_fill();
}

static void tab_cb(lv_event_t *e)
{
    U.show_fav = (bool)(intptr_t)lv_event_get_user_data(e);
    list_fill();
}

static void tabs_paint(void)
{
    if (!U.tab_hist) return;
    lv_obj_set_style_bg_color(U.tab_hist, U.show_fav ? AOS_C_CARD : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(U.tab_fav, U.show_fav ? AOS_C_CARD2 : AOS_C_CARD, 0);
}

static void list_fill(void)
{
    if (!U.list) return;
    tabs_paint();
    lv_obj_clean(U.list);
    lv_obj_update_layout(U.list);
    int32_t w = lv_obj_get_width(U.list);
    int shown = 0;
    for (int i = 0; i < td_hist_count(); i++) {
        td_hist_t *h = td_hist_at(i);
        if (U.show_fav && !h->fav) continue;
        if (++shown > 60) break;
        lv_obj_t *r = box(U.list, w, 124);
        lv_obj_set_style_radius(r, 22, 0);
        lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, hist_open_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, hist_del_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        char tag[16], a[3], b[3];
        upper2(a, h->src);
        upper2(b, h->dst);
        snprintf(tag, sizeof tag, "%s %s %s", a, LV_SYMBOL_RIGHT, b);
        lv_obj_t *tg = label(r, tag, aos_font_caption, AOS_C_DIM);
        lv_obj_align(tg, LV_ALIGN_TOP_RIGHT, -20, 14);
        if (h->fav) {
            lv_obj_t *s = label(r, AOS_SYM_STAR, &aos_sym_28, AOS_C_YELLOW);
            lv_obj_align(s, LV_ALIGN_BOTTOM_RIGHT, -20, -14);
        }
        lv_obj_t *la = label(r, "", aos_font_small, AOS_C_DIM);
        set_text_safe(la, h->a);
        lv_obj_set_width(la, w - 150);
        lv_label_set_long_mode(la, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(la, 30);
        lv_obj_set_pos(la, 22, 14);
        lv_obj_t *lb = label(r, "", aos_font_body, AOS_C_TEXT);
        set_text_safe(lb, h->b);
        lv_obj_set_width(lb, w - 100);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(lb, 36);
        lv_obj_set_pos(lb, 22, 66);
        aos_make_decorative(tg);
        aos_make_decorative(la);
        aos_make_decorative(lb);
    }
    if (!shown) {
        lv_obj_t *l = label(U.list,
                            U.show_fav ? _("Tocá la estrella de una traducción para tenerla acá.")
                                       : _("Lo que traduzcas queda acá. Mantené apretada una línea para borrarla."),
                            aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, w - 16);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_all(l, 12, 0);
    }
}

/* ---- the screen ---- */

static lv_obj_t *tab(lv_obj_t *parent, const char *t, bool fav, int32_t w)
{
    lv_obj_t *b = box(parent, w, 64);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)fav);
    lv_obj_t *l = label(b, t, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    return b;
}

static void build(void)
{
    const int32_t P = AOS_UI_PAD;
    int32_t colw = U.land ? U.W / 2 - P - P / 2 : U.W - 2 * P;
    int32_t y = 10;

    /* the languages */
    int32_t bar_h = 84;
    int32_t pw = (colw - 72 - 72 - 32) / 2;
    lv_obj_t *bar = box(U.root, colw, bar_h);
    lv_obj_set_pos(bar, P, y);
    U.src_pill = pill(bar, pw, 76, AOS_C_CARD2, src_pill_cb, NULL, &U.src_lbl);
    lv_obj_align(U.src_pill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = round_btn(bar, AOS_SYM_SWAP_HORIZONTAL, 72, AOS_C_CARD, swap_cb, NULL);
    lv_obj_align(sw, LV_ALIGN_LEFT_MID, pw + 8, 0);
    U.dst_pill = pill(bar, pw, 76, AOS_C_CARD2, dst_pill_cb, NULL, &U.dst_lbl);
    lv_obj_align(U.dst_pill, LV_ALIGN_LEFT_MID, pw + 88, 0);
    lv_obj_t *g = round_btn(bar, AOS_SYM_COG, 72, AOS_C_CARD, gear_cb, NULL);
    lv_obj_align(g, LV_ALIGN_RIGHT_MID, 0, 0);
    langs_show();
    y += bar_h + 12;

    /* the text */
    int32_t src_h = U.land ? 232 : 300;
    lv_obj_t *sc = card(U.root, P, y, colw, src_h);
    U.ta = lv_textarea_create(sc);
    lv_obj_set_size(U.ta, colw - 16, src_h - 96);
    lv_obj_set_pos(U.ta, 8, 8);
    lv_obj_set_style_bg_opa(U.ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_pad_all(U.ta, 14, 0);
    lv_textarea_set_placeholder_text(U.ta, _("Escribí lo que quieras traducir"));
    lv_textarea_set_max_length(U.ta, TD_TEXT_MAX);
    lv_obj_add_event_cb(U.ta, ta_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(U.ta, ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
    U.src_info = label(sc, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.src_info, LV_ALIGN_BOTTOM_LEFT, 24, -30);
    lv_obj_t *gb = box(sc, 210, 68);
    lv_obj_set_style_radius(gb, 34, 0);
    lv_obj_set_style_bg_color(gb, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(gb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(gb, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(gb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gb, go_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(gb, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    U.go_btn = gb;
    U.go_lbl = label(gb, _("Traducir"), aos_font_body, AOS_C_TEXT);
    lv_obj_center(U.go_lbl);
    lv_obj_t *cl = round_btn(sc, AOS_SYM_CLOSE, 64, AOS_C_CARD2, clear_cb, NULL);
    lv_obj_align(cl, LV_ALIGN_BOTTOM_RIGHT, -238, -16);
    lv_obj_t *pa = round_btn(sc, AOS_SYM_CONTENT_PASTE, 64, AOS_C_CARD2, paste_cb, NULL);
    lv_obj_align(pa, LV_ALIGN_BOTTOM_RIGHT, -314, -16);
    y += src_h + 12;

    /* the translation */
    int32_t res_h = U.land ? U.H - y - 12 : 420;
    U.res_card = card(U.root, P, y, colw, res_h);
    U.res_col = box(U.res_card, colw - 48, res_h - 104);
    lv_obj_set_pos(U.res_col, 24, 20);
    lv_obj_add_flag(U.res_col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(U.res_col, LV_DIR_VER);
    lv_obj_set_flex_flow(U.res_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.res_col, 6, 0);
    U.res_lbl = label(U.res_col, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(U.res_lbl, colw - 48);
    lv_label_set_long_mode(U.res_lbl, LV_LABEL_LONG_MODE_WRAP);
    U.dict_box = box(U.res_col, colw - 48, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.dict_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.dict_box, 2, 0);
    lv_obj_set_style_pad_top(U.dict_box, 18, 0);
    lv_obj_add_flag(U.dict_box, LV_OBJ_FLAG_HIDDEN);
    U.spinner = lv_spinner_create(U.res_card);
    lv_obj_set_size(U.spinner, 56, 56);
    lv_obj_set_style_arc_width(U.spinner, 6, 0);
    lv_obj_set_style_arc_width(U.spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(U.spinner, AOS_C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(U.spinner, AOS_C_CARD2, 0);
    lv_obj_align(U.spinner, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *foot = box(U.res_card, colw - 32, 72);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
    U.res_info = label(foot, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.res_info, colw - 32 - 240);
    lv_label_set_long_mode(U.res_info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.res_info, LV_ALIGN_LEFT_MID, 8, 0);
    U.res_star = round_btn(foot, AOS_SYM_STAR_OUTLINE, 64, AOS_C_CARD2, star_cb, NULL);
    lv_obj_align(U.res_star, LV_ALIGN_RIGHT_MID, -156, 0);
    lv_obj_t *cp = round_btn(foot, AOS_SYM_CONTENT_COPY, 64, AOS_C_CARD2, copy_cb, NULL);
    lv_obj_align(cp, LV_ALIGN_RIGHT_MID, -78, 0);
    lv_obj_t *us = round_btn(foot, AOS_SYM_SWAP_VERTICAL, 64, AOS_C_CARD2, use_cb, NULL);
    lv_obj_align(us, LV_ALIGN_RIGHT_MID, 0, 0);
    /* res_star's parent's children 1..3 are hidden without a result: the star
     * is child 1, so order matters here: info 0, star 1, copy 2, use 3 */
    y += res_h + 16;

    /* the lists */
    int32_t lx = U.land ? U.W / 2 + P / 2 : P;
    int32_t ly = U.land ? 10 : y;
    int32_t lw = colw;
    int32_t tw = (lw - 12) / 2;
    U.tab_hist = tab(U.root, _("Historial"), false, tw);
    lv_obj_set_pos(U.tab_hist, lx, ly);
    U.tab_fav = tab(U.root, _("Favoritos"), true, tw);
    lv_obj_set_pos(U.tab_fav, lx + tw + 12, ly);
    ly += 76;
    U.list = box(U.root, lw, U.H - ly - 8);
    lv_obj_set_pos(U.list, lx, ly);
    lv_obj_add_flag(U.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);
    lv_obj_set_flex_flow(U.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.list, 10, 0);
    lv_obj_set_style_pad_bottom(U.list, 20, 0);

    /* what was on screen comes back */
    char *keep = U.res_text;
    U.res_text = NULL;
    int keep_hist = U.res_hist;
    if (keep && keep[0]) {
        td_hist_t *h = td_hist_at(keep_hist);
        if (h) lv_textarea_set_text(U.ta, h->a);
        result_show(keep, U.res_info_text, true);
        U.res_hist = keep_hist;
        result_star_paint();
    } else {
        result_show("", "", true);
    }
    free(keep);
    busy_paint(td_job_busy());
    list_fill();
}

static void clean_root(void)
{
    if (U.sheet) {
        lv_obj_t *o = U.sheet;
        U.sheet = NULL;
        lv_async_call_cancel(sheet_del_cb, o);
    }
    if (U.kb) lv_keyboard_set_textarea(U.kb, NULL);
    lv_obj_clean(U.root);
    U.kb = U.ta = U.res_lbl = U.res_info = U.res_star = U.spinner = U.dict_box = U.list = NULL;
    U.src_lbl = U.dst_lbl = U.go_btn = U.go_lbl = U.src_info = U.tab_hist = U.tab_fav = U.res_col = NULL;
}

/* ---- timer, life cycle ---- */

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    td_job_poll();
    if (U.busy && U.res_info) {
        const char *s = td_job_stage();
        if (strcmp(s, U.last_stage)) {
            td_scpy(U.last_stage, sizeof U.last_stage, s);
            lv_label_set_text(U.res_info, s);
        }
    }
    uint32_t n = td_now_ms();
    if (n - U.last_files > 3000) {
        U.last_files = n;
        if (td_cfg_changed()) {
            td_cfg_load();
            aos_hal_log("traductor", "traductor.txt changed: reloaded");
        }
        if (!td_job_busy() && td_hist_changed()) {
            td_hist_load();
            U.res_hist = -1;
            result_star_paint();
            list_fill();
        }
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.res_hist = -1;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    td_cfg_load();
    td_hist_load();
    td_packs_scan();
    td_job_init();
    if (!aos_hal_pref_get_str("trad_src", U.src, sizeof U.src) || (td_lang_find(U.src) < 0 && strcmp(U.src, "auto")))
        td_scpy(U.src, sizeof U.src, "es");
    if (!aos_hal_pref_get_str("trad_dst", U.dst, sizeof U.dst) || td_lang_find(U.dst) < 0)
        td_scpy(U.dst, sizeof U.dst, "en");
    if (!strcmp(U.src, U.dst)) td_scpy(U.dst, sizeof U.dst, strcmp(U.src, "en") ? "en" : "es");
    build();
    U.last_files = td_now_ms();
    s_timer = lv_timer_create(timer_cb, 100, NULL);
    aos_hal_log("traductor", "started: %s > %s, engine %d, %d packs, %d in the history", U.src, U.dst,
                td_cfg()->engine, td_packs_count(), td_hist_count());
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    td_job_stop();
    td_dict_close();
    clean_root();
    td_hist_free();
    free(U.res_text);
    memset(&U, 0, sizeof U);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    char *keep_ta = NULL;
    if (U.ta) {
        const char *t = lv_textarea_get_text(U.ta);
        size_t l = strlen(t);
        keep_ta = (char *)malloc(l + 1);
        if (keep_ta) memcpy(keep_ta, t, l + 1);
    }
    clean_root();
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    build();
    if (keep_ta) {
        lv_textarea_set_text(U.ta, keep_ta);
        free(keep_ta);
    }
    return true;
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.sheet) {
        sheet_close();
        return true;
    }
    if (U.kb) {
        kb_hide();
        return true;
    }
    return false;
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    /* a pack the Store brought while the app was away */
    td_packs_scan();
}

static bool traductor_init(aos_app_t *app)
{
    app->desc.id       = "aos.traductor";
    app->desc.name     = "Traductor";
    app->desc.icon     = "A";
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x1D3557;
    app->desc.color_b  = 0x0B1622;
    app->desc.order    = 81;
    app->desc.flags    = AOS_APP_FLAG_NONE;
    aos_icon_set_ops(app, TRADUCTOR_ICON, sizeof TRADUCTOR_ICON);

    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->back    = back;
    app->show    = show;
    return true;
}

AOS_APP_ENTRY(traductor_init);
