/*
 * P4OS - Notes: the folder on the card.
 *
 * One Markdown file per note in /sdcard/notas, named after its title, so
 * the portal's file manager lists them by name; the bin is a folder inside
 * it. A file is renamed only when its note is closed (a title being typed
 * would rename it on every autosave), and it is written to a .tmp first and
 * swapped in, so a reset halfway leaves the old note and not half of it.
 */
#include "nt.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_FILE   (256 * 1024)     /* past this a file is not a note */
#define PREVIEW    2048             /* plain text kept per note, for search */

static char s_dir[128], s_trash[160];

static void ensure(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) mkdir(p, 0777);
}

const char *nt_dir(void)
{
    const char *root = aos_hal_path_sd_root();
    snprintf(s_dir, sizeof s_dir, "%s/notas", root ? root : aos_hal_path_data());
    ensure(s_dir);
    return s_dir;
}

const char *nt_trash_dir(void)
{
    snprintf(s_trash, sizeof s_trash, "%s/papelera", nt_dir());
    ensure(s_trash);
    return s_trash;
}

uint32_t nt_now(void)
{
    if (!aos_hal_time_is_valid()) return 0;
    time_t t = time(NULL);
    return t > 0 ? (uint32_t)t : 0;
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > MAX_FILE) { fclose(f); return NULL; }
    char *buf = nt_alloc((size_t)n + 1);
    if (buf && n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { nt_free(buf); buf = NULL; }
    fclose(f);
    if (buf) buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

static bool is_note(const char *name)
{
    if (name[0] == '.') return false;
    size_t n = strlen(name);
    return (n > 3 && strcasecmp(name + n - 3, ".md") == 0) ||
           (n > 4 && strcasecmp(name + n - 4, ".txt") == 0);
}

static void stem(const char *file, char *out, size_t n)
{
    snprintf(out, n, "%s", file);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

typedef struct {
    nt_index_t *ix;
    const char *dir;
} scan_t;

static bool scan_cb(const aos_dir_entry_t *e, void *ctx)
{
    scan_t *s = ctx;
    if (e->dir || !is_note(e->name) || strlen(e->name) >= sizeof(((nt_entry_t *)0)->file)) return true;
    nt_index_t *ix = s->ix;
    if (ix->n + 1 > ix->cap) {
        int cap = ix->cap ? ix->cap * 2 : 32;
        nt_entry_t *q = nt_realloc(ix->e, cap * sizeof(nt_entry_t));
        if (!q) return false;
        ix->e = q;
        ix->cap = cap;
    }
    nt_entry_t *en = &ix->e[ix->n];
    memset(en, 0, sizeof *en);
    snprintf(en->file, sizeof en->file, "%s", e->name);
    en->mtime = e->mtime;
    ix->n++;
    return true;
}

static void fill_entry(nt_entry_t *en, const char *dir)
{
    char path[256], name[96];
    snprintf(path, sizeof path, "%s/%s", dir, en->file);
    stem(en->file, name, sizeof name);
    char *src = read_all(path, NULL);
    nt_doc_t d;
    nt_doc_init(&d);
    nt_doc_from_md(&d, &en->meta, src ? src : "", name);
    nt_free(src);
    snprintf(en->title, sizeof en->title, "%s", d.b[0].txt);
    nt_sb_t sb = { 0 };
    nt_doc_plain(&d, &sb, false);
    if (sb.n > PREVIEW) {
        int cut = PREVIEW;
        while (cut > 0 && ((unsigned char)sb.s[cut] & 0xC0) == 0x80) cut--;
        sb.s[cut] = 0;
    }
    en->text = sb.s ? sb.s : nt_strdup("");
    for (int i = 1; i < d.n; i++) {
        if (d.b[i].type != BT_CHECK) continue;
        if (d.b[i].len == 0 && en->meta.kind == NT_KIND_TASKS) continue;
        en->total++;
        if (d.b[i].checked) en->done++;
    }
    nt_doc_clear(&d);
}

void nt_index_free(nt_index_t *ix)
{
    for (int i = 0; i < ix->n; i++) nt_free(ix->e[i].text);
    nt_free(ix->e);
    memset(ix, 0, sizeof *ix);
}

void nt_index_scan(nt_index_t *ix, bool trash)
{
    nt_index_free(ix);
    scan_t s = { ix, trash ? nt_trash_dir() : nt_dir() };
    aos_hal_dir_scan(s.dir, scan_cb, &s);
    for (int i = 0; i < ix->n; i++) fill_entry(&ix->e[i], s.dir);
}

static int s_order;

/* A letter without its accent, lower case: "Árbol" sorts with the a's. */
static int base_char(const char **p)
{
    unsigned char c = (unsigned char)**p;
    if (c == 0xC3 && (*p)[1]) {
        unsigned char d = (unsigned char)(*p)[1] | 0x20;
        *p += 2;
        return d >= 0xA0 && d <= 0xA5 ? 'a' : d >= 0xA8 && d <= 0xAB ? 'e' :
               d >= 0xAC && d <= 0xAF ? 'i' : d >= 0xB2 && d <= 0xB6 ? 'o' :
               d >= 0xB9 && d <= 0xBC ? 'u' : d == 0xB1 ? 'n' + 1 : d == 0xA7 ? 'c' : 0x100 + d;
    }
    (*p)++;
    return tolower(c);
}

static int cmp_title(const char *a, const char *b)
{
    while (*a && *b) {
        int x = base_char(&a), y = base_char(&b);
        if (x != y) return x - y;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static uint32_t when(const nt_entry_t *e, bool created)
{
    uint32_t t = created ? e->meta.created : e->meta.modified;
    return t ? t : e->mtime;
}

static int cmp_entry(const void *a, const void *b)
{
    const nt_entry_t *x = a, *y = b;
    if (x->meta.pinned != y->meta.pinned) return x->meta.pinned ? -1 : 1;
    if (s_order == NT_ORDER_TITLE) {
        int c = cmp_title(x->title, y->title);
        if (c) return c;
    } else {
        uint32_t tx = when(x, s_order == NT_ORDER_CREATED), ty = when(y, s_order == NT_ORDER_CREATED);
        if (tx != ty) return tx > ty ? -1 : 1;
    }
    return strcasecmp(x->file, y->file);
}

void nt_index_sort(nt_index_t *ix, int order)
{
    s_order = order;
    if (ix->n > 1) qsort(ix->e, ix->n, sizeof(nt_entry_t), cmp_entry);
}

bool nt_load(const char *dir, const char *file, nt_doc_t *d, nt_meta_t *m)
{
    char path[256], name[96];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    stem(file, name, sizeof name);
    char *src = read_all(path, NULL);
    nt_doc_from_md(d, m, src ? src : "", name);
    nt_free(src);
    return src != NULL;
}

/* A title as a file name: no characters FAT refuses, no dots or spaces at
 * the ends, not too long (cut on a character). */
static void safe_name(const char *title, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = title; *p && o + 1 < n && o < 64; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) c = '-';
        out[o++] = (char)c;
    }
    out[o] = 0;
    while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0xC0) out[--o] = 0;  /* a cut lead byte */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = 0;
    size_t s = 0;
    while (out[s] == ' ' || out[s] == '.') s++;
    if (s) memmove(out, out + s, strlen(out + s) + 1);
    if (!out[0]) snprintf(out, n, "%s", _("Sin título"));
}

static bool exists(const char *dir, const char *file)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    struct stat st;
    return stat(path, &st) == 0;
}

