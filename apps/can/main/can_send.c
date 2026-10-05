/*
 * P4OS - CAN: sending.
 *
 * The editor is the frame's fields - the id, eight data bytes - and a hex
 * pad under them: tap a field, type. A byte takes two digits and moves on to
 * the next; the length follows the last byte written, or the − and + next
 * to it. "Guardar" puts the frame in the list below with the period chosen
 * (by hand, or every 10 ms to 1 s); a saved one is sent with a tap, and a
 * periodic one runs with its ▶ until it is stopped, the bus closes, or
 * nobody acknowledges five in a row.
 *
 * In "solo escucha" nothing goes out: the controller cannot, and the tab
 * says so instead of failing in silence.
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <string.h>

static const uint32_t PERIODS[] = { 0, 10, 20, 50, 100, 200, 500, 1000 };
#define N_PERIODS (sizeof PERIODS / sizeof PERIODS[0])

static cn_frame_t E = { .id = 0x123, .len = 8 };     /* the editor's frame, kept across tabs */
static int s_sel = -1;                              /* -1 the id, 0..7 a byte */
static bool s_fresh = true;                         /* the next digit replaces the field */
static int s_period = 0;

static struct {
    lv_obj_t *id, *bytes[8], *ext, *rtr, *len, *hint, *list, *period;
    lv_obj_t *run[CN_SAVED_MAX];
} S;

static void field_style(lv_obj_t *o, bool sel, bool dim)
{
    lv_obj_set_style_bg_color(o, sel ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(o, 0), dim ? lv_color_hex(0x5A6270) : AOS_C_TEXT, 0);
}

static void editor_show(void)
{
    if (!S.id) return;
    char t[16];
    cn_fmt_id(&E, t, sizeof t);
    lv_label_set_text(lv_obj_get_child(S.id, 0), t);
    field_style(S.id, s_sel < 0, false);
    for (int i = 0; i < 8; i++) {
        snprintf(t, sizeof t, "%02X", E.data[i]);
        lv_label_set_text(lv_obj_get_child(S.bytes[i], 0), t);
        field_style(S.bytes[i], s_sel == i, i >= E.len || (E.fl & CN_RTR));
    }
    lv_obj_set_style_bg_color(S.ext, E.fl & CN_EXT ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(S.rtr, E.fl & CN_RTR ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    snprintf(t, sizeof t, "%u", E.len);
    lv_label_set_text(S.len, t);
}

void cn_send_load(const cn_frame_t *f)
{
    E = *f;
    E.fl &= CN_EXT | CN_RTR;
    s_sel = -1;
    s_fresh = true;
    editor_show();
}

static void field_cb(lv_event_t *e)
{
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    s_fresh = true;
    editor_show();
}

static void ext_cb(lv_event_t *e)
{
    (void)e;
    E.fl ^= CN_EXT;
    if (!(E.fl & CN_EXT)) E.id &= 0x7FF;
    editor_show();
}

static void rtr_cb(lv_event_t *e)
{
    (void)e;
    E.fl ^= CN_RTR;
    editor_show();
}

static void len_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int n = E.len + d;
    if (n >= 0 && n <= 8) E.len = (uint8_t)n;
    editor_show();
}

static void type_digit(int h)
{
    if (s_sel < 0) {
        /* digits go in on the right and the oldest fall off the left: three
         * for an 11-bit id (one over 7FF is refused when sent), eight for 29 */
        uint32_t v = s_fresh ? 0 : E.id;
        E.id = (v << 4 | (uint32_t)h) & (E.fl & CN_EXT ? 0x1FFFFFFFu : 0xFFFu);
        s_fresh = false;
    } else {
        if (s_fresh) {
            E.data[s_sel] = (uint8_t)h;
            s_fresh = false;
        } else {
            E.data[s_sel] = (uint8_t)(E.data[s_sel] << 4 | h);
            s_fresh = true;
            if (s_sel < 7) s_sel++;
        }
        if (E.len < s_sel + (s_fresh ? 0 : 1)) E.len = (uint8_t)(s_sel + (s_fresh ? 0 : 1));
        if (E.len > 8) E.len = 8;
    }
}

/* the pad: 0..F, then backspace (16) and next field (17) */
static void pad_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == 16) {
        if (s_sel < 0) E.id >>= 4;
        else E.data[s_sel] = 0;
        s_fresh = s_sel >= 0;
    } else if (k == 17) {
        s_sel = s_sel < 7 ? s_sel + 1 : -1;
        s_fresh = true;
    } else {
        type_digit(k);
    }
    editor_show();
}

