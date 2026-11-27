/*
 * Botonera - the pages and their buttons, and the sounds folder.
 *
 * The grid lives in <data>/botonera.txt, a text file that the app and its
 * page in the portal both read and write (the page edits the grid from the
 * phone; the app sees the file's date change and reads it again). One line
 * per page and one per button, fields split by tabs, so a name can have
 * spaces, accents and emoji:
 *
 *     # Botonera 1
 *     P   Efectos
 *     B   0   aplausos.wav   Aplausos   30D158   tap   cut   0   100
 *
 * B: the slot (0..11), the file (in the sounds folder), the name, the
 * colour, the icon's key, the mode (cut, over, hold), loop (0/1) and the
 * volume (0..100). A button that is not written is empty. Unknown lines are
 * skipped, so the format can grow.
 */
#include "bn.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

const bn_icon_t bn_icons[] = {
    { "note",    AOS_SYM_MUSIC_NOTE },
    { "bell",    AOS_SYM_BELL_RING },
    { "star",    AOS_SYM_STAR },
    { "fire",    AOS_SYM_FIRE },
    { "bomb",    AOS_SYM_BOMB },
    { "trophy",  AOS_SYM_TROPHY },
    { "bolt",    AOS_SYM_LIGHTNING_BOLT },
    { "heart",   AOS_SYM_HEART_PULSE },
    { "volume",  AOS_SYM_VOLUME_HIGH },
    { "mic",     AOS_SYM_MICROPHONE },
    { "tap",     AOS_SYM_GESTURE_TAP_BUTTON },
    { "check",   AOS_SYM_CHECK },
    { "close",   AOS_SYM_CLOSE },
    { "alarm",   AOS_SYM_ALARM },
    { "clock",   AOS_SYM_CLOCK_OUTLINE },
    { "sand",    AOS_SYM_TIMER_SAND },
    { "wave",    AOS_SYM_WAVEFORM },
    { "sine",    AOS_SYM_SINE_WAVE },
    { "rain",    AOS_SYM_WEATHER_RAINY },
    { "wind",    AOS_SYM_WEATHER_WINDY },
    { "sun",     AOS_SYM_WEATHER_SUNNY },
    { "snow",    AOS_SYM_SNOWFLAKE },
    { "cat",     AOS_SYM_CAT },
    { "robot",   AOS_SYM_ROBOT },
    { "rocket",  AOS_SYM_ROCKET_LAUNCH },
    { "phone",   AOS_SYM_PHONE },
    { "door",    AOS_SYM_DOOR },
    { "dice",    AOS_SYM_DICE_5 },
    { "gamepad", AOS_SYM_GAMEPAD_VARIANT },
    { "cards",   AOS_SYM_CARDS_PLAYING },
    { "flag",    AOS_SYM_FLAG_OUTLINE },
    { "magnet",  AOS_SYM_MAGNET },
    { "bulb",    AOS_SYM_LIGHTBULB },
    { "tv",      AOS_SYM_TELEVISION },
    { "person",  AOS_SYM_ACCOUNT_CIRCLE },
    { "rec",     AOS_SYM_RECORD_REC },
};
const int bn_n_icons = (int)(sizeof bn_icons / sizeof bn_icons[0]);

const uint32_t bn_colors[] = {
    0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x63E6BE, 0x40C8E0,
    0x0A84FF, 0x5E5CE6, 0xBF5AF2, 0xFF375F, 0xAC8E68, 0x8E8E93,
};
const int bn_n_colors = (int)(sizeof bn_colors / sizeof bn_colors[0]);

static const char *const MODE_KEY[BN_MODE_COUNT] = { "cut", "over", "hold" };

int bn_icon_find(const char *key)
{
    for (int i = 0; i < bn_n_icons; i++) if (!strcmp(bn_icons[i].key, key)) return i;
    return 0;
}

const char *bn_dir(void)
{
    static char p[96];
    if (!p[0]) {
        const char *root = aos_hal_path_sd_root();
        snprintf(p, sizeof p, "%s/sonidos", root ? root : "/sdcard");
    }
    return p;
}

const char *bn_grid_path(void)
{
    static char p[96];
    if (!p[0]) snprintf(p, sizeof p, "%s/botonera.txt", aos_hal_path_data());
    return p;
}

uint32_t bn_grid_mtime(void)
{
    struct stat st;
    if (stat(bn_grid_path(), &st) != 0) return 0;
    return (uint32_t)st.st_mtime ^ (uint32_t)st.st_size << 20;
}

