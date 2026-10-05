/*
 * P4OS - Infrarrojo: SmartIR's library on screen.
 *
 * Kind, brand, model: four kinds (televisions and audio, air conditioners,
 * fans, lights), the brands of the chosen kind, the models of a brand, and
 * a model with its commands. A command of a television is tried with a tap
 * before the device is added; an air conditioner is tried on and off. Added,
 * a television becomes a device of /sdcard/ir with its buttons (decoded
 * into NEC and the rest when they are, raw when not); an air conditioner
 * becomes a device that points at the library, because its file has a code
 * for every state and those stay in the pack.
 *
 * The search ("which remote is this button from") runs in the app's thread over
 * every code of the pack and shows the best device for each brand and
 * model, with the command it matched and how well.
 *
 * Climate files nest their codes as mode, then [preset], fan, [swing], then
 * temperature; ir_ac_shape() finds which level is which from the lists the
 * file itself carries (presetModes, fanModes, swingModes).
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

enum { SCR_BRANDS = 0, SCR_MODELS, SCR_DEVICE, SCR_FIND };

static struct {
    lv_obj_t      *page;
    ir_pack_blob_t blob;
    int            blob_dev;
    lv_obj_t      *bar, *bar_lbl;
    bool           finding;
    ir_match_t     found[IR_FIND_MAX];
    int            nfound;
    bool           have_find;
} B = { .blob_dev = -1 };

/* across a rebuild */
static int  s_scr, s_cls, s_dev = -1;
static char s_brand[64];
static bool s_from_find;

static void show(void);

static const char *CLASS_NAME[IR_C_COUNT] = { N_("Teles y audio"), N_("Aires"), N_("Ventiladores"), N_("Luces") };

/* ------------------------------------------------------------------ words */

static const struct { const char *key, *es; } WORDS[] = {
    { "cool", N_("Frío") }, { "heat", N_("Calor") }, { "dry", N_("Seco") },
    { "fan_only", N_("Ventilación") }, { "fan", N_("Ventilación") }, { "auto", N_("Auto") },
    { "heat_cool", N_("Frío/calor") }, { "low", N_("Bajo") }, { "lowest", N_("Mínimo") },
    { "mid", N_("Medio") }, { "medium", N_("Medio") }, { "middle", N_("Medio") },
    { "mid-low", N_("Medio bajo") }, { "mid-high", N_("Medio alto") },
    { "high", N_("Alto") }, { "highest", N_("Máximo") }, { "maximum", N_("Máximo") }, { "max", N_("Máximo") }, { "quiet", N_("Silencioso") },
    { "silent", N_("Silencioso") }, { "turbo", N_("Turbo") }, { "powerful", N_("Potente") },
    { "boost", N_("Turbo") }, { "eco", N_("Eco") }, { "sleep", N_("Noche") },
    { "none", N_("Ninguno") }, { "normal", N_("Normal") }, { "on", N_("Sí") }, { "off", N_("No") },
    { "swing", N_("Oscila") }, { "fixed", N_("Fijo") }, { "stop", N_("Quieto") },
    { "top", N_("Arriba") }, { "bottom", N_("Abajo") }, { "up", N_("Arriba") }, { "down", N_("Abajo") },
    { "horizontal", N_("Horizontal") }, { "vertical", N_("Vertical") }, { "both", N_("Los dos") },
    { "default", N_("Normal") }, { "reverse", N_("Al revés") },
};

const char *ir_ac_word(const char *key)
{
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++)
        if (!strcasecmp(WORDS[i].key, key)) return _(WORDS[i].es);
    return key;
}

