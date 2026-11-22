/*
 * METEGOL - table football, lying down: one against the board or two on
 * the same screen, national teams and clubs of Argentina, ten stadiums
 *
 * The art is 3D, modelled by code in Blender and rendered ahead
 * (tools/blender): each stadium is one picture with the table in it, and
 * the figures, the rods and the ball are sprites from the same camera.
 * The figures are rendered once with masks and painted in each team's kit
 * when a match loads (mg_kit.c). The board composes the frame (mg_gfx.c,
 * Bastion's compositor turned for the panel lying down): only the 16 x 16
 * tiles whose content changed are drawn again, straight into the panel's
 * free buffer, half of them on each core, and the buffer is flipped to.
 *
 * Threads: the app's worker (core 0) loads the match's art (it decodes a
 * JPEG) and then runs the match: the physics in steps of 1/240 s
 * (mg_game.c, mg_ai.c), the frame's list (mg_scene.c), the tiles and the
 * flip. LVGL's thread reads the fingers and the gamepads (mg_input.c),
 * plays the sounds the worker asks for, and owns the menus (mg_menu.c)
 * and the panels over a match. While LVGL or the system has something
 * over the match the worker holds, and LVGL shows the match's last frame
 * under it from a canvas.
 */
#include "mg_app.h"
#include "mg_view.h"

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
#define ACCENT       0x2FBF5A

void *mg_in_mx;

static void pause_show(mg_app_t *a);
static void result_show(mg_app_t *a);
static void panel_close(mg_app_t *a);

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ---- the settings ------------------------------------------------------------------------------------- */

static void cfg_path(char *p, size_t n)
{
    const char *d = aos_hal_path_data();
    snprintf(p, n, "%.120s/metegol/ajustes.txt", d ? d : ".");
}

static void cfg_defaults(mg_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->diff = MG_NORMAL;
    c->sfx = c->music = c->crowd = 1;
    /* the shake redraws the whole screen for a moment: off unless asked for */
    c->shake = 0;
    c->speed = 1;
    c->league = MG_LEAGUE_CLUB;
    c->format = FMT_TIME;
    c->minutes = 3;
    c->goals = 5;
    c->swap = 1;
    snprintf(c->team[0], sizeof c->team[0], "boca");
    snprintf(c->team[1], sizeof c->team[1], "river");
}

static const struct {
    const char *k;
    size_t off;
} INTS[] = {
    { "dificultad", offsetof(mg_cfg_t, diff) }, { "sonido", offsetof(mg_cfg_t, sfx) },
    { "musica", offsetof(mg_cfg_t, music) }, { "hinchada", offsetof(mg_cfg_t, crowd) },
    { "vibracion", offsetof(mg_cfg_t, shake) }, { "varilla_sola", offsetof(mg_cfg_t, one_rod) },
    { "lado_j1", offsetof(mg_cfg_t, p1_side) }, { "velocidad", offsetof(mg_cfg_t, speed) },
    { "ayuda", offsetof(mg_cfg_t, assist) }, { "idioma_marcador", offsetof(mg_cfg_t, board_lang) },
    { "modo", offsetof(mg_cfg_t, mode) }, { "liga", offsetof(mg_cfg_t, league) },
    { "formato", offsetof(mg_cfg_t, format) }, { "minutos", offsetof(mg_cfg_t, minutes) },
    { "goles", offsetof(mg_cfg_t, goals) }, { "cambio_de_lado", offsetof(mg_cfg_t, swap) },
};

void mg_cfg_load(mg_cfg_t *c)
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
        if (!strcmp(line, "local")) snprintf(c->team[0], sizeof c->team[0], "%.19s", v);
        else if (!strcmp(line, "visitante")) snprintf(c->team[1], sizeof c->team[1], "%.19s", v);
        else if (!strcmp(line, "estadio")) snprintf(c->stadium, sizeof c->stadium, "%.23s", v);
    }
    fclose(f);
    if (c->diff < 0 || c->diff >= MG_DIFF_N) c->diff = MG_NORMAL;
    if (c->minutes < 1 || c->minutes > 10) c->minutes = 3;
    if (c->goals != 3 && c->goals != 5 && c->goals != 7 && c->goals != 10) c->goals = 5;
    if (c->speed < 0 || c->speed > 2) c->speed = 1;
    if (c->board_lang < 0 || c->board_lang > 2) c->board_lang = 0;
    c->league = c->league ? MG_LEAGUE_CLUB : MG_LEAGUE_NAT;
}

