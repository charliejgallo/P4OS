/*
 * P4OS - a photo as the wallpaper, behind the home screen and behind the
 * lock screen, each its own (2026-10-22).
 *
 * The photo is decoded on a thread of its own with aos_hal_image_decode(),
 * cropped to cover the screen (fill), and kept in PSRAM as RGB565: 1.8 MB a
 * screen, 3.7 MB with both. It is darkened while decoding, as much as its
 * own brightness asks (more behind the lock screen's big clock), so the
 * white names stay readable at no cost per frame: a translucent layer over
 * it would be blended on every redraw. Internal RAM: nothing but this file's
 * few words, the state is in PSRAM too.
 *
 * Under the photo the gradient is still there: the photo missing or not
 * readable, it shows as before. The paths are prefs ("wall_img",
 * "lock_img"); turning the screen decodes again at the new size.
 *
 * The lock screen's only while it can be seen: with an app over the home
 * screen it is let go (1.8 MB back) and decoded again as the screen goes
 * dark and locks. The home screen's stays in PSRAM: let go too until
 * 2026-10-25, it took a second and a half to come back on the way home,
 * the gradient on show meanwhile. An app that needs every megabyte (the
 * doorbell's 1920x1080 stream was short of both photos, 2026-10-22) has it
 * let go while it is in front with aos_ui_wall_photo_spare().
 *
 * Where in PSRAM (2026-11-08). Each photo has a slot of its own, a screen's
 * worth (1.8 MB), taken first thing when the shell starts, right after the
 * panel's frame buffers (aos_wall_photo_reserve): the decoder's result is
 * copied into it and freed. And the photos are decoded only once the boot
 * is over (aos_wall_photo_start, after the scan of the card's apps): the
 * decoder's own buffers, let go when it ends, left holes among what the
 * boot keeps, and the scan opens every app (measured with the slots but
 * decoding at once: the free PSRAM's largest block 6.5 MB). Before,
 * the photos lay wherever the decoder's buffer had landed, at boot in the
 * middle of the heap (+7.6 MB and +15.8 MB of 25), and the 14 MB free were
 * two pieces of 6.4 and 7.5 MB: CAS (a 5.3 MB file and 5.5 MB of code at
 * once) fitted or not depending on the boot. With the slots at the bottom
 * the free PSRAM is one piece. So the lock screen's photo is no longer let
 * go behind an app (its slot would come back as a hole the next decode
 * might not find again): it stays decoded, and locking shows it at once.
 * Only aos_wall_photo_release() gives a slot back: an app that needs every
 * megabyte (aos_ui_wall_photo_spare) or one that does not fit
 * (aos_ui_make_room); the next decode takes a new one wherever it can.
 *
 * Smaller still (2026-11-08, asked for by the user). The lock screen's photo
 * is kept at half the screen's width and height (460 KB, not 1.8 MB) and
 * stretched to the screen when drawn: behind the big clock, darker and
 * covered by the notifications, the softness hardly shows. And when the
 * lock screen's photo is the home screen's (the same file, no album), it
 * has no slot nor decode of its own: it shows the home screen's pixels,
 * darkened the rest of the way by a recolour while they are drawn. The
 * photos went from 3.6 MB to 2.3 MB, or 1.8 MB with the same photo.
 *
 * Behind a game (2026-11-08): an app full screen that keeps the screen on
 * and is not kept in the background (the games, Video, Cameras) gets both
 * photos' memory, 2.3 MB: a Monster Hop level left 11 KB free with them.
 * They are let go when it opens (aos_wall_photo_for_game) and taken back
 * once it has been destroyed, slots first and decodes after, so they land
 * where the game's memory was and the free PSRAM stays in one piece. In
 * between, nothing asks for them (aos_wall_photo_want does nothing).
 *
 * Or a folder behind the lock screen, a different photo of it every time
 * the board locks (2026-10-25): pref "lock_album". The worker picks one at
 * random among the folder's photos, never the one just shown, so the
 * listing is not done on LVGL's task; nothing is written at each lock, and
 * a restart starts from another photo of the same folder.
 */
