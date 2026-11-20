/*
 * BASTION - the menus, in LVGL: the title, the campaign (a carousel of
 * worlds, three sectors each) and the laboratory, where the cores won in
 * battle buy what stays for good: new towers, each tower's mastery, the
 * fort's technology and the three powers.
 *
 * The worlds are whatever packs the card has (bastion_<id>_p4.pak), in
 * the order they say; nothing here knows a world by name.
 */
#include "bs_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THUMB_W 300
#define THUMB_H 534
#define CARD_W  340

enum { TAB_TOWERS, TAB_TECH, TAB_POWERS, TAB_N };

static void lab_build(bs_app_t *a);
static void map_theme(bs_app_t *a);

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static uint32_t mixc(uint32_t a, uint32_t b, int k)
{
    int r = (int)(((a >> 16) & 255) * (256 - k) + ((b >> 16) & 255) * k) >> 8;
    int g = (int)(((a >> 8) & 255) * (256 - k) + ((b >> 8) & 255) * k) >> 8;
    int bl = (int)((a & 255) * (256 - k) + (b & 255) * k) >> 8;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

int bs_lang(void)
{
    const char *c = aos_i18n_current();
    if (!c || !c[0] || !strncmp(c, "es", 2)) return 0;
    if (!strncmp(c, "de", 2)) return 2;
    return 1;
}

static const char *wname(const bs_world_t *w)
{
    int l = bs_lang();
    return w->name[l][0] ? w->name[l] : w->name[0];
}

static const char *wdesc(const bs_world_t *w)
{
    int l = bs_lang();
    return w->desc[l][0] ? w->desc[l] : w->desc[0];
}

/* an image of the pack as LVGL wants it: RGB565 or RGB565 + alpha plane */
uint8_t *bs_pak_image(bs_pak_t *pak, const char *name, lv_image_dsc_t *d)
{
    const bs_entry_t *e = bs_pak_find(pak, name);
    if (!e || (e->type != PK_RGB565 && e->type != PK_SPRITE)) return NULL;
    uint8_t *raw = (uint8_t *)bs_pak_load(pak, e);
    if (!raw) return NULL;
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = e->type == PK_SPRITE ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    d->header.w = e->w;
    d->header.h = e->h;
    d->header.stride = (uint32_t)e->w * 2;
    d->data = raw;
    d->data_size = e->rawlen;
    return raw;
}

lv_obj_t *bs_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, C(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

lv_obj_t *bs_button(lv_obj_t *parent, const char *text, uint32_t acc, bool primary, int w, int h, lv_event_cb_t cb,
                    void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, C(primary ? acc : 0x263046), 0);
    lv_obj_set_style_bg_grad_color(b, C(primary ? mixc(acc, 0x000000, 100) : 0x141A28), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, C(primary ? mixc(acc, 0xFFFFFF, 110) : acc), 0);
    lv_obj_set_style_shadow_width(b, primary ? 22 : 0, 0);
    lv_obj_set_style_shadow_color(b, C(acc), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_40, 0);
    int lum = (int)(((acc >> 16) & 255) * 3 + ((acc >> 8) & 255) * 6 + (acc & 255)) / 10;
    lv_obj_set_style_text_color(b, C(primary ? (lum > 140 ? 0x101010 : 0xFFFFFF) : 0xFFFFFF), 0);
    lv_obj_set_style_text_font(b, aos_font_body, 0);
    lv_obj_set_style_bg_color(b, C(mixc(primary ? acc : 0x263046, 0xFFFFFF, 60)), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_border_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *image(bs_app_t *a, lv_obj_t *parent, int id)
{
    if (!a->img[id]) return NULL;
    lv_obj_t *im = lv_image_create(parent);
    lv_image_set_src(im, &a->img_dsc[id]);
    return im;
}

/* ---- the worlds on the card ----------------------------------------------------------------- */

static bool scan_cb(const aos_dir_entry_t *e, void *ctx)
{
    bs_app_t *a = (bs_app_t *)ctx;
    size_t n = strlen(e->name);
    if (e->dir || e->name[0] == '.' || a->ncard >= BS_WORLDS_MAX) return true;
    /* bastion_<id>_p4.pak, and not the common one nor a part of it */
    if (strncmp(e->name, "bastion_", 8) || n < 16 || strcmp(e->name + n - 7, "_p4.pak")) return true;
    if (!strcmp(e->name, "bastion_p4.pak")) return true;
    bs_card_t *c = &a->card[a->ncard];
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%.100s/%.50s", aos_hal_path_apps(), e->name);
    a->ncard++;
    return true;
}

static int card_cmp(const void *x, const void *y)
{
    const bs_card_t *a = (const bs_card_t *)x, *b = (const bs_card_t *)y;
    if (a->head.order != b->head.order) return a->head.order - b->head.order;
    return strcmp(a->head.id, b->head.id);
}

void bs_cards_load(bs_app_t *a)
{
    a->ncard = 0;
    if (aos_hal_path_apps()) aos_hal_dir_scan(aos_hal_path_apps(), scan_cb, a);
    int k = 0;
    for (int i = 0; i < a->ncard; i++) {
        bs_card_t c = a->card[i];
        bs_pak_t pak;
        bool ok = false;
        if (bs_pak_open(&pak, c.path)) {
            char *txt = (char *)bs_pak_get(&pak, "def", NULL);
            ok = txt && bs_world_parse(&c.head, txt, true);
            bs_free(txt);
            bs_pak_close(&pak);
        }
        if (ok) a->card[k++] = c;
        else aos_hal_log("bastion", "%s is not a world", c.path);
    }
    a->ncard = k;
    if (k > 1) qsort(a->card, (size_t)k, sizeof(bs_card_t), card_cmp);
    aos_hal_log("bastion", "%d worlds on the card", k);
}

void bs_seen_from_progress(bs_app_t *a)
{
    if (a->save.seen) return;
    uint32_t seen = 0;
    for (int i = 0; i < a->ncard; i++) {
        int won = 0;
        for (int k = 0; k < BS_SECTORS; k++) won |= a->save.stars[i][k] != 0;
        if (!won) continue;
        bs_pak_t pak;
        if (!bs_pak_open(&pak, a->card[i].path)) continue;
        char *txt = (char *)bs_pak_get(&pak, "def", NULL);
        bs_world_t *w = (bs_world_t *)bs_calloc(sizeof(bs_world_t));
        if (txt && w && bs_world_parse(w, txt, false)) {
            for (int k = 0; k < BS_SECTORS; k++) {
                if (a->save.stars[i][k]) seen |= bs_world_aliens(w, k);
            }
        }
        if (w) bs_world_free(w);
        bs_free(w);
        bs_free(txt);
        bs_pak_close(&pak);
    }
    if (seen) {
        a->save.seen = seen;
        bs_save_store(&a->save);
        aos_hal_log("bastion", "the gallery starts with the aliens of the sectors won: %08x", (unsigned)seen);
    }
}

static void thumbs_load(bs_app_t *a)
{
    for (int i = 0; i < a->ncard; i++) {
        bs_card_t *c = &a->card[i];
        if (c->thumb) continue;
        bs_pak_t pak;
        if (!bs_pak_open(&pak, c->path)) continue;
        c->thumb = bs_pak_image(&pak, "thumb", &c->thumb_dsc);
        bs_pak_close(&pak);
    }
}

static void thumbs_free(bs_app_t *a)
{
    for (int i = 0; i < a->ncard; i++) {
        bs_free(a->card[i].thumb);
        a->card[i].thumb = NULL;
    }
}

void bs_cards_free(bs_app_t *a)
{
    thumbs_free(a);
    a->ncard = 0;
}

/* ---- the screens' frame ------------------------------------------------------------------------ */

static void images_load(bs_app_t *a)
{
    static const char *const NAMES[] = { "ic_core", "ic_star", "ic_star_off", "ic_lock", "fort_big" };
    if (!a->art_ok) return;
    for (int i = 0; i < 5; i++) {
        if (!a->img[i]) a->img[i] = bs_pak_image(&a->art.pak, NAMES[i], &a->img_dsc[i]);
    }
    if (!a->title_img) a->title_img = bs_pak_image(&a->art.pak, "title", &a->title_dsc);
}

static void portraits_load(bs_app_t *a)
{
    char name[BS_NAME_LEN];
    for (int t = 0; t < TW_N && a->art_ok; t++) {
        if (a->img[IMG_TOWER0 + t]) continue;
        snprintf(name, sizeof name, "ti_%s", BS_TOWER[t].id);
        a->img[IMG_TOWER0 + t] = bs_pak_image(&a->art.pak, name, &a->img_dsc[IMG_TOWER0 + t]);
    }
}

static void portraits_free(bs_app_t *a)
{
    for (int t = 0; t < TW_N; t++) {
        bs_free(a->img[IMG_TOWER0 + t]);
        a->img[IMG_TOWER0 + t] = NULL;
    }
}

void bs_menu_free(bs_app_t *a)
{
    aos_pad_menu_clear(&a->menu);
    bs_gallery_free(a);
    if (a->screen) lv_obj_delete(a->screen);
    a->screen = NULL;
    a->m_scroll = a->m_name = a->m_desc = a->m_cores = NULL;
    thumbs_free(a);
    for (int i = 0; i < 40; i++) {
        bs_free(a->img[i]);
        a->img[i] = NULL;
    }
    bs_free(a->title_img);
    a->title_img = NULL;
}

static lv_obj_t *screen_new(bs_app_t *a, int dim)
{
    aos_pad_menu_clear(&a->menu);
    if (a->screen) lv_obj_delete(a->screen);
    a->m_scroll = a->m_name = a->m_desc = a->m_cores = NULL;
    images_load(a);
    lv_obj_t *s = plain(a->root);
    lv_obj_set_size(s, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s, C(0x05070C), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    if (a->title_img) {
        lv_obj_t *bg = lv_image_create(s);
        lv_image_set_src(bg, &a->title_dsc);
        lv_obj_set_pos(bg, 0, 0);
        if (dim) {
            lv_obj_t *d = plain(s);
            lv_obj_set_size(d, lv_pct(100), lv_pct(100));
            lv_obj_set_style_bg_color(d, C(0x000000), 0);
            lv_obj_set_style_bg_opa(d, (lv_opa_t)dim, 0);
        }
    }
    a->screen = s;
    return s;
}

/* the cores the player holds, with their icon */
static lv_obj_t *cores_chip(bs_app_t *a, lv_obj_t *parent)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 60);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    image(a, row, IMG_CORE);
    char s[24];
    snprintf(s, sizeof s, "%d", (int)a->save.cores);
    a->m_cores = bs_label(row, s, aos_font_title, 0x7FE4FF);
    return row;
}

static void ev_go(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    int to = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (to == ST_LAB) a->lab_back = a->state;
    bs_audio_play(SND_TAP, 230);
    bs_menu_show(a, to);
}

static lv_obj_t *header(bs_app_t *a, lv_obj_t *s, const char *title, int back_to)
{
    lv_obj_t *b = bs_button(s, LV_SYMBOL_LEFT, BS_ACCENT, false, 84, 72, ev_go, a);
    lv_obj_set_user_data(b, (void *)(intptr_t)back_to);
    lv_obj_set_pos(b, 16, 18);
    lv_obj_t *t = bs_label(s, title, aos_font_title, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_t *c = cores_chip(a, s);
    lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -18, 24);
    return b;
}

/* ---- the title --------------------------------------------------------------------------------- */

static void ev_title_sfx(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    a->save.sfx = !a->save.sfx;
    bs_audio_set(a->save.sfx, a->save.music);
    bs_save_store(&a->save);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->save.sfx ? _("Efectos: sí") : _("Efectos: no"));
}

static void ev_title_music(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    a->save.music = !a->save.music;
    bs_audio_set(a->save.sfx, a->save.music);
    bs_save_store(&a->save);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), a->save.music ? _("Música: sí") : _("Música: no"));
}

