/*
 * TANQUES - a battle: the turn's states, the cameras, the HUD, touch and
 * the joystick (see tq_app.h)
 */
#include "tq_app.h"

#include <stdio.h>
#include <string.h>

#define DRIVE_MS   3.2f             /* m/s */
#define TURN_RS    (200.0f * TQ_DEG)

static void set_state(app_t *a, int s);
static void hud_refresh(app_t *a);

static aos_pad_t s_pad;
static float s_cur_x, s_cur_y;      /* the joystick's cursor on the map */
static bool s_cursor;

/* --------------------------------------------------------------------------
 * The cameras
 * -------------------------------------------------------------------------- */

static tq_tank_t *cur_tank(app_t *a)
{
    return &a->st.tank[a->st.cur];
}

/* The camera goes to (x, y). Unless snapping, it only moves when the spot
 * leaves the middle of the screen (and then glides to it): a battle view
 * that stands still is redrawn only round what moves. */
static void cam_to(app_t *a, float x, float y, bool snap)
{
    if (!snap) {
        tq_v3_t F, R, U;
        tq_top_axes(&F, &R, &U);
        float dx = x - a->cam_x, dy = y - a->cam_y;
        float sx = TQ_TOP_S * (dx * R.x + dy * R.y), sy = -TQ_TOP_S * (dx * U.x + dy * U.y);
        bool gliding = a->cam_follow && ((a->cam_tx - a->cam_x) * (a->cam_tx - a->cam_x) +
                                         (a->cam_ty - a->cam_y) * (a->cam_ty - a->cam_y)) > 0.0004f;
        if (!gliding && fabsf(sx) < a->W * 0.27f && fabsf(sy) < a->H * 0.25f) return;
        /* it jumps: one frame where a glide is many, and each costs about
         * the same on the board (the user chose it, 2026-11-23) */
        snap = true;
    }
    tq_scene_top_clamp(a->ter, a->W, a->H, &x, &y);
    a->cam_tx = x;
    a->cam_ty = y;
    a->cam_follow = true;
    if (snap) {
        a->cam_x = x;
        a->cam_y = y;
    }
}

void tqb_fill(app_t *a, tq_req_t *r)
{
    r->hp_bars = true;
    if (a->bstate == B_AIM) {
        const tq_tank_t *k = cur_tank(a);
        const tq_tank_type_t *T = &TQ_TANK[k->type];
        r->mode = TQ_VIEW_FP;
        r->me = a->st.cur;
        r->eye = tq_fp_eye(k->x, k->y, tq_tank_z(a->ter, k), k->tyaw, T->eye_x, T->eye_y, T->eye_z);
        r->yaw = k->tyaw;
        r->pitch = TQ_FP_PITCH * TQ_DEG;
        r->fp_half = a->blit_ok == 1;
        return;
    }
    r->mode = TQ_VIEW_TOP;
    float cx = a->cam_x, cy = a->cam_y;
    tq_scene_top_clamp(a->ter, a->W, a->H, &cx, &cy);
    r->cx = cx;
    r->cy = cy;
    if (a->bstate != B_MOVE && a->bstate != B_DRIVE) r->st.show_reach = false;
    if (s_cursor && a->bstate == B_MOVE && r->st.npath < 2) {
        /* the joystick's cursor: a path preview to it */
        int ap;
        tq_game_route(&r->st, a->ter, s_cur_x, s_cur_y, &ap);
    }
}

bool tqb_animating(app_t *a)
{
    if (a->st.npart || tq_game_flying(&a->st) || a->st.beam_t > 0) return true;
    for (int m = 0; m < TQ_MAX_MINES; m++)
        if (a->st.mine[m].live) return true;
    switch (a->bstate) {
    case B_DRIVE:
    case B_AI_AIM:
    case B_FLY:
    case B_BOOM:
        return true;
    default:
        break;
    }
    float dx = a->cam_tx - a->cam_x, dy = a->cam_ty - a->cam_y;
    return a->cam_follow && dx * dx + dy * dy > 0.0004f;
}

/* --------------------------------------------------------------------------
 * The floating words
 * -------------------------------------------------------------------------- */

static void say(app_t *a, tq_v3_t p, const char *txt, uint32_t rgb)
{
    int slot = 0;
    for (int i = 0; i < TQ_LABELS; i++) {
        if (a->lbl_t[i] <= 0) {
            slot = i;
            break;
        }
        if (a->lbl_t[i] < a->lbl_t[slot]) slot = i;
    }
    if (!a->lbl[slot]) return;
    lv_label_set_text(a->lbl[slot], txt);
    lv_obj_set_style_text_color(a->lbl[slot], lv_color_hex(rgb), 0);
    lv_obj_remove_flag(a->lbl[slot], LV_OBJ_FLAG_HIDDEN);
    a->lbl_t[slot] = 1.7f;
    a->lbl_p[slot] = p;
}

