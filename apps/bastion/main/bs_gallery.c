/*
 * BASTION - the gallery: a card for every alien, tower and world, unlocked
 * as the player meets them. An alien's card opens the first time it comes
 * on screen in any battle (the save keeps the bits); a tower's when the
 * laboratory unlocks it; a world's when the campaign reaches it.
 *
 * A card has the thing's portrait, rendered bigger than in the game by
 * tools/blender/gallery.py (an alien's is the four frames of its walk,
 * played as a loop), its numbers from bs_data.c and a line about it. What
 * is still locked shows as a dark shape with no name.
 */
#include "bs_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

enum { GT_ALIENS, GT_TOWERS, GT_WORLDS, GT_N };

#define N_CARDS 15          /* the aliens with a card: every one but the divider's children */

static const char *const ABOUT[N_CARDS] = {
    [AL_GRUB]     = N_("El más común. Lento y blando: llegan de a muchos."),
    [AL_RUNNER]   = N_("Rápido y frágil. Pasa de largo si nada dispara seguido."),
    [AL_SWARM]    = N_("Diminuto y por docenas. Las explosiones y los rayos en cadena lo barren."),
    [AL_SHELL]    = N_("Su coraza le resta 7 a cada golpe. Hacen falta golpes fuertes, o fuego."),
    [AL_WASP]     = N_("Vuela y corta las curvas del camino. Cañón, Lanzallamas, Riel y Mortero no le llegan."),
    [AL_SPLIT]    = N_("Al morir se parte en tres más chicos y más rápidos."),
    [AL_SHIELD]   = N_("Lleva un escudo que se recarga si lo dejan de golpear. El pulso EMP se lo apaga."),
    [AL_HEALER]   = N_("Cura a los que tiene alrededor. Conviene matarlo primero."),
    [AL_BRUTE]    = N_("Mucha vida, algo de coraza, y le saca tres vidas al fuerte."),
    [AL_PHASE]    = N_("Se vuelve invisible de a ratos: sólo un Amplificador o un pulso EMP lo delatan."),
    [AL_JAMMER]   = N_("Cada tanto apaga por dos segundos las torres que tiene cerca."),
    [AL_CARRIER]  = N_("Vuela, aguanta mucho y va soltando ácaros por el camino."),
    [AL_QUEEN]    = N_("La madre de la colmena: va poniendo larvas mientras avanza."),
    [AL_TITAN]    = N_("Coraza pesada, y sus pisotones apagan las torres cercanas."),
    [AL_OVERMIND] = N_("Un gran escudo, cura a los suyos y cada tanto salta hacia adelante."),
};

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static bool alien_open(const bs_app_t *a, int i)
{
    return (a->save.seen >> i) & 1;
}

static bool world_open(const bs_app_t *a, int i)
{
    return bs_save_open(&a->save, i, 0);
}

static const char *wname(const bs_world_t *w)
{
    int l = bs_lang();
    return w->name[l][0] ? w->name[l] : w->name[0];
}

/* ---- a card, open ---------------------------------------------------------------------------- */

static void card_free(bs_app_t *a)
{
    for (int i = 0; i < 4; i++) {
        bs_free(a->gal_frame[i]);
        a->gal_frame[i] = NULL;
    }
    a->gal_n = 0;
    a->gal_img = NULL;
}

static void card_close(bs_app_t *a)
{
    if (!a->gal_card) return;
    lv_obj_delete(a->gal_card);
    a->gal_card = NULL;
    card_free(a);
}

static void ev_close(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    bs_audio_play(SND_TAP, 230);
    /* the screen is made again, without the card: the grid's buttons are
     * the gamepad's again */
    bs_gallery_build(a);
}

