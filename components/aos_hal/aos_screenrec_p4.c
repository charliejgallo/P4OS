/*
 * P4OS - recording the screen to an MP4, with the P4's own hardware
 * (2026-10-22).
 *
 * The work is what the chip has engines for, and none of it is the CPU's:
 *
 *   the frame on the panel (aos_hal_display_fb: what LVGL drew and what the
 *   apps blitted or flipped, games and cameras included)
 *     -> the PPA: RGB565 to the YUV420 the encoder reads, turned on the way
 *        when the screen lies down
 *     -> the H.264 encoder (esp_h264's hardware one, 1920 wide at most)
 *     -> an MP4 written here: the encoder's Annex B NAL units become
 *        length-prefixed samples in 'mdat', and 'moov' goes at the end with
 *        the sample tables, so a Mac or a phone plays it as it is.
 *
 * One thread, at a steady rate (fps): each frame takes what the panel shows
 * at that moment; a frame that comes late makes the previous one last
 * longer (the sample durations are measured, not assumed). The orientation
 * is the one at the start. Buffers in PSRAM; the encoder asks for a few KB
 * of internal RAM for its DMA while it runs.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_h264_enc_single_hw.h"
#include "esp_h264_enc_single.h"

bool aos_h264_enc_take(void);       /* aos_h264_enc_p4.c */
void aos_h264_enc_give(void);

#define TAG      "screenrec"
#define LINE     128
#define TIMESCALE 1000                  /* sample times in ms */

/* ---- the MP4 ---------------------------------------------------------------- */

typedef struct {
    FILE     *f;
    uint32_t  mdat_at;                  /* where 'mdat' starts */
    uint32_t  pos;                      /* bytes written */
    uint32_t  n, cap;                   /* samples */
    uint32_t *size, *off, *dur;
    uint8_t  *key;
    uint8_t   sps[64], pps[32];
    int       sps_len, pps_len;
    int       w, h;
    bool      err;
} mp4_t;

static void wr(mp4_t *m, const void *p, size_t n)
{
    if (m->err) return;
    if (fwrite(p, 1, n, m->f) != n) m->err = true;
    m->pos += (uint32_t)n;
}

static void u32(mp4_t *m, uint32_t v) { uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; wr(m, b, 4); }
static void u16(mp4_t *m, uint16_t v) { uint8_t b[2] = { v >> 8, v }; wr(m, b, 2); }
static void u8_(mp4_t *m, uint8_t v) { wr(m, &v, 1); }
static void tag(mp4_t *m, const char *t) { wr(m, t, 4); }
static void zeros(mp4_t *m, int n) { while (n-- > 0) u8_(m, 0); }

/* A box: its size is written once its end is known */
static long box_open(mp4_t *m, const char *t)
{
    long at = ftell(m->f);
    u32(m, 0);
    tag(m, t);
    return at;
}

static void box_close(mp4_t *m, long at)
{
    long end = ftell(m->f);
    if (at < 0 || end < 0) { m->err = true; return; }
    fseek(m->f, at, SEEK_SET);
    uint32_t sz = (uint32_t)(end - at);
    uint8_t b[4] = { sz >> 24, sz >> 16, sz >> 8, sz };
    if (fwrite(b, 1, 4, m->f) != 4) m->err = true;
    fseek(m->f, end, SEEK_SET);
}

static bool mp4_open(mp4_t *m, const char *path, int w, int h)
{
    memset(m, 0, sizeof *m);
    m->f = fopen(path, "wb");
    if (!m->f) return false;
    m->w = w;
    m->h = h;
    long b = box_open(m, "ftyp");
    tag(m, "isom");
    u32(m, 0x200);
    tag(m, "isom"); tag(m, "iso2"); tag(m, "avc1"); tag(m, "mp41");
    box_close(m, b);
    m->mdat_at = (uint32_t)ftell(m->f);
    u32(m, 0);                          /* the size, at the end */
    tag(m, "mdat");
    m->pos = m->mdat_at + 8;
    return !m->err;
}

