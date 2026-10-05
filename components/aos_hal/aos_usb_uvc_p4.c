/*
 * P4OS - webcams on the USB host (aos_hal.h, docs/USB.md).
 *
 * Espressif's usb_host_uvc does the USB Video Class: it says when a camera
 * comes (with its frame sizes), negotiates a stream, and hands over whole
 * frames. P4OS asks for MJPEG, which every webcam gives and the P4's JPEG
 * engine decodes; the newest frame is copied into a buffer in PSRAM that
 * the reader (the Cameras app, through aos_hal_uvc_frame) takes when it is
 * ready, and the frames in between are dropped: a live picture, not a
 * recording.
 *
 * One camera streams at a time, shared: a second start of the camera that
 * is streaming (the Cameras app's mosaic and its full screen) joins it, and
 * the stream stops with the last stop. A camera is listed while it is
 * plugged in (the devices list says so); its sizes are the MJPEG ones it
 * declares.
 */
#include "aos_hal.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "usb";
#define CAM_MAX   2
#define SIZES_MAX 12
#define FRAME_MAX (512 * 1024)      /* the largest MJPEG frame kept (1080p at good quality fits) */

void aos_p4_usb_dev_use(uint8_t addr, const char *what);

typedef struct {
    bool used;
    uint8_t addr, stream_index;
    uint16_t vid, pid;
    char name[48];
    int nsizes;
    uint16_t size[SIZES_MAX][2];
} cam_t;

static struct {
    bool installed;
    SemaphoreHandle_t mx;
    cam_t c[CAM_MAX];
    /* the stream */
    uvc_host_stream_hdl_t stream;
    int streaming;                  /* the camera's slot, -1 none */
    int users;                      /* starts not yet stopped */
    volatile bool gone;
    uint8_t *frame;                 /* the newest frame, PSRAM */
    size_t frame_len;
    uint32_t seq;
    uint32_t frames, dropped_big;
    uint16_t w, h;
} U = { .streaming = -1 };

/* The name, from the devices list. That list may hear of the camera a
 * moment after this driver does: without its IDs yet, it is asked again
 * whenever the name is wanted (aos_hal_uvc_info). Under U.mx. */
static void name_of(cam_t *c)
{
    static aos_usb_dev_t d[12];
    if (c->vid) return;
    int nd = aos_hal_usb_devices(d, 12);
    for (int i = 0; i < nd; i++)
        if (d[i].addr == c->addr) {
            c->vid = d[i].vid;
            c->pid = d[i].pid;
            snprintf(c->name, sizeof c->name, "%.23s%s%.23s", d[i].vendor, d[i].vendor[0] && d[i].product[0] ? " " : "",
                     d[i].product);
        }
    /* many webcams declare no name (a Logitech C270 does not): its IDs */
    if (!c->name[0] && c->vid) snprintf(c->name, sizeof c->name, "Webcam %04x:%04x", c->vid, c->pid);
    if (!c->name[0]) snprintf(c->name, sizeof c->name, "Webcam");
}

static void driver_event(const uvc_host_driver_event_data_t *ev, void *ctx)
{
    (void)ctx;
    if (ev->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) return;
    uint8_t addr = ev->device_connected.dev_addr;
    xSemaphoreTake(U.mx, portMAX_DELAY);
    cam_t *c = NULL;
    for (int i = 0; i < CAM_MAX && !c; i++)
        if (U.c[i].used && U.c[i].addr == addr && U.c[i].stream_index == ev->device_connected.uvc_stream_index) c = &U.c[i];
    for (int i = 0; i < CAM_MAX && !c; i++) if (!U.c[i].used) c = &U.c[i];
    if (!c) {
        xSemaphoreGive(U.mx);
        return;
    }
    memset(c, 0, sizeof *c);
    c->used = true;
    c->addr = addr;
    c->stream_index = ev->device_connected.uvc_stream_index;
    /* its MJPEG sizes. Static, as the device list below: this runs on the
     * driver's task, whose stack two arrays of these and a log line
     * overflowed (a restart, on 2026-10-04, as the first webcam came) */
    static uvc_host_frame_info_t info[24];
    size_t n = sizeof info / sizeof info[0];
    if (uvc_host_get_frame_list(addr, c->stream_index, (uvc_host_frame_info_t (*)[])info, &n) == ESP_OK)
        for (size_t i = 0; i < n && c->nsizes < SIZES_MAX; i++)
            if (info[i].format == UVC_VS_FORMAT_MJPEG) {
                c->size[c->nsizes][0] = info[i].h_res;
                c->size[c->nsizes][1] = info[i].v_res;
                c->nsizes++;
            }
    name_of(c);
    xSemaphoreGive(U.mx);
    ESP_LOGI(TAG, "camera \"%s\" at address %u: %d MJPEG sizes%s", c->name, addr, c->nsizes,
             c->nsizes ? "" : " (none: it cannot be shown)");
    aos_p4_usb_dev_use(addr, "camera");
}

