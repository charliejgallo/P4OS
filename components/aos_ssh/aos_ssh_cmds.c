/*
 * P4OS - the SSH shell's commands. Each takes argc/argv like a Unix one and
 * returns its exit status; the ones that may write a lot hand a job to the
 * shell (aos_ssh_shell.c) and return at once.
 *
 * Their names are the Unix ones a terminal user types without thinking (ls,
 * cat, rm, top, free, df); what they print is in the board's language.
 */
#include "aos_ssh_int.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_app.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- helpers ---- */

typedef struct {
    char *p;
    size_t len, cap;
} tb_t;

static void tb_add(tb_t *t, const char *d, size_t n)
{
    if (t->len + n + 1 > t->cap) {
        size_t c = t->cap ? t->cap * 2 : 4096;
        while (c < t->len + n + 1) c *= 2;
        char *q = realloc(t->p, c);
        if (!q) return;
        t->p = q;
        t->cap = c;
    }
    memcpy(t->p + t->len, d, n);
    t->len += n;
    t->p[t->len] = 0;
}

static void tb_printf(tb_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tb_printf(tb_t *t, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) tb_add(t, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void tb_color(sh_t *s, tb_t *t, const char *sgr)
{
    if (!sh_is_pty(s)) return;
    tb_printf(t, "\x1b[%sm", sgr ? sgr : "0");
}

/* A text already made, written out as room allows: what a long listing
 * becomes. The job owns the text. */
typedef struct {
    tb_t t;
    size_t off;
} text_job_t;

static bool text_step(sh_t *s, void *st)
{
    text_job_t *j = st;
    size_t room = sh_out_room(s) / 2;      /* \n may become \r\n */
    size_t n = j->t.len - j->off;
    if (n > room) n = room;
    sh_write(s, j->t.p + j->off, n);
    j->off += n;
    return j->off >= j->t.len;
}

static void text_stop(sh_t *s, void *st)
{
    (void)s;
    text_job_t *j = st;
    free(j->t.p);
    free(j);
}

/* hands t over: printed now if it fits, else as a job */
static void emit(sh_t *s, tb_t *t)
{
    if (!t->len) {
        free(t->p);
        return;
    }
    if (t->len * 2 < sh_out_room(s)) {
        sh_write(s, t->p, t->len);
        free(t->p);
        return;
    }
    text_job_t *j = calloc(1, sizeof *j);
    if (!j) {
        free(t->p);
        return;
    }
    j->t = *t;
    sh_job_t job = { text_step, text_stop, NULL, j, 0 };
    sh_job(s, &job);
}

static void human(uint64_t b, char *out, size_t n)
{
    const char *u = "BKMGT";
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    if (!i) snprintf(out, n, "%u", (unsigned)b);
    else snprintf(out, n, v < 10 ? "%.1f%c" : "%.0f%c", v, u[i]);
}

static void say_err(sh_t *s, const char *cmd, const char *what, const char *why)
{
    sh_printf(s, "%s: %s: %s\n", cmd, what, why);
}

/* options of the form -abc before the arguments; a letter not in 'known'
 * is an error. Returns the index of the first argument, -1 on error. */
static int opts(sh_t *s, int argc, char **argv, const char *known, bool *seen)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) return i + 1;
        for (const char *p = argv[i] + 1; *p; p++) {
            const char *k = strchr(known, *p);
            if (!k) {
                sh_printf(s, _("%s: no conozco la opción -%c\n"), argv[0], *p);
                return -1;
            }
            seen[k - known] = true;
        }
    }
    return i;
}

static bool path_or_say(sh_t *s, const char *cmd, const char *in, char *virt, size_t vn, char *real, size_t rn)
{
    if (sh_path(s, in, virt, vn, real, rn)) return true;
    say_err(s, cmd, in, aos_hal_path_sd_root() ? _("ruta inválida") : _("no hay tarjeta"));
    return false;
}

/* ---- help, exit, clear, echo ---- */

static int c_help(sh_t *s, int argc, char **argv)
{
    int w = 0;
    for (int i = 0; i < SH_CMD_COUNT; i++) {
        int l = (int)(strlen(SH_CMDS[i].name) + 1 + strlen(SH_CMDS[i].args));
        if (l > w) w = l;
    }
    tb_t t = { 0 };
    for (int i = 0; i < SH_CMD_COUNT; i++) {
        const sh_cmd_t *c = &SH_CMDS[i];
        if (argc > 1 && strcmp(argv[1], c->name)) continue;
        char u[96];
        snprintf(u, sizeof u, "%s %s", c->name, c->args);
        tb_color(s, &t, "1");
        tb_printf(&t, "  %-*s", w, u);
        tb_color(s, &t, NULL);
        tb_printf(&t, "  %s\n", _(c->help));
    }
    if (argc > 1 && !t.len) {
        sh_printf(s, _("help: no existe «%s»\n"), argv[1]);
        return 1;
    }
    emit(s, &t);
    return 0;
}

static int c_exit(sh_t *s, int argc, char **argv)
{
    sh_request_exit(s, argc > 1 ? atoi(argv[1]) : 0);
    return argc > 1 ? atoi(argv[1]) : 0;
}