/* What a landing sounds like */
static void event_sound(app_t *a, const tq_event_t *e, bool direct, bool ko)
{
    static uint32_t last_blast;
    const tq_weapon_t *W = &TQ_WEAPON[e->weapon];
    uint32_t now = lv_tick_get();
    switch (e->type) {
    case TQ_EV_BLAST:
        if (W->fx == TQ_FX_QUAKE) {
            tq_snd(TQ_SND_QUAKE);
            break;
        }
        /* a cluster's or a shotgun's: not five big ones on top of each other */
        if (now - last_blast < 90 && !ko) break;
        last_blast = now;
        tq_snd(e->split || W->radius < 1.5f ? TQ_SND_BLAST_S : W->radius < 3.5f ? TQ_SND_BLAST : TQ_SND_BLAST_L);
        if (e->water) tq_snd(TQ_SND_SPLASH);
        if (W->fx == TQ_FX_NAPALM) tq_snd(TQ_SND_BURN);
        break;
    case TQ_EV_MINE: tq_snd(TQ_SND_BLAST); break;
    case TQ_EV_MISS: tq_snd(TQ_SND_MISS); break;
    case TQ_EV_HEAL: tq_snd(TQ_SND_HEAL); break;
    case TQ_EV_SHIELD: tq_snd(TQ_SND_SHIELD); break;
    case TQ_EV_EMP: tq_snd(TQ_SND_EMP); break;
    case TQ_EV_TELEPORT: tq_snd(TQ_SND_TELEPORT); break;
    case TQ_EV_BURN: tq_snd(TQ_SND_BURN); break;
    case TQ_EV_TESLA: tq_snd(TQ_SND_TESLA); break;
    case TQ_EV_FIRE2: tq_snd(TQ_SND_FIRE); break;
    case TQ_EV_POINT: tq_snd(TQ_SND_POINT); break;
    case TQ_EV_FLAG_TAKE:
    case TQ_EV_FLAG_BACK: tq_snd(TQ_SND_FLAG); break;
    case TQ_EV_CAPTURE: tq_snd(TQ_SND_CAPTURE); break;
    case TQ_EV_CHEST: tq_snd(TQ_SND_COIN); break;
    case TQ_EV_NONE:
        if (W->fx == TQ_FX_MINES) tq_snd(TQ_SND_MINES);
        else if (W->fx == TQ_FX_SMOKE) tq_snd(TQ_SND_SMOKE);
        else if (W->fx == TQ_FX_WALL) tq_snd(TQ_SND_WALL);
        break;
    default:
        break;
    }
    if (direct) tq_snd(TQ_SND_CLANG);
    if (ko) tq_snd(TQ_SND_KO);
    (void)a;
}

/* What a weapon sounds like leaving the barrel */
static void fire_sound(int w)
{
    const tq_weapon_t *W = &TQ_WEAPON[w];
    if (W->fx == TQ_FX_AIRSTRIKE) tq_snd(TQ_SND_FIRE_CALL);
    else if (W->fx == TQ_FX_SCATTER) tq_snd(TQ_SND_FIRE_SCATTER);
    else if (w == TQ_W_SNIPER) tq_snd(TQ_SND_FIRE_SNIPER);
    else if (W->fly == TQ_FLY_BEAM) tq_snd(TQ_SND_FIRE_LASER);
    else if (W->fly == TQ_FLY_GUIDED) tq_snd(TQ_SND_FIRE_ROCKET);
    else if (W->fly == TQ_FLY_DRILL) tq_snd(TQ_SND_FIRE_DRILL);
    else if (W->fly == TQ_FLY_LOB) tq_snd(TQ_SND_FIRE_LOB);
    else if (W->fly != TQ_FLY_SELF) tq_snd(TQ_SND_FIRE);
}

/* The first tank of a side (its colour) */
static int side_first(app_t *a, int side)
{
    for (int k = 0; k < a->st.ntank; k++)
        if ((a->st.tank[k].team & 1) == side) return k;
    return 0;
}

static void show_event(app_t *a, const tq_event_t *e)
{
    char txt[40];
    if (e->dirty.x1 >= e->dirty.x0) tq_scene_world_dirty(e->dirty);
    bool any = false, direct = false, ko = false;
    for (int k = 0; k < a->st.ntank; k++) {
        if (!e->damage[k]) continue;
        const tq_tank_t *tk = &a->st.tank[k];
        snprintf(txt, sizeof txt, e->direct[k] ? "-%d!" : "-%d", e->damage[k]);
        say(a, tq_v3(tk->x, tk->y, tq_tank_z(a->ter, tk) + 4.2f), txt, 0xFF5A46);
        if (!tk->alive) say(a, tq_v3(tk->x, tk->y, tq_tank_z(a->ter, tk) + 6.0f), _("¡Fuera de combate!"), 0xFFE9A8);
        any = true;
        direct |= e->direct[k];
        ko |= !tk->alive;
    }
    event_sound(a, e, direct, ko);
    switch (e->type) {
    case TQ_EV_BLAST:
        if (!any) say(a, tq_v3(e->at.x, e->at.y, e->at.z + 4.0f), _("¡Cerca!"), 0xFFFFFF);
        break;
    case TQ_EV_MISS:
        say(a, e->at, _("¡Afuera!"), 0xFFFFFF);
        break;
    case TQ_EV_HEAL:
        snprintf(txt, sizeof txt, "+%d", e->value);
        say(a, e->at, txt, 0x5ED16A);
        break;
    case TQ_EV_SHIELD:
        say(a, e->at, _("¡Escudo!"), 0x46C8FF);
        break;
    case TQ_EV_EMP:
        say(a, e->at, e->value ? _("¡Sin energía!") : _("EMP"), 0x5CC8E8);
        break;
    case TQ_EV_TELEPORT:
        say(a, e->at, _("¡Zas!"), 0xC77DFF);
        break;
    case TQ_EV_MINE:
        say(a, tq_v3(e->at.x, e->at.y, e->at.z + 3.0f), _("¡Mina!"), 0xFFD23C);
        break;
    case TQ_EV_BURN:
        say(a, tq_v3(e->at.x, e->at.y, e->at.z + 1.0f), _("¡Quema!"), 0xFF8A1E);
        break;
    case TQ_EV_TESLA:
        if (!any) say(a, e->at, _("¡Zap!"), 0x7FE0FF);
        break;
    case TQ_EV_POINT:
        say(a, e->at, "+1", 0xFFD23C);
        break;
    case TQ_EV_FLAG_TAKE:
        say(a, e->at, _("¡Bandera!"), a->rgb[side_first(a, e->value)]);
        break;
    case TQ_EV_FLAG_BACK:
        say(a, e->at, _("¡Recuperada!"), 0xFFFFFF);
        break;
    case TQ_EV_CAPTURE:
        say(a, e->at, _("¡Captura!"), 0xFFD23C);
        break;
    case TQ_EV_CHEST:
        say(a, e->at, _("¡Un cofre!"), 0xFFD23C);
        if (e->value == 0 && a->humans == 1) a->chest_coins += 30;
        break;
    default:
        break;
    }
    hud_refresh(a);
}

