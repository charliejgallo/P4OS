/*
 * P4OS - Mi casa: the editor.
 *
 * A new service starts from a template (Home Assistant, Frigate, a NAS, a
 * 3D printer, the router, a web page, any TCP port), which fills in the
 * address to change, the port, the path, the code and the word that tell
 * it is up; then the name, the group and how often. Typing addresses on 5"
 * works, but the portal's page is the comfortable place: the editor says
 * so.
 *
 * What is typed is kept in a draft (the screen may turn and be built
 * again); nothing is written until Guardar.
 */
#include "ec.h"

#include <stdlib.h>

#define A ec_app

static const int EVERY[] = { 10, 30, 60, 300, 900 };
#define N_EVERY (int)(sizeof EVERY / sizeof EVERY[0])

enum { F_NAME, F_GROUP, F_URL, F_HOST, F_PORT, F_EXPECT, F_WORD, F_SLOW, F_CERT, F_N };

static struct {
    int       i;                /* -1 new */
    ec_svc_t  d;                /* the draft */
    bool      picked;           /* a new one: a template was chosen */
    lv_obj_t *ta[F_N];
    lv_obj_t *kb, *form;
    lv_obj_t *kind_b[2], *every_b[N_EVERY];
    bool      del_armed;
    int32_t   form_h;
} E;

/* ---- the draft ---------------------------------------------------------------------------------- */

static void take(void)
{
    for (int f = 0; f < F_N; f++) {
        if (!E.ta[f]) continue;
        const char *v = lv_textarea_get_text(E.ta[f]);
        switch (f) {
        case F_NAME: ec_copy(E.d.name, sizeof E.d.name, v); break;
        case F_GROUP: ec_copy(E.d.group, sizeof E.d.group, v); break;
        case F_URL: ec_copy(E.d.url, sizeof E.d.url, v); break;
        case F_HOST: ec_copy(E.d.host, sizeof E.d.host, v); break;
        case F_PORT: E.d.port = atoi(v); break;
        case F_EXPECT: ec_copy(E.d.expect, sizeof E.d.expect, v); break;
        case F_WORD: ec_copy(E.d.word, sizeof E.d.word, v); break;
        case F_SLOW: E.d.slow_ms = atoi(v); break;
        case F_CERT: E.d.cert_days = atoi(v); break;
        }
    }
}

void ec_edit_open(int i)
{
    memset(&E, 0, sizeof E);
    E.i = i >= 0 && i < A.cfg.n ? i : -1;
    if (E.i >= 0) {
        E.d = A.cfg.s[E.i];
        E.picked = true;
    }
}

void ec_edit_keep(void)
{
    take();
}

void ec_edit_close(void)
{
    memset(E.ta, 0, sizeof E.ta);
    E.kb = E.form = NULL;
}

bool ec_edit_back(void)
{
    if (E.kb && !lv_obj_has_flag(E.kb, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(E.form, E.form_h);
        return true;
    }
    if (E.i < 0 && E.picked) {          /* back to the templates */
        E.picked = false;
        ec_edit_build();
        return true;
    }
    return false;
}

/* ---- checking and saving ------------------------------------------------------------------------- */

static const char *check(void)
{
    static char msg[120];
    const ec_svc_t *s = &E.d;
    if (!s->name[0]) return _("Falta el nombre");
    if (strchr(s->name, '[') || strchr(s->name, ']')) return _("El nombre no puede llevar corchetes");
    for (int k = 0; k < A.cfg.n; k++)
        if (k != E.i && !strcasecmp(A.cfg.s[k].name, s->name)) return _("Ya hay un servicio con ese nombre");
    if (s->kind == EC_HTTP) {
        char host[EC_HOST], path[EC_URL];
        int port;
        bool tls;
        if (strstr(s->url, "://:") || (strstr(s->url, "://") && !strstr(s->url, "://")[3]))
            return _("Falta la IP o el nombre en la dirección");
        if (!ec_url_split(s->url, host, sizeof host, &port, path, sizeof path, &tls))
            return _("La dirección tiene que empezar con http:// o https://");
        if (!s->expect[0] || !ec_expect_ok(s->expect, atoi(s->expect))) {
            snprintf(msg, sizeof msg, "%s", _("El código esperado: 200, 200-399 o 200,401"));
            return msg;
        }
    } else {
        if (!s->host[0] || strchr(s->host, ' ') || strchr(s->host, '/')) return _("Falta la dirección (IP o nombre)");
        if (s->port <= 0 || s->port > 65535) return _("El puerto va de 1 a 65535");
    }
    return NULL;
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    take();
    const char *bad = check();
    if (bad) {
        aos_ui_toast(bad, 2500);
        return;
    }
    if (E.d.slow_ms < 10) E.d.slow_ms = E.d.kind == EC_TCP ? 200 : 800;
    if (!E.d.timeout_ms) E.d.timeout_ms = 5000;
    int at = E.i;
    if (at < 0) {
        if (A.cfg.n >= EC_MAX) return;
        at = A.cfg.n++;
    }
    A.cfg.s[at] = E.d;
    if (!ec_cfg_commit()) aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2500);
    ec_edit_close();
    ec_go_detail(at);
}

