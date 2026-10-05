/*
 * P4OS - CAN: the sheets over the app.
 *
 *   Conexión     pins, speed, mode and the controller's filter; it applies
 *                on the next connection (it reconnects if the bus is open)
 *   Cableado     the transceiver drawn between the header and the bus, with
 *                the pins chosen, and what not to do on a car
 *   Grabaciones  the CSVs in <card>/can: replay one (once or in a loop),
 *                delete it
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const uint32_t RATES[] = { 125000, 250000, 500000, 1000000 };
static const char *const RATE_NAME[] = { "125k", "250k", "500k", "1M" };

static lv_obj_t *section(lv_obj_t *body, const char *title)
{
    lv_obj_t *l = aos_label(body, title, aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 10, 0);
    return l;
}

static lv_obj_t *note(lv_obj_t *body, const char *text, lv_color_t c)
{
    lv_obj_t *l = aos_label(body, text, aos_font_caption, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *wrap_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(r, 10, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* ================= Conexión ================= */

static int s_gpios[40], s_ngpios;
static lv_obj_t *s_conn_body;

static void conn_rebuild(void)
{
    int32_t y = s_conn_body ? lv_obj_get_scroll_y(s_conn_body) : 0;
    cn_conn_sheet();
    if (s_conn_body) lv_obj_scroll_to_y(s_conn_body, y, LV_ANIM_OFF);
}

static void pin_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t k = lv_dropdown_get_selected(lv_event_get_target(e));
    if (k >= (uint32_t)s_ngpios) return;
    if (which) CN_CFG.rx = s_gpios[k];
    else CN_CFG.tx = s_gpios[k];
    cn_cfg_save();
    conn_rebuild();
}

static void rate_cb(lv_event_t *e)
{
    CN_CFG.rate = RATES[(int)(intptr_t)lv_event_get_user_data(e)];
    cn_cfg_save();
    conn_rebuild();
}

static void rate_done(const char *v)
{
    /* kbit/s, with decimals: 83.3, 33.3, 47.619 */
    float k = strtof(v, NULL);
    uint32_t r = (uint32_t)(k * 1000.0f + 0.5f);
    if (r < 25000 || r > 1000000) aos_ui_toast(_("Entre 25 y 1000 kbit/s"), 1800);
    else {
        CN_CFG.rate = r;
        cn_cfg_save();
    }
    cn_conn_sheet();
}

static void rate_other_cb(lv_event_t *e)
{
    (void)e;
    char v[16];
    snprintf(v, sizeof v, "%u.%u", (unsigned)(CN_CFG.rate / 1000), (unsigned)(CN_CFG.rate % 1000 / 100));
    cn_text_entry(_("Velocidad en kbit/s"), v, LV_KEYBOARD_MODE_NUMBER, rate_done);
}

static void mode_cb(lv_event_t *e)
{
    CN_CFG.mode = (cn_mode_t)(int)(intptr_t)lv_event_get_user_data(e);
    if (CN_CFG.mode == CN_MODE_NORMAL)
        aos_ui_toast(_("Normal: la placa confirma tramas y puede mandar. En un auto, no."), 3000);
    conn_rebuild();
}

static void fkind_cb(lv_event_t *e)
{
    CN_CFG.fkind = (cn_filt_t)(int)(intptr_t)lv_event_get_user_data(e);
    if (CN_CFG.fkind != CN_FILT_NONE && !CN_CFG.fmask) CN_CFG.fmask = CN_CFG.fkind == CN_FILT_EXT ? 0x1FFFFFFF : 0x7FF;
    cn_cfg_save();
    conn_rebuild();
}

static void fid_done(const char *v)
{
    uint32_t x;
    if (cn_parse_hex(v, &x)) { CN_CFG.fid = x; cn_cfg_save(); }
    else aos_ui_toast(_("En hexa, como 0C0"), 1500);
    cn_conn_sheet();
}

static void fmask_done(const char *v)
{
    uint32_t x;
    if (cn_parse_hex(v, &x)) { CN_CFG.fmask = x; cn_cfg_save(); }
    else aos_ui_toast(_("En hexa, como 7FF"), 1500);
    cn_conn_sheet();
}

static void fid_cb(lv_event_t *e)
{
    (void)e;
    char v[12];
    snprintf(v, sizeof v, "%X", (unsigned)CN_CFG.fid);
    cn_text_entry(_("Id del filtro, en hexa"), v, LV_KEYBOARD_MODE_TEXT_UPPER, fid_done);
}

