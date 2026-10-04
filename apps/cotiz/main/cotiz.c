/*
 * P4OS - Exchange rates (from AmoledOS #45)
 *
 * The dollar in its seven houses and four more currencies, from dolarapi.com.
 * WHICH instruments are shown and in what order is chosen from the portal, at
 * /cotiz: in 368 px with a keyboard of 30 px per key, picking eleven things
 * out of a list is torture, and 'clima' had already taught the lesson when the
 * stored city ended up as "Buenos" because the space bar could not be hit.
 *
 * The app and the portal understand each other through TWO preferences, just
 * like clima:
 *   cz_list  the comma-separated list of keys ("blue,oficial,eur")
 *   cz_gen   goes up on every save; the app re-reads it and reloads itself
 *
 * P4OS: the page is the app's own (web/cotiz.js, from the card) and it
 * cannot reach preferences, so the list is also a file, <data>/cotiz.txt,
 * with the same comma-separated keys. When there is one it wins over
 * cz_list, and the app looks at its date along with cz_gen.
 *
 * The generation counter is what saves re-reading an NVS string every three
 * seconds just in case: an integer is compared, and only when it changed is
 * the list read. It is the same mechanism as 'remoto's rc_gen, and not
 * 'clima's, which always re-reads its three keys because they are three short
 * integers.
 *
 * What it does NOT have, and deliberately so:
 *  - It does not keep a copy on the microSD. 'clima' does, because a forecast
 *    is kilobytes and takes a while; here the response is a measured 1223
 *    bytes and comes back in two seconds. With no network it shows "no
 *    connection", which is the truth.
 *  - It draws no changes and no arrows. The API gives the value now, not
 *    yesterday's: inventing a "went up" by comparing against whatever was left
 *    in memory would say "up" or "down" depending on how long ago you opened
 *    the app.
 */
#include "aos_app.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include "cz_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define TICK_MS         200
#define REFRESH_MS      300000      /* 5 min: an exchange rate does not change faster */
#define PREF_MS         3000        /* how often the generation counter is checked */
#define KEY_LIST        "cz_list"
#define KEY_GEN         "cz_gen"
/* The 5" screen fits the whole family at once. */
#define LISTA_DEF       "oficial,blue,bolsa,contadoconliqui,cripto,tarjeta,eur"
#define CARD_H          124
#define GAP             16

typedef struct {
    lv_obj_t   *root;
    lv_obj_t   *cab;                /* the top line: source and age */
    lv_obj_t   *lista;              /* scrolling container */
    lv_obj_t   *pie;
    lv_timer_t *timer;

    /* One row per chosen instrument, created ONCE. Rebuilding them on every
     * refresh is the trap that already cost 111-124 ms in 'clima' and 77 ms in
     * the tuner: the expensive part is not computing, it is creating and
     * destroying LVGL objects. */
    int       orden[CZ_MAX];        /* indices into CZ_ESPECIES, in drawing order */
    int       n;
    lv_obj_t *lbl_venta[CZ_MAX];
    lv_obj_t *lbl_compra[CZ_MAX];
    char      txt_venta[CZ_MAX][16];
    char      txt_compra[CZ_MAX][24];
    char      txt_cab[48];
    char      txt_pie[64];

    cz_datos_t datos;
    int32_t    gen;
    uint32_t   pref_ms;
    long       file_mtime;          /* cotiz.txt as last seen, -1 none */

    int      req;                   /* request in flight, 0 = none */
    bool     pidiendo_monedas;      /* which of the two queries it is on */
    uint32_t fetched_ms;
    uint32_t retry_ms;
    bool     hubo_datos;
    int      ultimo_error;
} cz_t;

static cz_t s_cz;

/* What survives the app being built again (a turn of the screen, the app
 * leaving and coming back): the last answer and when it came. */
static struct {
    cz_datos_t datos;
    bool       hubo_datos;
    uint32_t   fetched_ms;
} s_keep;

static void arrancar(bool forzado);

/* LVGL does not compare: writing the same text to a label invalidates it all
 * the same. */
static void set_txt(lv_obj_t *obj, char *cache, size_t cap, const char *txt)
{
    if (!obj || !txt || strncmp(cache, txt, cap - 1) == 0) {
        return;
    }
    snprintf(cache, cap, "%s", txt);
    lv_label_set_text(obj, cache);
}

/* -------------------------------------------------------------------------- */
/* The chosen list                                                             */
/* -------------------------------------------------------------------------- */

/* Reads cz_list and turns it into indices. The drawing order is CZ_ESPECIES's
 * and not the preference's, so that adding an instrument from the portal does
 * not reorder the whole screen. */
