/*
 * LVGL port for the MIPI-DSI panel (HARDWARE.md tests 5, 6, 7).
 *
 * This is the prototype of the P4OS display port (PLAN.md D6), so it is
 * built to compare the options instead of picking one:
 *
 *   rot    0 / 90 / 180 / 270   logical orientation. LVGL renders in logical
 *                               coordinates; the flush rotates into the
 *                               portrait frame buffer.
 *   copy   cpu / ppa / dma2d    who moves the rendered area into the frame
 *                               buffer (dma2d only without rotation and with
 *                               one frame buffer: the driver copies into the
 *                               buffer it is scanning).
 *   fbs    1 / 2                one frame buffer (cheapest, may tear) or two
 *                               with a flip on the frame event and a copy of
 *                               the dirty areas into the new back buffer.
 *   rows   draw buffer height in rows of the logical width
 *   bmem   0 internal RAM / 1 PSRAM for the draw buffers
 *
 * All of them are NVS settings (`lvgl cfg ...`) applied on the next boot.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_console.h"
#include "driver/ppa.h"
#include "esp_async_fbcpy.h"
#include "esp_cache.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "lvgl.h"
#include "demos/lv_demos.h"
#include "bench.h"

static const char *TAG = "lvport";

static lv_display_t *s_disp;
/* only a copy flush_cb started gives LVGL its buffer back: a flip through
 * draw_bitmap reports "done" too */
static volatile bool s_async_pending;
static SemaphoreHandle_t s_lock;
static volatile bool s_paused;
static bool s_running;
static int s_rot, s_copy, s_nfb, s_rows, s_bmem;
static uint16_t *s_rot_tmp;        /* copy=dma2d turned: one chunk, rotated in internal RAM */
static size_t s_rot_tmp_bytes;
static int s_rotmode;              /* 0 CPU, 1 PPA, 2 CPU in tiles */
static int s_lw, s_lh;               /* logical size */
static int s_back;                   /* back buffer index when s_nfb == 2 */
static ppa_client_handle_t s_ppa;
static esp_lcd_touch_handle_t s_touch;

/* dirty areas of the current frame, in physical coordinates */
#define MAX_DIRTY 24
static lv_area_t s_dirty[MAX_DIRTY];
static int s_ndirty;
static bool s_dirty_full;

/* flush statistics for `lvgl stats` */
static uint32_t s_flushes, s_frames;
static int64_t s_flush_us, s_sync_us;

/* ---- on-screen log ---- */
#define UI_LINES 36
static char s_ui_lines[UI_LINES][96];
static int s_ui_next;
static volatile bool s_ui_dirty;
static char s_ui_status[160];
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t *s_log_label, *s_status_label;

void lvport_ui_log(const char *line)
{
    taskENTER_CRITICAL(&s_ui_mux);
    strlcpy(s_ui_lines[s_ui_next], line, sizeof s_ui_lines[0]);
    s_ui_next = (s_ui_next + 1) % UI_LINES;
    s_ui_dirty = true;
    taskEXIT_CRITICAL(&s_ui_mux);
}

void lvport_ui_status(const char *text)
{
    taskENTER_CRITICAL(&s_ui_mux);
    strlcpy(s_ui_status, text, sizeof s_ui_status);
    s_ui_dirty = true;
    taskEXIT_CRITICAL(&s_ui_mux);
}