static void fmask_cb(lv_event_t *e)
{
    (void)e;
    char v[12];
    snprintf(v, sizeof v, "%X", (unsigned)CN_CFG.fmask);
    cn_text_entry(_("Máscara del filtro, en hexa"), v, LV_KEYBOARD_MODE_TEXT_UPPER, fmask_done);
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    if (CB.open) cn_bus_close();
    const char *why = cn_bus_open(&CN_CFG);
    if (why) {
        aos_ui_toast(why, 3000);
        return;
    }
    cn_sheet_close();
    cn_ui_status();
    cn_ui_set_tab(U.tab);
}

static void wiring_cb(lv_event_t *e) { (void)e; cn_wiring_sheet(); }

static lv_obj_t *pin_dropdown(lv_obj_t *parent, int sel_gpio, int which)
{
    char opts[1024] = "";
    int sel = 0;
    size_t k = 0;
    for (int i = 0; i < s_ngpios; i++) {
        int g = s_gpios[i];
        const aos_io_pin_t *p = aos_io_pin_of_gpio(g);
        const char *o = aos_io_owner(g);
        if (g == sel_gpio) sel = i;
        k += (size_t)snprintf(opts + k, sizeof opts - k, "%sGPIO%d · %s %u%s%s", i ? "\n" : "", g, _("pata"),
                              p ? p->pin : 0, o && strcmp(o, "CAN") ? " · " : "", o && strcmp(o, "CAN") ? o : "");
        if (k >= sizeof opts) break;
    }
    lv_obj_t *d = lv_dropdown_create(parent);
    lv_dropdown_set_options(d, opts);
    lv_dropdown_set_selected(d, (uint32_t)sel);
    lv_obj_set_size(d, 340, 72);
    lv_obj_set_style_text_font(d, aos_font_small, 0);
    lv_obj_set_style_bg_color(d, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(d, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 20, 0);
    lv_obj_t *l = lv_dropdown_get_list(d);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_bg_color(l, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_set_style_max_height(l, U.H / 2, 0);
    lv_obj_add_event_cb(d, pin_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)which);
    return d;
}

static void mode_card(lv_obj_t *parent, cn_mode_t m, const char *desc)
{
    bool on = CN_CFG.mode == m;
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, U.land ? LV_PCT(32) : LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0x123A66) : AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, AOS_C_ACCENT, 0);
    lv_obj_set_style_border_width(c, on ? 3 : 0, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c, mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m);
    aos_label(c, cn_mode_name(m), aos_font_body, AOS_C_TEXT);
    lv_obj_t *d = aos_label(c, desc, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(d, LV_PCT(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    for (uint32_t i = 0; i < lv_obj_get_child_count(c); i++) lv_obj_remove_flag(lv_obj_get_child(c, i), LV_OBJ_FLAG_CLICKABLE);
}

void cn_conn_sheet(void)
{
    s_ngpios = 0;
    const aos_io_pin_t *h = aos_io_header();
    for (int i = 0; i < 40; i++) {
        if (h[i].gpio < 0 || (h[i].flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) continue;
        s_gpios[s_ngpios++] = h[i].gpio;
    }

    lv_obj_t *body = cn_sheet_open(_("Conexión"));
    s_conn_body = body;

    section(body, _("Pines del header"));
    lv_obj_t *r = wrap_row(body);
    aos_label(r, "TX", aos_font_body, AOS_C_TEXT);
    pin_dropdown(r, CN_CFG.tx, 0);
    lv_obj_t *r2 = U.land ? r : wrap_row(body);
    aos_label(r2, "RX", aos_font_body, AOS_C_TEXT);
    pin_dropdown(r2, CN_CFG.rx, 1);
    note(body, _("TX va a la D (CTX) del transceptor y RX a su R (CRX). En la autoprueba alcanza un pin: elegí el "
                 "mismo en los dos y no hace falta cablear nada."), AOS_C_DIM);

    section(body, _("Velocidad"));
    lv_obj_t *rr = wrap_row(body);
    bool std_rate = false;
    for (int i = 0; i < 4; i++) {
        bool on = CN_CFG.rate == RATES[i];
        std_rate |= on;
        cn_pill(rr, RATE_NAME[i], on, rate_cb, (void *)(intptr_t)i);
    }
    char other[32];
    if (std_rate) snprintf(other, sizeof other, "%s", _("Otra…"));
    else snprintf(other, sizeof other, _("Otra: %u.%u k"), (unsigned)(CN_CFG.rate / 1000), (unsigned)(CN_CFG.rate % 1000 / 100));
    cn_pill(rr, other, !std_rate, rate_other_cb, NULL);

    section(body, _("Modo"));
    lv_obj_t *mr = wrap_row(body);
    mode_card(mr, CN_MODE_LISTEN, _("No confirma ni manda nada: para espiar un bus ajeno, como el de un auto. "
                                    "Arranca siempre así."));
    mode_card(mr, CN_MODE_NORMAL, _("Participa: confirma las tramas de los otros y puede mandar."));
    mode_card(mr, CN_MODE_SELFTEST, _("Sin transceptor: manda y se escucha a sí misma. No sale al bus."));

    section(body, _("Filtro del controlador"));
    lv_obj_t *fr = wrap_row(body);
    cn_pill(fr, _("Sin filtro"), CN_CFG.fkind == CN_FILT_NONE, fkind_cb, (void *)(intptr_t)CN_FILT_NONE);
    cn_pill(fr, _("11 bits"), CN_CFG.fkind == CN_FILT_STD, fkind_cb, (void *)(intptr_t)CN_FILT_STD);
    cn_pill(fr, _("29 bits"), CN_CFG.fkind == CN_FILT_EXT, fkind_cb, (void *)(intptr_t)CN_FILT_EXT);
    if (CN_CFG.fkind != CN_FILT_NONE) {
        lv_obj_t *fv = wrap_row(body);
        char t[40];
        snprintf(t, sizeof t, _("Id %X"), (unsigned)CN_CFG.fid);
        cn_pill(fv, t, false, fid_cb, NULL);
        snprintf(t, sizeof t, _("Máscara %X"), (unsigned)CN_CFG.fmask);
        cn_pill(fv, t, false, fmask_cb, NULL);
        note(body, _("Pasa la trama cuyo id coincide con el del filtro en los bits de la máscara: 7FF deja un id "
                     "solo, 700 los de 0x100 en 0x100. Lo que no pasa no llega ni a la grabación."), AOS_C_DIM);
    }

    lv_obj_t *br = wrap_row(body);
    lv_obj_set_style_pad_top(br, 16, 0);
    lv_obj_t *go = cn_btn(br, AOS_SYM_LAN_CONNECT, CB.open ? _("Reconectar con esto") : _("Conectar"), go_cb, NULL);
    lv_obj_set_style_bg_color(go, AOS_C_GREEN, 0);
    cn_btn(br, AOS_SYM_CONNECTION, _("Cableado"), wiring_cb, NULL);
}

/* ================= Cableado ================= */

typedef struct { lv_point_precise_t p[2]; } seg_t;
static seg_t s_seg[12];
static int s_nseg;

static void line(lv_obj_t *parent, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, int32_t w)
{
    if (s_nseg >= (int)(sizeof s_seg / sizeof s_seg[0])) return;
    seg_t *s = &s_seg[s_nseg++];
    s->p[0].x = x1;
    s->p[0].y = y1;
    s->p[1].x = x2;
    s->p[1].y = y2;
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, s->p, 2);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
}

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *title, lv_color_t c)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, c, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *t = aos_label(b, title, aos_font_caption, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
    return b;
}

static void text_at(lv_obj_t *parent, int32_t x, int32_t y, const char *s, lv_color_t c, lv_align_t align)
{
    lv_obj_t *l = aos_label(parent, s, aos_font_caption, c);
    lv_obj_align(l, align, x, y);
}

void cn_wiring_sheet(void)
{
    lv_obj_t *body = cn_sheet_open(_("Cableado"));
    s_nseg = 0;
    const aos_io_pin_t *ptx = aos_io_pin_of_gpio(CN_CFG.tx), *prx = aos_io_pin_of_gpio(CN_CFG.rx);

    /* the drawing: header | transceiver | bus with its two 120 ohm ends */
    const int32_t W = 660, H = 300;
    lv_obj_t *d = lv_obj_create(body);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, W, H);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    if (U.land) lv_obj_set_style_align(d, LV_ALIGN_CENTER, 0);

    box(d, 0, 10, 200, 280, _("P4 · header"), AOS_C_CARD);
    box(d, 300, 10, 160, 280, "SN65HVD230", lv_color_hex(0x1D3B2A));
    const int32_t ys[4] = { 80, 130, 180, 230 };
    char t[40];
    snprintf(t, sizeof t, _("TX GPIO%d · p%u"), CN_CFG.tx, ptx ? ptx->pin : 0);
    text_at(d, 10, ys[0] - 12, t, AOS_C_TEXT, LV_ALIGN_TOP_LEFT);
    snprintf(t, sizeof t, _("RX GPIO%d · p%u"), CN_CFG.rx, prx ? prx->pin : 0);
    text_at(d, 10, ys[1] - 12, t, AOS_C_TEXT, LV_ALIGN_TOP_LEFT);
    text_at(d, 10, ys[2] - 12, _("3V3 · p18"), AOS_C_TEXT, LV_ALIGN_TOP_LEFT);
    text_at(d, 10, ys[3] - 12, _("GND · p19"), AOS_C_TEXT, LV_ALIGN_TOP_LEFT);
    const char *tp[4] = { "D", "R", "3V3", "GND" };
    const lv_color_t wc[4] = { AOS_C_ORANGE, AOS_C_TEAL, AOS_C_RED, AOS_C_DIM };
    for (int i = 0; i < 4; i++) {
        line(d, 196, ys[i], 304, ys[i], wc[i], 5);
        text_at(d, 312, ys[i] - 12, tp[i], AOS_C_TEXT, LV_ALIGN_TOP_LEFT);
    }
    text_at(d, 400, 108, "CANH", AOS_C_YELLOW, LV_ALIGN_TOP_LEFT);
    text_at(d, 400, 168, "CANL", AOS_C_GREEN, LV_ALIGN_TOP_LEFT);
    line(d, 460, 120, 560, 120, AOS_C_YELLOW, 5);
    line(d, 460, 180, 615, 180, AOS_C_GREEN, 5);
    line(d, 560, 30, 560, 280, AOS_C_YELLOW, 5);
    line(d, 615, 30, 615, 280, AOS_C_GREEN, 5);
    for (int k = 0; k < 2; k++) {
        lv_obj_t *res = box(d, 545, k ? 252 : 14, 86, 34, "", lv_color_hex(0x5A3A10));
        lv_obj_t *rl = aos_label(res, "120 Ω", aos_font_caption, AOS_C_TEXT);
        lv_obj_center(rl);
    }
    text_at(d, 488, 60, _("bus"), AOS_C_DIM, LV_ALIGN_TOP_LEFT);

    lv_obj_t *warn = lv_obj_create(body);
    lv_obj_remove_style_all(warn);
    lv_obj_set_size(warn, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(warn, 16, 0);
    lv_obj_set_style_radius(warn, 18, 0);
    lv_obj_set_style_bg_color(warn, lv_color_hex(0x4A1512), 0);
    lv_obj_set_style_bg_opa(warn, LV_OPA_COVER, 0);
    lv_obj_t *wl = aos_label(warn, _("En un auto, sólo escucha. Una trama mandada al bus de un auto puede apagar el "
                                     "motor, el tablero o los frenos asistidos, y un nodo que no confirma bien lo "
                                     "puede trabar entero. La app arranca siempre en Solo escucha."),
                             aos_font_small, lv_color_hex(0xFFD2CC));
    lv_obj_set_width(wl, LV_PCT(100));
    lv_label_set_long_mode(wl, LV_LABEL_LONG_MODE_WRAP);

    note(body, _("· El transceptor tiene que ser de 3,3 V: SN65HVD230, 231 o 232. Uno de 5 V (MCP2551, TJA1050) le "
                 "devuelve 5 V al pin RX, y el header no los aguanta."), AOS_C_TEXT);
    note(body, _("· Una resistencia de 120 Ω en cada punta del bus: con todo apagado se miden 60 Ω entre CANH y "
                 "CANL. Muchos módulos traen la suya; si la placa no va en una punta, sacala."), AOS_C_TEXT);
    note(body, _("· GND común con el resto del bus, y la misma velocidad en todos los nodos."), AOS_C_TEXT);
    note(body, _("· En el conector OBD-II de un auto: CANH en la pata 6, CANL en la 14, masa en la 4 o 5; casi "
                 "siempre a 500 kbit/s."), AOS_C_TEXT);
    note(body, _("· Sin transceptor: modo Autoprueba con TX y RX en el mismo pin. La placa se manda y se escucha."),
         AOS_C_DIM);
}

/* ================= Grabaciones ================= */

#define MAX_FILES 64
static char (*s_files)[48];
static uint32_t s_sizes[MAX_FILES];
static int s_nfiles;
static int s_confirm = -1;

static int name_cmp(const void *a, const void *b) { return -strcmp(a, b); }

static void files_scan(void)
{
    static char names[MAX_FILES][48];
    s_files = names;
    s_nfiles = 0;
    DIR *d = opendir(cn_dir());
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s_nfiles < MAX_FILES) {
        const char *n = de->d_name;
        size_t l = strlen(n);
        if (n[0] == '.' || l < 5 || l >= 48 || strcasecmp(n + l - 4, ".csv")) continue;
        memcpy(names[s_nfiles++], n, l + 1);
    }
    closedir(d);
    qsort(names, (size_t)s_nfiles, 48, name_cmp);
    for (int i = 0; i < s_nfiles; i++) {
        char p[160];
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", cn_dir(), names[i]);
        s_sizes[i] = stat(p, &sb) == 0 ? (uint32_t)sb.st_size : 0;
    }
}