static void lista_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cotiz.txt", aos_hal_path_data());
}

static long lista_mtime(void)
{
    char path[160];
    struct stat st;
    lista_path(path, sizeof(path));
    return stat(path, &st) == 0 ? (long)st.st_mtime : -1;
}

/* The page's file: one line of keys. false if there is none. */
static bool lista_de_archivo(char *out, size_t n)
{
    char path[160];
    lista_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    for (char *c = out; ok && *c; c++) {
        if (*c == '\r' || *c == '\n') {
            *c = 0;
            break;
        }
    }
    return ok && out[0];
}

static void leer_lista(void)
{
    char lista[160] = {0};
    const char *dev = getenv("CZ_LIST");     /* development switch */
    s_cz.file_mtime = lista_mtime();
    if (dev && dev[0]) {
        snprintf(lista, sizeof(lista), "%s", dev);
    } else if (lista_de_archivo(lista, sizeof(lista))) {
        /* the portal's page chose it */
    } else if (!aos_hal_pref_get_str(KEY_LIST, lista, sizeof(lista)) || !lista[0]) {
        snprintf(lista, sizeof(lista), "%s", LISTA_DEF);
    }

    bool elegida[CZ_MAX] = { false };
    char *p = lista;
    while (*p) {
        char *coma = strchr(p, ',');
        if (coma) {
            *coma = 0;
        }
        while (*p == ' ') {
            p++;
        }
        int i = cz_indice(p);
        if (i >= 0) {
            elegida[i] = true;
        }
        if (!coma) {
            break;
        }
        p = coma + 1;
    }

    s_cz.n = 0;
    for (int i = 0; i < CZ_MAX; i++) {
        if (elegida[i]) {
            s_cz.orden[s_cz.n++] = i;
        }
    }
    /* An empty list —the user unticked everything— leaves the screen blank
     * explaining nothing. It falls back to the factory one and the footer says
     * so. */
    if (s_cz.n == 0) {
        s_cz.orden[s_cz.n++] = cz_indice("blue");
    }
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

/* Cards, one per instrument: name and kind on the left, the SELL price
 * large on the right with the buy price under it. Upright they stack in one
 * column; lying down, two. */
static void construir_filas(void)
{
    lv_obj_clean(s_cz.lista);
    memset(s_cz.txt_venta, 0, sizeof(s_cz.txt_venta));
    memset(s_cz.txt_compra, 0, sizeof(s_cz.txt_compra));

    lv_obj_update_layout(s_cz.lista);
    int32_t lw = lv_obj_get_content_width(s_cz.lista);
    int cols = lw > 1000 ? 2 : 1;
    int32_t cw = (lw - (cols - 1) * GAP) / cols;

    for (int f = 0; f < s_cz.n; f++) {
        const cz_especie_t *esp = &CZ_ESPECIES[s_cz.orden[f]];

        lv_obj_t *fila = lv_obj_create(s_cz.lista);
        lv_obj_remove_style_all(fila);
        lv_obj_set_size(fila, cw, CARD_H);
        lv_obj_set_style_bg_color(fila, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(fila, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(fila, AOS_UI_RADIUS, 0);
        lv_obj_set_style_pad_hor(fila, 28, 0);
        lv_obj_remove_flag(fila, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(fila, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *nom = lv_label_create(fila);
        lv_label_set_text(nom, _(esp->nombre));
        lv_obj_set_style_text_font(nom, aos_font_title, 0);
        lv_obj_set_style_text_color(nom, AOS_C_TEXT, 0);
        lv_obj_align(nom, LV_ALIGN_LEFT_MID, 0, -20);

        lv_obj_t *tipo = lv_label_create(fila);
        lv_label_set_text(tipo, esp->dolar ? _("dólar") : _("moneda"));
        lv_obj_set_style_text_font(tipo, aos_font_small, 0);
        lv_obj_set_style_text_color(tipo, esp->dolar ? AOS_C_GREEN : AOS_C_TEAL, 0);
        lv_obj_align(tipo, LV_ALIGN_LEFT_MID, 0, 28);

        s_cz.lbl_venta[f] = lv_label_create(fila);
        lv_label_set_text(s_cz.lbl_venta[f], "--");
        lv_obj_set_style_text_font(s_cz.lbl_venta[f], aos_font_large, 0);
        lv_obj_set_style_text_color(s_cz.lbl_venta[f], AOS_C_TEXT, 0);
        lv_obj_align(s_cz.lbl_venta[f], LV_ALIGN_RIGHT_MID, 0, -16);

        s_cz.lbl_compra[f] = lv_label_create(fila);
        lv_label_set_text(s_cz.lbl_compra[f], "");
        lv_obj_set_style_text_font(s_cz.lbl_compra[f], aos_font_small, 0);
        lv_obj_set_style_text_color(s_cz.lbl_compra[f], AOS_C_DIM, 0);
        lv_obj_align(s_cz.lbl_compra[f], LV_ALIGN_RIGHT_MID, 0, 36);
    }
}

static void pintar(void)
{
    char num[24], buf[64], edad[24];
    int64_t ahora = (int64_t)time(NULL);
    int64_t mas_vieja = 0;

    for (int f = 0; f < s_cz.n; f++) {
        const cz_valor_t *v = &s_cz.datos.v[s_cz.orden[f]];
        if (!v->ok) {
            set_txt(s_cz.lbl_venta[f], s_cz.txt_venta[f],
                    sizeof(s_cz.txt_venta[f]), "--");
            set_txt(s_cz.lbl_compra[f], s_cz.txt_compra[f],
                    sizeof(s_cz.txt_compra[f]), "");
            continue;
        }
        set_txt(s_cz.lbl_venta[f], s_cz.txt_venta[f], sizeof(s_cz.txt_venta[f]),
                cz_precio(v->venta_cent, num, sizeof(num)));

        /* The SELL price is shown large because it is the one paid by whoever
         * buys dollars, which is what you nearly always want to know. The buy
         * price goes small below, and is not shown when it is zero: 'tarjeta'
         * and one or two other houses do not carry it. */
        if (v->compra_cent > 0) {
            snprintf(buf, sizeof(buf), _("compra %s"),
                     cz_precio(v->compra_cent, num, sizeof(num)));
        } else {
            buf[0] = 0;
        }
        set_txt(s_cz.lbl_compra[f], s_cz.txt_compra[f],
                sizeof(s_cz.txt_compra[f]), buf);
        lv_obj_align(s_cz.lbl_venta[f], LV_ALIGN_RIGHT_MID, 0, -16);
        lv_obj_align(s_cz.lbl_compra[f], LV_ALIGN_RIGHT_MID, 0, 36);

        if (v->fecha > 0 && (mas_vieja == 0 || v->fecha < mas_vieja)) {
            mas_vieja = v->fecha;
        }
    }

    /* The header carries the age of the OLDEST rate among those shown, not
     * that of the last request: if the wholesale one has not been quoted since
     * yesterday, that is the useful truth.
     *
     * And it says "the oldest" instead of just the age, because testing it
     * revealed the misunderstanding: with the blue up to date and the real not
     * quoted since the morning, a bare "4 h ago" reads as if the WHOLE screen
     * were stale. Three words stop you distrusting numbers that are fine. */
    cz_antiguedad(mas_vieja, ahora, edad, sizeof(edad));
    if (s_cz.req > 0) {
        snprintf(buf, sizeof(buf), "%s", _("actualizando…"));
    } else if (!edad[0]) {
        snprintf(buf, sizeof(buf), "dolarapi.com");
    } else if (strcmp(edad, "recien") == 0) {
        snprintf(buf, sizeof(buf), "%s", _("dolarapi.com  ·  al día"));
    } else {
        snprintf(buf, sizeof(buf), _("la más vieja, %s"), edad);
    }
    set_txt(s_cz.cab, s_cz.txt_cab, sizeof(s_cz.txt_cab), buf);

    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        snprintf(buf, sizeof(buf), "%s", _("sin conexión"));
    } else if (s_cz.ultimo_error == AOS_HTTP_ERR_SIN_HORA) {
        snprintf(buf, sizeof(buf), "%s", _("esperando la hora de la red…"));
    } else if (s_cz.ultimo_error == AOS_HTTP_ERR_TLS) {
        snprintf(buf, sizeof(buf), "%s", _("no pude verificar el servidor"));
    } else if (s_cz.ultimo_error != 0 && !s_cz.hubo_datos) {
        snprintf(buf, sizeof(buf), _("no se pudo consultar (%d)"), s_cz.ultimo_error);
    } else {
        snprintf(buf, sizeof(buf), "%s", _("tocá para actualizar  ·  elegí cuáles en el portal"));
    }
    set_txt(s_cz.pie, s_cz.txt_pie, sizeof(s_cz.txt_pie), buf);
}

/* -------------------------------------------------------------------------- */
/* Network                                                                     */
/* -------------------------------------------------------------------------- */

/* Is the currencies query needed? Only if some chosen instrument is not a
 * dollar one. Without this a handshake and 895 bytes would be paid for nothing
 * in the most common case, which is looking only at the dollar. */
static bool quiere_monedas(void)
{
    for (int f = 0; f < s_cz.n; f++) {
        if (!CZ_ESPECIES[s_cz.orden[f]].dolar) {
            return true;
        }
    }
    return false;
}

static bool quiere_dolares(void)
{
    for (int f = 0; f < s_cz.n; f++) {
        if (CZ_ESPECIES[s_cz.orden[f]].dolar) {
            return true;
        }
    }
    return false;
}

static void pedir(bool monedas)
{
    s_cz.pidiendo_monedas = monedas;
    s_cz.req = aos_hal_http_get(monedas ? cz_url_monedas() : cz_url_dolares(),
                                CZ_BUF_BYTES);
    aos_hal_log("cotiz", "requesting %s -> id %d", monedas ? "cotizaciones" : "dolares",
                s_cz.req);
}

static void arrancar(bool forzado)
{
    if (s_cz.req > 0) {
        return;
    }
    if (!forzado && s_cz.hubo_datos &&
        (uint32_t)(aos_hal_uptime_ms() - s_cz.fetched_ms) < REFRESH_MS) {
        return;
    }
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        return;
    }
    s_cz.retry_ms = 0;
    /* The two queries go one after the other and not in parallel, even though
     * there are three free HTTP slots: they are to the SAME host, so the
     * second reuses the TLS session the first left behind and saves a whole
     * handshake —1.6 s against 0.6, measured on the board—. In parallel both
     * would pay the full one. */
    if (quiere_dolares()) {
        pedir(false);
    } else {
        pedir(true);
    }
}

static void mirar_respuesta(void)
{
    if (s_cz.req <= 0) {
        return;
    }
    aos_http_state_t st = aos_hal_http_state(s_cz.req);
    if (st == AOS_HTTP_BUSY) {
        return;
    }

    bool era_monedas = s_cz.pidiendo_monedas;
    if (st == AOS_HTTP_DONE) {
        const char *cuerpo = aos_hal_http_body(s_cz.req);
        int len = aos_hal_http_len(s_cz.req);
        int n = cuerpo ? cz_parse(cuerpo, len, &s_cz.datos) : 0;
        aos_hal_log("cotiz", "%s: %d bytes, %d instruments",
                    era_monedas ? "cotizaciones" : "dolares", len, n);
        if (n > 0) {
            s_cz.hubo_datos  = true;
            s_cz.fetched_ms  = (uint32_t)aos_hal_uptime_ms();
            s_cz.ultimo_error = 0;
            s_keep.datos = s_cz.datos;
            s_keep.hubo_datos = true;
            s_keep.fetched_ms = s_cz.fetched_ms;
        }
    } else {
        int motivo = aos_hal_http_status(s_cz.req);
        s_cz.ultimo_error = motivo;
        aos_hal_log("cotiz", "%s failed: reason %d",
                    era_monedas ? "cotizaciones" : "dolares", motivo);
        /* With no time a certificate cannot be verified, and that fixes itself
         * as soon as SNTP lands. It retries instead of leaving the screen
         * empty until somebody opens the app again. */
        if (motivo == AOS_HTTP_ERR_SIN_HORA) {
            s_cz.retry_ms = (uint32_t)aos_hal_uptime_ms() + 3000;
        }
    }

    aos_hal_http_release(s_cz.req);
    s_cz.req = 0;

    /* The second query only goes out now, with the first already released. */
    if (!era_monedas && st == AOS_HTTP_DONE && quiere_monedas()) {
        pedir(true);
    }
    pintar();
}

/* -------------------------------------------------------------------------- */

static void mirar_pref(void)
{
    uint32_t ahora = (uint32_t)aos_hal_uptime_ms();
    if (ahora - s_cz.pref_ms < PREF_MS) {
        return;
    }
    s_cz.pref_ms = ahora;

    int32_t gen = 0;
    aos_hal_pref_get_i32(KEY_GEN, &gen);
    if (gen == s_cz.gen && lista_mtime() == s_cz.file_mtime) {
        return;
    }
    /* Saved from the portal with the app open. Here the objects ARE rebuilt,
     * and that is right: it happens once per save and not ten times a
     * second. */
    aos_hal_log("cotiz", "the list changed (gen %ld -> %ld)",
                (long)s_cz.gen, (long)gen);
    s_cz.gen = gen;
    leer_lista();
    construir_filas();
    pintar();
    arrancar(true);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    mirar_respuesta();
    mirar_pref();

    if (s_cz.retry_ms &&
        (int32_t)((uint32_t)aos_hal_uptime_ms() - s_cz.retry_ms) >= 0) {
        s_cz.retry_ms = 0;
        arrancar(true);
    }
    if (s_cz.req <= 0 && s_cz.hubo_datos) {
        arrancar(false);            /* the five-minute refresh */
    }
    /* The age grows on its own even if nothing new arrives. */
    static uint32_t ultimo;
    uint32_t ahora = (uint32_t)aos_hal_uptime_ms();
    if (ahora - ultimo > 30000) {
        ultimo = ahora;
        pintar();
    }
}

static void ev_toque(lv_event_t *e)
{
    (void)e;
    if (s_cz.req > 0) {
        return;
    }
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        aos_ui_toast(_("sin conexión"), 1200);
        return;
    }
    arrancar(true);
    pintar();
}

/* -------------------------------------------------------------------------- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&s_cz, 0, sizeof(s_cz));
    s_cz.root = root;

    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, ev_toque, LV_EVENT_CLICKED, NULL);

    int32_t W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    const int32_t pad = AOS_UI_PAD;

    lv_obj_t *titulo = lv_label_create(root);
    lv_label_set_text(titulo, _("Cotizaciones"));
    lv_obj_set_style_text_font(titulo, aos_font_large, 0);
    lv_obj_set_style_text_color(titulo, AOS_C_TEXT, 0);
    lv_obj_set_pos(titulo, pad + 4, 8);

    s_cz.cab = lv_label_create(root);
    lv_label_set_text(s_cz.cab, "dolarapi.com");
    lv_obj_set_style_text_font(s_cz.cab, aos_font_small, 0);
    lv_obj_set_style_text_color(s_cz.cab, AOS_C_DIM, 0);
    lv_obj_set_pos(s_cz.cab, pad + 6, 72);

    s_cz.lista = lv_obj_create(root);
    lv_obj_remove_style_all(s_cz.lista);
    lv_obj_set_size(s_cz.lista, W - 2 * pad, H - 120 - 60);
    lv_obj_set_pos(s_cz.lista, pad, 116);
    lv_obj_set_flex_flow(s_cz.lista, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_cz.lista, GAP, 0);
    lv_obj_set_style_pad_column(s_cz.lista, GAP, 0);
    lv_obj_set_scroll_dir(s_cz.lista, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cz.lista, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_cz.lista, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_cz.pie = lv_label_create(root);
    lv_label_set_text(s_cz.pie, "");
    lv_obj_set_style_text_font(s_cz.pie, aos_font_small, 0);
    lv_obj_set_style_text_color(s_cz.pie, AOS_C_DIM, 0);
    lv_obj_set_style_text_align(s_cz.pie, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_cz.pie, W, 40);
    lv_obj_set_pos(s_cz.pie, 0, H - 48);

    s_cz.datos = s_keep.datos;
    s_cz.hubo_datos = s_keep.hubo_datos;
    s_cz.fetched_ms = s_keep.fetched_ms;

    aos_hal_pref_get_i32(KEY_GEN, &s_cz.gen);
    leer_lista();
    construir_filas();
    pintar();

    arrancar(!s_cz.hubo_datos);
    s_cz.timer = lv_timer_create(tick, TICK_MS, NULL);
    return &s_cz;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (s_cz.timer) {
        lv_timer_delete(s_cz.timer);
        s_cz.timer = NULL;
    }
    /* If a query was left in flight it must ALWAYS be released: the slot is
     * marked as abandoned and the HAL's own task cleans it up when it comes
     * out of the recv. Without this, closing the app while it updates leaves
     * one of the three slots occupied forever. */
    if (s_cz.req > 0) {
        aos_hal_http_release(s_cz.req);
        s_cz.req = 0;
    }
    if (self && self->root) {
        lv_obj_clean(self->root);
    }
    memset(&s_cz, 0, sizeof(s_cz));
}

static bool cotiz_init(aos_app_t *app)
{
    app->desc.id       = "aos.cotiz";
    app->desc.name     = "Cotizaciones";
    app->desc.icon     = "$";
    app->desc.icon_vec = AOS_ICON_MONEY;
    app->desc.color_a  = 0x30D158;
    app->desc.color_b  = 0x0B5227;
    app->desc.order    = 78;
    app->desc.flags    = AOS_APP_FLAG_KEEP;

    app->create  = create;
    app->destroy = destroy;
    return true;
}

AOS_APP_ENTRY(cotiz_init);