static const char *diff_text(const bs_app_t *a)
{
    switch (a->save.diff) {
    case BS_DIFF_EASY: return _("Dificultad: fácil (más créditos)");
    case BS_DIFF_HARD: return _("Dificultad: difícil (menos créditos)");
    default:           return _("Dificultad: normal");
    }
}

static void ev_title_diff(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    /* easy, normal, hard, round */
    a->save.diff = a->save.diff == BS_DIFF_EASY ? BS_DIFF_NORMAL : a->save.diff == BS_DIFF_NORMAL ? BS_DIFF_HARD : BS_DIFF_EASY;
    bs_save_store(&a->save);
    bs_audio_play(SND_TAP, 230);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), diff_text(a));
}

static void title_build(bs_app_t *a)
{
    lv_obj_t *s = screen_new(a, 0);
    lv_obj_t *t = bs_label(s, "BASTIÓN", aos_font_huge, 0xFFFFFF);
    lv_obj_set_style_text_letter_space(t, 12, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 6, 120);
    lv_obj_t *sub = bs_label(s, _("El último fuerte entre ellos y nosotros"), aos_font_body, BS_ACCENT);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_t *fort = image(a, s, IMG_FORT);
    if (fort) lv_obj_align(fort, LV_ALIGN_TOP_MID, 0, 270);
    if (!a->art_ok || !a->ncard) {
        lv_obj_t *l = bs_label(s, !a->art_ok ? _("Falta bastion_p4.pak en la carpeta apps de la tarjeta.")
                                             : _("No hay ningún mundo (bastion_<mundo>_p4.pak) en la tarjeta."),
                               aos_font_body, 0xFFB0A0);
        lv_obj_set_width(l, 620);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 120);
        return;
    }
    char line[96];
    snprintf(line, sizeof line, "%s %d   ·   %d/%d", _("Rango"), bs_rank_of(a->save.xp), bs_save_stars(&a->save),
             a->ncard * BS_SECTORS * 3);
    lv_obj_t *st = bs_label(s, line, aos_font_body, 0xFFD23C);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 600);
    lv_obj_t *col = plain(s);
    lv_obj_set_size(col, 520, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 14, 0);
    lv_obj_set_style_pad_all(col, 12, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *items[6];
    items[0] = bs_button(col, _("Jugar"), BS_ACCENT, true, 480, 104, ev_go, a);
    lv_obj_set_user_data(items[0], (void *)(intptr_t)ST_MAP);
    lv_obj_t *row = plain(col);
    lv_obj_set_size(row, 480, 84);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    items[1] = bs_button(row, _("Laboratorio"), BS_ACCENT, false, 233, 84, ev_go, a);
    lv_obj_set_user_data(items[1], (void *)(intptr_t)ST_LAB);
    items[2] = bs_button(row, _("Galería"), BS_ACCENT, false, 233, 84, ev_go, a);
    lv_obj_set_user_data(items[2], (void *)(intptr_t)ST_GALLERY);
    lv_obj_set_pos(items[2], 247, 0);
    items[3] = bs_button(col, diff_text(a), BS_ACCENT, false, 480, 76, ev_title_diff, a);
    items[4] = bs_button(col, a->save.sfx ? _("Efectos: sí") : _("Efectos: no"), BS_ACCENT, false, 480, 72, ev_title_sfx, a);
    items[5] = bs_button(col, a->save.music ? _("Música: sí") : _("Música: no"), BS_ACCENT, false, 480, 72, ev_title_music, a);
    aos_pad_menu_set(&a->menu, items, 6, 0);
}

