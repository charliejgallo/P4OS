/*
 * P4OS - A file from the network onto the card. See aos_fetch.h.
 *
 * How it goes, and why:
 *
 *  - HTTP/1.1 and "Connection: close", as aos_http.c since 2026-11-22: there
 *    are servers that refuse HTTP/1.0 outright (adsb.lol answers 426), and a
 *    source of the Tienda may be anybody's. The body ends at its
 *    Content-Length, at its last chunk (undone as it arrives) or where the
 *    connection does.
 *  - Redirects are followed across hosts. GitHub's release downloads are two:
 *    /releases/latest/download/x -> /releases/download/<tag>/x -> a signed
 *    address on another host (seen 2026-11-20). That last address expires,
 *    so a try after a cut starts again from the address it was given, not
 *    from where the last redirect led. Never from https down to http.
 *  - What arrives goes to <path>.part in whole blocks from a buffer the card
 *    takes directly (aos_hal_io_alloc). A try after a cut asks for the rest
 *    with Range; the bytes on the card are never thrown away unless the
 *    server says the file is another one.
 *  - The .part is synced every megabyte, so what a restart or a power cut
 *    leaves on the card is all but the last megabyte, not an empty file.
 *  - The hash is taken at the end, reading the file back: one more pass over
 *    the card (~14 MB/s on the board), and what is checked is what the card
 *    holds, not what went through memory on its way there.
 *
 * Memory: everything is in PSRAM - the job, the address, the headers, the
 * TLS context and the block - and so is the thread's stack: nothing here
 * touches the flash (the clock's preference is read inside aos_tls_open,
 * through aos_flash_call).
 */
#include "aos_fetch.h"
#include "aos_hal.h"
#include "aos_tls.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef AOS_SIM
  #define BIG_ALLOC(n)  calloc(1, (n))
  #define LOGI(...)     do { printf("[fetch] " __VA_ARGS__); printf("\n"); } while (0)
  #define LOGW(...)     LOGI(__VA_ARGS__)
#else
  #include "esp_heap_caps.h"
  #include "esp_log.h"
  #define BIG_ALLOC(n)  heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
  #define LOGI(...)     ESP_LOGI("fetch", __VA_ARGS__)
  #define LOGW(...)     ESP_LOGW("fetch", __VA_ARGS__)
#endif

#define SLOTS           2
#define URL_MAX         4096    /* a signed address is long: GitHub's are ~1 KB */
#define HDR_MAX         16384   /* and so are its headers: one policy line is ~3 KB */
#define BLOCK           (64 * 1024)
#define MAX_REDIRECTS   6
#define MAX_IDLE_TRIES  5       /* in a row without a byte more */
#define CONNECT_MS      10000
#define RECV_MS         1000    /* so a cancel is noticed */
#define STALL_MS        20000   /* connected and silent this long: cut it and try again */
#define HEADERS_MS      15000
/* the handshake's deepest point is ~4.4 KB (aos_tls.c); the rest is ours */
#define STACK           8192
/* The card learns how long the .part is only when the file is closed or
 * synced: a board restarted 15 MB into a download came back to a .part of
 * 0 bytes (measured, 2026-11-20). So it is synced every this many blocks,
 * and a restart or a power cut costs at most the megabyte since. */
#define SYNC_BLOCKS     16

typedef struct {
    int      id;                /* 0 = free */
    bool     running;           /* its thread is alive */
    bool     cancel;
    bool     released;          /* nobody will ask again: the thread frees the slot */
    aos_fetch_info_t info;
    char    *url;               /* the address given, URL_MAX */
    char     path[224];
    char     part[232];
    bool     has_hash;
    uint8_t  want[32];
    uint32_t want_size;
    uint64_t t0;
} job_t;

AOS_BSS_PSRAM static job_t s_jobs[SLOTS];
static void  *s_lock;
static int    s_next_id = 1;

static void lock(void)   { aos_hal_mutex_lock(s_lock); }
static void unlock(void) { aos_hal_mutex_unlock(s_lock); }

static job_t *find(int id)      /* with the lock held */
{
    if (id <= 0) return NULL;
    for (int i = 0; i < SLOTS; i++)
        if (s_jobs[i].id == id && !s_jobs[i].released) return &s_jobs[i];
    return NULL;
}

static void job_free(job_t *j)  /* with the lock held */
{
    free(j->url);
    memset(j, 0, sizeof *j);
}

/* ---- addresses ------------------------------------------------------------- */

