/*
 * P4OS - Música: the songs on the card.
 *
 * Lists the card's music folder, subfolders included, and plays through the
 * HAL's player. From the watch (_pending/aos_app_music.c) it keeps the whole
 * model: the app is only the remote control, the queue, the next track and
 * the decoding live in the HAL (aos_hal_player_*), so the music goes on with
 * the app closed and a folder plays to its end and round again; opening the
 * app while something plays lands in the folder of what plays; the last
 * track heard is offered to go on from where it was, across restarts.
 *
 * What the 5" screen changes:
 *   - A folder is an album page, as on the phone: its cover, its name split
 *     into album and artist ("Artist - Album"), how many songs, and Play /
 *     Shuffle buttons; subfolders show their cover in the row.
 *   - Portrait: the list with a mini player docked at the bottom, and the
 *     big player (cover of 600 px, tinted with its average colour) one tap
 *     away. Landscape: the list on the left and the player on the right,
 *     both at once.
 *   - Covers are decoded by the HAL on the picture loader's thread
 *     (aos_app_music_cover.c), not by esp_new_jpeg in a task of our own.
 *
 * Everything worth keeping lives in S: turning the screen rebuilds the UI
 * (there is no resize()) and comes back to the same folder, scroll and view.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"
#include "aos_app_image.h"
#include "aos_app_music_cover.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_ENTRIES   256
#define NAME_LEN      256               /* FAT's long names go to 255 */
#define MAX_DEPTH     12
#define TAG_ROW       0x10000000u       /* loader tag of a folder row's art: + index */
#define TAG_HEADER    0x20000000u       /* ...and of the album page's cover         */

#define C_TINT        AOS_C_PINK
#define C_SEP         lv_color_hex(0x2C2C2E)
#define C_MINI        lv_color_hex(0x252528)

typedef struct {
    bool dir;
    char name[NAME_LEN];
} entry_t;

/* ---- state: survives closing the app and turning the screen ---------------- */

static struct {
    bool     init;
    char     cwd[200];                  /* the folder on show */
    bool     player;                    /* portrait: the big player in front */
    int32_t  scroll[MAX_DEPTH];         /* list scroll, per depth */
} S;

/* ---- the screen on show ---------------------------------------------------- */

typedef struct {
    lv_obj_t *art, *note, *img;         /* the art square: the note, or the cover */
} art_t;

typedef struct {
    lv_obj_t  *root;
    int32_t    W, H;
    bool       land;
    entry_t   *entries;                 /* MAX_ENTRIES, only while open */
    int        count, ntracks;
    char       album[NAME_LEN], artist[NAME_LEN];

    lv_obj_t  *list;                    /* the scrolling column */
    lv_obj_t  *lead[MAX_ENTRIES];       /* per track row: the number/glyph, the title */
    lv_obj_t  *ttl[MAX_ENTRIES];
    art_t      row_art[MAX_ENTRIES];    /* per folder row */
    img_result_t row_px[MAX_ENTRIES];
    lv_image_dsc_t row_dsc[MAX_ENTRIES];
    char       nums[MAX_ENTRIES][8];    /* what a track row leads with */
    art_t      head_art;
    img_result_t head_px;
    int        playing_row;             /* entry index highlighted, -1 */
    bool       playing_on;              /* ...and whether it showed as playing */

    lv_obj_t  *mini;                    /* portrait: the docked bar */
    art_t      mini_art;
    lv_obj_t  *mini_title, *mini_sub, *mini_play;

    lv_obj_t  *player;                  /* portrait: over the list; landscape: right */
    art_t      p_art;
    int        cover_px;
    lv_obj_t  *p_cap, *p_from, *p_title, *p_artist, *p_format;
    lv_obj_t  *p_progress, *p_elapsed, *p_remain;
    lv_obj_t  *p_play, *p_shuffle, *p_count, *p_volume;
    lv_timer_t *timer;
    bool       seeking;
    char       shown[256];              /* the path the player shows */
    char       mini_for[256];

    bool       has_last;                /* a track to go on from, after a restart */
    char       last_path[256], last_title[96], last_artist[96];
    uint32_t   last_pos;
} music_ui_t;

/* ~30 KB of row tables: PSRAM, the board's internal RAM is the scarce one */
AOS_BSS_PSRAM static music_ui_t U;

static void build_list(void);
static void refresh(void);
static void layout_titles(void);

/* ---- small things ---------------------------------------------------------- */

static bool is_playable(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0);
}

static int entry_cmp(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;       /* folders first */
    return img_name_cmp(x->name, y->name);
}

static void scan(void)
{
    U.count = U.ntracks = 0;
    if (!U.entries) return;
    DIR *dir = opendir(S.cwd);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && U.count < MAX_ENTRIES) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= NAME_LEN) continue;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            char full[480];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", S.cwd, e->d_name);
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (!is_dir && !is_playable(e->d_name)) continue;
        entry_t *en = &U.entries[U.count++];
        en->dir = is_dir;
        snprintf(en->name, sizeof en->name, "%s", e->d_name);
        if (!is_dir) U.ntracks++;
    }
    closedir(dir);
    qsort(U.entries, (size_t)U.count, sizeof(entry_t), entry_cmp);
}

static void fmt_time(char *out, size_t len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(out, len, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    } else {
        snprintf(out, len, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
}

/* Only when it changed: a label that is set is redrawn, and this runs ten
 * times a second. */
static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void set_text_safe(lv_obj_t *label, const char *text)
{
    char buf[256];
    img_text(buf, sizeof buf, text);
    set_text(label, buf);
}

/* "Artist - Title.mp3" into its halves, and a leading "07 - " or "07 " into
 * a track number. No " - ": all of it is the title. */
static void split_track(const char *file, char *title, size_t tlen, char *artist, size_t alen, int *num)
{
    const char *slash = strrchr(file, '/');
    char name[NAME_LEN];
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : file);
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    char *t = name;
    *num = 0;
    if (t[0] >= '0' && t[0] <= '9') {
        char *p = t;
        int n = 0;
        while (*p >= '0' && *p <= '9' && n < 10000) n = n * 10 + (*p++ - '0');
        if (p - t <= 3 && (*p == ' ' || *p == '.' || *p == '-' || *p == '_')) {
            while (*p == ' ' || *p == '.' || *p == '-' || *p == '_') p++;
            if (*p) {
                *num = n;
                t = p;
            }
        }
    }
    artist[0] = '\0';
    char *sep = strstr(t, " - ");
    if (sep) {
        *sep = '\0';
        snprintf(artist, alen, "%s", t);
        t = sep + 3;
        while (*t == ' ') t++;
    }
    snprintf(title, tlen, "%s", t);
}

/* "Artist - Album" into album and artist. */
static void split_folder(const char *name, char *album, size_t al, char *artist, size_t arl)
{
    const char *sep = strstr(name, " - ");
    if (sep) {
        snprintf(artist, arl, "%.*s", (int)(sep - name), name);
        const char *a = sep + 3;
        while (*a == ' ') a++;
        snprintf(album, al, "%s", a);
    } else {
        artist[0] = '\0';
        snprintf(album, al, "%s", name);
    }
}

static bool at_root(void)
{
    return strcmp(S.cwd, aos_hal_path_music()) == 0;
}

static int depth(void)
{
    int d = 0;
    for (const char *p = S.cwd + strlen(aos_hal_path_music()); *p; p++) d += *p == '/';
    return d < MAX_DEPTH ? d : MAX_DEPTH - 1;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* The first track of a folder, in list order: its embedded picture stands
 * in for a folder without a cover file. */
static bool first_track_in(const char *dir, char *out, size_t len)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    char best[NAME_LEN] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !is_playable(e->d_name) || strlen(e->d_name) >= NAME_LEN) continue;
        const char *dot = strrchr(e->d_name, '.');
        if (strcasecmp(dot, ".mp3") != 0) continue;
        if (!best[0] || img_name_cmp(e->d_name, best) < 0) snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    return best[0] && snprintf(out, len, "%s/%s", dir, best) < (int)len;
}

