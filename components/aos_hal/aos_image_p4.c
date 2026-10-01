/*
 * P4OS HAL - pictures: aos_hal_image_decode() on the board.
 *
 * What comes in: a JPEG, a PNG or a BMP, from a file or from a range of one
 * (the cover inside an MP3). What goes out: RGB565 in LVGL's order at the
 * size asked, fitted inside a box or covering it and cropped to the centre.
 * The simulator does the same over libavcodec (sim/hal_sim.c).
 *
 * The path a JPEG takes depends on its size, because the P4's JPEG engine
 * is fast but never scales:
 *
 *   up to 4 MP, baseline    the hardware engine decodes it whole (padded to
 *                           16 px), then the resampler below brings it to
 *                           size. 8 MB of output at most.
 *   bigger (a phone's 12 MP) esp_new_jpeg in software, which scales while it
 *                           decodes (down to 1/8, to multiples of 8): a
 *                           12 MP photo never exists in memory at full size -
 *                           it would be 24 MB of the 32.
 *   progressive             neither can: stb_image in software (the copy
 *                           LVGL ships), up to 2,5 MP. It keeps every DCT
 *                           coefficient until the last scan, ~6 bytes a
 *                           pixel with the output, all in PSRAM. Phones write
 *                           baseline; web images sometimes are progressive.
 *
 * The last step is always the same software resampler: an area average
 * when shrinking (a thumbnail from 1/8 of a photo needs a real low-pass or
 * it shimmers), bilinear when enlarging (a small cover filling a big box).
 * The PPA could do the scaling, but in steps of 1/16, too coarse for
 * thumbnails; it is the obvious speed-up once the board measures where the
 * time goes.
 *
 * The engine's channel order for RGB565 is not something to guess: the first
 * time it is used it decodes a 16x16 picture of known colours
 * (aos_image_probe.h) and keeps whichever order - and byte order - gives red
 * where red is.
 */
#include "aos_hal.h"
#include "aos_image_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/jpeg_decode.h"
#include "esp_jpeg_dec.h"
#include "png.h"

static const char *TAG = "image";

#define HW_MAX_PIXELS   (4u * 1024 * 1024)
#define FILE_MAX_BYTES  (16u * 1024 * 1024)
#define PNG_MAX_PIXELS  (16u * 1024 * 1024)
#define PROG_MAX_PIXELS (2500u * 1000)  /* ~15 MB of PSRAM at the peak */

static SemaphoreHandle_t s_mx;          /* one decode at a time: the engine and the memory */
static jpeg_decoder_handle_t s_hw;
static bool s_hw_failed;
static jpeg_dec_rgb_element_order_t s_hw_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
static bool s_hw_swap;                  /* the bytes of each pixel come swapped */
static bool s_hw_probed;

static void *psram(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

/* Progressive JPEG only; everything it takes comes from PSRAM. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC(n)      heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC(p, n)  heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p)        heap_caps_free(p)
#include "src/libs/gltf/stb_image/stb_image.h"

void aos_hal_image_free(void *px) { heap_caps_free(px); }

/* -------------------------------------------------------------------------- */
/* The resampler                                                               */
/* -------------------------------------------------------------------------- */

typedef enum { SRC_RGB565, SRC_RGB888, SRC_RGBA8888 } src_fmt_t;

static inline void src_px(const uint8_t *s, src_fmt_t f, int *r, int *g, int *b)
{
    if (f == SRC_RGB565) {
        uint16_t v = (uint16_t)(s[0] | s[1] << 8);
        *r = (v >> 8) & 0xF8; *r |= *r >> 5;
        *g = (v >> 3) & 0xFC; *g |= *g >> 6;
        *b = (v << 3) & 0xF8; *b |= *b >> 5;
    } else {
        *r = s[0]; *g = s[1]; *b = s[2];
    }
}

static inline uint16_t rgb565(int r, int g, int b) { return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3); }

