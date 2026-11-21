/*
 * P4OS - Tienda: the catalogue of apps and games, on the board
 * (docs/plan/TIENDA.md, phase 3).
 *
 * It is the face of the store's service (components/aos_store), which does
 * the work in a thread of its own: this file only shows what the service
 * knows and asks it for things. Two screens: the list, filtered by a row of
 * chips (everything, a category, what has an update, what is installed),
 * and an app's page, with what it is, what it needs and its extras.
 *
 * The catalogue's text is the service's; here each line is read once into a
 * small record (the name and the description in the board's language, the
 * colours, the icon's blob) and an app's page reads its own line again for
 * the rest. All of it in PSRAM, let go when the app closes.
 *
 * A download goes on with the app closed: the service does not need this
 * screen. aos_app_store_service_tick() keeps the number of updates on the
 * home icon.
 */
#include "aos_apps.h"
#include "aos_store.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef AOS_SIM
  #define BIG_ALLOC(n)  calloc(1, (n))
#else
  #include "esp_heap_caps.h"
  #define BIG_ALLOC(n)  heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

#define APP_ID      "aos.store"
#define ITEMS_MAX   256
#define CATS_MAX    12
#define ICON_MAX    512

/* the chips that are not categories */
enum { F_ALL = 0, F_UPDATES = -1, F_INSTALLED = -2 };

typedef struct {
    char     file[32], id[40], cat[12], ver[28];
    char     name[40], about[244];
    uint32_t color_a, color_b;
    uint8_t  icon[ICON_MAX];
    uint16_t icon_len;
    int      icon_vec;
    char     glyph[12];
    uint32_t size;              /* the package and the packs it cannot start without */
    uint8_t  extras;
    uint32_t line_off, line_len;
    aos_store_app_t st;
} item_t;

typedef struct { char id[12], name[28]; } cat_t;

static struct {
    lv_obj_t *root, *status, *banner, *banner_text, *bar, *list;
    char     *cat;              /* the catalogue's text, while the app is open */
    item_t   *items;
    int       n;
    cat_t     cats[CATS_MAX];
    int       ncats;
    char      source[40], release[28];
    int       filter;           /* F_*, or 1 + the category's index */
    int       sel;              /* the app whose page is up, -1 for the list */
    bool      sure;             /* "Quitar" was tapped once */
    uint32_t  jobs_done;
    bool      busy;
    char      job_file[32];
    bool      asked;            /* the catalogue was asked for on opening */
} S = { .sel = -1 };

static void build(void);

/* ---- small things ---------------------------------------------------------- */

static const char *pick(const cJSON *o)
{
    const char *lang = aos_i18n_current();
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, lang));
    if (!s || !s[0]) s = cJSON_GetStringValue(cJSON_GetObjectItem(o, strcmp(lang, "es") ? "en" : "es"));
    if (!s || !s[0]) s = cJSON_GetStringValue(cJSON_GetObjectItem(o, "es"));
    return s ? s : "";
}

static const char *jstr(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
    return s ? s : "";
}

static uint32_t jnum(const cJSON *o, const char *key)
{
    const cJSON *n = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(n) && n->valuedouble > 0 ? (uint32_t)n->valuedouble : 0;
}

/* "18,5 MB": a comma but in English */
static const char *size_text(uint32_t bytes, char out[20])
{
    char sep = strcmp(aos_i18n_current(), "en") ? ',' : '.';
    if (bytes >= 1024 * 1024) {
        uint32_t tenths = (uint32_t)(((uint64_t)bytes * 10 + 524288) / 1048576);
        snprintf(out, 20, "%u%c%u MB", (unsigned)(tenths / 10), sep, (unsigned)(tenths % 10));
    } else snprintf(out, 20, "%u KB", (unsigned)((bytes + 1023) / 1024));
    return out;
}

