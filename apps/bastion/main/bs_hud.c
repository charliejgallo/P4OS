/*
 * BASTION - the battle's HUD and what the player does on it (bs_view.h)
 *
 * The bar on top (life, credits, the wave, speed, pause) and a panel
 * below that is one of three things: with nothing chosen, the powers and
 * the button that calls the next wave (with who comes in it); with an
 * empty pad chosen, the towers to build there (a first tap shows one's
 * reach and what it does, a second builds it); with a tower chosen, what
 * it is and its upgrade, its aim and its sale.
 *
 * The buttons of the frame are a list (bs_hud_layout): a finger finds the
 * one under it and a gamepad walks them, so both do the same things. On
 * the field the gamepad's frame goes from pad to pad.
 */
#include "bs_view.h"

#include "aos_i18n.h"
#include "aos_pad.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define C_WHITE  0xFFFF
#define C_DIM    0x9CF3
#define C_GOLD   0xFEA0
#define C_RED    0xF9A6
#define C_GREEN  0x47E8
#define C_PANEL  0x0843
#define C_BTN    0x2149
#define C_BTN_OFF 0x10A3

static const char *const STRINGS[STR_N] = {
    [STR_WAVE] = N_("Oleada"),
    [STR_CALL] = N_("¡OLEADA!"),
    [STR_UPGRADE] = N_("Mejorar"),
    [STR_SELL] = N_("Vender"),
    [STR_MAX] = N_("Al máximo"),
    [STR_FIRST] = N_("Primero"),
    [STR_STRONG] = N_("Más fuerte"),
    [STR_WEAK] = N_("Más débil"),
    [STR_NEAR] = N_("Más cercano"),
    [STR_DMG] = N_("Daño"),
    [STR_RANGE] = N_("Alcance"),
    [STR_KILLS] = N_("Bajas"),
    [STR_LEFT] = N_("Quedan"),
    [STR_HINT_PAD] = N_("Tocá una plataforma para poner una torre"),
    [STR_HINT_BUILD] = N_("Tocá de nuevo para construirla"),
    [STR_HINT_AIM] = N_("Tocá dónde cae el rayo (o el botón otra vez: apunta solo)"),
    [STR_HINT_WAVE] = N_("Entre oleadas podés construir, mejorar y vender con calma"),
    [STR_NO_CREDITS] = N_("No alcanzan los créditos"),
    [STR_BOSS] = N_("¡JEFE!"),
    [STR_JAMMED] = N_("Saboteada"),
    [STR_PER_WAVE] = N_("por oleada"),
    [STR_PER_SEC] = N_("por segundo"),
    [STR_LAST] = N_("ÚLTIMA OLEADA"),
    [STR_AIR] = N_("aire"),
    [STR_GROUND] = N_("tierra"),
    [STR_BOOST] = N_("amplificada"),
    [STR_FINAL] = N_("Oleada final"),
    [STR_MILE] = N_("¡%d OLEADAS!"),
    [STR_MILE_PAY] = N_("+%d núcleos   +%d créditos   +%d vidas"),
    [STR_HINT_AIR] = N_("Voladores: no les llegan Cañón, Lanzallamas, Riel ni Mortero"),
};

