/*
 * P4OS - selecting text, and the system's clipboard (2026-11-08).
 *
 * In every lv_textarea (the theme of aos_textfield.c hands them here):
 *
 *   - a hold opens a bubble over the field: Seleccionar, Todo, Pegar; with
 *     something selected, Cortar, Copiar, Pegar, Todo. A field in password
 *     mode does not give its text away: no Cortar nor Copiar there.
 *   - a double tap selects the word under the finger.
 *   - with something selected, the space bar's trackpad (aos_keyboard.c)
 *     moves its end instead of the cursor, a key typed replaces it,
 *     backspace deletes it; a tap in the field or leaving it lets it go.
 *   - a USB keyboard: Ctrl (or Cmd) + C, X, V, A, and Shift + arrows
 *     (aos_hwkbd.c).
 *
 * The selection is LVGL's label selection (LV_LABEL_TEXT_SELECTION), drawn
 * in the accent colour; one at a time, the field's that is being edited.
 * Indexes are characters, not bytes. The clipboard is plain text in PSRAM,
 * up to CLIP_MAX, shared by the system, the apps (aos_ui_clip_set/get) and
 * the portal (/api/clip, the Pantalla page), so text crosses from the Mac
 * to the board and back.
 */
#include "aos_internal.h"
#include "aos_theme.h"
#include "aos_i18n.h"

#include "lvgl.h"
#include "lvgl_private.h"           /* lv_textarea_t, lv_text_encoded_* */

#include <stdlib.h>
#include <string.h>

#define CLIP_MAX    (16 * 1024)

/* ---- the clipboard ---- */

static char  *s_clip;               /* PSRAM, NULL while empty */
static size_t s_clip_len;