static bool check_frame(const cn_frame_t *f)
{
    if (!(f->fl & CN_EXT) && f->id > 0x7FF) {
        aos_ui_toast(_("Un id de 11 bits llega hasta 7FF: marcá 29 bits"), 2200);
        return false;
    }
    if (!CB.open) {
        aos_ui_toast(_("Conectá el bus primero"), 1800);
        return false;
    }
    if (CB.cfg.mode == CN_MODE_LISTEN) {
        aos_ui_toast(_("En solo escucha no sale nada: cambiá el modo"), 2200);
        return false;
    }
    return true;
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    if (check_frame(&E) && !cn_bus_send(&E)) aos_ui_toast(_("La cola de envío está llena"), 1500);
}

static void list_build(void);

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (!(E.fl & CN_EXT) && E.id > 0x7FF) {
        aos_ui_toast(_("Un id de 11 bits llega hasta 7FF: marcá 29 bits"), 2200);
        return;
    }
    if (CN_NSAVED >= CN_SAVED_MAX) {
        aos_ui_toast(_("La lista está llena"), 1500);
        return;
    }
    cn_saved_t *v = &CN_SAVED[CN_NSAVED++];
    memset(v, 0, sizeof *v);
    v->f = E;
    v->period_ms = PERIODS[s_period];
    cn_saved_save();
    list_build();
}

static void period_cb(lv_event_t *e)
{
    s_period = (int)lv_dropdown_get_selected(lv_event_get_target(e));
}

/* ---- the saved ones ---- */

static void saved_send_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < CN_NSAVED && check_frame(&CN_SAVED[i].f)) cn_bus_send(&CN_SAVED[i].f);
}

static void saved_load_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < CN_NSAVED) cn_send_load(&CN_SAVED[i].f);
}

static void run_show(int i)
{
    if (!S.run[i]) return;
    bool on = CN_SAVED[i].running;
    lv_label_set_text(lv_obj_get_child(S.run[i], 0), on ? AOS_SYM_STOP : AOS_SYM_PLAY);
    lv_obj_set_style_bg_color(S.run[i], on ? AOS_C_GREEN : AOS_C_CARD2, 0);
}

static void saved_run_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= CN_NSAVED) return;
    cn_saved_t *v = &CN_SAVED[i];
    if (v->running) {
        v->running = false;
        cn_periodic_set(i, NULL, 0, false);
    } else if (check_frame(&v->f)) {
        int on = 0;
        for (int k = 0; k < CN_NSAVED; k++) on += CN_SAVED[k].running;
        if (on >= CN_PERIODIC) {
            aos_ui_toast(_("Ya hay 8 periódicas andando"), 1800);
            return;
        }
        v->running = true;
        cn_periodic_set(i, &v->f, v->period_ms, true);
    }
    run_show(i);
}

static void saved_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= CN_NSAVED) return;
    /* the periodic ones are filed by their place in the list: move them along */
    for (int k = i; k < CN_NSAVED; k++)
        if (CN_SAVED[k].running) cn_periodic_set(k, NULL, 0, false);
    memmove(&CN_SAVED[i], &CN_SAVED[i + 1], sizeof(cn_saved_t) * (size_t)(CN_NSAVED - i - 1));
    CN_NSAVED--;
    for (int k = i; k < CN_NSAVED; k++)
        if (CN_SAVED[k].running) cn_periodic_set(k, &CN_SAVED[k].f, CN_SAVED[k].period_ms, true);
    cn_saved_save();
    list_build();
}

