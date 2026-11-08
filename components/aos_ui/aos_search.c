/*
 * P4OS - the system's search (2026-11-08), as the phone's: on the home
 * screen, a swipe down from the middle opens it (the top edge is still the
 * notifications' and the control centre's; aos_ui.c, G_SEARCH).
 *
 * A field with the keyboard up, and the results as it is typed:
 *
 *   Apps       the name, as shown (translated) or as written         open it
 *   Ajustes    the pages of Settings, by name and a few words each   open Settings on that page
 *   Cuenta     a sum typed in ("2*(3+4)", "15% de 80")              its result to the clipboard
 *   Notas      the notes' titles and text (the .md files in notas)   open Notes on it
 *   Archivos   the names of the card's files                         open Files in its folder
 *
 * The first three are found at once, on LVGL's task. Notes and files are a
 * walk of the card: a thread does it (PSRAM stack), with a cap on what it
 * looks at and on what it keeps, and hands back what it found through a
 * store under a mutex that a timer reads. Each query has a generation; a
 * thread still walking for an older one stops at its next look. Nothing is
 * indexed beforehand: nothing stays in memory while search is closed.
 *
 * Matching folds case and the Spanish accents (á -> a, ñ -> n), so "cancion"
 * finds "Canción". The apps open through aos_ui_open_app_with(): Settings
 * takes a page key, Files a path, Notes a file name (each in its show()).
 */
#include "aos_internal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_sys_glyphs.h"

#include "lvgl.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define Q_MAX           64
#define NOTES_MAX       12
#define FILES_MAX       30
#define FILE_VISITS     20000       /* entries the walk looks at, at most */
#define DIRS_MAX        2048        /* folders waiting to be looked into */
#define NOTE_READ       (48 * 1024) /* of each note, for its text */
#define APPS_MAX        12
#define DEBOUNCE_MS     220
#define PATH_N          200

/* ---- folding: lower case, Spanish accents to their letter ---- */

static size_t fold(const char *in, char *out, size_t n)
{
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && o + 1 < n) {
        unsigned c = *s++;
        if (c < 0x80) {
            out[o++] = (char)tolower((int)c);
            continue;
        }
        if ((c == 0xC3) && *s) {                /* Latin-1 letters, two bytes */
            unsigned d = *s++ | 0x20;           /* upper and lower alike */
            char b = 0;
            if (d >= 0xA0 && d <= 0xA5) b = 'a';
            else if (d == 0xA7) b = 'c';
            else if (d >= 0xA8 && d <= 0xAB) b = 'e';
            else if (d >= 0xAC && d <= 0xAF) b = 'i';
            else if (d == 0xB1) b = 'n';
            else if (d >= 0xB2 && d <= 0xB6) b = 'o';
            else if (d >= 0xB9 && d <= 0xBC) b = 'u';
            if (b) { out[o++] = b; continue; }
            if (o + 2 < n) { out[o++] = (char)c; out[o++] = (char)s[-1]; }
            continue;
        }
        out[o++] = (char)c;                     /* anything else, as it is */
    }
    out[o] = 0;
    return o;
}

/* where q starts in t: 0 at the start, 1 at a word's start, 2 inside, -1 not there */
static int match_rank(const char *t, const char *q)
{
    const char *p = strstr(t, q);
    if (!p) return -1;
    if (p == t) return 0;
    for (const char *w = t; (w = strstr(w, q)); w++)
        if (w == t || !isalnum((unsigned char)w[-1])) return 1;
    return 2;
}

/* ---- the sum ---- */

static const char *s_ep;
static bool s_eok;

static double e_expr(void);

static void e_ws(void) { while (*s_ep == ' ') s_ep++; }

static double e_num(void)
{
    e_ws();
    if (*s_ep == '(') {
        s_ep++;
        double v = e_expr();
        e_ws();
        if (*s_ep == ')') s_ep++;
        else s_eok = false;
        return v;
    }
    if (*s_ep == '-') { s_ep++; return -e_num(); }
    if (*s_ep == '+') { s_ep++; return e_num(); }
    char buf[40];
    int n = 0;
    while (n < 38 && (isdigit((unsigned char)*s_ep) || *s_ep == '.' || *s_ep == ',')) {
        buf[n++] = *s_ep == ',' ? '.' : *s_ep;
        s_ep++;
    }
    buf[n] = 0;
    if (!n) { s_eok = false; return 0; }
    return strtod(buf, NULL);
}