static bool on_card(const char *name)
{
    const char *root = aos_hal_path_sd_root();
    char path[200];
    struct stat st;
    if (!root) return false;
    snprintf(path, sizeof path, "%s/apps/%.80s", root, name);
    return stat(path, &st) == 0;
}

/* ---- the catalogue, read for the screen ------------------------------------ */

static void unload(void)
{
    free(S.cat);
    free(S.items);
    S.cat = NULL;
    S.items = NULL;
    S.n = S.ncats = 0;
}

static void states(void)
{
    aos_store_app_t a;
    for (int i = 0; aos_store_app(i, &a); i++)
        for (int k = 0; k < S.n; k++)
            if (!strcmp(S.items[k].file, a.file)) { S.items[k].st = a; break; }
}

static void load(void)
{
    unload();
    size_t len = 0;
    S.cat = aos_store_catalog(&len);
    if (!S.cat) return;
    S.items = BIG_ALLOC(sizeof(item_t) * ITEMS_MAX);
    if (!S.items) { unload(); return; }
    bool head = false;
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && S.cat[end] != '\n') end++;
        cJSON *o = end > at ? cJSON_ParseWithLength(S.cat + at, end - at) : NULL;
        if (o && !head) {
            head = true;
            snprintf(S.source, sizeof S.source, "%s", jstr(cJSON_GetObjectItem(o, "source"), "name"));
            snprintf(S.release, sizeof S.release, "%s", jstr(o, "release"));
            const cJSON *c;
            cJSON_ArrayForEach(c, cJSON_GetObjectItem(o, "cats")) {
                if (S.ncats >= CATS_MAX) break;
                snprintf(S.cats[S.ncats].id, sizeof S.cats[0].id, "%s", jstr(c, "id"));
                snprintf(S.cats[S.ncats].name, sizeof S.cats[0].name, "%s", pick(cJSON_GetObjectItem(c, "name")));
                S.ncats++;
            }
        } else if (o && S.n < ITEMS_MAX) {
            item_t *it = &S.items[S.n];
            const cJSON *pkg = cJSON_GetObjectItem(o, "pkg"), *colors = cJSON_GetObjectItem(o, "colors"), *p;
            snprintf(it->file, sizeof it->file, "%s", jstr(o, "file"));
            snprintf(it->id, sizeof it->id, "%s", jstr(o, "id"));
            snprintf(it->cat, sizeof it->cat, "%s", jstr(o, "cat"));
            snprintf(it->ver, sizeof it->ver, "%s", jstr(o, "ver"));
            snprintf(it->name, sizeof it->name, "%s", pick(cJSON_GetObjectItem(o, "name")));
            snprintf(it->about, sizeof it->about, "%s", pick(cJSON_GetObjectItem(o, "about")));
            const char *ca = cJSON_GetStringValue(cJSON_GetArrayItem(colors, 0));
            const char *cb = cJSON_GetStringValue(cJSON_GetArrayItem(colors, 1));
            it->color_a = ca ? (uint32_t)strtoul(ca, NULL, 16) : 0x3A3A3C;
            it->color_b = cb ? (uint32_t)strtoul(cb, NULL, 16) : 0x1C1C1E;
            const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(o, "icon"));
            size_t got = 0;
            if (b64 && mbedtls_base64_decode(it->icon, ICON_MAX, &got, (const unsigned char *)b64, strlen(b64)) == 0 &&
                aos_icon_ops_check(it->icon, got, NULL) >= 0) it->icon_len = (uint16_t)got;
            it->icon_vec = (int)jnum(o, "icon_vec");
            snprintf(it->glyph, sizeof it->glyph, "%s", jstr(o, "glyph"));
            it->size = jnum(pkg, "installed") ? jnum(pkg, "installed") : jnum(pkg, "size");
            cJSON_ArrayForEach(p, cJSON_GetObjectItem(o, "packs")) {
                if (cJSON_IsTrue(cJSON_GetObjectItem(p, "need"))) it->size += jnum(p, "size");
                else if (it->extras < 255) it->extras++;
            }
            it->line_off = (uint32_t)at;
            it->line_len = (uint32_t)(end - at);
            if (it->file[0]) S.n++;
        }
        cJSON_Delete(o);
        at = end + 1;
    }
    states();
}