typedef struct {
    bool        tls;
    int         port;
    char        host[96];
    const char *path;           /* into the address: "/..." or "" for "/" */
} url_t;

static bool url_parse(const char *url, url_t *u)
{
    memset(u, 0, sizeof *u);
    const char *h;
    if (!strncasecmp(url, "http://", 7))        { h = url + 7; u->port = 80; }
    else if (!strncasecmp(url, "https://", 8))  { h = url + 8; u->port = 443; u->tls = true; }
    else return false;
    const char *end = h + strcspn(h, "/?#");
    if (memchr(h, '@', (size_t)(end - h))) return false;    /* user:pass@ - not here */
    const char *colon = memchr(h, ':', (size_t)(end - h));
    const char *hend = colon ? colon : end;
    if (hend == h || (size_t)(hend - h) >= sizeof u->host) return false;
    memcpy(u->host, h, (size_t)(hend - h));
    if (colon) {
        u->port = atoi(colon + 1);
        if (u->port <= 0 || u->port > 65535) return false;
    }
    u->path = end;
    return true;
}

/* 'loc' as an address of its own, given where it was found. In place over
 * 'url' (URL_MAX), with 'tmp' (URL_MAX) to build it. */
static bool url_follow(char *url, const char *loc, char *tmp)
{
    url_t b;
    if (!url_parse(url, &b)) return false;
    int n;
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) {
        n = snprintf(tmp, URL_MAX, "%s", loc);
    } else {
        const char *scheme = b.tls ? "https" : "http";
        char hostport[112];
        if (b.port == (b.tls ? 443 : 80)) snprintf(hostport, sizeof hostport, "%s", b.host);
        else snprintf(hostport, sizeof hostport, "%s:%d", b.host, b.port);
        if (loc[0] == '/' && loc[1] == '/') n = snprintf(tmp, URL_MAX, "%s:%s", scheme, loc);
        else if (loc[0] == '/') n = snprintf(tmp, URL_MAX, "%s://%s%s", scheme, hostport, loc);
        else {
            /* beside the page it came from */
            size_t dir = strcspn(b.path, "?#");
            while (dir > 0 && b.path[dir - 1] != '/') dir--;
            n = snprintf(tmp, URL_MAX, "%s://%s%.*s%s%s", scheme, hostport, (int)dir, b.path,
                         dir ? "" : "/", loc);
        }
    }
    if (n <= 0 || n >= URL_MAX) return false;
    memcpy(url, tmp, (size_t)n + 1);
    return true;
}