/* "Compras.md", or "Compras (2).md" if that one is taken (but 'self' may
 * keep its own name). */
static void free_name(const char *dir, const char *base, const char *self, char *out, size_t n)
{
    snprintf(out, n, "%s.md", base);
    for (int k = 2; k < 1000; k++) {
        if (self && strcasecmp(out, self) == 0) return;
        if (!exists(dir, out)) return;
        snprintf(out, n, "%s (%d).md", base, k);
    }
}

bool nt_new_file(const char *title, char *file, size_t len)
{
    char base[96];
    safe_name(title, base, sizeof base);
    free_name(nt_dir(), base, NULL, file, len);
    return true;
}

static bool write_file(const char *dir, const char *file, const nt_sb_t *sb)
{
    char path[256], tmp[260];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(sb->s ? sb->s : "", 1, sb->n, f) == (size_t)sb->n;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return false; }
    remove(path);
    return rename(tmp, path) == 0;
}

bool nt_save(char *file, size_t len, nt_doc_t *d, nt_meta_t *m, bool rename_it)
{
    uint32_t now = nt_now();
    if (now) {
        m->modified = now;
        if (!m->created) m->created = now;
    }
    const char *dir = nt_dir();
    char want[96];
    if (!file[0] || rename_it) {
        char base[96];
        safe_name(d->b[0].txt, base, sizeof base);
        free_name(dir, base, file[0] ? file : NULL, want, sizeof want);
    } else {
        snprintf(want, sizeof want, "%s", file);
    }
    nt_sb_t sb = { 0 };
    nt_doc_to_md(d, m, &sb);
    bool ok = write_file(dir, want, &sb);
    nt_sb_free(&sb);
    if (!ok) return false;
    if (file[0] && strcmp(file, want) != 0) {
        char old[256];
        snprintf(old, sizeof old, "%s/%s", dir, file);
        /* on FAT "compras.md" -> "Compras.md" is the same file: written above */
        if (strcasecmp(file, want) != 0) remove(old);
    }
    snprintf(file, len, "%s", want);
    return true;
}

static bool move(const char *from_dir, const char *to_dir, const char *file)
{
    char base[96], name[96], a[256], b[256];
    stem(file, base, sizeof base);
    free_name(to_dir, base, NULL, name, sizeof name);
    snprintf(a, sizeof a, "%s/%s", from_dir, file);
    snprintf(b, sizeof b, "%s/%s", to_dir, name);
    return rename(a, b) == 0;
}

