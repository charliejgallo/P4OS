/*
 * P4OS - the HD icon pack. See aos_icon_pack.h and docs/plan/ICONOS.md.
 */
#include "aos_icon_pack.h"
#include "aos_emoji.h"
#include "aos_hal.h"
#include "lvgl_private.h"           /* lv_image_cache_drop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAGIC       "AICP"
#define VERSION     1
#define HEAD        16
#define ENTRY       48
#define MAX_ENTRIES 2048
#define MAX_SIDE    256                 /* a picture bigger than this is refused */
#define LOOK_MS     2000                /* how often the file's date is looked at */
#define PREF_PACK   "icon_pack"         /* "" the first pack there is, "off" none, or a name */
#define KEEP_BYTES  (1024 * 1024)       /* decoded pictures no icon shows, kept for the next screen */
#define KEEP_MS     1500                /* how long a screen being made again has to take them back */

typedef struct {
    char     id[AOS_ICON_PACK_ID_MAX + 1];
    uint16_t size;
    uint32_t off, len;
} entry_t;

static entry_t *s_idx;                  /* PSRAM, sorted by id then size */
static int      s_count;
static long     s_fsize;                /* the file the index was read from */
static long     s_mtime;
static uint32_t s_looked_ms;
static bool     s_looked;
static char     s_choice[AOS_ICON_PACK_NAME_MAX + 1];  /* the preference */
static bool     s_choice_read;
static char     s_cur[AOS_ICON_PACK_NAME_MAX + 1];     /* the pack in use, "" none */
static FILE    *s_file;                 /* open while a batch of icons is made */

/* The decoded pictures, shared by the icons that show the same one (the home
 * screen's and its folder's, the Store's list made again at every change of
 * a job: measured on the board, 65 pictures read again in 700 ms each time).
 * One that no icon shows any more stays a while, so a screen made again
 * takes it back; then they are let go, oldest first, down to KEEP_BYTES.
 * A picture of a pack that was read again or replaced is never handed out:
 * its generation is old. */
typedef struct pic {
    struct pic    *next;
    char           id[AOS_ICON_PACK_ID_MAX + 1];
    uint16_t       side;
    uint32_t       gen;
    int            refs;
    uint32_t       used_ms;
    size_t         bytes;
    lv_image_dsc_t dsc;                 /* its pixels follow the struct */
} pic_t;

static pic_t      *s_pics;
static uint32_t    s_gen = 1;
static size_t      s_idle_bytes;
static lv_timer_t *s_trim;

/* What a screenful of icons cost, for the log: the pictures made since the
 * first one of a batch, the time spent reading and decoding them and the
 * PSRAM they took, written half a second after the batch began. */
static lv_timer_t *s_batch;
static int         s_batch_n;
static int         s_batch_hits;
static uint64_t    s_batch_us;
static size_t      s_batch_bytes;
/* The pack stays open for the batch: opening a file on the card costs more
 * than reading and decoding a picture (measured on the board, 2026-11-24). */
static void close_file(void)
{
    if (s_file) fclose(s_file);
    s_file = NULL;
}

static void pack_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s.pak", aos_hal_path_icons(), name);
}