static void put(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void bs_ui_init(bs_ui_t *ui)
{
    memset(ui, 0, sizeof *ui);
    ui->sel = ui->pick = ui->aim = -1;
    ui->focus = -1;
    ui->speed = 1;
    ui->seen_wave = -1;
}

void bs_hud_strings(bs_ui_t *ui)
{
    for (int i = 0; i < STR_N; i++) put(ui->str[i], sizeof ui->str[i], _(STRINGS[i]));
    for (int i = 0; i < TW_N; i++) {
        put(ui->tw_name[i], sizeof ui->tw_name[i], _(BS_TOWER[i].name));
        put(ui->tw_desc[i], sizeof ui->tw_desc[i], _(BS_TOWER[i].desc));
    }
    for (int i = 0; i < AL_N; i++) put(ui->al_name[i], sizeof ui->al_name[i], _(BS_ALIEN[i].name));
    for (int i = 0; i < PW_N; i++) put(ui->pw_name[i], sizeof ui->pw_name[i], _(BS_POWER[i].name));
}

static void hint(bs_ui_t *ui, const char *s, float secs)
{
    put(ui->hint, sizeof ui->hint, s);
    ui->hint_t = secs;
}

/* ---- the buttons of the frame ------------------------------------------------------------ */

static void btn(bs_ui_t *ui, int x, int y, int w, int h, int act, int arg, bool enabled)
{
    if (ui->nbtn >= BS_MAX_BTN) return;
    bs_btn_t *b = &ui->btn[ui->nbtn++];
    b->x = (int16_t)x;
    b->y = (int16_t)y;
    b->w = (int16_t)w;
    b->h = (int16_t)h;
    b->act = (uint8_t)act;
    b->arg = (uint8_t)arg;
    b->enabled = enabled;
}

static bool aims(int type)
{
    return !BS_TOWER[type].spin || type == TW_TESLA || type == TW_DRONE;
}

void bs_hud_layout(bs_ui_t *ui, const bs_game_t *g)
{
    ui->nbtn = 0;
    bool playing = g->state == BS_BUILD || g->state == BS_WAVE;
    btn(ui, 556, 8, 76, 72, ACT_SPEED, 0, playing);
    btn(ui, 638, 8, 76, 72, ACT_PAUSE, 0, true);
    if (!playing) return;
    if (ui->sel >= g->w->npad) ui->sel = -1;
    if (ui->sel < 0) {
        for (int i = 0; i < PW_N; i++)
            btn(ui, 12 + i * 112, 1106, 104, 160, ACT_POWER, i, g->kit.power[i] && g->power_cd[i] <= 0);
        if (g->state == BS_BUILD) btn(ui, 356, 1106, 352, 160, ACT_WAVE, 0, true);
    } else if (!g->tw[ui->sel].on) {
        int n = 0;
        for (int t = 0; t < TW_N; t++) {
            if (!g->kit.owned[t]) continue;
            btn(ui, 10 + (n % 7) * 100, 1100 + (n / 7) * 90, 98, 86, ACT_PICK, t, g->credits >= bs_game_cost(g, t, 0));
            n++;
        }
    } else {
        const bs_tower_t *t = &g->tw[ui->sel];
        bool can = t->tier < BS_TIERS - 1;
        btn(ui, 12, 1186, 232, 82, ACT_UPGRADE, 0, can && g->credits >= bs_game_cost(g, t->type, t->tier + 1));
        btn(ui, 252, 1186, 224, 82, ACT_MODE, 0, aims(t->type));
        btn(ui, 484, 1186, 224, 82, ACT_SELL, 0, true);
        btn(ui, 644, 1100, 64, 64, ACT_CLOSE, 0, true);
    }
    if (ui->focus >= ui->nbtn) ui->focus = ui->nbtn - 1;
}

/* ---- what the buttons do ------------------------------------------------------------------- */

static bool auto_strike(bs_game_t *g)
{
    /* where they are thickest */
    int best = -1, most = 0;
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        const bs_alien_t *a = &g->al[i];
        if (!a->on || a->y > 38.0f) continue;
        int n = 0;
        for (int k = 0; k < BS_MAX_ALIENS; k++) {
            const bs_alien_t *o = &g->al[k];
            if (!o->on) continue;
            float dx = o->x - a->x, dy = o->y - a->y;
            if (dx * dx + dy * dy < 6.25f) n += BS_ALIEN[o->type].boss ? 12 : (o->type == AL_BRUTE || o->type == AL_CARRIER ? 4 : 1);
        }
        if (n > most) {
            most = n;
            best = i;
        }
    }
    if (best < 0) {
        bs_game_sound(g, SND_DENY);
        return false;
    }
    return bs_game_power(g, PW_STRIKE, g->al[best].x, g->al[best].y);
}

