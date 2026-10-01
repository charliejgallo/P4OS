/*
 * Hardware engines (HARDWARE.md test 13): the JPEG codec and the PPA.
 *
 *   jpeg [quality]   encode a 720x1280 RGB565 frame and decode it back,
 *                    then show the decoded frame for two seconds
 *   ppa              the operations P4OS leans on: the 3x "retro canvas"
 *                    upscale (APPS.md recipe C), a full-frame rotation
 *                    (landscape), and a full-frame alpha blend (panels,
 *                    fake blur)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_encode.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "bench.h"

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/* Something with gradients, edges and text-like detail, so the JPEG size is
 * representative of a UI screenshot rather than of a flat colour. */
static void test_image(uint16_t *p, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t r = x * 255 / w, g = y * 255 / h, b = ((x / 40 + y / 40) & 1) ? 200 : 40;
            if ((x % 90) < 3 || (y % 90) < 3) r = g = b = 255;
            if (((x * 7 + y * 3) % 23) == 0) r = g = b = 0;
            p[y * w + x] = rgb(r, g, b);
        }
}

static void show(const uint16_t *img, int w, int h, int ms)
{
    lvport_pause(true);
    uint16_t *fb = disp_fb(disp_front());
    memset(fb, 0, LCD_FB_BYTES);
    int ox = (LCD_W - w) / 2, oy = (LCD_H - h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    for (int y = 0; y < h && y + oy < LCD_H; y++)
        memcpy(fb + (y + oy) * LCD_W + ox, img + y * w, (w < LCD_W ? w : LCD_W) * 2);
    disp_cache_flush(fb, LCD_FB_BYTES);
    vTaskDelay(pdMS_TO_TICKS(ms));
    lvport_pause(false);
}

static int cmd_jpeg(int argc, char **argv)
{
    int q = arg_int(argc, argv, 1, 80);
    const int w = LCD_W, h = LCD_H;
    size_t in_sz = w * h * 2, got_in = 0, got_out = 0, got_dec = 0;
    jpeg_encode_memory_alloc_cfg_t ein = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t eout = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    uint16_t *raw = jpeg_alloc_encoder_mem(in_sz, &ein, &got_in);
    uint8_t *jpg = jpeg_alloc_encoder_mem(in_sz / 2, &eout, &got_out);
    jpeg_decode_memory_alloc_cfg_t din = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t dout = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    uint8_t *jin = jpeg_alloc_decoder_mem(in_sz / 2, &din, &got_in);
    uint16_t *dec = jpeg_alloc_decoder_mem(in_sz, &dout, &got_dec);
    if (!raw || !jpg || !jin || !dec) { bench_report("jpeg", "error=nomem"); goto out; }
    test_image(raw, w, h);

    jpeg_encoder_handle_t enc;
    jpeg_encode_engine_cfg_t ec = { .timeout_ms = 500 };
    jpeg_new_encoder_engine(&ec, &enc);
    jpeg_encode_cfg_t cfg = { .width = w, .height = h, .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                              .sub_sample = JPEG_DOWN_SAMPLING_YUV420, .image_quality = q };
    uint32_t jlen = 0;
    int64_t t = bench_us();
    for (int i = 0; i < 5; i++) jpeg_encoder_process(enc, &cfg, (uint8_t *)raw, in_sz, jpg, got_out, &jlen);
    double enc_ms = (bench_us() - t) / 5000.0;
    jpeg_del_encoder_engine(enc);

    memcpy(jin, jpg, jlen);
    jpeg_decoder_handle_t d;
    jpeg_decode_engine_cfg_t dc = { .timeout_ms = 500 };
    jpeg_new_decoder_engine(&dc, &d);
    jpeg_decode_cfg_t dcfg = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR };
    uint32_t dlen = 0;
    esp_err_t e = ESP_OK;
    t = bench_us();
    for (int i = 0; i < 5; i++) e = jpeg_decoder_process(d, &dcfg, jin, jlen, (uint8_t *)dec, got_dec, &dlen);
    double dec_ms = (bench_us() - t) / 5000.0;
    jpeg_del_decoder_engine(d);

    bench_report("jpeg", "w=%d h=%d quality=%d bytes=%lu enc_ms=%.2f enc_fps=%.1f dec_ms=%.2f dec_fps=%.1f dec_ok=%d",
                 w, h, q, (unsigned long)jlen, enc_ms, 1000 / enc_ms, dec_ms, 1000 / dec_ms, e == ESP_OK);
    if (e == ESP_OK) show(dec, w, h, 2000);
out:
    heap_caps_free(raw); heap_caps_free(jpg); heap_caps_free(jin); heap_caps_free(dec);
    return 0;
}

