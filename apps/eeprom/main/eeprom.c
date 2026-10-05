/*
 * P4OS - EEPROM: read, write and edit the serial memories of the bench.
 *
 *   Chip       which part, where it hangs (I2C port and address, SPI port,
 *              chip select and clock, the four Microwire GPIOs), detecting
 *              it, the size test, and its write protection.
 *   Memoria    the image in hex and ASCII: fast scrolling, go to an
 *              address, search bytes or text, edit cells; the bytes that
 *              differ from a saved version lit; read, write the changes,
 *              erase.
 *   Versiones  every read and every copy taken before a write, on the card
 *              (/eeprom/<chip>/): open one, compare, restore it (written
 *              and verified), a note, delete.
 *   Cableado   the chip's package with its pins and where each goes on the
 *              header.
 *
 * The bus work runs in a thread, one job at a time (ee_drv.c), and a timer
 * here picks the results up. The portal's page reaches the app through
 * files on the card (ee_portal.c). Built from the Bus app's I2C and SPI
 * tabs and the Programmer's list of files; HANDOFF-NUEVAS-APPS.md, T1.
 */
#include "ee.h"

#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A DIP-8 chip: three legs a side behind a dark body with its pin-1 dot. */
static const uint8_t EE_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -23, -16, 16, 7, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -23,   0, 16, 7, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -23,  16, 16, 7, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,  23, -16, 16, 7, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,  23,   0, 16, 7, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,  23,  16, 16, 7, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,   0,   0, 36, 56, 6, AIC_C_LIT(0x1C1C1E), 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_LEFT, 6, 6, 8, 8, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_OUT,
    AIC_END
};

ee_state_t S;
ee_ui_t U;

static lv_timer_t *s_timer;
static uint32_t s_seen_seq, s_poll_ms, s_prog_ms, s_last_dirty;
static bool s_last_busy;

static const char *const TAB_NAME[TAB_COUNT] = { N_("Chip"), N_("Memoria"), N_("Versiones"), N_("Cableado") };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_CHIP, AOS_SYM_GRID, AOS_SYM_DATABASE, AOS_SYM_CONNECTION };

const ee_chip_t *ee_cur(void) { return ee_chip_at(S.conf.chip); }

/* -------------------------------------------------------------------------- */
/* The image                                                                   */
/* -------------------------------------------------------------------------- */

void ee_img_free(void)
{
    free(S.img);
    free(S.dirty);
    S.img = S.dirty = NULL;
    S.size = 0;
    S.valid = false;
    S.ndirty = 0;
    S.nundo = 0;
}

bool ee_img_alloc(uint32_t size)
{
    if (S.img && S.size == size) return true;
    ee_img_free();
    S.img = ee_alloc(size);
    S.dirty = ee_alloc((size + 7) / 8);
    if (!S.img || !S.dirty) {
        ee_img_free();
        return false;
    }
    memset(S.img, 0xFF, size);
    memset(S.dirty, 0, (size + 7) / 8);
    S.size = size;
    return true;
}

void ee_dirty_clear(void)
{
    if (S.dirty) memset(S.dirty, 0, (S.size + 7) / 8);
    S.ndirty = 0;
    S.nundo = 0;
}

bool ee_is_dirty(uint32_t a)
{
    return S.dirty && a < S.size && (S.dirty[a >> 3] & (1u << (a & 7)));
}

void ee_poke(uint32_t a, uint8_t v)
{
    if (!S.valid || J.busy || a >= S.size) return;
    if (S.nundo == EE_UNDO) {
        memmove(&S.undo[0], &S.undo[1], sizeof S.undo[0] * (EE_UNDO - 1));
        S.nundo--;
    }
    S.undo[S.nundo].a = a;
    S.undo[S.nundo].old = S.img[a];
    S.nundo++;
    S.img[a] = v;
    if (!ee_is_dirty(a)) {
        S.dirty[a >> 3] |= (uint8_t)(1u << (a & 7));
        S.ndirty++;
    }
    ee_diff_count();
}

