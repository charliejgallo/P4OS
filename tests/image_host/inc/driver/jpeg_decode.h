/* fake P4 JPEG engine over libjpeg: RGB565 out, rows padded to 16.
 * FAKE_ENGINE=bgr_ok (default): BGR order gives LVGL's RGB565 LE
 * FAKE_ENGINE=rgb_ok: only RGB order does; FAKE_ENGINE=swapped: bytes swapped
 * FAKE_ENGINE=dead: every call fails */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct jpeg_decoder_t *jpeg_decoder_handle_t;
typedef enum { JPEG_DECODE_OUT_FORMAT_RGB565 = 1 } jpeg_dec_output_format_t;
typedef enum { JPEG_DEC_RGB_ELEMENT_ORDER_BGR = 0, JPEG_DEC_RGB_ELEMENT_ORDER_RGB = 1 } jpeg_dec_rgb_element_order_t;
typedef enum { JPEG_YUV_RGB_CONV_STD_BT601 = 0 } jpeg_yuv_rgb_conv_std_t;
typedef struct { jpeg_dec_output_format_t output_format; jpeg_dec_rgb_element_order_t rgb_order; jpeg_yuv_rgb_conv_std_t conv_std; } jpeg_decode_cfg_t;
typedef struct { int intr_priority; int timeout_ms; } jpeg_decode_engine_cfg_t;
typedef enum { JPEG_DEC_ALLOC_INPUT_BUFFER, JPEG_DEC_ALLOC_OUTPUT_BUFFER } jpeg_dec_buffer_alloc_direction_t;
typedef struct { jpeg_dec_buffer_alloc_direction_t buffer_direction; } jpeg_decode_memory_alloc_cfg_t;
esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *c, jpeg_decoder_handle_t *h);
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t h, const jpeg_decode_cfg_t *c, const uint8_t *in, uint32_t n, uint8_t *out, uint32_t out_size, uint32_t *got);
void *jpeg_alloc_decoder_mem(size_t size, const jpeg_decode_memory_alloc_cfg_t *c, size_t *allocated);
extern int fake_hw_calls;
