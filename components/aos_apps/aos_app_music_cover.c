/*
 * P4OS - Música's covers.
 *
 * Where a cover comes from, in order (as on the watch): the picture inside
 * the MP3 (ID3 APIC, whose offset the HAL's player found while reading the
 * tags), or a cover.jpg, folder.jpg or front.jpg beside the track, any case.
 * An embedded picture that does not decode falls back to the folder's; with
 * neither, the app draws the note.
 *
 * On the watch this file had its own task and esp_new_jpeg; here the
 * decoding is the HAL's (aos_hal_image_decode) and runs on the shared
 * loader's thread (aos_app_image.c), which also scales it to the square the
 * screen wants - 600 px in portrait, 300 in landscape - so the app draws it
 * without a transform. The small copy for the mini player and the average
 * colour that tints the player are made here from the big one.
 *
 * The cover on show lives here and not in the app: closing and opening the
 * app, turning the screen or the next track of the same album finds it
 * decoded already.
 */
#include "aos_app_music_cover.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct {
    img_result_t   big;
    uint16_t      *thumb;
    lv_image_dsc_t dsc_big, dsc_thumb;
    uint32_t       avg;
} cover_t;

static cover_t  s_cur, s_old;           /* on show, and the one it replaced */
static char     s_cur_key[300];         /* what s_cur is of ("" = nothing)  */
static char     s_want_key[300];        /* what was asked for last          */
static char     s_want_track[256];      /* for the fallback to the folder   */
static bool     s_want_embedded;
static uint32_t s_gen;
static bool     s_changed;

static void cover_free(cover_t *c)
{
    img_free(&c->big);
    free(c->thumb);
    memset(c, 0, sizeof *c);
}

/* The new cover in, the one it replaces out - but never pixels the screen may
 * still be drawing: until the app took the change, the screen shows s_old,
 * and a cover that was never taken can go straight away. */
static void replace(const cover_t *c)
{
    if (s_changed) {
        cover_free(&s_cur);
    } else {
        cover_free(&s_old);
        s_old = s_cur;
    }
    s_cur = *c;
    s_changed = true;
}

static void dsc_set(lv_image_dsc_t *d, const uint16_t *px, int w, int h)
{
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w * 2u;
    d->data_size = (uint32_t)w * (uint32_t)h * 2u;
    d->data = (const uint8_t *)px;
}

static bool is_cover_name(const char *n)
{
    static const char *const names[] = { "cover", "folder", "front" };
    const char *dot = strrchr(n, '.');
    if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png"))) {
        return false;
    }
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t l = strlen(names[i]);
        if ((size_t)(dot - n) == l && strncasecmp(n, names[i], l) == 0) return true;
    }
    return false;
}

static bool cover_in(const char *dir, char *out, size_t len, char *first_sub, size_t sub_len)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    if (first_sub) first_sub[0] = '\0';
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (is_cover_name(e->d_name)) {
            found = snprintf(out, len, "%s/%s", dir, e->d_name) < (int)len;
        } else if (first_sub) {
            bool is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {
                char full[512];
                struct stat st;
                snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
                is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (is_dir && (!first_sub[0] || img_name_cmp(e->d_name, first_sub) < 0)) {
                snprintf(first_sub, sub_len, "%s", e->d_name);
            }
        }
    }
    closedir(d);
    return found;
}

bool music_folder_cover(const char *dir, char *out, size_t len)
{
    char sub[256];
    if (cover_in(dir, out, len, sub, sizeof sub)) return true;
    if (!sub[0]) return false;
    char deeper[512];
    if (snprintf(deeper, sizeof deeper, "%s/%s", dir, sub) >= (int)sizeof deeper) return false;
    return cover_in(deeper, out, len, NULL, 0);
}

static void folder_of(const char *track, char *out, size_t len)
{
    const char *slash = strrchr(track, '/');
    snprintf(out, len, "%.*s", slash ? (int)(slash - track) : 0, track);
}

static void ask(const char *src, uint32_t off, uint32_t size, int px, bool id3)
{
    s_gen = (s_gen + 1) & 0xFFFFFF;
    img_job_t job = { .path = src, .offset = off, .size = size, .w = px, .h = px,
                      .fill = true, .urgent = true, .id3 = id3 };
    img_request(IMG_OWNER_MUSIC, MUSIC_TAG_COVER | s_gen, &job);
}

