/*
 * P4OS - Vision: the app. A USB webcam with on-board vision: faces, people,
 * the 80 COCO objects, hands and their gestures (esp-dl on the P4's SIMD,
 * engine/), and QR and bar codes; the boxes drawn over the live picture,
 * counted, with a sound and a note when something appears, a photo with the
 * boxes in it, and the same picture and findings on the portal's page.
 *
 * The screen: the picture on top (on the left lying down), and under it
 * (beside it) the count, the speed, the five modes, what is in the picture
 * now, and the photo, alert and test-picture buttons with the threshold.
 * vision.h says who does what; this file is LVGL's side only.
 */
#include "vision.h"

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An eye with a frame's corners around it: a camera that looks. */
static const uint8_t VISION_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_TOP_LEFT,      14, 14, 20, 5, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_TOP_LEFT,      14, 14, 5, 20, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_TOP_RIGHT,    -14, 14, 20, 5, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_TOP_RIGHT,    -14, 14, 5, 20, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_BOTTOM_LEFT,   14, -14, 20, 5, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_BOTTOM_LEFT,   14, -14, 5, 20, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_BOTTOM_RIGHT, -14, -14, 20, 5, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_BOTTOM_RIGHT, -14, -14, 5, 20, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 0, 0, 52, 30, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 0, 0, 24, 24, AIC_CIRCLE, AIC_C_BG, 255),
    AIC_RECT(AIC_CENTER, 0, 0, 12, 12, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_END
};

#define TICK_MS      33
#define LIST_MAX     6
#define ALERT_GAP_MS 3000
#define CODE_KEEP    6

typedef struct {
    lv_obj_t *root;
    lv_obj_t *stage, *canvas, *overlay, *msg, *msg2;
    lv_obj_t *box[VI_MAX_BOXES], *box_lbl[VI_MAX_BOXES];
    lv_obj_t *count, *speed, *src_lbl;
    lv_obj_t *mode_btn[VI_M_COUNT];
    lv_obj_t *list;
    lv_obj_t *row[LIST_MAX], *row_l[LIST_MAX], *row_r[LIST_MAX];
    lv_obj_t *btn_alert, *btn_test, *alert_sym, *test_sym;
    lv_obj_t *thr_row, *slider, *slider_val;
    lv_timer_t *timer;
    int stage_w, stage_h;
    int img_w, img_h;          /* the frame on the canvas now */
    int scale;                 /* 256 = 1:1 */
    uint32_t seq_shown;
    uint32_t photo_seq;
    uint32_t last_ui_ms;
    /* the alert: something appears after a while of nothing */
    bool alert;
    int prev_count;
    uint32_t empty_since;
    /* the codes read lately, newest first */
    vi_code_t codes[CODE_KEEP];
    int ncodes;
    float thr_shown;
} vi_ui_t;

static vi_ui_t *U;

/* ---- preferences ------------------------------------------------------------- */

static int pref_int(const char *key, int def)
{
    char b[16];
    return aos_hal_pref_get_str(key, b, sizeof b) ? atoi(b) : def;
}

static void pref_set_int(const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    aos_hal_pref_set_str(key, b);
}

/* "12,5": a decimal comma, as the interface writes numbers */
static void fmt1(char *out, size_t n, float v)
{
    int t = (int)(v * 10.f + 0.5f);
    snprintf(out, n, "%d,%d", t / 10, t % 10);
}

/* ---- the picture and its boxes ------------------------------------------------ */

static void place_image(int w, int h)
{
    if (w == U->img_w && h == U->img_h) return;
    U->img_w = w;
    U->img_h = h;
    int sx = U->stage_w * 256 / w, sy = U->stage_h * 256 / h;
    U->scale = sx < sy ? sx : sy;
    lv_image_set_pivot(U->canvas, w / 2, h / 2);
    lv_image_set_scale(U->canvas, U->scale == 256 ? LV_SCALE_NONE : U->scale);
    lv_obj_center(U->canvas);
    lv_obj_set_size(U->overlay, w * U->scale / 256, h * U->scale / 256);
    lv_obj_center(U->overlay);
}