/* The value of header 'name' in the block, or NULL. */
static const char *hdr_get(const char *hdrs, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *line = hdrs; line && *line;) {
        if (!strncasecmp(line, name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t n = strcspn(v, "\r\n");
            if (n >= cap) return NULL;      /* cut short it would be another address */
            memcpy(out, v, n);
            out[n] = 0;
            return out;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

/* ---- a body in chunks ------------------------------------------------------ */

typedef struct {
    bool     on, done;
    int      st;        /* 0 the size, 1 the rest of its line, 2 data, 3 the line end after it */
    uint32_t left;
} chunk_t;

/* Undoes them in place as they arrive: returns how many of the n bytes are
 * body, moved to the front. */
static int dechunk(chunk_t *k, uint8_t *p, int n)
{
    int w = 0;
    for (int r = 0; r < n && !k->done;) {
        switch (k->st) {
        case 0: {
            int ch = p[r], v = isdigit(ch) ? ch - '0' : isxdigit(ch) ? tolower(ch) - 'a' + 10 : -1;
            if (v >= 0) { k->left = k->left * 16 + (uint32_t)v; r++; }
            else k->st = 1;
            break;
        }
        case 1:
            if (p[r++] == '\n') {
                if (k->left == 0) k->done = true;
                else k->st = 2;
            }
            break;
        case 2: {
            int take = n - r;
            if ((uint32_t)take > k->left) take = (int)k->left;
            memmove(p + w, p + r, (size_t)take);
            w += take;
            r += take;
            k->left -= (uint32_t)take;
            if (!k->left) k->st = 3;
            break;
        }
        default:
            if (p[r++] == '\n') { k->st = 0; k->left = 0; }
            break;
        }
    }
    return w;
}

/* ---- one try --------------------------------------------------------------- */

enum { T_DONE, T_AGAIN, T_FATAL, T_CANCEL };

typedef struct {
    job_t     *j;
    char      *url, *tmp;       /* URL_MAX each */
    char      *hdr;             /* HDR_MAX + 1 */
    uint8_t   *buf;             /* BLOCK, the card's */
    aos_tls_t *c;
    uint32_t   have;            /* bytes in the .part */
} ctx_t;

static void say(job_t *j, int status, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(job_t *j, int status, const char *fmt, ...)
{
    char why[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    lock();
    j->info.status = status;
    snprintf(j->info.why, sizeof j->info.why, "%s", why);
    unlock();
}

static uint32_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
}

/* The folders above the file, made as they are missed: a package brings
 * folders the card may not have yet (apps/<app>/, web/, lang/<code>/). */
static void make_parents(const char *path)
{
    char dir[sizeof(((job_t *)0)->path)];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);       /* there already, most times */
            *p = '/';
        }
    }
}

static bool write_all(int fd, const uint8_t *p, int n)
{
    while (n > 0) {
        int w = (int)write(fd, p, (size_t)n);
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

static int try_once(ctx_t *x)
{
    job_t *j = x->j;
    snprintf(x->url, URL_MAX, "%s", j->url);
    bool was_tls = false;
    url_t u;
    int status = 0, hlen = 0, body_at = -1;

    for (int hop = 0;; hop++) {
        if (hop > MAX_REDIRECTS) {
            say(j, AOS_FETCH_ERR_REDIRECT, "more than %d redirects", MAX_REDIRECTS);
            return T_FATAL;
        }
        if (!url_parse(x->url, &u)) {
            say(j, AOS_FETCH_ERR_REDIRECT, "not an http(s) address");
            return T_FATAL;
        }
        if (was_tls && !u.tls) {
            say(j, AOS_FETCH_ERR_REDIRECT, "a redirect from https to http: refused");
            return T_FATAL;
        }
        was_tls = u.tls;
        lock();
        snprintf(j->info.host, sizeof j->info.host, "%s", u.host);
        j->info.redirects = (uint16_t)hop;
        j->info.connects++;
        unlock();

        char why[96] = "";
        uint64_t t = aos_hal_uptime_ms();
        int rc = aos_tls_open(x->c, u.host, u.port, u.tls ? AOS_TLS_ENCRYPT : 0, CONNECT_MS, why, sizeof why);
        lock();
        j->info.connect_ms += (uint32_t)(aos_hal_uptime_ms() - t);
        unlock();
        if (rc != 0) {
            say(j, rc, "%s: %s", u.host, why[0] ? why : "could not connect");
            /* a certificate that does not verify will not in ten seconds
             * either; a clock not set yet, a name, a connection may */
            return rc == AOS_HTTP_ERR_TLS ? T_FATAL : T_AGAIN;
        }

        char range[48] = "";
        if (x->have) snprintf(range, sizeof range, "Range: bytes=%u-\r\n", (unsigned)x->have);
        char hostport[112];
        if (u.port == (u.tls ? 443 : 80)) snprintf(hostport, sizeof hostport, "%s", u.host);
        else snprintf(hostport, sizeof hostport, "%s:%d", u.host, u.port);
        /* nothing that names the board: the system and its version */
        int n = snprintf(x->hdr, HDR_MAX,
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: P4OS/%s\r\n"
                         "Accept: */*\r\n"
                         "%s"
                         "Connection: close\r\n\r\n",
                         u.path[0] == '/' ? u.path : "/", hostport, aos_hal_firmware_version(), range);
        if (n <= 0 || n >= HDR_MAX || !aos_tls_send(x->c, x->hdr, n, CONNECT_MS)) {
            aos_tls_close(x->c);
            say(j, AOS_HTTP_ERR_SEND, "%s: could not send the request", u.host);
            return T_AGAIN;
        }

        hlen = 0;
        body_at = -1;
        t = aos_hal_uptime_ms();
        while (body_at < 0 && hlen < HDR_MAX) {
            if (j->cancel) { aos_tls_close(x->c); return T_CANCEL; }
            int got = aos_tls_recv(x->c, x->hdr + hlen, HDR_MAX - hlen, RECV_MS);
            if (got == AOS_TLS_TIMEOUT) {
                if (aos_hal_uptime_ms() - t > HEADERS_MS) break;
                continue;
            }
            if (got <= 0) break;
            hlen += got;
            x->hdr[hlen] = 0;
            char *e = strstr(x->hdr, "\r\n\r\n");
            if (e) body_at = (int)(e - x->hdr) + 4;
        }
        if (body_at < 0 || strncmp(x->hdr, "HTTP/", 5)) {
            aos_tls_close(x->c);
            say(j, AOS_HTTP_ERR_PROTO, "%s: no HTTP answer", u.host);
            return T_AGAIN;
        }
        x->hdr[body_at - 2] = 0;        /* the headers as a string of their own */
        const char *sp = strchr(x->hdr, ' ');
        status = sp ? atoi(sp + 1) : 0;
        if (status < 300 || status >= 400) break;

        aos_tls_close(x->c);
        /* the block is free until there is a body: the Location goes there */
        const char *loc = hdr_get(x->hdr, "Location", (char *)x->buf, URL_MAX);
        if (!loc || !url_follow(x->url, loc, x->tmp)) {
            say(j, AOS_FETCH_ERR_REDIRECT, "%s: a redirect (%d) with nowhere to go", u.host, status);
            return T_FATAL;
        }
    }

    /* x->c is open, the headers are in x->hdr and the body starts at body_at */
    char v[80];
    uint32_t total = 0;
    if (status == 206) {
        /* "bytes 100-749782/749783" */
        unsigned long from = 0, all = 0;
        const char *cr = hdr_get(x->hdr, "Content-Range", v, sizeof v);
        const char *slash = cr ? strchr(cr, '/') : NULL;
        if (!cr || sscanf(cr, "bytes %lu-", &from) != 1 || !slash || from != x->have) {
            aos_tls_close(x->c);
            remove(j->part);
            x->have = 0;
            say(j, 206, "%s: a range that is not the one asked for; starting again", u.host);
            return T_AGAIN;
        }
        all = strtoul(slash + 1, NULL, 10);
        total = (uint32_t)all;
    } else if (status == 200) {
        /* no ranges here (or nothing to continue): the whole file */
        x->have = 0;
        if (hdr_get(x->hdr, "Content-Length", v, sizeof v)) total = (uint32_t)strtoul(v, NULL, 10);
    } else if (status == 416 && x->have) {
        /* the .part is longer than the file, or of another one */
        aos_tls_close(x->c);
        remove(j->part);
        x->have = 0;
        say(j, 416, "%s: what was on the card is not of this file; starting again", u.host);
        return T_AGAIN;
    } else {
        aos_tls_close(x->c);
        say(j, status, "%s said %d", u.host, status);
        /* 5xx and 429 pass; a 404 does not */
        return (status >= 500 || status == 429 || status == 408) ? T_AGAIN : T_FATAL;
    }
    if (j->want_size && total && total != j->want_size) {
        aos_tls_close(x->c);
        remove(j->part);
        say(j, AOS_FETCH_ERR_SIZE, "%s has %u bytes of it and %u were expected", u.host,
            (unsigned)total, (unsigned)j->want_size);
        return T_FATAL;
    }
    chunk_t ck = { 0 };
    if (hdr_get(x->hdr, "Transfer-Encoding", v, sizeof v)) {
        for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
        ck.on = strstr(v, "chunked") != NULL;
    }
    if (ck.on) total = 0;       /* its Content-Length, if any, is not the body's */

    int fd = open(j->part, x->have ? (O_WRONLY | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC), 0644);
    if (fd < 0) {
        aos_tls_close(x->c);
        say(j, AOS_FETCH_ERR_FILE, "could not open %s (errno %d)", j->part, errno);
        return T_FATAL;
    }
    lock();
    j->info.status = status;
    j->info.total = total;
    j->info.got = x->have;
    j->info.why[0] = 0;
    unlock();

    /* what came after the headers is the body's first bytes */
    int fill = hlen - (body_at);
    if (fill > 0) {
        memcpy(x->buf, x->hdr + body_at, (size_t)fill);
        if (ck.on) fill = dechunk(&ck, x->buf, fill);
    } else fill = 0;

    int result = T_AGAIN, unsynced = 0;
    bool eof = false, broken = false;
    uint64_t last = aos_hal_uptime_ms();
    for (;;) {
        bool whole = total && x->have + (uint32_t)fill >= total;
        if (fill == BLOCK || ((whole || eof || broken || ck.done || j->cancel) && fill)) {
            if (!write_all(fd, x->buf, fill)) {
                say(j, AOS_FETCH_ERR_FILE, "could not write %s (errno %d): is the card full?", j->part, errno);
                result = T_FATAL;
                break;
            }
            x->have += (uint32_t)fill;
            fill = 0;
            if (++unsynced >= SYNC_BLOCKS) {
                fsync(fd);
                unsynced = 0;
            }
            lock();
            j->info.got = x->have;
            unlock();
        }
        if (j->cancel) { result = T_CANCEL; break; }
        if (whole || ck.done) { result = T_DONE; break; }
        if (eof) {
            /* with no length given, the end of the connection is the end */
            result = total ? T_AGAIN : T_DONE;
            if (total) say(j, AOS_HTTP_ERR_RECV, "%s closed at %u of %u bytes", u.host, (unsigned)x->have, (unsigned)total);
            break;
        }
        if (broken) {
            say(j, AOS_HTTP_ERR_RECV, "the connection to %s was cut at %u bytes", u.host, (unsigned)x->have);
            break;
        }
        int got = aos_tls_recv(x->c, x->buf + fill, BLOCK - fill, RECV_MS);
        if (got == AOS_TLS_TIMEOUT) {
            if (aos_hal_uptime_ms() - last > STALL_MS) broken = true;
            continue;
        }
        if (got == 0) { eof = true; continue; }
        if (got < 0) { broken = true; continue; }
        last = aos_hal_uptime_ms();
        if (ck.on) got = dechunk(&ck, x->buf + fill, got);
        fill += got;
    }
    close(fd);
    aos_tls_close(x->c);
    return result;
}

/* ---- the whole download ---------------------------------------------------- */

static bool verify(ctx_t *x)
{
    job_t *j = x->j;
    uint32_t size = file_size(j->part);
    if (j->want_size && size != j->want_size) {
        say(j, AOS_FETCH_ERR_SIZE, "%u bytes arrived and %u were expected", (unsigned)size, (unsigned)j->want_size);
        return false;
    }
    if (!j->has_hash) return true;
    uint64_t t = aos_hal_uptime_ms();
    int fd = open(j->part, O_RDONLY);
    aos_hal_sha256_ctx_t *h = fd >= 0 ? aos_hal_sha256_begin() : NULL;
    if (!h) {
        if (fd >= 0) close(fd);
        say(j, AOS_FETCH_ERR_FILE, "could not read %s back", j->part);
        return false;
    }
    for (;;) {
        int n = (int)read(fd, x->buf, BLOCK);
        if (n <= 0) break;
        aos_hal_sha256_update(h, x->buf, (size_t)n);
    }
    close(fd);
    uint8_t out[32];
    aos_hal_sha256_end(h, out);
    lock();
    j->info.verify_ms = (uint32_t)(aos_hal_uptime_ms() - t);
    unlock();
    if (memcmp(out, j->want, 32)) {
        say(j, AOS_FETCH_ERR_HASH, "the file is not the one published (its SHA-256 starts %02x%02x%02x%02x)",
            out[0], out[1], out[2], out[3]);
        return false;
    }
    return true;
}

static void work(void *arg)
{
    job_t *j = arg;
    ctx_t x = { .j = j };
    x.url = BIG_ALLOC(URL_MAX);
    x.tmp = BIG_ALLOC(URL_MAX);
    x.hdr = BIG_ALLOC(HDR_MAX + 1);
    x.buf = aos_hal_io_alloc(BLOCK);
    x.c   = aos_tls_new();
    aos_fetch_state_t end = AOS_FETCH_FAILED;

    if (!x.url || !x.tmp || !x.hdr || !x.buf || !x.c) {
        say(j, AOS_HTTP_ERR_MEM, "no memory for the download");
        goto out;
    }
    make_parents(j->path);
    x.have = file_size(j->part);
    if (j->want_size && x.have > j->want_size) {
        remove(j->part);
        x.have = 0;
    }
    lock();
    j->info.got = j->info.resumed = x.have;
    unlock();

    for (int idle = 0;;) {
        uint32_t before = x.have;
        int r = j->cancel ? T_CANCEL : try_once(&x);
        if (r == T_DONE) break;
        if (r == T_CANCEL) { end = AOS_FETCH_CANCELLED; say(j, j->info.status, "cancelled"); goto out; }
        if (r == T_FATAL) goto out;
        if (x.have > before) idle = 0;
        else if (++idle >= MAX_IDLE_TRIES) {
            lock();
            char last[sizeof j->info.why];
            snprintf(last, sizeof last, "%s", j->info.why);
            unlock();
            say(j, AOS_FETCH_ERR_GAVE_UP, "gave up after %d tries: %.64s", idle, last);
            goto out;
        }
        lock();
        j->info.retries++;
        LOGW("%s: %s; trying again", j->path, j->info.why);
        unlock();
        /* 1, 2, 4, 8 s: a network that just came back needs a moment */
        for (int w = 0; w < (5 << idle) && !j->cancel; w++) aos_hal_sleep_ms(100);
    }

    if (!verify(&x)) {
        remove(j->part);
        goto out;
    }
    remove(j->path);
    if (rename(j->part, j->path) != 0) {
        say(j, AOS_FETCH_ERR_FILE, "could not give %s its name (errno %d)", j->path, errno);
        goto out;
    }
    end = AOS_FETCH_DONE;

out:
    free(x.url);
    free(x.tmp);
    free(x.hdr);
    if (x.buf) aos_hal_io_free(x.buf);
    aos_tls_free(x.c);
    lock();
    j->info.ms = (uint32_t)(aos_hal_uptime_ms() - j->t0);
    j->info.state = end;
    if (end == AOS_FETCH_DONE) {
        j->info.why[0] = 0;
        LOGI("%s: %u bytes in %u ms (%u connecting, %u checking), %u connections, %u retries",
             j->path, (unsigned)j->info.got, (unsigned)j->info.ms, (unsigned)j->info.connect_ms,
             (unsigned)j->info.verify_ms, (unsigned)j->info.connects, (unsigned)j->info.retries);
    } else {
        LOGW("%s: %s (%d) at %u bytes", j->path, j->info.why, j->info.status, (unsigned)j->info.got);
    }
    j->running = false;
    if (j->released) job_free(j);
    unlock();
}

/* ---- the calls ------------------------------------------------------------- */

static bool hex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned b;
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]) ||
            sscanf(hex + 2 * i, "%2x", &b) != 1) return false;
        out[i] = (uint8_t)b;
    }
    return true;
}

