/*
 * DERIVA - rocks in space in neon: a ship that turns, pushes with
 * inertia and shoots, rocks of five kinds that split, enemies, bosses,
 * powers and crystals; a campaign of twelve sectors with a workshop
 * between them, and an endless arcade with its records, alone or two.
 *
 * Everything on the screen is a stroke of light drawn by code: the
 * compositor (ast_gfx.c, Bastion's and Metegol's, with a neon stroke)
 * redraws only the 16 x 16 tiles whose content changed, straight into the
 * panel's free buffer, half of them on each core, and flips to it.
 * Upright or lying down: the field and the deck for the thumbs are laid
 * out from the root, and turning the screen keeps the game.
 *
 * Threads: the app's worker (core 0) makes the music once, the menus'
 * still, and then runs the game: steps of 1/120 s (ast_game.c), the
 * frame's list (ast_scene.c), the tiles and the flip. LVGL's thread reads
 * the fingers and the gamepads (ast_input.c), plays the sounds the worker
 * asks for, and owns the menus (ast_menu.c) and the panels over a game.
 * While LVGL or the system has something over the game the worker holds,
 * and LVGL shows the game's last frame under it from a canvas.
 */
#include "ast_app.h"

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
#define ACCENT       0x5CF2FF
#define APP_NAME     "Deriva"

void *ast_in_mx;

static void pause_show(ast_app_t *a);
static void panel_close(ast_app_t *a);

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ---- the settings ------------------------------------------------------------------------------- */

static void cfg_path(char *p, size_t n)
{
    const char *d = aos_hal_path_data();
    snprintf(p, n, "%.120s/asteroides.txt", d ? d : ".");
}

static void cfg_defaults(ast_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->diff = AG_NORMAL;
    c->sfx = c->music = c->shake = 1;
    c->controls = CTL_STICK;
}

static const struct {
    const char *k;
    size_t off;
} INTS[] = {
    { "dificultad", offsetof(ast_cfg_t, diff) },       { "sonido", offsetof(ast_cfg_t, sfx) },
    { "musica", offsetof(ast_cfg_t, music) },          { "sacudidas", offsetof(ast_cfg_t, shake) },
    { "controles", offsetof(ast_cfg_t, controls) },    { "zurdo", offsetof(ast_cfg_t, lefty) },
    { "campana_sector", offsetof(ast_cfg_t, camp_sector) }, { "campana_vidas", offsetof(ast_cfg_t, camp_lives) },
    { "campana_puntos", offsetof(ast_cfg_t, camp_score) }, { "campana_cristales", offsetof(ast_cfg_t, camp_crystals) },
    { "campana_dificultad", offsetof(ast_cfg_t, camp_diff) }, { "campana_max", offsetof(ast_cfg_t, camp_max) },
    { "campana_ganada", offsetof(ast_cfg_t, camp_won) },
};

void ast_cfg_load(ast_cfg_t *c)
{
    cfg_defaults(c);
    char p[160];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) {
            if (!strcmp(line, INTS[i].k)) *(int *)((char *)c + INTS[i].off) = atoi(v);
        }
        if (!strcmp(line, "mejoras")) {
            for (int u = 0; u < UP_N && *v; u++) {
                c->camp_upg[u] = *v - '0';
                v++;
            }
        } else if (!strncmp(line, "record", 6)) {
            int i = atoi(line + 6);
            if (i >= 0 && i < AST_RECORDS) {
                c->best[i] = atoi(v);
                char *s = strchr(v, ',');
                c->best_wave[i] = s ? atoi(s + 1) : 0;
            }
        }
    }
    fclose(f);
    if (c->diff < 0 || c->diff >= AG_DIFF_N) c->diff = AG_NORMAL;
    if (c->camp_sector < 0 || c->camp_sector > AG_SECTORS) c->camp_sector = 0;
    for (int u = 0; u < UP_N; u++) {
        if (c->camp_upg[u] < 0 || c->camp_upg[u] > ag_upg_max(u)) c->camp_upg[u] = 0;
    }
}

void ast_cfg_save(const ast_cfg_t *c)
{
    char p[160];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    char line[96];
    fputs("# Deriva: los ajustes, los records y la campaña (la app lo reescribe)\n", f);
    for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) {
        snprintf(line, sizeof line, "%s=%d\n", INTS[i].k, *(const int *)((const char *)c + INTS[i].off));
        fputs(line, f);
    }
    char up[UP_N + 1];
    for (int u = 0; u < UP_N; u++) up[u] = (char)('0' + c->camp_upg[u]);
    up[UP_N] = 0;
    snprintf(line, sizeof line, "mejoras=%s\n", up);
    fputs(line, f);
    for (int i = 0; i < AST_RECORDS; i++) {
        if (!c->best[i]) continue;
        snprintf(line, sizeof line, "record%d=%d,%d\n", i, c->best[i], c->best_wave[i]);
        fputs(line, f);
    }
    fclose(f);
}

/* ---- where things go ------------------------------------------------------------------------------- */