static bool grow(mp4_t *m)
{
    if (m->n < m->cap) return true;
    uint32_t c = m->cap ? m->cap * 2 : 1024;
    uint32_t *a = heap_caps_realloc(m->size, c * 4, MALLOC_CAP_SPIRAM);
    if (!a) return false;
    m->size = a;
    if (!(a = heap_caps_realloc(m->off, c * 4, MALLOC_CAP_SPIRAM))) return false;
    m->off = a;
    if (!(a = heap_caps_realloc(m->dur, c * 4, MALLOC_CAP_SPIRAM))) return false;
    m->dur = a;
    uint8_t *k = heap_caps_realloc(m->key, c, MALLOC_CAP_SPIRAM);
    if (!k) return false;
    m->key = k;
    m->cap = c;
    return true;
}

/* One encoded frame, Annex B (start codes): parameter sets kept for 'avcC',
 * the rest written length-prefixed as one sample */
static bool mp4_frame(mp4_t *m, const uint8_t *p, uint32_t len, bool key)
{
    if (!grow(m)) return false;
    uint32_t start = m->pos, i = 0;
    while (i + 3 < len) {
        /* the next start code */
        uint32_t s = i;
        while (s + 3 < len && !(p[s] == 0 && p[s + 1] == 0 && (p[s + 2] == 1 || (p[s + 2] == 0 && p[s + 3] == 1)))) s++;
        if (s + 3 >= len) break;
        uint32_t nal = s + (p[s + 2] == 1 ? 3 : 4);
        uint32_t e = nal;
        while (e + 3 < len && !(p[e] == 0 && p[e + 1] == 0 && (p[e + 2] == 1 || (p[e + 2] == 0 && p[e + 3] == 1)))) e++;
        if (e + 3 >= len) e = len;
        uint32_t nl = e - nal;
        int type = nl ? p[nal] & 0x1F : 0;
        if (type == 7 && nl <= sizeof m->sps) { memcpy(m->sps, p + nal, nl); m->sps_len = (int)nl; }
        else if (type == 8 && nl <= sizeof m->pps) { memcpy(m->pps, p + nal, nl); m->pps_len = (int)nl; }
        else if (nl) { u32(m, nl); wr(m, p + nal, nl); }
        i = e;
    }
    if (m->pos == start) return !m->err;    /* parameter sets only */
    m->size[m->n] = m->pos - start;
    m->off[m->n] = start;
    m->dur[m->n] = 0;
    m->key[m->n] = key;
    m->n++;
    return !m->err;
}