/* ---- the map: the campaign, the mode without end, the free mode ------------------------------------ */

/* The campaign opens a sector when the one before is won; the mode without
 * end takes any world the campaign has reached; the free mode takes them
 * all. */
static bool mode_open(const bs_app_t *a, int world, int sector)
{
    if (a->mode == BS_MODE_FREE) return world >= 0 && world < a->ncard;
    if (a->mode == BS_MODE_ENDLESS) return bs_save_open(&a->save, world, 0);
    return bs_save_open(&a->save, world, sector);
}

static void ev_mode(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    a->mode = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    a->save.mode = (uint8_t)a->mode;
    bs_save_store(&a->save);
    bs_audio_play(SND_TAP, 230);
    bs_menu_show(a, ST_MAP);
}

static void ev_sector(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    int k = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (!mode_open(a, a->sel_world, k)) {
        bs_audio_play(SND_DENY, 230);
        aos_ui_toast(a->mode == BS_MODE_ENDLESS ? _("Llegá a este mundo en la campaña para jugarlo acá")
                     : k ? _("Ganá el sector anterior para abrir este") : _("Ganá el último sector del mundo anterior"), 2500);
        return;
    }
    bs_audio_play(SND_TAP, 230);
    bs_battle_start(a, a->sel_world, k);
}