/* --------------------------------------------------------------------------
 * The turn
 * -------------------------------------------------------------------------- */

static void finish(app_t *a)
{
    int w = tq_game_winner(&a->st);
    a->winner = w;
    /* coins: what the people's tanks did, and the win */
    int coins = 0, stars = 0;
    const tq_tank_t *me = &a->st.tank[0];
    bool won = w == me->team;
    coins += me->dealt / 2 + me->kills * 40 + a->chest_coins;
    if (a->st.mode == TQ_MODE_WAVES) {
        /* the waves lived through: the one it ended in did not count */
        a->waves_done = won ? a->st.wave : a->st.wave - 1;
        coins += 60 * a->waves_done;
        if (a->humans == 1 && a->waves_done > a->save.best_wave) a->save.best_wave = (uint8_t)a->waves_done;
    }
    if (won) {
        if (a->story) coins += TQ_MISSION[a->mission].reward;
        else coins += 120 + 80 * (a->st.ntank - 1) + 60 * a->setup.diff;
        stars = me->hp * 3 > me->hpmax * 2 ? 3 : (me->hp * 3 > me->hpmax ? 2 : 1);
    }
    if (a->humans > 1) coins = 0;
    a->coins_won = coins;
    a->stars_won = stars;
    a->save.coins += coins;
    a->save.battles++;
    if (won && a->humans == 1) a->save.wins++;
    if (a->story && won && stars > a->save.stars[a->mission]) a->save.stars[a->mission] = (uint8_t)stars;
    tq_snd_engine(0);
    tq_snd_flight(0, 0, 0);
    tq_snd(won || a->humans > 1 ? TQ_SND_WIN : TQ_SND_LOSE);
    tqa_save(a);
    aos_hal_log("tanques", "battle over: team %d won, %d coins, %d stars | %s, round %d, score %d-%d, wave %d", w, coins,
                stars, TQ_MODE_NAME[a->st.mode], a->st.round, a->st.score[0], a->st.score[1], a->st.wave);
    tqa_screen(a, SC_RESULT);
}

static void begin_turn(app_t *a)
{
    tq_event_t ev;
    tq_game_turn(&a->st, a->ter, &ev);
    if (ev.type == TQ_EV_BURN) show_event(a, &ev);
    tq_tank_t *k = cur_tank(a);
    if (!k->alive || tq_game_winner(&a->st) != -1) {
        if (tq_game_winner(&a->st) != -1) {
            finish(a);
            return;
        }
        a->st.cur = tq_game_next(&a->st);
        begin_turn(a);
        return;
    }
    a->st.npath = 0;
    a->st.narc = 0;
    tq_game_reach(&a->st, a->ter);
    cam_to(a, k->x, k->y, false);
    s_cur_x = k->x;
    s_cur_y = k->y;
    if (!k->ai) tq_snd(TQ_SND_TURN);
    if (k->ai) set_state(a, B_AI_THINK);
    else if (a->humans > 1) set_state(a, B_HANDOVER);
    else set_state(a, B_MOVE);
}

static void next_turn(app_t *a)
{
    if (tq_game_wave_due(&a->st)) {
        tqa_wait_worker(a);
        int n = tq_game_wave(&a->st, a->ter);
        char txt[48];
        snprintf(txt, sizeof txt, _("¡Oleada %d!"), a->st.wave);
        const tq_tank_t *k = &a->st.tank[0];
        say(a, tq_v3(k->x, k->y, tq_tank_z(a->ter, k) + 5.0f), txt, 0xFFD23C);
        tq_snd(TQ_SND_WAVE);
        aos_hal_log("tanques", "wave %d: %d tanks", a->st.wave, n);
    }
    if (tq_game_winner(&a->st) != -1) {
        finish(a);
        return;
    }
    a->st.cur = tq_game_next(&a->st);
    begin_turn(a);
}

void tqb_start(app_t *a, const tq_setup_t *su, bool story, int mission)
{
    tqa_wait_worker(a);
    a->setup = *su;
    a->story = story;
    a->mission = mission;
    a->humans = 0;
    uint32_t types = 0, fps = 0;
    for (int k = 0; k < su->ntank; k++) {
        types |= 1u << su->t[k].type;
        if (!su->t[k].ai) {
            fps |= 1u << su->t[k].type;
            a->humans++;
        }
    }
    tq_art_use_show(-1);
    if (!tq_art_use_biome(su->biome) || !tq_art_use_tanks(types, fps)) aos_ui_toast(_("No hay memoria para el arte"), 2000);
    tqa_paints(a);
    tq_game_new(&a->st, a->ter, su);
    a->chest_coins = 0;
    a->waves_done = 0;
    tq_scene_world_all();
    {
        /* what this battle asks, if it asks more than knocking them out */
        const char *info = story ? TQ_MISSION[mission].info : NULL;
        if (!info && su->mode != TQ_MODE_CLASSIC) info = TQ_MODE_INFO[su->mode];
        if (info) aos_ui_toast(_(info), 3000);
    }
    a->st.cur = 0;
    cam_to(a, a->st.tank[0].x, a->st.tank[0].y, true);
    a->screen = SC_BATTLE;
    tqa_screen(a, SC_BATTLE);
    begin_turn(a);
    aos_hal_log("tanques", "battle: %s, %.0f m, %d tanks, art %u KB, game %u KB", TQ_BIOME_KEY[su->biome],
                TQ_SIZE_M[su->size % TQ_SIZES], su->ntank, (unsigned)(tq_art_bytes() / 1024),
                (unsigned)(tq_mem_used() / 1024));
}

static void aim_changed(app_t *a)
{
    tq_game_arc(&a->st, a->ter);
    hud_refresh(a);
    a->dirty = true;
}