static void list_build(void)
{
    if (!S.list) return;
    lv_obj_clean(S.list);
    memset(S.run, 0, sizeof S.run);
    if (!CN_NSAVED) {
        lv_obj_t *l = aos_label(S.list, _("Sin tramas guardadas. Armá una arriba y tocá Guardar; también se "
                                          "editan desde el portal."), aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        return;
    }
    for (int i = 0; i < CN_NSAVED; i++) {
        cn_saved_t *v = &CN_SAVED[i];
        lv_obj_t *row = lv_obj_create(S.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 80);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *txt = lv_obj_create(row);
        lv_obj_remove_style_all(txt);
        lv_obj_set_height(txt, 72);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_add_flag(txt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(txt, saved_load_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char fr[40], sub[64];
        cn_fmt_candump(&v->f, fr, sizeof fr);
        lv_obj_t *a = aos_label(txt, fr, &aos_mono_22, AOS_C_TEXT);
        lv_obj_set_width(a, LV_PCT(100));
        lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(a, LV_ALIGN_TOP_LEFT, 0, 6);
        char per[32];
        if (v->period_ms) snprintf(per, sizeof per, _("cada %u ms"), (unsigned)v->period_ms);
        else snprintf(per, sizeof per, "%s", _("a mano"));
        if (v->name[0]) snprintf(sub, sizeof sub, "%s · %s", v->name, per);
        else snprintf(sub, sizeof sub, "%s", per);
        lv_obj_t *b = aos_label(txt, sub, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 0, -6);
        aos_make_decorative(a);
        aos_make_decorative(b);

        lv_obj_t *s = cn_btn(row, AOS_SYM_SEND, NULL, saved_send_cb, (void *)(intptr_t)i);
        lv_obj_set_height(s, 60);
        if (v->period_ms) {
            S.run[i] = cn_btn(row, AOS_SYM_PLAY, NULL, saved_run_cb, (void *)(intptr_t)i);
            lv_obj_set_height(S.run[i], 60);
            run_show(i);
        }
        lv_obj_t *d = cn_btn(row, AOS_SYM_DELETE, NULL, saved_del_cb, (void *)(intptr_t)i);
        lv_obj_set_height(d, 60);
    }
}

void cn_send_refresh(void)
{
    if (U.tab != CN_TAB_SEND || !S.hint) return;
    for (int i = 0; i < CN_NSAVED; i++) run_show(i);
    const char *h;
    lv_color_t c = AOS_C_ORANGE;
    if (!CB.open) h = _("Sin bus: conectalo arriba para mandar.");
    else if (CB.cfg.mode == CN_MODE_LISTEN) h = _("En solo escucha no sale nada: cambiá el modo en la conexión.");
    else if (CB.cfg.mode == CN_MODE_SELFTEST) { h = _("Autoprueba: lo que mandes vuelve sólo a esta placa."); c = AOS_C_DIM; }
    else { h = _("Normal: lo que mandes sale al bus."); c = AOS_C_GREEN; }
    lv_label_set_text(S.hint, h);
    lv_obj_set_style_text_color(S.hint, c, 0);
}

static lv_obj_t *field(lv_obj_t *parent, int32_t w, int32_t h, const lv_font_t *font, int sel)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, field_cb, LV_EVENT_CLICKED, (void *)(intptr_t)sel);
    lv_obj_center(aos_label(o, "", font, AOS_C_TEXT));
    return o;
}

static lv_obj_t *flex_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void cn_send_build(lv_obj_t *parent)
{
    memset(&S, 0, sizeof S);
    int32_t w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
    int32_t ew = U.land ? w * 11 / 20 : w;
    lv_obj_t *ed = lv_obj_create(parent);
    lv_obj_remove_style_all(ed);
    lv_obj_set_size(ed, ew, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ed, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ed, 10, 0);
    lv_obj_remove_flag(ed, LV_OBJ_FLAG_SCROLLABLE);

    S.hint = aos_label(ed, "", aos_font_caption, AOS_C_ORANGE);
    lv_obj_set_width(S.hint, LV_PCT(100));
    lv_label_set_long_mode(S.hint, LV_LABEL_LONG_MODE_DOTS);

    const int32_t fh = U.land ? 56 : 64;
    lv_obj_t *r1 = flex_row(ed);
    S.id = field(r1, 180, fh, &aos_mono_22, -1);
    S.ext = cn_pill(r1, _("29 bits"), false, ext_cb, NULL);
    S.rtr = cn_pill(r1, _("Remota"), false, rtr_cb, NULL);
    lv_obj_t *sp = lv_obj_create(r1);
    lv_obj_remove_style_all(sp);
    lv_obj_set_height(sp, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_t *mn = cn_btn(r1, AOS_SYM_MINUS, NULL, len_cb, (void *)(intptr_t)-1);
    lv_obj_set_height(mn, fh);
    S.len = aos_label(r1, "8", aos_font_body, AOS_C_TEXT);
    lv_obj_t *pl = cn_btn(r1, AOS_SYM_PLUS, NULL, len_cb, (void *)(intptr_t)1);
    lv_obj_set_height(pl, fh);

    lv_obj_t *r2 = flex_row(ed);
    lv_obj_set_style_pad_column(r2, 6, 0);
    int32_t bw = (ew - 7 * 6) / 8;
    for (int i = 0; i < 8; i++) S.bytes[i] = field(r2, bw, fh, &aos_mono_22, i);

    /* two rows of nine: 0-7 and backspace, 8-F and next */
    int32_t kw = (ew - 8 * 6) / 9, kh = U.land ? 54 : 64;
    for (int row = 0; row < 2; row++) {
        lv_obj_t *pr = flex_row(ed);
        lv_obj_set_style_pad_column(pr, 6, 0);
        for (int c = 0; c < 9; c++) {
            int k = c < 8 ? row * 8 + c : 16 + row;
            lv_obj_t *b = cn_btn(pr, c < 8 ? NULL : row ? AOS_SYM_ARROW_RIGHT_BOLD : AOS_SYM_BACKSPACE_OUTLINE, NULL,
                                 pad_cb, (void *)(intptr_t)k);
            lv_obj_set_size(b, kw, kh);
            lv_obj_set_style_pad_hor(b, 0, 0);
            lv_obj_set_style_radius(b, 12, 0);
            if (c < 8) {
                char t[2] = { "0123456789ABCDEF"[k], 0 };
                lv_obj_t *l = aos_label(b, t, &aos_mono_22, AOS_C_TEXT);
                aos_make_decorative(l);
            } else {
                lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A3E), 0);
            }
        }
    }

    lv_obj_t *r3 = flex_row(ed);
    lv_obj_t *sb = cn_btn(r3, AOS_SYM_SEND, _("Enviar"), send_cb, NULL);
    lv_obj_set_style_bg_color(sb, AOS_C_ACCENT, 0);
    lv_obj_set_flex_grow(sb, 1);
    S.period = lv_dropdown_create(r3);
    char opts[160] = "";
    for (size_t i = 0; i < N_PERIODS; i++) {
        char o[32];
        if (PERIODS[i]) snprintf(o, sizeof o, _("cada %u ms"), (unsigned)PERIODS[i]);
        else snprintf(o, sizeof o, "%s", _("a mano"));
        size_t k = strlen(opts);
        snprintf(opts + k, sizeof opts - k, "%s%s", i ? "\n" : "", o);
    }
    lv_dropdown_set_options(S.period, opts);
    lv_dropdown_set_selected(S.period, (uint32_t)s_period);
    lv_obj_set_size(S.period, 200, 72);
    lv_obj_set_style_text_font(S.period, aos_font_small, 0);
    lv_obj_set_style_bg_color(S.period, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(S.period, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(S.period, 0, 0);
    lv_obj_set_style_radius(S.period, 20, 0);
    lv_obj_t *dl = lv_dropdown_get_list(S.period);
    lv_obj_set_style_text_font(dl, aos_font_small, 0);
    lv_obj_set_style_bg_color(dl, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(dl, AOS_C_TEXT, 0);
    lv_obj_add_event_cb(S.period, period_cb, LV_EVENT_VALUE_CHANGED, NULL);
    cn_btn(r3, AOS_SYM_PLUS, _("Guardar"), save_cb, NULL);

    lv_obj_update_layout(ed);
    int32_t eh = lv_obj_get_height(ed);
    S.list = lv_obj_create(parent);
    lv_obj_remove_style_all(S.list);
    if (U.land) {
        lv_obj_set_pos(S.list, ew + 16, 0);
        lv_obj_set_size(S.list, w - ew - 16, h);
    } else {
        lv_obj_set_pos(S.list, 0, eh + 16);
        lv_obj_set_size(S.list, w, h - eh - 16);
    }
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 8, 0);
    lv_obj_set_scroll_dir(S.list, LV_DIR_VER);

    editor_show();
    list_build();
    cn_send_refresh();
}
