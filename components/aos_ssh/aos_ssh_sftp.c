/*
 * P4OS - SSH server: the "sftp" subsystem (version 3 of the protocol, the
 * one OpenSSH speaks: draft-ietf-secsh-filexfer-02).
 *
 * `sftp p4os.local`, `scp`, a file manager's "connect to server" and
 * rsync-over-sftp tools reach the card's files through the same door and
 * the same keys as the console. It is also what the board's own SFTP client
 * (the backups' copy to a server, components/aos_sshc) is tried against, in
 * the simulator.
 *
 * This file knows nothing of SSH: bytes of the channel come in through
 * aos_sftpd_feed(), whole replies go out through the 'send' it was made
 * with. So it runs as it is in tests/sftp_host.
 *
 * What a client sees is one tree, rooted at the card: "/" is the card's
 * root and nothing above it can be named ("/.." is "/"). A session made
 * read-only refuses everything that writes.
 *
 * Memory: one session holds a packet being put together and a reply (36 KB
 * each, PSRAM) and up to HANDLES open files or folders. Nothing here is
 * internal RAM but the stack of whoever calls feed().
 */
#include "aos_ssh_sftp.h"
#include "aos_hal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#ifdef AOS_SIM
  #define BIG_ALLOC(n)  calloc(1, (n))
#else
  #include "esp_heap_caps.h"
  #define BIG_ALLOC(n)  heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

#define TAG         "sftp"
#define HANDLES     8
#define DATA_MAX    32768               /* one READ or WRITE: what every client keeps under */
#define PKT_MAX     (DATA_MAX + 1024)   /* a packet, whole */
#define PATH_MAX_V  400                 /* a path as the client names it */
#define NAMES_MAX   48                  /* entries in one READDIR reply */

enum {
    FXP_INIT = 1, FXP_VERSION, FXP_OPEN, FXP_CLOSE, FXP_READ, FXP_WRITE, FXP_LSTAT, FXP_FSTAT, FXP_SETSTAT, FXP_FSETSTAT,
    FXP_OPENDIR, FXP_READDIR, FXP_REMOVE, FXP_MKDIR, FXP_RMDIR, FXP_REALPATH, FXP_STAT, FXP_RENAME, FXP_READLINK, FXP_SYMLINK,
    FXP_STATUS = 101, FXP_HANDLE, FXP_DATA, FXP_NAME, FXP_ATTRS, FXP_EXTENDED = 200, FXP_EXTENDED_REPLY,
};
enum { FX_OK = 0, FX_EOF, FX_NO_SUCH_FILE, FX_PERMISSION_DENIED, FX_FAILURE, FX_BAD_MESSAGE, FX_NO_CONNECTION, FX_CONNECTION_LOST, FX_OP_UNSUPPORTED };
enum { ATTR_SIZE = 1, ATTR_UIDGID = 2, ATTR_PERMISSIONS = 4, ATTR_ACMODTIME = 8 };
enum { FXF_READ = 1, FXF_WRITE = 2, FXF_APPEND = 4, FXF_CREAT = 8, FXF_TRUNC = 16, FXF_EXCL = 32 };

typedef struct {
    FILE *f;
    DIR  *d;
    char *path;         /* the real one: a folder's, to stat what it lists; a file's, for FSETSTAT */
    bool  written;
} handle_t;

struct aos_sftpd {
    aos_sftpd_send_t send;
    void    *ctx;
    char     root[96];
    bool     read_only;
    uint8_t *in;        /* the packet being put together: its 4 bytes of length, then it */
    size_t   in_len;
    uint8_t *out;       /* the reply being written */
    size_t   out_len;
    bool     out_bad;
    handle_t h[HANDLES];
    uint32_t files_read, files_written;
};

/* ---- the reply ---- */

static void put_raw(aos_sftpd_t *s, const void *p, size_t n)
{
    if (s->out_len + n > PKT_MAX + 4) { s->out_bad = true; return; }
    memcpy(s->out + s->out_len, p, n);
    s->out_len += n;
}
static void put_u8(aos_sftpd_t *s, uint8_t v) { put_raw(s, &v, 1); }
static void put_u32(aos_sftpd_t *s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put_raw(s, b, 4);
}
static void put_u64(aos_sftpd_t *s, uint64_t v) { put_u32(s, (uint32_t)(v >> 32)); put_u32(s, (uint32_t)v); }
static void put_str(aos_sftpd_t *s, const char *t) { size_t n = strlen(t); put_u32(s, (uint32_t)n); put_raw(s, t, n); }