static bool name_ok(const char *s, size_t len)
{
    if (!len || len > AOS_ICON_PACK_NAME_MAX) return false;
    for (size_t i = 0; i < len; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
    return true;
}

const char *aos_icon_pack_choice(void)
{
    if (!s_choice_read) {
        s_choice_read = true;
        if (!aos_hal_pref_get_str(PREF_PACK, s_choice, sizeof s_choice)) s_choice[0] = '\0';
    }
    return s_choice;
}

void aos_icon_pack_choose(const char *name)
{
    snprintf(s_choice, sizeof s_choice, "%s", name ? name : "");
    s_choice_read = true;
    aos_hal_pref_set_str(PREF_PACK, s_choice);
    s_looked = false;                   /* the next icon looks again */
}

void aos_icon_pack_reload(void)
{
    s_looked = false;
}

const char *aos_icon_pack_current(void)
{
    aos_icon_pack_refresh();
    return s_cur[0] ? s_cur : NULL;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int aos_icon_pack_list(char (*names)[AOS_ICON_PACK_NAME_MAX + 1], int max)
{
    DIR *d = opendir(aos_hal_path_icons());
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        if (len <= 4 || strcmp(e->d_name + len - 4, ".pak") || !name_ok(e->d_name, len - 4)) continue;
        memcpy(names[n], e->d_name, len - 4);
        names[n][len - 4] = '\0';
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_name);
    return n;
}

/* The pack to use: the one chosen if it is on the card, otherwise the first
 * there is by name (a pack the Store just put there shows with no choosing),
 * none when "off" is chosen. */
static void resolve(char *out)
{
    const char *c = aos_icon_pack_choice();
    out[0] = '\0';
    if (!strcmp(c, "off")) return;
    struct stat st;
    char path[192];
    if (c[0] && name_ok(c, strlen(c))) {
        pack_path(path, sizeof path, c);
        if (stat(path, &st) == 0) {
            strcpy(out, c);
            return;
        }
    }
    /* the pack in use, while it is there (a card busy with a download may
     * fail a listing: the icons must not go back and forth with it) */
    if (s_cur[0]) {
        pack_path(path, sizeof path, s_cur);
        if (stat(path, &st) == 0) {
            strcpy(out, s_cur);
            return;
        }
    }
    char all[16][AOS_ICON_PACK_NAME_MAX + 1];
    int n = aos_icon_pack_list(all, 16);
    if (n) strcpy(out, all[0]);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void pic_free(pic_t *p)
{
    lv_image_cache_drop(&p->dsc);
    lv_free(p);
}

/* Lets go of the pictures no icon shows: all of them, or down to 'keep' bytes,
 * the oldest first, and every one of an old generation. */
static void trim(size_t keep)
{
    for (;;) {
        pic_t **oldest = NULL;
        for (pic_t **pp = &s_pics; *pp; pp = &(*pp)->next) {
            pic_t *p = *pp;
            if (p->refs) continue;
            if (p->gen != s_gen) { oldest = pp; break; }
            if (!oldest || (int32_t)(p->used_ms - (*oldest)->used_ms) < 0) oldest = pp;
        }
        if (!oldest || ((*oldest)->gen == s_gen && s_idle_bytes <= keep)) return;
        pic_t *p = *oldest;
        *oldest = p->next;
        s_idle_bytes -= p->bytes;
        pic_free(p);
    }
}

static void trim_cb(lv_timer_t *t)
{
    (void)t;
    s_trim = NULL;
    trim(KEEP_BYTES);
}

void aos_icon_pack_let_go(void)
{
    trim(0);
}

static void drop_index(void)
{
    close_file();
    s_gen++;
    trim(0);
    lv_free(s_idx);
    s_idx = NULL;
    s_count = 0;
}

/* The index, read whole into PSRAM. A bad file leaves no index (the icons
 * fall back to their drawings) and says why in the log, once per change. */
static void read_index(const char *path, long fsize)
{
    drop_index();
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t head[HEAD];
    if (fread(head, 1, HEAD, f) != HEAD || memcmp(head, MAGIC, 4) || rd16(head + 4) != VERSION) {
        aos_hal_log("icon", "%s: not an icon pack of version %d", path, VERSION);
        fclose(f);
        return;
    }
    int count = rd16(head + 6);
    if (count <= 0 || count > MAX_ENTRIES || HEAD + (long)count * ENTRY > fsize) {
        aos_hal_log("icon", "%s: a bad index (%d entries)", path, count);
        fclose(f);
        return;
    }
    uint8_t *raw = lv_malloc((size_t)count * ENTRY);
    entry_t *idx = lv_malloc((size_t)count * sizeof *idx);
    if (!raw || !idx || fread(raw, ENTRY, (size_t)count, f) != (size_t)count) {
        lv_free(raw);
        lv_free(idx);
        fclose(f);
        aos_hal_log("icon", "%s: could not read its index", path);
        return;
    }
    fclose(f);
    int bad = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *e = raw + (size_t)i * ENTRY;
        memcpy(idx[i].id, e, AOS_ICON_PACK_ID_MAX);
        idx[i].id[AOS_ICON_PACK_ID_MAX] = '\0';
        idx[i].size = rd16(e + 36);
        idx[i].off = rd32(e + 40);
        idx[i].len = rd32(e + 44);
        if (!idx[i].id[0] || !idx[i].size || idx[i].size > MAX_SIDE ||
            (long)idx[i].off + (long)idx[i].len > fsize || idx[i].len < 22) {
            idx[i].id[0] = '\0';        /* never matched */
            bad++;
        }
    }
    lv_free(raw);
    s_idx = idx;
    s_count = count;
    aos_hal_log("icon", "%s: %d pictures%s", path, count - bad, bad ? " (some refused)" : "");
}

bool aos_icon_pack_refresh(void)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (s_looked && now - s_looked_ms < LOOK_MS) return s_idx != NULL;
    s_looked = true;
    s_looked_ms = now;
    char name[AOS_ICON_PACK_NAME_MAX + 1];
    resolve(name);
    struct stat st;
    char path[192];
    if (name[0]) pack_path(path, sizeof path, name);
    if (!name[0] || stat(path, &st) != 0 || st.st_size <= HEAD) {
        if (s_idx) drop_index();
        s_cur[0] = '\0';
        s_fsize = s_mtime = 0;
        return false;
    }
    if (strcmp(name, s_cur) || st.st_size != s_fsize || (long)st.st_mtime != s_mtime || !s_idx) {
        strcpy(s_cur, name);
        s_fsize = st.st_size;
        s_mtime = (long)st.st_mtime;
        read_index(path, s_fsize);
    }
    return s_idx != NULL;
}

