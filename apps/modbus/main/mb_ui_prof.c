/*
 * P4OS - Modbus: device profiles on the screen (mb_prof.h says what one is).
 *
 * One profile is active at a time, for the master and the slave alike:
 * chosen in Maestro, it is read over and over and each value shows with its
 * name and unit (a tap writes the writable ones); in Esclavo the app plays
 * the device, its values moving as the profile says (a tap sets one).
 *
 * The chooser lists the files of <card>/modbus/perfiles; the editor changes
 * one in place, saving each change. The portal's page (web/modbus.js) edits
 * the same files: an active profile whose file changes is read again. While
 * the page looks, the values go to it through the live channel.
 */
#include "mb_ui.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_APP "aos.modbus"

static struct {
    lv_obj_t *val[MB_PTS], *sub[MB_PTS];
    bool slave_view;
    uint32_t seen;
    uint32_t mtime;             /* of the active profile's file */
    mb_prof_t E;                /* the profile being edited */
    int ept;                    /* the point being edited */
    int edit_from;              /* 0 chooser, 1 the page */
    bool del_armed;
    char names[32][48];
} P;

static void sheet_chooser(void);
static void sheet_editor(void);
static void sheet_point(void);

/* ---- the active profile ---- */

static void activate(const mb_prof_t *p)
{
    mb_lock();
    if (p) {
        MB.prof = *p;
        MB.prof_on = true;
        if (p->baud) MB.cfg.baud = p->baud;
        if (p->par) { MB.cfg.par = p->par; MB.cfg.stop = p->stop ? p->stop : 1; }
        if (p->unit) MB.s_unit = p->unit;
        MB.mreq.unit = p->unit;
    } else MB.prof_on = false;
    for (int i = 0; i < MB_PTS; i++) MB.prc[i] = MB_E_TIMEOUT;     /* nothing read or played yet */
    MB.pv_seq++;
    MB.prof_gen++;
    MB.seq++;
    mb_unlock();
    aos_hal_pref_set_str("mbt_prof", p ? p->file : "");
    P.mtime = p ? mb_prof_mtime(p->file) : 0;
    mb_settings_save();
}

static bool load_and_activate(const char *file, bool talk)
{
    static mb_prof_t tmp;
    char err[64];
    if (strchr(file, '/') || strstr(file, "..")) return false;     /* a name in the folder, nothing else */
    if (!mb_prof_load(file, &tmp, err, sizeof err)) { aos_ui_toast(err, 2500); return false; }
    if (err[0]) aos_ui_toast(err, 2500);
    activate(&tmp);
    if (talk) {
        mb_lock();
        bool master = MB.role == MB_ROLE_MASTER;
        if (master) MB.want_open = true;        /* chosen to look at it: read it now */
        mb_unlock();
    }
    return true;
}

void prof_init(void)
{
    mb_prof_examples();
    char f[48];
    if (aos_hal_pref_get_str("mbt_prof", f, sizeof f) && f[0]) {
        if (!load_and_activate(f, false)) activate(NULL);
    }
}

/* Every couple of seconds from the timer: a file the portal changed is read
 * again; the values go to the portal's page while it looks. */