static const char *cat_name(const char *id)
{
    for (int i = 0; i < S.ncats; i++)
        if (!strcmp(S.cats[i].id, id)) return S.cats[i].name;
    return id;
}

static bool installed(const item_t *it)
{
    return it->st.state == AOS_STORE_INSTALLED || it->st.state == AOS_STORE_UPDATE || it->st.state == AOS_STORE_BY_HAND;
}

static bool shown(const item_t *it)
{
    if (S.filter == F_ALL) return true;
    if (S.filter == F_UPDATES) return it->st.state == AOS_STORE_UPDATE;
    if (S.filter == F_INSTALLED) return installed(it) || it->st.state == AOS_STORE_SET_ASIDE;
    return S.filter - 1 < S.ncats && !strcmp(it->cat, S.cats[S.filter - 1].id);
}

static int count_updates(void)
{
    int n = 0;
    for (int i = 0; i < S.n; i++) n += S.items[i].st.state == AOS_STORE_UPDATE;
    return n;
}

/* ---- what an app's button says and does ------------------------------------ */

static const char *action_text(const item_t *it, char out[24])
{
    aos_store_status_t st;
    aos_store_status(&st);
    /* its job is the one in hand: the bar above says how far it is */
    if (st.busy && !strcmp(st.file, it->file)) {
        snprintf(out, 24, "...");
        return out;
    }
    switch (it->st.state) {
    case AOS_STORE_INSTALLED:
    case AOS_STORE_BY_HAND:   return _("Abrir");
    case AOS_STORE_UPDATE:    return _("Actualizar");
    case AOS_STORE_SET_ASIDE: return _("Reinstalar");
    default:
        if (!it->st.fits) { snprintf(out, 24, "P4OS %.12s", it->st.fw_min); return out; }
        return _("Obtener");
    }
}

static void act(const item_t *it)
{
    aos_store_status_t st;
    aos_store_status(&st);
    if (st.busy && !strcmp(st.file, it->file)) return;
    if (it->st.state == AOS_STORE_INSTALLED || it->st.state == AOS_STORE_BY_HAND) {
        aos_ui_open(it->id);
        return;
    }
    if (!it->st.fits) {
        char msg[120];
        snprintf(msg, sizeof msg, _("Necesita P4OS %s o más nuevo.\nActualizá el sistema primero."), it->st.fw_min);
        aos_ui_toast(msg, 3500);
        return;
    }
    if (!aos_store_install(it->file, NULL)) aos_ui_toast(_("Hay demasiadas cosas en cola"), 2500);
}

/* ---- pieces ---------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *icon_of(lv_obj_t *parent, const item_t *it, int32_t size)
{
    aos_app_desc_t d = {
        .id = it->id, .name = it->name, .icon = it->glyph[0] ? it->glyph : NULL,
        .icon_vec = (aos_icon_id_t)(it->icon_vec > 0 && it->icon_vec < AOS_ICON_COUNT ? it->icon_vec : AOS_ICON_NONE),
        .color_a = it->color_a, .color_b = it->color_b,
    };
    lv_obj_t *ic = it->icon_len ? aos_icon_create_ops(parent, &d, it->icon, it->icon_len, size)
                                : aos_icon_create(parent, &d, size);
    aos_make_decorative(ic);
    return ic;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, bool strong, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_min_width(b, 168, 0);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_color(b, strong ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 12);
    lv_obj_t *l = aos_label(b, text, aos_font_small, strong ? AOS_C_TEXT : AOS_C_ACCENT);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

/* ---- the list -------------------------------------------------------------- */

