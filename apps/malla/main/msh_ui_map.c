/*
 * Malla - the map: where the nodes are, from what they said.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Drawn here, not with Mapas' tiles: rings of distance around our node, a
 * grid, north up, and every node that has sent a position, coloured as in
 * the list with its short name, dimmer the longer ago it was heard. A line
 * goes from us to each node heard directly (no hops). A finger drags it,
 * the buttons zoom and fit it all, and a tap on a node opens it.
 *
 * The projection is the plain local one (equirectangular around the
 * centre), good for the tens of kilometres a mesh spans. Everything is
 * drawn in LV_EVENT_DRAW_MAIN in screen coordinates: the object's own
 * coords are its origin (HANDOFF-APPS.md, "Lo que se dibuja a mano").
 */
#include "msh_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define M_PER_DEG_LAT 110540.0f
#define M_PER_DEG_LON 111320.0f
#define MPP_MIN       0.5f
#define MPP_MAX       5000.0f

/* metres east and north of the centre, then pixels */
static void project(int32_t lat, int32_t lon, float *x, float *y)
{
    float clat = (float)UI.map_lat * 1e-7f * 0.017453292f;
    float dx = (float)(lon - UI.map_lon) * 1e-7f * M_PER_DEG_LON * cosf(clat);
    float dy = (float)(lat - UI.map_lat) * 1e-7f * M_PER_DEG_LAT;
    *x = dx / UI.map_mpp;
    *y = -dy / UI.map_mpp;
}

/* Centres on all the nodes with a position and picks a scale that fits. */
static void fit(int w, int h)
{
    int32_t la0 = INT32_MAX, la1 = INT32_MIN, lo0 = INT32_MAX, lo1 = INT32_MIN;
    int n = 0;
    for (int i = 0; i < MSH.n_nodes; i++) {
        const msh_node_t *nd = &MSH.nodes[i];
        if (!nd->has_pos) continue;
        if (nd->lat_i < la0) la0 = nd->lat_i;
        if (nd->lat_i > la1) la1 = nd->lat_i;
        if (nd->lon_i < lo0) lo0 = nd->lon_i;
        if (nd->lon_i > lo1) lo1 = nd->lon_i;
        n++;
    }
    if (!n) return;
    UI.map_lat = la0 / 2 + la1 / 2;
    UI.map_lon = lo0 / 2 + lo1 / 2;
    float clat = (float)UI.map_lat * 1e-7f * 0.017453292f;
    float ew = (float)(lo1 - lo0) * 1e-7f * M_PER_DEG_LON * cosf(clat);
    float ns = (float)(la1 - la0) * 1e-7f * M_PER_DEG_LAT;
    float mpp = fmaxf(ew / (float)(w - 160), ns / (float)(h - 160));
    UI.map_mpp = fminf(fmaxf(mpp * 1.1f, 2.0f), MPP_MAX);
    UI.map_fitted = true;
}

static void draw_text(lv_layer_t *layer, const char *t, int32_t x, int32_t y, lv_color_t c, const lv_font_t *f)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;               /* t is on this function's caller's stack */
    d.color = c;
    d.font = f;
    lv_area_t a = { x, y, x + 300, y + lv_font_get_line_height(f) };
    lv_draw_label(layer, &d, &a);
}

/* A "nice" ring spacing for the scale: 1, 2 or 5 times a power of ten. */
static float ring_step(float mpp)
{
    float want = mpp * 160;
    float p = 1;
    while (p * 10 <= want) p *= 10;
    while (p > want) p /= 10;
    if (p * 5 <= want) return p * 5;
    if (p * 2 <= want) return p * 2;
    return p;
}

