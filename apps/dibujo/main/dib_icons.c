/*
 * DIBUJO - the tools' icons.
 *
 * The system's glyph font has a pencil, a brush and an eraser, but no
 * airbrush, bucket, eyedropper or shapes, and a toolbar of mixed styles
 * reads badly. So every tool is drawn here, white on transparent, with the
 * same antialiased rasterizer the shapes use (dib_raster.c), on a grid of
 * 24 units like the usual icon sets, once, the first time the editor is
 * built. 18 icons of 44 px are 136 KB of PSRAM.
 */
#include "dib_app.h"
#include "lvgl_private.h"         /* lv_image_cache_drop */

#include <math.h>
#include <string.h>

#define ICON_PX 44

static lv_image_dsc_t s_img[IC_COUNT];
static uint32_t *s_px[IC_COUNT];

typedef struct {
    uint32_t *px;
    uint8_t   alpha;
} ictx_t;

static void span_icon(void *user, int y, int x0, int n, const uint8_t *cov)
{
    ictx_t *c = user;
    for (int i = 0; i < n; i++) {
        unsigned a = cov[i] * c->alpha / 255;
        uint32_t *p = &c->px[y * ICON_PX + x0 + i];
        unsigned old = *p >> 24;
        unsigned na = a + old * (255 - a) / 255;
        if (na > 255) na = 255;
        *p = ((uint32_t)na << 24) | 0xFFFFFF;
    }
}

static dib_path_t P;
static ictx_t C;
#define U(v) ((float)(v) * ICON_PX / 24.0f)

static void draw(uint8_t alpha)
{
    C.alpha = alpha;
    dib_raster_fill(&P, (dib_rect_t){ 0, 0, ICON_PX, ICON_PX }, span_icon, &C);
    dib_path_reset(&P);
}

static void line(float x0, float y0, float x1, float y1, float w)
{
    dib_pt_t p[2] = { { U(x0), U(y0) }, { U(x1), U(y1) } };
    dib_path_polyline(&P, p, 2, false, U(w));
}

static void outline(const float *xy, int n, float w, bool closed)
{
    dib_pt_t p[16];
    for (int i = 0; i < n; i++) p[i] = (dib_pt_t){ U(xy[2 * i]), U(xy[2 * i + 1]) };
    dib_path_polyline(&P, p, n, closed, U(w));
}

static void poly(const float *xy, int n)
{
    dib_path_move(&P, U(xy[0]), U(xy[1]));
    for (int i = 1; i < n; i++) dib_path_line(&P, U(xy[2 * i]), U(xy[2 * i + 1]));
}

static void ring_rect(float cx, float cy, float w, float h, float r, float ang, float sw)
{
    dib_path_rrect(&P, U(cx), U(cy), U(w + sw), U(h + sw), U(r + sw / 2), ang, false);
    dib_path_rrect(&P, U(cx), U(cy), U(w - sw), U(h - sw), U(r > sw / 2 ? r - sw / 2 : 0), ang, true);
}

