/*
 * P4OS - Mensajes: pictures from the card, decoded off LVGL's thread.
 *
 * Avatars, the photos of the bubbles, stickers' thumbnails and the photo
 * viewer all come from JPEG files in the cache (mj_api.c downloads them).
 * aos_hal_image_decode() reads one, decodes it (the P4's JPEG engine on the
 * board) and scales it, which takes tens of milliseconds: that goes in a
 * thread of its own, and the screen gets the picture on a later timer once
 * mj_img_ver() moved.
 *
 * Each picture is an RGB565 image in PSRAM with its lv_image_dsc_t. An
 * lv_image that shows one holds it (mj_img_attach): a picture is only freed
 * once nothing on the screen points at it, the least recently used first,
 * when the pictures pass IMG_MAX.
 */
#include "mj_ui.h"

#define IMG_SLOTS 160
#define IMG_MAX   (6 * 1024 * 1024)

typedef struct {
    char path[176];
    int w, h;
    bool fill;
    uint8_t state;              /* 0 free, 1 queued, 2 decoding, 3 ready, 4 failed */
    uint16_t *px;
    lv_image_dsc_t dsc;
    int refs;
    uint32_t used;
    size_t bytes;
} img_t;

static img_t *s_img;
static void *s_lock;
static volatile bool s_stop, s_running;
static volatile uint32_t s_ver;
static uint32_t s_tick;
static size_t s_bytes;

uint32_t mj_img_ver(void)
{
    return s_ver;
}

size_t mj_img_bytes(void)
{
    return s_bytes;
}

static void free_px(img_t *m)
{
    if (m->px) aos_hal_image_free(m->px);
    m->px = NULL;
    s_bytes -= m->bytes;
    m->bytes = 0;
}

static void evict(void)
{
    while (s_bytes > IMG_MAX) {
        img_t *old = NULL;
        for (int i = 0; i < IMG_SLOTS; i++) {
            img_t *m = &s_img[i];
            if (m->state == 3 && m->refs <= 0 && (!old || m->used < old->used)) old = m;
        }
        if (!old) return;
        free_px(old);
        old->state = 0;
    }
}

static void worker(void *arg)
{
    (void)arg;
    while (!s_stop) {
        img_t *m = NULL;
        char path[176];
        int w = 0, h = 0;
        bool fill = false;
        mj_mutex_lock(s_lock);
        for (int i = 0; i < IMG_SLOTS; i++)
            if (s_img[i].state == 1) {
                m = &s_img[i];
                m->state = 2;
                memcpy(path, m->path, sizeof path);
                w = m->w;
                h = m->h;
                fill = m->fill;
                break;
            }
        mj_mutex_unlock(s_lock);
        if (!m) {
            mj_sleep_ms(30);
            continue;
        }
        int ow = 0, oh = 0;
        uint16_t *px = aos_hal_image_decode(path, 0, 0, w, h, fill, &ow, &oh, NULL, NULL);
        mj_mutex_lock(s_lock);
        if (m->state == 2 && !strcmp(m->path, path)) {
            if (px && ow > 0 && oh > 0) {
                m->px = px;
                px = NULL;
                memset(&m->dsc, 0, sizeof m->dsc);
                m->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                m->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                m->dsc.header.w = (uint32_t)ow;
                m->dsc.header.h = (uint32_t)oh;
                m->dsc.header.stride = (uint32_t)ow * 2;
                m->dsc.data = (const uint8_t *)m->px;
                m->dsc.data_size = (uint32_t)ow * oh * 2;
                m->bytes = (size_t)ow * oh * 2;
                s_bytes += m->bytes;
                m->state = 3;
            } else {
                m->state = 4;
                mj_log("img: cannot decode %s", path);
            }
            s_ver++;
        }
        mj_mutex_unlock(s_lock);
        if (px) aos_hal_image_free(px);
    }
    s_running = false;
}

void mj_img_start(void)
{
    if (s_running) return;
    if (!s_lock) s_lock = mj_mutex_new();
    if (!s_img) s_img = (img_t *)mj_calloc(IMG_SLOTS * sizeof *s_img);
    if (!s_img) return;
    s_stop = false;
    s_running = true;
    if (!mj_thread_start("mj_img", worker, NULL, 8192)) s_running = false;
}

void mj_img_stop(void)
{
    s_stop = true;
    for (int i = 0; i < 300 && s_running; i++) mj_sleep_ms(10);
    if (s_img) {
        for (int i = 0; i < IMG_SLOTS; i++) free_px(&s_img[i]);
        mj_free(s_img);
        s_img = NULL;
    }
    s_bytes = 0;
}

static void on_delete(lv_event_t *e)
{
    img_t *m = (img_t *)lv_event_get_user_data(e);
    if (!m || !s_img || m < s_img || m >= s_img + IMG_SLOTS) return;
    mj_mutex_lock(s_lock);
    m->refs--;
    mj_mutex_unlock(s_lock);
}

const lv_image_dsc_t *mj_img_get(const char *path, int w, int h, bool fill)
{
    if (!s_img || !path || !*path) return NULL;
    s_tick++;
    mj_mutex_lock(s_lock);
    img_t *hit = NULL, *free_slot = NULL;
    for (int i = 0; i < IMG_SLOTS; i++) {
        img_t *m = &s_img[i];
        if (m->state && m->w == w && m->h == h && m->fill == fill && !strcmp(m->path, path)) {
            hit = m;
            break;
        }
        if (!m->state && !free_slot) free_slot = m;
    }
    const lv_image_dsc_t *out = NULL;
    if (hit) {
        hit->used = s_tick;
        if (hit->state == 3) out = &hit->dsc;
    } else if (mj_file_exists(path)) {
        if (!free_slot) {
            /* no free slot: the oldest picture nothing shows */
            for (int i = 0; i < IMG_SLOTS; i++) {
                img_t *m = &s_img[i];
                if ((m->state == 3 || m->state == 4) && m->refs <= 0 && (!free_slot || m->used < free_slot->used))
                    free_slot = m;
            }
            if (free_slot) free_px(free_slot);
        }
        if (free_slot) {
            memset(free_slot, 0, sizeof *free_slot);
            snprintf(free_slot->path, sizeof free_slot->path, "%s", path);
            free_slot->w = w;
            free_slot->h = h;
            free_slot->fill = fill;
            free_slot->used = s_tick;
            free_slot->state = 1;
        }
    }
    evict();
    mj_mutex_unlock(s_lock);
    return out;
}

/* Shows the picture in an lv_image and holds it while the image lives. */
void mj_img_attach(lv_obj_t *img, const lv_image_dsc_t *d)
{
    if (!img || !d || !s_img) return;
    img_t *m = (img_t *)((uint8_t *)d - offsetof(img_t, dsc));
    mj_mutex_lock(s_lock);
    m->refs++;
    mj_mutex_unlock(s_lock);
    lv_image_set_src(img, d);
    lv_obj_add_event_cb(img, on_delete, LV_EVENT_DELETE, m);
}

void mj_img_forget(const char *path)
{
    if (!s_img) return;
    mj_mutex_lock(s_lock);
    for (int i = 0; i < IMG_SLOTS; i++) {
        img_t *m = &s_img[i];
        if (m->state == 4 && !strcmp(m->path, path)) m->state = 0;   /* failed: try again */
    }
    mj_mutex_unlock(s_lock);
}