static void enter_aim(app_t *a)
{
    if (a->bstate != B_MOVE && a->bstate != B_BOOM) return;
    tq_tank_t *k = cur_tank(a);
    if (!tq_can_fire(k)) {
        next_turn(a);               /* no shot left: the turn ends */
        return;
    }
    if (!tq_slot_ready(k, k->sel)) {
        for (int w = 0; w < k->nload; w++)
            if (tq_slot_ready(k, w)) {
                k->sel = w;
                break;
            }
    }
    if (k->shots) {
        /* the second shot: aimed where the first one was */
        set_state(a, B_AIM);
        return;
    }
    /* the turret stays where it was left (turning with the hull as it
     * drives): finding the other tank is the player's */
    const tq_weapon_t *W = &TQ_WEAPON[tq_weapon_of(k)];
    k->pitch = tq_clampf(k->pitch, W->min_pitch * TQ_DEG, W->max_pitch * TQ_DEG);
    set_state(a, B_AIM);
}

static void fire(app_t *a)
{
    tq_tank_t *k = cur_tank(a);
    if (!tq_slot_ready(k, k->sel)) return;
    tq_event_t ev[TQ_MAX_EVENTS];
    int nev = 0;
    tqa_wait_worker(a);
    fire_sound(tq_weapon_of(k));
    tq_game_fire(&a->st, a->ter, ev, &nev);
    for (int i = 0; i < nev; i++) show_event(a, &ev[i]);
    set_state(a, tq_game_flying(&a->st) ? B_FLY : B_BOOM);
}

/* --------------------------------------------------------------------------
 * HUD
 * -------------------------------------------------------------------------- */

static void on_aim(lv_event_t *e)
{
    enter_aim((app_t *)lv_event_get_user_data(e));
}

static void on_back(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->bstate != B_AIM) return;
    /* after a shot there is no driving: the button ends the turn */
    if (cur_tank(a)->shots) next_turn(a);
    else set_state(a, B_MOVE);
}

static void on_weapon(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
    tq_tank_t *k = cur_tank(a);
    for (int w = 0; w < k->nload; w++) {
        if (b != a->btn_w[w]) continue;
        if (!tq_slot_ready(k, w)) return;
        k->sel = w;
        const tq_weapon_t *W = &TQ_WEAPON[k->load[w]];
        k->pitch = tq_clampf(k->pitch, W->min_pitch * TQ_DEG, W->max_pitch * TQ_DEG);
        if (W->fly == TQ_FLY_LOB && k->pitch < 40 * TQ_DEG) k->pitch = 45 * TQ_DEG;
        aim_changed(a);
    }
}

static void on_fire(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->bstate == B_AIM) fire(a);
}

static void on_leave(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    bool yes = lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e)) != NULL;
    if (yes) {
        tqa_screen(a, a->story ? SC_STORY : SC_MENU);
        return;
    }
    set_state(a, a->bstate_before);
}

static void hud_refresh(app_t *a)
{
    if (!a->top_bar) return;
    const tq_state_t *s = &a->st;
    for (int k = 0; k < s->ntank; k++) {
        const tq_tank_t *tk = &s->tank[k];
        lv_bar_set_value(a->chip_hp[k], tk->hp * 100 / (tk->hpmax ? tk->hpmax : 1), LV_ANIM_ON);
        lv_obj_set_style_border_width(a->chip[k], k == s->cur ? 4 : 0, 0);
        lv_obj_set_style_opa(a->chip[k], tk->alive ? LV_OPA_COVER : (tk->gone ? LV_OPA_20 : LV_OPA_40), 0);
    }
    if (a->mode_lbl) {
        char m[64] = "";
        switch (s->mode) {
        case TQ_MODE_WAVES:
            if (s->target) snprintf(m, sizeof m, _("Oleada %d de %d"), s->wave, s->target);
            else snprintf(m, sizeof m, _("Oleada %d"), s->wave);
            break;
        case TQ_MODE_HILL: snprintf(m, sizeof m, _("Puntos %d-%d (a %d)"), s->score[0], s->score[1], s->target); break;
        case TQ_MODE_FLAG: snprintf(m, sizeof m, _("Banderas %d-%d (a %d)"), s->score[0], s->score[1], s->target); break;
        case TQ_MODE_TREASURE: {
            int left = 0;
            for (int c = 0; c < s->nchest; c++) left += s->chest_alive[c];
            snprintf(m, sizeof m, _("Cofres %d-%d (quedan %d)"), s->score[0], s->score[1], left);
            break;
        }
        default:
            if (s->goal == TQ_GOAL_HOLD || s->goal == TQ_GOAL_FAST)
                snprintf(m, sizeof m, _("Ronda %d de %d"), s->round, s->goal_n);
            break;
        }
        lv_label_set_text(a->mode_lbl, m);
    }
    char txt[160];
    const tq_tank_t *k = &s->tank[s->cur];
    if (k->ai) snprintf(txt, sizeof txt, _("Juega %s"), k->name);
    else if (a->humans == 1) snprintf(txt, sizeof txt, "%s", _("Tu turno"));
    else snprintf(txt, sizeof txt, _("Turno: %s"), k->name);
    lv_label_set_text(a->turn_lbl, txt);
    float wv = sqrtf(s->wind_x * s->wind_x + s->wind_y * s->wind_y);
    if (wv < 0.2f) {
        lv_label_set_text(a->wind_lbl, s->grav_k < 0.99f ? _("Gravedad baja") : "");
    } else {
        /* the wind's arrow as seen on the battle view (yaw 20 deg) */
        static const char *const ARROW[8] = { "\xE2\x86\x92", "\xE2\x86\x97", "\xE2\x86\x91", "\xE2\x86\x96",
                                              "\xE2\x86\x90", "\xE2\x86\x99", "\xE2\x86\x93", "\xE2\x86\x98" };
        float ang = atan2f(s->wind_y, s->wind_x) - TQ_TOP_YAW * TQ_DEG;
        int o = ((int)floorf(ang / (TQ_PI / 4) + 0.5f) % 8 + 8) % 8;
        snprintf(txt, sizeof txt, _("Viento %s %.0f"), ARROW[o], wv);
        lv_label_set_text(a->wind_lbl, txt);
    }
    snprintf(txt, sizeof txt, "%d AP", k->ap);
    lv_label_set_text(a->ap_lbl, txt);
    int cheap = 99;
    for (int w = 0; w < k->nload; w++)
        if (k->uses[w] && tq_shot_ap(k, w) < cheap) cheap = tq_shot_ap(k, w);
    for (int i = 0; i < 14; i++) {
        tqa_show(a->ap_dot[i], i < k->apmax);
        uint32_t c = 0x3A4652;
        if (i < k->ap) c = i >= k->ap - cheap ? 0xFFD23C : 0x46C8FF;
        lv_obj_set_style_bg_color(a->ap_dot[i], lv_color_hex(c), 0);
    }
    tqa_btn_text(a->btn_aim, tq_can_fire(k) ? _("Apuntar") : _("Terminar"));
    tqa_btn_text(a->btn_back, k->shots ? _("Terminar") : _("Mover"));
    for (int w = 0; w < TQ_SLOTS_MAX; w++) {
        tqa_show(a->btn_w[w], w < k->nload);
        if (w >= k->nload) continue;
        const tq_weapon_t *W = &TQ_WEAPON[k->load[w]];
        if (k->uses[w] != 0xFF) snprintf(txt, sizeof txt, "%s\n%d AP  x%d", _(W->name), tq_shot_ap(k, w), k->uses[w]);
        else snprintf(txt, sizeof txt, "%s\n%d AP", _(W->name), tq_shot_ap(k, w));
        tqa_btn_text(a->btn_w[w], txt);
        if (!tq_slot_ready(k, w)) lv_obj_add_state(a->btn_w[w], LV_STATE_DISABLED);
        else lv_obj_remove_state(a->btn_w[w], LV_STATE_DISABLED);
        lv_obj_set_style_border_width(a->btn_w[w], w == k->sel ? 5 : 0, 0);
        lv_obj_set_style_bg_color(a->btn_w[w], lv_color_hex(W->rgb), 0);
    }
    float yaw_rel = (k->tyaw - k->yaw) / TQ_DEG;
    while (yaw_rel > 180) yaw_rel -= 360;
    while (yaw_rel < -180) yaw_rel += 360;
    if (TQ_WEAPON[tq_weapon_of(k)].fly == TQ_FLY_SELF)
        snprintf(txt, sizeof txt, "%s", _(TQ_WEAPON[tq_weapon_of(k)].info));
    else
        snprintf(txt, sizeof txt, _("Elevación %d°   Giro %d°"), (int)roundf(k->pitch / TQ_DEG), (int)roundf(-yaw_rel));
    if (k->shots) {
        size_t l = strlen(txt);
        snprintf(txt + l, sizeof txt - l, "   %s", _("Segundo disparo: AP x2"));
    }
    lv_label_set_text(a->aim_lbl, txt);
}

