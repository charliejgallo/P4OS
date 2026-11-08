/*
 * P4OS - what runs on an SSH session channel.
 *
 * With a terminal (ssh p4os.local) it is a shell: a prompt, a line editor
 * that understands what terminals send (arrows, Home/End, Delete, Ctrl-A/E/
 * K/U/W/L, Ctrl-R is not there), a history and Tab to complete commands and
 * paths. Without one (ssh p4os.local log -n 20) it runs the one command and
 * ends with its exit status.
 *
 * Commands write into an output buffer the connection empties as the
 * client's window allows. A command that writes more than fits, or keeps
 * writing (cat of a big file, log -f, top), is a job: its step() runs again
 * whenever there is room, or every so many ms, until it is done or Ctrl-C
 * cuts it short.
 *
 * Paths: "/" is the card; /usb (and /usb2, /usb3) a pendrive while one is
 * mounted, as in the portal. ".." never leaves "/".
 */
#include "aos_ssh_int.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#else
static void *big_calloc(size_t n) { return calloc(1, n); }
#endif

#define OUT_CAP   (48 * 1024)
#define LINE_CAP  512
#define HIST_MAX  32
#define ARGS_MAX  32

enum { ST_NEW, ST_PROMPT, ST_JOB, ST_DONE };

struct sh {
    char user[32], from[48], term[32];
    bool pty, interactive, blocked;
    int cols, rows;
    int state, status;
    bool exit_req, eof;
    char *exec_cmd;
    char cwd[256];

    uint8_t *out;
    size_t out_off, out_len;
    bool last_cr;

    char line[LINE_CAP];
    int len, cur;
    int esc;                    /* 0 none, 1 after ESC, 2 in CSI, 3 after ESC O */
    char csi[8];
    int csin;
    bool after_cr;
    char *hist[HIST_MAX];
    int hist_n, hist_pos;
    char saved[LINE_CAP];       /* the line being typed while the history is browsed */

    sh_job_t job;
    bool job_on;
    uint32_t job_next;
};

/* ---- output ---- */

size_t sh_out_room(sh_t *s) { return OUT_CAP - (s->out_len - s->out_off); }

static void make_room(sh_t *s, size_t n)
{
    if (OUT_CAP - s->out_len >= n || !s->out_off) return;
    memmove(s->out, s->out + s->out_off, s->out_len - s->out_off);
    s->out_len -= s->out_off;
    s->out_off = 0;
}

static void put_raw(sh_t *s, const void *d, size_t n)
{
    make_room(s, n);
    if (n > OUT_CAP - s->out_len) n = OUT_CAP - s->out_len;     /* full: the rest is lost */
    memcpy(s->out + s->out_len, d, n);
    s->out_len += n;
}

/* On a terminal a newline is CR LF: the pty that would do it on Unix is not here */
void sh_write(sh_t *s, const void *d, size_t n)
{
    const uint8_t *p = d;
    if (!s->pty) {
        put_raw(s, p, n);
        return;
    }
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && p[j] != '\n') j++;
        if (j > i) {
            put_raw(s, p + i, j - i);
            s->last_cr = p[j - 1] == '\r';
        }
        if (j < n) {
            if (s->last_cr) put_raw(s, "\n", 1);
            else put_raw(s, "\r\n", 2);
            s->last_cr = false;
            j++;
        }
        i = j;
    }
}

void sh_puts(sh_t *s, const char *t) { sh_write(s, t, strlen(t)); }

