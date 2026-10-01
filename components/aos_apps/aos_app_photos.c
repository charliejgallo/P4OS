/*
 * P4OS - Fotos: the photos on the card.
 *
 * A grid of square thumbnails, as on the phone, with the subfolders as
 * albums on a shelf above it, and a viewer with pinch, double tap, pan with
 * fling, swipe to the next photo and swipe down to close.
 *
 * From the watch (_pending/aos_app_photos.c) it keeps the viewer's model:
 * the view is a scale and an offset that ease towards a target, clamped so
 * the photo never leaves a gap, smooth (antialiased) only at rest, and the
 * app takes NO_SWIPE | LONG_DRAG while a photo is up. What changed:
 *
 *   - The watch listed names and LVGL decoded the whole photo in the LVGL
 *     task (TJPGD). Here every picture comes from the HAL's decoder
 *     (aos_hal_image_decode) on the picture loader's thread
 *     (aos_app_image.c), already at the size it is drawn: a 176 px square
 *     for a tile, the screen for the viewer, and a bigger copy only once
 *     the person zooms in. Nothing is decoded in LVGL's task.
 *   - The grid is ONE object that draws the tiles in view itself, so a
 *     folder of a thousand photos is a thousand names and not a thousand
 *     LVGL objects. Thumbnails are asked for the rows in view (and three
 *     more each way), newest first, the ones that scrolled away are
 *     cancelled before they start, 128 stay in memory (8 MB of PSRAM), and
 *     every one is also kept on the card in <photos>/.thumbs, so a folder
 *     seen once opens at once.
 *   - The neighbours of the photo on show are decoded too: a swipe slides
 *     the next one in instead of waiting for it.
 *
 * resize() re-lays the screen out in place, so turning it keeps the
 * thumbnails, the scroll and the open photo.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_gesture.h"
#include "aos_sys_glyphs.h"
#include "aos_app_image.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_PHOTOS      2000
#define NAMES_BYTES     (128 * 1024)
#define MAX_ALBUMS      48
#define ALBUM_DEPTH     3               /* DCIM/100CANON is two */
#define SIDEBAR_W       360             /* landscape: 1280 - 360 = five tiles exactly */
#define TILE            176             /* thumbnail side, both orientations */
#define GAP             4
#define AHEAD_ROWS      3               /* asked for beyond the view, each way */
#define CACHE_N         128             /* thumbnails in memory: 62 KB each */
#define ALBUM_PX        220

#define TAG_THUMB       0x01000000u
#define TAG_ALBUM       0x02000000u
#define TAG_FIT         0x03000000u
#define TAG_HIRES       0x04000000u
#define TAG_KIND(t)     ((t) & 0x0F000000u)
#define TAG_IDX(t)      ((int)((t) & 0x00FFFFFFu))

#define HIRES_PIXELS    (4u * 1024u * 1024u)    /* the zoomed copy: 8 MB at most */
#define DOUBLE_TAP_ZOOM 3.0f            /* times the whole-photo view */
#define EASE            0.45f           /* of the remaining distance per frame */
#define FLING_DECAY     0.90f           /* speed kept per 16 ms frame */
#define PAGE_FLING      500.0f          /* px/s sideways that changes photo */
#define CLOSE_FLING     900.0f          /* px/s down that closes it */
#define PAGE_GAP        40              /* between a photo and the next */
#define FRAME_MS        16

#define C_TINT          AOS_C_ACCENT
#define C_TILE          lv_color_hex(0x1C1C1E)

enum { ST_NONE = 0, ST_QUEUED, ST_FAILED };

typedef struct {
    int            photo;               /* -1: free */
    img_result_t   r;
    lv_image_dsc_t dsc;
} thumb_t;

typedef struct {
    int            idx;                 /* -1: nothing here */
    bool           asked, failed;
    img_result_t   r;
    lv_image_dsc_t dsc;
} vimg_t;

/* ---- state: survives hiding, turning, everything but closing -------------- */

typedef struct {
    char     dir[240];                  /* the folder on show */
    int      count;
    uint32_t name_off[MAX_PHOTOS];
    char     names[NAMES_BYTES];
    uint32_t names_used;
    int16_t  slot[MAX_PHOTOS];          /* in cache[], -1 if not */
    uint8_t  st[MAX_PHOTOS];

    /* Albums: every folder under the photos folder that has photos, flat
     * (a camera's DCIM/100CANON is one album, named 100CANON), scanned once
     * when the app opens. */
    int      nalb, alb_total, root_count;
    struct {
        char rel[200];                  /* under the photos folder */
        char first[128];
        int  count;
        img_result_t r;
        lv_image_dsc_t dsc;
        lv_obj_t *img, *side_img;
    } alb[MAX_ALBUMS];

    thumb_t  cache[CACHE_N];
    int32_t  parent_scroll;             /* the root's, while in an album */
    bool     scanned;

    /* viewer */
    bool     viewing;
    int      cur;
    vimg_t   v[3];                      /* previous, on show, next */
    vimg_t   hi;                        /* the zoomed copy of the one on show */
} photos_t;

AOS_BSS_PSRAM static photos_t P;

static struct {
    aos_app_t *self;
    lv_obj_t  *root;
    int32_t    W, H;
    bool       land;
    int        cols, rows;
    int32_t    gx, gw;                  /* the grid's margin and the width it has */
    lv_obj_t  *scroll, *grid;
    lv_timer_t *poll;
    int        pressed;
    uint32_t   last_sched;

    lv_obj_t  *viewer, *touch, *img[3], *spinner, *top, *bottom;
    lv_obj_t  *v_title, *v_sub, *v_info, *err, *err_msg;
    lv_timer_t *anim;
    int32_t    vw, vh, vx0, vy0;
    float      fit, s, ox, oy, ts, tox, toy, vxs, vys;
    float      pan, tpan, drop, tdrop;
    int        step;                    /* a page turn under way: -1, +1 */
    int        mode;                    /* this drag: 0 undecided, 1 page, 2 close, 3 pan */
    bool       touching, chrome, smooth;
    int        src_w, src_h;
} U;

static void build(void);
static void schedule(void);
static void viewer_open(int index);
static void viewer_close(void);
static void apply_view(void);

/* ---- names ----------------------------------------------------------------- */

static const char *name_at(int i) { return P.names + P.name_off[i]; }

static bool is_photo(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") ||
                   !strcasecmp(dot, ".png") || !strcasecmp(dot, ".bmp"));
}