void bn_pad_clear(bn_pad_t *p)
{
    memset(p, 0, sizeof *p);
    p->color = 0x8E8E93;
    p->vol = 100;
}

bool bn_pad_empty(const bn_pad_t *p) { return !p->file[0]; }

static void copy(char *dst, const char *src, size_t n)
{
    size_t k = strlen(src);
    if (k >= n) {
        k = n - 1;
        /* not in the middle of a UTF-8 character */
        while (k && ((unsigned char)src[k] & 0xC0) == 0x80) k--;
    }
    memcpy(dst, src, k);
    dst[k] = 0;
}

void bn_pretty(const char *file, char *out, size_t n)
{
    char b[BN_NAME_MAX];
    copy(b, file, sizeof b);
    char *dot = strrchr(b, '.');
    if (dot) *dot = 0;
    for (char *c = b; *c; c++) if (*c == '_' || *c == '-') *c = ' ';
    if (b[0] >= 'a' && b[0] <= 'z') b[0] = (char)(b[0] - 'a' + 'A');
    copy(out, b, n);
}

void bn_grid_defaults(bn_grid_t *g)
{
    memset(g, 0, sizeof *g);
    g->n_pages = 2;
    copy(g->page[0].name, _("Efectos"), BN_NAME_MAX);
    copy(g->page[1].name, _("Ambiente"), BN_NAME_MAX);
    for (int p = 0; p < BN_PAGES; p++)
        for (int s = 0; s < BN_PER_PAGE; s++) bn_pad_clear(&g->page[p].pad[s]);
    for (int k = 0; k < bn_n_examples && k < 2 * BN_PER_PAGE; k++) {
        const bn_example_t *e = &bn_examples[k];
        bn_pad_t *p = &g->page[k / BN_PER_PAGE].pad[k % BN_PER_PAGE];
        copy(p->file, e->file, sizeof p->file);
        copy(p->name, _(e->name), sizeof p->name);
        p->color = e->color;
        p->icon = (uint8_t)bn_icon_find(e->icon);
        p->mode = e->mode;
        p->loop = e->loop;
        p->vol = 100;
    }
}

/* splits a line at the tabs, in place */
static int fields(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    return n;
}

bool bn_grid_load(bn_grid_t *g)
{
    FILE *f = fopen(bn_grid_path(), "r");
    if (!f) return false;
    memset(g, 0, sizeof *g);
    for (int p = 0; p < BN_PAGES; p++)
        for (int s = 0; s < BN_PER_PAGE; s++) bn_pad_clear(&g->page[p].pad[s]);
    char line[400];
    int page = -1;
    while (fgets(line, sizeof line, f)) {
        size_t k = strlen(line);
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r')) line[--k] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *fl[10];
        int n = fields(line, fl, 10);
        if (!strcmp(fl[0], "P") && n >= 2) {
            if (g->n_pages >= BN_PAGES) break;
            page = g->n_pages++;
            copy(g->page[page].name, fl[1], BN_NAME_MAX);
        } else if (!strcmp(fl[0], "B") && n >= 3 && page >= 0) {
            int s = atoi(fl[1]);
            if (s < 0 || s >= BN_PER_PAGE || !fl[2][0]) continue;
            /* a file name, not a path: nothing outside the folder */
            if (strchr(fl[2], '/') || strstr(fl[2], "..")) continue;
            bn_pad_t *p = &g->page[page].pad[s];
            copy(p->file, fl[2], sizeof p->file);
            if (n >= 4 && fl[3][0]) copy(p->name, fl[3], sizeof p->name);
            else bn_pretty(p->file, p->name, sizeof p->name);
            if (n >= 5) p->color = (uint32_t)strtoul(fl[4], NULL, 16) & 0xFFFFFF;
            if (n >= 6) p->icon = (uint8_t)bn_icon_find(fl[5]);
            if (n >= 7) {
                for (int m = 0; m < BN_MODE_COUNT; m++) if (!strcmp(fl[6], MODE_KEY[m])) p->mode = (uint8_t)m;
            }
            if (n >= 8) p->loop = atoi(fl[7]) != 0;
            if (n >= 9) {
                int v = atoi(fl[8]);
                p->vol = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
            }
        }
    }
    fclose(f);
    if (!g->n_pages) {
        g->n_pages = 1;
        copy(g->page[0].name, _("Efectos"), BN_NAME_MAX);
    }
    return true;
}

