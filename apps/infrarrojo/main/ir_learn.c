/*
 * P4OS - Infrarrojo: learning a button.
 *
 * While the tab is in front the receiver listens. Each capture is decoded
 * (ir_proto.c) and drawn; pressing the same button again confirms it - the
 * same protocol, address and command, or for a raw frame 85 % of its
 * durations within tolerance - and three in a row make it sure. A capture
 * that disagrees starts over with itself. A NEC repeat code alone (a key
 * held after its frame was missed) is not a button and is skipped.
 *
 * Learning for a button (from a remote's edit sheet) saves on the third
 * confirmation and goes back to the remote; otherwise the capture is saved
 * from here into any device, or a new one, or searched for in the library.
 *
 * The carrier cannot be measured: the receiver takes it away. What is shown
 * is the protocol's, and 38 kHz for a raw frame, the most common one.
 */
#include "ir.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

#define HIST    8
#define CONFIRM 3

typedef struct {
    uint32_t *d;
    int       n;
    ir_code_t code;
    bool      known;
    uint32_t  seq;
} take_t;

static struct {
    lv_obj_t *page;
    lv_obj_t *dot, *status, *pause_lbl, *target_card, *target_lbl;
    lv_obj_t *res, *title, *desc, *freq, *conf, *wave, *use_btn;
    lv_obj_t *hist_card;
    bool      shown, paused;
    uint32_t  seen;
    ir_capture_t *cap;
    take_t    cand;
    int       confirm;
    take_t    hist[HIST];
    int       nhist;
    uint32_t  blink_ms;
    /* the save sheet */
    lv_obj_t *ta_name, *dd_role, *dd_dev, *ta_new;
} L;

static char s_target[48];
static int  s_target_btn = -1;

static void refresh(void);

/* --------------------------------------------------------------- the wave */

typedef struct {
    uint32_t *d;
    int n;
} wave_t;

static void wave_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    wave_t *w = lv_obj_get_user_data(o);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t W = lv_area_get_width(&a), H = lv_area_get_height(&a);
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = AOS_C_CARD2;
    ln.width = 2;
    ln.p1.x = a.x1; ln.p2.x = a.x2;
    ln.p1.y = ln.p2.y = a.y2 - 2;
    lv_draw_line(layer, &ln);
    if (!w || w->n <= 0) return;
    /* long gaps between frames drawn short, with a break */
    const int32_t GAPW = 18;
    uint32_t total = 0;      /* 32 bits: no 64-bit to float in the firmware's table */
    int gaps = 0;
    for (int i = 0; i < w->n; i++) {
        if ((i & 1) && w->d[i] >= IR_GAP_US) gaps++;
        else total += w->d[i];
    }
    if (!total) return;
    float scale = (float)(W - gaps * GAPW) / (float)total;
    float x = (float)a.x1;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = AOS_C_RED;
    r.bg_opa = LV_OPA_COVER;
    for (int i = 0; i < w->n; i++) {
        if ((i & 1) && w->d[i] >= IR_GAP_US) {
            int32_t gx = (int32_t)x + GAPW / 2;
            ln.color = AOS_C_DIM;
            ln.width = 2;
            ln.p1.x = gx - 4; ln.p1.y = a.y1 + H / 3;
            ln.p2.x = gx + 2; ln.p2.y = a.y2 - H / 3;
            lv_draw_line(layer, &ln);
            x += GAPW;
            continue;
        }
        float wpx = (float)w->d[i] * scale;
        if (!(i & 1)) {
            lv_area_t m = { (int32_t)x, a.y1 + 8, (int32_t)(x + (wpx < 1 ? 1 : wpx)) - 1, a.y2 - 2 };
            if (m.x2 < m.x1) m.x2 = m.x1;
            lv_draw_rect(layer, &r, &m);
        }
        x += wpx;
    }
}

static void wave_free(lv_event_t *e)
{
    wave_t *w = lv_obj_get_user_data(lv_event_get_target(e));
    if (w) { ir_free(w->d); ir_free(w); }
}

lv_obj_t *ir_wave_create(lv_obj_t *parent, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), h);
    lv_obj_set_user_data(o, ir_alloc(sizeof(wave_t)));
    lv_obj_add_event_cb(o, wave_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, wave_free, LV_EVENT_DELETE, NULL);
    return o;
}

void ir_wave_set(lv_obj_t *o, const uint32_t *d, int n)
{
    wave_t *w = lv_obj_get_user_data(o);
    if (!w) return;
    ir_free(w->d);
    w->d = NULL;
    w->n = 0;
    if (n > 0 && (w->d = ir_alloc((size_t)n * sizeof *d))) {
        memcpy(w->d, d, (size_t)n * sizeof *d);
        w->n = n;
    }
    lv_obj_invalidate(o);
}

