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
 * An app's page also shows its pictures (the catalogue's "shots"): the
 * service brings each to the card once, a thread decodes it - decoding is
 * slow by LVGL's measure - and the page puts it in its frame when it is
 * ready. And a row of what the app is and does (3D, art packs, joystick,
 * languages...), which the catalogue worked out from its code.
 *
 * A download goes on with the app closed: the service does not need this
 * screen. aos_app_store_service_tick() keeps the number of updates on the
 * home icon.
 */
#include "aos_apps.h"
#include "aos_store.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_pack.h"
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
enum { F_ALL = 0, F_UPDATES = -1, F_INSTALLED = -2, F_SOURCES = -3, F_NEW = -4 };
/* The second row of chips, by where an app comes from: any, what was handed
 * in as a package, what was copied by hand, or 1 + a source's index. */
enum { O_ALL = 0, O_LOOSE = -1, O_HAND = -2 };

/* which screen is up: the catalogue (the list, or an app's page), the
 * sources, the address of one to add, and who that one says it is */
enum { V_STORE = 0, V_SOURCES, V_ADD, V_CONFIRM };

typedef struct {
    char     file[32], id[40], cat[12], ver[28];
    char     name[40], about[244];
    char     from[40];          /* of an app no source lists: the source it was installed from, now gone */
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

/* PSRAM, like the gallery and the decoder's slot below: a few kilobytes
 * that only this screen reads (docs/MEMORY.md). Memory placed there starts
 * as zeros and takes no initial values, so 'sel' gets its "none" (-1) the
 * first time the app is created. */
AOS_BSS_PSRAM static struct {
    lv_obj_t *root, *status, *banner, *banner_text, *banner_detail, *bar, *list;
    char     *cat;              /* the catalogue's text, while the app is open */
    item_t   *items;
    int       n;
    cat_t     cats[CATS_MAX];
    int       ncats;
    char      source[40], release[28];
    int       filter;           /* F_*, or 1 + the category's index */
    int       origin;           /* O_*, or 1 + the source's index */
    int       sel;              /* the app whose page is up, -1 for the list */
    bool      sure;             /* "Quitar" was tapped once */
    uint32_t  jobs_done;
    bool      busy;
    char      job_file[32];
    bool      asked;            /* the catalogue was asked for on opening */
    char      proposed[200];    /* the portal's proposal the screen was drawn with */
    int       view;             /* V_* */
    int       src_sure;         /* the source whose "Quitar" was tapped, 0 for none */
    bool      looking;          /* a source is being looked at */
    char      entry[200];       /* what was typed as a source's address */
    lv_obj_t *ta;
    bool      started;          /* 'sel' was set to "none" once */
    /* the list's icon places, an icon drawn only in those near the screen */
    lv_obj_t *slot[ITEMS_MAX];
    int       slot_item[ITEMS_MAX];
    int       nslot;
} S;

static void build(void);

/* ---- the pictures of the page that is up ----------------------------------- */

#define SHOTS_MAX 4
enum { SH_WANTED = 0, SH_ON_CARD, SH_DECODING, SH_READY, SH_FAILED };

typedef struct {
    char           url[320];
    int            w, h;            /* as the catalogue says: the frame's size */
    int            state;
    char           path[224];
    uint16_t      *px;
    lv_image_dsc_t dsc;
    lv_obj_t      *frame;           /* on the page; NULL between builds */
} shot_t;

AOS_BSS_PSRAM static struct {
    char   owner[32];               /* whose pictures these are */
    int    n;
    shot_t s[SHOTS_MAX];
} G;

/* the one decode in flight: the thread fills it and raises 'done' */
AOS_BSS_PSRAM static struct {
    volatile int busy, done;
    int       idx;
    uint32_t  gen;
    char      path[224];
    int       max_w, max_h, w, h;
    uint16_t *px;
} D;
static uint32_t s_gen;              /* moves when the gallery is let go */

static void gallery_free(void)
{
    s_gen++;
    for (int i = 0; i < G.n; i++)
        if (G.s[i].px) aos_hal_image_free(G.s[i].px);
    memset(&G, 0, sizeof G);
}

static void decode_thread(void *arg)
{
    (void)arg;
    D.px = aos_hal_image_decode(D.path, 0, 0, D.max_w, D.max_h, false, &D.w, &D.h, NULL, NULL);
    D.done = 1;
}

static void shot_show(shot_t *sh)
{
    if (!sh->frame) return;
    lv_obj_clean(sh->frame);
    if (sh->state == SH_READY) {
        lv_obj_t *img = lv_image_create(sh->frame);
        lv_image_set_src(img, &sh->dsc);
        lv_obj_center(img);
    } else {
        lv_obj_t *g = aos_label(sh->frame, sh->state == SH_FAILED ? AOS_SYM_IMAGE_BROKEN_VARIANT : AOS_SYM_IMAGE,
                                &aos_sym_72, AOS_C_CARD2);
        lv_obj_center(g);
    }
}

/* From the app's tick: what arrived, what to ask for, what to decode next. */
static void gallery_tick(void)
{
    if (D.done) {
        shot_t *sh = D.gen == s_gen && D.idx < G.n ? &G.s[D.idx] : NULL;
        if (sh && D.px) {
            sh->px = D.px;
            memset(&sh->dsc, 0, sizeof sh->dsc);
            sh->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            sh->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            sh->dsc.header.w = (uint32_t)D.w;
            sh->dsc.header.h = (uint32_t)D.h;
            sh->dsc.header.stride = (uint32_t)D.w * 2u;
            sh->dsc.data_size = (uint32_t)D.w * (uint32_t)D.h * 2u;
            sh->dsc.data = (const uint8_t *)D.px;
            sh->state = SH_READY;
            shot_show(sh);
        } else {
            if (D.px) aos_hal_image_free(D.px);     /* of a page that is gone */
            if (sh) { sh->state = SH_FAILED; shot_show(sh); }
        }
        D.px = NULL;
        D.done = 0;
        D.busy = 0;
    }
    for (int i = 0; i < G.n; i++) {
        shot_t *sh = &G.s[i];
        if (sh->state == SH_WANTED) {
            int r = aos_store_shot(sh->url, sh->path, sizeof sh->path);
            if (r > 0) sh->state = SH_ON_CARD;
            else if (r < 0) { sh->state = SH_FAILED; shot_show(sh); }
        }
        if (sh->state == SH_ON_CARD && !D.busy) {
            D.busy = 1;
            D.done = 0;
            D.idx = i;
            D.gen = s_gen;
            D.max_w = sh->w;
            D.max_h = sh->h;
            snprintf(D.path, sizeof D.path, "%s", sh->path);
            sh->state = SH_DECODING;
            if (!aos_hal_thread_start("store_img", decode_thread, NULL, 8192, 2)) {
                D.busy = 0;
                sh->state = SH_FAILED;
                shot_show(sh);
            }
        }
    }
}

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
        if (o && cJSON_GetObjectItem(o, "source")) {
            /* a source's own line: the first is the official one */
            if (!head) {
                snprintf(S.source, sizeof S.source, "%s", jstr(cJSON_GetObjectItem(o, "source"), "name"));
                snprintf(S.release, sizeof S.release, "%s", jstr(o, "release"));
            }
            head = true;
            const cJSON *c;
            cJSON_ArrayForEach(c, cJSON_GetObjectItem(o, "cats")) {
                bool have = false;
                for (int k = 0; k < S.ncats; k++) have = have || !strcmp(S.cats[k].id, jstr(c, "id"));
                if (have || S.ncats >= CATS_MAX) continue;
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
            snprintf(it->from, sizeof it->from, "%s", jstr(o, "from"));
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

/* What there is to say of an app no source lists, where the others have
 * their description. */
static const char *loose_text(const item_t *it, char out[200])
{
    if (it->st.origin == AOS_STORE_HAND) return _("Copiada a mano a la tarjeta: la Tienda no la maneja.");
    if (it->from[0]) {
        snprintf(out, 200, _("Instalada de %s, que ya no está entre las fuentes."), it->from);
        return out;
    }
    return _("Instalada desde un paquete (.p4app): no está en ningún catálogo.");
}

static bool of_origin(const item_t *it)
{
    if (S.origin == O_ALL) return true;
    if (S.origin == O_LOOSE) return it->st.origin == AOS_STORE_LOOSE;
    if (S.origin == O_HAND) return it->st.state == AOS_STORE_BY_HAND;
    return it->st.src == S.origin - 1;
}

static bool shown(const item_t *it)
{
    if (!of_origin(it)) return false;
    if (S.filter == F_ALL) return true;
    if (S.filter == F_UPDATES) return it->st.state == AOS_STORE_UPDATE;
    if (S.filter == F_NEW) return it->st.fresh;
    if (S.filter == F_INSTALLED) return installed(it) || it->st.state == AOS_STORE_SET_ASIDE;
    return S.filter - 1 < S.ncats && !strcmp(it->cat, S.cats[S.filter - 1].id);
}

static int count_updates(void)
{
    int n = 0;
    for (int i = 0; i < S.n; i++) n += S.items[i].st.state == AOS_STORE_UPDATE;
    return n;
}

/* The apps of the newest release that brought any (aos_store.h, 'fresh'). */
static int count_new(void)
{
    int n = 0;
    for (int i = 0; i < S.n; i++) n += S.items[i].st.fresh;
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
    /* a part of the system is not opened: it is there, or it is fetched */
    if (it->st.system && it->st.state == AOS_STORE_INSTALLED) return "";
    if (it->st.system && it->st.state == AOS_STORE_BY_HAND) return _("Obtener");
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
    if (it->st.system && it->st.state == AOS_STORE_INSTALLED) return;
    if (!it->st.system && (it->st.state == AOS_STORE_INSTALLED || it->st.state == AOS_STORE_BY_HAND)) {
        if (it->id[0]) aos_ui_open(it->id);
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

/* The app with this id among the ones the board has registered, or NULL. */
static const aos_app_t *registered(const char *id)
{
    for (int i = 0; id[0] && i < aos_ui_app_count(); i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        if (a && a->desc.id && !strcmp(a->desc.id, id)) return a;
    }
    return NULL;
}

static lv_obj_t *icon_of(lv_obj_t *parent, const item_t *it, int32_t size)
{
    /* no catalogue to say what an app nobody lists looks like: when it is
     * loaded, the home screen's own icon of it */
    const aos_app_t *own = it->st.src < 0 ? registered(it->id) : NULL;
    if (own) {
        lv_obj_t *ic = aos_icon_create(parent, &own->desc, size);
        aos_make_decorative(ic);
        return ic;
    }
    aos_app_desc_t d = {
        .id = it->id, .name = it->name, .icon = it->glyph[0] ? it->glyph : it->st.src < 0 ? LV_SYMBOL_FILE : NULL,
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
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (f == F_SOURCES) {
        S.view = V_SOURCES;
        S.src_sure = 0;
    } else S.filter = f;
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

/* Every app copied by hand that is not the catalogue's, installed over from
 * the Tienda in one job. It replaces code, so it asks twice: the first tap
 * turns the button into the question, for a few seconds. */
static uint32_t s_replace_armed;
static bool replace_armed(void)
{
    return s_replace_armed && (uint32_t)aos_hal_uptime_ms() - s_replace_armed < 6000;
}

static void replace_cb(lv_event_t *e)
{
    (void)e;
    if (!replace_armed()) s_replace_armed = (uint32_t)aos_hal_uptime_ms() | 1u;
    else {
        s_replace_armed = 0;
        aos_store_replace_by_hand();
    }
    build();
}

static void origin_cb(lv_event_t *e)
{
    S.origin = (int)(intptr_t)lv_event_get_user_data(e);
    build();
}

/* A chip of the first row (a filter) or, smaller, of the second (an origin). */
static void chip_of(lv_obj_t *parent, const char *text, bool on, bool small, lv_event_cb_t cb, int arg)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, LV_SIZE_CONTENT, small ? 48 : 60);
    lv_obj_set_style_radius(c, small ? 24 : 30, 0);
    lv_obj_set_style_bg_color(c, on ? AOS_C_ACCENT : AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(c, small ? 20 : 26, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(c, small ? 8 : 0);
    lv_obj_t *l = aos_label(c, text, small ? aos_font_caption : aos_font_small, on ? AOS_C_TEXT : AOS_C_DIM);
    lv_obj_center(l);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, (void *)(intptr_t)arg);
    /* the one chosen stays in sight when the row is longer than the screen */
    if (on) {
        lv_obj_update_layout(parent);
        lv_obj_scroll_to_view(c, LV_ANIM_OFF);
    }
}

static void chip(lv_obj_t *parent, const char *text, int filter)
{
    chip_of(parent, text, S.filter == filter, false, chip_cb, filter);
}

static lv_obj_t *chip_row(lv_obj_t *page, int32_t h)
{
    lv_obj_t *chips = box(page);
    lv_obj_set_size(chips, lv_pct(100), h);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chips, 12, 0);
    lv_obj_add_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
    return chips;
}

/* The list's icons, only where they can be seen: an icon pack's picture is
 * 46 KB at the list's 96 px, and "Actualizaciones" with 67 apps held almost
 * 4 MB of PSRAM (on the board, 2026-11-25). A row gets its icon when it comes
 * within half a screen of the list's window and lets go of it a screen and
 * a half away, and the pictures let go are not kept (decoding one again is
 * ~3 ms). With a screen each way and the pack's 1 MB of kept pictures,
 * "Todo" still held 2.75 MB against 3.3 without any of this. */
static void icons_near(void)
{
    if (!S.list || !S.nslot) return;
    lv_area_t win;
    lv_obj_get_coords(S.list, &win);
    int32_t h = lv_area_get_height(&win);
    bool let_go = false;
    for (int k = 0; k < S.nslot; k++) {
        lv_obj_t *sl = S.slot[k];
        if (!sl) continue;
        lv_area_t a;
        lv_obj_get_coords(sl, &a);
        bool near = a.y2 >= win.y1 - h / 2 && a.y1 <= win.y2 + h / 2;
        bool far = a.y2 < win.y1 - h * 3 / 2 || a.y1 > win.y2 + h * 3 / 2;
        if (near && !lv_obj_get_child_count(sl)) icon_of(sl, &S.items[S.slot_item[k]], 96);
        else if (far && lv_obj_get_child_count(sl)) {
            lv_obj_clean(sl);
            let_go = true;
        }
    }
    if (let_go) aos_icon_pack_let_go();
}

static void list_scrolled(lv_event_t *e)
{
    (void)e;
    icons_near();
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

    /* the icon's place; the icon when it comes near the screen (icons_near) */
    lv_obj_t *sl = box(r);
    lv_obj_set_size(sl, 96, 96);
    aos_make_decorative(sl);
    if (S.nslot < ITEMS_MAX) {
        S.slot[S.nslot] = sl;
        S.slot_item[S.nslot++] = i;
    }
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
    char whose[300], lt[200], fresh[340];
    aos_store_source_t sr;
    if (it->st.src > 0 && aos_store_source(it->st.src, &sr)) snprintf(whose, sizeof whose, "%s · %s", sr.name, it->about);
    else if (it->st.src < 0) snprintf(whose, sizeof whose, "%s", loose_text(it, lt));
    else snprintf(whose, sizeof whose, "%s", it->about);
    /* a new one says so before what it is, except under "New", where all are */
    const char *line = whose;
    if (it->st.fresh && S.filter != F_NEW && !installed(it)) {
        snprintf(fresh, sizeof fresh, "%s · %s", _("Nueva"), whose);
        line = fresh;
    }
    lv_obj_t *about = aos_label(t, it->st.state == AOS_STORE_SET_ASIDE ? _("Quedó apartada: hizo caer la placa") : line,
                                aos_font_caption, it->st.state == AOS_STORE_SET_ASIDE ? AOS_C_ORANGE : AOS_C_DIM);
    lv_obj_set_width(about, lv_pct(100));
    lv_label_set_long_mode(about, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(about, lv_font_get_line_height(aos_font_caption) * 2);

    /* a .so copied by hand that nobody lists has no id to open it by */
    char buf[24];
    if (it->st.system && it->st.state == AOS_STORE_INSTALLED) {
        /* a part of the system that is in place: nothing to do with it */
        lv_obj_t *ok = aos_label(r, AOS_SYM_CHECK, &aos_sym_44, AOS_C_GREEN);
        lv_obj_set_style_pad_right(ok, 24, 0);
    } else if (it->st.src >= 0 || it->id[0])
        pill(r, action_text(it, buf), it->st.state == AOS_STORE_UPDATE, action_cb, (void *)(intptr_t)i);
}

/* "Actualizar todas": every update this firmware can run, queued one after
 * another (the queue takes a backup's whole list, aos_store.c QUEUE). */
static void update_all_cb(lv_event_t *e)
{
    (void)e;
    int queued = 0;
    bool full = false;
    for (int i = 0; i < S.n && !full; i++) {
        const item_t *it = &S.items[i];
        if (it->st.state != AOS_STORE_UPDATE || !it->st.fits) continue;
        if (aos_store_install(it->file, NULL)) queued++;
        else full = true;
    }
    if (full) aos_ui_toast(_("Hay demasiadas cosas en cola"), 2500);
    else if (queued) {
        char msg[80];
        snprintf(msg, sizeof msg, _("%d actualizaciones en cola"), queued);
        aos_ui_toast(msg, 2500);
    }
    build();
}

/* At the head of "Actualizaciones": how many, how much at most (the packages
 * and the packs they need; a pack the card has already is not downloaded
 * again), and the button; the ones that need a newer P4OS are left out. */
static void update_all_card(lv_obj_t *list)
{
    int n = 0, later = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < S.n; i++) {
        const item_t *it = &S.items[i];
        if (it->st.state != AOS_STORE_UPDATE) continue;
        if (!it->st.fits) { later++; continue; }
        n++;
        bytes += it->size;
    }
    if (n < 2 && !later) return;        /* one is its own button */
    lv_obj_t *card = box(list);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    char buf[120], sz[20];
    if (n) {
        snprintf(buf, sizeof buf, _("%d actualizaciones, hasta %s"), n, size_text(bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes, sz));
        aos_label(card, buf, aos_font_body, AOS_C_TEXT);
    }
    if (later) {
        snprintf(buf, sizeof buf, _("%d necesitan un P4OS más nuevo: actualizá el sistema primero."), later);
        lv_obj_t *l = aos_label(card, buf, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    if (n) pill(card, _("Actualizar todas"), true, update_all_cb, NULL);
}

static void build_list(lv_obj_t *page)
{
    char buf[64];
    if (S.filter == F_NEW && !count_new()) S.filter = F_ALL;
    lv_obj_t *chips = chip_row(page, 76);
    chip(chips, _("Todo"), F_ALL);
    int ups = count_updates();
    if (ups) {
        snprintf(buf, sizeof buf, "%s  %d", _("Actualizaciones"), ups);
        chip(chips, buf, F_UPDATES);
    }
    /* "New" before the categories, while a source has apps that are (a
     * catalogue that no longer has any took the filter with it, above) */
    if (count_new()) chip(chips, _("Nuevas"), F_NEW);
    for (int i = 0; i < S.ncats; i++) chip(chips, S.cats[i].name, 1 + i);
    chip(chips, _("Instaladas"), F_INSTALLED);
    /* a dot on it while the portal's proposal waits for an answer */
    snprintf(buf, sizeof buf, "%s%s", _("Fuentes"), aos_store_proposal(NULL, 0) ? "  \xE2\x80\xA2" : "");
    chip(chips, buf, F_SOURCES);

    /* where the apps come from, once there is more than the official source
     * to tell apart: another source, a package handed in, a .so copied by hand */
    int nsrc = aos_store_sources(), loose = 0, hand = 0;
    for (int i = 0; i < S.n; i++) {
        loose += S.items[i].st.origin == AOS_STORE_LOOSE;
        hand += S.items[i].st.state == AOS_STORE_BY_HAND;
    }
    if ((S.origin > 0 && S.origin > nsrc) || (S.origin == O_LOOSE && !loose) || (S.origin == O_HAND && !hand)) S.origin = O_ALL;
    if (nsrc > 1 || loose || hand) {
        lv_obj_t *from = chip_row(page, 56);
        chip_of(from, _("Cualquier origen"), S.origin == O_ALL, true, origin_cb, O_ALL);
        for (int i = 0; i < nsrc; i++) {
            aos_store_source_t sr;
            if (!aos_store_source(i, &sr)) continue;
            chip_of(from, sr.name[0] ? sr.name : _("Sin catálogo todavía"), S.origin == 1 + i, true, origin_cb, 1 + i);
        }
        if (loose) chip_of(from, _("Paquetes sueltos"), S.origin == O_LOOSE, true, origin_cb, O_LOOSE);
        if (hand) chip_of(from, _("Copiadas a mano"), S.origin == O_HAND, true, origin_cb, O_HAND);
    }

    S.list = box(page);
    S.nslot = 0;
    lv_obj_add_event_cb(S.list, list_scrolled, LV_EVENT_SCROLL, NULL);
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
    for (int i = 0; i < S.n; i++) by_hand += S.items[i].st.state == AOS_STORE_BY_HAND && S.items[i].st.src >= 0;
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
        /* the ones that stay after that are other builds than the catalogue's */
        l = aos_label(card, _("Las que quedan no son iguales a las del catálogo (otra compilación, o una versión vieja). "
                              "Reemplazarlas baja la de la Tienda para cada una; lo que guardaron y sus packs quedan."),
                      aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        if (replace_armed()) {
            snprintf(buf, sizeof buf, _("Tocá otra vez: reemplaza %d apps"), by_hand);
            pill(card, buf, true, replace_cb, NULL);
        } else pill(card, _("Reemplazarlas por las de la Tienda"), false, replace_cb, NULL);
    }
    if (S.filter == F_UPDATES) update_all_card(S.list);
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
static void system_cb(lv_event_t *e)
{
    (void)e;
    aos_ui_open_app_with("aos.settings", "update");
}

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

static void badge(lv_obj_t *parent, const char *glyph, const char *text)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    aos_label(b, glyph, &aos_sym_28, AOS_C_ACCENT);
    aos_label(b, text, aos_font_caption, AOS_C_TEXT);
}

/* What the app is and does, as the catalogue worked it out. */
static void badges(lv_obj_t *parent, const cJSON *o)
{
    static const struct { const char *key, *glyph, *text; } F[] = {
        { "3d",        AOS_SYM_ROTATE_3D_VARIANT,      "3D" },
        { "packs",     AOS_SYM_PACKAGE_VARIANT,        N_("Arte en packs") },
        { "pad",       AOS_SYM_GAMEPAD_VARIANT,        "Joystick" },
        { "landscape", AOS_SYM_PHONE_ROTATE_LANDSCAPE, N_("Acostada") },
        { "portrait",  AOS_SYM_PHONE_ROTATE_PORTRAIT,  N_("De pie") },
        { "sound",     AOS_SYM_VOLUME_HIGH,            N_("Sonido") },
        { "net",       AOS_SYM_WIFI,                   N_("Usa la red") },
        { "bt",        AOS_SYM_BLUETOOTH,              "Bluetooth" },
        { "portal",    AOS_SYM_WEB,                    N_("Página en el portal") },
    };
    lv_obj_t *r = box(parent);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_style_pad_row(r, 10, 0);
    const cJSON *f;
    int n = 0;
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++)
        cJSON_ArrayForEach(f, cJSON_GetObjectItem(o, "feat"))
            if (cJSON_IsString(f) && !strcmp(f->valuestring, F[i].key)) { badge(r, F[i].glyph, aos_tr(F[i].text)); n++; }
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(o, "needs")) {
        if (!cJSON_IsString(f)) continue;
        if (!strcmp(f->valuestring, "usb-host")) { badge(r, AOS_SYM_USB, "USB"); n++; }
        else if (!strcmp(f->valuestring, "header")) { badge(r, AOS_SYM_EXPANSION_CARD, _("Header de 40 pines")); n++; }
    }
    /* the languages it speaks, when more than its own */
    const cJSON *langs = cJSON_GetObjectItem(o, "langs");
    if (cJSON_GetArraySize(langs) > 1) {
        char buf[64] = "";
        size_t w = 0;
        cJSON_ArrayForEach(f, langs) {
            if (!cJSON_IsString(f) || strlen(f->valuestring) < 2 || w + 8 >= sizeof buf) continue;
            w += (size_t)snprintf(buf + w, sizeof buf - w, "%s%c%c", w ? "  " : "",
                                  (char)(f->valuestring[0] & ~0x20), (char)(f->valuestring[1] & ~0x20));
        }
        badge(r, AOS_SYM_TRANSLATE, buf);
        n++;
    }
    if (!n) lv_obj_delete(r);
}

/* Its pictures, side by side, each in a frame of its size until it arrives. */
static void gallery(lv_obj_t *parent, const item_t *it, const cJSON *o)
{
    if (strcmp(G.owner, it->file)) {
        gallery_free();
        snprintf(G.owner, sizeof G.owner, "%s", it->file);
        const cJSON *sh;
        cJSON_ArrayForEach(sh, cJSON_GetObjectItem(o, "shots")) {
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(sh, "url"));
            int w = (int)jnum(sh, "w"), h = (int)jnum(sh, "h");
            if (!url || G.n >= SHOTS_MAX || w < 16 || h < 16 || w > 720 || h > 720) continue;
            aos_store_source_t from;
            if (strncmp(url, "http", 4) && aos_store_source(it->st.src, &from))
                snprintf(G.s[G.n].url, sizeof G.s[0].url, "%s%s", from.base, url);
            else snprintf(G.s[G.n].url, sizeof G.s[0].url, "%s", url);
            G.s[G.n].w = w;
            G.s[G.n].h = h;
            G.n++;
        }
    }
    if (!G.n) return;
    int tall = 0;
    for (int i = 0; i < G.n; i++) tall = G.s[i].h > tall ? G.s[i].h : tall;
    lv_obj_t *strip = box(parent);
    lv_obj_set_size(strip, lv_pct(100), tall);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(strip, 14, 0);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(strip, LV_SCROLL_SNAP_START);
    for (int i = 0; i < G.n; i++) {
        shot_t *sh = &G.s[i];
        sh->frame = box(strip);
        lv_obj_set_size(sh->frame, sh->w, sh->h);
        lv_obj_set_style_radius(sh->frame, 22, 0);
        lv_obj_set_style_clip_corner(sh->frame, true, 0);
        lv_obj_set_style_bg_color(sh->frame, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(sh->frame, LV_OPA_COVER, 0);
        shot_show(sh);
    }
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
    if (it->st.src >= 0) snprintf(buf, sizeof buf, "%s  ·  %s  ·  %s", cat_name(it->cat), it->ver, size_text(it->size, sz));
    else {
        /* a package handed in has no version to speak of: its receipt says "local" */
        bool ver = it->ver[0] && strcmp(it->ver, "local");
        snprintf(buf, sizeof buf, "%s%s%s", ver ? it->ver : "", ver ? "  ·  " : "", size_text(it->size, sz));
    }
    lv_obj_t *meta = aos_label(t, buf, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(meta, lv_pct(100));
    lv_label_set_long_mode(meta, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *btns = box(t);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(btns, 14, 0);
    lv_obj_set_style_pad_row(btns, 12, 0);
    char ab[24];
    if (it->st.system && it->st.state == AOS_STORE_INSTALLED) {
        lv_obj_t *ok = aos_label(btns, _("Está en la tarjeta"), aos_font_small, AOS_C_GREEN);
        (void)ok;
    } else if (it->st.src >= 0 || it->id[0]) pill(btns, action_text(it, ab), true, action_cb, (void *)(intptr_t)S.sel);
    /* a part of the system is not removed from here: nothing lists who else uses it */
    if (!it->st.system &&
        (it->st.state == AOS_STORE_INSTALLED || it->st.state == AOS_STORE_UPDATE || it->st.state == AOS_STORE_SET_ASIDE)) {
        lv_obj_t *rm = pill(btns, S.sure ? _("¿Seguro? Tocá otra vez") : _("Quitar"), false, remove_cb, NULL);
        lv_obj_set_style_text_color(lv_obj_get_child(rm, 0), AOS_C_RED, 0);
    }

    badges(body, o);
    gallery(body, it, o);
    if (it->st.src > 0) {
        aos_store_source_t from;
        if (aos_store_source(it->st.src, &from)) {
            snprintf(buf, sizeof buf, _("De %s, no de P4OS: nadie de P4OS la revisó."), from.name);
            note(body, AOS_SYM_ALERT_OUTLINE, AOS_C_ORANGE, buf);
        }
    }

    /* what state it is in, when that is not obvious */
    if (it->st.state == AOS_STORE_SET_ASIDE)
        note(body, AOS_SYM_ALERT_OUTLINE, AOS_C_ORANGE,
             _("Hizo caer la placa dos veces al cargarse y quedó apartada. Reinstalarla trae la versión del catálogo."));
    else if (it->st.state == AOS_STORE_BY_HAND && it->st.system)
        note(body, AOS_SYM_PACKAGE_VARIANT, AOS_C_DIM,
             _("Ya está en la tarjeta, copiado a mano. Obtenerlo lo revisa: si es el del catálogo no baja nada."));
    else if (it->st.state == AOS_STORE_BY_HAND && it->st.src >= 0) {
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
        /* where the system is updated from: Settings, Update */
        pill(body, _("Actualizar el sistema"), false, system_cb, NULL);
    }

    char lt[200];
    lv_obj_t *about = aos_label(body, it->st.src < 0 ? loose_text(it, lt) : it->about, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(about, lv_pct(100));
    lv_label_set_long_mode(about, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(about, 6, 0);

    /* the radio's firmware is only fetched here: Settings puts it in the C6 */
    if (it->st.system && it->st.state == AOS_STORE_INSTALLED && !strcmp(it->file, "sys-c6"))
        pill(body, _("Ir a Ajustes, Actualización"), false, system_cb, NULL);

    /* the long description, when its catalogue has one: a few paragraphs */
    const char *more = it->st.src < 0 ? "" : pick(cJSON_GetObjectItem(o, "desc"));
    if (more[0]) {
        lv_obj_t *d = aos_label(body, more, aos_font_small, AOS_C_DIM);
        lv_obj_set_width(d, lv_pct(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(d, 6, 0);
    }

    /* what the person brings: the store does not distribute it */
    const cJSON *uf = cJSON_GetObjectItem(o, "user_files");
    if (uf) note(body, AOS_SYM_ALERT_OUTLINE, AOS_C_YELLOW, pick(uf));

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

    /* whose it is: its source, or nobody's */
    aos_store_source_t whose;
    if (it->st.src >= 0 && aos_store_source(it->st.src, &whose))
        snprintf(buf, sizeof buf, "%s  ·  %s  ·  %s", whose.name, jstr(o, "license"), it->file);
    else snprintf(buf, sizeof buf, "%s.so", it->file);
    lv_obj_t *foot = aos_label(body, buf, aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(foot, lv_pct(100));
    lv_label_set_long_mode(foot, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_top(foot, 16, 0);
    cJSON_Delete(o);
}

/* ---- sources --------------------------------------------------------------- */

static lv_obj_t *back_row(lv_obj_t *page, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = box(page);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 64);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 16);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    aos_label(b, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, AOS_C_ACCENT);
    aos_label(b, text, aos_font_body, AOS_C_ACCENT);
    return b;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

static lv_obj_t *para(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, text, font, color);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static void to_store_cb(lv_event_t *e)   { (void)e; S.view = V_STORE; build(); }
static void to_sources_cb(lv_event_t *e) { (void)e; S.view = V_SOURCES; S.src_sure = 0; S.looking = false; build(); }
static void to_add_cb(lv_event_t *e)     { (void)e; S.view = V_ADD; S.looking = false; build(); }

static void src_remove_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    /* the first tap asks; the second says with its apps (2) or without (1) */
    if (v >= 100) S.src_sure = v - 100;
    else if (S.src_sure > 0) {
        aos_store_source_remove(S.src_sure, v == 2);
        S.src_sure = 0;
    }
    build();
}

static void dev_cb(lv_event_t *e)
{
    aos_store_set_dev(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void proposal_cb(lv_event_t *e)
{
    bool look = lv_event_get_user_data(e) != NULL;
    char url[200];
    if (look && aos_store_proposal(url, sizeof url) && aos_store_probe(url)) {
        snprintf(S.entry, sizeof S.entry, "%s", url);
        S.view = V_ADD;
        S.looking = true;
    }
    aos_store_proposal_clear();
    build();
}

static void build_sources(lv_obj_t *page)
{
    char buf[200];
    back_row(page, _("Tienda"), to_store_cb);
    lv_obj_t *body = box(page);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 18, 0);
    lv_obj_set_style_pad_bottom(body, 48, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    S.list = body;

    aos_label(body, _("Fuentes"), aos_font_title, AOS_C_TEXT);
    para(body, _("De dónde vienen las apps. La primera es la de P4OS; las demás las agregás vos, y sólo acá, en la placa."),
         aos_font_caption, AOS_C_DIM);

    aos_store_source_t sr;
    for (int i = 0; aos_store_source(i, &sr); i++) {
        lv_obj_t *c = card(body);
        aos_label(c, sr.name[0] ? sr.name : _("Sin catálogo todavía"), aos_font_body, AOS_C_TEXT);
        para(c, sr.url, aos_font_tiny, AOS_C_DIM);
        if (sr.loaded) {
            if (sr.skipped) snprintf(buf, sizeof buf, _("Apps: %d  ·  %s  ·  otras %d ya están en otra fuente y no se ofrecen"), sr.apps, sr.release, sr.skipped);
            else snprintf(buf, sizeof buf, _("Apps: %d  ·  %s"), sr.apps, sr.release);
            para(c, buf, aos_font_caption, AOS_C_DIM);
        }
        if (i == 0) para(c, _("La fuente oficial: no se quita."), aos_font_caption, AOS_C_GREEN);
        else if (S.src_sure == i) {
            lv_obj_t *r = box(c);
            lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_column(r, 12, 0);
            lv_obj_set_style_pad_row(r, 12, 0);
            lv_obj_t *a = pill(r, _("Quitarla con sus apps"), false, src_remove_cb, (void *)(intptr_t)2);
            lv_obj_set_style_text_color(lv_obj_get_child(a, 0), AOS_C_RED, 0);
            pill(r, _("Sólo la fuente"), false, src_remove_cb, (void *)(intptr_t)1);
        } else {
            lv_obj_t *rm = pill(c, _("Quitar"), false, src_remove_cb, (void *)(intptr_t)(100 + i));
            lv_obj_set_style_text_color(lv_obj_get_child(rm, 0), AOS_C_RED, 0);
        }
    }
    char asked[200];
    if (aos_store_proposal(asked, sizeof asked)) {
        /* the portal can ask; who is at the board decides, with the warning */
        lv_obj_t *c = card(body);
        lv_obj_set_style_border_color(c, AOS_C_ACCENT, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        para(c, _("Desde el portal proponen esta fuente:"), aos_font_small, AOS_C_TEXT);
        para(c, asked, aos_font_tiny, AOS_C_DIM);
        lv_obj_t *r = box(c);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(r, 12, 0);
        lv_obj_set_style_pad_row(r, 12, 0);
        pill(r, _("Mirarla"), true, proposal_cb, (void *)1);
        pill(r, _("Descartar"), false, proposal_cb, NULL);
    }
    if (aos_store_sources() < AOS_STORE_SOURCES_MAX) pill(body, _("Agregar una fuente"), true, to_add_cb, NULL);

    lv_obj_t *dev = card(body);
    lv_obj_t *r = box(dev);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    aos_label(r, _("Modo desarrollo"), aos_font_body, AOS_C_TEXT);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 96, 56);
    lv_obj_set_style_bg_color(sw, AOS_C_ACCENT, (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    if (aos_store_dev()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, dev_cb, LV_EVENT_VALUE_CHANGED, NULL);
    para(dev, _("Para quien escribe apps: acepta fuentes por http:// en una red de confianza, y deja cambiar el catálogo "
                "oficial por uno de prueba desde el portal. Apagado, todo vuelve al catálogo oficial."), aos_font_caption, AOS_C_DIM);
}

static void look_cb(lv_event_t *e)
{
    (void)e;
    char url[200];
    if (S.ta) snprintf(S.entry, sizeof S.entry, "%s", lv_textarea_get_text(S.ta));
    if (!aos_store_source_url(S.entry, url, sizeof url)) {
        aos_ui_toast(_("Eso no es una dirección:\nusuario/repositorio, o https://..."), 3000);
        return;
    }
    if (!aos_store_probe(url)) {
        aos_ui_toast(_("Hay demasiadas cosas en cola"), 2500);
        return;
    }
    S.looking = true;
    build();
}

static void build_add(lv_obj_t *page)
{
    back_row(page, _("Fuentes"), to_sources_cb);
    aos_label(page, _("Agregar una fuente"), aos_font_title, AOS_C_TEXT);
    para(page, _("Escribí usuario/repositorio de GitHub, o la dirección https:// de un catálogo."), aos_font_caption, AOS_C_DIM);
    S.ta = lv_textarea_create(page);
    lv_obj_set_width(S.ta, lv_pct(100));
    lv_textarea_set_one_line(S.ta, true);
    lv_textarea_set_max_length(S.ta, sizeof S.entry - 1);
    lv_textarea_set_placeholder_text(S.ta, "usuario/repositorio");
    lv_textarea_set_text(S.ta, S.entry);
    lv_obj_set_style_text_font(S.ta, aos_font_body, 0);
    lv_obj_add_event_cb(S.ta, look_cb, LV_EVENT_READY, NULL);
    lv_obj_t *go = pill(page, S.looking ? "..." : _("Buscar"), true, S.looking ? NULL : look_cb, NULL);
    (void)go;
    lv_obj_t *gap = box(page);
    lv_obj_set_flex_grow(gap, 1);
    lv_obj_t *kb = lv_keyboard_create(page);
    lv_obj_set_width(kb, lv_pct(100));
    aos_keyboard_style(kb, aos_font_body);
    lv_keyboard_set_textarea(kb, S.ta);
    lv_obj_add_event_cb(kb, look_cb, LV_EVENT_READY, NULL);
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    aos_store_probe_t pr;
    aos_store_probe_result(&pr);
    if (pr.ok) aos_store_source_add(pr.url);
    S.entry[0] = 0;
    S.view = V_SOURCES;
    build();
}

static void build_confirm(lv_obj_t *page)
{
    char buf[200];
    aos_store_probe_t pr;
    aos_store_probe_result(&pr);
    cJSON *src = cJSON_Parse(pr.source_json);
    back_row(page, _("Fuentes"), to_sources_cb);
    lv_obj_t *body = box(page);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 18, 0);
    lv_obj_set_style_pad_bottom(body, 48, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    S.list = body;

    /* who it says it is: its own words, nobody checked them */
    lv_obj_t *c = card(body);
    para(c, jstr(src, "name"), aos_font_title, AOS_C_TEXT);
    snprintf(buf, sizeof buf, _("Dice ser de %s  ·  apps: %d"), jstr(src, "by")[0] ? jstr(src, "by") : "?", pr.apps);
    para(c, buf, aos_font_caption, AOS_C_DIM);
    if (pick(cJSON_GetObjectItem(src, "about"))[0]) para(c, pick(cJSON_GetObjectItem(src, "about")), aos_font_small, AOS_C_TEXT);
    if (jstr(src, "url")[0]) para(c, jstr(src, "url"), aos_font_tiny, AOS_C_DIM);
    para(c, pr.url, aos_font_tiny, AOS_C_DIM);

    lv_obj_t *w = card(body);
    lv_obj_set_style_border_color(w, AOS_C_ORANGE, 0);
    lv_obj_set_style_border_width(w, 2, 0);
    aos_label(w, AOS_SYM_ALERT_OUTLINE, &aos_sym_44, AOS_C_ORANGE);
    para(w, _("Las apps de esta fuente no las revisó nadie de P4OS. Una app corre con todos los permisos del sistema: "
              "lee y escribe la tarjeta, ve las contraseñas guardadas, usa la red y los pines. "
              "Agregá sólo fuentes de gente en la que confiás."), aos_font_small, AOS_C_TEXT);

    lv_obj_t *r = box(body);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, 14, 0);
    lv_obj_set_style_pad_row(r, 12, 0);
    pill(r, _("Entiendo: agregarla"), true, add_cb, NULL);
    pill(r, _("Cancelar"), false, to_sources_cb, NULL);
    cJSON_Delete(src);
}

/* ---- the whole screen ------------------------------------------------------ */

/* How a job failed, as a sentence. */
static const char *why_text(int kind)
{
    switch (kind) {
    case AOS_STORE_E_NETWORK:    return _("No se pudo bajar: revisá la red y probá de nuevo.");
    case AOS_STORE_E_NOT_THERE:  return _("No está en el servidor.");
    case AOS_STORE_E_CLOCK:      return _("La placa todavía no tiene la hora de la red: probá en un momento.");
    case AOS_STORE_E_SERVER:     return _("No se pudo verificar el servidor.");
    case AOS_STORE_E_CARD:       return _("No se pudo escribir en la tarjeta: ¿está llena?");
    case AOS_STORE_E_PACKAGE:    return _("El paquete no es el publicado, o está dañado.");
    case AOS_STORE_E_RULES:      return _("El paquete quiere escribir donde no le corresponde.");
    case AOS_STORE_E_FIRMWARE:   return _("Necesita un P4OS más nuevo.");
    case AOS_STORE_E_CATALOG:    return _("El catálogo no se puede leer.");
    case AOS_STORE_E_SOURCE:     return _("Esa fuente no se puede usar.");
    case AOS_STORE_E_MEMORY:     return _("No alcanzó la memoria.");
    case AOS_STORE_E_NOT_LISTED: return _("No está en el catálogo.");
    case AOS_STORE_E_IN_STORE:   return _("Esa app es de la Tienda: se instala y se quita desde acá.");
    case AOS_STORE_E_NOT_STORE:  return _("No la instaló la Tienda.");
    case AOS_STORE_E_FLASH:      return _("No se pudo escribir la otra ranura.");
    case AOS_STORE_E_UP_TO_DATE: return _("Ya es la versión que corre.");
    default:                     return _("Algo salió mal.");
    }
}

static void banner_update(const aos_store_status_t *st)
{
    if (!S.banner) return;
    bool show = st->busy || (!st->last_ok && st->last[0] && st->jobs_done && st->last_kind != AOS_STORE_E_CANCELLED);
    if (!show) { lv_obj_add_flag(S.banner, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(S.banner, LV_OBJ_FLAG_HIDDEN);
    char buf[200];
    if (st->busy) {
        const char *who = st->file;
        for (int i = 0; i < S.n; i++)
            if (!strcmp(S.items[i].file, st->file)) who = S.items[i].name;
        const char *what = !strcmp(st->job, "refresh") ? _("Buscando el catálogo") :
                           !strcmp(st->job, "firmware") ? (!strcmp(st->step, "writing") ? _("Escribiendo P4OS") : _("Bajando P4OS")) :
                           !strcmp(st->job, "remove")  ? _("Quitando") :
                           !strcmp(st->job, "adopt")   ? _("Revisando lo que hay en la tarjeta") :
                           !strcmp(st->job, "replace") ? _("Reemplazando") :
                           !strncmp(st->step, "pack", 4) ? _("Bajando lo que necesita") :
                           !strcmp(st->step, "installing") ? _("Instalando") : _("Bajando");
        if (st->queued) snprintf(buf, sizeof buf, "%s %s  ·  +%d", what, who, st->queued);
        else snprintf(buf, sizeof buf, "%s %s", what, who);
        lv_obj_set_style_text_color(S.banner_text, AOS_C_TEXT, 0);
        lv_obj_remove_flag(S.bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(S.bar, st->total ? (int32_t)((uint64_t)st->got * 100 / st->total) : 0, LV_ANIM_OFF);
        if (S.banner_detail) lv_obj_add_flag(S.banner_detail, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *who = st->last_file;
        for (int i = 0; i < S.n; i++)
            if (!strcmp(S.items[i].file, st->last_file) || !strcmp(S.items[i].id, st->last_file)) who = S.items[i].name;
        char what[96];
        const char *j = st->last_job;
        if (!strcmp(j, "install")) snprintf(what, sizeof what, _("No se pudo instalar %s."), who);
        else if (!strcmp(j, "remove")) snprintf(what, sizeof what, _("No se pudo quitar %s."), who);
        else snprintf(what, sizeof what, "%s", !strcmp(j, "refresh")  ? _("No se pudo traer el catálogo.") :
                                                !strcmp(j, "look")     ? _("No se pudo mirar esa fuente.") :
                                                !strcmp(j, "add")      ? _("No se pudo agregar la fuente.") :
                                                !strcmp(j, "unsource") ? _("No se pudo quitar la fuente.") :
                                                !strcmp(j, "sideload") ? _("No se pudo instalar el paquete.") :
                                                !strcmp(j, "firmware") ? _("No se pudo actualizar el sistema.") : _("No se pudo."));
        snprintf(buf, sizeof buf, "%s %s", what, why_text(st->last_kind));
        lv_obj_set_style_text_color(S.banner_text, AOS_C_ORANGE, 0);
        lv_obj_add_flag(S.bar, LV_OBJ_FLAG_HIDDEN);
        /* the log's own line, small: what to tell whoever can fix it */
        if (S.banner_detail) {
            lv_obj_remove_flag(S.banner_detail, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(S.banner_detail, st->last);
        }
    }
    lv_label_set_text(S.banner_text, buf);
}

static void build(void)
{
    if (!S.root) return;
    int32_t scroll = S.list ? lv_obj_get_scroll_y(S.list) : 0;
    bool same_view = S.list != NULL;
    static int last_sel = -2, last_filter = 99, last_view = -1, last_origin = 99;
    if (last_sel != S.sel || last_filter != S.filter || last_view != S.view || last_origin != S.origin) same_view = false;
    last_origin = S.origin;
    last_view = S.view;
    last_sel = S.sel;
    last_filter = S.filter;
    lv_obj_clean(S.root);
    S.list = S.banner = S.banner_detail = NULL;
    S.nslot = 0;
    S.ta = NULL;
    if (S.sel >= S.n) S.sel = -1;
    for (int i = 0; i < G.n; i++) G.s[i].frame = NULL;
    if (S.sel < 0 || strcmp(G.owner, S.items[S.sel].file)) gallery_free();

    lv_obj_t *page = aos_page(S.root);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(page, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(page, 12, 0);
    lv_obj_set_style_pad_row(page, 12, 0);

    if (S.view == V_STORE && S.sel < 0) {
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
    S.banner_detail = aos_label(S.banner, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(S.banner_detail, lv_pct(100));
    lv_label_set_long_mode(S.banner_detail, LV_LABEL_LONG_MODE_WRAP);
    S.bar = lv_bar_create(S.banner);
    lv_obj_set_size(S.bar, lv_pct(100), 8);
    lv_obj_set_style_bg_color(S.bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(S.bar, AOS_C_ACCENT, LV_PART_INDICATOR);
    lv_bar_set_range(S.bar, 0, 100);

    if (S.view == V_SOURCES) build_sources(page);
    else if (S.view == V_ADD) build_add(page);
    else if (S.view == V_CONFIRM) build_confirm(page);
    else if (S.sel >= 0) build_detail(page);
    else build_list(page);

    aos_store_status_t st;
    aos_store_status(&st);
    banner_update(&st);
    if (same_view && S.list && scroll) {
        lv_obj_update_layout(S.list);
        lv_obj_scroll_to_y(S.list, scroll, LV_ANIM_OFF);
    }
    if (S.list) {
        lv_obj_update_layout(S.list);
        icons_near();
    }
}

/* ---- the app --------------------------------------------------------------- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    if (!S.started) {
        S.started = true;
        S.sel = -1;
    }
    S.root = root;
    S.list = NULL;
    /* a picture that finished decoding after the app was closed */
    if (D.done) {
        if (D.px) aos_hal_image_free(D.px);
        D.px = NULL;
        D.done = D.busy = 0;
    }
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
    gallery_free();
    unload();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (S.view == V_CONFIRM || S.view == V_ADD) { S.view = V_SOURCES; S.looking = false; build(); return true; }
    if (S.view == V_SOURCES) { S.view = V_STORE; build(); return true; }
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
    gallery_tick();
    aos_store_status_t st;
    aos_store_status(&st);
    /* the portal proposed a source, another one, or the proposal went away */
    char proposed[200] = "";
    aos_store_proposal(proposed, sizeof proposed);
    bool changed = st.jobs_done != S.jobs_done || st.busy != S.busy || strcmp(st.file, S.job_file) ||
                   (strcmp(proposed, S.proposed) && S.view != V_ADD);
    if (S.view != V_ADD) snprintf(S.proposed, sizeof S.proposed, "%s", proposed);
    if (st.jobs_done != S.jobs_done && st.last_ok && !strcmp(st.last_job, "sideload") && S.view == V_STORE) {
        /* a package handed in went in: the list of those, where it now is */
        S.sel = -1;
        S.filter = F_ALL;
        S.origin = O_LOOSE;
    }
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
    /* the look at a source ended: who it is and the warning, or back to the address */
    if (S.view == V_ADD && S.looking && !st.busy) {
        aos_store_probe_t pr;
        aos_store_probe_result(&pr);
        S.looking = false;
        if (pr.ok) S.view = V_CONFIRM;
        changed = true;
    }
    /* typing: a rebuild would take the text and the cursor away */
    if (changed && S.view == V_ADD && S.ta) snprintf(S.entry, sizeof S.entry, "%s", lv_textarea_get_text(S.ta));
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
