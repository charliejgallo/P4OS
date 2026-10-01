/* fake esp_new_jpeg over libjpeg: RGB565 LE, scaled to scale.width x height */
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef enum { JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1 } jpeg_error_t;
typedef enum { JPEG_PIXEL_FORMAT_RGB565_LE = 1, JPEG_PIXEL_FORMAT_RGB888 = 2 } jpeg_pixel_format_t;
typedef enum { JPEG_ROTATE_0D = 0 } jpeg_rotate_t;
typedef struct { uint16_t width, height; } jpeg_resolution_t;
typedef struct { jpeg_pixel_format_t output_type; jpeg_resolution_t scale, clipper; jpeg_rotate_t rotate; bool block_enable; } jpeg_dec_config_t;
#define DEFAULT_JPEG_DEC_CONFIG() { .output_type = JPEG_PIXEL_FORMAT_RGB888, .scale = {0, 0}, .clipper = {0, 0}, .rotate = JPEG_ROTATE_0D, .block_enable = false }
typedef void *jpeg_dec_handle_t;
typedef struct { uint16_t width, height; } jpeg_dec_header_info_t;
typedef struct { uint8_t *inbuf; int inbuf_len; int inbuf_remain; uint8_t *outbuf; int out_size; } jpeg_dec_io_t;
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *c, jpeg_dec_handle_t *h);
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t h, jpeg_dec_io_t *io, jpeg_dec_header_info_t *info);
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t h, int *len);
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t h, jpeg_dec_io_t *io);
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t h);
void *jpeg_calloc_align(size_t size, int aligned);
void jpeg_free_align(void *p);
extern int fake_sw_calls, fake_sw_last_w, fake_sw_last_h;
