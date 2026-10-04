/*
 * P4OS - Notes: a task list.
 *
 * The same document as a note, every block a BT_CHECK (style = priority),
 * drawn as rows: a round box, the text, a handle to drag it by. Ticked
 * tasks sink to a "Completed" section that folds away; ticking a task ticks
 * the ones indented under it. Rows are LVGL objects rebuilt on every change
 * - a list has tens of tasks, not thousands, and a rebuild is simpler than
 * keeping rows and blocks in step.
 *
 * Text is edited in place, in a textarea the app's keyboard types into;
 * Enter on a new task opens the next one, so a list is typed in one go.
 */
#include "nt.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

static struct {
    nt_doc_t   *doc;
    nt_meta_t  *meta;
    nt_tasks_cbs_t cbs;
    lv_obj_t   *list;
    lv_obj_t   *edit_ta;
    int         edit_blk;          /* block being edited, -1 none */
    bool        edit_new;          /* it is a task being added */
    bool        edit_title;
    /* drag */
    lv_obj_t   *ghost, *line;
    int         drag_blk;
    int32_t     drag_y0, drag_top;
    int         drop_at;
    int32_t     W;
} T;

static void rebuild(void);

static bool is_task(int i)
{
    return i > 0 && i < T.doc->n && T.doc->b[i].type == BT_CHECK;
}

/* Everything that is not a task becomes one; empty ones go. */
static void normalise(void)
{
    nt_doc_t *d = T.doc;
    for (int i = d->n - 1; i >= 1; i--) {
        nt_blk_t *b = &d->b[i];
        if (i == T.edit_blk) continue;
        if (b->type == BT_RULE || b->len == 0) {
            nt_doc_remove(d, i);
            if (i < T.edit_blk) T.edit_blk--;
            continue;
        }
        if (b->type != BT_CHECK) { b->type = BT_CHECK; b->style = 0; b->checked = 0; }
    }
}

static void changed(void)
{
    if (T.cbs.changed) T.cbs.changed();
}

/* -------------------------------------------------------------------------- */
/* Editing a task's text                                                       */
/* -------------------------------------------------------------------------- */

static void end_edit(bool keep)
{
    if (!T.edit_ta) return;
    const char *txt = lv_textarea_get_text(T.edit_ta);
    int bi = T.edit_blk;
    bool was_new = T.edit_new, title = T.edit_title;
    T.edit_ta = NULL;
    T.edit_blk = -1;
    T.edit_new = false;
    T.edit_title = false;
    if (title) {
        if (keep) {
            nt_blk_set(&T.doc->b[0], txt, 0);
            changed();
        }
    } else if (bi > 0 && bi < T.doc->n) {
        if (keep && txt[0]) {
            nt_blk_t *b = &T.doc->b[bi];
            if (strcmp(b->txt, txt) != 0) {
                nt_blk_set(b, txt, 0);
                changed();
            }
        } else if (was_new || !txt[0]) {
            nt_doc_remove(T.doc, bi);
            if (!was_new) changed();
        }
    }
    nt_kb_attach(NULL);
    if (T.cbs.keyboard) T.cbs.keyboard(false);
}

void nt_tasks_commit(void)
{
    if (T.edit_ta) {
        end_edit(true);
        rebuild();
    }
}

void nt_tasks_sync(void)
{
    if (!T.edit_ta) return;
    const char *txt = lv_textarea_get_text(T.edit_ta);
    if (T.edit_title) nt_blk_set(&T.doc->b[0], txt, 0);
    else if (T.edit_blk > 0 && T.edit_blk < T.doc->n) nt_blk_set(&T.doc->b[T.edit_blk], txt, 0);
}

bool nt_tasks_back(void)
{
    if (!T.edit_ta) return false;
    end_edit(true);
    rebuild();
    return true;
}

static void start_edit(int bi, bool fresh);