static double e_pow(void)
{
    double b = e_num();
    e_ws();
    if (*s_ep == '^') { s_ep++; return pow(b, e_pow()); }
    return b;
}

static double e_term(void)
{
    double v = e_pow();
    for (;;) {
        e_ws();
        if (*s_ep == '%') {                     /* "15% de 80", "15% of 80", or 15% alone */
            s_ep++;
            e_ws();
            if (!strncmp(s_ep, "de ", 3) || !strncmp(s_ep, "of ", 3)) {
                s_ep += 3;
                v = v / 100.0 * e_pow();
            } else {
                v /= 100.0;
            }
        } else if (*s_ep == '*' || *s_ep == 'x') {
            s_ep++;
            v *= e_pow();
        } else if (*s_ep == '/') {
            s_ep++;
            double d = e_pow();
            if (d == 0) s_eok = false;
            else v /= d;
        } else {
            return v;
        }
    }
}

static double e_expr(void)
{
    double v = e_term();
    for (;;) {
        e_ws();
        if (*s_ep == '+') { s_ep++; v += e_term(); }
        else if (*s_ep == '-') { s_ep++; v -= e_term(); }
        else return v;
    }
}

/* the result of a sum, or false: something that is not one */
static bool calc(const char *q, char *out, size_t n)
{
    bool op = false, digit = false;
    for (const char *p = q; *p; p++) {
        if (isdigit((unsigned char)*p)) digit = true;
        else if (strchr("+-*/^%x()", *p) && p != q) op = true;
    }
    if (!digit || !op) return false;
    s_ep = q;
    s_eok = true;
    double v = e_expr();
    e_ws();
    if (!s_eok || *s_ep || isnan(v) || isinf(v)) return false;
    if (fabs(v - llround(v)) < 1e-9 && fabs(v) < 1e15) snprintf(out, n, "%lld", (long long)llround(v));
    else {
        snprintf(out, n, "%.10g", v);
    }
    if (strcmp(aos_i18n_current(), "en"))               /* the decimal comma */
        for (char *p = out; *p; p++) if (*p == '.') *p = ',';
    return true;
}

/* ---- the pages of Settings ---- */

typedef struct {
    const char *key;            /* what Settings takes (aos_app_settings.c) */
    const char *title;          /* its title, as Settings shows it */
    const char *words;          /* folded, both languages */
} spage_t;

static const spage_t PAGES[] = {
    { "wifi",      N_("Wi-Fi"),               "wifi wi-fi red internet network contrasena password" },
    { "display",   N_("Pantalla"),            "pantalla brillo display brightness orientacion rotation girar apagar" },
    { "sound",     N_("Sonido"),              "sonido volumen sound volume parlante speaker teclado click" },
    { "wallpaper", N_("Fondo"),               "fondo wallpaper foto photo color acento accent" },
    { "lock",      N_("Pantalla de bloqueo"), "bloqueo lock codigo code pin reloj clock" },
    { "bluetooth", "Bluetooth",               "bluetooth iphone telefono phone teclado keyboard mouse" },
    { "usb",       "USB",                     "usb teclado keyboard mouse disco disk red network host pendrive" },
    { "expansion", N_("Expansión"),           "expansion header pines pins modulos modules conector" },
    { "language",  N_("Idioma"),              "idioma language ingles english aleman german espanol spanish" },
    { "time",      N_("Fecha y hora"),        "fecha hora date time zona zone reloj clock" },
    { "storage",   N_("Almacenamiento"),      "almacenamiento storage tarjeta card espacio space memoria" },
    { "update",    N_("Actualización"),       "actualizacion update firmware ota version c6" },
    { "portal",    N_("Portal web"),          "portal web contrasena password ssh acceso access https" },
    { "diag",      N_("Diagnóstico"),         "diagnostico diagnostics temperatura temperature memoria memory cpu" },
    { "developer", N_("Desarrollador"),       "desarrollador developer registro log segundo plano background" },
    { "about",     N_("Acerca de"),           "acerca about version placa board licencia license" },
};

/* ---- notes and files: the walk ---- */