void mg_cfg_save(const mg_cfg_t *c)
{
    char p[160];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    char line[96];
    fputs("# Metegol: los ajustes y el último partido (la app lo reescribe)\n", f);
    for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) {
        snprintf(line, sizeof line, "%s=%d\n", INTS[i].k, *(const int *)((const char *)c + INTS[i].off));
        fputs(line, f);
    }
    snprintf(line, sizeof line, "local=%.19s\nvisitante=%.19s\nestadio=%.23s\n", c->team[0], c->team[1], c->stadium);
    fputs(line, f);
    fclose(f);
}

int mg_lang(void)
{
    const char *l = aos_i18n_current();
    if (!l || !l[0] || !strncmp(l, "es", 2)) return 0;
    if (!strncmp(l, "de", 2)) return 2;
    return 1;
}

int mg_team_of_side(const mg_app_t *a, int side)
{
    int sw = a->game ? a->game->swapped : 0;
    return (side ^ a->cfg.p1_side ^ sw) & 1;
}

int mg_side_of_team(const mg_app_t *a, int team)
{
    int sw = a->game ? a->game->swapped : 0;
    return (team ^ a->cfg.p1_side ^ sw) & 1;
}

/* ---- the worker's frame ----------------------------------------------------------------------------------- */

static void sounds_push(mg_app_t *a, mg_game_t *g)
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

static mg_pbuf_t *pbuf(mg_app_t *a, uint16_t *b)
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

static void perf(mg_app_t *a)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!a->perf_t0) a->perf_t0 = now;
    if (now - a->perf_t0 < PERF_MS || !a->perf_frames) return;
    float s = (float)(now - a->perf_t0) / 1000.0f, n = (float)a->perf_frames;
    aos_hal_log("metegol", "%.1f fps, a frame %.2f ms (sim %.2f, list %.2f, draw %.2f), %u tiles, %u things, flip %d, %u KB held, ball %d,%d state %d",
                (double)(n / s), (double)((float)(a->perf_sim_us + a->perf_list_us + a->perf_draw_us) / 1000.0f / n),
                (double)((float)a->perf_sim_us / 1000.0f / n), (double)((float)a->perf_list_us / 1000.0f / n),
                (double)((float)a->perf_draw_us / 1000.0f / n), (unsigned)(a->perf_tiles / a->perf_frames),
                (unsigned)(a->perf_items / a->perf_frames), a->flip_ok, (unsigned)(mg_mem_used() / 1024),
                (int)a->game->bx, (int)a->game->by, a->game->state);
    a->perf_frames = a->perf_tiles = a->perf_items = 0;
    a->perf_sim_us = a->perf_list_us = a->perf_draw_us = 0;
    a->perf_t0 = now;
}