void tqb_build(app_t *a)
{
    lv_obj_t *ui = a->ui;
    int W = a->W, H = a->H;
    bool land = W > H;
    int m = 14;
    const tq_state_t *s = &a->st;

    /* the tanks and whose turn */
    int tbw = land ? 900 : W - 2 * m;
    a->top_bar = tqa_panel(ui, (W - tbw) / 2, m, tbw, 112, 26);
    int n = s->ntank > 0 ? s->ntank : 1;
    int cw = (tbw - 16 - (n - 1) * 8) / n;
    for (int k = 0; k < s->ntank; k++) {
        lv_obj_t *c = lv_obj_create(a->top_bar);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, cw, 66);
        lv_obj_set_pos(c, 8 + k * (cw + 8), 6);
        lv_obj_set_style_radius(c, 18, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(a->rgb[k]), 0);
        lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        a->chip[k] = c;
        lv_obj_t *nm = tqa_label(c, s->tank[k].name, aos_font_tiny, 0xFFFFFF);
        lv_obj_set_width(nm, cw - 16);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t *bar = lv_bar_create(c);
        lv_obj_set_size(bar, cw - 28, 12);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A3440), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(a->rgb[k]), LV_PART_INDICATOR);
        a->chip_hp[k] = bar;
    }
    a->turn_lbl = tqa_label(a->top_bar, "", aos_font_caption, 0xFFE9A8);
    lv_obj_align(a->turn_lbl, LV_ALIGN_BOTTOM_LEFT, 18, -6);
    a->wind_lbl = tqa_label(a->top_bar, "", aos_font_caption, 0xB8E4FF);
    lv_obj_align(a->wind_lbl, LV_ALIGN_BOTTOM_RIGHT, -18, -6);
    a->mode_lbl = tqa_label(a->top_bar, "", aos_font_caption, 0xFFD23C);
    lv_obj_align(a->mode_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* driving: the points and the button to aim */
    int mbw = land ? 700 : W - 2 * m, mbh = 112;
    a->move_bar = tqa_panel(ui, (W - mbw) / 2, H - mbh - m, mbw, mbh, 30);
    a->ap_lbl = tqa_label(a->move_bar, "", aos_font_body, 0xFFFFFF);
    lv_obj_align(a->ap_lbl, LV_ALIGN_TOP_LEFT, 26, 12);
    for (int i = 0; i < 14; i++) {
        lv_obj_t *d = lv_obj_create(a->move_bar);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 18, 18);
        lv_obj_set_style_radius(d, 9, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_pos(d, 26 + i * 23, 64);
        a->ap_dot[i] = d;
    }
    a->btn_aim = tqa_button(a->move_bar, _("Apuntar"), 0xE8572E, 220, 80, on_aim, a);
    lv_obj_align(a->btn_aim, LV_ALIGN_RIGHT_MID, -16, 0);
    a->hint = tqa_label(ui, _("Tocá el mapa adonde ir. Arrastrá para mirar."), aos_font_caption, 0xFFFFFF);
    lv_obj_set_style_bg_color(a->hint, lv_color_hex(0x16202A), 0);
    lv_obj_set_style_bg_opa(a->hint, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(a->hint, 16, 0);
    lv_obj_set_style_pad_ver(a->hint, 8, 0);
    lv_obj_set_style_radius(a->hint, 18, 0);
    lv_obj_align(a->hint, LV_ALIGN_BOTTOM_MID, 0, -(mbh + m + 12));

    /* aiming: the weapons, fire, and back to driving */
    int abw = land ? 1000 : W - 2 * m, abh = land ? 150 : 262;
    a->aim_bar = tqa_panel(ui, (W - abw) / 2, H - abh - m, abw, abh, 30);
    a->aim_lbl = tqa_label(a->aim_bar, "", aos_font_caption, 0xFFE9A8);
    lv_obj_align(a->aim_lbl, LV_ALIGN_TOP_LEFT, 22, 14);
    a->btn_back = tqa_button(a->aim_bar, _("Mover"), 0x3A4652, 150, land ? 44 : 52, on_back, a);
    lv_obj_set_style_text_font(lv_obj_get_child(a->btn_back, 0), aos_font_caption, 0);
    lv_obj_align(a->btn_back, LV_ALIGN_TOP_RIGHT, -16, 6);
    int bw = land ? 178 : 208, bh = land ? 80 : 84;
    for (int w = 0; w < TQ_SLOTS_MAX; w++) {
        a->btn_w[w] = tqa_button(a->aim_bar, "", 0x2E6FB0, bw, bh, on_weapon, a);
        lv_obj_set_style_text_font(lv_obj_get_child(a->btn_w[w], 0), aos_font_tiny, 0);
        if (land) lv_obj_set_pos(a->btn_w[w], 16 + w * (bw + 10), 58);
        else lv_obj_set_pos(a->btn_w[w], 16 + (w % 2) * (bw + 10), 68 + (w / 2) * (bh + 8));
    }
    a->btn_fire = tqa_button(a->aim_bar, _("¡Fuego!"), 0xE23B2E, land ? 200 : 230, land ? 80 : 160, on_fire, a);
    lv_obj_align(a->btn_fire, LV_ALIGN_BOTTOM_RIGHT, -16, land ? -12 : -14);
    /* the first person's picture goes between the bars */
    a->fp_y0 = (m + 112 + m + 1) & ~1;
    a->fp_y1 = (H - abh - 2 * m) & ~1;

    /* the floating words */
    for (int i = 0; i < TQ_LABELS; i++) {
        a->lbl[i] = tqa_label(ui, "", aos_font_title, 0xFF5A46);
        lv_obj_add_flag(a->lbl[i], LV_OBJ_FLAG_HIDDEN);
        a->lbl_t[i] = 0;
    }

    /* the banner between two people's turns */
    a->banner = tqa_panel(ui, 0, 0, land ? 640 : W - 80, 200, 30);
    lv_obj_center(a->banner);
    lv_obj_t *bl = tqa_label(a->banner, "", aos_font_title, 0xFFFFFF);
    lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(bl);

    /* leave? */
    a->leave = tqa_panel(ui, 0, 0, land ? 600 : W - 80, 260, 30);
    lv_obj_set_style_bg_opa(a->leave, LV_OPA_90, 0);
    lv_obj_center(a->leave);
    lv_obj_t *q = tqa_label(a->leave, _("¿Dejar la batalla?"), aos_font_title, 0xFFE9A8);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *yes = tqa_button(a->leave, _("Dejar"), 0xE23B2E, 220, 80, on_leave, a);
    lv_obj_set_user_data(yes, (void *)1);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_LEFT, 30, -36);
    lv_obj_t *no = tqa_button(a->leave, _("Seguir"), 0x2E6FB0, 220, 80, on_leave, a);
    lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -30, -36);

    set_state(a, a->bstate);
}

