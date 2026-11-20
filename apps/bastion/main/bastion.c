/*
 * BASTION - a tower defense: a fort in space, ten worlds, waves of aliens
 *
 * The art is 3D, modelled by code in Blender and rendered ahead
 * (tools/blender): each world is one picture with the depth of its
 * pixels, and the towers, the aliens and the fort are sheets of sprites
 * rendered by the same camera. The board composes them (bs_gfx.c): only
 * the 16 x 16 tiles whose content changed are drawn again, straight into
 * the panel's free buffer, half of them on each core, and the buffer is
 * flipped to. Nothing is copied to the panel.
 *
 * Threads: a battle runs on the app's worker (core 0): the simulation in
 * steps of 1/60 s (bs_game.c), the frame's list (bs_scene.c, bs_hud.c),
 * the tiles and the flip. LVGL's thread (core 1) hands it the taps and the
 * gamepad through a ring, plays the sounds it asks for through another,
 * and owns everything LVGL: the menus (bs_menu.c) and the panels over a
 * battle (pause, result). The two never draw at once: while LVGL has
 * something over the battle, or the system does, the worker holds, and
 * LVGL shows the battle's last frame under its panel from a canvas.
 *
 * Files: bs_pak (packs), bs_gfx (the picture), bs_art (the common pack's
 * sheets), bs_world (a world's definition), bs_data (the numbers),
 * bs_game (a battle), bs_scene and bs_hud (a battle on the screen),
 * bs_audio, bs_save, bs_menu.
 */
#include "bs_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS    14
#define PERF_MS     5000
#define WORKER_STACK (24 * 1024)

static void pause_show(bs_app_t *a);
static void result_show(bs_app_t *a);
static void panel_close(bs_app_t *a);

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ---- the rings between the two threads -------------------------------------------------- */

static void input_push(bs_app_t *a, int kind, int x, int y, uint32_t bits)
{
    uint32_t w = a->in_w;
    if (w - a->in_r >= 32) return;
    bs_input_t *e = &a->in[w & 31];
    e->kind = (uint8_t)kind;
    e->x = (int16_t)x;
    e->y = (int16_t)y;
    e->bits = bits;
    a->in_w = w + 1;
}

static void sounds_push(bs_app_t *a, bs_game_t *g)
{
    for (int i = 0; i < g->nsnd; i++) {
        uint32_t w = a->snd_w;
        if (w - a->snd_r >= 64) break;
        a->snd[w & 63] = g->snd[i];
        a->snd_w = w + 1;
    }
    g->nsnd = 0;
}

static void sounds_play(bs_app_t *a)
{
    while (a->snd_r != a->snd_w) {
        bs_audio_play(a->snd[a->snd_r & 63], 230);
        a->snd_r++;
    }
}

/* ---- a frame of the battle ---------------------------------------------------------------- */

static void frame_list(bs_app_t *a)
{
    bs_hud_layout(a->ui, a->game);
    bs_gfx_begin(&a->gfx);
    bs_scene_draw(&a->gfx, &a->art, a->ui, a->game);
    bs_hud_draw(&a->gfx, &a->art, a->ui, a->game);
}

static bs_pbuf_t *pbuf(bs_app_t *a, uint16_t *b)
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

static void perf(bs_app_t *a)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!a->perf_t0) a->perf_t0 = now;
    if (now - a->perf_t0 < PERF_MS || !a->perf_frames) return;
    float s = (float)(now - a->perf_t0) / 1000.0f;
    float n = (float)a->perf_frames;
    aos_hal_log("bastion", "%.1f fps, a frame %.2f ms (sim %.2f, list %.2f, draw %.2f), %u tiles, %u things, "
                "%d aliens, flip %d, %u KB held",
                (double)(n / s), (double)((float)(a->perf_sim_us + a->perf_list_us + a->perf_draw_us) / 1000.0f / n),
                (double)((float)a->perf_sim_us / 1000.0f / n), (double)((float)a->perf_list_us / 1000.0f / n),
                (double)((float)a->perf_draw_us / 1000.0f / n), (unsigned)(a->perf_tiles / a->perf_frames),
                (unsigned)(a->perf_items / a->perf_frames), a->game->alive, a->flip_ok, (unsigned)(bs_mem_used() / 1024));
    a->perf_frames = a->perf_tiles = a->perf_items = a->perf_steps = 0;
    a->perf_sim_us = a->perf_list_us = a->perf_draw_us = 0;
    a->perf_t0 = now;
}

/* one frame: on the worker (two cores), or on LVGL's thread when the
 * worker could not be had (one core) */
