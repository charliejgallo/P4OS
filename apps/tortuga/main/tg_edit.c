/*
 * Tortuga - the text editor (DESIGN.md, 6.3). A textarea in the mono font
 * with the line numbers in its left padding and a bar under the line that
 * runs (or failed). Enter keeps the indentation and adds a step after "[",
 * "do", "then" or ":". The palette writes what the level allows, already
 * indented, and leaves the cursor where the next thing goes.
 */
#include "tg_edit.h"

#include "aos_fonts.h"

#include <stdio.h>
#include <string.h>

#define GUTTER 52

static struct {
    lv_obj_t *box, *ta, *nums, *bar, *pal, *kb;
    int lang;
    const tg_level_t *lv;
    int world;
    void (*focus)(bool);
    bool raw;               /* the palette is writing: no auto-indent */
    bool shown;
    int lines;
    char ins[96];
} E;

static const char *unit(void)
{
    return E.lang == TG_LANG_PY ? "    " : "  ";
}

static bool en(void)
{
    return tg->tlang != TG_ES;
}

/* ------------------------------------------------------------- lines */

static int count_lines(const char *t)
{
    int n = 1;
    for (; *t; t++) n += *t == '\n';
    return n;
}

static void numbers(void)
{
    const char *t = lv_textarea_get_text(E.ta);
    int n = count_lines(t);
    if (n == E.lines) return;
    E.lines = n;
    static char buf[1200];
    int k = 0;
    for (int i = 1; i <= n && k < (int)sizeof(buf) - 8; i++) k += snprintf(buf + k, sizeof(buf) - (size_t)k, "%d\n", i);
    if (k) buf[k - 1] = 0;
    lv_label_set_text(E.nums, buf);
}

/* the letter index (not the byte) where a line starts */
static int line_letter(const char *t, int line)
{
    int l = 1, letters = 0;
    for (const char *p = t; *p && l < line; p++) {
        if (*p == '\n') l++;
        if (((unsigned char)*p & 0xC0) != 0x80) letters++;
    }
    return letters;
}