static void begin(aos_sftpd_t *s, uint8_t type, uint32_t id)
{
    s->out_len = 4;
    s->out_bad = false;
    put_u8(s, type);
    if (type != FXP_VERSION) put_u32(s, id);
}

static void end(aos_sftpd_t *s)
{
    if (s->out_bad) return;
    uint32_t n = (uint32_t)(s->out_len - 4);
    s->out[0] = (uint8_t)(n >> 24); s->out[1] = (uint8_t)(n >> 16); s->out[2] = (uint8_t)(n >> 8); s->out[3] = (uint8_t)n;
    s->send(s->ctx, s->out, s->out_len);
}

static void status(aos_sftpd_t *s, uint32_t id, uint32_t code)
{
    static const char *const TEXT[] = { "Success", "End of file", "No such file", "Permission denied", "Failure", "Bad message",
                                        "No connection", "Connection lost", "Operation unsupported" };
    begin(s, FXP_STATUS, id);
    put_u32(s, code);
    put_str(s, code < sizeof TEXT / sizeof TEXT[0] ? TEXT[code] : "Failure");
    put_str(s, "");
    end(s);
}

static uint32_t code_of_errno(void)
{
    return errno == ENOENT || errno == ENOTDIR ? FX_NO_SUCH_FILE : errno == EACCES || errno == EPERM || errno == EROFS ? FX_PERMISSION_DENIED : FX_FAILURE;
}

static void put_attrs(aos_sftpd_t *s, const struct stat *st)
{
    put_u32(s, ATTR_SIZE | ATTR_UIDGID | ATTR_PERMISSIONS | ATTR_ACMODTIME);
    put_u64(s, S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size);
    put_u32(s, 0); put_u32(s, 0);
    /* FAT has no owners nor modes: a folder is a folder and a file can be
     * written, which is what a client needs to know */
    put_u32(s, S_ISDIR(st->st_mode) ? 040755 : 0100644);
    put_u32(s, (uint32_t)st->st_mtime);
    put_u32(s, (uint32_t)st->st_mtime);
}

/* ---- the request ---- */

typedef struct { const uint8_t *p; size_t left; bool bad; } rd_t;

static uint32_t get_u32(rd_t *r)
{
    if (r->left < 4) { r->bad = true; r->left = 0; return 0; }
    uint32_t v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
    r->p += 4; r->left -= 4;
    return v;
}
static uint64_t get_u64(rd_t *r) { uint64_t hi = get_u32(r); return hi << 32 | get_u32(r); }
static const uint8_t *get_bytes(rd_t *r, uint32_t *len)
{
    uint32_t n = get_u32(r);
    if (r->bad || n > r->left) { r->bad = true; *len = 0; return NULL; }
    const uint8_t *p = r->p;
    r->p += n; r->left -= n;
    *len = n;
    return p;
}
/* A string as a C one, or false: too long, or with a NUL inside. */
static bool get_str(rd_t *r, char *out, size_t cap)
{
    uint32_t n;
    const uint8_t *p = get_bytes(r, &n);
    if (!p || n >= cap || memchr(p, 0, n)) { r->bad = true; return false; }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

/* What a client may set on a file: its size (to cut it) and its date. */
typedef struct { uint32_t flags; uint64_t size; uint32_t mtime; } attrs_t;
static void get_attrs(rd_t *r, attrs_t *a)
{
    memset(a, 0, sizeof *a);
    a->flags = get_u32(r);
    if (a->flags & ATTR_SIZE) a->size = get_u64(r);
    if (a->flags & ATTR_UIDGID) { get_u32(r); get_u32(r); }
    if (a->flags & ATTR_PERMISSIONS) get_u32(r);
    if (a->flags & ATTR_ACMODTIME) { get_u32(r); a->mtime = get_u32(r); }
    if (a->flags & 0x80000000u) {       /* extended pairs: read past them */
        uint32_t n = get_u32(r), l;
        for (uint32_t i = 0; i < n && !r->bad; i++) { get_bytes(r, &l); get_bytes(r, &l); }
    }
}

/* ---- paths ---- */

/* The path a client named, made whole: from the root, no "." nor "..", no
 * doubled slashes. "/.." is "/": nothing above the root has a name. */
static bool norm(const char *in, char out[PATH_MAX_V])
{
    size_t o = 0;
    out[o++] = '/';
    for (const char *p = in; *p; ) {
        while (*p == '/') p++;
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!n) break;
        if (n == 1 && p[0] == '.') { /* here */ }
        else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;
            if (o > 1) o--;
        } else {
            if (o + n + 2 >= PATH_MAX_V) return false;
            if (o > 1) out[o++] = '/';
            memcpy(out + o, p, n);
            o += n;
        }
        p += n;
    }
    out[o] = 0;
    return true;
}