void ast_layout(ast_app_t *a, int w, int h)
{
    ast_layout_t *L = &a->lay;
    memset(L, 0, sizeof *L);
    L->W = w;
    L->H = h;
    L->land = w > h;
    bool lefty = a->cfg.lefty != 0;
    if (!L->land) {
        /* upright: the field on top, the deck under it for both thumbs */
        L->fx = 0;
        L->fy = 0;
        L->fw = w;
        L->fh = h - 304;
        int dy = L->fh;
        L->stick_x = 178;
        L->stick_y = dy + 148;
        L->stick_r = 96;
        L->zone_x0 = 0;
        L->zone_x1 = w / 2;
        L->zone_y0 = dy;
        L->zone_y1 = h - 30;
        L->fire_x = w - 134;
        L->fire_y = dy + 136;
        L->fire_r = 88;
        L->jump_x = w / 2 + 70;
        L->jump_y = dy + 70;
        L->jump_r = 52;
        L->pause_x = w - 34;
        L->pause_y = 36;
        L->pause_r = 24;
        L->btn_r = 60;
        L->tl_x = 82;
        L->tl_y = dy + 196;
        L->tr_x = 230;
        L->tr_y = dy + 196;
        L->push_x = 156;
        L->push_y = dy + 74;
    } else {
        /* lying down: the field in the middle, a deck on each side */
        int side = 200;
        L->fx = side;
        L->fy = 0;
        L->fw = w - 2 * side;
        L->fh = h;
        L->stick_x = side / 2;
        L->stick_y = h - 190;
        L->stick_r = 80;
        L->zone_x0 = 0;
        L->zone_x1 = side;
        L->zone_y0 = 200;
        L->zone_y1 = h;
        L->fire_x = w - side / 2;
        L->fire_y = h - 170;
        L->fire_r = 78;
        L->jump_x = w - side / 2;
        L->jump_y = h - 370;
        L->jump_r = 52;
        L->pause_x = w - 38;
        L->pause_y = 40;
        L->pause_r = 24;
        L->btn_r = 46;
        L->tl_x = 52;
        L->tl_y = h - 120;
        L->tr_x = 148;
        L->tr_y = h - 120;
        L->push_x = 100;
        L->push_y = h - 250;
    }
    if (lefty) {
        /* the stick on the right, the buttons on the left */
#define MX(v) (v) = w - (v)
        MX(L->stick_x);
        MX(L->fire_x);
        MX(L->jump_x);
        MX(L->tl_x);
        MX(L->tr_x);
        MX(L->push_x);
        int z0 = L->zone_x0, z1 = L->zone_x1;
        L->zone_x0 = w - z1;
        L->zone_x1 = w - z0;
        int t = L->tl_x;
        L->tl_x = L->tr_x;
        L->tr_x = t;
        if (L->land) {
            L->pause_x = 38;
        }
#undef MX
    }
}

/* ---- the worker's frame ------------------------------------------------------------------------- */

static void sounds_push(ast_app_t *a, ag_game_t *g)
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

static ast_pbuf_t *pbuf(ast_app_t *a, uint16_t *b)
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

static void perf(ast_app_t *a)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!a->perf_t0) a->perf_t0 = now;
    if (now - a->perf_t0 < PERF_MS || !a->perf_frames) return;
    float s = (float)(now - a->perf_t0) / 1000.0f, n = (float)a->perf_frames;
    const ag_game_t *g = a->game;
    aos_hal_log("asteroides",
                "%.1f fps, a frame %.2f ms (sim %.2f, list %.2f, draw %.2f), %u tiles, %u things, flip %d, %u KB held, "
                "%s sector %d wave %d, %d rocks",
                (double)(n / s), (double)((float)(a->perf_sim_us + a->perf_list_us + a->perf_draw_us) / 1000.0f / n),
                (double)((float)a->perf_sim_us / 1000.0f / n), (double)((float)a->perf_list_us / 1000.0f / n),
                (double)((float)a->perf_draw_us / 1000.0f / n), (unsigned)(a->perf_tiles / a->perf_frames),
                (unsigned)(a->perf_items / a->perf_frames), a->flip_ok, (unsigned)(ast_mem_used() / 1024),
                g->mode == AG_ARCADE ? "arcade" : "campaign", g->sector, g->wave, ag_rocks_on(g));
    a->perf_frames = a->perf_tiles = a->perf_items = 0;
    a->perf_sim_us = a->perf_list_us = a->perf_draw_us = 0;
    a->perf_t0 = now;
}

/* a new sky when the sector's colours change; every buffer is drawn whole */
static void sky(ast_app_t *a)
{
    int pal = ast_palette(a->game);
    if (pal == a->bg_sector) return;
    ast_bg_make(a, pal);
    a->gfx.salt = (a->gfx.salt + 0x9E3779B9u) | 1u;
    a->fb_full = true;
}

