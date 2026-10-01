/*
 * P4OS (from AmoledOS) - Cameras: YUV and RGB565 to RGB565, scaled (cam_conv.h).
 *
 * The colour maths is tables, as on the watch: each chroma sample becomes
 * three offsets, and each output pixel is three table reads already shifted
 * into place, OR-ed together, with the clamp inside the tables. Two changes
 * from AmoledOS: the output is native RGB565 (LVGL's order, not the watch
 * panel's big-endian), and Y is taken as limited range (16-235, BT.601),
 * which is what H.264 cameras send; the watch treated it as full range and
 * showed the blacks a little grey.
 *
 * Nearest neighbour and not bilinear: at 720x1280 the pixels are small, and
 * the P4's PPA is the place for a filter if the board shows the need.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "cam_conv.h"

#include <stdlib.h>
#include <string.h>

#define CLAMP_OFF 384

typedef struct {
    int16_t  y[256], rv[256], gu[256], gv[256], bu[256];
    uint16_t r[1024], g[1024], b[1024];         /* index = value + CLAMP_OFF */
} tables_t;

static tables_t *s_t;

bool cam_conv_init(void)
{
    if (s_t) {
        return true;
    }
    tables_t *t = malloc(sizeof(*t));
    if (!t) {
        return false;
    }
    for (int i = 0; i < 256; i++) {
        int c = i - 128;
        t->y[i]  = (int16_t)((298 * (i - 16)) >> 8);
        t->rv[i] = (int16_t)((409 * c) >> 8);
        t->gu[i] = (int16_t)((100 * c) >> 8);
        t->gv[i] = (int16_t)((208 * c) >> 8);
        t->bu[i] = (int16_t)((516 * c) >> 8);
    }
    for (int i = 0; i < 1024; i++) {
        int v = i - CLAMP_OFF;
        uint16_t c = (uint16_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        t->r[i] = (uint16_t)((c & 0xF8) << 8);
        t->g[i] = (uint16_t)((c & 0xFC) << 3);
        t->b[i] = (uint16_t)(c >> 3);
    }
    s_t = t;
    return true;
}

void cam_geometry(int w, int h, int box_w, int box_h, cam_mode_t mode, cam_geom_t *g)
{
    memset(g, 0, sizeof(*g));
    if (w <= 0 || h <= 0 || box_w <= 0 || box_h <= 0) {
        return;
    }
    if (mode == CAM_FIT) {
        g->sw = w;
        g->sh = h;
        g->ow = box_w;
        g->oh = (int)(((int64_t)h * box_w + w / 2) / w);
        if (g->oh > box_h) {
            g->oh = box_h;
            g->ow = (int)(((int64_t)w * box_h + h / 2) / h);
        }
    } else {
        g->ow = box_w;
        g->oh = box_h;
        if ((int64_t)w * box_h > (int64_t)h * box_w) {       /* wider: crop the sides */
            g->sh = h;
            g->sw = (int)(((int64_t)h * box_w + box_h / 2) / box_h);
            if (g->sw > w) g->sw = w;
            g->sx = (w - g->sw) / 2;
        } else {                                             /* taller: crop top and bottom */
            g->sw = w;
            g->sh = (int)(((int64_t)w * box_h + box_w / 2) / box_w);
            if (g->sh > h) g->sh = h;
            g->sy = (h - g->sh) / 2;
        }
    }
    g->ow &= ~1;
    g->oh &= ~1;
    if (g->ow < 2) g->ow = 2;
    if (g->oh < 2) g->oh = 2;
}

void cam_conv_release(cam_conv_t *c)
{
    free(c->xmap);
    c->xmap = NULL;
    c->cap = 0;
}

static bool prepare(cam_conv_t *c, const cam_geom_t *g)
{
    if (g->ow > c->cap) {
        free(c->xmap);
        c->xmap = malloc((size_t)g->ow * sizeof(uint16_t));
        c->cap = c->xmap ? g->ow : 0;
        if (!c->xmap) {
            return false;
        }
    }
    for (int x = 0; x < g->ow; x++) {
        c->xmap[x] = (uint16_t)(g->sx + (x * g->sw + g->sw / 2) / g->ow);
    }
    return s_t != NULL;
}

bool cam_conv_i420(cam_conv_t *c, const aos_h264_pic_t *pic, const cam_geom_t *g, uint16_t *out)
{
    if (!prepare(c, g)) {
        return false;
    }
    const tables_t *t = s_t;
    const uint16_t *R = t->r + CLAMP_OFF, *G = t->g + CLAMP_OFF, *B = t->b + CLAMP_OFF;
    const uint16_t *xm = c->xmap;
    const int ow = g->ow;
    int last_sy = -1;
    uint16_t *prev = NULL;
    for (int y = 0; y < g->oh; y++) {
        int sy = g->sy + (y * g->sh + g->sh / 2) / g->oh;
        uint16_t *o = out + (size_t)y * ow;
        if (sy == last_sy) {                /* enlarging: the same source row again */
            memcpy(o, prev, (size_t)ow * 2);
            continue;
        }
        const uint8_t *ry = pic->y + (size_t)sy * pic->stride_y;
        const uint8_t *ru = pic->u + (size_t)(sy >> 1) * pic->stride_uv;
        const uint8_t *rv = pic->v + (size_t)(sy >> 1) * pic->stride_uv;
        for (int x = 0; x < ow; x++) {
            int sx = xm[x];
            int Y = t->y[ry[sx]];
            int u = ru[sx >> 1];
            int v = rv[sx >> 1];
            o[x] = R[Y + t->rv[v]] | G[Y - t->gu[u] - t->gv[v]] | B[Y + t->bu[u]];
        }
        last_sy = sy;
        prev = o;
    }
    return true;
}

bool cam_conv_rgb565(cam_conv_t *c, const uint16_t *src, int src_stride, const cam_geom_t *g, uint16_t *out)
{
    if (!prepare(c, g)) {
        return false;
    }
    const uint16_t *xm = c->xmap;
    const int ow = g->ow;
    const bool straight = g->sw == g->ow;
    int last_sy = -1;
    uint16_t *prev = NULL;
    for (int y = 0; y < g->oh; y++) {
        int sy = g->sy + (y * g->sh + g->sh / 2) / g->oh;
        uint16_t *o = out + (size_t)y * ow;
        if (sy == last_sy) {
            memcpy(o, prev, (size_t)ow * 2);
            continue;
        }
        const uint16_t *row = src + (size_t)sy * src_stride;
        if (straight) {
            memcpy(o, row + g->sx, (size_t)ow * 2);
        } else {
            for (int x = 0; x < ow; x++) {
                o[x] = row[xm[x]];
            }
        }
        last_sy = sy;
        prev = o;
    }
    return true;
}