/* ------------------------------------------------------------ the takes */

static void take_free(take_t *t)
{
    ir_free(t->d);
    memset(t, 0, sizeof *t);
}

static void take_copy(take_t *dst, const take_t *src)
{
    take_free(dst);
    *dst = *src;
    dst->d = NULL;
    if (src->n && (dst->d = ir_alloc((size_t)src->n * sizeof *src->d)))
        memcpy(dst->d, src->d, (size_t)src->n * sizeof *src->d);
    else dst->n = 0;
}

static bool same(const take_t *a, const take_t *b)
{
    if (!a->n || !b->n || a->known != b->known) return false;
    if (a->known)
        return !strcmp(a->code.proto, b->code.proto) && a->code.addr == b->code.addr &&
               a->code.cmd == b->code.cmd &&
               /* extra is RC5's and RC6's toggle, which flips on every press */
               (strcmp(a->code.proto, "Kaseikyo") || a->code.extra == b->code.extra);
    float s = ir_similarity(a->d, a->n, b->d, b->n);
    float s1 = ir_similarity(a->d, ir_frame_len(a->d, a->n), b->d, ir_frame_len(b->d, b->n));
    return (s > s1 ? s : s1) >= 0.85f;
}

static uint32_t take_carrier(const take_t *t)
{
    const ir_proto_t *p = t->known ? ir_proto_find(t->code.proto) : NULL;
    return p ? p->carrier : 38000;
}

/* the take into a button */
static void take_into(const take_t *t, ir_button_t *b)
{
    ir_button_clear(b);
    memset(&b->code, 0, sizeof b->code);
    if (t->known) b->code = t->code;
    else ir_button_set_raw(b, t->d, t->n, 38000);
}

static void target_done(bool saved)
{
    if (!saved && s_target[0]) {
        /* a button made only to be learned, and not learned: out again */
        ir_dev_t *d = ir_remote_find(s_target);
        if (d && s_target_btn >= 0 && s_target_btn < d->nbtn) {
            ir_button_t *b = &d->btn[s_target_btn];
            if (!ir_proto_find(b->code.proto) && !b->nraw) {
                ir_dev_remove_button(d, s_target_btn);
                ir_remote_saved(s_target);
            }
        }
    }
    s_target[0] = 0;
    s_target_btn = -1;
}

static bool save_target(void)
{
    ir_dev_t *d = ir_remote_find(s_target);
    if (!d || s_target_btn < 0 || s_target_btn >= d->nbtn || !L.cand.n) return false;
    take_into(&L.cand, &d->btn[s_target_btn]);
    char t[96];
    snprintf(t, sizeof t, _("Aprendido: %s"), d->btn[s_target_btn].name);
    char file[48];
    ir_copy(file, sizeof file, s_target);
    target_done(true);
    ir_remote_saved(file);
    ir_ui_toast(t);
    ir_ui_goto_tab(0);
    return true;
}

static void got(const ir_capture_t *c)
{
    take_t t = { 0 };
    t.known = ir_decode(c->d, c->n, &t.code);
    if (t.known && t.code.repeat) return;
    /* a known protocol keeps its first frame; raw keeps everything */
    int n = t.known ? ir_frame_len(c->d, c->n) : c->n;
    t.d = (uint32_t *)c->d;
    t.n = n;
    t.seq = c->seq;
    if (same(&t, &L.cand)) {
        if (L.confirm < CONFIRM) L.confirm++;
    } else {
        take_copy(&L.cand, &t);
        L.confirm = 1;
    }
    /* the history, newest first */
    take_free(&L.hist[HIST - 1]);
    memmove(&L.hist[1], &L.hist[0], (HIST - 1) * sizeof L.hist[0]);
    memset(&L.hist[0], 0, sizeof L.hist[0]);
    take_copy(&L.hist[0], &t);
    if (L.nhist < HIST) L.nhist++;
    L.blink_ms = (uint32_t)aos_hal_uptime_ms();
    if (s_target[0] && L.confirm >= CONFIRM && save_target()) return;
    refresh();
}

/* ------------------------------------------------------------- the sheet */

static void sheet_cancel(lv_event_t *e) { (void)e; ir_sheet_close(); }