void sh_printf(sh_t *s, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof buf) {
        sh_write(s, buf, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sh_write(s, big, (size_t)n);
    free(big);
}

void sh_color(sh_t *s, const char *sgr)
{
    if (!s->pty) return;
    char b[16];
    snprintf(b, sizeof b, "\x1b[%sm", sgr ? sgr : "0");
    put_raw(s, b, strlen(b));
}

size_t sh_out_peek(sh_t *s, const uint8_t **p)
{
    *p = s->out + s->out_off;
    return s->out_len - s->out_off;
}

void sh_out_take(sh_t *s, size_t n)
{
    s->out_off += n;
    if (s->out_off >= s->out_len) s->out_off = s->out_len = 0;
}

void sh_set_blocked(sh_t *s, bool b) { s->blocked = b; }
bool sh_is_pty(sh_t *s) { return s->pty; }
int sh_cols(sh_t *s) { return s->cols; }
int sh_rows(sh_t *s) { return s->rows; }
const char *sh_user(sh_t *s) { return s->user; }
const char *sh_from(sh_t *s) { return s->from; }
const char *sh_cwd(sh_t *s) { return s->cwd; }
void sh_set_cwd(sh_t *s, const char *v) { snprintf(s->cwd, sizeof s->cwd, "%s", v); }
int sh_exit_status(sh_t *s) { return s->status; }

void sh_request_exit(sh_t *s, int status)
{
    s->exit_req = true;
    s->status = status;
}

/* ---- paths ---- */

/* the pendrive behind "usb", "usb2"...: its mount point, while it is there */
static bool usb_root(const char *seg, size_t l, char *root, size_t rn)
{
    if (l < 3 || l > 4 || strncmp(seg, "usb", 3) || (l == 4 && (seg[3] < '2' || seg[3] > '9'))) return false;
    if (!aos_hal_usb_host_on()) return false;
    aos_usb_host_info_t in[AOS_USB_HOST_MAX];
    int n = aos_hal_usb_host_devices(in, AOS_USB_HOST_MAX);
    for (int i = 0; i < n; i++)
        if (in[i].mounted && strlen(in[i].path) == l + 1 && !strncmp(in[i].path + 1, seg, l) && rn > l + 1) {
            memcpy(root, in[i].path, l + 1);        /* "/usb2": its length is known */
            root[l + 1] = 0;
            return true;
        }
    return false;
}

bool sh_path(sh_t *s, const char *in, char *virt, size_t vn, char *real, size_t rn)
{
    char tmp[512];
    if (in[0] == '/') snprintf(tmp, sizeof tmp, "%s", in);
    else snprintf(tmp, sizeof tmp, "%s/%s", s->cwd, in);
    /* the segments, "." and ".." resolved; "/" is as far up as it goes */
    char out[512];
    size_t ol = 0;
    out[0] = 0;
    for (char *p = tmp; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        if (l == 1 && p[0] == '.') {
        } else if (l == 2 && p[0] == '.' && p[1] == '.') {
            while (ol && out[ol - 1] != '/') ol--;
            if (ol) ol--;
            out[ol] = 0;
        } else {
            if (ol + l + 2 >= sizeof out) return false;
            out[ol++] = '/';
            memcpy(out + ol, p, l);
            ol += l;
            out[ol] = 0;
        }
        p = e;
    }
    if (!ol) snprintf(out, sizeof out, "/");
    if (virt) snprintf(virt, vn, "%s", out);
    if (!real) return true;
    const char *seg = out + 1, *e = strchr(seg, '/');
    size_t sl = e ? (size_t)(e - seg) : strlen(seg);
    char root[24];
    if (usb_root(seg, sl, root, sizeof root)) {
        int k = snprintf(real, rn, "%s%s", root, seg + sl);
        return k > 0 && (size_t)k < rn;
    }
    const char *card = aos_hal_path_sd_root();
    if (!card) return false;
    int k = snprintf(real, rn, "%s%s", card, ol ? out : "");
    if (!strcmp(out, "/")) k = snprintf(real, rn, "%s", card);
    return k > 0 && (size_t)k < rn;
}

/* ---- the prompt and the line ---- */

/* columns a piece of UTF-8 takes: one per character (no wide ones here) */
static int width(const char *p, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        if (((uint8_t)p[i] & 0xC0) != 0x80) w++;
    return w;
}

static void prompt(sh_t *s)
{
    sh_color(s, "1;32");
    sh_puts(s, aos_hal_device_name());
    sh_color(s, NULL);
    sh_puts(s, ":");
    sh_color(s, "1;34");
    sh_puts(s, s->cwd);
    sh_color(s, NULL);
    sh_puts(s, "$ ");
}

static void redraw(sh_t *s)
{
    sh_puts(s, "\r");
    prompt(s);
    put_raw(s, s->line, (size_t)s->len);
    put_raw(s, "\x1b[K", 3);
    int back = width(s->line + s->cur, s->len - s->cur);
    if (back) {
        char b[16];
        snprintf(b, sizeof b, "\x1b[%dD", back);
        put_raw(s, b, strlen(b));
    }
}

static void set_line(sh_t *s, const char *t)
{
    snprintf(s->line, sizeof s->line, "%s", t);
    s->len = s->cur = (int)strlen(s->line);
    redraw(s);
}

static void insert(sh_t *s, const char *d, int n)
{
    if (s->len + n >= LINE_CAP) return;
    memmove(s->line + s->cur + n, s->line + s->cur, (size_t)(s->len - s->cur));
    memcpy(s->line + s->cur, d, (size_t)n);
    s->len += n;
    s->cur += n;
    s->line[s->len] = 0;
    if (s->cur == s->len) put_raw(s, d, (size_t)n);  /* at the end: just echo it */
    else redraw(s);
}

static int prev_char(sh_t *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((uint8_t)s->line[i] & 0xC0) == 0x80) i--;
    return i;
}