static lv_obj_t *card_open(bs_app_t *a, uint32_t acc)
{
    card_close(a);
    lv_obj_t *dim = bs_menu_plain(a->screen);
    lv_obj_set_size(dim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim, C(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *p = lv_obj_create(dim);
    lv_obj_set_size(p, 660, LV_SIZE_CONTENT);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, 30, 0);
    lv_obj_set_style_bg_color(p, C(0x141A28), 0);
    lv_obj_set_style_bg_grad_color(p, C(0x05070C), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 3, 0);
    lv_obj_set_style_border_color(p, C(acc), 0);
    lv_obj_set_style_shadow_width(p, 50, 0);
    lv_obj_set_style_shadow_color(p, C(acc), 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    a->gal_card = dim;
    return p;
}

static lv_obj_t *line(lv_obj_t *p, const char *text, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = bs_label(p, text, f, col);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* a number with its name: two to a row, or one when w says so */
static void stat_w(lv_obj_t *grid, const char *name, const char *value, uint32_t col, int w)
{
    lv_obj_t *c = bs_menu_plain(grid);
    lv_obj_set_size(c, w, 62);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_bg_color(c, C(0x1C2436), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_t *n = bs_label(c, name, aos_font_small, 0x9AA4B8);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t *v = bs_label(c, value, aos_font_body, col);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -14, 0);
}

static void stat(lv_obj_t *grid, const char *name, const char *value, uint32_t col)
{
    stat_w(grid, name, value, col, 296);
}

static lv_obj_t *stats(lv_obj_t *p)
{
    lv_obj_t *g = bs_menu_plain(p);
    lv_obj_set_size(g, 612, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, 20, 0);
    lv_obj_set_style_pad_row(g, 10, 0);
    return g;
}

static void card_end(bs_app_t *a, lv_obj_t *p, uint32_t acc)
{
    lv_obj_t *b = bs_button(p, _("Cerrar"), acc, true, 360, 84, ev_close, a);
    aos_pad_menu_set(&a->menu, &b, 1, 0);
    aos_pad_reset(&a->pad, lv_tick_get());
}

static void alien_card(bs_app_t *a, int i)
{
    const bs_alien_def_t *d = &BS_ALIEN[i];
    lv_obj_t *p = card_open(a, d->color);
    char s[96], name[BS_NAME_LEN];
    line(p, _(d->name), aos_font_title, d->color);
    line(p, d->boss ? _("Jefe") : d->fly ? _("Vuela") : _("Camina"), aos_font_small, 0xB0B8C8);
    /* its walk, as a loop */
    for (int f = 0; f < 4; f++) {
        snprintf(name, sizeof name, "ga_%s%d", d->id, f);
        a->gal_frame[f] = bs_pak_image(&a->art.pak, name, &a->gal_dsc[f]);
        if (!a->gal_frame[f]) break;
        a->gal_n = f + 1;
    }
    lv_obj_t *box = bs_menu_plain(p);
    lv_obj_set_size(box, 300, 250);
    if (a->gal_n) {
        a->gal_img = lv_image_create(box);
        lv_image_set_src(a->gal_img, &a->gal_dsc[0]);
        lv_obj_center(a->gal_img);
    } else if (a->gal_thumb[i]) {
        lv_obj_t *im = lv_image_create(box);
        lv_image_set_src(im, &a->gal_thumb_dsc[i]);
        lv_image_set_scale(im, 512);
        lv_obj_center(im);
    }
    a->gal_f = 0;
    a->gal_t = lv_tick_get();
    lv_obj_t *g = stats(p);
    snprintf(s, sizeof s, "%d", (int)d->hp);
    stat(g, _("Vida"), s, 0x60FF90);
    snprintf(s, sizeof s, "%d.%d m/s", (int)d->speed, (int)(d->speed * 10.0f + 0.5f) % 10);
    stat(g, _("Velocidad"), s, 0xFFFFFF);
    snprintf(s, sizeof s, "%d", d->armor);
    stat(g, _("Blindaje"), s, d->armor ? 0x70A0FF : 0x707888);
    snprintf(s, sizeof s, "%d", (int)d->shield);
    stat(g, _("Escudo"), s, d->shield > 0 ? 0x40E8FF : 0x707888);
    /* a boss's: what it pays on the way down and when it falls, in all */
    snprintf(s, sizeof s, "%d", d->boss ? BS_BOSS_PARTS * BS_BOSS_PART + d->reward : d->reward);
    stat(g, _("Recompensa"), s, 0xFFD23C);
    snprintf(s, sizeof s, "%d", d->lives);
    stat(g, _("Daño al fuerte"), s, 0xFF5040);
    line(p, _(ABOUT[i]), aos_font_small, 0xE0E4EC);
    line(p, _("La vida es la de base: cada sector la multiplica."), aos_font_small, 0x707888);
    card_end(a, p, d->color);
}

static void tower_card(bs_app_t *a, int t)
{
    const bs_tower_def_t *d = &BS_TOWER[t];
    lv_obj_t *p = card_open(a, d->accent);
    char s[96], name[BS_NAME_LEN];
    line(p, _(d->name), aos_font_title, d->accent);
    snprintf(name, sizeof name, "gt_%s", d->id);
    a->gal_frame[0] = bs_pak_image(&a->art.pak, name, &a->gal_dsc[0]);
    lv_obj_t *box = bs_menu_plain(p);
    lv_obj_set_size(box, 300, 310);
    if (a->gal_frame[0]) {
        lv_obj_t *im = lv_image_create(box);
        lv_image_set_src(im, &a->gal_dsc[0]);
        lv_obj_center(im);
    } else if (a->img[IMG_TOWER0 + t]) {
        lv_obj_t *im = lv_image_create(box);
        lv_image_set_src(im, &a->img_dsc[IMG_TOWER0 + t]);
        lv_image_set_scale(im, 512);
        lv_obj_center(im);
    }
    line(p, _(d->desc), aos_font_small, 0xE0E4EC);
    /* the three tiers, side by side */
    lv_obj_t *g = stats(p);
    bool per_s = d->rate <= 0 && t != TW_RADAR && t != TW_BANK;
    if (t == TW_BANK) {
        snprintf(s, sizeof s, "%d · %d · %d", (int)bs_tier_dmg(t, 0), (int)bs_tier_dmg(t, 1), (int)bs_tier_dmg(t, 2));
        stat_w(g, _("Créditos por oleada"), s, 0xFFD23C, 612);
    } else if (t == TW_RADAR) {
        stat_w(g, _("Daño de las vecinas"), "+20 · +30 · +40 %", 0xFF7060, 612);
    } else {
        snprintf(s, sizeof s, "%d · %d · %d%s", (int)bs_tier_dmg(t, 0), (int)bs_tier_dmg(t, 1), (int)bs_tier_dmg(t, 2),
                 per_s ? "/s" : "");
        stat_w(g, _("Daño"), s, 0xFF7060, 612);
    }
    if (d->range > 0) {
        float r0 = bs_tier_range(t, 0), r2 = bs_tier_range(t, 2);
        snprintf(s, sizeof s, "%d.%d - %d.%d m", (int)r0, (int)(r0 * 10.0f + 0.5f) % 10, (int)r2, (int)(r2 * 10.0f + 0.5f) % 10);
        stat_w(g, _("Alcance"), s, 0xFFFFFF, 612);
    }
    if (d->rate > 0) {
        snprintf(s, sizeof s, "%d.%02d s", (int)d->rate, (int)(d->rate * 100.0f + 0.5f) % 100);
        stat_w(g, _("Cadencia"), s, 0xFFFFFF, 612);
    }
    snprintf(s, sizeof s, "%d · %d · %d", bs_tier_cost(t, 0), bs_tier_cost(t, 1), bs_tier_cost(t, 2));
    stat_w(g, _("Costo"), s, 0xFFD23C, 612);
    stat_w(g, _("Ataca"), d->air && d->ground ? _("tierra y aire") : d->ground ? _("sólo tierra") : _("no ataca"),
         d->air ? 0x60FF90 : 0xB0B8C8, 612);
    snprintf(s, sizeof s, "%d / %d", a->save.mastery[t], BS_MASTERY_MAX);
    stat_w(g, _("Maestría"), s, d->accent, 612);
    line(p, _("Los tres números son los tres niveles de la torre."), aos_font_small, 0x707888);
    card_end(a, p, d->accent);
}

static void world_card(bs_app_t *a, int i)
{
    bs_card_t *c = &a->card[i];
    const bs_world_t *h = &c->head;
    lv_obj_t *p = card_open(a, h->accent);
    char s[400];
    int l = bs_lang();
    snprintf(s, sizeof s, "%d · %s", i + 1, wname(h));
    line(p, s, aos_font_title, h->accent);
    lv_obj_t *row = bs_menu_plain(p);
    lv_obj_set_size(row, 612, 540);
    if (c->thumb) {
        lv_obj_t *im = lv_image_create(row);
        lv_image_set_src(im, &c->thumb_dsc);
        lv_obj_set_pos(im, 0, 0);
    }
    lv_obj_t *col = bs_menu_plain(row);
    lv_obj_set_size(col, 296, 534);
    lv_obj_set_pos(col, 316, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_t *d = line(col, h->desc[l][0] ? h->desc[l] : h->desc[0], aos_font_small, 0xE0E4EC);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_LEFT, 0);
    int stars = 0;
    for (int k = 0; k < BS_SECTORS; k++) stars += a->save.stars[i][k];
    snprintf(s, sizeof s, "%d / 9", stars);
    stat(col, _("Estrellas"), s, 0xFFD23C);
    snprintf(s, sizeof s, "%d", (int)a->save.best[i]);
    stat(col, _("Récord"), s, a->save.best[i] ? 0xFFD23C : 0x707888);
    stat(col, _("Jefe"), alien_open(a, h->boss) ? _(BS_ALIEN[h->boss].name) : "???", BS_ALIEN[h->boss].color);
    /* who lives here: the ones already met by name */
    uint32_t mask = 0;
    bs_pak_t pak;
    if (bs_pak_open(&pak, c->path)) {
        char *txt = (char *)bs_pak_get(&pak, "def", NULL);
        bs_world_t *w = (bs_world_t *)bs_calloc(sizeof(bs_world_t));
        if (txt && w && bs_world_parse(w, txt, false)) {
            for (int k = 0; k < BS_SECTORS; k++) mask |= bs_world_aliens(w, k);
        }
        if (w) bs_world_free(w);
        bs_free(w);
        bs_free(txt);
        bs_pak_close(&pak);
    }
    size_t n = 0;
    s[0] = 0;
    int hidden = 0;
    for (int k = 0; k < N_CARDS; k++) {
        if (!((mask >> k) & 1) || BS_ALIEN[k].boss) continue;
        if (!alien_open(a, k)) {
            hidden++;
            continue;
        }
        n += (size_t)snprintf(s + n, sizeof s - n, "%s%.24s", n ? ", " : "", _(BS_ALIEN[k].name));
        if (n >= sizeof s - 40) break;
    }
    if (hidden && n < sizeof s - 40) snprintf(s + n, sizeof s - n, n ? ", ??? x%d" : "??? x%d", hidden);
    lv_obj_t *al = line(col, s, aos_font_small, 0xB0B8C8);
    lv_obj_set_style_text_align(al, LV_TEXT_ALIGN_LEFT, 0);
    card_end(a, p, h->accent);
}

/* ---- the grid --------------------------------------------------------------------------------- */

static void ev_tab(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    a->gal_tab = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    bs_audio_play(SND_TAP, 230);
    bs_gallery_build(a);
}

static void ev_cell(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    bool open = a->gal_tab == GT_ALIENS ? alien_open(a, i) : a->gal_tab == GT_TOWERS ? a->save.owned[i] != 0 : world_open(a, i);
    if (!open) {
        bs_audio_play(SND_DENY, 230);
        aos_ui_toast(a->gal_tab == GT_ALIENS ? _("Todavía no lo viste en batalla")
                     : a->gal_tab == GT_TOWERS ? _("Se desbloquea en el laboratorio")
                     : _("Llegá a este mundo en la campaña"), 2200);
        return;
    }
    bs_audio_play(SND_TAP, 230);
    if (a->gal_tab == GT_ALIENS) alien_card(a, i);
    else if (a->gal_tab == GT_TOWERS) tower_card(a, i);
    else world_card(a, i);
}

static lv_obj_t *cell(bs_app_t *a, lv_obj_t *grid, int i, const char *name, uint32_t acc, bool open, int w, int h)
{
    lv_obj_t *b = lv_button_create(grid);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, C(open ? 0x182033 : 0x0E121C), 0);
    lv_obj_set_style_bg_grad_color(b, C(0x0A0D14), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, C(open ? acc : 0x2A3040), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_user_data(b, (void *)(intptr_t)i);
    lv_obj_add_event_cb(b, ev_cell, LV_EVENT_CLICKED, a);
    lv_obj_t *l = bs_label(b, open ? name : "???", aos_font_small, open ? acc : 0x566074);
    lv_obj_set_width(l, w - 12);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, 30);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
    return b;
}

/* what is locked is a shape with no colour */
static void shade(lv_obj_t *im, bool open)
{
    if (open) return;
    lv_obj_set_style_image_recolor(im, C(0x05070C), 0);
    lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(im, LV_OPA_70, 0);
}

void bs_gallery_build(bs_app_t *a)
{
    static const char *const TABS[GT_N] = { N_("Aliens"), N_("Torres"), N_("Mundos") };
    /* the old screen goes first, and any card on it; then its pictures */
    lv_obj_t *s = bs_menu_screen(a, LV_OPA_70);
    a->gal_card = NULL;
    card_free(a);
    a->state = ST_GALLERY;
    if (a->gal_tab < 0 || a->gal_tab >= GT_N) a->gal_tab = GT_ALIENS;
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n = 0;
    items[n++] = bs_menu_header(a, s, _("Galería"), ST_TITLE);
    /* how many of each are open */
    int have[GT_N] = { 0, 0, 0 }, of[GT_N] = { N_CARDS, TW_N, a->ncard };
    for (int i = 0; i < N_CARDS; i++) have[GT_ALIENS] += alien_open(a, i);
    for (int i = 0; i < TW_N; i++) have[GT_TOWERS] += a->save.owned[i] != 0;
    for (int i = 0; i < a->ncard; i++) have[GT_WORLDS] += world_open(a, i);
    char t[64];
    for (int k = 0; k < GT_N; k++) {
        snprintf(t, sizeof t, "%s %d/%d", _(TABS[k]), have[k], of[k]);
        lv_obj_t *b = bs_button(s, t, BS_ACCENT, k == a->gal_tab, 224, 64, ev_tab, a);
        lv_obj_set_style_text_font(b, aos_font_small, 0);
        lv_obj_set_pos(b, 16 + k * 232, 104);
        lv_obj_set_user_data(b, (void *)(intptr_t)k);
        items[n++] = b;
    }
    lv_obj_t *grid = bs_menu_plain(s);
    lv_obj_set_size(grid, 720, 1280 - 184);
    lv_obj_set_pos(grid, 0, 184);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid, 18, 0);
    lv_obj_set_style_pad_column(grid, 18, 0);
    lv_obj_set_style_pad_row(grid, 18, 0);
    lv_obj_set_style_pad_bottom(grid, 60, 0);
    char name[BS_NAME_LEN];
    if (a->gal_tab == GT_ALIENS) {
        for (int i = 0; i < N_CARDS; i++) {
            if (!a->gal_thumb[i] && a->art_ok) {
                snprintf(name, sizeof name, "ai_%s", BS_ALIEN[i].id);
                a->gal_thumb[i] = bs_pak_image(&a->art.pak, name, &a->gal_thumb_dsc[i]);
            }
            bool open = alien_open(a, i);
            lv_obj_t *b = cell(a, grid, i, _(BS_ALIEN[i].name), BS_ALIEN[i].color, open, 216, 190);
            if (a->gal_thumb[i]) {
                lv_obj_t *im = lv_image_create(b);
                lv_image_set_src(im, &a->gal_thumb_dsc[i]);
                lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 22);
                shade(im, open);
            }
            if (n < AOS_PAD_MENU_MAX) items[n++] = b;
        }
    } else if (a->gal_tab == GT_TOWERS) {
        for (int i = 0; i < TW_N; i++) {
            bool open = a->save.owned[i] != 0;
            lv_obj_t *b = cell(a, grid, i, _(BS_TOWER[i].name), BS_TOWER[i].accent, open, 216, 200);
            if (a->img[IMG_TOWER0 + i]) {
                lv_obj_t *im = lv_image_create(b);
                lv_image_set_src(im, &a->img_dsc[IMG_TOWER0 + i]);
                lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 18);
                shade(im, open);
            }
            if (n < AOS_PAD_MENU_MAX) items[n++] = b;
        }
    } else {
        bs_thumbs_load(a);
        for (int i = 0; i < a->ncard; i++) {
            bs_card_t *c = &a->card[i];
            bool open = world_open(a, i);
            snprintf(t, sizeof t, "%d · %.40s", i + 1, wname(&c->head));
            lv_obj_t *b = cell(a, grid, i, t, c->head.accent, open, 333, 250);
            /* a window on the middle of its picture */
            lv_obj_t *win = bs_menu_plain(b);
            lv_obj_set_size(win, 300, 190);
            lv_obj_align(win, LV_ALIGN_TOP_MID, 0, 12);
            lv_obj_set_style_radius(win, 14, 0);
            lv_obj_set_style_clip_corner(win, true, 0);
            lv_obj_remove_flag(win, LV_OBJ_FLAG_CLICKABLE);
            if (c->thumb) {
                lv_obj_t *im = lv_image_create(win);
                lv_image_set_src(im, &c->thumb_dsc);
                lv_obj_set_pos(im, 0, -170);
                lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
                shade(im, open);
            }
            if (n < AOS_PAD_MENU_MAX) items[n++] = b;
        }
    }
    aos_pad_menu_set(&a->menu, items, n, 1 + a->gal_tab);
    aos_pad_reset(&a->pad, lv_tick_get());
}