int aos_fetch_start(const char *url, const char *path, const char *sha256_hex, uint32_t size)
{
    url_t u;
    if (!url || !path || !url_parse(url, &u) || strlen(url) >= URL_MAX) return -1;
    if (!path[0] || strlen(path) >= sizeof s_jobs[0].path) return -1;
    uint8_t want[32];
    if (sha256_hex && sha256_hex[0] && !hex32(sha256_hex, want)) return -1;

    /* the first call is the store's service or the portal, one thread each
     * and never at once at start-up */
    if (!s_lock) s_lock = aos_hal_mutex_create();
    if (!s_lock) return -3;

    lock();
    job_t *j = NULL;
    for (int i = 0; i < SLOTS; i++) {
        /* two downloads into one file would write over each other */
        if (s_jobs[i].id && s_jobs[i].running && !strcmp(s_jobs[i].path, path)) { unlock(); return -2; }
    }
    for (int i = 0; i < SLOTS && !j; i++)
        if (!s_jobs[i].id) j = &s_jobs[i];
    if (!j) { unlock(); return -2; }
    memset(j, 0, sizeof *j);
    j->url = BIG_ALLOC(URL_MAX);
    if (!j->url) { unlock(); return -3; }
    snprintf(j->url, URL_MAX, "%s", url);
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->part, sizeof j->part, "%s.part", path);
    if (sha256_hex && sha256_hex[0]) {
        memcpy(j->want, want, 32);
        j->has_hash = true;
    }
    j->want_size = size;
    j->t0 = aos_hal_uptime_ms();
    j->info.state = AOS_FETCH_BUSY;
    j->running = true;
    j->id = s_next_id++;
    if (s_next_id <= 0) s_next_id = 1;
    int id = j->id;
    unlock();

    /* below LVGL (4): a download waits on the network and on the card, and
     * neither should cost the screen a frame */
    if (!aos_hal_thread_start("aos_fetch", work, j, STACK, 3)) {
        lock();
        job_free(j);
        unlock();
        return -4;
    }
    return id;
}

bool aos_fetch_info(int id, aos_fetch_info_t *out)
{
    if (!s_lock) return false;
    lock();
    job_t *j = find(id);
    if (j && out) {
        *out = j->info;
        if (j->running) out->ms = (uint32_t)(aos_hal_uptime_ms() - j->t0);
    }
    unlock();
    return j != NULL;
}

void aos_fetch_cancel(int id)
{
    if (!s_lock) return;
    lock();
    job_t *j = find(id);
    if (j) j->cancel = true;
    unlock();
}

void aos_fetch_release(int id)
{
    if (!s_lock) return;
    lock();
    job_t *j = find(id);
    if (j) {
        if (j->running) {
            j->cancel = true;
            j->released = true;     /* the thread frees it on its way out */
        } else job_free(j);
    }
    unlock();
}