static int next_char(sh_t *s, int i)
{
    if (i >= s->len) return s->len;
    i++;
    while (i < s->len && ((uint8_t)s->line[i] & 0xC0) == 0x80) i++;
    return i;
}

static void cut(sh_t *s, int from, int to)
{
    if (to <= from) return;
    memmove(s->line + from, s->line + to, (size_t)(s->len - to));
    s->len -= to - from;
    s->line[s->len] = 0;
    if (s->cur > to) s->cur -= to - from;
    else if (s->cur > from) s->cur = from;
    redraw(s);
}

static void hist_add(sh_t *s, const char *l)
{
    if (!*l || (s->hist_n && !strcmp(s->hist[s->hist_n - 1], l))) return;
    if (s->hist_n == HIST_MAX) {
        free(s->hist[0]);
        memmove(s->hist, s->hist + 1, (HIST_MAX - 1) * sizeof s->hist[0]);
        s->hist_n--;
    }
    char *c = strdup(l);
    if (c) s->hist[s->hist_n++] = c;
}

static void hist_move(sh_t *s, int d)
{
    int p = s->hist_pos + d;
    if (p < 0 || p > s->hist_n) return;
    if (s->hist_pos == s->hist_n) snprintf(s->saved, sizeof s->saved, "%s", s->line);
    s->hist_pos = p;
    set_line(s, p == s->hist_n ? s->saved : s->hist[p]);
}

/* ---- Tab ---- */

typedef struct {
    char (*name)[64];
    bool *dir;
    int n, cap;
} cands_t;

static void cand_add(cands_t *c, const char *name, bool dir)
{
    if (c->n >= c->cap || strlen(name) >= 64) return;
    snprintf(c->name[c->n], 64, "%s", name);
    c->dir[c->n++] = dir;
}

static int cand_cmp(const void *a, const void *b) { return strcmp(a, b); }