typedef struct {
    char title[64];
    char file[96];              /* the note's file name */
    char snippet[96];
} note_hit_t;

typedef struct {
    char name[64];
    char path[PATH_N];          /* absolute */
    bool dir;
} file_hit_t;

typedef struct {
    void      *mx;
    uint32_t   gen;             /* the query these are for */
    uint32_t   version;         /* moves with every result added */
    bool       notes_done, files_done;
    int        nnotes, nfiles;
    note_hit_t notes[NOTES_MAX];
    file_hit_t files[FILES_MAX];
} store_t;

static store_t *s_store;         /* PSRAM, while search is open */
static volatile uint32_t s_gen;

typedef struct {
    uint32_t gen;
    char     q[Q_MAX];          /* folded */
} job_t;

static bool job_live(const job_t *j) { return j->gen == s_gen && s_store; }

static void put_note(const job_t *j, const note_hit_t *h)
{
    store_t *s = s_store;
    if (!s) return;
    aos_hal_mutex_lock(s->mx);
    if (s->gen == j->gen && s->nnotes < NOTES_MAX) {
        s->notes[s->nnotes++] = *h;
        s->version++;
    }
    aos_hal_mutex_unlock(s->mx);
}

static void put_file(const job_t *j, const file_hit_t *h)
{
    store_t *s = s_store;
    if (!s) return;
    aos_hal_mutex_lock(s->mx);
    if (s->gen == j->gen && s->nfiles < FILES_MAX) {
        s->files[s->nfiles++] = *h;
        s->version++;
    }
    aos_hal_mutex_unlock(s->mx);
}

static void set_done(const job_t *j, bool notes)
{
    store_t *s = s_store;
    if (!s) return;
    aos_hal_mutex_lock(s->mx);
    if (s->gen == j->gen) {
        if (notes) s->notes_done = true;
        else s->files_done = true;
        s->version++;
    }
    aos_hal_mutex_unlock(s->mx);
}

static bool ends_md(const char *n)
{
    size_t l = strlen(n);
    return l > 3 && !strcasecmp(n + l - 3, ".md");
}

/* a line of the note around where the query is, as it is written */
static void snippet_of(const char *text, const char *folded, const char *q, char *out, size_t n)
{
    out[0] = 0;
    const char *p = strstr(folded, q);
    if (!p) return;
    /* folding keeps most characters one for one; near enough for a glimpse */
    size_t at = (size_t)(p - folded), len = strlen(text);
    if (at > len) at = len;
    size_t from = at > 24 ? at - 24 : 0;
    while (from > 0 && ((unsigned char)text[from] & 0xC0) == 0x80) from--;
    size_t i = 0;
    if (from) { out[i++] = '.'; out[i++] = '.'; out[i++] = '.'; }
    for (const char *t = text + from; *t && i + 4 < n; t++) {
        char c = *t;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == '#' || c == '*' || c == '>' || c == '`') continue;    /* Markdown's marks */
        if (c == ' ' && i && out[i - 1] == ' ') continue;
        out[i++] = c;
    }
    while (i && ((unsigned char)out[i - 1] & 0xC0) == 0xC0) i--;        /* a character cut in two */
    out[i] = 0;
}

/* A note's text without its front matter ("---" ... "---", Notes' own
 * title, kind, colour and dates): those words are not what the person wrote */
static const char *note_body(const char *t)
{
    if (strncmp(t, "---", 3)) return t;
    const char *e = strstr(t + 3, "\n---");
    if (!e) return t;
    e += 4;
    while (*e && *e != '\n') e++;
    while (*e == '\n' || *e == '\r') e++;
    return e;
}

/* the first line with words, for a note found by its title */
static void first_line(const char *t, char *out, size_t n)
{
    while (*t == '\n' || *t == '\r' || *t == ' ' || *t == '#') t++;
    size_t i = 0;
    while (*t && *t != '\n' && i + 1 < n) out[i++] = *t++;
    while (i && ((unsigned char)out[i - 1] & 0xC0) == 0xC0) i--;
    out[i] = 0;
}

