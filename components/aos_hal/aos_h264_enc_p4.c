/*
 * P4OS - H.264 for the apps: the P4's hardware encoder (aos_hal.h,
 * aos_hal_h264_enc_*).
 *
 * The same pieces as the screen recording (aos_screenrec_p4.c): the PPA
 * turns the app's RGB565 into the encoder's YUV 4:2:0 (O_UYY_E_VYY), scaling
 * on the way when the sizes differ, and esp_h264's hardware encoder codes it.
 * One encoder at a time in the whole board: the screen recording and an app
 * take turns (s_taken).
 *
 * A key frame on request: esp_h264 has no call for one, so the encoder is
 * closed and opened again, which starts a new group with an IDR (and its
 * SPS and PPS).
 *
 * Buffers: PSRAM, aligned to the cache line, as the PPA and the encoder's
 * DMA want them.
 */
#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_h264_enc_single_hw.h"
#include "esp_h264_enc_single.h"

#define TAG  "h264enc"
#define LINE 128

struct aos_h264_enc {
    int w, h;
    uint8_t *yuv, *out;
    size_t yuv_n, out_n;
    ppa_client_handle_t srm;
    esp_h264_enc_handle_t enc;
    uint32_t pts;
};

static volatile bool s_taken;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* the screen recording asks too (aos_screenrec_p4.c) */
bool aos_h264_enc_take(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = !s_taken;
    if (ok) s_taken = true;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void aos_h264_enc_give(void) { s_taken = false; }

aos_h264_enc_t *aos_hal_h264_enc_open(int w, int h, int fps, int bitrate, int gop)
{
    if (w < 16 || h < 16 || w > 1920 || h > 1088 || (w & 15) || (h & 15)) return NULL;
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    if (gop <= 0) gop = fps * 2;
    if (gop > 255) gop = 255;
    if (bitrate < 100000) bitrate = 100000;
    if (!aos_h264_enc_take()) { ESP_LOGW(TAG, "the encoder is taken (a screen recording?)"); return NULL; }
    aos_h264_enc_t *e = heap_caps_calloc(1, sizeof *e, MALLOC_CAP_SPIRAM);
    if (!e) { aos_h264_enc_give(); return NULL; }
    e->w = w;
    e->h = h;
    e->yuv_n = ((size_t)w * h * 3 / 2 + LINE - 1) & ~(size_t)(LINE - 1);
    e->out_n = ((size_t)w * h + LINE - 1) & ~(size_t)(LINE - 1);
    e->yuv = heap_caps_aligned_calloc(LINE, 1, e->yuv_n, MALLOC_CAP_SPIRAM);
    e->out = heap_caps_aligned_calloc(LINE, 1, e->out_n, MALLOC_CAP_SPIRAM);
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1,
                               .data_burst_length = PPA_DATA_BURST_LENGTH_128 };
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = (uint8_t)gop,
        .fps = (uint8_t)fps,
        .res = { .width = (uint16_t)w, .height = (uint16_t)h },
        .rc = { .bitrate = (uint32_t)bitrate, .qp_min = 22, .qp_max = 40 },
    };
    if (!e->yuv || !e->out || ppa_register_client(&pc, &e->srm) != ESP_OK ||
        esp_h264_enc_hw_new(&cfg, &e->enc) != ESP_H264_ERR_OK || esp_h264_enc_open(e->enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "could not start (%dx%d)", w, h);
        aos_hal_h264_enc_close(e);
        return NULL;
    }
    aos_hal_log(TAG, "open: %dx%d, %d fps, %d kbit/s, a key frame every %d", w, h, fps, bitrate / 1000, gop);
    return e;
}

int aos_hal_h264_enc_frame(aos_h264_enc_t *e, const uint16_t *rgb565, int src_w, int src_h, int stride_px, bool key,
                           const uint8_t **out, bool *is_key)
{
    if (!e || !e->enc || !rgb565 || src_w <= 0 || src_h <= 0 || !out) return -1;
    if (stride_px < src_w) stride_px = src_w;
    if (key) {
        /* a new group: closed and opened, it starts with an IDR */
        esp_h264_enc_close(e->enc);
        if (esp_h264_enc_open(e->enc) != ESP_H264_ERR_OK) return -1;
    }
    float sx = (float)e->w / (float)src_w, sy = (float)e->h / (float)src_h;
    ppa_srm_oper_config_t c = {
        .in = { .buffer = rgb565, .pic_w = (uint32_t)stride_px, .pic_h = (uint32_t)src_h, .block_w = (uint32_t)src_w,
                .block_h = (uint32_t)src_h, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = e->yuv, .buffer_size = (uint32_t)e->yuv_n, .pic_w = (uint32_t)e->w, .pic_h = (uint32_t)e->h,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = PPA_COLOR_RANGE_LIMIT,
                 .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = sx, .scale_y = sy,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    /* the PPA reads the source by DMA: what the CPU wrote must be in PSRAM */
    esp_cache_msync((void *)rgb565, (size_t)stride_px * src_h * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ppa_do_scale_rotate_mirror(e->srm, &c) != ESP_OK) { ESP_LOGE(TAG, "the PPA refused the picture"); return -1; }
    esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = e->yuv, .len = (uint32_t)((size_t)e->w * e->h * 3 / 2) },
                                   .pts = e->pts };
    esp_h264_enc_out_frame_t o = { .raw_data = { .buffer = e->out, .len = (uint32_t)e->out_n } };
    e->pts += 1;
    if (esp_h264_enc_process(e->enc, &in, &o) != ESP_H264_ERR_OK) { ESP_LOGE(TAG, "the encoder refused a frame"); return -1; }
    if (is_key) *is_key = o.frame_type == ESP_H264_FRAME_TYPE_IDR || o.frame_type == ESP_H264_FRAME_TYPE_I;
    *out = e->out;
    return (int)o.length;
}

void aos_hal_h264_enc_close(aos_h264_enc_t *e)
{
    if (!e) return;
    if (e->enc) {
        esp_h264_enc_close(e->enc);
        esp_h264_enc_del(e->enc);
    }
    if (e->srm) ppa_unregister_client(e->srm);
    heap_caps_free(e->yuv);
    heap_caps_free(e->out);
    heap_caps_free(e);
    aos_h264_enc_give();
}