static void battle_frame(bs_app_t *a)
{
    bs_game_t *g = a->game;
    bs_ui_t *ui = a->ui;
    uint32_t t0 = (uint32_t)aos_hal_uptime_us();
    float dt = a->clock_reset ? 0.016f : (float)(uint32_t)(t0 - a->last_us) / 1e6f;
    a->clock_reset = false;
    a->last_us = t0;
    if (dt > 0.05f) dt = 0.05f;

    while (a->in_r != a->in_w) {
        const bs_input_t *e = &a->in[a->in_r & 31];
        if (e->kind == IN_TAP) bs_hud_tap(ui, g, e->x, e->y);
        else bs_hud_pad(ui, g, e->bits);
        a->in_r++;
    }
    a->acc += dt * (float)ui->speed;
    int steps = 0;
    while (a->acc >= BS_DT && steps < 9) {
        bs_game_step(g);
        a->acc -= BS_DT;
        steps++;
    }
    if (a->acc > BS_DT) a->acc = 0;
    sounds_push(a, g);
    ui->t += dt;
    if (ui->hint_t > 0) ui->hint_t -= dt;
    if (ui->banner_t > 0) ui->banner_t -= dt;
    if (g->state == BS_WAVE && ui->seen_wave != g->wave) {
        ui->seen_wave = g->wave;
        ui->banner_t = 1.8f;
    }
    /* a goal's banner stays four seconds of the clock's, whatever the speed */
    if (ui->mile_t > 0) ui->mile_t -= dt;
    if (g->miles != ui->seen_miles) {
        ui->seen_miles = g->miles;
        ui->mile_t = 4.0f;
    }
    uint32_t t1 = (uint32_t)aos_hal_uptime_us();
    frame_list(a);
    uint32_t t2 = (uint32_t)aos_hal_uptime_us();

    uint16_t *b = a->flip_ok ? aos_hal_display_back() : NULL;
    int tiles;
    if (b) {
        bs_pbuf_t *p = pbuf(a, b);
        uint32_t since = 0;
        bool touched = true;
        bool known = p && p->flip && aos_hal_display_back_age(b, &since, &touched) && !touched && since != UINT32_MAX &&
                     since == a->flips - p->flip;
        tiles = bs_gfx_end(&a->gfx, b, p ? p->hash : a->fb_hash, !known);
        if (!p) a->fb_full = true;
        if (aos_hal_display_flip(b)) {
            a->flips++;
            if (p) p->flip = a->flips;
        } else if (p) {
            p->flip = 0;
        }
    } else {
        tiles = bs_gfx_end(&a->gfx, a->fb, a->fb_hash, a->fb_full);
        a->fb_full = false;
        a->frame_gen++;
    }
    uint32_t t3 = (uint32_t)aos_hal_uptime_us();
    a->perf_frames++;
    a->perf_tiles += (uint32_t)tiles;
    a->perf_items += (uint32_t)a->gfx.nitem;
    a->perf_steps += (uint32_t)steps;
    a->perf_sim_us += t1 - t0;
    a->perf_list_us += t2 - t1;
    a->perf_draw_us += t3 - t2;
    perf(a);
}

static void worker_fn(void *arg)
{
    bs_app_t *a = (bs_app_t *)arg;
    while (!aos_hal_worker_should_stop()) {
        if (!a->run) {
            aos_hal_worker_sleep(8);
            continue;
        }
        uint32_t t0 = (uint32_t)aos_hal_uptime_us();
        aos_hal_mutex_lock(a->mx);
        if (a->run) battle_frame(a);
        aos_hal_mutex_unlock(a->mx);
        /* sixty frames a second are enough; what is left of the frame's
         * time goes back to the core */
        uint32_t ms = ((uint32_t)aos_hal_uptime_us() - t0) / 1000;
        aos_hal_worker_sleep(ms < 15 ? 16 - ms : 1);
    }
}

/* the worker stops drawing, and the battle's last frame goes to the
 * canvas, for LVGL to show under whatever comes over it */
static void battle_hold(bs_app_t *a)
{
    if (!a->run) return;
    a->run = false;
    aos_hal_mutex_lock(a->mx);
    a->gfx.one_core = true;
    bs_gfx_end(&a->gfx, a->fb, a->fb_hash, a->flip_ok || a->fb_full);
    a->gfx.one_core = a->worker_on ? false : true;
    a->fb_full = false;
    aos_hal_mutex_unlock(a->mx);
    if (a->canvas) lv_obj_invalidate(a->canvas);
}

static void battle_resume(bs_app_t *a)
{
    if (a->run || !a->loaded) return;
    a->clock_reset = true;
    /* in the simulator the canvas is the screen and is up to date; on the
     * board the panel's buffers say themselves that LVGL drew on them */
    a->run = true;
}

/* ---- panels over the battle ------------------------------------------------------------------ */