static void map_theme(bs_app_t *a)
{
    if (!a->m_name || !a->ncard) return;
    const bs_world_t *w = &a->card[a->sel_world].head;
    char line[96];
    snprintf(line, sizeof line, "%d · %s", a->sel_world + 1, wname(w));
    lv_label_set_text(a->m_name, line);
    lv_obj_set_style_text_color(a->m_name, C(w->accent), 0);
    lv_label_set_text(a->m_desc, a->mode == BS_MODE_ENDLESS
                                     ? _("El doble de vida y oleadas sin fin. Cada 20 hay premio, y te retirás cuando quieras.")
                                     : a->mode == BS_MODE_FREE
                                     ? _("Todas las torres y poderes, y el triple de créditos, para probar. No da núcleos.")
                                     : wdesc(w));
    for (int k = 0; k < BS_SECTORS; k++) {
        lv_obj_t *b = a->m_sector[k];
        if (!b) continue;
        bool open = mode_open(a, a->sel_world, k);
        int stars = a->save.stars[a->sel_world][k];
        lv_obj_set_style_border_color(b, C(open ? w->accent : 0x404858), 0);
        lv_obj_set_style_bg_color(b, C(open ? mixc(w->accent, 0x000000, 190) : 0x161A24), 0);
        lv_obj_set_style_bg_grad_color(b, C(0x0A0D14), 0);
        lv_obj_t *l = lv_obj_get_child(b, 0), *st = lv_obj_get_child(b, 1);
        lv_obj_set_style_text_color(l, C(open ? 0xFFFFFF : 0x707888), 0);
        if (a->mode == BS_MODE_ENDLESS) {
            /* one button: play, with the record under it */
            lv_label_set_text(l, _("Jugar"));
            int best = a->save.best[a->sel_world];
            if (!open) snprintf(line, sizeof line, LV_SYMBOL_CLOSE);
            else if (best) {
                /* and a mark for each goal of twenty waves it passed, up to three */
                int n = snprintf(line, sizeof line, _("Récord: %d oleadas"), best);
                int marks = best / BS_MILE_STEP > 3 ? 3 : best / BS_MILE_STEP;
                for (int q = 0; q < marks && n > 0 && n < (int)sizeof line - 8; q++)
                    n += snprintf(line + n, sizeof line - (size_t)n, "%s" LV_SYMBOL_OK, q ? " " : "   ");
            }
            else snprintf(line, sizeof line, "%s", _("Sin récord todavía"));
            lv_label_set_text(st, line);
            lv_obj_set_style_text_color(st, C(open ? (best ? 0xFFD23C : w->accent) : 0x707888), 0);
            continue;
        }
        snprintf(line, sizeof line, "%s %d", _("Sector"), k + 1);
        lv_label_set_text(l, line);
        if (!open) {
            lv_label_set_text(st, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(st, C(0x707888), 0);
        } else if (a->mode == BS_MODE_FREE) {
            lv_label_set_text(st, k == BS_SECTORS - 1 ? _("Jefe") : _("Libre"));
            lv_obj_set_style_text_color(st, C(w->accent), 0);
        } else {
            /* three marks: the ones won lit */
            lv_label_set_text(st, stars >= 3 ? LV_SYMBOL_OK " " LV_SYMBOL_OK " " LV_SYMBOL_OK
                                  : stars == 2 ? LV_SYMBOL_OK " " LV_SYMBOL_OK
                                  : stars == 1 ? LV_SYMBOL_OK : (k == BS_SECTORS - 1 ? _("Jefe") : _("Nuevo")));
            lv_obj_set_style_text_color(st, C(stars ? 0xFFD23C : w->accent), 0);
        }
    }
}

static void ev_map_scroll(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    if (!a->m_scroll) return;
    lv_area_t sa;
    lv_obj_get_coords(a->m_scroll, &sa);
    int mid = (sa.x1 + sa.x2) / 2, best = a->sel_world, bd = 1 << 30;
    for (int i = 0; i < a->ncard; i++) {
        if (!a->m_card[i]) continue;
        lv_area_t ca;
        lv_obj_get_coords(a->m_card[i], &ca);
        int d = abs((ca.x1 + ca.x2) / 2 - mid);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best != a->sel_world) {
        a->sel_world = best;
        map_theme(a);
    }
}

static void ev_card(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (i != a->sel_world && a->m_card[i]) lv_obj_scroll_to_view(a->m_card[i], LV_ANIM_ON);
}

static void map_build(bs_app_t *a)
{
    static const char *const MODE[BS_MODE_N] = { N_("Campaña"), N_("Supervivencia"), N_("Libre") };
    lv_obj_t *s = screen_new(a, LV_OPA_60);
    thumbs_load(a);
    if (a->mode < 0 || a->mode >= BS_MODE_N) a->mode = BS_MODE_CAMPAIGN;
    lv_obj_t *items[BS_MODE_N + BS_SECTORS + 2];
    int n = 0;
    header(a, s, _(MODE[a->mode]), ST_TITLE);
    /* the three modes, as tabs */
    for (int m = 0; m < BS_MODE_N; m++) {
        lv_obj_t *b = bs_button(s, _(MODE[m]), BS_ACCENT, m == a->mode, 224, 58, ev_mode, a);
        lv_obj_set_style_text_font(b, aos_font_small, 0);
        lv_obj_set_pos(b, 16 + m * 232, 102);
        lv_obj_set_user_data(b, (void *)(intptr_t)m);
        items[n++] = b;
    }
    lv_obj_t *sc = plain(s);
    a->m_scroll = sc;
    lv_obj_set_size(sc, 720, THUMB_H + 40);
    lv_obj_set_pos(sc, 0, 166);
    lv_obj_add_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sc, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(sc, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sc, 26, 0);
    lv_obj_set_style_pad_left(sc, (720 - CARD_W) / 2, 0);
    lv_obj_set_style_pad_right(sc, (720 - CARD_W) / 2, 0);
    lv_obj_add_event_cb(sc, ev_map_scroll, LV_EVENT_SCROLL, a);
    for (int i = 0; i < a->ncard; i++) {
        bs_card_t *c = &a->card[i];
        bool open = mode_open(a, i, 0);
        lv_obj_t *card = plain(sc);
        a->m_card[i] = card;
        lv_obj_set_size(card, CARD_W, THUMB_H + 20);
        lv_obj_set_style_radius(card, 26, 0);
        lv_obj_set_style_bg_color(card, C(0x0A0D14), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 4, 0);
        lv_obj_set_style_border_color(card, C(open ? c->head.accent : 0x353B4A), 0);
        lv_obj_set_style_clip_corner(card, true, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, ev_card, LV_EVENT_CLICKED, a);
        if (c->thumb) {
            lv_obj_t *im = lv_image_create(card);
            lv_image_set_src(im, &c->thumb_dsc);
            lv_obj_center(im);
            lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        }
        char t[40] = "";
        if (!open) {
            lv_obj_t *d = plain(card);
            lv_obj_set_size(d, lv_pct(100), lv_pct(100));
            lv_obj_set_style_bg_color(d, C(0x000000), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_70, 0);
            lv_obj_t *lk = image(a, card, IMG_LOCK);
            if (lk) lv_obj_center(lk);
        } else if (a->mode == BS_MODE_CAMPAIGN) {
            int stars = 0;
            for (int k = 0; k < BS_SECTORS; k++) stars += a->save.stars[i][k];
            snprintf(t, sizeof t, LV_SYMBOL_OK " %d/9", stars);
        } else if (a->mode == BS_MODE_ENDLESS && a->save.best[i]) {
            snprintf(t, sizeof t, LV_SYMBOL_UP " %d", (int)a->save.best[i]);
        }
        if (t[0]) {
            lv_obj_t *l = bs_label(card, t, aos_font_small, 0xFFD23C);
            lv_obj_set_style_bg_color(l, C(0x000000), 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_60, 0);
            lv_obj_set_style_pad_hor(l, 12, 0);
            lv_obj_set_style_pad_ver(l, 6, 0);
            lv_obj_set_style_radius(l, 12, 0);
            lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -14, 14);
        }
    }
    int y = 166 + THUMB_H + 44;
    a->m_name = bs_label(s, "", aos_font_title, 0xFFFFFF);
    lv_obj_align(a->m_name, LV_ALIGN_TOP_MID, 0, y);
    a->m_desc = bs_label(s, "", aos_font_small, 0xD0D6E4);
    lv_obj_set_width(a->m_desc, 670);
    lv_label_set_long_mode(a->m_desc, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(a->m_desc, LV_ALIGN_TOP_MID, 0, y + 46);
    /* three sectors, or one button for the mode without end */
    int nb = a->mode == BS_MODE_ENDLESS ? 1 : BS_SECTORS;
    int first_btn = n;
    for (int k = 0; k < BS_SECTORS; k++) {
        a->m_sector[k] = NULL;
        if (k >= nb) continue;
        lv_obj_t *b = lv_button_create(s);
        a->m_sector[k] = b;
        lv_obj_set_size(b, nb == 1 ? 684 : 216, 136);
        lv_obj_set_pos(b, 18 + k * 234, y + 122);
        lv_obj_set_style_radius(b, 22, 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_t *st = lv_label_create(b);
        lv_obj_set_style_text_font(st, aos_font_body, 0);
        lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -14);
        lv_obj_set_user_data(b, (void *)(intptr_t)k);
        lv_obj_add_event_cb(b, ev_sector, LV_EVENT_CLICKED, a);
        items[n++] = b;
    }
    lv_obj_t *lab = bs_button(s, _("Laboratorio"), BS_ACCENT, false, 420, 78, ev_go, a);
    lv_obj_set_user_data(lab, (void *)(intptr_t)ST_LAB);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_MID, 0, -22);
    items[n++] = lab;
    /* where the player stands: the furthest world this mode opens that
     * the campaign has reached */
    if (a->sel_world < 0 || a->sel_world >= a->ncard || !mode_open(a, a->sel_world, 0)) {
        a->sel_world = 0;
        for (int i = 0; i < a->ncard; i++) {
            if (bs_save_open(&a->save, i, 0)) a->sel_world = i;
        }
    }
    map_theme(a);
    lv_obj_update_layout(sc);
    if (a->m_card[a->sel_world]) lv_obj_scroll_to_view(a->m_card[a->sel_world], LV_ANIM_OFF);
    int first = 0;
    for (int k = 0; k < nb; k++) {
        if (mode_open(a, a->sel_world, k) && a->mode == BS_MODE_CAMPAIGN) first = k;
    }
    aos_pad_menu_set(&a->menu, items, n, first_btn + first);
}