void music_cover_request(const char *track, uint32_t cover_offset, uint32_t cover_size, int px)
{
    if (!track || !track[0] || px <= 0) return;
    /* The player knows where the embedded picture is only for the track that
     * plays; for the last one of before, the folder's file first and else
     * the tag, read by the loader. */
    char src[256] = "";
    uint32_t off = 0, size = 0;
    bool id3 = false;
    if (cover_offset && cover_size) {
        snprintf(src, sizeof src, "%s", track);
        off = cover_offset;
        size = cover_size;
    } else {
        char dir[256];
        folder_of(track, dir, sizeof dir);
        if (!music_folder_cover(dir, src, sizeof src)) {
            const char *dot = strrchr(track, '.');
            id3 = dot && strcasecmp(dot, ".mp3") == 0;
            snprintf(src, sizeof src, "%s", id3 ? track : "");
        }
    }
    char key[300];
    snprintf(key, sizeof key, "%s@%u%s#%d", src, (unsigned)off, id3 ? "i" : "", px);
    if (!src[0]) key[0] = '\0';
    if (strcmp(key, s_want_key) == 0) return;           /* asked already */
    snprintf(s_want_key, sizeof s_want_key, "%s", key);
    snprintf(s_want_track, sizeof s_want_track, "%s", track);
    s_want_embedded = off != 0;

    if (strcmp(key, s_cur_key) == 0) {                  /* on show already */
        s_gen = (s_gen + 1) & 0xFFFFFF;                 /* whatever is on its way is stale */
        return;
    }
    if (!src[0]) {                                      /* none: the note */
        cover_t none;
        memset(&none, 0, sizeof none);
        s_gen = (s_gen + 1) & 0xFFFFFF;
        replace(&none);
        s_cur_key[0] = '\0';
        return;
    }
    ask(src, off, size, px, id3);
}

bool music_cover_offer(img_result_t *r)
{
    if (!(r->tag & MUSIC_TAG_COVER)) return false;
    if ((r->tag & 0xFFFFFF) != s_gen) {                 /* asked again meanwhile */
        img_free(r);
        return true;
    }
    if (!r->px && s_want_embedded) {
        /* an embedded picture it cannot read: the folder's, if there is one */
        char dir[256], src[256];
        folder_of(s_want_track, dir, sizeof dir);
        s_want_embedded = false;
        const char *hash = strrchr(s_want_key, '#');
        int px = hash ? atoi(hash + 1) : 0;
        if (px > 0 && music_folder_cover(dir, src, sizeof src)) {
            ask(src, 0, 0, px, false);
            return true;
        }
    }
    cover_t c;
    memset(&c, 0, sizeof c);
    if (r->px) {
        c.big = *r;
        dsc_set(&c.dsc_big, r->px, r->w, r->h);
        c.thumb = img_downscale(r->px, r->w, r->h, MUSIC_THUMB_PX, MUSIC_THUMB_PX);
        if (c.thumb) dsc_set(&c.dsc_thumb, c.thumb, MUSIC_THUMB_PX, MUSIC_THUMB_PX);
        c.avg = img_average(r->px, r->w, r->h);
        snprintf(s_cur_key, sizeof s_cur_key, "%s", s_want_key);
    } else {
        /* No picture: remembered for this track (s_want_key stays), so the
         * next refresh does not ask again. Clearing it here re-read the tag
         * and repainted the player ten times a second for every MP3 without
         * a cover - half the screen at 10 fps on the board (2026-09-29). A
         * new track asks afresh. */
        s_cur_key[0] = '\0';
        img_free(r);
        if (!s_cur.big.px && !s_changed) return true;   /* the note is on show already */
    }
    replace(&c);
    memset(r, 0, sizeof *r);
    return true;
}

void music_cover_current(const lv_image_dsc_t **big, const lv_image_dsc_t **thumb, uint32_t *avg)
{
    if (big) *big = s_cur.big.px ? &s_cur.dsc_big : NULL;
    if (thumb) *thumb = s_cur.thumb ? &s_cur.dsc_thumb : NULL;
    if (avg) *avg = s_cur.avg;
}

bool music_cover_take(const lv_image_dsc_t **big, const lv_image_dsc_t **thumb, uint32_t *avg)
{
    if (!s_changed) {
        cover_free(&s_old);             /* the screen moved to s_cur a call ago */
        return false;
    }
    s_changed = false;
    music_cover_current(big, thumb, avg);
    return true;
}

void music_cover_abandon(void)
{
    s_gen = (s_gen + 1) & 0xFFFFFF;
    snprintf(s_want_key, sizeof s_want_key, "%s", s_cur_key);
    cover_free(&s_old);                 /* the screen that drew it is gone */
}
