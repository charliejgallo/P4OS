/*
 * P4OS HAL - JPEG frames in memory, for the Cameras app (aos_hal_jpeg_*).
 *
 * aos_image_p4.c decodes a file once, allocating as it goes; a camera hands
 * over 10-25 JPEGs a second from a socket. This keeps, per stream, an engine
 * handle and two DMA buffers (the stream in, the pixels out) that only grow,
 * so a frame costs the engine and nothing else.
 *
 *   baseline JPEG      the P4's JPEG engine. Its output is padded to 16 px
 *                      for 4:2:0 (the stride says so, from the byte count
 *                      the engine reports). The channel and byte order are
 *                      learnt once from the same 16x16 probe picture
 *                      aos_image_p4.c uses (aos_image_probe.h).
 *   anything else      esp_new_jpeg in software, unscaled (progressive,
 *                      odd sampling, or the engine failing on a frame).
 *
 * save() is esp_new_jpeg's encoder, from RGB565 little-endian: snapshots are
 * one frame now and then, so the hardware encoder's buffer rules are not
 * worth it here.
 *
 * Only the board confirms: the engine's speed on camera frames (it is rated
 * for 1080p at 30 fps), and that its padding rule is the one assumed.
 * The simulator's twin is sim/jpeg_sim.c.
 */
#include "aos_hal.h"
#include "aos_image_probe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/jpeg_decode.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"

static const char *TAG = "camjpeg";

#define MAX_SIDE 4096

struct aos_jpeg {
    jpeg_decoder_handle_t hw;
    bool                  hw_failed;
    uint8_t              *in;           /* the stream, where the engine's DMA reads it */
    size_t                in_cap;
    uint16_t             *out;          /* the pixels, from jpeg_alloc_decoder_mem or jpeg_calloc_align */
    size_t                out_cap;
    bool                  out_sw;       /* 'out' came from jpeg_calloc_align */
};

/* Shared by every handle: what the probe said. */
static bool s_probed, s_probe_failed, s_swap;
static jpeg_dec_rgb_element_order_t s_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

/* Width, height and whether it is progressive, from the frame header. */
static bool sof(const uint8_t *p, size_t n, int *w, int *h, bool *progressive)
{
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    for (size_t i = 2; i + 9 < n; ) {
        if (p[i] != 0xFF) return false;
        int m = p[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        size_t len = (size_t)p[i + 2] << 8 | p[i + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = p[i + 5] << 8 | p[i + 6];
            *w = p[i + 7] << 8 | p[i + 8];
            *progressive = m != 0xC0 && m != 0xC1;
            return *w > 0 && *h > 0;
        }
        if (m == 0xDA) return false;
        i += 2 + len;
    }
    return false;
}

static void free_out(aos_jpeg_t *j)
{
    if (!j->out) return;
    if (j->out_sw) jpeg_free_align(j->out);
    else heap_caps_free(j->out);
    j->out = NULL;
    j->out_cap = 0;
}

/* The engine's decode into j->out. false when it refuses. */
static bool hw_run(aos_jpeg_t *j, const uint8_t *data, size_t len, jpeg_dec_rgb_element_order_t order,
                   int w, int h, int *stride_px)
{
    if (j->hw_failed) return false;
    if (!j->hw) {
        jpeg_decode_engine_cfg_t ec = { .timeout_ms = 500 };
        if (jpeg_new_decoder_engine(&ec, &j->hw) != ESP_OK) { j->hw_failed = true; return false; }
    }
    if (len > j->in_cap) {
        heap_caps_free(j->in);
        jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
        size_t got = 0;
        j->in = jpeg_alloc_decoder_mem(len + len / 4, &in_cfg, &got);
        j->in_cap = j->in ? got : 0;
        if (!j->in) return false;
    }
    int pw = (w + 15) & ~15, ph = (h + 15) & ~15;
    size_t need = (size_t)pw * ph * 2;
    if (need > j->out_cap || j->out_sw) {
        free_out(j);
        jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        size_t got = 0;
        j->out = jpeg_alloc_decoder_mem(need, &out_cfg, &got);
        j->out_cap = j->out ? got : 0;
        j->out_sw = false;
        if (!j->out) return false;
    }
    memcpy(j->in, data, len);
    jpeg_decode_cfg_t dc = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, .rgb_order = order,
                             .conv_std = JPEG_YUV_RGB_CONV_STD_BT601 };
    uint32_t wrote = 0;
    if (jpeg_decoder_process(j->hw, &dc, j->in, (uint32_t)len, (uint8_t *)j->out, (uint32_t)j->out_cap, &wrote) != ESP_OK)
        return false;
    *stride_px = wrote >= (uint32_t)pw * h * 2 ? pw : w;
    return true;
}