static bool mp4_close(mp4_t *m, uint32_t last_dur)
{
    if (m->n) m->dur[m->n - 1] = last_dur;
    uint64_t total = 0;
    for (uint32_t i = 0; i < m->n; i++) total += m->dur[i];
    /* mdat's size */
    long end = ftell(m->f);
    fseek(m->f, m->mdat_at, SEEK_SET);
    uint32_t ms = (uint32_t)(end - m->mdat_at);
    uint8_t b[4] = { ms >> 24, ms >> 16, ms >> 8, ms };
    if (fwrite(b, 1, 4, m->f) != 4) m->err = true;
    fseek(m->f, end, SEEK_SET);

    long moov = box_open(m, "moov");
    long x = box_open(m, "mvhd");
    u32(m, 0); u32(m, 0); u32(m, 0);            /* version/flags, created, modified */
    u32(m, TIMESCALE); u32(m, (uint32_t)total);
    u32(m, 0x00010000); u16(m, 0x0100); zeros(m, 10);
    u32(m, 0x00010000); zeros(m, 12); u32(m, 0x00010000); zeros(m, 12); u32(m, 0x40000000);
    zeros(m, 24); u32(m, 2);
    box_close(m, x);
    long trak = box_open(m, "trak");
    x = box_open(m, "tkhd");
    u32(m, 3); u32(m, 0); u32(m, 0); u32(m, 1); u32(m, 0); u32(m, (uint32_t)total);
    zeros(m, 8); u16(m, 0); u16(m, 0); u16(m, 0); u16(m, 0);
    u32(m, 0x00010000); zeros(m, 12); u32(m, 0x00010000); zeros(m, 12); u32(m, 0x40000000);
    u32(m, (uint32_t)m->w << 16); u32(m, (uint32_t)m->h << 16);
    box_close(m, x);
    long mdia = box_open(m, "mdia");
    x = box_open(m, "mdhd");
    u32(m, 0); u32(m, 0); u32(m, 0); u32(m, TIMESCALE); u32(m, (uint32_t)total); u16(m, 0x55C4); u16(m, 0);
    box_close(m, x);
    x = box_open(m, "hdlr");
    u32(m, 0); u32(m, 0); tag(m, "vide"); zeros(m, 12); wr(m, "P4OS screen\0", 12);
    box_close(m, x);
    long minf = box_open(m, "minf");
    x = box_open(m, "vmhd");
    u32(m, 1); zeros(m, 8);
    box_close(m, x);
    long dinf = box_open(m, "dinf");
    x = box_open(m, "dref");
    u32(m, 0); u32(m, 1);
    long url = box_open(m, "url ");
    u32(m, 1);
    box_close(m, url);
    box_close(m, x);
    box_close(m, dinf);
    long stbl = box_open(m, "stbl");
    x = box_open(m, "stsd");
    u32(m, 0); u32(m, 1);
    long avc1 = box_open(m, "avc1");
    zeros(m, 6); u16(m, 1); zeros(m, 16);
    u16(m, (uint16_t)m->w); u16(m, (uint16_t)m->h);
    u32(m, 0x00480000); u32(m, 0x00480000); u32(m, 0); u16(m, 1);
    zeros(m, 32); u16(m, 0x18); u16(m, 0xFFFF);
    long avcc = box_open(m, "avcC");
    u8_(m, 1);
    u8_(m, m->sps_len > 3 ? m->sps[1] : 66);
    u8_(m, m->sps_len > 3 ? m->sps[2] : 0);
    u8_(m, m->sps_len > 3 ? m->sps[3] : 31);
    u8_(m, 0xFF);                               /* 4-byte lengths */
    u8_(m, 0xE1); u16(m, (uint16_t)m->sps_len); wr(m, m->sps, m->sps_len);
    u8_(m, 1); u16(m, (uint16_t)m->pps_len); wr(m, m->pps, m->pps_len);
    box_close(m, avcc);
    box_close(m, avc1);
    box_close(m, x);
    /* stts: runs of equal durations */
    x = box_open(m, "stts");
    u32(m, 0);
    long cnt_at = ftell(m->f);
    u32(m, 0);
    uint32_t runs = 0;
    for (uint32_t i = 0; i < m->n;) {
        uint32_t j = i;
        while (j < m->n && m->dur[j] == m->dur[i]) j++;
        u32(m, j - i); u32(m, m->dur[i]);
        runs++;
        i = j;
    }
    long here = ftell(m->f);
    fseek(m->f, cnt_at, SEEK_SET);
    uint8_t rb[4] = { runs >> 24, runs >> 16, runs >> 8, runs };
    if (fwrite(rb, 1, 4, m->f) != 4) m->err = true;
    fseek(m->f, here, SEEK_SET);
    box_close(m, x);
    x = box_open(m, "stss");
    u32(m, 0);
    uint32_t keys = 0;
    for (uint32_t i = 0; i < m->n; i++) keys += m->key[i];
    u32(m, keys);
    for (uint32_t i = 0; i < m->n; i++) if (m->key[i]) u32(m, i + 1);
    box_close(m, x);
    x = box_open(m, "stsc");
    u32(m, 0); u32(m, 1); u32(m, 1); u32(m, 1); u32(m, 1);
    box_close(m, x);
    x = box_open(m, "stsz");
    u32(m, 0); u32(m, 0); u32(m, m->n);
    for (uint32_t i = 0; i < m->n; i++) u32(m, m->size[i]);
    box_close(m, x);
    x = box_open(m, "stco");
    u32(m, 0); u32(m, m->n);
    for (uint32_t i = 0; i < m->n; i++) u32(m, m->off[i]);
    box_close(m, x);
    box_close(m, stbl);
    box_close(m, minf);
    box_close(m, mdia);
    box_close(m, trak);
    box_close(m, moov);
    bool ok = !m->err && fclose(m->f) == 0;
    heap_caps_free(m->size);
    heap_caps_free(m->off);
    heap_caps_free(m->dur);
    heap_caps_free(m->key);
    memset(m, 0, sizeof *m);
    return ok;
}

