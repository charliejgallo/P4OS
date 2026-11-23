/*
 * TANQUES - a turn-based tank battle with toy tanks.
 *
 * The turn: drive (the battle view from above: touch where to go, within
 * the tiles your action points reach), then aim (the gunner's view: drag to
 * turn the turret and raise the barrel, pick the weapon, fire). The shot's
 * flight, the blast, the crater and the damage are watched from above; then
 * the next tank plays - the board, or a second person passing it round.
 * Between battles, the hangar: six tanks to buy, upgrade, arm and paint.
 *
 * This file is the core: life cycle, the worker that draws the frames, the
 * canvas, touch and the joystick, and which screen is up. The battle is
 * tq_battle.c, the menus tq_menu.c, the hangar tq_hangar.c.
 *
 * Threads: everything runs in LVGL's task, which steps the game 30 times a
 * second and hands a copy of it to the worker (tq_scene_render), which draws
 * the next frame into the buffer the canvas is not showing and, on the
 * board, splits it over the two cores.
 */
#include "tq_app.h"
#include "aos_icon_ops.h"
#include "aos_pad_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS   33
#define MAXPX      (720 * 1280)

enum { JOB_IDLE = 0, JOB_RENDER, JOB_DONE };

/* --------------------------------------------------------------------------
 * The worker
 * -------------------------------------------------------------------------- */

static void worker_fn(void *arg)
{
    app_t *a = (app_t *)arg;
    tq_scene_set_split(aos_hal_worker_split);
    while (!aos_hal_worker_should_stop()) {
        if (__atomic_load_n(&a->job, __ATOMIC_ACQUIRE) == JOB_RENDER) {
            memset(&a->prof, 0, sizeof a->prof);
            tq_scene_render(a->req, a->ter, a->fb[a->target], &a->prof);
            __atomic_store_n(&a->job, JOB_DONE, __ATOMIC_RELEASE);
        } else {
            aos_hal_worker_sleep(2);
        }
    }
    tq_scene_set_split(NULL);
}

void tqa_wait_worker(app_t *a)
{
    for (int i = 0; i < 500 && __atomic_load_n(&a->job, __ATOMIC_ACQUIRE) == JOB_RENDER; i++) aos_hal_worker_sleep(2);
}

/* --------------------------------------------------------------------------
 * Frames: the log says what they cost
 * -------------------------------------------------------------------------- */

static void perf_frame(app_t *a, uint32_t now)
{
    if (!a->perf_t0 || a->perf_mode != a->req->mode) {
        a->perf_t0 = now;
        a->perf_frames = 0;
        a->perf_render_us = a->perf_world_us = 0;
        a->perf_mode = a->req->mode;
        memset(&a->prof_acc, 0, sizeof a->prof_acc);
        a->part_frames = a->part_us = a->part_px = 0;
        a->part_counted = false;
        return;
    }
    a->perf_frames++;
    if (!a->part_counted) {
        a->perf_render_us += a->prof.total_us;
        a->perf_world_us += a->prof.world_us;
    }
    a->part_counted = false;
    a->prof_acc.base_us += a->prof.base_us;
    a->prof_acc.up_us += a->prof.up_us;
    uint32_t el = now - a->perf_t0;
    if (el >= 3000 && a->perf_frames) {
        unsigned n = a->perf_frames;
        if (a->perf_mode == TQ_VIEW_FP) {
            aos_hal_log("tanques", "first person %.1f fps %dx%d, render %u ms (ground %u, doubling %u), %d triangles",
                        n * 1000.0f / el, a->W, a->H, (unsigned)(a->perf_render_us / n / 1000),
                        (unsigned)(a->prof_acc.base_us / n / 1000), (unsigned)(a->prof_acc.up_us / n / 1000),
                        a->prof.tris);
        } else if (a->perf_mode == TQ_VIEW_TOP) {
            unsigned whole = n > a->part_frames ? n - a->part_frames : 0, part = a->part_frames;
            aos_hal_log("tanques", "battle view %.1f fps %dx%d: %u whole, %u ms each (world again %u); %u in boxes, "
                        "%u ms each, %u kpx; %d things",
                        n * 1000.0f / el, a->W, a->H, whole, (unsigned)(whole ? a->perf_render_us / whole / 1000 : 0),
                        (unsigned)(whole ? a->perf_world_us / whole / 1000 : 0), part,
                        (unsigned)(part ? a->part_us / part / 1000 : 0), (unsigned)(part ? a->part_px / part / 1000 : 0),
                        a->prof.sprites);
        }
        a->part_frames = a->part_us = a->part_px = 0;
        a->perf_t0 = now;
        a->perf_frames = 0;
        a->perf_render_us = a->perf_world_us = 0;
        memset(&a->prof_acc, 0, sizeof a->prof_acc);
    }
}