static void match_frame(mg_app_t *a)
{
    mg_game_t *g = a->game;
    uint32_t t0 = (uint32_t)aos_hal_uptime_us();
    float dt = a->clock_reset ? 0.016f : (float)(uint32_t)(t0 - a->last_us) / 1e6f;
    a->clock_reset = false;
    a->last_us = t0;
    if (dt > 0.05f) dt = 0.05f;

    if (g->state == MG_ST_REPLAY) {
        /* the goal again, slower */
        g->rp_t += dt * 0.6f;
        int k = (int)(g->rp_t * 60.0f);
        aos_hal_mutex_lock(mg_in_mx);
        bool skip = a->skip_replay;
        a->skip_replay = false;
        aos_hal_mutex_unlock(mg_in_mx);
        if (skip || k >= g->rp_len) mg_replay_end(g);
        else mg_replay_apply(g, k);
    } else {
        a->acc += dt;
        int steps = 0;
        aos_hal_mutex_lock(mg_in_mx);
        while (a->acc >= MG_DT && steps < 24) {
            mg_hand_t h[2];
            const mg_hand_t *hp[2] = { NULL, NULL };
            for (int s = 0; s < 2; s++) {
                int team = mg_team_of_side(a, s);
                bool human = a->cfg.mode == MODE_TWO || team == 0;
                if (!human || g->ai_on[s]) continue;
                mg_input_hand(a, team, &h[s], steps == 0);
                hp[s] = &h[s];
            }
            mg_game_step(g, hp[0], hp[1]);
            sounds_push(a, g);
            a->acc -= MG_DT;
            steps++;
        }
        aos_hal_mutex_unlock(mg_in_mx);
        if (a->acc > MG_DT * 4) a->acc = 0;
    }
    uint32_t t1 = (uint32_t)aos_hal_uptime_us();
    mg_scene_draw(a, dt);
    uint32_t t2 = (uint32_t)aos_hal_uptime_us();
    uint16_t *b = a->flip_ok ? aos_hal_display_back() : NULL;
    int tiles;
    if (b) {
        mg_pbuf_t *p = pbuf(a, b);
        uint32_t since = 0;
        bool touched = true;
        bool known = p && p->flip && aos_hal_display_back_age(b, &since, &touched) && !touched && since != UINT32_MAX &&
                     since == a->flips - p->flip;
        tiles = mg_gfx_end(&a->gfx, b, a->out, p ? p->hash : a->fb_hash, !known);
        if (!p) a->fb_full = true;
        if (aos_hal_display_flip(b)) {
            a->flips++;
            if (p) p->flip = a->flips;
        } else if (p) {
            p->flip = 0;
        }
    } else {
        tiles = mg_gfx_end(&a->gfx, a->fb, MG_OUT_SCREEN, a->fb_hash, a->fb_full);
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

/* the match's art: the stadium (a JPEG) and the two teams painted */
static bool job_load(mg_app_t *a)
{
    bool ok = mg_art_stadium(&a->art, a->st_ix) && mg_art_teams(&a->art, &a->tm[0], &a->tm[1]);
    if (ok) mg_gfx_world(&a->gfx, a->art.bg);
    return ok;
}

/* a still of the table for the menus: the teams at kick off */
static bool job_still(mg_app_t *a)
{
    if (!job_load(a)) return false;
    mg_game_t *g = a->game;
    g->rnd = 0x1234;
    g->ball_speed = 1.0f;
    mg_game_start(g, 3, 0, false, false, 0, false, 0);
    g->ball_in = true;
    g->bx = 0;
    g->by = 0;
    g->nsnd = 0;
    a->ui->crowd_on = true;
    a->ui->hud_off = true;
    a->ui->hint_t = 0;
    mg_scene_strings(a);
    a->gfx.one_core = !a->worker_on;
    mg_scene_draw(a, 0.016f);
    mg_gfx_end(&a->gfx, a->fb, MG_OUT_SCREEN, a->fb_hash, true);
    a->frame_gen++;
    return true;
}

static void do_job(mg_app_t *a)
{
    int j = a->job;
    if (!j) return;
    aos_hal_mutex_lock(a->mx);
    bool ok = j == 1 ? job_load(a) : job_still(a);
    aos_hal_mutex_unlock(a->mx);
    a->job = 0;
    a->job_done = ok ? 1 : -1;
}

static void worker_fn(void *arg)
{
    mg_app_t *a = (mg_app_t *)arg;
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
static void match_hold(mg_app_t *a)
{
    if (!a->run) return;
    a->run = false;
    aos_hal_mutex_lock(a->mx);
    a->gfx.one_core = true;
    mg_gfx_end(&a->gfx, a->fb, MG_OUT_SCREEN, a->fb_hash, a->flip_ok || a->fb_full);
    a->gfx.one_core = !a->worker_on;
    a->fb_full = false;
    aos_hal_mutex_unlock(a->mx);
    if (a->canvas) lv_obj_invalidate(a->canvas);
}

static void match_resume(mg_app_t *a)
{
    if (a->run || !a->loaded) return;
    a->clock_reset = true;
    a->run = true;
}

/* ---- panels over the match ------------------------------------------------------------------------------ */

static lv_obj_t *panel_open(mg_app_t *a, int kind, int w)
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
    lv_obj_set_style_bg_color(p, C(0x152019), 0);
    lv_obj_set_style_bg_grad_color(p, C(0x070B09), 0);
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
    mg_audio_pause(kind == PN_PAUSE);
    aos_pad_reset(&a->pad, lv_tick_get());
    return p;
}

static void panel_close(mg_app_t *a)
{
    if (!a->panel) return;
    aos_pad_menu_clear(&a->menu);
    lv_obj_delete(a->panel);
    a->panel = NULL;
    a->panel_kind = PN_NONE;
    mg_audio_pause(false);
    aos_pad_reset(&a->pad, lv_tick_get());
}

static void pad_menu_from(mg_app_t *a, lv_obj_t *p)
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
    panel_close((mg_app_t *)lv_event_get_user_data(e));
}

static void ev_restart(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    mg_match_quit(a);
    mg_match_start(a);
}

static void ev_menu(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    mg_match_quit(a);
    mg_menu_show(a, SC_SETUP);
}

static void ev_sfx(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    a->cfg.sfx = !a->cfg.sfx;
    mg_audio_set(a->cfg.sfx, a->cfg.music, a->cfg.crowd);
    mg_cfg_save(&a->cfg);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->cfg.sfx ? _("Sonido: sí") : _("Sonido: no"));
}

