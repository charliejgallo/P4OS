/*
 * P4OS - Going through a game's LVGL buttons with a USB gamepad.
 *
 * Header only, on top of aos_pad.h. A screen hands over its buttons; the
 * d-pad moves to the nearest one in that direction (rows, columns and grids
 * alike, by where they are on screen), an outline shows which, and A clicks
 * it (LV_EVENT_CLICKED, what a finger sends). The outline only appears once
 * the pad is used, so a screen played by touch looks the same as ever.
 *
 *     static aos_pad_t pad;
 *     static aos_pad_menu_t menu;
 *     ...building the screen:
 *     lv_obj_t *items[] = { play_btn, shop_btn, sound_btn };
 *     aos_pad_menu_set(&menu, items, 3, 0);
 *     ...every tick while the screen shows:
 *     aos_pad_update(&pad, lv_tick_get());
 *     aos_pad_menu_step(&menu, &pad);
 *
 * Call aos_pad_menu_set() again (or aos_pad_menu_clear()) whenever those
 * buttons are deleted: the menu keeps their pointers. A button that is
 * hidden or disabled is skipped.
 *
 * The outline is drawn 9 px outside the button, and a parent clips it to
 * its own area: a row or column that holds the buttons tight needs
 * LV_OBJ_FLAG_OVERFLOW_VISIBLE and a bigger draw area of its own
 * (LV_EVENT_REFR_EXT_DRAW_SIZE), or some inner padding if it scrolls.
 */
#pragma once

#include "lvgl.h"
#include "aos_pad.h"

#define AOS_PAD_MENU_MAX 32

typedef struct {
    lv_obj_t *item[AOS_PAD_MENU_MAX];
    uint8_t   n, sel;
    bool      shown;                /* the outline is on (the pad was used) */
} aos_pad_menu_t;

static inline bool aos_pad_menu_usable_(lv_obj_t *o)
{
    return o && lv_obj_is_valid(o) && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) &&
           !lv_obj_has_state(o, LV_STATE_DISABLED);
}

/* (also on a hidden button: it must not come back with an old outline) */
static inline void aos_pad_menu_mark_(aos_pad_menu_t *m, bool on)
{
    if (m->sel >= m->n || !m->item[m->sel] || !lv_obj_is_valid(m->item[m->sel])) return;
    lv_obj_t *o = m->item[m->sel];
    lv_obj_set_style_outline_width(o, on ? 5 : 0, 0);
    lv_obj_set_style_outline_pad(o, on ? 4 : 0, 0);
    lv_obj_set_style_outline_color(o, lv_color_white(), 0);
    lv_obj_set_style_outline_opa(o, LV_OPA_COVER, 0);
}

static inline void aos_pad_menu_set(aos_pad_menu_t *m, lv_obj_t *const *items, int n, int sel)
{
    if (m->shown) aos_pad_menu_mark_(m, false);
    if (n > AOS_PAD_MENU_MAX) n = AOS_PAD_MENU_MAX;
    for (int i = 0; i < n; i++) m->item[i] = items[i];
    m->n = (uint8_t)n;
    m->sel = (uint8_t)(sel >= 0 && sel < n ? sel : 0);
    if (m->shown) aos_pad_menu_mark_(m, true);
}

static inline void aos_pad_menu_clear(aos_pad_menu_t *m)
{
    m->n = 0;
    m->sel = 0;
}

/* The selected button, or NULL. */
static inline lv_obj_t *aos_pad_menu_selected(const aos_pad_menu_t *m)
{
    return m->sel < m->n && aos_pad_menu_usable_(m->item[m->sel]) ? m->item[m->sel] : NULL;
}

/* Moves and clicks; true when the pad did something here (so the game does
 * not take the same press for itself). */
static inline bool aos_pad_menu_step(aos_pad_menu_t *m, const aos_pad_t *p)
{
    if (!m->n) return false;
    if (!aos_pad_menu_usable_(m->item[m->sel])) {
        /* the selected one went away (a buy button hidden once bought): the
         * outline moves at once to the nearest one usable, or the first */
        lv_obj_t *gone = m->item[m->sel];
        bool where = gone && lv_obj_is_valid(gone);
        lv_area_t g = { 0 };
        if (where) lv_obj_get_coords(gone, &g);
        int32_t best = INT32_MAX, bi = -1;
        for (int i = 0; i < m->n; i++) {
            if (!aos_pad_menu_usable_(m->item[i])) continue;
            if (!where) { bi = i; break; }
            lv_area_t b;
            lv_obj_get_coords(m->item[i], &b);
            int32_t dx = (b.x1 + b.x2 - g.x1 - g.x2) / 2, dy = (b.y1 + b.y2 - g.y1 - g.y2) / 2;
            int32_t d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (d < best) { best = d; bi = i; }
        }
        if (bi < 0) return false;
        if (m->shown) aos_pad_menu_mark_(m, false);
        m->sel = (uint8_t)bi;
        if (m->shown) aos_pad_menu_mark_(m, true);
    }
    uint32_t dirs = p->repeat & AOS_PAD_DIRS;
    bool click = aos_pad_pressed(p, AOS_PAD_A);
    if (!dirs && !click) return false;
    if (!m->shown) {
        /* the first press only shows where it is */
        m->shown = true;
        aos_pad_menu_mark_(m, true);
        return true;
    }
    if (dirs) {
        lv_area_t a;
        lv_obj_get_coords(m->item[m->sel], &a);
        int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
        int32_t best = INT32_MAX, bi = -1;
        for (int i = 0; i < m->n; i++) {
            if (i == m->sel || !aos_pad_menu_usable_(m->item[i])) continue;
            lv_area_t b;
            lv_obj_get_coords(m->item[i], &b);
            int32_t dx = (b.x1 + b.x2) / 2 - cx, dy = (b.y1 + b.y2) / 2 - cy;
            /* across: the gap between the two buttons sideways, 0 when they
             * overlap (a narrow button under a wide one is straight below) */
            int32_t along, across;
            bool vert = dirs & (AOS_PAD_UP | AOS_PAD_DOWN);
            if (dirs & AOS_PAD_UP)         along = -dy;
            else if (dirs & AOS_PAD_DOWN)  along = dy;
            else if (dirs & AOS_PAD_LEFT)  along = -dx;
            else                           along = dx;
            if (along <= 0) continue;
            if (vert) across = b.x1 > a.x2 ? b.x1 - a.x2 : a.x1 > b.x2 ? a.x1 - b.x2 : 0;
            else      across = b.y1 > a.y2 ? b.y1 - a.y2 : a.y1 > b.y2 ? a.y1 - b.y2 : 0;
            /* the next row or column first, then the nearest in it */
            int32_t d = along + 2 * across;
            if (d < best) { best = d; bi = i; }
        }
        if (bi >= 0) {
            aos_pad_menu_mark_(m, false);
            m->sel = (uint8_t)bi;
            aos_pad_menu_mark_(m, true);
            lv_obj_scroll_to_view_recursive(m->item[m->sel], LV_ANIM_ON);
        }
        return true;
    }
    lv_obj_send_event(m->item[m->sel], LV_EVENT_CLICKED, NULL);
    return true;
}
