#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include "driver/jpeg_decode.h"
#include "esp_jpeg_dec.h"
int fake_hw_calls, fake_sw_calls, fake_sw_last_w, fake_sw_last_h;

/* libjpeg -> RGB888 */
static uint8_t *dec888(const uint8_t *in, size_t n, int *w, int *h)
{
    struct jpeg_decompress_struct c; struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, in, n);
    if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&c); return NULL; }
    c.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c);
    *w = c.output_width; *h = c.output_height;
    uint8_t *px = malloc((size_t)*w * *h * 3);
    while (c.output_scanline < c.output_height) { uint8_t *r = px + (size_t)c.output_scanline * *w * 3; jpeg_read_scanlines(&c, &r, 1); }
    jpeg_finish_decompress(&c); jpeg_destroy_decompress(&c);
    return px;
}
static uint16_t p565(const uint8_t *s) { return (uint16_t)((s[0] & 0xF8) << 8 | (s[1] & 0xFC) << 3 | s[2] >> 3); }

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *c, jpeg_decoder_handle_t *h) { (void)c; *h = (void *)1; return ESP_OK; }
void *jpeg_alloc_decoder_mem(size_t size, const jpeg_decode_memory_alloc_cfg_t *c, size_t *a) { (void)c; *a = size; return malloc(size); }
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t h, const jpeg_decode_cfg_t *c, const uint8_t *in, uint32_t n, uint8_t *out, uint32_t out_size, uint32_t *got)
{
    (void)h;
    const char *mode = getenv("FAKE_ENGINE"); if (!mode) mode = "bgr_ok";
    if (!strcmp(mode, "dead")) return ESP_FAIL;
    fake_hw_calls++;
    int w, hh; uint8_t *px = dec888(in, n, &w, &hh);
    if (!px) return ESP_FAIL;
    int pw = (w + 15) & ~15, ph = (hh + 15) & ~15;
    if ((uint32_t)pw * ph * 2 > out_size) { free(px); return ESP_FAIL; }
    bool right = (!strcmp(mode, "rgb_ok")) == (c->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_RGB);
    uint16_t *o = (uint16_t *)out;
    for (int y = 0; y < hh; y++) for (int x = 0; x < w; x++) {
        const uint8_t *s = px + ((size_t)y * w + x) * 3;
        uint8_t sw[3] = { s[2], s[1], s[0] };
        uint16_t v = p565(right ? s : sw);
        if (!strcmp(mode, "swapped")) v = (uint16_t)(v << 8 | v >> 8);
        o[(size_t)y * pw + x] = v;
    }
    *got = (uint32_t)pw * ph * 2;
    free(px);
    return ESP_OK;
}

typedef struct { jpeg_dec_config_t cfg; int w, h, ow, oh; } fdec_t;
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *c, jpeg_dec_handle_t *h) { fdec_t *d = calloc(1, sizeof *d); d->cfg = *c; *h = d; return JPEG_ERR_OK; }
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t h, jpeg_dec_io_t *io, jpeg_dec_header_info_t *info)
{
    fdec_t *d = h;
    struct jpeg_decompress_struct c; struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e); jpeg_create_decompress(&c);
    jpeg_mem_src(&c, io->inbuf, io->inbuf_len);
    int r = jpeg_read_header(&c, TRUE);
    d->w = c.image_width; d->h = c.image_height; jpeg_destroy_decompress(&c);
    if (r != JPEG_HEADER_OK) return JPEG_ERR_FAIL;
    if (d->cfg.scale.width && (d->cfg.scale.width % 8 || d->cfg.scale.height % 8 || d->cfg.scale.width * 8 < d->w - 7 || d->cfg.scale.height * 8 < d->h - 7 || d->cfg.scale.width > d->w || d->cfg.scale.height > d->h)) {
        printf("  FAKE esp_new_jpeg: invalid scale %dx%d for %dx%d\n", d->cfg.scale.width, d->cfg.scale.height, d->w, d->h);
        return JPEG_ERR_FAIL;
    }
    d->ow = d->cfg.scale.width ? d->cfg.scale.width : d->w; d->oh = d->cfg.scale.height ? d->cfg.scale.height : d->h;
    info->width = d->w; info->height = d->h;
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t h, int *len) { fdec_t *d = h; *len = d->ow * d->oh * 2; return JPEG_ERR_OK; }
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t h, jpeg_dec_io_t *io)
{
    fdec_t *d = h; fake_sw_calls++; fake_sw_last_w = d->ow; fake_sw_last_h = d->oh;
    int w, hh; uint8_t *px = dec888(io->inbuf, io->inbuf_len, &w, &hh);
    if (!px) return JPEG_ERR_FAIL;
    uint16_t *o = (uint16_t *)io->outbuf;
    for (int y = 0; y < d->oh; y++) for (int x = 0; x < d->ow; x++) {    /* nearest: the real one does better */
        int sx = (int)((long)x * w / d->ow), sy = (int)((long)y * hh / d->oh);
        o[(size_t)y * d->ow + x] = p565(px + ((size_t)sy * w + sx) * 3);
    }
    free(px);
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t h) { free(h); return JPEG_ERR_OK; }
void *jpeg_calloc_align(size_t size, int a) { void *p = NULL; posix_memalign(&p, a, size); memset(p, 0, size); return p; }
void jpeg_free_align(void *p) { free(p); }