/* The rectangle (cx, cy, cw, ch) of the source to dw x dh. */
static void resample(const uint8_t *src, src_fmt_t f, int stride, int cx, int cy, int cw, int ch,
                     uint16_t *dst, int dw, int dh)
{
    int bpp = f == SRC_RGB565 ? 2 : f == SRC_RGB888 ? 3 : 4;
    if (cw >= dw && ch >= dh) {
        /* shrinking: each output pixel is the average of its source area */
        for (int y = 0; y < dh; y++) {
            int y0 = cy + (int)((int64_t)y * ch / dh), y1 = cy + (int)((int64_t)(y + 1) * ch / dh);
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                int x0 = cx + (int)((int64_t)x * cw / dw), x1 = cx + (int)((int64_t)(x + 1) * cw / dw);
                if (x1 <= x0) x1 = x0 + 1;
                /* on very large areas, every other row and column is plenty */
                int step = (x1 - x0) * (y1 - y0) > 64 ? 2 : 1;
                uint32_t sr = 0, sg = 0, sb = 0, n = 0;
                for (int yy = y0; yy < y1; yy += step) {
                    const uint8_t *row = src + (size_t)yy * stride;
                    for (int xx = x0; xx < x1; xx += step) {
                        int r, g, b;
                        src_px(row + (size_t)xx * bpp, f, &r, &g, &b);
                        sr += r; sg += g; sb += b; n++;
                    }
                }
                dst[(size_t)y * dw + x] = rgb565((int)(sr / n), (int)(sg / n), (int)(sb / n));
            }
        }
        return;
    }
    /* enlarging (in one direction at least): bilinear, 8-bit fractions */
    for (int y = 0; y < dh; y++) {
        int fy = (int)(((int64_t)(2 * y + 1) * ch * 128) / dh) - 128;       /* centre to centre */
        if (fy < 0) fy = 0;
        int yi = fy >> 8, yw = fy & 255;
        if (yi >= ch - 1) { yi = ch - 1; yw = 0; }
        const uint8_t *r0 = src + (size_t)(cy + yi) * stride, *r1 = src + (size_t)(cy + (yi + 1 < ch ? yi + 1 : yi)) * stride;
        for (int x = 0; x < dw; x++) {
            int fx = (int)(((int64_t)(2 * x + 1) * cw * 128) / dw) - 128;
            if (fx < 0) fx = 0;
            int xi = fx >> 8, xw = fx & 255;
            if (xi >= cw - 1) { xi = cw - 1; xw = 0; }
            int xa = cx + xi, xb = cx + (xi + 1 < cw ? xi + 1 : xi);
            int c[4][3];
            src_px(r0 + (size_t)xa * bpp, f, &c[0][0], &c[0][1], &c[0][2]);
            src_px(r0 + (size_t)xb * bpp, f, &c[1][0], &c[1][1], &c[1][2]);
            src_px(r1 + (size_t)xa * bpp, f, &c[2][0], &c[2][1], &c[2][2]);
            src_px(r1 + (size_t)xb * bpp, f, &c[3][0], &c[3][1], &c[3][2]);
            int o[3];
            for (int k = 0; k < 3; k++) {
                int top = c[0][k] * (256 - xw) + c[1][k] * xw, bot = c[2][k] * (256 - xw) + c[3][k] * xw;
                o[k] = (top * (256 - yw) + bot * yw) >> 16;
            }
            dst[(size_t)y * dw + x] = rgb565(o[0], o[1], o[2]);
        }
    }
}