static bool animating(app_t *a)
{
    if (a->screen == SC_HANGAR) return true;
    if (a->screen == SC_BATTLE) return tqb_animating(a);
    return a->st.npart > 0;
}

static void fill_req(app_t *a)
{
    tq_req_t *r = a->req;
    memset(r, 0, offsetof(tq_req_t, st));
    r->w = a->W;
    r->h = a->H;
    for (int k = 0; k < TQ_MAX_TANKS; k++) {
        r->lut[k] = &a->lut[k];
        r->rgb[k] = a->rgb[k];
    }
    r->wreck = &a->lut[TQ_MAX_TANKS];
    if (a->screen == SC_HANGAR) {
        tqh_fill(a, r);
        return;
    }
    memcpy(&r->st, &a->st, sizeof a->st);
    if (a->screen == SC_BATTLE) {
        tqb_fill(a, r);
        return;
    }
    r->mode = TQ_VIEW_TOP;
    float cx = a->cam_x, cy = a->cam_y;
    tq_scene_top_clamp(a->ter, a->W, a->H, &cx, &cy);
    r->cx = cx;
    r->cy = cy;
    r->st.show_reach = false;
    r->st.npath = 0;
    r->hp_bars = false;
}

/* The battle view drawn again only where things move, into the frame the
 * canvas shows, here in LVGL's task (tq_scene.c says when it may) */
static bool partial_frame(app_t *a, uint32_t now)
{
    tq_req_t *r = a->req;
    uint32_t key = r->mode == TQ_VIEW_TOP ? tq_scene_top_key(r, a->ter) : 0;
    tq_rects_t cur;
    cur.n = 0;
    if (key) tq_scene_top_rects(r, a->ter, &cur);
    if (!key || !a->shown_key || key != a->shown_key) {
        /* a whole frame: the worker draws it, and these boxes go with it */
        a->req_key = key;
        a->req_rects = cur;
        return false;
    }
    tq_rects_t draw = a->shown_rects;
    for (int i = 0; i < cur.n; i++)
        tq_rects_add(&draw, cur.r[i].x0, cur.r[i].y0, cur.r[i].x1, cur.r[i].y1, a->W, a->H);
    tq_prof_t p;
    memset(&p, 0, sizeof p);
    if (draw.n) tq_scene_top_partial(r, a->ter, a->fb[a->shown], &draw, &p);
    uint32_t px = 0;
    for (int i = 0; i < draw.n; i++) {
        lv_area_t ar = { draw.r[i].x0, draw.r[i].y0, draw.r[i].x1 - 1, draw.r[i].y1 - 1 };
        lv_obj_invalidate_area(a->canvas, &ar);
        px += (uint32_t)(draw.r[i].x1 - draw.r[i].x0) * (uint32_t)(draw.r[i].y1 - draw.r[i].y0);
    }
    a->shown_rects = cur;
    a->part_counted = true;
    a->part_frames++;
    a->part_us += p.total_us;
    a->part_px += px;
    (void)now;
    return true;
}