/* SmartIR's command names of the other kinds, and the role each gets */
static const struct { const char *key, *es, *role; } CMDS[] = {
    { "off", N_("Apagar"), "power_off" }, { "on", N_("Encender"), "power_on" },
    { "power", N_("Encender/apagar"), "power" },
    { "volumeUp", N_("Volumen +"), "vol_up" }, { "volumeDown", N_("Volumen −"), "vol_down" },
    { "mute", N_("Silencio"), "mute" }, { "nextChannel", N_("Canal +"), "ch_up" },
    { "previousChannel", N_("Canal −"), "ch_down" }, { "sources", N_("Fuente"), "" },
    { "play", N_("Reproducir"), "play" }, { "pause", N_("Pausa"), "pause" }, { "stop", N_("Detener"), "stop" },
    { "up", N_("Arriba"), "up" }, { "down", N_("Abajo"), "down" }, { "left", N_("Izquierda"), "left" },
    { "right", N_("Derecha"), "right" }, { "ok", N_("OK"), "ok" }, { "enter", N_("OK"), "ok" },
    { "back", N_("Volver"), "back" }, { "home", N_("Inicio"), "home" }, { "menu", N_("Menú"), "menu" },
    { "info", N_("Info"), "info" }, { "brighten", N_("Más luz"), "" }, { "dim", N_("Menos luz"), "" },
    { "colder", N_("Más fría"), "" }, { "warmer", N_("Más cálida"), "" }, { "night", N_("Luz de noche"), "" },
    { "oscillate", N_("Oscilar"), "" }, { "default", N_("Normal"), "" },
};

static const char *cmd_word(const char *key, const char **role)
{
    for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++)
        if (!strcasecmp(CMDS[i].key, key)) {
            if (role) *role = CMDS[i].role;
            return _(CMDS[i].es);
        }
    if (role) *role = "";
    return ir_ac_word(key);
}

/* --------------------------------------------------------------- climate */

static bool numeric(const char *s)
{
    char *e;
    strtod(s, &e);
    return e != s && *e == 0;
}

static int child(const ij_doc_t *j, int node, const char *key)
{
    if (node < 0 || j->n[node].type != IJ_OBJ) return -1;
    IJ_EACH(j, node, c) if (j->n[c].key && !strcmp(j->n[c].key, key)) return c;
    return -1;
}

static bool in_list(const ij_doc_t *j, int list, const char *key)
{
    IJ_EACH(j, list, c) if (j->n[c].type == IJ_STR && !strcmp(j->n[c].str, key)) return true;
    return false;
}

void ir_ac_shape(const ir_pack_blob_t *b, const char *mode, ir_ac_shape_t *s)
{
    memset(s, 0, sizeof *s);
    const ij_doc_t *j = &b->js;
    int node = child(j, ij_get(j, 0, "commands"), mode);
    int lists[3] = { ij_get(j, 0, "presetModes"), ij_get(j, 0, "fanModes"), ij_get(j, 0, "swingModes") };
    const char kinds[3] = { 'p', 'f', 's' };
    bool used[3] = { lists[0] < 0, lists[1] < 0, lists[2] < 0 };
    while (node >= 0 && j->n[node].type == IJ_OBJ && s->nlevels < IR_AC_LEVELS) {
        int first = j->n[node].kid;
        if (first < 0 || !j->n[first].key || numeric(j->n[first].key)) break;
        char k = '?';
        for (int i = 0; i < 3 && k == '?'; i++)
            if (!used[i] && in_list(j, lists[i], j->n[first].key)) { k = kinds[i]; used[i] = true; }
        for (int i = 0; i < 3 && k == '?'; i++)
            if (!used[i]) { k = kinds[i]; used[i] = true; }
        s->kind[s->nlevels++] = k;
        node = first;
    }
}

static int leaf_code(const ij_doc_t *j, int node)
{
    if (node < 0) return -1;
    if (j->n[node].type == IJ_ARR) node = j->n[node].kid;
    return (node >= 0 && j->n[node].type == IJ_NUM) ? (int)j->n[node].num : -1;
}

