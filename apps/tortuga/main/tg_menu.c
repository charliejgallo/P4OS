/*
 * Tortuga - the menus: the start, "how do you want to program?" (the four
 * ways, each with the same little program), the worlds and a world's
 * levels. A gamepad walks them with aos_pad_menu.
 */
#include "tg_app.h"

#include "aos_fonts.h"
#include "aos_pad_menu.h"
#include "tg_view.h"

#include <stdio.h>
#include <string.h>

static aos_pad_menu_t s_menu;
static lv_obj_t *s_items[AOS_PAD_MENU_MAX];
static int s_nitems;

/* the start's turtle, cheering on a patch of grass */
static struct {
    lv_obj_t *canvas;
    uint16_t *buf;
    int frame;
    uint32_t last;
} H;

#define HW 300
#define HH 150

static void home_draw(void)
{
    if (!H.buf) return;
    uint16_t bg = tg_rgb(0x101318);
    for (int i = 0; i < HW * HH; i++) H.buf[i] = bg;
    if (tg->have_pak) {
        const tg_sheet_t *g = &tg->sheet[SH_GROUND];
        for (int k = 0; k < 4; k++) tg_sheet_blit(g, k == 1 ? 2 : k == 3 ? 3 : 0, H.buf, HW, HH, 54 + 64 * k, 90);
        const tg_sheet_t *t = &tg->sheet[SH_PARTY];
        static const int LIFT[6] = { 0, 8, 16, 14, 6, 0 };
        tg_sheet_blit(t, (H.frame % 6) * t->cols + 3, H.buf, HW, HH, HW / 2, 102 - LIFT[H.frame % 6]);
    }
    lv_obj_invalidate(H.canvas);
}

void tg_menu_tick(void)
{
    if (tg->screen != SCR_HOME || !H.canvas) return;
    uint32_t now = lv_tick_get();
    if (now - H.last < 110) return;
    H.last = now;
    H.frame++;
    home_draw();
}

static void home_free(lv_event_t *e)
{
    (void)e;
    tg_free(H.buf);
    memset(&H, 0, sizeof H);
}

static void menu_reset(void)
{
    aos_pad_menu_clear(&s_menu);
    s_nitems = 0;
}

static void menu_add(lv_obj_t *o)
{
    if (s_nitems < AOS_PAD_MENU_MAX) s_items[s_nitems++] = o;
}

static void menu_done(int sel)
{
    aos_pad_menu_set(&s_menu, s_items, s_nitems, sel);
}

void tg_menu_pad(void)
{
    aos_pad_menu_step(&s_menu, &tg->pad);
    if (aos_pad_pressed(&tg->pad, AOS_PAD_B)) tg_menu_back();
}

static lv_obj_t *column(lv_obj_t *scr, bool scroll)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, tg->W, tg->H);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 16, 0);
    if (scroll) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(c, LV_DIR_VER);
    } else {
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    }
    return c;
}

static lv_obj_t *header(lv_obj_t *parent, const char *title, lv_event_cb_t back)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (back) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 64, 64);
        lv_obj_t *l = tg_label(b, AOS_SYM_ARROW_LEFT, &aos_sym_44, AOS_C_ORANGE);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, back, LV_EVENT_CLICKED, NULL);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
        menu_add(b);
    }
    lv_obj_t *t = tg_label(row, title, aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, back ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, back ? 76 : 0, 0);
    return row;
}

/* ------------------------------------------------------------------ home */

static void go_worlds(lv_event_t *e)
{
    (void)e;
    tg_audio_play(SND_TAP);
    tg_go(SCR_WORLDS);
}

static void go_pick(lv_event_t *e)
{
    (void)e;
    tg_audio_play(SND_TAP);
    tg_go(SCR_PICK);
}

static void go_free(lv_event_t *e)
{
    (void)e;
    tg_audio_play(SND_TAP);
    tg->free_mode = true;
    tg->level = -1;
    tg_go(SCR_PLAY);
}

static void go_continue(lv_event_t *e)
{
    (void)e;
    tg_audio_play(SND_TAP);
    /* the first level without stars in this language, in an open world */
    int lang = tg->prefs.lang;
    for (int i = 0; i < tg->levels.n; i++) {
        if (!tg->score[i].stars[lang] && tg_world_open(tg->levels.lv[i].world)) {
            tg->free_mode = false;
            tg->level = i;
            tg->world = tg->levels.lv[i].world;
            tg_go(SCR_PLAY);
            return;
        }
    }
    tg_go(SCR_WORLDS);
}

static void toggle_mode(lv_event_t *e)
{
    (void)e;
    tg->prefs.hard = !tg->prefs.hard;
    tg_save_prefs();
    tg_audio_play(SND_TAP);
    tg_go(SCR_HOME);
}