static void game_frame(ast_app_t *a)
{
    ag_game_t *g = a->game;
    uint32_t t0 = (uint32_t)aos_hal_uptime_us();
    float dt = a->clock_reset ? 0.016f : (float)(uint32_t)(t0 - a->last_us) / 1e6f;
    a->clock_reset = false;
    a->last_us = t0;
    if (dt > 0.05f) dt = 0.05f;
    a->acc += dt;
    int steps = 0;
    aos_hal_mutex_lock(ast_in_mx);
    while (a->acc >= AG_DT && steps < 12) {
        ag_hand_t h[2];
        const ag_hand_t *hp[2] = { NULL, NULL };
        for (int s = 0; s < g->players; s++) {
            if (a->demo) ast_bot_hand(&a->bot[s], g, s, &h[s]);
            else ast_input_hand(a, s, &h[s]);
            hp[s] = &h[s];
        }
        ag_step(g, hp[0], hp[1]);
        sounds_push(a, g);
        a->acc -= AG_DT;
        steps++;
    }
    aos_hal_mutex_unlock(ast_in_mx);
    if (a->acc > AG_DT * 4) a->acc = 0;
    ast_scene_events(a);
    sky(a);
    uint32_t t1 = (uint32_t)aos_hal_uptime_us();
    ast_scene_draw(a, dt);
    uint32_t t2 = (uint32_t)aos_hal_uptime_us();
    uint16_t *b = a->flip_ok ? aos_hal_display_back() : NULL;
    int tiles;
    if (b) {
        ast_pbuf_t *p = pbuf(a, b);
        uint32_t since = 0;
        bool touched = true;
        bool known = p && p->flip && aos_hal_display_back_age(b, &since, &touched) && !touched && since != UINT32_MAX &&
                     since == a->flips - p->flip;
        if (a->fb_full) {
            /* a new sky: every buffer from scratch */
            for (int i = 0; i < 3; i++) a->pb[i].flip = 0;
            known = false;
            a->fb_full = false;
        }
        tiles = ast_gfx_end(&a->gfx, b, a->out, p ? p->hash : a->fb_hash, !known);
        if (aos_hal_display_flip(b)) {
            a->flips++;
            if (p) p->flip = a->flips;
        } else if (p) {
            p->flip = 0;
        }
    } else {
        tiles = ast_gfx_end(&a->gfx, a->fb, AST_OUT_SCREEN, a->fb_hash, a->fb_full);
        a->fb_full = false;
        a->frame_gen++;
    }
    uint32_t t3 = (uint32_t)aos_hal_uptime_us();
    a->thrust_level = g->ship[0].alive ? g->ship[0].thrust : 0;
    if (g->players > 1 && g->ship[1].alive && g->ship[1].thrust > a->thrust_level) a->thrust_level = g->ship[1].thrust;
    a->perf_frames++;
    a->perf_tiles += (uint32_t)tiles;
    a->perf_items += (uint32_t)a->gfx.nitem;
    a->perf_sim_us += t1 - t0;
    a->perf_list_us += t2 - t1;
    a->perf_draw_us += t3 - t2;
    perf(a);
}

/* the menus' still: a sector's sky, rocks of every kind, the ship, the name */
static bool job_still(ast_app_t *a)
{
    ast_audio_make_music();
    int sector = a->cfg.camp_sector > 0 ? a->cfg.camp_sector : 1;
    ag_game_t *g = a->game;
    /* the still takes the whole screen: no deck under the menus */
    ast_layout_t keep = a->lay;
    a->lay.fx = a->lay.fy = 0;
    a->lay.fw = a->lay.W;
    a->lay.fh = a->lay.H;
    g->W = (float)a->lay.fw;
    g->H = (float)a->lay.fh;
    ag_title_scene(g, sector);
    ast_bg_make(a, sector);
    a->bg_sector = -1;
    ast_scene_reset(a);
    a->ui->title = true;
    a->gfx.one_core = !a->worker_on;
    a->gfx.salt = (a->gfx.salt + 0x9E3779B9u) | 1u;
    ast_scene_draw(a, 0.016f);
    ast_gfx_end(&a->gfx, a->fb, AST_OUT_SCREEN, a->fb_hash, true);
    a->ui->title = false;
    a->lay = keep;
    a->frame_gen++;
    return true;
}

static void do_job(ast_app_t *a)
{
    int j = a->job;
    if (!j) return;
    aos_hal_mutex_lock(a->mx);
    bool ok = job_still(a);
    aos_hal_mutex_unlock(a->mx);
    a->job = 0;
    a->job_done = ok ? 1 : -1;
}

static void worker_fn(void *arg)
{
    ast_app_t *a = (ast_app_t *)arg;
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
        if (a->run) game_frame(a);
        aos_hal_mutex_unlock(a->mx);
        uint32_t ms = ((uint32_t)aos_hal_uptime_us() - t0) / 1000;
        aos_hal_worker_sleep(ms < 15 ? 16 - ms : 1);
    }
}

/* the worker stops drawing, and the game's last frame goes to the canvas */
static void game_hold(ast_app_t *a)
{
    if (!a->run) return;
    a->run = false;
    aos_hal_mutex_lock(a->mx);
    a->gfx.one_core = true;
    ast_gfx_end(&a->gfx, a->fb, AST_OUT_SCREEN, a->fb_hash, true);
    a->gfx.one_core = !a->worker_on;
    a->fb_full = false;
    aos_hal_mutex_unlock(a->mx);
    if (a->canvas) lv_obj_invalidate(a->canvas);
    ast_audio_thrust(0);
}

static void game_resume(ast_app_t *a)
{
    if (a->run || !a->loaded) return;
    for (int i = 0; i < 3; i++) a->pb[i].flip = 0;
    a->clock_reset = true;
    a->run = true;
}

/* ---- panels over the game ---------------------------------------------------------------------- */