void prof_tick(void)
{
    static uint32_t n;
    n++;
    mb_lock();
    bool on = MB.prof_on;
    char file[48];
    snprintf(file, sizeof file, "%s", MB.prof.file);
    mb_unlock();
    if (on && n % 13 == 0) {
        uint32_t m = mb_prof_mtime(file);
        if (m && m != P.mtime) {
            if (load_and_activate(file, false)) {
                aos_ui_toast(_("Perfil actualizado"), 1500);
                if (MBU.tab <= TAB_SLAVE && !MBU.overlay) mb_rebuild_page();
            }
        }
    }
    /* the page asks: "perfil=<file>" uses one, "perfil=" none */
    char msg[96];
    int ml = aos_hal_live_take(LIVE_APP, msg, sizeof msg - 1);
    if (ml > 0) {
        msg[ml] = 0;
        if (!strncmp(msg, "perfil=", 7)) {
            if (msg[7]) load_and_activate(msg + 7, true);
            else activate(NULL);
            if (!MBU.overlay) mb_rebuild_page();
            mb_bar_refresh();
        }
    }
    if (n % 3 || aos_hal_live_idle_ms(LIVE_APP) > 4000) return;
    static char j[8192];
    static mb_prof_t p;
    static double v[MB_PTS];
    static int rc[MB_PTS];
    mb_lock();
    p = MB.prof;
    memcpy(v, MB.pv, sizeof v);
    memcpy(rc, MB.prc, sizeof rc);
    bool open = MB.open;
    int role = MB.role;
    on = MB.prof_on;
    mb_unlock();
    size_t k = (size_t)snprintf(j, sizeof j, "{\"perfil\":\"%s\",\"rol\":%d,\"abierto\":%s,\"pts\":[", on ? p.file : "", role, open ? "true" : "false");
    for (int i = 0; on && i < p.n && k + 160 < sizeof j; i++) {
        char t[48];
        if (rc[i]) snprintf(t, sizeof t, "%s", "");
        else mb_pt_format(&p.pt[i], v[i], t, sizeof t);
        k += (size_t)snprintf(j + k, sizeof j - k, "%s{\"i\":%d,\"v\":\"", i ? "," : "", i);
        for (const char *c = t; *c && k + 4 < sizeof j; c++) { if (*c == '"' || *c == '\\') j[k++] = '\\'; j[k++] = *c; }
        k += (size_t)snprintf(j + k, sizeof j - k, "\",\"e\":%d}", rc[i]);
    }
    k += (size_t)snprintf(j + k, sizeof j - k, "]}");
    aos_hal_live_put(LIVE_APP, "valores", "application/json", j, k);
}

/* ---- the points, as Maestro and Esclavo show them ---- */

static void open_link(void)
{
    mb_lock();
    MB.want_open = true;
    mb_unlock();
    mb_bar_refresh();
}

static int s_tap;

static void write_done(double v)
{
    mb_lock();
    mb_point_t pt = MB.prof.pt[s_tap];
    uint8_t unit = MB.prof.unit;
    bool slave = MB.role == MB_ROLE_SLAVE;
    mb_unlock();
    if (slave) { mb_bus_point_put(&pt, v); return; }
    uint16_t r[2];
    mb_pt_encode(&pt, v, r);
    mb_lock();
    if (MB.m_wqn < 8) {
        mb_mreq_t *w = &MB.m_wq[MB.m_wqn++];
        memset(w, 0, sizeof *w);
        w->unit = unit;
        w->addr = pt.addr;
        if (pt.table == MB_TAB_COIL) { w->fc = 5; w->wv[0] = r[0]; }
        else if (mb_pt_words(&pt) == 1) { w->fc = 6; w->wv[0] = r[0]; }
        else { w->fc = 16; w->wv[0] = r[0]; w->wv[1] = r[1]; w->wn = 2; }
        w->wn = w->wn ? w->wn : 1;
    }
    mb_unlock();
    open_link();
}

static void point_cb(lv_event_t *e)
{
    s_tap = (int)(intptr_t)lv_event_get_user_data(e);
    mb_lock();
    mb_point_t pt = MB.prof.pt[s_tap];
    double v = MB.pv[s_tap];
    mb_unlock();
    if (pt.type == MB_T_BIT) { write_done(v ? 0 : 1); return; }
    char t[64];
    snprintf(t, sizeof t, "%s%s%s%s", pt.name, pt.unit[0] ? " (" : "", pt.unit, pt.unit[0] ? ")" : "");
    if (pt.type == MB_T_HEX) MBU.kp_hex = true;
    mb_keypad(t, v, mb_pt_decimals(&pt), "", write_done);
}

static void edit_cb(lv_event_t *e)
{
    (void)e;
    mb_lock();
    P.E = MB.prof;
    mb_unlock();
    P.edit_from = 1;
    sheet_editor();
}

static void chooser_cb(lv_event_t *e) { (void)e; sheet_chooser(); }