static void act(bs_ui_t *ui, bs_game_t *g, const bs_btn_t *b)
{
    if (!b->enabled && b->act != ACT_PICK) {
        bs_game_sound(g, SND_DENY);
        if (b->act == ACT_UPGRADE && ui->sel >= 0 && g->tw[ui->sel].tier < BS_TIERS - 1) hint(ui, ui->str[STR_NO_CREDITS], 2.0f);
        return;
    }
    switch (b->act) {
    case ACT_SPEED:
        ui->speed = ui->speed % 3 + 1;
        bs_game_sound(g, SND_TAP);
        break;
    case ACT_PAUSE:
        ui->want_pause = true;
        break;
    case ACT_WAVE:
        bs_game_call_wave(g);
        ui->sel = ui->pick = -1;
        ui->aim = -1;
        ui->focus = -1;
        break;
    case ACT_POWER:
        if (b->arg == PW_STRIKE) {
            if (ui->aim == PW_STRIKE) {
                auto_strike(g);
                ui->aim = -1;
                ui->hint_t = 0;
            } else {
                ui->aim = PW_STRIKE;
                hint(ui, ui->str[STR_HINT_AIM], 6.0f);
                bs_game_sound(g, SND_TAP);
            }
        } else {
            bs_game_power(g, b->arg, 0, 0);
        }
        break;
    case ACT_PICK:
        if (ui->pick == b->arg) {
            if (bs_game_build(g, ui->sel, b->arg)) {
                ui->pick = -1;
                ui->sel = -1;
                ui->focus = -1;
                ui->hint_t = 0;
            } else {
                hint(ui, ui->str[STR_NO_CREDITS], 2.0f);
            }
        } else {
            ui->pick = b->arg;
            bs_game_sound(g, SND_TAP);
        }
        break;
    case ACT_UPGRADE:
        bs_game_upgrade(g, ui->sel);
        break;
    case ACT_SELL:
        bs_game_sell(g, ui->sel);
        ui->sel = -1;
        ui->focus = -1;
        break;
    case ACT_MODE:
        bs_game_mode(g, ui->sel);
        break;
    case ACT_CLOSE:
        ui->sel = ui->pick = -1;
        ui->focus = -1;
        break;
    default: break;
    }
}