/* The real path of what a client named, or NULL (to free()). */
static char *real(aos_sftpd_t *s, const char *named)
{
    char v[PATH_MAX_V];
    if (!norm(named, v)) return NULL;
    size_t cap = strlen(s->root) + strlen(v) + 2;
    char *out = BIG_ALLOC(cap);
    if (out) snprintf(out, cap, "%s%s", s->root, v[1] ? v : "");
    return out;
}

/* ---- handles ---- */

static int handle_new(aos_sftpd_t *s)
{
    for (int i = 0; i < HANDLES; i++)
        if (!s->h[i].f && !s->h[i].d) return i;
    return -1;
}

static void handle_close(aos_sftpd_t *s, int i)
{
    if (s->h[i].f) { fclose(s->h[i].f); if (s->h[i].written) s->files_written++; else s->files_read++; }
    if (s->h[i].d) closedir(s->h[i].d);
    free(s->h[i].path);
    memset(&s->h[i], 0, sizeof s->h[i]);
}

static int get_handle(aos_sftpd_t *s, rd_t *r)
{
    uint32_t n;
    const uint8_t *p = get_bytes(r, &n);
    if (!p || n != 4 || p[0] != 'h' || p[1] != 'd') return -1;
    int i = p[2] - '0';
    return i >= 0 && i < HANDLES && p[3] == 'x' && (s->h[i].f || s->h[i].d) ? i : -1;
}

static void reply_handle(aos_sftpd_t *s, uint32_t id, int i)
{
    char name[5] = { 'h', 'd', (char)('0' + i), 'x', 0 };
    begin(s, FXP_HANDLE, id);
    put_str(s, name);
    end(s);
}

