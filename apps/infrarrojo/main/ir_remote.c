/*
 * P4OS - Infrarrojo: the devices, and each one as a remote.
 *
 * A remote is laid out from its buttons' roles, the way real ones are: power
 * and the source on top, the arrows round OK, the volume and channel
 * rockers side by side, the transport keys, the four colours, the digits,
 * and then whatever has no role, by name. Portrait stacks the groups;
 * landscape puts them in two columns. Edit mode turns every key into a
 * door to its sheet (name, role, learn again, a code typed by hand) and
 * adds the device's own options.
 *
 * A key sends on press, and a held key sends its protocol's repeat (NEC's
 * short repeat code, the whole frame for the rest) every LVGL repeat
 * period, as a real remote does for the volume.
 *
 * An air conditioner from the library is a thermostat instead: mode, fan
 * and the other levels its file has, the temperature, on and off. Each
 * change is sent on its own 0.6 s after the last tap, as the remotes of
 * those machines do, and the state is kept in the device's file.
 *
 * The folder is watched every two seconds: what the portal changes shows up
 * here (never while a sheet is open, which holds an index into the list).
 */
#include "ir.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    lv_obj_t   *page;
    ir_dev_t   *devs;
    int         ndev;
    uint32_t    sig, sig_ms;
    int         open;           /* -1: the list */
    lv_obj_t   *led;
    uint32_t    sent_seen, led_ms;
    /* the air conditioner */
    ir_pack_blob_t blob;
    int         blob_dev;
    lv_obj_t   *ac_body;
    bool        ac_dirty, ac_sent_on;
    uint32_t    ac_ms;
    /* the button's sheet */
    int         sb;             /* button index, -1 new */
    lv_obj_t   *ta_name, *dd_role, *code_lbl, *hand, *dd_proto, *ta_addr, *ta_cmd;
    /* the device's sheet */
    lv_obj_t   *ta_dev;
    int         kind;
    lv_obj_t   *kind_btn[IR_K_COUNT];
    bool        dev_new;
} R = { .open = -1, .blob_dev = -1 };

static char s_open_file[48];    /* across a rebuild */
static bool s_edit;

static void show_list(void);
static void show_remote(void);
static void button_sheet(int idx);

static ir_dev_t *cur(void) { return (R.open >= 0 && R.open < R.ndev) ? &R.devs[R.open] : NULL; }

ir_dev_t *ir_remote_devices(int *n)
{
    *n = R.ndev;
    return R.devs;
}

ir_dev_t *ir_remote_find(const char *file)
{
    for (int i = 0; i < R.ndev; i++) if (!strcmp(R.devs[i].file, file)) return &R.devs[i];
    return NULL;
}

static void rescan(void)
{
    ir_store_free_all(R.devs, R.ndev);
    R.ndev = ir_store_scan(&R.devs);
    R.sig = ir_store_signature();
    R.open = -1;
    if (s_open_file[0])
        for (int i = 0; i < R.ndev; i++) if (!strcmp(R.devs[i].file, s_open_file)) R.open = i;
    if (R.open < 0) s_open_file[0] = 0;
}

static void redraw(void)
{
    if (!R.page) return;
    if (R.open >= 0) show_remote();
    else show_list();
}

void ir_remote_reload(void)
{
    if (!R.page || ir_sheet_is_open()) return;
    if (ir_store_signature() == R.sig) return;
    rescan();
    redraw();
}

void ir_remote_open_file(const char *file)
{
    ir_copy(s_open_file, sizeof s_open_file, file);
    s_edit = false;
    rescan();
    redraw();
}

void ir_remote_saved(const char *file)
{
    ir_dev_t *d = ir_remote_find(file);
    if (d) ir_dev_save(d);
    R.sig = ir_store_signature();
    redraw();
}

bool ir_remote_back(void)
{
    if (R.open < 0) return false;
    if (s_edit) { s_edit = false; show_remote(); return true; }
    R.open = -1;
    s_open_file[0] = 0;
    show_list();
    return true;
}

/* ------------------------------------------------------------------ the list */

static void open_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= R.ndev) return;
    R.open = i;
    ir_copy(s_open_file, sizeof s_open_file, R.devs[i].file);
    s_edit = false;
    show_remote();
}

static void dev_sheet(bool is_new);

static void new_cb(lv_event_t *e) { (void)e; dev_sheet(true); }
static void to_base_cb(lv_event_t *e) { (void)e; ir_ui_goto_tab(2); }
static void to_learn_cb(lv_event_t *e) { (void)e; ir_ui_goto_tab(1); }