static lv_obj_t *panel_open(bs_app_t *a, int kind, int w)
{
    panel_close(a);
    battle_hold(a);
    uint32_t acc = a->loaded && a->world.accent ? a->world.accent : BS_ACCENT;
    lv_obj_t *dim = lv_obj_create(a->root);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim, C(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *p = lv_obj_create(dim);
    lv_obj_set_size(p, w, LV_SIZE_CONTENT);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, 32, 0);
    lv_obj_set_style_bg_color(p, C(0x141A28), 0);
    lv_obj_set_style_bg_grad_color(p, C(0x05070C), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 3, 0);
    lv_obj_set_style_border_color(p, C(acc), 0);
    lv_obj_set_style_shadow_width(p, 50, 0);
    lv_obj_set_style_shadow_color(p, C(acc), 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(p, 28, 0);
    lv_obj_set_style_pad_row(p, 16, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    a->panel = dim;
    a->panel_kind = kind;
    bs_audio_pause(true);
    aos_pad_reset(&a->pad, lv_tick_get());
    return p;
}

static void panel_close(bs_app_t *a)
{
    if (!a->panel) return;
    aos_pad_menu_clear(&a->menu);
    lv_obj_delete(a->panel);
    a->panel = NULL;
    a->panel_kind = PN_NONE;
    bs_audio_pause(false);
    aos_pad_reset(&a->pad, lv_tick_get());
}

/* a line of a panel: as wide as the panel, wrapped */
static lv_obj_t *panel_label(lv_obj_t *p, const char *text, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = bs_label(p, text, f, col);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static void pad_menu_from(bs_app_t *a, lv_obj_t *p)
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
    panel_close((bs_app_t *)lv_event_get_user_data(e));
}

static void ev_restart(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    int w = a->sel_world, s = a->sel_sector;
    panel_close(a);
    bs_battle_quit(a);
    bs_battle_start(a, w, s);
}

static void ev_next(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    int w = a->sel_world, s = a->sel_sector + 1;
    if (s >= BS_SECTORS) {
        s = 0;
        w++;
    }
    panel_close(a);
    bs_battle_quit(a);
    if (w < a->ncard && bs_save_open(&a->save, w, s)) bs_battle_start(a, w, s);
    else bs_menu_show(a, ST_MAP);
}

static void ev_map(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    bs_battle_quit(a);
    bs_menu_show(a, ST_MAP);
}

static void ev_lab(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    bs_battle_quit(a);
    a->lab_back = ST_MAP;
    bs_menu_show(a, ST_LAB);
}

static void ev_sfx(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    a->save.sfx = !a->save.sfx;
    bs_audio_set(a->save.sfx, a->save.music);
    bs_save_store(&a->save);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->save.sfx ? _("Efectos: sí") : _("Efectos: no"));
}

static void ev_music(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    a->save.music = !a->save.music;
    bs_audio_set(a->save.sfx, a->save.music);
    bs_save_store(&a->save);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->save.music ? _("Música: sí") : _("Música: no"));
}

/* the mode without end has no last wave: the player leaves when it suits,
 * and what was held counts as if the fort had fallen there */
static void ev_retire(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    result_show(a);
}

static void pause_show(bs_app_t *a)
{
    if (!a->loaded || a->panel) return;
    uint32_t acc = a->world.accent;
    lv_obj_t *p = panel_open(a, PN_PAUSE, 580);
    panel_label(p, _("Pausa"), aos_font_title, acc);
    char line[200];
    const char *wn = a->world.name[bs_lang()][0] ? a->world.name[bs_lang()] : a->world.name[0];
    if (a->mode == BS_MODE_ENDLESS) snprintf(line, sizeof line, "%.40s  ·  %s", wn, _("Supervivencia"));
    else snprintf(line, sizeof line, "%.40s  ·  %s %d%s%s", wn, _("Sector"), a->sel_sector + 1,
                  a->mode == BS_MODE_FREE ? "  ·  " : "", a->mode == BS_MODE_FREE ? _("Libre") : "");
    panel_label(p, line, aos_font_small, 0xB0B8C8);
    bs_button(p, _("Seguir"), acc, true, 460, 96, ev_resume, a);
    bs_button(p, _("Empezar de nuevo"), acc, false, 460, 84, ev_restart, a);
    bs_button(p, a->save.sfx ? _("Efectos: sí") : _("Efectos: no"), acc, false, 460, 84, ev_sfx, a);
    bs_button(p, a->save.music ? _("Música: sí") : _("Música: no"), acc, false, 460, 84, ev_music, a);
    if (a->mode == BS_MODE_ENDLESS) {
        char goal[120];
        int step = a->game->mile_step > 0 ? a->game->mile_step : BS_MILE_STEP;
        snprintf(goal, sizeof goal, _("Próxima meta: oleada %d"), (a->game->wave / step + 1) * step);
        panel_label(p, goal, aos_font_small, 0xFFD23C);
        bs_button(p, _("Retirarse y cobrar"), acc, false, 460, 84, ev_retire, a);
    } else {
        bs_button(p, _("Salir al mapa"), acc, false, 460, 84, ev_map, a);
    }
    pad_menu_from(a, p);
}

/* what a battle leaves: stars, cores for the laboratory, experience.
 * The harder purse pays more cores and the easier one fewer; the mode
 * without end pays by the waves held and keeps the record; the free mode
 * leaves nothing. */
