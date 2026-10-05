/*
 * P4OS - PWM generator: the big knob.
 *
 * An endless knob, like an encoder: turning it moves the value by how much
 * the finger turned, never to where the finger is, so it can be grabbed
 * anywhere and turned in several strokes. A whole sweep of the arc (270
 * degrees) is the whole range of the value, a decade of frequency or a
 * decade of the pattern's period; with a second finger down anywhere on the
 * screen it goes ten times finer.
 *
 * The mouse wheel. A USB mouse's wheel (aos_hwmouse.c) does not reach an
 * app as an event: it scrolls whatever scrollable object is under the
 * arrow. So the knob sits in a "catcher": a container that can scroll (its
 * content is a tall invisible spacer) but not by touch (scroll_dir NONE),
 * with every visible part of the knob floating, so nothing moves when it
 * scrolls. The wheel scrolls it 90 px a notch; at the end of each scroll
 * the distance becomes notches, and the catcher goes back to its park.
 */
#include "pw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define K       (pw_g->knob)
#define SWEEP   270.0f          /* degrees of arc for a whole range */
#define NOTCH   90              /* px a wheel notch scrolls (aos_hwmouse.c) */
#define WHEEL   (1.0f / 250.0f) /* of a sweep, a notch */
#define PARK    20000

static float span_lo(void)
{
    switch (pw_g->target) {
    case PW_T_FREQ:  return 0.0f;
    case PW_T_SPEED: return -log10f(PW_PERIOD_MAX);
    default:         return 0.0f;
    }
}

static float span_hi(void)
{
    switch (pw_g->target) {
    case PW_T_FREQ:  return log10f((float)PW_FREQ_MAX);
    case PW_T_SPEED: return -log10f(PW_PERIOD_MIN);
    default:         return 1.0f;
    }
}

/* where the knob is, in its target's units: 0..1, log10 Hz, -log10 s */
static float knob_get(void)
{
    pw_ch_t *h = pw_cur();
    switch (pw_g->target) {
    case PW_T_FREQ:  return log10f((float)h->c.freq);
    case PW_T_SPEED: return -log10f(h->c.period);
    default:         return pw_val(&h->c);
    }
}

static void knob_put(float x, int nudge)
{
    pw_ch_t *h = pw_cur();
    switch (pw_g->target) {
    case PW_T_FREQ: {
        uint32_t f = pw_nice_freq(powf(10.0f, x));
        /* below 1 kHz a hundredth of a decade can round to the same hertz */
        if (nudge && f == h->c.freq) f = nudge > 0 ? f + 1 : f > 1 ? f - 1 : 1;
        if (f != h->c.freq) pw_set_freq(h, f);
        break;
    }
    case PW_T_SPEED: {
        float p = powf(10.0f, -x);
        p = p < PW_PERIOD_MIN ? PW_PERIOD_MIN : p > PW_PERIOD_MAX ? PW_PERIOD_MAX : p;
        /* keep the pattern where it is in its cycle */
        uint32_t now = lv_tick_get();
        float ph = fmodf((float)(uint32_t)(now - h->t0) / (h->c.period * 1000.0f), 1.0f);
        h->c.period = p;
        h->t0 = now - (uint32_t)(ph * p * 1000.0f);
        pw_store_changed();
        break;
    }
    default:
        if (h->c.pat != PW_P_NONE) return;          /* the pattern moves it */
        pw_val_set(&h->c, x);
        pw_touch(h);
        break;
    }
    pw_ui_refresh();
}

static void move_by(float d, int nudge)
{
    float x = knob_get() + d;
    float a = span_lo(), b = span_hi();
    knob_put(x < a ? a : x > b ? b : x, nudge);
}

void pw_knob_nudge(int dir)
{
    if (pw_cur()) move_by((float)dir * 0.01f, dir);
}

/* ---------------------------------------------------------------- touch */