int ir_ac_code(const ir_pack_blob_t *b, const ir_dev_t *d)
{
    const ij_doc_t *j = &b->js;
    int node = child(j, ij_get(j, 0, "commands"), d->ac_mode);
    ir_ac_shape_t s;
    ir_ac_shape(b, d->ac_mode, &s);
    for (int l = 0; l < s.nlevels && node >= 0; l++) {
        int c = child(j, node, d->ac_sel[l]);
        node = c >= 0 ? c : j->n[node].kid;
    }
    if (node >= 0 && j->n[node].type == IJ_OBJ) {
        int best = -1;
        float bd = 1e9f;
        IJ_EACH(j, node, c) {
            if (!j->n[c].key || !numeric(j->n[c].key)) continue;
            float t = strtof(j->n[c].key, NULL), dd = t > d->ac_temp ? t - d->ac_temp : d->ac_temp - t;
            if (dd < bd) { bd = dd; best = c; }
        }
        node = best;
    }
    return leaf_code(j, node);
}

/* ------------------------------------------------------------- the device */

static bool load_dev(int i)
{
    if (B.blob_dev == i && B.blob.buf) return true;
    ir_pack_blob_free(&B.blob);
    B.blob_dev = -1;
    if (!ir_pack_load(i, &B.blob)) return false;
    B.blob_dev = i;
    return true;
}

static void send_code(int code)
{
    static uint32_t buf[IR_TX_MAX];
    int n = ir_pack_code(&B.blob, code, buf, IR_TX_MAX);
    if (n <= 0 || !ir_hw_send(buf, n, 38000)) ir_ui_toast(_("No se pudo mandar"));
}

static void cmd_cb(lv_event_t *e)
{
    send_code((int)(intptr_t)lv_event_get_user_data(e));
}

static void ac_try(bool on)
{
    ir_dev_t d;
    memset(&d, 0, sizeof d);
    const ij_doc_t *j = &B.blob.js;
    int cmds = ij_get(j, 0, "commands");
    if (!on) {
        send_code(leaf_code(j, child(j, cmds, "off")));
        return;
    }
    /* cool at 24, or the first mode the file has */
    const char *mode = child(j, cmds, "cool") >= 0 ? "cool" : NULL;
    IJ_EACH(j, cmds, c) if (!mode && j->n[c].key && strcmp(j->n[c].key, "off") && strcmp(j->n[c].key, "on")) mode = j->n[c].key;
    if (!mode) return;
    ir_copy(d.ac_mode, sizeof d.ac_mode, mode);
    d.ac_temp = 24;
    int code = ir_ac_code(&B.blob, &d);
    int onc = leaf_code(j, child(j, cmds, "on"));
    if (onc >= 0) send_code(onc);
    send_code(code);
}

static void ac_on_cb(lv_event_t *e) { (void)e; ac_try(true); }
static void ac_off_cb(lv_event_t *e) { (void)e; ac_try(false); }

typedef struct {
    ir_dev_t *d;
    int on_code, off_code;
    ir_button_t *on_btn, *off_btn;
} add_ctx_t;

static void add_leaf(add_ctx_t *a, const char *name, const char *role, int code)
{
    static uint32_t buf[IR_TX_MAX];
    int n = ir_pack_code(&B.blob, code, buf, IR_TX_MAX);
    if (n <= 0) return;
    ir_button_t *b = ir_dev_add_button(a->d);
    if (!b) return;
    ir_copy(b->name, sizeof b->name, name);
    ir_copy(b->role, sizeof b->role, role);
    ir_code_t c;
    if (ir_decode(buf, n, &c) && !c.repeat) b->code = c;
    else ir_button_set_raw(b, buf, n, 38000);
    if (!strcmp(role, "power_on")) a->on_code = code;
    if (!strcmp(role, "power_off")) a->off_code = code;
}