static bool is_dir_entry(const char *dir, const struct dirent *e)
{
    if (e->d_type == DT_DIR) return true;
    if (e->d_type != DT_UNKNOWN) return false;
    char full[512];
    struct stat st;
    snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
    return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

static int name_cmp_idx(const void *a, const void *b)
{
    return img_name_cmp(P.names + *(const uint32_t *)a, P.names + *(const uint32_t *)b);
}

static int album_cmp(const void *a, const void *b)
{
    return img_name_cmp(((const char *)a), ((const char *)b));  /* rel is the first field */
}

static const char *album_name(int a)
{
    const char *s = strrchr(P.alb[a].rel, '/');
    return s ? s + 1 : P.alb[a].rel;
}

/* Which album is on show, -1 at the root. */
static int current_album(void)
{
    size_t rl = strlen(aos_hal_path_photos());
    if (strncmp(P.dir, aos_hal_path_photos(), rl) != 0 || P.dir[rl] != '/') return -1;
    for (int a = 0; a < P.nalb; a++) {
        if (strcmp(P.alb[a].rel, P.dir + rl + 1) == 0) return a;
    }
    return -1;
}

static bool at_root(void) { return strcmp(P.dir, aos_hal_path_photos()) == 0; }

static void photo_path(int i, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", P.dir, name_at(i));
}

static void thumbs_dir(char *out, size_t len)
{
    snprintf(out, len, "%s/.thumbs", aos_hal_path_photos());
}

/* What belongs to the folder on show: its thumbnails and the viewer's. */
static void free_folder(void)
{
    for (int i = 0; i < P.count; i++) {
        if (P.st[i] == ST_QUEUED) img_cancel(IMG_OWNER_PHOTOS, TAG_THUMB | (uintptr_t)i);
    }
    for (int k = 0; k < CACHE_N; k++) {
        img_free(&P.cache[k].r);
        P.cache[k].photo = -1;
    }
    for (int k = 0; k < 3; k++) {
        img_free(&P.v[k].r);
        memset(&P.v[k], 0, sizeof P.v[k]);
        P.v[k].idx = -1;
    }
    img_free(&P.hi.r);
    memset(&P.hi, 0, sizeof P.hi);
    P.hi.idx = -1;
}

static void free_all(void)
{
    img_cancel_all(IMG_OWNER_PHOTOS);
    for (int k = 0; k < CACHE_N; k++) {
        img_free(&P.cache[k].r);
        P.cache[k].photo = -1;
    }
    for (int a = 0; a < MAX_ALBUMS; a++) {
        img_free(&P.alb[a].r);
        P.alb[a].img = P.alb[a].side_img = NULL;
    }
    for (int k = 0; k < 3; k++) {
        img_free(&P.v[k].r);
        memset(&P.v[k], 0, sizeof P.v[k]);
        P.v[k].idx = -1;
    }
    img_free(&P.hi.r);
    memset(&P.hi, 0, sizeof P.hi);
    P.hi.idx = -1;
}

/* The photos of the folder on show. */
static void scan(void)
{
    free_folder();
    P.count = 0;
    P.names_used = 0;
    DIR *d = opendir(P.dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || !is_photo(e->d_name)) continue;
            size_t len = strlen(e->d_name) + 1;
            if (P.count >= MAX_PHOTOS || P.names_used + len > NAMES_BYTES) continue;
            P.name_off[P.count++] = P.names_used;
            memcpy(P.names + P.names_used, e->d_name, len);
            P.names_used += (uint32_t)len;
        }
        closedir(d);
    }
    qsort(P.name_off, (size_t)P.count, sizeof P.name_off[0], name_cmp_idx);
    for (int i = 0; i < P.count; i++) {
        P.slot[i] = -1;
        P.st[i] = ST_NONE;
    }
    P.scanned = true;
}

/* One folder of the album walk: its photos counted, its subfolders walked. */
static void walk(const char *rel, int depth)
{
    char dir[400];
    if (rel[0]) snprintf(dir, sizeof dir, "%s/%s", aos_hal_path_photos(), rel);
    else snprintf(dir, sizeof dir, "%s", aos_hal_path_photos());
    DIR *d = opendir(dir);
    if (!d) return;
    int count = 0;
    char first[128] = "";
    char subs[8][96];
    int nsub = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (is_photo(e->d_name)) {
            count++;
            if (strlen(e->d_name) < sizeof first && (!first[0] || img_name_cmp(e->d_name, first) < 0)) {
                snprintf(first, sizeof first, "%s", e->d_name);
            }
        } else if (depth < ALBUM_DEPTH && nsub < 8 && strlen(e->d_name) < sizeof subs[0] &&
                   is_dir_entry(dir, e)) {
            snprintf(subs[nsub++], sizeof subs[0], "%s", e->d_name);
        }
    }
    closedir(d);
    if (!rel[0]) {
        P.root_count = count;
    } else if (count && P.nalb < MAX_ALBUMS) {
        snprintf(P.alb[P.nalb].rel, sizeof P.alb[0].rel, "%s", rel);
        snprintf(P.alb[P.nalb].first, sizeof P.alb[0].first, "%s", first);
        P.alb[P.nalb].count = count;
        memset(&P.alb[P.nalb].r, 0, sizeof P.alb[0].r);
        P.alb[P.nalb].img = P.alb[P.nalb].side_img = NULL;
        P.nalb++;
        P.alb_total += count;
    }
    for (int k = 0; k < nsub; k++) {
        char sub[200];
        int n = rel[0] ? snprintf(sub, sizeof sub, "%s/%s", rel, subs[k])
                       : snprintf(sub, sizeof sub, "%.95s", subs[k]);
        if (n < 0 || n >= (int)sizeof sub) continue;
        walk(sub, depth + 1);
    }
}

static void scan_albums(void)
{
    for (int a = 0; a < MAX_ALBUMS; a++) img_free(&P.alb[a].r);
    P.nalb = P.alb_total = P.root_count = 0;
    walk("", 0);
    qsort(P.alb, (size_t)P.nalb, sizeof P.alb[0], album_cmp);
}

/* ---- the grid -------------------------------------------------------------- */

static void geometry(void)
{
    U.gw = U.land ? U.W - SIDEBAR_W : U.W;
    U.cols = (U.gw + GAP) / (TILE + GAP);
    if (U.cols < 3) U.cols = 3;
    int32_t used = U.cols * TILE + (U.cols - 1) * GAP;
    U.gx = (U.gw - used) / 2;
    U.rows = (P.count + U.cols - 1) / U.cols;
}

static void tile_area(int i, lv_area_t *a)
{
    lv_area_t c;
    lv_obj_get_coords(U.grid, &c);
    int r = i / U.cols, col = i % U.cols;
    a->x1 = c.x1 + U.gx + col * (TILE + GAP);
    a->y1 = c.y1 + r * (TILE + GAP);
    a->x2 = a->x1 + TILE - 1;
    a->y2 = a->y1 + TILE - 1;
}

static void tile_invalidate(int i)
{
    if (!U.grid || i < 0 || i >= P.count) return;
    lv_area_t a;
    tile_area(i, &a);
    lv_obj_invalidate_area(U.grid, &a);
}

static void grid_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(U.grid, &c);
    const lv_area_t *clip = &layer->_clip_area;
    int32_t rowh = TILE + GAP;
    int r0 = (int)((clip->y1 - c.y1) / rowh), r1 = (int)((clip->y2 - c.y1) / rowh);
    if (r0 < 0) r0 = 0;
    if (r1 >= U.rows) r1 = U.rows - 1;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = C_TILE;
    rd.bg_opa = LV_OPA_COVER;
    lv_draw_image_dsc_t id;
    lv_draw_image_dsc_init(&id);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &aos_sym_44;
    ld.color = lv_color_hex(0x48484A);
    ld.align = LV_TEXT_ALIGN_CENTER;
    ld.text = AOS_SYM_IMAGE_BROKEN_VARIANT;

    for (int r = r0; r <= r1; r++) {
        for (int col = 0; col < U.cols; col++) {
            int i = r * U.cols + col;
            if (i >= P.count) break;
            lv_area_t a = { c.x1 + U.gx + col * (TILE + GAP), c.y1 + r * rowh, 0, 0 };
            a.x2 = a.x1 + TILE - 1;
            a.y2 = a.y1 + TILE - 1;
            int k = P.slot[i];
            if (k >= 0) {
                id.src = &P.cache[k].dsc;
                lv_draw_image(layer, &id, &a);
            } else {
                lv_draw_rect(layer, &rd, &a);
                if (P.st[i] == ST_FAILED) {
                    lv_area_t g = { a.x1, a.y1 + TILE / 2 - 24, a.x2, a.y1 + TILE / 2 + 24 };
                    lv_draw_label(layer, &ld, &g);
                }
            }
            if (i == U.pressed) {
                lv_draw_rect_dsc_t pd;
                lv_draw_rect_dsc_init(&pd);
                pd.bg_color = lv_color_black();
                pd.bg_opa = LV_OPA_40;
                lv_draw_rect(layer, &pd, &a);
            }
        }
    }
}