static void result_take(bs_app_t *a)
{
    bs_game_t *g = a->game;
    int w = a->sel_world, s = a->sel_sector;
    int level = w * 3 + s;
    static const int PAY[BS_DIFF_N] = { 100, 80, 130 };
    int pay = PAY[a->save.diff < BS_DIFF_N ? a->save.diff : 0];
    a->res_rank = bs_rank_of(a->save.xp);
    a->res_new = 0;
    a->res_cores = 0;
    a->res_record = false;
    if (a->mode == BS_MODE_FREE) {
        /* what was seen is seen, in any mode */
        if (g->seen & ~a->save.seen) {
            a->save.seen |= g->seen;
            bs_save_store(&a->save);
        }
        aos_hal_log("bastion", "%s sector %d, free mode: wave %d/%d, %d lives of %d, %d kills", a->world.id, s + 1, g->wave,
                    g->nwaves, g->lives, g->max_lives, g->kills);
        return;
    }
    if (a->mode == BS_MODE_ENDLESS) {
        /* the waves held, and what the goals reached paid */
        int held = g->wave;
        a->res_cores = (held * (2 + w) / 4 + g->cores_won) * pay / 100;
        if (held > a->save.best[w]) {
            a->save.best[w] = (uint16_t)(held > 65535 ? 65535 : held);
            a->res_record = true;
        }
    } else if (g->state == BS_WON) {
        int old = a->save.stars[w][s];
        if (g->stars > old) {
            a->res_new = g->stars - old;
            a->save.stars[w][s] = (uint8_t)g->stars;
        }
        a->res_cores = (4 + level / 2 + a->res_new * (6 + w)) * pay / 100;
        a->save.wins++;
    } else {
        a->res_cores = g->wave / 2;
    }
    /* a run without end kills thousands: a third of its experience, or
     * two of them would be the whole of the ranks (the user's 49 waves
     * were 6379, a campaign sector is some 500) */
    if (a->mode == BS_MODE_ENDLESS) g->xp /= 3;
    a->save.cores += a->res_cores;
    a->save.xp += g->xp;
    a->save.kills += g->kills;
    a->save.seen |= g->seen;
    bs_save_store(&a->save);
    aos_hal_log("bastion", "%s sector %d mode %d purse %d %s: wave %d/%d, %d lives of %d, %d kills, +%d cores, +%d xp",
                a->world.id, s + 1, a->mode, a->save.diff,
                g->state == BS_WON ? "won" : g->state == BS_LOST ? "lost" : "left", g->wave,
                a->mode == BS_MODE_ENDLESS ? 0 : g->nwaves, g->lives, g->max_lives, g->kills, a->res_cores, g->xp);
}

static void result_show(bs_app_t *a)
{
    if (a->panel || a->result_done) return;
    a->result_done = true;
    result_take(a);
    bs_game_t *g = a->game;
    bool won = g->state == BS_WON;
    bool endless = a->mode == BS_MODE_ENDLESS, freem = a->mode == BS_MODE_FREE;
    uint32_t acc = a->world.accent;
    lv_obj_t *p = panel_open(a, PN_RESULT, 600);
    panel_label(p, endless ? _("Fin de la supervivencia") : won ? _("¡Victoria!") : _("El fuerte cayó"), aos_font_title,
                won || endless ? 0xFFD23C : 0xFF5040);
    char line[160];
    if (won || (endless && g->miles > 0)) {
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 10, 0);
        /* the stars, from the pack (the menus' pictures went when the
         * battle loaded; these two are small) */
        static const char *const STAR[2] = { "ic_star_off", "ic_star" };
        for (int k = 0; k < 2; k++) {
            if (!a->img[30 + k]) a->img[30 + k] = bs_pak_image(&a->art.pak, STAR[k], &a->img_dsc[30 + k]);
        }
        for (int i = 0; i < 3; i++) {
            int k = i < g->stars;
            if (a->img[30 + k]) {
                lv_obj_t *box = lv_obj_create(row);
                lv_obj_remove_style_all(box);
                lv_obj_set_size(box, 96, 96);
                lv_obj_t *st = lv_image_create(box);
                lv_image_set_src(st, &a->img_dsc[30 + k]);
                lv_image_set_scale(st, 800);
                lv_obj_center(st);
            } else {
                lv_obj_t *st = lv_label_create(row);
                lv_label_set_text(st, LV_SYMBOL_OK);
                lv_obj_set_style_text_font(st, aos_font_huge, 0);
                lv_obj_set_style_text_color(st, C(k ? 0xFFD23C : 0x3A4050), 0);
            }
        }
        if (endless) snprintf(line, sizeof line, _("Oleadas aguantadas: %d"), g->wave);
        else snprintf(line, sizeof line, _("Vida del fuerte: %d de %d"), g->lives, g->max_lives);
    } else if (endless) {
        snprintf(line, sizeof line, _("Oleadas aguantadas: %d"), g->wave);
    } else {
        snprintf(line, sizeof line, _("Llegaste a la oleada %d de %d"), g->wave + 1, g->nwaves);
    }
    panel_label(p, line, aos_font_body, 0xE0E4EC);
    if (endless) {
        if (a->res_record) snprintf(line, sizeof line, "%s", _("¡Récord nuevo!"));
        else snprintf(line, sizeof line, _("Tu récord en este mundo: %d"), (int)a->save.best[a->sel_world]);
        panel_label(p, line, aos_font_body, a->res_record ? 0x60FF90 : 0xB0B8C8);
    }
    if (freem) {
        snprintf(line, sizeof line, _("Bajas: %d"), g->kills);
        panel_label(p, line, aos_font_small, 0xB0B8C8);
        panel_label(p, _("El modo libre no da núcleos ni experiencia."), aos_font_small, 0xB0B8C8);
    } else {
        snprintf(line, sizeof line, _("Bajas: %d    Núcleos: +%d    Experiencia: +%d"), g->kills, a->res_cores, g->xp);
        panel_label(p, line, aos_font_small, 0xB0B8C8);
    }
    int rank = bs_rank_of(a->save.xp);
    if (rank > a->res_rank) {
        snprintf(line, sizeof line, _("¡Subiste al rango %d! Tus torres pegan un 1 %% más."), rank);
        panel_label(p, line, aos_font_small, 0x60FF90);
    }
    if (!won && !endless && !freem)
        panel_label(p, _("Gastá los núcleos en el laboratorio y volvé más fuerte."), aos_font_small, 0xB0B8C8);
    int w = a->sel_world, s = a->sel_sector + 1;
    if (s >= BS_SECTORS) {
        s = 0;
        w++;
    }
    bool campaign = a->mode == BS_MODE_CAMPAIGN;
    if (campaign && won && w < a->ncard) {
        bs_button(p, s ? _("Sector siguiente") : _("Mundo siguiente"), acc, true, 480, 96, ev_next, a);
        bs_button(p, _("Laboratorio"), acc, false, 480, 84, ev_lab, a);
        bs_button(p, _("Otra vez"), acc, false, 480, 84, ev_restart, a);
    } else {
        bs_button(p, _("Otra vez"), acc, true, 480, 96, ev_restart, a);
        bs_button(p, _("Laboratorio"), acc, false, 480, 84, ev_lab, a);
    }
    bs_button(p, _("Mapa"), acc, false, 480, 84, ev_map, a);
    pad_menu_from(a, p);
    bs_audio_pause(false);
}

