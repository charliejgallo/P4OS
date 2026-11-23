/*
 * TANQUES - a turn-based tank battle with toy tanks. THE SAMPLE: one small
 * map, two tanks, two weapons and a whole turn in both views, to settle the
 * look and the feel before the real art and the rest of the game.
 *
 * The turn: drive (the battle view from above: touch where to go, within
 * the tiles your action points reach), then aim (the gunner's view: drag to
 * turn the turret and raise the barrel, pick the weapon, fire). The shot's
 * flight, the blast, the crater and the damage are watched from above; then
 * the other tank plays - the board, or a second person passing it round.
 *
 * Threads: everything here runs in LVGL's task, which steps the battle 30
 * times a second and hands a copy of it to the worker (tq_scene_render),
 * which draws the next frame into the buffer the canvas is not showing and,
 * on the board, splits it over the two cores. LVGL shows the canvas and
 * the HUD's labels and buttons over it.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "tq_art.h"
#include "tq_game.h"
#include "tq_mem.h"
#include "tq_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS   33
#define MAXPX      (720 * 1280)
#define DRIVE_MS   3.2f             /* m/s */
#define TURN_RS    (200.0f * TQ_DEG)

enum {
    ST_TITLE = 0,
    ST_MOVE,        /* a person picks where to drive */
    ST_DRIVE,       /* a tank drives its path */
    ST_AIM,         /* a person aims, first person */
    ST_AI_THINK,
    ST_AI_AIM,      /* the board's turret turns to its aim */
    ST_FLY,         /* the shell in the air */
    ST_BOOM,        /* the blast settles */
    ST_HANDOVER,    /* two players: "pass the board" */
    ST_OVER,
};

enum { JOB_IDLE = 0, JOB_RENDER, JOB_DONE };

typedef struct {
    aos_app_t *self;
    lv_obj_t *root;
    int W, H;
    bool closing;

    /* pixels */
    uint16_t *fb[2];
    int shown;                      /* the canvas's buffer */
    lv_obj_t *canvas, *touch;
    tq_terrain_t *ter;
    tq_state_t st;
    tq_req_t *req;
    volatile int job;
    int target;
    bool worker_ok;
    bool dirty;                     /* a frame is wanted though nothing moves */
    tq_prof_t prof, prof_acc;
    uint32_t perf_t0, perf_frames, perf_render_us, perf_world_us;
    int perf_mode;

    /* the battle */
    int state;
    float st_t;                     /* seconds in the state */
    bool vs_ai;
    int scheme;
    float cam_x, cam_y, cam_tx, cam_ty;
    bool cam_follow;
    int drive_seg, drive_cost;
    float ai_yaw, ai_pitch;
    int ai_weapon;
    bool missed;
    uint32_t last_ms;

    /* touch */
    bool pressing, dragging;
    int px0, py0, plx, ply;

    /* HUD */
    lv_obj_t *top_bar, *turn_lbl, *chip[2], *chip_name[2], *chip_hp[2];
    lv_obj_t *move_bar, *ap_lbl, *ap_dot[10], *btn_aim, *hint;
    lv_obj_t *aim_bar, *btn_w[TQ_WEAPONS], *btn_fire, *btn_back, *aim_lbl;
    lv_obj_t *title, *sw[TQ_SCHEMES];
    lv_obj_t *over, *over_lbl;
    lv_obj_t *banner;
    lv_obj_t *dmg[3];
    float dmg_t[3];
    tq_v3_t dmg_p[3];
    lv_obj_t *nopack;
    lv_timer_t *timer;
} app_t;

static void hud_refresh(app_t *a);
static void set_state(app_t *a, int s);

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

static void wait_worker(app_t *a)
{
    for (int i = 0; i < 500 && __atomic_load_n(&a->job, __ATOMIC_ACQUIRE) == JOB_RENDER; i++) aos_hal_worker_sleep(2);
}

/* --------------------------------------------------------------------------
 * The views' cameras
 * -------------------------------------------------------------------------- */

static tq_tank_t *cur_tank(app_t *a)
{
    return &a->st.tank[a->st.cur];
}

static bool human_turn(app_t *a)
{
    return !cur_tank(a)->ai;
}

static void cam_to(app_t *a, float x, float y, bool snap)
{
    a->cam_tx = x;
    a->cam_ty = y;
    a->cam_follow = true;
    if (snap) {
        a->cam_x = x;
        a->cam_y = y;
    }
}