static void walk_notes(const job_t *j)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return;
    char dir[PATH_N];
    snprintf(dir, sizeof dir, "%s/notas", root);
    DIR *d = opendir(dir);
    if (!d) return;
    char *buf = malloc(NOTE_READ + 1), *fbuf = malloc(NOTE_READ + 1);
    struct dirent *e;
    while (buf && fbuf && (e = readdir(d)) && job_live(j)) {
        if (e->d_name[0] == '.' || !ends_md(e->d_name)) continue;
        char path[PATH_N + 100];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        /* the title: the name without .md */
        note_hit_t h;
        memset(&h, 0, sizeof h);
        snprintf(h.file, sizeof h.file, "%s", e->d_name);
        size_t tl = strlen(e->d_name) - 3;
        snprintf(h.title, sizeof h.title, "%.*s", (int)(tl < sizeof h.title - 1 ? tl : sizeof h.title - 1), e->d_name);
        char ft[128];
        fold(h.title, ft, sizeof ft);
        bool hit = strstr(ft, j->q) != NULL;
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t n = fread(buf, 1, NOTE_READ, f);
            fclose(f);
            buf[n] = 0;
            const char *body = note_body(buf);
            fold(body, fbuf, NOTE_READ + 1);
            if (strstr(fbuf, j->q)) {
                hit = true;
                snippet_of(body, fbuf, j->q, h.snippet, sizeof h.snippet);
            } else if (hit) {
                first_line(body, h.snippet, sizeof h.snippet);
            }
        }
        if (hit) put_note(j, &h);
    }
    closedir(d);
    free(buf);
    free(fbuf);
}

static void walk_files(const job_t *j)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return;
    char (*dirs)[PATH_N] = malloc(DIRS_MAX * sizeof *dirs);
    if (!dirs) return;
    int head = 0, tail = 0, visits = 0, found = 0;
    snprintf(dirs[tail++], PATH_N, "%s", root);
    char notas[PATH_N];         /* the notes are found as notes, not as files */
    snprintf(notas, sizeof notas, "%s/notas", root);
    while (head < tail && visits < FILE_VISITS && found < FILES_MAX && job_live(j)) {
        char dir[PATH_N];
        memcpy(dir, dirs[head++ % DIRS_MAX], PATH_N);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && visits < FILE_VISITS && found < FILES_MAX) {
            if (++visits % 256 == 0 && !job_live(j)) break;
            if (e->d_name[0] == '.') continue;
            char path[PATH_N];
            if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
            bool is_dir = false;
#ifdef DT_DIR
            if (e->d_type == DT_DIR) is_dir = true;
            else if (e->d_type == DT_UNKNOWN)
#endif
            {
                struct stat st;
                is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (is_dir && !strcmp(path, notas)) continue;
            if (is_dir && tail - head < DIRS_MAX) memcpy(dirs[tail++ % DIRS_MAX], path, PATH_N);
            char fn[128];
            fold(e->d_name, fn, sizeof fn);
            if (strstr(fn, j->q)) {
                file_hit_t h;
                snprintf(h.name, sizeof h.name, "%.63s", e->d_name);
                memcpy(h.path, path, PATH_N);
                h.dir = is_dir;
                put_file(j, &h);
                found++;
            }
        }
        closedir(d);
    }
    free(dirs);
}

static void worker(void *arg)
{
    job_t *j = arg;
    if (job_live(j)) walk_notes(j);
    set_done(j, true);
    if (job_live(j)) walk_files(j);
    set_done(j, false);
    free(j);
}

/* ---- the screen ---- */

enum { A_APP, A_PAGE, A_CALC, A_NOTE, A_FILE };

typedef struct {
    int  kind;
    char arg[PATH_N];
} action_t;

static struct {
    lv_obj_t *layer, *ta, *kb, *list;
    lv_timer_t *debounce, *poll;
    char q[Q_MAX];              /* folded, the one shown */
    uint32_t shown_version;
    action_t first;             /* what ✓ does: the first result */
    bool has_first;
} V;

static void close_cb(lv_event_t *e) { (void)e; aos_search_close(); }