static void ui_timer(lv_timer_t *t)
{
    if (!s_ui_dirty || !s_log_label) return;
    static char buf[UI_LINES * 98];
    char status[160];
    size_t n = 0;
    taskENTER_CRITICAL(&s_ui_mux);
    for (int i = 0; i < UI_LINES; i++) {
        const char *l = s_ui_lines[(s_ui_next + i) % UI_LINES];
        if (!*l) continue;
        size_t len = strlen(l);
        memcpy(buf + n, l, len);
        n += len;
        buf[n++] = '\n';
    }
    buf[n] = 0;
    strlcpy(status, s_ui_status, sizeof status);
    s_ui_dirty = false;
    taskEXIT_CRITICAL(&s_ui_mux);
    lv_label_set_text(s_log_label, buf);
    lv_label_set_text(s_status_label, status);
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "p4bench  %dx%d  rot %d", s_lw, s_lh, s_rot);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF2C14E), 0);

    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9FB3C8), 0);
    lv_label_set_text(s_status_label, "");

    s_log_label = lv_label_create(scr);
    lv_obj_set_width(s_log_label, lv_pct(100));
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_log_label, lv_color_hex(0xE8EEF2), 0);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_flex_grow(s_log_label, 1);
    lv_label_set_text(s_log_label, "");
    s_ui_dirty = true;
}

/* ---- flush ---- */

/* logical area -> physical area for the current rotation */
static void rotate_area(const lv_area_t *a, lv_area_t *p)
{
    switch (s_rot) {
    case 90:   /* px = W-1-ly, py = lx */
        p->x1 = LCD_W - 1 - a->y2; p->x2 = LCD_W - 1 - a->y1;
        p->y1 = a->x1;             p->y2 = a->x2;
        break;
    case 180:
        p->x1 = LCD_W - 1 - a->x2; p->x2 = LCD_W - 1 - a->x1;
        p->y1 = LCD_H - 1 - a->y2; p->y2 = LCD_H - 1 - a->y1;
        break;
    case 270:  /* px = ly, py = H-1-lx */
        p->x1 = a->y1;             p->x2 = a->y2;
        p->y1 = LCD_H - 1 - a->x2; p->y2 = LCD_H - 1 - a->x1;
        break;
    default:
        *p = *a;
    }
}

static void copy_cpu(uint16_t *fb, const lv_area_t *a, const uint16_t *src)
{
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    lv_area_t p;
    rotate_area(a, &p);
    switch (s_rot) {
    case 0:
        for (int y = 0; y < h; y++) memcpy(fb + (p.y1 + y) * LCD_W + p.x1, src + y * w, w * 2);
        break;
    case 90:
        for (int y = 0; y < h; y++) {
            int px = LCD_W - 1 - (a->y1 + y);
            const uint16_t *s = src + y * w;
            uint16_t *d = fb + a->x1 * LCD_W + px;
            for (int x = 0; x < w; x++, d += LCD_W) *d = s[x];
        }
        break;
    case 180:
        for (int y = 0; y < h; y++) {
            const uint16_t *s = src + y * w;
            uint16_t *d = fb + (LCD_H - 1 - (a->y1 + y)) * LCD_W + (LCD_W - 1 - a->x1);
            for (int x = 0; x < w; x++) *d-- = s[x];
        }
        break;
    case 270:
        for (int y = 0; y < h; y++) {
            int px = a->y1 + y;
            const uint16_t *s = src + y * w;
            uint16_t *d = fb + (LCD_H - 1 - a->x1) * LCD_W + px;
            for (int x = 0; x < w; x++, d -= LCD_W) *d = s[x];
        }
        break;
    }
    disp_cache_flush(fb + p.y1 * LCD_W, (p.y2 - p.y1 + 1) * LCD_W * 2);
}