#include "aos_internal.h"
#include "aos_lock.h"
#include "lvgl_private.h"           /* lv_image_cache_drop */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PATH_MAX_   256
/* The photo is darkened so its mean brightness is at most this (of 255):
 * a dark photo is left nearly as it is, a bright one comes down until the
 * white names on it read. Never below DIM_MIN of 256. */
#define LUMA_HOME   95
#define LUMA_LOCK   80
#define DIM_MIN     90
#define DIM_MAX     236

static const char *const PREF[2] = { "wall_img", "lock_img" };

typedef struct {
    char           path[PATH_MAX_];     /* "" none */
    uint16_t      *px;                  /* on show */
    int            w, h;
    lv_image_dsc_t dsc;
    uint32_t       gen;                 /* the request wanted */
    bool           want;                /* a request not yet on show */
    int            inflight;            /* decodes running: their results still to free */
    /* the worker's result, under s_mx */
    bool           ready;
    uint16_t      *new_px;
    int            new_w, new_h;
    uint32_t       new_gen;
    bool           new_thread;          /* from a decode (else from request(), with none) */
    uint32_t       tint, new_tint;      /* the photo's colour, lightened: the lock clock's */
    char           new_path[PATH_MAX_]; /* the album's photo the worker picked */
    uint16_t      *slot;                /* where px lives: a screen's worth, kept */
} wall_t;

AOS_BSS_PSRAM static wall_t W[2];
static void       *s_mx;
AOS_BSS_PSRAM static char s_album[PATH_MAX_];  /* the lock screen's folder; "" none */
static lv_timer_t *s_poll;
static lv_obj_t   *s_home_img;

typedef struct {
    int      which;
    uint32_t gen;
    int      w, h;
    char     path[PATH_MAX_];       /* the photo; with an album, the one shown now (not again) */
    char     album[PATH_MAX_];      /* "" or the folder to pick from */
} job_t;

static bool is_photo(const char *n)
{
    const char *dot = strrchr(n, '.');
    return n[0] != '.' && dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") ||
                                  !strcasecmp(dot, ".png") || !strcasecmp(dot, ".bmp"));
}

/* One of the folder's photos at random, not 'skip' if there are others:
 * reservoir sampling, one pass and no list */
static bool album_pick(const char *dir, const char *skip, char *out, size_t len)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    const char *skip_name = strrchr(skip, '/');
    skip_name = skip_name ? skip_name + 1 : skip;
    char other[PATH_MAX_] = "";
    uint32_t seen = 0;
    bool got = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_photo(e->d_name)) continue;
        if (!strcmp(e->d_name, skip_name)) {    /* kept only for a folder of one */
            if (snprintf(other, sizeof other, "%s/%s", dir, e->d_name) >= (int)sizeof other) other[0] = 0;
            continue;
        }
        uint32_t r;
        aos_hal_random(&r, sizeof r);
        if (r % ++seen == 0 && snprintf(out, len, "%s/%s", dir, e->d_name) < (int)len) got = true;
    }
    closedir(d);
    if (!got && other[0]) { snprintf(out, len, "%s", other); got = true; }
    return got;
}

static void dim(uint16_t *px, size_t n, unsigned target)
{
    uint64_t sum = 0;                   /* mean luma, from a pixel in 16 */
    size_t m = 0;
    for (size_t i = 0; i < n; i += 16, m++) {
        uint16_t p = px[i];
        unsigned r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
        sum += (r * 77 + g * 150 + b * 29) >> 8;
    }
    unsigned mean = m ? (unsigned)(sum / m) : 0;
    unsigned k = mean ? target * 256 / mean : DIM_MAX;
    if (k > DIM_MAX) k = DIM_MAX;
    if (k < DIM_MIN) k = DIM_MIN;
    for (size_t i = 0; i < n; i++) {
        uint16_t p = px[i];
        unsigned r = ((p >> 11) & 0x1F) * k >> 8, g = ((p >> 5) & 0x3F) * k >> 8, b = (p & 0x1F) * k >> 8;
        px[i] = (uint16_t)(r << 11 | g << 5 | b);
    }
}