static void draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(obj, &co);
    int w = lv_area_get_width(&co), h = lv_area_get_height(&co);
    int cx = co.x1 + w / 2, cy = co.y1 + h / 2;

    msh_lock();
    if (!UI.map_fitted) fit(w, h);
    const msh_node_t *me = msh_node_find(MSH.my_num);
    bool me_pos = me && me->has_pos;

    /* the rings, around us when we know where we are */
    float ox = 0, oy = 0;
    if (me_pos) project(me->lat_i, me->lon_i, &ox, &oy);
    float step = ring_step(UI.map_mpp);
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = AOS_C_CARD2;
    ad.width = 2;
    ad.start_angle = 0;
    ad.end_angle = 360;
    for (int k = 1; k <= 8; k++) {
        float r = step * (float)k / UI.map_mpp;
        if (r > (float)(w + h)) break;
        ad.center.x = cx + (int32_t)ox;
        ad.center.y = cy + (int32_t)oy;
        ad.radius = (uint16_t)r;
        lv_draw_arc(layer, &ad);
        char t[24];
        msh_ui_dist(t, sizeof t, step * (float)k);
        draw_text(layer, t, cx + (int32_t)ox + 6, cy + (int32_t)oy - (int32_t)r - 26, AOS_C_DIM, aos_font_caption);
    }
    /* the cross through the centre of the rings */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = AOS_C_CARD2;
    ld.width = 2;
    ld.p1.x = co.x1; ld.p1.y = cy + (int32_t)oy; ld.p2.x = co.x2; ld.p2.y = cy + (int32_t)oy;
    lv_draw_line(layer, &ld);
    ld.p1.x = cx + (int32_t)ox; ld.p1.y = co.y1; ld.p2.x = cx + (int32_t)ox; ld.p2.y = co.y2;
    lv_draw_line(layer, &ld);

    /* the lines to those heard directly */
    if (me_pos) {
        ld.width = 3;
        for (int i = 0; i < MSH.n_nodes; i++) {
            const msh_node_t *n = &MSH.nodes[i];
            if (!n->has_pos || n == me || n->hops != 0 || n->via_mqtt) continue;
            float x, y;
            project(n->lat_i, n->lon_i, &x, &y);
            ld.color = lv_color_hex(msh_node_rgb(n->num));
            ld.opa = LV_OPA_40;
            ld.p1.x = cx + (int32_t)ox; ld.p1.y = cy + (int32_t)oy;
            ld.p2.x = cx + (int32_t)x; ld.p2.y = cy + (int32_t)y;
            lv_draw_line(layer, &ld);
        }
    }

    /* the nodes */
    int shown = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < MSH.n_nodes; i++) {
            const msh_node_t *n = &MSH.nodes[i];
            if (!n->has_pos || (pass == 0) == (n == me)) continue;     /* ours last, on top */
            float x, y;
            project(n->lat_i, n->lon_i, &x, &y);
            int32_t px = cx + (int32_t)x, py = cy + (int32_t)y;
            if (px < co.x1 - 40 || px > co.x2 + 40 || py < co.y1 - 40 || py > co.y2 + 40) continue;
            shown++;
            int32_t age = msh_node_age(n);
            lv_opa_t opa = n == me || (age >= 0 && age < 3600) ? LV_OPA_COVER : age >= 0 && age < 86400 ? LV_OPA_70 : LV_OPA_40;
            lv_draw_rect_dsc_t rd;
            lv_draw_rect_dsc_init(&rd);
            rd.radius = LV_RADIUS_CIRCLE;
            rd.bg_color = lv_color_hex(msh_node_rgb(n->num));
            rd.bg_opa = opa;
            rd.border_color = n == me ? AOS_C_TEXT : AOS_C_BG;
            rd.border_width = n == me ? 4 : 2;
            int r = n == me ? 18 : 14;
            lv_area_t a = { px - r, py - r, px + r, py + r };
            lv_draw_rect(layer, &rd, &a);
            char sn[8], safe[24];
            msh_node_short(n->num, sn, sizeof sn);
            msh_ui_safe(safe, sizeof safe, sn);
            draw_text(layer, safe, px + r + 6, py - 14, n == me ? AOS_C_TEXT : AOS_C_DIM, aos_font_small);
        }
    msh_unlock();
    if (!shown) {
        draw_text(layer, _("Ningún nodo mandó su posición todavía."), co.x1 + 40, cy - 20, AOS_C_DIM, aos_font_small);
    }
    /* north */
    draw_text(layer, "N " LV_SYMBOL_UP, co.x2 - 90, co.y1 + 16, AOS_C_DIM, aos_font_small);
}

static void press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(in, &UI.map_press);
        UI.map_dragged = false;
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        if (v.x || v.y) {
            UI.map_dragged = true;
            float clat = (float)UI.map_lat * 1e-7f * 0.017453292f;
            UI.map_lon -= (int32_t)((float)v.x * UI.map_mpp / (M_PER_DEG_LON * cosf(clat)) * 1e7f);
            UI.map_lat += (int32_t)((float)v.y * UI.map_mpp / M_PER_DEG_LAT * 1e7f);
            UI.map_fitted = true;
            lv_obj_invalidate(UI.map_obj);
        }
    } else if (code == LV_EVENT_CLICKED && !UI.map_dragged) {
        /* the nearest node to the tap, within 50 px */
        lv_area_t co;
        lv_obj_get_coords(UI.map_obj, &co);
        int cx = co.x1 + lv_area_get_width(&co) / 2, cy = co.y1 + lv_area_get_height(&co) / 2;
        uint32_t best = 0;
        float bd = 50 * 50;
        msh_lock();
        for (int i = 0; i < MSH.n_nodes; i++) {
            const msh_node_t *n = &MSH.nodes[i];
            if (!n->has_pos) continue;
            float x, y;
            project(n->lat_i, n->lon_i, &x, &y);
            float dx = cx + x - UI.map_press.x, dy = cy + y - UI.map_press.y;
            if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = n->num; }
        }
        msh_unlock();
        if (best) msh_ui_open_node(best);
    }
}

static void zoom_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (dir == 0) {
        UI.map_fitted = false;
    } else {
        UI.map_mpp *= dir > 0 ? 0.5f : 2.0f;
        if (UI.map_mpp < MPP_MIN) UI.map_mpp = MPP_MIN;
        if (UI.map_mpp > MPP_MAX) UI.map_mpp = MPP_MAX;
    }
    if (UI.map_obj) lv_obj_invalidate(UI.map_obj);
}

void msh_ui_map_build(lv_obj_t *body)
{
    if (UI.map_mpp <= 0) UI.map_mpp = 20;
    lv_obj_t *m = lv_obj_create(body);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(m, lv_color_hex(0x0B0D10), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(m, press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(m, press_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(m, press_cb, LV_EVENT_CLICKED, NULL);
    UI.map_obj = m;

    lv_obj_t *col = lv_obj_create(m);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_align(col, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *b;
    b = msh_ui_btn(col, LV_SYMBOL_PLUS, AOS_C_CARD2, zoom_cb, (void *)1);
    lv_obj_set_size(b, 88, 88);
    b = msh_ui_btn(col, LV_SYMBOL_MINUS, AOS_C_CARD2, zoom_cb, (void *)-1);
    lv_obj_set_size(b, 88, 88);
    b = msh_ui_btn(col, LV_SYMBOL_REFRESH, AOS_C_CARD2, zoom_cb, (void *)0);
    lv_obj_set_size(b, 88, 88);
}
