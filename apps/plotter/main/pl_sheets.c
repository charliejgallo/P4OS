/*
 * P4OS - Monitor serie: the sheets.
 *
 * Each sheet covers the app and is built again after every change (the
 * scroll position kept), like the Terminal's connection sheet, which this
 * file copies for its own: the speed (any from 50 baud), the frame and the
 * USB adapter's DTR and RTS, saved per channel.
 *
 *   Formato    automatic or a saved profile; what the last lines gave
 *   Perfil     one profile: which token, the scale, the decimal point
 *   Series     show or hide, the big one, the alarms with their beep
 *   Vista      the window, time or XY, the value axis, clearing the data
 *   Enviar     a line to the port, with the line ending, and the answers
 */
#include "pl.h"

static const uint32_t BAUDS[] = { 300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200,
                                  230400, 460800, 921600 };
#define N_BAUDS (sizeof BAUDS / sizeof BAUDS[0])
#define BAUD_MIN 50
#define BAUD_MAX 5000000

static const char *const EOL_NAME[4] = { "CR LF", "LF", "CR", N_("nada") };
static const char *const EOL_SEQ[4] = { "\r\n", "\n", "\r", "" };

enum { SH_NONE, SH_CONN, SH_FMT, SH_PROF, SH_SERIES, SH_VIEW, SH_SEND };
static int s_kind;
static int32_t s_scroll;
static lv_obj_t *s_col;
static lv_obj_t *s_raw_l[8], *s_raw_g[8];
static int s_raw_n;

/* ---- building blocks ---- */

lv_obj_t *pl_mini(lv_obj_t *parent, const char *text, bool on, lv_color_t on_color, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_color(b, on ? on_color : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, text, aos_font_small, on ? lv_color_white() : AOS_C_TEXT);
    lv_obj_center(l);
    return b;
}

void pl_sheet_close(void)
{
    if (PLA.overlay) lv_obj_delete(PLA.overlay);
    PLA.overlay = PLA.sheet_ta = PLA.sheet_kb = PLA.raw_box = NULL;
    s_col = NULL;
    s_raw_n = 0;
    s_kind = SH_NONE;
    pl_view_flags(true);
}

static void close_cb(lv_event_t *e) { (void)e; pl_sheet_close(); pl_chart_want(); pl_refresh(true); }

/* The overlay and a column that scrolls; the same kind built again keeps
 * where it was scrolled to */