/* the devices list knows what is still plugged in */
static void prune(void)
{
    static aos_usb_dev_t d[12];         /* under U.mx, as every caller holds it */
    int nd = aos_hal_usb_devices(d, 12);
    for (int i = 0; i < CAM_MAX; i++) {
        if (!U.c[i].used || i == U.streaming) continue;
        bool here = false;
        for (int k = 0; k < nd; k++) here |= d[k].addr == U.c[i].addr;
        if (!here) U.c[i].used = false;
    }
}

static bool frame_cb(const uvc_host_frame_t *f, void *ctx)
{
    (void)ctx;
    if (f->data_len > FRAME_MAX || !U.frame) {
        U.dropped_big++;
        return true;
    }
    xSemaphoreTake(U.mx, portMAX_DELAY);
    memcpy(U.frame, f->data, f->data_len);
    U.frame_len = f->data_len;
    U.seq++;
    U.frames++;
    xSemaphoreGive(U.mx);
    return true;                    /* the driver's buffer goes back at once */
}

static void stream_event(const uvc_host_stream_event_data_t *ev, void *ctx)
{
    (void)ctx;
    if (ev->type == UVC_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "camera unplugged while streaming");
        U.gone = true;
    }
}

static void stream_close(void);

bool aos_p4_usb_uvc_start(void)
{
    if (!U.mx) U.mx = xSemaphoreCreateMutex();
    memset(U.c, 0, sizeof U.c);
    U.streaming = -1;
    const uvc_host_driver_config_t cfg = { .driver_task_stack_size = 6144, .driver_task_priority = 5, .xCoreID = 0,
                                           .create_background_task = true, .event_cb = driver_event };
    /* the driver warns about every device that is not a camera: a pendrive,
     * a keyboard. Only its errors are news. */
    esp_log_level_set("uvc", ESP_LOG_ERROR);
    /* and every lost isochronous packet ("usb err 1"), a few a minute on
     * a webcam that streams fine */
    esp_log_level_set("uvc-isoc", ESP_LOG_ERROR);
    esp_err_t e = uvc_host_install(&cfg);
    U.installed = e == ESP_OK;
    if (!U.installed) ESP_LOGW(TAG, "camera: uvc_host_install: %s", esp_err_to_name(e));
    return U.installed;
}

void aos_p4_usb_uvc_stop(void)
{
    if (!U.installed) return;
    stream_close();
    uvc_host_uninstall();
    U.installed = false;
    memset(U.c, 0, sizeof U.c);
}

int aos_hal_uvc_count(void)
{
    if (!U.installed) return 0;
    xSemaphoreTake(U.mx, portMAX_DELAY);
    prune();
    int n = 0;
    for (int i = 0; i < CAM_MAX; i++) n += U.c[i].used;
    xSemaphoreGive(U.mx);
    return n;
}

static int slot_of(int index)
{
    for (int i = 0, k = 0; i < CAM_MAX; i++)
        if (U.c[i].used && k++ == index) return i;
    return -1;
}

bool aos_hal_uvc_info(int index, char *name, size_t n, uint16_t (*sizes)[2], int max, int *nsizes)
{
    if (!U.installed) return false;
    xSemaphoreTake(U.mx, portMAX_DELAY);
    int i = slot_of(index);
    if (i >= 0) {
        name_of(&U.c[i]);
        if (name) snprintf(name, n, "%s", U.c[i].name);
        int k = 0;
        for (; sizes && k < U.c[i].nsizes && k < max; k++) {
            sizes[k][0] = U.c[i].size[k][0];
            sizes[k][1] = U.c[i].size[k][1];
        }
        if (nsizes) *nsizes = k;
    }
    xSemaphoreGive(U.mx);
    return i >= 0;
}