/* A line as `ls -l` writes it: what sftp's own ls prints. */
static void longname(char *out, size_t cap, const char *name, const struct stat *st)
{
    char when[20] = "";
    time_t t = st->st_mtime;
    struct tm tm;
    if (localtime_r(&t, &tm)) strftime(when, sizeof when, "%b %e %Y", &tm);
    snprintf(out, cap, "%s 1 p4os p4os %10llu %s %.255s", S_ISDIR(st->st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
             S_ISDIR(st->st_mode) ? 0ull : (unsigned long long)st->st_size, when, name);
}

/* ---- one request ---- */

static void do_stat(aos_sftpd_t *s, uint32_t id, const char *path)
{
    struct stat st;
    if (!path) { status(s, id, FX_FAILURE); return; }
    if (stat(path, &st) != 0) { status(s, id, code_of_errno()); return; }
    begin(s, FXP_ATTRS, id);
    put_attrs(s, &st);
    end(s);
}

static uint32_t set_attrs(const char *path, const attrs_t *a)
{
    uint32_t code = FX_OK;
    if (a->flags & ATTR_SIZE && truncate(path, (off_t)a->size) != 0) code = code_of_errno();
    if (a->flags & ATTR_ACMODTIME) {
        struct utimbuf t = { (time_t)a->mtime, (time_t)a->mtime };
        if (utime(path, &t) != 0 && code == FX_OK) code = FX_OK;    /* a date that cannot be set is not a failed copy */
    }
    return code;
}

static void request(aos_sftpd_t *s, const uint8_t *pkt, size_t len)
{
    rd_t r = { pkt + 1, len - 1, false };
    uint8_t type = pkt[0];
    char a[PATH_MAX_V], b[PATH_MAX_V];
    char *pa = NULL, *pb = NULL;
    attrs_t at;

    if (type == FXP_INIT) {
        begin(s, FXP_VERSION, 0);
        put_u32(s, 3);
        put_str(s, "posix-rename@openssh.com"); put_str(s, "1");
        end(s);
        return;
    }
    uint32_t id = get_u32(&r);
    bool writes = type == FXP_WRITE || type == FXP_SETSTAT || type == FXP_FSETSTAT || type == FXP_REMOVE || type == FXP_MKDIR ||
                  type == FXP_RMDIR || type == FXP_RENAME || type == FXP_EXTENDED;
    if (writes && s->read_only) { status(s, id, FX_PERMISSION_DENIED); return; }

    switch (type) {
    case FXP_REALPATH: {
        struct stat st;
        if (!get_str(&r, a, sizeof a) || !norm(a, b)) { status(s, id, FX_BAD_MESSAGE); break; }
        pa = real(s, b);
        begin(s, FXP_NAME, id);
        put_u32(s, 1);
        put_str(s, b);
        put_str(s, b);
        if (pa && stat(pa, &st) == 0) put_attrs(s, &st);
        else put_u32(s, 0);
        end(s);
        break;
    }
    case FXP_STAT:
    case FXP_LSTAT:
        if (!get_str(&r, a, sizeof a)) { status(s, id, FX_BAD_MESSAGE); break; }
        pa = real(s, a);
        do_stat(s, id, pa);
        break;
    case FXP_FSTAT: {
        int i = get_handle(s, &r);
        if (i < 0) status(s, id, FX_FAILURE);
        else do_stat(s, id, s->h[i].path);
        break;
    }
    case FXP_OPENDIR: {
        int i = handle_new(s);
        if (!get_str(&r, a, sizeof a)) { status(s, id, FX_BAD_MESSAGE); break; }
        pa = real(s, a);
        DIR *d = pa && i >= 0 ? opendir(pa) : NULL;
        if (!d) { status(s, id, !pa || i < 0 ? FX_FAILURE : code_of_errno()); break; }
        s->h[i].d = d;
        s->h[i].path = pa;
        pa = NULL;
        reply_handle(s, id, i);
        break;
    }
    case FXP_READDIR: {
        int i = get_handle(s, &r);
        if (i < 0 || !s->h[i].d) { status(s, id, FX_FAILURE); break; }
        begin(s, FXP_NAME, id);
        size_t count_at = s->out_len;
        uint32_t n = 0;
        put_u32(s, 0);
        struct dirent *de;
        size_t cap = strlen(s->h[i].path) + 260;
        char *full = BIG_ALLOC(cap), *line = BIG_ALLOC(360);
        while (full && line && n < NAMES_MAX && s->out_len + 700 < PKT_MAX && (de = readdir(s->h[i].d)) != NULL) {
            struct stat st;
            if (strlen(de->d_name) > 255) continue;
            snprintf(full, cap, "%s/%s", s->h[i].path, de->d_name);
            if (stat(full, &st) != 0) memset(&st, 0, sizeof st);
            longname(line, 360, de->d_name, &st);
            put_str(s, de->d_name);
            put_str(s, line);
            put_attrs(s, &st);
            n++;
        }
        free(full); free(line);
        if (!n) { status(s, id, FX_EOF); break; }
        s->out[count_at] = (uint8_t)(n >> 24); s->out[count_at + 1] = (uint8_t)(n >> 16);
        s->out[count_at + 2] = (uint8_t)(n >> 8); s->out[count_at + 3] = (uint8_t)n;
        end(s);
        break;
    }
    case FXP_OPEN: {
        int i = handle_new(s);
        if (!get_str(&r, a, sizeof a)) { status(s, id, FX_BAD_MESSAGE); break; }
        uint32_t pf = get_u32(&r);
        get_attrs(&r, &at);
        if (r.bad) { status(s, id, FX_BAD_MESSAGE); break; }
        if ((pf & (FXF_WRITE | FXF_APPEND | FXF_CREAT | FXF_TRUNC)) && s->read_only) { status(s, id, FX_PERMISSION_DENIED); break; }
        pa = real(s, a);
        if (!pa || i < 0) { status(s, id, FX_FAILURE); break; }
        struct stat st;
        bool there = stat(pa, &st) == 0;
        if (there && S_ISDIR(st.st_mode)) { status(s, id, FX_FAILURE); break; }
        if (!there && !(pf & FXF_CREAT)) { status(s, id, FX_NO_SUCH_FILE); break; }
        if (there && (pf & FXF_CREAT) && (pf & FXF_EXCL)) { status(s, id, FX_FAILURE); break; }
        const char *mode = !(pf & FXF_WRITE) ? "rb" : (pf & FXF_TRUNC) || !there ? "w+b" : "r+b";
        FILE *f = fopen(pa, mode);
        if (!f) { status(s, id, code_of_errno()); break; }
        s->h[i].f = f;
        s->h[i].path = pa;
        s->h[i].written = (pf & FXF_WRITE) != 0;
        pa = NULL;
        reply_handle(s, id, i);
        break;
    }
    case FXP_READ: {
        int i = get_handle(s, &r);
        uint64_t off = get_u64(&r);
        uint32_t want = get_u32(&r);
        if (r.bad || i < 0 || !s->h[i].f) { status(s, id, FX_FAILURE); break; }
        if (want > DATA_MAX) want = DATA_MAX;
        begin(s, FXP_DATA, id);
        if (fseeko(s->h[i].f, (off_t)off, SEEK_SET) != 0) { status(s, id, FX_FAILURE); break; }
        size_t got = fread(s->out + s->out_len + 4, 1, want, s->h[i].f);
        if (!got) { status(s, id, ferror(s->h[i].f) ? FX_FAILURE : FX_EOF); break; }
        put_u32(s, (uint32_t)got);
        s->out_len += got;
        end(s);
        break;
    }
    case FXP_WRITE: {
        int i = get_handle(s, &r);
        uint64_t off = get_u64(&r);
        uint32_t n;
        const uint8_t *data = get_bytes(&r, &n);
        if (r.bad || i < 0 || !s->h[i].f || !data) { status(s, id, FX_FAILURE); break; }
        bool ok = fseeko(s->h[i].f, (off_t)off, SEEK_SET) == 0 && fwrite(data, 1, n, s->h[i].f) == n;
        status(s, id, ok ? FX_OK : FX_FAILURE);
        break;
    }
    case FXP_CLOSE: {
        int i = get_handle(s, &r);
        if (i < 0) { status(s, id, FX_FAILURE); break; }
        bool ok = !s->h[i].f || fflush(s->h[i].f) == 0;
        handle_close(s, i);
        status(s, id, ok ? FX_OK : FX_FAILURE);
        break;
    }
    case FXP_SETSTAT:
        if (!get_str(&r, a, sizeof a)) { status(s, id, FX_BAD_MESSAGE); break; }
        get_attrs(&r, &at);
        pa = real(s, a);
        status(s, id, r.bad ? FX_BAD_MESSAGE : !pa ? FX_FAILURE : set_attrs(pa, &at));
        break;
    case FXP_FSETSTAT: {
        int i = get_handle(s, &r);
        get_attrs(&r, &at);
        if (r.bad || i < 0 || !s->h[i].f) { status(s, id, FX_FAILURE); break; }
        fflush(s->h[i].f);
        status(s, id, set_attrs(s->h[i].path, &at));
        break;
    }
    case FXP_REMOVE:
    case FXP_RMDIR:
    case FXP_MKDIR:
        if (!get_str(&r, a, sizeof a) || !norm(a, b)) { status(s, id, FX_BAD_MESSAGE); break; }
        pa = real(s, a);
        if (!pa || !b[1]) { status(s, id, FX_PERMISSION_DENIED); break; }       /* never the root itself */
        status(s, id, (type == FXP_REMOVE ? remove(pa) : type == FXP_RMDIR ? rmdir(pa) : mkdir(pa, 0775)) == 0 ? FX_OK : code_of_errno());
        break;
    case FXP_RENAME:
    case FXP_EXTENDED: {
        bool posix = false;
        if (type == FXP_EXTENDED) {
            if (!get_str(&r, a, sizeof a) || strcmp(a, "posix-rename@openssh.com")) { status(s, id, FX_OP_UNSUPPORTED); break; }
            posix = true;
        }
        if (!get_str(&r, a, sizeof a) || !get_str(&r, b, sizeof b)) { status(s, id, FX_BAD_MESSAGE); break; }
        pa = real(s, a);
        pb = real(s, b);
        struct stat st;
        if (!pa || !pb) { status(s, id, FX_FAILURE); break; }
        /* the plain one refuses a name that is taken; the posix one takes
         * its place, which FAT's rename does not do by itself */
        if (stat(pb, &st) == 0) {
            if (!posix || S_ISDIR(st.st_mode)) { status(s, id, FX_FAILURE); break; }
            remove(pb);
        }
        status(s, id, rename(pa, pb) == 0 ? FX_OK : code_of_errno());
        break;
    }
    default:
        status(s, id, FX_OP_UNSUPPORTED);       /* links, and whatever comes later */
        break;
    }
    free(pa); free(pb);
}

/* ---- the door ---- */

aos_sftpd_t *aos_sftpd_new(const char *root, bool read_only, aos_sftpd_send_t send, void *ctx)
{
    aos_sftpd_t *s = BIG_ALLOC(sizeof *s);
    if (!s || !root || strlen(root) >= sizeof s->root) { free(s); return NULL; }
    s->in = BIG_ALLOC(PKT_MAX + 4);
    s->out = BIG_ALLOC(PKT_MAX + 4);
    if (!s->in || !s->out) { free(s->in); free(s->out); free(s); return NULL; }
    snprintf(s->root, sizeof s->root, "%s", root);
    s->read_only = read_only;
    s->send = send;
    s->ctx = ctx;
    return s;
}

size_t aos_sftpd_feed_one(aos_sftpd_t *s, const uint8_t *data, size_t len, bool *bad)
{
    size_t used = 0;
    *bad = false;
    while (used < len) {
        /* the length first, then that many bytes: a packet can come in
         * pieces, and several in one piece */
        size_t want = s->in_len < 4 ? 4 - s->in_len : 0;
        if (!want) {
            uint32_t n = (uint32_t)s->in[0] << 24 | (uint32_t)s->in[1] << 16 | (uint32_t)s->in[2] << 8 | s->in[3];
            if (n < 1 || n > PKT_MAX) { *bad = true; return used; }      /* not SFTP, or more than this takes */
            want = 4 + n - s->in_len;
        }
        size_t take = len - used < want ? len - used : want;
        memcpy(s->in + s->in_len, data + used, take);
        s->in_len += take;
        used += take;
        if (s->in_len >= 4) {
            uint32_t n = (uint32_t)s->in[0] << 24 | (uint32_t)s->in[1] << 16 | (uint32_t)s->in[2] << 8 | s->in[3];
            if (n < 1 || n > PKT_MAX) { *bad = true; return used; }
            if (s->in_len == 4 + n) {
                request(s, s->in + 4, n);
                s->in_len = 0;
                return used;            /* one request, one reply: the caller sends it before the next */
            }
        }
    }
    return used;
}

bool aos_sftpd_feed(aos_sftpd_t *s, const uint8_t *data, size_t len)
{
    while (len) {
        bool bad;
        size_t used = aos_sftpd_feed_one(s, data, len, &bad);
        if (bad) return false;
        data += used;
        len -= used;
    }
    return true;
}

void aos_sftpd_free(aos_sftpd_t *s)
{
    if (!s) return;
    for (int i = 0; i < HANDLES; i++)
        if (s->h[i].f || s->h[i].d) handle_close(s, i);
    if (s->files_read || s->files_written)
        aos_hal_log(TAG, "session over: %u files read, %u written", (unsigned)s->files_read, (unsigned)s->files_written);
    free(s->in);
    free(s->out);
    free(s);
}
