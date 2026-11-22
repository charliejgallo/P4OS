/*
 * P4OS - Office: reading and editing documents (docs/internal: G5).
 *
 * The app: the files (the documents folder by default, any folder of the
 * card), the recent ones, new documents, and the two editors so far, the
 * spreadsheet (of_sheetview.c) and the text editor (of_textview.c). The
 * formats are in of_*.c without LVGL (of.h); README.md has the plan, by
 * phases, of the rest.
 *
 * Nothing is saved without asking: leaving a document with changes asks
 * whether to save them, and if the system closes the app meanwhile they go
 * to a copy beside the file. Saving writes through a .tmp and a rename, so
 * a card pulled out midway leaves the old one.
 */
#include "of_ui.h"
#include "of_doc.h"
#include "of_arc.h"
#include "of_slides.h"
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* A sheet over a grid of cells, and a page with lines: the icon */
static const uint8_t OF_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,  6,  -4, 52, 60,  6,          AIC_C_TEXT, 255),   /* the page   */
    AIC_RECT(AIC_CENTER, -8,   4, 52, 60,  6,          AIC_C_TEXT, 120),   /* behind it  */
    AIC_RECT(AIC_CENTER,  6,  -4, 52, 60,  6,          AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID,  0, 10, 36,  4,  0,          AIC_C_BG,   255),
    AIC_RECT(AIC_TOP_MID,  0, 20, 36,  4,  0,          AIC_C_BG,   255),
    AIC_RECT(AIC_TOP_MID,  0, 30, 36,  4,  0,          AIC_C_BG,   255),
    AIC_RECT(AIC_TOP_MID, -6, 10,  4, 36,  0,          AIC_C_BG,   255),
    AIC_RECT(AIC_TOP_MID,  8, 10,  4, 36,  0,          AIC_C_BG,   255),
    AIC_RECT(AIC_TOP_MID,  0, 40, 36,  4,  0,          AIC_C_BG,   255),
    AIC_OUT,
    AIC_END
};

#define MAX_FILES   2000
#define MAX_RECENT  30

typedef struct {
    char     name[128];
    bool     dir;
    uint32_t size, mtime;
} fent_t;

enum { SCR_HOME = 0, SCR_SHEET, SCR_TEXT, SCR_DOC, SCR_SLIDES, SCR_PDF };
enum { TAB_RECENT = 0, TAB_FILES };

static struct {
    aos_app_t *self;
    lv_obj_t  *root;
    lv_obj_t  *screen;          /* what is showing, inside root */
    int        scr;
    int        tab;
    char       dir[256];        /* the folder shown */
    fent_t    *files;
    int        nfiles;
    char      *recent[MAX_RECENT];
    int        nrecent;
    lv_obj_t  *list;
    int32_t    list_scroll;
    char       doc[300];        /* the open document */
    of_arc_t  *arc;             /* an archive shown in Files */
    char       arc_pre[260];    /* the folder inside it, "" or "a/b/" */
    /* the prompt */
    lv_obj_t  *pr_overlay, *pr_ta;
    void     (*pr_cb)(const char *, void *);
    void      *pr_ud;
} A;

static void build(void);

/* -------------------------------------------------------------------------- */
/* Paths and preferences                                                       */
/* -------------------------------------------------------------------------- */

const char *of_docs_dir(void)
{
    static char p[96];
    const char *sd = aos_hal_path_sd_root();
    snprintf(p, sizeof p, "%s/documentos", sd ? sd : "/sdcard");
    return p;
}

static void data_path(char *out, size_t len, const char *file)
{
    snprintf(out, len, "%s/oficina/%s", aos_hal_path_data(), file);
}

static void ensure_dirs(void)
{
    mkdir(of_docs_dir(), 0777);
    char p[160];
    snprintf(p, sizeof p, "%s/oficina", aos_hal_path_data());
    mkdir(p, 0777);
}

void of_app_toast(const char *msg)
{
    aos_ui_toast(msg, 2500);
}

bool of_app_landscape(void)
{
    return A.root && lv_obj_get_width(A.root) > lv_obj_get_height(A.root);
}

static void statusbar(void)
{
    aos_ui_statusbar_style(OFT.dark ? AOS_BAR_LIGHT : AOS_BAR_DARK);
}

/* While a document is open the app takes every drag and pinch for itself:
 * the edge swipe would steal the grid's (the back button is on screen). */
static void doc_flags(bool on)
{
    if (!A.self) return;
    if (on) A.self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    else A.self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
}

/* -------------------------------------------------------------------------- */
/* Recent files                                                                */
/* -------------------------------------------------------------------------- */

static void recent_load(void)
{
    for (int i = 0; i < A.nrecent; i++) of_free(A.recent[i]);
    A.nrecent = 0;
    char p[160];
    data_path(p, sizeof p, "recientes.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[320];
    while (A.nrecent < MAX_RECENT && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) A.recent[A.nrecent++] = of_strdup(line);
    }
    fclose(f);
}

static void recent_save(void)
{
    of_sb_t sb = { 0 };
    for (int i = 0; i < A.nrecent; i++) {
        of_sb_puts(&sb, A.recent[i]);
        of_sb_putc(&sb, '\n');
    }
    char p[160];
    data_path(p, sizeof p, "recientes.txt");
    of_file_write(p, sb.s ? sb.s : "", sb.n);
    of_sb_free(&sb);
}

void of_recent_add(const char *path)
{
    int at = -1;
    for (int i = 0; i < A.nrecent; i++)
        if (!strcmp(A.recent[i], path)) at = i;
    char *keep = at >= 0 ? A.recent[at] : of_strdup(path);
    if (at < 0) {
        if (A.nrecent == MAX_RECENT) of_free(A.recent[--A.nrecent]);
        at = A.nrecent++;
    }
    memmove(&A.recent[1], &A.recent[0], (size_t)at * sizeof(char *));
    A.recent[0] = keep;
    recent_save();
}

static void recent_remove(const char *path)
{
    for (int i = 0; i < A.nrecent; i++)
        if (!strcmp(A.recent[i], path)) {
            of_free(A.recent[i]);
            memmove(&A.recent[i], &A.recent[i + 1], (size_t)(A.nrecent - i - 1) * sizeof(char *));
            A.nrecent--;
            recent_save();
            return;
        }
}

/* -------------------------------------------------------------------------- */
/* Kinds of file                                                               */
/* -------------------------------------------------------------------------- */

enum { K_NONE = 0, K_TEXT, K_SHEET, K_LATER, K_BINARY, K_DOC, K_READ, K_ARC, K_DECK };

static const char *const TEXT_EXT[] = {
    "txt", "md", "markdown", "log", "ini", "conf", "cfg", "json", "xml", "yaml", "yml", "toml", "c", "h",
    "cpp", "hpp", "cc", "py", "js", "ts", "mjs", "css", "html", "htm", "sh", "lua", "java", "rs", "go",
    "sql", "srt", "vtt", "tex", "bib", "properties", "gitignore", "diff", "patch", "svg", "kml", "gpx",
    "vcf", "ics", "eml", "nfo", "rst", "org", "env", "bat", "ps1", "cmake", "mk", "s", "asm", "pas", "bas" };