static void row_cb(lv_event_t *e)
{
    S.sel = (int)(intptr_t)lv_event_get_user_data(e);
    S.sure = false;
    build();
}

static void action_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < S.n) act(&S.items[i]);
    build();
}

static void chip_cb(lv_event_t *e)
{
    S.filter = (int)(intptr_t)lv_event_get_user_data(e);
    build();
}

static void retry_cb(lv_event_t *e)
{
    (void)e;
    aos_store_refresh();
    build();
}

static void adopt_cb(lv_event_t *e)
{
    (void)e;
    aos_store_adopt();
    build();
}

static void chip(lv_obj_t *parent, const char *text, int filter)
{
    bool on = S.filter == filter;
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_bg_color(c, on ? AOS_C_ACCENT : AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(c, 26, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? AOS_C_TEXT : AOS_C_DIM);
    lv_obj_center(l);
    lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)filter);
    /* the one chosen stays in sight when the row is longer than the screen */
    if (on) {
        lv_obj_update_layout(parent);
        lv_obj_scroll_to_view(c, LV_ANIM_OFF);
    }
}

static void row(lv_obj_t *parent, int i)
{
    const item_t *it = &S.items[i];
    lv_obj_t *r = box(parent);
    lv_obj_set_size(r, lv_pct(100), 128);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 22, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 20, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    icon_of(r, it, 96);
    lv_obj_t *t = box(r);
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 4, 0);
    aos_make_decorative(t);
    lv_obj_t *name = aos_label(t, it->name, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(name, lv_font_get_line_height(aos_font_body));
    lv_obj_t *about = aos_label(t, it->st.state == AOS_STORE_SET_ASIDE ? _("Quedó apartada: hizo caer la placa") : it->about,
                                aos_font_caption, it->st.state == AOS_STORE_SET_ASIDE ? AOS_C_ORANGE : AOS_C_DIM);
    lv_obj_set_width(about, lv_pct(100));
    lv_label_set_long_mode(about, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(about, lv_font_get_line_height(aos_font_caption) * 2);

    char buf[24];
    pill(r, action_text(it, buf), it->st.state == AOS_STORE_UPDATE, action_cb, (void *)(intptr_t)i);
}

static void build_list(lv_obj_t *page)
{
    char buf[64];
    lv_obj_t *chips = box(page);
    lv_obj_set_size(chips, lv_pct(100), 76);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chips, 12, 0);
    lv_obj_add_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
    chip(chips, _("Todo"), F_ALL);
    int ups = count_updates();
    if (ups) {
        snprintf(buf, sizeof buf, "%s  %d", _("Actualizaciones"), ups);
        chip(chips, buf, F_UPDATES);
    }
    for (int i = 0; i < S.ncats; i++) chip(chips, S.cats[i].name, 1 + i);
    chip(chips, _("Instaladas"), F_INSTALLED);

    S.list = box(page);
    lv_obj_set_width(S.list, lv_pct(100));
    lv_obj_set_flex_grow(S.list, 1);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 6, 0);
    lv_obj_set_style_pad_bottom(S.list, 40, 0);
    lv_obj_add_flag(S.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(S.list, LV_DIR_VER);

    if (!S.n) {
        aos_store_status_t st;
        aos_store_status(&st);
        lv_obj_set_flex_align(S.list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(S.list, 28, 0);
        lv_obj_t *g = aos_label(S.list, AOS_SYM_PACKAGE_VARIANT, &aos_sym_72, AOS_C_DIM);
        (void)g;
        lv_obj_t *l = aos_label(S.list, st.busy ? _("Buscando el catálogo...") :
                                aos_hal_net_state() == AOS_NET_CONNECTED ? _("No se pudo traer el catálogo") :
                                                                           _("Sin red: no hay catálogo todavía"),
                                aos_font_body, AOS_C_DIM);
        lv_obj_set_width(l, lv_pct(86));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (!st.busy) pill(S.list, _("Reintentar"), true, retry_cb, NULL);
        return;
    }
    int by_hand = 0, any = 0;
    for (int i = 0; i < S.n; i++) by_hand += S.items[i].st.state == AOS_STORE_BY_HAND;
    if (S.filter == F_INSTALLED && by_hand) {
        /* what was copied to the card before there was a store */
        lv_obj_t *card = box(S.list);
        lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
        lv_obj_set_style_pad_all(card, 24, 0);
        lv_obj_set_style_pad_row(card, 18, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        snprintf(buf, sizeof buf, _("%d apps copiadas a mano"), by_hand);
        aos_label(card, buf, aos_font_body, AOS_C_TEXT);
        lv_obj_t *l = aos_label(card, _("La Tienda puede hacerse cargo de las que son las del catálogo, para avisarte "
                                        "cuando haya una versión nueva. No cambia ninguna app."), aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        pill(card, _("Que las maneje la Tienda"), true, adopt_cb, NULL);
    }
    for (int i = 0; i < S.n; i++) {
        if (!shown(&S.items[i])) continue;
        row(S.list, i);
        any++;
    }
    if (!any) {
        lv_obj_t *l = aos_label(S.list, S.filter == F_UPDATES ? _("Todo está al día") : _("Nada por acá todavía"),
                                aos_font_body, AOS_C_DIM);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 80, 0);
    }
}

/* ---- an app's page --------------------------------------------------------- */

static void back_cb(lv_event_t *e)
{
    (void)e;
    S.sel = -1;
    build();
}

static void remove_cb(lv_event_t *e)
{
    (void)e;
    if (S.sel < 0) return;
    if (!S.sure) S.sure = true;
    else {
        S.sure = false;
        aos_store_remove(S.items[S.sel].file);
    }
    build();
}

static void reinstall_cb(lv_event_t *e)
{
    (void)e;
    if (S.sel >= 0) aos_store_install(S.items[S.sel].file, NULL);
    build();
}

/* the extra's file name is the object's own text, kept in a hidden label */
static void extra_cb(lv_event_t *e)
{
    lv_obj_t *keep = lv_event_get_user_data(e);
    if (S.sel >= 0 && keep) aos_store_install(S.items[S.sel].file, lv_label_get_text(keep));
    build();
}

static void note(lv_obj_t *parent, const char *glyph, lv_color_t color, const char *text)
{
    lv_obj_t *n = box(parent);
    lv_obj_set_size(n, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(n, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(n, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(n, 22, 0);
    lv_obj_set_style_pad_all(n, 22, 0);
    lv_obj_set_style_pad_column(n, 18, 0);
    lv_obj_set_flex_flow(n, LV_FLEX_FLOW_ROW);
    aos_label(n, glyph, &aos_sym_44, color);
    lv_obj_t *l = aos_label(n, text, aos_font_caption, AOS_C_TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
}

static void build_detail(lv_obj_t *page)
{
    const item_t *it = &S.items[S.sel];
    char buf[160], sz[20];
    cJSON *o = S.cat ? cJSON_ParseWithLength(S.cat + it->line_off, it->line_len) : NULL;

    lv_obj_t *backb = box(page);
    lv_obj_set_size(backb, LV_SIZE_CONTENT, 64);
    lv_obj_set_flex_flow(backb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(backb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(backb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(backb, 16);
    lv_obj_add_event_cb(backb, back_cb, LV_EVENT_CLICKED, NULL);
    aos_label(backb, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, AOS_C_ACCENT);
    aos_label(backb, _("Tienda"), aos_font_body, AOS_C_ACCENT);

    lv_obj_t *body = box(page);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 22, 0);
    lv_obj_set_style_pad_bottom(body, 48, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    S.list = body;

    /* the icon, the name and what to do with it */
    lv_obj_t *top = box(body);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 28, 0);
    icon_of(top, it, 176);
    lv_obj_t *t = box(top);
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 10, 0);
    lv_obj_t *name = aos_label(t, it->name, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    snprintf(buf, sizeof buf, "%s  ·  %s  ·  %s", cat_name(it->cat), it->ver, size_text(it->size, sz));
    lv_obj_t *meta = aos_label(t, buf, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(meta, lv_pct(100));
    lv_label_set_long_mode(meta, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *btns = box(t);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(btns, 14, 0);
    lv_obj_set_style_pad_row(btns, 12, 0);
    char ab[24];
    pill(btns, action_text(it, ab), true, action_cb, (void *)(intptr_t)S.sel);
    if (it->st.state == AOS_STORE_INSTALLED || it->st.state == AOS_STORE_UPDATE || it->st.state == AOS_STORE_SET_ASIDE) {
        lv_obj_t *rm = pill(btns, S.sure ? _("¿Seguro? Tocá otra vez") : _("Quitar"), false, remove_cb, NULL);
        lv_obj_set_style_text_color(lv_obj_get_child(rm, 0), AOS_C_RED, 0);
    }

    /* what state it is in, when that is not obvious */
    if (it->st.state == AOS_STORE_SET_ASIDE)
        note(body, AOS_SYM_ALERT_OUTLINE, AOS_C_ORANGE,
             _("Hizo caer la placa dos veces al cargarse y quedó apartada. Reinstalarla trae la versión del catálogo."));
    else if (it->st.state == AOS_STORE_BY_HAND) {
        note(body, AOS_SYM_PACKAGE_VARIANT, AOS_C_DIM,
             _("Está en la tarjeta, copiada a mano: la Tienda no sabe qué versión es. Reinstalarla la deja al día."));
        pill(body, _("Reinstalar desde la Tienda"), false, reinstall_cb, NULL);
    } else if (it->st.state == AOS_STORE_UPDATE) {
        snprintf(buf, sizeof buf, _("Tenés la %s. La del catálogo es la %s."), it->st.have, it->ver);
        note(body, AOS_SYM_DOWNLOAD, AOS_C_ACCENT, buf);
    }
    if (!it->st.fits) {
        snprintf(buf, sizeof buf, _("Necesita P4OS %s o más nuevo."), it->st.fw_min);
        note(body, AOS_SYM_ALERT_OUTLINE, AOS_C_ORANGE, buf);
    }

    lv_obj_t *about = aos_label(body, it->about, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(about, lv_pct(100));
    lv_label_set_long_mode(about, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(about, 6, 0);

    /* what the person brings: the store does not distribute it */
    const cJSON *uf = cJSON_GetObjectItem(o, "user_files");
    if (uf) note(body, AOS_SYM_ALERT_OUTLINE, AOS_C_YELLOW, pick(uf));
    const cJSON *needs = cJSON_GetObjectItem(o, "needs"), *nd;
    cJSON_ArrayForEach(nd, needs) {
        const char *k = cJSON_GetStringValue(nd);
        if (!k) continue;
        note(body, AOS_SYM_CHIP, AOS_C_TEAL,
             !strcmp(k, "usb-host") ? _("Usa un aparato enchufado al puerto USB de la placa.") :
             !strcmp(k, "header")   ? _("Usa algo conectado al header de 40 pines.") : k);
    }

    /* the extras, one at a time */
    const cJSON *p;
    bool head = false;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(o, "packs")) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(p, "need"))) continue;
        if (!head) {
            head = true;
            lv_obj_t *h = aos_label(body, _("Extras"), aos_font_small, AOS_C_DIM);
            lv_obj_set_style_pad_top(h, 12, 0);
        }
        const char *pf = jstr(p, "file");
        lv_obj_t *r = box(body);
        lv_obj_set_size(r, lv_pct(100), 96);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 22, 0);
        lv_obj_set_style_pad_hor(r, 22, 0);
        lv_obj_t *col = box(r);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        aos_label(col, pick(cJSON_GetObjectItem(p, "name")), aos_font_small, AOS_C_TEXT);
        aos_label(col, size_text(jnum(p, "size"), sz), aos_font_caption, AOS_C_DIM);
        if (on_card(pf)) aos_label(r, AOS_SYM_CHECK, &aos_sym_44, AOS_C_GREEN);
        else {
            lv_obj_t *keep = aos_label(r, pf, aos_font_tiny, AOS_C_CARD);
            lv_obj_add_flag(keep, LV_OBJ_FLAG_HIDDEN);
            pill(r, _("Obtener"), false, extra_cb, keep);
        }
    }

    snprintf(buf, sizeof buf, "%s  ·  %s  ·  %s", S.source, jstr(o, "license"), it->file);
    lv_obj_t *foot = aos_label(body, buf, aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(foot, lv_pct(100));
    lv_label_set_long_mode(foot, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_top(foot, 16, 0);
    cJSON_Delete(o);
}

/* ---- the whole screen ------------------------------------------------------ */

static void banner_update(const aos_store_status_t *st)
{
    if (!S.banner) return;
    bool show = st->busy || (!st->last_ok && st->last[0] && st->jobs_done);
    if (!show) { lv_obj_add_flag(S.banner, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(S.banner, LV_OBJ_FLAG_HIDDEN);
    char buf[200];
    if (st->busy) {
        const char *who = st->file;
        for (int i = 0; i < S.n; i++)
            if (!strcmp(S.items[i].file, st->file)) who = S.items[i].name;
        const char *what = !strcmp(st->job, "refresh") ? _("Buscando el catálogo") :
                           !strcmp(st->job, "remove")  ? _("Quitando") :
                           !strcmp(st->job, "adopt")   ? _("Revisando lo que hay en la tarjeta") :
                           !strncmp(st->step, "pack", 4) ? _("Bajando lo que necesita") :
                           !strcmp(st->step, "installing") ? _("Instalando") : _("Bajando");
        if (st->queued) snprintf(buf, sizeof buf, "%s %s  ·  +%d", what, who, st->queued);
        else snprintf(buf, sizeof buf, "%s %s", what, who);
        lv_obj_set_style_text_color(S.banner_text, AOS_C_TEXT, 0);
        lv_obj_remove_flag(S.bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(S.bar, st->total ? (int32_t)((uint64_t)st->got * 100 / st->total) : 0, LV_ANIM_OFF);
    } else {
        snprintf(buf, sizeof buf, "%s %.150s", _("No se pudo:"), st->last);
        lv_obj_set_style_text_color(S.banner_text, AOS_C_ORANGE, 0);
        lv_obj_add_flag(S.bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(S.banner_text, buf);
}

static void build(void)
{
    if (!S.root) return;
    int32_t scroll = S.list ? lv_obj_get_scroll_y(S.list) : 0;
    bool same_view = S.list != NULL;
    static int last_sel = -2, last_filter = 99;
    if (last_sel != S.sel || last_filter != S.filter) same_view = false;
    last_sel = S.sel;
    last_filter = S.filter;
    lv_obj_clean(S.root);
    S.list = S.banner = NULL;
    if (S.sel >= S.n) S.sel = -1;

    lv_obj_t *page = aos_page(S.root);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(page, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(page, 12, 0);
    lv_obj_set_style_pad_row(page, 12, 0);

    if (S.sel < 0) {
        lv_obj_t *head = box(page);
        lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        aos_label(head, _("Tienda"), aos_font_title, AOS_C_TEXT);
        char buf[80];
        if (S.n) snprintf(buf, sizeof buf, "%s  ·  %s", S.source, S.release);
        else buf[0] = 0;
        S.status = aos_label(head, buf, aos_font_caption, AOS_C_DIM);
    }

    /* the job in hand, or how the last one failed */
    S.banner = box(page);
    lv_obj_set_size(S.banner, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(S.banner, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(S.banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(S.banner, 22, 0);
    lv_obj_set_style_pad_all(S.banner, 20, 0);
    lv_obj_set_style_pad_row(S.banner, 12, 0);
    lv_obj_set_flex_flow(S.banner, LV_FLEX_FLOW_COLUMN);
    S.banner_text = aos_label(S.banner, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_set_width(S.banner_text, lv_pct(100));
    lv_label_set_long_mode(S.banner_text, LV_LABEL_LONG_MODE_WRAP);
    S.bar = lv_bar_create(S.banner);
    lv_obj_set_size(S.bar, lv_pct(100), 8);
    lv_obj_set_style_bg_color(S.bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(S.bar, AOS_C_ACCENT, LV_PART_INDICATOR);
    lv_bar_set_range(S.bar, 0, 100);

    if (S.sel >= 0) build_detail(page);
    else build_list(page);

    aos_store_status_t st;
    aos_store_status(&st);
    banner_update(&st);
    if (same_view && S.list && scroll) {
        lv_obj_update_layout(S.list);
        lv_obj_scroll_to_y(S.list, scroll, LV_ANIM_OFF);
    }
}

/* ---- the app --------------------------------------------------------------- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    S.root = root;
    S.list = NULL;
    load();
    aos_store_status_t st;
    aos_store_status(&st);
    S.jobs_done = st.jobs_done;
    S.busy = st.busy;
    /* opening the store is asking for the catalogue (docs/plan/TIENDA.md, D12) */
    if (!S.asked && !st.busy) {
        S.asked = true;
        if (aos_hal_net_state() == AOS_NET_CONNECTED) aos_store_refresh();
    }
    build();
    return NULL;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (self && self->root) lv_obj_clean(self->root);
    S.root = S.list = S.banner = NULL;
    S.asked = false;
    unload();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (S.sel >= 0) { S.sel = -1; build(); return true; }
    return false;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self; (void)inst;
    S.root = root;
    S.list = NULL;
    build();
    return true;
}

static void tick(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (!S.root) return;
    aos_store_status_t st;
    aos_store_status(&st);
    bool changed = st.jobs_done != S.jobs_done || st.busy != S.busy || strcmp(st.file, S.job_file);
    if (st.jobs_done != S.jobs_done) {
        /* something ended: a new catalogue, or other states for the same one */
        char *now = aos_store_catalog(NULL);
        bool other = !S.cat || !now || strcmp(now, S.cat);
        free(now);
        if (other) load();
        else states();
    }
    S.jobs_done = st.jobs_done;
    S.busy = st.busy;
    snprintf(S.job_file, sizeof S.job_file, "%s", st.file);
    if (changed) build();
    else banner_update(&st);
}

void aos_app_store_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id       = APP_ID,
            .name     = "Tienda",
            .icon     = AOS_SYM_PACKAGE_VARIANT,
            .icon_vec = AOS_ICON_NONE,
            .color_a  = 0x2F9BFF,
            .color_b  = 0x0A5BD9,
            .flags    = AOS_APP_FLAG_KEEP,
            .order    = 30,
        },
        .create  = create,
        .destroy = destroy,
        .back    = back,
        .resize  = resize,
        .tick    = tick,
    };
}

/* With the app closed: the number of updates on its icon. Five times a second
 * from the main loop, so it looks every few seconds and never at boot. */
void aos_app_store_service_tick(void)
{
    static uint32_t next, seen = UINT32_MAX;
    static int shown = -1;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now < 30000 || now < next) return;
    next = now + 5000;
    aos_store_status_t st;
    aos_store_status(&st);
    if (st.jobs_done == seen && shown >= 0) return;
    seen = st.jobs_done;
    int n = 0;
    aos_store_app_t a;
    for (int i = 0; aos_store_app(i, &a); i++) n += a.state == AOS_STORE_UPDATE;
    if (n != shown) {
        shown = n;
        aos_ui_set_badge(APP_ID, n);
    }
}