/* The output size and the part of the source it comes from. */
static void geometry(int sw, int sh, int max_w, int max_h, bool fill, int *dw, int *dh, int *cx, int *cy, int *cw, int *ch)
{
    *cx = *cy = 0;
    *cw = sw;
    *ch = sh;
    if (!fill) {
        /* fit inside, never enlarged */
        double s = (double)max_w / sw < (double)max_h / sh ? (double)max_w / sw : (double)max_h / sh;
        if (s > 1) s = 1;
        *dw = (int)(sw * s + 0.5);
        *dh = (int)(sh * s + 0.5);
        if (*dw < 1) *dw = 1;
        if (*dh < 1) *dh = 1;
        return;
    }
    /* cover the box, crop the centre of the source to its aspect */
    *dw = max_w;
    *dh = max_h;
    if ((int64_t)sw * max_h > (int64_t)sh * max_w) {
        *cw = (int)((int64_t)sh * max_w / max_h);
        *cx = (sw - *cw) / 2;
    } else {
        *ch = (int)((int64_t)sw * max_h / max_w);
        *cy = (sh - *ch) / 2;
    }
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
}

/* A source bitmap to the answer. The geometry is worked out on the picture's
 * own size (pw x ph) and mapped onto the bitmap (sw x sh), which may be a
 * pre-shrunk copy of it with its sides rounded to multiples of 8. */
static uint16_t *finish_as(const uint8_t *src, src_fmt_t f, int stride, int sw, int sh, int pw, int ph,
                           int max_w, int max_h, bool fill, int *out_w, int *out_h)
{
    int dw, dh, cx, cy, cw, ch;
    geometry(pw, ph, max_w, max_h, fill, &dw, &dh, &cx, &cy, &cw, &ch);
    if (sw != pw || sh != ph) {
        cx = (int)((int64_t)cx * sw / pw);
        cw = (int)((int64_t)cw * sw / pw);
        cy = (int)((int64_t)cy * sh / ph);
        ch = (int)((int64_t)ch * sh / ph);
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        if (cx + cw > sw) cw = sw - cx;
        if (cy + ch > sh) ch = sh - cy;
    }
    uint16_t *out = psram((size_t)dw * dh * 2);
    if (!out) return NULL;
    resample(src, f, stride, cx, cy, cw, ch, out, dw, dh);
    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
    return out;
}

static uint16_t *finish(const uint8_t *src, src_fmt_t f, int stride, int sw, int sh, int max_w, int max_h, bool fill,
                        int *out_w, int *out_h)
{
    return finish_as(src, f, stride, sw, sh, sw, sh, max_w, max_h, fill, out_w, out_h);
}

/* -------------------------------------------------------------------------- */
/* JPEG                                                                        */
/* -------------------------------------------------------------------------- */

/* Size and kind from the frame header, without decoding anything. */
static bool jpeg_header(const uint8_t *p, size_t n, int *w, int *h, bool *progressive)
{
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 9 < n) {
        if (p[i] != 0xFF) { i++; continue; }
        uint8_t m = p[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        size_t len = (size_t)p[i + 2] << 8 | p[i + 3];
        bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (sof) {
            *h = p[i + 5] << 8 | p[i + 6];
            *w = p[i + 7] << 8 | p[i + 8];
            *progressive = m != 0xC0 && m != 0xC1;      /* SOF0/SOF1: baseline / extended sequential */
            return *w > 0 && *h > 0;
        }
        if (m == 0xDA) return false;                    /* scan before a frame header: broken */
        i += 2 + len;
    }
    return false;
}

static uint16_t *hw_decode_raw(const uint8_t *data, size_t len, jpeg_dec_rgb_element_order_t order,
                               int w, int h, int *stride_px);