static const char *const LATER_EXT[] = {
    "wps", "wpd", "123", "wk1" };

static int kind_of(const char *path, bool dir)
{
    const char *e = of_ext(path);
    /* Pages, Numbers and Keynote files copied from a Mac can be folders */
    if (dir) return of_reader_for(e) == OF_RD_IWORK ? K_READ : K_NONE;
    if (of_is_sheet_ext(e) || !strcmp(e, "fods")) return K_SHEET;
    if (of_is_doc_ext(e)) return K_DOC;
    if (of_is_deck_ext(e)) return K_DECK;
    if (of_reader_for(e)) return K_READ;
    if (of_is_arc_ext(e)) return K_ARC;
    for (size_t i = 0; i < sizeof TEXT_EXT / sizeof TEXT_EXT[0]; i++)
        if (!strcmp(e, TEXT_EXT[i])) return K_TEXT;
    for (size_t i = 0; i < sizeof LATER_EXT / sizeof LATER_EXT[0]; i++)
        if (!strcmp(e, LATER_EXT[i])) return K_LATER;
    /* no extension we know: text if the first bytes look like it */
    FILE *f = fopen(path, "rb");
    if (!f) return K_BINARY;
    uint8_t buf[1024];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t i = 0; i < n; i++)
        if (buf[i] == 0) return K_BINARY;
    return K_TEXT;
}

/* The badge of a file: a colour and up to four letters */
static void badge_of(const char *name, bool dir, lv_color_t *c, char *txt)
{
    const char *e = of_ext(name);
    txt[0] = 0;
    if (dir) { *c = lv_color_hex(0x0A84FF); return; }
    snprintf(txt, 6, "%.4s", e[0] ? e : "?");
    for (char *p = txt; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    uint32_t rgb = 0x8E8E93;
    if (of_is_sheet_ext(e) || !strcmp(e, "fods") || !strcmp(e, "xls") || !strcmp(e, "numbers")) rgb = 0x1E8E3E;
    else if (!strcmp(e, "docx") || !strcmp(e, "doc") || !strcmp(e, "odt") || !strcmp(e, "rtf") || !strcmp(e, "pages")) rgb = 0x2B6CD8;
    else if (!strcmp(e, "pdf")) rgb = 0xD93025;
    else if (!strcmp(e, "pptx") || !strcmp(e, "ppsx") || !strcmp(e, "potx") || !strcmp(e, "odp") || !strcmp(e, "ppt") || !strcmp(e, "key")) rgb = 0xE8710A;
    else if (!strcmp(e, "md") || !strcmp(e, "txt")) rgb = 0x6E6E73;
    else if (!strcmp(e, "epub") || !strcmp(e, "fb2") || !strcmp(e, "cbz")) rgb = 0xA0522D;
    else if (!strcmp(e, "zip") || !strcmp(e, "tar") || !strcmp(e, "gz") || !strcmp(e, "tgz")) rgb = 0xB8860B;
    else if (!strcmp(e, "eml") || !strcmp(e, "mht") || !strcmp(e, "vcf") || !strcmp(e, "ics")) rgb = 0x0A84FF;
    else if (!strcmp(e, "png") || !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "bmp")) rgb = 0xC2185B;
    else if (!strcmp(e, "json") || !strcmp(e, "xml") || !strcmp(e, "yaml") || !strcmp(e, "yml") || !strcmp(e, "toml") ||
             !strcmp(e, "py") || !strcmp(e, "c") || !strcmp(e, "h") || !strcmp(e, "js") || !strcmp(e, "html")) rgb = 0x8E44AD;
    *c = lv_color_hex(rgb);
}

/* -------------------------------------------------------------------------- */
/* Opening and leaving documents                                               */
/* -------------------------------------------------------------------------- */

static const char *of_tr_app(const char *es) { return _(es); }

static void clear_screen(void)
{
    ofu_sheet_forget();
    if (A.scr == SCR_SHEET) of_sheet_close_view();
    else if (A.scr == SCR_TEXT) of_textv_close();
    else if (A.scr == SCR_DOC) of_docv_close();
    else if (A.scr == SCR_SLIDES) of_slidev_close();
    else if (A.scr == SCR_PDF) of_pdfv_close();
    ofu_kb_attach(NULL);
    lv_obj_clean(A.root);
    A.screen = NULL;
    A.list = NULL;
    A.pr_overlay = NULL;
}

void of_app_home(void)
{
    clear_screen();
    A.scr = SCR_HOME;
    A.doc[0] = 0;
    doc_flags(false);
    build();
}

bool of_app_can_text(const char *path)
{
    const char *e = of_ext(path);
    for (size_t i = 0; i < sizeof TEXT_EXT / sizeof TEXT_EXT[0]; i++)
        if (!strcmp(e, TEXT_EXT[i])) return true;
    return !strcmp(e, "rtf") || !strcmp(e, "fb2") || !strcmp(e, "xhtml") || !strcmp(e, "markdown");
}

bool of_app_can_view(const char *path)
{
    const char *e = of_ext(path);
    return of_reader_for(e) || of_is_tree_ext(e);
}

/* a file inside the app's cache (taken out of an archive): not a recent one */
static bool in_cache(const char *path)
{
    char c[160];
    snprintf(c, sizeof c, "%s/oficina/cache/", aos_hal_path_data());
    return !strncmp(path, c, strlen(c));
}

static void arc_close(void)
{
    of_arc_free(A.arc);
    A.arc = NULL;
    A.arc_pre[0] = 0;
}

static void arc_open(const char *path)
{
    char err[32] = "";
    of_arc_t *a = of_arc_open(path, err, sizeof err);
    if (!a) {
        of_app_toast(!strcmp(err, "big") ? _("El archivo comprimido es muy grande para abrirlo acá (más de 64 MB)")
                                         : _("No se pudo leer el archivo comprimido"));
        return;
    }
    arc_close();
    A.arc = a;
    /* shown in Files, in the folder it is in */
    snprintf(A.dir, sizeof A.dir, "%s", path);
    char *sl = strrchr(A.dir, '/');
    if (sl) *sl = 0;
    A.tab = TAB_FILES;
    A.list_scroll = 0;
    of_recent_add(path);
    if (A.scr != SCR_HOME) of_app_home();
    else build();
}

void of_app_open(const char *path)
{
    of_app_open_as(path, OF_OPEN_AUTO);
}

static char s_pw_path[300];
static char s_pw[64];               /* the password that opened s_pw_path */
static bool s_pw_pages;

static bool show_doc(const char *p, of_doc_t *d);
static bool show_pages(const char *p, const char *pw, char *err, size_t errlen);

static bool pdf_pages_pref(void)
{
    int32_t v = 0;
    aos_hal_pref_get_i32("of_pdfpg", &v);
    return v != 0;
}