/* Asks the loader for a folder's art: its cover file, or else the picture
 * inside its first MP3. */
static void request_folder_art(const char *dir, int px, uintptr_t tag)
{
    char src[480];
    img_job_t job = { .path = src, .w = px, .h = px, .fill = true };
    if (music_folder_cover(dir, src, sizeof src)) {
        img_request(IMG_OWNER_MUSIC, tag, &job);
    } else if (first_track_in(dir, src, sizeof src)) {
        job.id3 = true;
        img_request(IMG_OWNER_MUSIC, tag, &job);
    }
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

/* A round-cornered square for a cover: a gradient with the note until the
 * picture is in. */
static art_t art_create(lv_obj_t *parent, int32_t size, int32_t radius)
{
    art_t a;
    a.art = box(parent);
    lv_obj_set_size(a.art, size, size);
    lv_obj_set_style_radius(a.art, radius, 0);
    lv_obj_set_style_clip_corner(a.art, true, 0);
    lv_obj_set_style_bg_color(a.art, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_grad_color(a.art, lv_color_hex(0x242426), 0);
    lv_obj_set_style_bg_grad_dir(a.art, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(a.art, LV_OPA_COVER, 0);
    const lv_font_t *f = size >= 200 ? &aos_sym_72 : (size >= 80 ? &aos_sym_44 : &aos_sym_28);
    a.note = glyph(a.art, AOS_SYM_MUSIC_NOTE, f, lv_color_hex(0x8E8E93));
    if (size >= 400) lv_obj_set_style_transform_scale(a.note, 512, 0);
    lv_obj_set_style_transform_pivot_x(a.note, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(a.note, lv_pct(50), 0);
    lv_obj_center(a.note);
    a.img = lv_image_create(a.art);
    lv_obj_add_flag(a.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(a.img);
    aos_make_decorative(a.art);
    return a;
}

/* The picture into the square; NULL puts the note back. A picture that is
 * not the square's size is scaled to it (the mini player's copy is exact;
 * a cover decoded for the other orientation until the new one arrives). */
static void art_set(art_t *a, const lv_image_dsc_t *dsc)
{
    if (!a->art) return;
    if (dsc) {
        lv_image_set_src(a->img, dsc);
        int32_t size = lv_obj_get_style_width(a->art, 0);
        uint32_t scale = dsc->header.w ? (uint32_t)(size * 256 / (int32_t)dsc->header.w) : 256;
        lv_image_set_scale(a->img, scale);
        lv_image_set_antialias(a->img, scale != 256);
        lv_obj_center(a->img);
        lv_obj_remove_flag(a->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a->note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(a->note, LV_OBJ_FLAG_HIDDEN);
    }
}

static void dsc_from(lv_image_dsc_t *d, const img_result_t *r)
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

/* The dark end of the player's background: the cover's colour, dimmed. */
static lv_color_t tint_of(uint32_t avg)
{
    if (!avg) return lv_color_hex(0x2A1A22);
    lv_color_t c = lv_color_hex(avg);
    return lv_color_darken(c, LV_OPA_60);
}

/* ---- playing ------------------------------------------------------------- */

static bool play_file(const char *path)
{
    if (!aos_hal_player_play_folder(path)) {
        aos_ui_toast(_("No se pudo reproducir"), 1600);
        return false;
    }
    U.has_last = false;
    refresh();
    return true;
}

static void play_entry(int i)
{
    char path[480];
    snprintf(path, sizeof path, "%s/%s", S.cwd, U.entries[i].name);
    play_file(path);
}

/* The play button: pause, go on, again, or the last track of before. */
static void toggle_play(void)
{
    aos_player_info_t in;
    aos_hal_player_info(&in);
    bool mine = !in.live;
    if (mine && in.state == AOS_PLAYER_PLAYING) {
        aos_hal_player_pause();
    } else if (mine && in.state == AOS_PLAYER_PAUSED) {
        aos_hal_player_resume();
    } else if (U.has_last) {
        U.has_last = false;
        if (!aos_hal_player_resume_last()) aos_ui_toast(_("No se pudo reproducir"), 1600);
    } else if (U.shown[0]) {
        play_file(U.shown);             /* a single file that ended: again */
    }
    refresh();
}

static void play_cb(lv_event_t *e) { (void)e; toggle_play(); }

static void skip_cb(lv_event_t *e)
{
    if ((intptr_t)lv_event_get_user_data(e) > 0) aos_hal_player_next();
    else aos_hal_player_prev();
    refresh();
}

static void shuffle_cb(lv_event_t *e)
{
    (void)e;
    aos_player_info_t in;
    aos_hal_player_info(&in);
    aos_hal_player_set_shuffle(!in.shuffle);
    aos_ui_toast(in.shuffle ? _("Aleatorio apagado") : _("Aleatorio"), 1000);
    refresh();
}

static void seek_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        U.seeking = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        aos_player_info_t in;
        aos_hal_player_info(&in);
        if (in.duration_ms && !in.live) {
            aos_hal_player_seek((uint32_t)((uint64_t)lv_slider_get_value(U.p_progress) *
                                           in.duration_ms / 1000));
        }
        U.seeking = false;
    }
}

static void volume_cb(lv_event_t *e)
{
    aos_hal_volume_set((int)lv_slider_get_value(lv_event_get_target(e)));
}

static void first_track_cb(lv_event_t *e)
{
    bool shuffle = (intptr_t)lv_event_get_user_data(e) != 0;
    if (!U.ntracks) return;
    int first = U.count - U.ntracks;
    int pick = shuffle ? first + (int)(aos_hal_uptime_ms() % (uint64_t)U.ntracks) : first;
    aos_hal_player_set_shuffle(shuffle);
    play_entry(pick);
}

static void show_player(bool on)
{
    if (U.land) return;                 /* both are always there */
    S.player = on;
    if (on) {
        lv_obj_remove_flag(U.player, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(U.player);
    } else {
        lv_obj_add_flag(U.player, LV_OBJ_FLAG_HIDDEN);
    }
}

static void open_player_cb(lv_event_t *e) { (void)e; show_player(true); }
static void close_player_cb(lv_event_t *e) { (void)e; show_player(false); }

/* ---- refresh (10 Hz) --------------------------------------------------------- */

static void mark_playing_row(const char *path, bool playing)
{
    int row = -1;
    if (path && path[0] && U.entries) {
        const char *slash = strrchr(path, '/');
        if (slash && (size_t)(slash - path) == strlen(S.cwd) && strncmp(path, S.cwd, strlen(S.cwd)) == 0) {
            for (int i = 0; i < U.count; i++) {
                if (!U.entries[i].dir && strcmp(U.entries[i].name, slash + 1) == 0) {
                    row = i;
                    break;
                }
            }
        }
    }
    /* Only when something changed: this runs every 100 ms, and setting the
     * same font again re-lays the whole list out under the player - on the
     * board, 77 % of a core in LVGL's draw threads for a progress bar
     * (2026-09-29). */
    if (row == U.playing_row && (row < 0 || playing == U.playing_on)) return;
    U.playing_on = playing;
    for (int k = 0; k < 2; k++) {
        int i = k == 0 ? U.playing_row : row;
        if (i < 0 || !U.lead[i]) continue;
        bool on = i == row;
        lv_obj_set_style_text_color(U.ttl[i], on ? C_TINT : AOS_C_TEXT, 0);
        const char *txt = lv_obj_get_user_data(U.lead[i]);
        if (on) {
            lv_obj_set_style_text_font(U.lead[i], &aos_sym_28, 0);
            lv_obj_set_style_text_color(U.lead[i], C_TINT, 0);
            set_text(U.lead[i], playing ? AOS_SYM_VOLUME_HIGH : AOS_SYM_PAUSE);
        } else {
            bool num = txt && txt[0] >= '0' && txt[0] <= '9';
            lv_obj_set_style_text_font(U.lead[i], num ? aos_font_body : &aos_sym_28, 0);
            lv_obj_set_style_text_color(U.lead[i], num ? AOS_C_DIM : lv_color_hex(0x636366), 0);
            set_text(U.lead[i], txt ? txt : "");
        }
    }
    U.playing_row = row;
}

static void cover_poll(void)
{
    img_result_t r;
    while (img_take(IMG_OWNER_MUSIC, &r)) {
        if (music_cover_offer(&r)) continue;
        if (r.tag & TAG_HEADER) {
            if (U.head_art.art && r.px && !U.head_px.px) {
                U.head_px = r;
                static lv_image_dsc_t d;
                dsc_from(&d, &U.head_px);
                art_set(&U.head_art, &d);
            } else {
                img_free(&r);
            }
            continue;
        }
        int i = (int)(r.tag & 0xFFFF);
        if ((r.tag & TAG_ROW) && i < U.count && U.row_art[i].art && r.px && !U.row_px[i].px) {
            U.row_px[i] = r;
            dsc_from(&U.row_dsc[i], &U.row_px[i]);
            art_set(&U.row_art[i], &U.row_dsc[i]);
        } else {
            img_free(&r);
        }
    }
    const lv_image_dsc_t *big, *thumb;
    uint32_t avg;
    if (music_cover_take(&big, &thumb, &avg)) {
        art_set(&U.p_art, big);
        art_set(&U.mini_art, thumb);
        if (U.player) {
            lv_obj_set_style_bg_color(U.player, tint_of(avg), 0);
        }
    }
}

static void refresh(void)
{
    aos_player_info_t in;
    if (!aos_hal_player_info(&in)) return;
    /* A station is the Radio app's: here it is as if nothing played, and
     * playing a track takes the speaker back from it. */
    bool active = in.state != AOS_PLAYER_STOPPED && !in.live && in.path[0];
    bool playing = active && in.state == AOS_PLAYER_PLAYING;
    const char *path = active ? in.path : (U.has_last ? U.last_path : "");
    char title[256], artist[256];
    int num;
    if (active) {
        snprintf(title, sizeof title, "%s", in.title[0] ? in.title : base_name(in.path));
        snprintf(artist, sizeof artist, "%s", in.artist);
    } else if (U.has_last) {
        snprintf(title, sizeof title, "%s", U.last_title);
        snprintf(artist, sizeof artist, "%s", U.last_artist);
    } else {
        split_track(U.shown, title, sizeof title, artist, sizeof artist, &num);
    }

    mark_playing_row(active ? in.path : "", playing);

    /* the cover of whatever the player and the mini bar show */
    if (path[0]) {
        music_cover_request(path, active ? in.cover_offset : 0, active ? in.cover_size : 0,
                            U.cover_px);
    }
    cover_poll();

    /* the mini player (portrait) */
    if (U.mini) {
        if (path[0]) {
            set_text_safe(U.mini_title, title);
            if (active || !U.has_last) {
                set_text_safe(U.mini_sub, artist[0] ? artist : _("Música"));
            } else {
                char when[16], sub[160];
                fmt_time(when, sizeof when, U.last_pos);
                snprintf(sub, sizeof sub, _("Continuar desde %s"), when);
                set_text(U.mini_sub, sub);
            }
            set_text(U.mini_play, playing ? AOS_SYM_PAUSE : AOS_SYM_PLAY);
            lv_obj_remove_flag(U.mini, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(U.mini, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!U.player) return;
    if (!path[0] && !U.shown[0]) {
        if (strcmp(lv_label_get_text(U.p_title), _("Nada sonando")) != 0) {
            lv_label_set_text(U.p_title, _("Nada sonando"));
            layout_titles();
        }
        set_text(U.p_artist, _("Elegí una canción de la lista"));
        set_text(U.p_format, "");
        set_text(U.p_from, "");
        lv_obj_add_flag(U.p_cap, LV_OBJ_FLAG_HIDDEN);
        set_text(U.p_count, "");
        set_text(U.p_play, AOS_SYM_PLAY);
        set_text(U.p_elapsed, "0:00");
        set_text(U.p_remain, "-0:00");
        lv_slider_set_value(U.p_progress, 0, LV_ANIM_OFF);
        return;
    }
    if (active) snprintf(U.shown, sizeof U.shown, "%s", in.path);
    char safe_title[256];
    img_text(safe_title, sizeof safe_title, title);
    if (strcmp(lv_label_get_text(U.p_title), safe_title) != 0) {
        lv_label_set_text(U.p_title, safe_title);
        layout_titles();
    }
    set_text_safe(U.p_artist, artist[0] ? artist : (in.album[0] ? in.album : ""));

    char buf[96];
    if (active && in.format && in.format[0]) {
        char khz[16];
        unsigned r = (unsigned)in.sample_rate;
        if (r % 1000) snprintf(khz, sizeof khz, "%u,%u kHz", r / 1000, (r % 1000) / 100);
        else snprintf(khz, sizeof khz, "%u kHz", r / 1000);
        if (in.kbps) {
            snprintf(buf, sizeof buf, "%s \xC2\xB7 %u kbps%s \xC2\xB7 %s", in.format, in.kbps,
                     in.vbr ? " VBR" : "", khz);
        } else {
            snprintf(buf, sizeof buf, "%s \xC2\xB7 %s", in.format, khz);
        }
        if (in.yielded) snprintf(buf, sizeof buf, "%s", _("En pausa: otra app usa el parlante"));
    } else if (!active && U.has_last) {
        char when[16];
        fmt_time(when, sizeof when, U.last_pos);
        snprintf(buf, sizeof buf, _("Continuar desde %s"), when);
    } else {
        buf[0] = '\0';
    }
    set_text(U.p_format, buf);

    /* where it plays from: the folder's name */
    char folder[NAME_LEN] = "";
    const char *slash = strrchr(path[0] ? path : U.shown, '/');
    if (slash) {
        const char *src = path[0] ? path : U.shown;
        char dir[NAME_LEN];
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - src), src);
        char a2[NAME_LEN];
        if (strcmp(dir, aos_hal_path_music()) == 0) snprintf(folder, sizeof folder, "%s", _("Música"));
        else split_folder(base_name(dir), folder, sizeof folder, a2, sizeof a2);
    }
    set_text_safe(U.p_from, folder);
    lv_obj_remove_flag(U.p_cap, LV_OBJ_FLAG_HIDDEN);

    if (active && in.count > 0) snprintf(buf, sizeof buf, _("%d de %d"), in.index + 1, in.count);
    else buf[0] = '\0';
    set_text(U.p_count, buf);
    bool shuf = active && in.shuffle;
    lv_obj_set_style_text_color(U.p_shuffle, shuf ? C_TINT : lv_color_hex(0xAEAEB2), 0);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(U.p_shuffle), shuf ? LV_OPA_20 : LV_OPA_TRANSP, 0);

    uint32_t pos = active ? in.position_ms : (U.has_last ? U.last_pos : 0);
    uint32_t dur = active ? in.duration_ms : 0;
    if (U.seeking && dur) {
        pos = (uint32_t)((uint64_t)lv_slider_get_value(U.p_progress) * dur / 1000);
    } else {
        int32_t v = dur ? (int32_t)((uint64_t)pos * 1000 / dur) : 0;
        if (lv_slider_get_value(U.p_progress) != v) lv_slider_set_value(U.p_progress, v, LV_ANIM_OFF);
    }
    char t1[16], t2[20];
    fmt_time(t1, sizeof t1, pos);
    t2[0] = '-';
    fmt_time(t2 + 1, sizeof t2 - 1, dur > pos ? dur - pos : 0);
    set_text(U.p_elapsed, t1);
    set_text(U.p_remain, dur ? t2 : "--:--");
    set_text(U.p_play, playing ? AOS_SYM_PAUSE : AOS_SYM_PLAY);
}

/* Portrait: one or two lines of title, and the artist and the format right
 * under whichever it is (a fixed two-line box left a hole under a short one). */
static void layout_titles(void)
{
    if (!U.p_title) return;
    const lv_font_t *tf = aos_font_title;
    int32_t line = lv_font_get_line_height(tf), tw = lv_obj_get_width(U.p_title);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(U.p_title), tf, 0, 0, tw, LV_TEXT_FLAG_NONE);
    int max = U.land ? 3 : 2;
    int lines = (int)((size.y + line / 2) / line);
    if (lines < 1) lines = 1;
    if (lines > max) lines = max;
    lv_obj_set_height(U.p_title, lines * line);
    lv_obj_align_to(U.p_artist, U.p_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
    lv_obj_align_to(U.p_format, U.p_artist, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

/* ---- the list -------------------------------------------------------------- */

static void go_to(const char *dir)
{
    S.scroll[depth()] = lv_obj_get_scroll_y(U.list);
    snprintf(S.cwd, sizeof S.cwd, "%s", dir);
    S.scroll[depth()] = 0;
    build_list();
}

static void up_cb(lv_event_t *e)
{
    (void)e;
    char *slash = strrchr(S.cwd, '/');
    if (!slash || at_root()) return;
    char dir[200];
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - S.cwd), S.cwd);
    S.scroll[depth()] = 0;
    snprintf(S.cwd, sizeof S.cwd, "%s", dir);
    build_list();
}

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= U.count) return;
    entry_t *en = &U.entries[i];
    if (en->dir) {
        char dir[200];
        if (snprintf(dir, sizeof dir, "%s/%s", S.cwd, en->name) < (int)sizeof dir) go_to(dir);
    } else {
        play_entry(i);
    }
}

static lv_obj_t *hairline(lv_obj_t *row, int32_t x)
{
    lv_obj_t *l = box(row);
    lv_obj_set_size(l, lv_pct(100), 1);
    lv_obj_set_style_bg_color(l, C_SEP, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(l, 0, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, x, 0);
    lv_obj_set_width(l, lv_obj_get_style_width(row, 0) - x);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *row_base(int32_t w, int32_t h, int i)
{
    lv_obj_t *row = box(U.list);
    lv_obj_set_size(row, w, h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    return row;
}

static void two_lines(lv_obj_t *row, int32_t x, int32_t w, const char *text, const char *sub,
                      lv_obj_t **title_out)
{
    char safe[NAME_LEN];
    img_text(safe, sizeof safe, text);
    lv_obj_t *t = aos_label(row, safe, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(t, w, lv_font_get_line_height(aos_font_body));
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    if (sub && sub[0]) {
        img_text(safe, sizeof safe, sub);
        lv_obj_t *s = aos_label(row, safe, aos_font_small, AOS_C_DIM);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(s, w, lv_font_get_line_height(aos_font_small));
        lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, x, -17);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, x, 19);
    } else {
        lv_obj_align(t, LV_ALIGN_LEFT_MID, x, 0);
    }
    if (title_out) *title_out = t;
}

static void folder_row(int i, int32_t w)
{
    const entry_t *en = &U.entries[i];
    lv_obj_t *row = row_base(w, 112, i);
    U.row_art[i] = art_create(row, 88, 12);
    lv_obj_align(U.row_art[i].art, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(U.row_art[i].art, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_grad_color(U.row_art[i].art, lv_color_hex(0x1C1C1E), 0);
    set_text(U.row_art[i].note, AOS_SYM_FOLDER_MUSIC);
    lv_obj_set_style_text_color(U.row_art[i].note, C_TINT, 0);

    char album[NAME_LEN], artist[NAME_LEN];
    split_folder(en->name, album, sizeof album, artist, sizeof artist);
    two_lines(row, 112, w - 112 - 56, album, artist[0] ? artist : _("Carpeta"), NULL);
    lv_obj_t *chev = glyph(row, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, lv_color_hex(0x636366));
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);
    hairline(row, 112);

    /* its cover, when it has one, off LVGL's task */
    char dir[480];
    snprintf(dir, sizeof dir, "%s/%s", S.cwd, en->name);
    request_folder_art(dir, 88, TAG_ROW | (uintptr_t)i);
}

static void track_row(int i, int32_t w)
{
    const entry_t *en = &U.entries[i];
    lv_obj_t *row = row_base(w, AOS_UI_ROW_H, i);
    char title[NAME_LEN], artist[NAME_LEN];
    int num;
    split_track(en->name, title, sizeof title, artist, sizeof artist, &num);
    /* in an album the artist is on the page already: only another one shows */
    const char *sub = artist[0] ? artist : NULL;
    if (sub && U.artist[0] && strcmp(artist, U.artist) == 0) sub = NULL;

    char *lead_txt = U.nums[i];
    if (num > 0) snprintf(lead_txt, sizeof U.nums[i], "%u", (unsigned)num % 10000u);
    else snprintf(lead_txt, sizeof U.nums[i], "%s", AOS_SYM_MUSIC_NOTE);
    lv_obj_t *lead = lv_label_create(row);
    lv_obj_set_width(lead, 64);
    lv_obj_set_style_text_align(lead, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_user_data(lead, lead_txt);
    lv_label_set_text(lead, lead_txt);
    lv_obj_set_style_text_font(lead, num > 0 ? aos_font_body : &aos_sym_28, 0);
    lv_obj_set_style_text_color(lead, num > 0 ? AOS_C_DIM : lv_color_hex(0x636366), 0);
    lv_obj_align(lead, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(lead, LV_OBJ_FLAG_CLICKABLE);
    U.lead[i] = lead;
    two_lines(row, 80, w - 80 - 16, title, sub, &U.ttl[i]);
    hairline(row, 80);
}

static void header_button(lv_obj_t *parent, const char *sym, const char *text, int32_t w,
                          intptr_t shuffle)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, 88);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, first_track_cb, LV_EVENT_CLICKED, (void *)shuffle);
    lv_obj_t *in = box(b);
    lv_obj_set_size(in, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(in, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(in, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(in, 12, 0);
    glyph(in, sym, &aos_sym_28, C_TINT);
    lv_obj_t *l = aos_label(in, text, aos_font_body, C_TINT);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(in);
    aos_make_decorative(in);
}

/* The top of the list: the big title at the root, the album page below. */
static void build_header(int32_t w)
{
    char safe[NAME_LEN];
    if (!at_root()) {
        /* back to the parent, named */
        const char *slash = strrchr(S.cwd, '/');
        char parent[200];
        snprintf(parent, sizeof parent, "%.*s", slash ? (int)(slash - S.cwd) : 0, S.cwd);
        char pname[NAME_LEN], a2[NAME_LEN];
        if (strcmp(parent, aos_hal_path_music()) == 0) snprintf(pname, sizeof pname, "%s", _("Música"));
        else split_folder(base_name(parent), pname, sizeof pname, a2, sizeof a2);
        lv_obj_t *nav = box(U.list);
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
        img_text(safe, sizeof safe, pname);
        lv_obj_t *l = aos_label(b, safe, aos_font_body, C_TINT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(l, w - 80, 0);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, -12, 0);
    }

    if (at_root() || U.ntracks == 0) {
        /* the large title, as on the phone */
        lv_obj_t *h = box(U.list);
        lv_obj_set_size(h, w, at_root() ? 132 : 96);
        img_text(safe, sizeof safe, at_root() ? _("Música") : U.album);
        lv_obj_t *t = aos_label(h, safe, aos_font_large, AOS_C_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, w);
        lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 0, -14);
        int folders = U.count - U.ntracks;
        if (U.count) {
            char n[80];
            if (folders && U.ntracks) snprintf(n, sizeof n, _("%d carpetas \xC2\xB7 %d canciones"), folders, U.ntracks);
            else if (folders) snprintf(n, sizeof n, folders == 1 ? _("1 carpeta") : _("%d carpetas"), folders);
            else snprintf(n, sizeof n, U.ntracks == 1 ? _("1 canción") : _("%d canciones"), U.ntracks);
            lv_obj_t *c = aos_label(U.list, n, aos_font_small, AOS_C_DIM);
            lv_obj_set_width(c, w);
            lv_obj_set_style_pad_bottom(c, 12, 0);
        }
        return;
    }

    /* An album: its cover, its name and artist, and Play / Shuffle.
     * Portrait: all of it centred, the cover big. Landscape (the list is
     * the left part): the cover beside the names, the buttons under both. */
    lv_obj_t *h = box(U.list);
    int32_t side = U.land ? 200 : LV_MIN(w - 200, 400);
    int32_t name_w = U.land ? w - side - 28 : w;
    lv_obj_set_size(h, w, U.land ? side + 8 : LV_SIZE_CONTENT);
    if (!U.land) {
        lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(h, 6, 0);
        lv_obj_set_style_pad_bottom(h, 8, 0);
    }
    U.head_art = art_create(h, side, 20);
    lv_obj_set_style_bg_color(U.head_art.art, lv_color_hex(0x5A2233), 0);
    lv_obj_set_style_bg_grad_color(U.head_art.art, lv_color_hex(0x2A1A22), 0);
    lv_obj_set_style_text_color(U.head_art.note, lv_color_hex(0xFF8FA6), 0);
    lv_obj_set_style_shadow_width(U.head_art.art, 40, 0);
    lv_obj_set_style_shadow_color(U.head_art.art, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(U.head_art.art, LV_OPA_60, 0);
    lv_obj_set_style_shadow_offset_y(U.head_art.art, 10, 0);
    if (!U.land) lv_obj_set_style_margin_bottom(U.head_art.art, 22, 0);
    request_folder_art(S.cwd, side, TAG_HEADER);

    lv_obj_t *col = h;
    if (U.land) {
        col = box(h);
        lv_obj_set_size(col, name_w, side);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(col, 6, 0);
        lv_obj_align(col, LV_ALIGN_TOP_LEFT, side + 28, 0);
    }
    lv_text_align_t al = U.land ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER;
    img_text(safe, sizeof safe, U.album);
    lv_obj_t *t = aos_label(col, safe, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, name_w);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_height(t, 3 * lv_font_get_line_height(aos_font_title), 0);
    lv_obj_set_style_text_align(t, al, 0);
    if (U.artist[0]) {
        img_text(safe, sizeof safe, U.artist);
        lv_obj_t *a = aos_label(col, safe, aos_font_body, C_TINT);
        lv_obj_set_size(a, name_w, lv_font_get_line_height(aos_font_body));
        lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(a, al, 0);
    }
    char n[64];
    snprintf(n, sizeof n, U.ntracks == 1 ? _("1 canción") : _("%d canciones"), U.ntracks);
    lv_obj_t *c = aos_label(col, n, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(c, name_w);
    lv_obj_set_style_text_align(c, al, 0);
    aos_make_decorative(h);

    lv_obj_t *btns = box(U.list);
    lv_obj_set_size(btns, w, 88);
    lv_obj_set_style_margin_top(btns, U.land ? 20 : 16, 0);
    lv_obj_set_style_margin_bottom(btns, 16, 0);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 20, 0);
    header_button(btns, AOS_SYM_PLAY, _("Reproducir"), (w - 20) / 2, 0);
    header_button(btns, AOS_SYM_SHUFFLE_VARIANT, _("Aleatorio"), (w - 20) / 2, 1);
}

static void free_rows(void)
{
    for (int i = 0; i < MAX_ENTRIES; i++) img_free(&U.row_px[i]);
    img_free(&U.head_px);
    memset(U.row_art, 0, sizeof U.row_art);
    memset(U.lead, 0, sizeof U.lead);
    memset(U.ttl, 0, sizeof U.ttl);
    memset(&U.head_art, 0, sizeof U.head_art);
    U.playing_row = -1;
}

static void build_list(void)
{
    /* the rows' jobs are for rows that are about to go */
    for (int i = 0; i < MAX_ENTRIES; i++) img_cancel(IMG_OWNER_MUSIC, TAG_ROW | (uintptr_t)i);
    img_cancel(IMG_OWNER_MUSIC, TAG_HEADER);
    lv_obj_clean(U.list);
    free_rows();                        /* after the objects that drew them */
    scan();
    if (at_root()) {
        U.album[0] = U.artist[0] = '\0';
    } else {
        split_folder(base_name(S.cwd), U.album, sizeof U.album, U.artist, sizeof U.artist);
    }

    int32_t w = lv_obj_get_content_width(U.list);
    build_header(w);
    for (int i = 0; i < U.count; i++) {
        if (U.entries[i].dir) folder_row(i, w);
        else track_row(i, w);
    }

    if (U.count == 0) {
        lv_obj_t *empty = box(U.list);
        lv_obj_set_size(empty, w, 420);
        lv_obj_t *g = glyph(empty, AOS_SYM_MUSIC_NOTE, &aos_sym_72, lv_color_hex(0x48484A));
        lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 60);
        lv_obj_t *m = aos_label(empty, at_root() ? _("No hay música en la tarjeta") : _("Carpeta vacía"),
                                aos_font_title, AOS_C_TEXT);
        lv_obj_set_width(m, w);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 170);
        if (at_root()) {
            char msg[240];
            snprintf(msg, sizeof msg, _("Copiá archivos .mp3 o .wav a la carpeta %s"), aos_hal_path_music());
            lv_obj_t *d = aos_label(empty, msg, aos_font_body, AOS_C_DIM);
            lv_obj_set_width(d, w - 40);
            lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 240);
        }
    }
    lv_obj_update_layout(U.list);
    lv_obj_scroll_to_y(U.list, S.scroll[depth()], LV_ANIM_OFF);
    refresh();
}

/* ---- the player ------------------------------------------------------------ */

static lv_obj_t *icon_button(lv_obj_t *parent, const char *sym, const lv_font_t *font, int32_t w,
                             int32_t h, lv_event_cb_t cb, void *ud, lv_obj_t **label)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = glyph(b, sym, font, AOS_C_TEXT);
    lv_obj_center(l);
    if (label) *label = l;
    return b;
}

static lv_obj_t *slider(lv_obj_t *parent, int32_t w, int32_t h, int32_t knob)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, w, h);
    lv_obj_set_style_bg_color(s, AOS_C_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, AOS_C_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, AOS_C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, knob, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 26);   /* a thin bar is a hard target */
    return s;
}

/* The player in a w x h panel. Portrait: a column, cover on top. Landscape
 * (the right half): the cover beside the titles, the controls under both. */
static void build_player(lv_obj_t *p, int32_t w, int32_t h, bool portrait)
{
    const lv_font_t *tf = aos_font_title;
    int32_t y;

    if (portrait) {
        /* top bar: close, where it plays from, shuffle */
        lv_obj_t *close = icon_button(p, AOS_SYM_CHEVRON_DOWN, &aos_sym_44, 96, 96, close_player_cb, NULL, NULL);
        lv_obj_align(close, LV_ALIGN_TOP_LEFT, 12, 4);
        lv_obj_t *cap = aos_label(p, _("REPRODUCIENDO DESDE"), aos_font_tiny, lv_color_hex(0xC7C7CC));
        lv_obj_set_style_text_letter_space(cap, 1, 0);
        lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 22);
        U.p_cap = cap;
        U.p_from = aos_label_boxed(p, "", aos_font_small, AOS_C_TEXT, w - 240, lv_font_get_line_height(aos_font_small));
        lv_label_set_long_mode(U.p_from, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(U.p_from, LV_ALIGN_TOP_MID, 0, 46);

        int32_t side = LV_MIN(w - 112, h - 610);
        U.cover_px = side;
        U.p_art = art_create(p, side, 28);
        lv_obj_set_style_shadow_width(U.p_art.art, 60, 0);
        lv_obj_set_style_shadow_color(U.p_art.art, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(U.p_art.art, LV_OPA_50, 0);
        lv_obj_set_style_shadow_offset_y(U.p_art.art, 16, 0);
        lv_obj_align(U.p_art.art, LV_ALIGN_TOP_MID, 0, 116);
        y = 116 + side + 36;

        int32_t tw = w - 112;
        U.p_title = aos_label_boxed(p, "", tf, AOS_C_TEXT, tw, 2 * lv_font_get_line_height(tf));
        lv_label_set_long_mode(U.p_title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(U.p_title, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.p_title, LV_ALIGN_TOP_LEFT, 56, y);
        y += 2 * lv_font_get_line_height(tf) + 2;
        U.p_artist = aos_label_boxed(p, "", aos_font_body, lv_color_hex(0xFF8FA6), tw,
                                     lv_font_get_line_height(aos_font_body));
        lv_label_set_long_mode(U.p_artist, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(U.p_artist, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.p_artist, LV_ALIGN_TOP_LEFT, 56, y);
        y += lv_font_get_line_height(aos_font_body) + 4;
        U.p_format = aos_label_boxed(p, "", aos_font_caption, lv_color_hex(0x98989D), tw, 26);
        lv_obj_set_style_text_align(U.p_format, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.p_format, LV_ALIGN_TOP_LEFT, 56, y);
        y += 26 + 30;
    } else {
        U.p_from = NULL;
        int32_t side = LV_MIN(h - 360, w / 2 - 40);
        U.cover_px = side;
        U.p_art = art_create(p, side, 22);
        lv_obj_set_style_shadow_width(U.p_art.art, 40, 0);
        lv_obj_set_style_shadow_color(U.p_art.art, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(U.p_art.art, LV_OPA_50, 0);
        lv_obj_set_style_shadow_offset_y(U.p_art.art, 12, 0);
        lv_obj_align(U.p_art.art, LV_ALIGN_TOP_LEFT, 48, 36);

        int32_t tx = 48 + side + 32, tw = w - tx - 40;
        lv_obj_t *cap = aos_label(p, _("REPRODUCIENDO DESDE"), aos_font_tiny, lv_color_hex(0xC7C7CC));
        lv_obj_set_style_text_letter_space(cap, 1, 0);
        lv_obj_align(cap, LV_ALIGN_TOP_LEFT, tx, 44);
        U.p_cap = cap;
        U.p_from = aos_label_boxed(p, "", aos_font_small, AOS_C_TEXT, tw, lv_font_get_line_height(aos_font_small));
        lv_label_set_long_mode(U.p_from, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(U.p_from, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.p_from, LV_ALIGN_TOP_LEFT, tx, 66);
        U.p_title = aos_label_boxed(p, "", tf, AOS_C_TEXT, tw, 3 * lv_font_get_line_height(tf));
        lv_label_set_long_mode(U.p_title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(U.p_title, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.p_title, LV_ALIGN_TOP_LEFT, tx, 110);
        U.p_artist = aos_label_boxed(p, "", aos_font_body, lv_color_hex(0xFF8FA6), tw,
                                     lv_font_get_line_height(aos_font_body));
        lv_label_set_long_mode(U.p_artist, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(U.p_artist, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.p_artist, LV_ALIGN_TOP_LEFT, tx, 36 + side - 70);
        U.p_format = aos_label_boxed(p, "", aos_font_caption, lv_color_hex(0x98989D), tw, 26);
        lv_obj_set_style_text_align(U.p_format, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.p_format, LV_ALIGN_TOP_LEFT, tx, 36 + side - 30);
        y = 36 + side + 40;
    }

    /* progress */
    int32_t pw = w - 112;
    U.p_progress = slider(p, pw, 8, 6);
    lv_slider_set_range(U.p_progress, 0, 1000);
    lv_obj_align(U.p_progress, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_event_cb(U.p_progress, seek_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(U.p_progress, seek_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(U.p_progress, seek_cb, LV_EVENT_PRESS_LOST, NULL);
    U.p_elapsed = aos_label(p, "0:00", aos_font_caption, lv_color_hex(0x98989D));
    lv_obj_align(U.p_elapsed, LV_ALIGN_TOP_LEFT, 56, y + 20);
    U.p_remain = aos_label(p, "-0:00", aos_font_caption, lv_color_hex(0x98989D));
    lv_obj_align(U.p_remain, LV_ALIGN_TOP_RIGHT, -56, y + 20);
    y += 20 + 28;

    /* transport: shuffle, previous, play, next, the place in the folder */
    int32_t th = portrait ? 150 : 120;
    lv_obj_t *row = box(p);
    lv_obj_set_size(row, w - 64, th);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y + (portrait ? 6 : 0));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sh = icon_button(row, AOS_SYM_SHUFFLE_VARIANT, &aos_sym_44, 88, 88, shuffle_cb, NULL, &U.p_shuffle);
    (void)sh;
    icon_button(row, AOS_SYM_SKIP_PREVIOUS, &aos_sym_72, 124, 124, skip_cb, (void *)(intptr_t)-1, NULL);
    lv_obj_t *play = icon_button(row, AOS_SYM_PLAY, &aos_sym_72, 136, 136, play_cb, NULL, &U.p_play);
    lv_obj_set_style_transform_scale(U.p_play, 330, 0);
    lv_obj_set_style_transform_pivot_x(U.p_play, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(U.p_play, lv_pct(50), 0);
    (void)play;
    icon_button(row, AOS_SYM_SKIP_NEXT, &aos_sym_72, 124, 124, skip_cb, (void *)(intptr_t)1, NULL);
    U.p_count = aos_label_boxed(row, "", aos_font_caption, lv_color_hex(0xAEAEB2), 88, 28);
    y += th + (portrait ? 18 : 4);

    /* volume */
    lv_obj_t *lo = glyph(p, AOS_SYM_VOLUME_LOW, &aos_sym_28, lv_color_hex(0xAEAEB2));
    lv_obj_t *hi = glyph(p, AOS_SYM_VOLUME_HIGH, &aos_sym_28, lv_color_hex(0xAEAEB2));
    U.p_volume = slider(p, pw - 110, 8, 10);
    lv_slider_set_range(U.p_volume, 0, 100);
    lv_slider_set_value(U.p_volume, aos_hal_volume_get(), LV_ANIM_OFF);
    lv_obj_add_event_cb(U.p_volume, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(U.p_volume, LV_ALIGN_TOP_MID, 0, y + 14);
    lv_obj_align_to(lo, U.p_volume, LV_ALIGN_OUT_LEFT_MID, -22, 0);
    lv_obj_align_to(hi, U.p_volume, LV_ALIGN_OUT_RIGHT_MID, 22, 0);
    (void)h;
}

/* ---- the mini player (portrait) ------------------------------------------------ */

static void build_mini(int32_t w, int32_t h)
{
    U.mini = box(U.root);
    lv_obj_set_size(U.mini, w - 24, 120);
    lv_obj_align(U.mini, LV_ALIGN_TOP_LEFT, 12, h - 120 - 8);
    lv_obj_set_style_radius(U.mini, 26, 0);
    lv_obj_set_style_bg_color(U.mini, C_MINI, 0);
    lv_obj_set_style_bg_opa(U.mini, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(U.mini, lv_color_hex(0x323236), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(U.mini, 40, 0);
    lv_obj_set_style_shadow_color(U.mini, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(U.mini, LV_OPA_70, 0);
    lv_obj_add_flag(U.mini, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.mini, open_player_cb, LV_EVENT_CLICKED, NULL);

    U.mini_art = art_create(U.mini, MUSIC_THUMB_PX, 14);
    lv_obj_align(U.mini_art.art, LV_ALIGN_LEFT_MID, 16, 0);
    int32_t tx = 16 + MUSIC_THUMB_PX + 20, tw = w - 24 - tx - 200;
    U.mini_title = aos_label_boxed(U.mini, "", aos_font_body, AOS_C_TEXT, tw,
                                   lv_font_get_line_height(aos_font_body));
    lv_label_set_long_mode(U.mini_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(U.mini_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(U.mini_title, LV_ALIGN_LEFT_MID, tx, -17);
    U.mini_sub = aos_label_boxed(U.mini, "", aos_font_small, AOS_C_DIM, tw,
                                 lv_font_get_line_height(aos_font_small));
    lv_label_set_long_mode(U.mini_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(U.mini_sub, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(U.mini_sub, LV_ALIGN_LEFT_MID, tx, 19);
    aos_make_decorative(U.mini_title);
    aos_make_decorative(U.mini_sub);

    lv_obj_t *play = icon_button(U.mini, AOS_SYM_PLAY, &aos_sym_72, 96, 96, play_cb, NULL, &U.mini_play);
    lv_obj_align(play, LV_ALIGN_RIGHT_MID, -104, 0);
    lv_obj_t *next = icon_button(U.mini, AOS_SYM_SKIP_NEXT, &aos_sym_44, 96, 96, skip_cb, (void *)(intptr_t)1, NULL);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(U.mini, LV_OBJ_FLAG_HIDDEN);
}

/* -------------------------------------------------------------------------- */

static void first_open(void)
{
    /* Something playing from under the music folder: open where it is.
     * Nothing playing but a last track remembered: open in its folder. */
    snprintf(S.cwd, sizeof S.cwd, "%s", aos_hal_path_music());
    aos_player_info_t in;
    aos_hal_player_info(&in);
    bool active = in.state != AOS_PLAYER_STOPPED && in.path[0] && !in.live;
    const char *here = active ? in.path : (U.has_last ? U.last_path : NULL);
    size_t root_len = strlen(S.cwd);
    if (here && strncmp(here, S.cwd, root_len) == 0 && here[root_len] == '/') {
        const char *slash = strrchr(here, '/');
        size_t len = (size_t)(slash - here);
        if (len < sizeof S.cwd) {
            memcpy(S.cwd, here, len);
            S.cwd[len] = '\0';
        }
    }
    S.player = active;
    memset(S.scroll, 0, sizeof S.scroll);
    S.init = true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&U, 0, sizeof U);
    U.playing_row = -1;
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    U.entries = malloc(MAX_ENTRIES * sizeof(entry_t));
    U.has_last = aos_hal_player_last(U.last_path, sizeof U.last_path, &U.last_pos);
    if (U.has_last) {
        aos_player_info_t in;
        aos_hal_player_info(&in);
        if (in.state != AOS_PLAYER_STOPPED && !in.live) U.has_last = false;
        int num;
        split_track(U.last_path, U.last_title, sizeof U.last_title, U.last_artist, sizeof U.last_artist, &num);
    }
    if (!S.init) first_open();
    struct stat st;
    if (stat(S.cwd, &st) != 0 || !S_ISDIR(st.st_mode)) {       /* the card changed */
        snprintf(S.cwd, sizeof S.cwd, "%s", aos_hal_path_music());
        memset(S.scroll, 0, sizeof S.scroll);
    }

    int32_t list_w = U.land ? U.W * 45 / 100 : U.W;
    U.list = box(root);
    lv_obj_add_flag(U.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(U.list, list_w, U.H);
    lv_obj_set_pos(U.list, 0, 0);
    lv_obj_set_flex_flow(U.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(U.list, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.list, 0, 0);
    lv_obj_set_style_pad_bottom(U.list, U.land ? 24 : 150, 0);   /* room for the mini player */
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.list, LV_SCROLLBAR_MODE_ACTIVE);

    if (U.land) {
        lv_obj_t *sep = box(root);
        lv_obj_set_size(sep, 1, U.H);
        lv_obj_set_pos(sep, list_w, 0);
        lv_obj_set_style_bg_color(sep, C_SEP, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        U.player = box(root);
        lv_obj_set_size(U.player, U.W - list_w - 1, U.H);
        lv_obj_set_pos(U.player, list_w + 1, 0);
    } else {
        U.player = box(root);
        lv_obj_set_size(U.player, U.W, U.H);
        lv_obj_add_flag(U.player, LV_OBJ_FLAG_CLICKABLE);      /* not through to the list */
    }
    const lv_image_dsc_t *big, *thumb;
    uint32_t avg;
    music_cover_current(&big, &thumb, &avg);
    lv_obj_set_style_bg_color(U.player, tint_of(avg), 0);
    lv_obj_set_style_bg_grad_color(U.player, AOS_C_BG, 0);
    lv_obj_set_style_bg_grad_dir(U.player, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(U.player, 0, 0);
    lv_obj_set_style_bg_grad_stop(U.player, 230, 0);
    lv_obj_set_style_bg_opa(U.player, LV_OPA_COVER, 0);
    build_player(U.player, lv_obj_get_style_width(U.player, 0), U.H, !U.land);

    if (!U.land) build_mini(U.W, U.H);
    build_list();
    art_set(&U.p_art, big);
    art_set(&U.mini_art, thumb);
    if (!U.land) {
        if (S.player) show_player(true);
        else lv_obj_add_flag(U.player, LV_OBJ_FLAG_HIDDEN);
    }
    U.timer = lv_timer_create(timer_cb, 100, NULL);
    refresh();
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_delete(U.timer);
    U.timer = NULL;
    if (U.list) S.scroll[depth()] = lv_obj_get_scroll_y(U.list);
    img_cancel_all(IMG_OWNER_MUSIC);
    music_cover_abandon();
    /* the objects are deleted by the runtime after this; the pixels of the
     * rows go now, and nothing draws in between */
    free_rows();
    free(U.entries);
    memset(&U, 0, sizeof U);
}

/* Archivos plays a song here (aos_ui_open_app_with): the song and the rest
 * of its folder, the player in front. A song under the music folder also
 * takes the list to its folder; one from anywhere else only plays, the list
 * stays where it was (the list only walks the music folder). */
static void open_arg(const char *path)
{
    const char *root = aos_hal_path_music();
    size_t rl = strlen(root);
    const char *slash = strrchr(path, '/');
    if (slash && strncmp(path, root, rl) == 0 && path[rl] == '/' && (size_t)(slash - path) < sizeof S.cwd) {
        char dir[sizeof S.cwd];
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = '\0';
        if (strcmp(dir, S.cwd) != 0) go_to(dir);
    }
    if (play_file(path)) show_player(true);
}

static void show(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_resume(U.timer);
    if (U.p_volume) lv_slider_set_value(U.p_volume, aos_hal_volume_get(), LV_ANIM_OFF);
    const char *arg = aos_ui_take_open_arg("aos.music");
    if (arg) open_arg(arg);
    refresh();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_pause(U.timer);
}

/* Back: from the player to the list, up one folder, and from the music
 * folder to the home screen. The music goes on: it is the HAL's. */
static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (!U.land && S.player) {
        show_player(false);
        return true;
    }
    if (!at_root()) {
        up_cb(NULL);
        return true;
    }
    return false;
}

void aos_app_music_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.music", .name = "Música", .icon = AOS_SYM_MUSIC,
            .color_a = 0xFF375F, .color_b = 0xB0123F,
            .flags = AOS_APP_FLAG_KEEP,
            .order = 45,
        },
        .create = create, .destroy = destroy, .show = show, .hide = hide, .back = back,
    };
}