/* The photo's mean colour, then three fifths of the way to white: a colour
 * of its own that a big clock can be drawn in and still read on it */
static uint32_t tint_of(const uint16_t *px, size_t n)
{
    uint32_t r = 0, g = 0, b = 0, m = 0;
    for (size_t i = 0; i < n; i += 37, m++) {
        uint16_t p = px[i];
        r += (p >> 11) << 3;
        g += ((p >> 5) & 0x3F) << 2;
        b += (p & 0x1F) << 3;
    }
    if (!m) return 0;
    r /= m; g /= m; b /= m;
    r += (255 - r) * 3 / 5; g += (255 - g) * 3 / 5; b += (255 - b) * 3 / 5;
    return r << 16 | g << 8 | b;
}

static void worker(void *arg)
{
    job_t *j = arg;
    int w = 0, h = 0;
    if (j->album[0]) {
        char pick[PATH_MAX_];
        if (album_pick(j->album, j->path, pick, sizeof pick)) memcpy(j->path, pick, sizeof pick);
        else aos_hal_log("ui", "wallpaper: no photos in %s", j->album);
    }
    uint16_t *px = aos_hal_image_decode(j->path, 0, 0, j->w, j->h, true, &w, &h, NULL, NULL);
    if (px && (w != j->w || h != j->h)) { aos_hal_image_free(px); px = NULL; }
    uint32_t tint = px ? tint_of(px, (size_t)w * h) : 0;
    if (px) dim(px, (size_t)w * h, j->which == AOS_WALL_HOME ? LUMA_HOME : LUMA_LOCK);
    else aos_hal_log("ui", "wallpaper: %s could not be read", j->path);
    wall_t *v = &W[j->which];
    aos_hal_mutex_lock(s_mx);
    if (v->ready && v->new_px) aos_hal_image_free(v->new_px);   /* an older one nobody took */
    v->new_px = px;
    v->new_w = w;
    v->new_h = h;
    v->new_gen = j->gen;
    v->new_tint = tint;
    snprintf(v->new_path, sizeof v->new_path, "%s", j->path);
    v->new_thread = true;
    v->ready = true;
    aos_hal_mutex_unlock(s_mx);
    free(j);
}

/* The size a photo is kept at: the screen's for the home screen, half of
 * it each way for the lock screen's (see the top) */
static void photo_size(int which, int *w, int *h)
{
    const aos_geo_t *g = aos_ui_geo();
    int k = which == AOS_WALL_LOCK ? 2 : 1;
    *w = g->w / k;
    *h = g->h / k;
}

/* A slot holds the photo whichever way the screen is turned */
static size_t slot_bytes(int which)
{
    size_t n = (size_t)aos_hal_screen_w() * aos_hal_screen_h() * 2;
    return which == AOS_WALL_LOCK ? n / 4 : n;
}

static void slot_take(wall_t *v)
{
    if (!v->slot) v->slot = malloc(slot_bytes((int)(v - W)));   /* PSRAM: over 1 KB */
}

static bool configured(int i)
{
    return W[i].path[0] || (i == AOS_WALL_LOCK && s_album[0]);
}

/* The lock screen shows the home screen's photo: the same file, no album */
static bool lock_shares(void)
{
    return !s_album[0] && W[AOS_WALL_LOCK].path[0] &&
           !strcmp(W[AOS_WALL_LOCK].path, W[AOS_WALL_HOME].path);
}

static void slot_free(wall_t *v)
{
    free(v->slot);
    v->slot = NULL;
}

static bool s_started;              /* the boot is over: decodes may run */
static bool s_spared;               /* let go for a game (see the top) */