bool ee_undo(void)
{
    if (!S.nundo || J.busy) return false;
    S.nundo--;
    uint32_t a = S.undo[S.nundo].a;
    S.img[a] = S.undo[S.nundo].old;
    bool more = false;
    for (int i = 0; i < S.nundo; i++) more |= S.undo[i].a == a;
    if (!more && ee_is_dirty(a)) {
        S.dirty[a >> 3] &= (uint8_t)~(1u << (a & 7));
        S.ndirty--;
    }
    S.cursor = a;
    ee_diff_count();
    return true;
}

void ee_diff_count(void)
{
    S.ndiff = 0;
    if (!S.ref || !S.valid) return;
    uint32_t n = S.size < S.ref_size ? S.size : S.ref_size;
    for (uint32_t i = 0; i < n; i++) S.ndiff += S.img[i] != S.ref[i];
    S.ndiff += (S.size > S.ref_size ? S.size - S.ref_size : S.ref_size - S.size);
}

void ee_ref_set(uint8_t *buf, uint32_t size, const char *name)
{
    free(S.ref);
    S.ref = buf;
    S.ref_size = size;
    snprintf(S.ref_name, sizeof S.ref_name, "%s", name ? name : "");
    ee_diff_count();
}

void ee_ref_clear(void)
{
    free(S.ref);
    S.ref = NULL;
    S.ref_size = 0;
    S.ref_name[0] = 0;
    S.ndiff = 0;
}

void ee_vers_reload(void)
{
    if (!S.vers) S.vers = ee_alloc(sizeof(ee_ver_t) * EE_VER_MAX);
    S.nvers = S.vers ? ee_ver_list(ee_cur()->name, S.vers, EE_VER_MAX) : 0;
    S.vers_stale = false;
}

/* -------------------------------------------------------------------------- */
/* Starting reads and writes                                                   */
/* -------------------------------------------------------------------------- */

static bool can_start(void)
{
    if (!J.busy) return true;
    aos_ui_toast(_("Hay un trabajo en curso"), 1600);
    return false;
}

bool ee_start_read(bool from_portal)
{
    if (!can_start()) return false;
    const ee_chip_t *c = ee_cur();
    if (!ee_img_alloc(c->size)) {
        aos_ui_toast(_("No hay memoria para la imagen"), 2000);
        return false;
    }
    S.valid = false;
    ee_dirty_clear();
    if (!ee_job_start(JOB_READ, 0)) {
        aos_ui_toast(_("No se pudo empezar"), 1600);
        return false;
    }
    J.from_portal = from_portal;
    ee_pg_mem_refresh();
    return true;
}

bool ee_start_write(int mode, bool from_portal)
{
    if (!can_start()) return false;
    const ee_chip_t *c = ee_cur();
    if (mode == WR_ERASE) {
        if (!ee_img_alloc(c->size)) {
            aos_ui_toast(_("No hay memoria para la imagen"), 2000);
            return false;
        }
    } else if (!S.valid || S.size != c->size) {
        aos_ui_toast(_("La imagen no es del tamaño de este chip"), 2000);
        return false;
    }
    if (mode == WR_DIRTY && !S.ndirty) {
        aos_ui_toast(_("No hay cambios para escribir"), 1600);
        return false;
    }
    if (!ee_job_start(JOB_WRITE, mode)) {
        aos_ui_toast(_("No se pudo empezar"), 1600);
        return false;
    }
    J.from_portal = from_portal;
    ee_pg_mem_refresh();
    return true;
}

/* -------------------------------------------------------------------------- */
/* Pages and the job bar                                                       */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    ee_pg_mem_forget();
    lv_obj_clean(U.content);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == S.tab ? C_AMBER : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 1), c, 0);
    }
    if (S.tab == TAB_VER && (S.vers_stale || !S.vers)) ee_vers_reload();
    switch (S.tab) {
    case TAB_CHIP: ee_pg_chip(U.content); break;
    case TAB_MEM:  ee_pg_mem(U.content); break;
    case TAB_VER:  ee_pg_ver(U.content); break;
    case TAB_WIRE: ee_pg_wire(U.content); break;
    }
}

void ee_rebuild(void) { build_page(); }

static void tab_cb(lv_event_t *e)
{
    S.tab = (int)(intptr_t)lv_event_get_user_data(e);
    build_page();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    J.cancel = true;
}