static void complete(sh_t *s)
{
    int ws = s->cur;
    while (ws > 0 && s->line[ws - 1] != ' ') ws--;
    bool first = true;
    for (int i = 0; i < ws; i++)
        if (s->line[i] != ' ') first = false;
    char word[256];
    int wl = s->cur - ws;
    if (wl >= (int)sizeof word) return;
    memcpy(word, s->line + ws, (size_t)wl);
    word[wl] = 0;

    cands_t c = { malloc(200 * 64), malloc(200), 0, 200 };
    if (!c.name || !c.dir) {
        free(c.name);
        free(c.dir);
        return;
    }
    const char *base = word;
    if (first) {
        for (int i = 0; i < SH_CMD_COUNT; i++)
            if (!strncmp(SH_CMDS[i].name, word, (size_t)wl)) cand_add(&c, SH_CMDS[i].name, false);
    } else {
        char dir[256], real[320];
        char *slash = strrchr(word, '/');
        if (slash) {
            snprintf(dir, sizeof dir, "%.*s", (int)(slash - word + 1), word);
            base = slash + 1;
        } else snprintf(dir, sizeof dir, ".");
        size_t bl = strlen(base);
        char virt[256];
        if (sh_path(s, dir, virt, sizeof virt, real, sizeof real)) {
            DIR *d = opendir(real);
            struct dirent *de;
            while (d && (de = readdir(d))) {
                if (de->d_name[0] == '.' && base[0] != '.') continue;
                if (strncmp(de->d_name, base, bl)) continue;
                char full[600];
                struct stat st;
                snprintf(full, sizeof full, "%s/%s", real, de->d_name);
                cand_add(&c, de->d_name, !stat(full, &st) && S_ISDIR(st.st_mode));
            }
            if (d) closedir(d);
            if (!strcmp(virt, "/"))         /* the pendrives, at the card's root */
                for (int i = 0; i < 3; i++) {
                    const char *u = i == 0 ? "usb" : i == 1 ? "usb2" : "usb3";
                    char r[24];
                    if (!strncmp(u, base, bl) && usb_root(u, strlen(u), r, sizeof r)) cand_add(&c, u, true);
                }
        }
    }
    size_t bl = strlen(base);
    if (c.n == 1) {
        char add[80];
        snprintf(add, sizeof add, "%s%s", c.name[0] + bl, c.dir[0] ? "/" : " ");
        insert(s, add, (int)strlen(add));
    } else if (c.n > 1) {
        size_t common = strlen(c.name[0]);
        for (int i = 1; i < c.n; i++) {
            size_t k = 0;
            while (k < common && c.name[i][k] == c.name[0][k]) k++;
            common = k;
        }
        if (common > bl) insert(s, c.name[0] + bl, (int)(common - bl));
        else {
            qsort(c.name, (size_t)c.n, 64, cand_cmp);
            sh_puts(s, "\n");
            int colw = 2;
            for (int i = 0; i < c.n; i++) { int w = (int)strlen(c.name[i]) + 2; if (w > colw) colw = w; }
            int per = s->cols / colw > 0 ? s->cols / colw : 1;
            for (int i = 0; i < c.n; i++) sh_printf(s, "%-*s%s", colw, c.name[i], (i + 1) % per && i + 1 < c.n ? "" : "\n");
            redraw(s);
        }
    }
    free(c.name);
    free(c.dir);
}

/* ---- running a line ---- */

/* words, with "..." and '...' and backslash; argv points into buf */
static int split_args(char *buf, char **argv, int max)
{
    int n = 0;
    char *r = buf, *w = buf;
    while (*r) {
        while (*r == ' ' || *r == '\t') r++;
        if (!*r) break;
        if (n == max) break;
        argv[n++] = w;
        char q = 0;
        while (*r && (q || (*r != ' ' && *r != '\t'))) {
            if (q && *r == q) q = 0;
            else if (!q && (*r == '"' || *r == '\'')) q = *r;
            else if (*r == '\\' && r[1] && q != '\'') *w++ = *++r;
            else *w++ = *r;
            r++;
        }
        if (*r) r++;
        *w++ = 0;
    }
    return n;
}

static void after_command(sh_t *s)
{
    if (s->interactive && !s->exit_req) {
        s->state = ST_PROMPT;
        s->len = s->cur = 0;
        s->line[0] = 0;
        s->hist_pos = s->hist_n;
        prompt(s);
    } else s->state = ST_DONE;
}

