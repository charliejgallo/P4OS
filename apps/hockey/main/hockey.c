/*
 * HOCKEY - table air hockey, upright: one against the board or two on the
 * same screen, each player owning their half
 *
 * The art is 3D, modelled by code in Blender and rendered ahead
 * (tools/blender): each table is one picture of the room and the table,
 * and the mallets and the puck are sprites from the same orthographic
 * camera, so a mallet is the same sprite anywhere on the table. The board
 * composes the frame (hk_gfx.c, Metegol's compositor upright): only the
 * 16 x 16 tiles whose content changed are drawn again, straight into the
 * panel's free buffer, half of them on each core, and the buffer is
 * flipped to.
 *
 * Threads: the app's worker (core 0) loads the match's table (it decodes a
 * JPEG) and then runs the match: the physics in steps of 1/240 s
 * (hk_game.c, hk_ai.c), the frame's list (hk_scene.c), the tiles and the
 * flip. LVGL's thread reads the fingers and the gamepads (hk_input.c),
 * plays the sounds the worker asks for, and owns the menus (hk_menu.c)
 * and the panels over a match. While LVGL or the system has something
 * over the match the worker holds, and LVGL shows the match's last frame
 * under it from a canvas.
 */
#include "hk_app.h"
#include "hk_view.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS     14
#define PERF_MS      5000
#define WORKER_STACK (24 * 1024)
#define ACCENT       0xE8443A

void *hk_in_mx;

static void pause_show(hk_app_t *a);
static void result_show(hk_app_t *a);
static void panel_close(hk_app_t *a);

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ---- the settings ------------------------------------------------------------------------------------- */

static void cfg_path(char *p, size_t n)
{
    const char *d = aos_hal_path_data();
    snprintf(p, n, "%.120s/hockey_ajustes.txt", d ? d : ".");
}

static void cfg_defaults(hk_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->diff = HK_NORMAL;
    c->sfx = c->air = 1;
    c->paddle = 1;
    c->speed = 1;
    c->format = FMT_GOALS;
    c->minutes = 3;
    c->goals = 7;
    snprintf(c->table, sizeof c->table, "bar");
}

static const struct {
    const char *k;
    size_t off;
} INTS[] = {
    { "dificultad", offsetof(hk_cfg_t, diff) }, { "sonido", offsetof(hk_cfg_t, sfx) },
    { "aire", offsetof(hk_cfg_t, air) }, { "paleta", offsetof(hk_cfg_t, paddle) },
    { "disco", offsetof(hk_cfg_t, speed) }, { "modo", offsetof(hk_cfg_t, mode) },
    { "formato", offsetof(hk_cfg_t, format) }, { "minutos", offsetof(hk_cfg_t, minutes) },
    { "goles", offsetof(hk_cfg_t, goals) },
};

void hk_cfg_load(hk_cfg_t *c)
{
    cfg_defaults(c);
    char p[160];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) {
            if (!strcmp(line, INTS[i].k)) *(int *)((char *)c + INTS[i].off) = atoi(v);
        }
        if (!strcmp(line, "mesa")) snprintf(c->table, sizeof c->table, "%.15s", v);
    }
    fclose(f);
    if (c->diff < 0 || c->diff >= HK_DIFF_N) c->diff = HK_NORMAL;
    if (c->minutes < 1 || c->minutes > 10) c->minutes = 3;
    if (c->goals != 3 && c->goals != 5 && c->goals != 7 && c->goals != 10) c->goals = 7;
    if (c->speed < 0 || c->speed > 2) c->speed = 1;
    if (c->paddle < 0 || c->paddle > 2) c->paddle = 1;
    c->mode = c->mode ? MODE_TWO : MODE_CPU;
    c->format = c->format ? FMT_TIME : FMT_GOALS;
}

void hk_cfg_save(const hk_cfg_t *c)
{
    char p[160];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    char line[96];
    fputs("# Hockey de mesa: los ajustes y el último partido (la app lo reescribe)\n", f);
    for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) {
        snprintf(line, sizeof line, "%s=%d\n", INTS[i].k, *(const int *)((const char *)c + INTS[i].off));
        fputs(line, f);
    }
    snprintf(line, sizeof line, "mesa=%.15s\n", c->table);
    fputs(line, f);
    fclose(f);
}