static lv_color_t kind_color(int k)
{
    static const uint32_t C[IR_K_COUNT] = { 0x0A84FF, 0xBF5AF2, 0x40C8E0, 0x30D158, 0xFF9F0A, 0x8E8E93 };
    return lv_color_hex(C[k >= 0 && k < IR_K_COUNT ? k : IR_K_OTHER]);
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_color_t color, int32_t d,
                           lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_t *g = lv_label_create(b);
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_font(g, d >= 80 ? &aos_sym_44 : &aos_sym_28, 0);
    lv_obj_set_style_text_color(g, AOS_C_TEXT, 0);
    lv_obj_center(g);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static void show_list(void)
{
    lv_obj_clean(R.page);
    R.led = NULL;
    R.ac_body = NULL;
    lv_obj_t *h = ir_ui_header(R.page, _("Controles"), NULL, NULL, NULL);
    lv_obj_t *add = round_btn(h, AOS_SYM_PLUS, AOS_C_ACCENT, 72, new_cb, NULL);
    lv_obj_align(add, LV_ALIGN_RIGHT_MID, -AOS_UI_PAD, 6);
    lv_obj_t *s = ir_ui_scroll(R.page);
    lv_obj_set_style_pad_row(s, 12, 0);
    if (!R.ndev) {
        lv_obj_t *c = ir_ui_card(s);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, _("Todavía no hay aparatos."));
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
        l = lv_label_create(c);
        lv_label_set_text(l, _("Creá uno con + y aprendé los botones de su control, o buscá tu tele o tu aire en Códigos."));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
        lv_obj_t *r = ir_sheet_buttons(c);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ir_ui_btn(r, _("Nuevo aparato"), AOS_C_ACCENT, new_cb, NULL);
        ir_ui_btn(r, _("Buscar en Códigos"), AOS_C_CARD2, to_base_cb, NULL);
        return;
    }
    for (int i = 0; i < R.ndev; i++) {
        ir_dev_t *d = &R.devs[i];
        char sub[96];
        if (d->smartir) {
            int k = ir_pack_find((uint16_t)d->smartir);
            const ir_pack_dev_t *p = ir_pack_at(k);
            if (p) snprintf(sub, sizeof sub, "%s · %s", p->brand, p->models);
            else snprintf(sub, sizeof sub, _("SmartIR %d (falta la base)"), d->smartir);
        } else if (d->nbtn == 1) {
            snprintf(sub, sizeof sub, "%s · %s", ir_kind_label(d->kind), _("1 botón"));
        } else {
            snprintf(sub, sizeof sub, _("%s · %d botones"), ir_kind_label(d->kind), d->nbtn);
        }
        ir_ui_row(s, ir_kind_glyph(d->kind), kind_color(d->kind), d->name, sub, open_cb, (void *)(intptr_t)i);
    }
}

/* ---------------------------------------------------------------- the keys */

static void key_cb(lv_event_t *e)
{
    ir_dev_t *d = cur();
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!d || i < 0 || i >= d->nbtn) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (s_edit) {
        if (code == LV_EVENT_CLICKED) button_sheet(i);
        return;
    }
    const ir_button_t *b = &d->btn[i];
    if (!ir_proto_find(b->code.proto) && !b->nraw) {
        if (code == LV_EVENT_PRESSED) ir_ui_toast(_("Este botón todavía no tiene código: Editar"));
        return;
    }
    if (code == LV_EVENT_PRESSED) ir_hw_send_button(b, false);
    else if (code == LV_EVENT_LONG_PRESSED_REPEAT) ir_hw_send_button(b, true);
}

