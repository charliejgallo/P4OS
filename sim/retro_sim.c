/*
 * P4OS simulator - the retro canvas's scaler (aos_hal.h, "Retro canvas
 * scaler"); the board's is components/aos_hal/aos_retro_p4.c.
 *
 * It stands in for the PPA with the same contract: it gets the whole aligned
 * block and writes k*in_w by k*in_h pixels at the offset, so the split that
 * components/aos_ui/aos_retro.c does (block to the "hardware", ragged edges to
 * the CPU) runs here exactly as it will on the board.
 *
 *   P4_SIM_RETRO_BILINEAR=1   scale the way the PPA's SRM does (the IDF
 *                             documents it as bilinear), to see what pixel
 *                             art looks like through it before the board
 *   P4_SIM_RETRO_HW=0         decline, so everything goes through the CPU
 *                             path of aos_retro.c (the board's fallback)
 */
#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

void *aos_hal_retro_alloc(size_t bytes)
{
    size_t n = (bytes + 127) & ~(size_t)127;
    void *p = NULL;
    if (posix_memalign(&p, 128, n) != 0) return NULL;
    memset(p, 0, n);
    return p;
}

void aos_hal_retro_hw_reload(void) {}

void aos_hal_retro_free(void *p)
{
    free(p);
}

uint64_t aos_hal_uptime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int s_mode = -1;             /* 0 off, 1 nearest, 2 bilinear */

static int mode(void)
{
    if (s_mode < 0) {
        const char *hw = getenv("P4_SIM_RETRO_HW");
        const char *bl = getenv("P4_SIM_RETRO_BILINEAR");
        s_mode = (hw && hw[0] == '0') ? 0 : (bl && bl[0] && bl[0] != '0') ? 2 : 1;
    }
    return s_mode;
}

static inline uint16_t mix565(uint16_t a, uint16_t b, uint16_t c, uint16_t d, int fx, int fy)
{
    /* fx, fy in 1/256 */
    int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
    int r = ((a >> 11) * w00 + (b >> 11) * w10 + (c >> 11) * w01 + (d >> 11) * w11) >> 16;
    int g = (((a >> 5) & 63) * w00 + ((b >> 5) & 63) * w10 + ((c >> 5) & 63) * w01 + ((d >> 5) & 63) * w11) >> 16;
    int bl = ((a & 31) * w00 + (b & 31) * w10 + (c & 31) * w01 + (d & 31) * w11) >> 16;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

bool aos_hal_retro_scale(const uint16_t *src, int src_w, int src_h,
                         int in_x, int in_y, int in_w, int in_h, int k,
                         uint16_t *dst, size_t dst_bytes, int dst_stride_px, int dst_h,
                         int out_x, int out_y)
{
    int m = mode();
    if (m == 0 || k < 1) return false;
    if (in_x < 0 || in_y < 0 || in_x + in_w > src_w || in_y + in_h > src_h) return false;
    if (out_x < 0 || out_y < 0 || out_x + in_w * k > dst_stride_px || out_y + in_h * k > dst_h) return false;
    if ((size_t)dst_stride_px * dst_h * 2 > dst_bytes) return false;

    const int ow = in_w * k;
    for (int by = 0; by < in_h; by++) {
        const uint16_t *s = src + (size_t)(in_y + by) * src_w + in_x;
        uint16_t *d = dst + (size_t)(out_y + by * k) * dst_stride_px + out_x;
        if (m == 1) {
            for (int x = 0; x < in_w; x++) {
                uint16_t c = s[x];
                for (int j = 0; j < k; j++) d[x * k + j] = c;
            }
            for (int j = 1; j < k; j++) memcpy(d + (size_t)j * dst_stride_px, d, (size_t)ow * 2);
            continue;
        }
        /* bilinear, pixel centres aligned, clamped to the block's edges */
        for (int j = 0; j < k; j++) {
            int fy256 = ((2 * j + 1) * 256 / (2 * k)) - 128;      /* offset from this row's centre */
            int sy0 = by, sy1 = by;
            int fy = fy256;
            if (fy < 0) { sy0 = by - 1; fy += 256; } else { sy1 = by + 1; }
            if (sy0 < 0) sy0 = 0;
            if (sy1 >= in_h) sy1 = in_h - 1;
            const uint16_t *r0 = src + (size_t)(in_y + sy0) * src_w + in_x;
            const uint16_t *r1 = src + (size_t)(in_y + sy1) * src_w + in_x;
            uint16_t *dr = d + (size_t)j * dst_stride_px;
            for (int ox = 0; ox < ow; ox++) {
                int bx = ox / k, i = ox % k;
                int fx = ((2 * i + 1) * 256 / (2 * k)) - 128;
                int sx0 = bx, sx1 = bx;
                if (fx < 0) { sx0 = bx - 1; fx += 256; } else { sx1 = bx + 1; }
                if (sx0 < 0) sx0 = 0;
                if (sx1 >= in_w) sx1 = in_w - 1;
                dr[ox] = mix565(r0[sx0], r0[sx1], r1[sx0], r1[sx1], fx, fy);
            }
        }
    }
    return true;
}