static void hud_for_state(app_t *a)
{
    int s = a->bstate;
    const tq_tank_t *k = cur_tank(a);
    tqa_show(a->top_bar, true);
    tqa_show(a->move_bar, s == B_MOVE);
    tqa_show(a->hint, s == B_MOVE && k->ap >= 3);
    tqa_show(a->btn_back, s == B_AIM);
    tqa_show(a->aim_bar, s == B_AIM);
    tqa_show(a->banner, s == B_HANDOVER);
    tqa_show(a->leave, s == B_LEAVE);
    if (s == B_HANDOVER && a->banner) {
        char txt[160];
        snprintf(txt, sizeof txt, _("Turno de %s\nPasale la placa"), k->name);
        lv_label_set_text(lv_obj_get_child(a->banner, 0), txt);
    }
    hud_refresh(a);
}

static void set_state(app_t *a, int s)
{
    if (a->bstate == B_AIM && s != B_AIM) {
        /* the canvas under the picture is stale: the next frame is whole */
        a->fp_shown = false;
        a->fp_dark = false;
        a->shown_key = 0;
    }
    a->bstate = s;
    a->st_t = 0;
    a->st.show_reach = s == B_MOVE;
    if (s == B_AIM) aim_changed(a);
    else a->st.narc = 0;
    if (a->top_bar) hud_for_state(a);
    a->dirty = true;
}