/* The first person's half-size picture, doubled by the PPA straight into
 * the panel between the bars (the board; the simulator says no and the
 * worker doubles it into the canvas instead). The canvas under it is
 * painted dark once, so the bars' margins do not show a stale battle. */
static bool fp_blit(app_t *a)
{
    int w, h;
    const uint16_t *px = tq_scene_fp_half(&w, &h);
    if (!px || w * 2 != a->W) return false;
    int r0 = a->fp_y0 / 2, r1 = a->fp_y1 / 2;
    if (r1 > h) r1 = h;
    if (r1 <= r0) return false;
    return aos_hal_display_blit_scaled(0, r0 * 2, w, r1 - r0, px + (size_t)r0 * w, 2, false);
}

static void fp_dark(app_t *a)
{
    if (a->fp_dark) return;
    uint16_t *fb = a->fb[a->shown];
    for (size_t i = 0; i < (size_t)a->W * a->H; i++) fb[i] = 0x10A2;
    lv_obj_invalidate(a->canvas);
    a->fp_dark = true;
}

/* LVGL drew over the screen: the first person's picture goes up again */
static void refr_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing || !a->fp_shown || a->blit_ok != 1 || a->screen != SC_BATTLE || a->bstate != B_AIM) return;
    if (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) return;
    /* not while the worker draws the next picture into the same buffer */
    if (__atomic_load_n(&a->job, __ATOMIC_ACQUIRE) == JOB_RENDER) return;
    fp_blit(a);
}

static void push_frames(app_t *a, uint32_t now)
{
    if (!a->fb[0]) return;
    int job = __atomic_load_n(&a->job, __ATOMIC_ACQUIRE);
    if (job == JOB_DONE && a->req->mode == TQ_VIEW_FP && a->req->fp_half) {
        /* through the PPA: the canvas keeps its buffer */
        if (!fp_blit(a)) {
            a->blit_ok = 0;
            a->dirty = true;
            aos_hal_log("tanques", "the PPA said no: the first person goes through the canvas");
        } else {
            fp_dark(a);
            a->fp_shown = true;
        }
        a->shown_key = 0;
        perf_frame(a, now);
        __atomic_store_n(&a->job, JOB_IDLE, __ATOMIC_RELEASE);
        job = JOB_IDLE;
    } else if (job == JOB_DONE) {
        if (a->req->mode == TQ_VIEW_FP && a->blit_ok < 0) {
            /* the first whole picture: does the PPA take it? */
            a->blit_ok = fp_blit(a) ? 1 : 0;
            aos_hal_log("tanques", "first person through %s", a->blit_ok ? "the PPA" : "the canvas");
        }
        a->shown = a->target;
        lv_canvas_set_buffer(a->canvas, a->fb[a->shown], a->W, a->H, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(a->canvas);
        a->shown_key = a->req_key;
        a->shown_rects = a->req_rects;
        perf_frame(a, now);
        __atomic_store_n(&a->job, JOB_IDLE, __ATOMIC_RELEASE);
        job = JOB_IDLE;
    }
    if (job != JOB_IDLE) return;
    if (!a->dirty && !animating(a)) return;
    if (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) return;
    a->dirty = false;
    fill_req(a);
    if (partial_frame(a, now)) {
        perf_frame(a, now);
        return;
    }
    a->target = a->shown ^ 1;
    if (a->worker_ok) {
        __atomic_store_n(&a->job, JOB_RENDER, __ATOMIC_RELEASE);
    } else {
        memset(&a->prof, 0, sizeof a->prof);
        tq_scene_render(a->req, a->ter, a->fb[a->target], &a->prof);
        __atomic_store_n(&a->job, JOB_DONE, __ATOMIC_RELEASE);
    }
}

/* --------------------------------------------------------------------------
 * Widgets the screens share
 * -------------------------------------------------------------------------- */

lv_obj_t *tqa_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x16202A), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t *tqa_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    return l;
}