void bs_gallery_free(bs_app_t *a)
{
    /* the screen's objects go with the screen; these are the pictures */
    a->gal_card = NULL;
    card_free(a);
    for (int i = 0; i < AL_N; i++) {
        bs_free(a->gal_thumb[i]);
        a->gal_thumb[i] = NULL;
    }
}

bool bs_gallery_back(bs_app_t *a)
{
    if (!a->gal_card) return false;
    bs_gallery_build(a);
    return true;
}

void bs_gallery_pad(bs_app_t *a)
{
    aos_pad_t *p = &a->pad;
    /* the open card's alien walks: a frame every 150 ms */
    if (a->gal_card && a->gal_img && a->gal_n > 1 && lv_tick_get() - a->gal_t >= 150) {
        a->gal_t = lv_tick_get();
        a->gal_f = (a->gal_f + 1) % a->gal_n;
        lv_image_set_src(a->gal_img, &a->gal_dsc[a->gal_f]);
    }
    if (aos_pad_pressed(p, AOS_PAD_B)) {
        if (!bs_gallery_back(a)) bs_menu_show(a, ST_TITLE);
        return;
    }
    if (!a->gal_card) {
        int d = aos_pad_pressed(p, AOS_PAD_L) ? -1 : aos_pad_pressed(p, AOS_PAD_R) ? 1 : 0;
        if (d) {
            a->gal_tab = (a->gal_tab + d + GT_N) % GT_N;
            bs_gallery_build(a);
            return;
        }
    }
    aos_pad_menu_step(&a->menu, p);
}