static void request(int which);

static void request(int which)
{
    wall_t *v = &W[which];
    bool album = which == AOS_WALL_LOCK && s_album[0];
    if (which == AOS_WALL_LOCK && lock_shares()) {
        /* nothing of its own: the home screen's photo is the one */
        aos_wall_photo_release(AOS_WALL_LOCK);
        if (s_started && !W[AOS_WALL_HOME].px && !W[AOS_WALL_HOME].want) request(AOS_WALL_HOME);
        aos_lock_layout();
        return;
    }
    v->gen++;
    v->want = true;
    if (!s_started) return;             /* aos_wall_photo_start() asks for it */
    if (!v->path[0] && !album) {                  /* none: back to the gradient at once */
        aos_hal_mutex_lock(s_mx);
        if (v->ready && v->new_px) aos_hal_image_free(v->new_px);
        v->new_px = NULL;
        v->new_w = v->new_h = 0;
        v->new_gen = v->gen;
        v->new_thread = false;
        v->ready = true;
        aos_hal_mutex_unlock(s_mx);
    } else {
        job_t *j = malloc(sizeof *j);
        if (!j) return;
        j->which = which;
        j->gen = v->gen;
        photo_size(which, &j->w, &j->h);
        snprintf(j->path, sizeof j->path, "%s", v->path);
        snprintf(j->album, sizeof j->album, "%s", album ? s_album : "");
        if (!aos_hal_thread_start("wallpaper", worker, j, 8192, 3)) { free(j); v->want = false; return; }
        v->inflight++;
    }
    lv_timer_resume(s_poll);
}