static void del_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    if (!E.del_armed) {
        E.del_armed = true;
        ec_set_text(lv_obj_get_child(b, 1), _("Tocá de nuevo para borrar"));
        return;
    }
    if (E.i < 0 || E.i >= A.cfg.n) return;
    char path[200];
    ec_cert_pin_path(ec_svc_key(&A.cfg.s[E.i]), path, sizeof path);
    unlink(path);
    memmove(&A.cfg.s[E.i], &A.cfg.s[E.i + 1], sizeof A.cfg.s[0] * (size_t)(A.cfg.n - E.i - 1));
    A.cfg.n--;
    ec_cfg_commit();
    ec_edit_close();
    ec_go_list();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    if (!ec_edit_back()) {
        ec_edit_close();
        if (E.i >= 0) ec_go_detail(E.i);
        else ec_go_list();
    }
}

/* ---- the keyboard -------------------------------------------------------------------------------- */

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
        lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(E.form, E.form_h);
    }
}

static void ta_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED || !E.kb) return;
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    bool num = f == F_PORT || f == F_SLOW || f == F_CERT;
    lv_keyboard_set_mode(E.kb, num ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(E.kb, ta);
    lv_obj_remove_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(E.form, E.form_h - lv_obj_get_height(E.kb));
    lv_obj_update_layout(E.form);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
}

/* ---- the form ------------------------------------------------------------------------------------- */

static lv_obj_t *field(lv_obj_t *parent, int f, const char *label, const char *val, const char *hint, int32_t w)
{
    lv_obj_t *col = ec_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    ec_text(col, label, aos_font_caption, AOS_C_DIM, w);
    lv_obj_t *ta = lv_textarea_create(col);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, w);
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 16, 0);
    lv_obj_set_style_pad_all(ta, 16, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_textarea_set_text(ta, val);
    if (hint) lv_textarea_set_placeholder_text(ta, hint);
    if (f == F_PORT || f == F_SLOW || f == F_CERT) lv_textarea_set_accepted_chars(ta, "0123456789");
    lv_obj_add_event_cb(ta, ta_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)f);
    E.ta[f] = ta;
    return col;
}

static void seg_paint(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, on ? AOS_C_TEXT : AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
}

static lv_obj_t *seg_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int v)
{
    lv_obj_t *b = ec_box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return b;
}

static lv_obj_t *seg_row(lv_obj_t *parent, const char *label, int32_t w)
{
    lv_obj_t *col = ec_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    ec_text(col, label, aos_font_caption, AOS_C_DIM, w);
    lv_obj_t *r = ec_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_style_pad_row(r, 10, 0);
    return r;
}

static void kind_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == E.d.kind) return;
    take();
    /* carry the host over */
    if (k == EC_TCP) {
        char host[EC_HOST], path[EC_URL];
        int port;
        bool tls;
        if (ec_url_split(E.d.url, host, sizeof host, &port, path, sizeof path, &tls)) ec_copy(E.d.host, sizeof E.d.host, host);
        if (!E.d.port || E.d.port == 80 || E.d.port == 443) E.d.port = 22;
        if (E.d.slow_ms == 800) E.d.slow_ms = 200;
    } else {
        if (!E.d.url[0]) snprintf(E.d.url, sizeof E.d.url, "http://%.96s/", E.d.host);
        if (!E.d.expect[0]) ec_copy(E.d.expect, sizeof E.d.expect, "200-399");
        if (E.d.slow_ms == 200) E.d.slow_ms = 800;
    }
    E.d.kind = (uint8_t)k;
    ec_edit_build();
}