int hk_lang(void)
{
    const char *l = aos_i18n_current();
    if (!l || !l[0] || !strncmp(l, "es", 2)) return 0;
    if (!strncmp(l, "de", 2)) return 2;
    return 1;
}

float hk_mallet_r(const hk_cfg_t *c)
{
    static const float R[3] = { HK_MALLET_S, HK_MALLET_M, HK_MALLET_L };
    return R[c->paddle < 0 || c->paddle > 2 ? 1 : c->paddle];
}

/* ---- the worker's frame ----------------------------------------------------------------------------------- */

static void sounds_push(hk_app_t *a, hk_game_t *g)
{
    for (int i = 0; i < g->nsnd; i++) {
        uint32_t w = a->snd_w;
        if (w - a->snd_r >= 64) break;
        a->snd[w & 63] = g->snd[i];
        a->snd_vol[w & 63] = g->snd_vol[i];
        a->snd_w = w + 1;
    }
    g->nsnd = 0;
}

static hk_pbuf_t *pbuf(hk_app_t *a, uint16_t *b)
{
    for (int i = 0; i < 3; i++) {
        if (a->pb[i].buf == b) return &a->pb[i];
    }
    for (int i = 0; i < 3; i++) {
        if (!a->pb[i].buf) {
            a->pb[i].buf = b;
            a->pb[i].flip = 0;
            return &a->pb[i];
        }
    }
    return NULL;
}

static void perf(hk_app_t *a)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!a->perf_t0) a->perf_t0 = now;
    if (now - a->perf_t0 < PERF_MS || !a->perf_frames) return;
    float s = (float)(now - a->perf_t0) / 1000.0f, n = (float)a->perf_frames;
    aos_hal_log("hockey", "%.1f fps, a frame %.2f ms (sim %.2f, list %.2f, draw %.2f), %u tiles, %u things, flip %d, %u KB held, puck %d,%d state %d",
                (double)(n / s), (double)((float)(a->perf_sim_us + a->perf_list_us + a->perf_draw_us) / 1000.0f / n),
                (double)((float)a->perf_sim_us / 1000.0f / n), (double)((float)a->perf_list_us / 1000.0f / n),
                (double)((float)a->perf_draw_us / 1000.0f / n), (unsigned)(a->perf_tiles / a->perf_frames),
                (unsigned)(a->perf_items / a->perf_frames), a->flip_ok, (unsigned)(hk_mem_used() / 1024),
                (int)a->game->px, (int)a->game->py, a->game->state);
    a->perf_frames = a->perf_tiles = a->perf_items = 0;
    a->perf_sim_us = a->perf_list_us = a->perf_draw_us = 0;
    a->perf_t0 = now;
}