static void fill_req(app_t *a)
{
    tq_req_t *r = a->req;
    r->w = a->W;
    r->h = a->H;
    r->hp_bars = a->state != ST_TITLE;
    memcpy(&r->st, &a->st, sizeof a->st);
    if (a->state == ST_AIM) {
        const tq_tank_t *k = cur_tank(a);
        r->mode = TQ_VIEW_FP;
        r->me = a->st.cur;
        r->eye = tq_fp_eye(k->x, k->y, tq_tank_z(a->ter, k), k->tyaw);
        r->yaw = k->tyaw;
        r->pitch = TQ_FP_PITCH * TQ_DEG;
    } else {
        r->mode = TQ_VIEW_TOP;
        float cx = a->cam_x, cy = a->cam_y;
        tq_scene_top_clamp(a->ter, a->W, a->H, &cx, &cy);
        r->cx = cx;
        r->cy = cy;
        if (a->state != ST_MOVE && a->state != ST_DRIVE) r->st.show_reach = false;
    }
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
        return;
    }
    a->perf_frames++;
    a->perf_render_us += a->prof.total_us;
    a->perf_world_us += a->prof.world_us;
    a->prof_acc.base_us += a->prof.base_us;
    a->prof_acc.things_us += a->prof.things_us;
    a->prof_acc.up_us += a->prof.up_us;
    uint32_t el = now - a->perf_t0;
    if (el >= 3000 && a->perf_frames) {
        unsigned n = a->perf_frames;
        if (a->perf_mode == TQ_VIEW_FP) {
            aos_hal_log("tanques", "first person %.1f fps %dx%d, render %u ms (ground %u, doubling %u), %d triangles",
                        n * 1000.0f / el, a->W, a->H, (unsigned)(a->perf_render_us / n / 1000),
                        (unsigned)(a->prof_acc.base_us / n / 1000), (unsigned)(a->prof_acc.up_us / n / 1000),
                        a->prof.tris);
        } else {
            aos_hal_log("tanques", "battle view %.1f fps %dx%d, render %u ms (world again %u), %d things",
                        n * 1000.0f / el, a->W, a->H, (unsigned)(a->perf_render_us / n / 1000),
                        (unsigned)(a->perf_world_us / n / 1000), a->prof.sprites);
        }
        a->perf_t0 = now;
        a->perf_frames = 0;
        a->perf_render_us = a->perf_world_us = 0;
        memset(&a->prof_acc, 0, sizeof a->prof_acc);
    }
}

static bool animating(app_t *a)
{
    if (a->st.npart || a->st.shell.live) return true;
    switch (a->state) {
    case ST_DRIVE:
    case ST_AI_AIM:
    case ST_FLY:
    case ST_BOOM:
        return true;
    default:
        break;
    }
    float dx = a->cam_tx - a->cam_x, dy = a->cam_ty - a->cam_y;
    return a->cam_follow && dx * dx + dy * dy > 0.0004f;
}

static void push_frames(app_t *a, uint32_t now)
{
    if (!a->fb[0]) return;
    int job = __atomic_load_n(&a->job, __ATOMIC_ACQUIRE);
    if (job == JOB_DONE) {
        a->shown = a->target;
        lv_canvas_set_buffer(a->canvas, a->fb[a->shown], a->W, a->H, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(a->canvas);
        perf_frame(a, now);
        __atomic_store_n(&a->job, JOB_IDLE, __ATOMIC_RELEASE);
        job = JOB_IDLE;
    }
    if (job != JOB_IDLE) return;
    if (!a->dirty && !animating(a) && a->state != ST_TITLE) return;
    if (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) return;
    a->dirty = false;
    fill_req(a);
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
 * The turn
 * -------------------------------------------------------------------------- */

static const char *player_name(app_t *a, int k)
{
    if (a->vs_ai) return k == 0 ? _("Vos") : _("La placa");
    return k == 0 ? _("Jugador 1") : _("Jugador 2");
}

static void start_turn(app_t *a)
{
    tq_tank_t *k = cur_tank(a);
    k->ap = k->apmax;
    a->st.npath = 0;
    a->st.narc = 0;
    tq_game_reach(&a->st, a->ter);
    cam_to(a, k->x, k->y, false);
    if (k->ai) set_state(a, ST_AI_THINK);
    else if (!a->vs_ai) set_state(a, ST_HANDOVER);
    else set_state(a, ST_MOVE);
}

static void next_turn(app_t *a)
{
    for (int i = 0; i < 2; i++) {
        if (a->st.tank[i].hp <= 0) {
            set_state(a, ST_OVER);
            return;
        }
    }
    a->st.cur ^= 1;
    start_turn(a);
}

static void new_battle(app_t *a, bool vs_ai)
{
    wait_worker(a);
    a->vs_ai = vs_ai;
    uint32_t seed = 7;
    int other = (a->scheme + 1) % TQ_SCHEMES;
    tq_game_new(&a->st, a->ter, seed, a->scheme, other, vs_ai);
    tq_scene_world_all();
    a->st.cur = 0;
    cam_to(a, a->st.tank[0].x, a->st.tank[0].y, true);
    start_turn(a);
    a->dirty = true;
}

static void show_damage(app_t *a, int slot, tq_v3_t p, const char *txt, uint32_t rgb)
{
    if (slot < 0 || slot > 2 || !a->dmg[slot]) return;
    lv_label_set_text(a->dmg[slot], txt);
    lv_obj_set_style_text_color(a->dmg[slot], lv_color_hex(rgb), 0);
    lv_obj_remove_flag(a->dmg[slot], LV_OBJ_FLAG_HIDDEN);
    a->dmg_t[slot] = 1.6f;
    a->dmg_p[slot] = p;
}

static void land(app_t *a, tq_v3_t at)
{
    tq_blast_t bl;
    tq_vbox_t dirty;
    int w = a->st.shell.weapon;
    wait_worker(a);
    tq_game_blast(&a->st, a->ter, at, w, a->st.shell.owner, &bl, &dirty);
    tq_scene_world_dirty(dirty);
    int slot = 0;
    for (int k = 0; k < 2; k++) {
        if (!bl.damage[k]) continue;
        const tq_tank_t *tk = &a->st.tank[k];
        char txt[32];
        snprintf(txt, sizeof txt, bl.direct[k] ? "-%d!" : "-%d", bl.damage[k]);
        show_damage(a, slot++, tq_v3(tk->x, tk->y, tq_tank_z(a->ter, tk) + 4.2f), txt, 0xFF5A46);
    }
    if (!bl.damage[0] && !bl.damage[1])
        show_damage(a, 0, tq_v3(at.x, at.y, at.z + 4.0f), _("¡Cerca!"), 0xFFFFFF);
    aos_hal_beep(90, 120);
    set_state(a, ST_BOOM);
}

static void aim_changed(app_t *a)
{
    tq_game_arc(&a->st, a->ter);
    hud_refresh(a);
    a->dirty = true;
}

/* --------------------------------------------------------------------------
 * HUD
 * -------------------------------------------------------------------------- */

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, int radius)
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

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, uint32_t rgb, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4A5560), LV_STATE_DISABLED);
    lv_obj_t *l = label(b, txt, aos_font_body, 0xFFFFFF);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void btn_text(lv_obj_t *b, const char *txt)
{
    lv_label_set_text(lv_obj_get_child(b, 0), txt);
}