static lv_obj_t *sheet_begin(int kind, const char *title, int32_t *wout)
{
    int32_t keep = 0;
    if (PLA.overlay && s_kind == kind && s_col) keep = lv_obj_get_scroll_y(s_col);
    if (!PLA.overlay) {
        PLA.overlay = lv_obj_create(PLA.root);
        lv_obj_remove_style_all(PLA.overlay);
        lv_obj_set_size(PLA.overlay, PLA.W, PLA.H);
        lv_obj_set_style_bg_color(PLA.overlay, lv_color_hex(0x0B0E12), 0);
        lv_obj_set_style_bg_opa(PLA.overlay, LV_OPA_COVER, 0);
        lv_obj_add_flag(PLA.overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(PLA.overlay, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clean(PLA.overlay);
    PLA.sheet_ta = PLA.sheet_kb = PLA.raw_box = NULL;
    s_raw_n = 0;
    s_kind = kind;
    pl_view_flags(false);
    int32_t w = PLA.land ? 900 : PLA.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = lv_obj_create(PLA.overlay);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, w, PLA.H);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_style_pad_bottom(col, 40, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);     /* or it does not scroll with a finger */
    s_col = col;
    lv_obj_t *head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, w, 96);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tl = aos_label(head, title, aos_font_large, AOS_C_TEXT);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(tl, w - 180);
    lv_obj_align(tl, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_t *done = pl_mini(head, _("Listo"), true, AOS_C_ACCENT, close_cb, NULL);
    lv_obj_align(done, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    if (keep) {
        lv_obj_update_layout(col);
        lv_obj_scroll_to_y(col, keep, LV_ANIM_OFF);
    }
    s_scroll = keep;
    *wout = w;
    return col;
}

static lv_obj_t *section(lv_obj_t *col, int32_t w, const char *title)
{
    if (title) {
        lv_obj_t *t = aos_label(col, title, aos_font_caption, AOS_C_DIM);
        lv_obj_set_style_pad_left(t, 8, 0);
        lv_obj_set_style_pad_top(t, 10, 0);
    }
    lv_obj_t *r = lv_obj_create(col);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static lv_obj_t *note(lv_obj_t *col, int32_t w, const char *text)
{
    lv_obj_t *n = aos_label(col, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    return n;
}

static lv_obj_t *card(lv_obj_t *col, int32_t w)
{
    lv_obj_t *c = lv_obj_create(col);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x161A20), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(c, 10, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *card_title(lv_obj_t *c, int32_t w, const char *text, lv_color_t col)
{
    lv_obj_t *l = aos_label(c, text, aos_font_body, col);
    lv_obj_set_width(l, w - 32);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

/* ---- the text entry ---- */

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[64];
    pl_scpy(v, sizeof v, lv_textarea_get_text(PLA.sheet_ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? PLA.text_done : NULL;
    void (*back)(const char *) = PLA.text_done;
    (void)back;
    if (done) done(v);
    else if (PLA.text_done) PLA.text_done(NULL);       /* cancelled: the sheet again */
}

void pl_text_entry(const char *title, const char *value, bool numeric, void (*done)(const char *))
{
    int32_t w;
    lv_obj_t *col = sheet_begin(-1, title, &w);
    (void)col;
    lv_obj_add_flag(lv_obj_get_child(lv_obj_get_child(s_col, 0), 1), LV_OBJ_FLAG_HIDDEN);   /* no "Listo" */
    PLA.text_done = done;
    PLA.sheet_ta = lv_textarea_create(PLA.overlay);
    lv_textarea_set_one_line(PLA.sheet_ta, true);
    lv_textarea_set_max_length(PLA.sheet_ta, 60);
    lv_textarea_set_text(PLA.sheet_ta, value);
    lv_obj_set_size(PLA.sheet_ta, w, 88);
    lv_obj_align(PLA.sheet_ta, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_text_font(PLA.sheet_ta, &aos_mono_22, 0);
    lv_obj_set_style_bg_color(PLA.sheet_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(PLA.sheet_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(PLA.sheet_ta, 0, 0);
    lv_obj_set_style_radius(PLA.sheet_ta, 20, 0);
    lv_obj_set_style_pad_hor(PLA.sheet_ta, 24, 0);
    lv_obj_set_style_pad_ver(PLA.sheet_ta, 24, 0);
    PLA.sheet_kb = lv_keyboard_create(PLA.overlay);
    lv_obj_set_size(PLA.sheet_kb, PLA.W, PLA.land ? PLA.H / 2 : PLA.H * 2 / 5);
    lv_obj_align(PLA.sheet_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(PLA.sheet_kb, aos_font_body);
    lv_keyboard_set_textarea(PLA.sheet_kb, PLA.sheet_ta);
    if (numeric) lv_keyboard_set_mode(PLA.sheet_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(PLA.sheet_kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ---- the raw view: the last lines and what came out of them ---- */

static void utf8_clean(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n > 0;
        for (int i = 1; ok && i < n; i++) ok = (p[i] & 0xC0) == 0x80;
        if (!ok) { *p++ = '?'; continue; }
        p += n;
    }
}

static void raw_box(lv_obj_t *col, int32_t w, int lines)
{
    lv_obj_t *b = lv_obj_create(col);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_pad_all(b, 14, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    PLA.raw_box = b;
    s_raw_n = lines > 8 ? 8 : lines;
    for (int i = 0; i < s_raw_n; i++) {
        s_raw_l[i] = aos_label(b, "", &aos_mono_18, AOS_C_TEXT);
        lv_obj_set_width(s_raw_l[i], w - 28);
        lv_label_set_long_mode(s_raw_l[i], LV_LABEL_LONG_MODE_DOTS);
        s_raw_g[i] = aos_label(b, "", aos_font_caption, AOS_C_TEAL);
        lv_obj_set_width(s_raw_g[i], w - 28);
        lv_label_set_long_mode(s_raw_g[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_pad_bottom(s_raw_g[i], 6, 0);
    }
    pl_sheet_raw_update();
}

void pl_sheet_raw_update(void)
{
    if (!PLA.raw_box || !s_raw_n) return;
    uint32_t have = PLA.nraw < PL_RAW_N ? PLA.nraw : PL_RAW_N;
    for (int i = 0; i < s_raw_n; i++) {
        /* the newest at the bottom */
        int back = s_raw_n - 1 - i;
        if ((uint32_t)back >= have) {
            lv_label_set_text(s_raw_l[i], i == s_raw_n - 1 && !have ? _("(todavía no llegó nada)") : "");
            lv_label_set_text(s_raw_g[i], "");
            continue;
        }
        const pl_rawline_t *r = &PLA.raw[(PLA.nraw - 1 - (uint32_t)back) % PL_RAW_N];
        char t[AOS_SERIAL_LINE_MAX + 4];
        pl_scpy(t, sizeof t, r->text);
        utf8_clean(t);
        lv_label_set_text(s_raw_l[i], t[0] ? t : " ");
        char g[110];
        snprintf(g, sizeof g, "→ %s", r->got);
        utf8_clean(g);
        lv_label_set_text(s_raw_g[i], g);
    }
}

/* ---- the connection ---- */

static const char *key(const char *what, int ch, char *buf, size_t cap)
{
    snprintf(buf, cap, "pl_%s%d", what, ch);
    return buf;
}

void pl_conn_load(void)
{
    char k[20];
    char saved[AOS_IO_PORT_NAME_MAX];
    pl_scpy(saved, sizeof saved, "uart.b");
    /* the Terminal's choice for the channel, until this app has its own */
    aos_hal_pref_get_str(PLS.ch ? "ser_port1" : "ser_port", saved, sizeof saved);
    aos_hal_pref_get_str(key("port", PLS.ch, k, sizeof k), saved, sizeof saved);
    aos_serial_stat_t st;
    aos_serial_stat(PLS.ch, &st);
    if (st.running) pl_scpy(saved, sizeof saved, st.port);
    if (PLA.port_dd) {
        int sel = lv_dropdown_get_option_index(PLA.port_dd, saved);
        if (sel >= 0) lv_dropdown_set_selected(PLA.port_dd, (uint32_t)sel);
    }
    int32_t baud = 115200, ctl = AOS_SERIAL_DTR | AOS_SERIAL_RTS;
    aos_hal_pref_get_i32(PLS.ch ? "ser_baud1" : "ser_baud", &baud);
    aos_hal_pref_get_i32(key("baud", PLS.ch, k, sizeof k), &baud);
    if (baud < BAUD_MIN || baud > BAUD_MAX) baud = 115200;
    char frame[8] = "8N1";
    aos_hal_pref_get_str(key("frame", PLS.ch, k, sizeof k), frame, sizeof frame);
    if (!aos_io_frame_parse(frame, NULL, NULL, NULL)) pl_scpy(frame, sizeof frame, "8N1");
    aos_hal_pref_get_i32(key("ctl", PLS.ch, k, sizeof k), &ctl);
    PLA.baud = (uint32_t)baud;
    pl_scpy(PLA.frame, sizeof PLA.frame, frame);
    PLA.ctl = (uint8_t)(ctl & (AOS_SERIAL_DTR | AOS_SERIAL_RTS));
    if (st.running) {
        PLA.baud = st.baud;
        pl_scpy(PLA.frame, sizeof PLA.frame, st.frame);
        PLA.ctl = st.ctl;
    }
}

/* Saved, and a running capture starts again on its port with it */
void pl_conn_apply(void)
{
    char k[20];
    aos_hal_pref_set_i32(key("baud", PLS.ch, k, sizeof k), (int32_t)PLA.baud);
    aos_hal_pref_set_str(key("frame", PLS.ch, k, sizeof k), PLA.frame);
    aos_hal_pref_set_i32(key("ctl", PLS.ch, k, sizeof k), PLA.ctl);
    aos_serial_stat_t st;
    aos_serial_stat(PLS.ch, &st);
    if (st.running && (st.baud != PLA.baud || strcmp(st.frame, PLA.frame) || st.ctl != PLA.ctl)) {
        char port[AOS_IO_PORT_NAME_MAX];
        pl_scpy(port, sizeof port, st.port);
        aos_serial_stop(PLS.ch);
        if (!aos_serial_start_ex(PLS.ch, port, PLA.baud, PLA.frame, PLA.ctl))
            aos_ui_toast(_("No se pudo abrir el puerto"), 2500);
    }
}

static void conn_baud_cb(lv_event_t *e)
{
    PLA.baud = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    pl_conn_apply();
    pl_sheet_conn();
}

static void conn_other_done(const char *v)
{
    if (v) {
        char *end;
        unsigned long b = strtoul(v, &end, 10);
        while (*end == ' ') end++;
        if (*end || b < BAUD_MIN || b > BAUD_MAX) {
            aos_ui_toast(_("Entre 50 y 5000000 baudios"), 2200);
        } else {
            PLA.baud = (uint32_t)b;
            pl_conn_apply();
        }
    }
    pl_sheet_conn();
}

static void conn_other_cb(lv_event_t *e)
{
    (void)e;
    char v[16];
    snprintf(v, sizeof v, "%u", (unsigned)PLA.baud);
    pl_text_entry(_("Velocidad en baudios"), v, true, conn_other_done);
}

static void conn_frame_cb(lv_event_t *e)
{
    uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    PLA.frame[v >> 8] = (char)(v & 0xFF);
    pl_conn_apply();
    pl_sheet_conn();
}

static void conn_line_cb(lv_event_t *e)
{
    PLA.ctl ^= (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    pl_conn_apply();
    pl_sheet_conn();
}

void pl_sheet_conn(void)
{
    int32_t w;
    char title[48];
    snprintf(title, sizeof title, "%s %c", _("Conexión"), PLS.ch ? 'B' : 'A');
    lv_obj_t *col = sheet_begin(SH_CONN, title, &w);
    lv_obj_t *r = section(col, w, _("VELOCIDAD (BAUDIOS)"));
    bool listed = false;
    for (size_t i = 0; i < N_BAUDS; i++) {
        char b[12];
        snprintf(b, sizeof b, "%u", (unsigned)BAUDS[i]);
        bool on = BAUDS[i] == PLA.baud;
        listed |= on;
        pl_mini(r, b, on, AOS_C_ACCENT, conn_baud_cb, (void *)(uintptr_t)BAUDS[i]);
    }
    char other[32];
    if (listed) pl_scpy(other, sizeof other, _("Otra…"));
    else snprintf(other, sizeof other, "%u…", (unsigned)PLA.baud);
    pl_mini(r, other, !listed, AOS_C_ACCENT, conn_other_cb, NULL);
    r = section(col, w, _("BITS DE DATOS"));
    for (char c = '5'; c <= '8'; c++) {
        char b[2] = { c, 0 };
        pl_mini(r, b, PLA.frame[0] == c, AOS_C_ACCENT, conn_frame_cb, (void *)(uintptr_t)(0 << 8 | (uint8_t)c));
    }
    r = section(col, w, _("PARIDAD"));
    static const char PAR[3] = { 'N', 'E', 'O' };
    const char *par_name[3] = { _("Ninguna (N)"), _("Par (E)"), _("Impar (O)") };
    for (int i = 0; i < 3; i++)
        pl_mini(r, par_name[i], PLA.frame[1] == PAR[i], AOS_C_ACCENT, conn_frame_cb, (void *)(uintptr_t)(1 << 8 | (uint8_t)PAR[i]));
    r = section(col, w, _("BITS DE STOP"));
    pl_mini(r, "1", PLA.frame[2] == '1', AOS_C_ACCENT, conn_frame_cb, (void *)(uintptr_t)(2 << 8 | '1'));
    pl_mini(r, "2", PLA.frame[2] == '2', AOS_C_ACCENT, conn_frame_cb, (void *)(uintptr_t)(2 << 8 | '2'));
    r = section(col, w, _("LÍNEAS DEL ADAPTADOR USB"));
    pl_mini(r, "DTR", PLA.ctl & AOS_SERIAL_DTR, AOS_C_GREEN, conn_line_cb, (void *)(uintptr_t)AOS_SERIAL_DTR);
    pl_mini(r, "RTS", PLA.ctl & AOS_SERIAL_RTS, AOS_C_GREEN, conn_line_cb, (void *)(uintptr_t)AOS_SERIAL_RTS);
    note(col, w, _("Prendidas, como las deja una computadora. El cable óptico de muchos multímetros se alimenta de DTR con RTS apagado: si no llega nada, probá apagar RTS. Los puertos del header no tienen estas líneas."));
    note(col, w, _("El canal es el mismo de la Terminal: los dos miran el mismo aparato a la vez, y la captura sigue con la app cerrada. Con la velocidad o el formato equivocados llegan caracteres raros: la vista de Formato muestra qué llega."));
}

/* ---- the format ---- */

static void fmt_pick_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (f != PLS.fmt) {
        PLS.fmt = f;
        pl_set_save();
        pl_restart_reading();
    }
    pl_sheet_format();
}

static void prof_edit(int p);

static void fmt_new_cb(lv_event_t *e)
{
    int tmpl = (int)(intptr_t)lv_event_get_user_data(e);
    if (PLC.np >= PL_MAXP) { aos_ui_toast(_("Ya hay 12 perfiles"), 1800); return; }
    pl_profile_default(&PLC.p[PLC.np], tmpl);
    PLC.np++;
    pl_cfg_save();
    PLS.fmt = PLC.np - 1;
    pl_set_save();
    pl_restart_reading();
    prof_edit(PLS.fmt);
}

static void fmt_edit_cb(lv_event_t *e) { (void)e; if (PLS.fmt >= 0) prof_edit(PLS.fmt); }

void pl_sheet_format(void)
{
    int32_t w;
    lv_obj_t *col = sheet_begin(SH_FMT, _("Formato"), &w);
    lv_obj_t *r = section(col, w, _("CÓMO LEER LAS LÍNEAS"));
    pl_mini(r, _("Automático"), PLS.fmt < 0, AOS_C_ACCENT, fmt_pick_cb, (void *)(intptr_t)-1);
    for (int i = 0; i < PLC.np; i++) pl_mini(r, PLC.p[i].name, PLS.fmt == i, AOS_C_ACCENT, fmt_pick_cb, (void *)(intptr_t)i);
    if (PLS.fmt < 0) {
        note(col, w, _("Automático entiende números separados por espacios, comas, punto y coma o tabs (una serie por columna), el formato nombre:valor de los plotters serie (temp:21.5,hum:40), una palabra antes de cada número (Temp 21.5) y CSV con encabezado. Las líneas sin números se saltean."));
    } else {
        r = section(col, w, NULL);
        pl_mini(r, _("Editar el perfil…"), true, AOS_C_ACCENT, fmt_edit_cb, NULL);
    }
    r = section(col, w, _("PERFIL NUEVO"));
    pl_mini(r, _("En blanco"), false, AOS_C_ACCENT, fmt_new_cb, (void *)0);
    pl_mini(r, _("Valor y punto decimal"), false, AOS_C_ACCENT, fmt_new_cb, (void *)1);
    note(col, w, _("Un perfil sirve para lo que el automático no entiende: elige qué campo de la línea es cada serie, con una escala y un desplazamiento, o con el punto decimal según otro campo (el «+0265 4» de un multímetro es 26.5). Cambiar el formato vuelve a leer las últimas 3000 líneas del canal."));
    lv_obj_t *t = aos_label(col, _("LO QUE LLEGA"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 8, 0);
    lv_obj_set_style_pad_top(t, 10, 0);
    raw_box(col, w, PLA.land ? 5 : 8);
}

/* ---- one profile ---- */

static pl_profile_t *P(void) { return PLA.edit_p >= 0 && PLA.edit_p < PLC.np ? &PLC.p[PLA.edit_p] : NULL; }

static void prof_changed(void)
{
    pl_cfg_save();
    if (PLA.edit_p == PLS.fmt) pl_restart_reading();
    prof_edit(PLA.edit_p);
}

static void prof_again(const char *v) { (void)v; prof_edit(PLA.edit_p); }

static void prof_name_done(const char *v)
{
    pl_profile_t *p = P();
    if (v && p && v[0]) { pl_scpy(p->name, sizeof p->name, v); prof_changed(); return; }
    prof_again(NULL);
}

static void prof_name_cb(lv_event_t *e) { (void)e; if (P()) pl_text_entry(_("Nombre del perfil"), P()->name, false, prof_name_done); }

static void prof_match_done(const char *v)
{
    pl_profile_t *p = P();
    if (v && p) { pl_scpy(p->match, sizeof p->match, v); prof_changed(); return; }
    prof_again(NULL);
}

static void prof_match_cb(lv_event_t *e)
{
    (void)e;
    if (P()) pl_text_entry(_("Usar sólo las líneas que tengan… (vacío: todas)"), P()->match, false, prof_match_done);
}

static void prof_comma_cb(lv_event_t *e) { (void)e; if (P()) { P()->comma = !P()->comma; prof_changed(); } }

static void prof_add_cb(lv_event_t *e)
{
    (void)e;
    pl_profile_t *p = P();
    if (!p || p->nf >= PL_MAXF) return;
    pl_pfield_t *f = &p->f[p->nf];
    memset(f, 0, sizeof *f);
    f->field = p->nf ? p->f[p->nf - 1].field + 1 : 1;
    f->scale = 1;
    p->nf++;
    prof_changed();
}

static void prof_del_cb(lv_event_t *e)
{
    (void)e;
    if (PLA.edit_p < 0 || PLA.edit_p >= PLC.np) return;
    memmove(&PLC.p[PLA.edit_p], &PLC.p[PLA.edit_p + 1], sizeof PLC.p[0] * (size_t)(PLC.np - PLA.edit_p - 1));
    PLC.np--;
    pl_cfg_save();
    if (PLS.fmt == PLA.edit_p) PLS.fmt = -1;
    else if (PLS.fmt > PLA.edit_p) PLS.fmt--;
    pl_set_save();
    pl_restart_reading();
    pl_sheet_format();
}

/* ud: field << 8 | what */
enum { FD_FIELD_DN, FD_FIELD_UP, FD_NAME, FD_UNIT, FD_SCALE, FD_OFFSET, FD_DP_DN, FD_DP_UP, FD_MAP, FD_DEL };

static void field_done(const char *v)
{
    pl_profile_t *p = P();
    if (!v || !p || PLA.edit_f < 0 || PLA.edit_f >= p->nf) { prof_again(NULL); return; }
    pl_pfield_t *f = &p->f[PLA.edit_f];
    switch (PLA.edit_s) {
    case FD_NAME: pl_scpy(f->name, sizeof f->name, v); break;
    case FD_UNIT: pl_scpy(f->unit, sizeof f->unit, v); break;
    case FD_SCALE: { float x = strtof(v, NULL); f->scale = x == 0 ? 1 : x; break; }
    case FD_OFFSET: f->offset = strtof(v, NULL); break;
    case FD_MAP: pl_scpy(f->dp_map, sizeof f->dp_map, v); break;
    default: break;
    }
    prof_changed();
}

static void field_cb(lv_event_t *e)
{
    uintptr_t u = (uintptr_t)lv_event_get_user_data(e);
    pl_profile_t *p = P();
    int fi = (int)(u >> 8), what = (int)(u & 0xFF);
    if (!p || fi >= p->nf) return;
    pl_pfield_t *f = &p->f[fi];
    PLA.edit_f = fi;
    PLA.edit_s = what;
    char v[48];
    switch (what) {
    case FD_FIELD_DN: if (f->field > 1) f->field--; prof_changed(); return;
    case FD_FIELD_UP: if (f->field < 24) f->field++; prof_changed(); return;
    case FD_DP_DN: if (f->dp_field > 0) f->dp_field--; prof_changed(); return;
    case FD_DP_UP: if (f->dp_field < 24) f->dp_field++; prof_changed(); return;
    case FD_DEL:
        memmove(&p->f[fi], &p->f[fi + 1], sizeof p->f[0] * (size_t)(p->nf - fi - 1));
        p->nf--;
        prof_changed();
        return;
    case FD_NAME: pl_text_entry(_("Nombre de la serie"), f->name, false, field_done); return;
    case FD_UNIT: pl_text_entry(_("Unidad (V, °C, kg…)"), f->unit, false, field_done); return;
    case FD_SCALE:
        snprintf(v, sizeof v, "%g", (double)f->scale);
        pl_text_entry(_("Multiplicar por"), v, true, field_done);
        return;
    case FD_OFFSET:
        snprintf(v, sizeof v, "%g", (double)f->offset);
        pl_text_entry(_("Y sumarle"), v, true, field_done);
        return;
    case FD_MAP:
        pl_text_entry(_("Código:decimales, separados por comas (vacío: el campo dice los decimales)"), f->dp_map, false,
                      field_done);
        return;
    }
}

#define FUD(fi, w) ((void *)(uintptr_t)(((uintptr_t)(fi) << 8) | (w)))

static void prof_edit(int pi)
{
    PLA.edit_p = pi;
    pl_profile_t *p = P();
    if (!p) { pl_sheet_format(); return; }
    int32_t w;
    char title[48];
    snprintf(title, sizeof title, "%s", p->name);
    lv_obj_t *col = sheet_begin(SH_PROF, title, &w);
    char b[96];
    lv_obj_t *r = section(col, w, _("EL PERFIL"));
    snprintf(b, sizeof b, "%s: %s", _("Nombre"), p->name);
    pl_mini(r, b, false, AOS_C_ACCENT, prof_name_cb, NULL);
    snprintf(b, sizeof b, "%s: %s", _("Sólo líneas con"), p->match[0] ? p->match : _("todas"));
    pl_mini(r, b, p->match[0] != 0, AOS_C_ACCENT, prof_match_cb, NULL);
    pl_mini(r, _("Coma decimal"), p->comma, AOS_C_ACCENT, prof_comma_cb, NULL);

    /* the last line, cut and numbered */
    lv_obj_t *t = aos_label(col, _("LA ÚLTIMA LÍNEA, POR CAMPOS"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 8, 0);
    lv_obj_set_style_pad_top(t, 10, 0);
    lv_obj_t *cb = card(col, w);
    if (PLA.nraw) {
        const pl_rawline_t *rl = &PLA.raw[(PLA.nraw - 1) % PL_RAW_N];
        char tok[24][24];
        int nt = pl_tokens(rl->text, p->comma, tok, 24);
        char line[400];
        size_t k = 0;
        line[0] = 0;
        for (int i = 0; i < nt && k < sizeof line - 40; i++)
            k += (size_t)snprintf(line + k, sizeof line - k, "%s%d:%s", i ? "   " : "", i + 1, tok[i]);
        utf8_clean(line);
        lv_obj_t *l = aos_label(cb, nt ? line : _("(sin campos)"), &aos_mono_22, AOS_C_TEXT);
        lv_obj_set_width(l, w - 32);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        pl_parsed_t o;
        int n = pl_parse_profile(p, rl->text, &o);
        char g[160];
        if (n < 0) pl_scpy(g, sizeof g, _("(no coincide: salteada)"));
        else if (!n) pl_scpy(g, sizeof g, _("(sin números)"));
        else {
            k = 0;
            g[0] = 0;
            for (int i = 0; i < n && k < sizeof g; i++) {
                char v[24];
                pl_fmt(v, sizeof v, o.v[i], o.dec[i]);
                k += (size_t)snprintf(g + k, sizeof g - k, "%s%s = %s %s", i ? "   " : "", o.name[i], v, o.unit[i]);
            }
        }
        lv_obj_t *gl = aos_label(cb, "", aos_font_small, AOS_C_TEAL);
        lv_label_set_text_fmt(gl, "→ %s", g);
        lv_obj_set_width(gl, w - 32);
        lv_label_set_long_mode(gl, LV_LABEL_LONG_MODE_WRAP);
    } else {
        aos_label(cb, _("(todavía no llegó nada)"), aos_font_small, AOS_C_DIM);
    }

    for (int i = 0; i < p->nf; i++) {
        pl_pfield_t *f = &p->f[i];
        lv_obj_t *c = card(col, w);
        char nm[PL_NAME + 16];
        if (f->name[0]) pl_scpy(nm, sizeof nm, f->name);
        else snprintf(nm, sizeof nm, _("Valor %d"), i + 1);
        card_title(c, w, nm, lv_color_hex(0x40C8E0));
        snprintf(b, sizeof b, "%s %d", _("Campo"), f->field);
        pl_mini(c, "−", false, AOS_C_ACCENT, field_cb, FUD(i, FD_FIELD_DN));
        pl_mini(c, b, true, AOS_C_ACCENT, NULL, NULL);
        pl_mini(c, "+", false, AOS_C_ACCENT, field_cb, FUD(i, FD_FIELD_UP));
        pl_mini(c, _("Nombre…"), false, AOS_C_ACCENT, field_cb, FUD(i, FD_NAME));
        snprintf(b, sizeof b, "%s: %s", _("Unidad"), f->unit[0] ? f->unit : "-");
        pl_mini(c, b, false, AOS_C_ACCENT, field_cb, FUD(i, FD_UNIT));
        snprintf(b, sizeof b, "× %g", (double)f->scale);
        pl_mini(c, b, f->scale != 1, AOS_C_ACCENT, field_cb, FUD(i, FD_SCALE));
        snprintf(b, sizeof b, "+ %g", (double)f->offset);
        pl_mini(c, b, f->offset != 0, AOS_C_ACCENT, field_cb, FUD(i, FD_OFFSET));
        lv_obj_t *br = lv_obj_create(c);      /* a line break in the flex row */
        lv_obj_remove_style_all(br);
        lv_obj_set_size(br, w - 32, 0);
        if (f->dp_field) snprintf(b, sizeof b, "%s %d", _("Punto según el campo"), f->dp_field);
        else pl_scpy(b, sizeof b, _("Punto: como viene"));
        pl_mini(c, "−", false, AOS_C_ACCENT, field_cb, FUD(i, FD_DP_DN));
        pl_mini(c, b, f->dp_field != 0, AOS_C_ACCENT, NULL, NULL);
        pl_mini(c, "+", false, AOS_C_ACCENT, field_cb, FUD(i, FD_DP_UP));
        if (f->dp_field) {
            snprintf(b, sizeof b, "%s: %s", _("Códigos"), f->dp_map[0] ? f->dp_map : _("el valor"));
            pl_mini(c, b, false, AOS_C_ACCENT, field_cb, FUD(i, FD_MAP));
        }
        if (p->nf > 1) pl_mini(c, _("Quitar"), false, AOS_C_RED, field_cb, FUD(i, FD_DEL));
    }
    r = section(col, w, NULL);
    if (p->nf < PL_MAXF) pl_mini(r, _("Agregar una serie"), true, AOS_C_ACCENT, prof_add_cb, NULL);
    note(col, w, _("Los campos se cuentan cortando la línea en espacios, tabs, comas, punto y coma, «:» y «=». Con el punto según otro campo, el número se divide por 10 a la cantidad de decimales que dice ese campo; los códigos traducen su valor (4:1 quiere decir que un 4 son un decimal)."));
    r = section(col, w, NULL);
    pl_mini(r, _("Borrar el perfil"), false, AOS_C_RED, prof_del_cb, NULL);
}

/* ---- series and alarms ---- */

enum { SE_SHOW, SE_BIG, SE_LO, SE_HI, SE_BEEP };

static void series_done(const char *v)
{
    if (v && PLA.edit_f >= 0 && PLA.edit_f < PLD.ns) {
        pl_series_t *s = &PLD.s[PLA.edit_f];
        char *end;
        float x = strtof(v, &end);
        bool on = end != v;
        if (PLA.edit_s == SE_LO) { s->lo_on = on; if (on) s->lo = x; }
        else { s->hi_on = on; if (on) s->hi = x; }
        s->state = 0;
        pl_cfg_alarm_store(s);
    }
    pl_sheet_series();
}

static void series_cb(lv_event_t *e)
{
    uintptr_t u = (uintptr_t)lv_event_get_user_data(e);
    int si = (int)(u >> 8), what = (int)(u & 0xFF);
    if (si >= PLD.ns) return;
    pl_series_t *s = &PLD.s[si];
    char v[32] = "";
    PLA.edit_f = si;
    PLA.edit_s = what;
    switch (what) {
    case SE_SHOW: s->hidden = !s->hidden; break;
    case SE_BIG: PLS.big = si; s->hidden = false; pl_set_save(); break;
    case SE_BEEP: s->beep = !s->beep; pl_cfg_alarm_store(s); break;
    case SE_LO:
    case SE_HI: {
        bool on = what == SE_LO ? s->lo_on : s->hi_on;
        if (on) snprintf(v, sizeof v, "%g", (double)(what == SE_LO ? s->lo : s->hi));
        pl_text_entry(what == SE_LO ? _("Avisar por debajo de… (vacío: sin aviso)") : _("Avisar por encima de… (vacío: sin aviso)"),
                      v, true, series_done);
        return;
    }
    }
    pl_chart_want();
    pl_sheet_series();
}

void pl_sheet_series(void)
{
    int32_t w;
    lv_obj_t *col = sheet_begin(SH_SERIES, _("Series y avisos"), &w);
    if (!PLD.ns) {
        note(col, w, _("Todavía no llegaron números. Cada nombre que aparece en las líneas (o cada columna) es una serie."));
        return;
    }
    for (int i = 0; i < PLD.ns; i++) {
        pl_series_t *s = &PLD.s[i];
        lv_obj_t *c = card(col, w);
        char v[24], t[80];
        pl_fmt(v, sizeof v, s->last, s->dec);
        snprintf(t, sizeof t, "%s   %s %s", s->name, v, s->unit);
        lv_obj_t *tl = card_title(c, w, t, lv_color_hex(s->col));
        if (s->state) lv_obj_set_style_text_color(tl, AOS_C_RED, 0);
        pl_mini(c, s->hidden ? _("Oculta") : _("Se ve"), !s->hidden, lv_color_hex(0x2E7D4F), series_cb, FUD(i, SE_SHOW));
        pl_mini(c, _("Grande"), PLS.big == i, AOS_C_ACCENT, series_cb, FUD(i, SE_BIG));
        char a[24];
        if (s->lo_on) pl_fmt(a, sizeof a, s->lo, s->dec < 0 ? 2 : s->dec); else pl_scpy(a, sizeof a, "-");
        snprintf(t, sizeof t, "%s %s", _("Mínimo"), a);
        pl_mini(c, t, s->lo_on, AOS_C_ORANGE, series_cb, FUD(i, SE_LO));
        if (s->hi_on) pl_fmt(a, sizeof a, s->hi, s->dec < 0 ? 2 : s->dec); else pl_scpy(a, sizeof a, "-");
        snprintf(t, sizeof t, "%s %s", _("Máximo"), a);
        pl_mini(c, t, s->hi_on, AOS_C_ORANGE, series_cb, FUD(i, SE_HI));
        pl_mini(c, _("Pitido"), s->beep, AOS_C_ORANGE, series_cb, FUD(i, SE_BEEP));
    }
    note(col, w, _("Cuando una serie sale de su mínimo o su máximo aparece un aviso, también con otra app adelante, y si tiene pitido, suena; como mucho una vez cada 10 segundos. Los límites se guardan por el nombre de la serie y se ven en el gráfico como líneas punteadas. Tocar una serie en el gráfico la pone grande; tocarla de nuevo la oculta."));
}

/* ---- the view ---- */

enum { VW_WIN, VW_MODE, VW_X, VW_Y, VW_AUTO, VW_FIX, VW_MIN, VW_MAX, VW_CLEAR };

static void view_done(const char *v)
{
    if (v) {
        char *end;
        float x = strtof(v, &end);
        if (end != v) {
            if (PLA.edit_s == VW_MIN) PLS.ymin = x;
            else PLS.ymax = x;
            if (PLS.ymax <= PLS.ymin) aos_ui_toast(_("El máximo tiene que ser mayor que el mínimo"), 2200);
            pl_set_save();
        }
    }
    pl_chart_want();
    pl_sheet_view();
}

static void view_cb(lv_event_t *e)
{
    uintptr_t u = (uintptr_t)lv_event_get_user_data(e);
    int arg = (int)(u >> 8), what = (int)(u & 0xFF);
    char v[24];
    PLA.edit_s = what;
    switch (what) {
    case VW_WIN: PLS.win = arg; PLA.win_ms = PL_WINDOWS[arg]; break;
    case VW_MODE: PLS.mode = arg; break;
    case VW_X: PLS.xs = arg; break;
    case VW_Y: PLS.ys = arg; break;
    case VW_AUTO: PLS.yfix = false; break;
    case VW_FIX:
        PLS.yfix = true;
        if (PLS.ymax <= PLS.ymin) { PLS.ymin = 0; PLS.ymax = 100; }
        break;
    case VW_MIN:
    case VW_MAX:
        snprintf(v, sizeof v, "%g", (double)(what == VW_MIN ? PLS.ymin : PLS.ymax));
        pl_text_entry(what == VW_MIN ? _("Abajo del gráfico") : _("Arriba del gráfico"), v, true, view_done);
        return;
    case VW_CLEAR: {
        aos_serial_stat_t st;
        aos_serial_stat(PLS.ch, &st);
        pl_data_clear();
        PLA.next = st.lines;          /* and not read the ring again */
        PLA.paused = false;
        break;
    }
    }
    pl_set_save();
    pl_chart_want();
    pl_sheet_view();
}

void pl_sheet_view(void)
{
    int32_t w;
    lv_obj_t *col = sheet_begin(SH_VIEW, _("Vista"), &w);
    static const char *const WN[] = { "10 s", "30 s", "1 min", "5 min", "15 min", "1 h", "4 h", "8 h" };
    lv_obj_t *r = section(col, w, _("VENTANA DE TIEMPO"));
    for (int i = 0; i < PL_NWIN; i++)
        pl_mini(r, WN[i], PLA.win_ms == PL_WINDOWS[i], AOS_C_ACCENT, view_cb, FUD(i, VW_WIN));
    r = section(col, w, _("GRÁFICO"));
    pl_mini(r, _("Contra el tiempo"), PLS.mode == PL_MODE_TIME, AOS_C_ACCENT, view_cb, FUD(PL_MODE_TIME, VW_MODE));
    pl_mini(r, _("XY: una contra otra"), PLS.mode == PL_MODE_XY, AOS_C_ACCENT, view_cb, FUD(PL_MODE_XY, VW_MODE));
    if (PLS.mode == PL_MODE_XY) {
        r = section(col, w, _("EJE X"));
        for (int i = 0; i < PLD.ns; i++) pl_mini(r, PLD.s[i].name, PLS.xs == i, lv_color_hex(PLD.s[i].col), view_cb, FUD(i, VW_X));
        r = section(col, w, _("EJE Y"));
        for (int i = 0; i < PLD.ns; i++) pl_mini(r, PLD.s[i].name, PLS.ys == i, lv_color_hex(PLD.s[i].col), view_cb, FUD(i, VW_Y));
        if (PLD.ns < 2) note(col, w, _("Hacen falta dos series con datos"));
    }
    r = section(col, w, _("ESCALA"));
    pl_mini(r, _("Automática"), !PLS.yfix, AOS_C_ACCENT, view_cb, FUD(0, VW_AUTO));
    pl_mini(r, _("Fija"), PLS.yfix, AOS_C_ACCENT, view_cb, FUD(0, VW_FIX));
    if (PLS.yfix) {
        char b[48], v[24];
        pl_fmt(v, sizeof v, PLS.ymin, 3);
        snprintf(b, sizeof b, "%s %g", _("Desde"), (double)PLS.ymin);
        pl_mini(r, b, false, AOS_C_ACCENT, view_cb, FUD(0, VW_MIN));
        snprintf(b, sizeof b, "%s %g", _("Hasta"), (double)PLS.ymax);
        pl_mini(r, b, false, AOS_C_ACCENT, view_cb, FUD(0, VW_MAX));
    }
    r = section(col, w, _("DATOS"));
    pl_mini(r, _("Borrar el gráfico"), false, AOS_C_RED, view_cb, FUD(0, VW_CLEAR));
    char t[200];
    uint32_t first = pl_data_first_t(), now = pl_now();
    char dur[24];
    pl_fmt_dur(dur, sizeof dur, PLD.rows ? now - first : 0);
    snprintf(t, sizeof t, _("%u lecturas en %s. En memoria quedan las filas de los últimos minutos u horas, según cuántas lleguen, y un resumen por segundo de las últimas 8 horas."),
             (unsigned)PLD.rows, dur);
    note(col, w, t);
    note(col, w, _("En el gráfico: un dedo lee el punto que toca; dos dedos acercan o alejan, y arrastrados van hacia atrás en el tiempo (el gráfico se pausa; En vivo vuelve)."));
}

/* ---- send ---- */

static void send_eol_cb(lv_event_t *e)
{
    PLS.eol = (int)(intptr_t)lv_event_get_user_data(e);
    pl_set_save();
    pl_sheet_send();
}

static void send_ta_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY) return;
    const char *txt = lv_textarea_get_text(PLA.sheet_ta);
    char buf[260];
    int n = snprintf(buf, sizeof buf, "%s%s", txt, EOL_SEQ[PLS.eol]);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    if (n > 0 && aos_serial_send(PLS.ch, buf, n) < 0) aos_ui_toast(_("Conectá un puerto primero"), 1800);
    lv_textarea_set_text(PLA.sheet_ta, "");
}

void pl_sheet_send(void)
{
    int32_t w;
    lv_obj_t *col = sheet_begin(SH_SEND, _("Enviar al puerto"), &w);
    lv_obj_t *r = section(col, w, _("FIN DE LÍNEA"));
    for (int i = 0; i < 4; i++) pl_mini(r, _(EOL_NAME[i]), PLS.eol == i, AOS_C_ACCENT, send_eol_cb, (void *)(intptr_t)i);
    raw_box(col, w, PLA.land ? 2 : 5);
    PLA.sheet_ta = lv_textarea_create(col);
    lv_textarea_set_one_line(PLA.sheet_ta, true);
    lv_textarea_set_placeholder_text(PLA.sheet_ta, _("Línea para enviar…"));
    lv_obj_set_size(PLA.sheet_ta, w, 80);
    lv_obj_set_style_text_font(PLA.sheet_ta, &aos_mono_22, 0);
    lv_obj_set_style_bg_color(PLA.sheet_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(PLA.sheet_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(PLA.sheet_ta, 0, 0);
    lv_obj_set_style_radius(PLA.sheet_ta, 20, 0);
    lv_obj_set_style_pad_hor(PLA.sheet_ta, 20, 0);
    lv_obj_set_style_pad_ver(PLA.sheet_ta, 22, 0);
    lv_obj_add_event_cb(PLA.sheet_ta, send_ta_cb, LV_EVENT_READY, NULL);
    PLA.sheet_kb = lv_keyboard_create(PLA.overlay);
    lv_obj_set_size(PLA.sheet_kb, PLA.W, PLA.land ? PLA.H / 2 : PLA.H * 2 / 5);
    lv_obj_align(PLA.sheet_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(PLA.sheet_kb, aos_font_body);
    lv_keyboard_set_textarea(PLA.sheet_kb, PLA.sheet_ta);
    lv_obj_set_height(s_col, PLA.H - lv_obj_get_height(PLA.sheet_kb));
}