bool nt_trash(const char *file)
{
    return move(nt_dir(), nt_trash_dir(), file);
}

bool nt_restore(const char *file)
{
    return move(nt_trash_dir(), nt_dir(), file);
}

bool nt_purge(const char *file)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s", nt_trash_dir(), file);
    return remove(p) == 0;
}

static bool purge_cb(const aos_dir_entry_t *e, void *ctx)
{
    int *n = ctx;
    if (!e->dir && is_note(e->name) && nt_purge(e->name)) (*n)++;
    return true;
}

int nt_purge_all(void)
{
    int n = 0;
    aos_hal_dir_scan(nt_trash_dir(), purge_cb, &n);
    return n;
}

bool nt_duplicate(const char *file, char *out, size_t len)
{
    nt_doc_t d;
    nt_meta_t m;
    nt_doc_init(&d);
    if (!nt_load(nt_dir(), file, &d, &m)) { nt_doc_clear(&d); return false; }
    char title[160];
    snprintf(title, sizeof title, "%s %s", d.b[0].txt, _("(copia)"));
    nt_blk_set(&d.b[0], title, 0);
    m.pinned = false;
    m.created = 0;
    out[0] = 0;
    bool ok = nt_save(out, len, &d, &m, true);
    nt_doc_clear(&d);
    return ok;
}

/* -------------------------------------------------------------------------- */
/* The first time: a note that shows what the app does, and a list            */
/* -------------------------------------------------------------------------- */

/* One string per line, so a translator sees them one at a time. The block
 * marks stay out of the strings (a catalogue line that starts with "#" is a
 * comment); the inline ones are the formatting and have to survive the
 * translation. */
typedef struct {
    const char *mark;
    const char *text;
} seed_line_t;

static const seed_line_t WELCOME[] = {
    { "# ", N_("Lo que se puede hacer") },
    { "", N_("Texto en **negrita**, *cursiva*, <u>subrayado</u> y ~~tachado~~.") },
    { "", N_("<span style=\"color:red\">Colores</span>, <span style=\"background:yellow\">resaltado</span> y <span style=\"font-size:x-large\">tamaños</span>.") },
    { "## ", N_("Listas") },
    { "- ", N_("Con viñetas") },
    { "  \xE2\x97\xA6 ", N_("y con sangría") },
    { "\xE2\x98\x85 ", N_("De varios tipos") },
    { "1. ", N_("Numeradas") },
    { "2. ", N_("Que se cuentan solas") },
    { "- [x] ", N_("Y con casillas") },
    { "- [ ] ", N_("Que se tildan con un toque") },
    { "> ", N_("Las notas son archivos Markdown en la carpeta notas de la tarjeta: también se editan desde el portal.") },
    { "---", NULL },
    { "", N_("Tocá **Aa** para el formato. Escribir \"- \", \"1. \" o \"[] \" al principio de una línea también arma la lista.") },
};

static const seed_line_t SHOPPING[] = {
    { "- [ ] ", N_("Pan") },
    { "- [ ] ", N_("Leche") },
    { "- [ ] !! ", N_("Alimento para Mila") },
    { "- [x] ", N_("Café") },
    { "- [ ] ", N_("Pilas AA") },
};

static void seed_one(const char *title, const seed_line_t *lines, int n, int kind, int tag, bool pinned)
{
    nt_sb_t src = { 0 };
    nt_sb_put(&src, "---\ntitle: x\n---\n", -1);        /* so a first heading stays a heading */
    for (int i = 0; i < n; i++) {
        nt_sb_put(&src, lines[i].mark, -1);
        if (lines[i].text) nt_sb_put(&src, _(lines[i].text), -1);
        nt_sb_putc(&src, '\n');
    }
    nt_doc_t d;
    nt_meta_t m;
    nt_doc_init(&d);
    nt_doc_from_md(&d, &m, src.s, title);
    nt_sb_free(&src);
    nt_blk_set(&d.b[0], title, 0);
    m.kind = (uint8_t)kind;
    m.tag = (uint8_t)tag;
    m.pinned = pinned;
    char file[96] = "";
    nt_save(file, sizeof file, &d, &m, true);
    nt_doc_clear(&d);
}

static bool any_cb(const aos_dir_entry_t *e, void *ctx)
{
    if (!e->dir && is_note(e->name)) { *(bool *)ctx = true; return false; }
    return true;
}

void nt_seed(void)
{
    int32_t seeded = 0;
    if (aos_hal_pref_get_i32("nt_seeded", &seeded) && seeded) return;
    bool any = false;
    aos_hal_dir_scan(nt_dir(), any_cb, &any);
    if (!any) {
        seed_one(_("Compras"), SHOPPING, (int)(sizeof SHOPPING / sizeof SHOPPING[0]), NT_KIND_TASKS, 4, false);
        seed_one(_("Bienvenida a Notas"), WELCOME, (int)(sizeof WELCOME / sizeof WELCOME[0]), NT_KIND_NOTE, 3, true);
    }
    aos_hal_pref_set_i32("nt_seeded", 1);
}