static void pw_done(const char *text, void *ud)
{
    (void)ud;
    char err[32] = "";
    if (s_pw_pages) {
        if (show_pages(s_pw_path, text, err, sizeof err)) { snprintf(s_pw, sizeof s_pw, "%s", text); return; }
        if (strcmp(err, "password")) {
            of_app_toast(_("No se pudo leer el documento"));
            return;
        }
        of_app_toast(_("La contraseña no es esa"));
        return;
    }
    of_doc_t *d = of_pdf_load(s_pw_path, text, err, sizeof err);
    if (!d) {
        of_app_toast(!strcmp(err, "password") ? _("La contraseña no es esa") : !strcmp(err, "old") ? _("Es del formato de Office 95 o anterior: la app no lo lee") : _("No se pudo leer el documento"));
        return;
    }
    snprintf(s_pw, sizeof s_pw, "%s", text);
    show_doc(s_pw_path, d);
}

/* A PDF's pages as printed, or its text reflowed; the choice is kept for the next ones. */
void of_app_pdf_mode(const char *path, bool pages)
{
    char p[300];
    snprintf(p, sizeof p, "%s", path);
    aos_hal_pref_set_i32("of_pdfpg", pages);
    const char *pw = !strcmp(p, s_pw_path) ? s_pw : NULL;
    char err[32] = "";
    if (pages) {
        if (show_pages(p, pw, err, sizeof err)) return;
        if (!strcmp(err, "password")) {
            snprintf(s_pw_path, sizeof s_pw_path, "%s", p);
            s_pw_pages = true;
            ofu_prompt(_("Este PDF tiene contraseña"), "", _("Abrir"), pw_done, NULL);
            return;
        }
        of_app_toast(!strcmp(err, "worker") ? _("Otra app está usando el dibujo en segundo plano: cerrala y probá de nuevo") : _("No se pudo leer el documento"));
        return;
    }
    of_tr = of_tr_app;
    of_doc_t *d = of_pdf_load(p, pw, err, sizeof err);
    if (!d) { of_app_toast(_("No se pudo leer el documento")); return; }
    show_doc(p, d);
}

void of_app_open_as(const char *path, int how)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        of_app_toast(_("Ese archivo ya no está"));
        recent_remove(path);
        if (A.scr == SCR_HOME) build();
        return;
    }
    int k = kind_of(path, S_ISDIR(st.st_mode));
    if (how == OF_OPEN_TEXT) k = K_TEXT;
    else if (how == OF_OPEN_VIEW) k = K_READ;
    if (k == K_ARC) { arc_open(path); return; }
    if (k == K_LATER) {
        of_app_toast(_("Este formato todavía no se abre: llega en una próxima versión"));
        return;
    }
    if (k == K_BINARY) {
        of_app_toast(_("No es un documento que la app conozca"));
        return;
    }
    char p[300];
    snprintf(p, sizeof p, "%s", path);
    if (k == K_DECK) {
        char err[64] = "";
        of_tr = of_tr_app;
        of_deck_t *dk = of_deck_load(p, err, sizeof err);
        if (!dk) {
            of_app_toast(!strcmp(err, "encrypted") ? _("El archivo tiene contraseña: la app no lo puede abrir")
                                                   : !strcmp(err, "old") ? _("Es del formato de Office 95 o anterior: la app no lo lee") : _("No se pudo leer la presentación"));
            return;
        }
        clear_screen();
        A.scr = SCR_SLIDES;
        snprintf(A.doc, sizeof A.doc, "%s", p);
        doc_flags(true);
        lv_obj_set_style_bg_color(A.root, OFT.bg, 0);
        if (!of_slidev_open(A.root, p, dk)) { of_app_home(); return; }
    } else if ((k == K_DOC || k == K_READ) && !strcmp(of_ext(p), "pdf") && pdf_pages_pref()) {
        char err[64] = "";
        if (!show_pages(p, NULL, err, sizeof err)) {
            if (!strcmp(err, "password")) {
                snprintf(s_pw_path, sizeof s_pw_path, "%s", p);
                s_pw_pages = true;
                ofu_prompt(_("Este PDF tiene contraseña"), "", _("Abrir"), pw_done, NULL);
            } else {
                of_app_toast(!strcmp(err, "encrypted") ? _("El PDF está cifrado de una forma que la app no conoce")
                                                       : !strcmp(err, "worker") ? _("Otra app está usando el dibujo en segundo plano: cerrala y probá de nuevo")
                                                                                : _("No se pudo leer el documento"));
            }
        }
        return;
    } else if (k == K_DOC || k == K_READ) {
        char err[64] = "";
        of_tr = of_tr_app;
        of_doc_t *d = k == K_DOC ? of_doc_load(p, err, sizeof err) : of_read_any(p, err, sizeof err);
        if (!d && !strcmp(err, "password")) {
            /* a PDF with a password: asked, and opened with it */
            snprintf(s_pw_path, sizeof s_pw_path, "%s", p);
            s_pw_pages = false;
            ofu_prompt(_("Este PDF tiene contraseña"), "", _("Abrir"), pw_done, NULL);
            return;
        }
        if (!d && !strcmp(err, "encrypted")) {
            of_app_toast(_("El PDF está cifrado de una forma que la app no conoce"));
            return;
        }
        if (!d) {
            of_app_toast(!strcmp(err, "encrypted") ? _("El archivo tiene contraseña: la app no lo puede abrir")
                                                   : !strcmp(err, "old") ? _("Es del formato de Office 95 o anterior: la app no lo lee") : _("No se pudo leer el documento"));
            return;
        }
        if (!show_doc(p, d)) return;
    } else if (k == K_SHEET) {
        char err[64] = "";
        of_book_t *b = of_book_load(p, err, sizeof err);
        if (!b) {
            of_app_toast(!strcmp(err, "encrypted") ? _("El archivo tiene contraseña: la app no lo puede abrir")
                                                   : !strcmp(err, "old") ? _("Es del formato de Office 95 o anterior: la app no lo lee") : _("No se pudo leer la planilla"));
            return;
        }
        clear_screen();
        A.scr = SCR_SHEET;
        snprintf(A.doc, sizeof A.doc, "%s", p);
        doc_flags(true);
        lv_obj_set_style_bg_color(A.root, OFT.bg, 0);
        if (!of_sheet_open(A.root, p, b)) {
            of_app_home();
            return;
        }
    } else {
        clear_screen();
        A.scr = SCR_TEXT;
        snprintf(A.doc, sizeof A.doc, "%s", p);
        doc_flags(true);
        lv_obj_set_style_bg_color(A.root, OFT.bg, 0);
        if (!of_textv_open(A.root, p)) {
            of_app_toast(_("No se pudo leer el archivo"));
            of_app_home();
            return;
        }
    }
    if (in_cache(p)) of_app_toast(_("Es una copia sacada del archivo comprimido: para quedártela, extraela con una pulsación larga"));
    else of_recent_add(p);
}

static bool show_doc(const char *p, of_doc_t *d)
{
    char path[300];
    snprintf(path, sizeof path, "%s", p);
    clear_screen();
    A.scr = SCR_DOC;
    snprintf(A.doc, sizeof A.doc, "%s", path);
    doc_flags(true);
    lv_obj_set_style_bg_color(A.root, OFT.bg, 0);
    if (!of_docv_open(A.root, path, d)) {
        of_app_home();
        return false;
    }
    if (!in_cache(path)) of_recent_add(path);
    return true;
}