static void copy_ppa(uint16_t *fb, const lv_area_t *a, const uint16_t *src)
{
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    lv_area_t p;
    rotate_area(a, &p);
    /* PPA angles are counter-clockwise; our 90 is clockwise */
    ppa_srm_rotation_angle_t ang = s_rot == 90 ? PPA_SRM_ROTATION_ANGLE_270 :
                                   s_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                                   s_rot == 270 ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0;
    ppa_srm_oper_config_t op = {
        .in = { .buffer = src, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = fb, .buffer_size = LCD_FB_BYTES, .pic_w = LCD_W, .pic_h = LCD_H,
                 .block_offset_x = p.x1, .block_offset_y = p.y1, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = ang,
        .scale_x = 1, .scale_y = 1,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t e = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (e != ESP_OK) {
        static bool warned;
        if (!warned) { ESP_LOGE(TAG, "PPA SRM failed (%s), falling back to CPU", esp_err_to_name(e)); warned = true; }
        copy_cpu(fb, a, src);
    }
}

/* DMA2D into any frame buffer, blocking: esp_lcd's own engine, used
 * directly because its draw_bitmap only ever writes the buffer being
 * scanned. src must be written back from the cache first if the CPU drew it. */
static esp_async_fbcpy_handle_t s_fbcpy;
static SemaphoreHandle_t s_fbcpy_done;

static bool IRAM_ATTR fbcpy_cb(esp_async_fbcpy_handle_t h, esp_async_fbcpy_event_data_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_fbcpy_done, &woken);
    return woken == pdTRUE;
}

static bool dma2d_copy(const void *src, int sw, int sh, int sx, int sy, uint16_t *dst, int dx, int dy, int w, int h)
{
    if (!s_fbcpy) return false;
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = src, .dst_buffer = dst,
        .src_buffer_size_x = sw, .src_buffer_size_y = sh,
        .dst_buffer_size_x = LCD_W, .dst_buffer_size_y = LCD_H,
        .src_offset_x = sx, .src_offset_y = sy, .dst_offset_x = dx, .dst_offset_y = dy,
        .copy_size_x = w, .copy_size_y = h,
        .pixel_format_unique_id = { .color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565) },
    };
    if (esp_async_fbcpy(s_fbcpy, &t, fbcpy_cb, NULL) != ESP_OK) return false;
    return xSemaphoreTake(s_fbcpy_done, pdMS_TO_TICKS(100)) == pdTRUE;
}