static void show_box(int i, int x0, int y0, int x1, int y1, lv_color_t c, const char *text)
{
    lv_obj_t *b = U->box[i];
    int s = U->scale;
    lv_obj_set_pos(b, x0 * s / 256, y0 * s / 256);
    lv_obj_set_size(b, (x1 - x0 + 1) * s / 256, (y1 - y0 + 1) * s / 256);
    lv_obj_set_style_border_color(b, c, 0);
    lv_obj_set_style_bg_color(U->box_lbl[i], c, 0);
    lv_label_set_text(U->box_lbl[i], text);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
}

static void box_text(const vi_frame_t *f, const ve_box_t *b, char *out, size_t n)
{
    int pct = (int)(b->score * 100.f + 0.5f);
    switch (f->mode) {
    case VI_M_COCO:
        snprintf(out, n, "%s %d%%", vi_coco_name(b->cls), pct);
        break;
    case VI_M_HAND:
        snprintf(out, n, "%s", vi_gesture_name(b->cls));
        break;
    default:
        snprintf(out, n, "%d%%", pct);
        break;
    }
}

static const char *code_kind(vi_code_kind_t k)
{
    switch (k) {
    case VI_CODE_QR: return "QR";
    case VI_CODE_EAN13: return "EAN-13";
    case VI_CODE_EAN8: return "EAN-8";
    case VI_CODE_UPCA: return "UPC-A";
    }
    return "";
}

static void draw_frame(const vi_frame_t *f)
{
    lv_canvas_set_buffer(U->canvas, f->px, f->w, f->h, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(U->canvas, LV_OBJ_FLAG_HIDDEN);
    place_image(f->w, f->h);
    lv_obj_invalidate(U->canvas);
    int n = 0;
    char t[64];
    for (int i = 0; i < f->nbox && n < VI_MAX_BOXES; i++, n++) {
        const ve_box_t *b = &f->box[i];
        box_text(f, b, t, sizeof t);
        show_box(n, b->x0, b->y0, b->x1, b->y1, vi_class_color(f->mode, b->cls), t);
    }
    for (int i = 0; i < f->ncode && n < VI_MAX_BOXES; i++, n++) {
        const vi_code_t *c = &f->code[i];
        show_box(n, c->x0, c->y0, c->x1, c->y1, vi_class_color(VI_M_CODES, 0), code_kind(c->kind));
    }
    for (; n < VI_MAX_BOXES; n++) lv_obj_add_flag(U->box[n], LV_OBJ_FLAG_HIDDEN);
}

/* ---- the list under the picture ---------------------------------------------- */

static void set_row(int i, const char *left, const char *right, lv_color_t dot)
{
    if (i >= LIST_MAX) return;
    lv_label_set_text(U->row_l[i], left);
    lv_label_set_text(U->row_r[i], right);
    lv_obj_set_style_border_color(U->row[i], dot, 0);
    lv_obj_remove_flag(U->row[i], LV_OBJ_FLAG_HIDDEN);
}

static void fill_list(const vi_frame_t *f)
{
    int n = 0;
    char l[VI_CODE_LEN + 8], r[24];
    if (f->mode == VI_M_CODES) {
        for (int i = 0; i < U->ncodes && n < LIST_MAX; i++, n++)
            set_row(n, U->codes[i].text, code_kind(U->codes[i].kind), vi_class_color(VI_M_CODES, 0));
    } else if (f->mode == VI_M_COCO) {
        /* one row per class, with how many */
        int seen[80] = {0};
        for (int i = 0; i < f->nbox; i++)
            if (f->box[i].cls >= 0 && f->box[i].cls < 80) seen[f->box[i].cls]++;
        for (int c = 0; c < 80 && n < LIST_MAX; c++) {
            if (!seen[c]) continue;
            snprintf(l, sizeof l, "%s", vi_coco_name(c));
            snprintf(r, sizeof r, "%d", seen[c]);
            set_row(n++, l, r, vi_class_color(VI_M_COCO, c));
        }
    } else {
        for (int i = 0; i < f->nbox && n < LIST_MAX; i++, n++) {
            const ve_box_t *b = &f->box[i];
            if (f->mode == VI_M_HAND)
                snprintf(l, sizeof l, "%s", vi_gesture_name(b->cls));
            else
                snprintf(l, sizeof l, "%s %d", vi_mode_noun(f->mode, 1), i + 1);
            snprintf(r, sizeof r, "%d%%", (int)(b->score * 100.f + 0.5f));
            set_row(n, l, r, vi_class_color(f->mode, b->cls));
        }
    }
    for (; n < LIST_MAX; n++) lv_obj_add_flag(U->row[n], LV_OBJ_FLAG_HIDDEN);
}

static void row_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (vi.mode != VI_M_CODES || i >= U->ncodes) return;
    aos_ui_clip_set(U->codes[i].text);
    aos_ui_toast(_("Copiado"), 1200);
}