static int c_clear(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (sh_is_pty(s)) sh_puts(s, "\x1b[H\x1b[2J");
    return 0;
}

static int c_echo(sh_t *s, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) sh_printf(s, "%s%s", i > 1 ? " " : "", argv[i]);
    sh_puts(s, "\n");
    return 0;
}

/* ---- files ---- */

static int c_pwd(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_printf(s, "%s\n", sh_cwd(s));
    return 0;
}

static int c_cd(sh_t *s, int argc, char **argv)
{
    char virt[256], real[320];
    if (!path_or_say(s, "cd", argc > 1 ? argv[1] : "/", virt, sizeof virt, real, sizeof real)) return 1;
    struct stat st;
    if (stat(real, &st) || !S_ISDIR(st.st_mode)) {
        say_err(s, "cd", argc > 1 ? argv[1] : "/", _("no es una carpeta"));
        return 1;
    }
    sh_set_cwd(s, virt);
    return 0;
}

typedef struct {
    char name[256];
    bool dir;
    uint64_t size;
    time_t mtime;
} ent_t;

static int ent_cmp(const void *a, const void *b)
{
    return strcasecmp(((const ent_t *)a)->name, ((const ent_t *)b)->name);
}

static int width_of(const char *p)
{
    int w = 0;
    for (; *p; p++)
        if (((uint8_t)*p & 0xC0) != 0x80) w++;
    return w;
}

#define LS_MAX 4000

/* the pendrives, at the card's root */
static int usb_ents(ent_t *e, int max)
{
    int n = 0;
    if (!aos_hal_usb_host_on()) return 0;
    aos_usb_host_info_t in[AOS_USB_HOST_MAX];
    int k = aos_hal_usb_host_devices(in, AOS_USB_HOST_MAX);
    for (int i = 0; i < k && n < max; i++)
        if (in[i].mounted && in[i].path[0] == '/') {
            memset(&e[n], 0, sizeof e[n]);
            snprintf(e[n].name, sizeof e[n].name, "%s", in[i].path + 1);
            e[n].dir = true;
            n++;
        }
    return n;
}

/* false: it is not there */
static bool ls_one(sh_t *s, tb_t *t, const char *virt, const char *real, bool lng, bool all, bool title)
{
    struct stat st;
    if (stat(real, &st)) {
        tb_printf(t, "ls: %s: %s\n", virt, strerror(errno));
        return false;
    }
    ent_t *e = malloc(sizeof(ent_t) * LS_MAX);
    if (!e) return false;
    int n = 0;
    bool more = false;
    if (!S_ISDIR(st.st_mode)) {
        const char *b = strrchr(virt, '/');
        snprintf(e[0].name, sizeof e[0].name, "%s", b ? b + 1 : virt);
        e[0].dir = false;
        e[0].size = (uint64_t)st.st_size;
        e[0].mtime = st.st_mtime;
        n = 1;
    } else {
        DIR *d = opendir(real);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (de->d_name[0] == '.' && !all) continue;
            if (n == LS_MAX) {
                more = true;
                break;
            }
            snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name);
            e[n].dir = de->d_type == DT_DIR;
            e[n].size = 0;
            e[n].mtime = 0;
            if (lng || de->d_type == DT_UNKNOWN) {
                char full[600];
                struct stat fs;
                snprintf(full, sizeof full, "%s/%s", real, de->d_name);
                if (!stat(full, &fs)) {
                    e[n].dir = S_ISDIR(fs.st_mode);
                    e[n].size = (uint64_t)fs.st_size;
                    e[n].mtime = fs.st_mtime;
                }
            }
            n++;
        }
        if (d) closedir(d);
        if (!strcmp(virt, "/")) n += usb_ents(e + n, LS_MAX - n);
    }
    qsort(e, (size_t)n, sizeof *e, ent_cmp);
    if (title) tb_printf(t, "%s:\n", virt);
    if (lng) {
        for (int i = 0; i < n; i++) {
            char sz[16], when[24] = "";
            human(e[i].size, sz, sizeof sz);
            if (e[i].mtime > 0) {
                struct tm tm;
                localtime_r(&e[i].mtime, &tm);
                strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
            }
            tb_printf(t, "%c %7s  %-16s  ", e[i].dir ? 'd' : '-', e[i].dir ? "" : sz, when);
            if (e[i].dir) tb_color(s, t, "1;34");
            tb_printf(t, "%s%s", e[i].name, e[i].dir ? "/" : "");
            if (e[i].dir) tb_color(s, t, NULL);
            tb_add(t, "\n", 1);
        }
    } else if (!sh_is_pty(s)) {
        for (int i = 0; i < n; i++) tb_printf(t, "%s\n", e[i].name);
    } else if (n) {
        /* in columns, down then across, as ls does */
        int w = 1;
        for (int i = 0; i < n; i++) {
            int l = width_of(e[i].name) + e[i].dir;
            if (l > w) w = l;
        }
        w += 2;
        int per = sh_cols(s) / w > 0 ? sh_cols(s) / w : 1;
        int rows = (n + per - 1) / per;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < per; c++) {
                int i = c * rows + r;
                if (i >= n) break;
                if (e[i].dir) tb_color(s, t, "1;34");
                tb_printf(t, "%s%s", e[i].name, e[i].dir ? "/" : "");
                if (e[i].dir) tb_color(s, t, NULL);
                int pad = w - width_of(e[i].name) - e[i].dir;
                if (c + 1 < per && (c + 1) * rows + r < n)
                    for (int k = 0; k < pad; k++) tb_add(t, " ", 1);
            }
            tb_add(t, "\n", 1);
        }
    }
    if (more) tb_printf(t, _("(y más: se muestran las primeras %d)\n"), LS_MAX);
    free(e);
    return true;
}