/* Which order and bytes the engine writes, from the probe picture. */
static void hw_probe(void)
{
    s_hw_probed = true;
    static const jpeg_dec_rgb_element_order_t ORDERS[2] = { JPEG_DEC_RGB_ELEMENT_ORDER_BGR, JPEG_DEC_RGB_ELEMENT_ORDER_RGB };
    for (int o = 0; o < 2; o++) {
        int stride = 0;
        size_t plen = 0;
        const uint8_t *pic = aos_jpeg_probe_picture(&plen);
        uint16_t *px = pic ? hw_decode_raw(pic, plen, ORDERS[o], AOS_PROBE_SIDE, AOS_PROBE_SIDE, &stride) : NULL;
        if (!px) { s_hw_failed = true; return; }
        uint16_t v = px[8 * stride + 8];                /* the left half is red */
        heap_caps_free(px);
        for (int swap = 0; swap < 2; swap++) {
            uint16_t c = swap ? (uint16_t)(v << 8 | v >> 8) : v;
            if ((c >> 11) >= 26 && ((c >> 5) & 63) < 12 && (c & 31) < 6) {
                s_hw_order = ORDERS[o];
                s_hw_swap = swap;
                ESP_LOGI(TAG, "JPEG engine: %s order%s", o ? "RGB" : "BGR", swap ? ", bytes swapped" : "");
                return;
            }
        }
    }
    ESP_LOGW(TAG, "JPEG engine: the probe came out in no known order; using software only");
    s_hw_failed = true;
}

/* The engine's whole-picture output, padded; *stride_px its row length. */
static uint16_t *hw_decode_raw(const uint8_t *data, size_t len, jpeg_dec_rgb_element_order_t order,
                               int w, int h, int *stride_px)
{
    if (!s_hw) {
        jpeg_decode_engine_cfg_t ec = { .timeout_ms = 1000 };
        if (jpeg_new_decoder_engine(&ec, &s_hw) != ESP_OK) { s_hw_failed = true; return NULL; }
    }
    /* the engine reads by DMA: the stream goes in a buffer of its own kind */
    jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t in_sz = 0, out_sz = 0;
    uint8_t *in = jpeg_alloc_decoder_mem(len, &in_cfg, &in_sz);
    int pw = (w + 15) & ~15, ph = (h + 15) & ~15;
    uint16_t *out = jpeg_alloc_decoder_mem((size_t)pw * ph * 2, &out_cfg, &out_sz);
    if (!in || !out) { heap_caps_free(in); heap_caps_free(out); return NULL; }
    memcpy(in, data, len);
    jpeg_decode_cfg_t dc = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, .rgb_order = order,
                             .conv_std = JPEG_YUV_RGB_CONV_STD_BT601 };
    uint32_t got = 0;
    esp_err_t e = jpeg_decoder_process(s_hw, &dc, in, (uint32_t)len, (uint8_t *)out, (uint32_t)out_sz, &got);
    heap_caps_free(in);
    if (e != ESP_OK) { heap_caps_free(out); return NULL; }
    /* padded rows or exact ones: the size it wrote says which */
    *stride_px = got >= (uint32_t)pw * h * 2 ? pw : w;
    return out;
}

static uint16_t *jpeg_hw(const uint8_t *data, size_t len, int w, int h, int max_w, int max_h, bool fill,
                         int *out_w, int *out_h)
{
    if (!s_hw_probed) hw_probe();
    if (s_hw_failed) return NULL;
    int stride = 0;
    uint16_t *raw = hw_decode_raw(data, len, s_hw_order, w, h, &stride);
    if (!raw) return NULL;
    if (s_hw_swap)
        for (size_t i = 0; i < (size_t)stride * h; i++) raw[i] = (uint16_t)(raw[i] << 8 | raw[i] >> 8);
    uint16_t *out = finish((const uint8_t *)raw, SRC_RGB565, stride * 2, w, h, max_w, max_h, fill, out_w, out_h);
    heap_caps_free(raw);
    return out;
}