static void stream_close(void)
{
    if (!U.stream) return;
    if (!U.gone) uvc_host_stream_stop(U.stream);
    uvc_host_stream_close(U.stream);
    U.stream = NULL;
    U.streaming = -1;
    U.users = 0;
    ESP_LOGI(TAG, "camera stream closed (%u frames)", (unsigned)U.frames);
}

bool aos_hal_uvc_start(int index, int w, int h)
{
    if (!U.installed) return false;
    xSemaphoreTake(U.mx, portMAX_DELAY);
    int i = slot_of(index);
    bool join = U.stream && !U.gone && i >= 0 && i == U.streaming;
    if (join) U.users++;
    xSemaphoreGive(U.mx);
    if (join) return true;
    stream_close();                 /* another camera, or a dead stream */
    xSemaphoreTake(U.mx, portMAX_DELAY);
    i = slot_of(index);
    cam_t c = i >= 0 ? U.c[i] : (cam_t){ 0 };
    xSemaphoreGive(U.mx);
    if (i < 0 || !c.nsizes) return false;
    /* the size asked for, or the largest that is not bigger than it, or the smallest */
    int best = -1;
    for (int k = 0; k < c.nsizes; k++) {
        int kw = c.size[k][0], kh = c.size[k][1];
        if (kw <= w && kh <= h && (best < 0 || kw * kh > c.size[best][0] * c.size[best][1])) best = k;
    }
    if (best < 0) {
        best = 0;
        for (int k = 1; k < c.nsizes; k++)
            if (c.size[k][0] * c.size[k][1] < c.size[best][0] * c.size[best][1]) best = k;
    }
    if (!U.frame) U.frame = heap_caps_malloc(FRAME_MAX, MALLOC_CAP_SPIRAM);
    if (!U.frame) return false;
    const uvc_host_stream_config_t sc = {
        .event_cb = stream_event, .frame_cb = frame_cb,
        .usb = { .dev_addr = c.addr, .uvc_stream_index = c.stream_index },
        .vs_format = { .h_res = c.size[best][0], .v_res = c.size[best][1], .fps = 0, .format = UVC_VS_FORMAT_MJPEG },
        .advanced = { .number_of_frame_buffers = 3, .frame_size = 0, .frame_heap_caps = MALLOC_CAP_SPIRAM,
                      .number_of_urbs = 3, .urb_size = 0 },
    };
    U.gone = false;
    U.frames = U.dropped_big = 0;
    esp_err_t e = uvc_host_stream_open(&sc, 1000, &U.stream);
    if (e == ESP_OK) e = uvc_host_stream_start(U.stream);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "camera \"%s\" %ux%u: %s", c.name, c.size[best][0], c.size[best][1], esp_err_to_name(e));
        if (U.stream) uvc_host_stream_close(U.stream);
        U.stream = NULL;
        return false;
    }
    U.streaming = i;
    U.users = 1;
    U.w = c.size[best][0];
    U.h = c.size[best][1];
    ESP_LOGI(TAG, "camera \"%s\" streaming MJPEG %ux%u", c.name, U.w, U.h);
    return true;
}

void aos_hal_uvc_stop(void)
{
    if (!U.stream) return;
    xSemaphoreTake(U.mx, portMAX_DELAY);
    bool last = --U.users <= 0;
    xSemaphoreGive(U.mx);
    if (last) stream_close();
}

bool aos_hal_uvc_streaming(uint16_t *w, uint16_t *h, uint32_t *frames)
{
    if (!U.stream || U.gone) return false;
    if (w) *w = U.w;
    if (h) *h = U.h;
    if (frames) *frames = U.frames;
    return true;
}

int aos_hal_uvc_frame(uint8_t *buf, int max, uint32_t *seq)
{
    if (!U.stream || U.gone) return -1;
    int n = 0;
    xSemaphoreTake(U.mx, portMAX_DELAY);
    if (U.seq != *seq && U.frame_len && (int)U.frame_len <= max) {
        memcpy(buf, U.frame, U.frame_len);
        n = (int)U.frame_len;
        *seq = U.seq;
    }
    xSemaphoreGive(U.mx);
    return n;
}