static void add_walk(add_ctx_t *a, const ij_doc_t *j, int node, char *path, size_t n, const char *role)
{
    int code = leaf_code(j, node);
    if (code >= 0) { add_leaf(a, path[0] ? path : "?", role, code); return; }
    if (j->n[node].type != IJ_OBJ) return;
    size_t at = strlen(path);
    IJ_EACH(j, node, c) {
        if (!j->n[c].key) continue;
        const char *r;
        const char *key = j->n[c].key;
        const char *w = cmd_word(key, &r);
        /* SmartIR keeps a television's digits as sources "0".."9" */
        int leaf = leaf_code(j, c);
        if (at && leaf >= 0 && key[0] >= '0' && key[0] <= '9' && !key[1]) {
            char name[2] = { key[0], 0 }, role[4] = { 'd', key[0], 0 };
            add_leaf(a, name, role, leaf);
            continue;
        }
        snprintf(path + at, n - at, "%s%s", at ? " · " : "", w);
        add_walk(a, j, c, path, n, at ? "" : r);
        path[at] = 0;
    }
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    const ir_pack_dev_t *p = ir_pack_at(s_dev);
    if (!p || !load_dev(s_dev)) return;
    ir_dev_t d;
    memset(&d, 0, sizeof d);
    char name[48];
    char model[32];
    ir_copy(model, sizeof model, p->models);
    char *comma = strchr(model, ',');
    if (comma) *comma = 0;
    snprintf(name, sizeof name, "%s %s", p->brand, model);
    ir_copy(d.name, sizeof d.name, name);
    if (p->cls == IR_C_CLIMATE) {
        d.kind = IR_K_AC;
        d.smartir = p->id;
        d.ac_temp = 24;
    } else {
        d.kind = p->cls == IR_C_FAN ? IR_K_FAN : p->cls == IR_C_LIGHT ? IR_K_LIGHT : IR_K_TV;
        add_ctx_t a = { &d, -1, -1, NULL, NULL };
        char path[96] = "";
        const ij_doc_t *j = &B.blob.js;
        add_walk(&a, j, ij_get(j, 0, "commands"), path, sizeof path, "");
        /* one code for on and off is a toggle: one power key */
        if (a.on_code >= 0 && a.on_code == a.off_code) {
            for (int i = 0; i < d.nbtn; i++)
                if (!strcmp(d.btn[i].role, "power_off")) { ir_dev_remove_button(&d, i); break; }
            for (int i = 0; i < d.nbtn; i++)
                if (!strcmp(d.btn[i].role, "power_on")) {
                    ir_copy(d.btn[i].role, sizeof d.btn[i].role, "power");
                    ir_copy(d.btn[i].name, sizeof d.btn[i].name, _("Encender/apagar"));
                }
        }
    }
    bool ok = ir_dev_save(&d);
    char file[48];
    ir_copy(file, sizeof file, d.file);
    ir_dev_clear(&d);
    if (!ok) { ir_ui_toast(_("No se pudo escribir en la tarjeta")); return; }
    ir_ui_toast(_("Agregado a Controles"));
    ir_remote_open_file(file);
    ir_ui_goto_tab(0);
}

/* --------------------------------------------------------------- screens */

static void go(int scr)
{
    s_scr = scr;
    show();
}

bool ir_base_back(void)
{
    if (s_scr == SCR_DEVICE && s_from_find) { s_from_find = false; go(SCR_FIND); return true; }
    if (s_scr == SCR_FIND) { go(SCR_BRANDS); ir_ui_goto_tab(1); return true; }
    if (s_scr > SCR_BRANDS) { go(s_scr - 1); return true; }
    return false;
}

static void back_cb(lv_event_t *e) { (void)e; ir_base_back(); }

static void cls_cb(lv_event_t *e)
{
    s_cls = (int)(intptr_t)lv_event_get_user_data(e);
    go(SCR_BRANDS);
}

static void brand_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const ir_pack_dev_t *p = ir_pack_at(i);
    if (!p) return;
    ir_copy(s_brand, sizeof s_brand, p->brand);
    go(SCR_MODELS);
}

static void model_cb(lv_event_t *e)
{
    s_dev = (int)(intptr_t)lv_event_get_user_data(e);
    s_from_find = false;
    go(SCR_DEVICE);
}

static void found_cb(lv_event_t *e)
{
    s_dev = (int)(intptr_t)lv_event_get_user_data(e);
    s_from_find = true;
    go(SCR_DEVICE);
}

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