static void match_frame(hk_app_t *a)
{
    hk_game_t *g = a->game;
    uint32_t t0 = (uint32_t)aos_hal_uptime_us();
    float dt = a->clock_reset ? 0.016f : (float)(uint32_t)(t0 - a->last_us) / 1e6f;
    a->clock_reset = false;
    a->last_us = t0;
    if (dt > 0.05f) dt = 0.05f;

    if (g->state == HK_ST_REPLAY) {
        /* the goal again, slower */
        g->rp_t += dt * 0.55f;
        int k = (int)(g->rp_t * 60.0f);
        aos_hal_mutex_lock(hk_in_mx);
        bool skip = a->skip_replay;
        a->skip_replay = false;
        aos_hal_mutex_unlock(hk_in_mx);
        if (skip || k >= g->rp_len) {
            hk_replay_end(g);
            sounds_push(a, g);
        } else {
            hk_replay_apply(g, k);
        }
    } else {
        a->acc += dt;
        int steps = 0;
        aos_hal_mutex_lock(hk_in_mx);
        a->skip_replay = false;
        while (a->acc >= HK_DT && steps < 24) {
            hk_hand_t h[2];
            const hk_hand_t *hp[2] = { NULL, NULL };
            for (int s = 0; s < 2; s++) {
                if (g->ai_on[s]) continue;
                hk_input_hand(a, s, &h[s]);
                hp[s] = &h[s];
            }
            hk_game_step(g, hp[0], hp[1]);
            sounds_push(a, g);
            a->acc -= HK_DT;
            steps++;
        }
        aos_hal_mutex_unlock(hk_in_mx);
        if (a->acc > HK_DT * 4) a->acc = 0;
    }
    uint32_t t1 = (uint32_t)aos_hal_uptime_us();
    hk_scene_draw(a, dt);
    uint32_t t2 = (uint32_t)aos_hal_uptime_us();
    uint16_t *b = a->flip_ok ? aos_hal_display_back() : NULL;
    int tiles;
    if (b) {
        hk_pbuf_t *p = pbuf(a, b);
        uint32_t since = 0;
        bool touched = true;
        bool known = p && p->flip && aos_hal_display_back_age(b, &since, &touched) && !touched && since != UINT32_MAX &&
                     since == a->flips - p->flip;
        tiles = hk_gfx_end(&a->gfx, b, p ? p->hash : a->fb_hash, !known);
        if (!p) a->fb_full = true;
        if (aos_hal_display_flip(b)) {
            a->flips++;
            if (p) p->flip = a->flips;
        } else if (p) {
            p->flip = 0;
        }
    } else {
        tiles = hk_gfx_end(&a->gfx, a->fb, a->fb_hash, a->fb_full);
        a->fb_full = false;
        a->frame_gen++;
    }
    uint32_t t3 = (uint32_t)aos_hal_uptime_us();
    a->perf_frames++;
    a->perf_tiles += (uint32_t)tiles;
    a->perf_items += (uint32_t)a->gfx.nitem;
    a->perf_sim_us += t1 - t0;
    a->perf_list_us += t2 - t1;
    a->perf_draw_us += t3 - t2;
    perf(a);
}

/* the match's table (a JPEG to decode) */
static bool job_load(hk_app_t *a, int tb)
{
    bool ok = hk_art_table(&a->art, tb);
    if (ok) hk_gfx_world(&a->gfx, a->art.bg);
    return ok;
}

/* a still of the table for the menus: the mallets at home, the puck in the middle */
static bool job_still(hk_app_t *a)
{
    if (!job_load(a, a->still_tb)) return false;
    hk_game_t *g = a->game;
    g->rnd = 0x1234;
    g->puck_speed = 1.0f;
    hk_game_start(g, 0, 7, false, 0, false, 0, hk_mallet_r(&a->cfg));
    g->px = 70;
    g->py = -60;
    g->m[0].x = -40;
    g->m[1].x = 60;
    g->nsnd = 0;
    memset(a->ui, 0, sizeof *a->ui);
    a->ui->hud_off = true;
    hk_scene_strings(a);
    a->gfx.one_core = !a->worker_on;
    hk_scene_draw(a, 0.016f);
    hk_gfx_end(&a->gfx, a->fb, a->fb_hash, true);
    a->frame_gen++;
    return true;
}

static void do_job(hk_app_t *a)
{
    int j = a->job;
    if (!j) return;
    aos_hal_mutex_lock(a->mx);
    bool ok = j == 1 ? job_load(a, a->tb_ix) : job_still(a);
    aos_hal_mutex_unlock(a->mx);
    a->job = 0;
    a->job_done = ok ? 1 : -1;
}

static void worker_fn(void *arg)
{
    hk_app_t *a = (hk_app_t *)arg;
    while (!aos_hal_worker_should_stop()) {
        if (a->job) {
            do_job(a);
            continue;
        }
        if (!a->run) {
            aos_hal_worker_sleep(8);
            continue;
        }
        uint32_t t0 = (uint32_t)aos_hal_uptime_us();
        aos_hal_mutex_lock(a->mx);
        if (a->run) match_frame(a);
        aos_hal_mutex_unlock(a->mx);
        uint32_t ms = ((uint32_t)aos_hal_uptime_us() - t0) / 1000;
        aos_hal_worker_sleep(ms < 15 ? 16 - ms : 1);
    }
}