/* ---- the recorder ------------------------------------------------------------- */

static struct {
    volatile bool on, stop;
    char          path[192];
    int           fps;
    uint64_t      t0_ms;
    volatile uint32_t frames;
    volatile uint64_t bytes;
    volatile bool done_ok;
} R;

static void rec_task(void *arg)
{
    (void)arg;
    int rot = aos_hal_display_get_rotation();
    bool side = rot == 90 || rot == 270;
    int w = side ? AOS_PANEL_H : AOS_PANEL_W, h = side ? AOS_PANEL_W : AOS_PANEL_H;
    size_t yuv_n = ((size_t)w * h * 3 / 2 + LINE - 1) & ~(size_t)(LINE - 1);
    size_t out_n = ((size_t)w * h + LINE - 1) & ~(size_t)(LINE - 1);
    uint8_t *yuv = heap_caps_aligned_calloc(LINE, 1, yuv_n, MALLOC_CAP_SPIRAM);
    uint8_t *out = heap_caps_aligned_calloc(LINE, 1, out_n, MALLOC_CAP_SPIRAM);
    ppa_client_handle_t srm = NULL;
    esp_h264_enc_handle_t enc = NULL;
    mp4_t m;
    bool mp4 = false, ok = false;
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1,
                               .data_burst_length = PPA_DATA_BURST_LENGTH_128 };
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = (uint8_t)(R.fps * 2),
        .fps = (uint8_t)R.fps,
        .res = { .width = (uint16_t)w, .height = (uint16_t)h },
        .rc = { .bitrate = 3 * 1000 * 1000, .qp_min = 22, .qp_max = 38 },
    };
    /* one hardware encoder for the board: an app may hold it (aos_h264_enc_p4.c) */
    bool taken = aos_h264_enc_take();
    if (!taken) {
        ESP_LOGE(TAG, "the H.264 encoder is taken by an app");
    } else if (!yuv || !out || ppa_register_client(&pc, &srm) != ESP_OK) {
        ESP_LOGE(TAG, "no memory or no PPA");
    } else if (esp_h264_enc_hw_new(&cfg, &enc) != ESP_H264_ERR_OK || esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "the H.264 encoder did not start (%dx%d)", w, h);
    } else if (!(mp4 = mp4_open(&m, R.path, w, h))) {
        ESP_LOGE(TAG, "%s: cannot write it", R.path);
    } else {
        ok = true;
        aos_hal_log("screenrec", "recording %dx%d at %d fps to %s", w, h, R.fps, R.path);
    }
    uint32_t period = 1000 / (R.fps > 0 ? R.fps : 15), prev_ms = 0, enc_us = 0;
    while (ok && !R.stop) {
        uint64_t t = aos_hal_uptime_ms();
        const uint16_t *fb = NULL;
        int fw = 0, fh = 0;
        if (!aos_hal_display_fb(&fb, &fw, &fh) || !fb) break;
        uint64_t u0 = aos_hal_uptime_us();
        ppa_srm_oper_config_t c = {
            .in = { .buffer = fb, .pic_w = (uint32_t)fw, .pic_h = (uint32_t)fh, .block_w = (uint32_t)fw,
                    .block_h = (uint32_t)fh, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .out = { .buffer = yuv, .buffer_size = (uint32_t)yuv_n, .pic_w = (uint32_t)w, .pic_h = (uint32_t)h,
                     .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = PPA_COLOR_RANGE_LIMIT,
                     .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601 },
            /* the panel is portrait; the screen turned 90 shows it turned
             * back the other way */
            .rotation_angle = rot == 90 ? PPA_SRM_ROTATION_ANGLE_270 : rot == 270 ? PPA_SRM_ROTATION_ANGLE_90
                            : rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 : PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(srm, &c) != ESP_OK) { ESP_LOGE(TAG, "PPA refused the frame"); break; }
        uint32_t ms = (uint32_t)(t - R.t0_ms);
        esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = yuv, .len = (uint32_t)((size_t)w * h * 3 / 2) }, .pts = ms };
        esp_h264_enc_out_frame_t o = { .raw_data = { .buffer = out, .len = (uint32_t)out_n } };
        if (esp_h264_enc_process(enc, &in, &o) != ESP_H264_ERR_OK) { ESP_LOGE(TAG, "the encoder refused a frame"); break; }
        enc_us += (uint32_t)(aos_hal_uptime_us() - u0);
        if (m.n) m.dur[m.n - 1] = ms - prev_ms;        /* the last one lasted until now */
        prev_ms = ms;
        bool key = o.frame_type == ESP_H264_FRAME_TYPE_IDR || o.frame_type == ESP_H264_FRAME_TYPE_I;
        if (!mp4_frame(&m, out, o.length, key)) { ESP_LOGE(TAG, "the card refused the frame"); break; }
        R.frames++;
        R.bytes = m.pos;
        uint32_t spent = (uint32_t)(aos_hal_uptime_ms() - t);
        if (spent < period) vTaskDelay(pdMS_TO_TICKS(period - spent));
    }
    if (mp4) {
        R.done_ok = mp4_close(&m, period);
        aos_hal_log("screenrec", "%s: %u frames, %u KB, %u ms a frame by the PPA and the encoder%s",
                    R.path, (unsigned)R.frames, (unsigned)(R.bytes / 1024),
                    R.frames ? (unsigned)(enc_us / 1000 / R.frames) : 0, R.done_ok ? "" : " - NOT closed well");
    }
    if (enc) { esp_h264_enc_close(enc); esp_h264_enc_del(enc); }
    if (taken) aos_h264_enc_give();
    if (srm) ppa_unregister_client(srm);
    heap_caps_free(yuv);
    heap_caps_free(out);
    R.on = false;
}