static uint16_t *jpeg_sw(const uint8_t *data, size_t len, int w, int h, int max_w, int max_h, bool fill,
                         int *out_w, int *out_h)
{
    /* decode already shrunk: to multiples of 8, no smaller than 1/8 nor
     * than what the resampler needs to start from */
    int dw, dh, cx, cy, cw, ch;
    geometry(w, h, max_w, max_h, fill, &dw, &dh, &cx, &cy, &cw, &ch);
    double need = (double)dw / cw > (double)dh / ch ? (double)dw / cw : (double)dh / ch;   /* output per source px */
    if (need > 1) need = 1;
    int sw = (int)(w * need + 7) & ~7, sh = (int)(h * need + 7) & ~7;
    if (sw < ((w + 7) / 8 + 7) / 8 * 8) sw = ((w + 7) / 8 + 7) / 8 * 8;
    if (sh < ((h + 7) / 8 + 7) / 8 * 8) sh = ((h + 7) / 8 + 7) / 8 * 8;
    bool scale = sw < (w & ~7) && sh < (h & ~7);
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (scale) { cfg.scale.width = (uint16_t)sw; cfg.scale.height = (uint16_t)sh; }
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return NULL;
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)data, .inbuf_len = (int)len };
    jpeg_dec_header_info_t info;
    uint16_t *out = NULL;
    int olen = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &olen) == JPEG_ERR_OK) {
        uint8_t *buf = jpeg_calloc_align((size_t)olen, 16);
        if (buf) {
            io.outbuf = buf;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                int ow = scale ? sw : info.width, oh = scale ? sh : info.height;
                out = finish_as(buf, SRC_RGB565, ow * 2, ow, oh, w, h, max_w, max_h, fill, out_w, out_h);
            }
            jpeg_free_align(buf);
        }
    }
    jpeg_dec_close(dec);
    return out;
}

static uint16_t *jpeg_prog(const uint8_t *data, size_t len, int w, int h, int max_w, int max_h, bool fill,
                           int *out_w, int *out_h)
{
    if ((uint64_t)w * h > PROG_MAX_PIXELS) return NULL;
    int iw = 0, ih = 0, comp = 0;
    uint8_t *px = stbi_load_from_memory(data, (int)len, &iw, &ih, &comp, 3);
    if (!px) return NULL;
    uint16_t *out = finish(px, SRC_RGB888, iw * 3, iw, ih, max_w, max_h, fill, out_w, out_h);
    stbi_image_free(px);
    return out;
}

/* -------------------------------------------------------------------------- */
/* PNG and BMP                                                                 */
/* -------------------------------------------------------------------------- */

static uint16_t *png_decode(const uint8_t *data, size_t len, int max_w, int max_h, bool fill,
                            int *out_w, int *out_h, int *src_w, int *src_h)
{
    png_image img = { .version = PNG_IMAGE_VERSION };
    if (!png_image_begin_read_from_memory(&img, data, len)) return NULL;
    if ((uint64_t)img.width * img.height > PNG_MAX_PIXELS) { png_image_free(&img); return NULL; }
    img.format = PNG_FORMAT_RGB;                         /* alpha composited on black */
    uint8_t *px = psram(PNG_IMAGE_SIZE(img));
    png_color black = { 0, 0, 0 };
    uint16_t *out = NULL;
    if (px && png_image_finish_read(&img, &black, px, 0, NULL)) {
        if (src_w) *src_w = (int)img.width;
        if (src_h) *src_h = (int)img.height;
        out = finish(px, SRC_RGB888, (int)PNG_IMAGE_ROW_STRIDE(img), (int)img.width, (int)img.height, max_w, max_h, fill, out_w, out_h);
    }
    png_image_free(&img);
    heap_caps_free(px);
    return out;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Uncompressed 24- and 32-bit BMP, and 16-bit RGB565 (BI_BITFIELDS, the
 * BOOT button's screenshots), bottom-up or top-down. */
static uint16_t *bmp_decode(const uint8_t *d, size_t len, int max_w, int max_h, bool fill,
                            int *out_w, int *out_h, int *src_w, int *src_h)
{
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    uint32_t off = le32(d + 10);
    int w = (int)le32(d + 18), h = (int)le32(d + 22), bpp = d[28] | d[29] << 8;
    uint32_t comp = le32(d + 30);
    bool down = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || !h || (bpp != 16 && bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) return NULL;
    if (bpp == 16 && comp != 3) return NULL;           /* 16 bits only as RGB565 with its masks */
    int stride = ((w * bpp / 8) + 3) & ~3;
    if (off + (size_t)stride * h > len) return NULL;
    /* BGR rows -> RGB888 top-down, then the common path */
    uint8_t *rgb = psram((size_t)w * h * 3);
    if (!rgb) return NULL;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = d + off + (size_t)(down ? h - 1 - y : y) * stride;
        uint8_t *o = rgb + (size_t)y * w * 3;
        if (bpp == 16) {
            for (int x = 0; x < w; x++, s += 2, o += 3) {
                uint16_t v = (uint16_t)(s[0] | s[1] << 8);
                o[0] = (uint8_t)((v >> 8) & 0xF8);
                o[1] = (uint8_t)((v >> 3) & 0xFC);
                o[2] = (uint8_t)(v << 3);
            }
            continue;
        }
        for (int x = 0; x < w; x++, s += bpp / 8, o += 3) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; }
    }
    if (src_w) *src_w = w;
    if (src_h) *src_h = h;
    uint16_t *out = finish(rgb, SRC_RGB888, w * 3, w, h, max_w, max_h, fill, out_w, out_h);
    heap_caps_free(rgb);
    return out;
}