/* the worker stops drawing, and the match's last frame goes to the canvas */
static void match_hold(hk_app_t *a)
{
    if (!a->run) return;
    a->run = false;
    aos_hal_mutex_lock(a->mx);
    a->gfx.one_core = true;
    hk_gfx_end(&a->gfx, a->fb, a->fb_hash, a->flip_ok || a->fb_full);
    a->gfx.one_core = !a->worker_on;
    a->fb_full = false;
    aos_hal_mutex_unlock(a->mx);
    if (a->canvas) lv_obj_invalidate(a->canvas);
}

static void match_resume(hk_app_t *a)
{
    if (a->run || !a->loaded) return;
    a->clock_reset = true;
    a->run = true;
}

void hk_still(hk_app_t *a)
{
    int tb = hk_table_find(&a->art, a->cfg.table);
    a->still_tb = tb >= 0 ? tb : 0;
    a->job_done = 0;
    a->job = 2;
    if (!a->worker_on) do_job(a);
}

/* ---- panels over the match ------------------------------------------------------------------------------ */

static lv_obj_t *panel_open(hk_app_t *a, int kind, int w)
{
    panel_close(a);
    match_hold(a);
    lv_obj_t *dim = lv_obj_create(a->root);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim, C(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_50, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *p = lv_obj_create(dim);
    lv_obj_set_size(p, w, LV_SIZE_CONTENT);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, 28, 0);
    lv_obj_set_style_bg_color(p, C(0x1A1D26), 0);
    lv_obj_set_style_bg_grad_color(p, C(0x0B0C11), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 3, 0);
    lv_obj_set_style_border_color(p, C(ACCENT), 0);
    lv_obj_set_style_pad_all(p, 26, 0);
    lv_obj_set_style_pad_row(p, 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    a->panel = dim;
    a->panel_kind = kind;
    hk_audio_pause(kind == PN_PAUSE);
    aos_pad_reset(&a->pad, lv_tick_get());
    return p;
}

static void panel_close(hk_app_t *a)
{
    if (!a->panel) return;
    aos_pad_menu_clear(&a->menu);
    lv_obj_delete(a->panel);
    a->panel = NULL;
    a->panel_kind = PN_NONE;
    hk_audio_pause(false);
    aos_pad_reset(&a->pad, lv_tick_get());
}

static void pad_menu_from(hk_app_t *a, lv_obj_t *p)
{
    lv_obj_t *items[16];
    int n = 0;
    uint32_t cnt = lv_obj_get_child_count(p);
    for (uint32_t i = 0; i < cnt && n < 16; i++) {
        lv_obj_t *c = lv_obj_get_child(p, (int32_t)i);
        if (lv_obj_check_type(c, &lv_button_class)) items[n++] = c;
    }
    aos_pad_menu_set(&a->menu, items, n, 0);
}

static void ev_resume(lv_event_t *e)
{
    panel_close((hk_app_t *)lv_event_get_user_data(e));
}

static void ev_restart(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    hk_match_quit(a);
    hk_match_start(a);
}

static void ev_menu(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    hk_match_quit(a);
    hk_menu_show(a, SC_SETUP);
    hk_still(a);
}

static void ev_sfx(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    a->cfg.sfx = !a->cfg.sfx;
    hk_audio_set(a->cfg.sfx, a->cfg.air);
    hk_cfg_save(&a->cfg);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->cfg.sfx ? _("Sonido: sí") : _("Sonido: no"));
}

static void score_line(hk_app_t *a, char *s, size_t n)
{
    hk_game_t *g = a->game;
    snprintf(s, n, "%.30s  %d - %d  %.30s", a->ui->name[0], g->score[0], g->score[1], a->ui->name[1]);
}

static void pause_show(hk_app_t *a)
{
    if (!a->loaded || a->panel) return;
    lv_obj_t *p = panel_open(a, PN_PAUSE, 560);
    hk_label(p, _("Pausa"), aos_font_title, ACCENT);
    char line[96];
    score_line(a, line, sizeof line);
    lv_obj_t *l = hk_label(p, line, aos_font_small, 0xC8CCD8);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    hk_button(p, _("Seguir"), true, 460, 90, ev_resume, a);
    hk_button(p, _("Empezar de nuevo"), false, 460, 78, ev_restart, a);
    hk_button(p, a->cfg.sfx ? _("Sonido: sí") : _("Sonido: no"), false, 460, 78, ev_sfx, a);
    hk_button(p, _("Salir al menú"), false, 460, 78, ev_menu, a);
    pad_menu_from(a, p);
}