const uint8_t *aos_jpeg_probe_picture(size_t *len)
{
    static uint8_t *jpg;
    static int n;
    if (!jpg) {
        const int side = AOS_PROBE_SIDE;
        uint16_t *px = heap_caps_malloc((size_t)side * side * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *out = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
        cfg.width = side;
        cfg.height = side;
        cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        cfg.subsampling = JPEG_SUBSAMPLE_420;
        cfg.quality = 90;
        jpeg_enc_handle_t enc = NULL;
        if (px && out && jpeg_enc_open(&cfg, &enc) == JPEG_ERR_OK) {
            for (int y = 0; y < side; y++)
                for (int x = 0; x < side; x++) px[y * side + x] = x < side / 2 ? 0xF800 : 0x001F;
            if (jpeg_enc_process(enc, (const uint8_t *)px, side * side * 2, out, 16 * 1024, &n) != JPEG_ERR_OK) n = 0;
            jpeg_enc_close(enc);
        }
        heap_caps_free(px);
        if (n > 0) jpg = out;
        else heap_caps_free(out);
    }
    if (len) *len = jpg ? (size_t)n : 0;
    return jpg;
}

static void probe(aos_jpeg_t *j)
{
    s_probed = true;
    static const jpeg_dec_rgb_element_order_t ORDERS[2] = { JPEG_DEC_RGB_ELEMENT_ORDER_BGR, JPEG_DEC_RGB_ELEMENT_ORDER_RGB };
    for (int o = 0; o < 2; o++) {
        int stride = 0;
        size_t plen = 0;
        const uint8_t *pic = aos_jpeg_probe_picture(&plen);
        if (!pic || !hw_run(j, pic, plen, ORDERS[o], AOS_PROBE_SIDE, AOS_PROBE_SIDE, &stride)) break;
        uint16_t v = j->out[8 * stride + 8];                    /* the left half is red */
        for (int swap = 0; swap < 2; swap++) {
            uint16_t c = swap ? (uint16_t)(v << 8 | v >> 8) : v;
            if ((c >> 11) >= 26 && ((c >> 5) & 63) < 12 && (c & 31) < 6) {
                s_order = ORDERS[o];
                s_swap = swap;
                ESP_LOGI(TAG, "engine: %s order%s", o ? "RGB" : "BGR", swap ? ", bytes swapped" : "");
                return;
            }
        }
    }
    ESP_LOGW(TAG, "engine: the probe failed; camera JPEGs go to software");
    s_probe_failed = true;
}

static const uint16_t *sw_run(aos_jpeg_t *j, const uint8_t *data, size_t len, int *w, int *h, int *stride_px)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return NULL;
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)data, .inbuf_len = (int)len, .inbuf_remain = (int)len };
    jpeg_dec_header_info_t info = { 0 };
    const uint16_t *res = NULL;
    int olen = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK &&
        jpeg_dec_get_outbuf_len(dec, &olen) == JPEG_ERR_OK && olen == info.width * info.height * 2) {
        if ((size_t)olen > j->out_cap || !j->out_sw) {
            free_out(j);
            j->out = jpeg_calloc_align((size_t)olen, 16);
            j->out_cap = j->out ? (size_t)olen : 0;
            j->out_sw = true;
        }
        if (j->out) {
            io.outbuf = (uint8_t *)j->out;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                *w = info.width;
                *h = info.height;
                *stride_px = info.width;
                res = j->out;
            }
        }
    }
    jpeg_dec_close(dec);
    return res;
}

aos_jpeg_t *aos_hal_jpeg_open(void)
{
    return heap_caps_calloc(1, sizeof(aos_jpeg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

const uint16_t *aos_hal_jpeg_decode(aos_jpeg_t *j, const void *data, size_t len,
                                    int *w, int *h, int *stride_px)
{
    int pw = 0, ph = 0;
    bool progressive = false;
    if (!j || !data || !sof(data, len, &pw, &ph, &progressive) || pw > MAX_SIDE || ph > MAX_SIDE) return NULL;
    if (!progressive) {
        if (!s_probed) probe(j);
        int stride = 0;
        if (!s_probe_failed && hw_run(j, data, len, s_order, pw, ph, &stride)) {
            if (s_swap)
                for (size_t i = 0; i < (size_t)stride * ph; i++) j->out[i] = (uint16_t)(j->out[i] << 8 | j->out[i] >> 8);
            *w = pw;
            *h = ph;
            *stride_px = stride;
            return j->out;
        }
    }
    return sw_run(j, data, len, w, h, stride_px);
}

void aos_hal_jpeg_close(aos_jpeg_t *j)
{
    if (!j) return;
    if (j->hw) jpeg_del_decoder_engine(j->hw);
    heap_caps_free(j->in);
    free_out(j);
    heap_caps_free(j);
}

static void make_parent(const char *path)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    mkdir(dir, 0755);
}

bool aos_hal_jpeg_save(const char *path, const uint16_t *px, int w, int h, int stride_px, int quality)
{
    if (!path || !px || w < 16 || h < 16 || stride_px < w) return false;
    w &= ~1;
    h &= ~1;
    const uint16_t *src = px;
    uint16_t *packed = NULL;
    if (stride_px != w) {           /* the encoder reads rows back to back */
        packed = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!packed) return false;
        for (int y = 0; y < h; y++) memcpy(packed + (size_t)y * w, px + (size_t)y * stride_px, (size_t)w * 2);
        src = packed;
    }
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = (uint8_t)(quality < 1 ? 1 : quality > 100 ? 100 : quality);
    bool ok = false;
    jpeg_enc_handle_t enc = NULL;
    size_t cap = (size_t)w * h + 64 * 1024;
    uint8_t *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out && jpeg_enc_open(&cfg, &enc) == JPEG_ERR_OK) {
        int n = 0;
        if (jpeg_enc_process(enc, (const uint8_t *)src, w * h * 2, out, (int)cap, &n) == JPEG_ERR_OK && n > 0) {
            make_parent(path);
            FILE *f = fopen(path, "wb");
            if (f) {
                ok = fwrite(out, 1, (size_t)n, f) == (size_t)n;
                ok = (fclose(f) == 0) && ok;
            }
        }
        jpeg_enc_close(enc);
    }
    heap_caps_free(out);
    heap_caps_free(packed);
    if (!ok) ESP_LOGW(TAG, "could not save %s", path);
    return ok;
}
