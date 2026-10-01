/*
 * P4OS - Archivos: the card, like the phone's Files.
 *
 * What it does
 *   - Browses the card: a large title, a path bar with the folders above, a
 *     search field (this folder, or every folder under it), sort by name,
 *     date, size or type, a list or a grid of icons (photos as thumbnails),
 *     and at the foot of the list how many things there are and how much
 *     room is left. Landscape adds a sidebar of places (the card, Fotos,
 *     Música, Registros, Firmware, Apps) and the folders the person pinned.
 *   - Acts on it: a long press for the menu of one item, or "Seleccionar"
 *     for several: new folder, rename, duplicate, copy / cut and paste in
 *     another folder, delete (a folder with everything in it), and an
 *     information sheet (a folder's size is added up in the background).
 *   - Opens what it can: photos in Fotos, songs in Música, a .bin under
 *     /firmware in the Programador (aos_ui_open_app_with(), aos_open_arg.c),
 *     and any text - logs, CSV, JSON, YAML, the system's own .txt - in a
 *     viewer of its own: monospaced, read from the card a block at a time
 *     (a file is never loaded whole), CSV as a table with its header row
 *     held on top and a chart of any numeric column, binary files as hex.
 *
 * How it stays smooth with thousands of files
 *   Nothing that walks the card runs in LVGL's task; only the viewer reads
 *   there, 32 KB at a time. A folder is listed by a thread in two passes:
 *   the names with each file's size and date in one walk of the directory
 *   (aos_hal_dir_scan: FatFs's own records on the board, because on FAT a
 *   stat() per file walks the directory again and a big folder would take
 *   O(n^2)), published in batches as they come, then a count of what each
 *   folder holds.
 *   Sorting and the search filter run on a thread too. The list itself is
 *   a recycled pool of rows - a few more than fit on the screen - rebound
 *   to whatever index scrolls into view, over a spacer as tall as the whole
 *   list. Copying, moving and deleting run on a thread of their own with a
 *   progress sheet, and go on with the app closed.
 *
 *   Every job is a heap object with two references, the thread's and the
 *   screen's, and whichever lets go last frees it: leaving a folder that is
 *   still being read costs a flag, never a wait.
 *
 * The system's own files - menu.txt, modules.txt, ha.txt, /apps, /firmware,
 * /data, /lang, /icons - can be changed, but only past a warning.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"
#include "aos_mono.h"
#include "aos_app_image.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define APP_ID          "aos.files"
#define C_TINT          AOS_C_ACCENT
#define C_SEP           lv_color_hex(0x2C2C2E)
#define C_SHEET         lv_color_hex(0x1C1C1E)

#define PATH_LEN        512
#define NAME_LEN        256
#define MAX_ENTRIES     32768
#define ECHUNK_SHIFT    11
#define ECHUNK          (1 << ECHUNK_SHIFT)     /* entries per block: 32 KB, PSRAM */
#define NCHUNK_BYTES    65536                   /* names per block */
#define MAX_NCHUNKS     192
#define SEARCH_MAX      500                     /* results of a search in subfolders */
#define SEARCH_VISITS   40000                   /* entries it looks at, at most */
#define SEARCH_QUEUE    4096                    /* folders waiting to be looked into */
#define WALK_DEPTH      24
#define COPY_BUF        (32 * 1024)
#define JOB_STACK       8192
#define IMG_OWNER_FILES 3                       /* aos_app_image.h: MUSIC 1, PHOTOS 2 */
#define TH_N            48                      /* thumbnails kept */
#define TH_PX           144
#define TH_ASK_MAX      8                       /* asked and not back yet */
#define MAX_FAV         8
#define MAX_CLIP        4000

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static uint32_t perf_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u);
}

/* -------------------------------------------------------------------------- */
/* Paths and names                                                             */
/* -------------------------------------------------------------------------- */

/* Copies with a cap; never leaves 'd' unterminated. */
static void scpy(char *d, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memmove(d, s, l);
    d[l] = 0;
}

/* a + "/" + b into out (out may be a). false if it does not fit: nothing is
 * ever cut short, a cut path would name another file. */
static bool join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t slash = (la && lb) ? 1 : 0;
    if (la + slash + lb + 1 > n) return false;
    if (out != a) memmove(out, a, la);
    if (slash) out[la] = '/';
    memmove(out + la + slash, b, lb + 1);
    return true;
}

static const char *base_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static const char *card(void) { return aos_hal_path_sd_root(); }

/* The card's path of 'abs' ("" for the root itself), NULL if it is not on it. */
static const char *rel_of(const char *abs)
{
    const char *r = card();
    if (!r || !abs) return NULL;
    size_t n = strlen(r);
    if (strncmp(abs, r, n)) return NULL;
    if (!abs[n]) return "";
    return abs[n] == '/' ? abs + n + 1 : NULL;
}

static bool abs_of(char *out, size_t n, const char *rel)
{
    const char *r = card();
    return r && join(out, n, r, rel);
}

/* The folder above a card path, in place ("a/b" -> "a", "a" -> ""). */
static void rel_up(char *rel)
{
    char *s = strrchr(rel, '/');
    if (s) *s = 0;
    else rel[0] = 0;
}

static int rel_depth(const char *rel)
{
    if (!rel[0]) return 0;
    int d = 1;
    for (const char *p = rel; *p; p++) d += *p == '/';
    return d;
}

/* 'p' is 'dir' or under it. */
static bool path_under(const char *p, const char *dir)
{
    size_t n = strlen(dir);
    return !strncmp(p, dir, n) && (p[n] == 0 || p[n] == '/');
}

static bool exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool is_dir_path(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Lower case without accents, for the search: "Dúo" and "duo" match. The
 * combining accent a Mac writes is dropped, the composed one mapped. */
static void fold(char *out, size_t n, const char *in)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 4 < n) {
        unsigned c = *p;
        if (c < 0x80) { out[o++] = (char)tolower((int)c); p++; continue; }
        if (c == 0xCC || (c == 0xCD && p[1] && p[1] <= 0xAF)) { p += p[1] ? 2 : 1; continue; }  /* U+0300..036F */
        if (c == 0xC3 && p[1]) {
            unsigned d = p[1] & ~0x20u;         /* the lower-case half onto the upper */
            char m = 0;
            if (d >= 0x80 && d <= 0x85) m = 'a';
            else if (d == 0x87) m = 'c';
            else if (d >= 0x88 && d <= 0x8B) m = 'e';
            else if (d >= 0x8C && d <= 0x8F) m = 'i';
            else if (d == 0x91) m = 'n';
            else if (d >= 0x92 && d <= 0x96) m = 'o';
            else if (d >= 0x99 && d <= 0x9C) m = 'u';
            else if (d == 0x9D || p[1] == 0xBF) m = 'y';
            if (m) { out[o++] = m; p += 2; continue; }
        }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        for (int i = 0; i < len && *p; i++) out[o++] = (char)*p++;
    }
    out[o] = 0;
}

/* A name somebody typed, for a file or a folder: what FAT refuses, said. */
static const char *name_problem(const char *s)
{
    if (!s[0]) return _("Falta el nombre.");
    if (!strcmp(s, ".") || !strcmp(s, "..")) return _("Ese nombre no se puede usar.");
    if (s[0] == '.') return _("Un nombre que empieza con punto queda oculto.");
    if (strpbrk(s, "/\\:*?\"<>|")) return _("El nombre no puede llevar / \\ : * ? \" < > |");
    size_t n = strlen(s);
    if (s[n - 1] == ' ' || s[n - 1] == '.') return _("El nombre no puede terminar en espacio ni en punto.");
    if (n > 200) return _("El nombre es demasiado largo.");
    return NULL;
}