static void ev_crowd(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    a->cfg.crowd = !a->cfg.crowd;
    mg_audio_set(a->cfg.sfx, a->cfg.music, a->cfg.crowd);
    if (a->ui) a->ui->crowd_on = a->cfg.crowd;
    mg_cfg_save(&a->cfg);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->cfg.crowd ? _("Hinchada: sí") : _("Hinchada: no"));
}

static void score_line(mg_app_t *a, char *s, size_t n)
{
    mg_game_t *g = a->game;
    snprintf(s, n, "%.30s  %d - %d  %.30s", a->tm[0].name, g->score[mg_side_of_team(a, 0)],
             g->score[mg_side_of_team(a, 1)], a->tm[1].name);
}

static void pause_show(mg_app_t *a)
{
    if (!a->loaded || a->panel) return;
    lv_obj_t *p = panel_open(a, PN_PAUSE, 520);
    mg_label(p, _("Pausa"), aos_font_title, ACCENT);
    char line[96];
    score_line(a, line, sizeof line);
    lv_obj_t *l = mg_label(p, line, aos_font_small, 0xC8D0C8);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    mg_button(p, _("Seguir"), true, 420, 84, ev_resume, a);
    mg_button(p, _("Empezar de nuevo"), false, 420, 72, ev_restart, a);
    mg_button(p, a->cfg.sfx ? _("Sonido: sí") : _("Sonido: no"), false, 420, 72, ev_sfx, a);
    mg_button(p, a->cfg.crowd ? _("Hinchada: sí") : _("Hinchada: no"), false, 420, 72, ev_crowd, a);
    mg_button(p, _("Salir al menú"), false, 420, 72, ev_menu, a);
    pad_menu_from(a, p);
}

static void result_show(mg_app_t *a)
{
    if (a->panel || a->result_done) return;
    a->result_done = true;
    mg_game_t *g = a->game;
    lv_obj_t *p = panel_open(a, PN_RESULT, 600);
    int s0 = g->score[mg_side_of_team(a, 0)], s1 = g->score[mg_side_of_team(a, 1)];
    char line[120];
    if (s0 == s1) snprintf(line, sizeof line, "%s", _("Empate"));
    else if (a->cfg.mode == MODE_CPU) snprintf(line, sizeof line, "%s", s0 > s1 ? _("¡Ganaste!") : _("Ganó la placa"));
    else snprintf(line, sizeof line, _("¡Ganó %s!"), s0 > s1 ? a->tm[0].name : a->tm[1].name);
    mg_label(p, line, aos_font_title, s0 >= s1 || a->cfg.mode == MODE_TWO ? 0xFFE14A : 0xFF6050);
    score_line(a, line, sizeof line);
    lv_obj_t *l = mg_label(p, line, aos_font_body, 0xFFFFFF);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    mg_button(p, _("Revancha"), true, 440, 84, ev_restart, a);
    mg_button(p, _("Menú"), false, 440, 72, ev_menu, a);
    pad_menu_from(a, p);
    mg_audio_pause(false);
    aos_hal_log("metegol", "match over: %s %d - %d %s (difficulty %d, mode %d), %d kicks", a->tm[0].id, s0, s1,
                a->tm[1].id, a->cfg.diff, a->cfg.mode, g->kicks);
}

/* ---- a match comes and goes ----------------------------------------------------------------------------- */