static float finger_angle(lv_obj_t *o, float *r)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float dx = (float)p.x - (float)(a.x1 + a.x2) / 2.0f;
    float dy = (float)p.y - (float)(a.y1 + a.y2) / 2.0f;
    *r = sqrtf(dx * dx + dy * dy);
    return atan2f(dy, dx) * 57.29578f;              /* clockwise is positive: y grows down */
}

static void input_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *o = lv_event_get_target(e);
    pw_ch_t *h = pw_cur();
    if (!h) return;
    float r, ang = finger_angle(o, &r);
    float inner = (float)lv_obj_get_width(o) * 0.12f;

    if (code == LV_EVENT_PRESSED) {
        K.last_ang = ang;
        K.acc = knob_get();
        K.dragging = r > inner;
        if (pw_g->target == PW_T_VALUE && h->c.pat != PW_P_NONE)
            aos_ui_toast(_("Con un patrón andando, la perilla gira la velocidad"), 1500);
    } else if (code == LV_EVENT_PRESSING) {
        if (r <= inner) { K.dragging = false; return; }   /* the angle is noise near the centre */
        if (!K.dragging) { K.dragging = true; K.last_ang = ang; return; }
        float d = ang - K.last_ang;
        if (d > 180.0f) d -= 360.0f;
        if (d < -180.0f) d += 360.0f;
        K.last_ang = ang;
        aos_touch_point_t pts[2];
        bool fine = aos_touch_points(pts) >= 2;
        if (fine) lv_obj_remove_flag(K.fine, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(K.fine, LV_OBJ_FLAG_HIDDEN);
        float a = span_lo(), b = span_hi();
        K.acc += d / SWEEP * (fine ? 0.1f : 1.0f);
        K.acc = K.acc < a ? a : K.acc > b ? b : K.acc;
        knob_put(K.acc, 0);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        K.dragging = false;
        lv_obj_add_flag(K.fine, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------- the wheel */

static void wheel_cb(lv_event_t *e)
{
    (void)e;
    if (K.recentering || !pw_cur()) return;
    int32_t d = K.park - lv_obj_get_scroll_y(K.catcher);    /* > 0: the wheel went up */
    if (!d) return;
    K.recentering = true;
    lv_obj_scroll_to_y(K.catcher, K.park, LV_ANIM_OFF);
    K.recentering = false;
    move_by((float)d / (float)NOTCH * WHEEL, 0);
}

static void nudge_cb(lv_event_t *e)
{
    pw_knob_nudge((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---------------------------------------------------------------- building */

static lv_obj_t *round_btn(lv_obj_t *parent, const char *txt, int dir, int32_t d)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, AOS_C_DIM, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_t *l = aos_label(b, txt, aos_font_large, AOS_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, nudge_cb, LV_EVENT_PRESSED, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, nudge_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)dir);
    return b;
}

void pw_knob_build(lv_obj_t *parent, int32_t size)
{
    memset(&K, 0, sizeof K);
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, size, size);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_ELASTIC |
                          LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(c, wheel_cb, LV_EVENT_SCROLL_END, NULL);
    K.catcher = c;

    lv_obj_t *sp = lv_obj_create(c);                 /* what makes it scrollable */
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 1, size + 2 * PARK);
    lv_obj_set_pos(sp, 0, 0);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *arc = lv_arc_create(c);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_pos(arc, 0, 0);
    lv_arc_set_bg_angles(arc, 135, 45);
    lv_arc_set_range(arc, 0, 1000);
    int32_t w = size / 13;
    lv_obj_set_style_arc_width(arc, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(arc, w / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(arc, AOS_C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, w / 5, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(arc, 0, LV_PART_KNOB);
    K.arc = arc;

    /* the face: a disc inside the ring */
    lv_obj_t *face = lv_obj_create(arc);
    lv_obj_remove_style_all(face);
    int32_t fd = size - 4 * w;
    lv_obj_set_size(face, fd, fd);
    lv_obj_center(face);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE);

    bool big = size >= 400;
    K.what = aos_label(arc, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(K.what, LV_ALIGN_CENTER, 0, -size / 6);
    K.val = aos_label(arc, "", big ? aos_font_huge : aos_font_large, AOS_C_TEXT);
    lv_obj_align(K.val, LV_ALIGN_CENTER, 0, -size / 40);
    K.sub = aos_label(arc, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(K.sub, LV_ALIGN_CENTER, 0, size / 8);
    K.fine = aos_label(arc, _("fino ×0,1"), aos_font_tiny, AOS_C_ORANGE);
    lv_obj_align(K.fine, LV_ALIGN_CENTER, 0, size / 8 + 40);
    lv_obj_add_flag(K.fine, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *in = lv_obj_create(c);
    lv_obj_remove_style_all(in);
    lv_obj_set_size(in, size, size);
    lv_obj_set_pos(in, 0, 0);
    lv_obj_add_flag(in, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(in, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(in, input_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(in, input_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(in, input_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(in, input_cb, LV_EVENT_PRESS_LOST, NULL);
    K.input = in;

    /* - and + in the gap the arc leaves at the bottom */
    int32_t bd = size >= 400 ? 84 : 72;
    lv_obj_t *mi = round_btn(c, "−", -1, bd);
    lv_obj_set_pos(mi, size / 2 - bd - size / 9, size - bd - 4);
    lv_obj_t *pl = round_btn(c, "+", 1, bd);
    lv_obj_set_pos(pl, size / 2 + size / 9, size - bd - 4);
    pw_g->minus = mi;
    pw_g->plus = pl;

    lv_obj_update_layout(c);
    K.park = PARK;
    lv_obj_scroll_to_y(c, PARK, LV_ANIM_OFF);
}

void pw_knob_refresh(void)
{
    pw_ch_t *h = pw_cur();
    if (!h || !K.arc) return;
    const pw_cfg_t *c = &h->c;
    char big[40], sub[64];
    float pos;
    const char *what;
    switch (pw_g->target) {
    case PW_T_FREQ: {
        float lf = log10f((float)c->freq);
        pos = lf - floorf(lf);                       /* within its decade */
        pw_fmt_freq(big, sizeof big, (float)c->freq);
        if (h->pwm) {
            char f[24];
            pw_fmt_freq(f, sizeof f, (float)aos_io_pwm_freq(h->pwm));
            snprintf(sub, sizeof sub, _("real %s · %d bits"), f, aos_io_pwm_bits(h->pwm));
        } else {
            snprintf(sub, sizeof sub, "%s", _("al encender"));
        }
        what = _("FRECUENCIA");
        break;
    }
    case PW_T_SPEED: {
        float x = -log10f(c->period);
        pos = (x - span_lo()) / (span_hi() - span_lo());
        pw_fmt_time(big, sizeof big, c->period);
        char n[16];
        pw_fnum(n, sizeof n, 1.0f / c->period, 2);
        snprintf(sub, sizeof sub, "%s · %s Hz", pw_pat_name(c->pat), n);
        what = _("PERÍODO");
        break;
    }
    default: {
        float v = c->pat != PW_P_NONE ? h->live : pw_val(c);
        pos = v;
        pw_fmt_value(h, v, big, sizeof big, sub, sizeof sub);
        static const char *const W[PW_M_COUNT] = { N_("CICLO"), N_("ÁNGULO"), N_("BRILLO"), N_("NIVEL") };
        what = c->pat != PW_P_NONE ? _("EN VIVO") : _(W[c->mode < PW_M_COUNT ? c->mode : 0]);
        break;
    }
    }
    lv_arc_set_value(K.arc, (int32_t)(pos * 1000.0f + 0.5f));
    bool live = h->pwm || h->dac;
    lv_color_t col = lv_color_hex(pw_mode_color(c->mode));
    lv_obj_set_style_arc_color(K.arc, live ? col : AOS_C_DIM, LV_PART_INDICATOR);
    lv_label_set_text(K.what, what);
    lv_label_set_text(K.val, big);
    lv_label_set_text(K.sub, sub);
    lv_obj_set_style_text_color(K.val, live ? AOS_C_TEXT : AOS_C_DIM, 0);
}