static void jobbar_refresh(void)
{
    if (!U.jobbar) return;
    if (!J.busy) {
        lv_obj_add_flag(U.jobbar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(U.jobbar, LV_OBJ_FLAG_HIDDEN);
    uint32_t total = J.total, done = J.done;
    int pct = total ? (int)((uint64_t)done * 100 / total) : 0;
    const char *who = J.from_portal ? _(" (pedido del portal)") : "";
    if (total) lv_label_set_text_fmt(U.job_text, "%s  %d %%%s", ee_phase_text(J.phase), pct, who);
    else lv_label_set_text_fmt(U.job_text, "%s%s", ee_phase_text(J.phase), who);
    int32_t tw = lv_obj_get_width(lv_obj_get_parent(U.job_fill));
    lv_obj_set_width(U.job_fill, total ? tw * pct / 100 : tw / 8);
    bool can = J.job == JOB_READ || J.job == JOB_WRITE;
    if (can) lv_obj_remove_flag(U.job_cancel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.job_cancel, LV_OBJ_FLAG_HIDDEN);
}

static void build_jobbar(int32_t y)
{
    int32_t w = U.W - 2 * AOS_UI_PAD;
    U.jobbar = ee_box(U.root, w, 96);
    lv_obj_set_pos(U.jobbar, AOS_UI_PAD, y - 96 - 12);
    lv_obj_set_style_bg_color(U.jobbar, lv_color_hex(0x26262A), 0);
    lv_obj_set_style_bg_opa(U.jobbar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.jobbar, 24, 0);
    lv_obj_set_style_shadow_width(U.jobbar, 30, 0);
    lv_obj_set_style_shadow_opa(U.jobbar, LV_OPA_60, 0);
    lv_obj_add_flag(U.jobbar, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    U.job_text = aos_label(U.jobbar, "", aos_font_small, AOS_C_TEXT);
    lv_obj_align(U.job_text, LV_ALIGN_TOP_LEFT, 24, 14);
    lv_obj_t *track = ee_box(U.jobbar, w - 48 - 200, 12);
    lv_obj_align(track, LV_ALIGN_BOTTOM_LEFT, 24, -18);
    lv_obj_set_style_bg_color(track, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 6, 0);
    U.job_fill = ee_box(track, 0, 12);
    lv_obj_set_style_bg_color(U.job_fill, C_AMBER, 0);
    lv_obj_set_style_bg_opa(U.job_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.job_fill, 6, 0);
    U.job_cancel = ee_pill(U.jobbar, NULL, _("Cancelar"), AOS_C_CARD2, cancel_cb, NULL);
    lv_obj_set_height(U.job_cancel, 64);
    lv_obj_align(U.job_cancel, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_update_layout(U.jobbar);
    jobbar_refresh();
}

static void build_all(void)
{
    lv_obj_t *root = U.root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    const int32_t tab_h = U.land ? 96 : 116;

    U.content = ee_box(root, U.W, U.H - tab_h);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.content, 8, 0);
    lv_obj_update_layout(U.content);
    U.CW = U.W - 2 * AOS_UI_PAD;
    U.CH = U.H - tab_h - 8;

    lv_obj_t *bar = ee_box(root, U.W, tab_h);
    lv_obj_set_pos(bar, 0, U.H - tab_h);
    lv_obj_set_style_bg_color(bar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = ee_box(bar, U.W / TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *g = aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, U.land ? -14 : -16);
        lv_obj_t *n = aos_label(t, _(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, U.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }
    build_jobbar(U.H - tab_h);
    build_page();
}

/* -------------------------------------------------------------------------- */
/* What a job brought back                                                     */
/* -------------------------------------------------------------------------- */

static void when(char *out, size_t len)
{
    if (!aos_hal_time_is_valid()) { out[0] = 0; return; }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, len, " %02d:%02d", tm.tm_hour, tm.tm_min);
}

static void tell(bool ok, const char *title)
{
    if (ok) aos_ui_toast(J.msg, 2600);
    else ee_confirm(title, J.msg[0] ? J.msg : _("Falló."), NULL, false, NULL, NULL);
}

static void job_done(void)
{
    char hm[12];
    switch (J.job) {
    case JOB_READ:
        S.valid = J.ok;
        if (J.ok) {
            snprintf(S.img_chip, sizeof S.img_chip, "%s", ee_chip_at(J.conf.chip)->name);
            S.sums = J.sums;
            ee_dirty_clear();
            when(hm, sizeof hm);
            snprintf(S.img_from, sizeof S.img_from, "%s%s", _("leída"), hm);
            ee_diff_count();
            S.vers_stale = true;
        }
        if (!J.from_portal) tell(J.ok, _("No se pudo leer"));
        break;
    case JOB_WRITE:
        if (J.ok) {
            S.valid = true;
            snprintf(S.img_chip, sizeof S.img_chip, "%s", ee_chip_at(J.conf.chip)->name);
            S.sums = J.sums;
            ee_dirty_clear();
            when(hm, sizeof hm);
            snprintf(S.img_from, sizeof S.img_from, "%s%s", J.mode == WR_ERASE ? _("borrada") : _("escrita"), hm);
            ee_diff_count();
        }
        S.vers_stale = true;
        if (!J.from_portal || !J.ok) tell(J.ok, _("No se pudo escribir"));
        break;
    case JOB_DETECT:
    case JOB_SIZE:
        snprintf(S.det, sizeof S.det, "%s", J.msg);
        S.sugg_chip = J.sugg_chip;
        S.sugg_addr = J.sugg_addr;
        S.sugg_org = J.sugg_org;
        if (J.have_sr) { S.have_sr = true; S.sr = J.sr; }
        break;
    case JOB_STATUS:
    case JOB_PROTECT:
        if (J.have_sr) { S.have_sr = true; S.sr = J.sr; }
        tell(J.ok, _("Protección"));
        break;
    }
    if (J.from_portal) ee_portal_done();
    if (S.tab == TAB_MEM) ee_pg_mem_refresh();
    else if (S.tab == TAB_CHIP) ee_pg_chip_job_done();
    else if (S.tab == TAB_VER && S.vers_stale) build_page();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (J.seq != s_seen_seq && !J.busy) {
        s_seen_seq = J.seq;
        job_done();
    }
    jobbar_refresh();
    if (J.busy != s_last_busy || S.ndirty != s_last_dirty) {
        s_last_busy = J.busy;
        s_last_dirty = S.ndirty;
        ee_portal_chip();
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (J.busy && J.from_portal && now - s_prog_ms >= 800) {
        s_prog_ms = now;
        ee_portal_progress();
    }
    if (now - s_poll_ms >= 1000) {
        s_poll_ms = now;
        ee_portal_poll();
    }
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static void *ee_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&U, 0, sizeof U);
    ee_conf_load(&S.conf);
    S.sugg_chip = S.sugg_addr = -1;
    S.vers_stale = true;
    U.root = root;
    s_seen_seq = J.seq;
    build_all();
    s_timer = lv_timer_create(timer_cb, 120, NULL);
    ee_portal_chip();
    return &S;
}

static bool ee_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    ee_pg_mem_forget();
    lv_obj_clean(root);
    ee_sheet_forget();
    U.root = root;
    U.jobbar = NULL;
    build_all();
    return true;
}

static void ee_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    ee_job_stop();
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    ee_pg_mem_forget();
    ee_sheet_forget();
    ee_conf_save(&S.conf);
    ee_portal_closed();
    if (J.busy) {
        /* a job that would not stop keeps its buffers: freeing them under it is worse */
        aos_hal_log("eeprom", "job still running at exit; its buffers stay");
    } else {
        ee_img_free();
        ee_ref_clear();
        free(S.vers);
    }
    memset(&S, 0, sizeof S);
    memset(&U, 0, sizeof U);
}

static bool ee_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (ee_sheet_open()) { ee_sheet_close(); return true; }
    if (S.tab == TAB_MEM && ee_pg_mem_back()) return true;
    return false;
}

static bool ee_init(aos_app_t *app)
{
    app->desc.id       = "aos.eeprom";      /* EE_ID: a literal, so tools/gen_lang.py finds it */
    app->desc.name     = "EEPROM";
    app->desc.icon     = AOS_SYM_CHIP;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x10B981;
    app->desc.color_b  = 0x047857;
    app->desc.order    = 515;
    app->desc.flags    = AOS_APP_FLAG_KEEP;
    aos_icon_set_ops(app, EE_ICON, sizeof EE_ICON);

    app->create  = ee_create;
    app->destroy = ee_destroy;
    app->back    = ee_back;
    app->resize  = ee_resize;
    return true;
}

AOS_APP_ENTRY(ee_init);