lv_obj_t *tqa_button(lv_obj_t *parent, const char *txt, uint32_t rgb, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, h / 2 < 26 ? h / 2 : 26, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4A5560), LV_STATE_DISABLED);
    lv_obj_t *l = tqa_label(b, txt, aos_font_body, 0xFFFFFF);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void tqa_btn_text(lv_obj_t *b, const char *txt)
{
    lv_label_set_text(lv_obj_get_child(b, 0), txt);
}

void tqa_show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void tqa_save(app_t *a)
{
    tq_save_store(&a->save);
}

void tqa_paints(app_t *a)
{
    for (int k = 0; k < TQ_MAX_TANKS; k++) {
        int type = k < a->setup.ntank ? a->setup.t[k].type : 0;
        uint32_t p1 = TQ_PAINT[(k * 2) % TQ_PAINTS], p2 = TQ_PAINT[(k * 2 + 1) % TQ_PAINTS], ep = TQ_PAINT[6];
        int em = -1;
        (void)type;
        if (k < a->setup.ntank) {
            /* a person's tank wears the garage's paint; the board's its own */
            const tq_garage_t *g = NULL;
            if (!a->setup.t[k].ai && k == 0) g = &a->save.g[a->setup.t[k].type];
            if (g) {
                p1 = TQ_PAINT[g->paint1 % TQ_PAINTS];
                p2 = TQ_PAINT[g->paint2 % TQ_PAINTS];
                em = g->emblem;
                ep = TQ_PAINT[g->emblem_paint % TQ_PAINTS];
            } else {
                static const uint8_t RIVAL[TQ_MAX_TANKS][3] = { { 2, 3, 8 }, { 6, 7, 1 }, { 4, 5, 8 }, { 9, 1, 3 } };
                int q = a->setup.t[k].ai ? k : (k + 1) % TQ_MAX_TANKS;
                p1 = TQ_PAINT[RIVAL[q][0]];
                p2 = TQ_PAINT[RIVAL[q][1]];
                ep = TQ_PAINT[RIVAL[q][2]];
                em = 4 + q;
            }
        }
        tq_art_tank_lut(&a->lut[k], p1, p2, em, ep);
        a->rgb[k] = p1;
    }
    tq_art_tank_lut(&a->lut[TQ_MAX_TANKS], 0x3A3634, 0x2A2624, -1, 0);
}

/* --------------------------------------------------------------------------
 * Screens
 * -------------------------------------------------------------------------- */

static void build_ui(app_t *a)
{
    a->ui = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->ui);
    lv_obj_set_size(a->ui, a->W, a->H);
    lv_obj_remove_flag(a->ui, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->top_bar = a->move_bar = a->aim_bar = a->banner = a->leave = NULL;
    a->coins_lbl = a->h_name = a->h_role = a->h_list = a->h_buy = NULL;
    for (int i = 0; i < TQ_LABELS; i++) a->lbl[i] = NULL;
    switch (a->screen) {
    case SC_MENU: tqm_build_menu(a); break;
    case SC_STORY: tqm_build_story(a); break;
    case SC_FREE: tqm_build_free(a); break;
    case SC_HANGAR: tqh_build(a); break;
    case SC_BATTLE: tqb_build(a); break;
    case SC_RESULT: tqm_build_result(a); break;
    default: break;
    }
    if (!a->art_ok) {
        lv_obj_t *l = tqa_label(a->ui, _("Falta el arte: copiá tanques_p4.pak en /apps de la tarjeta."), aos_font_body,
                                0xFFFFFF);
        lv_obj_set_width(l, a->W - 80);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_bg_color(l, lv_color_hex(0x16202A), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_90, 0);
        lv_obj_set_style_pad_all(l, 20, 0);
        lv_obj_center(l);
    }
}

void tqa_screen(app_t *a, int screen)
{
    if (a->ui) lv_obj_delete(a->ui);
    a->ui = NULL;
    a->screen = screen;
    a->shown_key = 0;
    if (screen == SC_MENU) tqm_demo(a);
    build_ui(a);
    a->dirty = true;
}