static int c_ls(sh_t *s, int argc, char **argv)
{
    bool o[2] = { 0 };
    int i = opts(s, argc, argv, "la", o);
    if (i < 0) return 2;
    tb_t t = { 0 };
    int many = argc - i > 1, rc = 0;
    if (i == argc) {
        char virt[256], real[320];
        if (!path_or_say(s, "ls", ".", virt, sizeof virt, real, sizeof real) ||
            !ls_one(s, &t, virt, real, o[0], o[1], false))
            rc = 1;
    }
    for (; i < argc; i++) {
        char virt[256], real[320];
        if (!path_or_say(s, "ls", argv[i], virt, sizeof virt, real, sizeof real)) {
            rc = 1;
            continue;
        }
        if (!ls_one(s, &t, virt, real, o[0], o[1], many)) rc = 1;
        if (many && i + 1 < argc) tb_add(&t, "\n", 1);
    }
    emit(s, &t);
    return rc;
}

/* cat: the files one after the other, in pieces */
typedef struct {
    char **paths;
    int n, i;
    FILE *f;
    char *buf;
    int rc;
    char cmd[8];
} cat_job_t;

static bool cat_step(sh_t *s, void *st)
{
    cat_job_t *j = st;
    while (!j->f) {
        if (j->i >= j->n) return true;
        j->f = fopen(j->paths[j->i], "rb");
        if (!j->f) {
            say_err(s, "cat", j->paths[j->i], strerror(errno));
            j->rc = 1;
        }
        j->i++;
    }
    size_t room = sh_out_room(s) / 2;
    if (room > 16384) room = 16384;
    size_t k = fread(j->buf, 1, room, j->f);
    if (k) sh_write(s, j->buf, k);
    if (k < room) {
        fclose(j->f);
        j->f = NULL;
    }
    return false;
}

static void cat_stop(sh_t *s, void *st)
{
    (void)s;
    cat_job_t *j = st;
    if (j->f) fclose(j->f);
    for (int i = 0; i < j->n; i++) free(j->paths[i]);
    free(j->paths);
    free(j->buf);
    free(j);
}

static int c_cat(sh_t *s, int argc, char **argv)
{
    if (argc < 2) {
        sh_puts(s, _("cat: ¿qué archivo?\n"));
        return 2;
    }
    cat_job_t *j = calloc(1, sizeof *j);
    if (!j) return 1;
    j->paths = calloc((size_t)argc, sizeof *j->paths);
    j->buf = malloc(16384);
    int rc = 0;
    for (int i = 1; j->paths && i < argc; i++) {
        char virt[256], real[320];
        if (!path_or_say(s, "cat", argv[i], virt, sizeof virt, real, sizeof real)) {
            rc = 1;
            continue;
        }
        j->paths[j->n++] = strdup(real);
    }
    if (!j->paths || !j->buf || !j->n) {
        cat_stop(s, j);
        return rc ? rc : 1;
    }
    sh_job_t job = { cat_step, cat_stop, NULL, j, 0 };
    sh_job(s, &job);
    return rc;
}

/* head and tail: lines of one file */
static int line_count(sh_t *s, int argc, char **argv, int *first)
{
    int n = 10;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        n = atoi(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) {
        n = atoi(argv[i] + 1);
        i++;
    }
    *first = i;
    if (i >= argc) {
        sh_printf(s, _("%s: ¿qué archivo?\n"), argv[0]);
        return -1;
    }
    return n < 0 ? 10 : n;
}

static int c_head(sh_t *s, int argc, char **argv)
{
    int i, n = line_count(s, argc, argv, &i);
    if (n < 0) return 2;
    char virt[256], real[320];
    if (!path_or_say(s, "head", argv[i], virt, sizeof virt, real, sizeof real)) return 1;
    FILE *f = fopen(real, "rb");
    if (!f) {
        say_err(s, "head", argv[i], strerror(errno));
        return 1;
    }
    tb_t t = { 0 };
    char line[1024];
    for (int k = 0; k < n && fgets(line, sizeof line, f); k++) tb_add(&t, line, strlen(line));
    fclose(f);
    emit(s, &t);
    return 0;
}

/* the last n lines of a piece of text, from where they start */
static const char *last_lines(const char *p, size_t len, int n)
{
    if (n <= 0) return p;
    const char *e = p + len;
    if (e > p && e[-1] == '\n') e--;
    while (e > p) {
        e--;
        if (*e == '\n' && --n == 0) return e + 1;
    }
    return p;
}