static void ta_done(lv_obj_t *ta)
{
    (void)ta;
    bool fresh = T.edit_new;
    int bi = T.edit_blk;
    bool title = T.edit_title;
    const char *txt = T.edit_ta ? lv_textarea_get_text(T.edit_ta) : "";
    bool empty = !txt[0];
    end_edit(true);
    if (title) {
        /* from the title straight to the first task, if there is none */
        bool any = false;
        for (int i = 1; i < T.doc->n; i++) any |= is_task(i);
        if (!any) { start_edit(T.doc->n, true); return; }
    } else if (fresh && !empty) {
        /* the next one, right below */
        start_edit(bi + 1, true);
        return;
    }
    rebuild();
}

static void start_edit(int bi, bool fresh)
{
    if (T.edit_ta) end_edit(true);
    if (fresh) {
        int indent = (bi - 1 >= 1 && bi - 1 < T.doc->n) ? T.doc->b[bi - 1].indent : 0;
        nt_blk_t *b = nt_doc_insert(T.doc, bi, BT_CHECK);
        if (!b) return;
        b->indent = (uint8_t)indent;
    }
    T.edit_blk = bi;
    T.edit_new = fresh;
    T.edit_title = false;
    rebuild();
}

static void edit_title(void)
{
    if (T.edit_ta) end_edit(true);
    T.edit_title = true;
    T.edit_blk = 0;
    rebuild();
}

/* -------------------------------------------------------------------------- */
/* Rows                                                                        */
/* -------------------------------------------------------------------------- */

static void box_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    int bi = (int)(intptr_t)lv_obj_get_user_data(o);
    if (!is_task(bi)) return;
    nt_blk_t *b = &T.doc->b[bi];
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t s = 46;
    int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    lv_area_t r = { cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2 };
    lv_color_t ring = b->style ? nt_prio_color(b->style) : NT.dim;
    nt_draw_checkbox(lv_event_get_layer(e), &r, b->checked, true, ring, b->style ? ring : NT.accent);
}

static void check_children(int bi, bool on)
{
    int ind = T.doc->b[bi].indent;
    for (int i = bi + 1; i < T.doc->n && T.doc->b[i].indent > ind; i++) T.doc->b[i].checked = on;
}

static void box_cb(lv_event_t *e)
{
    int bi = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (!is_task(bi)) return;
    if (T.edit_ta) end_edit(true);
    if (!is_task(bi)) { rebuild(); return; }
    nt_blk_t *b = &T.doc->b[bi];
    b->checked ^= 1;
    check_children(bi, b->checked);
    aos_hal_beep(b->checked ? 1800 : 1200, 15);
    changed();
    rebuild();
}

static void text_cb(lv_event_t *e)
{
    int bi = (int)(intptr_t)lv_event_get_user_data(e);
    start_edit(bi, false);
}

/* Long press: what else can be done with a task. */
static int s_menu_blk;

static void prio_pick(int i, void *ud)
{
    (void)ud;
    if (!is_task(s_menu_blk)) return;
    T.doc->b[s_menu_blk].style = (uint8_t)i;
    changed();
    rebuild();
}

static void row_menu_pick(int i, void *ud)
{
    (void)ud;
    int bi = s_menu_blk;
    if (!is_task(bi)) return;
    nt_blk_t *b = &T.doc->b[bi];
    switch (i) {
    case 0: start_edit(bi, false); return;
    case 1: {
        nt_item_t it[4] = {
            { _("Sin prioridad"), -1, false, b->style == 0 },
            { _("Baja"), -1, false, b->style == 1 },
            { _("Media"), -1, false, b->style == 2 },
            { _("Alta"), -1, false, b->style == 3 },
        };
        nt_sheet(_("Prioridad"), it, 4, prio_pick, NULL);
        return;
    }
    case 2: if (b->indent < NT_MAX_INDENT) b->indent++; break;
    case 3: if (b->indent > 0) b->indent--; break;
    case 4: {
        nt_blk_t *nb = nt_doc_insert(T.doc, bi + 1, BT_CHECK);
        b = &T.doc->b[bi];
        if (nb) {
            nb->indent = b->indent;
            nb->style = b->style;
            nt_blk_set(nb, b->txt, 0);
        }
        break;
    }
    case 5: nt_doc_remove(T.doc, bi); break;
    default: return;
    }
    changed();
    rebuild();
}