static lv_obj_t *key(lv_obj_t *parent, int idx, int32_t w, int32_t h, lv_color_t color)
{
    ir_dev_t *d = cur();
    const ir_button_t *b = &d->btn[idx];
    const ir_role_t *r = ir_role_find(b->role);
    lv_obj_t *k = lv_button_create(parent);
    lv_obj_remove_style_all(k);
    lv_obj_set_size(k, w, h);
    lv_obj_set_style_radius(k, w == h ? LV_RADIUS_CIRCLE : h / 2 > 40 ? 40 : h / 2, 0);
    lv_obj_set_style_bg_color(k, color, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k, AOS_C_TEXT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(k, LV_OPA_40, LV_STATE_PRESSED);
    bool empty = !ir_proto_find(b->code.proto) && !b->nraw;
    if (s_edit) {
        lv_obj_set_style_border_width(k, 3, 0);
        lv_obj_set_style_border_color(k, empty ? AOS_C_ORANGE : AOS_C_ACCENT, 0);
    }
    lv_obj_t *l = lv_label_create(k);
    /* a glyph when the role has one and the name is the role's own (in
     * Spanish, as the library and the portal write it, or translated) */
    bool own_name = !strcmp(b->name, r->label) || !strcmp(b->name, _(r->label));
    if (r->glyph && (own_name || r->role[0] == 'd')) {
        lv_label_set_text(l, r->glyph);
        bool sym = (unsigned char)r->glyph[0] >= 0xF0;
        lv_obj_set_style_text_font(l, sym ? (h >= 100 ? &aos_sym_44 : &aos_sym_28) : aos_font_title, 0);
    } else {
        lv_label_set_text(l, own_name ? _(r->label) : b->name);
        bool digit = r->role[0] == 'd' && !b->name[1];
        lv_obj_set_style_text_font(l, digit ? aos_font_title : w < 150 ? aos_font_caption : aos_font_small, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, w - 16);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_set_style_text_color(l, empty && !s_edit ? AOS_C_DIM : AOS_C_TEXT, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(k, key_cb, LV_EVENT_PRESSED, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(k, key_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(k, key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return k;
}

static int find_role(const char *role)
{
    ir_dev_t *d = cur();
    for (int i = 0; i < d->nbtn; i++) if (!strcmp(d->btn[i].role, role)) return i;
    return -1;
}

static bool any_role(const char *const *roles)
{
    for (; *roles; roles++) if (find_role(*roles) >= 0) return true;
    return false;
}

static lv_obj_t *section(lv_obj_t *s, int32_t cw, const char *title)
{
    lv_obj_t *c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 16, 0);
    lv_obj_set_style_pad_row(c, 16, 0);
    if (title) {
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
        lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
        lv_obj_set_width(l, LV_PCT(100));
    }
    return c;
}

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static void rocker(lv_obj_t *parent, const char *up, const char *down, const char *label)
{
    int iu = find_role(up), id = find_role(down);
    if (iu < 0 && id < 0) return;
    lv_obj_t *r = box(parent, 150, 330);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 75, 0);
    if (iu >= 0) lv_obj_align(key(r, iu, 150, 130, AOS_C_CARD2), LV_ALIGN_TOP_MID, 0, 0);
    if (id >= 0) lv_obj_align(key(r, id, 150, 130, AOS_C_CARD2), LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    lv_obj_center(l);
}

static void add_btn_cb(lv_event_t *e)
{
    (void)e;
    button_sheet(-1);
}

static void edit_cb(lv_event_t *e)
{
    (void)e;
    s_edit = !s_edit;
    show_remote();
}

static void back_cb(lv_event_t *e) { (void)e; s_edit = false; ir_remote_back(); }
static void dev_edit_cb(lv_event_t *e) { (void)e; dev_sheet(false); }

static void dev_delete(void *user)
{
    (void)user;
    ir_dev_t *d = cur();
    if (!d) return;
    ir_dev_delete(d);
    s_open_file[0] = 0;
    s_edit = false;
    rescan();
    show_list();
}

static void dev_delete_cb(lv_event_t *e)
{
    (void)e;
    ir_dev_t *d = cur();
    if (!d) return;
    char t[160];
    snprintf(t, sizeof t, _("Se borra %s y todos sus botones."), d->name);
    ir_ask(_("¿Borrar el aparato?"), t, _("Borrar"), AOS_C_RED, dev_delete, NULL);
}

/* Lying down, two columns, each group into the shorter one by an estimate
 * of its height; upright, the one column. */
static struct { lv_obj_t *col[2]; int32_t h[2]; lv_obj_t *one; } C;

static lv_obj_t *col_for(int32_t est)
{
    if (!C.col[0]) return C.one;
    int k = C.h[1] < C.h[0] ? 1 : 0;
    C.h[k] += est;
    return C.col[k];
}

static void remote_keys(lv_obj_t *s, int32_t cw)
{
    ir_dev_t *d = cur();
    bool land = ir_ui_landscape();
    memset(&C, 0, sizeof C);
    C.one = s;
    if (land) {
        for (int k = 0; k < 2; k++) {
            C.col[k] = box(s, cw, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(C.col[k], LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(C.col[k], 18, 0);
        }
    }
    static const char *const TOP[] = { "power", "power_on", "power_off", "input", "mute", "info", NULL };
    static const char *const NAV[] = { "up", "down", "left", "right", "ok", "back", "home", "menu", "guide", NULL };
    static const char *const ROCK[] = { "vol_up", "vol_down", "ch_up", "ch_down", NULL };
    static const char *const MEDIA[] = { "rew", "play", "play_pause", "pause", "stop", "ffwd", "prev", "next", "rec", NULL };
    static const char *const COLORS[] = { "red", "green", "yellow", "blue", NULL };
    static const char *const DIGITS[] = { "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d0", NULL };

    if (any_role(TOP)) {
        lv_obj_t *c = section(col_for(160), cw, NULL);
        for (int i = 0; TOP[i]; i++) {
            int k = find_role(TOP[i]);
            if (k < 0) continue;
            bool pw = !strncmp(TOP[i], "power", 5);
            bool glyph = !strcmp(TOP[i], "power") || !strcmp(TOP[i], "mute") || !strcmp(TOP[i], "info");
            key(c, k, glyph ? 112 : 170, 112, pw ? AOS_C_RED : AOS_C_CARD2);
        }
    }
    if (any_role(NAV)) {
        lv_obj_t *c = section(col_for(560), cw, NULL);
        int32_t dp = cw - 48 > 420 ? 420 : cw - 48;
        if (dp > 360 && land) dp = 360;
        lv_obj_t *pad = box(c, dp, dp);
        lv_obj_set_style_bg_color(pad, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pad, LV_RADIUS_CIRCLE, 0);
        int32_t kk = dp * 30 / 100;
        static const struct { const char *role; lv_align_t al; } DP[] = {
            { "up", LV_ALIGN_TOP_MID }, { "down", LV_ALIGN_BOTTOM_MID },
            { "left", LV_ALIGN_LEFT_MID }, { "right", LV_ALIGN_RIGHT_MID },
        };
        for (int i = 0; i < 4; i++) {
            int k = find_role(DP[i].role);
            if (k >= 0) lv_obj_align(key(pad, k, kk, kk, AOS_C_CARD2), DP[i].al, 0, 0);
        }
        int k = find_role("ok");
        if (k >= 0) lv_obj_center(key(pad, k, kk + 20, kk + 20, AOS_C_ACCENT));
        static const char *const UNDER[] = { "back", "home", "menu", "guide" };
        for (int i = 0; i < 4; i++) {
            int u = find_role(UNDER[i]);
            if (u >= 0) key(c, u, 150, 88, AOS_C_CARD2);
        }
    }
    if (any_role(ROCK)) {
        lv_obj_t *c = section(col_for(380), cw, NULL);
        rocker(c, "vol_up", "vol_down", "VOL");
        rocker(c, "ch_up", "ch_down", "CH");
    }
    if (any_role(MEDIA)) {
        lv_obj_t *c = section(col_for(260), cw, NULL);
        for (int i = 0; MEDIA[i]; i++) {
            int k = find_role(MEDIA[i]);
            if (k >= 0) key(c, k, 104, 104, AOS_C_CARD2);
        }
    }
    if (any_role(COLORS)) {
        lv_obj_t *c = section(col_for(112), cw, NULL);
        static const uint32_t COL[] = { 0xD03030, 0x30A040, 0xD0B020, 0x2060D0 };
        for (int i = 0; COLORS[i]; i++) {
            int k = find_role(COLORS[i]);
            if (k >= 0) key(c, k, (cw - 48 - 3 * 16) / 4, 64, lv_color_hex(COL[i]));
        }
    }
    if (any_role(DIGITS)) {
        lv_obj_t *c = section(col_for(500), cw, NULL);
        int32_t kw = (cw - 48 - 2 * 24) / 3;
        if (kw > 170) kw = 170;
        lv_obj_set_style_pad_column(c, 24, 0);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int i = 0; DIGITS[i]; i++) {
            int k = find_role(DIGITS[i]);
            if (k >= 0) key(c, k, kw, 92, AOS_C_CARD2);
            else box(c, kw, 92);
        }
    }
    /* the rest, by name */
    int others = 0;
    for (int i = 0; i < d->nbtn; i++) if (!d->btn[i].role[0]) others++;
    if (others || s_edit) {
        int cols = cw > 600 ? 3 : 2;
        lv_obj_t *c = section(col_for(((others + 1) / cols + 1) * 112 + 60), cw, others ? _("Otros") : NULL);
        int32_t kw = (cw - 48 - (cols - 1) * 16) / cols;
        for (int i = 0; i < d->nbtn; i++) if (!d->btn[i].role[0]) key(c, i, kw, 96, AOS_C_CARD2);
        if (s_edit) {
            lv_obj_t *b = ir_ui_btn(c, _("+ Botón"), AOS_C_ACCENT, add_btn_cb, NULL);
            lv_obj_set_width(b, kw);
        }
    }
}

/* ----------------------------------------------------- the air conditioner */

static void ac_build(void);

static int child_index(const ij_doc_t *j, int node, const char *key)
{
    IJ_EACH(j, node, c) if (j->n[c].key && !strcmp(j->n[c].key, key)) return c;
    return -1;
}

/* the keys of a node, for the chips */
static int keys_of(const ij_doc_t *j, int node, const char **out, int max, bool skip_onoff)
{
    int n = 0;
    if (node < 0 || j->n[node].type != IJ_OBJ) return 0;
    IJ_EACH(j, node, c) {
        if (!j->n[c].key) continue;
        if (skip_onoff && (!strcmp(j->n[c].key, "off") || !strcmp(j->n[c].key, "on"))) continue;
        if (n < max) out[n++] = j->n[c].key;
    }
    return n;
}

static bool numeric(const char *s)
{
    char *e;
    strtod(s, &e);
    return e != s && *e == 0;
}

/* walks the state down the tree, fixing any choice the file does not have,
 * and returns the node of the temperatures (-1 none) */
static int ac_fix(ir_dev_t *d, const ij_doc_t *j, ir_ac_shape_t *shape)
{
    int cmds = ij_get(j, 0, "commands");
    const char *modes[16];
    int nm = keys_of(j, cmds, modes, 16, true);
    if (!nm) return -1;
    if (child_index(j, cmds, d->ac_mode) < 0 || !strcmp(d->ac_mode, "off") || !strcmp(d->ac_mode, "on"))
        ir_copy(d->ac_mode, sizeof d->ac_mode, modes[0]);
    int node = child_index(j, cmds, d->ac_mode);
    ir_ac_shape(&R.blob, d->ac_mode, shape);
    for (int l = 0; l < shape->nlevels && node >= 0; l++) {
        if (j->n[node].type != IJ_OBJ) return -1;
        int c = child_index(j, node, d->ac_sel[l]);
        if (c < 0) {
            c = j->n[node].kid;
            ir_copy(d->ac_sel[l], sizeof d->ac_sel[0], c >= 0 ? j->n[c].key : "");
        }
        node = c;
    }
    for (int l = shape->nlevels; l < IR_AC_LEVELS; l++) d->ac_sel[l][0] = 0;
    if (node < 0 || j->n[node].type != IJ_OBJ) return -1;
    /* the temperature: the nearest the file has */
    int best = -1;
    float bd = 1e9f;
    IJ_EACH(j, node, c) {
        if (!j->n[c].key || !numeric(j->n[c].key)) continue;
        float t = strtof(j->n[c].key, NULL), dd = t > d->ac_temp ? t - d->ac_temp : d->ac_temp - t;
        if (dd < bd) { bd = dd; best = c; }
    }
    if (best < 0) return -1;
    d->ac_temp = strtof(j->n[best].key, NULL);
    return node;
}

static void ac_changed(void)
{
    R.ac_dirty = true;
    R.ac_ms = (uint32_t)aos_hal_uptime_ms();
    ac_build();
}

static void ac_power_cb(lv_event_t *e)
{
    (void)e;
    ir_dev_t *d = cur();
    d->ac_on = !d->ac_on;
    /* on and off go at once, without waiting */
    ir_ac_send(d);
    R.ac_dirty = false;
    ir_dev_save(d);
    R.sig = ir_store_signature();
    ac_build();
}

static void ac_temp_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    ir_dev_t *d = cur();
    const ij_doc_t *j = &R.blob.js;
    ir_ac_shape_t sh;
    int node = ac_fix(d, j, &sh);
    if (node < 0) return;
    /* the next temperature up or down that the file has */
    float best = d->ac_temp;
    bool found = false;
    IJ_EACH(j, node, c) {
        if (!j->n[c].key || !numeric(j->n[c].key)) continue;
        float t = strtof(j->n[c].key, NULL);
        if (dir > 0 && t > d->ac_temp + 0.01f && (!found || t < best)) { best = t; found = true; }
        if (dir < 0 && t < d->ac_temp - 0.01f && (!found || t > best)) { best = t; found = true; }
    }
    if (!found) return;
    d->ac_temp = best;
    if (!d->ac_on) d->ac_on = true;
    ac_changed();
}

static void ac_mode_cb(lv_event_t *e)
{
    const char *key = lv_event_get_user_data(e);
    ir_dev_t *d = cur();
    ir_copy(d->ac_mode, sizeof d->ac_mode, key);
    d->ac_on = true;
    ac_changed();
}

static void ac_level_cb(lv_event_t *e)
{
    const char *key = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_target(e);
    int level = (int)(intptr_t)lv_obj_get_user_data(t);
    ir_dev_t *d = cur();
    if (level < 0 || level >= IR_AC_LEVELS) return;
    ir_copy(d->ac_sel[level], sizeof d->ac_sel[0], key);
    d->ac_on = true;
    ac_changed();
}

static void ac_send_cb(lv_event_t *e)
{
    (void)e;
    ir_dev_t *d = cur();
    ir_ac_send(d);
    R.ac_dirty = false;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 76);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_bg_color(b, on ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static void ac_build(void)
{
    if (!R.ac_body) return;
    lv_obj_t *s = R.ac_body;
    int32_t sy = lv_obj_get_scroll_y(s);
    lv_obj_clean(s);
    ir_dev_t *d = cur();
    bool land = ir_ui_landscape();
    int32_t cw = land ? (ir_ui_width() - 2 * AOS_UI_PAD - 18) / 2 : ir_ui_width() - 2 * AOS_UI_PAD;
    const ij_doc_t *j = &R.blob.js;
    if (!R.blob.buf) {
        lv_obj_t *c = ir_ui_card(s);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, _("Falta la base de códigos (infrarrojo_p4.pak) o no trae este aire."));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, AOS_C_ORANGE, 0);
        return;
    }
    ir_ac_shape_t sh;
    int tnode = ac_fix(d, j, &sh);

    /* power and the temperature */
    lv_obj_t *c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *pw = round_btn(c, AOS_SYM_POWER, d->ac_on ? AOS_C_GREEN : AOS_C_CARD2, 112, ac_power_cb, NULL);
    (void)pw;
    lv_obj_t *mid = box(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 18, 0);
    if (tnode >= 0) {
        round_btn(mid, AOS_SYM_MINUS, AOS_C_CARD2, 88, ac_temp_cb, (void *)(intptr_t)-1);
        lv_obj_t *t = lv_label_create(mid);
        int tt = (int)(d->ac_temp * 10 + 0.5f);
        if (tt % 10) lv_label_set_text_fmt(t, "%d,%d°", tt / 10, tt % 10);
        else lv_label_set_text_fmt(t, "%d°", tt / 10);
        lv_obj_set_style_text_font(t, aos_font_huge, 0);
        lv_obj_set_style_text_color(t, d->ac_on ? AOS_C_TEXT : AOS_C_DIM, 0);
        round_btn(mid, AOS_SYM_PLUS, AOS_C_CARD2, 88, ac_temp_cb, (void *)(intptr_t)1);
    }

    /* the mode */
    int cmds = ij_get(j, 0, "commands");
    const char *modes[16];
    int nm = keys_of(j, cmds, modes, 16, true);
    c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(c, 12, 0);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, _("Modo"));
    lv_obj_set_style_text_font(l, aos_font_caption, 0);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    lv_obj_set_width(l, LV_PCT(100));
    for (int i = 0; i < nm; i++) chip(c, ir_ac_word(modes[i]), !strcmp(modes[i], d->ac_mode), ac_mode_cb, (void *)modes[i]);

    /* the levels under the mode */
    int node = child_index(j, cmds, d->ac_mode);
    for (int lv = 0; lv < sh.nlevels && node >= 0 && j->n[node].type == IJ_OBJ; lv++) {
        c = ir_ui_card(s);
        lv_obj_set_width(c, cw);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(c, 12, 0);
        l = lv_label_create(c);
        lv_label_set_text(l, sh.kind[lv] == 'f' ? _("Ventilador") : sh.kind[lv] == 's' ? _("Oscilación") :
                             sh.kind[lv] == 'p' ? _("Modo especial") : _("Opción"));
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
        lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
        lv_obj_set_width(l, LV_PCT(100));
        IJ_EACH(j, node, k) {
            if (!j->n[k].key) continue;
            lv_obj_t *b = chip(c, ir_ac_word(j->n[k].key), !strcmp(j->n[k].key, d->ac_sel[lv]), ac_level_cb,
                               (void *)j->n[k].key);
            lv_obj_set_user_data(b, (void *)(intptr_t)lv);
        }
        node = child_index(j, node, d->ac_sel[lv]);
    }

    /* about it */
    c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    const ir_pack_dev_t *p = ir_pack_at(R.blob_dev);
    l = lv_label_create(c);
    lv_label_set_text_fmt(l, "%s · %s", p ? p->brand : "?", p ? p->models : "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    l = lv_label_create(c);
    lv_label_set_text_fmt(l, _("Cada cambio se manda solo, con el estado entero, como el control del aire. Código %d de SmartIR."),
                          d->smartir);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, aos_font_caption, 0);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    ir_ui_btn(c, _("Mandar otra vez"), AOS_C_CARD2, ac_send_cb, NULL);

    /* learned extras, if any */
    if (d->nbtn || s_edit) {
        lv_obj_t *o = section(s, cw, _("Otros"));
        int32_t kw = (cw - 48 - 16) / 2;
        for (int i = 0; i < d->nbtn; i++) key(o, i, kw, 96, AOS_C_CARD2);
        if (s_edit) {
            lv_obj_t *b = ir_ui_btn(o, _("+ Botón"), AOS_C_ACCENT, add_btn_cb, NULL);
            lv_obj_set_width(b, kw);
        }
    }
    lv_obj_update_layout(s);
    lv_obj_scroll_to_y(s, sy, LV_ANIM_OFF);
}

bool ir_ac_send(ir_dev_t *d)
{
    if (!d->smartir) return false;
    int pi = ir_pack_find((uint16_t)d->smartir);
    if (pi < 0) return false;
    bool loaded_here = false;
    ir_pack_blob_t tmp, *b = &R.blob;
    if (R.blob_dev != pi || !R.blob.buf) {
        if (!ir_pack_load(pi, &tmp)) return false;
        b = &tmp;
        loaded_here = true;
    }
    const ij_doc_t *j = &b->js;
    int cmds = ij_get(j, 0, "commands");
    static uint32_t buf[IR_TX_MAX];
    bool ok = false;
    if (!d->ac_on) {
        int off = ij_get(j, cmds, "off");
        if (off >= 0) {
            int code = j->n[off].type == IJ_ARR ? (int)ij_num(j, j->n[off].kid, -1) : (int)ij_num(j, off, -1);
            int n = ir_pack_code(b, code, buf, IR_TX_MAX);
            ok = n > 0 && ir_hw_send(buf, n, 38000);
        }
        R.ac_sent_on = false;
    } else {
        /* a separate "on" first, for the files that have it, when it was off */
        int on = ij_get(j, cmds, "on");
        if (on >= 0 && !R.ac_sent_on) {
            int code = (int)ij_num(j, j->n[on].type == IJ_ARR ? j->n[on].kid : on, -1);
            int n = ir_pack_code(b, code, buf, IR_TX_MAX);
            if (n > 0) ir_hw_send(buf, n, 38000);
        }
        int code = ir_ac_code(b, d);
        int n = ir_pack_code(b, code, buf, IR_TX_MAX);
        ok = n > 0 && ir_hw_send(buf, n, 38000);
        R.ac_sent_on = true;
    }
    if (loaded_here) ir_pack_blob_free(&tmp);
    if (!ok) ir_ui_toast(_("Ese estado no está en la base de este aire"));
    return ok;
}

/* -------------------------------------------------------------- the remote */

static void show_remote(void)
{
    ir_dev_t *d = cur();
    if (!d) { show_list(); return; }
    lv_obj_clean(R.page);
    R.ac_body = NULL;
    lv_obj_t *h = ir_ui_header(R.page, d->name, _("Controles"), back_cb, NULL);
    lv_obj_t *ed = ir_ui_btn(h, s_edit ? _("Listo") : _("Editar"), s_edit ? AOS_C_ACCENT : AOS_C_CARD2, edit_cb, NULL);
    lv_obj_set_style_min_width(ed, 120, 0);
    lv_obj_set_height(ed, 68);
    lv_obj_align(ed, LV_ALIGN_RIGHT_MID, -AOS_UI_PAD, 0);
    /* the LED that lights with each transmission, inside the button so it
     * never sits on a long title */
    lv_obj_set_style_pad_left(ed, 48, 0);
    lv_obj_set_style_pad_right(ed, 24, 0);
    R.led = lv_obj_create(ed);
    lv_obj_remove_style_all(R.led);
    lv_obj_set_size(R.led, 16, 16);
    lv_obj_set_style_radius(R.led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(R.led, AOS_C_RED, 0);
    lv_obj_set_style_bg_opa(R.led, LV_OPA_20, 0);
    lv_obj_add_flag(R.led, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(R.led, LV_ALIGN_LEFT_MID, -28, 0);
    lv_obj_t *el = lv_obj_get_child(ed, 0);
    lv_obj_align(el, LV_ALIGN_RIGHT_MID, 0, 0);
    R.sent_seen = ir_hw_sent();

    lv_obj_t *s = ir_ui_scroll(R.page);
    bool land = ir_ui_landscape();
    if (land) {
        lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(s, 18, 0);
    }
    int32_t cw = land ? (ir_ui_width() - 2 * AOS_UI_PAD - 18) / 2 : ir_ui_width() - 2 * AOS_UI_PAD;

    memset(&C, 0, sizeof C);
    C.one = s;
    if (d->smartir) {
        int pi = ir_pack_find((uint16_t)d->smartir);
        if (pi != R.blob_dev) {
            ir_pack_blob_free(&R.blob);
            R.blob_dev = -1;
            if (pi >= 0 && ir_pack_load(pi, &R.blob)) R.blob_dev = pi;
        }
        R.ac_body = s;
        ac_build();
    } else {
        if (!d->nbtn && !s_edit) {
            lv_obj_t *c = ir_ui_card(s);
            lv_obj_set_width(c, cw);
            lv_obj_t *l = lv_label_create(c);
            lv_label_set_text(l, _("Sin botones. Tocá Editar y agregá uno, o aprendelo en Aprender."));
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_width(l, LV_PCT(100));
            lv_obj_set_style_text_font(l, aos_font_body, 0);
            lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
            lv_obj_t *r = ir_sheet_buttons(c);
            lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            ir_ui_btn(r, _("Editar"), AOS_C_ACCENT, edit_cb, NULL);
            ir_ui_btn(r, _("Aprender"), AOS_C_CARD2, to_learn_cb, NULL);
        }
        remote_keys(s, cw);
    }
    if (s_edit) {
        lv_obj_t *c = ir_ui_card(d->smartir ? s : col_for(200));
        lv_obj_set_width(c, cw);
        lv_obj_t *r = ir_sheet_buttons(c);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ir_ui_btn(r, _("Nombre y tipo"), AOS_C_CARD2, dev_edit_cb, NULL);
        ir_ui_btn(r, _("Borrar aparato"), AOS_C_RED, dev_delete_cb, NULL);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text_fmt(l, _("Archivo: ir/%s (se edita también desde el portal)"), d->file);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
        lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    }
}

/* ----------------------------------------------------- the button's sheet */

static void sheet_cancel(lv_event_t *e) { (void)e; ir_sheet_close(); }

static uint32_t parse_num(lv_obj_t *ta)
{
    const char *t = lv_textarea_get_text(ta);
    while (*t == ' ') t++;
    return (uint32_t)strtoul(t, NULL, 16);     /* "0x" or not */
}

/* name and role from the sheet into the button; the role's label when the
 * name was left empty */
static void sheet_take(ir_button_t *b)
{
    const ir_role_t *r = ir_role_at((int)lv_dropdown_get_selected(R.dd_role));
    ir_copy(b->role, sizeof b->role, r ? r->role : "");
    const char *n = lv_textarea_get_text(R.ta_name);
    if (!n[0]) n = r && r->role[0] ? _(r->label) : _("Botón");
    ir_copy(b->name, sizeof b->name, n);
    if (R.hand && !lv_obj_has_flag(R.hand, LV_OBJ_FLAG_HIDDEN)) {
        const ir_proto_t *p = ir_proto_at((int)lv_dropdown_get_selected(R.dd_proto));
        if (p) {
            ir_button_clear(b);
            memset(&b->code, 0, sizeof b->code);
            ir_copy(b->code.proto, sizeof b->code.proto, p->name);
            b->code.addr = parse_num(R.ta_addr) & p->addr_max;
            b->code.cmd = parse_num(R.ta_cmd) & p->cmd_max;
            if (!strcmp(p->name, "Kaseikyo")) b->code.extra = 0x2002;
        }
    }
}

static ir_button_t *sheet_button(void)
{
    ir_dev_t *d = cur();
    if (!d) return NULL;
    if (R.sb < 0) {
        ir_button_t *b = ir_dev_add_button(d);
        if (!b) return NULL;
        R.sb = d->nbtn - 1;
    }
    return R.sb < d->nbtn ? &d->btn[R.sb] : NULL;
}

static void sheet_save(lv_event_t *e)
{
    (void)e;
    ir_button_t *b = sheet_button();
    if (!b) return;
    sheet_take(b);
    ir_sheet_close();
    ir_remote_saved(cur()->file);
}

static void sheet_learn(lv_event_t *e)
{
    (void)e;
    ir_button_t *b = sheet_button();
    if (!b) return;
    sheet_take(b);
    int idx = R.sb;
    char file[48];
    ir_copy(file, sizeof file, cur()->file);
    ir_sheet_close();
    ir_remote_saved(file);
    ir_ui_learn_for(file, idx);
}

static void sheet_test(lv_event_t *e)
{
    (void)e;
    ir_dev_t *d = cur();
    ir_button_t tmp;
    memset(&tmp, 0, sizeof tmp);
    if (R.sb >= 0 && R.sb < d->nbtn) {
        tmp = d->btn[R.sb];
        tmp.raw = NULL;
        tmp.nraw = 0;
        if (d->btn[R.sb].nraw) ir_button_set_raw(&tmp, d->btn[R.sb].raw, d->btn[R.sb].nraw, d->btn[R.sb].freq);
        else tmp.code = d->btn[R.sb].code;
    }
    if (R.hand && !lv_obj_has_flag(R.hand, LV_OBJ_FLAG_HIDDEN)) {
        const ir_proto_t *p = ir_proto_at((int)lv_dropdown_get_selected(R.dd_proto));
        ir_button_clear(&tmp);
        memset(&tmp.code, 0, sizeof tmp.code);
        if (p) {
            ir_copy(tmp.code.proto, sizeof tmp.code.proto, p->name);
            tmp.code.addr = parse_num(R.ta_addr) & p->addr_max;
            tmp.code.cmd = parse_num(R.ta_cmd) & p->cmd_max;
            if (!strcmp(p->name, "Kaseikyo")) tmp.code.extra = 0x2002;
        }
    }
    if (!ir_hw_send_button(&tmp, false)) ir_ui_toast(_("No hay código para mandar"));
    ir_button_clear(&tmp);
}

static void sheet_hand(lv_event_t *e)
{
    (void)e;
    if (lv_obj_has_flag(R.hand, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(R.hand, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(R.hand, LV_OBJ_FLAG_HIDDEN);
}

static void btn_delete(void *user)
{
    (void)user;
    ir_dev_t *d = cur();
    if (!d) return;
    ir_dev_remove_button(d, R.sb);
    ir_remote_saved(d->file);
}

static void sheet_delete(lv_event_t *e)
{
    (void)e;
    ir_ask(_("¿Borrar el botón?"), _("Se borra con su código."), _("Borrar"), AOS_C_RED, btn_delete, NULL);
}

static void role_changed(lv_event_t *e)
{
    (void)e;
    /* an empty name follows the role */
    const ir_role_t *r = ir_role_at((int)lv_dropdown_get_selected(R.dd_role));
    const char *n = lv_textarea_get_text(R.ta_name);
    bool is_label = !n[0];
    for (int i = 0; i < ir_role_count() && !is_label; i++) if (!strcmp(n, _(ir_role_at(i)->label))) is_label = true;
    if (is_label && r && r->role[0]) lv_textarea_set_text(R.ta_name, _(r->label));
}

static lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int sel)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, (uint32_t)(sel < 0 ? 0 : sel));
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

static void button_sheet(int idx)
{
    ir_dev_t *d = cur();
    if (!d) return;
    R.sb = idx;
    ir_button_t *b = idx >= 0 && idx < d->nbtn ? &d->btn[idx] : NULL;
    lv_obj_t *s = ir_sheet_open(b ? _("Botón") : _("Botón nuevo"));
    R.ta_name = ir_sheet_text(s, _("Nombre"), b ? b->name : "", 39);
    lv_obj_t *l = lv_label_create(s);
    lv_label_set_text(l, _("Función en el control"));
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    ir_sb_t opts = { 0 };
    int sel = 0;
    for (int i = 0; i < ir_role_count(); i++) {
        const ir_role_t *r = ir_role_at(i);
        if (i) ir_sb_put(&opts, "\n", 1);
        ir_sb_put(&opts, _(r->label), -1);
        if (b && !strcmp(b->role, r->role)) sel = i;
    }
    R.dd_role = dropdown(s, opts.s, sel);
    ir_sb_free(&opts);
    lv_obj_add_event_cb(R.dd_role, role_changed, LV_EVENT_VALUE_CHANGED, NULL);

    R.code_lbl = lv_label_create(s);
    char t[96];
    if (b && ir_proto_find(b->code.proto)) ir_code_text(&b->code, t, sizeof t);
    else if (b && b->nraw) snprintf(t, sizeof t, _("crudo · %d duraciones · %u kHz"), b->nraw, (unsigned)((b->freq ? b->freq : 38000) / 1000));
    else snprintf(t, sizeof t, "%s", _("sin código todavía"));
    lv_label_set_text(R.code_lbl, t);
    lv_obj_set_style_text_font(R.code_lbl, aos_font_small, 0);
    lv_obj_set_style_text_color(R.code_lbl, AOS_C_ACCENT, 0);

    /* a code by hand */
    R.hand = lv_obj_create(s);
    lv_obj_remove_style_all(R.hand);
    lv_obj_set_size(R.hand, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(R.hand, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(R.hand, 12, 0);
    opts = (ir_sb_t){ 0 };
    int psel = 0;
    for (int i = 0; i < ir_proto_count(); i++) {
        if (i) ir_sb_put(&opts, "\n", 1);
        ir_sb_put(&opts, ir_proto_at(i)->label, -1);
        if (b && !strcmp(b->code.proto, ir_proto_at(i)->name)) psel = i;
    }
    R.dd_proto = dropdown(R.hand, opts.s, psel);
    ir_sb_free(&opts);
    char hx[16];
    snprintf(hx, sizeof hx, "%X", b ? (unsigned)b->code.addr : 0);
    R.ta_addr = ir_sheet_text(R.hand, _("Dirección (hexa)"), hx, 8);
    snprintf(hx, sizeof hx, "%X", b ? (unsigned)b->code.cmd : 0);
    R.ta_cmd = ir_sheet_text(R.hand, _("Comando (hexa)"), hx, 8);
    lv_obj_add_flag(R.hand, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *r = ir_sheet_buttons(s);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ir_ui_btn(r, _("Aprender"), AOS_C_ACCENT, sheet_learn, NULL);
    ir_ui_btn(r, _("Probar"), AOS_C_CARD2, sheet_test, NULL);
    ir_ui_btn(r, _("Código a mano"), AOS_C_CARD2, sheet_hand, NULL);
    if (b) ir_ui_btn(r, _("Borrar"), AOS_C_RED, sheet_delete, NULL);
    r = ir_sheet_buttons(s);
    ir_ui_btn(r, _("Cancelar"), AOS_C_CARD2, sheet_cancel, NULL);
    ir_ui_btn(r, _("Guardar"), AOS_C_GREEN, sheet_save, NULL);
}

/* ----------------------------------------------------- the device's sheet */

static void kind_cb(lv_event_t *e)
{
    R.kind = (int)(intptr_t)lv_event_get_user_data(e);
    for (int k = 0; k < IR_K_COUNT; k++)
        lv_obj_set_style_bg_color(R.kind_btn[k], k == R.kind ? AOS_C_ACCENT : AOS_C_CARD2, 0);
}

static void dev_save_cb(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(R.ta_dev);
    if (!name[0]) { ir_ui_toast(_("Falta el nombre")); return; }
    if (R.dev_new) {
        ir_dev_t d;
        memset(&d, 0, sizeof d);
        ir_copy(d.name, sizeof d.name, name);
        d.kind = R.kind;
        if (!ir_dev_save(&d)) { ir_ui_toast(_("No se pudo escribir en la tarjeta")); return; }
        ir_sheet_close();
        s_edit = true;
        ir_copy(s_open_file, sizeof s_open_file, d.file);
        rescan();
        redraw();
        return;
    }
    ir_dev_t *d = cur();
    if (!d) return;
    ir_copy(d->name, sizeof d->name, name);
    if (!d->smartir) d->kind = R.kind;
    ir_sheet_close();
    ir_remote_saved(d->file);
}

static void dev_sheet(bool is_new)
{
    ir_dev_t *d = is_new ? NULL : cur();
    R.dev_new = is_new;
    R.kind = d ? d->kind : IR_K_TV;
    lv_obj_t *s = ir_sheet_open(is_new ? _("Aparato nuevo") : _("Nombre y tipo"));
    R.ta_dev = ir_sheet_text(s, _("Nombre"), d ? d->name : "", 40);
    if (!d || !d->smartir) {
        lv_obj_t *row = ir_sheet_buttons(s);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int k = 0; k < IR_K_COUNT; k++) {
            if (k == IR_K_AC) { R.kind_btn[k] = lv_obj_create(row); lv_obj_add_flag(R.kind_btn[k], LV_OBJ_FLAG_HIDDEN); continue; }
            R.kind_btn[k] = chip(row, ir_kind_label(k), k == R.kind, kind_cb, (void *)(intptr_t)k);
        }
        lv_obj_t *n = lv_label_create(s);
        lv_label_set_text(n, _("Un aire acondicionado se agrega desde Códigos: manda el estado entero y necesita su modelo."));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(n, LV_PCT(100));
        lv_obj_set_style_text_font(n, aos_font_caption, 0);
        lv_obj_set_style_text_color(n, AOS_C_DIM, 0);
    }
    lv_obj_t *r = ir_sheet_buttons(s);
    ir_ui_btn(r, _("Cancelar"), AOS_C_CARD2, sheet_cancel, NULL);
    ir_ui_btn(r, is_new ? _("Crear") : _("Guardar"), AOS_C_GREEN, dev_save_cb, NULL);
}

/* ------------------------------------------------------------------- life */

void ir_remote_build(lv_obj_t *page)
{
    R.page = page;
    rescan();
    redraw();
}

void ir_remote_tick(void)
{
    if (!R.page) return;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    /* the LED in the header lights with each transmission */
    uint32_t sent = ir_hw_sent();
    if (R.led) {
        if (sent != R.sent_seen) {
            R.sent_seen = sent;
            R.led_ms = now;
            lv_obj_set_style_bg_opa(R.led, LV_OPA_COVER, 0);
        } else if (now - R.led_ms > 250) {
            lv_obj_set_style_bg_opa(R.led, LV_OPA_20, 0);
        }
    }
    ir_dev_t *d = cur();
    if (d && R.ac_dirty && now - R.ac_ms > 600) {
        R.ac_dirty = false;
        ir_ac_send(d);
        ir_dev_save(d);
        R.sig = ir_store_signature();
    }
    if (now - R.sig_ms > 2000) {
        R.sig_ms = now;
        if (!R.ac_dirty) ir_remote_reload();
    }
}

void ir_remote_free(void)
{
    ir_store_free_all(R.devs, R.ndev);
    ir_pack_blob_free(&R.blob);
    memset(&R, 0, sizeof R);
    R.open = -1;
    R.blob_dev = -1;
}