static void make(icon_t i)
{
    const float D = 0.7854f;        /* 45 degrees */
    switch (i) {
    case IC_PENCIL: {
        const float body[] = { 15.5f, 3.5f, 20.5f, 8.5f, 8.5f, 20.5f, 3.5f, 15.5f };
        outline(body, 4, 1.8f, true);
        const float tip[] = { 3.5f, 15.5f, 8.5f, 20.5f, 2.5f, 21.5f };
        poly(tip, 3);
        draw(255);
        line(13, 6, 18, 11, 1.6f);
        break;
    }
    case IC_SOFT:
        line(20.5f, 3.5f, 11.5f, 12.5f, 2.2f);
        draw(255);
        dib_path_ellipse(&P, U(7.5f), U(16.5f), U(4.6f), U(3.4f), -D, false);
        draw(255);
        dib_path_ellipse(&P, U(5.0f), U(19.0f), U(3.0f), U(2.0f), -D, false);
        draw(160);
        break;
    case IC_AIR: {
        ring_rect(14.5f, 15, 8, 12, 2, 0, 1.8f);
        draw(255);
        line(14.5f, 9, 14.5f, 6, 3.2f);
        draw(255);
        const float dots[][2] = { { 7, 4 }, { 4.5f, 6.5f }, { 8, 7.5f }, { 3, 3 }, { 5.5f, 2 }, { 2.5f, 9.5f } };
        for (size_t k = 0; k < sizeof dots / sizeof dots[0]; k++) {
            dib_path_circle(&P, U(dots[k][0]), U(dots[k][1]), U(0.95f));
        }
        draw(255);
        break;
    }
    case IC_MARKER: {
        line(3, 21, 15, 21, 3.4f);
        draw(110);
        const float body[] = { 16, 2.5f, 21.5f, 8, 12, 17.5f, 6.5f, 12 };
        outline(body, 4, 1.8f, true);
        const float tip[] = { 6.5f, 12, 12, 17.5f, 8, 19.5f, 4.5f, 16 };
        poly(tip, 4);
        draw(255);
        break;
    }
    case IC_ERASER: {
        const float body[] = { 14, 3, 21, 10, 11, 20, 4, 13 };
        outline(body, 4, 1.8f, true);
        draw(255);
        const float half[] = { 4, 13, 8.5f, 8.5f, 15.5f, 15.5f, 11, 20 };
        poly(half, 4);
        draw(255);
        line(11, 21, 21, 21, 1.6f);
        draw(255);
        break;
    }
    case IC_LINE:
        line(4, 20, 20, 4, 2.4f);
        draw(255);
        break;
    case IC_RECT:
        ring_rect(12, 12, 16, 13, 1.5f, 0, 2.0f);
        draw(255);
        break;
    case IC_ELLIPSE:
        dib_path_ellipse(&P, U(12), U(12), U(9.2f), U(7.2f), 0, false);
        dib_path_ellipse(&P, U(12), U(12), U(7.2f), U(5.2f), 0, true);
        draw(255);
        break;
    case IC_POLY: {
        float xy[10];
        for (int k = 0; k < 5; k++) {
            float t = -1.5708f + (float)k * 1.2566f;
            xy[2 * k] = 12 + 8.5f * cosf(t);
            xy[2 * k + 1] = 12.8f + 8.5f * sinf(t);
        }
        outline(xy, 5, 2.0f, true);
        draw(255);
        break;
    }
    case IC_ARROW: {
        line(4, 20, 16, 8, 2.2f);
        const float head[] = { 20.5f, 3.5f, 18.5f, 12.5f, 11.5f, 5.5f };
        poly(head, 3);
        draw(255);
        break;
    }
    case IC_TEXT: {
        const float t[] = { 4, 4, 20, 4, 20, 8, 18, 8, 17, 6.5f, 13.6f, 6.5f, 13.6f, 18.5f, 16, 19,
                            16, 21, 8, 21, 8, 19, 10.4f, 18.5f, 10.4f, 6.5f, 7, 6.5f, 6, 8, 4, 8 };
        poly(t, 16);
        draw(255);
        break;
    }
    case IC_FILL: {
        const float cup[] = { 10, 3, 18.5f, 11.5f, 11, 19, 2.5f, 10.5f };
        outline(cup, 4, 1.8f, true);
        draw(255);
        const float paint[] = { 2.5f, 10.5f, 18.5f, 11.5f, 11, 19 };
        poly(paint, 3);
        draw(255);
        dib_path_move(&P, U(20), U(13.5f));
        dib_path_line(&P, U(22.2f), U(17.8f));
        dib_path_line(&P, U(17.8f), U(17.8f));
        draw(255);
        dib_path_circle(&P, U(20), U(18.6f), U(2.2f));
        draw(255);
        break;
    }
    case IC_PICK:
        line(5, 19, 14, 10, 2.0f);
        draw(255);
        line(12.5f, 8.5f, 15.5f, 11.5f, 3.4f);
        draw(255);
        dib_path_circle(&P, U(17.5f), U(6.5f), U(3.6f));
        draw(255);
        dib_path_circle(&P, U(4), U(20), U(1.4f));
        draw(255);
        break;
    case IC_SELECT: {
        /* a dashed square */
        const float segs[][4] = { { 3, 3, 7, 3 }, { 10, 3, 14, 3 }, { 17, 3, 21, 3 }, { 21, 6, 21, 10 },
                                  { 21, 13, 21, 17 }, { 21, 20, 17, 21 }, { 14, 21, 10, 21 }, { 7, 21, 3, 21 },
                                  { 3, 17, 3, 13 }, { 3, 10, 3, 6 } };
        for (size_t k = 0; k < sizeof segs / sizeof segs[0]; k++) line(segs[k][0], segs[k][1], segs[k][2], segs[k][3], 2.0f);
        draw(255);
        break;
    }
    case IC_HAND:
        dib_path_rrect(&P, U(12.5f), U(15.5f), U(11), U(10), U(4), 0, false);
        draw(255);
        for (int k = 0; k < 4; k++) {
            float x = 8.4f + (float)k * 2.75f, top = k == 1 || k == 2 ? 3.2f : 4.8f;
            dib_path_rrect(&P, U(x), U((top + 13) / 2), U(2.2f), U(13 - top), U(1.1f), 0, false);
        }
        draw(255);
        dib_path_rrect(&P, U(5.6f), U(13.8f), U(2.2f), U(7), U(1.1f), -0.6f, false);
        draw(255);
        break;
    case IC_UNDO:
    case IC_REDO: {
        /* a hook: along the top, round the right, back along the bottom;
         * the head on the top's left end. Redo is its mirror. */
        bool r = i == IC_REDO;
        float xy[2 * 16];
        int n = 0;
        xy[n++] = 8.5f; xy[n++] = 8.0f;
        for (int k = 0; k <= 10; k++) {
            float t = -1.5708f + 3.14159f * (float)k / 10.0f;
            xy[n++] = 14.0f + 5.5f * cosf(t);
            xy[n++] = 13.5f + 5.5f * sinf(t);
        }
        xy[n++] = 7.0f; xy[n++] = 19.0f;
        float head[6] = { 3.0f, 8.0f, 9.5f, 3.0f, 9.5f, 13.0f };
        if (r) {
            for (int k = 0; k < n; k += 2) xy[k] = 24.0f - xy[k];
            for (int k = 0; k < 6; k += 2) head[k] = 24.0f - head[k];
        }
        outline(xy, n / 2, 2.2f, false);
        draw(255);
        poly(head, 3);
        draw(255);
        break;
    }
    case IC_LAYERS: {
        const float top[] = { 12, 3, 21, 8.5f, 12, 14, 3, 8.5f };
        poly(top, 4);
        draw(255);
        const float mid[] = { 3, 12.5f, 12, 18, 21, 12.5f };
        outline(mid, 3, 1.8f, false);
        draw(255);
        const float low[] = { 3, 16.5f, 12, 22, 21, 16.5f };
        outline(low, 3, 1.8f, false);
        draw(255);
        break;
    }
    default:
        break;
    }
}

const lv_image_dsc_t *dj_icon_get(icon_t i)
{
    if (i < 0 || i >= IC_COUNT) return NULL;
    if (!s_px[i]) {
        s_px[i] = dib_calloc(ICON_PX * ICON_PX * 4);
        if (!s_px[i]) return NULL;
        dib_path_init(&P);
        C.px = s_px[i];
        make(i);
        dib_path_free(&P);
        lv_image_dsc_t *d = &s_img[i];
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_ARGB8888;
        d->header.w = ICON_PX;
        d->header.h = ICON_PX;
        d->header.stride = ICON_PX * 4;
        d->data_size = ICON_PX * ICON_PX * 4;
        d->data = (const uint8_t *)s_px[i];
    }
    return &s_img[i];
}

void dj_icons_free(void)
{
    for (int i = 0; i < IC_COUNT; i++) {
        if (s_px[i]) lv_image_cache_drop(&s_img[i]);
        dib_free(s_px[i]);
        s_px[i] = NULL;
    }
}