/* The entry for 'id' whose size is the smallest at or above 'size', or the
 * largest there is. The index is sorted by id and size, so a binary search
 * finds the id's first entry and the sizes follow it. */
static const entry_t *find(const char *id, int32_t size)
{
    if (!id || !id[0] || !aos_icon_pack_refresh()) return NULL;
    int lo = 0, hi = s_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strncmp(s_idx[mid].id, id, AOS_ICON_PACK_ID_MAX) < 0) lo = mid + 1;
        else hi = mid;
    }
    const entry_t *best = NULL;
    for (int i = lo; i < s_count && !strncmp(s_idx[i].id, id, AOS_ICON_PACK_ID_MAX); i++) {
        best = &s_idx[i];
        if (s_idx[i].size >= size) break;
    }
    return best;
}

bool aos_icon_pack_has(const char *id)
{
    return find(id, 0) != NULL;
}

static void release_cb(lv_event_t *e)
{
    pic_t *p = lv_event_get_user_data(e);
    if (!p || --p->refs > 0) return;
    p->used_ms = (uint32_t)aos_hal_uptime_ms();
    s_idle_bytes += p->bytes;
    if (p->gen != s_gen) trim(KEEP_BYTES);
    else if (!s_trim) {
        s_trim = lv_timer_create(trim_cb, KEEP_MS, NULL);
        lv_timer_set_repeat_count(s_trim, 1);
    } else lv_timer_reset(s_trim);
}

static void batch_cb(lv_timer_t *t)
{
    (void)t;
    close_file();
    if (s_batch_n)
        aos_hal_log("icon", "HD icons: %d pictures (%d decoded, the rest shared or kept) in %u ms, %u KB new", s_batch_n,
                    s_batch_n - s_batch_hits, (unsigned)((s_batch_us + 500) / 1000), (unsigned)(s_batch_bytes / 1024));
    s_batch = NULL;
    s_batch_n = 0;
    s_batch_hits = 0;
    s_batch_us = 0;
    s_batch_bytes = 0;
}

lv_obj_t *aos_icon_pack_create(lv_obj_t *parent, const char *id, int32_t size)
{
    const entry_t *e = find(id, size);
    if (!e) return NULL;
    uint64_t t0 = aos_hal_uptime_us();
    if (!s_batch) {
        s_batch = lv_timer_create(batch_cb, 500, NULL);
        lv_timer_set_repeat_count(s_batch, 1);
    }
    int side = e->size;
    pic_t *pic = NULL;
    for (pic_t *p = s_pics; p; p = p->next)
        if (p->gen == s_gen && p->side == side && !strncmp(p->id, e->id, AOS_ICON_PACK_ID_MAX)) { pic = p; break; }
    if (pic) {
        if (!pic->refs) s_idle_bytes -= pic->bytes;
        s_batch_hits++;
    } else {
        char path[192];
        pack_path(path, sizeof path, s_cur);
        uint8_t *q = lv_malloc(e->len);
        size_t px_bytes = (size_t)side * side * 4;
        pic = lv_malloc(sizeof *pic + px_bytes);
        uint8_t *px = pic ? (uint8_t *)(pic + 1) : NULL;
        bool ok = false;
        if (q && pic && !s_file) s_file = fopen(path, "rb");
        if (q && pic && s_file) {
            ok = fseek(s_file, (long)e->off, SEEK_SET) == 0 && fread(q, 1, e->len, s_file) == e->len &&
                 aos_qoi_decode(q, e->len, px, side, side);
            if (!ok) close_file();
        }
        lv_free(q);
        if (!ok) {
            lv_free(pic);
            aos_hal_log("icon", "%s: its %d px picture could not be read; drawn as before", id, side);
            return NULL;
        }
        /* RGBA from QOI into LVGL's ARGB8888, which in memory is B, G, R, A */
        for (size_t i = 0; i < (size_t)side * side; i++) {
            uint8_t *c = px + i * 4, r = c[0];
            c[0] = c[2];
            c[2] = r;
        }
        memset(pic, 0, sizeof *pic);
        memcpy(pic->id, e->id, sizeof pic->id);
        pic->side = (uint16_t)side;
        pic->gen = s_gen;
        pic->bytes = sizeof *pic + px_bytes;
        pic->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        pic->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        pic->dsc.header.w = side;
        pic->dsc.header.h = side;
        pic->dsc.header.stride = side * 4;
        pic->dsc.data_size = px_bytes;
        pic->dsc.data = px;
        pic->next = s_pics;
        s_pics = pic;
        s_batch_bytes += pic->bytes;
    }
    pic->refs++;

    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &pic->dsc);
    lv_obj_set_size(img, size, size);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    if (side != size) {
        lv_image_set_scale(img, (uint32_t)(256 * size / side));
        lv_image_set_antialias(img, true);
    }
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, release_cb, LV_EVENT_DELETE, pic);

    s_batch_n++;
    s_batch_us += aos_hal_uptime_us() - t0;
    return img;
}