static void replay_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int i = v & 0xFF;
    bool loop = v & 0x100;
    if (i >= s_nfiles) return;
    if (!CB.open) aos_ui_toast(_("Conectá el bus primero"), 1800);
    else if (CB.cfg.mode == CN_MODE_LISTEN) aos_ui_toast(_("Para repetir hace falta modo Normal o Autoprueba"), 2400);
    else if (CB.replay) aos_ui_toast(_("Ya hay una repitiéndose"), 1800);
    else if (cn_replay_start(s_files[i], loop)) {
        cn_sheet_close();
        cn_ui_status();
    }
}

static void stop_replay_cb(lv_event_t *e)
{
    (void)e;
    cn_replay_stop();
    cn_files_sheet();
}

static void del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_nfiles) return;
    if (CB.rec && !strcmp(CB.rec_name, s_files[i])) {
        aos_ui_toast(_("Se está grabando: pará la grabación primero"), 2000);
        return;
    }
    if (s_confirm != i) {
        s_confirm = i;
        lv_obj_t *b = lv_event_get_target(e);
        lv_obj_set_style_bg_color(b, AOS_C_RED, 0);
        lv_label_set_text(lv_obj_get_child(b, lv_obj_get_child_count(b) - 1), _("¿Borrar?"));
        return;
    }
    char p[160];
    snprintf(p, sizeof p, "%s/%s", cn_dir(), s_files[i]);
    unlink(p);
    cn_files_sheet();
}