static void toggle_sound(lv_event_t *e)
{
    (void)e;
    tg->prefs.sound = !tg->prefs.sound;
    tg_save_prefs();
    tg_audio_play(SND_TAP);
    tg_go(SCR_HOME);
}

static lv_obj_t *chip(lv_obj_t *parent, const char *sym, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 72);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x262b33), 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    tg_label(b, sym, &aos_sym_28, color);
    tg_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    menu_add(b);
    return b;
}

void tg_menu_home(lv_obj_t *scr)
{
    menu_reset();
    lv_obj_t *c = column(scr, true);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *left = c, *right = c;
    if (tg->land) {
        /* lying down: the turtle and the title on the left, the buttons on the right */
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        for (int k = 0; k < 2; k++) {
            lv_obj_t *col = lv_obj_create(c);
            lv_obj_remove_style_all(col);
            lv_obj_set_size(col, k ? 600 : 560, LV_PCT(100));
            lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(col, 16, 0);
            lv_obj_add_flag(col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            if (k) right = col;
            else left = col;
        }
    }

    H.buf = tg_alloc(HW * HH * 2);
    if (H.buf) {
        H.canvas = lv_canvas_create(left);
        lv_canvas_set_buffer(H.canvas, H.buf, HW, HH, LV_COLOR_FORMAT_RGB565);
        lv_obj_add_event_cb(H.canvas, home_free, LV_EVENT_DELETE, NULL);
        /* drawn at the board's size, shown bigger */
        lv_obj_set_style_transform_scale(H.canvas, 420, 0);
        lv_obj_set_style_transform_pivot_x(H.canvas, HW / 2, 0);
        lv_obj_set_style_transform_pivot_y(H.canvas, HH, 0);
        lv_obj_set_style_margin_top(H.canvas, 60, 0);
        H.frame = 0;
        home_draw();
    }
    lv_obj_t *t = tg_label(left, "Tortuga", aos_font_huge, lv_color_hex(0xF2C27A));
    (void)t;
    tg_label(left, _("Programá a la tortuga hasta la meta"), aos_font_body, AOS_C_DIM);

    char buf[64];
    snprintf(buf, sizeof buf, "%d", tg_total_stars());
    lv_obj_t *st = lv_obj_create(left);
    lv_obj_remove_style_all(st);
    lv_obj_set_size(st, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 8, 0);
    tg_label(st, AOS_SYM_STAR, &aos_sym_44, AOS_C_YELLOW);
    tg_label(st, buf, aos_font_title, AOS_C_TEXT);

    int w = tg->land ? 560 : tg->W - 80;
    c = right;
    lv_obj_t *play = tg_button(c, NULL, lv_color_hex(0xFF9F0A), go_continue, NULL);
    lv_obj_set_size(play, w, 96);
    lv_obj_t *pl = tg_label(play, _("Jugar"), aos_font_title, lv_color_hex(0x2a1a05));
    lv_obj_center(pl);
    menu_add(play);

    lv_obj_t *worlds = tg_button(c, _("Mundos"), lv_color_hex(0x2c3440), go_worlds, NULL);
    lv_obj_set_size(worlds, w, 80);
    menu_add(worlds);
    lv_obj_t *fr = tg_button(c, _("Hoja libre"), lv_color_hex(0x2c3440), go_free, NULL);
    lv_obj_set_size(fr, w, 80);
    menu_add(fr);

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    chip(row, AOS_SYM_PUZZLE, tg_lang_name(tg->prefs.lang), tg_lang_color(tg->prefs.lang), go_pick);
    chip(row, tg->prefs.hard ? AOS_SYM_LOCK : AOS_SYM_CHECK, tg->prefs.hard ? _("Difícil") : _("Fácil"),
         tg->prefs.hard ? AOS_C_RED : AOS_C_GREEN, toggle_mode);
    chip(row, tg->prefs.sound ? AOS_SYM_VOLUME_HIGH : AOS_SYM_VOLUME_OFF, _("Sonido"), AOS_C_TEAL, toggle_sound);

    lv_obj_t *note = tg_label(c, tg->prefs.hard ? _("Difícil: la tortuga tiene que terminar justo en la meta.")
                                                : _("Fácil: se gana al pasar por la meta."),
                              aos_font_small, AOS_C_DIM);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, w);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    menu_done(1);
}

/* ------------------------------------------------------------------ pick */

static void pick_cb(lv_event_t *e)
{
    int lang = (int)(intptr_t)lv_event_get_user_data(e);
    if (!tg_lang_available(lang)) {
        tg_audio_play(SND_FAIL);
        return;
    }
    tg_audio_play(SND_TAP);
    tg->prefs.lang = (uint8_t)lang;
    tg->prefs.chosen = 1;
    tg_save_prefs();
    tg_go(SCR_HOME);
}