static int stadium_for(mg_app_t *a)
{
    int want = MG_LEAGUE_CLUB == a->cfg.league ? MG_ST_CLUBS : MG_ST_NATIONS;
    int s = a->cfg.stadium[0] ? mg_stadium_find(&a->art, a->cfg.stadium) : -1;
    if (s >= 0 && (a->art.st[s].league == MG_ST_BOTH || a->art.st[s].league == want)) return s;
    if (a->tm[0].stadium[0]) {
        s = mg_stadium_find(&a->art, a->tm[0].stadium);
        if (s >= 0) return s;
    }
    for (int i = 0; i < a->art.nst; i++) {
        if (a->art.st[i].league == MG_ST_BOTH || a->art.st[i].league == want) return i;
    }
    return a->art.nst ? 0 : -1;
}

/* the two teams of the setup, the visitor in its other kit if they clash */
static void teams_resolve(mg_app_t *a)
{
    for (int t = 0; t < 2; t++) {
        int i = mg_teams_find(&a->teams, a->cfg.team[t]);
        if (i < 0 || a->teams.t[i].league != a->cfg.league) {
            int list[MG_TEAMS_MAX];
            int n = mg_teams_of(&a->teams, a->cfg.league, list, MG_TEAMS_MAX);
            i = n ? list[t % n] : 0;
            snprintf(a->cfg.team[t], sizeof a->cfg.team[t], "%s", a->teams.t[i].id);
        }
        a->team_ix[t] = i;
        a->tm[t] = a->teams.t[i];
    }
    if (mg_kit_clash(&a->tm[0], &a->tm[1])) mg_kit_away(&a->teams.t[a->team_ix[1]], &a->tm[1]);
}

void mg_match_quit(mg_app_t *a)
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
    mg_audio_crowd(-1);
    mg_audio_music(true);
    a->fb_full = true;
}

static void start_play(mg_app_t *a)
{
    mg_game_t *g = a->game;
    static const float SPEED[3] = { 0.85f, 1.0f, 1.18f };
    g->ball_speed = SPEED[a->cfg.speed];
    g->rnd = (uint32_t)aos_hal_uptime_us() | 1u;
    g->swapped = false;
    bool cpu = a->cfg.mode == MODE_CPU;
    /* side s holds team (s ^ p1_side) at the start */
    bool ai0 = cpu && ((0 ^ a->cfg.p1_side) == 1), ai1 = cpu && ((1 ^ a->cfg.p1_side) == 1);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* MG_DEMO=1: the board plays both sides (screenshots) */
    if (getenv("MG_DEMO")) ai0 = ai1 = true;
#endif
    mg_game_start(g, a->cfg.format == FMT_TIME ? a->cfg.minutes : 0, a->cfg.format == FMT_GOALS ? a->cfg.goals : 0,
                  a->cfg.format == FMT_TIME && a->cfg.swap, ai0, a->cfg.diff, ai1, a->cfg.diff);
    memset(a->ui, 0, sizeof *a->ui);
    a->ui->crowd_on = a->cfg.crowd;
    a->ui->shake_on = a->cfg.shake;
    a->ui->hint_t = 6.0f;
    a->ui->seen_state = g->state;
    mg_scene_strings(a);
    memset(a->in, 0, sizeof a->in);
    a->in[0].sel = a->in[1].sel = -1;
    memset(a->fing, 0, sizeof a->fing);
}

static void loaded_cb(mg_app_t *a)
{
    if (a->loading) lv_obj_delete(a->loading);
    a->loading = NULL;
    mg_menu_free(a);
    start_play(a);
    a->flip_ok = aos_hal_display_back() != NULL;
    a->out = aos_hal_display_get_rotation() == 270 ? MG_OUT_ROT270 : MG_OUT_ROT90;
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
    mg_scene_draw(a, 0.016f);
    mg_gfx_end(&a->gfx, a->fb, MG_OUT_SCREEN, a->fb_hash, true);
    a->gfx.one_core = !a->worker_on;
    a->fb_full = false;
    lv_obj_invalidate(a->canvas);
    aos_hal_display_refresh_request(60);
    mg_audio_music(false);
    mg_audio_crowd(0.5f);
    a->state = ST_MATCH;
    a->clock_reset = true;
    a->run = true;
    aos_hal_log("metegol", "%s v %s at %s, difficulty %d, mode %d, %u KB held", a->tm[0].id, a->tm[1].id,
                a->art.st[a->st_ix].id, a->cfg.diff, a->cfg.mode, (unsigned)(mg_mem_used() / 1024));
}