static int c_tail(sh_t *s, int argc, char **argv)
{
    int i, n = line_count(s, argc, argv, &i);
    if (n < 0) return 2;
    char virt[256], real[320];
    if (!path_or_say(s, "tail", argv[i], virt, sizeof virt, real, sizeof real)) return 1;
    FILE *f = fopen(real, "rb");
    if (!f) {
        say_err(s, "tail", argv[i], strerror(errno));
        return 1;
    }
    const long back = 65536;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > back ? size - back : 0, SEEK_SET);
    char *buf = malloc((size_t)back + 1);
    size_t k = buf ? fread(buf, 1, (size_t)back, f) : 0;
    fclose(f);
    if (!buf) return 1;
    buf[k] = 0;
    const char *from = last_lines(buf, k, n);
    tb_t t = { 0 };
    tb_add(&t, from, k - (size_t)(from - buf));
    free(buf);
    emit(s, &t);
    return 0;
}

static bool rm_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st)) return false;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0;
    DIR *d = opendir(path);
    struct dirent *de;
    bool ok = d != NULL;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *sub = malloc(600);
        if (!sub) {
            ok = false;
            break;
        }
        snprintf(sub, 600, "%s/%s", path, de->d_name);
        ok &= rm_tree(sub);
        free(sub);
    }
    if (d) closedir(d);
    return ok && rmdir(path) == 0;
}

/* a top folder (the card's root, a pendrive's) is not for rm, rmdir or mv */
static bool is_top(const char *virt)
{
    if (!strcmp(virt, "/")) return true;
    return !strncmp(virt, "/usb", 4) && !strchr(virt + 1, '/');
}

static int c_rm(sh_t *s, int argc, char **argv)
{
    bool o[2] = { 0 };
    int i = opts(s, argc, argv, "rf", o);
    if (i < 0) return 2;
    if (i == argc) {
        sh_puts(s, _("rm: ¿qué borro?\n"));
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++) {
        char virt[256], real[320];
        struct stat st;
        if (!path_or_say(s, "rm", argv[i], virt, sizeof virt, real, sizeof real)) {
            rc = 1;
            continue;
        }
        if (is_top(virt)) {
            say_err(s, "rm", argv[i], _("eso no se borra"));
            rc = 1;
            continue;
        }
        if (stat(real, &st)) {
            if (!o[1]) say_err(s, "rm", argv[i], strerror(errno));
            rc |= !o[1];
            continue;
        }
        if (S_ISDIR(st.st_mode) && !o[0]) {
            say_err(s, "rm", argv[i], _("es una carpeta (rm -r)"));
            rc = 1;
            continue;
        }
        if (!(S_ISDIR(st.st_mode) ? rm_tree(real) : remove(real) == 0)) {
            say_err(s, "rm", argv[i], strerror(errno));
            rc = 1;
        } else aos_hal_log("ssh", "%s: rm %s", sh_from(s), virt);
    }
    return rc;
}