static void back_home(lv_event_t *e)
{
    (void)e;
    tg_go(SCR_HOME);
}

void tg_menu_pick(lv_obj_t *scr)
{
    menu_reset();
    lv_obj_t *c = column(scr, true);
    header(c, _("¿Cómo querés programar?"), tg->prefs.chosen ? back_home : NULL);
    static const char *const SAMPLE[TG_LANG_N] = {
        NULL,
        "repetir 4 [\n  avanzar 2\n  girar derecha\n]",
        "for i = 1, 4 do\n  avanzar(2)\n  girar_derecha()\nend",
        "for _ in range(4):\n    avanzar(2)\n    girar_derecha()",
    };
    static const char *const SAMPLE_EN[TG_LANG_N] = {
        NULL,
        "repeat 4 [\n  forward 2\n  turn right\n]",
        "for i = 1, 4 do\n  forward(2)\n  turn_right()\nend",
        "for _ in range(4):\n    forward(2)\n    turn_right()",
    };
    int cw = tg->land ? (tg->W - 40 - 3 * 16) / 2 : tg->W - 40;
    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (int lang = 0; lang < TG_LANG_N; lang++) {
        bool ok = tg_lang_available(lang);
        lv_obj_t *card = lv_button_create(grid);
        lv_obj_set_size(card, cw, tg->land ? 270 : 262);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1f242b), 0);
        lv_obj_set_style_border_color(card, tg_lang_color(lang), 0);
        lv_obj_set_style_border_width(card, lang == tg->prefs.lang && tg->prefs.chosen ? 4 : 2, 0);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_add_event_cb(card, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)lang);
        lv_obj_t *name = tg_label(card, tg_lang_name(lang), aos_font_title, tg_lang_color(lang));
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
        static const char *const WHO[TG_LANG_N] = {
            N_("Sin teclado: se tocan las piezas"), N_("El clásico de la tortuga, con palabras"),
            N_("Un lenguaje de verdad, chico"), N_("Se instala aparte"),
        };
        lv_obj_t *who = tg_label(card, _(WHO[lang]), aos_font_small, AOS_C_DIM);
        lv_obj_align(who, LV_ALIGN_TOP_LEFT, 0, 46);
        if (lang == TG_LANG_BLOCKS) {
            lv_obj_t *bx = lv_obj_create(card);
            lv_obj_remove_style_all(bx);
            lv_obj_set_size(bx, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(bx, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(bx, 6, 0);
            lv_obj_align(bx, LV_ALIGN_TOP_LEFT, 0, 86);
            lv_obj_remove_flag(bx, LV_OBJ_FLAG_CLICKABLE);
            static const char *const B[3] = { N_("repetir 4"), N_("avanzar 2"), N_("girar derecha") };
            static const uint32_t BC[3] = { 0xff9f0a, 0x34c759, 0x34c759 };
            for (int k = 0; k < 3; k++) {
                lv_obj_t *pill = lv_obj_create(bx);
                lv_obj_remove_style_all(pill);
                lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(pill, lv_color_hex(BC[k]), 0);
                lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(pill, 10, 0);
                lv_obj_set_style_pad_hor(pill, 12, 0);
                lv_obj_set_style_pad_ver(pill, 4, 0);
                lv_obj_set_style_margin_left(pill, k ? 24 : 0, 0);
                lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
                tg_label(pill, _(B[k]), aos_font_small, lv_color_hex(0x10140c));
            }
        } else {
            const char *s = tg->tlang == TG_ES ? SAMPLE[lang] : SAMPLE_EN[lang];
            lv_obj_t *code = tg_label(card, s, &aos_mono_22, ok ? AOS_C_TEXT : AOS_C_DIM);
            lv_obj_align(code, LV_ALIGN_TOP_LEFT, 0, 86);
        }
        if (!ok) {
            lv_obj_set_style_bg_opa(card, LV_OPA_60, 0);
            lv_obj_t *soon = tg_label(card, _("pronto"), aos_font_small, AOS_C_ORANGE);
            lv_obj_align(soon, LV_ALIGN_TOP_RIGHT, 0, 4);
        }
        menu_add(card);
    }
    menu_done(tg->prefs.chosen ? 1 + tg->prefs.lang : 0);
}

/* ---------------------------------------------------------------- worlds */

static void world_cb(lv_event_t *e)
{
    int w = (int)(intptr_t)lv_event_get_user_data(e);
    if (!tg_world_open(w)) {
        tg_audio_play(SND_FAIL);
        return;
    }
    tg_audio_play(SND_TAP);
    tg->world = w;
    tg_go(SCR_LEVELS);
}