void prof_chip(lv_obj_t *parent, int32_t w)
{
    mb_lock();
    bool on = MB.prof_on;
    char name[48];
    snprintf(name, sizeof name, "%s", MB.prof.name);
    mb_unlock();
    lv_obj_t *r = mb_chips_row(parent, w);
    char t[80];
    if (on) snprintf(t, sizeof t, "%s: %s", _("Perfil"), name);
    else snprintf(t, sizeof t, "%s", _("Sin perfil: a mano"));
    mb_chip(r, t, on, chooser_cb, NULL);
    if (on) mb_chip(r, _("Editar"), false, edit_cb, NULL);
}

/* "hr 0 · u16 · rw · seno": what a point is, under its name */
static void sub_text(const mb_point_t *pt, bool slave, char *o, size_t n)
{
    snprintf(o, n, "%s %u · %s%s%s%s", MB_TABLE_NAME[pt->table], pt->addr, MB_TYPE_NAME[pt->type], pt->rw ? " · rw" : "",
             slave && pt->sim ? " · " : "", slave && pt->sim ? MB_SIM_NAME[pt->sim] : "");
}

void prof_refresh(void)
{
    mb_lock();
    uint32_t seq = MB.pv_seq;
    mb_unlock();
    if (seq == P.seen) return;
    P.seen = seq;
    static mb_prof_t p;
    static double v[MB_PTS];
    static int rc[MB_PTS];
    mb_lock();
    p = MB.prof;
    memcpy(v, MB.pv, sizeof v);
    memcpy(rc, MB.prc, sizeof rc);
    bool open = MB.open;
    mb_unlock();
    for (int i = 0; i < p.n; i++) {
        if (!P.val[i]) continue;
        char t[48];
        if (rc[i]) {
            lv_label_set_text(P.val[i], "—");
            lv_obj_set_style_text_color(P.val[i], AOS_C_DIM, 0);
            lv_label_set_text_fmt(P.sub[i], "%s %u · %s", MB_TABLE_NAME[p.pt[i].table], p.pt[i].addr,
                                  rc[i] == MB_E_TIMEOUT && !open ? _("sin leer") : mb_strerror(rc[i]));
            continue;
        }
        char sub[80];
        sub_text(&p.pt[i], P.slave_view, sub, sizeof sub);
        if (strcmp(lv_label_get_text(P.sub[i]), sub)) lv_label_set_text(P.sub[i], sub);   /* an error shown before goes */
        mb_pt_format(&p.pt[i], v[i], t, sizeof t);
        lv_label_set_text(P.val[i], t);
        lv_obj_set_style_text_color(P.val[i], p.pt[i].type == MB_T_BIT ? (v[i] ? AOS_C_GREEN : AOS_C_DIM) : C_AMBER, 0);
    }
}

