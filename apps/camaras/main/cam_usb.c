/*
 * P4OS - Cameras: a webcam on the board's USB host, usb://N (aos_hal_uvc_*,
 * docs/USB.md in the firmware).
 *
 * The HAL streams the camera in MJPEG and keeps its newest frame; this
 * session takes each new one and hands it to the view as a whole JPEG, the
 * same way an MJPEG over HTTP does, so decoding, scaling and the snapshot
 * are the view's as for any other camera. The mosaic and the full screen
 * share the one stream (the HAL counts its users); it is asked at 1280 x
 * 720, or the largest size the camera has below that.
 */
#include "cam.h"
#include "cam_view.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>

#define USB_W     1280
#define USB_H     720
#define FRAME_CAP (512 * 1024)
#define POLL_MS   8
#define LATE_MS   5000             /* no frame for this long: the camera is not sending */

bool cam_usb_run(cam_view_t *v, const cam_t *cam, const cam_url_t *url, char *why, size_t why_len)
{
    (void)cam;
    cam_view_state(v, CAM_ST_CONNECTING, NULL);
    if (url->usb_index >= aos_hal_uvc_count()) {
        snprintf(why, why_len, "%s", _("La cámara USB no está conectada"));
        return false;
    }
    if (!aos_hal_uvc_start(url->usb_index, USB_W, USB_H)) {
        snprintf(why, why_len, "%s", _("La cámara USB no arrancó"));
        return false;
    }
    uint8_t *buf = aos_hal_io_alloc(FRAME_CAP);
    if (!buf) {
        aos_hal_uvc_stop();
        snprintf(why, why_len, "%s", _("Sin memoria para la cámara"));
        return false;
    }
    cam_view_codec(v, CAM_CODEC_JPEG);
    cam_view_state(v, CAM_ST_WAITING, NULL);
    uint32_t seq = 0;
    uint64_t last = aos_hal_uptime_ms();
    bool ok = true;
    while (!cam_should_stop(v)) {
        int n = aos_hal_uvc_frame(buf, FRAME_CAP, &seq);
        if (n < 0) {
            snprintf(why, why_len, "%s", _("Se desenchufó la cámara USB"));
            ok = false;
            break;
        }
        if (n > 0) {
            last = aos_hal_uptime_ms();
            cam_view_bytes(v, n);
            cam_view_jpeg(v, buf, n, -1);
            cam_view_flush(v);
            continue;
        }
        if (aos_hal_uptime_ms() - last > LATE_MS) {
            snprintf(why, why_len, "%s", _("La cámara USB no manda imagen"));
            ok = false;
            break;
        }
        aos_hal_sleep_ms(POLL_MS);
    }
    aos_hal_io_free(buf);
    aos_hal_uvc_stop();
    return ok;
}