void mg_match_start(mg_app_t *a)
{
    if (a->state == ST_LOADING || !a->art_ok) return;
    teams_resolve(a);
    a->st_ix = stadium_for(a);
    if (a->st_ix < 0) {
        aos_ui_toast(_("Falta el paquete del juego: bajalo de la Tienda"), 4000);
        return;
    }
    mg_cfg_save(&a->cfg);
    a->state = ST_LOADING;
    a->loading = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->loading);
    lv_obj_set_size(a->loading, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(a->loading, C(0x070B09), 0);
    lv_obj_set_style_bg_opa(a->loading, LV_OPA_COVER, 0);
    char line[96];
    snprintf(line, sizeof line, "%.36s  -  %.36s", a->tm[0].name, a->tm[1].name);
    lv_obj_t *l = mg_label(a->loading, line, aos_font_title, 0xFFFFFF);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -30);
    const mg_stadium_t *s = &a->art.st[a->st_ix];
    l = mg_label(a->loading, s->name[mg_lang()][0] ? s->name[mg_lang()] : s->name[0], aos_font_body, s->accent);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 30);
    l = mg_label(a->loading, _("Preparando la cancha..."), aos_font_small, 0x80A088);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 80);
    a->job_done = 0;
    if (a->worker_on) {
        a->job = 1;
    } else {
        a->job = 1;
        do_job(a);
    }
}

/* ---- LVGL's side ------------------------------------------------------------------------------------------ */

static void sounds_play(mg_app_t *a)
{
    while (a->snd_r != a->snd_w) {
        mg_audio_play(a->snd[a->snd_r & 63], a->snd_vol[a->snd_r & 63]);
        a->snd_r++;
    }
}