static int pad_at(const bs_game_t *g, int x, int y)
{
    int best = -1;
    float bd = 1.35f;
    for (int i = 0; i < g->w->npad; i++) {
        float dx = (float)(x - BS_SX(g->w->pad_x[i])) / 48.0f;
        /* a tower stands above its pad: the finger goes for the tower */
        float dy = (float)(y - (BS_SY(g->w->pad_y[i]) - (g->tw[i].on ? 22 : 0))) / (g->tw[i].on ? 56.0f : 40.0f);
        float d = dx * dx + dy * dy;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

void bs_hud_tap(bs_ui_t *ui, bs_game_t *g, int x, int y)
{
    ui->pad_on = false;
    bs_hud_layout(ui, g);
    for (int i = 0; i < ui->nbtn; i++) {
        const bs_btn_t *b = &ui->btn[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            act(ui, g, b);
            return;
        }
    }
    if (y < BS_TOP_H || y >= BS_PANEL_Y) return;
    if (g->state != BS_BUILD && g->state != BS_WAVE) return;
    if (ui->aim >= 0) {
        bs_game_power(g, ui->aim, ((float)x - 360.0f) / BS_PX, (1280.0f - (float)y) / BS_ROW_Y);
        ui->aim = -1;
        ui->hint_t = 0;
        return;
    }
    int p = pad_at(g, x, y);
    if (p != ui->sel) bs_game_sound(g, SND_TAP);
    ui->sel = p;
    ui->pick = -1;
    if (p >= 0) ui->cursor = p;
}

/* the nearest of a set of points in a direction: the one that costs
 * least going mostly that way */
static int toward(const float *px, const float *py, int n, float x, float y, int dx, int dy, int skip)
{
    int best = -1;
    float bs = 1e9f;
    for (int i = 0; i < n; i++) {
        if (i == skip) continue;
        float ax = px[i] - x, ay = py[i] - y;
        float along = ax * (float)dx + ay * (float)dy;
        float across = fabsf(ax * (float)dy - ay * (float)dx);
        if (along < 8.0f || across > along * 1.6f) continue;
        float sc = along + across * 1.8f;
        if (sc < bs) {
            bs = sc;
            best = i;
        }
    }
    return best;
}

void bs_hud_pad(bs_ui_t *ui, bs_game_t *g, uint32_t pressed)
{
    if (!pressed) return;
    bs_hud_layout(ui, g);
    if (!ui->pad_on) {
        ui->pad_on = true;
        if (ui->cursor < 0 || ui->cursor >= g->w->npad) ui->cursor = 0;
    }
    int dx = (pressed & AOS_PAD_RIGHT) ? 1 : (pressed & AOS_PAD_LEFT) ? -1 : 0;
    int dy = (pressed & AOS_PAD_DOWN) ? 1 : (pressed & AOS_PAD_UP) ? -1 : 0;
    float px[BS_MAX_BTN], py[BS_MAX_BTN];
    if (ui->focus < 0) {
        if (dx || dy) {
            for (int i = 0; i < g->w->npad; i++) {
                px[i] = (float)BS_SX(g->w->pad_x[i]);
                py[i] = (float)BS_SY(g->w->pad_y[i]);
            }
            int c = ui->cursor;
            int n = toward(px, py, g->w->npad, px[c], py[c], dx, dx ? 0 : dy, c);
            if (n >= 0) ui->cursor = n;
            else if (dy > 0 && ui->nbtn > 2) ui->focus = 2;    /* off the bottom: the panel */
            if (ui->sel >= 0) {
                ui->sel = ui->cursor;
                ui->pick = -1;
            }
        }
        if (pressed & AOS_PAD_A) {
            if (ui->aim >= 0) {
                auto_strike(g);
                ui->aim = -1;
                ui->hint_t = 0;
            } else {
                ui->sel = ui->cursor;
                ui->pick = -1;
                bs_hud_layout(ui, g);
                ui->focus = ui->nbtn > 2 ? 2 : -1;
                bs_game_sound(g, SND_TAP);
            }
        }
        if (pressed & AOS_PAD_B) {
            ui->sel = ui->pick = -1;
            ui->aim = -1;
        }
        if (pressed & AOS_PAD_L) {
            ui->sel = ui->pick = -1;
            bs_hud_layout(ui, g);
            ui->focus = ui->nbtn > 2 ? 2 : -1;
        }
    } else {
        if (dx || dy) {
            for (int i = 0; i < ui->nbtn; i++) {
                px[i] = (float)(ui->btn[i].x + ui->btn[i].w / 2);
                py[i] = (float)(ui->btn[i].y + ui->btn[i].h / 2);
            }
            int f = ui->focus;
            int n = toward(px, py, ui->nbtn, px[f], py[f], dx, dx ? 0 : dy, f);
            /* the bar on top is reached with START and R, not by walking */
            if (n >= 2) {
                ui->focus = n;
                if (ui->btn[n].act == ACT_PICK) ui->pick = ui->btn[n].arg;
            } else if (dy < 0) {
                ui->focus = -1;
            }
        }
        if (pressed & AOS_PAD_A) {
            bs_btn_t b = ui->btn[ui->focus];
            act(ui, g, &b);
        }
        if (pressed & AOS_PAD_B) {
            ui->sel = ui->pick = -1;
            ui->aim = -1;
            ui->focus = -1;
        }
    }
    if (pressed & AOS_PAD_R) {
        bs_btn_t b = { 0 };
        b.enabled = 1;
        b.act = g->state == BS_BUILD ? ACT_WAVE : ACT_SPEED;
        act(ui, g, &b);
    }
    bs_hud_layout(ui, g);
}

/* ---- drawing ------------------------------------------------------------------------------- */

static void icon(bs_gfx_t *gf, const bs_art_t *art, int ic, int cx, int cy, int alpha)
{
    bs_spr(gf, &art->ui, ic, cx, cy, 0, alpha, 0, 0);
}

static void frame(bs_gfx_t *gf, int x, int y, int w, int h, uint16_t c, int t)
{
    bs_rect(gf, x, y, w, t, c, 255, 0);
    bs_rect(gf, x, y + h - t, w, t, c, 255, 0);
    bs_rect(gf, x, y + t, t, h - 2 * t, c, 255, 0);
    bs_rect(gf, x + w - t, y + t, t, h - 2 * t, c, 255, 0);
}

static void button(bs_gfx_t *gf, const bs_btn_t *b, uint16_t color, int alpha)
{
    bs_rect(gf, b->x, b->y, b->w, b->h, b->enabled ? color : C_BTN_OFF, alpha, 14);
}

static const bs_btn_t *find(const bs_ui_t *ui, int act_id, int arg)
{
    for (int i = 0; i < ui->nbtn; i++) {
        if (ui->btn[i].act == act_id && ui->btn[i].arg == arg) return &ui->btn[i];
    }
    return NULL;
}

static void top_bar(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g)
{
    char s[40];
    const bs_font_t *fl = &art->font[FONT_L], *fm = &art->font[FONT_M];
    bs_rect(gf, 0, 0, BS_W, BS_TOP_H, 0x0000, 165, 0);
    bs_rect(gf, 0, BS_TOP_H, BS_W, 2, bs_rgb(g->w->accent), 200, 0);
    icon(gf, art, IC_HEART, 40, 44, 255);
    snprintf(s, sizeof s, "%d", g->lives);
    bs_text(gf, fl, 74, 22, s, g->lives * 4 <= g->max_lives ? C_RED : C_WHITE, 0);
    icon(gf, art, IC_COIN, 190, 44, 255);
    snprintf(s, sizeof s, "%d", g->credits);
    bs_text(gf, fl, 224, 22, s, C_GOLD, 0);
    icon(gf, art, IC_WAVE, 392, 44, 255);
    int shown = g->wave + (g->state == BS_BUILD ? 0 : 1);
    if (shown > g->nwaves) shown = g->nwaves;
    /* without end, the wave against the next goal */
    if (g->mode == BS_MODE_ENDLESS && g->mile_step > 0)
        snprintf(s, sizeof s, "%d/%d", shown, (g->wave / g->mile_step + 1) * g->mile_step);
    else if (g->mode == BS_MODE_ENDLESS) snprintf(s, sizeof s, "%d", shown);
    else snprintf(s, sizeof s, "%d/%d", shown, g->nwaves);
    bs_text(gf, fm, 424, 30, s, C_WHITE, 0);
    const bs_btn_t *b = find(ui, ACT_SPEED, 0);
    if (b) {
        button(gf, b, ui->speed > 1 ? bs_rgb(g->w->accent) : C_BTN, 230);
        icon(gf, art, IC_SPEED1 + ui->speed - 1, b->x + b->w / 2, b->y + b->h / 2, 255);
    }
    b = find(ui, ACT_PAUSE, 0);
    if (b) {
        button(gf, b, C_BTN, 230);
        icon(gf, art, IC_PAUSE, b->x + b->w / 2, b->y + b->h / 2, 255);
    }
}

static void idle_panel(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g)
{
    char s[160];
    const bs_font_t *fs = &art->font[FONT_S], *fm = &art->font[FONT_M], *fl = &art->font[FONT_L];
    for (int i = 0; i < PW_N; i++) {
        const bs_btn_t *b = find(ui, ACT_POWER, i);
        if (!b) continue;
        bool owned = g->kit.power[i] != 0;
        uint16_t c = bs_rgb(BS_POWER[i].color);
        button(gf, b, ui->aim == i ? c : C_BTN, 240);
        int cx = b->x + b->w / 2;
        if (!owned) {
            icon(gf, art, IC_LOCK, cx, b->y + 70, 150);
        } else {
            icon(gf, art, IC_STRIKE + i, cx, b->y + 62, b->enabled ? 255 : 90);
            if (g->power_cd[i] > 0) {
                snprintf(s, sizeof s, "%d", (int)(g->power_cd[i] + 0.99f));
                bs_text(gf, fm, cx, b->y + 112, s, C_DIM, BS_F_CENTER);
                /* the wait, as a bar that empties */
                int w = (int)((float)(b->w - 20) * g->power_cd[i] / BS_POWER[i].cooldown);
                if (w > b->w - 20) w = b->w - 20;
                bs_rect(gf, b->x + 10, b->y + b->h - 14, w, 5, c, 255, 2);
            } else {
                bs_rect(gf, b->x + 10, b->y + b->h - 14, b->w - 20, 5, c, 255, 2);
            }
        }
    }
    const bs_btn_t *b = find(ui, ACT_WAVE, 0);
    if (b) {
        uint16_t acc = bs_rgb(g->w->accent);
        int pulse = (int)(ui->t * 5.0f) & 1;
        bs_rect(gf, b->x, b->y, b->w, b->h, acc, 255, 18);
        bs_rect(gf, b->x + 4, b->y + 4, b->w - 8, b->h - 8, 0x0841, pulse ? 205 : 225, 15);
        bool last = g->wave == g->nwaves - 1;
        icon(gf, art, IC_PLAY, b->x + 44, b->y + 44, 255);
        snprintf(s, sizeof s, "%s %d", last ? ui->str[STR_FINAL] : ui->str[STR_WAVE], g->wave + 1);
        bs_text(gf, fl, b->x + 84, b->y + 20, last ? ui->str[STR_FINAL] : s, C_WHITE, 0);
        /* who comes */
        int counts[AL_N];
        bs_game_preview(g, counts);
        int x = b->x + 22, shown = 0;
        for (int a = 0; a < AL_N && shown < 4; a++) {
            if (!counts[a]) continue;
            bs_spr(gf, &art->al_icon, a, x + 22, b->y + 118, 0, 255, 0, 0);
            snprintf(s, sizeof s, "%d", counts[a]);
            bs_text(gf, fs, x + 46, b->y + 106, s, BS_ALIEN[a].boss ? C_RED : C_WHITE, 0);
            x += 82;
            shown++;
        }
    } else if (g->state == BS_WAVE) {
        if (g->mode == BS_MODE_ENDLESS) snprintf(s, sizeof s, "%s %d", ui->str[STR_WAVE], g->wave + 1);
        else snprintf(s, sizeof s, "%s %d/%d", ui->str[STR_WAVE], g->wave + 1, g->nwaves);
        bs_text(gf, fl, 532, 1126, s, C_WHITE, BS_F_CENTER);
        int left = g->alive;
        for (int i = 0; i < g->nsp; i++) left += g->sp[i].left;
        snprintf(s, sizeof s, "%s: %d", ui->str[STR_LEFT], left);
        bs_text(gf, fm, 532, 1190, s, C_DIM, BS_F_CENTER);
    }
}

static void build_panel(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g)
{
    char s[40];
    const bs_font_t *fs = &art->font[FONT_S];
    for (int i = 0; i < ui->nbtn; i++) {
        const bs_btn_t *b = &ui->btn[i];
        if (b->act != ACT_PICK) continue;
        int t = b->arg;
        bool sel = ui->pick == t;
        bs_rect(gf, b->x, b->y, b->w, b->h, sel ? bs_rgb(BS_TOWER[t].accent) : C_BTN, sel ? 255 : 235, 12);
        if (sel) bs_rect(gf, b->x + 3, b->y + 3, b->w - 6, b->h - 6, 0x18E5, 255, 10);
        bs_spr(gf, &art->tw_icon, t, b->x + b->w / 2, b->y + 34, 0, b->enabled ? 255 : 100, 0, 0);
        snprintf(s, sizeof s, "%d", bs_game_cost(g, t, 0));
        bs_text(gf, fs, b->x + b->w / 2, b->y + 62, s, b->enabled ? C_GOLD : C_RED, BS_F_CENTER);
    }
}

static void tower_panel(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g)
{
    char s[320];
    const bs_tower_t *t = &g->tw[ui->sel];
    const bs_tower_def_t *d = &BS_TOWER[t->type];
    const bs_font_t *fs = &art->font[FONT_S], *fm = &art->font[FONT_M];
    uint16_t acc = bs_rgb(d->accent);
    bs_spr(gf, &art->tw_icon, t->type, 50, 1140, 0, 255, 0, 0);
    int w = bs_text(gf, fm, 100, 1102, ui->tw_name[t->type], acc, 0);
    for (int k = 0; k < BS_TIERS; k++) icon(gf, art, k <= t->tier ? IC_STAR : IC_STAR_OFF, 100 + w + 30 + k * 34, 1118, 255);
    /* what it does now, and what the next tier would */
    float dmg = bs_game_dmg(g, t->type, t->tier) * t->boost, rng = bs_game_range(g, t->type, t->tier);
    if (t->type == TW_BANK) {
        snprintf(s, sizeof s, "+%d %s", (int)(bs_tier_dmg(TW_BANK, t->tier) + 0.5f), ui->str[STR_PER_WAVE]);
    } else if (t->type == TW_RADAR) {
        snprintf(s, sizeof s, "%s +%d%%   %s %d.%d", ui->str[STR_DMG], 20 + 10 * t->tier, ui->str[STR_RANGE], (int)rng,
                 (int)(rng * 10.0f) % 10);
    } else {
        snprintf(s, sizeof s, "%s %d   %s %d.%d   %s %d", ui->str[STR_DMG], (int)(dmg + 0.5f), ui->str[STR_RANGE], (int)rng,
                 (int)(rng * 10.0f) % 10, ui->str[STR_KILLS], t->kills);
    }
    bs_text(gf, fs, 100, 1140, s, C_WHITE, 0);
    if (t->off_t > 0) bs_text(gf, fs, 100, 1162, ui->str[STR_JAMMED], C_RED, 0);
    else if (t->boost > 1.0f) bs_text(gf, fs, 100, 1162, ui->str[STR_BOOST], C_GREEN, 0);

    const bs_btn_t *b = find(ui, ACT_UPGRADE, 0);
    if (b) {
        bool max = t->tier >= BS_TIERS - 1;
        button(gf, b, max ? C_BTN_OFF : 0x2E45, 255);
        if (max) {
            bs_text(gf, fm, b->x + b->w / 2, b->y + 26, ui->str[STR_MAX], C_DIM, BS_F_CENTER);
        } else {
            icon(gf, art, IC_UPGRADE, b->x + 34, b->y + b->h / 2, b->enabled ? 255 : 110);
            bs_text(gf, fs, b->x + 64, b->y + 12, ui->str[STR_UPGRADE], b->enabled ? C_WHITE : C_DIM, 0);
            snprintf(s, sizeof s, "%d", bs_game_cost(g, t->type, t->tier + 1));
            bs_text(gf, fm, b->x + 64, b->y + 38, s, b->enabled ? C_GOLD : C_RED, 0);
        }
    }
    b = find(ui, ACT_MODE, 0);
    if (b) {
        button(gf, b, C_BTN, 255);
        if (b->enabled) {
            icon(gf, art, IC_TARGET, b->x + 32, b->y + b->h / 2, 255);
            bs_text(gf, fs, b->x + 60, b->y + 28, ui->str[STR_FIRST + t->mode], C_WHITE, 0);
        } else {
            snprintf(s, sizeof s, "%s%s%s", d->ground ? ui->str[STR_GROUND] : "", d->ground && d->air ? " + " : "",
                     d->air ? ui->str[STR_AIR] : "");
            bs_text(gf, fs, b->x + b->w / 2, b->y + 28, s, C_DIM, BS_F_CENTER);
        }
    }
    b = find(ui, ACT_SELL, 0);
    if (b) {
        button(gf, b, 0x6904, 255);
        icon(gf, art, IC_SELL, b->x + 34, b->y + b->h / 2, 255);
        bs_text(gf, fs, b->x + 64, b->y + 12, ui->str[STR_SELL], C_WHITE, 0);
        snprintf(s, sizeof s, "+%d", bs_game_sell_value(g, ui->sel));
        bs_text(gf, fm, b->x + 64, b->y + 38, s, C_GOLD, 0);
    }
    b = find(ui, ACT_CLOSE, 0);
    if (b) {
        button(gf, b, C_BTN, 255);
        icon(gf, art, IC_CLOSE, b->x + b->w / 2, b->y + b->h / 2, 255);
    }
}

static bool fliers_next(const bs_game_t *g)
{
    for (int i = 0; i < g->nnext; i++) {
        if (BS_ALIEN[g->next[i].alien].fly) return true;
    }
    return false;
}

void bs_hud_draw(bs_gfx_t *gf, const bs_art_t *art, const bs_ui_t *ui, const bs_game_t *g)
{
    const bs_font_t *fs = &art->font[FONT_S], *fm = &art->font[FONT_M], *fl = &art->font[FONT_L];
    bool playing = g->state == BS_BUILD || g->state == BS_WAVE;
    top_bar(gf, art, ui, g);
    bs_rect(gf, 0, BS_PANEL_Y, BS_W, BS_H - BS_PANEL_Y, C_PANEL, 232, 0);
    bs_rect(gf, 0, BS_PANEL_Y, BS_W, 2, bs_rgb(g->w->accent), 255, 0);
    if (playing) {
        if (ui->sel < 0) idle_panel(gf, art, ui, g);
        else if (!g->tw[ui->sel].on) build_panel(gf, art, ui, g);
        else tower_panel(gf, art, ui, g);
    }
    /* a line over the panel: what the chosen tower is, or a hint */
    const char *line = NULL, *line2 = NULL;
    uint16_t lc = C_WHITE;
    if (playing && ui->sel >= 0 && !g->tw[ui->sel].on && ui->pick >= 0) {
        line = ui->tw_name[ui->pick];
        line2 = ui->tw_desc[ui->pick];
        lc = bs_rgb(BS_TOWER[ui->pick].accent);
    } else if (ui->hint_t > 0) {
        line2 = ui->hint;
    } else if (playing && g->state == BS_BUILD && g->built == 0 && ui->sel < 0) {
        line2 = ui->str[STR_HINT_PAD];
    } else if (playing && g->state == BS_BUILD && ui->sel < 0 && fliers_next(g)) {
        /* the ones in the air go past half the towers: say so before */
        line2 = ui->str[STR_HINT_AIR];
        lc = C_GOLD;
    } else if (playing && g->state == BS_BUILD && g->wave == 1 && g->sector == 0 && g->w->order == 1 && ui->sel < 0) {
        line2 = ui->str[STR_HINT_WAVE];
    }
    if (line || line2) {
        bs_rect(gf, 0, BS_PANEL_Y - 62, BS_W, 62, 0x0000, 190, 0);
        if (line) {
            bs_text(gf, fm, 14, BS_PANEL_Y - 60, line, lc, 0);
            bs_text(gf, fs, 14, BS_PANEL_Y - 28, line2, C_WHITE, 0);
            int w = bs_text_width(fs, ui->str[STR_HINT_BUILD]);
            if (bs_text_width(fm, line) + w + 40 < BS_W)
                bs_text(gf, fs, BS_W - 14, BS_PANEL_Y - 56, ui->str[STR_HINT_BUILD], C_DIM, BS_F_RIGHT);
        } else {
            bs_text(gf, fs, BS_W / 2, BS_PANEL_Y - 42, line2, lc, BS_F_CENTER);
        }
    }
    /* the wave's name across the field as it starts */
    if (ui->banner_t > 0 && g->state == BS_WAVE) {
        char s[120];
        bool boss = false;
        for (int i = 0; i < g->nsp; i++) boss |= BS_ALIEN[g->sp[i].alien].boss != 0;
        int a = ui->banner_t > 0.4f ? 255 : (int)(ui->banner_t * 637.0f);
        bs_rect(gf, 0, 400, BS_W, 96, 0x0000, a * 150 / 255, 0);
        if (boss) {
            bs_text(gf, fl, BS_W / 2, 424, ui->str[STR_BOSS], C_RED, BS_F_CENTER | BS_F_SHADOW);
        } else if (g->wave == g->nwaves - 1) {
            bs_text(gf, fl, BS_W / 2, 424, ui->str[STR_LAST], C_GOLD, BS_F_CENTER | BS_F_SHADOW);
        } else {
            snprintf(s, sizeof s, "%s %d", ui->str[STR_WAVE], g->wave + 1);
            bs_text(gf, fl, BS_W / 2, 424, s, C_WHITE, BS_F_CENTER | BS_F_SHADOW);
        }
    }
    /* a goal of the mode without end, reached */
    if (ui->mile_t > 0 && g->mode == BS_MODE_ENDLESS) {
        char s[200];
        int a = ui->mile_t > 0.5f ? 255 : (int)(ui->mile_t * 510.0f);
        bs_rect(gf, 0, 540, BS_W, 130, 0x0000, a * 170 / 255, 0);
        snprintf(s, sizeof s, ui->str[STR_MILE], g->miles * g->mile_step);
        bs_text(gf, fl, BS_W / 2, 552, s, C_GOLD, BS_F_CENTER | BS_F_SHADOW);
        snprintf(s, sizeof s, ui->str[STR_MILE_PAY], g->mile_cores, g->mile_credits, g->mile_lives);
        bs_text(gf, fm, BS_W / 2, 612, s, C_WHITE, BS_F_CENTER | BS_F_SHADOW);
    }
    if (ui->pad_on && ui->focus >= 0 && ui->focus < ui->nbtn) {
        const bs_btn_t *b = &ui->btn[ui->focus];
        frame(gf, b->x - 4, b->y - 4, b->w + 8, b->h + 8, C_WHITE, 3);
    }
}