/* ---- the laboratory ------------------------------------------------------------------------------ */

static int lab_cost(const bs_app_t *a, int tab, int i)
{
    const bs_save_t *sv = &a->save;
    switch (tab) {
    case TAB_TOWERS:
        if (!sv->owned[i]) return BS_TOWER[i].unlock;
        return sv->mastery[i] < BS_MASTERY_MAX ? BS_MASTERY_COST[sv->mastery[i]] : -1;
    case TAB_TECH:
        return sv->tech[i] < BS_TECH_MAX ? BS_TECH[i].cost[sv->tech[i]] : -1;
    default:
        return sv->power[i] < BS_POWER_MAX ? BS_POWER[i].cost[sv->power[i]] : -1;
    }
}

static void ev_buy(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    int cost = lab_cost(a, a->lab_tab, i);
    if (cost < 0) return;
    if (a->save.cores < cost) {
        bs_audio_play(SND_DENY, 230);
        aos_ui_toast(_("Te faltan núcleos: se ganan en las batallas"), 2200);
        return;
    }
    a->save.cores -= cost;
    switch (a->lab_tab) {
    case TAB_TOWERS:
        if (!a->save.owned[i]) a->save.owned[i] = 1;
        else a->save.mastery[i]++;
        break;
    case TAB_TECH: a->save.tech[i]++; break;
    default: a->save.power[i]++; break;
    }
    bs_save_store(&a->save);
    bs_audio_play(SND_UPGRADE, 230);
    int keep = a->menu.sel;
    lab_build(a);
    if (keep < a->menu.n) a->menu.sel = (uint8_t)keep;
}