static int tile_at_point(void)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t c;
    lv_obj_get_coords(U.grid, &c);
    int32_t x = p.x - c.x1 - U.gx, y = p.y - c.y1;
    if (x < 0 || y < 0) return -1;
    int col = (int)(x / (TILE + GAP)), r = (int)(y / (TILE + GAP));
    if (col >= U.cols || x % (TILE + GAP) >= TILE) return -1;
    int i = r * U.cols + col;
    return i < P.count ? i : -1;
}

static void grid_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        U.pressed = tile_at_point();
        tile_invalidate(U.pressed);
    } else if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_RELEASED || code == LV_EVENT_SCROLL_BEGIN) {
        int was = U.pressed;
        U.pressed = -1;
        tile_invalidate(was);
    } else if (code == LV_EVENT_CLICKED) {
        int i = tile_at_point();
        if (i >= 0) viewer_open(i);
    }
}

/* A thumbnail in: into a free slot, or the one of the photo furthest from
 * the view; if every slot is in view (never, at 128) it is dropped. */
static void thumb_in(img_result_t *r, int first, int last)
{
    int i = TAG_IDX(r->tag);
    if (i >= P.count || P.slot[i] >= 0) {
        img_free(r);
        return;
    }
    P.st[i] = r->px ? ST_NONE : ST_FAILED;
    if (!r->px) {
        tile_invalidate(i);
        return;
    }
    int best = -1, far = -1, mid = (first + last) / 2;
    for (int k = 0; k < CACHE_N; k++) {
        if (P.cache[k].photo < 0) { best = k; break; }
        int p = P.cache[k].photo;
        int dist = p < mid ? mid - p : p - mid;
        if ((p < first || p > last) && dist > far) {
            far = dist;
            best = k;
        }
    }
    if (best < 0) {
        img_free(r);
        return;
    }
    thumb_t *t = &P.cache[best];
    if (t->photo >= 0) {
        P.slot[t->photo] = -1;
        tile_invalidate(t->photo);
        img_free(&t->r);                /* off the screen already: it is out of view */
    }
    t->photo = i;
    t->r = *r;
    memset(&t->dsc, 0, sizeof t->dsc);
    t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    t->dsc.header.w = (uint32_t)r->w;
    t->dsc.header.h = (uint32_t)r->h;
    t->dsc.header.stride = (uint32_t)r->w * 2u;
    t->dsc.data_size = (uint32_t)(r->w * r->h * 2);
    t->dsc.data = (const uint8_t *)r->px;
    P.slot[i] = (int16_t)best;
    tile_invalidate(i);
}

/* The rows in view, in the grid's own index space. */
static bool view_window(int *vis0, int *vis1, int *first, int *last)
{
    if (!U.grid || !U.scroll || P.count == 0) return false;
    lv_area_t g, s;
    lv_obj_get_coords(U.grid, &g);
    lv_obj_get_coords(U.scroll, &s);
    int32_t rowh = TILE + GAP;
    int32_t top = s.y1 - g.y1, bot = s.y2 - g.y1;
    int r0 = top < 0 ? 0 : (int)(top / rowh), r1 = bot < 0 ? -1 : (int)(bot / rowh);
    if (r1 >= U.rows) r1 = U.rows - 1;
    int w0 = r0 - AHEAD_ROWS < 0 ? 0 : r0 - AHEAD_ROWS;
    int w1 = r1 + AHEAD_ROWS >= U.rows ? U.rows - 1 : r1 + AHEAD_ROWS;
    if (r1 < r0) {                      /* the grid is below the view yet */
        r0 = r1 = 0;
        w0 = 0;
        w1 = AHEAD_ROWS < U.rows ? AHEAD_ROWS : U.rows - 1;
    }
    *vis0 = r0 * U.cols;
    *vis1 = (r1 + 1) * U.cols - 1;
    *first = w0 * U.cols;
    *last = (w1 + 1) * U.cols - 1;
    if (*vis1 >= P.count) *vis1 = P.count - 1;
    if (*last >= P.count) *last = P.count - 1;
    return true;
}

static void ask_thumb(int i, const char *cache)
{
    if (P.slot[i] >= 0 || P.st[i] != ST_NONE) return;
    char path[400];
    photo_path(i, path, sizeof path);
    img_job_t job = { .path = path, .w = TILE, .h = TILE, .fill = true, .cache_dir = cache };
    if (img_request(IMG_OWNER_PHOTOS, TAG_THUMB | (uintptr_t)i, &job)) P.st[i] = ST_QUEUED;
}

/* Asks for what is in view and a bit beyond, cancels what went out of it.
 * The loader takes the newest first: the rows beyond are asked first, then
 * the ones in view from the bottom up, so the top left comes in first. */
static void schedule(void)
{
    int vis0, vis1, first, last;
    if (U.viewer && !lv_obj_has_flag(U.viewer, LV_OBJ_FLAG_HIDDEN)) return;
    if (!view_window(&vis0, &vis1, &first, &last)) return;
    for (int i = 0; i < P.count; i++) {
        if (P.st[i] == ST_QUEUED && (i < first || i > last)) {
            img_cancel(IMG_OWNER_PHOTOS, TAG_THUMB | (uintptr_t)i);
            P.st[i] = ST_NONE;
        }
    }
    char cache[260];
    thumbs_dir(cache, sizeof cache);
    for (int i = last; i > vis1; i--) ask_thumb(i, cache);
    for (int i = first; i < vis0; i++) ask_thumb(i, cache);
    for (int i = vis1; i >= vis0; i--) ask_thumb(i, cache);
}

static void scroll_cb(lv_event_t *e)
{
    (void)e;
    uint32_t now = lv_tick_get();
    if (now - U.last_sched >= 50) {     /* a fling scrolls every frame */
        U.last_sched = now;
        schedule();
    }
}

/* ---- the viewer: pictures --------------------------------------------------- */

static void vimg_free(vimg_t *v)
{
    if (v->idx >= 0 && v->asked && !v->r.px) {         /* still on its way: not any more */
        img_cancel(IMG_OWNER_PHOTOS, TAG_FIT | (uintptr_t)v->idx);
        img_cancel(IMG_OWNER_PHOTOS, TAG_HIRES | (uintptr_t)v->idx);
    }
    img_free(&v->r);
    memset(v, 0, sizeof *v);
    v->idx = -1;
}

static void dsc_of(lv_image_dsc_t *d, const img_result_t *r)
{
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = (uint32_t)r->w;
    d->header.h = (uint32_t)r->h;
    d->header.stride = (uint32_t)r->w * 2u;
    d->data_size = (uint32_t)(r->w * r->h * 2);
    d->data = (const uint8_t *)r->px;
}

static void ask_fit(int k)
{
    vimg_t *v = &P.v[k];
    if (v->idx < 0 || v->asked || v->r.px) return;
    char path[400];
    photo_path(v->idx, path, sizeof path);
    img_job_t job = { .path = path, .w = U.vw, .h = U.vh, .urgent = true };
    v->asked = img_request(IMG_OWNER_PHOTOS, TAG_FIT | (uintptr_t)v->idx, &job);
}

static void ask_hires(void)
{
    vimg_t *c = &P.v[1];
    if (P.hi.asked || P.hi.r.px || !c->r.px || !U.src_w) return;
    if (c->r.w >= U.src_w * 9 / 10) return;         /* on show at its own size already */
    float k = sqrtf((float)HIRES_PIXELS / ((float)U.src_w * (float)U.src_h));
    if (k > 1.0f) k = 1.0f;
    char path[400];
    photo_path(c->idx, path, sizeof path);
    img_job_t job = { .path = path, .w = (int)(U.src_w * k), .h = (int)(U.src_h * k), .urgent = true };
    P.hi.idx = c->idx;
    P.hi.asked = img_request(IMG_OWNER_PHOTOS, TAG_HIRES | (uintptr_t)c->idx, &job);
}