bool tqb_back(app_t *a)
{
    if (a->bstate == B_AIM) {
        set_state(a, B_MOVE);
        return true;
    }
    if (a->bstate == B_LEAVE) {
        set_state(a, a->bstate_before);
        return true;
    }
    a->bstate_before = a->bstate;
    set_state(a, B_LEAVE);
    return true;
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

static void go_to(app_t *a, float wx, float wy)
{
    int ap = 0;
    if (!tq_game_route(&a->st, a->ter, wx, wy, &ap)) {
        aos_ui_toast(_("No llega hasta ahí"), 900);
        return;
    }
    a->drive_seg = 1;
    a->drive_cost = ap;
    s_cursor = false;
    set_state(a, B_DRIVE);
}

static void tap(app_t *a, int x, int y)
{
    if (a->bstate == B_HANDOVER) {
        set_state(a, B_MOVE);
        return;
    }
    if (a->bstate != B_MOVE || cur_tank(a)->ai) return;
    float cx = a->cam_x, cy = a->cam_y, wx, wy;
    tq_scene_top_clamp(a->ter, a->W, a->H, &cx, &cy);
    if (!tq_scene_top_pick(a->ter, cx, cy, a->W, a->H, (float)x, (float)y, &wx, &wy)) return;
    go_to(a, wx, wy);
}

void tqb_touch(app_t *a, int ev, int x, int y, int dx, int dy)
{
    if (ev == 9) {                  /* development: straight into the gunner's view */
        enter_aim(a);
        return;
    }
    if (ev == 2) {
        tap(a, x, y);
        return;
    }
    if (ev != 1) return;
    if (a->bstate == B_AIM) {
        tq_tank_t *k = cur_tank(a);
        const tq_weapon_t *W = &TQ_WEAPON[tq_weapon_of(k)];
        k->tyaw -= dx * 0.0035f;
        k->pitch -= dy * 0.0028f;
        k->pitch = tq_clampf(k->pitch, W->min_pitch * TQ_DEG, W->max_pitch * TQ_DEG);
        aim_changed(a);
    } else if (a->bstate != B_LEAVE && a->bstate != B_HANDOVER) {
        pan(a, dx, dy);
    }
}

/* --------------------------------------------------------------------------
 * The joystick: a cursor on the map, the aim with the stick
 * -------------------------------------------------------------------------- */

static void pad_tick(app_t *a, float dt)
{
    aos_pad_update(&s_pad, lv_tick_get());
    if (!s_pad.connected) return;
    tq_tank_t *k = cur_tank(a);
    if (k->ai) return;
    uint32_t p = s_pad.pressed, h = s_pad.held;
    switch (a->bstate) {
    case B_HANDOVER:
        if (p & (AOS_PAD_A | AOS_PAD_START)) set_state(a, B_MOVE);
        break;
    case B_MOVE: {
        /* the cursor moves along the screen's axes */
        float sx = (h & AOS_PAD_RIGHT ? 1.0f : 0) - (h & AOS_PAD_LEFT ? 1.0f : 0);
        float sy = (h & AOS_PAD_UP ? 1.0f : 0) - (h & AOS_PAD_DOWN ? 1.0f : 0);
        if (s_pad.analog) {
            sx += s_pad.x / 32767.0f;
            sy -= s_pad.y / 32767.0f;
        }
        if (sx || sy) {
            tq_v3_t F, R, U;
            tq_top_axes(&F, &R, &U);
            float uh = sqrtf(U.x * U.x + U.y * U.y);
            float sp = 7.0f * dt;
            s_cur_x = tq_clampf(s_cur_x + (sx * R.x + sy * U.x / uh) * sp, 1, a->st.grid - 1);
            s_cur_y = tq_clampf(s_cur_y + (sx * R.y + sy * U.y / uh) * sp, 1, a->st.grid - 1);
            s_cursor = true;
            a->st.npath = 0;
            cam_to(a, s_cur_x, s_cur_y, false);
            a->dirty = true;
        }
        if ((p & AOS_PAD_A) && s_cursor) go_to(a, s_cur_x, s_cur_y);
        else if (p & (AOS_PAD_START | AOS_PAD_R)) enter_aim(a);
        else if (p & AOS_PAD_B) tqb_back(a);
        break;
    }
    case B_AIM: {
        float yaw = (h & AOS_PAD_LEFT ? 1.0f : 0) - (h & AOS_PAD_RIGHT ? 1.0f : 0);
        float pit = (h & AOS_PAD_UP ? 1.0f : 0) - (h & AOS_PAD_DOWN ? 1.0f : 0);
        if (s_pad.analog) {
            yaw -= s_pad.x / 32767.0f;
            pit -= s_pad.y / 32767.0f;
        }
        if (yaw || pit) {
            const tq_weapon_t *W = &TQ_WEAPON[tq_weapon_of(k)];
            k->tyaw += yaw * 40 * TQ_DEG * dt;
            k->pitch = tq_clampf(k->pitch + pit * 20 * TQ_DEG * dt, W->min_pitch * TQ_DEG, W->max_pitch * TQ_DEG);
            aim_changed(a);
        }
        if (p & (AOS_PAD_L | AOS_PAD_R)) {
            for (int i = 1; i <= k->nload; i++) {
                int w = (k->sel + (p & AOS_PAD_R ? i : k->nload - i)) % k->nload;
                if (k->uses[w] && TQ_WEAPON[k->load[w]].ap <= k->ap) {
                    k->sel = w;
                    break;
                }
            }
            aim_changed(a);
        }
        if (p & AOS_PAD_A) fire(a);
        else if (p & AOS_PAD_B) set_state(a, B_MOVE);
        break;
    }
    case B_LEAVE:
        if (p & AOS_PAD_B) tqb_back(a);
        if (p & AOS_PAD_A) tqa_screen(a, a->story ? SC_STORY : SC_MENU);
        break;
    default:
        if (p & AOS_PAD_START) tqb_back(a);
        break;
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
    if (a->drive_seg >= s->npath || !k->alive) {
        k->ap -= a->drive_cost;
        if (k->ap < 0) k->ap = 0;
        if (a->drive_cost) k->moved = true;
        s->npath = 0;
        tqa_wait_worker(a);
        tq_game_reach(s, a->ter);
        if (!k->alive) {
            next_turn(a);
        } else if (!tq_can_fire(k)) {
            /* no AP left for a shot: the turn ends by itself */
            say(a, tq_v3(k->x, k->y, tq_tank_z(a->ter, k) + 3.5f), _("Sin AP"), 0xFFD23C);
            tq_snd(TQ_SND_DENY);
            set_state(a, B_BOOM);
            a->st_t = 1.0f;
        } else if (k->ai) {
            set_state(a, B_AI_AIM);
        } else {
            set_state(a, B_MOVE);
        }
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
            q->rgb = a->ter->biome == TQ_B_NIEVE ? 0xEEF2F8 : 0xC9B48E;
            q->kind = TQ_PART_PUFF;
        }
        tq_event_t ev;
        if (tq_game_mines(s, a->ter, s->cur, &ev)) {
            show_event(a, &ev);
            a->drive_seg = s->npath;            /* the mine stops it */
        }
        if (tq_game_pickups(s, a->ter, s->cur, &ev)) show_event(a, &ev);
    }
    cam_to(a, k->x, k->y, false);
}

void tqb_tick(app_t *a, float dt)
{
    tq_state_t *s = &a->st;
    tq_tank_t *k = cur_tank(a);
    a->st_t += dt;
    s->pulse = fmodf(s->pulse + dt * 0.8f, 1.0f);
    pad_tick(a, dt);
    switch (a->bstate) {
    case B_DRIVE:
        drive_step(a, dt);
        break;
    case B_AI_THINK:
        if (a->st_t > 0.7f) {
            int ap;
            tqa_wait_worker(a);
            tq_ai_plan_move(s, a->ter, &ap);
            if (s->npath >= 2) {
                a->drive_seg = 1;
                a->drive_cost = ap;
                set_state(a, B_DRIVE);
            } else {
                set_state(a, B_AI_AIM);
            }
        }
        break;
    case B_AI_AIM:
        if (a->st_t < dt * 1.5f) {
            tqa_wait_worker(a);
            tq_ai_plan_aim(s, a->ter, &a->ai_yaw, &a->ai_pitch, &a->ai_slot);
            if (a->ai_slot >= 0) k->sel = a->ai_slot;
        }
        if (a->ai_slot < 0) {
            next_turn(a);
            break;
        }
        k->tyaw = turn_to(k->tyaw, a->ai_yaw, 80 * TQ_DEG * dt);
        k->pitch += tq_clampf(a->ai_pitch - k->pitch, -40 * TQ_DEG * dt, 40 * TQ_DEG * dt);
        cam_to(a, k->x, k->y, false);
        if (a->st_t > 0.6f && fabsf(wrap_pi(a->ai_yaw - k->tyaw)) < 0.01f && fabsf(a->ai_pitch - k->pitch) < 0.01f)
            fire(a);
        break;
    case B_FLY: {
        const tq_shell_t *sh = tq_game_lead(s);
        if (sh) cam_to(a, sh->p.x, sh->p.y, false);
        else set_state(a, B_BOOM);
        break;
    }
    case B_BOOM:
        if (a->st_t > 1.8f && !tq_game_flying(s)) {
            if (k->alive && k->shots && tq_can_fire(k) && tq_game_winner(s) == -1) {
                /* it did not drive: a second shot, at twice the AP */
                if (k->ai) set_state(a, B_AI_AIM);
                else enter_aim(a);
            } else {
                next_turn(a);
            }
        }
        break;
    default:
        break;
    }
    if (a->screen != SC_BATTLE) return;         /* the battle ended */
    /* the beds: the engine while driving, the shell in the air */
    tq_snd_engine(a->bstate == B_DRIVE ? 1.0f : 0);
    {
        const tq_shell_t *sh = tq_game_lead(s);
        if (sh && !sh->under && TQ_WEAPON[sh->weapon].fly != TQ_FLY_BEAM) {
            float h = sh->p.z - tq_ter_height(a->ter, sh->p.x, sh->p.y);
            bool rocket = TQ_WEAPON[sh->weapon].fly == TQ_FLY_GUIDED;
            /* the whistle when it comes down; a rocket hisses all the way */
            tq_snd_flight(rocket ? 1 : 0, rocket ? 0.8f : (sh->v.z < 0 ? 0.7f : 0), h);
        } else {
            tq_snd_flight(0, 0, 0);
        }
    }
    /* the world moves on: shells, particles, stones falling */
    tq_event_t ev[TQ_MAX_EVENTS];
    int nev = 0;
    bool flying = tq_game_flying(s);
    if (flying || s->npart || s->beam_t > 0) {
        if (flying) tqa_wait_worker(a);
        tq_game_step(s, a->ter, dt, ev, &nev);
        for (int i = 0; i < nev; i++) show_event(a, &ev[i]);
    }
    /* the camera */
    if (a->cam_follow) {
        /* a short glide that ends: each step of it moves the picture */
        float f = tq_clampf(dt * 6.0f, 0, 1);
        a->cam_x += (a->cam_tx - a->cam_x) * f;
        a->cam_y += (a->cam_ty - a->cam_y) * f;
        float ex = a->cam_tx - a->cam_x, ey = a->cam_ty - a->cam_y;
        if (ex * ex + ey * ey < 0.05f * 0.05f) {
            a->cam_x = a->cam_tx;
            a->cam_y = a->cam_ty;
        }
    }
    /* the floating words follow the battle view */
    for (int i = 0; i < TQ_LABELS; i++) {
        if (a->lbl_t[i] <= 0 || !a->lbl[i]) continue;
        a->lbl_t[i] -= dt;
        if (a->lbl_t[i] <= 0 || a->bstate == B_AIM) {
            lv_obj_add_flag(a->lbl[i], LV_OBJ_FLAG_HIDDEN);
            if (a->bstate == B_AIM) a->lbl_t[i] = 0;
            continue;
        }
        float cx = a->cam_x, cy = a->cam_y, sx, sy;
        tq_scene_top_clamp(a->ter, a->W, a->H, &cx, &cy);
        tq_v3_t p = a->lbl_p[i];
        p.z += (1.7f - a->lbl_t[i]) * 0.8f;
        tq_scene_top_project(a->ter, cx, cy, a->W, a->H, p, &sx, &sy);
        sx = tq_clampf(sx, 140, a->W - 140);
        sy = tq_clampf(sy, 200, a->H - 200);
        lv_obj_update_layout(a->lbl[i]);
        lv_obj_set_pos(a->lbl[i], (int)sx - lv_obj_get_width(a->lbl[i]) / 2, (int)sy - 30);
        lv_obj_set_style_opa(a->lbl[i], (lv_opa_t)(a->lbl_t[i] > 0.5f ? 255 : a->lbl_t[i] * 510), 0);
    }
}