/* --------------------------------------------------------------------------
 * Touch and the joystick
 * -------------------------------------------------------------------------- */

static void touch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t co;
    lv_obj_get_coords(a->canvas, &co);
    int x = pt.x - co.x1, y = pt.y - co.y1;
    lv_event_code_t code = lv_event_get_code(e);
    int ev;
    if (code == LV_EVENT_PRESSED) {
        a->pressing = true;
        a->dragging = false;
        a->px0 = a->plx = x;
        a->py0 = a->ply = y;
        ev = 0;
    } else if (code == LV_EVENT_PRESSING) {
        if (!a->pressing) return;
        if (!a->dragging && (abs(x - a->px0) > 14 || abs(y - a->py0) > 14)) a->dragging = true;
        ev = a->dragging ? 1 : -1;
    } else {
        if (!a->pressing) return;
        a->pressing = false;
        ev = a->dragging ? 3 : 2;           /* 2: a tap */
    }
    int dx = x - a->plx, dy = y - a->ply;
    a->plx = x;
    a->ply = y;
    if (ev < 0) return;
    if (a->screen == SC_BATTLE) tqb_touch(a, ev, x, y, dx, dy);
    else if (a->screen == SC_HANGAR) tqh_touch(a, ev, x, y, dx, dy);
}

/* The menus and the hangar with a joystick: the d-pad walks the buttons on
 * screen (whatever the screen built: they are gathered every tick), A
 * presses, B goes back, L and R change the hangar's tank */
static aos_pad_t s_pad;
static aos_pad_menu_t s_menu;

static void gather(lv_obj_t *o, lv_obj_t **it, int *n)
{
    uint32_t c = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < c && *n < AOS_PAD_MENU_MAX; i++) {
        lv_obj_t *ch = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(ch, &lv_button_class)) it[(*n)++] = ch;
        else gather(ch, it, n);
    }
}

static bool app_back(aos_app_t *self, void *inst);

static void pad_menus(app_t *a)
{
    aos_pad_update(&s_pad, lv_tick_get());
    if (!s_pad.connected || !a->ui) return;
    lv_obj_t *it[AOS_PAD_MENU_MAX];
    int n = 0;
    gather(a->ui, it, &n);
    bool same = n == s_menu.n;
    for (int i = 0; i < n && same; i++) same = it[i] == s_menu.item[i];
    if (!same) aos_pad_menu_set(&s_menu, it, n, s_menu.sel < n ? s_menu.sel : 0);
    uint8_t before = s_menu.sel;
    aos_pad_menu_step(&s_menu, &s_pad);
    if (s_menu.sel != before && s_menu.sel < s_menu.n) lv_obj_scroll_to_view(s_menu.item[s_menu.sel], LV_ANIM_ON);
    if (s_pad.pressed & AOS_PAD_B) app_back(a->self, a);
    if (a->screen == SC_HANGAR && (s_pad.pressed & (AOS_PAD_L | AOS_PAD_R)))
        tqh_cycle(a, s_pad.pressed & AOS_PAD_R ? 1 : -1);
}

/* --------------------------------------------------------------------------
 * The clock
 * -------------------------------------------------------------------------- */

static void frame_cb(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->closing) return;
    uint32_t now = lv_tick_get();
    float dt = a->last_ms ? (now - a->last_ms) / 1000.0f : FRAME_MS / 1000.0f;
    a->last_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    if (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) return;
    if (a->art_ok) {
        if (a->screen == SC_BATTLE) tqb_tick(a, dt);
        else if (a->screen == SC_MENU || a->screen == SC_STORY || a->screen == SC_FREE || a->screen == SC_RESULT)
            tqm_menu_tick(a, dt);
        else if (a->screen == SC_HANGAR) a->h_spin += dt * 0.35f;
        if (a->screen != SC_BATTLE) pad_menus(a);
    }
    push_frames(a, now);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void build_all(app_t *a)
{
    lv_obj_t *root = a->root;
    a->W = AOS_SCREEN_W;
    a->H = AOS_SCREEN_H;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x5D8FAE), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    a->canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(a->canvas, a->fb[a->shown], a->W, a->H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    a->touch = lv_obj_create(root);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, a->W, a->H);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_RELEASED, a);
    a->ui = NULL;
    build_ui(a);
}