/* A free name in 'dir' for 'name': itself, else "name 2.ext", "name 3.ext"... */
static bool unique_name(const char *dir, const char *name, bool is_dir, char *out, size_t n)
{
    char p[PATH_LEN];
    if (!join(p, sizeof p, dir, name)) return false;
    if (!exists(p)) { scpy(out, n, name); return true; }
    const char *dot = is_dir ? NULL : strrchr(name, '.');
    if (dot == name) dot = NULL;
    size_t stem = dot ? (size_t)(dot - name) : strlen(name);
    /* "foto 2.jpg" copied again gives "foto 3.jpg", not "foto 2 2.jpg" */
    size_t s2 = stem;
    while (s2 > 0 && isdigit((unsigned char)name[s2 - 1])) s2--;
    if (s2 > 1 && s2 < stem && name[s2 - 1] == ' ') stem = s2 - 1;
    for (int i = 2; i < 1000; i++) {
        char cand[NAME_LEN];
        int w = snprintf(cand, sizeof cand, "%.*s %d%s", (int)(stem > 200 ? 200 : stem), name, i, dot ? dot : "");
        if (w <= 0 || w >= (int)sizeof cand) return false;
        if (!join(p, sizeof p, dir, cand)) return false;
        if (!exists(p)) { scpy(out, n, cand); return true; }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Numbers and dates, the way the rest of the system writes them               */
/* -------------------------------------------------------------------------- */

static void comma(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

/* 1000 -> "1.000" */
static void fmt_count(char *out, size_t n, long v)
{
    char t[24];
    snprintf(t, sizeof t, "%ld", v < 0 ? -v : v);
    size_t l = strlen(t), o = 0;
    if (v < 0 && o + 1 < n) out[o++] = '-';
    for (size_t i = 0; i < l && o + 2 < n; i++) {
        out[o++] = t[i];
        if ((l - i - 1) % 3 == 0 && i + 1 < l) out[o++] = '.';
    }
    out[o] = 0;
}

/* As the phone does: powers of 1000, "512 bytes", "12 KB", "1,2 MB". */
static void fmt_size(char *out, size_t n, uint64_t b)
{
    if (b < 1000) {
        if (b == 1) scpy(out, n, _("1 byte"));
        else snprintf(out, n, _("%u bytes"), (unsigned)b);
        return;
    }
    static const char *const UNIT[] = { "KB", "MB", "GB", "TB" };
    double v = (double)b / 1000.0;
    int u = 0;
    while (v >= 999.5 && u < 3) { v /= 1000.0; u++; }
    if (u == 0 || v >= 9.95) snprintf(out, n, "%.0f %s", v, UNIT[u]);
    else snprintf(out, n, "%.1f %s", v, UNIT[u]);
    comma(out);
}

static void fmt_items(char *out, size_t n, uint32_t items)
{
    char c[16];
    fmt_count(c, sizeof c, (long)items);
    if (items == 1) scpy(out, n, _("1 elemento"));
    else snprintf(out, n, _("%s elementos"), c);
}

static const char *const MONTH[12] = {
    N_("ene"), N_("feb"), N_("mar"), N_("abr"), N_("may"), N_("jun"),
    N_("jul"), N_("ago"), N_("sep"), N_("oct"), N_("nov"), N_("dic"),
};

/* "Hoy 14:28", "Ayer 09:10", "28 sep 2026"; long: "28 sep 2026, 14:28". */
static void fmt_date(char *out, size_t n, uint32_t t, bool full)
{
    if (!t) { scpy(out, n, "-"); return; }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    if (!full && aos_hal_time_is_valid()) {
        struct tm nw;
        aos_hal_time_now(&nw);
        if (nw.tm_year == tm.tm_year && nw.tm_yday == tm.tm_yday) {
            snprintf(out, n, _("Hoy %02d:%02d"), tm.tm_hour, tm.tm_min);
            return;
        }
        nw.tm_isdst = -1;
        time_t y = mktime(&nw) - 86400;
        struct tm ytm;
        localtime_r(&y, &ytm);
        if (ytm.tm_year == tm.tm_year && ytm.tm_yday == tm.tm_yday) {
            snprintf(out, n, _("Ayer %02d:%02d"), tm.tm_hour, tm.tm_min);
            return;
        }
    }
    if (full) snprintf(out, n, "%d %s %d, %02d:%02d", tm.tm_mday, aos_tr(MONTH[tm.tm_mon % 12]), tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    else snprintf(out, n, "%d %s %d", tm.tm_mday, aos_tr(MONTH[tm.tm_mon % 12]), tm.tm_year + 1900);
}

/* -------------------------------------------------------------------------- */
/* Kinds of file                                                               */
/* -------------------------------------------------------------------------- */

enum { K_DIR, K_IMAGE, K_AUDIO, K_TEXT, K_CSV, K_CODE, K_FW, K_APP, K_ICON, K_OTHER, K_COUNT };

static const struct { const char *glyph; uint32_t color; const char *name; } KIND[K_COUNT] = {
    [K_DIR]   = { AOS_SYM_FOLDER,                0x60A5FA, N_("Carpeta") },
    [K_IMAGE] = { AOS_SYM_FILE_IMAGE_OUTLINE,    0xFBBF24, N_("Imagen") },
    [K_AUDIO] = { AOS_SYM_FILE_MUSIC_OUTLINE,    0xFF375F, N_("Audio") },
    [K_TEXT]  = { AOS_SYM_FILE_DOCUMENT_OUTLINE, 0xD1D1D6, N_("Texto") },
    [K_CSV]   = { AOS_SYM_FILE_DELIMITED_OUTLINE, 0x30D158, N_("Tabla CSV") },
    [K_CODE]  = { AOS_SYM_FILE_CODE_OUTLINE,     0xBF5AF2, N_("Datos o configuración") },
    [K_FW]    = { AOS_SYM_CHIP,                  0xA78BFA, N_("Firmware") },
    [K_APP]   = { AOS_SYM_PACKAGE_VARIANT,       0xFF9F0A, N_("App de P4OS") },
    [K_ICON]  = { AOS_SYM_PALETTE,               0x40C8E0, N_("Ícono de app") },
    [K_OTHER] = { AOS_SYM_FILE_OUTLINE,          0x8E8E93, N_("Archivo") },
};

static int kind_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return K_OTHER;
    static const struct { const char *ext; int kind; } EXT[] = {
        { "jpg", K_IMAGE }, { "jpeg", K_IMAGE }, { "png", K_IMAGE }, { "bmp", K_IMAGE },
        { "mp3", K_AUDIO }, { "wav", K_AUDIO },
        { "txt", K_TEXT }, { "log", K_TEXT }, { "md", K_TEXT },
        { "csv", K_CSV }, { "tsv", K_CSV },
        { "json", K_CODE }, { "yaml", K_CODE }, { "yml", K_CODE }, { "ini", K_CODE }, { "cfg", K_CODE },
        { "conf", K_CODE }, { "toml", K_CODE }, { "xml", K_CODE }, { "html", K_CODE }, { "htm", K_CODE },
        { "css", K_CODE }, { "js", K_CODE }, { "c", K_CODE }, { "h", K_CODE }, { "cpp", K_CODE },
        { "py", K_CODE }, { "lua", K_CODE }, { "sh", K_CODE }, { "m3u", K_CODE }, { "pls", K_CODE },
        { "bin", K_FW }, { "elf", K_FW }, { "hex", K_FW }, { "uf2", K_FW },
        { "so", K_APP }, { "aic", K_ICON },
    };
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++)
        if (!strcasecmp(dot + 1, EXT[i].ext)) return EXT[i].kind;
    return K_OTHER;
}

static bool kind_is_text(int k) { return k == K_TEXT || k == K_CSV || k == K_CODE; }

/* -------------------------------------------------------------------------- */
/* The system's files: they can be touched, past a warning                     */
/* -------------------------------------------------------------------------- */

/* Why 'abs' matters to the system, or NULL. */
static const char *system_reason(const char *abs)
{
    const char *rel = rel_of(abs);
    if (!rel) return NULL;
    if (!strcmp(abs, aos_hal_path_menu())) return _("menu.txt ordena la pantalla de inicio.");
    if (!strcmp(rel, "modules.txt")) return _("modules.txt dice qué hay conectado al header.");
    if (!strcmp(rel, "ha.txt")) return _("ha.txt tiene los paneles de Home Assistant.");
    if (!strcmp(rel, "prefs.txt")) return _("prefs.txt guarda los ajustes del sistema.");
    if (path_under(abs, aos_hal_path_apps())) return _("En /apps están las apps instaladas.");
    if (path_under(rel, "firmware")) return _("En /firmware está lo que graba el Programador.");
    if (path_under(abs, aos_hal_path_data())) return _("En /data guardan su estado las apps.");
    if (path_under(abs, aos_hal_path_lang())) return _("En /lang están los idiomas.");
    if (path_under(abs, aos_hal_path_icons())) return _("En /icons están los íconos de las apps.");
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Jobs                                                                        */
/* -------------------------------------------------------------------------- */

static void *s_mx;
static void lk(void) { aos_hal_mutex_lock(s_mx); }
static void ulk(void) { aos_hal_mutex_unlock(s_mx); }

typedef struct job_s job_t;
struct job_s {
    int  refs;                  /* the screen's and the thread's, under the lock */
    volatile bool cancel;
    bool done;                  /* under the lock */
    void (*free_fn)(job_t *);
};

static void job_put(job_t *j)
{
    if (!j) return;
    lk();
    bool last = --j->refs == 0;
    ulk();
    if (last) j->free_fn(j);
}

static job_t *job_get(job_t *j)
{
    lk();
    j->refs++;
    ulk();
    return j;
}

/* The screen lets go of a job: it stops as soon as it looks. */
static void job_drop(job_t *j)
{
    if (!j) return;
    j->cancel = true;
    job_put(j);
}

static bool job_done(job_t *j)
{
    lk();
    bool d = j->done;
    ulk();
    return d;
}

static void job_finish(job_t *j)
{
    lk();
    j->done = true;
    ulk();
    job_put(j);                 /* the thread's reference */
}

/* Starts fn(j) on a thread of its own; the screen keeps a reference. */
static bool job_run(const char *name, void (*fn)(void *), job_t *j, uint32_t stack)
{
    j->refs = 2;
    if (!aos_hal_thread_start(name, fn, j, stack, 3)) {
        j->refs = 1;
        j->done = true;
        return false;
    }
    return true;
}

/* ---- a folder, or the results of a search --------------------------------- */

enum { EF_DIR = 1, EF_STATED = 2 };

typedef struct {
    uint32_t name;              /* name block << 16 | offset */
    uint32_t size;              /* bytes; a folder: what it holds, UINT32_MAX unknown */
    uint32_t mtime;
    uint8_t  kind, flags;
    uint16_t pad;
} ent_t;

typedef struct {
    job_t    job;
    bool     search;
    char     base[PATH_LEN];    /* the folder, absolute */
    char     query[64];         /* search: folded */
    ent_t   *ech[MAX_ENTRIES / ECHUNK];
    char    *nch[MAX_NCHUNKS];
    int      nnch;
    uint32_t nused;
    /* published, under the lock */
    int      n;                 /* entries with a name */
    int      stated;            /* [0, stated) have their size and date */
    bool     named;             /* the names are all in */
    bool     truncated;
    int      err;
    int      visited;
    uint32_t ms_names, ms_stat;
} listing_t;

static inline ent_t *E(const listing_t *l, int i) { return &l->ech[i >> ECHUNK_SHIFT][i & (ECHUNK - 1)]; }
static inline const char *NM(const listing_t *l, const ent_t *e) { return l->nch[e->name >> 16] + (e->name & 0xFFFF); }

static void ls_free(job_t *j)
{
    listing_t *l = (listing_t *)j;
    for (size_t i = 0; i < MAX_ENTRIES / ECHUNK; i++) free(l->ech[i]);
    for (int i = 0; i < l->nnch; i++) free(l->nch[i]);
    free(l);
}

/* The thread's side: a new entry i with this name. NULL when full. */
static ent_t *ls_add(listing_t *l, int i, const char *name)
{
    size_t len = strlen(name);
    if (i >= MAX_ENTRIES || len >= PATH_LEN) return NULL;
    int c = i >> ECHUNK_SHIFT;
    if (!l->ech[c] && !(l->ech[c] = malloc(ECHUNK * sizeof(ent_t)))) return NULL;
    if (!l->nnch || l->nused + len + 1 > NCHUNK_BYTES) {
        if (l->nnch >= MAX_NCHUNKS || !(l->nch[l->nnch] = malloc(NCHUNK_BYTES))) return NULL;
        l->nnch++;
        l->nused = 0;
    }
    memcpy(l->nch[l->nnch - 1] + l->nused, name, len + 1);
    ent_t *e = E(l, i);
    memset(e, 0, sizeof *e);
    e->name = (uint32_t)(l->nnch - 1) << 16 | l->nused;
    l->nused += (uint32_t)len + 1;
    return e;
}

static uint32_t count_items(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return UINT32_MAX;
    uint32_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static bool dirent_is_dir(const char *dir, const struct dirent *e)
{
    if (e->d_type == DT_DIR) return true;
    if (e->d_type != DT_UNKNOWN) return false;
    char p[PATH_LEN];
    return join(p, sizeof p, dir, e->d_name) && is_dir_path(p);
}

static void stat_into(ent_t *en, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        en->mtime = st.st_mtime > 0 ? (uint32_t)st.st_mtime : 0;
        if (!(en->flags & EF_DIR)) en->size = (uint32_t)st.st_size;
    }
    if (en->flags & EF_DIR) en->size = count_items(path);
    en->flags |= EF_STATED;
}

typedef struct { listing_t *l; int n; uint32_t last; } scan_ctx_t;

/* One entry of aos_hal_dir_scan: the name, and for files already the size
 * and the date (on the board they come from FatFs's directory records, so
 * the files need no stat() at all; only folders are counted afterwards). */
static bool list_entry(const aos_dir_entry_t *de, void *arg)
{
    scan_ctx_t *c = arg;
    listing_t *l = c->l;
    if (l->job.cancel) return false;
    if (de->name[0] == '.') return true;            /* hidden, and . .. */
    if (strlen(de->name) >= NAME_LEN) return true;
    ent_t *en = ls_add(l, c->n, de->name);
    if (!en) { l->truncated = true; return false; }
    en->flags = de->dir ? EF_DIR : EF_STATED;
    en->kind = de->dir ? K_DIR : (uint8_t)kind_of(de->name);
    en->size = de->dir ? UINT32_MAX : de->size;
    en->mtime = de->mtime;
    c->n++;
    if ((c->n & 63) == 0 && now_ms() - c->last > 30) {
        c->last = now_ms();
        lk(); l->n = c->n; ulk();
    }
    return true;
}

static void list_thread(void *arg)
{
    listing_t *l = arg;
    uint32_t t0 = now_ms();
    scan_ctx_t sc = { .l = l, .last = t0 };
    if (aos_hal_dir_scan(l->base, list_entry, &sc) < 0) {
        lk();
        l->err = errno ? errno : ENOENT;
        l->named = true;
        ulk();
        job_finish(&l->job);
        return;
    }
    int n = sc.n;
    uint32_t t1 = now_ms();
    lk();
    l->n = n;
    l->named = true;
    l->ms_names = t1 - t0;
    ulk();
    /* second pass: sizes and dates */
    char path[PATH_LEN];
    uint32_t last = t1;
    for (int i = 0; i < n && !l->job.cancel; i++) {
        ent_t *en = E(l, i);
        if (en->flags & EF_STATED) continue;       /* a file: the scan brought its size and date */
        if (join(path, sizeof path, l->base, NM(l, en))) stat_into(en, path);
        else en->flags |= EF_STATED;
        if ((i & 15) == 15 && now_ms() - last > 25) {
            last = now_ms();
            lk(); l->stated = i + 1; ulk();
        }
    }
    lk();
    if (!l->job.cancel) l->stated = n;
    l->ms_stat = now_ms() - t1;
    ulk();
    job_finish(&l->job);
}

/* Every folder under base, nearest first, for names holding the query. */
static void search_thread(void *arg)
{
    listing_t *l = arg;
    uint32_t t0 = now_ms();
    char **q = malloc(SEARCH_QUEUE * sizeof(char *));
    int qh = 0, qt = 0, n = 0, visited = 0;
    if (q) q[qt++] = strdup("");
    char dir[PATH_LEN], rel[PATH_LEN], full[PATH_LEN], f[NAME_LEN];
    while (q && qh < qt && !l->job.cancel && !l->truncated) {
        char *r = q[qh];
        q[qh++] = NULL;
        if (!r) continue;
        DIR *d = join(dir, sizeof dir, l->base, r) ? opendir(dir) : NULL;
        if (d) {
            struct dirent *e;
            while (!l->job.cancel && (e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                if (++visited > SEARCH_VISITS) { l->truncated = true; break; }
                if (!join(rel, sizeof rel, r, e->d_name)) continue;
                bool isdir = dirent_is_dir(dir, e);
                fold(f, sizeof f, e->d_name);
                if (strstr(f, l->query)) {
                    ent_t *en = ls_add(l, n, rel);
                    if (!en) { l->truncated = true; break; }
                    en->flags = isdir ? EF_DIR : 0;
                    en->kind = isdir ? K_DIR : (uint8_t)kind_of(e->d_name);
                    if (join(full, sizeof full, dir, e->d_name)) stat_into(en, full);
                    else en->flags |= EF_STATED;
                    n++;
                    lk(); l->n = l->stated = n; l->visited = visited; ulk();
                    if (n >= SEARCH_MAX) { l->truncated = true; break; }
                }
                if (isdir && rel_depth(rel) < WALK_DEPTH) {
                    if (qt < SEARCH_QUEUE) q[qt++] = strdup(rel);
                    else l->truncated = true;
                }
            }
            closedir(d);
        }
        free(r);
    }
    if (q) {
        for (int i = qh; i < qt; i++) free(q[i]);
        free(q);
    }
    lk();
    l->n = l->stated = n;
    l->visited = visited;
    l->named = true;
    l->ms_names = now_ms() - t0;
    ulk();
    job_finish(&l->job);
}

static listing_t *listing_start(const char *abs, const char *query)
{
    listing_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->job.free_fn = ls_free;
    scpy(l->base, sizeof l->base, abs);
    if (query) {
        l->search = true;
        fold(l->query, sizeof l->query, query);
    }
    if (!job_run(query ? "fs_find" : "fs_list", query ? search_thread : list_thread, &l->job, JOB_STACK)) {
        l->err = ENOMEM;
        l->named = true;
    }
    return l;
}

/* ---- sorting and the filter of the search ---------------------------------- */

enum { SORT_NAME, SORT_DATE, SORT_SIZE, SORT_KIND, SORT_COUNT };
static const char *const SORT_NAME_[SORT_COUNT] = { N_("Nombre"), N_("Fecha"), N_("Tamaño"), N_("Tipo") };
static const bool SORT_DESC_DEFAULT[SORT_COUNT] = { false, true, true, false };

typedef struct {
    job_t      job;
    listing_t *l;
    int        n, stated;
    int        mode;
    bool       desc;
    char       query[64];       /* folded; "" = everything */
    int32_t   *order;
    int        norder;
    uint32_t   us;
} sortjob_t;

static void sort_free(job_t *j)
{
    sortjob_t *s = (sortjob_t *)j;
    if (s->l) job_put(&s->l->job);
    free(s->order);
    free(s);
}

static const char *ext_of(const char *n)
{
    const char *d = strrchr(n, '.');
    return d ? d + 1 : "";
}

static int cmp_ent(const sortjob_t *s, int32_t ia, int32_t ib)
{
    const ent_t *a = E(s->l, ia), *b = E(s->l, ib);
    bool da = a->flags & EF_DIR, db = b->flags & EF_DIR;
    if (da != db) return da ? -1 : 1;           /* folders first */
    const char *na = base_of(NM(s->l, a)), *nb = base_of(NM(s->l, b));
    int r = 0;
    if (s->mode != SORT_NAME) {
        uint32_t va = 0, vb = 0;
        bool sa = ia < s->stated, sb = ib < s->stated;
        if (s->mode == SORT_DATE) { va = sa ? a->mtime : 0; vb = sb ? b->mtime : 0; }
        else if (s->mode == SORT_SIZE) { va = sa ? a->size : 0; vb = sb ? b->size : 0; }
        else { va = a->kind; vb = b->kind; }
        r = va < vb ? -1 : va > vb;
        if (!r && s->mode == SORT_KIND) r = strcasecmp(ext_of(na), ext_of(nb));
        if (s->desc) r = -r;
    }
    if (!r) {
        r = img_name_cmp(na, nb);
        if (s->mode == SORT_NAME && s->desc) r = -r;
    }
    return r;
}

/* Bottom-up merge sort: stable, and with a context, which qsort() has not. */
static void msort(const sortjob_t *s, int32_t *v, int n)
{
    int32_t *t = malloc((size_t)(n > 0 ? n : 1) * sizeof *t);
    if (!t) return;
    int32_t *src = v, *dst = t;
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) dst[k++] = cmp_ent(s, src[i], src[j]) <= 0 ? src[i++] : src[j++];
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        int32_t *x = src; src = dst; dst = x;
        if (s->job.cancel) break;
    }
    if (src != v) memcpy(v, src, (size_t)n * sizeof *v);
    free(t);
}

static void sort_thread(void *arg)
{
    sortjob_t *s = arg;
    uint32_t t0 = perf_us();
    s->order = malloc((size_t)(s->n > 0 ? s->n : 1) * sizeof(int32_t));
    int m = 0;
    if (s->order) {
        char f[NAME_LEN];
        for (int i = 0; i < s->n; i++) {
            if (s->query[0]) {
                fold(f, sizeof f, base_of(NM(s->l, E(s->l, i))));
                if (!strstr(f, s->query)) continue;
            }
            s->order[m++] = i;
        }
        msort(s, s->order, m);
    }
    s->norder = m;
    s->us = perf_us() - t0;
    job_finish(&s->job);
}

/* ---- a folder's size, for the information sheet ---------------------------- */

typedef struct {
    job_t    job;
    char   **paths;
    int      npaths;
    uint64_t bytes;             /* published under the lock */
    uint32_t files, dirs;
    char     p[PATH_LEN];
} dujob_t;

static void du_free(job_t *j)
{
    dujob_t *d = (dujob_t *)j;
    for (int i = 0; i < d->npaths; i++) free(d->paths[i]);
    free(d->paths);
    free(d);
}

static void du_rec(dujob_t *j, int depth)
{
    DIR *d = opendir(j->p);
    if (!d) return;
    size_t len = strlen(j->p);
    struct dirent *e;
    while (!j->job.cancel && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!join(j->p, sizeof j->p, j->p, e->d_name)) continue;
        struct stat st;
        if (stat(j->p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                lk(); j->dirs++; ulk();
                if (depth < WALK_DEPTH) du_rec(j, depth + 1);
            } else {
                lk(); j->files++; j->bytes += (uint64_t)st.st_size; ulk();
            }
        }
        j->p[len] = 0;
    }
    closedir(d);
}

static void du_thread(void *arg)
{
    dujob_t *j = arg;
    for (int i = 0; i < j->npaths && !j->job.cancel; i++) {
        struct stat st;
        if (stat(j->paths[i], &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scpy(j->p, sizeof j->p, j->paths[i]);
            if (j->npaths > 1) { lk(); j->dirs++; ulk(); }
            du_rec(j, 0);
        } else {
            lk(); j->files++; j->bytes += (uint64_t)st.st_size; ulk();
        }
    }
    job_finish(&j->job);
}

/* ---- the card's free space: f_getfree can take seconds on a big card ------- */

static struct {
    bool     busy, ok;
    uint32_t seq;
    uint64_t total, freeb;
} USG;

static void usage_thread(void *arg)
{
    (void)arg;
    uint64_t t = 0, f = 0;
    bool ok = aos_hal_sd_usage(&t, &f);
    lk();
    USG.total = t;
    USG.freeb = f;
    USG.ok = ok;
    USG.seq++;
    USG.busy = false;
    ulk();
}

static void usage_request(void)
{
    lk();
    bool go = !USG.busy;
    if (go) USG.busy = true;
    ulk();
    if (go && !aos_hal_thread_start("fs_usage", usage_thread, NULL, 6144, 2)) {
        lk(); USG.busy = false; ulk();
    }
}

/* ---- copy, move, duplicate, delete ----------------------------------------- */

enum { OP_COPY, OP_MOVE, OP_DUP, OP_DELETE };
enum { OPS_IDLE, OPS_PREP, OPS_RUN, OPS_DONE };

static struct {
    int      kind, state;       /* state under the lock */
    char   **src;
    int      nsrc;
    char     dst[PATH_LEN];     /* the folder things go to */
    volatile bool cancel;
    /* progress, under the lock */
    uint64_t bytes_total, bytes_done;
    uint32_t items_total, items_done;
    char     cur[NAME_LEN];
    int      errors, done_top;
    char     err[200];
    bool     cancelled;
    uint32_t seq;               /* bumps when a job ends */
    uint32_t t0, ms;
    /* the thread's own, on the heap while it runs */
    char    *a, *b;
    uint8_t *buf;
} OP;

static void op_error(const char *fmt, const char *what)
{
    lk();
    OP.errors++;
    if (!OP.err[0]) snprintf(OP.err, sizeof OP.err, fmt, what);
    ulk();
}

static void op_cur(const char *path)
{
    lk();
    scpy(OP.cur, sizeof OP.cur, base_of(path));
    ulk();
}

/* The names in a folder, read before anything in it changes (a delete in the
 * middle of a readdir() is asking for trouble). A block of NUL-separated
 * names, n of them. */
static char *names_of(const char *dir, int *n)
{
    *n = 0;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    size_t cap = 4096, used = 0;
    char *buf = malloc(cap);
    struct dirent *e;
    while (buf && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t l = strlen(e->d_name) + 1;
        if (used + l > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        memcpy(buf + used, e->d_name, l);
        used += l;
        (*n)++;
    }
    closedir(d);
    return buf;
}

/* OP.a (a file or a folder) into totals. */
static void prep_rec(int depth)
{
    struct stat st;
    if (stat(OP.a, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        lk(); OP.bytes_total += (uint64_t)st.st_size; OP.items_total++; ulk();
        return;
    }
    lk(); OP.items_total++; ulk();
    if (depth >= WALK_DEPTH || OP.cancel) return;
    int n;
    char *names = names_of(OP.a, &n);
    size_t len = strlen(OP.a);
    const char *p = names;
    for (int i = 0; i < n && !OP.cancel; i++, p += strlen(p) + 1) {
        if (join(OP.a, PATH_LEN, OP.a, p)) prep_rec(depth + 1);
        OP.a[len] = 0;
    }
    free(names);
}

static bool copy_file(const char *a, const char *b, const struct stat *st)
{
    op_cur(a);
    FILE *in = fopen(a, "rb");
    if (!in) { op_error(_("No se pudo leer %s"), base_of(a)); return false; }
    FILE *out = fopen(b, "wb");
    if (!out) { fclose(in); op_error(_("No se pudo crear %s"), base_of(b)); return false; }
    /* No stdio buffers in the way: each 32 KB goes straight between the card
     * and OP.buf, which the DMA takes as it is (aos_hal_io_alloc). Through
     * stdio's 8 KB the copy went 2.5 MB/s on the board, reading at 4.3
     * (2026-09-29). Reading is read() on the descriptor: an unbuffered
     * fread() in newlib goes a byte at a time. Writing unbuffered is fine. */
    setvbuf(out, NULL, _IONBF, 0);
    int in_fd = fileno(in);
    bool ok = true;
    size_t r;
    uint64_t t0 = aos_hal_uptime_ms(), t_read = 0, t_write = 0, total = 0;
    for (;;) {
        uint64_t t = aos_hal_uptime_ms();
        ssize_t got = read(in_fd, OP.buf, COPY_BUF);
        t_read += aos_hal_uptime_ms() - t;
        if (got < 0) { op_error(_("Error al leer %s"), base_of(a)); ok = false; break; }
        r = (size_t)got;
        if (r == 0) break;
        t = aos_hal_uptime_ms();
        size_t w = fwrite(OP.buf, 1, r, out);
        t_write += aos_hal_uptime_ms() - t;
        if (w != r) {
            op_error(_("No entra %s: la tarjeta está llena"), base_of(a));
            ok = false;
            break;
        }
        total += r;
        lk(); OP.bytes_done += r; ulk();
        if (OP.cancel) { ok = false; break; }
    }
    fclose(in);
    uint64_t t = aos_hal_uptime_ms();
    if (fclose(out) != 0 && ok) { op_error(_("Error al escribir %s"), base_of(b)); ok = false; }
    t_write += aos_hal_uptime_ms() - t;
    uint64_t ms = aos_hal_uptime_ms() - t0;
    if (total >= 1024 * 1024)       /* what the card gives, for the big ones */
        aos_hal_log("files", "copied %s: %llu KB in %llu ms (%llu KB/s; reading %llu ms, writing %llu ms)",
                    base_of(a), (unsigned long long)(total / 1024), (unsigned long long)ms,
                    (unsigned long long)(ms ? total / ms * 1000 / 1024 : 0),
                    (unsigned long long)t_read, (unsigned long long)t_write);
    if (!ok) remove(b);
    else if (st->st_mtime > 0) {
        struct utimbuf u = { .actime = st->st_mtime, .modtime = st->st_mtime };
        utime(b, &u);           /* keep the date; FAT keeps only the modification */
    }
    lk(); OP.items_done++; ulk();
    return ok;
}

/* OP.a into OP.b, a folder with everything in it. */
static bool copy_rec(int depth)
{
    struct stat st;
    if (stat(OP.a, &st) != 0) { op_error(_("No se encontró %s"), base_of(OP.a)); return false; }
    if (!S_ISDIR(st.st_mode)) return copy_file(OP.a, OP.b, &st);
    op_cur(OP.a);
    if (mkdir(OP.b, 0775) != 0 && errno != EEXIST) { op_error(_("No se pudo crear la carpeta %s"), base_of(OP.b)); return false; }
    lk(); OP.items_done++; ulk();
    if (depth >= WALK_DEPTH) return true;
    int n;
    char *names = names_of(OP.a, &n);
    size_t la = strlen(OP.a), lb = strlen(OP.b);
    const char *p = names;
    bool ok = true;
    for (int i = 0; i < n && !OP.cancel; i++, p += strlen(p) + 1) {
        if (join(OP.a, PATH_LEN, OP.a, p) && join(OP.b, PATH_LEN, OP.b, p)) ok &= copy_rec(depth + 1);
        else { op_error(_("Ruta demasiado larga: %s"), p); ok = false; }
        OP.a[la] = 0;
        OP.b[lb] = 0;
    }
    free(names);
    return ok && !OP.cancel;
}

static bool delete_rec(int depth)
{
    struct stat st;
    if (stat(OP.a, &st) != 0) return true;              /* already gone */
    op_cur(OP.a);
    if (S_ISDIR(st.st_mode)) {
        if (depth < WALK_DEPTH) {
            int n;
            char *names = names_of(OP.a, &n);
            size_t la = strlen(OP.a);
            const char *p = names;
            for (int i = 0; i < n && !OP.cancel; i++, p += strlen(p) + 1) {
                if (join(OP.a, PATH_LEN, OP.a, p)) delete_rec(depth + 1);
                OP.a[la] = 0;
            }
            free(names);
        }
        if (OP.cancel) return false;
        if (rmdir(OP.a) != 0) { op_error(_("No se pudo borrar la carpeta %s"), base_of(OP.a)); return false; }
    } else if (remove(OP.a) != 0) {
        op_error(_("No se pudo borrar %s"), base_of(OP.a));
        return false;
    }
    lk(); OP.items_done++; ulk();
    return true;
}

static void op_thread(void *arg)
{
    (void)arg;
    uint32_t t0 = now_ms();
    OP.a = malloc(PATH_LEN);
    OP.b = malloc(PATH_LEN);
    if (!OP.a || !OP.b) { op_error("%s", _("No hay memoria.")); OP.cancel = true; }
    /* what there is to do */
    for (int i = 0; i < OP.nsrc && !OP.cancel; i++) {
        if (OP.kind == OP_MOVE) { lk(); OP.items_total++; ulk(); continue; }
        /* (OP.a is there: a failed malloc cancelled the job above) */
        scpy(OP.a, PATH_LEN, OP.src[i]);
        prep_rec(0);
    }
    bool copying = OP.kind == OP_COPY || OP.kind == OP_DUP;
    if (copying && !OP.cancel) {
        uint64_t t, f;
        if (aos_hal_sd_usage(&t, &f) && f < OP.bytes_total + 64 * 1024) {
            char need[24], have[24], msg[96];
            fmt_size(need, sizeof need, OP.bytes_total);
            fmt_size(have, sizeof have, f);
            snprintf(msg, sizeof msg, _("Hace falta %s y quedan %s libres."), need, have);
            op_error("%s", msg);
            OP.cancel = true;
        }
    }
    lk(); OP.state = OPS_RUN; ulk();
    if (copying) OP.buf = aos_hal_io_alloc(COPY_BUF);    /* the card DMAs it directly */
    if (copying && !OP.buf) { op_error("%s", _("No hay memoria para copiar.")); OP.cancel = true; }
    for (int i = 0; i < OP.nsrc && !OP.cancel; i++) {
        const char *src = OP.src[i];
        bool dir = is_dir_path(src);
        char name[NAME_LEN];
        bool ok = false;
        switch (OP.kind) {
        case OP_COPY:
        case OP_DUP:
            if (dir && path_under(OP.dst, src)) { op_error(_("No se puede copiar %s dentro de sí misma."), base_of(src)); break; }
            if (!unique_name(OP.dst, base_of(src), dir, name, sizeof name) || !join(OP.b, PATH_LEN, OP.dst, name)) {
                op_error(_("No hay un nombre libre para %s"), base_of(src));
                break;
            }
            scpy(OP.a, PATH_LEN, src);
            ok = copy_rec(0);
            if (!ok && OP.cancel) {
                /* the half-copied folder goes too */
                scpy(OP.a, PATH_LEN, OP.b);
                bool c = OP.cancel;
                OP.cancel = false;
                if (is_dir_path(OP.a)) delete_rec(0);
                OP.cancel = c;
            }
            break;
        case OP_MOVE: {
            op_cur(src);
            char parent[PATH_LEN];
            scpy(parent, sizeof parent, src);
            char *s = strrchr(parent, '/');
            if (s) *s = 0;
            if (!strcmp(parent, OP.dst)) { ok = true; break; }  /* already there */
            if (dir && path_under(OP.dst, src)) { op_error(_("No se puede mover %s dentro de sí misma."), base_of(src)); break; }
            if (!unique_name(OP.dst, base_of(src), dir, name, sizeof name) || !join(OP.b, PATH_LEN, OP.dst, name)) {
                op_error(_("No hay un nombre libre para %s"), base_of(src));
                break;
            }
            ok = rename(src, OP.b) == 0;
            if (!ok) op_error(_("No se pudo mover %s"), base_of(src));
            lk(); OP.items_done++; ulk();
            break;
        }
        case OP_DELETE:
            scpy(OP.a, PATH_LEN, src);
            ok = delete_rec(0);
            break;
        }
        if (ok) { lk(); OP.done_top++; ulk(); }
    }
    aos_hal_io_free(OP.buf);
    free(OP.a);
    free(OP.b);
    OP.buf = NULL;
    OP.a = OP.b = NULL;
    lk();
    OP.cancelled = OP.cancel;
    OP.ms = now_ms() - t0;
    OP.state = OPS_DONE;
    OP.seq++;
    ulk();
}

static bool op_busy(void)
{
    lk();
    bool b = OP.state == OPS_PREP || OP.state == OPS_RUN;
    ulk();
    return b;
}

/* Takes 'src' (strings the job frees). */
static bool op_start(int kind, char **src, int nsrc, const char *dst)
{
    if (op_busy()) return false;
    for (int i = 0; i < OP.nsrc; i++) free(OP.src[i]);
    free(OP.src);
    lk();
    OP.kind = kind;
    OP.src = src;
    OP.nsrc = nsrc;
    scpy(OP.dst, sizeof OP.dst, dst ? dst : "");
    OP.cancel = false;
    OP.bytes_total = OP.bytes_done = 0;
    OP.items_total = OP.items_done = 0;
    OP.cur[0] = OP.err[0] = 0;
    OP.errors = OP.done_top = 0;
    OP.cancelled = false;
    OP.t0 = now_ms();
    OP.state = OPS_PREP;
    ulk();
    if (!aos_hal_thread_start("fs_op", op_thread, NULL, JOB_STACK, 3)) {
        lk(); OP.state = OPS_IDLE; ulk();
        return false;
    }
    return true;
}

/* ---- the viewer's index: where each row starts ----------------------------- */

#define IX_SHIFT     14
#define IX_CHUNK     (1 << IX_SHIFT)            /* 64 KB of offsets */
#define IX_CHUNKS    64                         /* a million rows */
#define VBLK         (32 * 1024)

enum { IX_WRAP, IX_LINES };

typedef struct {
    job_t     job;
    char      path[PATH_LEN];
    int       mode, cols;
    uint32_t  size;
    uint32_t *ch[IX_CHUNKS];
    /* published under the lock */
    int       n;                /* rows whose start is known */
    uint32_t  pos;              /* bytes read */
    uint32_t  lines, maxcols;
    bool      truncated;
} index_t;

static inline uint32_t IX(const index_t *x, int k) { return x->ch[k >> IX_SHIFT][k & (IX_CHUNK - 1)]; }

static void ix_free(job_t *j)
{
    index_t *x = (index_t *)j;
    for (int i = 0; i < IX_CHUNKS; i++) free(x->ch[i]);
    free(x);
}

static bool ix_push(index_t *x, int *n, uint32_t off)
{
    int c = *n >> IX_SHIFT;
    if (c >= IX_CHUNKS) { x->truncated = true; return false; }
    if (!x->ch[c] && !(x->ch[c] = malloc(IX_CHUNK * sizeof(uint32_t)))) { x->truncated = true; return false; }
    x->ch[c][*n & (IX_CHUNK - 1)] = off;
    (*n)++;
    return true;
}

static void ix_thread(void *arg)
{
    index_t *x = arg;
    FILE *f = fopen(x->path, "rb");
    uint8_t *buf = malloc(VBLK);
    int n = 0;
    uint32_t lines = 0, maxc = 0, col = 0, off = 0;
    bool room = true;
    if (f && buf && x->size) room = ix_push(x, &n, 0);
    uint32_t last = now_ms();
    while (f && buf && room && off < x->size && !x->job.cancel) {
        size_t want = x->size - off < VBLK ? x->size - off : VBLK;
        size_t r = fread(buf, 1, want, f);
        if (!r) break;
        for (size_t i = 0; i < r && room; i++) {
            uint8_t c = buf[i];
            uint32_t at = off + (uint32_t)i;
            if (c == '\n') {
                lines++;
                if (col > maxc) maxc = col;
                col = 0;
                if (at + 1 < x->size) room = ix_push(x, &n, at + 1);
            } else if (c != '\r' && (c & 0xC0) != 0x80) {
                uint32_t w = c == '\t' ? 4 : 1;
                if (x->mode == IX_WRAP && col + w > (uint32_t)x->cols && col > 0) {
                    room = ix_push(x, &n, at);
                    col = 0;
                }
                col += w;
            }
        }
        off += (uint32_t)r;
        if (now_ms() - last > 40) {
            last = now_ms();
            lk(); x->n = n; x->pos = off; x->lines = lines; x->maxcols = maxc; ulk();
        }
    }
    if (col > maxc) maxc = col;
    if (col) lines++;
    if (f) fclose(f);
    free(buf);
    lk();
    x->n = n;
    x->pos = off;
    x->lines = lines;
    x->maxcols = maxc;
    ulk();
    job_finish(&x->job);
}

static index_t *index_start(const char *path, uint32_t size, int mode, int cols)
{
    index_t *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->job.free_fn = ix_free;
    scpy(x->path, sizeof x->path, path);
    x->size = size;
    x->mode = mode;
    x->cols = cols < 8 ? 8 : cols;
    job_run("fs_index", ix_thread, &x->job, 6144);
    return x;
}

/* ---- the viewer's chart: one column of a CSV, the whole file --------------- */

#define CSV_MAXC     24
#define CHART_PTS    400
#define CHART_MAXV   (1u << 21)

typedef struct {
    job_t   job;
    char    path[PATH_LEN];
    int     col, xcol;
    char    delim;
    /* results, read once done */
    float   pts[CHART_PTS];
    int     npts, count;
    double  vmin, vmax, vsum;
    double  x0, x1;
    bool    has_x;
} chart_t;

static void chart_free(job_t *j) { free(j); }

/* Splits one CSV line in place; returns the number of cells. Quotes are
 * honoured ("a,b" is one cell) and dropped. */
static int csv_split(char *line, char delim, char **cell, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        char *start = p, *w = p;
        bool q = false;
        if (*p == '"') { q = true; p++; start = w = p; }
        while (*p) {
            if (q) {
                if (*p == '"' && p[1] == '"') { *w++ = '"'; p += 2; continue; }
                if (*p == '"') { q = false; p++; continue; }
            } else if (*p == delim) break;
            *w++ = *p++;
        }
        bool more = *p == delim;
        *w = 0;
        cell[n++] = start;
        if (!more) break;
        p++;
    }
    return n;
}

static bool parse_num(const char *s, double *out)
{
    while (*s == ' ') s++;
    if (!*s) return false;
    char t[48];
    scpy(t, sizeof t, s);
    for (char *c = t; *c; c++) if (*c == ',') *c = '.';
    char *end;
    double v = strtod(t, &end);
    while (*end == ' ') end++;
    if (*end || end == t || isnan(v) || isinf(v)) return false;
    *out = v;
    return true;
}

static void chart_thread(void *arg)
{
    chart_t *c = arg;
    FILE *f = fopen(c->path, "rb");
    float *v = malloc(65536 * sizeof(float));
    uint32_t cap = 65536, nv = 0;
    char *line = malloc(4096);
    bool first = true;
    double x0 = 0, x1 = 0;
    bool hx = false;
    while (f && v && line && !c->job.cancel && fgets(line, 4096, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (first) { first = false; continue; }         /* the header */
        char *cell[CSV_MAXC];
        int nc = csv_split(line, c->delim, cell, CSV_MAXC);
        double d;
        if (c->col >= nc || !parse_num(cell[c->col], &d)) continue;
        if (nv == cap) {
            if (cap >= CHART_MAXV) break;
            float *nvp = realloc(v, cap * 2 * sizeof(float));
            if (!nvp) break;
            v = nvp;
            cap *= 2;
        }
        v[nv++] = (float)d;
        double xv;
        if (c->xcol >= 0 && c->xcol < nc && parse_num(cell[c->xcol], &xv)) {
            if (!hx) { x0 = xv; hx = true; }
            x1 = xv;
        }
    }
    if (f) fclose(f);
    free(line);
    double mn = INFINITY, mx = -INFINITY, sum = 0;
    for (uint32_t i = 0; i < nv; i++) { mn = fmin(mn, v[i]); mx = fmax(mx, v[i]); sum += v[i]; }
    int np = nv < CHART_PTS ? (int)nv : CHART_PTS;
    for (int i = 0; i < np; i++) {
        uint32_t a = (uint32_t)((uint64_t)i * nv / (uint32_t)np), b = (uint32_t)((uint64_t)(i + 1) * nv / (uint32_t)np);
        if (b <= a) b = a + 1;
        double s = 0;
        for (uint32_t k = a; k < b; k++) s += v[k];
        c->pts[i] = (float)(s / (double)(b - a));
    }
    free(v);
    c->npts = np;
    c->count = (int)nv;
    c->vmin = mn;
    c->vmax = mx;
    c->vsum = sum;
    c->x0 = x0;
    c->x1 = x1;
    c->has_x = hx;
    job_finish(&c->job);
}

/* -------------------------------------------------------------------------- */
/* The screen's state                                                          */
/* -------------------------------------------------------------------------- */

/* What survives closing the app and turning the screen. */
static struct {
    bool     init;
    char     rel[PATH_LEN];     /* the folder on show, under the card ("" = the card) */
    int      sort;
    bool     desc, grid;
    char     fav[MAX_FAV][128];
    int      nfav;
    int32_t  scroll[WALK_DEPTH + 2];
    char   **clip;              /* absolute paths */
    int      nclip;
    bool     clip_cut;
    uint32_t op_seen;           /* the last OP.seq told */
    bool     op_hidden;         /* the progress sheet sent to a pill */
} S;

typedef struct {
    lv_obj_t *obj, *icon, *img, *name, *sub, *chev, *check, *tick, *sep;
    int p;                      /* position in the order, -1 none */
    int ei;                     /* the entry it shows */
} cell_t;

#define POOL_MAX    64
#define SIDE_W      340
#define GRID_H      214

typedef struct { const char *glyph; const char *text; lv_color_t color; void (*fn)(void); } act_t;

static struct {
    aos_app_t *self;
    lv_obj_t *root;
    int32_t   W, H;
    bool      land;
    lv_obj_t *main, *side;
    lv_obj_t *title, *sel_all;
    lv_obj_t *search, *ta, *clear, *rec, *sort_lbl, *kb;
    lv_obj_t *list, *spacer, *footer, *empty, *empty_g, *empty_t;
    lv_obj_t *bottom, *bar_btn[5];
    int32_t   bottom_h;
    lv_obj_t *pill, *pill_bar, *pill_lbl;
    lv_obj_t *overlay;
    lv_obj_t *entry_ta, *entry_kb, *entry_err;
    lv_obj_t *op_title, *op_cur, *op_bar, *op_detail;
    lv_obj_t *info_size, *info_items;
    cell_t   *cells;            /* POOL_MAX, while the app is open */
    int       ncell, cols;
    int32_t   rh, cw, gx, list_w, list_h;
    lv_timer_t *timer;
    bool      select;
    uint32_t  tick;
    act_t     acts[16];
    int       nacts;
    int       ctx;              /* the entry of the long press, -1 */
    void    (*confirm_fn)(void);
    void    (*entry_done)(const char *);
    dujob_t  *du;
    bool      viewing;
} U;

/* The list on show. */
static struct {
    listing_t *l;               /* the folder, or a search's results */
    listing_t *nl;              /* the folder read again, shown once sorted */
    listing_t *folder;          /* the folder's own while a search shows */
    sortjob_t *sj;
    int32_t   *order;
    int        norder;
    int        sort_n;
    bool       sort_final;
    uint32_t   sort_ms;
    int        seen_stated, seen_n;
    bool       seen_named;
    uint8_t   *sel;             /* per entry of l */
    int        sel_cap, nsel;
    char       query[64], fq[64];
    bool       recursive, query_dirty;
    uint32_t   query_ms;
    uint32_t   gen;
    uint32_t   t_req;
    bool       logged;
    int32_t    want_scroll;     /* -1 none */
    char       reveal[NAME_LEN]; /* scroll to this name when it shows up */
} L = { .want_scroll = -1 };

/* Thumbnails of the photos, for the grid. */
typedef struct {
    int ei;                     /* -1 free */
    uint32_t gen, used;
    img_result_t r;
    lv_image_dsc_t dsc;
} thumb_t;

static thumb_t *TH;              /* TH_N of them, allocated once */
static uint8_t *TS;             /* per entry: 0 not asked, 1 asked, 2 in TH, 3 failed */
static int TS_cap, TS_asked;

static void build(void);
static void pool_refresh(bool force);
static void footer_refresh(void);
static void select_refresh(void);
static void list_layout(void);
static void relist(void);
static void go_to(const char *rel);
static void viewer_open(const char *path, int kind);
static void viewer_close(void);
static void viewer_build(void);
static bool viewer_back(void);
static void viewer_poll(void);
static void info_open(void);
static void op_sheet(void);

/* -------------------------------------------------------------------------- */
/* Widgets                                                                     */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *glyph(lv_obj_t *parent, const char *sym, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, sym, font, color);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, s, font, color);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void tap(lv_obj_t *o, lv_event_cb_t cb, void *ud)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, ud);
}

/* A round button with a glyph, the phone's toolbar kind. */
static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, const lv_font_t *font, lv_color_t fg, lv_color_t bg,
                           int32_t size, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    tap(b, cb, ud);
    lv_obj_center(glyph(b, sym, font, fg));
    return b;
}