static void run_line(sh_t *s, const char *text)
{
    char *buf = strdup(text);
    char *argv[ARGS_MAX + 1];
    int argc = buf ? split_args(buf, argv, ARGS_MAX) : 0;
    argv[argc] = NULL;
    if (argc) {
        const sh_cmd_t *cmd = NULL;
        for (int i = 0; i < SH_CMD_COUNT && !cmd; i++)
            if (!strcmp(SH_CMDS[i].name, argv[0])) cmd = &SH_CMDS[i];
        if (cmd) s->status = cmd->run(s, argc, argv);
        else {
            sh_printf(s, _("%s: no existe ese comando; «help» muestra los que hay.\n"), argv[0]);
            s->status = 127;
        }
    }
    free(buf);
    if (s->job_on) {
        s->state = ST_JOB;
        s->job_next = 0;
    } else after_command(s);
}

void sh_job(sh_t *s, const sh_job_t *j)
{
    s->job = *j;
    s->job_on = true;
}

static void job_end(sh_t *s)
{
    if (!s->job_on) return;
    s->job_on = false;
    if (s->job.stop) s->job.stop(s, s->job.st);
    after_command(s);
}

/* ---- input ---- */

static void key_csi(sh_t *s, char final)
{
    int n = s->csin ? atoi(s->csi) : 0;
    switch (final) {
    case 'A': hist_move(s, -1); break;
    case 'B': hist_move(s, +1); break;
    case 'C': if (s->cur < s->len) { s->cur = next_char(s, s->cur); redraw(s); } break;
    case 'D': if (s->cur > 0) { s->cur = prev_char(s, s->cur); redraw(s); } break;
    case 'H': s->cur = 0; redraw(s); break;
    case 'F': s->cur = s->len; redraw(s); break;
    case '~':
        if (n == 1 || n == 7) { s->cur = 0; redraw(s); }
        else if (n == 4 || n == 8) { s->cur = s->len; redraw(s); }
        else if (n == 3) cut(s, s->cur, next_char(s, s->cur));
        break;
    default: break;
    }
}

static void key(sh_t *s, uint8_t c)
{
    if (s->esc == 1) {
        s->esc = c == '[' ? 2 : c == 'O' ? 3 : 0;
        s->csin = 0;
        return;
    }
    if (s->esc == 2) {
        if ((c >= '0' && c <= '9') || c == ';') {
            if (s->csin < (int)sizeof s->csi - 1) s->csi[s->csin++] = (char)c;
            s->csi[s->csin] = 0;
            return;
        }
        s->esc = 0;
        key_csi(s, (char)c);
        return;
    }
    if (s->esc == 3) {
        s->esc = 0;
        s->csin = 0;
        key_csi(s, (char)c);
        return;
    }
    if (c == '\n' && s->after_cr) {     /* CR LF from a client that sends both */
        s->after_cr = false;
        return;
    }
    s->after_cr = c == '\r';
    switch (c) {
    case 0x1b: s->esc = 1; return;
    case '\r':
    case '\n': {
        sh_puts(s, "\n");
        char *l = strdup(s->line);
        hist_add(s, s->line);
        s->len = s->cur = 0;
        s->line[0] = 0;
        if (l) run_line(s, l);
        free(l);
        return;
    }
    case 0x03:                          /* Ctrl-C */
        sh_puts(s, "^C\n");
        s->len = s->cur = 0;
        s->line[0] = 0;
        s->hist_pos = s->hist_n;
        prompt(s);
        return;
    case 0x04:                          /* Ctrl-D */
        if (!s->len) {
            sh_puts(s, "\n");
            sh_request_exit(s, 0);
            s->state = ST_DONE;
        } else cut(s, s->cur, next_char(s, s->cur));
        return;
    case 0x7f:
    case 0x08: cut(s, prev_char(s, s->cur), s->cur); return;
    case 0x01: s->cur = 0; redraw(s); return;
    case 0x05: s->cur = s->len; redraw(s); return;
    case 0x02: s->cur = prev_char(s, s->cur); redraw(s); return;
    case 0x06: s->cur = next_char(s, s->cur); redraw(s); return;
    case 0x0b: cut(s, s->cur, s->len); return;
    case 0x15: cut(s, 0, s->cur); return;
    case 0x17: {
        int i = s->cur;
        while (i > 0 && s->line[i - 1] == ' ') i--;
        while (i > 0 && s->line[i - 1] != ' ') i--;
        cut(s, i, s->cur);
        return;
    }
    case 0x0c: put_raw(s, "\x1b[H\x1b[2J", 7); redraw(s); return;
    case 0x10: hist_move(s, -1); return;
    case 0x0e: hist_move(s, +1); return;
    case 0x09: complete(s); return;
    default:
        if (c >= 0x20) insert(s, (const char *)&c, 1);
        return;
    }
}

