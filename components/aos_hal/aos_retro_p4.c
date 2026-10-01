/*
 * P4OS - the retro canvas's scaler on the board: the PPA (aos_hal.h, "Retro
 * canvas scaler"; the service is components/aos_ui/aos_retro.c, the contract
 * docs/RETRO.md). The simulator's stand-in is sim/retro_sim.c.
 *
 * NOT RUN ON THE BOARD YET: written against esp_driver_ppa of the IDF in
 * ~/esp/esp-idf and compiled; what only the board can settle is listed in
 * docs/RETRO.md, "What the board still has to confirm". The assumptions:
 *
 *  - One SRM operation per refreshed chunk: the aligned block of the canvas
 *    goes, scaled x k, straight into LVGL's draw buffer at its place. No
 *    intermediate scaled copy anywhere. Blocking: the LVGL render waits for
 *    it, like the flush's rotation in aos_hal_p4.c already does.
 *
 *  - Its own SRM client, apart from the flush's (aos_hal_p4.c registers one
 *    for the rotation). The driver queues both on the one SRM engine.
 *
 *  - The destination is an LVGL draw buffer. The PPA only takes one whose
 *    address AND size are multiples of the 128-byte cache line
 *    (ppa_check_buffer_alignment). The display's two draw buffers are
 *    allocated that way in aos_hal_p4.c (128-aligned, 1280*80*2 bytes =
 *    1600 lines). LVGL's own layers and snapshot buffers are only aligned to
 *    CONFIG_LV_DRAW_BUF_ALIGN (4): for those this returns false and the CPU
 *    scales, which is why the service always keeps a CPU path.
 *
 *  - Cache: the driver writes back the source window itself (C2M) and, for
 *    the destination, INVALIDATES whole rows of the output picture
 *    (M2C, aligned outwards to the cache line) before the DMA writes them.
 *    Whatever the CPU drew in those rows and has not written back yet -the
 *    screen's background beside the canvas, the controls- would be thrown
 *    away. So this writes those rows back first (C2M). And the service only
 *    calls here when no other draw task is in progress on the layer
 *    (aos_retro.c, unit_dispatch), so no software draw thread can dirty the
 *    same rows between the write-back and the DMA.
 *
 *  - The source canvas is in PSRAM, 128-aligned and padded (retro_alloc),
 *    which the DMA reads without conditions (flash encryption would change
 *    that for PSRAM: the SRM refuses external buffers then; not enabled).
 *
 *  - SCALING QUALITY: the IDF documents the SRM as BILINEAR ("may cause
 *    chromatic aberration and loss of contrast at the edges"). Pixel art x3
 *    through a bilinear filter comes out soft, each pixel with a blended
 *    ring. The simulator shows what that looks like with
 *    P4_SIM_RETRO_BILINEAR=1. If it is not acceptable on the glass, the
 *    preference "retro_hw" = 0 sends everything to the CPU path (nearest
 *    neighbour, crisp) with no rebuild; it is read when a game opens.
 */
#include "aos_hal.h"

#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LINE 128u                   /* CONFIG_CACHE_L2_CACHE_LINE_SIZE on the P4 */

static const char *TAG = "retro";
static ppa_client_handle_t s_srm;
static int s_state;                 /* 0 not tried, 1 ready, -1 off or failed */
static uint32_t s_fail_log;

uint64_t aos_hal_uptime_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

void *aos_hal_retro_alloc(size_t bytes)
{
    size_t n = (bytes + LINE - 1) & ~(size_t)(LINE - 1);
    void *p = heap_caps_aligned_calloc(LINE, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_aligned_calloc(LINE, 1, n, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (p) {
        /* zeroed through the cache: make it true in memory too, since the
         * first read of it may be the DMA's */
        esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    return p;
}

void aos_hal_retro_free(void *p)
{
    heap_caps_free(p);
}

static bool ready(void)
{
    if (s_state == 0) {
        int32_t on = 1;
        aos_hal_pref_get_i32("retro_hw", &on);
        if (!on) {
            s_state = -1;
            ESP_LOGI(TAG, "PPA off by preference: the CPU scales");
        } else {
            ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
            s_state = ppa_register_client(&pc, &s_srm) == ESP_OK ? 1 : -1;
            if (s_state < 0) ESP_LOGW(TAG, "no PPA client: the CPU scales");
        }
    }
    return s_state > 0;
}

bool aos_hal_retro_scale(const uint16_t *src, int src_w, int src_h,
                         int in_x, int in_y, int in_w, int in_h, int k,
                         uint16_t *dst, size_t dst_bytes, int dst_stride_px, int dst_h,
                         int out_x, int out_y)
{
    if (!ready() || k < 1 || in_w <= 0 || in_h <= 0) return false;
    if (((uintptr_t)dst & (LINE - 1)) || (dst_bytes & (LINE - 1))) return false;
    if (out_x < 0 || out_y < 0 || out_x + in_w * k > dst_stride_px || out_y + in_h * k > dst_h) return false;
    if ((size_t)dst_stride_px * dst_h * 2 > dst_bytes) return false;

    /* write back the rows the driver is about to invalidate, widened to
     * whole cache lines and kept inside the buffer */
    uintptr_t b0 = (uintptr_t)dst;
    uintptr_t a = b0 + (uintptr_t)out_y * dst_stride_px * 2;
    uintptr_t e = a + (uintptr_t)in_h * k * dst_stride_px * 2;
    a &= ~(uintptr_t)(LINE - 1);
    e = (e + LINE - 1) & ~(uintptr_t)(LINE - 1);
    if (e > b0 + dst_bytes) e = b0 + dst_bytes;
    esp_cache_msync((void *)a, e - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src, .pic_w = (uint32_t)src_w, .pic_h = (uint32_t)src_h,
            .block_w = (uint32_t)in_w, .block_h = (uint32_t)in_h,
            .block_offset_x = (uint32_t)in_x, .block_offset_y = (uint32_t)in_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst, .buffer_size = (uint32_t)dst_bytes,
            .pic_w = (uint32_t)dst_stride_px, .pic_h = (uint32_t)dst_h,
            .block_offset_x = (uint32_t)out_x, .block_offset_y = (uint32_t)out_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)k,
        .scale_y = (float)k,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    if (err != ESP_OK) {
        if (s_fail_log++ < 4) ESP_LOGW(TAG, "SRM refused (%s): the CPU scales this one", esp_err_to_name(err));
        return false;
    }
    return true;
}