void aos_ui_clip_set(const char *text)
{
    size_t n = text ? strlen(text) : 0;
    if (n > CLIP_MAX) n = CLIP_MAX;
    /* not in the middle of a UTF-8 character */
    while (n && n < strlen(text) && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    char *c = n ? malloc(n + 1) : NULL;
    if (n && !c) return;
    if (c) {
        memcpy(c, text, n);
        c[n] = 0;
    }
    free(s_clip);
    s_clip = c;
    s_clip_len = n;
}

size_t aos_ui_clip_get(char *out, size_t size)
{
    if (out && size) {
        size_t n = s_clip_len < size - 1 ? s_clip_len : size - 1;
        if (n) memcpy(out, s_clip, n);
        out[n] = 0;
    }
    return s_clip_len;
}

/* ---- the selection ---- */

static struct {
    lv_obj_t *ta;
    uint32_t  anchor, end;          /* characters; the end moves */
    bool      busy;                 /* our own edit: not the user's */
} S;

static lv_obj_t *s_menu;            /* the bubble's backdrop on lv_layer_top */
static lv_obj_t *s_menu_ta;

static uint32_t char_count(lv_obj_t *ta)
{
    return lv_text_get_encoded_length(lv_textarea_get_text(ta));
}

static void sel_draw(void)
{
    lv_obj_t *label = lv_textarea_get_label(S.ta);
    uint32_t lo = S.anchor < S.end ? S.anchor : S.end, hi = S.anchor < S.end ? S.end : S.anchor;
    if (lo == hi) {
        lv_label_set_text_selection_start(label, LV_LABEL_TEXT_SELECTION_OFF);
        lv_label_set_text_selection_end(label, LV_LABEL_TEXT_SELECTION_OFF);
        return;
    }
    lv_obj_set_style_bg_color(label, AOS_C_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(label, LV_OPA_60, LV_PART_SELECTED);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_SELECTED);
    lv_label_set_text_selection_start(label, lo);
    lv_label_set_text_selection_end(label, hi);
}

void aos_textsel_clear(void)
{
    if (S.ta && lv_obj_is_valid(S.ta)) {
        lv_obj_t *label = lv_textarea_get_label(S.ta);
        lv_label_set_text_selection_start(label, LV_LABEL_TEXT_SELECTION_OFF);
        lv_label_set_text_selection_end(label, LV_LABEL_TEXT_SELECTION_OFF);
    }
    memset(&S, 0, sizeof S);
}

static void sel_set(lv_obj_t *ta, uint32_t anchor, uint32_t end)
{
    if (S.ta && S.ta != ta) aos_textsel_clear();
    uint32_t n = char_count(ta);
    S.ta = ta;
    S.anchor = anchor > n ? n : anchor;
    S.end = end > n ? n : end;
    S.busy = true;
    lv_textarea_set_cursor_pos(ta, (int32_t)S.end);
    S.busy = false;
    sel_draw();
}

bool aos_textsel_has(lv_obj_t *ta)
{
    return ta && S.ta == ta && S.anchor != S.end;
}

/* The selected text, malloc'd, or NULL */
static char *sel_text(void)
{
    if (!S.ta || S.anchor == S.end) return NULL;
    const char *t = lv_textarea_get_text(S.ta);
    uint32_t lo = S.anchor < S.end ? S.anchor : S.end, hi = S.anchor < S.end ? S.end : S.anchor;
    uint32_t b0 = lv_text_encoded_get_byte_id(t, lo), b1 = lv_text_encoded_get_byte_id(t, hi);
    char *c = malloc(b1 - b0 + 1);
    if (!c) return NULL;
    memcpy(c, t + b0, b1 - b0);
    c[b1 - b0] = 0;
    return c;
}

bool aos_textsel_delete(lv_obj_t *ta)
{
    if (!aos_textsel_has(ta)) return false;
    const char *t = lv_textarea_get_text(ta);
    uint32_t lo = S.anchor < S.end ? S.anchor : S.end, hi = S.anchor < S.end ? S.end : S.anchor;
    uint32_t b0 = lv_text_encoded_get_byte_id(t, lo), b1 = lv_text_encoded_get_byte_id(t, hi);
    size_t len = strlen(t);
    char *n = malloc(len - (b1 - b0) + 1);
    if (!n) return false;
    memcpy(n, t, b0);
    memcpy(n + b0, t + b1, len - b1 + 1);
    aos_textsel_clear();
    S.busy = true;
    lv_textarea_set_text(ta, n);
    lv_textarea_set_cursor_pos(ta, (int32_t)lo);
    S.busy = false;
    free(n);
    return true;
}

void aos_textsel_extend(lv_obj_t *ta, int dx, int dy)
{
    if (!aos_textsel_has(ta)) {
        uint32_t c = lv_textarea_get_cursor_pos(ta);
        if (S.ta != ta) aos_textsel_clear();
        S.ta = ta;
        S.anchor = S.end = c;
    }
    S.busy = true;
    lv_textarea_set_cursor_pos(ta, (int32_t)S.end);
    for (; dx > 0; dx--) lv_textarea_cursor_right(ta);
    for (; dx < 0; dx++) lv_textarea_cursor_left(ta);
    for (; dy > 0; dy--) lv_textarea_cursor_down(ta);
    for (; dy < 0; dy++) lv_textarea_cursor_up(ta);
    S.end = lv_textarea_get_cursor_pos(ta);
    S.busy = false;
    sel_draw();
}

static bool is_sep(uint32_t c)
{
    return c <= ' ' || (c < 0x80 && strchr(".,;:!?()[]{}<>\"'/\\|=+*&^%$#@~`", (int)c));
}

static void select_word(lv_obj_t *ta)
{
    const char *t = lv_textarea_get_text(ta);
    uint32_t n = char_count(ta), cur = lv_textarea_get_cursor_pos(ta);
    if (!n) return;
    /* the characters, as code points, to look around the cursor */
    uint32_t *cp = malloc(n * sizeof *cp);
    if (!cp) return;
    uint32_t i = 0, b = 0;
    while (i < n && t[b]) cp[i++] = lv_text_encoded_next(t, &b);
    n = i;
    if (cur > n) cur = n;
    uint32_t lo = cur, hi = cur;
    if (lo == n || is_sep(cp[lo])) {
        if (lo > 0 && !is_sep(cp[lo - 1])) lo--, hi = lo + 1;   /* just after a word */
    }
    while (lo > 0 && !is_sep(cp[lo - 1])) lo--;
    while (hi < n && !is_sep(cp[hi])) hi++;
    free(cp);
    if (hi > lo) sel_set(ta, lo, hi);
}

/* ---- the operations ---- */

static void do_copy(lv_obj_t *ta, bool cut)
{
    if (lv_textarea_get_password_mode(ta)) return;
    char *c = sel_text();
    if (!c) return;
    aos_ui_clip_set(c);
    free(c);
    if (cut) aos_textsel_delete(ta);
}

static void do_paste(lv_obj_t *ta)
{
    if (!s_clip_len) return;
    char *c = malloc(s_clip_len + 1);
    if (!c) return;
    memcpy(c, s_clip, s_clip_len + 1);
    if (lv_textarea_get_one_line(ta))           /* a line break would be Enter */
        for (char *p = c; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    aos_textsel_delete(ta);
    S.busy = true;
    lv_textarea_add_text(ta, c);
    S.busy = false;
    free(c);
}

static void do_all(lv_obj_t *ta) { sel_set(ta, 0, char_count(ta)); }

void aos_textsel_key(lv_obj_t *ta, char k)
{
    if (!ta) return;
    switch (k) {
    case 'c': do_copy(ta, false); break;
    case 'x': do_copy(ta, true); break;
    case 'v': do_paste(ta); break;
    case 'a': do_all(ta); break;
    default: break;
    }
}

bool aos_ui_clip_paste_here(void)
{
    lv_obj_t *ta = aos_textfield_active();
    if (!ta || !s_clip_len) return false;
    do_paste(ta);
    return true;
}

/* ---- the bubble ---- */

static void menu_close(void)
{
    if (s_menu) lv_obj_delete(s_menu);
    s_menu = NULL;
    s_menu_ta = NULL;
}

enum { M_CUT, M_COPY, M_PASTE, M_SELECT, M_ALL };

static void menu_item_cb(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *ta = s_menu_ta;
    menu_close();
    if (!ta || !lv_obj_is_valid(ta)) return;
    switch (what) {
    case M_CUT: do_copy(ta, true); break;
    case M_COPY: do_copy(ta, false); break;
    case M_PASTE: do_paste(ta); break;
    case M_SELECT: select_word(ta); break;
    case M_ALL: do_all(ta); break;
    default: break;
    }
}

static void backdrop_cb(lv_event_t *e)
{
    (void)e;
    menu_close();
}

static void menu_add(lv_obj_t *row, const char *text, int what)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 84);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, menu_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    /* a press on it must not take the focus from the field: that would
     * let its selection go before the click */
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

static void menu_open(lv_obj_t *ta)
{
    menu_close();
    bool sel = aos_textsel_has(ta), pwd = lv_textarea_get_password_mode(ta);
    bool text = char_count(ta) > 0, clip = s_clip_len > 0;
    if (!sel && !text && !clip) return;

    const aos_geo_t *g = aos_ui_geo();
    s_menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, g->w, g->h);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_menu, backdrop_cb, LV_EVENT_PRESSED, NULL);
    s_menu_ta = ta;

    lv_obj_t *row = lv_obj_create(s_menu);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_set_style_pad_column(row, 2, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_shadow_width(row, 30, 0);
    lv_obj_set_style_shadow_opa(row, LV_OPA_60, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);    /* a press on it is not the backdrop's */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (sel && !pwd) menu_add(row, _("Cortar"), M_CUT);
    if (sel && !pwd) menu_add(row, _("Copiar"), M_COPY);
    if (!sel && text) menu_add(row, _("Seleccionar"), M_SELECT);
    if (clip) menu_add(row, _("Pegar"), M_PASTE);
    if (text) menu_add(row, _("Todo"), M_ALL);
    lv_obj_update_layout(row);

    /* over the cursor's line, or under the field when there is no room */
    lv_textarea_t *t = (lv_textarea_t *)ta;
    lv_area_t a;
    lv_obj_get_coords(ta, &a);
    int32_t cy = a.y1 + t->cursor.area.y1 - lv_obj_get_scroll_y(ta);
    int32_t cx = a.x1 + t->cursor.area.x1 - lv_obj_get_scroll_x(ta);
    if (cy < a.y1) cy = a.y1;
    int32_t w = lv_obj_get_width(row), h = lv_obj_get_height(row);
    int32_t x = cx - w / 2, y = cy - h - 14;
    if (y < g->bar_h) y = (t->cursor.area.y2 - lv_obj_get_scroll_y(ta)) + a.y1 + 14;
    if (x + w > g->w - 8) x = g->w - 8 - w;
    if (x < 8) x = 8;
    lv_obj_set_pos(row, x, y);
}

/* ---- the field's events ---- */

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_current_target(e);
    switch (code) {
    case LV_EVENT_PRESSED:
        if (S.ta == ta && !S.busy) aos_textsel_clear();
        break;
    case LV_EVENT_LONG_PRESSED:
        menu_open(ta);
        break;
    case LV_EVENT_DOUBLE_CLICKED:
        select_word(ta);
        menu_open(ta);
        break;
    case LV_EVENT_VALUE_CHANGED:
        if (S.ta == ta && !S.busy) aos_textsel_clear();
        break;
    case LV_EVENT_DEFOCUSED:
        /* the field of a shown keyboard keeps its focus (aos_textfield.c),
         * so this is the field really left */
        if (S.ta == ta && s_menu_ta != ta) aos_textsel_clear();
        break;
    case LV_EVENT_DELETE:
        if (S.ta == ta) memset(&S, 0, sizeof S);
        if (s_menu_ta == ta) menu_close();
        break;
    default:
        break;
    }
}

void aos_textsel_attach(lv_obj_t *ta)
{
    lv_obj_add_event_cb(ta, ta_cb, LV_EVENT_ALL, NULL);
}