static void row_deleted_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void do_action(const action_t *a)
{
    action_t act = *a;          /* the row goes with the layer */
    switch (act.kind) {
    case A_CALC:
        aos_ui_clip_set(act.arg);
        aos_ui_toast(_("Copiado"), 1000);
        return;
    case A_APP:
        aos_search_close();
        aos_ui_open(act.arg);
        return;
    case A_PAGE:
        aos_search_close();
        aos_ui_open_app_with("aos.settings", act.arg);
        return;
    case A_NOTE:
        aos_search_close();
        aos_ui_open_app_with("aos.notas", act.arg);
        return;
    case A_FILE:
        aos_search_close();
        aos_ui_open_app_with("aos.files", act.arg);
        return;
    default:
        return;
    }
}

static void row_cb(lv_event_t *e) { do_action(lv_event_get_user_data(e)); }

static void header(const char *text)
{
    lv_obj_t *l = aos_label(V.list, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 18, 0);
    lv_obj_set_style_pad_left(l, 8, 0);
    aos_make_decorative(l);
}

/* a row: an icon (an app's, or a glyph in a tile), a title and a line under it */
static lv_obj_t *row(int kind, const char *arg, const aos_app_t *app, const char *glyph, uint32_t tile,
                     const char *title, const char *sub)
{
    action_t *a = malloc(sizeof *a);
    if (!a) return NULL;
    a->kind = kind;
    snprintf(a->arg, sizeof a->arg, "%s", arg ? arg : "");
    if (!V.has_first) { V.first = *a; V.has_first = true; }

    lv_obj_t *r = lv_obj_create(V.list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 92, 0);
    lv_obj_set_style_pad_hor(r, 12, 0);
    lv_obj_set_style_pad_ver(r, 10, 0);
    lv_obj_set_style_pad_column(r, 20, 0);
    lv_obj_set_style_radius(r, 18, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(r, row_deleted_cb, LV_EVENT_DELETE, a);

    lv_obj_t *ic;
    if (app) {
        ic = aos_home_app_icon(r, app, 68);
    } else {
        ic = lv_obj_create(r);
        lv_obj_remove_style_all(ic);
        lv_obj_set_size(ic, 68, 68);
        lv_obj_set_style_radius(ic, 16, 0);
        lv_obj_set_style_bg_color(ic, lv_color_hex(tile), 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_t *g = lv_label_create(ic);
        lv_label_set_text(g, glyph ? glyph : "");
        /* a Material glyph (four bytes from U+F0000), or plain text */
        lv_obj_set_style_text_font(g, glyph && (unsigned char)glyph[0] == 0xF3 ? &aos_sym_44 : aos_font_title, 0);
        lv_obj_set_style_text_color(g, lv_color_white(), 0);
        lv_obj_center(g);
    }
    if (ic) lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = aos_label(col, title, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, lv_pct(100));
    aos_make_decorative(t);
    if (sub && sub[0]) {
        lv_obj_t *s = aos_label(col, sub, aos_font_caption, AOS_C_DIM);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s, lv_pct(100));
        aos_make_decorative(s);
    }
    return r;
}

typedef struct { const aos_app_t *app; int rank; } app_hit_t;

static int app_cmp(const void *a, const void *b)
{
    const app_hit_t *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    return strcmp(aos_tr(x->app->desc.name), aos_tr(y->app->desc.name));
}

/* everything again: what is found at once, and what the walk has so far */
static void fill(void)
{
    if (!V.list) return;
    lv_obj_clean(V.list);
    V.has_first = false;
    if (!V.q[0]) return;
    char raw[Q_MAX];
    snprintf(raw, sizeof raw, "%s", lv_textarea_get_text(V.ta));

    /* the sum */
    char res[48];
    if (calc(raw, res, sizeof res)) {
        header(_("Cuenta"));
        char t[64];
        snprintf(t, sizeof t, "= %s", res);
        row(A_CALC, res, NULL, "=", 0xFF9F0A, t, _("Tocá para copiarlo"));
    }

    /* the apps */
    app_hit_t hits[APPS_MAX];
    int nh = 0;
    char f[96];
    for (int i = 0; i < aos_ui_app_count() && nh < APPS_MAX; i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        if (!a || !a->desc.name || !a->desc.id) continue;
        fold(aos_tr(a->desc.name), f, sizeof f);
        int r = match_rank(f, V.q);
        if (r < 0) {
            fold(a->desc.name, f, sizeof f);
            r = match_rank(f, V.q);
        }
        if (r >= 0) hits[nh++] = (app_hit_t){ a, r };
    }
    qsort(hits, (size_t)nh, sizeof hits[0], app_cmp);
    if (nh) header(_("Apps"));
    for (int i = 0; i < nh; i++) row(A_APP, hits[i].app->desc.id, hits[i].app, NULL, 0, aos_tr(hits[i].app->desc.name), NULL);

    /* the pages of Settings */
    bool any = false;
    for (size_t i = 0; i < sizeof PAGES / sizeof PAGES[0]; i++) {
        fold(aos_tr(PAGES[i].title), f, sizeof f);
        if (match_rank(f, V.q) < 0 && !strstr(PAGES[i].words, V.q)) continue;
        if (!any) header(_("Ajustes"));
        any = true;
        row(A_PAGE, PAGES[i].key, NULL, AOS_SYM_COG, 0x8E8E93, aos_tr(PAGES[i].title), _("Ajustes"));
    }

    /* what the walk found */
    store_t *s = s_store;
    if (!s) return;
    aos_hal_mutex_lock(s->mx);
    if (s->gen == s_gen) {
        if (s->nnotes) header(_("Notas"));
        for (int i = 0; i < s->nnotes; i++)
            row(A_NOTE, s->notes[i].file, NULL, AOS_SYM_FILE_DOCUMENT_OUTLINE, 0xE0A800, s->notes[i].title,
                s->notes[i].snippet);
        if (s->nfiles) header(_("Archivos"));
        for (int i = 0; i < s->nfiles; i++) {
            const char *rel = s->files[i].path;
            const char *root = aos_hal_path_sd_root();
            if (root && !strncmp(rel, root, strlen(root))) rel += strlen(root);
            row(A_FILE, s->files[i].path, NULL, s->files[i].dir ? AOS_SYM_FOLDER : AOS_SYM_FILE_OUTLINE,
                s->files[i].dir ? 0x0A84FF : 0x636366, s->files[i].name, rel);
        }
        if (!s->files_done) {
            lv_obj_t *l = aos_label(V.list, _("Buscando en la tarjeta…"), aos_font_caption, AOS_C_DIM);
            lv_obj_set_style_pad_top(l, 18, 0);
            lv_obj_set_style_pad_left(l, 8, 0);
            aos_make_decorative(l);
        }
        V.shown_version = s->version;
    }
    aos_hal_mutex_unlock(s->mx);
    if (!V.has_first) {
        lv_obj_t *l = aos_label(V.list, _("Nada con eso"), aos_font_body, AOS_C_DIM);
        lv_obj_set_style_pad_top(l, 40, 0);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

/* the query settled: a new generation, a walk for it */
static void run_cb(lv_timer_t *t)
{
    (void)t;
    lv_timer_pause(V.debounce);
    char q[Q_MAX];
    fold(lv_textarea_get_text(V.ta), q, sizeof q);
    /* spaces at the ends do not count */
    char *b = q;
    while (*b == ' ') b++;
    size_t n = strlen(b);
    while (n && b[n - 1] == ' ') b[--n] = 0;
    memmove(q, b, n + 1);
    snprintf(V.q, sizeof V.q, "%s", q);
    uint32_t gen = ++s_gen;
    if (s_store) {
        aos_hal_mutex_lock(s_store->mx);
        s_store->gen = gen;
        s_store->nnotes = s_store->nfiles = 0;
        s_store->notes_done = s_store->files_done = false;
        s_store->version++;
        aos_hal_mutex_unlock(s_store->mx);
    }
    if (q[0] && s_store && aos_hal_path_sd_root()) {
        job_t *j = malloc(sizeof *j);
        if (j) {
            j->gen = gen;
            snprintf(j->q, sizeof j->q, "%s", q);
            if (!aos_hal_thread_start("search", worker, j, 6144, 2)) free(j);
        }
    } else if (s_store) {
        s_store->notes_done = s_store->files_done = true;
    }
    fill();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_store && s_store->version != V.shown_version) fill();
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_timer_reset(V.debounce);
        lv_timer_resume(V.debounce);
    } else if (code == LV_EVENT_READY) {
        if (V.has_first) do_action(&V.first);
    }
}

static void kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CANCEL) lv_obj_add_flag(V.kb, LV_OBJ_FLAG_HIDDEN);
}