static void result_show(hk_app_t *a)
{
    if (a->panel || a->result_done) return;
    a->result_done = true;
    hk_game_t *g = a->game;
    lv_obj_t *p = panel_open(a, PN_RESULT, 600);
    int s0 = g->score[0], s1 = g->score[1];
    char line[120];
    if (s0 == s1) snprintf(line, sizeof line, "%s", _("Empate"));
    else if (a->cfg.mode == MODE_CPU) snprintf(line, sizeof line, "%s", s0 > s1 ? _("¡Ganaste!") : _("Ganó la placa"));
    else snprintf(line, sizeof line, "%s", s0 > s1 ? _("¡Ganó el jugador 1!") : _("¡Ganó el jugador 2!"));
    hk_label(p, line, aos_font_title, s0 >= s1 || a->cfg.mode == MODE_TWO ? 0xFFE14A : 0xFF6050);
    score_line(a, line, sizeof line);
    lv_obj_t *l = hk_label(p, line, aos_font_body, 0xFFFFFF);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    hk_button(p, _("Revancha"), true, 460, 90, ev_restart, a);
    hk_button(p, _("Menú"), false, 460, 78, ev_menu, a);
    pad_menu_from(a, p);
    hk_audio_pause(false);
    aos_hal_log("hockey", "match over: %d - %d (difficulty %d, mode %d), %d hard shots, %.0f s", s0, s1, a->cfg.diff,
                a->cfg.mode, g->shots, (double)g->t);
}

/* ---- a match comes and goes ----------------------------------------------------------------------------- */

void hk_match_quit(hk_app_t *a)
{
    a->run = false;
    panel_close(a);
    if (a->loading) lv_obj_delete(a->loading);
    a->loading = NULL;
    aos_hal_display_refresh_request(0);
    for (int i = 0; i < 3; i++) a->pb[i].buf = NULL;
    a->loaded = false;
    a->state = ST_MENU;
    a->snd_r = a->snd_w;
    hk_audio_air(false);
    a->fb_full = true;
}

static void start_play(hk_app_t *a)
{
    hk_game_t *g = a->game;
    static const float SPEED[3] = { 0.8f, 1.0f, 1.25f };
    g->puck_speed = SPEED[a->cfg.speed];
    g->rnd = (uint32_t)aos_hal_uptime_us() | 1u;
    bool cpu = a->cfg.mode == MODE_CPU;
    bool ai0 = false, ai1 = cpu;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* HK_DEMO=1: the board plays both sides (screenshots) */
    if (getenv("HK_DEMO")) ai0 = ai1 = true;
#endif
    hk_game_start(g, a->cfg.format == FMT_TIME ? a->cfg.minutes : 0, a->cfg.format == FMT_GOALS ? a->cfg.goals : 0,
                  ai0, a->cfg.diff, ai1, a->cfg.diff, hk_mallet_r(&a->cfg));
    memset(a->ui, 0, sizeof *a->ui);
    a->ui->hint_t = 4.5f;
    a->ui->seen_state = g->state;
    hk_scene_strings(a);
    memset(a->in, 0, sizeof a->in);
    memset(a->fing, 0, sizeof a->fing);
}