/* ---- a battle comes and goes ---------------------------------------------------------------- */

void bs_battle_quit(bs_app_t *a)
{
    a->run = false;
    if (a->worker_on) {
        aos_hal_worker_stop();
        a->worker_on = false;
    }
    /* a battle left half way still showed its aliens: the gallery keeps them */
    if (a->loaded && (a->game->seen & ~a->save.seen)) {
        a->save.seen |= a->game->seen;
        bs_save_store(&a->save);
    }
    aos_hal_display_refresh_request(0);
    aos_ui_wall_photo_spare(false);
    panel_close(a);
    if (a->canvas) lv_obj_delete(a->canvas);
    if (a->touch) lv_obj_delete(a->touch);
    a->canvas = a->touch = NULL;
    bs_gfx_world(&a->gfx, NULL, NULL);
    bs_free(a->bg);
    bs_free(a->depth);
    bs_free(a->fb);
    a->bg = a->depth = a->fb = NULL;
    bs_world_free(&a->world);
    bs_pak_close(&a->wpak);
    bs_art_battle_free(&a->art);
    for (int i = 0; i < 3; i++) a->pb[i].buf = NULL;
    a->loaded = false;
    a->in_r = a->in_w;
    a->snd_r = a->snd_w;
}

static void ev_touch(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || !a->run) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);
    input_push(a, IN_TAP, pt.x - rc.x1, pt.y - rc.y1, 0);
}