/* A text button in the nav bar: accent text, dimmed while pressed. */
static lv_obj_t *link_btn(lv_obj_t *parent, const char *sym, const char *s, const lv_font_t *font,
                          lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 80);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_style_pad_column(b, 4, 0);
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    tap(b, cb, ud);
    if (sym) glyph(b, sym, &aos_sym_44, C_TINT);
    if (s) {
        lv_obj_t *l = text(b, s, font, C_TINT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(l, U.land ? 300 : 330, 0);
    }
    return b;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *s, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? C_TINT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    tap(c, cb, ud);
    lv_obj_center(text(c, s, aos_font_small, AOS_C_TEXT));
    return c;
}

static void chip_set(lv_obj_t *c, bool on)
{
    lv_obj_set_style_bg_color(c, on ? C_TINT : AOS_C_CARD2, 0);
}

static lv_obj_t *pill(lv_obj_t *parent, const char *sym, const char *s, lv_color_t bg, lv_color_t fg,
                      lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    tap(b, cb, ud);
    if (sym) glyph(b, sym, &aos_sym_28, fg);
    if (s) text(b, s, aos_font_body, fg);
    return b;
}

static lv_obj_t *hairline(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *l = box(parent, w, 1);
    lv_obj_set_style_bg_color(l, C_SEP, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    return l;
}

static int32_t utf8_chars(const char *s)
{
    int32_t n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static int32_t text_w(const char *s, const lv_font_t *f)
{
    lv_point_t p;
    lv_text_get_size(&p, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

/* A file name made fit to draw (a Mac's decomposed accents composed). */
static void disp(char *out, size_t n, const char *name) { img_text(out, n, name); }

/* The glyph and colour of an entry; the card's own folders get theirs. */
static void icon_for(const listing_t *l, const ent_t *e, const char **g, uint32_t *c)
{
    *g = KIND[e->kind].glyph;
    *c = KIND[e->kind].color;
    if (!(e->flags & EF_DIR)) return;
    char p[PATH_LEN];
    if (!join(p, sizeof p, l->base, NM(l, e))) return;
    if (!strcmp(p, aos_hal_path_music())) *g = AOS_SYM_FOLDER_MUSIC;
}

/* -------------------------------------------------------------------------- */
/* Overlays: sheets, dialogs, the text entry                                   */
/* -------------------------------------------------------------------------- */

static void overlay_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
    U.entry_ta = U.entry_kb = U.entry_err = NULL;
    U.op_title = U.op_cur = U.op_bar = U.op_detail = NULL;
    U.info_size = U.info_items = NULL;
    if (U.du) { job_drop(&U.du->job); U.du = NULL; }
}

static void overlay_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) overlay_close();
}

/* A dimmed layer over the whole app; a tap on the dim closes it. */
static lv_obj_t *overlay_open(bool tap_closes)
{
    overlay_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_60, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    if (tap_closes) lv_obj_add_event_cb(U.overlay, overlay_bg_cb, LV_EVENT_CLICKED, NULL);
    return U.overlay;
}

/* A card: at the bottom in portrait, as the phone's action sheet; centred in
 * landscape. */
static lv_obj_t *sheet_card(int32_t w)
{
    lv_obj_t *c = box(U.overlay, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, C_SHEET, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);      /* not through to the dim */
    return c;
}

static void sheet_act_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    void (*fn)(void) = i >= 0 && i < U.nacts ? U.acts[i].fn : NULL;
    overlay_close();
    if (fn) fn();
}

static void sheet_cancel_cb(lv_event_t *e) { (void)e; overlay_close(); U.ctx = -1; }

static lv_obj_t *sheet_row(lv_obj_t *card, int32_t w, const char *sym, const char *s, lv_color_t color, int act)
{
    lv_obj_t *r = box(card, w, 88);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, sheet_act_cb, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    lv_obj_t *l = text(r, s, aos_font_body, color);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 28, 0);
    if (sym) lv_obj_align(glyph(r, sym, &aos_sym_44, color), LV_ALIGN_RIGHT_MID, -24, 0);
    return r;
}

/* The action sheet: a header (glyph, title, subtitle) and a row per action,
 * then Cancelar on its own, as on the phone. */