static void loaded_cb(hk_app_t *a)
{
    if (a->loading) lv_obj_delete(a->loading);
    a->loading = NULL;
    hk_menu_free(a);
    start_play(a);
    /* straight into the panel only while it is upright as the screen is */
    a->flip_ok = aos_hal_display_get_rotation() == 0 && aos_hal_display_back() != NULL;
    a->flips = 0;
    for (int i = 0; i < 3; i++) {
        a->pb[i].buf = NULL;
        a->pb[i].flip = 0;
    }
    a->loaded = true;
    a->result_done = false;
    a->acc = 0;
    a->snd_r = a->snd_w;
    a->perf_t0 = 0;
    a->perf_frames = 0;
    a->fb_full = true;
    /* the first frame now, on this thread, for whatever opens over the match */
    a->gfx.one_core = true;
    hk_scene_draw(a, 0.016f);
    hk_gfx_end(&a->gfx, a->fb, a->fb_hash, true);
    a->gfx.one_core = !a->worker_on;
    a->fb_full = false;
    lv_obj_invalidate(a->canvas);
    aos_hal_display_refresh_request(60);
    hk_audio_air(true);
    a->state = ST_MATCH;
    a->clock_reset = true;
    a->run = true;
    aos_hal_log("hockey", "a match on '%s', difficulty %d, mode %d, %u KB held", a->art.tb[a->tb_ix].id, a->cfg.diff,
                a->cfg.mode, (unsigned)(hk_mem_used() / 1024));
}

void hk_match_start(hk_app_t *a)
{
    if (a->state == ST_LOADING || !a->art_ok) return;
    int tb = hk_table_find(&a->art, a->cfg.table);
    a->tb_ix = tb >= 0 ? tb : 0;
    hk_cfg_save(&a->cfg);
    a->state = ST_LOADING;
    a->loading = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->loading);
    lv_obj_set_size(a->loading, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(a->loading, C(0x0B0C11), 0);
    lv_obj_set_style_bg_opa(a->loading, LV_OPA_COVER, 0);
    const hk_table_t *t = &a->art.tb[a->tb_ix];
    lv_obj_t *l = hk_label(a->loading, t->name[hk_lang()][0] ? t->name[hk_lang()] : t->name[0], aos_font_title, t->accent);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -20);
    l = hk_label(a->loading, _("Prendiendo el aire..."), aos_font_small, 0x9098A8);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 40);
    a->job_done = 0;
    a->job = 1;
    if (!a->worker_on) do_job(a);
}

/* ---- LVGL's side ------------------------------------------------------------------------------------------ */

static void sounds_play(hk_app_t *a)
{
    while (a->snd_r != a->snd_w) {
        hk_audio_play(a->snd[a->snd_r & 63], a->snd_vol[a->snd_r & 63]);
        a->snd_r++;
    }
}

static void match_tick(hk_app_t *a)
{
    uint32_t over = aos_ui_overlay();
    hk_game_t *g = a->game;
    if (over) {
        const uint32_t PAUSING = AOS_UI_OVER_PANEL | AOS_UI_OVER_SWITCHER | AOS_UI_OVER_GESTURE | AOS_UI_OVER_NOT_FRONT |
                                 AOS_UI_OVER_LOCK;
        if ((over & PAUSING) && !a->panel && g->state != HK_ST_END) pause_show(a);
        match_hold(a);
        return;
    }
    if (a->panel) {
        if (a->panel_kind == PN_PAUSE && aos_pad_pressed(&a->pad, AOS_PAD_B | AOS_PAD_START)) {
            panel_close(a);
            return;
        }
        aos_pad_menu_step(&a->menu, &a->pad);
        return;
    }
    if (!a->run) match_resume(a);
    hk_input_touch(a);
    hk_input_pads(a);
    if (a->button_pause) {
        a->button_pause = false;
        if (g->state != HK_ST_END) {
            pause_show(a);
            return;
        }
    }
    sounds_play(a);
    hk_audio_air(g->state != HK_ST_END);
    if (!a->worker_on) match_frame(a);
    if (a->frame_seen != a->frame_gen) {
        a->frame_seen = a->frame_gen;
        if (a->canvas && !a->flip_ok) lv_obj_invalidate(a->canvas);
    }
    if (g->state == HK_ST_END && g->state_t > 0) {
        /* the end's banner for a moment, then the result */
        static const float WAIT = 2.5f;
        if (a->ui->banner == 2 && a->ui->banner_t < 30.0f - WAIT) result_show(a);
    }
}