static void sheet_save(lv_event_t *e)
{
    (void)e;
    int n;
    ir_dev_t *devs = ir_remote_devices(&n);
    int sel = (int)lv_dropdown_get_selected(L.dd_dev);
    const ir_role_t *r = ir_role_at((int)lv_dropdown_get_selected(L.dd_role));
    const char *name = lv_textarea_get_text(L.ta_name);
    if (!name[0]) name = r && r->role[0] ? _(r->label) : _("Botón");
    char file[48];
    if (sel >= n) {
        /* a new device */
        ir_dev_t d;
        memset(&d, 0, sizeof d);
        const char *dn = lv_textarea_get_text(L.ta_new);
        ir_copy(d.name, sizeof d.name, dn[0] ? dn : _("Aparato nuevo"));
        d.kind = IR_K_TV;
        ir_button_t *b = ir_dev_add_button(&d);
        if (b) {
            ir_copy(b->name, sizeof b->name, name);
            ir_copy(b->role, sizeof b->role, r ? r->role : "");
            take_into(&L.cand, b);
        }
        bool ok = ir_dev_save(&d);
        ir_copy(file, sizeof file, d.file);
        ir_dev_clear(&d);
        if (!ok) { ir_ui_toast(_("No se pudo escribir en la tarjeta")); return; }
    } else {
        ir_dev_t *d = &devs[sel];
        ir_button_t *b = ir_dev_add_button(d);
        if (!b) return;
        ir_copy(b->name, sizeof b->name, name);
        ir_copy(b->role, sizeof b->role, r ? r->role : "");
        take_into(&L.cand, b);
        ir_copy(file, sizeof file, d->file);
        ir_dev_save(d);
    }
    ir_sheet_close();
    ir_remote_open_file(file);
    char t[96];
    snprintf(t, sizeof t, _("Guardado: %s"), name);
    ir_ui_toast(t);
}