static void every_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    E.d.every_s = EVERY[k];
    for (int j = 0; j < N_EVERY; j++) seg_paint(E.every_b[j], j == k);
}

static void tmpl_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    ec_svc_from_tmpl(&E.d, t, NULL);
    /* "NAS", "NAS 2": a name not taken */
    char base[EC_NAME];
    ec_copy(base, sizeof base, E.d.name);
    for (int n = 2; n < 100; n++) {
        bool taken = false;
        for (int k = 0; k < A.cfg.n; k++) taken |= !strcasecmp(A.cfg.s[k].name, E.d.name);
        if (!taken) break;
        snprintf(E.d.name, sizeof E.d.name, "%.30s %d", base, n);
    }
    E.picked = true;
    ec_edit_build();
}

static void build_templates(lv_obj_t *page)
{
    lv_obj_t *col = ec_box(page, A.W, A.H - (A.land ? 100 : 120));
    lv_obj_set_y(col, A.land ? 100 : 120);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(col, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_column(col, 16, 0);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_set_style_pad_bottom(col, 40, 0);
    int32_t inner = A.W - 2 * AOS_UI_PAD;
    int cols = A.land ? 3 : 2;
    int32_t w = (inner - (cols - 1) * 16) / cols;
    for (int t = 0; t < ec_tmpl_n(); t++) {
        const ec_tmpl_t *m = ec_tmpl(t);
        lv_obj_t *c = ec_card(col, w, 150);
        lv_obj_set_style_radius(c, 24, 0);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, tmpl_cb, LV_EVENT_CLICKED, (void *)(intptr_t)t);
        lv_obj_t *g = aos_label(c, m->glyph, &aos_sym_44, AOS_C_TEAL);
        lv_obj_set_pos(g, 22, 18);
        aos_make_decorative(g);
        lv_obj_t *n = ec_text(c, _(m->label), aos_font_body, AOS_C_TEXT, w - 44);
        lv_obj_set_pos(n, 22, 76);
        char b[64];
        if (m->kind == EC_TCP) snprintf(b, sizeof b, _("puerto TCP %d"), m->port);
        else snprintf(b, sizeof b, "%s :%d%s", m->tls ? "https" : "http", m->port, m->path);
        lv_obj_t *p = ec_text(c, b, aos_font_caption, AOS_C_DIM, w - 44);
        lv_obj_set_pos(p, 22, 112);
    }
    lv_obj_t *hint = aos_label(col, _("Las direcciones se cargan más cómodo desde la página Mi casa del portal."),
                               aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(hint, inner);
}

void ec_edit_build(void)
{
    lv_obj_clean(A.root);
    A.W = lv_obj_get_width(A.root);
    A.H = lv_obj_get_height(A.root);
    A.land = A.W > A.H;
    memset(E.ta, 0, sizeof E.ta);
    E.kb = E.form = NULL;
    E.del_armed = false;
    lv_obj_t *page = ec_box(A.root, A.W, A.H);
    lv_obj_t *bk = ec_round_btn(page, AOS_SYM_CLOSE, cancel_cb, NULL);
    lv_obj_set_pos(bk, AOS_UI_PAD - 8, A.land ? 8 : 16);
    const char *title = E.i >= 0 ? _("Editar servicio") : E.picked ? _("Servicio nuevo") : _("¿Qué querés vigilar?");
    lv_obj_t *t = ec_text(page, title, aos_font_title, AOS_C_TEXT, A.W - 2 * AOS_UI_PAD - AOS_UI_TAP_MIN - 200);
    lv_obj_set_pos(t, AOS_UI_PAD + AOS_UI_TAP_MIN + 8, A.land ? 26 : 34);
    if (E.i < 0 && !E.picked) {
        build_templates(page);
        return;
    }
    lv_obj_t *sv = ec_pill(page, AOS_SYM_CHECK, _("Guardar"), AOS_C_ACCENT, save_cb, NULL);
    lv_obj_align(sv, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, A.land ? 12 : 20);

    int32_t top = A.land ? 100 : 120;
    E.form_h = A.H - top;
    E.form = ec_box(page, A.W, E.form_h);
    lv_obj_set_y(E.form, top);
    lv_obj_add_flag(E.form, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(E.form, LV_DIR_VER);
    lv_obj_set_flex_flow(E.form, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(E.form, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_column(E.form, 24, 0);
    lv_obj_set_style_pad_row(E.form, 18, 0);
    lv_obj_set_style_pad_bottom(E.form, 60, 0);
    int32_t inner = A.W - 2 * AOS_UI_PAD;
    int32_t half = A.land ? (inner - 24) / 2 : inner;
    char num[16];

    field(E.form, F_NAME, _("Nombre"), E.d.name, NULL, half);
    field(E.form, F_GROUP, _("Grupo"), E.d.group, _("Casa, Red, Cámaras..."), half);
    lv_obj_t *r = seg_row(E.form, _("Cómo se prueba"), inner);
    E.kind_b[0] = seg_btn(r, _("Pedido HTTP(S)"), kind_cb, EC_HTTP);
    E.kind_b[1] = seg_btn(r, _("Puerto TCP abierto"), kind_cb, EC_TCP);
    seg_paint(E.kind_b[0], E.d.kind == EC_HTTP);
    seg_paint(E.kind_b[1], E.d.kind == EC_TCP);
    if (E.d.kind == EC_HTTP) {
        field(E.form, F_URL, _("Dirección"), E.d.url, "http://10.20.30.40:8123/", inner);
        field(E.form, F_EXPECT, _("Códigos que valen"), E.d.expect, "200-399", half);
        field(E.form, F_WORD, _("Palabra que tiene que estar (opcional)"), E.d.word, NULL, half);
    } else {
        field(E.form, F_HOST, _("Dirección (IP o nombre)"), E.d.host, "10.20.30.40", half);
        snprintf(num, sizeof num, "%d", E.d.port);
        field(E.form, F_PORT, _("Puerto"), num, NULL, half);
    }
    r = seg_row(E.form, _("Probar cada"), inner);
    static const char *const EV[N_EVERY] = { "10 s", "30 s", "1 min", "5 min", "15 min" };
    int sel = -1;
    for (int k = 0; k < N_EVERY; k++) {
        E.every_b[k] = seg_btn(r, EV[k], every_cb, k);
        if (E.d.every_s == EVERY[k]) sel = k;
    }
    if (sel < 0) {          /* a value from the file that is not on the buttons: the nearest */
        sel = 0;
        for (int k = 1; k < N_EVERY; k++)
            if (abs(EVERY[k] - E.d.every_s) < abs(EVERY[sel] - E.d.every_s)) sel = k;
        E.d.every_s = EVERY[sel];
    }
    for (int k = 0; k < N_EVERY; k++) seg_paint(E.every_b[k], k == sel);
    snprintf(num, sizeof num, "%d", E.d.slow_ms);
    field(E.form, F_SLOW, _("Lento desde (ms)"), num, NULL, half);
    if (E.d.kind == EC_HTTP) {
        snprintf(num, sizeof num, "%d", E.d.cert_days);
        field(E.form, F_CERT, _("Avisar si al certificado le quedan menos días (0: no)"), num, NULL, half);
    }
    if (E.i >= 0) {
        lv_obj_t *d = ec_pill(E.form, AOS_SYM_DELETE, _("Borrar este servicio"), lv_color_hex(0x8E1C17), del_cb, NULL);
        lv_obj_set_style_margin_top(d, 16, 0);
    }

    E.kb = lv_keyboard_create(page);
    aos_keyboard_style(E.kb, aos_font_body);
    lv_obj_set_size(E.kb, A.W, A.land ? A.H * 52 / 100 : A.H * 36 / 100);
    lv_obj_align(E.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(E.kb, kb_cb, LV_EVENT_ALL, NULL);
}
