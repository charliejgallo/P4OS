/*
 * P4OS - Grabador: the screens.
 *
 *   Cámara     the live picture (a few times a second), the state, the motion
 *              bar, armed or not, recording now by hand; the zones are chosen
 *              on the picture itself
 *   Grabaciones  the clips, newest first, by day: play (in the Video app),
 *              delete
 *   Ajustes    size, frames, quality, sensitivity, before and after, the
 *              hours, the space, RTSP and the bot's notes
 */
#include "gr.h"

#include <stdlib.h>

extern void lv_image_cache_drop(const void *src);

enum { SCR_CAM = 0, SCR_CLIPS, SCR_SET };

static struct {
    lv_obj_t *page, *img, *state, *sub, *meter, *meter_fill, *rec, *zones, *arm_lbl, *rec_lbl;
    lv_obj_t *tabs[3];
    int       scr;
    bool      editing_zones;
    uint16_t *px;                   /* the picture shown, GR_PREV_W x GR_PREV_H */
    lv_image_dsc_t dsc;
    uint32_t  seq;
    int       pw, ph;
    int32_t   W, H;
    uint32_t  clip_sig;
} U;

static gr_clip_t s_clips[GR_CLIPS_MAX];
static int s_nclips;

#define C_REC lv_color_hex(0xFF453A)

static void build_page(void);

/* ---------------------------------------------------------------- pieces */