static bool app_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || a->closing) return false;
    tqa_wait_worker(a);
    if (__atomic_load_n(&a->job, __ATOMIC_ACQUIRE) == JOB_DONE) __atomic_store_n(&a->job, JOB_IDLE, __ATOMIC_RELEASE);
    for (int i = 0; i < TQ_LABELS; i++) a->lbl_t[i] = 0;
    a->root = root;
    lv_obj_clean(root);
    build_all(a);
    a->perf_t0 = 0;
    a->shown_key = 0;
    a->fp_dark = false;
    a->fp_shown = false;
    a->dirty = true;
    aos_hal_log("tanques", "the screen turned: %dx%d", a->W, a->H);
    return true;
}

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    switch (a->screen) {
    case SC_MENU:
        return false;
    case SC_BATTLE:
        return tqb_back(a);
    case SC_RESULT:
        tqa_screen(a, a->story ? SC_STORY : SC_MENU);
        return true;
    default:
        tqa_screen(a, SC_MENU);
        return true;
    }
}

static void free_all(app_t *a)
{
    tq_free(a->fb[0]);
    tq_free(a->fb[1]);
    tq_free(a->ter);
    tq_free(a->req);
    tq_free(a->lut);
    tqh_free();
    tq_scene_free();
    tq_art_close();
}

static void *tanques_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    uint64_t t0 = aos_hal_uptime_us();
    a->fb[0] = (uint16_t *)tq_malloc(MAXPX * 2);
    a->fb[1] = (uint16_t *)tq_malloc(MAXPX * 2);
    a->ter = (tq_terrain_t *)tq_calloc(sizeof(tq_terrain_t));
    a->req = (tq_req_t *)tq_calloc(sizeof(tq_req_t));
    a->lut = (tq_lut_t *)tq_calloc(sizeof(tq_lut_t) * (TQ_MAX_TANKS + 1));
    if (!a->fb[0] || !a->fb[1] || !a->ter || !a->req || !a->lut || !tq_scene_init()) {
        aos_hal_log("tanques", "out of memory");
        free_all(a);
        lv_free(a);
        return NULL;
    }
    memset(a->fb[0], 0, MAXPX * 2);
    if (!tq_save_load(&a->save)) tq_save_defaults(&a->save);
    a->art_ok = tq_art_open();
    a->screen = SC_MENU;
    if (a->art_ok) tqm_demo(a);
    build_all(a);
    a->worker_ok = aos_hal_worker_start_on("tanques", worker_fn, a, 12 * 1024, 0, 3);
    if (!a->worker_ok) aos_hal_log("tanques", "no worker: drawing in LVGL's task");
    a->dirty = true;
    a->timer = lv_timer_create(frame_cb, FRAME_MS, a);
    a->blit_ok = -1;
    lv_display_add_event_cb(lv_display_get_default(), refr_cb, LV_EVENT_REFR_READY, a);
    aos_hal_log("tanques", "ready %dx%d in %u ms | game %u KB, psram free before %u KB", a->W, a->H,
                (unsigned)((aos_hal_uptime_us() - t0) / 1000), (unsigned)(tq_mem_used() / 1024), (unsigned)(hp / 1024));