/* a code read now goes to the top of the list (once) */
static void remember_codes(const vi_frame_t *f)
{
    for (int i = 0; i < f->ncode; i++) {
        const vi_code_t *c = &f->code[i];
        int k;
        for (k = 0; k < U->ncodes; k++)
            if (U->codes[k].kind == c->kind && strcmp(U->codes[k].text, c->text) == 0) break;
        if (k == 0 && U->ncodes) continue;
        bool is_new = k == U->ncodes;
        if (k == U->ncodes && U->ncodes < CODE_KEEP) U->ncodes++;
        if (k >= CODE_KEEP) k = CODE_KEEP - 1;
        memmove(&U->codes[1], &U->codes[0], sizeof(vi_code_t) * (size_t)k);
        U->codes[0] = *c;
        if (is_new && U->alert) aos_hal_beep(2200, 60);
    }
}

/* ---- the status ----------------------------------------------------------------- */

static void show_msg(const char *big, const char *small)
{
    if (!big) {
        lv_obj_add_flag(U->msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U->msg2, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(U->msg, big);
    lv_label_set_text(U->msg2, small ? small : "");
    lv_obj_remove_flag(U->msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(U->msg2, LV_OBJ_FLAG_HIDDEN);
}

static void update_status(const vi_frame_t *f)
{
    aos_hal_mutex_lock(vi.mx);
    vi_state_t st = vi.state;
    vi_src_kind_t src = vi.src;
    float fps = vi.fps, thr = vi.thr;
    char src_name[48];
    snprintf(src_name, sizeof src_name, "%s", vi.src_name);
    char err[96];
    snprintf(err, sizeof err, "%s", vi.err);
    aos_hal_mutex_unlock(vi.mx);

    char a[64], b[96], fs[16], ms[16];
    switch (st) {
    case VI_ST_NO_SOURCE:
        show_msg(_("Enchufá una cámara USB"),
                 _("O poné fotos .jpg o un video .mjpeg en la carpeta vision de la tarjeta."));
        break;
    case VI_ST_WAITING:
        show_msg(_("Esperando la imagen de la cámara"), NULL);
        break;
    case VI_ST_LOADING:
        show_msg(_("Cargando el modelo"), NULL);
        break;
    case VI_ST_NO_PACK:
        show_msg(_("Falta el paquete de este modo"),
                 vi.mode == VI_M_FACE || vi.mode == VI_M_PERSON
                     ? _("Instalá la app de nuevo desde la Tienda.")
                     : _("Es un extra: bajalo desde la página de Visión en la Tienda."));
        break;
    case VI_ST_NO_ENGINE:
        show_msg(_("Falta el motor de visión"), _("Instalá la app de nuevo desde la Tienda."));
        break;
    case VI_ST_MODEL_FAILED:
        show_msg(_("El modelo no cargó"), err);
        break;
    default:
        show_msg(NULL, NULL);
        break;
    }

    vi_mode_t m = f ? f->mode : vi.mode;
    int n = f ? (m == VI_M_CODES ? f->ncode : f->nbox) : 0;
    if (st == VI_ST_RUNNING && f) {
        snprintf(a, sizeof a, "%d %s", n, vi_mode_noun(m, n));
        fmt1(fs, sizeof fs, fps);
        fmt1(ms, sizeof ms, (m == VI_M_CODES ? f->run_us : f->t.model_us) / 1000.f);
        snprintf(b, sizeof b, "%s fps  ·  %s ms", fs, ms);
    } else {
        snprintf(a, sizeof a, "%s", vi_mode_name(m));
        b[0] = 0;
    }
    lv_label_set_text(U->count, a);
    lv_label_set_text(U->speed, b);
    lv_label_set_text(U->src_lbl, src == VI_SRC_CAMERA ? src_name : src == VI_SRC_TEST ? _("Fotos de prueba") : "");

    if (thr != U->thr_shown && thr > 0) {
        U->thr_shown = thr;
        lv_slider_set_value(U->slider, (int)(thr * 100.f + 0.5f), LV_ANIM_OFF);
        snprintf(a, sizeof a, "%d%%", (int)(thr * 100.f + 0.5f));
        lv_label_set_text(U->slider_val, a);
    }
    /* the codes have no threshold */
    if (m != VI_M_CODES) lv_obj_remove_flag(U->thr_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U->thr_row, LV_OBJ_FLAG_HIDDEN);
}

/* something appeared after a while of nothing: a sound and a note */
static void check_alert(const vi_frame_t *f)
{
    int n = f->mode == VI_M_CODES ? f->ncode : f->nbox;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (n == 0) {
        if (U->prev_count) U->empty_since = now;
    } else if (U->prev_count == 0 && U->alert && f->mode != VI_M_CODES && now - U->empty_since > ALERT_GAP_MS) {
        char t[64];
        if (f->mode == VI_M_COCO)
            snprintf(t, sizeof t, "%s: %s", _("Apareció"), vi_coco_name(f->box[0].cls));
        else
            snprintf(t, sizeof t, "%s: %d %s", _("Apareció"), n, vi_mode_noun(f->mode, n));
        aos_hal_beep(1800, 90);
        aos_ui_toast(t, 1500);
    }
    U->prev_count = n;
}

static void apply_portal(void)
{
    char msg[AOS_LIVE_MSG_MAX];
    int n;
    while ((n = aos_hal_live_take(VI_ID, msg, sizeof msg - 1)) > 0) {
        msg[n] = 0;
        if (strncmp(msg, "mode=", 5) == 0) {
            int m = atoi(msg + 5);
            if (m >= 0 && m < VI_M_COUNT) {
                vi.want = (vi_mode_t)m;
                pref_set_int("vi_mode", m);
                for (int i = 0; i < VI_M_COUNT; i++)
                    if (i == m) lv_obj_add_state(U->mode_btn[i], LV_STATE_CHECKED);
                    else lv_obj_remove_state(U->mode_btn[i], LV_STATE_CHECKED);
                U->ncodes = 0;
            }
        } else if (strcmp(msg, "photo") == 0) {
            vi.want_photo = true;
        } else if (strncmp(msg, "thr=", 4) == 0) {
            float t = (float)atoi(msg + 4) / 100.f;
            if (t > 0.04f && t < 0.96f) vi.want_thr = t;
        } else if (strncmp(msg, "test=", 5) == 0) {
            vi.want_test = msg[5] == '1';
        }
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!U) return;
    apply_portal();
    vi_frame_t *f = NULL;
    aos_hal_mutex_lock(vi.mx);
    if (vi.ready >= 0) {
        vi.shown = vi.ready;
        vi.ready = -1;
    }
    if (vi.shown >= 0) f = &vi.fr[vi.shown];
    uint32_t photo_seq = vi.photo_seq;
    bool photo_ok = vi.photo_ok;
    aos_hal_mutex_unlock(vi.mx);

    bool fresh = f && f->seq != U->seq_shown;
    if (fresh) {
        U->seq_shown = f->seq;
        draw_frame(f);
        if (f->mode == VI_M_CODES) remember_codes(f);
        check_alert(f);
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - U->last_ui_ms >= 250 || fresh) {
        if (now - U->last_ui_ms >= 250) {
            U->last_ui_ms = now;
            update_status(f && f->mode == vi.mode ? f : NULL);
            if (f && f->mode == vi.mode) fill_list(f);
        }
    }
    if (photo_seq != U->photo_seq) {
        U->photo_seq = photo_seq;
        aos_ui_toast(photo_ok ? _("Foto guardada en Fotos") : _("No se pudo guardar la foto"), 1500);
        if (photo_ok) aos_hal_beep(2600, 40);
    }
}

/* ---- buttons ------------------------------------------------------------------------ */

static void mode_clicked(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < VI_M_COUNT; i++)
        if (i == m) lv_obj_add_state(U->mode_btn[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(U->mode_btn[i], LV_STATE_CHECKED);
    if (vi.want != (vi_mode_t)m) {
        vi.want_thr = 0;
        U->thr_shown = 0;
        U->ncodes = 0;
        U->prev_count = 0;
        U->empty_since = (uint32_t)aos_hal_uptime_ms();
    }
    vi.want = (vi_mode_t)m;
    pref_set_int("vi_mode", m);
}

static void photo_clicked(lv_event_t *e)
{
    (void)e;
    vi.want_photo = true;
}

static void alert_clicked(lv_event_t *e)
{
    (void)e;
    U->alert = !U->alert;
    pref_set_int("vi_alert", U->alert);
    lv_label_set_text(U->alert_sym, U->alert ? AOS_SYM_BELL_RING : AOS_SYM_BELL_OFF);
    if (U->alert) lv_obj_add_state(U->btn_alert, LV_STATE_CHECKED);
    else lv_obj_remove_state(U->btn_alert, LV_STATE_CHECKED);
}

static void test_clicked(lv_event_t *e)
{
    (void)e;
    vi.want_test = !vi.want_test;
    pref_set_int("vi_test", vi.want_test);
    if (vi.want_test) lv_obj_add_state(U->btn_test, LV_STATE_CHECKED);
    else lv_obj_remove_state(U->btn_test, LV_STATE_CHECKED);
}

static void slider_changed(lv_event_t *e)
{
    (void)e;
    int v = lv_slider_get_value(U->slider);
    char t[16];
    snprintf(t, sizeof t, "%d%%", v);
    lv_label_set_text(U->slider_val, t);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) vi.want_thr = v / 100.f;
}

/* ---- building the screen --------------------------------------------------------------- */

static lv_obj_t *box_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static bool s_narrow;   /* lying down: the panel's buttons are narrower */

static lv_obj_t *icon_button(lv_obj_t *parent, const char *sym, const char *text, lv_event_cb_t cb, lv_obj_t **sym_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 64);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, s_narrow ? 4 : 10, 0);
    lv_obj_set_style_pad_hor(b, s_narrow ? 4 : 12, 0);
    lv_obj_t *s = lv_label_create(b);
    lv_obj_set_style_text_font(s, &aos_sym_28, 0);
    lv_label_set_text(s, sym);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, s_narrow ? aos_font_caption : aos_font_small, 0);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (sym_out) *sym_out = s;
    return b;
}