void sh_input(sh_t *s, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        if (s->state == ST_JOB) {
            bool end = s->job.key ? s->job.key(s, s->job.st, c) : false;
            if (c == 0x03 && !end) {
                sh_puts(s, "^C\n");
                end = true;
                s->status = 130;
            }
            if (end) job_end(s);
            continue;           /* what is typed while a job runs is not kept */
        }
        if (s->state == ST_PROMPT) key(s, c);
    }
}

void sh_eof(sh_t *s)
{
    s->eof = true;
    if (s->state == ST_PROMPT) {
        sh_request_exit(s, 0);
        s->state = ST_DONE;
    }
}

void sh_resize(sh_t *s, int cols, int rows)
{
    s->cols = cols;
    s->rows = rows;
}

/* ---- life ---- */

sh_t *sh_new(const sh_opts_t *o)
{
    sh_t *s = big_calloc(sizeof *s);
    if (!s) return NULL;
    s->out = big_calloc(OUT_CAP);
    if (!s->out) {
        free(s);
        return NULL;
    }
    snprintf(s->user, sizeof s->user, "%s", o->user);
    snprintf(s->from, sizeof s->from, "%s", o->from);
    snprintf(s->term, sizeof s->term, "%s", o->term ? o->term : "");
    s->pty = o->pty;
    s->cols = o->cols;
    s->rows = o->rows;
    snprintf(s->cwd, sizeof s->cwd, "/");
    return s;
}

void sh_free(sh_t *s)
{
    if (!s) return;
    if (s->job_on && s->job.stop) s->job.stop(s, s->job.st);
    for (int i = 0; i < s->hist_n; i++) free(s->hist[i]);
    free(s->exec_cmd);
    free(s->out);
    free(s);
}

void sh_start_shell(sh_t *s)
{
    s->interactive = true;
}

void sh_start_exec(sh_t *s, const char *cmd)
{
    s->interactive = false;
    s->exec_cmd = strdup(cmd);
}

int sh_poll(sh_t *s)
{
    if (s->state == ST_NEW) {
        if (s->interactive) {
            sh_color(s, "1");
            sh_printf(s, "P4OS %s", aos_hal_firmware_version());
            sh_color(s, NULL);
            sh_printf(s, " · %s\n", aos_hal_device_name());
            sh_puts(s, _("«help» muestra los comandos; Ctrl-D o «exit» para salir.\n"));
            after_command(s);
        } else {
            char *cmd = s->exec_cmd;
            s->exec_cmd = NULL;
            run_line(s, cmd ? cmd : "");
            free(cmd);
        }
    }
    if (s->state == ST_JOB && s->job_on && !s->blocked) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        bool due = !s->job.every_ms || (int32_t)(now - s->job_next) >= 0;
        if (due && sh_out_room(s) >= OUT_CAP / 2) {
            if (s->job.every_ms) s->job_next = now + s->job.every_ms;
            if (s->job.step(s, s->job.st)) job_end(s);
        }
    }
    if (s->exit_req && s->state != ST_JOB) s->state = ST_DONE;
    if (s->state == ST_DONE) return SH_DONE;
    return s->state == ST_JOB && !s->job.every_ms ? SH_BUSY : SH_IDLE;
}