lv_obj_t *ast_panel(ast_app_t *a, int w)
{
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
    lv_obj_set_style_bg_color(p, C(0x0C1620), 0);
    lv_obj_set_style_bg_grad_color(p, C(0x04070C), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, C(ACCENT), 0);
    lv_obj_set_style_shadow_width(p, 30, 0);
    lv_obj_set_style_shadow_color(p, C(ACCENT), 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(p, a->lay.land ? 20 : 26, 0);
    lv_obj_set_style_pad_row(p, a->lay.land ? 10 : 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *panel_open(ast_app_t *a, int kind, int w)
{
    panel_close(a);
    game_hold(a);
    lv_obj_t *p = ast_panel(a, w);
    a->panel = lv_obj_get_parent(p);
    a->panel_kind = kind;
    ast_audio_pause(kind == PN_PAUSE);
    aos_pad_reset(&a->pad, lv_tick_get());
    return p;
}

static void panel_close(ast_app_t *a)
{
    if (!a->panel) return;
    aos_pad_menu_clear(&a->menu);
    lv_obj_delete(a->panel);
    a->panel = NULL;
    a->panel_kind = PN_NONE;
    ast_audio_pause(false);
    aos_pad_reset(&a->pad, lv_tick_get());
}

static void pad_menu_from(ast_app_t *a, lv_obj_t *p)
{
    lv_obj_t *items[16];
    int n = 0;
    uint32_t cnt = lv_obj_get_child_count(p);
    for (uint32_t i = 0; i < cnt && n < 16; i++) {
        lv_obj_t *c = lv_obj_get_child(p, (int32_t)i);
        if (lv_obj_check_type(c, &lv_button_class)) {
            items[n++] = c;
        } else {
            uint32_t m = lv_obj_get_child_count(c);
            for (uint32_t j = 0; j < m && n < 16; j++) {
                lv_obj_t *d = lv_obj_get_child(c, (int32_t)j);
                if (lv_obj_check_type(d, &lv_button_class)) items[n++] = d;
                else if (lv_obj_get_child_count(d) && lv_obj_check_type(lv_obj_get_child(d, 0), &lv_button_class))
                    items[n++] = lv_obj_get_child(d, 0);
            }
        }
    }
    aos_pad_menu_set(&a->menu, items, n, 0);
}

static void ev_resume(lv_event_t *e)
{
    panel_close((ast_app_t *)lv_event_get_user_data(e));
}

static void ev_menu(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    ast_game_quit(a);
    ast_menu_show(a, SC_TITLE);
}

static void ev_sfx(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    a->cfg.sfx = !a->cfg.sfx;
    ast_audio_set(a->cfg.sfx, a->cfg.music);
    ast_cfg_save(&a->cfg);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->cfg.sfx ? _("Sonido: sí") : _("Sonido: no"));
}

static void ev_music(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    a->cfg.music = !a->cfg.music;
    ast_audio_set(a->cfg.sfx, a->cfg.music);
    ast_cfg_save(&a->cfg);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->cfg.music ? _("Música: sí") : _("Música: no"));
}

static void ev_retry(lv_event_t *e);

static void pause_show(ast_app_t *a)
{
    if (!a->loaded || a->panel) return;
    const ag_game_t *g = a->game;
    lv_obj_t *p = panel_open(a, PN_PAUSE, a->lay.land ? 560 : 520);
    ast_label(p, _("Pausa"), aos_font_title, ACCENT);
    char line[96];
    if (g->mode == AG_CAMPAIGN) snprintf(line, sizeof line, _("Sector %d, oleada %d de %d  ·  %d puntos"), g->sector, g->wave, g->waves, g->score);
    else snprintf(line, sizeof line, _("Oleada %d  ·  %d puntos"), g->sector, g->score);
    ast_label(p, line, aos_font_small, 0xC8D0DC);
    int bw = a->lay.land ? 460 : 420, bh = a->lay.land ? 64 : 76;
    ast_button(p, _("Seguir"), true, bw, bh + 8, ev_resume, a);
    ast_button(p, g->mode == AG_CAMPAIGN ? _("Empezar el sector de nuevo") : _("Empezar de nuevo"), false, bw, bh, ev_retry, a);
    ast_button(p, a->cfg.sfx ? _("Sonido: sí") : _("Sonido: no"), false, bw, bh, ev_sfx, a);
    ast_button(p, a->cfg.music ? _("Música: sí") : _("Música: no"), false, bw, bh, ev_music, a);
    ast_button(p, _("Salir al menú"), false, bw, bh, ev_menu, a);
    pad_menu_from(a, p);
}

/* ---- a game comes and goes ----------------------------------------------------------------------- */

static void save_campaign(ast_app_t *a)
{
    ag_game_t *g = a->game;
    ast_cfg_t *c = &a->cfg;
    c->camp_sector = g->sector;
    c->camp_lives = g->ship[0].lives;
    c->camp_score = g->score;
    c->camp_crystals = g->crystals;
    c->camp_diff = g->diff;
    for (int u = 0; u < UP_N; u++) c->camp_upg[u] = g->upg[u];
    if (g->sector > c->camp_max) c->camp_max = g->sector;
    ast_cfg_save(c);
}

static void sector_go(ast_app_t *a, int sector)
{
    ag_game_t *g = a->game;
    aos_hal_mutex_lock(a->mx);
    ag_sector_start(g, sector);
    ast_scene_reset(a);
    aos_hal_mutex_unlock(a->mx);
    if (g->mode == AG_CAMPAIGN) save_campaign(a);
    a->over_done = false;
    a->over_t = 0;
}

static void ev_retry(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ag_game_t *g = a->game;
    panel_close(a);
    if (g->mode == AG_CAMPAIGN) {
        /* the sector again, from how it started */
        int lives = a->cfg.camp_lives > 0 ? a->cfg.camp_lives : (g->diff == AG_EASY ? 4 : 3);
        aos_hal_mutex_lock(a->mx);
        g->score = a->cfg.camp_score;
        for (int s = 0; s < g->players; s++) {
            g->ship[s].in_play = true;
            g->ship[s].lives = lives < 3 ? 3 : lives;
        }
        aos_hal_mutex_unlock(a->mx);
        sector_go(a, g->sector);
    } else {
        int players = g->players;
        ast_game_quit(a);
        ast_game_start(a, AG_ARCADE, players, false);
        return;
    }
    game_resume(a);
}

static void ev_next(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    panel_close(a);
    sector_go(a, a->game->sector + 1);
    game_resume(a);
}

static void shop_show(ast_app_t *a)
{
    lv_obj_t *p = panel_open(a, PN_SHOP, a->lay.land ? 1180 : 640);
    ast_shop_fill(a, p);
    char line[64];
    snprintf(line, sizeof line, _("Al sector %d"), a->game->sector + 1);
    ast_button(p, line, true, a->lay.land ? 460 : 520, a->lay.land ? 64 : 80, ev_next, a);
    pad_menu_from(a, p);
}

static int record_insert(ast_app_t *a, int score, int wave)
{
    ast_cfg_t *c = &a->cfg;
    for (int i = 0; i < AST_RECORDS; i++) {
        if (score > c->best[i]) {
            for (int j = AST_RECORDS - 1; j > i; j--) {
                c->best[j] = c->best[j - 1];
                c->best_wave[j] = c->best_wave[j - 1];
            }
            c->best[i] = score;
            c->best_wave[i] = wave;
            ast_cfg_save(c);
            return i;
        }
    }
    return -1;
}

static void over_show(ast_app_t *a)
{
    ag_game_t *g = a->game;
    lv_obj_t *p = panel_open(a, PN_OVER, a->lay.land ? 600 : 560);
    ast_label(p, _("Sin naves"), aos_font_title, 0xFF6A6A);
    char line[96];
    int bw = a->lay.land ? 460 : 460, bh = a->lay.land ? 66 : 80;
    if (g->mode == AG_CAMPAIGN) {
        snprintf(line, sizeof line, _("Sector %d  ·  %d puntos"), g->sector, g->score);
        ast_label(p, line, aos_font_body, 0xFFFFFF);
        ast_label(p, _("Los cristales y las mejoras quedan"), aos_font_small, 0x9AB0C4);
        ast_button(p, _("Probar el sector otra vez"), true, bw, bh, ev_retry, a);
    } else {
        snprintf(line, sizeof line, _("Oleada %d  ·  %d puntos"), g->sector, g->score);
        ast_label(p, line, aos_font_body, 0xFFFFFF);
        int r = record_insert(a, g->score, g->sector);
        if (r == 0) ast_label(p, _("¡Récord nuevo!"), aos_font_body, 0xFFE45C);
        else if (r > 0) {
            snprintf(line, sizeof line, _("El %d.º mejor"), r + 1);
            ast_label(p, line, aos_font_small, 0xFFE45C);
        }
        ast_button(p, _("Otra vez"), true, bw, bh, ev_retry, a);
    }
    ast_button(p, _("Menú"), false, bw, bh, ev_menu, a);
    pad_menu_from(a, p);
    aos_hal_log("asteroides", "game over: %s, sector/wave %d, %d points, %d crystals, %d kills, %d bosses",
                g->mode == AG_CAMPAIGN ? "campaign" : "arcade", g->sector, g->score, g->crystals, g->kills, g->bosses);
}

static void won_show(ast_app_t *a)
{
    ag_game_t *g = a->game;
    a->cfg.camp_won = 1;
    a->cfg.camp_sector = 0;
    a->cfg.camp_max = AG_SECTORS;
    ast_cfg_save(&a->cfg);
    lv_obj_t *p = panel_open(a, PN_WON, a->lay.land ? 620 : 580);
    ast_label(p, _("¡Campaña completa!"), aos_font_title, 0xFFE45C);
    char line[96];
    snprintf(line, sizeof line, _("%d puntos y %d cristales"), g->score, g->crystals);
    ast_label(p, line, aos_font_body, 0xFFFFFF);
    ast_label(p, _("Los doce sectores, limpios. Ahora, el arcade."), aos_font_small, 0x9AB0C4);
    ast_button(p, _("Menú"), true, 460, 80, ev_menu, a);
    pad_menu_from(a, p);
}

void ast_game_quit(ast_app_t *a)
{
    a->run = false;
    panel_close(a);
    aos_hal_display_refresh_request(0);
    for (int i = 0; i < 3; i++) a->pb[i].buf = NULL;
    a->loaded = false;
    a->state = ST_MENU;
    a->snd_r = a->snd_w;
    ast_audio_thrust(0);
    ast_audio_music(1);
    a->fb_full = true;
    a->job_done = 0;
    a->job = 1;
    if (!a->worker_on) do_job(a);
}

void ast_game_start(ast_app_t *a, int mode, int players, bool resume)
{
    while (a->job) aos_hal_sleep_ms(5);
    ast_menu_free(a);
    ag_game_t *g = a->game;
    a->mode = mode;
    a->players = players;
    ast_cfg_t *c = &a->cfg;
    int diff = c->diff;
    if (mode == AG_CAMPAIGN && resume && c->camp_sector > 0) diff = c->camp_diff;
    aos_hal_mutex_lock(a->mx);
    ag_new(g, mode, players, diff, (float)a->lay.fw, (float)a->lay.fh, (uint32_t)aos_hal_uptime_us() | 1u);
    int sector = 1;
    if (mode == AG_CAMPAIGN && resume && c->camp_sector > 0) {
        sector = c->camp_sector;
        g->ship[0].lives = c->camp_lives > 0 ? c->camp_lives : 3;
        g->score = c->camp_score;
        g->crystals = c->camp_crystals;
        for (int u = 0; u < UP_N; u++) g->upg[u] = c->camp_upg[u];
    }
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* AST_SECTOR=<n>: start there, with what the sectors before would have bought */
    if (getenv("AST_SECTOR")) {
        sector = atoi(getenv("AST_SECTOR"));
        if (mode == AG_CAMPAIGN) {
            for (int u = 0; u < UP_N; u++) g->upg[u] = sector > 4 ? ag_upg_max(u) / 2 + (sector > 8) : 0;
            g->crystals = 30 * sector;
        }
    }
#endif
    a->demo = false;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    if (getenv("AST_DEMO")) a->demo = true;
#endif
    for (int s = 0; s < 2; s++) ast_bot_init(&a->bot[s], BOT_GOOD, 17u + (uint32_t)s);
    ag_sector_start(g, sector);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* AST_WAVE=<n>: that wave of the sector (the last brings the boss) */
    if (getenv("AST_WAVE")) ag_wave_jump(g, atoi(getenv("AST_WAVE")));
#endif
    memset(a->in, 0, sizeof a->in);
    memset(a->fing, 0, sizeof a->fing);
    a->jump_latch[0] = a->jump_latch[1] = false;
    memset(a->ui, 0, sizeof *a->ui);
    ast_scene_strings(a);
    ast_scene_reset(a);
    a->bg_sector = -1;
    ast_bg_make(a, ast_palette(g));
    a->gfx.salt = (a->gfx.salt + 0x9E3779B9u) | 1u;
    aos_hal_mutex_unlock(a->mx);
    if (mode == AG_CAMPAIGN) save_campaign(a);
    a->flip_ok = aos_hal_display_back() != NULL;
    a->out = !a->lay.land ? AST_OUT_SCREEN : aos_hal_display_get_rotation() == 270 ? AST_OUT_ROT270 : AST_OUT_ROT90;
    a->flips = 0;
    for (int i = 0; i < 3; i++) {
        a->pb[i].buf = NULL;
        a->pb[i].flip = 0;
    }
    a->loaded = true;
    a->over_done = false;
    a->over_t = 0;
    a->acc = 0;
    a->snd_r = a->snd_w;
    a->perf_t0 = 0;
    a->perf_frames = 0;
    a->fb_full = true;
    a->gfx.one_core = true;
    ast_scene_draw(a, 0.016f);
    ast_gfx_end(&a->gfx, a->fb, AST_OUT_SCREEN, a->fb_hash, true);
    a->gfx.one_core = !a->worker_on;
    a->fb_full = false;
    lv_obj_invalidate(a->canvas);
    aos_hal_display_refresh_request(60);
    ast_audio_music(2);
    a->state = ST_PLAY;
    a->clock_reset = true;
    a->run = true;
    aos_hal_log("asteroides", "%s, %d player(s), sector %d, difficulty %d, %s, %u KB held",
                mode == AG_CAMPAIGN ? "campaign" : "arcade", players, sector, diff, a->lay.land ? "lying down" : "upright",
                (unsigned)(ast_mem_used() / 1024));
}

/* ---- LVGL's side ------------------------------------------------------------------------------------ */

static void sounds_play(ast_app_t *a)
{
    while (a->snd_r != a->snd_w) {
        ast_audio_play(a->snd[a->snd_r & 63], a->snd_vol[a->snd_r & 63]);
        a->snd_r++;
    }
}

static void game_tick(ast_app_t *a)
{
    uint32_t over = aos_ui_overlay();
    ag_game_t *g = a->game;
    if (over) {
        const uint32_t PAUSING = AOS_UI_OVER_PANEL | AOS_UI_OVER_SWITCHER | AOS_UI_OVER_GESTURE | AOS_UI_OVER_NOT_FRONT |
                                 AOS_UI_OVER_LOCK;
        if ((over & PAUSING) && !a->panel && g->state != AG_ST_OVER) pause_show(a);
        game_hold(a);
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
    if (!a->run) game_resume(a);
    ast_input_touch(a);
    ast_input_pads(a);
    if (a->button_pause) {
        a->button_pause = false;
        if (g->state != AG_ST_OVER) {
            pause_show(a);
            return;
        }
    }
    sounds_play(a);
    ast_audio_thrust(a->thrust_level);
    ast_audio_music(g->boss.on || g->state == AG_ST_BOSS_IN ? 3 : 2);
    if (!a->worker_on) game_frame(a);
    if (a->frame_seen != a->frame_gen) {
        a->frame_seen = a->frame_gen;
        if (a->canvas && !a->flip_ok) lv_obj_invalidate(a->canvas);
    }
    if (g->state == AG_ST_DONE && g->state_t > 1.6f && !a->over_done) {
        a->over_done = true;
        bool shop_anyway = false;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
        shop_anyway = getenv("AST_SHOP") != NULL;   /* the workshop's screenshot */
#endif
        if (a->demo && !shop_anyway) {
            sector_go(a, g->sector + 1);
        } else {
            shop_show(a);
        }
    } else if (g->state == AG_ST_OVER && g->state_t > 2.2f && !a->over_done) {
        a->over_done = true;
        over_show(a);
    } else if (g->state == AG_ST_WON && g->state_t > 3.0f && !a->over_done) {
        a->over_done = true;
        won_show(a);
    }
}

static void timer_cb(lv_timer_t *tm)
{
    ast_app_t *a = (ast_app_t *)lv_timer_get_user_data(tm);
    aos_pad_update(&a->pad, lv_tick_get());
    if (a->state == ST_PLAY && a->loaded) {
        game_tick(a);
    } else {
        if (a->job_done == 1) {
            a->job_done = 0;
            if (a->canvas) lv_obj_invalidate(a->canvas);
        }
        ast_menu_pad(a);
    }
    ast_audio_pump();
}

/* ---- life cycle --------------------------------------------------------------------------------------- */

static void canvas_make(ast_app_t *a)
{
    if (!a->fb) return;
    if (!a->canvas) {
        a->canvas = lv_canvas_create(a->root);
        lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(a->canvas, 0);
    }
    lv_canvas_set_buffer(a->canvas, a->fb, a->lay.W, a->lay.H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->canvas, 0, 0);
}

static void *ast_create(aos_app_t *self, lv_obj_t *root)
{
    ast_app_t *a = (ast_app_t *)ast_calloc(sizeof(ast_app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->bg_sector = -1;
    a->game = (ag_game_t *)ast_calloc(sizeof(ag_game_t));
    a->ui = (ast_ui_t *)ast_calloc(sizeof(ast_ui_t));
    size_t px = (size_t)720 * 1280;
    a->fb = (uint16_t *)ast_calloc(px * 2);
    a->bg = (uint16_t *)ast_calloc(px * 2);
    a->fb_hash = (uint32_t *)ast_calloc(sizeof(uint32_t) * AST_NT);
    for (int i = 0; i < 3; i++) a->pb[i].hash = (uint32_t *)ast_calloc(sizeof(uint32_t) * AST_NT);
    a->mx = aos_hal_mutex_create();
    if (!ast_in_mx) ast_in_mx = aos_hal_mutex_create();
    bool ok = a->game && a->ui && a->fb && a->bg && a->fb_hash && a->pb[0].hash && a->pb[1].hash && a->pb[2].hash && a->mx &&
              ast_in_mx && ast_gfx_init(&a->gfx);
    lv_obj_set_style_bg_color(root, C(0x02040A), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(root);
    ast_cfg_load(&a->cfg);
    int w = lv_obj_get_width(root), h = lv_obj_get_height(root);
    if (w < 64 || h < 64) {
        w = 720;
        h = 1280;
    }
    ast_layout(a, w, h);
    if (!ok) {
        ast_label(root, _("No hay memoria para el juego"), aos_font_body, 0xFF6A6A);
        aos_hal_log("asteroides", "no memory");
        return a;
    }
    ast_gfx_size(&a->gfx, w, h);
    ast_gfx_world(&a->gfx, a->bg);
    snprintf(a->ui->s_title, sizeof a->ui->s_title, "%s", APP_NAME);
    canvas_make(a);
    ast_audio_set(a->cfg.sfx, a->cfg.music);
    ast_audio_open();
    ast_audio_music(1);
    a->worker_on = aos_hal_worker_start_on("asteroides", worker_fn, a, WORKER_STACK, 0, 3);
    a->gfx.one_core = !a->worker_on;
    if (!a->worker_on) aos_hal_log("asteroides", "no worker: everything on LVGL's thread, on one core");
    a->state = ST_MENU;
    a->job = 1;
    if (!a->worker_on) do_job(a);
    ast_menu_show(a, SC_TITLE);
    a->timer = lv_timer_create(timer_cb, FRAME_MS, a);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* AST_PLAY=campaign|arcade|two starts a game straight away (scripts) */
    if (getenv("AST_DIFF")) a->cfg.diff = atoi(getenv("AST_DIFF")) % AG_DIFF_N;
    if (getenv("AST_CONTROLS")) a->cfg.controls = atoi(getenv("AST_CONTROLS")) ? CTL_BUTTONS : CTL_STICK;
    const char *play = getenv("AST_PLAY");
    if (play) {
        while (a->job) aos_hal_sleep_ms(5);
        if (!strcmp(play, "arcade")) ast_game_start(a, AG_ARCADE, 1, false);
        else if (!strcmp(play, "two")) ast_game_start(a, AG_ARCADE, 2, false);
        else ast_game_start(a, AG_CAMPAIGN, 1, false);
    }
#endif
    return a;
}

static void ast_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    ast_app_t *a = (ast_app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->timer = NULL;
    a->run = false;
    if (a->worker_on) aos_hal_worker_stop();
    a->worker_on = false;
    if (a->state == ST_PLAY && a->loaded && a->game->mode == AG_ARCADE && a->game->state == AG_ST_OVER && !a->over_done)
        record_insert(a, a->game->score, a->game->sector);
    a->run = false;
    panel_close(a);
    a->loaded = false;
    aos_hal_display_refresh_request(0);
    ast_menu_free(a);
    ast_audio_close();
    ast_gfx_free(&a->gfx);
    for (int i = 0; i < 3; i++) ast_free(a->pb[i].hash);
    ast_free(a->fb_hash);
    ast_free(a->fb);
    ast_free(a->bg);
    ast_free(a->game);
    ast_free(a->ui);
    ast_free(a);
    /* what the app's own allocator still holds: 0 unless something leaked */
    aos_hal_log("asteroides", "closed, %u KB of its own still held", (unsigned)(ast_mem_used() / 1024));
}

/* the screen turned: the same game, laid out again */
static bool ast_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    ast_app_t *a = (ast_app_t *)inst;
    if (!a || !a->game || !a->fb) return false;
    while (a->job) aos_hal_sleep_ms(5);
    bool playing = a->state == ST_PLAY && a->loaded;
    if (playing) {
        a->run = false;
        aos_hal_mutex_lock(a->mx);
        aos_hal_mutex_unlock(a->mx);
    }
    a->root = root;
    lv_obj_update_layout(root);
    int w = lv_obj_get_width(root), h = lv_obj_get_height(root);
    if (w < 64 || h < 64) return false;
    aos_hal_mutex_lock(a->mx);
    ast_layout(a, w, h);
    ast_gfx_size(&a->gfx, w, h);
    a->gfx.salt = (a->gfx.salt + 0x9E3779B9u) | 1u;
    if (playing) {
        ag_resize(a->game, (float)a->lay.fw, (float)a->lay.fh);
        a->bg_sector = -1;
        ast_bg_make(a, ast_palette(a->game));
        a->ui->npart = 0;
    }
    aos_hal_mutex_unlock(a->mx);
    memset(a->fing, 0, sizeof a->fing);
    memset(a->in, 0, sizeof a->in);
    canvas_make(a);
    a->out = !a->lay.land ? AST_OUT_SCREEN : aos_hal_display_get_rotation() == 270 ? AST_OUT_ROT270 : AST_OUT_ROT90;
    for (int i = 0; i < 3; i++) {
        a->pb[i].buf = NULL;
        a->pb[i].flip = 0;
    }
    if (playing) {
        int kind = a->panel_kind;
        panel_close(a);
        a->fb_full = true;
        aos_hal_mutex_lock(a->mx);
        a->gfx.one_core = true;
        ast_scene_draw(a, 0.0f);
        ast_gfx_end(&a->gfx, a->fb, AST_OUT_SCREEN, a->fb_hash, true);
        a->gfx.one_core = !a->worker_on;
        a->fb_full = false;
        aos_hal_mutex_unlock(a->mx);
        lv_obj_invalidate(a->canvas);
        a->run = true;
        a->loaded = true;
        /* a turn is a pause, except over the workshop or the end */
        if (kind == PN_SHOP) shop_show(a);
        else if (kind == PN_OVER) over_show(a);
        else if (kind == PN_WON) won_show(a);
        else pause_show(a);
    } else {
        a->job_done = 0;
        a->job = 1;
        if (!a->worker_on) do_job(a);
        ast_menu_show(a, a->screen);
    }
    return true;
}

static void ast_show(aos_app_t *self, void *inst)
{
    (void)self;
    ast_app_t *a = (ast_app_t *)inst;
    if (!a) return;
    if (a->loaded) ast_scene_strings(a);
    ast_audio_pause(a->panel != NULL && a->panel_kind == PN_PAUSE);
}

static void ast_hide(aos_app_t *self, void *inst)
{
    (void)self;
    ast_app_t *a = (ast_app_t *)inst;
    if (!a) return;
    if (a->state == ST_PLAY && a->loaded) {
        if (!a->panel && a->game->state != AG_ST_OVER) pause_show(a);
        game_hold(a);
    }
    ast_audio_pause(true);
}

static bool ast_back(aos_app_t *self, void *inst)
{
    (void)self;
    ast_app_t *a = (ast_app_t *)inst;
    if (!a) return false;
    if (a->state == ST_PLAY) {
        if (a->panel_kind == PN_PAUSE) panel_close(a);
        else if (!a->panel) pause_show(a);
        return true;
    }
    return ast_menu_back(a);
}

static bool ast_button_cb(aos_app_t *self, void *inst, int action)
{
    (void)self;
    ast_app_t *a = (ast_app_t *)inst;
    if (!a || a->state != ST_PLAY || action != AOS_BUTTON_CLICK) return false;
    a->button_pause = true;
    return true;
}

/* a neon ship over a split rock, on deep blue */
static const uint8_t AST_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -16, 14, 44, 44, AIC_CIRCLE, AIC_C_LIT(0x0B1830), 255),
    AIC_BORDER(3, AIC_C_LIT(0x5CF2FF), 255),
    AIC_RECT(AIC_CENTER, 22, -20, 24, 24, AIC_CIRCLE, AIC_C_LIT(0x0B1830), 255),
    AIC_BORDER(3, AIC_C_LIT(0xFF5FD2), 255),
    AIC_RECT(AIC_CENTER, 16, 22, 30, 6, 3, AIC_C_LIT(0xFFFFFF), 255),
    AIC_ROT(-450),
    AIC_RECT(AIC_CENTER, 28, 10, 6, 6, AIC_CIRCLE, AIC_C_LIT(0x8AF8FF), 255),
    AIC_END
};

static bool ast_init(aos_app_t *app)
{
    app->desc.id       = "demo.asteroides";
    app->desc.name     = "Deriva";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x123A5C;
    app->desc.color_b  = 0x050A18;
    app->desc.order    = 170;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    aos_icon_set_ops(app, AST_ICON, sizeof AST_ICON);
    app->create  = ast_create;
    app->destroy = ast_destroy;
    app->show    = ast_show;
    app->hide    = ast_hide;
    app->back    = ast_back;
    app->button  = ast_button_cb;
    app->resize  = ast_resize;
    return true;
}

AOS_APP_ENTRY(ast_init);