static double srm(ppa_client_handle_t c, const void *in, int iw, int ih, void *out, int ow, int oh, size_t out_sz,
                  int ox, int oy, float scale, ppa_srm_rotation_angle_t rot, int reps)
{
    ppa_srm_oper_config_t op = {
        .in = { .buffer = in, .pic_w = iw, .pic_h = ih, .block_w = iw, .block_h = ih, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = out, .buffer_size = out_sz, .pic_w = ow, .pic_h = oh, .block_offset_x = ox, .block_offset_y = oy,
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = rot, .scale_x = scale, .scale_y = scale, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t t = bench_us();
    for (int i = 0; i < reps; i++)
        if (ppa_do_scale_rotate_mirror(c, &op) != ESP_OK) return -1;
    return (bench_us() - t) / 1000.0 / reps;
}

static int cmd_ppa(int argc, char **argv)
{
    ppa_client_handle_t c, b;
    ppa_client_config_t cs = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ppa_client_config_t cb = { .oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1 };
    if (ppa_register_client(&cs, &c) != ESP_OK || ppa_register_client(&cb, &b) != ESP_OK) {
        bench_report("ppa", "error=client");
        return 0;
    }
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    uint16_t *small = heap_caps_aligned_alloc(128, 240 * 426 * 2, caps);
    uint16_t *full = heap_caps_aligned_alloc(128, LCD_FB_BYTES, caps);
    uint16_t *land = heap_caps_aligned_alloc(128, LCD_FB_BYTES, caps);
    uint32_t *argb = heap_caps_aligned_alloc(128, LCD_W * LCD_H * 4, caps);
    if (!small || !full || !land || !argb) { bench_report("ppa", "error=nomem"); goto out; }

    test_image(small, 240, 426);
    test_image(full, LCD_W, LCD_H);
    for (int i = 0; i < LCD_W * LCD_H; i++) argb[i] = 0x80FFFFFF;   /* 50 % white glass */

    double up3 = srm(c, small, 240, 426, land, 720, 1280, LCD_FB_BYTES, 0, 1, 3.0f, PPA_SRM_ROTATION_ANGLE_0, 10);
    double up3w = srm(c, small, 184, 224, land, 720, 1280, LCD_FB_BYTES, 84, 304, 3.0f, PPA_SRM_ROTATION_ANGLE_0, 10);
    double rot = srm(c, full, LCD_W, LCD_H, land, LCD_H, LCD_W, LCD_FB_BYTES, 0, 0, 1.0f, PPA_SRM_ROTATION_ANGLE_90, 10);
    double copy = srm(c, full, LCD_W, LCD_H, land, LCD_W, LCD_H, LCD_FB_BYTES, 0, 0, 1.0f, PPA_SRM_ROTATION_ANGLE_0, 10);
    double down = srm(c, full, LCD_W, LCD_H, small, 180, 320, 240 * 426 * 2, 0, 0, 0.25f, PPA_SRM_ROTATION_ANGLE_0, 10);

    ppa_blend_oper_config_t bo = {
        .in_bg = { .buffer = full, .pic_w = LCD_W, .pic_h = LCD_H, .block_w = LCD_W, .block_h = LCD_H, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .in_fg = { .buffer = argb, .pic_w = LCD_W, .pic_h = LCD_H, .block_w = LCD_W, .block_h = LCD_H, .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888 },
        .out = { .buffer = land, .buffer_size = LCD_FB_BYTES, .pic_w = LCD_W, .pic_h = LCD_H, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t t = bench_us();
    esp_err_t be = ESP_OK;
    for (int i = 0; i < 10; i++) be = ppa_do_blend(b, &bo);
    double blend = be == ESP_OK ? (bench_us() - t) / 10000.0 : -1;

    bench_report("ppa", "up3_240x426_ms=%.2f up3_184x224_ms=%.2f rot90_full_ms=%.2f copy_full_ms=%.2f down4_full_ms=%.2f blend_full_ms=%.2f",
                 up3, up3w, rot, copy, down, blend);

    /* what the retro canvas looks like at 3x (nearest or smoothed?) */
    srm(c, small, 240, 426, land, 720, 1280, LCD_FB_BYTES, 0, 1, 3.0f, PPA_SRM_ROTATION_ANGLE_0, 1);
    bench_say("ppa: 240x426 canvas scaled x3 - is it sharp (nearest) or blurry (bilinear)?");
    show(land, LCD_W, LCD_H, 3000);
out:
    heap_caps_free(small); heap_caps_free(full); heap_caps_free(land); heap_caps_free(argb);
    ppa_unregister_client(c);
    ppa_unregister_client(b);
    return 0;
}

void reg_codec(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "jpeg", .help = "jpeg [quality]: HW encode + decode of a full frame", .func = cmd_jpeg },
        { .command = "ppa", .help = "PPA scale x3, rotate, blend on full frames", .func = cmd_ppa },
    };
    for (int i = 0; i < sizeof cmds / sizeof cmds[0]; i++) esp_console_cmd_register(&cmds[i]);
}