/* Which picture of the one on show is drawn: the zoomed copy once it is in. */
static const vimg_t *shown_cur(void)
{
    if (P.hi.r.px && P.hi.idx == P.v[1].idx) return &P.hi;
    return &P.v[1];
}

static float fit_of(int sw, int sh)
{
    if (sw <= 0 || sh <= 0) return 1.0f;
    float fx = (float)U.vw / (float)sw, fy = (float)U.vh / (float)sh;
    return fx < fy ? fx : fy;
}

static void set_chrome(bool on)
{
    U.chrome = on;
    lv_obj_t *parts[] = { U.top, U.bottom };
    for (unsigned i = 0; i < 2; i++) {
        if (on) lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_smooth(bool on)
{
    if (U.smooth != on) {
        U.smooth = on;
        for (int k = 0; k < 3; k++) lv_image_set_antialias(U.img[k], on);
    }
}

static void update_texts(void)
{
    char buf[160];
    snprintf(buf, sizeof buf, _("%d de %d"), P.cur + 1, P.count);
    lv_label_set_text(U.v_title, buf);
    char safe[256];
    img_text(safe, sizeof safe, name_at(P.cur));
    lv_label_set_text(U.v_sub, safe);
    char path[400];
    photo_path(P.cur, path, sizeof path);
    struct stat st;
    long kb = stat(path, &st) == 0 ? (long)(st.st_size / 1024) : 0;
    char size[32];
    if (kb >= 1024) snprintf(size, sizeof size, "%ld,%ld MB", kb / 1024, (kb % 1024) * 10 / 1024);
    else snprintf(size, sizeof size, "%ld KB", kb);
    if (U.src_w) snprintf(buf, sizeof buf, "%d \xC3\x97 %d  \xC2\xB7  %s", U.src_w, U.src_h, size);
    else snprintf(buf, sizeof buf, "%s", size);
    lv_label_set_text(U.v_info, buf);
}

/* The one on show got its picture (or failed): the view starts at fit. */
static void cur_ready(void)
{
    vimg_t *c = &P.v[1];
    if (c->failed) {
        lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
        char msg[300], safe[256];
        img_text(safe, sizeof safe, name_at(P.cur));
        snprintf(msg, sizeof msg, "%s\n%s", _("No se pudo abrir esta foto"), safe);
        lv_label_set_text(U.err_msg, msg);
        lv_obj_remove_flag(U.err, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!c->r.px) {
        lv_obj_remove_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.err, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.err, LV_OBJ_FLAG_HIDDEN);
    U.src_w = c->r.src_w ? c->r.src_w : c->r.w;
    U.src_h = c->r.src_h ? c->r.src_h : c->r.h;
    U.fit = fit_of(U.src_w, U.src_h);
    U.ts = U.fit;
    U.tox = (U.vw - U.src_w * U.fit) * 0.5f;
    U.toy = (U.vh - U.src_h * U.fit) * 0.5f;
    U.s = U.ts;
    U.ox = U.tox;
    U.oy = U.toy;
    update_texts();
}

/* The three image objects show the three slots. */
static void bind_images(void)
{
    for (int k = 0; k < 3; k++) {
        const vimg_t *v = k == 1 ? shown_cur() : &P.v[k];
        if (v->r.px) {
            lv_image_set_src(U.img[k], &v->dsc);
            lv_obj_remove_flag(U.img[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(U.img[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void load_slots(void)
{
    int want[3] = { P.cur - 1, P.cur, P.cur + 1 };
    for (int k = 0; k < 3; k++) {
        if (want[k] < 0 || want[k] >= P.count) want[k] = -1;
        if (P.v[k].idx != want[k]) {
            vimg_free(&P.v[k]);
            P.v[k].idx = want[k];
        }
    }
    ask_fit(1);                         /* the one on show first: urgent in order */
    ask_fit(2);
    ask_fit(0);
}

static void viewer_in(img_result_t *r)
{
    int i = TAG_IDX(r->tag);
    if (TAG_KIND(r->tag) == TAG_HIRES) {
        if (P.hi.idx == i && P.hi.asked && !P.hi.r.px && r->px && P.v[1].idx == i) {
            P.hi.r = *r;
            dsc_of(&P.hi.dsc, &P.hi.r);
            bind_images();
            apply_view();
        } else {
            img_free(r);
        }
        return;
    }
    for (int k = 0; k < 3; k++) {
        vimg_t *v = &P.v[k];
        if (v->idx == i && v->asked && !v->r.px && !v->failed) {
            if (r->px) {
                v->r = *r;
                dsc_of(&v->dsc, &v->r);
            } else {
                v->failed = true;
            }
            bind_images();
            if (k == 1) cur_ready();
            apply_view();
            return;
        }
    }
    img_free(r);
}

/* ---- the viewer: the view --------------------------------------------------- */

static float max_scale(void)
{
    float m = U.fit * 4.0f, cap = U.fit * 12.0f < 2.0f ? U.fit * 12.0f : 2.0f;
    return m > cap ? m : cap;
}

static bool at_fit(void) { return U.ts <= U.fit * 1.02f; }

static void clamp_target(void)
{
    if (U.ts < U.fit) U.ts = U.fit;
    if (U.ts > max_scale()) U.ts = max_scale();
    float sw = (float)U.src_w * U.ts, sh = (float)U.src_h * U.ts;
    if (sw <= U.vw) U.tox = (U.vw - sw) * 0.5f;
    else if (U.tox > 0.0f) U.tox = 0.0f;
    else if (U.tox < U.vw - sw) U.tox = U.vw - sw;
    if (sh <= U.vh) U.toy = (U.vh - sh) * 0.5f;
    else if (U.toy > 0.0f) U.toy = 0.0f;
    else if (U.toy < U.vh - sh) U.toy = U.vh - sh;
}

/* Places an image of slot k whose photo is sw x sh, showing it at scale s
 * (screen px per photo px) with its corner at (x, y). */
static void place(int k, const vimg_t *v, float s, float x, float y)
{
    if (!v->r.px) return;
    int sw = v->r.src_w ? v->r.src_w : v->r.w;
    uint32_t scale = (uint32_t)(s * (float)sw / (float)v->r.w * 256.0f + 0.5f);
    lv_image_set_scale(U.img[k], scale ? scale : 1);
    lv_obj_set_pos(U.img[k], (int32_t)lroundf(x), (int32_t)lroundf(y));
}

static void apply_view(void)
{
    float page = (float)(U.vw + PAGE_GAP);
    const vimg_t *c = shown_cur();
    place(1, c, U.s, U.ox + U.pan, U.oy + U.drop);
    for (int k = 0; k < 3; k += 2) {
        const vimg_t *v = &P.v[k];
        if (!v->r.px) continue;
        int sw = v->r.src_w ? v->r.src_w : v->r.w, sh = v->r.src_h ? v->r.src_h : v->r.h;
        float f = fit_of(sw, sh);
        float x = (U.vw - sw * f) * 0.5f + U.pan + (k == 0 ? -page : page);
        place(k, v, f, x, (U.vh - sh * f) * 0.5f + U.drop);
    }
    /* swiping down fades the black away, towards the grid */
    int32_t fade = (int32_t)(U.drop > 0 ? U.drop * 255.0f / 500.0f : 0);
    lv_obj_set_style_bg_opa(U.viewer, (lv_opa_t)(fade > 200 ? 55 : 255 - fade), 0);
}

static void anim_kick(void)
{
    set_smooth(false);
    lv_timer_resume(U.anim);
}

static void commit_step(void)
{
    int step = U.step;
    U.step = 0;
    U.pan = U.tpan = 0;
    vimg_free(&P.hi);
    if (step > 0) {
        vimg_free(&P.v[0]);
        P.v[0] = P.v[1];
        P.v[1] = P.v[2];
        memset(&P.v[2], 0, sizeof P.v[2]);
        P.v[2].idx = -1;
    } else {
        vimg_free(&P.v[2]);
        P.v[2] = P.v[1];
        P.v[1] = P.v[0];
        memset(&P.v[0], 0, sizeof P.v[0]);
        P.v[0].idx = -1;
    }
    /* the structs moved: their descriptors point at the same pixels, good */
    P.cur += step;
    load_slots();
    bind_images();
    U.src_w = U.src_h = 0;
    cur_ready();
    if (!P.v[1].r.px) {
        lv_label_set_text_fmt(U.v_title, _("%d de %d"), P.cur + 1, P.count);
    }
}

static void anim_cb(lv_timer_t *t)
{
    (void)t;
    if (!U.touching && (fabsf(U.vxs) > 20.0f || fabsf(U.vys) > 20.0f)) {
        float dt = FRAME_MS / 1000.0f;
        float nx = U.tox + U.vxs * dt, ny = U.toy + U.vys * dt;
        U.tox = nx;
        U.toy = ny;
        clamp_target();
        U.vxs = U.tox != nx ? 0.0f : U.vxs * FLING_DECAY;
        U.vys = U.toy != ny ? 0.0f : U.vys * FLING_DECAY;
    } else if (!U.touching) {
        U.vxs = U.vys = 0.0f;
    }
    float ds = U.ts - U.s, dx = U.tox - U.ox, dy = U.toy - U.oy;
    float dp = U.tpan - U.pan, dd = U.tdrop - U.drop;
    bool settled = fabsf(dx) < 0.5f && fabsf(dy) < 0.5f && fabsf(ds) < 0.002f * U.ts &&
                   fabsf(dp) < 0.5f && fabsf(dd) < 0.5f;
    if (settled) {
        U.s = U.ts;
        U.ox = U.tox;
        U.oy = U.toy;
        U.pan = U.tpan;
        U.drop = U.tdrop;
    } else {
        U.s += ds * EASE;
        U.ox += dx * EASE;
        U.oy += dy * EASE;
        U.pan += dp * (U.touching ? 1.0f : EASE);
        U.drop += dd * (U.touching ? 1.0f : EASE);
    }
    if (settled && U.step) {
        commit_step();
    }
    apply_view();
    if (!at_fit()) ask_hires();
    if (settled && !U.touching && U.vxs == 0 && U.vys == 0 && !U.step) {
        set_smooth(true);
        lv_timer_pause(U.anim);
    }
}

static void zoom_at(float k, float cx, float cy)
{
    float ns = U.ts * k;
    float lo = U.fit * 0.75f, hi = max_scale() * 1.25f;        /* a bit of give */
    if (ns < lo) ns = lo;
    if (ns > hi) ns = hi;
    k = ns / U.ts;
    U.tox = cx - (cx - U.tox) * k;
    U.toy = cy - (cy - U.toy) * k;
    U.ts = ns;
}

static void page_to(int step)
{
    int n = P.cur + step;
    if (n < 0 || n >= P.count) {
        U.tpan = 0;
        return;
    }
    U.step = step;
    U.tpan = -(float)step * (float)(U.vw + PAGE_GAP);
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (U.step) return;                 /* a page turn is finishing */
    bool ready = P.v[1].r.px && U.src_w;
    switch (ev->type) {
    case AOS_GESTURE_TAP:
        set_chrome(!U.chrome);
        break;

    case AOS_GESTURE_DOUBLE_TAP:
        if (!ready) break;
        if (at_fit()) {
            zoom_at(DOUBLE_TAP_ZOOM, ev->x - U.vx0, ev->y - U.vy0);
            set_chrome(false);
        } else {
            U.ts = U.fit;
        }
        clamp_target();
        anim_kick();
        break;

    case AOS_GESTURE_DRAG_BEGIN:
    case AOS_GESTURE_PINCH_BEGIN:
        U.touching = true;
        U.mode = ev->type == AOS_GESTURE_PINCH_BEGIN ? 3 : (ready && !at_fit() ? 3 : 0);
        U.vxs = U.vys = 0;
        anim_kick();
        break;

    case AOS_GESTURE_DRAG: {
        if (U.mode == 0) {
            float ax = fabsf(ev->dx), ay = fabsf(ev->dy);
            if (ax < 0.5f && ay < 0.5f) break;
            U.mode = ay > ax && ev->dy > 0 ? 2 : 1;
        }
        if (U.mode == 1) {
            /* past the first or the last it gives, at a third */
            bool edge = (U.tpan > 0 && P.cur == 0) || (U.tpan < 0 && P.cur == P.count - 1);
            U.tpan += edge ? ev->dx / 3.0f : ev->dx;
            U.pan = U.tpan;
        } else if (U.mode == 2) {
            U.tdrop += ev->dy > 0 || U.tdrop > 0 ? ev->dy : ev->dy / 3.0f;
            U.drop = U.tdrop;
        } else if (ready) {
            float sw = (float)U.src_w * U.ts, sh = (float)U.src_h * U.ts;
            float nx = U.tox + ev->dx, ny = U.toy + ev->dy;
            bool out_x = sw <= U.vw || nx > 0 || nx < U.vw - sw;
            bool out_y = sh <= U.vh || ny > 0 || ny < U.vh - sh;
            U.tox += out_x ? ev->dx / 3.0f : ev->dx;
            U.toy += out_y ? ev->dy / 3.0f : ev->dy;
        }
        break;
    }

    case AOS_GESTURE_DRAG_END:
        U.touching = false;
        if (U.mode == 1) {
            float third = (float)U.vw / 4.0f;
            if (U.tpan < -third || ev->vx < -PAGE_FLING) page_to(1);
            else if (U.tpan > third || ev->vx > PAGE_FLING) page_to(-1);
            else U.tpan = 0;
        } else if (U.mode == 2) {
            if (U.tdrop > 180.0f || ev->vy > CLOSE_FLING) {
                U.mode = 0;
                viewer_close();
                return;
            }
            U.tdrop = 0;
        } else if (ready) {
            U.vxs = ev->vx;
            U.vys = ev->vy;
            clamp_target();
        }
        U.mode = 0;
        anim_kick();
        break;

    case AOS_GESTURE_PINCH:
        if (!ready) break;
        zoom_at(ev->scale, ev->x - U.vx0, ev->y - U.vy0);
        U.tox += ev->dx;
        U.toy += ev->dy;
        if (!at_fit()) set_chrome(false);
        break;

    case AOS_GESTURE_PINCH_END:
        U.touching = false;
        U.mode = 0;
        clamp_target();
        anim_kick();
        break;

    default:
        break;
    }
}

static void view_geometry(void)
{
    lv_obj_update_layout(U.viewer);
    lv_area_t va;
    lv_obj_get_coords(U.viewer, &va);
    U.vx0 = va.x1;
    U.vy0 = va.y1;
    U.vw = lv_area_get_width(&va);
    U.vh = lv_area_get_height(&va);
}

static void viewer_open(int index)
{
    if (index < 0 || index >= P.count) return;
    if (P.cur != index || !P.viewing) vimg_free(&P.hi);
    P.cur = index;
    P.viewing = true;
    U.pan = U.tpan = U.drop = U.tdrop = 0;
    U.step = 0;
    U.mode = 0;
    U.touching = false;
    U.src_w = U.src_h = 0;
    lv_obj_remove_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(U.viewer, LV_OPA_COVER, 0);
    view_geometry();
    load_slots();
    bind_images();
    cur_ready();
    lv_label_set_text_fmt(U.v_title, _("%d de %d"), P.cur + 1, P.count);
    set_chrome(true);
    set_smooth(true);
    apply_view();
    /* On the photo every drag is ours: no global back swipe, and no cut of
     * a long drag at 50 px. The chevron and a swipe down go back. */
    U.self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
}

static void viewer_close(void)
{
    if (!U.viewer) return;
    P.viewing = false;
    lv_obj_add_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(U.anim);
    for (int k = 0; k < 3; k++) vimg_free(&P.v[k]);
    vimg_free(&P.hi);
    U.self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
    /* the photo that was on show, in view in the grid */
    if (U.grid) {
        lv_area_t a, s;
        tile_area(P.cur, &a);
        lv_obj_get_coords(U.scroll, &s);
        if (a.y1 < s.y1 || a.y2 > s.y2) {
            lv_obj_scroll_by(U.scroll, 0, (s.y1 + s.y2) / 2 - (a.y1 + a.y2) / 2, LV_ANIM_OFF);
        }
    }
    schedule();
}

static void close_cb(lv_event_t *e) { (void)e; viewer_close(); }

/* ---- building -------------------------------------------------------------- */

/* To another folder: the root, or album a. */
static void go(int a)
{
    if (a >= P.nalb) return;
    char dir[240];
    if (a < 0) snprintf(dir, sizeof dir, "%s", aos_hal_path_photos());
    else if (snprintf(dir, sizeof dir, "%s/%s", aos_hal_path_photos(), P.alb[a].rel) >= (int)sizeof dir) return;
    if (strcmp(dir, P.dir) == 0) return;
    bool leaving_root = at_root();
    int32_t y = U.scroll ? lv_obj_get_scroll_y(U.scroll) : 0;
    if (leaving_root) P.parent_scroll = y;
    snprintf(P.dir, sizeof P.dir, "%s", dir);
    scan();
    build();
    lv_obj_update_layout(U.scroll);
    if (a < 0 && !U.land) lv_obj_scroll_to_y(U.scroll, P.parent_scroll, LV_ANIM_OFF);
    schedule();
}

static void up(void) { go(-1); }

static void up_cb(lv_event_t *e) { (void)e; up(); }

static void album_cb(lv_event_t *e)
{
    go((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *glyph(lv_obj_t *parent, const char *sym, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, sym);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void count_text(char *n, size_t len, int photos, int albums)
{
    if (photos && albums) {
        snprintf(n, len, albums == 1 ? _("%d fotos \xC2\xB7 1 álbum") : _("%d fotos \xC2\xB7 %d álbumes"),
                 photos, albums);
    } else if (photos) {
        snprintf(n, len, photos == 1 ? _("1 foto") : _("%d fotos"), photos);
    } else {
        n[0] = '\0';
    }
}

/* The album covers: asked for once, kept while the app lives. */
static void ask_album(int a)
{
    if (P.alb[a].r.px) return;
    char path[400], cache[260];
    snprintf(path, sizeof path, "%s/%s/%s", aos_hal_path_photos(), P.alb[a].rel, P.alb[a].first);
    thumbs_dir(cache, sizeof cache);
    img_job_t job = { .path = path, .w = ALBUM_PX, .h = ALBUM_PX, .fill = true, .cache_dir = cache };
    img_request(IMG_OWNER_PHOTOS, TAG_ALBUM | (uintptr_t)a, &job);
}

static void album_image(int a, lv_obj_t *img, int32_t size)
{
    lv_obj_center(img);
    if (P.alb[a].r.px) lv_image_set_src(img, &P.alb[a].dsc);
    if (size != ALBUM_PX) {
        lv_image_set_scale(img, (uint32_t)(size * 256 / ALBUM_PX));
        lv_image_set_antialias(img, true);
    }
}

/* Portrait (and the album pages): the top of the scrolling column. */
static void build_header(int32_t w)
{
    char safe[256];
    bool back = !at_root() && !U.land;
    if (back) {
        lv_obj_t *nav = box(U.scroll);
        lv_obj_set_size(nav, w, 88);
        lv_obj_t *b = box(nav);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 88);
        lv_obj_set_style_pad_right(b, 20, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, up_cb, LV_EVENT_CLICKED, NULL);
        glyph(b, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, C_TINT);
        lv_obj_t *l = aos_label(b, _("Fotos"), aos_font_body, C_TINT);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, -12, 0);
    }
    int ca = current_album();
    const char *title = at_root() ? (U.land && P.nalb ? _("Biblioteca") : _("Fotos")) : album_name(ca >= 0 ? ca : 0);
    if (!at_root() && ca < 0) {
        const char *sl = strrchr(P.dir, '/');   /* a folder opened from Archivos may have none */
        title = sl ? sl + 1 : P.dir;
    }
    lv_obj_t *h = box(U.scroll);
    lv_obj_set_size(h, w, back || U.land ? 96 : 132);
    img_text(safe, sizeof safe, title);
    lv_obj_t *t = aos_label(h, safe, U.land ? aos_font_title : aos_font_large, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, w);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 0, U.land ? -8 : -14);

    char n[96];
    if (at_root() && !U.land) count_text(n, sizeof n, P.root_count + P.alb_total, P.nalb);
    else count_text(n, sizeof n, P.count, 0);
    if (n[0]) {
        lv_obj_t *c = aos_label(U.scroll, n, aos_font_small, AOS_C_DIM);
        lv_obj_set_width(c, w);
        lv_obj_set_style_pad_bottom(c, 18, 0);
    }
}

/* Portrait, at the root: the shelf of albums. */
static void build_albums(int32_t w)
{
    if (!P.nalb || U.land || !at_root()) return;
    lv_obj_t *t = aos_label(U.scroll, _("Álbumes"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, w);
    lv_obj_set_style_pad_bottom(t, 12, 0);

    lv_obj_t *shelf = box(U.scroll);
    lv_obj_add_flag(shelf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(shelf, U.W, ALBUM_PX + 90);
    lv_obj_set_flex_flow(shelf, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(shelf, 20, 0);
    lv_obj_set_style_pad_hor(shelf, AOS_UI_PAD, 0);
    lv_obj_set_scroll_dir(shelf, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(shelf, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_margin_bottom(shelf, 16, 0);
    for (int a = 0; a < P.nalb; a++) {
        lv_obj_t *card = box(shelf);
        lv_obj_set_size(card, ALBUM_PX, ALBUM_PX + 90);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(card, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_add_event_cb(card, album_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
        lv_obj_t *cover = box(card);
        lv_obj_set_size(cover, ALBUM_PX, ALBUM_PX);
        lv_obj_set_style_radius(cover, 18, 0);
        lv_obj_set_style_clip_corner(cover, true, 0);
        lv_obj_set_style_bg_color(cover, C_TILE, 0);
        lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
        lv_obj_t *g = glyph(cover, AOS_SYM_IMAGE_MULTIPLE, &aos_sym_72, lv_color_hex(0x48484A));
        lv_obj_center(g);
        P.alb[a].img = lv_image_create(cover);
        album_image(a, P.alb[a].img, ALBUM_PX);
        ask_album(a);
        char safe[128];
        img_text(safe, sizeof safe, album_name(a));
        lv_obj_t *nm = aos_label(card, safe, aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(nm, ALBUM_PX, lv_font_get_line_height(aos_font_body));
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 2, ALBUM_PX + 10);
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%d", P.alb[a].count);
        lv_obj_t *c = aos_label(card, cnt, aos_font_small, AOS_C_DIM);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 2, ALBUM_PX + 12 + lv_font_get_line_height(aos_font_body));
        aos_make_decorative(cover);
        aos_make_decorative(nm);
        aos_make_decorative(c);
    }
    if (P.count) {
        lv_obj_t *t2 = aos_label(U.scroll, _("Biblioteca"), aos_font_title, AOS_C_TEXT);
        lv_obj_set_width(t2, w);
        lv_obj_set_style_pad_bottom(t2, 12, 0);
    }
}

/* Landscape: the library and the albums in a column on the left, as on the
 * tablet, and the folder chosen in the grid on the right. */
static lv_obj_t *side_row(lv_obj_t *side, int a, int32_t w, bool selected)
{
    lv_obj_t *row = box(side);
    lv_obj_set_size(row, w, 96);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_color(row, selected ? lv_color_hex(0x2C2C2E) : lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, album_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    lv_obj_t *cover = box(row);
    lv_obj_set_size(cover, 72, 72);
    lv_obj_set_style_radius(cover, 12, 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_set_style_bg_color(cover, selected ? lv_color_hex(0x3A3A3C) : C_TILE, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_align(cover, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *g = glyph(cover, AOS_SYM_IMAGE_MULTIPLE, &aos_sym_44, a < 0 ? C_TINT : lv_color_hex(0x636366));
    lv_obj_center(g);
    if (a >= 0) {
        P.alb[a].side_img = lv_image_create(cover);
        album_image(a, P.alb[a].side_img, 72);
        ask_album(a);
    }
    char safe[128], cnt[32];
    img_text(safe, sizeof safe, a < 0 ? _("Biblioteca") : album_name(a));
    lv_obj_t *nm = aos_label(row, safe, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(nm, w - 112, lv_font_get_line_height(aos_font_body));
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 100, -16);
    snprintf(cnt, sizeof cnt, "%d", a < 0 ? P.root_count : P.alb[a].count);
    lv_obj_t *c = aos_label(row, cnt, aos_font_small, AOS_C_DIM);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 100, 18);
    aos_make_decorative(cover);
    aos_make_decorative(nm);
    aos_make_decorative(c);
    return row;
}

static void build_sidebar(void)
{
    lv_obj_t *side = box(U.root);
    lv_obj_add_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(side, SIDEBAR_W, U.H);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(side, 16, 0);
    lv_obj_set_style_pad_bottom(side, 24, 0);
    lv_obj_set_style_pad_row(side, 4, 0);
    lv_obj_set_scroll_dir(side, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(side, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(side, lv_color_hex(0x0E0E10), 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    int32_t w = SIDEBAR_W - 32;

    lv_obj_t *h = box(side);
    lv_obj_set_size(h, w, 100);
    lv_obj_t *t = aos_label(h, _("Fotos"), aos_font_large, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 8, -12);
    char n[96];
    count_text(n, sizeof n, P.root_count + P.alb_total, P.nalb);
    lv_obj_t *c = aos_label(side, n, aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_left(c, 8, 0);
    lv_obj_set_style_pad_bottom(c, 14, 0);

    int cur = current_album();
    lv_obj_t *sel = NULL;
    if (P.root_count || !P.nalb) sel = side_row(side, -1, w, at_root());
    if (P.nalb) {
        lv_obj_t *al = aos_label(side, _("Álbumes"), aos_font_small, AOS_C_DIM);
        lv_obj_set_style_pad_left(al, 8, 0);
        lv_obj_set_style_pad_top(al, 18, 0);
        lv_obj_set_style_pad_bottom(al, 6, 0);
    }
    for (int a = 0; a < P.nalb; a++) {
        lv_obj_t *r = side_row(side, a, w, a == cur);
        if (a == cur) sel = r;
    }
    /* the chosen row in view */
    if (sel && cur >= 0) {
        lv_obj_update_layout(side);
        lv_obj_scroll_to_view(sel, LV_ANIM_OFF);
    }
    lv_obj_t *sep = box(U.root);
    lv_obj_set_size(sep, 1, U.H);
    lv_obj_set_pos(sep, SIDEBAR_W - 1, 0);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
}

static void build_viewer(void)
{
    U.viewer = box(U.root);
    lv_obj_set_size(U.viewer, U.W, U.H);
    lv_obj_set_style_bg_color(U.viewer, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(U.viewer, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);

    /* laid out by hand (position and scale): a picture wider than the view
     * inside a scrollable parent would make the parent scroll */
    for (int k = 0; k < 3; k++) {
        U.img[k] = lv_image_create(U.viewer);
        lv_image_set_pivot(U.img[k], 0, 0);
        lv_obj_add_flag(U.img[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.img[k], LV_OBJ_FLAG_CLICKABLE);
    }
    U.smooth = true;

    U.spinner = lv_spinner_create(U.viewer);
    lv_obj_set_size(U.spinner, 72, 72);
    lv_obj_set_style_arc_width(U.spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(U.spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(U.spinner, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(U.spinner, AOS_C_TEXT, LV_PART_INDICATOR);
    lv_obj_center(U.spinner);
    lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);

    U.err = box(U.viewer);
    lv_obj_set_size(U.err, U.W - 80, 300);
    lv_obj_center(U.err);
    lv_obj_t *eg = glyph(U.err, AOS_SYM_IMAGE_BROKEN_VARIANT, &aos_sym_72, lv_color_hex(0x636366));
    lv_obj_align(eg, LV_ALIGN_TOP_MID, 0, 20);
    U.err_msg = aos_label(U.err, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_width(U.err_msg, U.W - 80);
    lv_label_set_long_mode(U.err_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(U.err_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(U.err_msg, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_add_flag(U.err, LV_OBJ_FLAG_HIDDEN);
    aos_make_decorative(U.err);

    /* over the photo and under the controls: where the fingers land */
    U.touch = box(U.viewer);
    lv_obj_set_size(U.touch, U.W, U.H);
    aos_gesture_attach(U.touch, 0, gesture_cb, NULL);

    /* the chrome: a bar on top with the way back, the place and the name */
    U.top = box(U.viewer);
    lv_obj_set_size(U.top, U.W, 150);
    lv_obj_set_style_bg_color(U.top, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(U.top, LV_OPA_60, 0);
    lv_obj_set_style_bg_grad_color(U.top, AOS_C_BG, 0);
    lv_obj_set_style_bg_grad_opa(U.top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_dir(U.top, LV_GRAD_DIR_VER, 0);
    lv_obj_t *back = box(U.top);
    lv_obj_set_size(back, 96, 96);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bg = glyph(back, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(bg);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 4);
    U.v_title = aos_label_boxed(U.top, "", aos_font_body, AOS_C_TEXT, U.W - 240,
                                lv_font_get_line_height(aos_font_body));
    lv_obj_align(U.v_title, LV_ALIGN_TOP_MID, 0, 14);
    U.v_sub = aos_label_boxed(U.top, "", aos_font_caption, lv_color_hex(0xC7C7CC), U.W - 240, 26);
    lv_label_set_long_mode(U.v_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.v_sub, LV_ALIGN_TOP_MID, 0, 54);

    U.bottom = box(U.viewer);
    lv_obj_set_size(U.bottom, U.W, 110);
    lv_obj_align(U.bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(U.bottom, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(U.bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_color(U.bottom, AOS_C_BG, 0);
    lv_obj_set_style_bg_grad_opa(U.bottom, LV_OPA_60, 0);
    lv_obj_set_style_bg_grad_dir(U.bottom, LV_GRAD_DIR_VER, 0);
    U.v_info = aos_label_boxed(U.bottom, "", aos_font_small, lv_color_hex(0xC7C7CC), U.W - 80, 32);
    lv_obj_align(U.v_info, LV_ALIGN_CENTER, 0, 16);
    aos_make_decorative(U.bottom);

    U.anim = lv_timer_create(anim_cb, FRAME_MS, NULL);
    lv_timer_pause(U.anim);
}

/* The whole screen for the folder on show; the pictures are the statics'. */
static void build(void)
{
    if (U.anim) lv_timer_delete(U.anim);
    U.anim = NULL;
    lv_obj_clean(U.root);
    U.viewer = U.grid = U.scroll = NULL;
    for (int a = 0; a < MAX_ALBUMS; a++) P.alb[a].img = P.alb[a].side_img = NULL;
    U.W = lv_obj_get_width(U.root);
    U.H = lv_obj_get_height(U.root);
    U.land = U.W > U.H;
    U.pressed = -1;
    geometry();
    int32_t x0 = U.land ? SIDEBAR_W : 0;
    int32_t w = U.gw - 2 * (U.land ? U.gx + 8 : AOS_UI_PAD);

    if (U.land) build_sidebar();
    U.scroll = box(U.root);
    lv_obj_add_flag(U.scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(U.scroll, U.gw, U.H);
    lv_obj_set_pos(U.scroll, x0, 0);
    lv_obj_set_flex_flow(U.scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(U.scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(U.scroll, 40, 0);
    lv_obj_set_scroll_dir(U.scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(U.scroll, scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(U.scroll, grid_event_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    build_header(w);
    build_albums(w);
    if (P.count) {
        U.grid = box(U.scroll);
        lv_obj_set_size(U.grid, U.gw, U.rows * (TILE + GAP) - GAP);
        lv_obj_add_flag(U.grid, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(U.grid, grid_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(U.grid, grid_event_cb, LV_EVENT_ALL, NULL);
    } else if (!P.nalb || !at_root()) {
        lv_obj_t *empty = box(U.scroll);
        lv_obj_set_size(empty, w, 460);
        lv_obj_t *g = glyph(empty, AOS_SYM_IMAGE_MULTIPLE, &aos_sym_72, lv_color_hex(0x48484A));
        lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 60);
        lv_obj_t *m = aos_label(empty, at_root() && !P.nalb ? _("No hay fotos en la tarjeta") : _("Carpeta vacía"),
                                aos_font_title, AOS_C_TEXT);
        lv_obj_set_width(m, w);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 170);
        if (at_root()) {
            char msg[240];
            snprintf(msg, sizeof msg, P.nalb ? _("Las fotos están en los álbumes")
                                            : _("Copiá fotos .jpg o .png a la carpeta %s"),
                     aos_hal_path_photos());
            lv_obj_t *d = aos_label(empty, msg, aos_font_body, AOS_C_DIM);
            lv_obj_set_width(d, w - 40);
            lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 240);
        }
    }
    build_viewer();
}

/* ---- the timer ------------------------------------------------------------- */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    int vis0 = 0, vis1 = -1, first = 0, last = -1;
    view_window(&vis0, &vis1, &first, &last);
    img_result_t r;
    int n = 0;
    while (n < 24 && img_take(IMG_OWNER_PHOTOS, &r)) {
        n++;
        switch (TAG_KIND(r.tag)) {
        case TAG_THUMB:
            thumb_in(&r, first, last);
            break;
        case TAG_ALBUM: {
            int a = TAG_IDX(r.tag);
            if (a < P.nalb && r.px && !P.alb[a].r.px) {
                P.alb[a].r = r;
                dsc_of(&P.alb[a].dsc, &P.alb[a].r);
                if (P.alb[a].img) album_image(a, P.alb[a].img, ALBUM_PX);
                if (P.alb[a].side_img) album_image(a, P.alb[a].side_img, 72);
            } else {
                img_free(&r);
            }
            break;
        }
        case TAG_FIT:
        case TAG_HIRES:
            viewer_in(&r);
            break;
        default:
            img_free(&r);
        }
    }
    uint32_t now = lv_tick_get();
    if (now - U.last_sched >= 250) {    /* whatever the queue refused, again */
        U.last_sched = now;
        schedule();
    }
}

/* -------------------------------------------------------------------------- */

static void restore_viewer(void)
{
    if (!P.viewing || P.cur >= P.count) {
        P.viewing = false;
        return;
    }
    /* the pictures were decoded for the other shape of screen: again */
    for (int k = 0; k < 3; k++) vimg_free(&P.v[k]);
    vimg_free(&P.hi);
    viewer_open(P.cur);
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    U.self = self;
    U.root = root;
    U.pressed = -1;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    for (int k = 0; k < CACHE_N; k++) memset(&P.cache[k], 0, sizeof P.cache[k]), P.cache[k].photo = -1;
    for (int k = 0; k < 3; k++) memset(&P.v[k], 0, sizeof P.v[k]), P.v[k].idx = -1;
    memset(&P.hi, 0, sizeof P.hi);
    P.hi.idx = -1;
    for (int a = 0; a < MAX_ALBUMS; a++) memset(&P.alb[a].r, 0, sizeof P.alb[a].r);
    struct stat st;
    if (!P.dir[0] || stat(P.dir, &st) != 0) snprintf(P.dir, sizeof P.dir, "%s", aos_hal_path_photos());
    P.viewing = false;
    scan_albums();
    scan();
    build();
    U.poll = lv_timer_create(poll_cb, 30, NULL);
    lv_obj_update_layout(U.scroll);
    schedule();
    return &P;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (U.poll) lv_timer_delete(U.poll);
    if (U.anim) lv_timer_delete(U.anim);
    U.poll = U.anim = NULL;
    self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
    free_all();                         /* the objects go right after, nothing draws between */
    P.viewing = false;
    P.scanned = false;
    memset(&U, 0, sizeof U);
}

/* The screen turned: the same folder, the same photos in view, the same
 * photo open, laid out again. */
static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self; (void)inst;
    int vis0 = 0, vis1, first, last;
    bool had = view_window(&vis0, &vis1, &first, &last);
    int32_t top = U.scroll ? lv_obj_get_scroll_y(U.scroll) : 0;
    bool viewing = P.viewing;
    U.root = root;
    build();
    lv_obj_update_layout(U.scroll);
    if (had && vis0 > 0 && U.grid) {
        lv_area_t a, s;
        tile_area(vis0, &a);
        lv_obj_get_coords(U.scroll, &s);
        lv_obj_scroll_by(U.scroll, 0, s.y1 - a.y1, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(U.scroll, top, LV_ANIM_OFF);
    }
    P.viewing = viewing;
    restore_viewer();
    schedule();
    return true;
}

/* Archivos opens a photo here (aos_ui_open_app_with): its folder, and the
 * photo on show in the viewer. Any folder of the card will do, not only
 * those under the photos folder. */
static void open_arg(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) >= sizeof P.dir) return;
    char dir[sizeof P.dir];
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = '\0';
    if (P.viewing) viewer_close();
    if (strcmp(dir, P.dir) != 0 || !P.scanned) {
        snprintf(P.dir, sizeof P.dir, "%s", dir);
        scan();
        build();
        lv_obj_update_layout(U.scroll);
    }
    for (int i = 0; i < P.count; i++) {
        if (strcmp(name_at(i), slash + 1) == 0) {
            viewer_open(i);
            break;
        }
    }
}

static void show(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.poll) lv_timer_resume(U.poll);
    const char *arg = aos_ui_take_open_arg("aos.photos");
    if (arg) open_arg(arg);
    schedule();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.poll) lv_timer_pause(U.poll);
    if (U.anim) lv_timer_pause(U.anim);
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (P.viewing) {
        viewer_close();
        return true;
    }
    if (!at_root()) {
        up();
        return true;
    }
    return false;
}

void aos_app_photos_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.photos", .name = "Fotos", .icon = AOS_SYM_IMAGE,
            .color_a = 0xFBBF24, .color_b = 0xEC4899,
            .flags = AOS_APP_FLAG_KEEP,
            .order = 50,
        },
        .create = create, .destroy = destroy, .show = show, .hide = hide,
        .back = back, .resize = resize,
    };
}