static void ta_click_cb(lv_event_t *e)
{
    (void)e;
    if (V.kb) lv_obj_remove_flag(V.kb, LV_OBJ_FLAG_HIDDEN);
}

bool aos_search_is_open(void) { return V.layer != NULL; }

void aos_search_open(void)
{
    if (V.layer) return;
    const aos_geo_t *g = aos_ui_geo();
    memset(&V, 0, sizeof V);
    if (!s_store) {
        s_store = calloc(1, sizeof *s_store);
        if (s_store) s_store->mx = aos_hal_mutex_create();
        if (s_store && !s_store->mx) { free(s_store); s_store = NULL; }
    }

    V.layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(V.layer);
    lv_obj_move_to_index(V.layer, 0);       /* under the status bar and the panels */
    lv_obj_set_size(V.layer, g->w, g->h);
    lv_obj_set_style_bg_color(V.layer, lv_color_hex(0x0E0E10), 0);
    lv_obj_set_style_bg_opa(V.layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(V.layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(V.layer, LV_OBJ_FLAG_SCROLLABLE);

    int32_t top = g->bar_h + 12, pad = AOS_UI_PAD, cancel_w = 170;
    V.ta = lv_textarea_create(V.layer);
    lv_textarea_set_one_line(V.ta, true);
    lv_textarea_set_max_length(V.ta, Q_MAX - 1);
    lv_textarea_set_placeholder_text(V.ta, _("Buscar"));
    lv_obj_set_pos(V.ta, pad, top);
    lv_obj_set_width(V.ta, g->w - 2 * pad - cancel_w);
    lv_obj_set_style_text_font(V.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(V.ta, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(V.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(V.ta, 0, 0);
    lv_obj_set_style_radius(V.ta, 22, 0);
    lv_obj_set_style_pad_hor(V.ta, 24, 0);
    lv_obj_set_style_pad_ver(V.ta, 16, 0);
    lv_obj_add_event_cb(V.ta, ta_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(V.ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(V.ta);
    int32_t ta_h = lv_obj_get_height(V.ta);

    lv_obj_t *cancel = lv_label_create(V.layer);
    lv_label_set_text(cancel, _("Cancelar"));
    lv_obj_set_style_text_font(cancel, aos_font_body, 0);
    lv_obj_set_style_text_color(cancel, AOS_C_ACCENT, 0);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(cancel, 24);
    lv_obj_align_to(cancel, V.ta, LV_ALIGN_OUT_RIGHT_MID, 24, 0);
    lv_obj_add_event_cb(cancel, close_cb, LV_EVENT_CLICKED, NULL);

    int32_t kb_h = g->landscape ? g->h / 2 : g->h * 2 / 5;
    int32_t list_y = top + ta_h + 12;
    V.list = lv_obj_create(V.layer);
    lv_obj_remove_style_all(V.list);
    lv_obj_set_pos(V.list, pad, list_y);
    lv_obj_set_size(V.list, g->w - 2 * pad, g->h - list_y - kb_h);
    lv_obj_set_flex_flow(V.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(V.list, 8, 0);
    lv_obj_set_style_pad_bottom(V.list, 20, 0);
    lv_obj_set_scroll_dir(V.list, LV_DIR_VER);

    V.kb = lv_keyboard_create(V.layer);
    lv_obj_set_size(V.kb, g->w, kb_h);
    lv_obj_align(V.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(V.kb, aos_font_body);
    lv_keyboard_set_textarea(V.kb, V.ta);
    lv_obj_add_event_cb(V.kb, kb_cb, LV_EVENT_CANCEL, NULL);

    V.debounce = lv_timer_create(run_cb, DEBOUNCE_MS, NULL);
    lv_timer_pause(V.debounce);
    V.poll = lv_timer_create(poll_cb, 150, NULL);

    lv_obj_set_style_opa(V.layer, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(V.layer, 160, 0);
}

void aos_search_close(void)
{
    if (!V.layer) return;
    s_gen++;                                /* a walk still going stops */
    if (V.debounce) lv_timer_delete(V.debounce);
    if (V.poll) lv_timer_delete(V.poll);
    lv_obj_delete(V.layer);
    memset(&V, 0, sizeof V);
    /* the store stays for a thread that may still write to it: it checks
     * the generation under the mutex, and the next open reuses it */
}