static lv_obj_t *chip(lv_obj_t *parent, const char *t, bool on, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_bg_color(b, on ? AOS_C_ACCENT : AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static void credit(lv_obj_t *s)
{
    text(s, _("Códigos de SmartIR, licencia MIT: © 2019 Vassilis Panos, 2024 Li Tin O've Weedle."),
         aos_font_caption, AOS_C_DIM);
}

static void scr_brands(lv_obj_t *s)
{
    lv_obj_t *row = lv_obj_create(s);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_row(row, 12, 0);
    int count[IR_C_COUNT] = { 0 };
    for (int i = 0; i < ir_pack_count(); i++) count[ir_pack_at(i)->cls % IR_C_COUNT]++;
    for (int c = 0; c < IR_C_COUNT; c++) {
        char t[48];
        snprintf(t, sizeof t, "%s %d", _(CLASS_NAME[c]), count[c]);
        chip(row, t, c == s_cls, cls_cb, (void *)(intptr_t)c);
    }
    /* one row per brand of the kind: the pack is sorted by kind and brand */
    const char *last = NULL;
    int first = -1, models = 0;
    for (int i = 0; i <= ir_pack_count(); i++) {
        const ir_pack_dev_t *p = ir_pack_at(i);
        bool same = p && p->cls == s_cls && last && !strcasecmp(p->brand, last);
        if (!same && last) {
            char sub[32];
            snprintf(sub, sizeof sub, models == 1 ? _("%d modelo") : _("%d modelos"), models);
            ir_ui_row(s, NULL, AOS_C_CARD, last, sub, brand_cb, (void *)(intptr_t)first);
            last = NULL;
        }
        if (!p || p->cls != s_cls) continue;
        if (!same) { last = p->brand; first = i; models = 0; }
        models++;
    }
    credit(s);
}

static void scr_models(lv_obj_t *s)
{
    for (int i = 0; i < ir_pack_count(); i++) {
        const ir_pack_dev_t *p = ir_pack_at(i);
        if (p->cls != s_cls || strcasecmp(p->brand, s_brand)) continue;
        char sub[32];
        snprintf(sub, sizeof sub, _("código %d de SmartIR"), p->id);
        ir_ui_row(s, NULL, AOS_C_CARD, p->models[0] ? p->models : _("modelo sin nombre"), sub, model_cb,
                  (void *)(intptr_t)i);
    }
}

static void cmd_rows(lv_obj_t *s, const ij_doc_t *j, int node, char *path, size_t n)
{
    int code = leaf_code(j, node);
    if (code >= 0) {
        ir_ui_row(s, AOS_SYM_SEND, AOS_C_ACCENT, path, NULL, cmd_cb, (void *)(intptr_t)code);
        return;
    }
    if (j->n[node].type != IJ_OBJ) return;
    size_t at = strlen(path);
    IJ_EACH(j, node, c) {
        if (!j->n[c].key) continue;
        snprintf(path + at, n - at, "%s%s", at ? " · " : "", cmd_word(j->n[c].key, NULL));
        cmd_rows(s, j, c, path, n);
        path[at] = 0;
    }
}

static void scr_device(lv_obj_t *s)
{
    const ir_pack_dev_t *p = ir_pack_at(s_dev);
    if (!p || !load_dev(s_dev)) {
        text(s, _("No se pudo leer este modelo de la base."), aos_font_body, AOS_C_ORANGE);
        return;
    }
    const ij_doc_t *j = &B.blob.js;
    lv_obj_t *c = ir_ui_card(s);
    text(c, p->models[0] ? p->models : _("modelo sin nombre"), aos_font_body, AOS_C_TEXT);
    char t[160];
    snprintf(t, sizeof t, _("%s · %s · código %d de SmartIR · %d códigos"), p->brand, _(CLASS_NAME[p->cls % IR_C_COUNT]),
             p->id, B.blob.ncodes);
    text(c, t, aos_font_small, AOS_C_DIM);
    int cmds = ij_get(j, 0, "commands");
    if (p->cls == IR_C_CLIMATE) {
        ir_sb_t sb = { 0 };
        IJ_EACH(j, cmds, k) {
            if (!j->n[k].key || !strcmp(j->n[k].key, "off") || !strcmp(j->n[k].key, "on")) continue;
            if (sb.n) ir_sb_put(&sb, ", ", 2);
            ir_sb_put(&sb, ir_ac_word(j->n[k].key), -1);
        }
        int lo = (int)ij_num(j, ij_get(j, 0, "minTemperature"), 0), hi = (int)ij_num(j, ij_get(j, 0, "maxTemperature"), 0);
        snprintf(t, sizeof t, _("Modos: %s · de %d a %d °C"), sb.s ? sb.s : "-", lo, hi);
        ir_sb_free(&sb);
        text(c, t, aos_font_small, AOS_C_DIM);
        lv_obj_t *r = ir_sheet_buttons(c);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ir_ui_btn(r, _("Probar: prender"), AOS_C_CARD2, ac_on_cb, NULL);
        ir_ui_btn(r, _("Probar: apagar"), AOS_C_CARD2, ac_off_cb, NULL);
        ir_ui_btn(r, _("Agregar a Controles"), AOS_C_GREEN, add_cb, NULL);
        text(c, _("Prender manda frío a 24 °C (o el primer modo que tenga). Si el aire contesta, es este modelo."),
             aos_font_caption, AOS_C_DIM);
    } else {
        lv_obj_t *r = ir_sheet_buttons(c);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ir_ui_btn(r, _("Agregar a Controles"), AOS_C_GREEN, add_cb, NULL);
        text(c, _("Tocá un comando para probarlo antes de agregarlo."), aos_font_caption, AOS_C_DIM);
        char path[96] = "";
        cmd_rows(s, j, cmds, path, sizeof path);
    }
}

static void scr_find(lv_obj_t *s)
{
    lv_obj_t *c = ir_ui_card(s);
    B.bar_lbl = text(c, B.finding ? _("Buscando en la base…") : _("Listo."), aos_font_body, AOS_C_TEXT);
    B.bar = lv_bar_create(c);
    lv_obj_set_size(B.bar, LV_PCT(100), 16);
    lv_bar_set_range(B.bar, 0, 100);
    lv_obj_set_style_bg_color(B.bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(B.bar, AOS_C_ACCENT, LV_PART_INDICATOR);
    int p = ir_hw_find_progress();
    lv_bar_set_value(B.bar, p < 0 ? 0 : p, LV_ANIM_OFF);
    if (B.finding) return;
    lv_obj_add_flag(B.bar, LV_OBJ_FLAG_HIDDEN);
    if (!B.nfound) {
        lv_label_set_text(B.bar_lbl, _("Ningún control de la base manda algo parecido. Puede ser un modelo que SmartIR no tiene."));
        lv_label_set_long_mode(B.bar_lbl, LV_LABEL_LONG_MODE_WRAP);
        return;
    }
    lv_label_set_text(B.bar_lbl, B.found[0].score >= 0.999f ? _("Mandan este mismo código:") :
                                                         _("Los más parecidos, de más a menos:"));
    for (int i = 0; i < B.nfound; i++) {
        const ir_pack_dev_t *d = ir_pack_at(B.found[i].dev);
        if (!d) continue;
        char name[64] = "", sub[128];
        ir_pack_blob_t b;
        if (ir_pack_load(B.found[i].dev, &b)) {
            char raw[64];
            if (ir_pack_code_name(&b, B.found[i].code, raw, sizeof raw)) {
                /* each step of the path in words */
                char *save = raw, *seg;
                while ((seg = save)) {
                    char *dot = strstr(seg, " · ");
                    if (dot) { *dot = 0; save = dot + strlen(" · "); }
                    else save = NULL;
                    size_t l = strlen(name);
                    snprintf(name + l, sizeof name - l, "%s%s", l ? " · " : "", cmd_word(seg, NULL));
                }
            }
            ir_pack_blob_free(&b);
        }
        char title[128];
        snprintf(title, sizeof title, "%s · %s", d->brand, d->models[0] ? d->models : "?");
        snprintf(sub, sizeof sub, "%d %% · %s · %s", (int)(B.found[i].score * 100 + 0.5f), _(CLASS_NAME[d->cls % IR_C_COUNT]),
                 name[0] ? name : "?");
        ir_ui_row(s, NULL, AOS_C_CARD, title, sub, found_cb, (void *)(intptr_t)B.found[i].dev);
    }
    credit(s);
}

static void show(void)
{
    if (!B.page) return;
    lv_obj_clean(B.page);
    B.bar = B.bar_lbl = NULL;
    if (!ir_pack_open()) {
        ir_ui_header(B.page, _("Códigos"), NULL, NULL, NULL);
        lv_obj_t *s = ir_ui_scroll(B.page);
        lv_obj_t *c = ir_ui_card(s);
        text(c, _("Falta la base de códigos"), aos_font_body, AOS_C_TEXT);
        text(c, _("Es infrarrojo_p4.pak, en la carpeta apps de la tarjeta. Se arma con apps/infrarrojo/tools/pack_smartir.py a partir de SmartIR, y tools/install_apps.sh la sube."),
             aos_font_small, AOS_C_DIM);
        text(c, _("Sin ella se aprenden y se mandan códigos igual."), aos_font_small, AOS_C_DIM);
        return;
    }
    if (s_scr == SCR_MODELS && !s_brand[0]) s_scr = SCR_BRANDS;
    if (s_scr == SCR_DEVICE && !ir_pack_at(s_dev)) s_scr = SCR_BRANDS;
    if (s_scr == SCR_FIND && !B.have_find) s_scr = SCR_BRANDS;
    lv_obj_t *s;
    switch (s_scr) {
    case SCR_MODELS:
        ir_ui_header(B.page, s_brand, _("Marcas"), back_cb, NULL);
        s = ir_ui_scroll(B.page);
        lv_obj_set_style_pad_row(s, 12, 0);
        scr_models(s);
        break;
    case SCR_DEVICE: {
        const ir_pack_dev_t *p = ir_pack_at(s_dev);
        ir_ui_header(B.page, p ? p->brand : "", s_from_find ? _("Resultados") : p ? p->brand : _("Marcas"), back_cb, NULL);
        s = ir_ui_scroll(B.page);
        lv_obj_set_style_pad_row(s, 12, 0);
        scr_device(s);
        break;
    }
    case SCR_FIND:
        ir_ui_header(B.page, _("¿De qué control es?"), _("Aprender"), back_cb, NULL);
        s = ir_ui_scroll(B.page);
        lv_obj_set_style_pad_row(s, 12, 0);
        scr_find(s);
        break;
    default:
        ir_ui_header(B.page, _("Códigos"), NULL, NULL, NULL);
        s = ir_ui_scroll(B.page);
        lv_obj_set_style_pad_row(s, 12, 0);
        scr_brands(s);
        break;
    }
}

void ir_base_find(const uint32_t *d, int n)
{
    if (!ir_hw_find(d, n, 0xF)) { ir_ui_toast(_("Ya hay una búsqueda en curso")); return; }
    B.finding = true;
    B.have_find = true;
    B.nfound = 0;
    s_from_find = false;
    s_scr = SCR_FIND;
    show();
    ir_ui_goto_tab(2);
}

void ir_base_build(lv_obj_t *page)
{
    B.page = page;
    show();
}

void ir_base_tick(void)
{
    if (!B.finding) return;
    int p = ir_hw_find_progress();
    if (p >= 100) {
        B.finding = false;
        B.nfound = ir_hw_find_results(B.found, IR_FIND_MAX);
        if (s_scr == SCR_FIND) show();
    } else if (B.bar && p >= 0) {
        lv_bar_set_value(B.bar, p, LV_ANIM_OFF);
    }
}

void ir_base_free(void)
{
    ir_pack_blob_free(&B.blob);
    bool finding = B.finding, have = B.have_find;
    int nf = B.nfound;
    ir_match_t f[IR_FIND_MAX];
    memcpy(f, B.found, sizeof f);
    memset(&B, 0, sizeof B);
    B.blob_dev = -1;
    /* the search's answer survives a turn of the screen (the thread does not) */
    B.have_find = have && !finding;
    B.nfound = nf;
    memcpy(B.found, f, sizeof f);
}
