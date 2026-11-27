/*
 * P4OS - Grabador: is something moving.
 *
 * The picture is reduced to a grid of 32 x 24 cells, each the mean
 * brightness of a few pixels in it; a cell has moved when it differs from the
 * background by more than the sensitivity's threshold. The background follows
 * the picture slowly (a light turned on goes in in a few seconds and is not a
 * clip of its own). The grid is split in the zones (GR_ZONES_W x GR_ZONES_H)
 * the user left on; motion is a share of the cells of those zones, and a
 * whole-picture jump (the camera's own exposure, a light switched) is not.
 */
#include "gr.h"

#include <math.h>

#define GW 32
#define GH 24
static const float THRESH[3] = { 30.0f, 20.0f, 12.0f };     /* of 255 */
static const float SHARE[3] = { 0.06f, 0.03f, 0.012f };     /* of the zones' cells */

void gr_motion_reset(gr_motion_t *m) { memset(m, 0, sizeof *m); }

bool gr_motion_feed(gr_motion_t *m, const uint16_t *px, int w, int h, int stride, int sens, uint16_t zones)
{
    if (sens < 0) sens = 0;
    if (sens > 2) sens = 2;
    float cell[GW * GH];
    for (int gy = 0; gy < GH; gy++)
        for (int gx = 0; gx < GW; gx++) {
            /* 4 x 4 samples in each cell */
            int x0 = gx * w / GW, y0 = gy * h / GH, cw = w / GW, ch = h / GH;
            float sum = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    uint16_t p = px[(size_t)(y0 + sy * ch / 4) * stride + x0 + sx * cw / 4];
                    int r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
                    sum += 0.299f * r + 0.587f * g + 0.114f * b;
                }
            cell[gy * GW + gx] = sum / 16.0f;
        }
    m->frames++;
    if (!m->ready) {
        memcpy(m->bg, cell, sizeof cell);
        m->ready = true;
        m->level = 0;
        return false;
    }
    /* the whole picture's change first: the exposure or a light */
    float mean = 0;
    for (int i = 0; i < GW * GH; i++) mean += cell[i] - m->bg[i];
    mean /= (float)(GW * GH);
    int moved = 0, total = 0;
    memset(m->moved, 0, sizeof m->moved);
    for (int gy = 0; gy < GH; gy++)
        for (int gx = 0; gx < GW; gx++) {
            int i = gy * GW + gx;
            int z = (gy * GR_ZONES_H / GH) * GR_ZONES_W + gx * GR_ZONES_W / GW;
            float d = fabsf(cell[i] - m->bg[i] - mean);
            /* the background follows: fast at first, slowly after */
            float a = m->frames < 20 ? 0.3f : 0.04f;
            m->bg[i] += a * (cell[i] - m->bg[i]);
            if (!(zones & (1u << z))) continue;
            total++;
            if (d > THRESH[sens]) { moved++; m->moved[z]++; }
        }
    m->level = total ? (float)moved / (float)total : 0;
    /* a second of warming up: the camera settling */
    return m->frames > 20 && m->level >= SHARE[sens];
}