static bool show_pages(const char *p, const char *pw, char *err, size_t errlen)
{
    char path[300];
    snprintf(path, sizeof path, "%s", p);
    clear_screen();
    A.scr = SCR_PDF;
    snprintf(A.doc, sizeof A.doc, "%s", path);
    doc_flags(true);
    lv_obj_set_style_bg_color(A.root, OFT.bg, 0);
    if (!of_pdfv_open(A.root, path, pw, err, errlen)) {
        of_app_home();
        return false;
    }
    if (!in_cache(path)) of_recent_add(path);
    return true;
}

/* -------------------------------------------------------------------------- */
/* The prompt: a line to type                                                  */
/* -------------------------------------------------------------------------- */

static void pr_close(void)
{
    if (!A.pr_overlay) return;
    ofu_kb_attach(NULL);
    lv_obj_t *o = A.pr_overlay;
    A.pr_overlay = NULL;
    A.pr_ta = NULL;
    lv_obj_delete(o);
}

static void pr_done(lv_obj_t *ta)
{
    (void)ta;
    if (!A.pr_ta) return;
    char *text = of_strdup(lv_textarea_get_text(A.pr_ta));
    void (*cb)(const char *, void *) = A.pr_cb;
    void *ud = A.pr_ud;
    pr_close();
    if (cb && text) cb(text, ud);
    of_free(text);
}

static void pr_ok_cb(lv_event_t *e) { (void)e; pr_done(NULL); }
static void pr_cancel_cb(lv_event_t *e) { (void)e; pr_close(); }