void cn_files_sheet(void)
{
    s_confirm = -1;
    files_scan();
    lv_obj_t *body = cn_sheet_open(_("Grabaciones"));
    if (CB.replay) {
        lv_obj_t *r = wrap_row(body);
        char t[96];
        snprintf(t, sizeof t, _("Repitiendo %s"), CB.replay_name);
        aos_label(r, t, aos_font_small, AOS_C_GREEN);
        cn_btn(r, AOS_SYM_STOP, _("Parar"), stop_replay_cb, NULL);
    }
    note(body, _("En la tarjeta, carpeta can, en el CSV de SavvyCAN (GVRET). Se bajan desde la página CAN del "
                 "portal. Repetir manda cada trama a su tiempo: hace falta modo Normal o Autoprueba."), AOS_C_DIM);
    if (!s_nfiles) {
        note(body, _("Todavía no hay grabaciones: el botón rojo de arriba graba."), AOS_C_TEXT);
        return;
    }
    for (int i = 0; i < s_nfiles; i++) {
        lv_obj_t *row = lv_obj_create(body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 10, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        char t[96];
        if (s_sizes[i] >= 1024 * 1024) snprintf(t, sizeof t, "%s · %u.%u MB", s_files[i], (unsigned)(s_sizes[i] >> 20),
                                                 (unsigned)((s_sizes[i] & 0xFFFFF) * 10 >> 20));
        else snprintf(t, sizeof t, "%s · %u KB", s_files[i], (unsigned)((s_sizes[i] + 1023) / 1024));
        lv_obj_t *l = aos_label(row, t, &aos_mono_18, AOS_C_TEXT);
        lv_obj_set_width(l, LV_PCT(100));
        cn_btn(row, AOS_SYM_PLAY, _("Repetir"), replay_cb, (void *)(intptr_t)i);
        cn_btn(row, AOS_SYM_SHUFFLE_VARIANT, _("En bucle"), replay_cb, (void *)(intptr_t)(i | 0x100));
        cn_btn(row, AOS_SYM_DELETE, _("Borrar"), del_cb, (void *)(intptr_t)i);
    }
}