static void ev_tab(lv_event_t *e)
{
    bs_app_t *a = (bs_app_t *)lv_event_get_user_data(e);
    a->lab_tab = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    bs_audio_play(SND_TAP, 230);
    lab_build(a);
}

static lv_obj_t *lab_row(bs_app_t *a, lv_obj_t *list, int i, const char *name, const char *desc, uint32_t acc, int level,
                         int max, const char *state, lv_obj_t **img_parent)
{
    lv_obj_t *row = plain(list);
    lv_obj_set_size(row, 684, 172);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_set_style_bg_color(row, C(0x121826), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_90, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, C(mixc(acc, 0x000000, 120)), 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    *img_parent = row;
    lv_obj_t *n = bs_label(row, name, aos_font_body, acc);
    lv_obj_set_pos(n, 132, 10);
    lv_obj_t *d = bs_label(row, desc, aos_font_small, 0xC8CEDC);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(d, 330);
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(d, 132, 46);
    /* the level, as pips */
    for (int k = 0; k < max; k++) {
        lv_obj_t *p = plain(row);
        lv_obj_set_size(p, 22, 8);
        lv_obj_set_pos(p, 132 + k * 28, 154);
        lv_obj_set_style_radius(p, 4, 0);
        lv_obj_set_style_bg_color(p, C(k < level ? acc : 0x303848), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    }
    int cost = lab_cost(a, a->lab_tab, i);
    char t[48];
    if (cost < 0) snprintf(t, sizeof t, "%s", _("Al máximo"));
    else snprintf(t, sizeof t, "%s\n%d", state, cost);
    lv_obj_t *b = bs_button(row, t, acc, cost >= 0 && a->save.cores >= cost, 196, 112, ev_buy, a);
    lv_obj_set_style_text_font(b, aos_font_small, 0);
    lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_user_data(b, (void *)(intptr_t)i);
    if (cost < 0) lv_obj_add_state(b, LV_STATE_DISABLED);
    return b;
}

static void lab_build(bs_app_t *a)
{
    lv_obj_t *s = screen_new(a, LV_OPA_70);
    portraits_load(a);
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n = 0;
    items[n++] = header(a, s, _("Laboratorio"), a->lab_back == ST_MAP ? ST_MAP : ST_TITLE);
    /* the rank and how far the next one is */
    int rank = bs_rank_of(a->save.xp);
    char line[96];
    if (rank < BS_RANK_MAX)
        snprintf(line, sizeof line, "%s %d  ·  %d / %d", _("Rango"), rank, (int)a->save.xp, bs_rank_xp(rank + 1));
    else
        snprintf(line, sizeof line, "%s %d", _("Rango"), rank);
    lv_obj_t *rk = bs_label(s, line, aos_font_small, 0xFFD23C);
    lv_obj_align(rk, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_t *bar = lv_bar_create(s);
    lv_obj_set_size(bar, 420, 10);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_set_style_bg_color(bar, C(0x283040), 0);
    lv_obj_set_style_bg_color(bar, C(0xFFD23C), LV_PART_INDICATOR);
    int lo = bs_rank_xp(rank), hi = bs_rank_xp(rank + 1);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, rank >= BS_RANK_MAX || hi <= lo ? 100 : (int)((a->save.xp - lo) * 100 / (hi - lo)), LV_ANIM_OFF);

    static const char *const TABS[TAB_N] = { N_("Torres"), N_("Tecnología"), N_("Poderes") };
    for (int t = 0; t < TAB_N; t++) {
        lv_obj_t *b = bs_button(s, _(TABS[t]), BS_ACCENT, t == a->lab_tab, 220, 72, ev_tab, a);
        lv_obj_set_pos(b, 18 + t * 232, 160);
        lv_obj_set_user_data(b, (void *)(intptr_t)t);
        items[n++] = b;
    }
    lv_obj_t *list = plain(s);
    lv_obj_set_size(list, 720, 1280 - 250);
    lv_obj_set_pos(list, 0, 250);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 14, 0);
    lv_obj_set_style_pad_top(list, 12, 0);
    lv_obj_set_style_pad_bottom(list, 40, 0);
    lv_obj_t *row;
    if (a->lab_tab == TAB_TOWERS) {
        for (int i = 0; i < TW_N; i++) {
            bool owned = a->save.owned[i];
            snprintf(line, sizeof line, "%s", owned ? _("+8 % de daño y -3 % de costo por nivel de maestría.") : _(BS_TOWER[i].desc));
            lv_obj_t *b = lab_row(a, list, i, _(BS_TOWER[i].name), line, BS_TOWER[i].accent, owned ? a->save.mastery[i] : 0,
                                  BS_MASTERY_MAX, owned ? _("Maestría") : _("Desbloquear"), &row);
            lv_obj_t *im = image(a, row, IMG_TOWER0 + i);
            if (im) {
                lv_obj_set_pos(im, 4, 22);
                if (!owned) lv_obj_set_style_image_opa(im, LV_OPA_40, 0);
            }
            if (!owned) {
                lv_obj_t *lk = image(a, row, IMG_LOCK);
                if (lk) lv_obj_set_pos(lk, 36, 54);
            }
            if (n < AOS_PAD_MENU_MAX) items[n++] = b;
        }
    } else if (a->lab_tab == TAB_TECH) {
        for (int i = 0; i < TECH_N; i++) {
            lv_obj_t *b = lab_row(a, list, i, _(BS_TECH[i].name), _(BS_TECH[i].desc), BS_ACCENT, a->save.tech[i], BS_TECH_MAX,
                                  _("Investigar"), &row);
            snprintf(line, sizeof line, "%d", a->save.tech[i]);
            lv_obj_t *lv = bs_label(row, line, aos_font_huge, a->save.tech[i] ? BS_ACCENT : 0x505868);
            lv_obj_set_pos(lv, 44, 42);
            if (n < AOS_PAD_MENU_MAX) items[n++] = b;
        }
    } else {
        for (int i = 0; i < PW_N; i++) {
            int lv_ = a->save.power[i];
            lv_obj_t *b = lab_row(a, list, i, _(BS_POWER[i].name), _(BS_POWER[i].desc), BS_POWER[i].color, lv_, BS_POWER_MAX,
                                  lv_ ? _("Mejorar") : _("Desbloquear"), &row);
            snprintf(line, sizeof line, "%d", lv_);
            lv_obj_t *l = bs_label(row, line, aos_font_huge, lv_ ? BS_POWER[i].color : 0x505868);
            lv_obj_set_pos(l, 44, 42);
            if (n < AOS_PAD_MENU_MAX) items[n++] = b;
        }
    }
    aos_pad_menu_set(&a->menu, items, n, 1 + a->lab_tab);
}

/* ---- which screen ---------------------------------------------------------------------------------- */

void bs_menu_show(bs_app_t *a, int state)
{
    if (state != ST_LAB && state != ST_GALLERY) portraits_free(a);
    if (state != ST_MAP && state != ST_GALLERY) thumbs_free(a);
    if (state != ST_GALLERY) bs_gallery_free(a);
    a->state = state;
    lv_obj_set_style_bg_color(a->root, C(0x05070C), 0);
    if (a->art_ok) bs_audio_music(&a->art.pak, BS_MUSIC_MENU);
    switch (state) {
    case ST_MAP: map_build(a); break;
    case ST_LAB: lab_build(a); break;
    case ST_GALLERY:
        portraits_load(a);
        bs_gallery_build(a);
        break;
    default:
        a->state = ST_TITLE;
        title_build(a);
        break;
    }
    aos_pad_reset(&a->pad, lv_tick_get());
}

lv_obj_t *bs_menu_screen(bs_app_t *a, int dim) { return screen_new(a, dim); }
lv_obj_t *bs_menu_header(bs_app_t *a, lv_obj_t *s, const char *title, int back_to) { return header(a, s, title, back_to); }
lv_obj_t *bs_menu_plain(lv_obj_t *parent) { return plain(parent); }
void bs_thumbs_load(bs_app_t *a) { thumbs_load(a); }

bool bs_menu_back(bs_app_t *a)
{
    if (a->state == ST_GALLERY) {
        if (!bs_gallery_back(a)) bs_menu_show(a, ST_TITLE);
        return true;
    }
    if (a->state == ST_MAP) {
        bs_menu_show(a, ST_TITLE);
        return true;
    }
    if (a->state == ST_LAB) {
        bs_menu_show(a, a->lab_back == ST_MAP ? ST_MAP : ST_TITLE);
        return true;
    }
    return false;
}

void bs_menu_pad(bs_app_t *a)
{
    aos_pad_t *p = &a->pad;
    if (a->state == ST_GALLERY) {
        bs_gallery_pad(a);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_B)) {
        if (bs_menu_back(a)) return;
    }
    if (a->state == ST_MAP && a->m_scroll) {
        int d = aos_pad_repeat(p, AOS_PAD_L) ? -1 : aos_pad_repeat(p, AOS_PAD_R) ? 1 : 0;
        if (d) {
            int i = a->sel_world + d;
            if (i >= 0 && i < a->ncard && a->m_card[i]) lv_obj_scroll_to_view(a->m_card[i], LV_ANIM_ON);
        }
        if (aos_pad_pressed(p, AOS_PAD_START)) {
            /* the furthest sector open of the world in front */
            for (int k = a->mode == BS_MODE_ENDLESS ? 0 : BS_SECTORS - 1; k >= 0; k--) {
                if (mode_open(a, a->sel_world, k)) {
                    bs_battle_start(a, a->sel_world, k);
                    return;
                }
            }
        }
    } else if (a->state == ST_LAB) {
        int d = aos_pad_pressed(p, AOS_PAD_L) ? -1 : aos_pad_pressed(p, AOS_PAD_R) ? 1 : 0;
        if (d) {
            a->lab_tab = (a->lab_tab + d + TAB_N) % TAB_N;
            lab_build(a);
            return;
        }
    } else if (a->state == ST_TITLE && aos_pad_pressed(p, AOS_PAD_START) && a->ncard) {
        bs_menu_show(a, ST_MAP);
        return;
    }
    aos_pad_menu_step(&a->menu, p);
}