void tg_menu_worlds(lv_obj_t *scr)
{
    menu_reset();
    lv_obj_t *c = column(scr, true);
    header(c, _("Mundos"), back_home);
    int sel = 1;
    int cw = tg->land ? (tg->W - 40 - 16) / 2 : tg->W - 40;
    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (int w = 1; w <= tg_world_max(); w++) {
        int n = tg_world_count(w);
        if (!n) continue;
        bool open = tg_world_open(w);
        lv_obj_t *card = lv_button_create(grid);
        lv_obj_set_size(card, cw, 116);
        lv_obj_set_style_bg_color(card, open ? lv_color_hex(0x1f2a1c) : lv_color_hex(0x1c1f24), 0);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_add_event_cb(card, world_cb, LV_EVENT_CLICKED, (void *)(intptr_t)w);
        char buf[80];
        snprintf(buf, sizeof buf, "%d  %s", w, tg_world_name(w));
        lv_obj_t *t = tg_label(card, buf, aos_font_body, open ? AOS_C_TEXT : AOS_C_DIM);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *idea = tg_label(card, tg_world_idea(w), aos_font_small, AOS_C_DIM);
        lv_obj_align(idea, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (open) {
            snprintf(buf, sizeof buf, "%d/%d", tg_world_stars(w, tg->prefs.lang), n * 3);
            lv_obj_t *s = lv_obj_create(card);
            lv_obj_remove_style_all(s);
            lv_obj_set_size(s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(s, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(s, 6, 0);
            lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
            tg_label(s, AOS_SYM_STAR, &aos_sym_28, AOS_C_YELLOW);
            tg_label(s, buf, aos_font_small, AOS_C_TEXT);
            lv_obj_align(s, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        } else {
            lv_obj_t *l = tg_label(card, AOS_SYM_LOCK, &aos_sym_44, AOS_C_DIM);
            lv_obj_align(l, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        if (w == tg->world) sel = s_nitems;
        menu_add(card);
    }
    menu_done(sel);
}

/* ---------------------------------------------------------------- levels */

static void level_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    tg_audio_play(SND_TAP);
    tg->free_mode = false;
    tg->level = i;
    tg_go(SCR_PLAY);
}

static void back_worlds(lv_event_t *e)
{
    (void)e;
    tg_go(SCR_WORLDS);
}

void tg_menu_levels(lv_obj_t *scr)
{
    menu_reset();
    lv_obj_t *c = column(scr, true);
    char buf[96];
    snprintf(buf, sizeof buf, "%d  %s", tg->world, tg_world_name(tg->world));
    header(c, buf, back_worlds);
    lv_obj_t *idea = tg_label(c, tg_world_idea(tg->world), aos_font_body, AOS_C_DIM);
    (void)idea;
    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_column(grid, 14, 0);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    int per = tg->land ? 4 : 2;
    int cw = (tg->W - 40 - (per - 1) * 14) / per;
    int sel = 1;
    for (int i = 0; i < tg->levels.n; i++) {
        const tg_level_t *lv = &tg->levels.lv[i];
        if (lv->world != tg->world) continue;
        lv_obj_t *card = lv_button_create(grid);
        lv_obj_set_size(card, cw, 150);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x232a33), 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_add_event_cb(card, level_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *num = tg_label(card, lv->id, aos_font_title, lv_color_hex(0xF2C27A));
        lv_obj_align(num, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *t = tg_label(card, tg_level_text(lv->title, tg->tlang), aos_font_small, AOS_C_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, cw - 24);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 48);
        lv_obj_t *st = lv_obj_create(card);
        lv_obj_remove_style_all(st);
        lv_obj_set_size(st, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_remove_flag(st, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(st, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        tg_stars_draw(st, tg->score[i].stars[tg->prefs.lang], 28);
        /* the other languages it was won in: a dot of each one's colour */
        lv_obj_t *dots = lv_obj_create(card);
        lv_obj_remove_style_all(dots);
        lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(dots, 4, 0);
        lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dots, LV_ALIGN_TOP_RIGHT, 0, 6);
        for (int k = 0; k < TG_LANG_N; k++) {
            if (!tg->score[i].stars[k]) continue;
            lv_obj_t *d = lv_obj_create(dots);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, 14, 14);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, tg_lang_color(k), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        }
        if (tg->score[i].medal & (1u << tg->prefs.lang)) {
            lv_obj_t *m = tg_label(card, AOS_SYM_LOCK, &aos_sym_28, AOS_C_RED);
            lv_obj_align(m, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        }
        if (i == tg->level) sel = s_nitems;
        menu_add(card);
    }
    menu_done(sel);
}

bool tg_menu_back(void)
{
    switch (tg->screen) {
    case SCR_LEVELS: tg_go(SCR_WORLDS); return true;
    case SCR_WORLDS: tg_go(SCR_HOME); return true;
    case SCR_PICK:
        if (!tg->prefs.chosen) return false;
        tg_go(SCR_HOME);
        return true;
    default: return false;
    }
}