void ofu_prompt(const char *title, const char *initial, const char *ok, void (*cb)(const char *text, void *ud), void *ud)
{
    pr_close();
    lv_obj_t *root = A.root;
    int32_t W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    bool land = W > H;
    A.pr_cb = cb;
    A.pr_ud = ud;
    A.pr_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(A.pr_overlay);
    lv_obj_set_size(A.pr_overlay, W, H);
    lv_obj_set_style_bg_color(A.pr_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(A.pr_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(A.pr_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(A.pr_overlay, LV_OBJ_FLAG_SCROLLABLE);
    int32_t kbh = land ? 300 : 420;
    lv_obj_t *card = lv_obj_create(A.pr_overlay);
    lv_obj_remove_style_all(card);
    int32_t cw = land ? 760 : W - 32;
    lv_obj_set_size(card, cw, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, OFT.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, land ? 16 : 40);
    lv_obj_t *t = ofu_text(card, title, ofu_font(OF_ST_BOLD, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, cw - 40);
    A.pr_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(A.pr_ta, true);
    lv_obj_set_width(A.pr_ta, cw - 40);
    lv_obj_set_style_text_font(A.pr_ta, ofu_font(0, OF_SZ_M), 0);
    lv_obj_set_style_bg_color(A.pr_ta, OFT.surface2, 0);
    lv_obj_set_style_text_color(A.pr_ta, OFT.text, 0);
    lv_obj_set_style_border_width(A.pr_ta, 0, 0);
    lv_obj_set_style_radius(A.pr_ta, 14, 0);
    lv_obj_set_user_data(A.pr_ta, (void *)1);       /* no shift at the start */
    lv_textarea_set_text(A.pr_ta, initial ? initial : "");
    lv_obj_add_state(A.pr_ta, LV_STATE_FOCUSED);
    lv_obj_t *row = ofu_box(card, cw - 40, 84);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_obj_create(row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, (cw - 56) / 2, 80);
        lv_obj_set_style_radius(b, 20, 0);
        lv_obj_set_style_bg_color(b, i ? OFT.accent : OFT.surface2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = ofu_text(b, i ? ok : _("Cancelar"), ofu_font(OF_ST_BOLD, OF_SZ_M), i ? lv_color_white() : OFT.text);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, i ? pr_ok_cb : pr_cancel_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *kb = ofu_kb_create(A.pr_overlay, W, kbh);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    ofu_kb_attach_textarea(A.pr_ta, pr_done);
}

/* -------------------------------------------------------------------------- */
/* The folder                                                                  */
/* -------------------------------------------------------------------------- */

static int fent_cmp(const void *a, const void *b)
{
    const fent_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scan(void)
{
    A.nfiles = 0;
    if (!A.files) A.files = of_alloc(MAX_FILES * sizeof(fent_t));
    if (!A.files) return;
    DIR *d = opendir(A.dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && A.nfiles < MAX_FILES) {
        if (e->d_name[0] == '.') continue;          /* also macOS's ._ files */
        fent_t *f = &A.files[A.nfiles];
        snprintf(f->name, sizeof f->name, "%.127s", e->d_name);
        char p[520];
        snprintf(p, sizeof p, "%.255s/%.255s", A.dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        f->dir = S_ISDIR(st.st_mode);
        f->size = (uint32_t)st.st_size;
        f->mtime = (uint32_t)st.st_mtime;
        A.nfiles++;
    }
    closedir(d);
    qsort(A.files, (size_t)A.nfiles, sizeof(fent_t), fent_cmp);
}

static void fmt_size(uint32_t n, char *out, size_t len)
{
    if (n < 1024) snprintf(out, len, "%lu B", (unsigned long)n);
    else if (n < 1024 * 1024) snprintf(out, len, "%lu KB", (unsigned long)((n + 512) / 1024));
    else snprintf(out, len, "%lu,%lu MB", (unsigned long)(n / (1024 * 1024)), (unsigned long)(n % (1024 * 1024) * 10 / (1024 * 1024)));
}

static void fmt_date(uint32_t t, char *out, size_t len)
{
    out[0] = 0;
    if (t < 1000000000u) return;
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    if (of_locale_comma()) snprintf(out, len, "%02d/%02d/%04d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    else snprintf(out, len, "%d/%d/%04d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
}

/* -------------------------------------------------------------------------- */
/* A row of the list                                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    char    path[300];          /* in an archive: a folder's prefix */
    bool    dir;
    int     aidx;               /* an archive's entry, -1 a file of the card */
    lv_color_t color;
    char    badge[6];
} row_t;

static void badge_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    row_t *r = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (r->dir) {
        ofu_draw_icon(layer, OF_IC_FOLDER, &a, r->color, 4);
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = r->color;
    d.radius = 14;
    lv_draw_rect(layer, &d, &a);
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = strlen(r->badge) > 3 ? &aos_inter_16 : &aos_inter_20;
    l.color = lv_color_white();
    l.text = r->badge;
    l.text_local = 1;
    l.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t t = a;
    t.y1 = (a.y1 + a.y2) / 2 - l.font->line_height / 2;
    t.y2 = t.y1 + l.font->line_height;
    lv_draw_label(layer, &l, &t);
}

static void row_free_cb(lv_event_t *e)
{
    of_free(lv_event_get_user_data(e));
}

static void enter_dir(const char *path)
{
    snprintf(A.dir, sizeof A.dir, "%s", path);
    A.list_scroll = 0;
    build();
}

/* an archive's entry: taken out to the cache and opened */
static void arc_open_entry(int i)
{
    if (!A.arc || i < 0 || i >= A.arc->n) return;
    char dir[160], out[400], err[32] = "";
    snprintf(dir, sizeof dir, "%s/oficina/cache", aos_hal_path_data());
    mkdir(dir, 0777);
    snprintf(dir, sizeof dir, "%s/oficina/cache/arc", aos_hal_path_data());
    mkdir(dir, 0777);
    snprintf(out, sizeof out, "%s/%s", dir, of_basename(A.arc->e[i].name));
    if (!of_arc_extract(A.arc, i, out, err, sizeof err)) {
        of_app_toast(!strcmp(err, "encrypted") ? _("Ese archivo tiene contraseña: la app no lo puede sacar") : _("No se pudo sacar el archivo"));
        return;
    }
    of_app_open(out);
}

/* a name free in a folder: "a.txt", "a 2.txt"... */
static void free_name(const char *dir, const char *name, char *out, size_t len)
{
    snprintf(out, len, "%.200s/%.90s", dir, name);
    const char *e = of_ext(name);
    int el = e[0] ? (int)strlen(e) + 1 : 0;
    int bl = (int)strlen(name) - el;
    for (int k = 2; k < 1000 && of_file_exists(out); k++)
        snprintf(out, len, "%.200s/%.*s %d%s", dir, bl > 80 ? 80 : bl, name, k, el ? name + bl : "");
}

static int s_act_arc;

static void arc_extract_here(int i)
{
    char out[320], err[32] = "";
    free_name(A.dir, of_basename(A.arc->e[i].name), out, sizeof out);
    if (!of_arc_extract(A.arc, i, out, err, sizeof err)) { of_app_toast(_("No se pudo sacar el archivo")); return; }
    char m[200];
    snprintf(m, sizeof m, _("Extraído en la carpeta: %s"), of_basename(out));
    of_app_toast(m);
}

static void arc_extract_all(void)
{
    if (!A.arc) return;
    char base[128], dest[320];
    snprintf(base, sizeof base, "%s", of_basename(A.arc->path));
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    dot = strrchr(base, '.');
    if (dot && !strcasecmp(dot, ".tar")) *dot = 0;
    free_name(A.dir, base, dest, sizeof dest);
    mkdir(dest, 0777);
    int ok = 0, bad = 0;
    for (int i = 0; i < A.arc->n; i++) {
        if (A.arc->e[i].dir) continue;
        char out[600], err[32];
        snprintf(out, sizeof out, "%s/%s", dest, A.arc->e[i].name);
        if (of_arc_extract(A.arc, i, out, err, sizeof err)) ok++;
        else bad++;
    }
    char m[200];
    if (bad) snprintf(m, sizeof m, _("Se extrajeron %d archivos en «%s»; %d no se pudieron"), ok, of_basename(dest), bad);
    else snprintf(m, sizeof m, _("Se extrajeron %d archivos en «%s»"), ok, of_basename(dest));
    of_app_toast(m);
}

static void arc_act_pick(int i, void *ud)
{
    (void)ud;
    if (i == 0) arc_open_entry(s_act_arc);
    else if (i == 1) arc_extract_here(s_act_arc);
}

static void row_click_cb(lv_event_t *e)
{
    row_t *r = lv_event_get_user_data(e);
    if (r->aidx >= 0) { arc_open_entry(r->aidx); return; }
    if (A.arc && r->dir) {
        snprintf(A.arc_pre, sizeof A.arc_pre, "%.259s", r->path);
        A.list_scroll = 0;
        build();
        return;
    }
    if (r->dir && kind_of(r->path, true) == K_NONE) enter_dir(r->path);
    else of_app_open(r->path);
}

/* the actions of a file, on a long press */
static char s_act_path[300];

static void rename_done(const char *text, void *ud)
{
    (void)ud;
    if (!text[0] || strchr(text, '/')) return;
    char np[320];
    char dir[300];
    snprintf(dir, sizeof dir, "%s", s_act_path);
    char *sl = strrchr(dir, '/');
    if (!sl) return;
    *sl = 0;
    /* the extension stays if the new name has none */
    const char *e = of_ext(s_act_path);
    bool has_ext = strchr(text, '.') != NULL;
    snprintf(np, sizeof np, "%.200s/%.80s%s%.10s", dir, text, has_ext || !e[0] ? "" : ".", has_ext ? "" : e);
    if (of_file_exists(np)) { of_app_toast(_("Ya hay un archivo con ese nombre")); return; }
    if (rename(s_act_path, np) != 0) { of_app_toast(_("No se pudo cambiar el nombre")); return; }
    for (int i = 0; i < A.nrecent; i++)
        if (!strcmp(A.recent[i], s_act_path)) { of_free(A.recent[i]); A.recent[i] = of_strdup(np); recent_save(); }
    build();
}

static void delete_yes(void *ud)
{
    (void)ud;
    if (remove(s_act_path) != 0) { of_app_toast(_("No se pudo borrar")); return; }
    recent_remove(s_act_path);
    build();
}

static void duplicate(void)
{
    size_t n;
    uint8_t *d = of_file_read(s_act_path, &n, 64u * 1024 * 1024);
    if (!d) { of_app_toast(_("No se pudo leer el archivo")); return; }
    char np[320];
    const char *e = of_ext(s_act_path);
    int el = e[0] ? (int)strlen(e) + 1 : 0;
    int bl = (int)strlen(s_act_path) - el;
    for (int k = 2; k < 100; k++) {
        snprintf(np, sizeof np, "%.*s %d%s", bl > 280 ? 280 : bl, s_act_path, k, el ? s_act_path + bl : "");
        if (!of_file_exists(np)) break;
    }
    if (!of_file_write(np, d, n)) of_app_toast(_("No se pudo copiar"));
    of_free(d);
    build();
}

static void act_pick(int i, void *ud)
{
    (void)ud;
    switch (i) {
    case 0: of_app_open(s_act_path); break;
    case 1: {
        char base[128];
        snprintf(base, sizeof base, "%s", of_basename(s_act_path));
        char *dot = strrchr(base, '.');
        if (dot && dot != base) *dot = 0;
        ofu_prompt(_("Nuevo nombre"), base, _("Cambiar"), rename_done, NULL);
        break;
    }
    case 2: duplicate(); break;
    case 3: {
        char t[200];
        snprintf(t, sizeof t, _("¿Borrar «%s»?"), of_basename(s_act_path));
        ofu_confirm(t, _("No se puede deshacer."), _("Borrar"), true, delete_yes, NULL);
        break;
    }
    case 4: recent_remove(s_act_path); build(); break;
    }
}

static void row_long_cb(lv_event_t *e)
{
    row_t *r = lv_event_get_user_data(e);
    if (r->aidx >= 0) {
        s_act_arc = r->aidx;
        ofu_item_t it[2] = {
            { _("Abrir"), OF_IC_DOC, false, false },
            { _("Extraer en esta carpeta"), OF_IC_FOLDER, false, false },
        };
        lv_indev_wait_release(lv_indev_active());
        ofu_sheet(of_basename(A.arc->e[r->aidx].name), it, 2, arc_act_pick, NULL);
        return;
    }
    if (r->dir) return;
    snprintf(s_act_path, sizeof s_act_path, "%s", r->path);
    ofu_item_t items[5] = {
        { _("Abrir"), OF_IC_DOC, false, false },
        { _("Cambiar el nombre"), OF_IC_TYPE, false, false },
        { _("Duplicar"), OF_IC_COPY, false, false },
        { _("Borrar"), OF_IC_TRASH, true, false },
        { _("Sacar de recientes"), OF_IC_CLOSE, false, false },
    };
    lv_indev_wait_release(lv_indev_active());
    ofu_sheet(of_basename(r->path), items, A.tab == TAB_RECENT ? 5 : 4, act_pick, NULL);
}

static lv_obj_t *file_row(lv_obj_t *list, int32_t w, const char *path, const char *name, bool dir,
                          const char *sub, int aidx)
{
    lv_obj_t *r = lv_obj_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, 108);
    lv_obj_set_style_radius(r, 20, 0);
    lv_obj_set_style_bg_color(r, OFT.surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    row_t *u = of_alloc(sizeof *u);
    if (!u) return r;
    snprintf(u->path, sizeof u->path, "%s", path);
    u->dir = dir;
    u->aidx = aidx;
    badge_of(name, dir, &u->color, u->badge);
    lv_obj_t *ic = ofu_box(r, 68, 68);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_event_cb(ic, badge_draw_cb, LV_EVENT_DRAW_MAIN, u);
    lv_obj_t *t = ofu_text(r, name, ofu_font(0, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, w - 120);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 104, sub && sub[0] ? 18 : 36);
    if (sub && sub[0]) {
        lv_obj_t *s = ofu_text(r, sub, ofu_font(0, OF_SZ_XS), OFT.dim);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s, w - 120);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 104, 60);
    }
    lv_obj_add_event_cb(r, row_click_cb, LV_EVENT_CLICKED, u);
    lv_obj_add_event_cb(r, row_long_cb, LV_EVENT_LONG_PRESSED, u);
    lv_obj_add_event_cb(r, row_free_cb, LV_EVENT_DELETE, u);
    return r;
}

/* -------------------------------------------------------------------------- */
/* New documents                                                               */
/* -------------------------------------------------------------------------- */

static int s_new_kind;              /* 0 docx, 1 odt, 2 xlsx, 3 ods, 4 csv, 5 txt, 6 md */

static void new_done(const char *text, void *ud)
{
    (void)ud;
    static const char *const EXT[] = { "docx", "odt", "xlsx", "ods", "csv", "txt", "md" };
    if (!text[0] || strchr(text, '/')) return;
    char p[320];
    const char *dir = A.tab == TAB_FILES ? A.dir : of_docs_dir();
    bool has = !strcasecmp(of_ext(text), EXT[s_new_kind]);
    snprintf(p, sizeof p, "%.200s/%.100s%s%s", dir, text, has ? "" : ".", has ? "" : EXT[s_new_kind]);
    if (of_file_exists(p)) { of_app_toast(_("Ya hay un archivo con ese nombre")); return; }
    char err[64];
    bool ok;
    if (s_new_kind <= 1) {
        ok = of_doc_new_file(s_new_kind == 0 ? OF_DOC_DOCX : OF_DOC_ODT, p, NULL);
    } else if (s_new_kind <= 4) {
        int fmt = s_new_kind == 2 ? OF_FMT_XLSX : s_new_kind == 3 ? OF_FMT_ODS : OF_FMT_CSV;
        of_book_t *b = of_book_new(fmt);
        ok = b && of_book_add_sheet(b, _("Hoja1"));
        ok = ok && of_book_save(b, fmt, 0, p, err, sizeof err);
        of_book_free(b);
    } else {
        ok = of_file_write(p, "", 0);
    }
    if (!ok) { of_app_toast(_("No se pudo crear el archivo")); return; }
    of_app_open(p);
}

static void new_pick(int i, void *ud)
{
    (void)ud;
    s_new_kind = i;
    bool sheet = i >= 2 && i <= 4;
    ofu_prompt(sheet ? _("Nombre de la planilla") : _("Nombre del documento"), sheet ? _("Planilla") : _("Documento"),
               _("Crear"), new_done, NULL);
}

static void new_cb(lv_event_t *e)
{
    (void)e;
    ofu_item_t items[7] = {
        { _("Documento de Word (.docx)"), OF_IC_DOC, false, false },
        { _("Documento de OpenDocument (.odt)"), OF_IC_DOC, false, false },
        { _("Planilla de Excel (.xlsx)"), OF_IC_SHEET, false, false },
        { _("Planilla de OpenDocument (.ods)"), OF_IC_SHEET, false, false },
        { _("Datos separados por comas (.csv)"), OF_IC_SHEET, false, false },
        { _("Texto (.txt)"), OF_IC_DOC, false, false },
        { _("Markdown (.md)"), OF_IC_DOC, false, false },
    };
    ofu_sheet(_("Nuevo"), items, 7, new_pick, NULL);
}

static void more_pick(int i, void *ud)
{
    (void)ud;
    if (i == 0) {
        ofu_theme_set(!OFT.dark);
        aos_hal_pref_set_i32("of_dark", OFT.dark);
        statusbar();
        build();
    } else if (i == 1) {
        arc_close();
        enter_dir(of_docs_dir());
    } else if (i == 2) {
        arc_extract_all();
        build();
    }
}

static void more_cb(lv_event_t *e)
{
    (void)e;
    ofu_item_t items[3] = {
        { OFT.dark ? _("Fondo claro") : _("Fondo oscuro"), OFT.dark ? OF_IC_SUN : OF_IC_MOON, false, false },
        { _("Ir a la carpeta de documentos"), OF_IC_FOLDER, false, false },
        { _("Extraer todo en una carpeta"), OF_IC_FOLDER, false, false },
    };
    ofu_sheet(NULL, items, A.arc && A.tab == TAB_FILES ? 3 : 2, more_pick, NULL);
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == A.tab) return;
    A.tab = t;
    A.list_scroll = 0;
    build();
}

static void up_cb(lv_event_t *e)
{
    (void)e;
    if (A.arc) {
        /* up a folder inside the archive, or out of it */
        size_t l = strlen(A.arc_pre);
        if (l) {
            A.arc_pre[l - 1] = 0;
            char *s2 = strrchr(A.arc_pre, '/');
            if (s2) s2[1] = 0;
            else A.arc_pre[0] = 0;
        } else {
            arc_close();
        }
        A.list_scroll = 0;
        build();
        return;
    }
    char *sl = strrchr(A.dir, '/');
    const char *sd = aos_hal_path_sd_root();
    if (!sl || sl == A.dir || (sd && strlen(A.dir) <= strlen(sd))) return;
    *sl = 0;
    A.list_scroll = 0;
    build();
}

static void list_scroll_cb(lv_event_t *e)
{
    A.list_scroll = lv_obj_get_scroll_y(lv_event_get_target(e));
}

/* -------------------------------------------------------------------------- */
/* The home screen                                                             */
/* -------------------------------------------------------------------------- */

static int aent_cmp(const void *a, const void *b)
{
    const of_aent_t *x = *(const of_aent_t *const *)a, *y = *(const of_aent_t *const *)b;
    return strcasecmp(x->name, y->name);
}

/* The rows of an archive's folder: its subfolders, then its files. */
static int arc_rows(int32_t cw)
{
    size_t pl = strlen(A.arc_pre);
    const of_aent_t **v = of_alloc((size_t)(A.arc->n + 1) * sizeof *v);
    if (!v) return 0;
    int nv = 0;
    for (int i = 0; i < A.arc->n; i++)
        if (!strncmp(A.arc->e[i].name, A.arc_pre, pl) && A.arc->e[i].name[pl]) v[nv++] = &A.arc->e[i];
    qsort(v, (size_t)nv, sizeof *v, aent_cmp);
    int shown = 0;
    /* folders: the first part of what is below, once each */
    char last[200] = "";
    for (int k = 0; k < nv; k++) {
        const char *rest = v[k]->name + pl;
        const char *sl = strchr(rest, '/');
        if (!sl && !v[k]->dir) continue;
        size_t l = sl ? (size_t)(sl - rest) : strlen(rest);
        char nm[200];
        snprintf(nm, sizeof nm, "%.*s", (int)(l < sizeof nm - 1 ? l : sizeof nm - 1), rest);
        if (!strcmp(nm, last)) continue;
        snprintf(last, sizeof last, "%s", nm);
        char pre[300];
        snprintf(pre, sizeof pre, "%.200s%.90s/", A.arc_pre, nm);
        file_row(A.list, cw, pre, nm, true, "", -1);
        shown++;
    }
    for (int k = 0; k < nv; k++) {
        const char *rest = v[k]->name + pl;
        if (v[k]->dir || strchr(rest, '/')) continue;
        char sub[48], sz[24];
        fmt_size((uint32_t)(v[k]->size > 0xFFFFFFFFu ? 0xFFFFFFFFu : v[k]->size), sz, sizeof sz);
        fmt_date(v[k]->mtime, sub, sizeof sub);
        char line[120];
        snprintf(line, sizeof line, "%s%s%s%s%s", sub, sub[0] ? "  \xC2\xB7  " : "", sz, v[k]->locked ? "  \xC2\xB7  " : "", v[k]->locked ? _("con contraseña") : "");
        file_row(A.list, cw, "", rest, false, line, (int)(v[k] - A.arc->e));
        shown++;
    }
    of_free(v);
    return shown;
}

static void build(void)
{
    if (A.scr != SCR_HOME) return;
    ofu_sheet_forget();
    lv_obj_clean(A.root);
    A.pr_overlay = NULL;
    ofu_ui_root = A.root;
    lv_obj_set_style_bg_color(A.root, OFT.bg, 0);
    lv_obj_set_style_bg_opa(A.root, LV_OPA_COVER, 0);
    int32_t W = lv_obj_get_width(A.root), H = lv_obj_get_height(A.root);
    bool land = W > H;
    int32_t pad = land ? 40 : 24;
    int32_t cw = W - 2 * pad;

    A.screen = ofu_box(A.root, W, H);
    lv_obj_t *head = ofu_box(A.screen, W, 104);
    lv_obj_t *title = ofu_text(head, _("Oficina"), ofu_font(OF_ST_BOLD, OF_SZ_XL), OFT.text);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, pad, 0);
    lv_obj_t *more = ofu_icon_btn(head, OF_IC_MORE, 80, more_cb, NULL);
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, -pad + 8, 0);
    lv_obj_t *plus = ofu_icon_btn(head, OF_IC_PLUS, 80, new_cb, NULL);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -pad - 80, 0);
    ofu_icon_color(plus, OFT.accent);

    /* the two tabs */
    lv_obj_t *tabs = ofu_box(A.screen, cw, 72);
    lv_obj_set_pos(tabs, pad, 104);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 12, 0);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t0 = ofu_pill(tabs, _("Recientes"), tab_cb, (void *)(intptr_t)TAB_RECENT);
    lv_obj_t *t1 = ofu_pill(tabs, _("Archivos"), tab_cb, (void *)(intptr_t)TAB_FILES);
    lv_obj_add_state(A.tab == TAB_RECENT ? t0 : t1, LV_STATE_CHECKED);

    int32_t y = 104 + 84;
    if (A.tab == TAB_FILES) {
        /* where we are */
        lv_obj_t *bar = ofu_box(A.screen, cw, 72);
        lv_obj_set_pos(bar, pad, y);
        lv_obj_t *up = ofu_icon_btn(bar, OF_IC_UP, 64, up_cb, NULL);
        lv_obj_align(up, LV_ALIGN_LEFT_MID, 0, 0);
        const char *sd = aos_hal_path_sd_root();
        const char *shown = A.dir;
        if (sd && !strncmp(A.dir, sd, strlen(sd))) shown = A.dir + strlen(sd);
        char where[260];
        if (A.arc) snprintf(where, sizeof where, "%.120s/%.60s%s%.70s", shown, of_basename(A.arc->path), A.arc_pre[0] ? " \xE2\x80\xBA " : "", A.arc_pre);
        else snprintf(where, sizeof where, "%s", shown[0] ? shown : "/");
        lv_obj_t *wl = ofu_text(bar, where, ofu_font(0, OF_SZ_S), OFT.dim);
        lv_label_set_long_mode(wl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(wl, cw - 80);
        lv_obj_align(wl, LV_ALIGN_LEFT_MID, 76, 0);
        y += 80;
    }
    A.list = lv_obj_create(A.screen);
    lv_obj_remove_style_all(A.list);
    lv_obj_set_pos(A.list, pad, y);
    lv_obj_set_size(A.list, cw, H - y);
    lv_obj_set_flex_flow(A.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(A.list, 2, 0);
    lv_obj_set_style_pad_bottom(A.list, 40, 0);
    lv_obj_set_scroll_dir(A.list, LV_DIR_VER);
    lv_obj_add_flag(A.list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(A.list, list_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    int shown_n = 0;
    if (A.tab == TAB_RECENT) {
        for (int i = 0; i < A.nrecent; i++) {
            struct stat st;
            if (stat(A.recent[i], &st) != 0) continue;
            char sub[320], sz[24], dt[32];
            fmt_size((uint32_t)st.st_size, sz, sizeof sz);
            fmt_date((uint32_t)st.st_mtime, dt, sizeof dt);
            /* the folder, as the person knows it */
            char dir[200];
            snprintf(dir, sizeof dir, "%s", A.recent[i]);
            char *sl = strrchr(dir, '/');
            if (sl) *sl = 0;
            const char *sd = aos_hal_path_sd_root();
            const char *d = dir;
            if (sd && !strncmp(d, sd, strlen(sd))) d += strlen(sd);
            snprintf(sub, sizeof sub, "%.31s  \xC2\xB7  %.23s  \xC2\xB7  %.199s", dt, sz, d[0] ? d : "/");
            file_row(A.list, cw, A.recent[i], of_basename(A.recent[i]), false, sub, -1);
            shown_n++;
        }
    } else if (A.arc) {
        shown_n = arc_rows(cw);
    } else {
        scan();
        for (int i = 0; i < A.nfiles; i++) {
            char p[400], sub[80] = "", sz[24], dt[32];
            snprintf(p, sizeof p, "%s/%s", A.dir, A.files[i].name);
            if (!A.files[i].dir) {
                fmt_size(A.files[i].size, sz, sizeof sz);
                fmt_date(A.files[i].mtime, dt, sizeof dt);
                snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", dt, sz);
            }
            file_row(A.list, cw, p, A.files[i].name, A.files[i].dir && kind_of(p, true) == K_NONE, sub, -1);
            shown_n++;
        }
    }
    if (!shown_n) {
        lv_obj_t *e = ofu_text(A.list, A.tab == TAB_RECENT
                                            ? _("Todavía no abriste ningún documento. En Archivos están los de la tarjeta, y con + se empieza uno nuevo.")
                                            : _("No hay archivos en esta carpeta. Se suben desde la página Oficina del portal, o con + se crea uno."),
                               ofu_font(0, OF_SZ_M), OFT.dim);
        lv_label_set_long_mode(e, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(e, cw - 32);
        lv_obj_set_style_pad_all(e, 24, 0);
    }
    lv_obj_update_layout(A.list);
    lv_obj_scroll_to_y(A.list, A.list_scroll, LV_ANIM_OFF);
}

/* -------------------------------------------------------------------------- */
/* The app                                                                     */
/* -------------------------------------------------------------------------- */

static void locale(void)
{
    const char *l = aos_i18n_current();
    bool en = l && !strncmp(l, "en", 2);
    of_locale_set(!en);
    of_locale_lang(en ? 0 : (l && !strncmp(l, "de", 2)) ? 2 : 1);
}

static void *of_create(aos_app_t *self, lv_obj_t *root)
{
    A.self = self;
    A.root = root;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    int32_t v = 1;
    aos_hal_pref_get_i32("of_dark", &v);
    ofu_theme_set(v != 0);
    locale();
    ensure_dirs();
    if (!A.dir[0]) snprintf(A.dir, sizeof A.dir, "%s", of_docs_dir());
    recent_load();
    A.tab = A.nrecent ? TAB_RECENT : TAB_FILES;
    A.scr = SCR_HOME;
    statusbar();
    build();
    return &A;
}

static void of_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    /* the system closes a kept app when a fifth one opens: a document with
     * changes goes to a copy beside it, "(sin guardar)", never over it */
    if (A.scr == SCR_SHEET && of_sheet_modified()) of_sheet_save_recovery();
    if (A.scr == SCR_TEXT && of_textv_modified()) of_textv_save_recovery();
    if (A.scr == SCR_DOC && of_docv_modified()) of_docv_save_recovery();
    clear_screen();
    doc_flags(false);
    for (int i = 0; i < A.nrecent; i++) of_free(A.recent[i]);
    A.nrecent = 0;
    of_free(A.files);
    A.files = NULL;
    arc_close();
    A.root = NULL;
    ofu_ui_root = NULL;
    ofu_kb_destroyed();
}

static void of_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    statusbar();
    locale();
    const char *arg = aos_ui_take_open_arg("aos.oficina");
    if (arg && arg[0]) {
        char p[300];
        snprintf(p, sizeof p, "%s", arg);
        if (A.scr != SCR_HOME) of_app_home();
        of_app_open(p);
        return;
    }
    if (A.scr == SCR_HOME) build();        /* the portal may have changed the folder */
}

static void leave_pick(int i, void *ud)
{
    (void)ud;
    if (i == 0) {
        bool ok = A.scr == SCR_SHEET ? of_sheet_save() : A.scr == SCR_DOC ? of_docv_save() : of_textv_save();
        if (!ok) return;
    }
    if (i <= 1) of_app_home();
}

void of_app_back_from_doc(void)
{
    bool mod = A.scr == SCR_SHEET ? of_sheet_modified() : A.scr == SCR_DOC ? of_docv_modified() : of_textv_modified();
    if (!mod) { of_app_home(); return; }
    ofu_item_t items[2] = {
        { _("Guardar"), OF_IC_SAVE, false, false },
        { _("Salir sin guardar"), OF_IC_CLOSE, true, false },
    };
    ofu_sheet(_("Hay cambios sin guardar"), items, 2, leave_pick, NULL);
}

static bool of_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (A.pr_overlay) { pr_close(); return true; }
    if (ofu_sheet_open()) { ofu_sheet_close(); return true; }
    if (A.scr == SCR_SLIDES) {
        if (!of_slidev_back()) of_app_home();
        return true;
    }
    if (A.scr == SCR_PDF) {
        of_app_home();
        return true;
    }
    if (A.scr == SCR_SHEET || A.scr == SCR_TEXT || A.scr == SCR_DOC) {
        if (A.scr == SCR_SHEET ? of_sheet_back() : A.scr == SCR_DOC ? of_docv_back() : of_textv_back()) return true;
        of_app_back_from_doc();
        return true;
    }
    if (A.tab == TAB_FILES && (A.arc || strcmp(A.dir, of_docs_dir()) != 0)) {
        up_cb(NULL);
        return true;
    }
    return false;
}

static void of_tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (A.scr == SCR_SHEET) of_sheet_tick();
}

static bool of_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    A.root = root;
    ofu_ui_root = root;
    if (A.scr == SCR_HOME) {
        A.list_scroll = 0;
        build();
        return true;
    }
    /* a document is built again with the new size, keeping its state */
    char p[300];
    snprintf(p, sizeof p, "%s", A.doc);
    if (A.scr == SCR_SHEET) {
        ofu_sheet_forget();
        pr_close();
        lv_obj_clean(root);
        A.pr_overlay = NULL;
        of_sheet_open(root, p, NULL);       /* NULL: the same book, laid out again */
    } else if (A.scr == SCR_TEXT) {
        ofu_sheet_forget();
        pr_close();
        lv_obj_clean(root);
        A.pr_overlay = NULL;
        of_textv_open(root, NULL);
    } else if (A.scr == SCR_DOC) {
        ofu_sheet_forget();
        pr_close();
        lv_obj_clean(root);
        A.pr_overlay = NULL;
        of_docv_open(root, p, NULL);
    } else if (A.scr == SCR_SLIDES) {
        ofu_sheet_forget();
        pr_close();
        lv_obj_clean(root);
        A.pr_overlay = NULL;
        of_slidev_open(root, p, NULL);
    } else if (A.scr == SCR_PDF) {
        ofu_sheet_forget();
        pr_close();
        lv_obj_clean(root);
        A.pr_overlay = NULL;
        of_pdfv_open(root, NULL, NULL, NULL, 0);
    }
    return true;
}

static bool of_init(aos_app_t *app)
{
    app->desc.id       = "aos.oficina";
    app->desc.name     = "Oficina";
    app->desc.icon     = LV_SYMBOL_FILE;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x34C759;
    app->desc.color_b  = 0x0A7A3A;
    app->desc.flags    = AOS_APP_FLAG_KEEP;
    app->desc.order    = 125;
    aos_icon_set_ops(app, OF_ICON, sizeof OF_ICON);
    app->create  = of_create;
    app->destroy = of_destroy;
    app->show    = of_show;
    app->back    = of_back;
    app->tick    = of_tick;
    app->resize  = of_resize;
    return true;
}

AOS_APP_ENTRY(of_init);