static void apply_home(void)
{
    wall_t *v = &W[AOS_WALL_HOME];
    if (!v->px) {
        if (s_home_img) lv_obj_delete(s_home_img);
        s_home_img = NULL;
        return;
    }
    if (!s_home_img) {
        s_home_img = lv_image_create(aos_ui_wall());
        lv_obj_remove_flag(s_home_img, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_image_set_src(s_home_img, NULL);     /* the same dsc with new pixels: read it again */
    lv_image_set_src(s_home_img, &v->dsc);
    lv_obj_center(s_home_img);
}

/* LVGL's task: takes what the worker finished */
static void poll_cb(lv_timer_t *t)
{
    for (int i = 0; i < 2; i++) {
        wall_t *v = &W[i];
        aos_hal_mutex_lock(s_mx);
        bool ready = v->ready;
        uint16_t *px = v->new_px;
        int w = v->new_w, h = v->new_h;
        uint32_t gen = v->new_gen;
        bool from_thread = v->new_thread;
        uint32_t tint = v->new_tint;
        char path[PATH_MAX_];
        snprintf(path, sizeof path, "%s", v->new_path);
        v->ready = false;
        v->new_px = NULL;
        aos_hal_mutex_unlock(s_mx);
        if (!ready) continue;
        if (from_thread && v->inflight > 0) v->inflight--;
        if (gen != v->gen) { if (px) aos_hal_image_free(px); continue; }   /* asked for again since */
        v->want = false;
        uint16_t *old = v->px;
        lv_image_cache_drop(&v->dsc);
        /* into the slot (see the top): the decoder's buffer goes back */
        if (px) {
            slot_take(v);
            if (v->slot && (size_t)w * h * 2 <= slot_bytes(i)) {
                memcpy(v->slot, px, (size_t)w * h * 2);
                aos_hal_image_free(px);
                px = v->slot;
            }
        }
        if (old == v->slot) old = NULL;                 /* the slot is not freed */
        if (!px) slot_free(v);                          /* no photo: no slot either */
        v->px = px;
        v->w = w;
        v->h = h;
        if (px) v->tint = tint;
        if (px && from_thread && i == AOS_WALL_LOCK && s_album[0]) snprintf(v->path, sizeof v->path, "%s", path);
        memset(&v->dsc, 0, sizeof v->dsc);
        if (px) {
            v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            v->dsc.header.w = (uint32_t)w;
            v->dsc.header.h = (uint32_t)h;
            v->dsc.header.stride = (uint32_t)w * 2;
            v->dsc.data = (const uint8_t *)px;
            v->dsc.data_size = (uint32_t)w * h * 2;
        }
        if (i == AOS_WALL_HOME) apply_home();
        if (i == AOS_WALL_LOCK || lock_shares()) aos_lock_layout();
        if (old) aos_hal_image_free(old);
    }
    if (!W[0].want && !W[1].want && !W[0].inflight && !W[1].inflight) lv_timer_pause(t);
}

/* Lets the photo go (want false) or has it decoded again (true) */
void aos_wall_photo_want(int which, bool want)
{
    if (which < 0 || which > 1 || !s_mx) return;
    wall_t *v = &W[which];
    if (!v->path[0] && !(which == AOS_WALL_LOCK && s_album[0])) return;
    if (want && s_spared) return;       /* a game has their memory */
    if (which == AOS_WALL_LOCK && lock_shares()) {
        if (want) aos_wall_photo_want(AOS_WALL_HOME, true);
        return;                         /* the home screen's is not the lock's to let go */
    }
    if (want) {
        if (!v->px && !v->want) request(which);
        return;
    }
    /* in its slot it stays decoded: nothing to give back (see the top) */
    if (v->px && v->px == v->slot) return;
    aos_wall_photo_release(which);
}

bool aos_wall_photo_release(int which)
{
    if (which < 0 || which > 1 || !s_mx) return false;
    wall_t *v = &W[which];
    bool had = v->px || v->slot;
    v->gen++;                           /* whatever is being decoded is dropped when it comes */
    v->want = false;
    if (which == AOS_WALL_HOME && s_home_img) {
        lv_obj_delete(s_home_img);
        s_home_img = NULL;
    }
    if (v->px) {
        lv_image_cache_drop(&v->dsc);
        if (v->px != v->slot) aos_hal_image_free(v->px);
        v->px = NULL;
        v->w = v->h = 0;
        memset(&v->dsc, 0, sizeof v->dsc);
    }
    slot_free(v);
    return had;
}

void aos_ui_wall_photo_spare(bool spare)
{
    if (!spare) aos_wall_photo_want(AOS_WALL_HOME, true);      /* back, before the way home */
    else if (aos_ui_current_app()) aos_wall_photo_release(AOS_WALL_HOME);
}

uint32_t aos_wall_photo_tint(int which)
{
    if (which == AOS_WALL_LOCK && lock_shares()) which = AOS_WALL_HOME;
    return which >= 0 && which <= 1 && W[which].path[0] ? W[which].tint : 0;
}

const lv_image_dsc_t *aos_wall_photo_dsc(int which)
{
    if (which == AOS_WALL_LOCK && lock_shares()) which = AOS_WALL_HOME;
    if (which < 0 || which > 1 || !W[which].px) return NULL;
    return &W[which].dsc;
}

uint8_t aos_wall_photo_lock_dim(void)
{
    /* the home screen's photo was darkened to LUMA_HOME; the lock's wants
     * LUMA_LOCK: black over it for the difference */
    return lock_shares() ? (uint8_t)(255 - 255 * LUMA_LOCK / LUMA_HOME) : 0;
}

/* A photo changed: the lock screen starts or stops sharing the home
 * screen's, and gives back its own or decodes it */
static void lock_sync(void)
{
    if (lock_shares()) {
        if (aos_wall_photo_release(AOS_WALL_LOCK)) aos_lock_layout();
    } else if (configured(AOS_WALL_LOCK) && !W[AOS_WALL_LOCK].px && !W[AOS_WALL_LOCK].want) {
        request(AOS_WALL_LOCK);
    }
}

void aos_ui_wall_photo_set(int which, const char *path)
{
    if (which < 0 || which > 1 || !s_mx) return;
    if (which == AOS_WALL_LOCK && s_album[0]) {       /* one photo, or none: no album any more */
        s_album[0] = 0;
        aos_hal_pref_set_str("lock_album", "");
    }
    snprintf(W[which].path, sizeof W[which].path, "%s", path ? path : "");
    aos_hal_pref_set_str(PREF[which], W[which].path);
    request(which);
    lock_sync();
}

bool aos_ui_wall_photo(int which, char *out, size_t len)
{
    if (which < 0 || which > 1 || !W[which].path[0]) return false;
    if (out && len) snprintf(out, len, "%s", W[which].path);
    return true;
}

void aos_ui_lock_album_set(const char *dir)
{
    if (!s_mx) return;
    snprintf(s_album, sizeof s_album, "%s", dir ? dir : "");
    aos_hal_pref_set_str("lock_album", s_album);
    if (!s_album[0]) {
        /* the photo on show stays, as the one photo */
        aos_hal_pref_set_str(PREF[AOS_WALL_LOCK], W[AOS_WALL_LOCK].path);
        lock_sync();
        return;
    }
    request(AOS_WALL_LOCK);
}

bool aos_ui_lock_album(char *out, size_t len)
{
    if (!s_album[0]) return false;
    if (out && len) snprintf(out, len, "%s", s_album);
    return true;
}

/* The board locks: with an album, another of its photos (true); without,
 * nothing to do here (false) */
bool aos_wall_photo_next(void)
{
    if (!s_mx || !s_album[0]) return false;
    request(AOS_WALL_LOCK);
    return true;
}

void aos_wall_photo_reserve(void)
{
    if (!aos_hal_pref_get_str("lock_album", s_album, sizeof s_album)) s_album[0] = 0;
    for (int i = 0; i < 2; i++) {
        if (!aos_hal_pref_get_str(PREF[i], W[i].path, sizeof W[i].path)) W[i].path[0] = 0;
        if (configured(i) && !(i == AOS_WALL_LOCK && lock_shares()))
            slot_take(&W[i]);           /* now, at the bottom of the PSRAM */
    }
}

void aos_wall_photo_init(void)
{
    if (s_mx) return;
    s_mx = aos_hal_mutex_create();
    s_poll = lv_timer_create(poll_cb, 100, NULL);
    lv_timer_pause(s_poll);
}

void aos_wall_photo_for_game(bool game)
{
    if (!s_mx) return;
    if (game) {
        if (s_spared) return;
        s_spared = true;
        aos_wall_photo_release(AOS_WALL_HOME);
        if (!aos_lock_is_locked()) aos_wall_photo_release(AOS_WALL_LOCK);
        aos_hal_log("ui", "wallpaper photos let go for a full-screen app");
        return;
    }
    if (!s_spared) return;
    s_spared = false;
    aos_hal_log("ui", "wallpaper photos back");
    if (!s_started) return;
    for (int i = 0; i < 2; i++)         /* the places first, where the game was */
        if (configured(i) && !(i == AOS_WALL_LOCK && lock_shares())) slot_take(&W[i]);
    for (int i = 0; i < 2; i++)
        if (configured(i) && !W[i].px) request(i);
}

void aos_wall_photo_start(void)
{
    if (!s_mx || s_started) return;
    s_started = true;
    for (int i = 0; i < 2; i++) {
        if (!configured(i)) continue;
        if (!(i == AOS_WALL_LOCK && lock_shares()))
            slot_take(&W[i]);           /* let go during the scan (aos_ui_make_room) */
        request(i);
    }
}

/* The screen turned: each photo again, cropped to the new shape */
void aos_wall_photo_relayout(void)
{
    if (!s_mx) return;
    for (int i = 0; i < 2; i++) {
        int w, h;
        photo_size(i, &w, &h);
        if (W[i].path[0] && (W[i].w != w || W[i].h != h)) request(i);
    }
    if (s_home_img) lv_obj_center(s_home_img);
}