void prof_points(lv_obj_t *parent, int32_t w, bool slave)
{
    memset(P.val, 0, sizeof P.val);
    memset(P.sub, 0, sizeof P.sub);
    P.slave_view = slave;
    P.seen = 0xFFFFFFFF;
    static mb_prof_t p;
    mb_lock();
    p = MB.prof;
    mb_unlock();
    char t[160];
    lv_obj_t *hd = mb_caption(parent, "", w);
    snprintf(t, sizeof t, "%s%s%s %u · %s %ums", p.note, p.note[0] ? "\n" : "", _("esclavo"), p.unit, _("cada"), p.poll_ms);
    lv_label_set_text(hd, t);
    if (!p.n) { mb_caption(parent, _("Este perfil no tiene puntos todavía: Editar los agrega."), w); return; }
    int cols = MBU.land && w > 600 ? 2 : 1;
    int32_t cw = (w - (cols - 1) * 10) / cols;
    lv_obj_t *g = mb_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, 10, 0);
    for (int i = 0; i < p.n; i++) {
        const mb_point_t *pt = &p.pt[i];
        bool tap = slave || pt->rw;
        lv_obj_t *c = mb_card(g, cw, 84);
        lv_obj_set_style_pad_hor(c, 16, 0);
        if (tap) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
            lv_obj_add_event_cb(c, point_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_obj_t *n = aos_label(c, pt->name, aos_font_small, AOS_C_TEXT);
        lv_obj_set_size(n, cw * 45 / 100, lv_font_get_line_height(aos_font_small));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 10);
        char sub[80];
        sub_text(pt, slave, sub, sizeof sub);
        P.sub[i] = aos_label(c, sub, aos_font_tiny, AOS_C_DIM);
        lv_obj_align(P.sub[i], LV_ALIGN_BOTTOM_LEFT, 0, -10);
        P.val[i] = aos_label(c, "—", cols > 1 || w < 640 ? aos_font_body : aos_font_title, C_AMBER);
        lv_obj_align(P.val[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    mb_caption(parent, slave ? _("La app hace de este equipo: contesta en sus direcciones y los valores se mueven como dice el perfil. Tocá uno para cambiarlo (uno que se mueve vuelve a moverse).")
                             : _("Lee el perfil entero cada período. Tocá un valor que se puede escribir (rw) para cambiarlo."), w);
    prof_refresh();
}

/* ---- the chooser ---- */

static lv_obj_t *sheet_base(const char *title, lv_event_cb_t done, lv_obj_t **col, int32_t *w)
{
    mb_overlay_close();
    MBU.overlay = mb_box(MBU.root, MBU.W, MBU.H);
    lv_obj_set_style_bg_color(MBU.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(MBU.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(MBU.overlay, LV_OBJ_FLAG_CLICKABLE);
    *w = MBU.W - 2 * AOS_UI_PAD;
    *col = mb_column(MBU.overlay, *w, MBU.H - 20);
    lv_obj_set_pos(*col, AOS_UI_PAD, 20);
    lv_obj_add_flag(*col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *head = mb_box(*col, *w, 80);
    lv_obj_t *tl = aos_label(head, title, aos_font_large, AOS_C_TEXT);
    lv_obj_set_width(tl, *w - 220);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_align(mb_pill(head, AOS_SYM_CHECK, _("Listo"), C_TEAL, done, NULL), LV_ALIGN_RIGHT_MID, 0, 0);
    return head;
}

static void close_cb(lv_event_t *e) { (void)e; mb_overlay_close(); mb_rebuild_page(); }

static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0) activate(NULL);
    else load_and_activate(P.names[i], true);
    mb_overlay_close();
    mb_rebuild_page();
    mb_bar_refresh();
}

static void new_done(const char *name)
{
    if (!name[0]) { sheet_chooser(); return; }
    memset(&P.E, 0, sizeof P.E);
    snprintf(P.E.name, sizeof P.E.name, "%s", name);
    mb_prof_file_for(name, P.E.file, sizeof P.E.file);
    for (int k = 2; mb_prof_mtime(P.E.file) && k < 100; k++) {       /* a name already taken */
        char base[40];
        mb_prof_file_for(name, base, sizeof base);
        base[strlen(base) - 4] = 0;
        snprintf(P.E.file, sizeof P.E.file, "%.40s-%d.csv", base, k);
    }
    mb_lock();
    P.E.unit = MB.mreq.unit ? MB.mreq.unit : 1;
    P.E.baud = MB.cfg.baud;
    P.E.par = MB.cfg.par ? MB.cfg.par : 'N';
    P.E.stop = MB.cfg.stop ? MB.cfg.stop : 1;
    mb_unlock();
    P.E.poll_ms = 1000;
    if (!mb_prof_save(&P.E)) { aos_ui_toast(_("No se pudo escribir en la tarjeta"), 2500); return; }
    activate(&P.E);
    P.edit_from = 0;
    sheet_editor();
}

static void new_cb(lv_event_t *e) { (void)e; mb_text_entry(_("Nombre del perfil nuevo"), "", new_done); }

static void sheet_chooser(void)
{
    lv_obj_t *col;
    int32_t w;
    sheet_base(_("Perfiles"), close_cb, &col, &w);
    P.del_armed = false;
    mb_lock();
    bool on = MB.prof_on;
    char cur[48];
    snprintf(cur, sizeof cur, "%s", MB.prof.file);
    mb_unlock();
    int n = mb_prof_list(P.names, 32);
    static mb_prof_t tmp;
    lv_obj_t *none = mb_card(col, w, 84);
    lv_obj_set_style_pad_hor(none, 20, 0);
    lv_obj_add_flag(none, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(none, !on ? lv_color_hex(0x1F3A30) : AOS_C_CARD, 0);
    lv_obj_add_event_cb(none, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_align(aos_label(none, _("Sin perfil: el pedido a mano"), aos_font_body, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    for (int i = 0; i < n; i++) {
        char err[64];
        bool ok = mb_prof_load(P.names[i], &tmp, err, sizeof err);
        lv_obj_t *c = mb_card(col, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 18, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 4, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c, on && !strcmp(cur, P.names[i]) ? lv_color_hex(0x1F3A30) : AOS_C_CARD, 0);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        aos_make_decorative(aos_label(c, ok ? tmp.name : P.names[i], aos_font_body, AOS_C_TEXT));
        char t[200];
        if (ok) {
            char link[32] = "";
            if (tmp.baud) snprintf(link, sizeof link, " · %lu 8%c%u", (unsigned long)tmp.baud, tmp.par ? tmp.par : 'N', tmp.stop ? tmp.stop : 1);
            snprintf(t, sizeof t, "%s%s%s %u%s · %d %s · %s", tmp.note, tmp.note[0] ? "\n" : "", _("esclavo"), tmp.unit, link, tmp.n, _("puntos"), P.names[i]);
        } else snprintf(t, sizeof t, "%s", err);
        lv_obj_t *l = aos_label(c, t, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(l, w - 36);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        aos_make_decorative(l);
    }
    lv_obj_t *r = mb_chips_row(col, w);
    mb_pill(r, AOS_SYM_PLUS, _("Nuevo"), AOS_C_CARD2, new_cb, NULL);
    mb_caption(col, _("Un perfil dice qué tiene un equipo y dónde: al elegirlo, Maestro lo lee entero y Esclavo hace de ese equipo. Toma su unidad, velocidad y formato. Son archivos CSV en la tarjeta (modbus/perfiles): la página Modbus del portal los edita, los sube y los baja."), w);
}

/* ---- the editor ---- */

static void ed_save(void)
{
    if (!mb_prof_save(&P.E)) { aos_ui_toast(_("No se pudo escribir en la tarjeta"), 2500); return; }
    mb_lock();
    bool active = MB.prof_on && !strcmp(MB.prof.file, P.E.file);
    mb_unlock();
    if (active) activate(&P.E);
}

static void ed_done_cb(lv_event_t *e)
{
    (void)e;
    if (P.edit_from == 0) sheet_chooser();
    else { mb_overlay_close(); mb_rebuild_page(); }
}

static void ed_name_done(const char *s) { if (s[0]) { snprintf(P.E.name, sizeof P.E.name, "%s", s); ed_save(); } sheet_editor(); }
static void ed_note_done(const char *s) { snprintf(P.E.note, sizeof P.E.note, "%s", s); ed_save(); sheet_editor(); }
static void ed_name_cb(lv_event_t *e) { (void)e; mb_text_entry(_("Nombre"), P.E.name, ed_name_done); }
static void ed_note_cb(lv_event_t *e) { (void)e; mb_text_entry(_("Nota (qué es, de dónde salió el mapa)"), P.E.note, ed_note_done); }
static void ed_unit_done(double v) { P.E.unit = (uint8_t)(v < 0 ? 0 : v > 247 ? 247 : v); ed_save(); sheet_editor(); }
static void ed_unit_cb(lv_event_t *e) { (void)e; mb_keypad(_("Unidad del esclavo"), P.E.unit, 0, "", ed_unit_done); }
static void ed_link_cb(lv_event_t *e)
{
    (void)e;
    mb_lock();
    P.E.baud = MB.cfg.baud;
    P.E.par = MB.cfg.par ? MB.cfg.par : 'N';
    P.E.stop = MB.cfg.stop ? MB.cfg.stop : 1;
    mb_unlock();
    ed_save();
    sheet_editor();
}
static const uint16_t POLLS[] = { 250, 500, 1000, 2000, 5000 };
static void ed_poll_cb(lv_event_t *e) { P.E.poll_ms = POLLS[(intptr_t)lv_event_get_user_data(e)]; ed_save(); sheet_editor(); }
static void ed_pt_cb(lv_event_t *e) { P.ept = (int)(intptr_t)lv_event_get_user_data(e); sheet_point(); }
static void ed_add_cb(lv_event_t *e)
{
    (void)e;
    if (P.E.n >= MB_PTS) { aos_ui_toast(_("No entran más puntos"), 2000); return; }
    mb_point_t *pt = &P.E.pt[P.E.n];
    memset(pt, 0, sizeof *pt);
    const mb_point_t *last = P.E.n ? &P.E.pt[P.E.n - 1] : NULL;
    pt->table = last ? last->table : MB_TAB_HR;
    pt->addr = last ? (uint16_t)(last->addr + mb_pt_words(last)) : 0;
    pt->type = pt->table <= MB_TAB_DI ? MB_T_BIT : MB_T_U16;
    pt->scale = 1;
    pt->max = pt->type == MB_T_BIT ? 1 : 100;
    snprintf(pt->name, sizeof pt->name, "%s %d", _("Valor"), P.E.n + 1);
    P.ept = P.E.n++;
    ed_save();
    sheet_point();
}
static void ed_del_cb(lv_event_t *e)
{
    (void)e;
    if (!P.del_armed) { P.del_armed = true; aos_ui_toast(_("Tocá de nuevo para borrar el archivo"), 2500); return; }
    mb_lock();
    bool active = MB.prof_on && !strcmp(MB.prof.file, P.E.file);
    mb_unlock();
    if (active) activate(NULL);
    mb_prof_delete(P.E.file);
    sheet_chooser();
}

static void sheet_editor(void)
{
    lv_obj_t *col;
    int32_t w;
    sheet_base(P.E.name, ed_done_cb, &col, &w);
    P.del_armed = false;
    mb_section(col, P.E.file);
    lv_obj_t *r = mb_chips_row(col, w);
    char t[80];
    snprintf(t, sizeof t, "%s: %s", _("Nombre"), P.E.name);
    mb_chip(r, t, false, ed_name_cb, NULL);
    mb_chip(r, P.E.note[0] ? P.E.note : _("Nota…"), false, ed_note_cb, NULL);
    r = mb_chips_row(col, w);
    snprintf(t, sizeof t, "%s %u", _("Esclavo"), P.E.unit);
    mb_chip(r, t, false, ed_unit_cb, NULL);
    if (P.E.baud) snprintf(t, sizeof t, "%lu 8%c%u · %s", (unsigned long)P.E.baud, P.E.par ? P.E.par : 'N', P.E.stop ? P.E.stop : 1, _("tomar la de la conexión"));
    else snprintf(t, sizeof t, "%s", _("Velocidad: tomar la de la conexión"));
    mb_chip(r, t, false, ed_link_cb, NULL);
    mb_section(col, _("LEERLO CADA"));
    r = mb_chips_row(col, w);
    for (size_t i = 0; i < sizeof POLLS / sizeof POLLS[0]; i++) {
        snprintf(t, sizeof t, "%u ms", POLLS[i]);
        mb_chip(r, t, P.E.poll_ms == POLLS[i], ed_poll_cb, (void *)(intptr_t)i);
    }
    mb_section(col, _("PUNTOS"));
    for (int i = 0; i < P.E.n; i++) {
        const mb_point_t *pt = &P.E.pt[i];
        lv_obj_t *c = mb_card(col, w, 72);
        lv_obj_set_style_pad_hor(c, 18, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, ed_pt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *n = aos_label(c, pt->name, aos_font_small, AOS_C_TEXT);
        lv_obj_set_width(n, w / 2);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        char sc[16];
        snprintf(sc, sizeof sc, "%g", (double)pt->scale);
        snprintf(t, sizeof t, "%s %u · %s%s%s %s%s", MB_TABLE_NAME[pt->table], pt->addr, MB_TYPE_NAME[pt->type],
                 pt->type == MB_T_BIT || pt->scale == 1 ? "" : " x", pt->type == MB_T_BIT || pt->scale == 1 ? "" : sc, pt->unit, pt->rw ? " · rw" : "");
        lv_obj_t *d = aos_label(c, t, aos_font_caption, AOS_C_DIM);
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    r = mb_chips_row(col, w);
    mb_pill(r, AOS_SYM_PLUS, _("Agregar punto"), C_TEAL, ed_add_cb, NULL);
    mb_pill(r, AOS_SYM_DELETE, _("Borrar perfil"), AOS_C_CARD2, ed_del_cb, NULL);
    mb_caption(col, _("Cada cambio se guarda en la tarjeta. Un punto nuevo sigue al último; tocalo para cambiarlo."), w);
}

/* ---- one point ---- */

static mb_point_t *EP(void) { return &P.E.pt[P.ept]; }
static void pt_changed(void) { ed_save(); sheet_point(); }
static void pt_name_done(const char *s) { if (s[0]) snprintf(EP()->name, sizeof EP()->name, "%s", s); pt_changed(); }
static void pt_unit_done(const char *s) { snprintf(EP()->unit, sizeof EP()->unit, "%s", s); pt_changed(); }
static void pt_name_cb(lv_event_t *e) { (void)e; mb_text_entry(_("Nombre del punto"), EP()->name, pt_name_done); }
static void pt_unit_cb(lv_event_t *e) { (void)e; mb_text_entry(_("Unidad (°C, V, rpm…)"), EP()->unit, pt_unit_done); }
static void pt_table_cb(lv_event_t *e)
{
    mb_point_t *pt = EP();
    pt->table = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    bool bits = pt->table <= MB_TAB_DI;
    if (bits) { pt->type = MB_T_BIT; pt->scale = 1; }
    else if (pt->type == MB_T_BIT) pt->type = MB_T_U16;
    if (pt->table == MB_TAB_DI || pt->table == MB_TAB_IR) pt->rw = false;
    pt_changed();
}
static void pt_addr_done(double v) { EP()->addr = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); pt_changed(); }
static void pt_addr_cb(lv_event_t *e) { (void)e; MBU.kp_hex = true; mb_keypad(_("Dirección (desde 0; 0x para hex)"), EP()->addr, 0, "", pt_addr_done); }
static void pt_type_cb(lv_event_t *e) { EP()->type = (uint8_t)(intptr_t)lv_event_get_user_data(e); pt_changed(); }
static const float SCALES[] = { 1, 0.1f, 0.01f, 0.001f, 10, 100 };
static void pt_scale_cb(lv_event_t *e) { EP()->scale = SCALES[(intptr_t)lv_event_get_user_data(e)]; pt_changed(); }
static void pt_scale_done(double v) { if (v != 0) EP()->scale = (float)v; pt_changed(); }
static void pt_scale_other_cb(lv_event_t *e) { (void)e; mb_keypad(_("Escala: el valor es el número crudo por esto"), EP()->scale, 4, "", pt_scale_done); }
static void pt_rw_cb(lv_event_t *e) { EP()->rw = (intptr_t)lv_event_get_user_data(e) != 0; pt_changed(); }
static void pt_sim_cb(lv_event_t *e) { EP()->sim = (uint8_t)(intptr_t)lv_event_get_user_data(e); pt_changed(); }
static void pt_min_done(double v) { EP()->min = (float)v; pt_changed(); }
static void pt_max_done(double v) { EP()->max = (float)v; pt_changed(); }
static void pt_val_done(double v) { EP()->value = (float)v; pt_changed(); }
static void pt_min_cb(lv_event_t *e) { (void)e; mb_keypad(_("Mínimo"), EP()->min, 3, EP()->unit, pt_min_done); }
static void pt_max_cb(lv_event_t *e) { (void)e; mb_keypad(_("Máximo"), EP()->max, 3, EP()->unit, pt_max_done); }
static void pt_val_cb(lv_event_t *e) { (void)e; mb_keypad(_("Valor inicial"), EP()->value, 3, EP()->unit, pt_val_done); }
static void pt_done_cb(lv_event_t *e) { (void)e; sheet_editor(); }
static void pt_del_cb(lv_event_t *e)
{
    (void)e;
    memmove(&P.E.pt[P.ept], &P.E.pt[P.ept + 1], (size_t)(P.E.n - P.ept - 1) * sizeof P.E.pt[0]);
    P.E.n--;
    ed_save();
    sheet_editor();
}

static void sheet_point(void)
{
    lv_obj_t *col;
    int32_t w;
    mb_point_t *pt = EP();
    sheet_base(pt->name, pt_done_cb, &col, &w);
    char t[80];
    lv_obj_t *r = mb_chips_row(col, w);
    snprintf(t, sizeof t, "%s: %s", _("Nombre"), pt->name);
    mb_chip(r, t, false, pt_name_cb, NULL);
    snprintf(t, sizeof t, "%s: %s", _("Unidad"), pt->unit[0] ? pt->unit : "—");
    mb_chip(r, t, false, pt_unit_cb, NULL);
    mb_section(col, _("TABLA Y DIRECCIÓN"));
    r = mb_chips_row(col, w);
    static const char *const TN[5] = { "", N_("coil (01)"), N_("entrada (02)"), N_("holding (03)"), N_("input (04)") };
    for (int k = 1; k <= 4; k++) mb_chip(r, aos_tr(TN[k]), pt->table == k, pt_table_cb, (void *)(intptr_t)k);
    snprintf(t, sizeof t, "%s %u (0x%04X)", _("Dirección"), pt->addr, pt->addr);
    mb_chip(r, t, false, pt_addr_cb, NULL);
    if (pt->table >= MB_TAB_HR) {
        mb_section(col, _("TIPO"));
        r = mb_chips_row(col, w);
        for (int k = 1; k < MB_T_COUNT; k++) mb_chip(r, MB_TYPE_NAME[k], pt->type == k, pt_type_cb, (void *)(intptr_t)k);
        mb_caption(col, _("32 bits: u32, s32 y f32 toman dos registros, el alto primero (ABCD); los \"cdab\", el bajo primero."), w);
        mb_section(col, _("ESCALA (VALOR = CRUDO x ESCALA)"));
        r = mb_chips_row(col, w);
        bool std = false;
        for (size_t k = 0; k < sizeof SCALES / sizeof SCALES[0]; k++) {
            snprintf(t, sizeof t, "x%g", (double)SCALES[k]);
            mb_chip(r, t, pt->scale == SCALES[k], pt_scale_cb, (void *)(intptr_t)k);
            std |= pt->scale == SCALES[k];
        }
        if (std) mb_chip(r, _("otra…"), false, pt_scale_other_cb, NULL);
        else {
            snprintf(t, sizeof t, "x%g", (double)pt->scale);
            mb_chip(r, t, true, pt_scale_other_cb, NULL);
        }
    }
    if (pt->table == MB_TAB_COIL || pt->table == MB_TAB_HR) {
        mb_section(col, _("ACCESO"));
        r = mb_chips_row(col, w);
        mb_chip(r, _("sólo lectura"), !pt->rw, pt_rw_cb, (void *)0);
        mb_chip(r, _("lectura y escritura"), pt->rw, pt_rw_cb, (void *)1);
    }
    mb_section(col, _("CUANDO LA APP HACE DE ESCLAVO"));
    r = mb_chips_row(col, w);
    for (int k = 0; k < MB_SIM_COUNT; k++) mb_chip(r, MB_SIM_NAME[k], pt->sim == k, pt_sim_cb, (void *)(intptr_t)k);
    r = mb_chips_row(col, w);
    char a[24];
    snprintf(a, sizeof a, "%g", (double)pt->min);
    snprintf(t, sizeof t, "%s %s", _("mín"), a);
    mb_chip(r, t, false, pt_min_cb, NULL);
    snprintf(a, sizeof a, "%g", (double)pt->max);
    snprintf(t, sizeof t, "%s %s", _("máx"), a);
    mb_chip(r, t, false, pt_max_cb, NULL);
    snprintf(a, sizeof a, "%g", (double)pt->value);
    snprintf(t, sizeof t, "%s %s", _("empieza en"), a);
    mb_chip(r, t, false, pt_val_cb, NULL);
    mb_caption(col, _("fijo se queda en el valor inicial; seno y rampa van de mín a máx en un minuto; deriva camina al azar entre los dos; contador sube máx por hora; alterna cambia cada pocos segundos; azar salta una vez por segundo."), w);
    r = mb_chips_row(col, w);
    mb_pill(r, AOS_SYM_DELETE, _("Borrar este punto"), AOS_C_CARD2, pt_del_cb, NULL);
}

void prof_sheet(void) { sheet_chooser(); }

/* The page went away: its labels with it. */
void prof_forget(void)
{
    memset(P.val, 0, sizeof P.val);
    memset(P.sub, 0, sizeof P.sub);
}