static void build(lv_obj_t *root)
{
    int W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    bool land = W > H;
    s_narrow = land;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* the picture: 4:3, the whole width standing, the whole height lying */
    if (land) {
        U->stage_h = H;
        U->stage_w = H * 4 / 3;
    } else {
        U->stage_w = W;
        U->stage_h = W * 3 / 4;
    }
    U->stage = box_obj(root);
    lv_obj_set_size(U->stage, U->stage_w, U->stage_h);
    lv_obj_set_pos(U->stage, 0, 0);
    lv_obj_set_style_bg_color(U->stage, lv_color_hex(0x0A0A0C), 0);
    lv_obj_set_style_bg_opa(U->stage, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(U->stage, true, 0);

    U->canvas = lv_canvas_create(U->stage);
    lv_obj_add_flag(U->canvas, LV_OBJ_FLAG_HIDDEN);
    U->overlay = box_obj(U->stage);
    U->img_w = U->img_h = 0;
    U->scale = 256;
    for (int i = 0; i < VI_MAX_BOXES; i++) {
        lv_obj_t *b = box_obj(U->overlay);
        lv_obj_set_style_border_width(b, 4, 0);
        lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(l, 6, 0);
        lv_obj_set_style_pad_ver(l, 1, 0);
        lv_obj_set_style_radius(l, 4, 0);
        lv_obj_set_pos(l, -4, -4);
        U->box[i] = b;
        U->box_lbl[i] = l;
    }
    U->msg = lv_label_create(U->stage);
    lv_obj_set_style_text_font(U->msg, aos_font_body, 0);
    lv_obj_set_style_text_color(U->msg, AOS_C_TEXT, 0);
    lv_obj_set_width(U->msg, U->stage_w - 80);
    lv_obj_set_style_text_align(U->msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(U->msg, LV_ALIGN_CENTER, 0, -24);
    U->msg2 = lv_label_create(U->stage);
    lv_obj_set_style_text_font(U->msg2, aos_font_small, 0);
    lv_obj_set_style_text_color(U->msg2, AOS_C_DIM, 0);
    lv_obj_set_width(U->msg2, U->stage_w - 80);
    lv_obj_set_style_text_align(U->msg2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(U->msg2, LV_ALIGN_CENTER, 0, 36);
    lv_label_set_text(U->msg, "");
    lv_label_set_text(U->msg2, "");

    /* the panel */
    lv_obj_t *p = box_obj(root);
    if (land) {
        lv_obj_set_pos(p, U->stage_w, 0);
        lv_obj_set_size(p, W - U->stage_w, H);
    } else {
        lv_obj_set_pos(p, 0, U->stage_h);
        lv_obj_set_size(p, W, H - U->stage_h);
    }
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_pad_row(p, 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);

    /* the count and the speed */
    lv_obj_t *head = box_obj(p);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    U->count = lv_label_create(head);
    lv_obj_set_style_text_font(U->count, aos_font_title, 0);
    lv_obj_set_style_text_color(U->count, AOS_C_TEXT, 0);
    lv_label_set_text(U->count, "");
    U->speed = lv_label_create(head);
    lv_obj_set_style_text_font(U->speed, aos_font_small, 0);
    lv_obj_set_style_text_color(U->speed, AOS_C_ACCENT, 0);
    lv_label_set_text(U->speed, "");
    U->src_lbl = lv_label_create(head);
    lv_obj_set_style_text_font(U->src_lbl, aos_font_caption, 0);
    lv_obj_set_style_text_color(U->src_lbl, AOS_C_DIM, 0);
    lv_label_set_long_mode(U->src_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(U->src_lbl, "");
    if (land) {
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(U->src_lbl, W - U->stage_w - 40);
    } else {
        lv_obj_align(U->count, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_align(U->speed, LV_ALIGN_TOP_RIGHT, 0, 4);
        lv_obj_align(U->src_lbl, LV_ALIGN_TOP_RIGHT, 0, 36);
        lv_obj_set_width(U->src_lbl, 300);
        lv_obj_set_style_text_align(U->src_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* the five modes */
    lv_obj_t *modes = box_obj(p);
    lv_obj_set_size(modes, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(modes, land ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(modes, 8, 0);
    lv_obj_set_style_pad_row(modes, 8, 0);
    for (int m = 0; m < VI_M_COUNT; m++) {
        lv_obj_t *b = lv_button_create(modes);
        lv_obj_set_height(b, 56);
        if (land) lv_obj_set_width(b, (W - U->stage_w - 40 - 16) / 3);
        else lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 28, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_hor(b, 4, 0);
        lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
        lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_CHECKED);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
        lv_label_set_text(l, vi_mode_name((vi_mode_t)m));
        lv_obj_center(l);
        if (!vi_pack_present((vi_mode_t)m)) lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
        if (m == (int)vi.want) lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_obj_add_event_cb(b, mode_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)m);
        U->mode_btn[m] = b;
    }

    /* what is in the picture now (the codes: the last read, to copy) */
    U->list = box_obj(p);
    lv_obj_set_width(U->list, LV_PCT(100));
    lv_obj_set_flex_grow(U->list, 1);
    lv_obj_set_flex_flow(U->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U->list, 6, 0);
    for (int i = 0; i < LIST_MAX; i++) {
        lv_obj_t *r = box_obj(U->list);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(r, LV_PCT(100), 44);
        lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(r, 6, 0);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_t *l = lv_label_create(r);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(l, LV_PCT(78), lv_font_get_line_height(aos_font_small));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *rr = lv_label_create(r);
        lv_obj_set_style_text_font(rr, aos_font_caption, 0);
        lv_obj_set_style_text_color(rr, AOS_C_DIM, 0);
        lv_obj_align(rr, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U->row[i] = r;
        U->row_l[i] = l;
        U->row_r[i] = rr;
    }

    /* the threshold */
    lv_obj_t *thr = box_obj(p);
    U->thr_row = thr;
    lv_obj_set_size(thr, LV_PCT(100), 48);
    lv_obj_t *tl = lv_label_create(thr);
    lv_obj_set_style_text_font(tl, aos_font_small, 0);
    lv_obj_set_style_text_color(tl, AOS_C_DIM, 0);
    lv_label_set_text(tl, _("Umbral"));
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);
    U->slider = lv_slider_create(thr);
    lv_slider_set_range(U->slider, 5, 95);
    lv_obj_set_size(U->slider, land ? W - U->stage_w - 40 - 230 : W - 40 - 260, 12);
    lv_obj_align(U->slider, LV_ALIGN_LEFT_MID, 130, 0);
    lv_obj_add_event_cb(U->slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(U->slider, slider_changed, LV_EVENT_RELEASED, NULL);
    U->slider_val = lv_label_create(thr);
    lv_obj_set_style_text_font(U->slider_val, aos_font_small, 0);
    lv_obj_set_style_text_color(U->slider_val, AOS_C_TEXT, 0);
    lv_label_set_text(U->slider_val, "");
    lv_obj_align(U->slider_val, LV_ALIGN_RIGHT_MID, 0, 0);
    U->thr_shown = 0;

    /* photo, alert, test pictures */
    lv_obj_t *acts = box_obj(p);
    lv_obj_set_size(acts, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(acts, land ? 6 : 10, 0);
    icon_button(acts, AOS_SYM_CAMERA, _("Foto"), photo_clicked, NULL);
    U->btn_alert = icon_button(acts, U->alert ? AOS_SYM_BELL_RING : AOS_SYM_BELL_OFF, _("Avisar"), alert_clicked,
                               &U->alert_sym);
    if (U->alert) lv_obj_add_state(U->btn_alert, LV_STATE_CHECKED);
    U->btn_test = icon_button(acts, AOS_SYM_IMAGE_MULTIPLE, _("Prueba"), test_clicked, &U->test_sym);
    if (vi.want_test) lv_obj_add_state(U->btn_test, LV_STATE_CHECKED);

    U->seq_shown = 0;
    U->last_ui_ms = 0;
}

/* ---- the app ------------------------------------------------------------------------------ */

static void *vision_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    U = (vi_ui_t *)lv_malloc_zeroed(sizeof(vi_ui_t));
    if (!U) return NULL;
    U->root = root;
    memset(&vi, 0, sizeof vi);
    vi.shown = vi.ready = -1;
    vi.want = (vi_mode_t)pref_int("vi_mode", VI_M_PERSON);
    if (vi.want >= VI_M_COUNT) vi.want = VI_M_PERSON;
    vi.want_test = pref_int("vi_test", 0) != 0;
    U->alert = pref_int("vi_alert", 1) != 0;
    U->empty_since = (uint32_t)aos_hal_uptime_ms();
    vi.photo_seq = 0;
    U->photo_seq = 0;
    build(root);
    if (!vi_run_start()) aos_ui_toast(_("Sin memoria para la cámara"), 2000);
    U->timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    return U;
}

static void vision_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!U) return;
    if (U->timer) lv_timer_delete(U->timer);
    vi_run_stop();
    lv_free(U);
    U = NULL;
}

static void vision_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    /* no camera and no model while out of sight */
    if (!U) return;
    if (U->timer) lv_timer_pause(U->timer);
    lv_obj_add_flag(U->canvas, LV_OBJ_FLAG_HIDDEN);
    vi_run_stop();
}

static void vision_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!U || vi.running) return;
    vi.shown = vi.ready = -1;
    U->seq_shown = 0;
    U->img_w = U->img_h = 0;
    /* the canvas pointed into the frames freed by hide(): out of sight
     * until the first new one */
    lv_obj_add_flag(U->canvas, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VI_MAX_BOXES; i++) lv_obj_add_flag(U->box[i], LV_OBJ_FLAG_HIDDEN);
    if (!vi_run_start()) aos_ui_toast(_("Sin memoria para la cámara"), 2000);
    if (U->timer) lv_timer_resume(U->timer);
}

static bool vision_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    if (!U) return false;
    lv_obj_clean(root);
    build(root);
    return true;
}

static bool vision_init(aos_app_t *app)
{
    app->desc.id = "aos.vision";
    app->desc.name = "Visión";
    app->desc.icon = LV_SYMBOL_EYE_OPEN;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a = 0x30D158;
    app->desc.color_b = 0x0A6E3A;
    app->desc.order = 200;
    app->desc.flags = AOS_APP_FLAG_KEEP_AWAKE;
    aos_icon_set_ops(app, VISION_ICON, sizeof VISION_ICON);

    app->create = vision_create;
    app->destroy = vision_destroy;
    app->hide = vision_hide;
    app->show = vision_show;
    app->resize = vision_resize;
    return true;
}

AOS_APP_ENTRY(vision_init);