static void enter_aim(app_t *a)
{
    if (a->state != ST_MOVE) return;
    tq_tank_t *k = cur_tank(a);
    if (k->ap < TQ_WEAPON[TQ_W_CANNON].ap) {
        /* no shot left: the turn ends */
        next_turn(a);
        return;
    }
    if (k->ap < TQ_WEAPON[k->weapon].ap) k->weapon = TQ_W_CANNON;
    /* the turret starts facing the other tank: the aim is the finger's */
    const tq_tank_t *e = &a->st.tank[a->st.cur ^ 1];
    k->tyaw = atan2f(e->y - k->y, e->x - k->x);
    set_state(a, ST_AIM);
}

static void on_aim(lv_event_t *e)
{
    enter_aim((app_t *)lv_event_get_user_data(e));
}

static void on_back(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->state == ST_AIM) set_state(a, ST_MOVE);
}

static void on_weapon(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
    for (int w = 0; w < TQ_WEAPONS; w++) {
        if (b != a->btn_w[w]) continue;
        tq_tank_t *k = cur_tank(a);
        if (k->ap < TQ_WEAPON[w].ap) return;
        k->weapon = w;
        if (w == TQ_W_MORTAR && k->pitch < 35 * TQ_DEG) k->pitch = 45 * TQ_DEG;
        if (w == TQ_W_CANNON && k->pitch > 25 * TQ_DEG) k->pitch = 8 * TQ_DEG;
        aim_changed(a);
    }
}

static void on_fire(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->state != ST_AIM) return;
    tq_tank_t *k = cur_tank(a);
    if (k->ap < TQ_WEAPON[k->weapon].ap) return;
    tq_game_fire(&a->st, a->ter);
    aos_hal_beep(140, 90);
    set_state(a, ST_FLY);
}

static void on_scheme(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
    for (int k = 0; k < TQ_SCHEMES; k++) {
        if (a->sw[k] == b) a->scheme = k;
        lv_obj_set_style_border_width(a->sw[k], a->sw[k] == b ? 6 : 0, 0);
    }
    /* the map behind shows the paint at once */
    a->st.tank[0].scheme = a->scheme;
    a->st.tank[1].scheme = (a->scheme + 1) % TQ_SCHEMES;
    a->dirty = true;
}

static void on_play(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    new_battle(a, lv_event_get_target(e) == lv_obj_get_child(a->title, -1) ? false : true);
}

static void on_again(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    set_state(a, ST_TITLE);
}