static void dev_changed(lv_event_t *e)
{
    (void)e;
    int n;
    ir_remote_devices(&n);
    bool is_new = (int)lv_dropdown_get_selected(L.dd_dev) >= n;
    if (is_new) lv_obj_remove_flag(L.ta_new, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(L.ta_new, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *dropdown(lv_obj_t *parent, const char *opts)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_width(dd, LV_PCT(100));
    lv_obj_set_style_text_font(dd, aos_font_body, 0);
    lv_obj_set_style_bg_color(dd, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(dd, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_radius(dd, 18, 0);
    lv_obj_set_style_pad_all(dd, 18, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, aos_font_body, 0);
    lv_obj_set_style_bg_color(list, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(list, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_max_height(list, 520, 0);
    return dd;
}

static void label(lv_obj_t *s, const char *t)
{
    lv_obj_t *l = lv_label_create(s);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (!L.cand.n) return;
    if (s_target[0]) { save_target(); return; }
    lv_obj_t *s = ir_sheet_open(_("Guardar el botón"));
    L.ta_name = ir_sheet_text(s, _("Nombre"), "", 39);
    label(s, _("Función en el control"));
    ir_sb_t o = { 0 };
    for (int i = 0; i < ir_role_count(); i++) {
        if (i) ir_sb_put(&o, "\n", 1);
        ir_sb_put(&o, _(ir_role_at(i)->label), -1);
    }
    L.dd_role = dropdown(s, o.s);
    ir_sb_free(&o);
    label(s, _("Aparato"));
    int n;
    ir_dev_t *devs = ir_remote_devices(&n);
    o = (ir_sb_t){ 0 };
    for (int i = 0; i < n; i++) {
        ir_sb_put(&o, devs[i].name, -1);
        ir_sb_put(&o, "\n", 1);
    }
    ir_sb_put(&o, _("Aparato nuevo…"), -1);
    L.dd_dev = dropdown(s, o.s);
    ir_sb_free(&o);
    lv_obj_add_event_cb(L.dd_dev, dev_changed, LV_EVENT_VALUE_CHANGED, NULL);
    L.ta_new = ir_sheet_text(s, NULL, "", 40);
    lv_textarea_set_placeholder_text(L.ta_new, _("Nombre del aparato nuevo"));
    if (n) lv_obj_add_flag(L.ta_new, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *r = ir_sheet_buttons(s);
    ir_ui_btn(r, _("Cancelar"), AOS_C_CARD2, sheet_cancel, NULL);
    ir_ui_btn(r, _("Guardar"), AOS_C_GREEN, sheet_save, NULL);
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    if (!L.cand.n) return;
    ir_button_t b;
    memset(&b, 0, sizeof b);
    take_into(&L.cand, &b);
    ir_hw_send_button(&b, false);
    ir_button_clear(&b);
}

static void find_cb(lv_event_t *e)
{
    (void)e;
    if (!L.cand.n) return;
    if (!ir_pack_open()) { ir_ui_toast(_("Falta la base de códigos")); return; }
    ir_base_find(L.cand.d, L.cand.n);
}

static void pause_cb(lv_event_t *e)
{
    (void)e;
    L.paused = !L.paused;
    ir_hw_listen(IR_WHO_UI, L.shown && !L.paused);
    refresh();
}

static void cancel_target_cb(lv_event_t *e)
{
    (void)e;
    target_done(false);
    refresh();
    ir_ui_goto_tab(0);
}

static void hist_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= L.nhist) return;
    take_copy(&L.cand, &L.hist[i]);
    L.confirm = 1;
    refresh();
}

/* -------------------------------------------------------------- drawing */

static lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static void take_title(const take_t *t, char *out, size_t n)
{
    if (t->known) ir_code_text(&t->code, out, n);
    else snprintf(out, n, "%s", _("Crudo (protocolo desconocido)"));
}

static void refresh(void)
{
    if (!L.page) return;
    bool listening = L.shown && !L.paused;
    lv_label_set_text(L.status, L.paused ? _("En pausa.") :
                      _("Escuchando. Apuntá el control al receptor y apretá un botón."));
    lv_label_set_text(L.pause_lbl, L.paused ? _("Escuchar") : _("Pausar"));
    lv_obj_set_style_bg_color(L.dot, listening ? AOS_C_RED : AOS_C_CARD2, 0);

    if (s_target[0]) {
        ir_dev_t *d = ir_remote_find(s_target);
        const char *bn = (d && s_target_btn >= 0 && s_target_btn < d->nbtn) ? d->btn[s_target_btn].name : "?";
        char t[128];
        snprintf(t, sizeof t, _("Aprendiendo «%s» de %s: apretalo %d veces."), bn, d ? d->name : "?", CONFIRM);
        lv_label_set_text(L.target_lbl, t);
        lv_obj_remove_flag(L.target_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(L.target_card, LV_OBJ_FLAG_HIDDEN);
    }

    if (!L.cand.n) {
        lv_obj_add_flag(L.res, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(L.res, LV_OBJ_FLAG_HIDDEN);
        char t[200];
        take_title(&L.cand, t, sizeof t);
        lv_label_set_text(L.title, t);
        ir_describe(L.cand.d, L.cand.n, t, sizeof t);
        lv_label_set_text(L.desc, t);
        uint32_t f = take_carrier(&L.cand);
        if (L.cand.known)
            snprintf(t, sizeof t, _("Portadora: %u,%u kHz, la del protocolo (el receptor no la mide)."),
                     (unsigned)(f / 1000), (unsigned)(f % 1000 / 100));
        else
            snprintf(t, sizeof t, "%s", _("Portadora: probablemente 38 kHz (el receptor no la mide)."));
        lv_label_set_text(L.freq, t);
        if (L.confirm >= CONFIRM) snprintf(t, sizeof t, _("Confirmado: %d veces igual."), L.confirm);
        else snprintf(t, sizeof t, _("%d de %d: apretá el mismo botón otra vez para confirmar."), L.confirm, CONFIRM);
        lv_label_set_text(L.conf, t);
        lv_obj_set_style_text_color(L.conf, L.confirm >= CONFIRM ? AOS_C_GREEN : AOS_C_ORANGE, 0);
        ir_wave_set(L.wave, L.cand.d, L.cand.n);
        lv_label_set_text(lv_obj_get_child(L.use_btn, 0), s_target[0] ? _("Usar esta") : _("Guardar en un aparato"));
    }

    lv_obj_clean(L.hist_card);
    if (L.nhist) {
        text(L.hist_card, _("Últimas capturas"), aos_font_caption, AOS_C_DIM);
        for (int i = 0; i < L.nhist; i++) {
            char t[120], sub[48];
            take_title(&L.hist[i], t, sizeof t);
            /* the # outside: a catalog line that starts with it is a comment */
            int k = snprintf(sub, sizeof sub, "#%u · ", (unsigned)L.hist[i].seq);
            snprintf(sub + k, sizeof sub - k, _("%d duraciones"), L.hist[i].n);
            lv_obj_t *r = ir_ui_row(L.hist_card, NULL, AOS_C_CARD, t, sub, hist_cb, (void *)(intptr_t)i);
            lv_obj_set_style_bg_color(r, AOS_C_CARD2, 0);
        }
        lv_obj_remove_flag(L.hist_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(L.hist_card, LV_OBJ_FLAG_HIDDEN);
    }
}

void ir_learn_build(lv_obj_t *page)
{
    L.page = page;
    if (!L.cap) L.cap = ir_alloc(sizeof *L.cap);
    ir_ui_header(page, _("Aprender"), NULL, NULL, NULL);
    lv_obj_t *s = ir_ui_scroll(page);
    bool land = ir_ui_landscape();
    if (land) {
        lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(s, 18, 0);
    }
    int32_t cw = land ? (ir_ui_width() - 2 * AOS_UI_PAD - 18) / 2 : ir_ui_width() - 2 * AOS_UI_PAD;

    lv_obj_t *left = s;
    if (land) {
        left = lv_obj_create(s);
        lv_obj_remove_style_all(left);
        lv_obj_set_size(left, cw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 18, 0);
    }

    L.target_card = ir_ui_card(left);
    lv_obj_set_style_bg_color(L.target_card, lv_color_hex(0x10284A), 0);
    L.target_lbl = text(L.target_card, "", aos_font_body, AOS_C_TEXT);
    ir_ui_btn(L.target_card, _("Cancelar"), AOS_C_CARD2, cancel_target_cb, NULL);

    lv_obj_t *c = ir_ui_card(left);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 20, 0);
    L.dot = lv_obj_create(c);
    lv_obj_remove_style_all(L.dot);
    lv_obj_set_size(L.dot, 36, 36);
    lv_obj_set_style_radius(L.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(L.dot, LV_OPA_COVER, 0);
    L.status = lv_label_create(c);
    lv_label_set_long_mode(L.status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_flex_grow(L.status, 1);
    lv_obj_set_style_text_font(L.status, aos_font_small, 0);
    lv_obj_set_style_text_color(L.status, AOS_C_TEXT, 0);
    lv_obj_t *pb = ir_ui_btn(c, _("Pausar"), AOS_C_CARD2, pause_cb, NULL);
    lv_obj_set_style_min_width(pb, 150, 0);
    L.pause_lbl = lv_obj_get_child(pb, 0);

    L.res = ir_ui_card(left);
    L.title = text(L.res, "", aos_font_body, AOS_C_TEXT);
    L.conf = text(L.res, "", aos_font_small, AOS_C_ORANGE);
    L.wave = ir_wave_create(L.res, 110);
    L.desc = text(L.res, "", aos_font_caption, AOS_C_DIM);
    L.freq = text(L.res, "", aos_font_caption, AOS_C_DIM);
    lv_obj_t *r = ir_sheet_buttons(L.res);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    L.use_btn = ir_ui_btn(r, _("Guardar en un aparato"), AOS_C_GREEN, save_cb, NULL);
    ir_ui_btn(r, _("Mandar"), AOS_C_CARD2, send_cb, NULL);
    ir_ui_btn(r, _("¿De qué control es?"), AOS_C_CARD2, find_cb, NULL);

    L.hist_card = ir_ui_card(s);
    lv_obj_set_width(L.hist_card, cw);
    lv_obj_set_style_pad_row(L.hist_card, 10, 0);
    refresh();
}

void ir_learn_target(const char *file, int button)
{
    if (s_target[0] && strcmp(s_target, file)) target_done(false);
    ir_copy(s_target, sizeof s_target, file);
    s_target_btn = button;
    /* start clean: the confirmations are for this button */
    take_free(&L.cand);
    L.confirm = 0;
    L.paused = false;
    refresh();
}

void ir_learn_shown(bool shown)
{
    if (shown && !L.shown) L.seen = ir_hw_capture_seq();
    L.shown = shown;
    ir_hw_listen(IR_WHO_UI, shown && !L.paused);
    if (L.page) refresh();
}

void ir_learn_tick(void)
{
    if (!L.page || !L.shown || !L.cap) return;
    if (ir_hw_capture(&L.seen, L.cap)) got(L.cap);
    /* the dot blinks with each capture */
    if (L.dot && !L.paused) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        bool flash = now - L.blink_ms < 300;
        lv_obj_set_style_bg_color(L.dot, flash ? AOS_C_GREEN : AOS_C_RED, 0);
    }
}

void ir_learn_free(void)
{
    ir_hw_listen(IR_WHO_UI, false);
    /* the takes survive a turn of the screen; only the objects go */
    L.page = NULL;
    L.dot = L.status = L.pause_lbl = L.target_card = L.target_lbl = NULL;
    L.res = L.title = L.desc = L.freq = L.conf = L.wave = L.use_btn = L.hist_card = NULL;
    L.shown = false;
}