static void row_long_cb(lv_event_t *e)
{
    int bi = (int)(intptr_t)lv_event_get_user_data(e);
    if (!is_task(bi)) return;
    if (T.edit_ta) { end_edit(true); rebuild(); if (!is_task(bi)) return; }
    s_menu_blk = bi;
    nt_blk_t *b = &T.doc->b[bi];
    nt_item_t it[6] = {
        { _("Editar"), NT_IC_TYPE, false, false },
        { _("Prioridad"), NT_IC_PIN, false, false },
        { _("Más adentro"), NT_IC_INDENT, false, false },
        { _("Más afuera"), NT_IC_OUTDENT, false, false },
        { _("Duplicar"), NT_IC_COPY, false, false },
        { _("Borrar"), NT_IC_TRASH, true, false },
    };
    lv_indev_wait_release(lv_indev_active());
    nt_sheet(b->txt, it, 6, row_menu_pick, NULL);
}

/* Dragging by the handle */

static int32_t row_top_of(lv_obj_t *row)
{
    lv_area_t a;
    lv_obj_get_coords(row, &a);
    return a.y1;
}

static void handle_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *h = lv_event_get_target(e);
    lv_obj_t *row = lv_obj_get_parent(h);
    int bi = (int)(intptr_t)lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (code == LV_EVENT_PRESSED) {
        if (T.edit_ta) return;
        T.drag_blk = bi;
        T.drag_y0 = p.y;
        T.drag_top = row_top_of(row);
        T.drop_at = bi;
        lv_obj_remove_flag(T.list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(row, LV_OPA_30, 0);
        /* a copy of the row floats with the finger */
        lv_obj_t *parent = lv_obj_get_parent(T.list);
        lv_area_t pa, ra;
        lv_obj_get_coords(parent, &pa);
        lv_obj_get_coords(row, &ra);
        T.ghost = lv_obj_create(parent);
        lv_obj_remove_style_all(T.ghost);
        lv_obj_add_flag(T.ghost, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(T.ghost, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(T.ghost, lv_area_get_width(&ra), lv_area_get_height(&ra));
        lv_obj_set_pos(T.ghost, ra.x1 - pa.x1, ra.y1 - pa.y1);
        lv_obj_set_style_bg_color(T.ghost, NT.surface2, 0);
        lv_obj_set_style_bg_opa(T.ghost, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(T.ghost, 18, 0);
        lv_obj_set_style_shadow_width(T.ghost, 40, 0);
        lv_obj_set_style_shadow_opa(T.ghost, LV_OPA_50, 0);
        lv_obj_t *l = nt_text(T.ghost, T.doc->b[bi].txt, nt_font(0, NT_SZ_M), NT.text);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, lv_area_get_width(&ra) - 140);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 96 + T.doc->b[bi].indent * 48, 0);
        T.line = lv_obj_create(T.list);
        lv_obj_remove_style_all(T.line);
        lv_obj_add_flag(T.line, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(T.line, T.W - 48, 4);
        lv_obj_set_style_bg_color(T.line, NT.accent, 0);
        lv_obj_set_style_bg_opa(T.line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(T.line, 2, 0);
        lv_obj_add_flag(T.line, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_PRESSING && T.ghost) {
        lv_obj_t *parent = lv_obj_get_parent(T.list);
        lv_area_t pa;
        lv_obj_get_coords(parent, &pa);
        int32_t top = T.drag_top + (p.y - T.drag_y0);
        lv_obj_set_y(T.ghost, top - pa.y1);
        /* auto-scroll at the edges */
        lv_area_t la;
        lv_obj_get_coords(T.list, &la);
        if (p.y < la.y1 + 80) lv_obj_scroll_by_bounded(T.list, 0, 20, LV_ANIM_OFF);
        else if (p.y > la.y2 - 80) lv_obj_scroll_by_bounded(T.list, 0, -20, LV_ANIM_OFF);
        /* the drop point: before the first pending row whose middle is below the finger */
        T.drop_at = -1;
        int32_t line_y = 0;
        uint32_t n = lv_obj_get_child_count(T.list);
        for (uint32_t k = 0; k < n; k++) {
            lv_obj_t *c = lv_obj_get_child(T.list, (int32_t)k);
            if (!lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) continue;     /* pending rows only */
            lv_area_t ca;
            lv_obj_get_coords(c, &ca);
            int b = (int)(intptr_t)lv_obj_get_user_data(c);
            if (p.y < (ca.y1 + ca.y2) / 2) { T.drop_at = b; line_y = ca.y1; break; }
            T.drop_at = b + 1;
            line_y = ca.y2;
        }
        if (T.drop_at >= 0) {
            lv_obj_remove_flag(T.line, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(T.line, 24, line_y - la.y1 + lv_obj_get_scroll_y(T.list) - 2);
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && T.ghost) {
        lv_obj_delete(T.ghost);
        T.ghost = NULL;
        if (T.line) { lv_obj_delete(T.line); T.line = NULL; }
        lv_obj_add_flag(T.list, LV_OBJ_FLAG_SCROLLABLE);
        int from = T.drag_blk, to = T.drop_at;
        if (to >= 1 && to != from && to != from + 1 && is_task(from)) {
            /* the task and the ones indented under it move together */
            int ind = T.doc->b[from].indent, n = 1;
            while (from + n < T.doc->n && T.doc->b[from + n].indent > ind) n++;
            if (!(to > from && to <= from + n)) {
                nt_blk_t *tmp = nt_alloc(sizeof(nt_blk_t) * n);
                if (tmp) {
                    memcpy(tmp, &T.doc->b[from], sizeof(nt_blk_t) * n);
                    memmove(&T.doc->b[from], &T.doc->b[from + n], sizeof(nt_blk_t) * (T.doc->n - from - n));
                    int dest = to > from ? to - n : to;
                    memmove(&T.doc->b[dest + n], &T.doc->b[dest], sizeof(nt_blk_t) * (T.doc->n - n - dest));
                    memcpy(&T.doc->b[dest], tmp, sizeof(nt_blk_t) * n);
                    nt_free(tmp);
                    changed();
                }
            }
        }
        rebuild();
    }
}

static lv_obj_t *add_row(int bi)
{
    nt_blk_t *b = &T.doc->b[bi];
    lv_obj_t *row = lv_obj_create(T.list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, T.W);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 84, 0);
    lv_obj_set_style_pad_left(row, 16 + b->indent * 48, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void *)(intptr_t)bi);
    if (!b->checked) lv_obj_add_flag(row, LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(row, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)bi);

    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 72, 64);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(box, (void *)(intptr_t)bi);
    lv_obj_add_event_cb(box, box_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(box, box_cb, LV_EVENT_CLICKED, NULL);

    int32_t tw = T.W - 16 - b->indent * 48 - 72 - 72 - 32;
    if (bi == T.edit_blk && !T.edit_title) {
        lv_obj_t *ta = lv_textarea_create(row);
        lv_obj_set_width(ta, tw + 72);
        lv_obj_set_height(ta, LV_SIZE_CONTENT);
        lv_textarea_set_one_line(ta, false);
        lv_textarea_set_text(ta, b->txt);
        lv_textarea_set_placeholder_text(ta, _("Nueva tarea"));
        lv_obj_set_style_text_font(ta, nt_font(0, NT_SZ_M), 0);
        lv_obj_set_style_text_color(ta, NT.text, 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_pad_all(ta, 4, 0);
        lv_obj_set_style_bg_color(ta, NT.accent, LV_PART_CURSOR);
        lv_obj_set_style_border_color(ta, NT.accent, LV_PART_CURSOR);
        lv_obj_set_style_text_color(ta, NT.dim, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_add_state(ta, LV_STATE_FOCUSED);
        lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        T.edit_ta = ta;
        nt_kb_attach_textarea(ta, ta_done);
        if (T.cbs.keyboard) T.cbs.keyboard(true);
        return row;
    }
    lv_obj_t *l = nt_text(row, b->txt, nt_font(0, NT_SZ_M), b->checked ? NT.dim : NT.text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, tw);
    if (b->checked) lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l, text_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)bi);
    lv_obj_add_event_cb(l, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)bi);

    if (b->style && !b->checked) {
        static const char *const bangs[4] = { "", "!", "!!", "!!!" };
        lv_obj_t *pr = nt_text(row, bangs[b->style & 3], nt_font(NT_ST_BOLD, NT_SZ_M), nt_prio_color(b->style));
        lv_obj_set_width(pr, 40);
        lv_obj_set_style_text_align(pr, LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        nt_box(row, 40, 10);
    }
    if (!b->checked) {
        lv_obj_t *h = nt_icon_btn(row, NT_IC_HANDLE, 60, NULL, NULL);
        nt_icon_color(h, NT.hair);
        lv_obj_add_event_cb(h, handle_cb, LV_EVENT_ALL, (void *)(intptr_t)bi);
    }
    return row;
}

/* -------------------------------------------------------------------------- */
/* The list                                                                    */
/* -------------------------------------------------------------------------- */

static void add_cb(lv_event_t *e)
{
    (void)e;
    /* after the last pending task */
    int at = T.doc->n;
    for (int i = T.doc->n - 1; i >= 1; i--) {
        if (is_task(i) && !T.doc->b[i].checked) { at = i + 1; break; }
        if (i == 1) at = 1;
    }
    start_edit(at, true);
}

static void title_cb(lv_event_t *e)
{
    (void)e;
    edit_title();
}

static void fold_cb(lv_event_t *e)
{
    (void)e;
    if (T.edit_ta) end_edit(true);
    T.meta->hide_done = !T.meta->hide_done;
    changed();
    rebuild();
}

static lv_obj_t *section(const char *s, bool fold)
{
    lv_obj_t *r = nt_box(T.list, T.W, 72);
    lv_obj_set_style_pad_left(r, 28, 0);
    lv_obj_t *l = nt_text(r, s, nt_font(NT_ST_BOLD, NT_SZ_S), NT.dim);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 6);
    if (fold) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, fold_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *c = nt_icon_btn(r, NT_IC_CHEVRON, 56, NULL, NULL);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        nt_icon_color(c, NT.dim);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -12, 6);
        if (T.meta->hide_done) lv_obj_set_style_transform_rotation(c, -900, 0);
        lv_obj_set_style_transform_pivot_x(c, 28, 0);
        lv_obj_set_style_transform_pivot_y(c, 28, 0);
    }
    return r;
}

static void header(void)
{
    int total = 0, done = 0;
    for (int i = 1; i < T.doc->n; i++) {
        if (!is_task(i) || (i == T.edit_blk && T.edit_new)) continue;
        total++;
        done += T.doc->b[i].checked;
    }
    lv_obj_t *h = nt_box(T.list, T.W, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(h, 32, 0);
    lv_obj_set_style_pad_top(h, 12, 0);
    lv_obj_set_style_pad_bottom(h, 8, 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(h, 14, 0);
    if (T.edit_title) {
        lv_obj_t *ta = lv_textarea_create(h);
        lv_obj_set_width(ta, T.W - 64);
        lv_obj_set_height(ta, LV_SIZE_CONTENT);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_text(ta, T.doc->b[0].txt);
        lv_textarea_set_placeholder_text(ta, _("Título"));
        lv_obj_set_style_text_font(ta, nt_font(NT_ST_BOLD, NT_SZ_XL), 0);
        lv_obj_set_style_text_color(ta, NT.text, 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_pad_all(ta, 0, 0);
        lv_obj_set_style_bg_color(ta, NT.accent, LV_PART_CURSOR);
        lv_obj_set_style_border_color(ta, NT.accent, LV_PART_CURSOR);
        lv_obj_set_style_text_color(ta, NT.hair, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_add_state(ta, LV_STATE_FOCUSED);
        T.edit_ta = ta;
        nt_kb_attach_textarea(ta, ta_done);
        if (T.cbs.keyboard) T.cbs.keyboard(true);
    } else {
        lv_obj_t *t = nt_text(h, T.doc->b[0].len ? T.doc->b[0].txt : _("Título"),
                              nt_font(NT_ST_BOLD, NT_SZ_XL), T.doc->b[0].len ? NT.text : NT.hair);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(t, T.W - 64);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, title_cb, LV_EVENT_CLICKED, NULL);
    }
    if (total) {
        lv_obj_t *row = nt_box(h, T.W - 64, 36);
        char buf[64];
        snprintf(buf, sizeof buf, _("%d de %d"), done, total);
        lv_obj_t *l = nt_text(row, buf, nt_font(0, NT_SZ_S), NT.dim);
        lv_obj_align(l, LV_ALIGN_RIGHT_MID, 0, 0);
        int32_t bw = T.W - 64 - 140;
        lv_obj_t *bar = nt_box(row, bw, 12);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_set_style_bg_color(bar, NT.surface2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *fillb = nt_box(bar, done ? LV_MAX(12, bw * done / total) : 0, 12);
        lv_obj_set_style_radius(fillb, 6, 0);
        lv_obj_set_style_bg_color(fillb, done == total ? lv_color_hex(0x30D158) : NT.accent, 0);
        lv_obj_set_style_bg_opa(fillb, LV_OPA_COVER, 0);
    }
}

static void rebuild(void)
{
    if (!T.list) return;
    int32_t sy = lv_obj_get_scroll_y(T.list);
    /* an edit in progress survives the rebuild: its text goes to the doc */
    if (T.edit_ta) {
        const char *txt = lv_textarea_get_text(T.edit_ta);
        if (T.edit_title) nt_blk_set(&T.doc->b[0], txt, 0);
        else if (T.edit_blk > 0 && T.edit_blk < T.doc->n) nt_blk_set(&T.doc->b[T.edit_blk], txt, 0);
    }
    T.edit_ta = NULL;
    lv_obj_clean(T.list);
    normalise();
    nt_doc_number(T.doc);
    header();

    int pending = 0, done = 0;
    for (int i = 1; i < T.doc->n; i++) {
        if (!is_task(i)) continue;
        if (T.doc->b[i].checked) { done++; continue; }
        add_row(i);
        pending++;
    }
    if (!pending && !done && T.edit_blk < 0) {
        lv_obj_t *e = nt_text(T.list, _("Todavía no hay tareas"), nt_font(0, NT_SZ_M), NT.dim);
        lv_obj_set_style_pad_all(e, 32, 0);
    }
    if (T.edit_blk < 0 || T.edit_title) {
        lv_obj_t *add = nt_box(T.list, T.W, 84);
        lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(add, NT.surface, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(add, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(add, 18, 0);
        lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ic = nt_icon_btn(add, NT_IC_PLUS, 56, NULL, NULL);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        nt_icon_color(ic, NT.accent);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 24, 0);
        lv_obj_t *l = nt_text(add, _("Agregar tarea"), nt_font(0, NT_SZ_M), NT.accent);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 104, 0);
    }
    if (done) {
        char buf[64];
        snprintf(buf, sizeof buf, _("Completadas (%d)"), done);
        section(buf, true);
        if (!T.meta->hide_done) {
            for (int i = 1; i < T.doc->n; i++) {
                if (is_task(i) && T.doc->b[i].checked) add_row(i);
            }
        }
    }
    nt_box(T.list, T.W, 160);           /* room to scroll the last row up */
    lv_obj_update_layout(T.list);
    lv_obj_scroll_to_y(T.list, sy, LV_ANIM_OFF);
    if (T.edit_ta) lv_obj_scroll_to_view_recursive(T.edit_ta, LV_ANIM_OFF);
}

void nt_tasks_refresh(void)
{
    rebuild();
}

static void list_delete_cb(lv_event_t *e)
{
    (void)e;
    T.list = NULL;
    T.edit_ta = NULL;
    T.ghost = NULL;
    T.line = NULL;
}

lv_obj_t *nt_tasks_create(lv_obj_t *parent, nt_doc_t *doc, nt_meta_t *meta, const nt_tasks_cbs_t *cbs)
{
    if (T.doc != doc) {
        T.edit_blk = -1;
        T.edit_new = false;
        T.edit_title = false;
    }
    T.doc = doc;
    T.meta = meta;
    T.cbs = *cbs;
    T.list = lv_obj_create(parent);
    lv_obj_remove_style_all(T.list);
    lv_obj_set_width(T.list, lv_pct(100));
    lv_obj_set_flex_grow(T.list, 1);
    lv_obj_set_flex_flow(T.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(T.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(T.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(T.list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(T.list, list_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_update_layout(parent);
    int32_t w = lv_obj_get_width(parent);
    T.W = w > 900 ? 900 : w;
    /* a new list starts by naming it */
    if (T.edit_blk < 0 && doc->b[0].len == 0 && doc->n <= 2 && (doc->n == 1 || doc->b[1].len == 0)) {
        T.edit_title = true;
        T.edit_blk = 0;
    }
    rebuild();
    return T.list;
}

void nt_tasks_destroyed(void)
{
    T.list = NULL;
    T.edit_ta = NULL;
    T.ghost = NULL;
    T.line = NULL;
}

void nt_tasks_forget(void)
{
    T.doc = NULL;
    T.edit_blk = -1;
    T.edit_new = false;
    T.edit_title = false;
}

int32_t nt_tasks_get_scroll(void)
{
    return T.list ? lv_obj_get_scroll_y(T.list) : 0;
}

void nt_tasks_set_scroll(int32_t y)
{
    if (!T.list) return;
    lv_obj_update_layout(T.list);
    lv_obj_scroll_to_y(T.list, y, LV_ANIM_OFF);
}

void nt_tasks_uncheck_all(void)
{
    if (T.edit_ta) end_edit(true);
    for (int i = 1; i < T.doc->n; i++) T.doc->b[i].checked = 0;
    changed();
    rebuild();
}

void nt_tasks_delete_done(void)
{
    if (T.edit_ta) end_edit(true);
    for (int i = T.doc->n - 1; i >= 1; i--) if (T.doc->b[i].checked) nt_doc_remove(T.doc, i);
    changed();
    rebuild();
}

void nt_tasks_sort_prio(void)
{
    if (T.edit_ta) end_edit(true);
    /* stable: higher priority first, the rest keep their order; children travel with parents */
    nt_doc_t *d = T.doc;
    int n = d->n - 1;
    if (n < 2) return;
    nt_blk_t *out = nt_alloc(sizeof(nt_blk_t) * n);
    if (!out) return;
    int o = 0;
    for (int p = 3; p >= 0; p--) {
        for (int i = 1; i < d->n; i++) {
            if (d->b[i].indent != 0) continue;
            if ((d->b[i].style & 3) != p) continue;
            out[o++] = d->b[i];
            for (int k = i + 1; k < d->n && d->b[k].indent > 0; k++) out[o++] = d->b[k];
        }
    }
    /* orphans (indented with no parent above) keep their place at the end */
    for (int i = 1; i < d->n && o < n; i++) {
        if (d->b[i].indent == 0) continue;
        bool has_parent = false;
        for (int k = i - 1; k >= 1; k--) if (d->b[k].indent == 0) { has_parent = true; break; }
        if (!has_parent) out[o++] = d->b[i];
    }
    if (o == n) memcpy(&d->b[1], out, sizeof(nt_blk_t) * n);
    nt_free(out);
    changed();
    rebuild();
}