static void hud_refresh(app_t *a)
{
    if (!a->top_bar) return;
    for (int k = 0; k < 2; k++) {
        const tq_tank_t *tk = &a->st.tank[k];
        lv_label_set_text(a->chip_name[k], player_name(a, k));
        lv_bar_set_value(a->chip_hp[k], tk->hp, LV_ANIM_ON);
        lv_obj_set_style_border_color(a->chip[k], lv_color_hex(tq_art_scheme_rgb(tk->scheme)), 0);
        lv_obj_set_style_border_width(a->chip[k], k == a->st.cur ? 4 : 0, 0);
        lv_obj_set_style_bg_color(a->chip_hp[k], lv_color_hex(tq_art_scheme_rgb(tk->scheme)), LV_PART_INDICATOR);
    }
    char txt[96];
    const tq_tank_t *k = cur_tank(a);
    if (k->ai) snprintf(txt, sizeof txt, "%s", _("Juega la placa"));
    else if (a->vs_ai) snprintf(txt, sizeof txt, "%s", _("Tu turno"));
    else snprintf(txt, sizeof txt, _("Turno: %s"), player_name(a, a->st.cur));
    lv_label_set_text(a->turn_lbl, txt);
    snprintf(txt, sizeof txt, "%d AP", k->ap);
    lv_label_set_text(a->ap_lbl, txt);
    for (int i = 0; i < 10; i++) {
        /* the last three are the cannon's: driving past them leaves no shot */
        uint32_t c = 0x3A4652;
        if (i < k->ap) c = i >= k->ap - TQ_WEAPON[TQ_W_CANNON].ap ? 0xFFD23C : 0x46C8FF;
        lv_obj_set_style_bg_color(a->ap_dot[i], lv_color_hex(c), 0);
    }
    btn_text(a->btn_aim, k->ap >= TQ_WEAPON[TQ_W_CANNON].ap ? _("Apuntar") : _("Terminar"));
    for (int w = 0; w < TQ_WEAPONS; w++) {
        snprintf(txt, sizeof txt, "%s  %d AP", _(TQ_WEAPON[w].name), TQ_WEAPON[w].ap);
        btn_text(a->btn_w[w], txt);
        if (k->ap < TQ_WEAPON[w].ap) lv_obj_add_state(a->btn_w[w], LV_STATE_DISABLED);
        else lv_obj_remove_state(a->btn_w[w], LV_STATE_DISABLED);
        lv_obj_set_style_border_width(a->btn_w[w], w == k->weapon ? 5 : 0, 0);
    }
    float yaw_rel = (k->tyaw - k->yaw) / TQ_DEG;
    while (yaw_rel > 180) yaw_rel -= 360;
    while (yaw_rel < -180) yaw_rel += 360;
    snprintf(txt, sizeof txt, _("Elevación %d°   Giro %d°"), (int)roundf(k->pitch / TQ_DEG), (int)roundf(-yaw_rel));
    lv_label_set_text(a->aim_lbl, txt);
}