static lv_obj_t *box(lv_obj_t *p, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *p)
{
    lv_obj_t *c = box(p, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

static lv_obj_t *row(lv_obj_t *p)
{
    lv_obj_t *r = box(p, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_style_pad_row(r, 12, 0);
    return r;
}

static lv_obj_t *pill(lv_obj_t *p, const char *t, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(p, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = aos_label(b, t, aos_font_body, AOS_C_TEXT);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *text(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = aos_label(p, t, f, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static void save_cfg(void)
{
    gr_cfg_save(&gr_app.cfg);
    gr_engine_cfg(&gr_app.cfg);
    /* armed, it starts with the board: a camera that watches after a power cut */
    aos_ui_boot_start_set(GR_APP_ID, gr_app.cfg.armed);
}

/* ---------------------------------------------------------------- Cámara */

static void arm_cb(lv_event_t *e)
{
    gr_app.cfg.armed = !gr_app.cfg.armed;
    save_cfg();
    build_page();
}

static void rec_cb(lv_event_t *e)
{
    gr_status_t st;
    gr_engine_status(&st);
    gr_engine_record(!st.manual);
}

static void zones_cb(lv_event_t *e)
{
    U.editing_zones = !U.editing_zones;
    build_page();
}

static void zone_cb(lv_event_t *e)
{
    int z = (int)(intptr_t)lv_event_get_user_data(e);
    gr_app.cfg.zones ^= (uint16_t)(1u << z);
    if (!gr_app.cfg.zones) gr_app.cfg.zones = (uint16_t)(1u << z);     /* at least one */
    save_cfg();
    lv_obj_set_style_bg_opa(lv_event_get_target(e), (gr_app.cfg.zones & (1u << z)) ? LV_OPA_TRANSP : LV_OPA_70, 0);
}

static void zones_draw(lv_obj_t *parent, int32_t w, int32_t h)
{
    U.zones = box(parent, w, h);
    lv_obj_set_pos(U.zones, 0, 0);
    if (!U.editing_zones) {
        /* the zones off, shaded */
        for (int z = 0; z < GR_ZONES_W * GR_ZONES_H; z++) {
            if (gr_app.cfg.zones & (1u << z)) continue;
            lv_obj_t *c = box(U.zones, w / GR_ZONES_W, h / GR_ZONES_H);
            lv_obj_set_pos(c, (z % GR_ZONES_W) * w / GR_ZONES_W, (z / GR_ZONES_W) * h / GR_ZONES_H);
            lv_obj_set_style_bg_color(c, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_50, 0);
        }
        return;
    }
    for (int z = 0; z < GR_ZONES_W * GR_ZONES_H; z++) {
        lv_obj_t *c = box(U.zones, w / GR_ZONES_W, h / GR_ZONES_H);
        lv_obj_set_pos(c, (z % GR_ZONES_W) * w / GR_ZONES_W, (z / GR_ZONES_W) * h / GR_ZONES_H);
        lv_obj_set_style_bg_color(c, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(c, (gr_app.cfg.zones & (1u << z)) ? LV_OPA_TRANSP : LV_OPA_70, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0xFFD60A), 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, zone_cb, LV_EVENT_CLICKED, (void *)(intptr_t)z);
    }
}

static void scr_cam(lv_obj_t *s)
{
    int cw, ch;
    gr_res_size(gr_app.cfg.res, &cw, &ch);
    int32_t w = U.W - 2 * AOS_UI_PAD, h = w * ch / cw;
    lv_obj_t *frame = box(s, w, h);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x111114), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(frame, 18, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    U.img = lv_image_create(frame);
    lv_obj_set_size(U.img, w, h);
    lv_image_set_inner_align(U.img, LV_IMAGE_ALIGN_STRETCH);
    if (U.pw) {
        lv_image_set_src(U.img, &U.dsc);
    }
    zones_draw(frame, w, h);
    U.rec = aos_label(frame, "", aos_font_body, C_REC);
    lv_obj_align(U.rec, LV_ALIGN_TOP_LEFT, 16, 12);
    U.meter = box(frame, w - 32, 10);
    lv_obj_align(U.meter, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(U.meter, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(U.meter, LV_OPA_50, 0);
    lv_obj_set_style_radius(U.meter, 5, 0);
    U.meter_fill = box(U.meter, 0, 10);
    lv_obj_set_style_bg_color(U.meter_fill, lv_color_hex(0xFFD60A), 0);
    lv_obj_set_style_bg_opa(U.meter_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.meter_fill, 5, 0);

    lv_obj_t *c = card(s);
    U.state = text(c, "", aos_font_title, AOS_C_TEXT);
    U.sub = text(c, "", aos_font_small, AOS_C_DIM);
    lv_obj_t *r = row(c);
    lv_obj_t *a = pill(r, gr_app.cfg.armed ? _("Desarmar") : _("Armar"), gr_app.cfg.armed ? AOS_C_CARD2 : AOS_C_GREEN, arm_cb, NULL);
    U.arm_lbl = lv_obj_get_child(a, 0);
    lv_obj_t *b = pill(r, _("Grabar ahora"), C_REC, rec_cb, NULL);
    U.rec_lbl = lv_obj_get_child(b, 0);
    pill(r, U.editing_zones ? _("Listo") : _("Zonas"), AOS_C_CARD2, zones_cb, NULL);
    if (U.editing_zones)
        text(c, _("Tocá las partes de la imagen que no hay que mirar (una calle, un árbol que se mueve): se oscurecen."),
             aos_font_small, AOS_C_ORANGE);
}

static void cam_tick(void)
{
    if (!U.state) return;
    int w, h;
    if (U.px && gr_engine_preview(U.px, GR_PREV_W, GR_PREV_H, &w, &h, &U.seq)) {
        lv_image_dsc_t *d = &U.dsc;
        memset(d, 0, sizeof *d);
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = (uint32_t)w;
        d->header.h = (uint32_t)h;
        d->header.stride = (uint32_t)w * 2;
        d->data_size = (uint32_t)(w * h * 2);
        d->data = (const uint8_t *)U.px;
        U.pw = w;
        U.ph = h;
        lv_image_cache_drop(d);
        lv_image_set_src(U.img, d);
        lv_obj_invalidate(U.img);
    }
    gr_status_t st;
    gr_engine_status(&st);
    char t[160], s2[200];
    switch (st.state) {
    case GR_ST_NO_CAMERA: snprintf(t, sizeof t, "%s", _("Sin cámara")); break;
    case GR_ST_STARTING: snprintf(t, sizeof t, "%s", _("Arrancando la cámara…")); break;
    case GR_ST_ERROR: snprintf(t, sizeof t, "%s", _("Con problemas")); break;
    case GR_ST_RECORDING:
        snprintf(t, sizeof t, "%s", st.manual ? _("Grabando a mano") : _("Grabando: hay movimiento"));
        break;
    default:
        snprintf(t, sizeof t, "%s", gr_app.cfg.armed ? _("Vigilando") : _("Mirando (desarmado)"));
    }
    lv_label_set_text(U.state, t);
    if (st.why[0]) snprintf(s2, sizeof s2, "%s", st.why);
    else {
        int n = snprintf(s2, sizeof s2, _("%s · %dx%d · %.1f cuadros/s · %u kbit/s"), st.camera, st.w, st.h, (double)st.fps,
                         (unsigned)st.kbps);
        if (gr_app.cfg.rtsp && n < (int)sizeof s2)
            n += snprintf(s2 + n, sizeof s2 - (size_t)n, _("\nRTSP: rtsp://%s.local:%d/ · %d mirando"), aos_hal_device_name(),
                          gr_app.cfg.rtsp_port, st.viewers);
        if (st.clips_today && n < (int)sizeof s2)
            snprintf(s2 + n, sizeof s2 - (size_t)n, _("\n%u grabaciones desde que se abrió"), (unsigned)st.clips_today);
    }
    lv_label_set_text(U.sub, s2);
    if (st.state == GR_ST_RECORDING)
        lv_label_set_text_fmt(U.rec, "● %u:%02u", (unsigned)(st.rec_ms / 60000), (unsigned)(st.rec_ms / 1000 % 60));
    else
        lv_label_set_text(U.rec, "");
    lv_label_set_text(U.rec_lbl, st.manual ? _("Detener") : _("Grabar ahora"));
    int32_t mw = lv_obj_get_width(U.meter);
    float lvl = st.level * 8.0f;        /* a few percent of the picture is a lot */
    if (lvl > 1) lvl = 1;
    lv_obj_set_width(U.meter_fill, (int32_t)(mw * lvl));
    lv_obj_set_style_bg_color(U.meter_fill, st.moving ? C_REC : lv_color_hex(0xFFD60A), 0);
}

/* ---------------------------------------------------------------- Grabaciones */

static void play_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nclips) return;
    char dir[96], path[160];
    if (!gr_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/%.31s", dir, s_clips[i].name);
    if (!aos_ui_open_app_with("aos.video", path)) aos_ui_toast(_("Hace falta la app Video (Tienda)"), 2500);
}

static void del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nclips) return;
    gr_clip_delete(s_clips[i].name);
    build_page();
}

static void scr_clips(lv_obj_t *s)
{
    s_nclips = gr_clips(s_clips, GR_CLIPS_MAX);
    if (!s_nclips) {
        lv_obj_t *c = card(s);
        text(c, _("Todavía no hay grabaciones. Armá la cámara: graba cuando algo se mueve."), aos_font_body, AOS_C_DIM);
        return;
    }
    char day[32] = "";
    uint64_t total = 0;
    for (int i = 0; i < s_nclips; i++) total += s_clips[i].bytes;
    char t[96];
    snprintf(t, sizeof t, _("%d grabaciones · %u MB de %d"), s_nclips, (unsigned)(total >> 20), gr_app.cfg.space_mb);
    text(s, t, aos_font_small, AOS_C_DIM);
    lv_obj_t *c = NULL;
    for (int i = 0; i < s_nclips && i < 80; i++) {
        const char *n = s_clips[i].name;
        char d[32];
        snprintf(d, sizeof d, "%.2s/%.2s/%.4s", n + 6, n + 4, n);
        if (strcmp(d, day)) {
            snprintf(day, sizeof day, "%s", d);
            text(s, day, aos_font_small, AOS_C_DIM);
            c = card(s);
        }
        lv_obj_t *r = row(c);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        char hm[48];
        snprintf(hm, sizeof hm, "%.2s:%.2s:%.2s · %.1f MB", n + 9, n + 11, n + 13, s_clips[i].bytes / 1048576.0);
        lv_obj_t *l = aos_label(r, hm, aos_font_body, AOS_C_TEXT);
        lv_obj_set_flex_grow(l, 1);
        pill(r, _("Ver"), AOS_C_ACCENT, play_cb, (void *)(intptr_t)i);
        pill(r, _("Borrar"), AOS_C_CARD2, del_cb, (void *)(intptr_t)i);
    }
}

/* ---------------------------------------------------------------- Ajustes */

typedef struct { const char *label; int *val; const int *vals; const char *const *names; int n; } opt_t;

static const int RES_V[] = { 0, 1 };
static const char *const RES_N[] = { "640 x 480", "1280 x 720" };
static const int FPS_V[] = { 5, 10, 15 };
static const char *const FPS_N[] = { "5", "10", "15" };
static const int KBPS_V[] = { 800, 1500, 3000, 5000 };
static const char *const KBPS_N[] = { N_("Baja (0,8 Mbit/s)"), N_("Media (1,5 Mbit/s)"), N_("Alta (3 Mbit/s)"), N_("Muy alta (5 Mbit/s)") };
static const int SENS_V[] = { 0, 1, 2 };
static const char *const SENS_N[] = { N_("Baja"), N_("Media"), N_("Alta") };
static const int PRE_V[] = { 0, 2, 3, 5 };
static const char *const PRE_N[] = { N_("Nada"), N_("2 segundos"), N_("3 segundos"), N_("5 segundos") };
static const int POST_V[] = { 5, 10, 20, 30, 60 };
static const char *const POST_N[] = { N_("5 segundos"), N_("10 segundos"), N_("20 segundos"), N_("30 segundos"), N_("1 minuto") };
static const int CLIP_V[] = { 60, 300, 600 };
static const char *const CLIP_N[] = { N_("1 minuto"), N_("5 minutos"), N_("10 minutos") };
static const int SPACE_V[] = { 512, 2048, 8192, 32768 };
static const char *const SPACE_N[] = { "512 MB", "2 GB", "8 GB", "32 GB" };

static opt_t s_opts[9];

static void opt_cb(lv_event_t *e)
{
    opt_t *o = lv_event_get_user_data(e);
    int i = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    if (i >= 0 && i < o->n) *o->val = o->vals[i];
    save_cfg();
}

static void option(lv_obj_t *c, opt_t *o)
{
    aos_label(c, aos_tr(o->label), aos_font_small, AOS_C_DIM);
    char opts[256] = "";
    int sel = 0;
    for (int i = 0; i < o->n; i++) {
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof opts - l, "%s%s", i ? "\n" : "", aos_tr(o->names[i]));
        if (o->vals[i] == *o->val) sel = i;
    }
    lv_obj_t *d = lv_dropdown_create(c);
    lv_dropdown_set_options(d, opts);
    lv_dropdown_set_selected(d, (uint32_t)sel);
    lv_obj_set_size(d, LV_PCT(100), 76);
    lv_obj_set_style_text_font(d, aos_font_body, 0);
    lv_obj_set_style_bg_color(d, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(d, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 18, 0);
    lv_obj_t *list = lv_dropdown_get_list(d);
    lv_obj_set_style_text_font(list, aos_font_body, 0);
    lv_obj_set_style_bg_color(list, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(list, AOS_C_TEXT, 0);
    lv_obj_add_event_cb(d, opt_cb, LV_EVENT_VALUE_CHANGED, o);
}

static void sw_cb(lv_event_t *e)
{
    bool *b = lv_event_get_user_data(e);
    *b = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    save_cfg();
    build_page();
}

static void toggle(lv_obj_t *c, const char *label, bool *val)
{
    lv_obj_t *r = row(c);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = aos_label(r, label, aos_font_body, AOS_C_TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 100, 56);
    lv_obj_set_state(sw, LV_STATE_CHECKED, *val);
    lv_obj_add_event_cb(sw, sw_cb, LV_EVENT_VALUE_CHANGED, val);
}

static void hours_cb(lv_event_t *e)
{
    int *v = lv_event_get_user_data(e);
    *v = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    save_cfg();
}

static void scr_set(lv_obj_t *s)
{
    gr_cfg_t *c = &gr_app.cfg;
    opt_t o[] = {
        { N_("Tamaño"), &c->res, RES_V, RES_N, 2 },
        { N_("Cuadros por segundo"), &c->fps, FPS_V, FPS_N, 3 },
        { N_("Calidad"), &c->kbps, KBPS_V, KBPS_N, 4 },
        { N_("Sensibilidad al movimiento"), &c->sens, SENS_V, SENS_N, 3 },
        { N_("Grabar desde antes del movimiento"), &c->pre_s, PRE_V, PRE_N, 4 },
        { N_("Seguir grabando después"), &c->post_s, POST_V, POST_N, 5 },
        { N_("Cortar las grabaciones cada"), &c->clip_max_s, CLIP_V, CLIP_N, 3 },
        { N_("Espacio para grabaciones"), &c->space_mb, SPACE_V, SPACE_N, 4 },
    };
    memcpy(s_opts, o, sizeof o);
    lv_obj_t *cd = card(s);
    for (int i = 0; i < 4; i++) option(cd, &s_opts[i]);
    text(cd, _("A 1280 x 720 la app Video la reproduce más lento que en vivo (la placa decodifica H.264 por software)."),
         aos_font_caption, AOS_C_DIM);
    cd = card(s);
    for (int i = 4; i < 8; i++) option(cd, &s_opts[i]);
    cd = card(s);
    toggle(cd, _("Sólo a ciertas horas"), &c->sched);
    if (c->sched) {
        char hours[24 * 3 + 1] = "";
        for (int hh = 0; hh < 24; hh++) snprintf(hours + strlen(hours), sizeof hours - strlen(hours), "%s%02d", hh ? "\n" : "", hh);
        lv_obj_t *r = row(cd);
        aos_label(r, _("de"), aos_font_body, AOS_C_DIM);
        lv_obj_t *d1 = lv_dropdown_create(r);
        lv_dropdown_set_options(d1, hours);
        lv_dropdown_set_selected(d1, (uint32_t)c->from_h);
        lv_obj_set_width(d1, 140);
        lv_obj_add_event_cb(d1, hours_cb, LV_EVENT_VALUE_CHANGED, &c->from_h);
        aos_label(r, _("a"), aos_font_body, AOS_C_DIM);
        lv_obj_t *d2 = lv_dropdown_create(r);
        lv_dropdown_set_options(d2, hours);
        lv_dropdown_set_selected(d2, (uint32_t)c->to_h);
        lv_obj_set_width(d2, 140);
        lv_obj_add_event_cb(d2, hours_cb, LV_EVENT_VALUE_CHANGED, &c->to_h);
    }
    toggle(cd, _("Avisar por el bot de Telegram"), &c->notify);
    toggle(cd, _("Cámara IP por RTSP (Frigate, VLC, Cámaras)"), &c->rtsp);
    if (c->rtsp) {
        char t[96];
        snprintf(t, sizeof t, "rtsp://%s.local:%d/", aos_hal_device_name(), c->rtsp_port);
        text(cd, t, aos_font_small, AOS_C_DIM);
    }
}

/* ---------------------------------------------------------------- the page */

static void tab_cb(lv_event_t *e)
{
    U.scr = (int)(intptr_t)lv_event_get_user_data(e);
    U.editing_zones = false;
    build_page();
}

static void build_page(void)
{
    if (!gr_app.root) return;
    if (U.page) lv_obj_delete(U.page);
    U.state = U.sub = U.img = U.rec = U.meter = U.meter_fill = U.zones = NULL;
    U.W = lv_obj_get_width(gr_app.root);
    U.H = lv_obj_get_height(gr_app.root);
    int32_t tab_h = 110;
    U.page = box(gr_app.root, U.W, U.H);
    lv_obj_t *s = box(U.page, U.W, U.H - tab_h);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_ver(s, 20, 0);
    lv_obj_set_style_pad_row(s, 14, 0);
    if (U.scr == SCR_CLIPS) scr_clips(s);
    else if (U.scr == SCR_SET) scr_set(s);
    else scr_cam(s);
    lv_obj_t *bar = box(U.page, U.W, tab_h);
    lv_obj_set_pos(bar, 0, U.H - tab_h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const NAMES[3] = { N_("Cámara"), N_("Grabaciones"), N_("Ajustes") };
    const char *const GLY[3] = { AOS_SYM_CCTV, AOS_SYM_VIDEO, AOS_SYM_COG };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *t = box(bar, U.W / 3, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_color_t col = i == U.scr ? AOS_C_ACCENT : AOS_C_DIM;
        lv_obj_align(aos_label(t, GLY[i], &aos_sym_44, col), LV_ALIGN_CENTER, 0, -16);
        lv_obj_align(aos_label(t, aos_tr(NAMES[i]), aos_font_tiny, col), LV_ALIGN_CENTER, 0, 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }
    if (U.scr == SCR_CAM) cam_tick();
}

void gr_ui_build(void)
{
    if (!U.px) U.px = malloc((size_t)GR_PREV_W * GR_PREV_H * 2);
    U.seq = 0;
    U.page = NULL;
    build_page();
}

void gr_ui_tick(void)
{
    if (U.scr == SCR_CAM) cam_tick();
    else if (U.scr == SCR_CLIPS) {
        /* a clip closed while looking: the list again */
        gr_status_t st;
        gr_engine_status(&st);
        uint32_t sig = st.clips_today * 2 + (st.state == GR_ST_RECORDING);
        if (sig != U.clip_sig) { U.clip_sig = sig; build_page(); }
    }
}

bool gr_ui_back(void)
{
    if (U.editing_zones) { U.editing_zones = false; build_page(); return true; }
    if (U.scr != SCR_CAM) { U.scr = SCR_CAM; build_page(); return true; }
    return false;
}

void gr_ui_free(void)
{
    if (U.img) lv_image_set_src(U.img, NULL);
    free(U.px);
    memset(&U, 0, sizeof U);
}