bool aos_hal_screenrec_start(const char *path, int fps)
{
    if (R.on) return false;
    if (fps < 1 || fps > 30) fps = 15;
    char def[96];
    if (!path || !path[0]) {
        /* the card's videos folder, where the Video app looks too */
        const char *root = aos_hal_path_sd_root();
        if (!root) return false;
        snprintf(def, sizeof def, "%s/videos", root);
        mkdir(def, 0777);
        struct tm t;
        aos_hal_time_now(&t);
        if (aos_hal_time_is_valid())
            snprintf(def, sizeof def, "%s/videos/P4OS_%04d%02d%02d_%02d%02d%02d.mp4", root, t.tm_year + 1900,
                     t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        else
            snprintf(def, sizeof def, "%s/videos/P4OS_up%lu.mp4", root, (unsigned long)(aos_hal_uptime_ms() / 1000));
        path = def;
    }
    memset((void *)&R, 0, sizeof R);
    snprintf(R.path, sizeof R.path, "%s", path);
    R.fps = fps;
    R.t0_ms = aos_hal_uptime_ms();
    R.on = true;
    /* a stack in PSRAM: the work is the engines', the thread mostly waits */
    if (!aos_hal_thread_start("screenrec", rec_task, NULL, 8192, 3)) {
        R.on = false;
        return false;
    }
    return true;
}

void aos_hal_screenrec_stop(void)
{
    R.stop = true;
}

const char *aos_hal_screenrec_file(void) { return R.path; }
bool aos_hal_screenrec_done_ok(void) { return R.done_ok; }

bool aos_hal_screenrec_state(uint32_t *ms, uint32_t *frames, uint64_t *bytes)
{
    if (ms) *ms = R.on ? (uint32_t)(aos_hal_uptime_ms() - R.t0_ms) : 0;
    if (frames) *frames = R.frames;
    if (bytes) *bytes = R.bytes;
    return R.on;
}