static bool battle_load(bs_app_t *a, int wi, int sector)
{
    bs_card_t *c = &a->card[wi];
    if (!bs_pak_open(&a->wpak, c->path)) return false;
    char *txt = (char *)bs_pak_get(&a->wpak, "def", NULL);
    bool ok = txt && bs_world_parse(&a->world, txt, false);
    bs_free(txt);
    if (!ok) return false;
    const bs_entry_t *e;
    a->bg = (uint16_t *)bs_pak_get(&a->wpak, "bg", &e);
    if (!a->bg || e->w != BS_W || e->h != BS_H) return false;
    a->depth = (uint16_t *)bs_pak_get(&a->wpak, "depth", &e);
    if (a->depth && (e->w != BS_W || e->h != BS_H)) {
        bs_free(a->depth);
        a->depth = NULL;
    }
    a->fb = (uint16_t *)bs_malloc((size_t)BS_W * BS_H * 2);
    if (!a->fb) return false;
    bs_kit_t kit;
    bs_save_kit(&a->save, &kit);
    kit.mode = (uint8_t)a->mode;
    bool all = a->mode == BS_MODE_FREE;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    if (getenv("BS_ALL")) all = true;
#endif
    if (all) {
        /* the free mode: every tower and power, whatever the laboratory has */
        memset(kit.owned, 1, sizeof kit.owned);
        for (int i = 0; i < PW_N; i++) {
            if (!kit.power[i]) kit.power[i] = 1;
        }
    }
    /* without end, any alien of the world may come, and its boss */
    uint32_t aliens = bs_world_aliens(&a->world, sector);
    if (a->mode == BS_MODE_ENDLESS) {
        for (int k = 0; k < BS_SECTORS; k++) aliens |= bs_world_aliens(&a->world, k);
        aliens |= 1u << a->world.boss;
        if (a->world.boss == AL_QUEEN) aliens |= 1u << AL_GRUB;
    }
    if (!bs_art_battle(&a->art, aliens, kit.owned, a->world.hue) || a->wpak.fails) {
        a->no_memory = true;
        return false;
    }
    bs_gfx_world(&a->gfx, a->bg, a->depth);
    bs_game_start(a->game, &a->world, sector, &kit);
    bs_ui_init(a->ui);
    bs_hud_strings(a->ui);
    a->ui->speed = a->save.speed >= 1 && a->save.speed <= 3 ? a->save.speed : 1;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    if (getenv("BS_CREDITS")) a->game->credits = atoi(getenv("BS_CREDITS"));
    /* BS_MILE=<n>: a goal every n waves of the mode without end (to see one) */
    if (getenv("BS_MILE") && atoi(getenv("BS_MILE")) > 0 && a->game->mile_step) a->game->mile_step = atoi(getenv("BS_MILE"));
    /* BS_SHORT=<n>: the sector ends after n waves (to see a victory) */
    if (getenv("BS_SHORT") && atoi(getenv("BS_SHORT")) > 0 && atoi(getenv("BS_SHORT")) < a->game->nwaves)
        a->game->nwaves = atoi(getenv("BS_SHORT"));
#endif
    bs_audio_music(&a->art.pak, a->world.music);
    return true;
}

static void load_cb(lv_timer_t *tm)
{
    bs_app_t *a = (bs_app_t *)lv_timer_get_user_data(tm);
    lv_timer_delete(tm);
    a->load_timer = NULL;
    /* the menus' pictures go before the battle's come in */
    bs_menu_free(a);
    a->no_memory = false;
    aos_ui_wall_photo_spare(true);
    if (!battle_load(a, a->sel_world, a->sel_sector)) {
        bool nomem = a->no_memory;
        aos_hal_log("bastion", "cannot load %s", a->card[a->sel_world].path);
        bs_battle_quit(a);
        if (a->loading) lv_obj_delete(a->loading);
        a->loading = NULL;
        bs_menu_show(a, ST_MAP);
        aos_ui_toast(nomem ? _("No hay memoria para la batalla: cerrá otras apps y probá de nuevo")
                           : _("No se pudo cargar el mundo"), 4000);
        return;
    }
    if (a->loading) lv_obj_delete(a->loading);
    a->loading = NULL;
    lv_obj_set_style_bg_color(a->root, C(0x000000), 0);
    lv_obj_set_style_bg_opa(a->root, LV_OPA_COVER, 0);
    a->canvas = lv_canvas_create(a->root);
    lv_canvas_set_buffer(a->canvas, a->fb, BS_W, BS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    a->touch = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(a->touch, ev_touch, LV_EVENT_PRESSED, a);

    a->flip_ok = aos_hal_display_back() != NULL;
    a->flips = 0;
    for (int i = 0; i < 3; i++) {
        a->pb[i].buf = NULL;
        a->pb[i].flip = 0;
    }
    a->loaded = true;
    a->result_done = false;
    a->acc = 0;
    a->in_r = a->in_w;
    a->snd_r = a->snd_w;
    a->perf_t0 = 0;
    a->perf_frames = 0;
    /* the first frame now, on this thread, so whatever opens over the
     * battle has it under it */
    a->gfx.one_core = true;
    frame_list(a);
    bs_gfx_end(&a->gfx, a->fb, a->fb_hash, true);
    a->fb_full = false;
    lv_obj_invalidate(a->canvas);
    aos_hal_display_refresh_request(60);
    a->worker_on = aos_hal_worker_start_on("bastion", worker_fn, a, WORKER_STACK, 0, 3);
    a->gfx.one_core = !a->worker_on;
    if (!a->worker_on) aos_hal_log("bastion", "no worker: the frames on one core");
    a->state = ST_BATTLE;
    a->clock_reset = true;
    a->run = true;
    aos_hal_log("bastion", "%s sector %d, mode %d, purse %d: %d waves, %d pads, %u KB held, depth %d", a->world.id,
                a->sel_sector + 1, a->mode, a->save.diff, a->mode == BS_MODE_ENDLESS ? 0 : a->game->nwaves, a->world.npad,
                (unsigned)(bs_mem_used() / 1024), a->depth != NULL);
}

void bs_battle_start(bs_app_t *a, int world, int sector)
{
    if (world < 0 || world >= a->ncard || a->state == ST_LOADING) return;
    a->sel_world = world;
    a->sel_sector = sector;
    a->state = ST_LOADING;
    const bs_world_t *w = &a->card[world].head;
    a->loading = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->loading);
    lv_obj_set_size(a->loading, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(a->loading, C(0x05070C), 0);
    lv_obj_set_style_bg_opa(a->loading, LV_OPA_COVER, 0);
    lv_obj_t *l = bs_label(a->loading, w->name[bs_lang()][0] ? w->name[bs_lang()] : w->name[0], aos_font_title, w->accent);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -40);
    char line[48];
    if (a->mode == BS_MODE_ENDLESS) snprintf(line, sizeof line, "%s", _("Supervivencia"));
    else snprintf(line, sizeof line, "%s %d", _("Sector"), sector + 1);
    l = bs_label(a->loading, line, aos_font_body, 0xE0E4EC);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 30);
    l = bs_label(a->loading, _("Desplegando defensas..."), aos_font_small, 0x8088A0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 90);
    a->load_timer = lv_timer_create(load_cb, 60, a);
}