/* a tab or a line break in a name would break the line */
static void clean(char *dst, const char *src, size_t n)
{
    copy(dst, src, n);
    for (char *c = dst; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
}

bool bn_grid_save(const bn_grid_t *g)
{
    char tmp[120];
    snprintf(tmp, sizeof tmp, "%s.part", bn_grid_path());
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fputs("# Botonera 1: P <page>; B <slot> <file> <name> <color> <icon> <cut|over|hold> <loop> <vol>\n", f) >= 0;
    for (int p = 0; p < g->n_pages && ok; p++) {
        char nm[BN_NAME_MAX], line[400];
        clean(nm, g->page[p].name, sizeof nm);
        snprintf(line, sizeof line, "P\t%s\n", nm);
        ok = fputs(line, f) >= 0;
        for (int s = 0; s < BN_PER_PAGE && ok; s++) {
            const bn_pad_t *b = &g->page[p].pad[s];
            if (bn_pad_empty(b)) continue;
            char fn[BN_FILE_MAX];
            clean(nm, b->name, sizeof nm);
            clean(fn, b->file, sizeof fn);
            snprintf(line, sizeof line, "B\t%d\t%s\t%s\t%06X\t%s\t%s\t%d\t%d\n", s, fn, nm,
                     (unsigned)(b->color & 0xFFFFFF), bn_icons[b->icon < bn_n_icons ? b->icon : 0].key,
                     MODE_KEY[b->mode < BN_MODE_COUNT ? b->mode : 0], b->loop ? 1 : 0, b->vol);
            ok = fputs(line, f) >= 0;
        }
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(bn_grid_path());
    return rename(tmp, bn_grid_path()) == 0;
}

/* ---- the folder ---- */

bool bn_is_sound(const char *name)
{
    if (name[0] == '.') return false;           /* macOS's ._ files, and the hidden */
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".wav") || !strcasecmp(dot, ".mp3"));
}

static bool scan_cb(const aos_dir_entry_t *e, void *ctx)
{
    bn_files_t *f = ctx;
    if (e->dir || !bn_is_sound(e->name) || strlen(e->name) >= BN_FILE_MAX) return true;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 32;
        char (*nn)[BN_FILE_MAX] = bn_alloc((size_t)cap * BN_FILE_MAX);
        uint32_t *ns = bn_alloc((size_t)cap * sizeof(uint32_t));
        uint32_t *nt = bn_alloc((size_t)cap * sizeof(uint32_t));
        if (!nn || !ns || !nt) {
            bn_free(nn);
            bn_free(ns);
            bn_free(nt);
            return false;
        }
        if (f->n) {
            memcpy(nn, f->name, (size_t)f->n * BN_FILE_MAX);
            memcpy(ns, f->size, (size_t)f->n * sizeof(uint32_t));
            memcpy(nt, f->mtime, (size_t)f->n * sizeof(uint32_t));
        }
        bn_free(f->name);
        bn_free(f->size);
        bn_free(f->mtime);
        f->name = nn;
        f->size = ns;
        f->mtime = nt;
        f->cap = cap;
    }
    copy(f->name[f->n], e->name, BN_FILE_MAX);
    f->size[f->n] = e->size;
    f->mtime[f->n] = e->mtime;
    f->n++;
    return true;
}

bool bn_files_scan(bn_files_t *f)
{
    bn_files_free(f);
    if (aos_hal_dir_scan(bn_dir(), scan_cb, f) < 0) return false;
    /* by name, case aside (an insertion sort: a folder of sounds is short) */
    char tmp[BN_FILE_MAX];
    for (int i = 1; i < f->n; i++) {
        uint32_t sz = f->size[i], mt = f->mtime[i];
        memcpy(tmp, f->name[i], BN_FILE_MAX);
        int j = i - 1;
        while (j >= 0 && strcasecmp(f->name[j], tmp) > 0) {
            memcpy(f->name[j + 1], f->name[j], BN_FILE_MAX);
            f->size[j + 1] = f->size[j];
            f->mtime[j + 1] = f->mtime[j];
            j--;
        }
        memcpy(f->name[j + 1], tmp, BN_FILE_MAX);
        f->size[j + 1] = sz;
        f->mtime[j + 1] = mt;
    }
    return true;
}

int bn_files_find(const bn_files_t *f, const char *name)
{
    for (int i = 0; i < f->n; i++) if (!strcmp(f->name[i], name)) return i;
    return -1;
}

void bn_files_free(bn_files_t *f)
{
    bn_free(f->name);
    bn_free(f->size);
    bn_free(f->mtime);
    memset(f, 0, sizeof *f);
}