#ifdef AOS_SIM_BUILTIN
    /* development: TQ_SCREEN=story|free|hangar; TQ_START=1|2 straight into a
     * free battle (against the board, two players), TQ_AIM=1 then into the
     * gunner's view, TQ_AUTO=1 the board plays all; TQ_COINS=n */
    const char *e = getenv("TQ_COINS");
    if (e && e[0]) a->save.coins = atoi(e);
    e = getenv("TQ_SCREEN");
    if (e && e[0] == 's') tqa_screen(a, SC_STORY);
    if (e && e[0] == 'f') tqa_screen(a, SC_FREE);
    if (e && e[0] == 'h') tqa_screen(a, SC_HANGAR);
    e = getenv("TQ_START");
    if (e && e[0] && a->art_ok) {
        tq_setup_t su;
        memset(&su, 0, sizeof su);
        su.biome = getenv("TQ_BIOME") ? atoi(getenv("TQ_BIOME")) % TQ_BIOMES : 0;
        su.size = 1;
        su.diff = 1;
        su.weather = getenv("TQ_WEATHER") ? atoi(getenv("TQ_WEATHER")) % TQ_WEATHERS : 0;
        su.seed = getenv("TQ_SEED") ? (uint32_t)atoi(getenv("TQ_SEED")) : 7;
        a->setup = su;
        tqh_player_tank(a, 0, a->save.tank, true);
        if (e[0] == '2') {
            tqh_player_tank(a, 1, TQ_T_BOMBO, false);
            a->setup.t[1].team = 1;
            a->setup.ntank = 2;
        } else {
            int rivals = getenv("TQ_RIVALS") ? atoi(getenv("TQ_RIVALS")) : 1;
            for (int k = 1; k <= rivals && k < TQ_MAX_TANKS; k++) {
                tqh_player_tank(a, k, (k * 2 + 1) % TQ_TANK_TYPES, false);
                a->setup.t[k].ai = true;
                a->setup.t[k].team = 1;
            }
            a->setup.ntank = 1 + rivals;
        }
        if (getenv("TQ_AUTO")) a->setup.t[0].ai = true;
        su = a->setup;
        tqb_start(a, &su, false, 0);
        if (getenv("TQ_AIM")) tqb_touch(a, 9, 0, 0, 0, 0);
    }
#endif
    return a;
}

static void tanques_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->closing = true;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), refr_cb, a);
    if (a->worker_ok) aos_hal_worker_stop();
    if (a->timer) lv_timer_delete(a->timer);
    if (a->root) lv_obj_clean(a->root);
    tqa_save(a);
    aos_hal_log("tanques", "closing | game %u KB, peak %u KB", (unsigned)(tq_mem_used() / 1024),
                (unsigned)(tq_mem_peak() / 1024));
    free_all(a);
    lv_free(a);
}

/* The launcher icon: a toy tank, a round turret on a hull with tracks */
static const uint8_t TQ_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,  22, 74, 22, AIC_CIRCLE, AIC_C_LIT(0x403D45), 255),
    AIC_RECT(AIC_CENTER,   0,   8, 64, 20, 8,          AIC_C_LIT(0xF58C29), 255),
    AIC_RECT(AIC_CENTER,  -4,  -8, 38, 26, AIC_CIRCLE, AIC_C_LIT(0xF58C29), 255),
    AIC_RECT(AIC_CENTER,  22, -12, 34,  8, 4,          AIC_C_LIT(0xFAE09E), 255),
    AIC_RECT(AIC_CENTER,  -4, -10, 10, 10, AIC_CIRCLE, AIC_C_LIT(0xFAE09E), 255),
    AIC_END
};

static bool tanques_init(aos_app_t *app)
{
    app->desc.id       = "demo.tanques";
    app->desc.name     = "Tanques";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, TQ_ICON, sizeof TQ_ICON);
    app->desc.color_a  = 0x6FB04A;
    app->desc.color_b  = 0x2E6FB0;
    app->desc.order    = 165;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_LONG_DRAG;

    app->create  = tanques_create;
    app->destroy = tanques_destroy;
    app->back    = app_back;
    app->resize  = app_resize;
    return true;
}

AOS_APP_ENTRY(tanques_init);