static void timer_cb(lv_timer_t *tm)
{
    hk_app_t *a = (hk_app_t *)lv_timer_get_user_data(tm);
    aos_pad_update(&a->pad, lv_tick_get());
    if (a->state == ST_LOADING) {
        if (a->job_done == 1) loaded_cb(a);
        else if (a->job_done == -1) {
            a->job_done = 0;
            hk_match_quit(a);
            hk_menu_show(a, SC_SETUP);
            aos_ui_toast(_("No se pudo cargar la mesa"), 3000);
        }
    } else if (a->state == ST_MATCH && a->loaded) {
        match_tick(a);
    } else {
        if (a->job_done == 1) {
            /* the still for the menus is ready */
            a->job_done = 0;
            if (a->canvas) lv_obj_invalidate(a->canvas);
        }
        hk_menu_pad(a);
    }
    hk_audio_pump();
}

/* ---- life cycle ----------------------------------------------------------------------------------------- */

static void *hockey_create(aos_app_t *self, lv_obj_t *root)
{
    hk_app_t *a = (hk_app_t *)hk_calloc(sizeof(hk_app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->game = (hk_game_t *)hk_calloc(sizeof(hk_game_t));
    a->ui = (hk_ui_t *)hk_calloc(sizeof(hk_ui_t));
    a->fb = (uint16_t *)hk_calloc((size_t)HK_W * HK_H * 2);
    a->fb_hash = (uint32_t *)hk_calloc(sizeof(uint32_t) * HK_NT);
    for (int i = 0; i < 3; i++) a->pb[i].hash = (uint32_t *)hk_calloc(sizeof(uint32_t) * HK_NT);
    a->mx = aos_hal_mutex_create();
    if (!hk_in_mx) hk_in_mx = aos_hal_mutex_create();
    bool ok = a->game && a->ui && a->fb && a->fb_hash && a->pb[0].hash && a->pb[1].hash && a->pb[2].hash && a->mx &&
              hk_in_mx && hk_gfx_init(&a->gfx);
    lv_obj_set_style_bg_color(root, C(0x0B0C11), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    hk_cfg_load(&a->cfg);
    const char *apps = aos_hal_path_apps();
    a->art_ok = ok && hk_art_open(&a->art, apps ? apps : ".");
    if (a->art_ok) {
        hk_audio_set(a->cfg.sfx, a->cfg.air);
        hk_audio_open(&a->art.pak);
    }
    /* the canvas under everything: the menus' still, a held match */
    if (a->fb) {
        a->canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(a->canvas, a->fb, HK_W, HK_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(a->canvas, 0, 0);
        lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    }
    a->worker_on = ok && aos_hal_worker_start_on("hockey", worker_fn, a, WORKER_STACK, 0, 3);
    a->gfx.one_core = !a->worker_on;
    if (!a->worker_on) aos_hal_log("hockey", "no worker: everything on LVGL's thread, on one core");
    a->state = ST_MENU;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* HK_PLAY=1 starts a match straight away (scripts) */
    if (getenv("HK_TABLE")) snprintf(a->cfg.table, sizeof a->cfg.table, "%.15s", getenv("HK_TABLE"));
    if (getenv("HK_MODE")) a->cfg.mode = atoi(getenv("HK_MODE")) ? MODE_TWO : MODE_CPU;
    if (getenv("HK_DIFF")) a->cfg.diff = atoi(getenv("HK_DIFF")) % HK_DIFF_N;
    if (getenv("HK_PADDLE")) a->cfg.paddle = atoi(getenv("HK_PADDLE")) % 3;
#endif
    if (a->art_ok) hk_still(a);
    hk_menu_show(a, SC_TITLE);
    a->timer = lv_timer_create(timer_cb, FRAME_MS, a);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    if (getenv("HK_PLAY") && a->art_ok) {
        while (a->job) aos_hal_sleep_ms(5);
        a->job_done = 0;
        hk_match_start(a);
    }
#endif
    return a;
}

static void hockey_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    hk_app_t *a = (hk_app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->timer = NULL;
    a->run = false;
    if (a->worker_on) aos_hal_worker_stop();
    a->worker_on = false;
    hk_match_quit(a);
    hk_menu_free(a);
    hk_audio_close();
    hk_art_close(&a->art);
    hk_gfx_free(&a->gfx);
    for (int i = 0; i < 3; i++) hk_free(a->pb[i].hash);
    hk_free(a->fb_hash);
    hk_free(a->fb);
    hk_free(a->game);
    hk_free(a->ui);
    hk_free(a);
    /* what the app's own allocator still holds: 0 unless something leaked */
    aos_hal_log("hockey", "closed, %u KB of its own still held", (unsigned)(hk_mem_used() / 1024));
}

static void hockey_show(aos_app_t *self, void *inst)
{
    (void)self;
    hk_app_t *a = (hk_app_t *)inst;
    if (!a) return;
    if (a->loaded) hk_scene_strings(a);
    hk_audio_pause(a->panel != NULL && a->panel_kind == PN_PAUSE);
}

static void hockey_hide(aos_app_t *self, void *inst)
{
    (void)self;
    hk_app_t *a = (hk_app_t *)inst;
    if (!a) return;
    if (a->state == ST_MATCH && a->loaded) {
        if (!a->panel && a->game->state != HK_ST_END) pause_show(a);
        match_hold(a);
    }
    hk_audio_pause(true);
}

static bool hockey_back(aos_app_t *self, void *inst)
{
    (void)self;
    hk_app_t *a = (hk_app_t *)inst;
    if (!a) return false;
    if (a->state == ST_LOADING) return true;
    if (a->state == ST_MATCH) {
        if (a->panel_kind == PN_PAUSE) panel_close(a);
        else if (!a->panel) pause_show(a);
        return true;
    }
    return hk_menu_back(a);
}

static bool hockey_button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    hk_app_t *a = (hk_app_t *)inst;
    if (!a || a->state != ST_MATCH || action != AOS_BUTTON_CLICK) return false;
    a->button_pause = true;
    return true;
}

/* the table from above: the rails, the white surface, the red line, two
 * mallets and the puck */
static const uint8_t HOCKEY_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 0, 70, 104, 14, AIC_C_LIT(0x1A1C22), 255),
    AIC_RECT(AIC_CENTER, 0, 0, 56, 90, 10, AIC_C_LIT(0xE8EEF2), 255),
    AIC_GRAD(AIC_C_LIT(0xC9D3DC), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER, 0, 0, 56, 3, 0, AIC_C_LIT(0xD3262B), 230),
    AIC_RECT(AIC_CENTER, 0, 0, 22, 22, AIC_CIRCLE, AIC_C_LIT(0xD3262B), 70),
    AIC_RECT(AIC_CENTER, 0, -47, 24, 4, 1, AIC_C_LIT(0x050506), 255),
    AIC_RECT(AIC_CENTER, 0, 47, 24, 4, 1, AIC_C_LIT(0x050506), 255),
    AIC_RECT(AIC_CENTER, -6, -28, 20, 20, AIC_CIRCLE, AIC_C_LIT(0x1E5BD8), 255),
    AIC_RECT(AIC_CENTER, -6, -28, 9, 9, AIC_CIRCLE, AIC_C_LIT(0x5A8BF0), 255),
    AIC_RECT(AIC_CENTER, 8, 26, 20, 20, AIC_CIRCLE, AIC_C_LIT(0xD3262B), 255),
    AIC_RECT(AIC_CENTER, 8, 26, 9, 9, AIC_CIRCLE, AIC_C_LIT(0xF06A6E), 255),
    AIC_RECT(AIC_CENTER, 10, -4, 12, 12, AIC_CIRCLE, AIC_C_LIT(0xF4B400), 255),
    AIC_END
};

static bool hockey_init(aos_app_t *app)
{
    app->desc.id       = "demo.hockey";
    app->desc.name     = "Hockey de mesa";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xD3262B;
    app->desc.color_b  = 0x1A1C22;
    app->desc.order    = 161;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_PORTRAIT |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    aos_icon_set_ops(app, HOCKEY_ICON, sizeof HOCKEY_ICON);
    app->create  = hockey_create;
    app->destroy = hockey_destroy;
    app->show    = hockey_show;
    app->hide    = hockey_hide;
    app->back    = hockey_back;
    app->button  = hockey_button;
    return true;
}

AOS_APP_ENTRY(hockey_init);