/* ---- LVGL's side of a battle ------------------------------------------------------------------ */

static void battle_tick(bs_app_t *a)
{
    uint32_t over = aos_ui_overlay();
    bs_game_t *g = a->game;
    if (over) {
        /* the system is over us: under a panel, the switcher or the lock
         * the battle pauses; a banner or a toast only holds it */
        const uint32_t PAUSING = AOS_UI_OVER_PANEL | AOS_UI_OVER_SWITCHER | AOS_UI_OVER_GESTURE | AOS_UI_OVER_NOT_FRONT |
                                 AOS_UI_OVER_LOCK;
        if ((over & PAUSING) && !a->panel && (g->state == BS_BUILD || g->state == BS_WAVE)) pause_show(a);
        battle_hold(a);
        a->over_seen = true;
        return;
    }
    a->over_seen = false;
    if (a->panel) {
        if (a->panel_kind == PN_PAUSE && aos_pad_pressed(&a->pad, AOS_PAD_B | AOS_PAD_START)) {
            panel_close(a);
            return;
        }
        aos_pad_menu_step(&a->menu, &a->pad);
        return;
    }
    if (!a->run) battle_resume(a);
    /* the gamepad: START pauses, the rest is the battle's */
    if (aos_pad_pressed(&a->pad, AOS_PAD_START) || a->button_pause || a->ui->want_pause) {
        a->button_pause = false;
        a->ui->want_pause = false;
        if (g->state == BS_BUILD || g->state == BS_WAVE) {
            pause_show(a);
            return;
        }
    }
    uint32_t bits = (a->pad.repeat & AOS_PAD_DIRS) | (a->pad.pressed & ~(uint32_t)(AOS_PAD_DIRS | AOS_PAD_START));
    if (bits) input_push(a, IN_PAD, 0, 0, bits);
    sounds_play(a);
    if (!a->worker_on) battle_frame(a);
    if (a->frame_seen != a->frame_gen) {
        a->frame_seen = a->frame_gen;
        if (a->canvas) lv_obj_invalidate(a->canvas);
    }
    if ((g->state == BS_WON || g->state == BS_LOST) && g->state_t > 2.4f) {
        if (a->ui->speed != a->save.speed) a->save.speed = (uint8_t)a->ui->speed;
        result_show(a);
    }
}

static void timer_cb(lv_timer_t *tm)
{
    bs_app_t *a = (bs_app_t *)lv_timer_get_user_data(tm);
    aos_pad_update(&a->pad, lv_tick_get());
    if (a->state == ST_BATTLE && a->loaded) battle_tick(a);
    else if (a->state != ST_LOADING) bs_menu_pad(a);
    bs_audio_pump();
}

/* ---- life cycle --------------------------------------------------------------------------------- */