static void build_hud(app_t *a)
{
    lv_obj_t *root = a->root;
    int W = a->W, H = a->H;
    bool land = W > H;
    int m = 14;

    /* the two tanks and whose turn */
    int tbw = land ? 760 : W - 2 * m;
    a->top_bar = panel(root, (W - tbw) / 2, m, tbw, 92, 26);
    int cw = land ? 250 : 210;
    for (int k = 0; k < 2; k++) {
        lv_obj_t *c = lv_obj_create(a->top_bar);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, cw, 76);
        lv_obj_align(c, k ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, k ? -8 : 8, 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        a->chip[k] = c;
        a->chip_name[k] = label(c, "", aos_font_caption, 0xFFFFFF);
        lv_obj_align(a->chip_name[k], LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t *bar = lv_bar_create(c);
        lv_obj_set_size(bar, cw - 36, 14);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A3440), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        a->chip_hp[k] = bar;
    }
    a->turn_lbl = label(a->top_bar, "", aos_font_caption, 0xFFE9A8);
    lv_obj_set_width(a->turn_lbl, tbw - 2 * cw - 24);
    lv_obj_set_style_text_align(a->turn_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(a->turn_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(a->turn_lbl, LV_ALIGN_CENTER, 0, 0);

    /* driving: the points and the button to aim */
    int mbw = land ? 640 : W - 2 * m, mbh = 112;
    a->move_bar = panel(root, (W - mbw) / 2, H - mbh - m, mbw, mbh, 30);
    a->ap_lbl = label(a->move_bar, "", aos_font_body, 0xFFFFFF);
    lv_obj_align(a->ap_lbl, LV_ALIGN_TOP_LEFT, 26, 14);
    for (int i = 0; i < 10; i++) {
        lv_obj_t *d = lv_obj_create(a->move_bar);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 22, 22);
        lv_obj_set_style_radius(d, 11, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_pos(d, 26 + i * 28, 62);
        a->ap_dot[i] = d;
    }
    a->btn_aim = button(a->move_bar, _("Apuntar"), 0xE8572E, land ? 240 : 230, 80, on_aim, a);
    lv_obj_align(a->btn_aim, LV_ALIGN_RIGHT_MID, -16, 0);
    a->hint = label(root, _("Tocá el mapa adonde ir. Arrastrá para mirar."), aos_font_caption, 0xFFFFFF);
    lv_obj_set_style_bg_color(a->hint, lv_color_hex(0x16202A), 0);
    lv_obj_set_style_bg_opa(a->hint, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(a->hint, 16, 0);
    lv_obj_set_style_pad_ver(a->hint, 8, 0);
    lv_obj_set_style_radius(a->hint, 18, 0);
    lv_obj_align(a->hint, LV_ALIGN_BOTTOM_MID, 0, -(mbh + m + 12));

    /* aiming: back, the weapons, fire */
    int abw = land ? 900 : W - 2 * m, abh = land ? 112 : 200;
    a->aim_bar = panel(root, (W - abw) / 2, H - abh - m, abw, abh, 30);
    a->aim_lbl = label(a->aim_bar, "", aos_font_caption, 0xFFE9A8);
    a->btn_fire = button(a->aim_bar, _("¡Fuego!"), 0xE23B2E, land ? 220 : 250, 84, on_fire, a);
    for (int w = 0; w < TQ_WEAPONS; w++) {
        a->btn_w[w] = button(a->aim_bar, "", 0x2E6FB0, land ? 250 : 300, 70, on_weapon, a);
        lv_obj_set_style_border_color(a->btn_w[w], lv_color_hex(0xFFFFFF), 0);
        lv_obj_t *l = lv_obj_get_child(a->btn_w[w], 0);
        lv_obj_set_style_text_font(l, aos_font_caption, 0);
    }
    if (land) {
        lv_obj_align(a->btn_w[0], LV_ALIGN_LEFT_MID, 16, 8);
        lv_obj_align(a->btn_w[1], LV_ALIGN_LEFT_MID, 16 + 262, 8);
        lv_obj_align(a->btn_fire, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_align(a->aim_lbl, LV_ALIGN_TOP_LEFT, 24, 4);
    } else {
        lv_obj_align(a->aim_lbl, LV_ALIGN_TOP_LEFT, 24, 12);
        lv_obj_align(a->btn_w[0], LV_ALIGN_TOP_LEFT, 16, 48);
        lv_obj_align(a->btn_w[1], LV_ALIGN_BOTTOM_LEFT, 16, -12);
        lv_obj_align(a->btn_fire, LV_ALIGN_RIGHT_MID, -16, 16);
    }
    a->btn_back = button(root, _("Mover"), 0x2A3440, 160, 70, on_back, a);
    lv_obj_align(a->btn_back, LV_ALIGN_TOP_LEFT, m, 92 + 2 * m);

    /* the floating damage */
    for (int i = 0; i < 3; i++) {
        a->dmg[i] = label(root, "", aos_font_title, 0xFF5A46);
        lv_obj_set_style_text_outline_stroke_width(a->dmg[i], 0, 0);
        lv_obj_add_flag(a->dmg[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* the banner between two players' turns */
    a->banner = panel(root, 0, 0, land ? 640 : W - 80, 180, 30);
    lv_obj_center(a->banner);
    lv_obj_t *bl = label(a->banner, "", aos_font_title, 0xFFFFFF);
    lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(bl);

    /* the title */
    int tw = land ? 620 : W - 80;
    a->title = panel(root, 0, 0, tw, 520, 36);
    lv_obj_set_style_bg_opa(a->title, LV_OPA_90, 0);
    lv_obj_center(a->title);
    lv_obj_t *tt = label(a->title, _("Tanques"), aos_font_large, 0xFFE9A8);
    lv_obj_align(tt, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_t *st = label(a->title, _("Muestra: un mapa, dos tanques, un turno"), aos_font_caption, 0xB8C4CF);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_t *cl = label(a->title, _("Tu color"), aos_font_body, 0xFFFFFF);
    lv_obj_align(cl, LV_ALIGN_TOP_MID, 0, 150);
    for (int k = 0; k < TQ_SCHEMES; k++) {
        lv_obj_t *b = lv_button_create(a->title);
        lv_obj_set_size(b, 84, 84);
        lv_obj_set_style_radius(b, 42, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(tq_art_scheme_rgb(k)), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(b, k == a->scheme ? 6 : 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_align(b, LV_ALIGN_TOP_MID, (k - 1) * 120, 200);
        lv_obj_add_event_cb(b, on_scheme, LV_EVENT_CLICKED, a);
        a->sw[k] = b;
    }
    lv_obj_t *p1 = button(a->title, _("Contra la placa"), 0xE8572E, tw - 120, 84, on_play, a);
    lv_obj_align(p1, LV_ALIGN_TOP_MID, 0, 320);
    lv_obj_t *p2 = button(a->title, _("Dos jugadores"), 0x2E6FB0, tw - 120, 84, on_play, a);
    lv_obj_align(p2, LV_ALIGN_TOP_MID, 0, 416);         /* the last child: on_play reads it */

    /* the end */
    a->over = panel(root, 0, 0, land ? 600 : W - 80, 300, 36);
    lv_obj_set_style_bg_opa(a->over, LV_OPA_90, 0);
    lv_obj_center(a->over);
    a->over_lbl = label(a->over, "", aos_font_title, 0xFFE9A8);
    lv_obj_align(a->over_lbl, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_t *again = button(a->over, _("Otra vez"), 0xE8572E, 300, 84, on_again, a);
    lv_obj_align(again, LV_ALIGN_BOTTOM_MID, 0, -40);

    if (!tq_art_ready()) {
        a->nopack = label(root, _("Falta el arte: copiá tanques_p4.pak en /apps de la tarjeta."), aos_font_body,
                          0xFFFFFF);
        lv_obj_set_width(a->nopack, W - 80);
        lv_label_set_long_mode(a->nopack, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_center(a->nopack);
    }
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void hud_for_state(app_t *a)
{
    int s = a->state;
    bool playing = s != ST_TITLE && s != ST_OVER;
    show(a->top_bar, playing);
    show(a->move_bar, s == ST_MOVE);
    show(a->hint, s == ST_MOVE && a->st.tank[a->st.cur].ap >= 3);
    show(a->aim_bar, s == ST_AIM);
    show(a->btn_back, s == ST_AIM);
    show(a->title, s == ST_TITLE);
    show(a->over, s == ST_OVER);
    show(a->banner, s == ST_HANDOVER);
    if (s == ST_HANDOVER) {
        char txt[96];
        snprintf(txt, sizeof txt, _("Turno de %s\nPasale la placa"), player_name(a, a->st.cur));
        lv_label_set_text(lv_obj_get_child(a->banner, 0), txt);
    }
    if (s == ST_OVER) {
        int win = a->st.tank[0].hp > 0 ? 0 : 1;
        char txt[96];
        if (a->vs_ai) snprintf(txt, sizeof txt, "%s", win == 0 ? _("¡Ganaste!") : _("Ganó la placa"));
        else snprintf(txt, sizeof txt, _("¡Ganó %s!"), player_name(a, win));
        lv_label_set_text(a->over_lbl, txt);
    }
    hud_refresh(a);
}

static void set_state(app_t *a, int s)
{
    a->state = s;
    a->st_t = 0;
    a->st.show_reach = s == ST_MOVE;
    if (s == ST_AIM) aim_changed(a);
    else a->st.narc = 0;
    hud_for_state(a);
    a->dirty = true;
}

/* --------------------------------------------------------------------------
 * Touch
 * -------------------------------------------------------------------------- */

static void pan(app_t *a, int dx, int dy)
{
    tq_v3_t F, R, U;
    tq_top_axes(&F, &R, &U);
    float uh = sqrtf(U.x * U.x + U.y * U.y);
    a->cam_x += (-dx * R.x + dy * U.x / uh / uh) / TQ_TOP_S;
    a->cam_y += (-dx * R.y + dy * U.y / uh / uh) / TQ_TOP_S;
    tq_scene_top_clamp(a->ter, a->W, a->H, &a->cam_x, &a->cam_y);
    a->cam_tx = a->cam_x;
    a->cam_ty = a->cam_y;
    a->cam_follow = false;
    a->dirty = true;
}

static void tap(app_t *a, int x, int y)
{
    if (a->state != ST_MOVE || !human_turn(a)) return;
    float cx = a->cam_x, cy = a->cam_y, wx, wy;
    tq_scene_top_clamp(a->ter, a->W, a->H, &cx, &cy);
    if (!tq_scene_top_pick(a->ter, cx, cy, a->W, a->H, (float)x, (float)y, &wx, &wy)) return;
    int ap = 0;
    if (!tq_game_route(&a->st, a->ter, wx, wy, &ap)) {
        aos_ui_toast(_("No llega hasta ahí"), 900);
        return;
    }
    a->drive_seg = 1;
    a->drive_cost = ap;
    set_state(a, ST_DRIVE);
    a->st.show_reach = false;
}

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
    if (code == LV_EVENT_PRESSED) {
        a->pressing = true;
        a->dragging = false;
        a->px0 = a->plx = x;
        a->py0 = a->ply = y;
        return;
    }
    if (!a->pressing) return;
    if (code == LV_EVENT_PRESSING) {
        if (!a->dragging && (abs(x - a->px0) > 14 || abs(y - a->py0) > 14)) a->dragging = true;
        if (a->dragging) {
            int dx = x - a->plx, dy = y - a->ply;
            if (a->state == ST_AIM) {
                tq_tank_t *k = cur_tank(a);
                k->tyaw -= dx * 0.0035f;
                k->pitch -= dy * 0.0028f;
                k->pitch = tq_clampf(k->pitch, -5 * TQ_DEG, 60 * TQ_DEG);
                if (k->weapon == TQ_W_MORTAR && k->pitch < 30 * TQ_DEG) k->pitch = 30 * TQ_DEG;
                aim_changed(a);
            } else if (a->state != ST_TITLE && a->state != ST_OVER) {
                pan(a, dx, dy);
            }
        }
        a->plx = x;
        a->ply = y;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        a->pressing = false;
        if (!a->dragging) tap(a, x, y);
        if (a->state == ST_HANDOVER) set_state(a, ST_MOVE);
    }
}

/* --------------------------------------------------------------------------
 * The battle's clock
 * -------------------------------------------------------------------------- */

static float wrap_pi(float a)
{
    while (a > TQ_PI) a -= 2 * TQ_PI;
    while (a < -TQ_PI) a += 2 * TQ_PI;
    return a;
}

static float turn_to(float from, float to, float max)
{
    float d = wrap_pi(to - from);
    if (d > max) d = max;
    if (d < -max) d = -max;
    return from + d;
}

static void drive_step(app_t *a, float dt)
{
    tq_tank_t *k = cur_tank(a);
    tq_state_t *s = &a->st;
    if (a->drive_seg >= s->npath) {
        k->ap -= a->drive_cost;
        if (k->ap < 0) k->ap = 0;
        s->npath = 0;
        wait_worker(a);
        tq_game_reach(s, a->ter);
        if (k->ai) set_state(a, ST_AI_AIM);
        else set_state(a, ST_MOVE);
        return;
    }
    float tx = s->path[a->drive_seg][0], ty = s->path[a->drive_seg][1];
    float dx = tx - k->x, dy = ty - k->y, d = sqrtf(dx * dx + dy * dy);
    float want = atan2f(dy, dx);
    float before = k->yaw;
    if (d > 0.05f) k->yaw = turn_to(k->yaw, want, TURN_RS * dt);
    k->tyaw += k->yaw - before;
    float off = fabsf(wrap_pi(want - k->yaw));
    if (off < 30 * TQ_DEG) {
        float step = DRIVE_MS * dt * (1.0f - off / (40 * TQ_DEG));
        if (step >= d) {
            k->x = tx;
            k->y = ty;
            a->drive_seg++;
        } else {
            k->x += dx / d * step;
            k->y += dy / d * step;
        }
        /* dust from the tracks */
        if (tq_rand() < 0.5f && s->npart < TQ_MAX_PARTS - 20) {
            tq_part_t *q = &s->part[s->npart++];
            float side = tq_rand() < 0.5f ? -0.9f : 0.9f;
            float cy = cosf(k->yaw), sy = sinf(k->yaw);
            q->p = tq_v3(k->x - cy * 1.3f - sy * side, k->y - sy * 1.3f + cy * side, tq_tank_z(a->ter, k) + 0.15f);
            q->v = tq_v3((tq_rand() - 0.5f) * 0.6f, (tq_rand() - 0.5f) * 0.6f, 0.4f);
            q->age = 0;
            q->life = 0.9f;
            q->r0 = 0.15f;
            q->r1 = 0.45f;
            q->rgb = 0xC9B48E;
            q->kind = TQ_PART_PUFF;
        }
    }
    cam_to(a, k->x, k->y, false);
}

static void tick_state(app_t *a, float dt)
{
    tq_state_t *s = &a->st;
    tq_tank_t *k = cur_tank(a);
    a->st_t += dt;
    s->pulse = fmodf(s->pulse + dt * 0.8f, 1.0f);
    switch (a->state) {
    case ST_TITLE: {
        /* the map drifts behind the title */
        float t = a->st_t * 0.12f;
        cam_to(a, 18 + 6 * cosf(t), 18 + 6 * sinf(t), a->st_t < 0.05f);
        break;
    }
    case ST_DRIVE:
        drive_step(a, dt);
        break;
    case ST_AI_THINK:
        if (a->st_t > 0.7f) {
            int ap;
            wait_worker(a);
            tq_ai_plan_move(s, a->ter, &ap);
            if (s->npath >= 2) {
                a->drive_seg = 1;
                a->drive_cost = ap;
                set_state(a, ST_DRIVE);
            } else {
                set_state(a, ST_AI_AIM);
            }
        }
        break;
    case ST_AI_AIM:
        if (a->st_t < dt * 1.5f) tq_ai_plan_aim(s, a->ter, &a->ai_yaw, &a->ai_pitch, &a->ai_weapon);
        k->tyaw = turn_to(k->tyaw, a->ai_yaw, 80 * TQ_DEG * dt);
        k->pitch += tq_clampf(a->ai_pitch - k->pitch, -40 * TQ_DEG * dt, 40 * TQ_DEG * dt);
        k->weapon = a->ai_weapon;
        cam_to(a, k->x, k->y, false);
        if (a->st_t > 0.6f && fabsf(wrap_pi(a->ai_yaw - k->tyaw)) < 0.01f && fabsf(a->ai_pitch - k->pitch) < 0.01f) {
            if (k->ap < TQ_WEAPON[k->weapon].ap) {
                next_turn(a);
                break;
            }
            tq_game_fire(s, a->ter);
            aos_hal_beep(140, 90);
            set_state(a, ST_FLY);
        }
        break;
    case ST_FLY:
        if (s->shell.live) cam_to(a, s->shell.p.x, s->shell.p.y, false);
        break;
    case ST_BOOM:
        if (a->st_t > 2.0f) next_turn(a);
        break;
    default:
        break;
    }
    /* the world moves on: the shell, the particles, stones falling */
    tq_v3_t at;
    bool was = s->shell.live;
    if (tq_game_step(s, a->ter, dt, &at)) land(a, at);
    else if (was && !s->shell.live && a->state == ST_FLY) {
        show_damage(a, 0, s->shell.p, _("¡Afuera!"), 0xFFFFFF);
        set_state(a, ST_BOOM);
    }
    /* the camera */
    if (a->cam_follow) {
        float f = tq_clampf(dt * (a->state == ST_FLY ? 6.0f : 3.5f), 0, 1);
        a->cam_x += (a->cam_tx - a->cam_x) * f;
        a->cam_y += (a->cam_ty - a->cam_y) * f;
    }
    /* the floating numbers */
    for (int i = 0; i < 3; i++) {
        if (a->dmg_t[i] <= 0) continue;
        a->dmg_t[i] -= dt;
        if (a->dmg_t[i] <= 0 || a->state == ST_AIM) {
            lv_obj_add_flag(a->dmg[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float cx = a->cam_x, cy = a->cam_y, sx, sy;
        tq_scene_top_clamp(a->ter, a->W, a->H, &cx, &cy);
        tq_v3_t p = a->dmg_p[i];
        p.z += (1.6f - a->dmg_t[i]) * 0.8f;
        tq_scene_top_project(a->ter, cx, cy, a->W, a->H, p, &sx, &sy);
        sx = tq_clampf(sx, 120, a->W - 120);
        sy = tq_clampf(sy, 200, a->H - 200);
        lv_obj_update_layout(a->dmg[i]);
        lv_obj_set_pos(a->dmg[i], (int)sx - lv_obj_get_width(a->dmg[i]) / 2, (int)sy - 30);
        lv_obj_set_style_opa(a->dmg[i], (lv_opa_t)(a->dmg_t[i] > 0.5f ? 255 : a->dmg_t[i] * 510), 0);
    }
    static int s_hp[2] = { -1, -1 };
    if (s_hp[0] != s->tank[0].hp || s_hp[1] != s->tank[1].hp) {
        s_hp[0] = s->tank[0].hp;
        s_hp[1] = s->tank[1].hp;
        hud_refresh(a);
    }
}

static void frame_cb(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->closing) return;
    uint32_t now = lv_tick_get();
    float dt = a->last_ms ? (now - a->last_ms) / 1000.0f : FRAME_MS / 1000.0f;
    a->last_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    if (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) return;
    if (tq_art_ready()) tick_state(a, dt);
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
    build_hud(a);
    hud_for_state(a);
}

static bool app_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || a->closing) return false;
    wait_worker(a);
    if (__atomic_load_n(&a->job, __ATOMIC_ACQUIRE) == JOB_DONE) __atomic_store_n(&a->job, JOB_IDLE, __ATOMIC_RELEASE);
    for (int i = 0; i < 3; i++) a->dmg_t[i] = 0;
    a->root = root;
    lv_obj_clean(root);
    a->top_bar = NULL;
    a->nopack = NULL;
    build_all(a);
    a->perf_t0 = 0;
    a->dirty = true;
    aos_hal_log("tanques", "the screen turned: %dx%d", a->W, a->H);
    return true;
}

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    if (a->state == ST_TITLE) return false;
    if (a->state == ST_AIM) set_state(a, ST_MOVE);
    else set_state(a, ST_TITLE);
    return true;
}

static void free_all(app_t *a)
{
    tq_free(a->fb[0]);
    tq_free(a->fb[1]);
    tq_free(a->ter);
    tq_free(a->req);
    tq_scene_free();
    tq_art_free();
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
    if (!a->fb[0] || !a->fb[1] || !a->ter || !a->req || !tq_scene_init()) {
        aos_hal_log("tanques", "out of memory");
        free_all(a);
        lv_free(a);
        return NULL;
    }
    memset(a->fb[0], 0, MAXPX * 2);
    tq_art_load();
    tq_game_new(&a->st, a->ter, 7, 0, 1, true);
    a->state = ST_TITLE;
    build_all(a);
    a->worker_ok = aos_hal_worker_start_on("tanques", worker_fn, a, 12 * 1024, 0, 3);
    if (!a->worker_ok) aos_hal_log("tanques", "no worker: drawing in LVGL's task");
    a->dirty = true;
    a->timer = lv_timer_create(frame_cb, FRAME_MS, a);
    aos_hal_log("tanques", "ready %dx%d in %u ms | game %u KB (art %u KB), psram free before %u KB",
                a->W, a->H, (unsigned)((aos_hal_uptime_us() - t0) / 1000), (unsigned)(tq_mem_used() / 1024),
                (unsigned)(tq_art_bytes() / 1024), (unsigned)(hp / 1024));
#ifdef AOS_SIM_BUILTIN
    /* development: TQ_START=1|2 straight into a battle (against the board,
     * two players); TQ_AIM=1 then into the gunner's view; TQ_AUTO=1 the
     * board plays both */
    const char *e = getenv("TQ_START");
    if (e && e[0]) {
        new_battle(a, e[0] != '2');
        if (getenv("TQ_AUTO")) {
            /* the board plays both tanks */
            a->st.tank[0].ai = true;
            start_turn(a);
        }
        if (getenv("TQ_AIM")) enter_aim(a);
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
    if (a->worker_ok) aos_hal_worker_stop();
    if (a->timer) lv_timer_delete(a->timer);
    if (a->root) lv_obj_clean(a->root);
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