/* physical rectangle copy between two frame buffers (keeps the back one in sync) */
static void sync_rect(uint16_t *dst, const uint16_t *src, const lv_area_t *p)
{
    int w = lv_area_get_width(p), h = lv_area_get_height(p);
    if (s_copy == COPY_DMA2D && dma2d_copy(src, LCD_W, LCD_H, p->x1, p->y1, dst, p->x1, p->y1, w, h)) return;
    if (s_copy == COPY_PPA) {
        ppa_srm_oper_config_t op = {
            .in = { .buffer = src, .pic_w = LCD_W, .pic_h = LCD_H, .block_w = w, .block_h = h,
                    .block_offset_x = p->x1, .block_offset_y = p->y1, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .out = { .buffer = dst, .buffer_size = LCD_FB_BYTES, .pic_w = LCD_W, .pic_h = LCD_H,
                     .block_offset_x = p->x1, .block_offset_y = p->y1, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0, .scale_x = 1, .scale_y = 1,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK) return;
    }
    for (int y = p->y1; y <= p->y2; y++) memcpy(dst + y * LCD_W + p->x1, src + y * LCD_W + p->x1, w * 2);
    disp_cache_flush(dst + p->y1 * LCD_W, h * LCD_W * 2);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    if (s_paused) { lv_display_flush_ready(disp); return; }
    bool async = false;
    int64_t t0 = esp_timer_get_time();
    const uint16_t *src = (const uint16_t *)px;
    /* raw tests may have flipped buffers behind our back: the back buffer is
     * always the one not being scanned */
    if (s_nfb == 2) s_back = 1 - disp_front();
    uint16_t *fb = disp_fb(s_nfb == 2 ? s_back : 0);

    if (s_copy == COPY_DMA2D && s_nfb == 2 && s_fbcpy) {
        /* two buffers: DMA2D into the back one, flipped once a frame */
        int w = lv_area_get_width(area), h = lv_area_get_height(area);
        lv_area_t p;
        rotate_area(area, &p);
        const uint16_t *from = src;
        if (s_rot && s_rot_tmp) {
            uint16_t *t = s_rot_tmp;
            if (s_rot == 90) {
                for (int y = 0; y < h; y++) { const uint16_t *r = src + y * w; for (int x = 0; x < w; x++) t[x * h + (h - 1 - y)] = r[x]; }
            } else if (s_rot == 180) {
                for (int y = 0; y < h; y++) { const uint16_t *r = src + y * w; uint16_t *d = t + (h - 1 - y) * w + (w - 1); for (int x = 0; x < w; x++) *d-- = r[x]; }
            } else {
                for (int y = 0; y < h; y++) { const uint16_t *r = src + y * w; for (int x = 0; x < w; x++) t[(w - 1 - x) * h + y] = r[x]; }
            }
            from = t;
        }
        int pw = p.x2 - p.x1 + 1, ph = p.y2 - p.y1 + 1;
        esp_cache_msync((void *)from, (size_t)pw * ph * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (!dma2d_copy(from, pw, ph, 0, 0, fb, p.x1, p.y1, pw, ph)) copy_cpu(fb, area, src);
    } else if (s_copy == COPY_DMA2D && s_rot == 0 && s_nfb == 1) {
        /* the driver copies with DMA2D into the scanned buffer and returns at
         * once: LVGL gets its buffer back from the done callback */
        async = true;
        s_async_pending = true;
        esp_lcd_panel_draw_bitmap(disp_panel(), area->x1, area->y1, area->x2 + 1, area->y2 + 1, src);
    } else if (s_copy == COPY_DMA2D && s_nfb == 1 && s_rot_tmp) {
        /* turned: the CPU rotates the chunk inside internal RAM, where the
         * scattered writes are cheap, and DMA2D carries it to PSRAM */
        int w = lv_area_get_width(area), h = lv_area_get_height(area);
        lv_area_t p;
        rotate_area(area, &p);
        uint16_t *t = s_rot_tmp;
        bool done = false;
        if (s_rotmode == 1 && s_ppa) {          /* the PPA, internal RAM to internal RAM */
            ppa_srm_rotation_angle_t ang = s_rot == 90 ? PPA_SRM_ROTATION_ANGLE_270 :
                                           s_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 : PPA_SRM_ROTATION_ANGLE_90;
            int pw = p.x2 - p.x1 + 1, ph = p.y2 - p.y1 + 1;
            ppa_srm_oper_config_t op = {
                .in = { .buffer = src, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                .out = { .buffer = t, .buffer_size = s_rot_tmp_bytes, .pic_w = pw, .pic_h = ph, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                .rotation_angle = ang, .scale_x = 1, .scale_y = 1, .mode = PPA_TRANS_MODE_BLOCKING,
            };
            done = ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
        } else if (s_rotmode == 2 && s_rot == 90) {   /* the CPU in 16x16 tiles */
            for (int y0 = 0; y0 < h; y0 += 16)
                for (int x0 = 0; x0 < w; x0 += 16) {
                    int y1 = y0 + 16 < h ? y0 + 16 : h, x1 = x0 + 16 < w ? x0 + 16 : w;
                    for (int y = y0; y < y1; y++) { const uint16_t *r = src + y * w; for (int x = x0; x < x1; x++) t[x * h + (h - 1 - y)] = r[x]; }
                }
            done = true;
        }
        if (done) {
        } else if (s_rot == 90) {
            for (int y = 0; y < h; y++) { const uint16_t *r = src + y * w; for (int x = 0; x < w; x++) t[x * h + (h - 1 - y)] = r[x]; }
        } else if (s_rot == 180) {
            for (int y = 0; y < h; y++) { const uint16_t *r = src + y * w; uint16_t *d = t + (h - 1 - y) * w + (w - 1); for (int x = 0; x < w; x++) *d-- = r[x]; }
        } else {
            for (int y = 0; y < h; y++) { const uint16_t *r = src + y * w; for (int x = 0; x < w; x++) t[(w - 1 - x) * h + y] = r[x]; }
        }
        async = true;
        s_async_pending = true;
        esp_lcd_panel_draw_bitmap(disp_panel(), p.x1, p.y1, p.x2 + 1, p.y2 + 1, t);
    } else if (s_copy == COPY_PPA && s_ppa) {
        copy_ppa(fb, area, src);
    } else {
        copy_cpu(fb, area, src);
    }
    s_flushes++;

    if (s_nfb == 2) {
        lv_area_t p;
        rotate_area(area, &p);
        if (s_ndirty < MAX_DIRTY) s_dirty[s_ndirty++] = p; else s_dirty_full = true;
    }
    int64_t t1 = esp_timer_get_time();
    s_flush_us += t1 - t0;

    if (lv_display_flush_is_last(disp)) {
        s_frames++;
        if (s_nfb == 2) {
            disp_show_fb(s_back);
            disp_wait_frame(50);          /* the old front stops being scanned */
            int front = s_back;
            s_back = 1 - front;
            uint16_t *dst = disp_fb(s_back), *srcfb = disp_fb(front);
            if (s_dirty_full) {
                lv_area_t all = { 0, 0, LCD_W - 1, LCD_H - 1 };
                sync_rect(dst, srcfb, &all);
            } else {
                for (int i = 0; i < s_ndirty; i++) sync_rect(dst, srcfb, &s_dirty[i]);
            }
            s_ndirty = 0;
            s_dirty_full = false;
        }
        s_sync_us += esp_timer_get_time() - t1;
    }
    if (!async) lv_display_flush_ready(disp);
}

static void IRAM_ATTR dma2d_done(void)
{
    if (s_async_pending && s_disp) { s_async_pending = false; lv_display_flush_ready(s_disp); }
}

/* ---- touch ---- */

static uint32_t s_touch_reads, s_touch_hits;
static volatile bool s_touch_held;

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->state = LV_INDEV_STATE_RELEASED;
    if (!s_touch || s_touch_held) return;
    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    esp_lcd_touch_get_data(s_touch, pt, &n, 1);
    s_touch_reads++;
    if (!n) return;
    s_touch_hits++;
    int px = pt[0].x, py = pt[0].y, lx, ly;
    switch (s_rot) {
    case 90:  lx = py;             ly = LCD_W - 1 - px; break;
    case 180: lx = LCD_W - 1 - px; ly = LCD_H - 1 - py; break;
    case 270: lx = LCD_H - 1 - py; ly = px;             break;
    default:  lx = px;             ly = py;
    }
    data->point.x = lx;
    data->point.y = ly;
    data->state = LV_INDEV_STATE_PRESSED;
}

esp_lcd_touch_handle_t lvport_touch(void) { return s_touch; }
void lvport_touch_hold(bool hold) { s_touch_held = hold; }

/* ---- task ---- */

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool lvport_lock(int timeout_ms)
{
    if (!s_lock) return false;
    return xSemaphoreTakeRecursive(s_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lvport_unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

void lvport_pause(bool pause)
{
    if (pause) {
        lvport_lock(-1);        /* wait for any flush in progress */
        s_paused = true;
        lvport_unlock();
    } else {
        s_paused = false;
        if (lvport_lock(1000)) {
            if (s_disp) lv_obj_invalidate(lv_screen_active());
            lvport_unlock();
        }
    }
}

bool lvport_running(void) { return s_running; }
int lvport_rotation(void) { return s_rot; }

static void lv_task(void *arg)
{
    for (;;) {
        uint32_t wait = 5;
        if (lvport_lock(-1)) {
            wait = lv_timer_handler();
            lvport_unlock();
        }
        if (wait < 1) wait = 1;
        if (wait > 20) wait = 20;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void lvport_start(void)
{
    s_rot  = bench_cfg_get("rot", 0);
    s_copy = bench_cfg_get("copy", COPY_PPA);
    s_rows = bench_cfg_get("rows", 80);
    s_bmem = bench_cfg_get("bmem", 0);
    s_nfb  = disp_num_fbs() >= 2 ? 2 : 1;
    if (s_rot != 90 && s_rot != 180 && s_rot != 270) s_rot = 0;
    s_lw = (s_rot == 90 || s_rot == 270) ? LCD_H : LCD_W;
    s_lh = (s_rot == 90 || s_rot == 270) ? LCD_W : LCD_H;

    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (ppa_register_client(&pc, &s_ppa) != ESP_OK) s_ppa = NULL;

    s_lock = xSemaphoreCreateRecursiveMutex();
    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(s_lw, s_lh);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    size_t buf_bytes = (size_t)s_lw * s_rows * 2;
    uint32_t caps = (s_bmem ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_DMA;
    void *b1 = heap_caps_aligned_alloc(128, buf_bytes, caps);
    void *b2 = heap_caps_aligned_alloc(128, buf_bytes, caps);
    if (!b1 || !b2) {
        ESP_LOGW(TAG, "draw buffers of %u bytes do not fit in %s, using PSRAM", (unsigned)buf_bytes, s_bmem ? "PSRAM" : "internal RAM");
        heap_caps_free(b1); heap_caps_free(b2);
        b1 = heap_caps_aligned_alloc(128, buf_bytes, MALLOC_CAP_SPIRAM);
        b2 = heap_caps_aligned_alloc(128, buf_bytes, MALLOC_CAP_SPIRAM);
    }
    lv_display_set_buffers(s_disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    if (s_copy == COPY_DMA2D) {
        esp_async_fbcpy_config_t fc = { 0 };
        s_fbcpy_done = xSemaphoreCreateBinary();
        if (esp_async_fbcpy_install(&fc, &s_fbcpy) != ESP_OK) s_fbcpy = NULL;
    }
    if (s_copy == COPY_DMA2D && s_rot) {
        s_rot_tmp = heap_caps_aligned_alloc(128, buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_rot_tmp_bytes = buf_bytes;
    }
    s_rotmode = bench_cfg_get("rotmode", 0);
    lv_display_set_flush_cb(s_disp, flush_cb);
    disp_set_trans_done(dma2d_done);
    s_back = 1;

    bsp_display_cfg_t tcfg = { 0 };
    if (bsp_touch_new(&tcfg, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch not found");
        s_touch = NULL;
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    ui_create();
    lv_timer_create(ui_timer, 200, NULL);

    xTaskCreatePinnedToCore(lv_task, "lvgl", 16384, NULL, 4, NULL, 1);
    s_running = true;
    bench_report("lvgl.cfg", "rotmode=%d rot=%d copy=%s fbs=%d rows=%d bmem=%s logical=%dx%d draw_units=%d os=%d ppa_draw=%d",
                 s_rotmode, s_rot, s_copy == COPY_CPU ? "cpu" : s_copy == COPY_PPA ? "ppa" : "dma2d",
                 s_nfb, s_rows, s_bmem ? "psram" : "internal", s_lw, s_lh,
                 LV_DRAW_SW_DRAW_UNIT_CNT, LV_USE_OS != LV_OS_NONE, LV_USE_PPA);
}

/* ---- commands ---- */

static void bench_end_cb(const lv_demo_benchmark_summary_t *sum)
{
    for (int i = 0; sum->scenes && sum->scenes[i].create_cb; i++) {
        const lv_demo_benchmark_scene_dsc_t *s = &sum->scenes[i];
        if (!s->measurement_cnt) continue;
        bench_report("lvgl.scene", "name=\"%s\" cpu=%lu fps=%lu time_ms=%lu render_ms=%lu flush_ms=%lu",
                     s->name, (unsigned long)(s->cpu_avg_usage / s->measurement_cnt),
                     (unsigned long)(s->fps_avg / s->measurement_cnt),
                     (unsigned long)(s->render_avg_time + s->flush_avg_time) / s->measurement_cnt,
                     (unsigned long)(s->render_avg_time / s->measurement_cnt),
                     (unsigned long)(s->flush_avg_time / s->measurement_cnt));
    }
    if (sum->valid_scene_cnt) {
        bench_report("lvgl.bench", "rot=%d copy=%d fbs=%d rows=%d units=%d ppa_draw=%d avg_cpu=%lu avg_fps=%lu render_ms=%lu flush_ms=%lu",
                     s_rot, s_copy, s_nfb, s_rows, LV_DRAW_SW_DRAW_UNIT_CNT, LV_USE_PPA,
                     (unsigned long)(sum->total_avg_cpu / sum->valid_scene_cnt),
                     (unsigned long)(sum->total_avg_fps / sum->valid_scene_cnt),
                     (unsigned long)(sum->total_avg_render_time / sum->valid_scene_cnt),
                     (unsigned long)(sum->total_avg_flush_time / sum->valid_scene_cnt));
    }
    lv_demo_benchmark_summary_display(sum);
}

static int cmd_lvgl(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "help");
    if (!strcmp(sub, "bench")) {
        lvport_lock(-1);
        lv_obj_clean(lv_screen_active());
        s_log_label = s_status_label = NULL;
        lv_demo_benchmark_set_end_cb(bench_end_cb);
        lv_demo_benchmark();
        lvport_unlock();
        bench_say("lv_demo_benchmark running (~1 min), results come as BENCH lvgl.scene");
        return 0;
    }
    if (!strcmp(sub, "widgets") || !strcmp(sub, "music")) {
        lvport_lock(-1);
        lv_obj_clean(lv_screen_active());
        s_log_label = s_status_label = NULL;
        if (!strcmp(sub, "widgets")) lv_demo_widgets(); else lv_demo_music();
        lvport_unlock();
        return 0;
    }
    if (!strcmp(sub, "ui")) {
        lvport_lock(-1);
        ui_create();
        lvport_unlock();
        return 0;
    }
    if (!strcmp(sub, "stats")) {
        bench_report("lvgl.stats", "frames=%lu flushes=%lu flush_ms_per_frame=%.2f sync_ms_per_frame=%.2f touch_reads=%lu touch_hits=%lu",
                     (unsigned long)s_frames, (unsigned long)s_flushes,
                     s_frames ? s_flush_us / 1000.0 / s_frames : 0, s_frames ? s_sync_us / 1000.0 / s_frames : 0,
                     (unsigned long)s_touch_reads, (unsigned long)s_touch_hits);
        s_frames = s_flushes = 0;
        s_flush_us = s_sync_us = 0;
        return 0;
    }
    if (!strcmp(sub, "cfg")) {
        /* lvgl cfg rot=90 copy=ppa fbs=2 rows=80 bmem=0 */
        for (int i = 2; i < argc; i++) {
            char key[16];
            const char *eq = strchr(argv[i], '=');
            if (!eq || eq - argv[i] >= (int)sizeof key) continue;
            memcpy(key, argv[i], eq - argv[i]);
            key[eq - argv[i]] = 0;
            const char *v = eq + 1;
            int val = !strcmp(v, "cpu") ? COPY_CPU : !strcmp(v, "ppa") ? COPY_PPA : !strcmp(v, "dma2d") ? COPY_DMA2D : atoi(v);
            if (!strcmp(key, "rot") || !strcmp(key, "copy") || !strcmp(key, "rows") || !strcmp(key, "bmem") || !strcmp(key, "fbs") || !strcmp(key, "rotmode")) {
                bench_cfg_set(key, val);
                bench_say("%s=%d (reboot to apply)", key, val);
            }
        }
        return 0;
    }
    printf("lvgl bench|widgets|music|ui|stats|cfg rot=0|90|180|270 copy=cpu|ppa|dma2d fbs=1|2 rows=N bmem=0|1\n");
    return 0;
}

void reg_lvgl(void)
{
    const esp_console_cmd_t cmd = { .command = "lvgl", .help = "LVGL port tests (bench widgets music ui stats cfg)", .func = cmd_lvgl };
    esp_console_cmd_register(&cmd);
}