static int c_mkdir(sh_t *s, int argc, char **argv)
{
    bool o[1] = { 0 };
    int i = opts(s, argc, argv, "p", o);
    if (i < 0) return 2;
    if (i == argc) {
        sh_puts(s, _("mkdir: ¿qué carpeta?\n"));
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++) {
        char virt[256], real[320];
        if (!path_or_say(s, "mkdir", argv[i], virt, sizeof virt, real, sizeof real)) {
            rc = 1;
            continue;
        }
        if (o[0]) {
            /* each missing parent on the way, as mkdir -p does */
            size_t root = strlen(real) - strlen(virt);
            for (char *p = real + root + 1; *p; p++)
                if (*p == '/') {
                    *p = 0;
                    mkdir(real, 0777);
                    *p = '/';
                }
            struct stat st;
            if (!stat(real, &st) && S_ISDIR(st.st_mode)) continue;
        }
        if (mkdir(real, 0777)) {
            say_err(s, "mkdir", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int c_rmdir(sh_t *s, int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        sh_puts(s, _("rmdir: ¿qué carpeta?\n"));
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        char virt[256], real[320];
        if (!path_or_say(s, "rmdir", argv[i], virt, sizeof virt, real, sizeof real)) {
            rc = 1;
            continue;
        }
        if (is_top(virt) || rmdir(real)) {
            say_err(s, "rmdir", argv[i], is_top(virt) ? _("eso no se borra") : strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

/* the destination of mv and cp: inside it when it is a folder */
static bool dest_path(sh_t *s, const char *cmd, const char *src_virt, const char *dst, char *real, size_t rn)
{
    char virt[256];
    if (!path_or_say(s, cmd, dst, virt, sizeof virt, real, rn)) return false;
    struct stat st;
    if (!stat(real, &st) && S_ISDIR(st.st_mode)) {
        const char *b = strrchr(src_virt, '/');
        size_t l = strlen(real);
        snprintf(real + l, rn - l, "/%s", b ? b + 1 : src_virt);
    }
    return true;
}

static int c_mv(sh_t *s, int argc, char **argv)
{
    if (argc != 3) {
        sh_puts(s, _("mv: mv origen destino\n"));
        return 2;
    }
    char sv[256], sr[320], dr[600];
    if (!path_or_say(s, "mv", argv[1], sv, sizeof sv, sr, sizeof sr)) return 1;
    if (is_top(sv)) {
        say_err(s, "mv", argv[1], _("eso no se mueve"));
        return 1;
    }
    if (!dest_path(s, "mv", sv, argv[2], dr, sizeof dr)) return 1;
    if (rename(sr, dr)) {
        say_err(s, "mv", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

typedef struct {
    FILE *in, *out;
    char *buf;
    char dst[600];
    bool failed;
} cp_job_t;

static bool cp_step(sh_t *s, void *st)
{
    (void)s;
    cp_job_t *j = st;
    for (int k = 0; k < 4; k++) {               /* 128 KB a turn, so Ctrl-C is heard */
        size_t n = fread(j->buf, 1, 32768, j->in);
        if (n && fwrite(j->buf, 1, n, j->out) != n) {
            j->failed = true;
            return true;
        }
        if (n < 32768) return true;
    }
    return false;
}

static void cp_stop(sh_t *s, void *st)
{
    cp_job_t *j = st;
    bool done = !j->failed && feof(j->in);
    fclose(j->in);
    if (fclose(j->out)) done = false;
    if (!done) {
        remove(j->dst);
        sh_puts(s, _("cp: no terminó; el destino se borró\n"));
    }
    free(j->buf);
    free(j);
}

static int c_cp(sh_t *s, int argc, char **argv)
{
    if (argc != 3) {
        sh_puts(s, _("cp: cp origen destino\n"));
        return 2;
    }
    char sv[256], sr[320];
    if (!path_or_say(s, "cp", argv[1], sv, sizeof sv, sr, sizeof sr)) return 1;
    struct stat st;
    if (stat(sr, &st) || S_ISDIR(st.st_mode)) {
        say_err(s, "cp", argv[1], S_ISDIR(st.st_mode) ? _("es una carpeta") : strerror(errno));
        return 1;
    }
    cp_job_t *j = calloc(1, sizeof *j);
    if (!j || !dest_path(s, "cp", sv, argv[2], j->dst, sizeof j->dst)) {
        free(j);
        return 1;
    }
    j->buf = malloc(32768);
    j->in = fopen(sr, "rb");
    j->out = j->in ? fopen(j->dst, "wb") : NULL;
    if (!j->buf || !j->in || !j->out) {
        say_err(s, "cp", j->in ? argv[2] : argv[1], strerror(errno));
        if (j->in) fclose(j->in);
        free(j->buf);
        free(j);
        return 1;
    }
    sh_job_t job = { cp_step, cp_stop, NULL, j, 0 };
    sh_job(s, &job);
    return 0;
}

static int c_touch(sh_t *s, int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char virt[256], real[320];
        if (!path_or_say(s, "touch", argv[i], virt, sizeof virt, real, sizeof real)) {
            rc = 1;
            continue;
        }
        FILE *f = fopen(real, "ab");
        if (!f) {
            say_err(s, "touch", argv[i], strerror(errno));
            rc = 1;
        } else fclose(f);
    }
    return rc;
}

static int c_df(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint64_t total = 0, freeb = 0;
    char a[16], b[16], c[16];
    sh_printf(s, "%-8s %8s %8s %8s\n", "", _("total"), _("usado"), _("libre"));
    if (aos_hal_sd_usage(&total, &freeb)) {
        human(total, a, sizeof a);
        human(total - freeb, b, sizeof b);
        human(freeb, c, sizeof c);
        sh_printf(s, "%-8s %8s %8s %8s\n", "/", a, b, c);
    } else sh_printf(s, "%-8s %s\n", "/", _("no hay tarjeta"));
    if (aos_hal_usb_host_on()) {
        aos_usb_host_info_t in[AOS_USB_HOST_MAX];
        int n = aos_hal_usb_host_devices(in, AOS_USB_HOST_MAX);
        for (int i = 0; i < n; i++)
            if (in[i].mounted) {
                human(in[i].bytes, a, sizeof a);
                sh_printf(s, "%-8s %8s %8s %8s\n", in[i].path, a, "?", "?");
            }
    }
    return 0;
}

/* ---- the system ---- */

static void uptime_text(char *out, size_t n)
{
    uint64_t t = aos_hal_uptime_ms() / 1000;
    unsigned d = (unsigned)(t / 86400), h = (unsigned)(t / 3600 % 24), m = (unsigned)(t / 60 % 60);
    if (d) snprintf(out, n, _("%u d %u h %u min"), d, h, m);
    else if (h) snprintf(out, n, _("%u h %u min"), h, m);
    else snprintf(out, n, _("%u min %u s"), m, (unsigned)(t % 60));
}

static int c_uptime(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char u[48];
    uptime_text(u, sizeof u);
    sh_printf(s, _("prendida hace %s\n"), u);
    return 0;
}

static int c_date(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!aos_hal_time_is_valid()) {
        sh_puts(s, _("la placa todavía no sabe la hora\n"));
        return 1;
    }
    struct tm tm;
    aos_hal_time_now(&tm);
    char b[64];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    sh_printf(s, "%s  (%s)\n", b, aos_hal_timezone_get());
    return 0;
}

static int c_uname(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    aos_sys_info_t in;
    memset(&in, 0, sizeof in);
    aos_hal_sys_info(&in);
    sh_printf(s, "P4OS %s · %s\n", aos_hal_firmware_version(), aos_hal_board_name());
    sh_printf(s, _("%s rev %d.%d, %d núcleos a %d MHz · ESP-IDF %s\n"), in.chip ? in.chip : "?", in.chip_rev / 100,
              in.chip_rev % 100, in.cores, in.cpu_mhz, in.idf_version ? in.idf_version : "?");
    sh_printf(s, _("ranura %s · último reinicio: %s\n"), in.app_slot ? in.app_slot : "?",
              in.reset_reason ? in.reset_reason : "?");
    return 0;
}

static int c_free(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    aos_sys_info_t in;
    memset(&in, 0, sizeof in);
    if (!aos_hal_sys_info(&in)) return 1;
    static const char *const NAMES[] = { N_("interna"), "DMA", "PSRAM" };
    sh_printf(s, "%-8s %8s %8s %8s %8s %8s\n", "", _("total"), _("usada"), _("libre"), _("bloque"), _("mínima"));
    for (int i = 0; i < 3; i++) {
        const aos_mem_region_t *m = &in.mem[i];
        char a[16], b[16], c[16], d[16], e[16];
        human(m->total, a, sizeof a);
        human(m->total - m->free, b, sizeof b);
        human(m->free, c, sizeof c);
        human(m->largest, d, sizeof d);
        human(m->min_free, e, sizeof e);
        sh_printf(s, "%-8s %8s %8s %8s %8s %8s\n", _(NAMES[i]), a, b, c, d, e);
    }
    return 0;
}

static int task_cmp(const void *a, const void *b)
{
    const aos_task_info_t *x = a, *y = b;
    if (x->cpu_x10 != y->cpu_x10) return y->cpu_x10 - x->cpu_x10;
    return strcmp(x->name, y->name);
}

static const char *task_state(uint8_t st)
{
    switch (st) {
    case AOS_TASK_RUNNING: return "R";
    case AOS_TASK_READY: return "r";
    case AOS_TASK_BLOCKED: return "B";
    case AOS_TASK_SUSPENDED: return "S";
    case AOS_TASK_DELETED: return "D";
    default: return "?";
    }
}

/* the task table, at most 'max' rows (0: all) */
static void tasks_text(sh_t *s, tb_t *t, int max)
{
    aos_task_info_t *tk = malloc(sizeof *tk * 96);
    if (!tk) return;
    int n = aos_hal_tasks(tk, 96);
    if (n > 96) n = 96;
    qsort(tk, (size_t)n, sizeof *tk, task_cmp);
    tb_color(s, t, "7");
    tb_printf(t, "%-20s %4s %4s %2s %6s %7s %7s", _("tarea"), _("cpu"), _("prio"), "", _("núcleo"), _("pila"), _("libre"));
    tb_color(s, t, NULL);
    tb_add(t, "\n", 1);
    for (int i = 0; i < n && (!max || i < max); i++) {
        char cpu[8] = "-", core[4] = "*", st[12] = "?", fr[12] = "?";
        if (tk[i].cpu_x10 >= 0) snprintf(cpu, sizeof cpu, "%d.%d", tk[i].cpu_x10 / 10, tk[i].cpu_x10 % 10);
        if (tk[i].core >= 0) snprintf(core, sizeof core, "%d", tk[i].core);
        if (tk[i].stack_size >= 0) snprintf(st, sizeof st, "%ld%s", (long)tk[i].stack_size / 1024, tk[i].stack_psram ? "K*" : "K");
        if (tk[i].stack_min_free >= 0) snprintf(fr, sizeof fr, "%ld", (long)tk[i].stack_min_free);
        tb_printf(t, "%-20.20s %4s %4u %2s %6s %7s %7s\n", tk[i].name, cpu, tk[i].prio, task_state(tk[i].state), core, st, fr);
    }
    if (max && n > max) tb_printf(t, _("(y %d más)\n"), n - max);
    free(tk);
}

static int c_ps(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    tb_t t = { 0 };
    tasks_text(s, &t, 0);
    tb_printf(&t, "%s", _("cpu: % de un núcleo · pila: K* en PSRAM · libre: lo mínimo que tuvo, en bytes\n"));
    emit(s, &t);
    return 0;
}

static bool top_step(sh_t *s, void *st)
{
    (void)st;
    tb_t t = { 0 };
    tb_add(&t, "\x1b[H\x1b[2J", 7);
    char u[48];
    uptime_text(u, sizeof u);
    aos_sys_info_t in;
    memset(&in, 0, sizeof in);
    aos_hal_sys_info(&in);
    char a[16], b[16];
    human(in.mem[AOS_MEM_INTERNAL].free, a, sizeof a);
    human(in.mem[AOS_MEM_SPIRAM].free, b, sizeof b);
    tb_printf(&t, _("P4OS %s · prendida hace %s · libre: %s interna, %s PSRAM · q sale\n\n"),
              aos_hal_firmware_version(), u, a, b);
    tasks_text(s, &t, sh_rows(s) > 6 ? sh_rows(s) - 5 : 0);
    sh_write(s, t.p, t.len);
    free(t.p);
    return false;
}

static bool top_key(sh_t *s, void *st, uint8_t c)
{
    (void)s;
    (void)st;
    return c == 'q' || c == 'Q';
}

static int c_top(sh_t *s, int argc, char **argv)
{
    if (!sh_is_pty(s)) return c_ps(s, argc, argv);
    sh_job_t job = { top_step, NULL, top_key, NULL, 1000 };
    sh_job(s, &job);
    return 0;
}

/* log: the ring's last lines, the previous boot's, and -f to follow */
typedef struct {
    size_t next;
    char *buf;
} log_job_t;

#define LOG_CHUNK 16384

static bool log_step(sh_t *s, void *st)
{
    log_job_t *j = st;
    size_t room = sh_out_room(s) / 2;
    if (room > LOG_CHUNK) room = LOG_CHUNK;
    size_t next = j->next;
    size_t k = aos_hal_log_read(j->next, j->buf, room + 1, &next);
    if (next - j->next > k + 1) sh_printf(s, _("[se perdieron %u bytes del registro]\n"), (unsigned)(next - j->next - k));
    if (k) sh_write(s, j->buf, k);
    j->next = next;
    return false;
}

static void log_stop(sh_t *s, void *st)
{
    (void)s;
    log_job_t *j = st;
    free(j->buf);
    free(j);
}

static int c_log(sh_t *s, int argc, char **argv)
{
    bool follow = false, prev = false;
    int n = 40;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) follow = true;
        else if (!strcmp(argv[i], "-p")) prev = true;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else {
            sh_puts(s, _("log: log [-f] [-p] [-n líneas]\n"));
            return 2;
        }
    }
    const size_t want = 65536;
    char *buf = malloc(want + 1);
    if (!buf) return 1;
    size_t next = 0, k;
    if (prev) {
        k = aos_hal_log_prev_read(0, buf, want + 1, &next);
        if (!k) sh_puts(s, _("no hay registro del arranque anterior\n"));
    } else {
        size_t total = aos_hal_log_total();
        k = aos_hal_log_read(total > want ? total - want : 0, buf, want + 1, &next);
    }
    const char *from = last_lines(buf, k, n);
    tb_t t = { 0 };
    tb_add(&t, from, k - (size_t)(from - buf));
    if (!follow || prev) {
        free(buf);
        emit(s, &t);
        return 0;
    }
    sh_write(s, t.p ? t.p : "", t.len);    /* at most 64 KB: may be cut if the buffer is full */
    free(t.p);
    log_job_t *j = calloc(1, sizeof *j);
    if (!j) {
        free(buf);
        return 1;
    }
    j->next = next;
    j->buf = buf;
    sh_job_t job = { log_step, log_stop, NULL, j, 250 };
    sh_job(s, &job);
    return 0;
}

static int c_ip(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    aos_sys_info_t in;
    memset(&in, 0, sizeof in);
    aos_hal_sys_info(&in);
    aos_net_state_t st = aos_hal_net_state();
    static const char *const ST[] = { N_("apagado"), N_("conectando"), N_("conectado"), N_("falló") };
    sh_printf(s, "Wi-Fi: %s", _(ST[st <= AOS_NET_FAILED ? st : AOS_NET_FAILED]));
    if (st == AOS_NET_CONNECTED) {
        sh_printf(s, " · %s · %d dBm", aos_hal_net_ssid(), aos_hal_net_rssi());
        if (in.net_channel) sh_printf(s, _(" · canal %d"), in.net_channel);
        sh_printf(s, "\n  %s/%s  gw %s  dns %s\n", aos_hal_net_ip(), in.net_mask, in.net_gw, in.net_dns);
    } else sh_puts(s, "\n");
    if (in.net_mac[0]) sh_printf(s, "  MAC %s\n", in.net_mac);
    if (aos_hal_net_ap_active())
        sh_printf(s, _("Red propia: %s · %s · %d conectados\n"), aos_hal_net_ap_ssid(), aos_hal_net_ap_ip(),
                  aos_hal_net_ap_clients());
    return 0;
}

static int c_apps(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    aos_app_t *alive[16];
    int na = aos_ui_alive(alive, 16);
    const char *front = aos_ui_current_app();
    tb_t t = { 0 };
    for (int i = 0; i < aos_ui_app_count(); i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        if (!a || !a->desc.id || !a->create) continue;
        bool on = false;
        for (int k = 0; k < na; k++) on |= alive[k] && alive[k]->desc.id && !strcmp(alive[k]->desc.id, a->desc.id);
        bool fr = front && !strcmp(front, a->desc.id);
        tb_printf(&t, "%c %-24s %s\n", fr ? '>' : on ? '*' : ' ', a->desc.id, a->desc.name ? _(a->desc.name) : "");
    }
    tb_printf(&t, "%s", _("> al frente · * abierta\n"));
    emit(s, &t);
    return 0;
}

/* by id, or by name as the menu shows it (any case) */
static const aos_app_t *find_app(const char *q)
{
    for (int i = 0; i < aos_ui_app_count(); i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        if (a && a->desc.id && !strcmp(a->desc.id, q)) return a;
    }
    for (int i = 0; i < aos_ui_app_count(); i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        if (a && a->desc.name && (!strcasecmp(a->desc.name, q) || !strcasecmp(_(a->desc.name), q))) return a;
    }
    return NULL;
}

static int c_open(sh_t *s, int argc, char **argv)
{
    if (argc < 2) {
        sh_puts(s, _("open: ¿qué app? («apps» las lista)\n"));
        return 2;
    }
    char q[64] = "";
    for (int i = 1; i < argc; i++)
        snprintf(q + strlen(q), sizeof q - strlen(q), "%s%s", i > 1 ? " " : "", argv[i]);
    const aos_app_t *a = find_app(q);
    if (!a) {
        sh_printf(s, _("open: no encuentro «%s»\n"), q);
        return 1;
    }
    aos_ui_request_open(a->desc.id);
    aos_hal_log("ssh", "%s: open %s", sh_from(s), a->desc.id);
    return 0;
}

static int c_home(sh_t *s, int argc, char **argv)
{
    (void)s;
    (void)argc;
    aos_ui_request_nav(!strcmp(argv[0], "back") ? AOS_UI_NAV_BACK : AOS_UI_NAV_HOME);
    return 0;
}

static int c_who(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ssh_who_t w[4];
    int n = ssh_who(w, 4);
    uint32_t now = (uint32_t)(aos_hal_uptime_ms() / 1000);
    for (int i = 0; i < n; i++) {
        uint32_t m = (now - w[i].since_s) / 60;
        sh_printf(s, _("%-12s desde %-16s hace %u min (%s)\n"), w[i].user, w[i].from, (unsigned)m, _(w[i].how));
    }
    return 0;
}

static bool reboot_step(sh_t *s, void *st)
{
    (void)s;
    int *k = st;
    if (++*k >= 2) aos_hal_reboot();    /* the second turn: the goodbye has gone out */
    return false;
}

static void reboot_stop(sh_t *s, void *st)
{
    (void)s;
    free(st);
}

static int c_reboot(sh_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int *k = calloc(1, sizeof *k);
    if (!k) return 1;
    sh_puts(s, _("Reiniciando la placa...\n"));
    aos_hal_log("ssh", "%s: reboot", sh_from(s));
    sh_job_t job = { reboot_step, reboot_stop, NULL, k, 500 };
    sh_job(s, &job);
    return 0;
}

const sh_cmd_t SH_CMDS[] = {
    { "help",   "[comando]",          N_("esta lista"),                                    c_help },
    { "ls",     "[-la] [ruta...]",    N_("lo que hay en una carpeta (-l con detalle)"),   c_ls },
    { "cd",     "[carpeta]",          N_("cambia de carpeta (sola: la raíz de la tarjeta)"), c_cd },
    { "pwd",    "",                   N_("la carpeta en la que estás"),                    c_pwd },
    { "cat",    "archivo...",         N_("muestra archivos"),                              c_cat },
    { "head",   "[-n N] archivo",     N_("las primeras líneas"),                           c_head },
    { "tail",   "[-n N] archivo",     N_("las últimas líneas"),                            c_tail },
    { "cp",     "origen destino",     N_("copia un archivo"),                              c_cp },
    { "mv",     "origen destino",     N_("mueve o renombra"),                              c_mv },
    { "rm",     "[-rf] ruta...",      N_("borra (-r: carpetas con todo adentro)"),         c_rm },
    { "mkdir",  "[-p] carpeta...",    N_("crea carpetas"),                                 c_mkdir },
    { "rmdir",  "carpeta...",         N_("borra carpetas vacías"),                         c_rmdir },
    { "touch",  "archivo...",         N_("crea archivos vacíos"),                          c_touch },
    { "df",     "",                   N_("el espacio de la tarjeta y los pendrives"),      c_df },
    { "log",    "[-f] [-p] [-n N]",   N_("el registro (-f lo sigue, -p el arranque anterior)"), c_log },
    { "top",    "",                   N_("las tareas y cuánto usan, cada segundo"),        c_top },
    { "ps",     "",                   N_("las tareas, una vez"),                           c_ps },
    { "free",   "",                   N_("la memoria: interna, DMA y PSRAM"),              c_free },
    { "uptime", "",                   N_("cuánto hace que está prendida"),                 c_uptime },
    { "date",   "",                   N_("la fecha y la hora"),                            c_date },
    { "uname",  "",                   N_("versión, chip y último reinicio"),               c_uname },
    { "ip",     "",                   N_("el Wi-Fi y las direcciones"),                    c_ip },
    { "apps",   "",                   N_("las apps instaladas"),                           c_apps },
    { "open",   "app",                N_("abre una app en la pantalla"),                   c_open },
    { "home",   "",                   N_("vuelve al inicio en la pantalla"),               c_home },
    { "back",   "",                   N_("atrás, en la pantalla"),                         c_home },
    { "who",    "",                   N_("quiénes están conectados por SSH"),              c_who },
    { "reboot", "",                   N_("reinicia la placa"),                             c_reboot },
    { "clear",  "",                   N_("limpia la pantalla"),                            c_clear },
    { "echo",   "texto...",           N_("escribe lo que le pasen"),                       c_echo },
    { "exit",   "[estado]",           N_("cierra la sesión (también Ctrl-D)"),             c_exit },
};
const int SH_CMD_COUNT = sizeof SH_CMDS / sizeof SH_CMDS[0];