static void sheet_open(const char *sym, uint32_t sym_color, const char *title, const char *sub,
                       const act_t *acts, int n)
{
    overlay_open(true);
    U.nacts = n < 16 ? n : 16;
    memcpy(U.acts, acts, (size_t)U.nacts * sizeof *acts);
    int32_t w = U.land ? 600 : U.W - 32;
    lv_obj_t *col = box(U.overlay, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 14, 0);
    /* taller than the screen (landscape, a long menu): it scrolls */
    lv_obj_set_style_max_height(col, U.H - 32, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *c = sheet_card(w);
    lv_obj_set_parent(c, col);
    if (title) {
        lv_obj_t *h = box(c, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(h, 22, 0);
        lv_obj_set_style_pad_left(h, sym ? 110 : 28, 0);
        lv_obj_set_style_min_height(h, 110, 0);
        if (sym) lv_obj_align(glyph(h, sym, &aos_sym_72, lv_color_hex(sym_color)), LV_ALIGN_LEFT_MID, -86, 0);
        lv_obj_t *tl = text(h, title, aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(tl, w - (sym ? 140 : 56));
        lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, sub ? 4 : 18);
        if (sub) {
            lv_obj_t *sl = text(h, sub, aos_font_caption, AOS_C_DIM);
            lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(sl, w - (sym ? 140 : 56));
            lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 0, 46);
        }
        hairline(c, w);
    }
    for (int i = 0; i < U.nacts; i++) {
        if (i) hairline(c, w);
        sheet_row(c, w, acts[i].glyph, acts[i].text, acts[i].color, i);
    }
    lv_obj_t *cc = sheet_card(w);
    lv_obj_set_parent(cc, col);
    lv_obj_t *cr = box(cc, w, 88);
    lv_obj_set_style_bg_color(cr, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cr, LV_OPA_COVER, LV_STATE_PRESSED);
    tap(cr, sheet_cancel_cb, NULL);
    lv_obj_center(text(cr, _("Cancelar"), aos_font_body, C_TINT));
    if (U.land) lv_obj_center(col);
    else lv_obj_align(col, LV_ALIGN_BOTTOM_MID, 0, -16);
}

/* ---- a question with a dangerous answer ---- */

static void confirm_ok_cb(lv_event_t *e)
{
    (void)e;
    void (*fn)(void) = U.confirm_fn;
    overlay_close();
    if (fn) fn();
}

static void confirm(const char *title, const char *msg, const char *warn, const char *ok, bool danger, void (*fn)(void))
{
    overlay_open(true);
    U.confirm_fn = fn;
    int32_t w = U.land ? 620 : U.W - 80;
    lv_obj_t *c = sheet_card(w);
    lv_obj_set_style_pad_all(c, 30, 0);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_center(c);
    lv_obj_t *t = text(c, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, w - 60);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    if (msg) {
        lv_obj_t *m = text(c, msg, aos_font_body, AOS_C_DIM);
        lv_obj_set_width(m, w - 60);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
    }
    if (warn) {
        lv_obj_t *wr = box(c, w - 60, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(wr, lv_color_hex(0x3A2A10), 0);
        lv_obj_set_style_bg_opa(wr, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(wr, 18, 0);
        lv_obj_set_style_pad_all(wr, 18, 0);
        lv_obj_set_style_pad_left(wr, 72, 0);
        lv_obj_align(glyph(wr, AOS_SYM_ALERT_OUTLINE, &aos_sym_44, AOS_C_ORANGE), LV_ALIGN_LEFT_MID, -58, 0);
        lv_obj_t *wl = text(wr, warn, aos_font_small, AOS_C_ORANGE);
        lv_obj_set_width(wl, w - 60 - 90);
        lv_label_set_long_mode(wl, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_t *row = box(c, w - 60, 88);
    lv_obj_set_style_pad_top(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    pill(row, NULL, _("Cancelar"), AOS_C_CARD2, AOS_C_TEXT, sheet_cancel_cb, NULL);
    pill(row, NULL, ok, danger ? AOS_C_RED : C_TINT, lv_color_white(), confirm_ok_cb, NULL);
}

/* ---- a name, typed (copied from Modbus's text field) ---- */

static void entry_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    if (c == LV_EVENT_CANCEL) { overlay_close(); return; }
    char v[NAME_LEN];
    scpy(v, sizeof v, lv_textarea_get_text(U.entry_ta));
    /* trim the ends: a trailing space is easy to type and FAT refuses it */
    size_t n = strlen(v);
    while (n && v[n - 1] == ' ') v[--n] = 0;
    char *s = v;
    while (*s == ' ') s++;
    const char *bad = name_problem(s);
    if (bad) {
        lv_label_set_text(U.entry_err, bad);
        return;
    }
    void (*done)(const char *) = U.entry_done;
    char keep[NAME_LEN];
    scpy(keep, sizeof keep, s);
    overlay_close();
    if (done) done(keep);
}

static void entry_btn_cb(lv_event_t *e)
{
    bool ok = (intptr_t)lv_event_get_user_data(e) != 0;
    if (!ok) { overlay_close(); return; }
    lv_obj_send_event(U.entry_kb, LV_EVENT_READY, NULL);
}

static void text_entry(const char *title, const char *value, const char *ok, void (*done)(const char *))
{
    overlay_open(false);
    U.entry_done = done;
    lv_obj_set_style_bg_color(U.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_t *head = box(U.overlay, U.W, 88);
    lv_obj_t *cancel = link_btn(head, NULL, _("Cancelar"), aos_font_body, entry_btn_cb, (void *)0);
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *t = text(head, title, aos_font_body, AOS_C_TEXT);
    lv_obj_center(t);
    lv_obj_t *okb = link_btn(head, NULL, ok, aos_font_body, entry_btn_cb, (void *)1);
    lv_obj_align(okb, LV_ALIGN_RIGHT_MID, -12, 0);
    U.entry_ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.entry_ta, true);
    lv_textarea_set_text(U.entry_ta, value);
    lv_obj_set_size(U.entry_ta, U.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(U.entry_ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 104);
    lv_obj_set_style_text_font(U.entry_ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.entry_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.entry_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(U.entry_ta, 0, 0);
    lv_obj_set_style_radius(U.entry_ta, 20, 0);
    lv_obj_set_style_pad_hor(U.entry_ta, 24, 0);
    lv_obj_set_style_pad_ver(U.entry_ta, 22, 0);
    lv_obj_add_state(U.entry_ta, LV_STATE_FOCUSED);
    /* the cursor before the extension, as the phone does */
    const char *dot = strrchr(value, '.');
    if (dot && dot != value) lv_textarea_set_cursor_pos(U.entry_ta, utf8_chars(value) - utf8_chars(dot));
    U.entry_err = text(U.overlay, "", aos_font_small, AOS_C_ORANGE);
    lv_obj_set_width(U.entry_err, U.W - 2 * AOS_UI_PAD);
    lv_label_set_long_mode(U.entry_err, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(U.entry_err, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + 8, 206);
    U.entry_kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.entry_kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(U.entry_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.entry_kb, aos_font_body);
    lv_keyboard_set_textarea(U.entry_kb, U.entry_ta);
    lv_obj_add_event_cb(U.entry_kb, entry_cb, LV_EVENT_ALL, NULL);
}

/* -------------------------------------------------------------------------- */
/* Preferences                                                                 */
/* -------------------------------------------------------------------------- */

static void prefs_load(void)
{
    int32_t v;
    if (aos_hal_pref_get_i32("fs_view", &v)) {
        S.sort = (v & 15) < SORT_COUNT ? (v & 15) : SORT_NAME;
        S.desc = (v >> 4) & 1;
        S.grid = (v >> 5) & 1;
    }
    char fav[MAX_FAV * 130];
    S.nfav = 0;
    if (aos_hal_pref_get_str("fs_fav", fav, sizeof fav)) {
        char *save = NULL;
        for (char *t = strtok_r(fav, "|", &save); t && S.nfav < MAX_FAV; t = strtok_r(NULL, "|", &save))
            scpy(S.fav[S.nfav++], sizeof S.fav[0], t);
    }
}

static void prefs_save(void)
{
    aos_hal_pref_set_i32("fs_view", S.sort | (S.desc ? 16 : 0) | (S.grid ? 32 : 0));
    char fav[MAX_FAV * 130] = "";
    for (int i = 0; i < S.nfav; i++) {
        if (i) strcat(fav, "|");
        strcat(fav, S.fav[i]);
    }
    aos_hal_pref_set_str("fs_fav", fav);
}

static int fav_find(const char *rel)
{
    for (int i = 0; i < S.nfav; i++) if (!strcmp(S.fav[i], rel)) return i;
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Thumbnails                                                                  */
/* -------------------------------------------------------------------------- */

static void th_reset(void)
{
    img_cancel_all(IMG_OWNER_FILES);
    for (int i = 0; TH && i < TH_N; i++) {
        img_free(&TH[i].r);
        memset(&TH[i], 0, sizeof TH[i]);
        TH[i].ei = -1;
    }
    free(TS);
    TS = NULL;
    TS_cap = TS_asked = 0;
}

/* Forget what was asked and never came (the app went to the back). */
static void th_forget_asked(void)
{
    img_cancel_all(IMG_OWNER_FILES);
    for (int i = 0; i < TS_cap; i++) if (TS[i] == 1) TS[i] = 0;
    TS_asked = 0;
}

static thumb_t *th_find(int ei)
{
    if (!TS || ei >= TS_cap || TS[ei] != 2) return NULL;
    for (int i = 0; TH && i < TH_N; i++) if (TH[i].ei == ei && TH[i].gen == L.gen) return &TH[i];
    return NULL;
}

static uintptr_t th_tag(int ei) { return (uintptr_t)(L.gen & 0x7F) << 24 | (uintptr_t)ei; }

static void th_ask(int ei)
{
    if (!L.l || ei >= L.l->n) return;
    if (!TS || ei >= TS_cap) {
        int cap = L.l->n + 256;
        uint8_t *t = realloc(TS, (size_t)cap);
        if (!t) return;
        memset(t + TS_cap, 0, (size_t)(cap - TS_cap));
        TS = t;
        TS_cap = cap;
    }
    if (TS[ei] || TS_asked >= TH_ASK_MAX) return;
    char path[PATH_LEN];
    if (!join(path, sizeof path, L.l->base, NM(L.l, E(L.l, ei)))) { TS[ei] = 3; return; }
    img_job_t job = { .path = path, .w = TH_PX, .h = TH_PX, .fill = true };
    if (img_request(IMG_OWNER_FILES, th_tag(ei), &job)) {
        TS[ei] = 1;
        TS_asked++;
    }
}

/* A cell that stops showing entry ei: if its thumbnail was only queued, it
 * goes out of the queue, so what is in view comes first. */
static void th_unask(int ei)
{
    if (!TS || ei < 0 || ei >= TS_cap || TS[ei] != 1) return;
    img_cancel(IMG_OWNER_FILES, th_tag(ei));
    TS[ei] = 0;
    if (TS_asked) TS_asked--;
}

static bool th_in_view(int ei)
{
    for (int k = 0; k < U.ncell; k++) if (U.cells[k].ei == ei && U.cells[k].p >= 0) return true;
    return false;
}

static void th_poll(void)
{
    img_result_t r;
    int n = 0;
    while (n < 12 && img_take(IMG_OWNER_FILES, &r)) {
        n++;
        int ei = (int)(r.tag & 0xFFFFFF);
        bool mine = ((r.tag >> 24) & 0x7F) == (L.gen & 0x7F) && TS && ei < TS_cap;
        if (mine && TS[ei] == 1 && TS_asked) TS_asked--;
        if (!mine) { img_free(&r); continue; }
        if (!r.px) { TS[ei] = 3; img_free(&r); continue; }
        /* the least recently used slot that is not on screen */
        int best = -1;
        for (int i = 0; TH && i < TH_N; i++) {
            if (TH[i].ei < 0) { best = i; break; }
            if (th_in_view(TH[i].ei)) continue;
            if (best < 0 || TH[i].used < TH[best].used) best = i;
        }
        if (best < 0) { TS[ei] = 0; img_free(&r); continue; }
        thumb_t *t = &TH[best];
        if (t->ei >= 0 && t->ei < TS_cap) TS[t->ei] = 0;
        img_free(&t->r);
        t->ei = ei;
        t->gen = L.gen;
        t->used = U.tick;
        t->r = r;
        memset(&t->dsc, 0, sizeof t->dsc);
        t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        t->dsc.header.w = (uint32_t)r.w;
        t->dsc.header.h = (uint32_t)r.h;
        t->dsc.header.stride = (uint32_t)r.w * 2u;
        t->dsc.data_size = (uint32_t)(r.w * r.h * 2);
        t->dsc.data = (const uint8_t *)r.px;
        TS[ei] = 2;
        for (int k = 0; k < U.ncell; k++) if (U.cells[k].ei == ei && U.cells[k].p >= 0) U.cells[k].p = -2;   /* rebind */
    }
    if (n) pool_refresh(false);
}

/* -------------------------------------------------------------------------- */
/* The list: a listing, its order, the selection                               */
/* -------------------------------------------------------------------------- */

static void sel_clear(void)
{
    if (L.sel) memset(L.sel, 0, (size_t)L.sel_cap);
    L.nsel = 0;
}

static void order_clear(void)
{
    free(L.order);
    L.order = NULL;
    L.norder = 0;
    L.sort_n = -1;
    L.sort_final = false;
    if (L.sj) { job_drop(&L.sj->job); L.sj = NULL; }
}

/* The same folder read again: the thumbnails already decoded stay, found
 * again by name in the new list (what was only asked is asked again). */
static void th_remap(const listing_t *old, const listing_t *nw, uint32_t new_gen)
{
    img_cancel_all(IMG_OWNER_FILES);    /* in flight for the old indexes */
    TS_asked = 0;
    lk();
    int n = nw->n;
    ulk();
    uint8_t *ts = calloc((size_t)n + 256, 1);
    for (int k = 0; TH && k < TH_N; k++) {
        thumb_t *t = &TH[k];
        if (t->ei < 0) continue;
        int found = -1;
        if (ts && t->gen == L.gen && t->ei < old->n) {
            const char *nm = NM(old, E(old, t->ei));
            for (int i = 0; i < n; i++) {
                const ent_t *e = E(nw, i);
                if (e->kind == K_IMAGE && !strcmp(NM(nw, e), nm)) { found = i; break; }
            }
        }
        if (found < 0) {
            img_free(&t->r);
            memset(t, 0, sizeof *t);
            t->ei = -1;
        } else {
            t->ei = found;
            t->gen = new_gen;
            ts[found] = 2;
        }
    }
    free(TS);
    TS = ts;
    TS_cap = ts ? n + 256 : 0;
}

/* A new listing is on show: what belonged to the old one goes. */
static void list_switched(void)
{
    order_clear();
    sel_clear();
    th_reset();
    L.gen++;
    L.seen_stated = L.seen_n = -1;
    L.seen_named = false;
    L.logged = false;
    L.t_req = now_ms();
}

static void list_release(void)
{
    order_clear();
    if (L.l) job_drop(&L.l->job);
    if (L.nl) job_drop(&L.nl->job);
    if (L.folder) job_drop(&L.folder->job);
    L.l = L.nl = L.folder = NULL;
    free(L.sel);
    L.sel = NULL;
    L.sel_cap = L.nsel = 0;
    th_reset();
}

/* The folder S.rel, read from the start: the screen shows it as it comes. */
static void list_open(void)
{
    list_release();
    L.query[0] = L.fq[0] = 0;
    L.query_dirty = false;
    char abs[PATH_LEN];
    if (!card() || !abs_of(abs, sizeof abs, S.rel)) return;
    L.l = listing_start(abs, NULL);
    list_switched();
}

/* The same folder read again (after a change): the old list stays on show
 * until the new one is sorted, so nothing blinks. */
static void relist(void)
{
    if (!L.l || L.l->search) {
        /* a search on show: its folder is read again, the search too */
        if (L.folder) { job_drop(&L.folder->job); L.folder = NULL; }
        if (L.l && L.l->search && L.query[0]) {
            char abs[PATH_LEN];
            scpy(abs, sizeof abs, L.l->base);
            job_drop(&L.l->job);
            L.l = listing_start(abs, L.query);
            list_switched();
            char fa[PATH_LEN];
            if (abs_of(fa, sizeof fa, S.rel)) L.folder = listing_start(fa, NULL);
            return;
        }
        list_open();
        return;
    }
    if (L.nl) job_drop(&L.nl->job);
    L.nl = listing_start(L.l->base, NULL);
}

static void sort_take(void);

static void sort_request(listing_t *l)
{
    if (!l) return;
    if (L.sj) { job_drop(&L.sj->job); L.sj = NULL; }
    sortjob_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->job.free_fn = sort_free;
    lk();
    s->n = l->n;
    s->stated = l->stated;
    bool final = l->job.done && l->stated == l->n;
    ulk();
    s->l = (listing_t *)job_get(&l->job);
    s->mode = S.sort;
    s->desc = S.desc;
    if (!l->search) scpy(s->query, sizeof s->query, L.fq);
    L.sj = s;
    if (l == L.l) {
        L.sort_n = s->n;
        L.sort_final = final;
    }
    L.sort_ms = now_ms();
    /* a small folder is sorted here and now: a thread would only add a frame */
    if (s->n <= 400) {
        s->job.refs = 2;
        sort_thread(s);
        sort_take();
        return;
    }
    if (!job_run("fs_sort", sort_thread, &s->job, 6144)) sort_thread(s);  /* no thread: here, once */
}

/* A sort came back: its order goes on show (and a folder read again takes
 * the place of the old one). */
static void sort_take(void)
{
    sortjob_t *s = L.sj;
    L.sj = NULL;
    if (s->l == L.nl) {
        /* the folder read again: it replaces the old list now */
        int32_t sy = U.list ? lv_obj_get_scroll_y(U.list) : 0;
        listing_t *old = L.l;
        L.l = L.nl;
        L.nl = NULL;
        th_remap(old, L.l, L.gen + 1);
        /* list_switched() without its th_reset(): the pictures were kept */
        order_clear();
        sel_clear();
        L.gen++;
        L.seen_stated = L.seen_n = -1;
        L.seen_named = false;
        L.logged = false;
        L.t_req = now_ms();
        job_drop(&old->job);
        lk();
        L.sort_n = s->n;
        L.sort_final = L.l->job.done && L.l->stated == L.l->n && s->stated == s->n;
        ulk();
        L.want_scroll = sy;
        lk();
        L.seen_stated = L.l->stated;
        ulk();
    } else if (s->l != L.l) {
        job_put(&s->job);
        return;
    }
    free(L.order);
    L.order = s->order;
    L.norder = s->norder;
    s->order = NULL;
    uint32_t us = s->us;
    int n = s->n;
    job_put(&s->job);
    if (!L.logged && L.l->named) {
        L.logged = true;
        lk();
        uint32_t mn = L.l->ms_names;
        ulk();
        aos_hal_log("files", "%s: %d entries, names %u ms, sorted in %u us, on screen %u ms after asking",
                    L.l->base, n, (unsigned)mn, (unsigned)us, (unsigned)(now_ms() - L.t_req));
    }
    list_layout();
    if (L.reveal[0]) {
        for (int p = 0; p < L.norder; p++) {
            if (!strcmp(base_of(NM(L.l, E(L.l, L.order[p]))), L.reveal)) {
                int32_t y = (S.grid ? p / U.cols : p) * U.rh;
                if (U.list && (y < lv_obj_get_scroll_y(U.list) || y + U.rh > lv_obj_get_scroll_y(U.list) + U.list_h))
                    L.want_scroll = y - U.list_h / 3 > 0 ? y - U.list_h / 3 : 0;
                L.reveal[0] = 0;
                break;
            }
        }
    }
    if (U.list && L.want_scroll >= 0) {
        lv_obj_update_layout(U.list);
        lv_obj_scroll_to_y(U.list, L.want_scroll, LV_ANIM_OFF);
        L.want_scroll = -1;
    }
    pool_refresh(true);
    footer_refresh();
}

static void list_poll(void)
{
    if (L.sj && job_done(&L.sj->job)) sort_take();
    /* a folder read again: sorted as soon as its names are in */
    if (L.nl && !L.sj) {
        lk();
        bool named = L.nl->named;
        ulk();
        if (named) sort_request(L.nl);
    }
    listing_t *l = L.l;
    if (!l) return;
    lk();
    int n = l->n, st = l->stated;
    bool named = l->named, done = l->job.done;
    ulk();
    if (!L.sj && !L.nl) {
        bool need = n != L.sort_n && (named || now_ms() - L.sort_ms > 300);
        if (done && !L.sort_final && (S.sort != SORT_NAME || n != L.sort_n)) need = true;
        if (L.query_dirty && !L.recursive) need = true;
        if (need) {
            if (!L.recursive) L.query_dirty = false;
            sort_request(l);
        }
    }
    if (st != L.seen_stated) { L.seen_stated = st; pool_refresh(true); }
    if (n != L.seen_n || named != L.seen_named) {
        L.seen_n = n;
        L.seen_named = named;
        footer_refresh();
    } else if (!done && U.tick % 8 == 0) {
        footer_refresh();
    }
}

static bool ent_path(int ei, char *out, size_t n)
{
    return L.l && ei >= 0 && ei < L.l->n && join(out, n, L.l->base, NM(L.l, E(L.l, ei)));
}

/* -------------------------------------------------------------------------- */
/* The pool of rows                                                            */
/* -------------------------------------------------------------------------- */

static void cell_cb(lv_event_t *e);

static void cell_make(cell_t *c)
{
    memset(c, 0, sizeof *c);
    c->p = -1;
    c->ei = -1;
    c->obj = box(U.list, U.cw, U.rh);
    lv_obj_set_style_radius(c->obj, 18, 0);
    lv_obj_set_style_bg_color(c->obj, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(c->obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c->obj, cell_cb, LV_EVENT_SHORT_CLICKED, c);
    lv_obj_add_event_cb(c->obj, cell_cb, LV_EVENT_LONG_PRESSED, c);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_HIDDEN);
    c->icon = glyph(c->obj, "", &aos_sym_72, AOS_C_DIM);
    c->img = lv_image_create(c->obj);
    lv_obj_remove_flag(c->img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_inner_align(c->img, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(c->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(c->img, 10, 0);
    lv_obj_set_style_clip_corner(c->img, true, 0);
    c->name = text(c->obj, "", S.grid ? aos_font_caption : aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(c->name, LV_LABEL_LONG_MODE_DOTS);
    c->sub = text(c->obj, "", S.grid ? aos_font_tiny : aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(c->sub, LV_LABEL_LONG_MODE_DOTS);
    c->check = box(c->obj, 44, 44);
    lv_obj_set_style_radius(c->check, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c->check, 3, 0);
    lv_obj_set_style_border_color(c->check, lv_color_hex(0x636366), 0);
    lv_obj_set_style_bg_color(c->check, C_TINT, 0);
    c->tick = glyph(c->check, AOS_SYM_CHECK, &aos_sym_28, lv_color_white());
    lv_obj_center(c->tick);
    lv_obj_remove_flag(c->check, LV_OBJ_FLAG_CLICKABLE);
    if (S.grid) {
        int32_t ic = 112;
        lv_obj_set_size(c->img, ic, ic);
        lv_obj_align(c->img, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_align(c->icon, LV_ALIGN_TOP_MID, 0, 22);
        lv_obj_set_size(c->name, U.cw - 16, 52);
        lv_obj_set_style_text_align(c->name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(c->name, LV_ALIGN_TOP_MID, 0, 128);
        lv_obj_set_width(c->sub, U.cw - 16);
        lv_obj_set_style_text_align(c->sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(c->sub, LV_ALIGN_TOP_MID, 0, 182);
        lv_obj_align(c->check, LV_ALIGN_TOP_RIGHT, -14, 6);
    } else {
        lv_obj_set_size(c->img, 72, 72);
        lv_obj_set_style_pad_hor(c->obj, 8, 0);
        c->chev = glyph(c->obj, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, lv_color_hex(0x48484A));
        lv_obj_align(c->chev, LV_ALIGN_RIGHT_MID, -4, 0);
        c->sep = hairline(c->obj, U.cw);
    }
}

/* Lays a list cell out: in select mode everything moves over for the circle. */
static void cell_place_list(cell_t *c, bool dir)
{
    int32_t x = U.select ? 60 : 0;
    lv_obj_set_flag(c->check, LV_OBJ_FLAG_HIDDEN, !U.select);
    lv_obj_align(c->check, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_align(c->icon, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_align(c->img, LV_ALIGN_LEFT_MID, x, 0);
    int32_t tx = x + 92, tw = U.cw - 16 - tx - (dir && !U.select ? 44 : 8);
    lv_obj_set_width(c->name, tw);
    lv_obj_set_width(c->sub, tw);
    lv_obj_set_pos(c->name, tx, (U.rh - 66) / 2 - 2);
    lv_obj_set_pos(c->sub, tx, (U.rh - 66) / 2 + 38);
    lv_obj_set_flag(c->chev, LV_OBJ_FLAG_HIDDEN, !dir || U.select);
    lv_obj_set_size(c->sep, U.cw - tx - 8, 1);
    lv_obj_set_pos(c->sep, tx, U.rh - 1);
}

static void sub_text(const ent_t *e, int ei, char *out, size_t n)
{
    bool st = ei < L.seen_stated || ((e->flags & EF_STATED) && L.l->search);
    char a[48] = "", b[32] = "", loc[PATH_LEN] = "";
    if (L.l->search) {
        const char *nm = NM(L.l, e);
        const char *sl = strrchr(nm, '/');
        if (sl) {
            char d[PATH_LEN];
            scpy(d, sizeof d, nm);
            d[sl - nm] = 0;
            disp(loc, sizeof loc, d);
        } else {
            const char *r = rel_of(L.l->base);
            disp(loc, sizeof loc, r && r[0] ? base_of(r) : _("Tarjeta"));
        }
    }
    if (!st) scpy(a, sizeof a, "…");
    else if (e->flags & EF_DIR) { if (e->size != UINT32_MAX) fmt_items(a, sizeof a, e->size); }
    else {
        fmt_size(a, sizeof a, e->size);
        if (!S.grid) fmt_date(b, sizeof b, e->mtime, false);
    }
    if (S.grid) snprintf(out, n, "%s", a);
    else if (loc[0]) snprintf(out, n, "%s%s%s", loc, a[0] ? " · " : "", a);
    else if (b[0]) snprintf(out, n, "%s · %s", b, a);
    else snprintf(out, n, "%s", a);
}

static void bind(cell_t *c, int p)
{
    int old = c->ei;
    c->p = p;
    if (!L.l || !L.order || p < 0 || p >= L.norder) {
        c->ei = -1;
        c->p = p < 0 ? -1 : p;
        lv_obj_add_flag(c->obj, LV_OBJ_FLAG_HIDDEN);
        if (old >= 0) th_unask(old);
        return;
    }
    int ei = L.order[p];
    if (old >= 0 && old != ei) th_unask(old);
    c->ei = ei;
    const ent_t *e = E(L.l, ei);
    lv_obj_remove_flag(c->obj, LV_OBJ_FLAG_HIDDEN);
    if (S.grid) lv_obj_set_pos(c->obj, U.gx + (p % U.cols) * U.cw, (p / U.cols) * U.rh + 6);
    else lv_obj_set_pos(c->obj, U.gx, p * U.rh);
    bool dir = e->flags & EF_DIR;
    if (!S.grid) cell_place_list(c, dir);
    else lv_obj_set_flag(c->check, LV_OBJ_FLAG_HIDDEN, !U.select);
    const char *g;
    uint32_t col;
    icon_for(L.l, e, &g, &col);
    thumb_t *t = e->kind == K_IMAGE ? th_find(ei) : NULL;
    if (t) {
        t->used = U.tick;
        lv_image_set_src(c->img, &t->dsc);
        lv_obj_remove_flag(c->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(c->icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(c->icon, g);
        lv_obj_set_style_text_color(c->icon, lv_color_hex(col), 0);
        if (e->kind == K_IMAGE) th_ask(ei);
    }
    char nm[NAME_LEN + 8], sub[PATH_LEN];
    disp(nm, sizeof nm, base_of(NM(L.l, e)));
    lv_label_set_text(c->name, nm);
    sub_text(e, ei, sub, sizeof sub);
    lv_label_set_text(c->sub, sub);
    bool on = L.sel && ei < L.sel_cap && L.sel[ei];
    lv_obj_set_style_bg_opa(c->check, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(c->check, on ? C_TINT : lv_color_hex(0x636366), 0);
    lv_obj_set_flag(c->tick, LV_OBJ_FLAG_HIDDEN, !on);
    lv_obj_set_style_bg_color(c->obj, on && U.select ? lv_color_hex(0x1A2A40) : AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(c->obj, on && U.select ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void pool_refresh(bool force)
{
    if (!U.list || !U.ncell) return;
    int32_t sy = lv_obj_get_scroll_y(U.list);
    int row0 = sy / U.rh - 1;
    if (row0 < 0) row0 = 0;
    int first = row0 * U.cols;
    for (int k = 0; k < U.ncell; k++) {
        int p = first + k;
        cell_t *c = &U.cells[p % U.ncell];
        if (force || c->p != p) bind(c, p);
    }
}

static void list_scroll_cb(lv_event_t *e) { (void)e; pool_refresh(false); }

/* The spacer and the footer under the last row: that is what makes the list
 * as tall as it is. */
static void list_layout(void)
{
    if (!U.list) return;
    int rows = S.grid ? (L.norder + U.cols - 1) / U.cols : L.norder;
    int32_t y = rows * U.rh + (S.grid ? 12 : 0);
    lv_obj_set_pos(U.footer, 0, y + 24);
    lv_obj_set_pos(U.spacer, 0, y + 24 + 60 + U.bottom_h);
}

static void footer_refresh(void)
{
    if (!U.footer) return;
    char t[200] = "", c[24], f[24] = "";
    listing_t *l = L.l;
    if (!l) {
        lv_label_set_text(U.footer, "");
        return;
    }
    lk();
    int n = l->n, st = l->stated, vis = l->visited;
    bool named = l->named, done = l->job.done, trunc = l->truncated;
    int err = l->err;
    ulk();
    lk();
    bool uok = USG.ok;
    uint64_t freeb = USG.freeb;
    ulk();
    if (uok) fmt_size(f, sizeof f, freeb);
    if (l->search) {
        fmt_count(c, sizeof c, L.norder);
        if (!done) {
            char v[24];
            fmt_count(v, sizeof v, vis);
            snprintf(t, sizeof t, _("Buscando… %s encontrados, %s revisados"), c, v);
        } else if (trunc) snprintf(t, sizeof t, _("%s resultados · la búsqueda se cortó ahí"), c);
        else if (L.norder == 1) scpy(t, sizeof t, _("1 resultado"));
        else snprintf(t, sizeof t, _("%s resultados"), c);
    } else if (!named) {
        fmt_count(c, sizeof c, n);
        snprintf(t, sizeof t, _("Leyendo… %s"), c);
    } else {
        char items[48];
        if (L.fq[0]) {
            char m[24];
            fmt_count(c, sizeof c, L.norder);
            fmt_count(m, sizeof m, n);
            snprintf(items, sizeof items, _("%s de %s"), c, m);
        } else {
            fmt_items(items, sizeof items, (uint32_t)n);
        }
        if (f[0]) snprintf(t, sizeof t, _("%s · %s disponibles"), items, f);
        else scpy(t, sizeof t, items);
        if (!done && n > 100) {
            size_t k = strlen(t);
            snprintf(t + k, sizeof t - k, _(" · leyendo tamaños (%d %%)"), n ? st * 100 / n : 0);
        }
        if (trunc) {
            size_t k = strlen(t);
            snprintf(t + k, sizeof t - k, "%s", _(" · no entran todos"));
        }
    }
    lv_label_set_text(U.footer, t);
    /* the empty state */
    const char *msg = NULL;
    const char *g = AOS_SYM_FOLDER_OPEN;
    if (L.norder == 0 && !L.sj) {
        if (err) { msg = _("No se pudo abrir la carpeta"); g = AOS_SYM_ALERT_OUTLINE; }
        else if (l->search && done) { msg = _("No se encontró nada"); g = AOS_SYM_MAGNIFY; }
        else if (L.fq[0] && named) { msg = L.recursive ? _("Buscando…") : _("Nada coincide en esta carpeta"); g = AOS_SYM_MAGNIFY; }
        else if (named && !n) msg = _("Carpeta vacía");
        else if (!named && now_ms() - L.t_req > 250) { msg = _("Leyendo…"); g = AOS_SYM_SD; }
    }
    if (U.empty) {
        lv_obj_set_flag(U.empty, LV_OBJ_FLAG_HIDDEN, !msg);
        if (msg) {
            lv_label_set_text(U.empty_t, msg);
            lv_label_set_text(U.empty_g, g);
        }
    }
    lv_obj_set_flag(U.footer, LV_OBJ_FLAG_HIDDEN, msg != NULL);
}

static void pool_build(void)
{
    U.gx = S.grid ? 12 : (U.land ? 16 : AOS_UI_PAD - 8);
    if (S.grid) {
        U.cols = U.land ? 5 : 4;
        U.cw = (U.list_w - 2 * U.gx) / U.cols;
        U.rh = GRID_H;
    } else {
        U.cols = 1;
        U.cw = U.list_w - 2 * U.gx;
        U.rh = U.land ? 92 : 104;
    }
    int rows = U.list_h / U.rh + 3;
    U.ncell = rows * U.cols;
    if (U.ncell > POOL_MAX) U.ncell = POOL_MAX;
    if (!U.cells) U.ncell = 0;
    for (int k = 0; k < U.ncell; k++) cell_make(&U.cells[k]);
    U.footer = text(U.list, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.footer, U.list_w);
    lv_obj_set_style_text_align(U.footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(U.footer, LV_LABEL_LONG_MODE_WRAP);
    U.spacer = box(U.list, 1, 1);
    U.empty = box(U.list, U.list_w, 360);
    lv_obj_set_pos(U.empty, 0, U.land ? 40 : 120);
    U.empty_g = glyph(U.empty, AOS_SYM_FOLDER_OPEN, &aos_sym_72, lv_color_hex(0x48484A));
    lv_obj_align(U.empty_g, LV_ALIGN_TOP_MID, 0, 20);
    U.empty_t = text(U.empty, "", aos_font_title, AOS_C_DIM);
    lv_obj_align(U.empty_t, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_add_flag(U.empty, LV_OBJ_FLAG_HIDDEN);
}

/* -------------------------------------------------------------------------- */
/* Going places                                                                */
/* -------------------------------------------------------------------------- */

static int32_t list_scroll(void) { return U.list ? lv_obj_get_scroll_y(U.list) : 0; }

/* The browser built again (a mode, a bar that comes or goes), the list where it was. */
static void rebuild_keep(void)
{
    int32_t sy = list_scroll();
    build();
    if (!U.list) return;
    lv_obj_update_layout(U.list);
    lv_obj_scroll_to_y(U.list, sy, LV_ANIM_OFF);
    pool_refresh(true);
}

/* To another folder of the card. Up (to an ancestor) comes back to where the
 * list was; down starts at the top. */
static void go_to(const char *rel)
{
    char dst[PATH_LEN];
    scpy(dst, sizeof dst, rel);
    int d0 = rel_depth(S.rel), d1 = rel_depth(dst);
    if (d0 <= WALK_DEPTH) S.scroll[d0] = list_scroll();
    bool up = path_under(S.rel, dst) && d1 < d0;
    scpy(S.rel, sizeof S.rel, dst);
    U.select = false;
    L.recursive = false;
    list_open();
    L.want_scroll = up && d1 <= WALK_DEPTH ? S.scroll[d1] : 0;
    if (!up) for (int i = d1; i <= WALK_DEPTH; i++) S.scroll[i] = 0;
    build();
}

static void go_up(void)
{
    if (!S.rel[0]) return;
    char r[PATH_LEN];
    scpy(r, sizeof r, S.rel);
    rel_up(r);
    go_to(r);
}

static void up_cb(lv_event_t *e) { (void)e; go_up(); }

static void crumb_cb(lv_event_t *e)
{
    int depth = (int)(intptr_t)lv_event_get_user_data(e);
    char r[PATH_LEN];
    scpy(r, sizeof r, S.rel);
    while (rel_depth(r) > depth) rel_up(r);
    if (strcmp(r, S.rel)) go_to(r);
}

/* ---- places: the card's own folders, and the pinned ones ---- */

typedef struct { const char *glyph; uint32_t color; const char *name; char rel[160]; } place_t;
#define N_PLACES 6

static int places(place_t *p, int max)
{
    const struct { const char *g; uint32_t c; const char *n; const char *abs; const char *rel; } P[] = {
        { AOS_SYM_SD, 0x8E8E93, N_("Tarjeta"), NULL, "" },
        { AOS_SYM_IMAGE, 0xFBBF24, N_("Fotos"), aos_hal_path_photos(), NULL },
        { AOS_SYM_MUSIC, 0xFF375F, N_("Música"), aos_hal_path_music(), NULL },
        { AOS_SYM_CHART_LINE, 0x30D158, N_("Registros"), NULL, "logs" },
        { AOS_SYM_CHIP, 0x8B5CF6, N_("Firmware"), NULL, "firmware" },
        { AOS_SYM_PACKAGE_VARIANT, 0xFF9F0A, N_("Apps"), aos_hal_path_apps(), NULL },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0] && n < max; i++) {
        const char *r = P[i].rel ? P[i].rel : rel_of(P[i].abs);
        if (!r) continue;
        p[n].glyph = P[i].g;
        p[n].color = P[i].c;
        p[n].name = P[i].n;
        scpy(p[n].rel, sizeof p[n].rel, r);
        n++;
    }
    return n;
}

/* A row's user data is its index: < 100 a place, 100 + i favourite i. */
static void place_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char r[PATH_LEN];
    if (idx >= 100) {
        if (idx - 100 >= S.nfav) return;
        scpy(r, sizeof r, S.fav[idx - 100]);
    } else {
        place_t P[N_PLACES];
        if (idx >= places(P, N_PLACES)) return;
        scpy(r, sizeof r, P[idx].rel);
    }
    overlay_close();
    char abs[PATH_LEN];
    if (!abs_of(abs, sizeof abs, r) || !is_dir_path(abs)) {
        aos_ui_toast(_("Esa carpeta no está en la tarjeta"), 1800);
        return;
    }
    go_to(r);
}

static void usage_text(char *out, size_t n)
{
    lk();
    bool ok = USG.ok;
    uint64_t t = USG.total, f = USG.freeb;
    ulk();
    if (!ok) { scpy(out, n, _("Calculando el espacio…")); return; }
    char a[24], b[24];
    fmt_size(a, sizeof a, f);
    fmt_size(b, sizeof b, t);
    snprintf(out, n, _("%s libres de %s"), a, b);
}

static lv_obj_t *place_row(lv_obj_t *parent, int32_t w, const char *sym, uint32_t color, const char *name,
                           int idx, bool on)
{
    lv_obj_t *r = box(parent, w, U.land ? 62 : 80);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_bg_color(r, on ? C_TINT : AOS_C_CARD2, on ? 0 : LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, on ? 0 : LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, place_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_align(glyph(r, sym, &aos_sym_44, on ? lv_color_white() : lv_color_hex(color)), LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *l = text(r, name, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, w - 100);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 80, 0);
    return r;
}

static void capacity_bar(lv_obj_t *parent, int32_t w)
{
    lk();
    bool ok = USG.ok;
    uint64_t t = USG.total, f = USG.freeb;
    ulk();
    char u[64];
    usage_text(u, sizeof u);
    lv_obj_t *c = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_t *bar = lv_bar_create(c);
    lv_obj_set_size(bar, w - 32, 10);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, ok && t ? (int32_t)((t - f) * 1000 / t) : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(bar, C_TINT, LV_PART_INDICATOR);
    text(c, u, aos_font_caption, AOS_C_DIM);
}

/* Portrait: the places in a sheet. */
static void places_sheet(void)
{
    overlay_open(true);
    int32_t w = U.W - 32;
    lv_obj_t *c = sheet_card(w);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *t = text(c, _("Ubicaciones"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_style_pad_left(t, 12, 0);
    place_t P[N_PLACES];
    int n = places(P, N_PLACES);
    for (int i = 0; i < n; i++) {
        place_row(c, w - 28, P[i].glyph, P[i].color, aos_tr(P[i].name), i, !strcmp(P[i].rel, S.rel));
        if (i == 0) capacity_bar(c, w - 28);
    }
    lv_obj_t *ft = text(c, _("Favoritos"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(ft, 16, 0);
    lv_obj_set_style_pad_top(ft, 10, 0);
    if (!S.nfav) {
        lv_obj_t *m = text(c, _("Mantené apretada una carpeta y elegí Agregar a favoritos."), aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(m, w - 60);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_left(m, 16, 0);
    }
    for (int i = 0; i < S.nfav; i++) {
        char d[NAME_LEN];
        disp(d, sizeof d, base_of(S.fav[i]));
        place_row(c, w - 28, AOS_SYM_FOLDER, 0x60A5FA, d, 100 + i, !strcmp(S.fav[i], S.rel));
    }
}

static void places_cb(lv_event_t *e) { (void)e; places_sheet(); }

static void build_sidebar(void)
{
    U.side = box(U.root, SIDE_W, U.H);
    lv_obj_add_flag(U.side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.side, LV_DIR_VER);
    lv_obj_set_style_bg_color(U.side, lv_color_hex(0x0E0E10), 0);
    lv_obj_set_style_bg_opa(U.side, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(U.side, 14, 0);
    lv_obj_set_style_pad_row(U.side, 4, 0);
    lv_obj_set_flex_flow(U.side, LV_FLEX_FLOW_COLUMN);
    int32_t w = SIDE_W - 28;
    lv_obj_t *t = text(U.side, _("Archivos"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_bottom(t, 2, 0);
    place_t P[N_PLACES];
    int n = places(P, N_PLACES);
    for (int i = 0; i < n; i++) {
        place_row(U.side, w, P[i].glyph, P[i].color, aos_tr(P[i].name), i, !strcmp(P[i].rel, S.rel));
        if (i == 0) capacity_bar(U.side, w);
    }
    lv_obj_t *ft = text(U.side, _("Favoritos"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(ft, 12, 0);
    lv_obj_set_style_pad_top(ft, 12, 0);
    if (!S.nfav) {
        lv_obj_t *m = text(U.side, _("Mantené apretada una carpeta para agregarla."), aos_font_caption, lv_color_hex(0x636366));
        lv_obj_set_width(m, w - 20);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_left(m, 12, 0);
    }
    for (int i = 0; i < S.nfav; i++) {
        char d[NAME_LEN];
        disp(d, sizeof d, base_of(S.fav[i]));
        place_row(U.side, w, AOS_SYM_FOLDER, 0x60A5FA, d, 100 + i, !strcmp(S.fav[i], S.rel));
    }
}

/* -------------------------------------------------------------------------- */
/* The header: nav bar, title, path bar, search                                */
/* -------------------------------------------------------------------------- */

static void select_cb(lv_event_t *e);
static void more_cb(lv_event_t *e);
static void sel_all_cb(lv_event_t *e);

static const char *folder_title(void)
{
    static char t[NAME_LEN];
    if (!S.rel[0]) return _("Tarjeta");
    disp(t, sizeof t, base_of(S.rel));
    return t;
}

static lv_obj_t *s_crumbs;

static lv_obj_t *build_crumbs(lv_obj_t *parent, int32_t w, int32_t h, bool title_last)
{
    lv_obj_t *row = box(parent, w, h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 2, 0);
    int depth = rel_depth(S.rel);
    char seg[PATH_LEN];
    const char *p = S.rel;
    for (int d = 0; d <= depth; d++) {
        bool last = d == depth;
        const char *name;
        if (d == 0) name = _("Tarjeta");
        else {
            const char *s = strchr(p, '/');
            size_t l = s ? (size_t)(s - p) : strlen(p);
            char raw[NAME_LEN];
            scpy(raw, l + 1 < sizeof raw ? l + 1 : sizeof raw, p);
            disp(seg, sizeof seg, raw);
            name = seg;
            p = s ? s + 1 : p + l;
        }
        if (d) glyph(row, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, lv_color_hex(0x636366));
        lv_obj_t *b = box(row, LV_SIZE_CONTENT, h - 8);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_pad_hor(b, 12, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, 8, 0);
        if (!last) {
            lv_obj_set_style_bg_color(b, AOS_C_CARD2, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, crumb_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
        }
        if (d == 0) glyph(b, AOS_SYM_SD, &aos_sym_28, last ? AOS_C_TEXT : C_TINT);
        lv_obj_t *l = text(b, name, last && title_last ? aos_font_title : aos_font_small, last ? AOS_C_TEXT : C_TINT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(l, last ? (U.land ? 440 : 520) : 260, 0);
    }
    s_crumbs = row;
    return row;
}

static void build_nav(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *nav = box(parent, w, U.land ? 84 : 88);
    lv_obj_set_style_pad_hor(nav, U.land ? 12 : 8, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, 6, 0);
    U.sel_all = NULL;
    if (U.select) {
        U.sel_all = link_btn(nav, NULL, "", aos_font_body, sel_all_cb, NULL);
    } else if (U.land) {
        if (S.rel[0]) link_btn(nav, AOS_SYM_CHEVRON_LEFT, NULL, aos_font_body, up_cb, NULL);
    } else if (S.rel[0]) {
        char parent_name[NAME_LEN];
        char pr[PATH_LEN];
        scpy(pr, sizeof pr, S.rel);
        rel_up(pr);
        if (pr[0]) disp(parent_name, sizeof parent_name, base_of(pr));
        else scpy(parent_name, sizeof parent_name, _("Tarjeta"));
        link_btn(nav, AOS_SYM_CHEVRON_LEFT, parent_name, aos_font_body, up_cb, NULL);
    } else {
        link_btn(nav, AOS_SYM_SD, _("Ubicaciones"), aos_font_body, places_cb, NULL);
    }
    /* the middle: the path bar in landscape, room in portrait */
    lv_obj_t *mid;
    if (U.land && !U.select) {
        mid = build_crumbs(nav, 10, 72, true);
    } else if (U.land) {
        mid = box(nav, 10, 72);
        U.title = text(mid, "", aos_font_title, AOS_C_TEXT);
        lv_obj_center(U.title);
    } else {
        mid = box(nav, 10, 10);
    }
    lv_obj_set_flex_grow(mid, 1);
    link_btn(nav, NULL, U.select ? _("OK") : _("Seleccionar"), aos_font_body, select_cb, NULL);
    if (!U.select) round_btn(nav, AOS_SYM_DOTS_HORIZONTAL, &aos_sym_44, C_TINT, AOS_C_CARD2, 60, more_cb, NULL);
}

/* ---- search, sort, view ---- */

static void kb_hide(void)
{
    if (U.kb) lv_obj_add_flag(U.kb, LV_OBJ_FLAG_HIDDEN);
    if (U.ta) lv_obj_remove_state(U.ta, LV_STATE_FOCUSED);
}

static void search_start(void)
{
    char abs[PATH_LEN];
    if (!abs_of(abs, sizeof abs, S.rel)) return;
    if (L.l && L.l->search) job_drop(&L.l->job);
    else if (L.l) { if (L.folder) job_drop(&L.folder->job); L.folder = L.l; }
    if (L.nl) { job_drop(&L.nl->job); L.nl = NULL; }
    L.l = listing_start(abs, L.query);
    list_switched();
    footer_refresh();
    pool_refresh(true);
}

/* Back from a search's results to the folder's own list. */
static void search_end(void)
{
    if (!L.l || !L.l->search) return;
    job_drop(&L.l->job);
    L.l = L.folder;
    L.folder = NULL;
    list_switched();
    if (!L.l) list_open();
    footer_refresh();
    pool_refresh(true);
}

static void search_changed(void)
{
    const char *t = U.ta ? lv_textarea_get_text(U.ta) : "";
    scpy(L.query, sizeof L.query, t);
    fold(L.fq, sizeof L.fq, t);
    bool any = L.query[0] != 0;
    if (U.clear) lv_obj_set_flag(U.clear, LV_OBJ_FLAG_HIDDEN, !any);
    if (U.rec) lv_obj_set_flag(U.rec, LV_OBJ_FLAG_HIDDEN, !any);
    if (!any && L.l && L.l->search) search_end();
    L.query_dirty = true;
    L.query_ms = now_ms();
    if (U.list) lv_obj_scroll_to_y(U.list, 0, LV_ANIM_OFF);
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_VALUE_CHANGED) search_changed();
    else if (c == LV_EVENT_FOCUSED && U.kb) {
        lv_keyboard_set_textarea(U.kb, U.ta);
        lv_obj_remove_flag(U.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(U.kb);
    }
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    kb_hide();
    bool same = L.l && L.l->search && !strcmp(L.l->query, L.fq);     /* already under way */
    if (c == LV_EVENT_READY && L.recursive && L.query[0] && !same) { L.query_dirty = false; search_start(); }
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    if (U.ta) lv_textarea_set_text(U.ta, "");
}

static void rec_cb(lv_event_t *e)
{
    (void)e;
    L.recursive = !L.recursive;
    chip_set(U.rec, L.recursive);
    if (!L.query[0]) return;
    if (L.recursive) { L.query_dirty = false; search_start(); }
    else { search_end(); L.query_dirty = true; }
}

static void sort_label(char *out, size_t n)
{
    snprintf(out, n, "%s %s", aos_tr(SORT_NAME_[S.sort]), S.desc ? "\xE2\x86\x93" : "\xE2\x86\x91");
}

static void sort_pick_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    if (m == S.sort) S.desc = !S.desc;
    else { S.sort = m; S.desc = SORT_DESC_DEFAULT[m]; }
    prefs_save();
    overlay_close();
    if (U.sort_lbl) {
        char t[48];
        sort_label(t, sizeof t);
        lv_label_set_text(U.sort_lbl, t);
    }
    L.sort_final = false;
    sort_request(L.l);
}

/* A small menu under the sort chip. */
static void sort_menu(lv_obj_t *anchor)
{
    overlay_open(true);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_20, 0);
    int32_t w = 380;
    lv_obj_t *c = sheet_card(w);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_area_t a;
    lv_obj_get_coords(anchor, &a);
    lv_area_t r;
    lv_obj_get_coords(U.root, &r);
    int32_t x = a.x2 - r.x1 - w;
    if (x < 16) x = 16;
    lv_obj_set_pos(c, x, a.y2 - r.y1 + 10);
    for (int m = 0; m < SORT_COUNT; m++) {
        if (m) hairline(c, w);
        lv_obj_t *row = box(c, w, 80);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, sort_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m);
        if (m == S.sort) lv_obj_align(glyph(row, AOS_SYM_CHECK, &aos_sym_28, C_TINT), LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_align(text(row, aos_tr(SORT_NAME_[m]), aos_font_body, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 64, 0);
        if (m == S.sort) lv_obj_align(text(row, S.desc ? "\xE2\x86\x93" : "\xE2\x86\x91", aos_font_body, C_TINT), LV_ALIGN_RIGHT_MID, -24, 0);
    }
}

static void sort_cb(lv_event_t *e) { sort_menu(lv_event_get_current_target(e)); }

static void view_cb(lv_event_t *e)
{
    (void)e;
    int32_t sy = list_scroll();
    int first = S.grid ? sy / U.rh * U.cols : sy / U.rh;
    S.grid = !S.grid;
    prefs_save();
    build();
    /* the same items in view */
    int32_t y = (S.grid ? first / U.cols : first) * U.rh;
    lv_obj_update_layout(U.list);
    lv_obj_scroll_to_y(U.list, y, LV_ANIM_OFF);
    pool_refresh(true);
}

static void build_search(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *row = box(parent, w, U.land ? 80 : 88);
    lv_obj_set_style_pad_hor(row, U.land ? 16 : AOS_UI_PAD, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    U.search = box(row, 10, 64);
    lv_obj_set_flex_grow(U.search, 1);
    lv_obj_set_style_radius(U.search, 20, 0);
    lv_obj_set_style_bg_color(U.search, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(U.search, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(U.search, 14, 0);
    lv_obj_set_flex_flow(U.search, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.search, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    glyph(U.search, AOS_SYM_MAGNIFY, &aos_sym_28, AOS_C_DIM);
    U.ta = lv_textarea_create(U.search);
    lv_textarea_set_one_line(U.ta, true);
    lv_textarea_set_placeholder_text(U.ta, _("Buscar"));
    lv_obj_set_height(U.ta, 64);
    lv_obj_set_flex_grow(U.ta, 1);
    lv_obj_set_style_bg_opa(U.ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_pad_ver(U.ta, 14, 0);
    lv_obj_set_style_pad_hor(U.ta, 10, 0);
    lv_textarea_set_text(U.ta, L.query);
    lv_obj_add_event_cb(U.ta, ta_cb, LV_EVENT_ALL, NULL);
    U.clear = round_btn(U.search, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT, lv_color_hex(0x48484A), 40, clear_cb, NULL);
    lv_obj_set_style_margin_right(U.clear, 12, 0);
    lv_obj_set_flag(U.clear, LV_OBJ_FLAG_HIDDEN, !L.query[0]);
    U.rec = chip(row, _("Subcarpetas"), L.recursive, rec_cb, NULL);
    lv_obj_set_flag(U.rec, LV_OBJ_FLAG_HIDDEN, !L.query[0]);
    char t[48];
    sort_label(t, sizeof t);
    lv_obj_t *sc = chip(row, t, false, sort_cb, NULL);
    U.sort_lbl = lv_obj_get_child(sc, 0);
    round_btn(row, S.grid ? LV_SYMBOL_LIST : AOS_SYM_VIEW_GRID_OUTLINE, S.grid ? aos_font_body : &aos_sym_28,
              AOS_C_TEXT, AOS_C_CARD2, 64, view_cb, NULL);
}

/* -------------------------------------------------------------------------- */
/* Selecting                                                                   */
/* -------------------------------------------------------------------------- */

static void sel_grow(void)
{
    if (!L.l) return;
    int n = L.l->n;
    if (n <= L.sel_cap) return;
    int cap = n + 256;
    uint8_t *s = realloc(L.sel, (size_t)cap);
    if (!s) return;
    memset(s + L.sel_cap, 0, (size_t)(cap - L.sel_cap));
    L.sel = s;
    L.sel_cap = cap;
}

static void sel_toggle(cell_t *c)
{
    sel_grow();
    if (!L.sel || c->ei < 0 || c->ei >= L.sel_cap) return;
    L.sel[c->ei] ^= 1;
    L.nsel += L.sel[c->ei] ? 1 : -1;
    bind(c, c->p);
    select_refresh();
}

static void sel_all_cb(lv_event_t *e)
{
    (void)e;
    sel_grow();
    if (!L.sel) return;
    bool all = L.nsel == L.norder && L.norder > 0;
    sel_clear();
    if (!all) for (int p = 0; p < L.norder; p++) { L.sel[L.order[p]] = 1; L.nsel++; }
    pool_refresh(true);
    select_refresh();
}

static void select_set(bool on)
{
    U.select = on;
    if (!on) sel_clear();
    kb_hide();
    rebuild_keep();
}

static void select_cb(lv_event_t *e) { (void)e; select_set(!U.select); }

/* The long-pressed item, by its path: the list may be read again while its
 * menu is up (a copy that ends, coming back to the app), and an index would
 * then name another file. */
static char s_ctx[PATH_LEN];

static bool ctx_path(char *out, size_t n)
{
    if (U.ctx < 0 || !s_ctx[0]) return false;
    scpy(out, n, s_ctx);
    return true;
}

/* The absolute paths the actions act on: the long-pressed one, else the
 * selection. The strings are the caller's. */
static char **targets(int *n)
{
    *n = 0;
    int cnt = U.ctx >= 0 ? 1 : L.nsel;
    if (!cnt || !L.l) return NULL;
    char **v = calloc((size_t)cnt, sizeof *v);
    if (!v) return NULL;
    char p[PATH_LEN];
    if (U.ctx >= 0) {
        if (ctx_path(p, sizeof p)) v[(*n)++] = strdup(p);
    } else {
        for (int q = 0; q < L.norder && *n < cnt; q++) {
            int ei = L.order[q];
            if (ei < L.sel_cap && L.sel[ei] && ent_path(ei, p, sizeof p)) v[(*n)++] = strdup(p);
        }
    }
    return v;
}

static void free_paths(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* -------------------------------------------------------------------------- */
/* Actions                                                                     */
/* -------------------------------------------------------------------------- */

static void open_file(const char *path, int kind)
{
    const char *rel = rel_of(path);
    if (kind == K_IMAGE) { if (!aos_ui_open_app_with("aos.photos", path)) aos_ui_toast(_("No se pudo abrir Fotos"), 1600); return; }
    if (kind == K_AUDIO) { if (!aos_ui_open_app_with("aos.music", path)) aos_ui_toast(_("No se pudo abrir Música"), 1600); return; }
    if (kind == K_FW && rel && path_under(rel, "firmware")) {
        if (!aos_ui_open_app_with("aos.flasher", path)) aos_ui_toast(_("No se pudo abrir el Programador"), 1600);
        return;
    }
    /* A firmware image elsewhere is just bytes: the viewer, in hex (or as
     * text, an Intel .hex). It used to open the information sheet. */
    if (kind == K_APP || kind == K_ICON) { info_open(); return; }
    viewer_open(path, kind);
}

static void open_entry(int ei)
{
    char p[PATH_LEN];
    if (!ent_path(ei, p, sizeof p)) return;
    const ent_t *e = E(L.l, ei);
    if (e->flags & EF_DIR) {
        const char *r = rel_of(p);
        if (r) go_to(r);
        return;
    }
    U.ctx = ei;
    scpy(s_ctx, sizeof s_ctx, p);
    open_file(p, e->kind);
    U.ctx = -1;
}

static void act_open(void)
{
    char p[PATH_LEN];
    bool ok = ctx_path(p, sizeof p);
    U.ctx = -1;
    if (!ok) return;
    struct stat st;
    if (stat(p, &st) != 0) { aos_ui_toast(_("Ya no está"), 1400); return; }
    if (S_ISDIR(st.st_mode)) {
        const char *r = rel_of(p);
        if (r) go_to(r);
        return;
    }
    U.ctx = 0;                  /* for the sheet, if that is what opens */
    open_file(p, kind_of(p));
    U.ctx = -1;
}

static void act_as_text(void)
{
    char p[PATH_LEN];
    if (ctx_path(p, sizeof p)) viewer_open(p, K_TEXT);
    U.ctx = -1;
}

static void act_flasher(void)
{
    char p[PATH_LEN];
    if (ctx_path(p, sizeof p) && !aos_ui_open_app_with("aos.flasher", p))
        aos_ui_toast(_("No se pudo abrir el Programador"), 1600);
    U.ctx = -1;
}

static void act_show_in_folder(void)
{
    char p[PATH_LEN];
    if (!ctx_path(p, sizeof p)) return;
    U.ctx = -1;
    const char *r = rel_of(p);
    if (!r) return;
    char dir[PATH_LEN];
    scpy(dir, sizeof dir, r);
    rel_up(dir);
    scpy(L.reveal, sizeof L.reveal, base_of(r));
    go_to(dir);
    scpy(L.reveal, sizeof L.reveal, base_of(r));
}

static void act_fav(void)
{
    char p[PATH_LEN];
    if (!ctx_path(p, sizeof p)) return;
    U.ctx = -1;
    const char *r = rel_of(p);
    if (!r) return;
    int i = fav_find(r);
    if (i >= 0) {
        memmove(S.fav[i], S.fav[i + 1], (size_t)(S.nfav - i - 1) * sizeof S.fav[0]);
        S.nfav--;
        aos_ui_toast(_("Quitada de favoritos"), 1400);
    } else if (S.nfav >= MAX_FAV) {
        aos_ui_toast(_("Ya hay 8 favoritos: quitá uno primero"), 1800);
        return;
    } else if (strlen(r) >= sizeof S.fav[0]) {
        aos_ui_toast(_("La ruta es demasiado larga para favoritos"), 1800);
        return;
    } else {
        scpy(S.fav[S.nfav++], sizeof S.fav[0], r);
        aos_ui_toast(_("Agregada a favoritos"), 1400);
    }
    prefs_save();
    if (U.land) rebuild_keep();
}

/* ---- copy, cut, paste ---- */

static void clip_clear(void)
{
    free_paths(S.clip, S.nclip);
    S.clip = NULL;
    S.nclip = 0;
}

static void clip_set(bool cut)
{
    int n;
    char **v = targets(&n);
    if (!n) { free(v); return; }
    clip_clear();
    if (n > MAX_CLIP) {
        free_paths(v, n);
        aos_ui_toast(_("Demasiados elementos a la vez"), 1800);
        return;
    }
    S.clip = v;
    S.nclip = n;
    S.clip_cut = cut;
    U.ctx = -1;
    char t[96], c[16];
    fmt_count(c, sizeof c, n);
    if (n == 1) scpy(t, sizeof t, cut ? _("Cortado: andá a otra carpeta y tocá Pegar") : _("Copiado: andá a otra carpeta y tocá Pegar"));
    else snprintf(t, sizeof t, cut ? _("%s elementos para mover: tocá Pegar en otra carpeta") : _("%s elementos copiados: tocá Pegar en otra carpeta"), c);
    aos_ui_toast(t, 2400);
    if (U.select) select_set(false);
    else rebuild_keep();
}

static void do_cut(void) { clip_set(true); }
static void act_copy(void) { clip_set(false); }

static const char *targets_reason(void)
{
    int n;
    char **v = targets(&n);
    const char *r = NULL;
    for (int i = 0; i < n && !r; i++) r = system_reason(v[i]);
    free_paths(v, n);
    return r;
}

static void act_cut(void)
{
    const char *r = targets_reason();
    if (!r) { do_cut(); return; }
    char w[240];
    snprintf(w, sizeof w, _("Es del sistema: %s Si lo movés, lo que depende de él deja de encontrarlo."), r);
    confirm(_("¿Mover archivos del sistema?"), NULL, w, _("Cortar igual"), true, do_cut);
}

static void paste_now(void)
{
    char dst[PATH_LEN];
    if (!S.nclip || !abs_of(dst, sizeof dst, S.rel)) return;
    char **v = calloc((size_t)S.nclip, sizeof *v);
    if (!v) return;
    for (int i = 0; i < S.nclip; i++) v[i] = strdup(S.clip[i]);
    bool cut = S.clip_cut;
    if (!op_start(cut ? OP_MOVE : OP_COPY, v, S.nclip, dst)) {
        free_paths(v, S.nclip);
        aos_ui_toast(_("Ya hay una copia en curso"), 1600);
        return;
    }
    if (cut) { clip_clear(); rebuild_keep(); }
    S.op_hidden = false;
    op_sheet();
}

static void paste_cb(lv_event_t *e) { (void)e; paste_now(); }

static void clip_x_cb(lv_event_t *e)
{
    (void)e;
    clip_clear();
    rebuild_keep();
}

static void act_dup(void)
{
    int n;
    char **v = targets(&n);
    U.ctx = -1;
    if (!n) { free(v); return; }
    char dst[PATH_LEN];
    scpy(dst, sizeof dst, v[0]);
    char *s = strrchr(dst, '/');
    if (s) *s = 0;
    if (!op_start(OP_DUP, v, n, dst)) { free_paths(v, n); aos_ui_toast(_("Ya hay una copia en curso"), 1600); return; }
    S.op_hidden = false;
    op_sheet();
}

/* ---- delete ---- */

static void do_delete(void)
{
    int n;
    char **v = targets(&n);
    U.ctx = -1;
    if (!n) { free(v); return; }
    if (!op_start(OP_DELETE, v, n, NULL)) { free_paths(v, n); aos_ui_toast(_("Hay otra operación en curso"), 1600); return; }
    if (U.select) select_set(false);
    S.op_hidden = false;
    op_sheet();
}

static void act_delete(void)
{
    int n;
    char **v = targets(&n);
    if (!n) { free(v); return; }
    char title[NAME_LEN + 32], name[NAME_LEN];
    const char *msg;
    bool dir = is_dir_path(v[0]);
    if (n == 1) {
        disp(name, sizeof name, base_of(v[0]));
        snprintf(title, sizeof title, _("¿Borrar «%s»?"), name);
        msg = dir ? _("Se borra la carpeta con todo lo que tiene adentro. La tarjeta no tiene papelera.")
                  : _("Se borra de la tarjeta. No tiene papelera.");
    } else {
        char c[16];
        fmt_count(c, sizeof c, n);
        snprintf(title, sizeof title, _("¿Borrar %s elementos?"), c);
        msg = _("Se borran de la tarjeta, las carpetas con todo lo que tienen adentro. No hay papelera.");
    }
    const char *r = NULL;
    for (int i = 0; i < n && !r; i++) r = system_reason(v[i]);
    char w[240] = "";
    if (r) snprintf(w, sizeof w, _("Es del sistema: %s"), r);
    free_paths(v, n);
    confirm(title, msg, r ? w : NULL, r ? _("Borrar igual") : _("Borrar"), true, do_delete);
}

/* ---- rename, new folder ---- */

static char s_rename_from[PATH_LEN];

static void rename_done(const char *name)
{
    char dir[PATH_LEN], dst[PATH_LEN];
    scpy(dir, sizeof dir, s_rename_from);
    char *s = strrchr(dir, '/');
    if (!s) return;
    *s = 0;
    if (!strcmp(name, base_of(s_rename_from))) return;
    if (!join(dst, sizeof dst, dir, name)) { aos_ui_toast(_("El nombre es demasiado largo"), 1600); return; }
    /* FAT does not tell "a" from "A": only a change of case may land on itself */
    if (exists(dst) && strcasecmp(name, base_of(s_rename_from))) { aos_ui_toast(_("Ya hay algo con ese nombre"), 1800); return; }
    if (rename(s_rename_from, dst) != 0) { aos_ui_toast(_("No se pudo cambiar el nombre"), 1800); return; }
    if (U.select) select_set(false);
    scpy(L.reveal, sizeof L.reveal, name);
    relist();
}

static void do_rename(void)
{
    char name[NAME_LEN];
    scpy(name, sizeof name, base_of(s_rename_from));
    text_entry(_("Cambiar nombre"), name, _("Listo"), rename_done);
}

static void act_rename(void)
{
    int n;
    char **v = targets(&n);
    U.ctx = -1;
    if (n != 1) { free_paths(v, n); return; }
    scpy(s_rename_from, sizeof s_rename_from, v[0]);
    const char *r = system_reason(v[0]);
    free_paths(v, n);
    if (r) {
        char w[240];
        snprintf(w, sizeof w, _("Es del sistema: %s Con otro nombre, lo que lo busca no lo encuentra."), r);
        confirm(_("¿Cambiar el nombre?"), NULL, w, _("Cambiar igual"), true, do_rename);
    } else {
        do_rename();
    }
}

static void mkdir_done(const char *name)
{
    char dir[PATH_LEN], p[PATH_LEN];
    if (!abs_of(dir, sizeof dir, S.rel) || !join(p, sizeof p, dir, name)) return;
    if (exists(p)) { aos_ui_toast(_("Ya hay algo con ese nombre"), 1800); return; }
    if (mkdir(p, 0775) != 0) { aos_ui_toast(_("No se pudo crear la carpeta"), 1800); return; }
    scpy(L.reveal, sizeof L.reveal, name);
    relist();
}

static void act_mkdir(void)
{
    char dir[PATH_LEN], name[NAME_LEN];
    if (!abs_of(dir, sizeof dir, S.rel)) return;
    if (!unique_name(dir, _("Carpeta nueva"), true, name, sizeof name)) scpy(name, sizeof name, _("Carpeta nueva"));
    text_entry(_("Carpeta nueva"), name, _("Crear"), mkdir_done);
}

static void act_info(void) { info_open(); }
static void act_refresh(void) { relist(); usage_request(); }
static void act_paste(void) { paste_now(); }

static void act_fav_here(void)
{
    /* the folder on show, as if long-pressed */
    if (!S.rel[0]) return;
    int i = fav_find(S.rel);
    if (i >= 0) {
        memmove(S.fav[i], S.fav[i + 1], (size_t)(S.nfav - i - 1) * sizeof S.fav[0]);
        S.nfav--;
        aos_ui_toast(_("Quitada de favoritos"), 1400);
    } else if (S.nfav < MAX_FAV && strlen(S.rel) < sizeof S.fav[0]) {
        scpy(S.fav[S.nfav++], sizeof S.fav[0], S.rel);
        aos_ui_toast(_("Agregada a favoritos"), 1400);
    } else {
        aos_ui_toast(_("Ya hay 8 favoritos: quitá uno primero"), 1800);
        return;
    }
    prefs_save();
    if (U.land) rebuild_keep();
}

static void act_info_here(void)
{
    U.ctx = -2;                 /* the folder on show */
    info_open();
}

static void more_cb(lv_event_t *e)
{
    (void)e;
    act_t a[8];
    int n = 0;
    U.ctx = -1;
    a[n++] = (act_t){ AOS_SYM_FOLDER_PLUS, _("Carpeta nueva"), AOS_C_TEXT, act_mkdir };
    if (S.nclip) a[n++] = (act_t){ AOS_SYM_CONTENT_PASTE, S.clip_cut ? _("Mover acá") : _("Pegar acá"), AOS_C_TEXT, act_paste };
    if (S.rel[0]) a[n++] = (act_t){ fav_find(S.rel) >= 0 ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE,
                                    fav_find(S.rel) >= 0 ? _("Quitar de favoritos") : _("Agregar a favoritos"), AOS_C_TEXT, act_fav_here };
    a[n++] = (act_t){ AOS_SYM_INFORMATION_OUTLINE, _("Información de la carpeta"), AOS_C_TEXT, act_info_here };
    a[n++] = (act_t){ AOS_SYM_RESTART, _("Volver a leer"), AOS_C_TEXT, act_refresh };
    sheet_open(S.rel[0] ? AOS_SYM_FOLDER : AOS_SYM_SD, S.rel[0] ? 0x60A5FA : 0x8E8E93, folder_title(), NULL, a, n);
}

/* The menu of one item, from a long press. */
static void ctx_menu(int ei)
{
    const ent_t *e = E(L.l, ei);
    char p[PATH_LEN];
    if (!ent_path(ei, p, sizeof p)) return;
    U.ctx = ei;
    scpy(s_ctx, sizeof s_ctx, p);
    const char *rel = rel_of(p);
    bool dir = e->flags & EF_DIR;
    act_t a[14];
    int n = 0;
    const char *open = _("Abrir");
    if (e->kind == K_IMAGE) open = _("Abrir en Fotos");
    else if (e->kind == K_AUDIO) open = _("Reproducir en Música");
    else if (e->kind == K_FW && rel && path_under(rel, "firmware")) open = _("Abrir en el Programador");
    else if (kind_is_text(e->kind)) open = _("Ver");
    if (dir || e->kind != K_OTHER) a[n++] = (act_t){ dir ? AOS_SYM_FOLDER_OPEN : AOS_SYM_CHEVRON_RIGHT, open, AOS_C_TEXT, act_open };
    if (!dir && !kind_is_text(e->kind)) a[n++] = (act_t){ AOS_SYM_FILE_DOCUMENT_OUTLINE, _("Ver el contenido"), AOS_C_TEXT, act_as_text };
    if (dir && rel && path_under(rel, "firmware") && rel_depth(rel) == 2)
        a[n++] = (act_t){ AOS_SYM_CHIP, _("Abrir en el Programador"), AOS_C_TEXT, act_flasher };
    a[n++] = (act_t){ AOS_SYM_INFORMATION_OUTLINE, _("Información"), AOS_C_TEXT, act_info };
    if (L.l->search) a[n++] = (act_t){ AOS_SYM_FOLDER, _("Mostrar en su carpeta"), AOS_C_TEXT, act_show_in_folder };
    a[n++] = (act_t){ AOS_SYM_PENCIL, _("Cambiar nombre"), AOS_C_TEXT, act_rename };
    a[n++] = (act_t){ AOS_SYM_PLUS, _("Duplicar"), AOS_C_TEXT, act_dup };
    a[n++] = (act_t){ AOS_SYM_CONTENT_COPY, _("Copiar"), AOS_C_TEXT, act_copy };
    a[n++] = (act_t){ AOS_SYM_CONTENT_CUT, _("Cortar"), AOS_C_TEXT, act_cut };
    if (dir && rel) {
        bool f = fav_find(rel) >= 0;
        a[n++] = (act_t){ f ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE, f ? _("Quitar de favoritos") : _("Agregar a favoritos"), AOS_C_TEXT, act_fav };
    }
    a[n++] = (act_t){ AOS_SYM_DELETE, _("Borrar"), AOS_C_RED, act_delete };
    char nm[NAME_LEN], sub[PATH_LEN + 64];
    disp(nm, sizeof nm, base_of(NM(L.l, e)));
    char s2[PATH_LEN];
    sub_text(e, ei, s2, sizeof s2);
    snprintf(sub, sizeof sub, "%s · %s", aos_tr(KIND[e->kind].name), s2);
    const char *g;
    uint32_t col;
    icon_for(L.l, e, &g, &col);
    sheet_open(g, col, nm, sub, a, n);
}

static void cell_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    if (!c || c->ei < 0) return;
    kb_hide();
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        if (U.select) sel_toggle(c);
        else ctx_menu(c->ei);
        return;
    }
    if (U.select) sel_toggle(c);
    else open_entry(c->ei);
}

/* ---- the bottom: the selection's actions, or the clipboard ---- */

static void bar_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!L.nsel || (i == 2 && L.nsel != 1)) return;
    U.ctx = -1;
    switch (i) {
    case 0: act_copy(); break;
    case 1: act_cut(); break;
    case 2: act_rename(); break;
    case 3: act_info(); break;
    case 4: act_delete(); break;
    }
}

static void select_refresh(void)
{
    if (U.sel_all) {
        bool all = L.nsel == L.norder && L.norder > 0;
        lv_label_set_text(lv_obj_get_child(U.sel_all, 0), all ? _("Ninguno") : _("Todos"));
    }
    if (U.title && U.select) {
        char c[16], t[64];
        fmt_count(c, sizeof c, L.nsel);
        if (!L.nsel) scpy(t, sizeof t, _("Elegí archivos"));
        else if (L.nsel == 1) scpy(t, sizeof t, _("1 elegido"));
        else snprintf(t, sizeof t, _("%s elegidos"), c);
        lv_label_set_text(U.title, t);
    }
    for (int i = 0; i < 5; i++) {
        if (!U.bar_btn[i]) continue;
        bool on = L.nsel > 0 && (i != 2 || L.nsel == 1);
        lv_obj_set_style_opa(U.bar_btn[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void build_bottom(lv_obj_t *parent, int32_t w)
{
    memset(U.bar_btn, 0, sizeof U.bar_btn);
    U.bottom = NULL;
    U.bottom_h = 0;
    if (U.select) {
        U.bottom_h = U.land ? 96 : 112;
        U.bottom = box(parent, w, U.bottom_h);
        lv_obj_add_flag(U.bottom, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(U.bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(U.bottom, lv_color_hex(0x121216), 0);
        lv_obj_set_style_bg_opa(U.bottom, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(U.bottom, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(U.bottom, 1, 0);
        lv_obj_set_style_border_color(U.bottom, C_SEP, 0);
        lv_obj_set_flex_flow(U.bottom, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(U.bottom, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        static const char *const G[5] = { AOS_SYM_CONTENT_COPY, AOS_SYM_CONTENT_CUT, AOS_SYM_PENCIL, AOS_SYM_INFORMATION_OUTLINE, AOS_SYM_DELETE };
        static const char *const T[5] = { N_("Copiar"), N_("Cortar"), N_("Renombrar"), N_("Info"), N_("Borrar") };
        for (int i = 0; i < 5; i++) {
            lv_obj_t *b = box(U.bottom, w / 5, U.bottom_h);
            lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
            tap(b, bar_cb, (void *)(intptr_t)i);
            lv_color_t c = i == 4 ? AOS_C_RED : C_TINT;
            lv_obj_align(glyph(b, G[i], &aos_sym_44, c), LV_ALIGN_CENTER, 0, -14);
            lv_obj_align(text(b, aos_tr(T[i]), aos_font_tiny, c), LV_ALIGN_CENTER, 0, 30);
            U.bar_btn[i] = b;
        }
        select_refresh();
    } else if (S.nclip) {
        U.bottom_h = 112;
        U.bottom = box(parent, w - 32, 96);
        lv_obj_add_flag(U.bottom, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(U.bottom, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_bg_color(U.bottom, lv_color_hex(0x26262A), 0);
        lv_obj_set_style_bg_opa(U.bottom, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(U.bottom, 28, 0);
        lv_obj_set_style_shadow_width(U.bottom, 30, 0);
        lv_obj_set_style_shadow_color(U.bottom, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(U.bottom, LV_OPA_60, 0);
        lv_obj_set_style_pad_hor(U.bottom, 20, 0);
        lv_obj_set_flex_flow(U.bottom, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(U.bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(U.bottom, 14, 0);
        glyph(U.bottom, S.clip_cut ? AOS_SYM_CONTENT_CUT : AOS_SYM_CONTENT_COPY, &aos_sym_44, C_TINT);
        char t[96], c[16], nm[NAME_LEN];
        fmt_count(c, sizeof c, S.nclip);
        if (S.nclip == 1) { disp(nm, sizeof nm, base_of(S.clip[0])); scpy(t, sizeof t, nm); }
        else snprintf(t, sizeof t, S.clip_cut ? _("%s elementos para mover") : _("%s elementos copiados"), c);
        lv_obj_t *l = text(U.bottom, t, aos_font_small, AOS_C_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(l, 1);
        pill(U.bottom, AOS_SYM_CONTENT_PASTE, S.clip_cut ? _("Mover acá") : _("Pegar acá"), C_TINT, lv_color_white(), paste_cb, NULL);
        round_btn(U.bottom, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT, AOS_C_CARD2, 56, clip_x_cb, NULL);
    }
}

/* ---- the progress of a copy that was sent to the back ---- */

static void pill_cb(lv_event_t *e) { (void)e; S.op_hidden = false; op_sheet(); }

static void build_pill(lv_obj_t *parent)
{
    U.pill = box(parent, 420, 64);
    lv_obj_add_flag(U.pill, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(U.pill, LV_ALIGN_BOTTOM_MID, 0, -(U.bottom_h + 16));
    lv_obj_set_style_bg_color(U.pill, lv_color_hex(0x26262A), 0);
    lv_obj_set_style_bg_opa(U.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.pill, 32, 0);
    lv_obj_set_style_shadow_width(U.pill, 24, 0);
    lv_obj_set_style_shadow_opa(U.pill, LV_OPA_60, 0);
    tap(U.pill, pill_cb, NULL);
    U.pill_lbl = text(U.pill, "", aos_font_small, AOS_C_TEXT);
    lv_obj_align(U.pill_lbl, LV_ALIGN_LEFT_MID, 24, -6);
    U.pill_bar = lv_bar_create(U.pill);
    lv_obj_set_size(U.pill_bar, 372, 6);
    lv_obj_align(U.pill_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_bar_set_range(U.pill_bar, 0, 1000);
    lv_obj_set_style_bg_color(U.pill_bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(U.pill_bar, C_TINT, LV_PART_INDICATOR);
    lv_obj_remove_flag(U.pill_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(U.pill, LV_OBJ_FLAG_HIDDEN);
}

/* -------------------------------------------------------------------------- */
/* Building the browser                                                        */
/* -------------------------------------------------------------------------- */

static void build(void)
{
    overlay_close();
    lv_obj_clean(U.root);
    U.main = U.side = U.title = U.sel_all = U.search = U.ta = U.clear = U.rec = U.sort_lbl = U.kb = NULL;
    U.list = U.spacer = U.footer = U.empty = U.bottom = U.pill = NULL;
    U.ncell = 0;
    s_crumbs = NULL;
    U.W = lv_obj_get_width(U.root);
    U.H = lv_obj_get_height(U.root);
    U.land = U.W > U.H;
    if (U.viewing) { viewer_build(); return; }
    int32_t x0 = 0, w = U.W;
    if (U.land) {
        build_sidebar();
        x0 = SIDE_W;
        w = U.W - SIDE_W;
    }
    U.main = box(U.root, w, U.H);
    lv_obj_set_x(U.main, x0);
    lv_obj_set_flex_flow(U.main, LV_FLEX_FLOW_COLUMN);
    if (!card()) {
        build_nav(U.main, w);
        lv_obj_t *m = box(U.main, w, 500);
        lv_obj_align(glyph(m, AOS_SYM_SD, &aos_sym_72, lv_color_hex(0x48484A)), LV_ALIGN_TOP_MID, 0, 120);
        lv_obj_t *t = text(m, _("No hay tarjeta"), aos_font_title, AOS_C_TEXT);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 220);
        lv_obj_t *d = text(m, _("Poné una microSD en la ranura de la placa."), aos_font_body, AOS_C_DIM);
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 280);
        return;
    }
    build_nav(U.main, w);
    if (!U.land) {
        lv_obj_t *h = box(U.main, w, 72);
        U.title = text(h, U.select ? "" : folder_title(), aos_font_large, AOS_C_TEXT);
        lv_label_set_long_mode(U.title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(U.title, w - 2 * AOS_UI_PAD);
        lv_obj_align(U.title, LV_ALIGN_LEFT_MID, AOS_UI_PAD, 0);
        if (S.rel[0]) {
            lv_obj_t *cr = box(U.main, w, 60);
            lv_obj_set_style_pad_hor(cr, 12, 0);
            build_crumbs(cr, w - 24, 60, false);
        }
    }
    build_search(U.main, w);
    U.list = box(U.main, w, 10);
    lv_obj_set_flex_grow(U.list, 1);
    lv_obj_add_flag(U.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(U.list, list_scroll_cb, LV_EVENT_SCROLL, NULL);
    build_bottom(U.main, w);
    lv_obj_update_layout(U.main);
    if (s_crumbs) lv_obj_scroll_to_x(s_crumbs, LV_COORD_MAX, LV_ANIM_OFF);   /* the folder on show in view */
    U.list_w = lv_obj_get_width(U.list);
    U.list_h = lv_obj_get_height(U.list);
    pool_build();
    build_pill(U.main);
    U.kb = lv_keyboard_create(U.root);
    lv_obj_set_size(U.kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_obj_add_event_cb(U.kb, kb_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(U.kb, LV_OBJ_FLAG_HIDDEN);
    list_layout();
    pool_refresh(true);
    footer_refresh();
    select_refresh();
}

/* -------------------------------------------------------------------------- */
/* The information sheet                                                       */
/* -------------------------------------------------------------------------- */

static char s_info_path[PATH_LEN];
static int s_info_kind;

static lv_obj_t *info_row(lv_obj_t *card, int32_t w, const char *key, const char *val)
{
    lv_obj_t *r = box(card, w, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 64, 0);
    lv_obj_set_style_pad_ver(r, 12, 0);
    lv_obj_align(text(r, key, aos_font_body, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *v = text(r, val, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(v, w * 62 / 100);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    hairline(card, w);
    return v;
}

static void info_open_cb(lv_event_t *e)
{
    (void)e;
    char p[PATH_LEN];
    scpy(p, sizeof p, s_info_path);
    int k = s_info_kind;
    overlay_close();
    if (k == K_DIR) {
        const char *r = rel_of(p);
        if (r) go_to(r);
    } else {
        open_file(p, k);
    }
}

static void info_done_cb(lv_event_t *e) { (void)e; overlay_close(); }

static void du_text(void)
{
    if (!U.du || !U.info_size) return;
    lk();
    uint64_t b = U.du->bytes;
    uint32_t f = U.du->files, d = U.du->dirs;
    bool done = U.du->job.done;
    ulk();
    char s[40], t[96], cf[16], cd[16];
    fmt_size(s, sizeof s, b);
    if (done) {
        char exact[24];
        fmt_count(exact, sizeof exact, (long)(b > 0x7FFFFFFF ? 0 : b));
        if (b < 1000 || b > 0x7FFFFFFF) lv_label_set_text(U.info_size, s);
        else lv_label_set_text_fmt(U.info_size, "%s (%s bytes)", s, exact);
    } else {
        lv_label_set_text_fmt(U.info_size, _("%s y contando…"), s);
    }
    fmt_count(cf, sizeof cf, f);
    fmt_count(cd, sizeof cd, d);
    snprintf(t, sizeof t, _("%s archivos, %s carpetas"), cf, cd);
    if (U.info_items) lv_label_set_text(U.info_items, t);
}

static void info_open(void)
{
    char **v;
    int n;
    if (U.ctx == -2) {
        char p[PATH_LEN];
        v = calloc(1, sizeof *v);
        n = 0;
        if (v && abs_of(p, sizeof p, S.rel)) v[n++] = strdup(p);
    } else {
        v = targets(&n);
    }
    U.ctx = -1;
    if (!n) { free(v); return; }
    overlay_open(true);
    int32_t w = U.land ? 700 : U.W - 48;
    lv_obj_t *c = sheet_card(w);
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_set_style_pad_row(c, 0, 0);
    lv_obj_center(c);
    int32_t iw = w - 56;
    lv_obj_t *head = box(c, iw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(head, 110, 0);
    lv_obj_set_style_min_height(head, 100, 0);
    lv_obj_set_style_pad_bottom(head, 16, 0);
    struct stat st;
    bool dir = n > 1 || (stat(v[0], &st) == 0 && S_ISDIR(st.st_mode));
    if (n == 1 && stat(v[0], &st) != 0) memset(&st, 0, sizeof st);
    int kind = n > 1 ? K_OTHER : dir ? K_DIR : kind_of(v[0]);
    const char *g = n > 1 ? AOS_SYM_CONTENT_COPY : KIND[kind].glyph;
    uint32_t col = n > 1 ? 0x8E8E93 : KIND[kind].color;
    lv_obj_align(glyph(head, g, &aos_sym_72, lv_color_hex(col)), LV_ALIGN_LEFT_MID, -100, 0);
    char nm[NAME_LEN], t[PATH_LEN];
    if (n > 1) {
        char cnt[16];
        fmt_count(cnt, sizeof cnt, n);
        snprintf(nm, sizeof nm, _("%s elementos"), cnt);
    } else {
        const char *r = rel_of(v[0]);
        disp(nm, sizeof nm, r && !r[0] ? _("Tarjeta") : base_of(v[0]));
    }
    lv_obj_t *tl = text(head, nm, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(tl, iw - 110);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *kl = text(head, n > 1 ? _("Selección") : aos_tr(KIND[kind].name), aos_font_small, AOS_C_DIM);
    lv_obj_align_to(kl, tl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    hairline(c, iw);
    U.info_size = U.info_items = NULL;
    if (dir) {
        U.info_size = info_row(c, iw, _("Tamaño"), _("Calculando…"));
        U.info_items = info_row(c, iw, _("Contiene"), "…");
        dujob_t *d = calloc(1, sizeof *d);
        if (d) {
            d->job.free_fn = du_free;
            d->paths = v;
            d->npaths = n;
            v = NULL;
            U.du = d;
            job_run("fs_du", du_thread, &d->job, JOB_STACK);
        }
    } else {
        char s[24], exact[24];
        fmt_size(s, sizeof s, (uint64_t)st.st_size);
        fmt_count(exact, sizeof exact, (long)st.st_size);
        if (st.st_size >= 1000) snprintf(t, sizeof t, "%s (%s bytes)", s, exact);
        else scpy(t, sizeof t, s);
        info_row(c, iw, _("Tamaño"), t);
    }
    const char *path0 = U.du ? U.du->paths[0] : v[0];
    if (n == 1) {
        fmt_date(t, sizeof t, st.st_mtime > 0 ? (uint32_t)st.st_mtime : 0, true);
        info_row(c, iw, _("Modificado"), t);
        const char *r = rel_of(path0);
        char where[PATH_LEN + 8] = "/";
        if (r && r[0]) {
            char pr[PATH_LEN];
            scpy(pr, sizeof pr, r);
            rel_up(pr);
            char d2[PATH_LEN];
            disp(d2, sizeof d2, pr);
            snprintf(where, sizeof where, "/%s", d2);
        } else if (r) {
            scpy(where, sizeof where, _("La tarjeta"));
        }
        info_row(c, iw, _("Ubicación"), where);
        const char *why = system_reason(path0);
        if (why) {
            lv_obj_t *wr = box(c, iw, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_top(wr, 16, 0);
            lv_obj_t *wl = text(wr, why, aos_font_small, AOS_C_ORANGE);
            lv_obj_set_width(wl, iw);
            lv_label_set_long_mode(wl, LV_LABEL_LONG_MODE_WRAP);
        }
        scpy(s_info_path, sizeof s_info_path, path0);
        s_info_kind = kind;
    }
    lv_obj_t *row = box(c, iw, 100);
    lv_obj_set_style_pad_top(row, 20, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    const char *r0 = rel_of(path0);
    bool can_open = n == 1 && !(dir && r0 && !strcmp(r0, S.rel)) && kind != K_APP && kind != K_ICON;
    if (can_open) pill(row, NULL, _("Abrir"), AOS_C_CARD2, AOS_C_TEXT, info_open_cb, NULL);
    pill(row, NULL, _("Listo"), C_TINT, lv_color_white(), info_done_cb, NULL);
    if (v) free_paths(v, n);
    du_text();
}

/* -------------------------------------------------------------------------- */
/* Copying, moving, deleting: the progress                                     */
/* -------------------------------------------------------------------------- */

static const char *op_verb(int kind, int state)
{
    if (state == OPS_PREP) return _("Preparando…");
    switch (kind) {
    case OP_MOVE: return _("Moviendo…");
    case OP_DUP: return _("Duplicando…");
    case OP_DELETE: return _("Borrando…");
    default: return _("Copiando…");
    }
}

static void op_hide_cb(lv_event_t *e) { (void)e; S.op_hidden = true; overlay_close(); }
static void op_cancel_cb(lv_event_t *e) { (void)e; OP.cancel = true; }

static void op_sheet(void)
{
    overlay_open(false);
    int32_t w = U.land ? 640 : U.W - 64;
    lv_obj_t *c = sheet_card(w);
    lv_obj_set_style_pad_all(c, 30, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_center(c);
    U.op_title = text(c, "", aos_font_title, AOS_C_TEXT);
    U.op_cur = text(c, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_width(U.op_cur, w - 60);
    lv_label_set_long_mode(U.op_cur, LV_LABEL_LONG_MODE_DOTS);
    U.op_bar = lv_bar_create(c);
    lv_obj_set_size(U.op_bar, w - 60, 14);
    lv_bar_set_range(U.op_bar, 0, 1000);
    lv_obj_set_style_bg_color(U.op_bar, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_color(U.op_bar, C_TINT, LV_PART_INDICATOR);
    U.op_detail = text(c, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(U.op_detail, w - 60);
    lv_label_set_long_mode(U.op_detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *row = box(c, w - 60, 96);
    lv_obj_set_style_pad_top(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    pill(row, NULL, _("Seguir usando"), AOS_C_CARD2, AOS_C_TEXT, op_hide_cb, NULL);
    pill(row, NULL, _("Cancelar"), AOS_C_RED, lv_color_white(), op_cancel_cb, NULL);
}

static void op_poll(void)
{
    lk();
    int state = OP.state, kind = OP.kind;
    uint64_t bt = OP.bytes_total, bd = OP.bytes_done;
    uint32_t it = OP.items_total, id = OP.items_done, seq = OP.seq, t0 = OP.t0;
    char cur[NAME_LEN], err[200];
    scpy(cur, sizeof cur, OP.cur);
    scpy(err, sizeof err, OP.err);
    int errors = OP.errors, done_top = OP.done_top;
    bool cancelled = OP.cancelled;
    ulk();
    bool running = state == OPS_PREP || state == OPS_RUN;
    int permille = 0;
    if (state == OPS_RUN) {
        /* a copy weighs its bytes and its files: creating a file on FAT costs
         * about what writing 32 KB does, and a folder of empty files would
         * otherwise sit at 0 % to the end */
        bool bytes = kind == OP_COPY || kind == OP_DUP;
        uint64_t tot = (bytes ? bt : 0) + (uint64_t)it * 32768u, done = (bytes ? bd : 0) + (uint64_t)id * 32768u;
        permille = tot ? (int)(done * 1000 / tot) : 0;
        if (permille > 1000) permille = 1000;
    }
    if (running && U.op_bar) {
        lv_label_set_text(U.op_title, op_verb(kind, state));
        char nm[NAME_LEN];
        disp(nm, sizeof nm, cur);
        lv_label_set_text(U.op_cur, nm);
        lv_bar_set_value(U.op_bar, permille, LV_ANIM_OFF);
        char a[24], b[24], c1[16], c2[16], sp[24] = "", d[128];
        fmt_count(c1, sizeof c1, id);
        fmt_count(c2, sizeof c2, it);
        if (kind == OP_COPY || kind == OP_DUP) {
            fmt_size(a, sizeof a, bd);
            fmt_size(b, sizeof b, bt);
            uint32_t ms = now_ms() - t0;
            if (ms > 1000 && bd) {
                fmt_size(sp, sizeof sp, bd * 1000 / ms);
                strcat(sp, "/s");
            }
            snprintf(d, sizeof d, _("%s de %s · %s de %s%s%s"), a, b, c1, c2, sp[0] ? " · " : "", sp);
        } else {
            snprintf(d, sizeof d, _("%s de %s"), c1, c2);
        }
        lv_label_set_text(U.op_detail, d);
    }
    if (U.pill) {
        bool show = running && (S.op_hidden || !U.op_bar) && !U.overlay;
        lv_obj_set_flag(U.pill, LV_OBJ_FLAG_HIDDEN, !show);
        if (show) {
            lv_label_set_text_fmt(U.pill_lbl, "%s %d %%", op_verb(kind, state), permille / 10);
            lv_bar_set_value(U.pill_bar, permille, LV_ANIM_OFF);
        }
    }
    if (state == OPS_DONE && seq != S.op_seen) {
        S.op_seen = seq;
        if (U.op_bar) overlay_close();
        char msg[240], c[16];
        fmt_count(c, sizeof c, done_top);
        if (cancelled) scpy(msg, sizeof msg, _("Cancelado"));
        else if (errors > 1) snprintf(msg, sizeof msg, _("%s (y %d problemas más)"), err, errors - 1);
        else if (errors) scpy(msg, sizeof msg, err);
        else {
            static const char *const ONE[] = { N_("Copiado"), N_("Movido"), N_("Duplicado"), N_("Borrado") };
            static const char *const MANY[] = { N_("%s elementos copiados"), N_("%s elementos movidos"),
                                                N_("%s elementos duplicados"), N_("%s elementos borrados") };
            if (done_top == 1) scpy(msg, sizeof msg, aos_tr(ONE[kind & 3]));
            else snprintf(msg, sizeof msg, aos_tr(MANY[kind & 3]), c);
        }
        aos_ui_toast(msg, errors ? 3500 : 1800);
        if (!U.viewing) relist();
        usage_request();
    }
}

/* -------------------------------------------------------------------------- */
/* The viewer: text, CSV, hex                                                  */
/* -------------------------------------------------------------------------- */

enum { VM_TEXT, VM_CSV, VM_HEX };
#define VPOOL       64
#define ROW_BYTES   2048                        /* what a row reads at most */
#define CELL_CHARS  64

typedef struct {
    lv_obj_t *obj, *lbl;
    lv_obj_t *cell[CSV_MAXC];
    int k;
} vrow_t;

static struct {
    char      path[PATH_LEN];
    int       kind, mode;
    bool      wrap;
    FILE     *f;
    uint32_t  size;
    uint8_t  *blk;
    uint32_t  blk_off, blk_len;
    index_t  *ix;
    int       ix_n;                             /* snapshots of the index */
    bool      ix_done, ix_trunc;
    uint32_t  ix_pos, ix_lines, ix_maxc, built_maxc;
    int       cols, bpr;
    int32_t   chw, rh, lw, lh, cw;              /* cw: the content's width */
    int32_t   list_y;
    char      delim;
    int       ncols;
    int32_t   colx[CSV_MAXC + 1];
    bool      numeric[CSV_MAXC];
    bool      laid;
    char    (*hdr)[48];                         /* CSV_MAXC, with the file */
    char     *raw, *out;                        /* a row's bytes, and as drawn */
    int       chart_col, xcol;
    chart_t  *cj;
    bool      chart_on;
    lv_obj_t *list, *spacer, *info, *hdr_box, *hdr_row, *wrap_chip, *chart_chip;
    lv_obj_t *chart_box, *chart, *chart_hi, *chart_lo, *chart_info, *chart_chips;
    lv_chart_series_t *ser;
    vrow_t   *rows;                             /* VPOOL, with the file */
    int       nrows;
    uint32_t  keep_off;                         /* UINT32_MAX none */
    bool      want_end;
    int32_t   ret_scroll;
} VW;
/* Not initialised in place, so that it is .bss and goes to PSRAM (psram.lf). */
__attribute__((constructor)) static void VW_defaults(void) { VW.keep_off = UINT32_MAX; }

/* Bytes of the file through a 32 KB window: a row never reads the card
 * unless it falls outside what is already here. */
static uint32_t vw_read(uint32_t off, uint32_t len, char *out)
{
    if (!VW.f || off >= VW.size) return 0;
    if (len > VW.size - off) len = VW.size - off;
    if (off < VW.blk_off || off + len > VW.blk_off + VW.blk_len) {
        uint32_t start = off > 4096 ? (off - 4096) & ~4095u : 0;
        if (fseek(VW.f, (long)start, SEEK_SET) != 0) return 0;
        VW.blk_off = start;
        VW.blk_len = (uint32_t)fread(VW.blk, 1, VBLK, VW.f);
        if (off >= VW.blk_off + VW.blk_len) return 0;
        if (off + len > VW.blk_off + VW.blk_len) len = VW.blk_off + VW.blk_len - off;
    }
    memcpy(out, VW.blk + (off - VW.blk_off), len);
    return len;
}

/* What the monospaced font can draw: JetBrains Mono carries ASCII, Latin-1,
 * the bullet, the ellipsis and the box-drawing blocks. A tab is four spaces,
 * as the index counted it. */
static bool mono_has(uint32_t cp)
{
    return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF) || cp == 0x2022 || cp == 0x2026 ||
           (cp >= 0x2500 && cp <= 0x259F);
}

static void mono_safe(char *out, size_t n, const char *in, size_t len)
{
    size_t o = 0, i = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (i < len && o + 5 < n) {
        unsigned c = p[i];
        if (c == '\t') { for (int k = 0; k < 4 && o + 1 < n; k++) out[o++] = ' '; i++; continue; }
        if (c < 0x20 || c == 0x7F) { i++; continue; }
        if (c < 0x80) { out[o++] = (char)c; i++; continue; }
        int l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        uint32_t cp = l == 4 ? c & 7 : l == 3 ? c & 15 : c & 31;
        bool ok = l && i + (size_t)l <= len;
        for (int k = 1; ok && k < l; k++) {
            if ((p[i + k] & 0xC0) != 0x80) ok = false;
            else cp = cp << 6 | (p[i + k] & 0x3F);
        }
        if (ok && mono_has(cp)) { memcpy(out + o, p + i, (size_t)l); o += (size_t)l; i += (size_t)l; }
        else { out[o++] = '?'; i += ok ? (size_t)l : 1; }
    }
    out[o] = 0;
}

/* The bytes of row k of the index, without its line ending. */
static uint32_t vw_row_bytes(int k, char *buf, uint32_t cap)
{
    if (!VW.ix || k >= VW.ix_n) return 0;
    uint32_t a = IX(VW.ix, k);
    uint32_t b = k + 1 < VW.ix_n ? IX(VW.ix, k + 1) : VW.size;
    uint32_t len = b > a ? b - a : 0;
    if (len > cap) len = cap;
    len = vw_read(a, len, buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    return len;
}

static int vw_total(void)
{
    if (VW.mode == VM_HEX) return VW.bpr ? (int)((VW.size + (uint32_t)VW.bpr - 1) / (uint32_t)VW.bpr) : 0;
    int n = VW.ix_done ? VW.ix_n : VW.ix_n - 1;
    if (VW.mode == VM_CSV) n--;                 /* the header is not a row */
    return n > 0 ? n : 0;
}

/* A number of a chart: as many decimals as the spread of the column needs
 * (3 to 7 V is "7,00", not "7,000", which reads as seven thousand). */
static double s_num_span = 1;

static void vw_fmt_num(char *out, size_t n, double v)
{
    double a = s_num_span > 1e-9 ? s_num_span : fabs(v);        /* a flat column: by its size */
    int dec = a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : 3;
    snprintf(out, n, "%.*f", dec, v);
    comma(out);
}

static void vbind(vrow_t *r, int k)
{
    r->k = k;
    if (k < 0 || k >= vw_total()) { lv_obj_add_flag(r->obj, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(r->obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(r->obj, 0, k * VW.rh);
    char *raw = VW.raw, *out = VW.out;
    const size_t out_len = ROW_BYTES * 2;
    if (VW.mode == VM_HEX) {
        uint8_t b[16];
        uint32_t off = (uint32_t)k * (uint32_t)VW.bpr;
        uint32_t n = vw_read(off, (uint32_t)VW.bpr, (char *)b);
        int o = snprintf(out, out_len, "%08X  ", (unsigned)off);
        for (int i = 0; i < VW.bpr; i++) o += i < (int)n ? snprintf(out + o, out_len - (size_t)o, "%02X ", b[i]) : snprintf(out + o, out_len - (size_t)o, "   ");
        out[o++] = ' ';
        for (uint32_t i = 0; i < n; i++) out[o++] = b[i] >= 0x20 && b[i] < 0x7F ? (char)b[i] : '.';
        out[o] = 0;
        lv_label_set_text(r->lbl, out);
        return;
    }
    if (VW.mode == VM_TEXT) {
        uint32_t n = vw_row_bytes(k, raw, ROW_BYTES);
        mono_safe(out, out_len, raw, n);
        lv_label_set_text(r->lbl, out);
        /* a wrapped line's continuation gets a bar on its left, so the lines
         * of the file still read as lines */
        char before = '\n';
        uint32_t a = IX(VW.ix, k);
        if (VW.wrap && a) vw_read(a - 1, 1, &before);
        lv_obj_set_style_border_width(r->obj, before != '\n' ? 3 : 0, 0);
        return;
    }
    /* CSV: row k is line k + 1 */
    uint32_t n = vw_row_bytes(k + 1, raw, ROW_BYTES);
    raw[n] = 0;
    char *cell[CSV_MAXC];
    int nc = csv_split(raw, VW.delim, cell, CSV_MAXC);
    lv_obj_set_style_bg_opa(r->obj, k & 1 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    for (int i = 0; i < VW.ncols; i++) {
        if (!r->cell[i]) continue;
        char c[CELL_CHARS * 2 + 8];
        if (i < nc) {
            char cut[CELL_CHARS + 1];
            scpy(cut, sizeof cut, cell[i]);
            img_text(c, sizeof c, cut);
        } else c[0] = 0;
        lv_label_set_text(r->cell[i], c);
    }
}

static void vw_refresh(bool force)
{
    if (!VW.list || !VW.nrows) return;
    int32_t sy = lv_obj_get_scroll_y(VW.list);
    int first = sy / VW.rh - 1;
    if (first < 0) first = 0;
    for (int j = 0; j < VW.nrows; j++) {
        int k = first + j;
        vrow_t *r = &VW.rows[k % VW.nrows];
        if (force || r->k != k) vbind(r, k);
    }
}

static void vw_scroll_cb(lv_event_t *e)
{
    (void)e;
    if (VW.hdr_row) lv_obj_set_x(VW.hdr_row, -lv_obj_get_scroll_x(VW.list));
    vw_refresh(false);
}

static void vw_layout(void)
{
    if (!VW.spacer) return;
    int32_t h = vw_total() * VW.rh + 40;
    lv_obj_set_pos(VW.spacer, VW.cw > VW.lw ? VW.cw - 1 : 0, h);
}

static void vw_info(void)
{
    if (!VW.info) return;
    char t[200], sz[24], c[16];
    fmt_size(sz, sizeof sz, VW.size);
    if (VW.mode == VM_HEX) {
        snprintf(t, sizeof t, _("%s · se muestra en hexadecimal"), sz);
    } else if (VW.mode == VM_CSV) {
        fmt_count(c, sizeof c, vw_total());
        snprintf(t, sizeof t, _("%s filas · %d columnas · %s"), c, VW.ncols, sz);
    } else {
        fmt_count(c, sizeof c, (long)VW.ix_lines);
        if (VW.ix_lines == 1) snprintf(t, sizeof t, _("1 línea · %s"), sz);
        else snprintf(t, sizeof t, _("%s líneas · %s"), c, sz);
    }
    if (VW.mode != VM_HEX && !VW.ix_done) {
        size_t k = strlen(t);
        snprintf(t + k, sizeof t - k, _(" · leyendo %d %%"), VW.size ? (int)((uint64_t)VW.ix_pos * 100 / VW.size) : 100);
    }
    if (VW.ix_trunc) {
        size_t k = strlen(t);
        snprintf(t + k, sizeof t - k, "%s", _(" · se muestra el primer millón de filas"));
    }
    lv_label_set_text(VW.info, t);
}

static void vrows_build(void)
{
    for (int j = 0; j < VW.nrows; j++) if (VW.rows[j].obj) lv_obj_delete(VW.rows[j].obj);
    memset(VW.rows, 0, VPOOL * sizeof *VW.rows);
    VW.nrows = VW.lh / VW.rh + 3;
    if (VW.nrows > VPOOL) VW.nrows = VPOOL;
    for (int j = 0; j < VW.nrows; j++) {
        vrow_t *r = &VW.rows[j];
        r->k = -1;
        r->obj = box(VW.list, VW.cw, VW.rh);
        lv_obj_remove_flag(r->obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r->obj, LV_OBJ_FLAG_HIDDEN);
        if (VW.mode == VM_CSV) {
            lv_obj_set_style_bg_color(r->obj, lv_color_hex(0x121214), 0);
            for (int i = 0; i < VW.ncols; i++) {
                r->cell[i] = text(r->obj, "", aos_font_caption, AOS_C_TEXT);
                lv_label_set_long_mode(r->cell[i], LV_LABEL_LONG_MODE_DOTS);
                lv_obj_set_width(r->cell[i], VW.colx[i + 1] - VW.colx[i] - 20);
                lv_obj_set_pos(r->cell[i], VW.colx[i], (VW.rh - 26) / 2);
                if (VW.numeric[i]) lv_obj_set_style_text_align(r->cell[i], LV_TEXT_ALIGN_RIGHT, 0);
            }
        } else {
            lv_obj_set_style_border_side(r->obj, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(r->obj, lv_color_hex(0x3A3A3C), 0);
            r->lbl = text(r->obj, "", &aos_mono_18, lv_color_hex(0xE5E5EA));
            lv_label_set_long_mode(r->lbl, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_width(r->lbl, VW.cw - 24);
            lv_obj_set_pos(r->lbl, 16, 0);
        }
    }
}

/* CSV: columns measured on the first 200 rows, numbers to the right. */
static void vw_csv_layout(void)
{
    char *raw = VW.raw;
    int sample = VW.ix_n < 201 ? VW.ix_n : 201;
    int32_t wmax[CSV_MAXC] = { 0 };
    int nnum[CSV_MAXC] = { 0 }, nval[CSV_MAXC] = { 0 };
    VW.ncols = 0;
    for (int k = 0; k < sample; k++) {
        uint32_t n = vw_row_bytes(k, raw, ROW_BYTES);
        raw[n] = 0;
        char *cell[CSV_MAXC];
        int nc = csv_split(raw, VW.delim, cell, CSV_MAXC);
        if (k == 0) {
            VW.ncols = nc;
            for (int i = 0; i < nc; i++) {
                char cut[CELL_CHARS + 1];
                scpy(cut, sizeof cut, cell[i]);
                img_text(VW.hdr[i], sizeof VW.hdr[0], cut);
                wmax[i] = text_w(VW.hdr[i], aos_font_label);
            }
            continue;
        }
        for (int i = 0; i < nc && i < VW.ncols; i++) {
            double d;
            if (cell[i][0]) { nval[i]++; if (parse_num(cell[i], &d)) nnum[i]++; }
            char cut[CELL_CHARS + 1];
            scpy(cut, sizeof cut, cell[i]);
            int32_t w = text_w(cut, aos_font_caption);
            if (w > wmax[i]) wmax[i] = w;
        }
    }
    if (VW.ncols < 1) VW.ncols = 1;
    VW.colx[0] = 16;
    for (int i = 0; i < VW.ncols; i++) {
        int32_t w = wmax[i] + 36;
        if (w < 90) w = 90;
        if (w > 420) w = 420;
        VW.colx[i + 1] = VW.colx[i] + w;
        VW.numeric[i] = nval[i] > 0 && nnum[i] * 10 >= nval[i] * 8;
    }
    VW.cw = VW.colx[VW.ncols] + 16 > VW.lw ? VW.colx[VW.ncols] + 16 : VW.lw;
    VW.laid = true;
    /* the default column for the chart: the first number that is not the clock */
    VW.chart_col = VW.xcol = -1;
    for (int i = 0; i < VW.ncols; i++) {
        if (!VW.numeric[i]) continue;
        char f[48];
        fold(f, sizeof f, VW.hdr[i]);
        bool clock = !strcmp(f, "seconds") || !strcmp(f, "segundos") || !strcmp(f, "s") || !strcmp(f, "t") ||
                     !strcmp(f, "time") || !strcmp(f, "ms") || !strcmp(f, "tiempo");
        if (clock && VW.xcol < 0) VW.xcol = i;
        else if (!clock && VW.chart_col < 0) VW.chart_col = i;
    }
    if (VW.chart_col < 0) VW.chart_col = VW.xcol;
}

static void vw_header(void)
{
    if (!VW.hdr_box) return;
    lv_obj_clean(VW.hdr_box);
    VW.hdr_row = box(VW.hdr_box, VW.cw, 52);
    for (int i = 0; i < VW.ncols; i++) {
        lv_obj_t *l = text(VW.hdr_row, VW.hdr[i], aos_font_label, AOS_C_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, VW.colx[i + 1] - VW.colx[i] - 20);
        lv_obj_set_pos(l, VW.colx[i], 13);
        if (VW.numeric[i]) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_set_x(VW.hdr_row, VW.list ? -lv_obj_get_scroll_x(VW.list) : 0);
}

/* ---- the chart of a column ---- */

static void chart_start(void)
{
    if (VW.cj) { job_drop(&VW.cj->job); VW.cj = NULL; }
    if (VW.chart_col < 0) return;
    chart_t *c = calloc(1, sizeof *c);
    if (!c) return;
    c->job.free_fn = chart_free;
    scpy(c->path, sizeof c->path, VW.path);
    c->col = VW.chart_col;
    c->xcol = VW.xcol;
    c->delim = VW.delim;
    VW.cj = c;
    job_run("fs_chart", chart_thread, &c->job, 6144);
    if (VW.chart_info) lv_label_set_text(VW.chart_info, _("Leyendo la columna…"));
}

static void chart_show(void)
{
    chart_t *c = VW.cj;
    if (!c || !VW.chart || !job_done(&c->job)) return;
    char a[32], b[32], m[32], n[16], t[200];
    if (!c->count) {
        lv_chart_set_point_count(VW.chart, 2);
        lv_label_set_text(VW.chart_info, _("Esa columna no tiene números"));
        lv_label_set_text(VW.chart_hi, "");
        lv_label_set_text(VW.chart_lo, "");
        return;
    }
    double lo = c->vmin, hi = c->vmax;
    if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
    lv_chart_set_point_count(VW.chart, (uint32_t)(c->npts > 1 ? c->npts : 2));
    int32_t *y = lv_chart_get_series_y_array(VW.chart, VW.ser);
    for (int i = 0; i < c->npts; i++) y[i] = (int32_t)lround((c->pts[i] - lo) * 1000.0 / (hi - lo));
    if (c->npts == 1) y[1] = y[0];
    lv_chart_set_axis_range(VW.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_refresh(VW.chart);
    s_num_span = c->vmax - c->vmin;
    vw_fmt_num(a, sizeof a, c->vmin);
    vw_fmt_num(b, sizeof b, c->vmax);
    vw_fmt_num(m, sizeof m, c->vsum / c->count);
    fmt_count(n, sizeof n, c->count);
    lv_label_set_text(VW.chart_hi, b);
    lv_label_set_text(VW.chart_lo, a);
    if (c->has_x) {
        char x0[32], x1[32];
        s_num_span = fabs(c->x1 - c->x0);
        vw_fmt_num(x0, sizeof x0, c->x0);
        vw_fmt_num(x1, sizeof x1, c->x1);
        snprintf(t, sizeof t, _("mín %s · máx %s · media %s · %s valores, de %s a %s %s"), a, b, m, n, x0, x1,
                 VW.xcol >= 0 ? VW.hdr[VW.xcol] : "");
    } else {
        snprintf(t, sizeof t, _("mín %s · máx %s · media %s · %s valores"), a, b, m, n);
    }
    lv_label_set_text(VW.chart_info, t);
}

static void chart_col_cb(lv_event_t *e);

static void chart_close(void)
{
    if (VW.chart_box) lv_obj_delete(VW.chart_box);
    VW.chart_box = VW.chart = VW.chart_hi = VW.chart_lo = VW.chart_info = VW.chart_chips = NULL;
    VW.chart_on = false;
    if (VW.chart_chip) chip_set(VW.chart_chip, false);
}

static void chart_close_cb(lv_event_t *e) { (void)e; chart_close(); }

static void chart_panel(void)
{
    if (VW.chart_box) lv_obj_delete(VW.chart_box);
    VW.chart_on = true;
    if (VW.chart_chip) chip_set(VW.chart_chip, true);
    int32_t w = VW.lw - 32, h = U.land ? 470 : 600;
    if (h > VW.lh - 16) h = VW.lh - 16;
    VW.chart_box = box(U.root, w, h);
    lv_obj_set_pos(VW.chart_box, 16, VW.list_y + 8);
    lv_obj_set_style_bg_color(VW.chart_box, C_SHEET, 0);
    lv_obj_set_style_bg_opa(VW.chart_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(VW.chart_box, AOS_UI_RADIUS, 0);
    lv_obj_set_style_shadow_width(VW.chart_box, 40, 0);
    lv_obj_set_style_shadow_opa(VW.chart_box, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(VW.chart_box, 20, 0);
    lv_obj_add_flag(VW.chart_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = text(VW.chart_box, _("Gráfico"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_align(round_btn(VW.chart_box, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT, AOS_C_CARD2, 56, chart_close_cb, NULL),
                 LV_ALIGN_TOP_RIGHT, 0, 0);
    VW.chart_chips = box(VW.chart_box, w - 40, 76);
    lv_obj_set_pos(VW.chart_chips, 0, 66);
    lv_obj_add_flag(VW.chart_chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(VW.chart_chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(VW.chart_chips, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(VW.chart_chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(VW.chart_chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(VW.chart_chips, 10, 0);
    for (int i = 0; i < VW.ncols; i++)
        if (VW.numeric[i] && i != VW.xcol) chip(VW.chart_chips, VW.hdr[i], i == VW.chart_col, chart_col_cb, (void *)(intptr_t)i);
    int32_t ch = h - 40 - 150 - 70;
    VW.chart = lv_chart_create(VW.chart_box);
    lv_obj_set_size(VW.chart, w - 40 - 110, ch);
    lv_obj_set_pos(VW.chart, 110, 150);
    lv_obj_set_style_bg_color(VW.chart, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(VW.chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(VW.chart, 0, 0);
    lv_obj_set_style_radius(VW.chart, 16, 0);
    lv_obj_set_style_pad_all(VW.chart, 12, 0);
    lv_obj_set_style_line_color(VW.chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(VW.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(VW.chart, 5, 0);
    lv_obj_set_style_size(VW.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(VW.chart, 3, LV_PART_ITEMS);
    lv_chart_set_point_count(VW.chart, 2);
    VW.ser = lv_chart_add_series(VW.chart, AOS_C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(VW.chart, VW.ser, LV_CHART_POINT_NONE);
    VW.chart_hi = text(VW.chart_box, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(VW.chart_hi, 100);
    lv_obj_set_pos(VW.chart_hi, 0, 154);
    VW.chart_lo = text(VW.chart_box, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(VW.chart_lo, 100);
    lv_obj_set_pos(VW.chart_lo, 0, 150 + ch - 28);
    VW.chart_info = text(VW.chart_box, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(VW.chart_info, w - 40);
    lv_label_set_long_mode(VW.chart_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(VW.chart_info, 0, 150 + ch + 14);
    if (!VW.cj || VW.cj->col != VW.chart_col) chart_start();
    chart_show();
}

static void chart_col_cb(lv_event_t *e)
{
    VW.chart_col = (int)(intptr_t)lv_event_get_user_data(e);
    chart_start();
    chart_panel();
}

static void chart_cb(lv_event_t *e)
{
    (void)e;
    if (VW.chart_on) chart_close();
    else chart_panel();
}

/* ---- the viewer's screen ---- */

static void vw_back_cb(lv_event_t *e) { (void)e; viewer_back(); }

static void wrap_cb(lv_event_t *e)
{
    (void)e;
    VW.wrap = !VW.wrap;
    /* where we were, to come back to it with the new rows */
    int32_t sy = VW.list ? lv_obj_get_scroll_y(VW.list) : 0;
    int k = sy / VW.rh;
    VW.keep_off = VW.ix && k < VW.ix_n ? IX(VW.ix, k) : 0;
    if (VW.ix) { job_drop(&VW.ix->job); VW.ix = NULL; }
    build();
}

static void end_cb(lv_event_t *e)
{
    (void)e;
    if (VW.mode != VM_HEX && !VW.ix_done) {
        VW.want_end = true;
        aos_ui_toast(_("Leyendo el archivo hasta el final…"), 1400);
        return;
    }
    lv_obj_update_layout(VW.list);
    lv_obj_scroll_to_y(VW.list, LV_COORD_MAX, LV_ANIM_OFF);
    vw_refresh(false);
}

static void top_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_scroll_to_y(VW.list, 0, LV_ANIM_OFF);
    vw_refresh(false);
}

static uint32_t vw_top_off(void)
{
    if (!VW.list || !VW.rh) return 0;
    int k = lv_obj_get_scroll_y(VW.list) / VW.rh;
    if (VW.mode == VM_HEX) return (uint32_t)k * (uint32_t)VW.bpr;
    if (VW.mode == VM_CSV) k++;
    return VW.ix && k < VW.ix_n ? IX(VW.ix, k) : 0;
}

static void viewer_build(void)
{
    int32_t W = U.W, H = U.H;
    /* build() has just cleaned the root: every object of before is gone */
    VW.list = VW.spacer = VW.info = VW.hdr_box = VW.hdr_row = VW.wrap_chip = VW.chart_chip = NULL;
    VW.chart_box = VW.chart = VW.chart_hi = VW.chart_lo = VW.chart_info = VW.chart_chips = NULL;
    VW.ser = NULL;
    VW.nrows = 0;
    lv_obj_t *top = box(U.root, W, 88);
    lv_obj_set_style_pad_hor(top, 8, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 10, 0);
    link_btn(top, AOS_SYM_CHEVRON_LEFT, U.land ? folder_title() : NULL, aos_font_body, vw_back_cb, NULL);
    char nm[NAME_LEN];
    disp(nm, sizeof nm, base_of(VW.path));
    lv_obj_t *t = text(top, nm, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, lv_font_get_line_height(aos_font_body));      /* one line, cut */
    lv_obj_set_flex_grow(t, 1);
    VW.wrap_chip = VW.chart_chip = NULL;
    if (VW.mode == VM_TEXT) VW.wrap_chip = chip(top, _("Ajustar"), VW.wrap, wrap_cb, NULL);
    if (VW.mode == VM_CSV) {
        VW.chart_chip = chip(top, _("Gráfico"), false, chart_cb, NULL);
        lv_obj_add_flag(VW.chart_chip, LV_OBJ_FLAG_HIDDEN);
    }
    round_btn(top, AOS_SYM_CHEVRON_UP, &aos_sym_28, AOS_C_TEXT, AOS_C_CARD2, 60, top_cb, NULL);
    round_btn(top, AOS_SYM_CHEVRON_DOWN, &aos_sym_28, AOS_C_TEXT, AOS_C_CARD2, 60, end_cb, NULL);
    VW.info = text(U.root, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(VW.info, W - 48);
    lv_label_set_long_mode(VW.info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(VW.info, 24, 88);
    int32_t y = 128;
    VW.hdr_box = VW.hdr_row = NULL;
    if (VW.mode == VM_CSV) {
        VW.hdr_box = box(U.root, W, 52);
        lv_obj_set_pos(VW.hdr_box, 0, y);
        lv_obj_set_style_bg_color(VW.hdr_box, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(VW.hdr_box, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(VW.hdr_box, true, 0);
        y += 52;
    }
    VW.list = box(U.root, W, H - y);
    lv_obj_set_pos(VW.list, 0, y);
    lv_obj_add_flag(VW.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(VW.list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(VW.list, vw_scroll_cb, LV_EVENT_SCROLL, NULL);
    VW.lw = W;
    VW.lh = H - y;
    VW.list_y = y;
    VW.chw = lv_font_get_glyph_width(&aos_mono_18, 'M', 'M');
    if (VW.chw < 6) VW.chw = 11;
    VW.rh = VW.mode == VM_CSV ? 56 : 30;
    VW.cols = (VW.lw - 40) / VW.chw;
    VW.bpr = VW.cols >= 75 ? 16 : 8;
    /* the index: from the start again if the width it was made for changed */
    int ixmode = VW.mode == VM_TEXT && VW.wrap ? IX_WRAP : IX_LINES;
    if (VW.mode != VM_HEX && (!VW.ix || VW.ix->mode != ixmode || (ixmode == IX_WRAP && VW.ix->cols != VW.cols))) {
        if (VW.ix) job_drop(&VW.ix->job);
        VW.ix = index_start(VW.path, VW.size, ixmode, VW.cols);
        VW.ix_n = 0;
        VW.ix_done = false;
    }
    bool nowrap = VW.mode == VM_TEXT && !VW.wrap;
    VW.cw = VW.lw;
    VW.built_maxc = VW.ix_maxc;
    if (nowrap) {
        int32_t need = (int32_t)(VW.ix_maxc > 400 ? 400 : VW.ix_maxc) * VW.chw + 40;
        if (need > VW.cw) VW.cw = need;
    }
    if (VW.mode == VM_CSV && VW.laid) VW.cw = VW.colx[VW.ncols] + 16 > VW.lw ? VW.colx[VW.ncols] + 16 : VW.lw;
    lv_obj_set_scroll_dir(VW.list, VW.mode == VM_CSV || nowrap ? LV_DIR_ALL : LV_DIR_VER);
    VW.spacer = box(VW.list, 1, 1);
    VW.nrows = 0;
    memset(VW.rows, 0, VPOOL * sizeof *VW.rows);
    if (VW.mode != VM_CSV || VW.laid) vrows_build();
    if (VW.mode == VM_CSV && VW.laid) {
        vw_header();
        bool any = false;
        for (int i = 0; i < VW.ncols; i++) any |= VW.numeric[i] && i != VW.xcol;
        lv_obj_set_flag(VW.chart_chip, LV_OBJ_FLAG_HIDDEN, !any);
    }
    if (VW.mode == VM_HEX && VW.keep_off != UINT32_MAX) {
        lv_obj_update_layout(VW.list);
        vw_layout();
        lv_obj_update_layout(VW.list);
        lv_obj_scroll_to_y(VW.list, (int32_t)(VW.keep_off / (uint32_t)VW.bpr) * VW.rh, LV_ANIM_OFF);
        VW.keep_off = UINT32_MAX;
    }
    vw_layout();
    vw_info();
    vw_refresh(true);
    if (VW.chart_on) chart_panel();
}

static void viewer_poll(void)
{
    if (!U.viewing || !VW.ix) return;
    index_t *x = VW.ix;
    lk();
    int n = x->n;
    bool done = x->job.done;
    uint32_t pos = x->pos, lines = x->lines, maxc = x->maxcols;
    bool trunc = x->truncated;
    ulk();
    bool changed = n != VW.ix_n || done != VW.ix_done;
    VW.ix_n = n;
    VW.ix_done = done;
    VW.ix_pos = pos;
    VW.ix_lines = lines;
    VW.ix_trunc = trunc;
    VW.ix_maxc = maxc;
    if (VW.mode == VM_TEXT && !VW.wrap && done && maxc != VW.built_maxc) {
        {
            /* the widest line is known: the rows get as wide */
            int32_t sy = lv_obj_get_scroll_y(VW.list);
            build();
            lv_obj_update_layout(VW.list);
            lv_obj_scroll_to_y(VW.list, sy, LV_ANIM_OFF);
            vw_refresh(true);
            return;
        }
    }
    if (VW.mode == VM_CSV && !VW.laid && (n > 201 || done)) {
        vw_csv_layout();
        build();
        return;
    }
    if (!changed) { if (VW.cj && VW.chart && job_done(&VW.cj->job) && lv_chart_get_point_count(VW.chart) == 2) chart_show(); return; }
    vw_layout();
    vw_info();
    if (VW.keep_off != UINT32_MAX && (done || (n && IX(x, n - 1) >= VW.keep_off))) {
        /* back to the row that holds the byte that was on top */
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (IX(x, mid) <= VW.keep_off) lo = mid;
            else hi = mid - 1;
        }
        if (VW.mode == VM_CSV) lo--;
        lv_obj_update_layout(VW.list);
        lv_obj_scroll_to_y(VW.list, (lo > 0 ? lo : 0) * VW.rh, LV_ANIM_OFF);
        VW.keep_off = UINT32_MAX;
    }
    if (VW.want_end && done) {
        VW.want_end = false;
        lv_obj_update_layout(VW.list);
        lv_obj_scroll_to_y(VW.list, LV_COORD_MAX, LV_ANIM_OFF);
    }
    vw_refresh(true);
    if (VW.cj && VW.chart && job_done(&VW.cj->job)) chart_show();
}

static void viewer_free(void)
{
    if (VW.ix) job_drop(&VW.ix->job);
    if (VW.cj) job_drop(&VW.cj->job);
    if (VW.f) fclose(VW.f);
    free(VW.blk);
    free(VW.rows);
    free(VW.hdr);
    free(VW.raw);
    free(VW.out);
    memset(&VW, 0, sizeof VW);
    VW.keep_off = UINT32_MAX;
}

static void viewer_open(const char *path, int kind)
{
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) { aos_ui_toast(_("No se pudo abrir el archivo"), 1600); return; }
    int32_t ret = list_scroll();
    viewer_free();
    VW.f = fopen(path, "rb");
    VW.blk = malloc(VBLK);
    VW.rows = calloc(VPOOL, sizeof *VW.rows);
    VW.hdr = calloc(CSV_MAXC, sizeof *VW.hdr);
    VW.raw = malloc(ROW_BYTES + 4);
    VW.out = malloc(ROW_BYTES * 2);
    if (!VW.f || !VW.blk || !VW.rows || !VW.hdr || !VW.raw || !VW.out) { viewer_free(); aos_ui_toast(_("No se pudo abrir el archivo"), 1600); return; }
    scpy(VW.path, sizeof VW.path, path);
    VW.kind = kind;
    VW.size = (uint32_t)st.st_size;
    VW.ret_scroll = ret;
    VW.wrap = true;
    VW.keep_off = UINT32_MAX;
    /* text or not: NUL bytes, or too many control characters, in the first 4 KB */
    VW.blk_off = 0;
    VW.blk_len = (uint32_t)fread(VW.blk, 1, VBLK, VW.f);
    uint32_t probe = VW.blk_len < 4096 ? VW.blk_len : 4096, bad = 0;
    for (uint32_t i = 0; i < probe; i++) {
        uint8_t c = VW.blk[i];
        if (c == 0) { bad = probe; break; }
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != 0x1B) bad++;
    }
    bool binary = probe && bad * 20 > probe;
    VW.mode = binary ? VM_HEX : kind == K_CSV ? VM_CSV : VM_TEXT;
    if (VW.mode == VM_CSV) {
        int nc = 0, ns = 0, nt = 0;
        for (uint32_t i = 0; i < VW.blk_len && VW.blk[i] != '\n'; i++) {
            nc += VW.blk[i] == ',';
            ns += VW.blk[i] == ';';
            nt += VW.blk[i] == '\t';
        }
        VW.delim = nt > nc && nt > ns ? '\t' : ns > nc ? ';' : ',';
    }
    kb_hide();
    overlay_close();
    U.viewing = true;
    build();
}

static void viewer_close(void)
{
    int32_t ret = VW.ret_scroll;
    viewer_free();
    U.viewing = false;
    build();
    lv_obj_update_layout(U.root);
    if (U.list) {
        lv_obj_scroll_to_y(U.list, ret, LV_ANIM_OFF);
        pool_refresh(true);
    }
}

static bool viewer_back(void)
{
    if (!U.viewing) return false;
    if (VW.chart_on) { chart_close(); return true; }
    viewer_close();
    return true;
}

/* -------------------------------------------------------------------------- */
/* The app                                                                     */
/* -------------------------------------------------------------------------- */

static uint32_t s_usage_seen;

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    U.tick++;
    if (!U.root) return;
    list_poll();
    th_poll();
    op_poll();
    du_text();
    viewer_poll();
    if (L.query_dirty && L.recursive && L.query[0] && now_ms() - L.query_ms > 400) {
        L.query_dirty = false;
        search_start();
    }
    lk();
    uint32_t us = USG.seq;
    ulk();
    if (us != s_usage_seen) {
        s_usage_seen = us;
        footer_refresh();
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    if (!s_mx) s_mx = aos_hal_mutex_create();
    if (!S.init) {
        S.init = true;
        prefs_load();
        lk();
        S.op_seen = OP.seq;
        ulk();
    }
    if (!TH && (TH = calloc(TH_N, sizeof *TH)) != NULL)
        for (int i = 0; i < TH_N; i++) TH[i].ei = -1;
    memset(&U, 0, sizeof U);
    U.self = self;
    U.root = root;
    U.ctx = -1;
    U.cells = calloc(POOL_MAX, sizeof *U.cells);
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    char abs[PATH_LEN];
    if (S.rel[0] && (!abs_of(abs, sizeof abs, S.rel) || !is_dir_path(abs))) S.rel[0] = 0;   /* the card changed */
    list_open();
    int d = rel_depth(S.rel);
    L.want_scroll = d <= WALK_DEPTH ? S.scroll[d] : 0;
    usage_request();
    build();
    U.timer = lv_timer_create(timer_cb, 40, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_delete(U.timer);
    U.timer = NULL;
    int d = rel_depth(S.rel);
    if (U.list && d <= WALK_DEPTH) S.scroll[d] = lv_obj_get_scroll_y(U.list);
    if (U.du) { job_drop(&U.du->job); U.du = NULL; }
    viewer_free();
    list_release();
    free(U.cells);
    memset(&U, 0, sizeof U);
}

static void show(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_resume(U.timer);
    /* back from somewhere else: the card may have changed meanwhile */
    if (U.tick > 2 && !U.viewing) relist();
    usage_request();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_pause(U.timer);
    kb_hide();
    th_forget_asked();          /* nobody collects them while we are away */
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self; (void)inst;
    U.root = root;
    int first = U.list && U.rh ? lv_obj_get_scroll_y(U.list) / U.rh * U.cols : 0;
    if (U.viewing) VW.keep_off = vw_top_off();
    build();
    if (!U.viewing && U.list) {
        lv_obj_update_layout(U.list);
        lv_obj_scroll_to_y(U.list, (U.cols ? first / U.cols : first) * U.rh, LV_ANIM_OFF);
        pool_refresh(true);
    }
    return true;
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.overlay) {
        bool op = U.op_bar != NULL;
        if (op) S.op_hidden = true;
        overlay_close();
        U.ctx = -1;
        return true;
    }
    if (U.kb && !lv_obj_has_flag(U.kb, LV_OBJ_FLAG_HIDDEN)) { kb_hide(); return true; }
    if (U.viewing) return viewer_back();
    if (U.select) { select_set(false); return true; }
    if (L.query[0] && U.ta) { lv_textarea_set_text(U.ta, ""); return true; }
    if (S.rel[0]) { go_up(); return true; }
    return false;
}

void aos_app_files_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = APP_ID, .name = "Archivos", .icon = AOS_SYM_FOLDER,
            .color_a = 0x60A5FA, .color_b = 0x1D4ED8,
            .flags = AOS_APP_FLAG_KEEP,
            .order = 230,
        },
        .create = create, .destroy = destroy, .show = show, .hide = hide,
        .back = back, .resize = resize,
    };
}