static void *bastion_create(aos_app_t *self, lv_obj_t *root)
{
    bs_app_t *a = (bs_app_t *)bs_calloc(sizeof(bs_app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->game = (bs_game_t *)bs_calloc(sizeof(bs_game_t));
    a->ui = (bs_ui_t *)bs_calloc(sizeof(bs_ui_t));
    a->fb_hash = (uint32_t *)bs_calloc(sizeof(uint32_t) * BS_NT);
    for (int i = 0; i < 3; i++) a->pb[i].hash = (uint32_t *)bs_calloc(sizeof(uint32_t) * BS_NT);
    a->mx = aos_hal_mutex_create();
    bool ok = a->game && a->ui && a->fb_hash && a->pb[0].hash && a->pb[1].hash && a->pb[2].hash && a->mx &&
              bs_gfx_init(&a->gfx);
    lv_obj_set_style_bg_color(root, C(0x05070C), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    bs_save_load(&a->save);
    a->mode = a->save.mode;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    if (getenv("BS_CORES")) a->save.cores = atoi(getenv("BS_CORES"));
#endif
    char path[200];
    snprintf(path, sizeof path, "%s/bastion_p4.pak", aos_hal_path_apps() ? aos_hal_path_apps() : ".");
    a->art_ok = ok && bs_art_open(&a->art, path);
    if (a->art_ok) {
        bs_audio_set(a->save.sfx, a->save.music);
        bs_audio_open(&a->art.pak);
        bs_cards_load(a);
        bs_seen_from_progress(a);
    }
    bs_menu_show(a, ST_TITLE);
    a->timer = lv_timer_create(timer_cb, FRAME_MS, a);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* BS_WORLD=<n> [BS_SECTOR=<k>] opens that battle straight away (scripts) */
    if (getenv("BS_MODE")) a->mode = atoi(getenv("BS_MODE")) % BS_MODE_N;
    if (getenv("BS_DIFF")) a->save.diff = (uint8_t)(atoi(getenv("BS_DIFF")) % BS_DIFF_N);
    if (getenv("BS_WORLD") && a->ncard) {
        bs_battle_start(a, atoi(getenv("BS_WORLD")) % a->ncard, getenv("BS_SECTOR") ? atoi(getenv("BS_SECTOR")) % 3 : 0);
    }
#endif
    return a;
}

static void bastion_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    bs_app_t *a = (bs_app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    if (a->load_timer) lv_timer_delete(a->load_timer);
    a->timer = a->load_timer = NULL;
    bs_battle_quit(a);
    bs_menu_free(a);
    bs_cards_free(a);
    bs_audio_close();
    bs_art_close(&a->art);
    bs_gfx_free(&a->gfx);
    for (int i = 0; i < 3; i++) bs_free(a->pb[i].hash);
    bs_free(a->fb_hash);
    bs_free(a->game);
    bs_free(a->ui);
    /* the mutex stays: the HAL has no call to give one back, and it is a
     * few bytes an opening */
    bs_free(a);
}

static void bastion_show(aos_app_t *self, void *inst)
{
    (void)self;
    bs_app_t *a = (bs_app_t *)inst;
    if (!a) return;
    /* the catalogue may have been read again while we were away */
    if (a->loaded && !a->run) bs_hud_strings(a->ui);
    bs_audio_pause(a->panel != NULL && a->panel_kind == PN_PAUSE);
}

static void bastion_hide(aos_app_t *self, void *inst)
{
    (void)self;
    bs_app_t *a = (bs_app_t *)inst;
    if (!a) return;
    if (a->state == ST_BATTLE && a->loaded) {
        if (!a->panel && (a->game->state == BS_BUILD || a->game->state == BS_WAVE)) pause_show(a);
        battle_hold(a);
    }
    bs_audio_pause(true);
}

static bool bastion_back(aos_app_t *self, void *inst)
{
    (void)self;
    bs_app_t *a = (bs_app_t *)inst;
    if (!a) return false;
    if (a->state == ST_LOADING) return true;
    if (a->state == ST_BATTLE) {
        if (a->panel_kind == PN_PAUSE) panel_close(a);
        else if (!a->panel) pause_show(a);
        return true;
    }
    return bs_menu_back(a);
}

static bool bastion_button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    bs_app_t *a = (bs_app_t *)inst;
    if (!a || a->state != ST_BATTLE || action != AOS_BUTTON_CLICK) return false;
    a->button_pause = true;
    return true;
}

/* a fort's keep under its shield, and three lit shots going out */
static const uint8_t BASTION_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, -6, 60, 60, AIC_CIRCLE, AIC_C_LIT(0x38D8FF), 70),
    AIC_BORDER(3, AIC_C_LIT(0x38D8FF), 255),
    AIC_RECT(AIC_CENTER, 0, 14, 44, 26, 6, AIC_C_LIT(0x5D6883), 255),
    AIC_GRAD(AIC_C_LIT(0x2A3044), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER, 0, -8, 26, 30, 5, AIC_C_LIT(0x8A96B4), 255),
    AIC_GRAD(AIC_C_LIT(0x3A4256), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER, 0, -10, 12, 12, AIC_CIRCLE, AIC_C_LIT(0xFFD23C), 255),
    AIC_RECT(AIC_CENTER, -30, -30, 9, 9, AIC_CIRCLE, AIC_C_LIT(0xFF3BD4), 255),
    AIC_RECT(AIC_CENTER, 31, -26, 9, 9, AIC_CIRCLE, AIC_C_LIT(0x9BE830), 255),
    AIC_RECT(AIC_CENTER, 22, 34, 8, 8, AIC_CIRCLE, AIC_C_LIT(0xFF7A1A), 255),
    AIC_END
};

static bool bastion_init(aos_app_t *app)
{
    app->desc.id       = "demo.bastion";
    app->desc.name     = "Bastión";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x1B3A6E;
    app->desc.color_b  = 0x070A18;
    app->desc.order    = 159;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_PORTRAIT |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    aos_icon_set_ops(app, BASTION_ICON, sizeof BASTION_ICON);
    app->create  = bastion_create;
    app->destroy = bastion_destroy;
    app->show    = bastion_show;
    app->hide    = bastion_hide;
    app->back    = bastion_back;
    app->button  = bastion_button;
    return true;
}

AOS_APP_ENTRY(bastion_init);