void tg_edit_mark(int line, bool error)
{
    if (!E.ta) return;
    if (line <= 0) {
        lv_obj_add_flag(E.bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *t = lv_textarea_get_text(E.ta);
    if (line > count_lines(t)) line = count_lines(t);
    lv_obj_t *label = lv_textarea_get_label(E.ta);
    lv_point_t p;
    lv_label_get_letter_pos(label, (uint32_t)line_letter(t, line), &p);
    int lh = lv_font_get_line_height(&aos_mono_22) + 4;
    lv_obj_set_style_bg_color(E.bar, error ? lv_color_hex(0xa8322d) : lv_color_hex(0x8a5a12), 0);
    lv_obj_set_pos(E.bar, -GUTTER + 4, lv_obj_get_y(label) + p.y - 2);
    lv_obj_set_size(E.bar, lv_obj_get_width(E.ta) - 12, lh);
    lv_obj_remove_flag(E.bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(E.bar, 0);
    lv_obj_scroll_to_y(E.ta, p.y > 80 ? p.y - 80 : 0, LV_ANIM_ON);
}

/* ------------------------------------------------------------ typing */

/* the indentation of the line the cursor is on, and whether it opens a block */
static void current_indent(char *out, size_t n, bool *opens)
{
    const char *t = lv_textarea_get_text(E.ta);
    uint32_t cur = lv_textarea_get_cursor_pos(E.ta);
    /* the byte of the cursor */
    const char *p = t;
    for (uint32_t i = 0; *p && i < cur; i++) {
        p++;
        while (((unsigned char)*p & 0xC0) == 0x80) p++;
    }
    const char *start = p;
    while (start > t && start[-1] != '\n') start--;
    size_t k = 0;
    while ((start[k] == ' ' || start[k] == '\t') && k < n - 1) {
        out[k] = start[k];
        k++;
    }
    out[k] = 0;
    /* what ends the line before the cursor */
    const char *e = p;
    while (e > start && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *opens = false;
    if (e > start) {
        char c = e[-1];
        if (c == '[' || c == ':') *opens = true;
        size_t len = (size_t)(e - start);
        if (len >= 2 && !strncmp(e - 2, "do", 2) && (len == 2 || e[-3] == ' ')) *opens = true;
        if (len >= 4 && !strncmp(e - 4, "then", 4)) *opens = true;
        if (len >= 4 && !strncmp(start + k, "para", 4)) *opens = true;
        if (len >= 2 && !strncmp(start + k, "to ", 3)) *opens = true;
        if (len >= 8 && !strncmp(start + k, "function", 8)) *opens = true;
    }
}

static void insert_cb(lv_event_t *e)
{
    if (E.raw) return;
    const char *s = lv_event_get_param(e);
    if (!s || strcmp(s, "\n")) return;
    char ind[40];
    bool opens;
    current_indent(ind, sizeof ind, &opens);
    snprintf(E.ins, sizeof E.ins, "\n%s%s", ind, opens ? unit() : "");
    lv_textarea_set_insert_replace(E.ta, E.ins);
}

static void changed_cb(lv_event_t *e)
{
    (void)e;
    numbers();
    lv_obj_add_flag(E.bar, LV_OBJ_FLAG_HIDDEN);
}

/* ----------------------------------------------------------- keyboard */

int tg_edit_keyboard_height(void)
{
    return tg->land ? tg->H * 52 / 100 : tg->H * 36 / 100;
}

void tg_edit_keyboard(bool show)
{
    if (!E.kb || show == E.shown) return;
    E.shown = show;
    if (show) {
        lv_obj_remove_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(E.kb, E.ta);
        lv_obj_add_state(E.ta, LV_STATE_FOCUSED);
    } else {
        lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(E.kb, NULL);
        lv_obj_remove_state(E.ta, LV_STATE_FOCUSED);
    }
    if (E.focus) E.focus(show);
}

static void ta_click(lv_event_t *e)
{
    (void)e;
    tg_edit_keyboard(true);
}

static void kb_done(lv_event_t *e)
{
    (void)e;
    tg_edit_keyboard(false);
}

/* ------------------------------------------------------------ palette */

/* writes a template: "\n" keeps the indentation, "\t" is one step in, and
 * "|" is where the cursor stays */
static void write_template(const char *tpl)
{
    char ind[40];
    bool opens;
    current_indent(ind, sizeof ind, &opens);
    static char out[256];
    int k = 0, cursor = -1;
    for (const char *p = tpl; *p && k < (int)sizeof(out) - 48; p++) {
        if (*p == '\n') {
            k += snprintf(out + k, sizeof(out) - (size_t)k, "\n%s", ind);
        } else if (*p == '\t') {
            k += snprintf(out + k, sizeof(out) - (size_t)k, "%s", unit());
        } else if (*p == '|') {
            cursor = k;
        } else {
            out[k++] = *p;
        }
    }
    out[k] = 0;
    uint32_t start = lv_textarea_get_cursor_pos(E.ta);
    E.raw = true;
    lv_textarea_add_text(E.ta, out);
    E.raw = false;
    if (cursor >= 0) lv_textarea_set_cursor_pos(E.ta, (int32_t)(start + (uint32_t)cursor));
    tg_audio_play(SND_TAP);
}

static void pal_cb(lv_event_t *e)
{
    const char *tpl = lv_event_get_user_data(e);
    write_template(tpl);
}

static void chip(const char *text, const char *tpl, uint32_t color)
{
    lv_obj_t *b = lv_button_create(E.pal);
    lv_obj_set_height(b, 52);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_t *l = tg_label(b, text, &aos_mono_22, lv_color_hex(0x10140c));
    lv_obj_center(l);
    lv_obj_add_event_cb(b, pal_cb, LV_EVENT_CLICKED, (void *)tpl);
}

static bool allowed(int o)
{
    return !E.lv->orders || (E.lv->orders & (1u << o));
}

/* the templates live as long as the editor: built once per open */
static char s_tpl[48][64];
static int s_ntpl;

static const char *keep(const char *fmt, const char *a)
{
    if (s_ntpl >= 48) return "";
    snprintf(s_tpl[s_ntpl], sizeof s_tpl[0], fmt, a);
    return s_tpl[s_ntpl++];
}

static void build_palette(void)
{
    s_ntpl = 0;
    bool logo = E.lang == TG_LANG_LOGO, py = E.lang == TG_LANG_PY;
    for (int o = 0; o < TG_O_N; o++) {
        if (!allowed(o)) continue;
        const tg_order_t *t = &tg_orders[o];
        if (t->sense) continue;
        const char *name = logo ? (en() ? t->logo_en : t->logo_es) : (en() ? t->en : t->es);
        const char *tpl;
        if (logo) {
            if (o == TG_O_FORWARD || o == TG_O_BACK) tpl = keep("%s |1", name);
            else if (o == TG_O_SAY) tpl = keep("%s |", name);
            else if (o == TG_O_COLOR) tpl = keep("%s \"|rojo\"", name);
            else tpl = keep("%s|", name);
        } else {
            if (o == TG_O_FORWARD || o == TG_O_BACK) tpl = keep("%s(|1)", name);
            else if (o == TG_O_SAY || o == TG_O_COLOR) tpl = keep("%s(|)", name);
            else tpl = keep("%s()|", name);
        }
        chip(name, tpl, o >= TG_O_PENDOWN ? 0xff8fb8 : 0x34c759);
    }
    int w = E.world;
    if (w >= 2) {
        const char *r = logo ? (en() ? "repeat |4 [\n\t\n]" : "repetir |4 [\n\t\n]")
                             : py ? "for _ in range(|4):\n\t" : "for i = 1, |4 do\n\t\nend";
        chip(logo ? (en() ? "repeat" : "repetir") : "for", r, 0xff9f0a);
    }
    if (w >= 3) {
        const char *t = logo ? (en() ? "to |name\n\t\nend" : "para |nombre\n\t\nfin")
                             : py ? "def |nombre():\n\t" : "function |nombre()\n\t\nend";
        chip(logo ? (en() ? "to" : "para") : py ? "def" : "function", t, 0x64b5ff);
    }
    if (w >= 4) {
        const char *t = logo ? (en() ? "if | [\n\t\n]" : "si | [\n\t\n]") : py ? "if |:\n\t" : "if | then\n\t\nend";
        chip(logo ? (en() ? "if" : "si") : "if", t, 0xbf5af2);
        const char *el = logo ? (en() ? " else [\n\t|\n]" : " sino [\n\t|\n]") : py ? "else:\n\t|" : "else\n\t|";
        chip(logo ? (en() ? "else" : "sino") : "else", el, 0xbf5af2);
    }
    if (w >= 5) {
        const char *t = logo ? (en() ? "while | [\n\t\n]" : "mientras | [\n\t\n]") : py ? "while |:\n\t" : "while | do\n\t\nend";
        chip(logo ? (en() ? "while" : "mientras") : "while", t, 0xbf5af2);
        chip(logo ? (en() ? "not" : "no") : "not", logo ? (en() ? "not |" : "no |") : "not |", 0xbf5af2);
    }
    for (int o = 0; o < TG_O_N; o++) {
        const tg_order_t *t = &tg_orders[o];
        if (!t->sense || !allowed(o)) continue;
        const char *name = logo ? (en() ? t->logo_en : t->logo_es) : (en() ? t->en : t->es);
        chip(name, logo ? keep("%s|", name) : keep("%s()|", name), 0xd9b8ff);
    }
    if (w >= 6) {
        chip(en() ? "count = 0" : "cuenta = 0", en() ? "count = |0" : "cuenta = |0", 0xffd60a);
        chip("+= 1", " += |1", 0xffd60a);
    }
    static const char *const SYM[] = { "[", "]", "(", ")", "=", "+", "-", ":", "\"", NULL };
    for (int i = 0; SYM[i]; i++) {
        if (logo && (SYM[i][0] == '(' || SYM[i][0] == ')' || SYM[i][0] == ':')) continue;
        if (!logo && (SYM[i][0] == '[' || SYM[i][0] == ']')) continue;
        chip(SYM[i], keep("%s|", SYM[i]), 0xc7ccd4);
    }
    chip("tab", keep("%s|", unit()), 0xc7ccd4);
}

/* ------------------------------------------------------------- setup */

void tg_edit_open(lv_obj_t *scr, int lang, const tg_level_t *lv, int world, const char *text,
                  void (*focus)(bool editing))
{
    tg_edit_close();
    E.lang = lang;
    E.lv = lv;
    E.world = world;
    E.focus = focus;
    E.lines = -1;

    E.box = lv_obj_create(scr);
    lv_obj_remove_style_all(E.box);
    lv_obj_set_flex_flow(E.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(E.box, 8, 0);
    lv_obj_remove_flag(E.box, LV_OBJ_FLAG_SCROLLABLE);

    E.ta = lv_textarea_create(E.box);
    lv_obj_set_width(E.ta, LV_PCT(100));
    lv_obj_set_flex_grow(E.ta, 1);
    lv_obj_set_style_text_font(E.ta, &aos_mono_22, 0);
    lv_obj_set_style_text_line_space(E.ta, 4, 0);
    lv_obj_set_style_bg_color(E.ta, lv_color_hex(0x1b2027), 0);
    lv_obj_set_style_border_width(E.ta, 0, 0);
    lv_obj_set_style_radius(E.ta, 16, 0);
    lv_obj_set_style_pad_left(E.ta, GUTTER, 0);
    lv_obj_set_style_pad_top(E.ta, 12, 0);
    lv_obj_set_style_text_color(E.ta, AOS_C_TEXT, 0);
    lv_textarea_set_placeholder_text(E.ta, lang == TG_LANG_LOGO
                                               ? _("Escribí el programa, o tocá las órdenes de abajo")
                                               : _("Escribí el programa en Lua, o tocá las órdenes de abajo"));
    lv_textarea_set_text(E.ta, text);
    lv_textarea_set_cursor_pos(E.ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(E.ta, insert_cb, LV_EVENT_INSERT, NULL);
    lv_obj_add_event_cb(E.ta, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(E.ta, ta_click, LV_EVENT_CLICKED, NULL);

    E.bar = lv_obj_create(E.ta);
    lv_obj_remove_style_all(E.bar);
    lv_obj_set_style_bg_opa(E.bar, LV_OPA_70, 0);
    lv_obj_set_style_radius(E.bar, 6, 0);
    lv_obj_remove_flag(E.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(E.bar, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);

    E.nums = lv_label_create(E.ta);
    lv_obj_set_style_text_font(E.nums, &aos_mono_22, 0);
    lv_obj_set_style_text_line_space(E.nums, 4, 0);
    lv_obj_set_style_text_color(E.nums, lv_color_hex(0x5d6673), 0);
    lv_obj_set_style_text_align(E.nums, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(E.nums, GUTTER - 14);
    lv_obj_add_flag(E.nums, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(E.nums, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(E.nums, -GUTTER + 2, 0);
    numbers();

    E.pal = lv_obj_create(E.box);
    lv_obj_remove_style_all(E.pal);
    lv_obj_set_size(E.pal, LV_PCT(100), 124);
    lv_obj_set_flex_flow(E.pal, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(E.pal, 8, 0);
    lv_obj_set_style_pad_column(E.pal, 8, 0);
    lv_obj_add_flag(E.pal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(E.pal, LV_DIR_VER);
    build_palette();

    E.kb = lv_keyboard_create(scr);
    lv_obj_set_size(E.kb, tg->W, tg_edit_keyboard_height());
    lv_obj_align(E.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(E.kb, kb_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(E.kb, kb_done, LV_EVENT_CANCEL, NULL);
}

void tg_edit_place(int x, int y, int w, int h)
{
    if (!E.box) return;
    lv_obj_set_pos(E.box, x, y);
    lv_obj_set_size(E.box, w, h);
    lv_obj_set_height(E.pal, h > 420 ? 124 : 60);
}

const char *tg_edit_text(void)
{
    return E.ta ? lv_textarea_get_text(E.ta) : "";
}

void tg_edit_close(void)
{
    if (E.kb) lv_obj_delete(E.kb);
    if (E.box) lv_obj_delete(E.box);
    memset(&E, 0, sizeof E);
}