static void match_tick(mg_app_t *a)
{
    uint32_t over = aos_ui_overlay();
    mg_game_t *g = a->game;
    if (over) {
        const uint32_t PAUSING = AOS_UI_OVER_PANEL | AOS_UI_OVER_SWITCHER | AOS_UI_OVER_GESTURE | AOS_UI_OVER_NOT_FRONT |
                                 AOS_UI_OVER_LOCK;
        if ((over & PAUSING) && !a->panel && g->state != MG_ST_END) pause_show(a);
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
    mg_input_touch(a);
    mg_input_pads(a);
    if (a->button_pause) {
        a->button_pause = false;
        if (g->state != MG_ST_END) {
            pause_show(a);
            return;
        }
    }
    sounds_play(a);
    /* the murmur: louder near a goal, and after one */
    float lvl = 0.35f + 0.55f * g->near_goal;
    if (g->state == MG_ST_GOAL || g->state == MG_ST_REPLAY) lvl = 0.9f;
    mg_audio_crowd(lvl);
    if (!a->worker_on) {
        match_frame(a);
    }
    if (a->frame_seen != a->frame_gen) {
        a->frame_seen = a->frame_gen;
        if (a->canvas && !a->flip_ok) lv_obj_invalidate(a->canvas);
    }
    if (g->state == MG_ST_END && g->state_t > 3.0f) result_show(a);
}

static void timer_cb(lv_timer_t *tm)
{
    mg_app_t *a = (mg_app_t *)lv_timer_get_user_data(tm);
    aos_pad_update(&a->pad, lv_tick_get());
    if (a->state == ST_LOADING) {
        if (a->job_done == 1) loaded_cb(a);
        else if (a->job_done == -1) {
            a->job_done = 0;
            mg_match_quit(a);
            mg_menu_show(a, SC_SETUP);
            aos_ui_toast(_("No se pudo cargar el estadio"), 3000);
        }
    } else if (a->state == ST_MATCH && a->loaded) {
        match_tick(a);
    } else {
        if (a->job_done == 1) {
            /* the still for the menus is ready */
            a->job_done = 0;
            if (a->canvas) lv_obj_invalidate(a->canvas);
        }
        mg_menu_pad(a);
    }
    mg_audio_pump();
}

/* ---- life cycle ----------------------------------------------------------------------------------------- */

static void *metegol_create(aos_app_t *self, lv_obj_t *root)
{
    mg_app_t *a = (mg_app_t *)mg_calloc(sizeof(mg_app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->game = (mg_game_t *)mg_calloc(sizeof(mg_game_t));
    a->ui = (mg_ui_t *)mg_calloc(sizeof(mg_ui_t));
    a->fb = (uint16_t *)mg_calloc((size_t)MG_W * MG_H * 2);
    a->fb_hash = (uint32_t *)mg_calloc(sizeof(uint32_t) * MG_NT);
    for (int i = 0; i < 3; i++) a->pb[i].hash = (uint32_t *)mg_calloc(sizeof(uint32_t) * MG_NT);
    a->mx = aos_hal_mutex_create();
    if (!mg_in_mx) mg_in_mx = aos_hal_mutex_create();
    bool ok = a->game && a->ui && a->fb && a->fb_hash && a->pb[0].hash && a->pb[1].hash && a->pb[2].hash && a->mx &&
              mg_in_mx && mg_gfx_init(&a->gfx);
    lv_obj_set_style_bg_color(root, C(0x070B09), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    mg_cfg_load(&a->cfg);
    const char *data = aos_hal_path_data();
    mg_teams_load(&a->teams, data ? data : ".");
    const char *apps = aos_hal_path_apps();
    a->art_ok = ok && mg_art_open(&a->art, apps ? apps : ".");
    a->in[0].sel = a->in[1].sel = -1;
    if (a->art_ok) {
        mg_audio_set(a->cfg.sfx, a->cfg.music, a->cfg.crowd);
        mg_audio_open(&a->art.pak);
        mg_audio_music(true);
    }
    /* the canvas under everything: the menus' still, a held match */
    if (a->fb) {
        a->canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(a->canvas, a->fb, MG_W, MG_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(a->canvas, 0, 0);
        lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    }
    a->worker_on = ok && aos_hal_worker_start_on("metegol", worker_fn, a, WORKER_STACK, 0, 3);
    a->gfx.one_core = !a->worker_on;
    if (!a->worker_on) aos_hal_log("metegol", "no worker: everything on LVGL's thread, on one core");
    a->state = ST_MENU;
    if (a->art_ok) {
        teams_resolve(a);
        a->st_ix = stadium_for(a);
        if (a->st_ix >= 0) {
            a->job = 2;
            if (!a->worker_on) do_job(a);
        }
    }
    mg_menu_show(a, SC_TITLE);
    a->timer = lv_timer_create(timer_cb, FRAME_MS, a);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* MG_PLAY=1 starts a match straight away (scripts); MG_TEAMS=boca,river */
    if (getenv("MG_TEAMS")) {
        char buf[48];
        snprintf(buf, sizeof buf, "%s", getenv("MG_TEAMS"));
        char *c = strchr(buf, ',');
        if (c) {
            *c = 0;
            snprintf(a->cfg.team[0], sizeof a->cfg.team[0], "%.19s", buf);
            snprintf(a->cfg.team[1], sizeof a->cfg.team[1], "%.19s", c + 1);
            int i = mg_teams_find(&a->teams, buf);
            if (i >= 0) a->cfg.league = a->teams.t[i].league;
        }
    }
    if (getenv("MG_STADIUM")) snprintf(a->cfg.stadium, sizeof a->cfg.stadium, "%.23s", getenv("MG_STADIUM"));
    if (getenv("MG_MODE")) a->cfg.mode = atoi(getenv("MG_MODE")) ? MODE_TWO : MODE_CPU;
    if (getenv("MG_DIFF")) a->cfg.diff = atoi(getenv("MG_DIFF")) % MG_DIFF_N;
    if (getenv("MG_PLAY") && a->art_ok) {
        while (a->job) aos_hal_sleep_ms(5);
        a->job_done = 0;
        mg_match_start(a);
    }
#endif
    return a;
}

static void metegol_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    mg_app_t *a = (mg_app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->timer = NULL;
    a->run = false;
    if (a->worker_on) aos_hal_worker_stop();
    a->worker_on = false;
    mg_match_quit(a);
    mg_menu_free(a);
    mg_audio_close();
    mg_art_close(&a->art);
    mg_gfx_free(&a->gfx);
    for (int i = 0; i < 3; i++) mg_free(a->pb[i].hash);
    mg_free(a->fb_hash);
    mg_free(a->fb);
    mg_free(a->game);
    mg_free(a->ui);
    mg_free(a);
    /* what the app's own allocator still holds: 0 unless something leaked */
    aos_hal_log("metegol", "closed, %u KB of its own still held", (unsigned)(mg_mem_used() / 1024));
}

static void metegol_show(aos_app_t *self, void *inst)
{
    (void)self;
    mg_app_t *a = (mg_app_t *)inst;
    if (!a) return;
    if (a->loaded) mg_scene_strings(a);
    mg_audio_pause(a->panel != NULL && a->panel_kind == PN_PAUSE);
}

static void metegol_hide(aos_app_t *self, void *inst)
{
    (void)self;
    mg_app_t *a = (mg_app_t *)inst;
    if (!a) return;
    if (a->state == ST_MATCH && a->loaded) {
        if (!a->panel && a->game->state != MG_ST_END) pause_show(a);
        match_hold(a);
    }
    mg_audio_pause(true);
}

static bool metegol_back(aos_app_t *self, void *inst)
{
    (void)self;
    mg_app_t *a = (mg_app_t *)inst;
    if (!a) return false;
    if (a->state == ST_LOADING) return true;
    if (a->state == ST_MATCH) {
        if (a->panel_kind == PN_PAUSE) panel_close(a);
        else if (!a->panel) pause_show(a);
        return true;
    }
    return mg_menu_back(a);
}

static bool metegol_button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    mg_app_t *a = (mg_app_t *)inst;
    if (!a || a->state != ST_MATCH || action != AOS_BUTTON_CLICK) return false;
    a->button_pause = true;
    return true;
}

static void metegol_tick(aos_app_t *self, void *inst)
{
    (void)self;
    mg_app_t *a = (mg_app_t *)inst;
    /* the portal's page changed the teams: read them again in the menus */
    if (a && a->state == ST_MENU) {
        const char *data = aos_hal_path_data();
        if (mg_teams_changed(&a->teams, data ? data : ".")) {
            mg_teams_load(&a->teams, data ? data : ".");
            mg_menu_show(a, a->screen);
        }
    }
}

/* the table seen from above: green with its lines, a ball, two figures */
static const uint8_t METEGOL_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 0, 92, 60, 10, AIC_C_LIT(0x8A5530), 255),
    AIC_RECT(AIC_CENTER, 0, 0, 80, 48, 6, AIC_C_LIT(0x2F9A44), 255),
    AIC_GRAD(AIC_C_LIT(0x237A35), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER, 0, 0, 2, 48, 0, AIC_C_LIT(0xF2F2EA), 230),
    AIC_RECT(AIC_CENTER, 0, 0, 18, 18, AIC_CIRCLE, AIC_C_LIT(0xF2F2EA), 70),
    AIC_RECT(AIC_CENTER, -18, -50, 4, 104, 0, AIC_C_LIT(0xD0D6DE), 255),
    AIC_RECT(AIC_CENTER, 18, -50, 4, 104, 0, AIC_C_LIT(0xD0D6DE), 255),
    AIC_RECT(AIC_CENTER, -18, -10, 12, 16, 4, AIC_C_LIT(0x1A50C8), 255),
    AIC_RECT(AIC_CENTER, 18, 10, 12, 16, 4, AIC_C_LIT(0xE33030), 255),
    AIC_RECT(AIC_CENTER, 4, 2, 9, 9, AIC_CIRCLE, AIC_C_LIT(0xFFFFFF), 255),
    AIC_END
};

static bool metegol_init(aos_app_t *app)
{
    app->desc.id       = "demo.metegol";
    app->desc.name     = "Metegol";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x1F7A3A;
    app->desc.color_b  = 0x08200F;
    app->desc.order    = 160;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_LANDSCAPE |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    aos_icon_set_ops(app, METEGOL_ICON, sizeof METEGOL_ICON);
    app->create  = metegol_create;
    app->destroy = metegol_destroy;
    app->show    = metegol_show;
    app->hide    = metegol_hide;
    app->back    = metegol_back;
    app->button  = metegol_button;
    app->tick    = metegol_tick;
    return true;
}

AOS_APP_ENTRY(metegol_init);