/* -------------------------------------------------------------------------- */
/* The HAL call                                                                */
/* -------------------------------------------------------------------------- */

static uint8_t *read_range(const char *path, uint32_t offset, uint32_t size, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (!size) {
        struct stat st;
        if (stat(path, &st) || st.st_size <= (off_t)offset) { fclose(f); return NULL; }
        size = (uint32_t)(st.st_size - offset);
    }
    if (size > FILE_MAX_BYTES || fseek(f, (long)offset, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *b = psram(size);
    if (b && fread(b, 1, size, f) != size) { heap_caps_free(b); b = NULL; }
    fclose(f);
    *len = size;
    return b;
}

uint16_t *aos_hal_image_decode(const char *path, uint32_t offset, uint32_t size,
                               int max_w, int max_h, bool fill,
                               int *out_w, int *out_h, int *src_w, int *src_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!path || max_w <= 0 || max_h <= 0) return NULL;
    if (!s_mx) {
        static StaticSemaphore_t buf;
        s_mx = xSemaphoreCreateMutexStatic(&buf);       /* first call: the loader thread, before any other */
    }
    size_t len = 0;
    uint8_t *data = read_range(path, offset, size, &len);
    if (!data) return NULL;
    int64_t t0 = esp_timer_get_time();
    uint16_t *out = NULL;
    const char *how = "";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int w = 0, h = 0;
    bool prog = false;
    if (jpeg_header(data, len, &w, &h, &prog)) {
        if (src_w) *src_w = w;
        if (src_h) *src_h = h;
        if (prog) {
            out = jpeg_prog(data, len, w, h, max_w, max_h, fill, out_w, out_h);
            how = "progressive";
        } else {
            if ((uint64_t)w * h <= HW_MAX_PIXELS) { out = jpeg_hw(data, len, w, h, max_w, max_h, fill, out_w, out_h); how = "engine"; }
            if (!out) { out = jpeg_sw(data, len, w, h, max_w, max_h, fill, out_w, out_h); how = "software"; }
        }
    } else if (len > 8 && !memcmp(data, "\x89PNG", 4)) {
        out = png_decode(data, len, max_w, max_h, fill, out_w, out_h, src_w, src_h);
        how = "png";
    } else {
        out = bmp_decode(data, len, max_w, max_h, fill, out_w, out_h, src_w, src_h);
        how = "bmp";
    }
    xSemaphoreGive(s_mx);
    heap_caps_free(data);
    ESP_LOGD(TAG, "%s: %s -> %dx%d in %lld ms", path, how, out_w ? *out_w : 0, out_h ? *out_h : 0,
             (esp_timer_get_time() - t0) / 1000);
    if (!out && prog) ESP_LOGI(TAG, "%s: progressive JPEG of %dx%d, too big or broken", path, w, h);
    return out;
}
