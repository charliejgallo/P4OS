/*
 * P4OS - Correo: the portal's page, over the live channel (apps/correo/web/correo.js).
 *
 * The page cannot speak IMAP from the browser, so it asks the board: a
 * short line of text in (POST /api/live?app=aos.correo), JSON out under a
 * key (GET /api/live?app=aos.correo&key=...):
 *
 *   state                       -> "state": accounts, their folders, unread
 *   list <a> <f> [text]         -> "list": up to 300 rows (a = -1: every INBOX)
 *   open <a> <f> <uid>          -> "msg": headers, the formatted blocks, links,
 *                                  attachments and the plain text for a reply
 *   att <a> <f> <uid> <i>       -> "att": the attachment saved under
 *                                  <data>/correo/web/, for the page to fetch
 *   flag <a> <f> <uid> <add> <del>, move <a> <f> <uid> <dest>,
 *   archive|delete <a> <f> <uid>, sync <a> <f>
 *   send <path>                 -> "sent": a message the page wrote as JSON
 *                                  with fsPut (to, cc, bcc, subject, body,
 *                                  irt, refs, attachments as card paths)
 *
 * Everything goes through the same jobs the screens use; the password never
 * leaves the board and is not part of anything the page can read.
 */
#include "cr_ui.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LIVE_MAX (60 * 1024)

static struct {
    uint32_t ver_sent;
    uint32_t last_state_ms;
    uint32_t open_token, send_token, att_token;
    bool     looked;
} PT;

bool cr_portal_owns(const cr_job_t *j)
{
    return TOKEN_OWN(j->token) == OWN_PORTAL;
}

static cr_msg_t *s_msg;

void cr_portal_stop(void)
{
    cr_msg_free(s_msg);
    s_msg = NULL;
    aos_hal_live_clear(CR_APP_ID);
    memset(&PT, 0, sizeof PT);
}

static void put(const char *key, cr_buf_t *b)
{
    if (b->oom) return;
    if (b->n > LIVE_MAX) b->n = LIVE_MAX;
    aos_hal_live_put(CR_APP_ID, key, "application/json", b->p ? b->p : "{}", b->n);
}

/* ---- state and lists ---------------------------------------------------------------------------- */

static void put_state(void)
{
    cr_buf_t b = {0};
    cr_buf_str(&b, "{\"accounts\":[");
    cr_lock();
    for (int i = 0; i < CR.na; i++) {
        cr_acct_t *a = &CR.a[i];
        cr_buf_printf(&b, "%s{\"i\":%d,\"label\":", i ? "," : "", i);
        cr_json_str(&b, ui_acct_name(i));
        cr_buf_str(&b, ",\"email\":");
        cr_json_str(&b, a->cfg.email);
        cr_buf_str(&b, ",\"name\":");
        cr_json_str(&b, a->cfg.name);
        cr_buf_printf(&b, ",\"color\":\"#%06lX\",\"state\":", (unsigned long)a->cfg.color);
        cr_json_str(&b, ui_state_text(a));
        cr_buf_printf(&b, ",\"ok\":%s,\"unseen\":%lu,\"folders\":[",
                      a->state == CR_ST_ONLINE || a->state == CR_ST_IDLE ? "true" : "false", (unsigned long)a->unseen);
        bool first = true;
        for (int k = 0; k < a->nf; k++) {
            cr_folder_t *f = &a->f[k];
            if (f->gone || f->noselect) continue;
            cr_buf_printf(&b, "%s{\"i\":%d,\"name\":", first ? "" : ",", k);
            cr_json_str(&b, cr_folder_label(f));
            cr_buf_printf(&b, ",\"sp\":%d,\"depth\":%d,\"unseen\":%lu,\"n\":%lu}", f->special, f->depth,
                          (unsigned long)f->unseen, (unsigned long)f->exists);
            first = false;
        }
        cr_buf_str(&b, "]}");
    }
    cr_unlock();
    cr_buf_printf(&b, "],\"threads\":%s}", CR.threads ? "true" : "false");
    put("state", &b);
    cr_buf_free(&b);
}

typedef struct {
    const cr_hdr_t *h;
} pref_t;

static int cmp_pdate(const void *x, const void *y)
{
    const cr_hdr_t *a = ((const pref_t *)x)->h, *b = ((const pref_t *)y)->h;
    return a->date < b->date ? 1 : a->date > b->date ? -1 : 0;
}

static void put_list(int acct, int folder, const char *q)
{
    cr_buf_t b = {0};
    cr_lock();
    int total = 0;
    for (int ai = 0; ai < CR.na; ai++) {
        if (acct >= 0 && ai != acct) continue;
        int fi = acct >= 0 ? folder : cr_folder_special(&CR.a[ai], CR_SP_INBOX);
        if (fi >= 0 && fi < CR.a[ai].nf) total += CR.a[ai].f[fi].n;
    }
    pref_t *r = total ? cr_alloc(sizeof(pref_t) * (size_t)total) : NULL;
    int n = 0;
    for (int ai = 0; ai < CR.na && r; ai++) {
        if (acct >= 0 && ai != acct) continue;
        int fi = acct >= 0 ? folder : cr_folder_special(&CR.a[ai], CR_SP_INBOX);
        if (fi < 0 || fi >= CR.a[ai].nf) continue;
        cr_folder_t *f = &CR.a[ai].f[fi];
        for (int i = 0; i < f->n && n < total; i++) {
            cr_hdr_t *h = &f->h[i];
            h->acct = (uint8_t)ai;
            h->folder = (uint8_t)fi;
            if (q && q[0] && !cr_istr(h->subject, q) && !cr_istr(h->from, q) && !cr_istr(h->from_addr, q)) continue;
            r[n++].h = h;
        }
    }
    if (n > 1) qsort(r, (size_t)n, sizeof(pref_t), cmp_pdate);
    cr_buf_printf(&b, "{\"a\":%d,\"f\":%d,\"total\":%d,\"rows\":[", acct, folder, n);
    for (int i = 0; i < n && i < 300; i++) {
        const cr_hdr_t *h = r[i].h;
        cr_buf_printf(&b, "%s{\"a\":%d,\"f\":%d,\"u\":%lu,\"d\":%lu,\"fl\":%u,\"sz\":%lu,\"t\":%lu,\"from\":", i ? "," : "",
                      h->acct, h->folder, (unsigned long)h->uid, (unsigned long)h->date, h->flags, (unsigned long)h->size,
                      (unsigned long)h->root_h);
        cr_json_str(&b, h->from[0] ? h->from : h->from_addr);
        cr_buf_str(&b, ",\"addr\":");
        cr_json_str(&b, h->from_addr);
        cr_buf_str(&b, ",\"to\":");
        cr_json_str(&b, h->to);
        cr_buf_str(&b, ",\"s\":");
        cr_json_str(&b, h->subject);
        cr_buf_str(&b, "}");
    }
    cr_unlock();
    cr_free(r);
    cr_buf_str(&b, "]}");
    put("list", &b);
    cr_buf_free(&b);
}

/* ---- a message --------------------------------------------------------------------------------- */

static void put_msg(const cr_msg_t *m)
{
    cr_buf_t b = {0};
    cr_buf_printf(&b, "{\"a\":%d,\"f\":%d,\"u\":%lu,\"d\":%lu,\"fl\":%u,\"partial\":%s,\"remote\":%s,\"from\":", m->acct,
                  m->folder, (unsigned long)m->uid, (unsigned long)m->date, m->flags, m->partial ? "true" : "false",
                  m->remote_img ? "true" : "false");
    char tmp[700];
    cr_hdr_decode(tmp, sizeof tmp, m->from, strlen(m->from));
    cr_json_str(&b, tmp);
    cr_buf_str(&b, ",\"to\":");
    cr_hdr_decode(tmp, sizeof tmp, m->to, strlen(m->to));
    cr_json_str(&b, tmp);
    cr_buf_str(&b, ",\"cc\":");
    cr_hdr_decode(tmp, sizeof tmp, m->cc, strlen(m->cc));
    cr_json_str(&b, tmp);
    cr_buf_str(&b, ",\"reply_to\":");
    cr_hdr_decode(tmp, sizeof tmp, m->reply_to, strlen(m->reply_to));
    cr_json_str(&b, tmp);
    cr_buf_str(&b, ",\"subject\":");
    cr_json_str(&b, m->subject);
    cr_buf_str(&b, ",\"msgid\":");
    cr_json_str(&b, m->msgid);
    cr_buf_str(&b, ",\"refs\":");
    cr_json_str(&b, m->refs);
    cr_buf_str(&b, ",\"links\":[");
    for (int i = 0; i < m->nlinks; i++) {
        if (i) cr_buf_str(&b, ",");
        cr_json_str(&b, m->links[i]);
    }
    cr_buf_str(&b, "],\"att\":[");
    for (int i = 0; i < m->natt; i++) {
        cr_buf_printf(&b, "%s{\"i\":%d,\"name\":", i ? "," : "", i);
        cr_json_str(&b, m->att[i].name);
        cr_buf_str(&b, ",\"type\":");
        cr_json_str(&b, m->att[i].type);
        cr_buf_printf(&b, ",\"size\":%lu,\"inl\":%s,\"cid\":", (unsigned long)m->att[i].size, m->att[i].inl ? "true" : "false");
        cr_json_str(&b, m->att[i].cid);
        cr_buf_str(&b, "}");
    }
    /* the blocks, while they fit; the page draws them as the board does */
    cr_buf_str(&b, "],\"blocks\":[");
    for (int i = 0; i < m->nb && b.n < LIVE_MAX - 12000; i++) {
        const cr_block_t *bl = &m->b[i];
        cr_buf_printf(&b, "%s{\"t\":%d,\"l\":%d,\"img\":%d,\"b\":", i ? "," : "", bl->type, bl->lvl, bl->img);
        cr_json_str(&b, bl->bullet);
        cr_buf_str(&b, ",\"x\":");
        cr_json_str(&b, bl->text ? bl->text : "");
        cr_buf_str(&b, ",\"s\":[");
        for (int k = 0; k < bl->nsp; k++) {
            cr_buf_printf(&b, "%s[%lu,%lu,%u,%d]", k ? "," : "", (unsigned long)bl->sp[k].start,
                          (unsigned long)bl->sp[k].len, bl->sp[k].style, bl->sp[k].link);
        }
        cr_buf_str(&b, "]}");
    }
    cr_buf_str(&b, "],\"quote\":");
    char *plain = cr_msg_plain(m, "> ");
    if (plain && strlen(plain) > 8000) plain[8000] = '\0';
    cr_json_str(&b, plain ? plain : "");
    cr_free(plain);
    cr_buf_str(&b, "}");
    put("msg", &b);
    cr_buf_free(&b);
}


/* ---- a message the page wrote ------------------------------------------------------------------ */

/* The string value of "key" in a flat JSON object, unescaped. */
static bool jstr(const char *j, const char *key, char *out, size_t n)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') {
        /* a number */
        size_t k = 0;
        while (*p && *p != ',' && *p != '}' && k + 1 < n) out[k++] = *p++;
        out[k] = '\0';
        return k > 0;
    }
    p++;
    size_t k = 0;
    while (*p && *p != '"' && k + 5 < n) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'n') out[k++] = '\n';
            else if (c == 't') out[k++] = '\t';
            else if (c == 'r') {
            } else if (c == 'u' && p[1] && p[2] && p[3] && p[4]) {
                char hx[5] = {p[1], p[2], p[3], p[4], 0};
                uint32_t cp = (uint32_t)strtoul(hx, NULL, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                    char h2[5] = {p[3], p[4], p[5], p[6], 0};
                    uint32_t lo = (uint32_t)strtoul(h2, NULL, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                if (cp < 0x80) out[k++] = (char)cp;
                else if (cp < 0x800) {
                    out[k++] = (char)(0xC0 | (cp >> 6));
                    out[k++] = (char)(0x80 | (cp & 63));
                } else if (cp < 0x10000) {
                    out[k++] = (char)(0xE0 | (cp >> 12));
                    out[k++] = (char)(0x80 | ((cp >> 6) & 63));
                    out[k++] = (char)(0x80 | (cp & 63));
                } else {
                    out[k++] = (char)(0xF0 | (cp >> 18));
                    out[k++] = (char)(0x80 | ((cp >> 12) & 63));
                    out[k++] = (char)(0x80 | ((cp >> 6) & 63));
                    out[k++] = (char)(0x80 | (cp & 63));
                }
            } else {
                out[k++] = c;
            }
            p++;
            continue;
        }
        out[k++] = *p++;
    }
    out[k] = '\0';
    return true;
}

static void sent_answer(bool ok, const char *why)
{
    cr_buf_t b = {0};
    cr_buf_printf(&b, "{\"ok\":%s,\"t\":%lu,\"why\":", ok ? "true" : "false", (unsigned long)aos_hal_uptime_ms());
    cr_json_str(&b, why ? why : "");
    cr_buf_str(&b, "}");
    put("sent", &b);
    cr_buf_free(&b);
}

static void send_file(const char *path)
{
    /* only what the page writes, in Correo's own folder */
    char dir[200];
    snprintf(dir, sizeof dir, "%s/salida/", cr_dir());
    if (strncmp(path, dir, strlen(dir)) || strstr(path, "..")) {
        sent_answer(false, "not a compose file");
        return;
    }
    size_t n;
    char *j = cr_file_read(path, &n, 2 * 1024 * 1024);
    if (!j) {
        sent_answer(false, "cannot read it");
        return;
    }
    cr_out_t *o = cr_alloc(sizeof *o);
    char num[24];
    o->acct = jstr(j, "acct", num, sizeof num) ? atoi(num) : 0;
    if (o->acct < 0 || o->acct >= CR.na) o->acct = 0;
    jstr(j, "to", o->to, sizeof o->to);
    jstr(j, "cc", o->cc, sizeof o->cc);
    jstr(j, "bcc", o->bcc, sizeof o->bcc);
    jstr(j, "subject", o->subject, sizeof o->subject);
    jstr(j, "irt", o->irt, sizeof o->irt);
    char *big = cr_alloc(n + 1);
    if (big && jstr(j, "body", big, n + 1)) o->body = big;
    else cr_free(big);
    char *refs = cr_alloc(2048);
    if (refs && jstr(j, "refs", refs, 2048)) o->refs = refs;
    else cr_free(refs);
    o->reply_folder = jstr(j, "reply_folder", num, sizeof num) ? atoi(num) : -1;
    o->reply_uid = jstr(j, "reply_uid", num, sizeof num) ? (uint32_t)strtoul(num, NULL, 10) : 0;
    o->forward = jstr(j, "forward", num, sizeof num) && !strcmp(num, "true");
    o->draft_folder = -1;
    /* "attachments": ["path", ...] */
    const char *at = strstr(j, "\"attachments\"");
    if (at && (at = strchr(at, '['))) {
        o->att = cr_alloc(sizeof(cr_out_att_t) * 16);
        const char *p = at + 1;
        while (o->att && o->natt < 16 && (p = strchr(p, '"'))) {
            const char *e = strchr(p + 1, '"');
            const char *close = strchr(at, ']');
            if (!e || (close && p > close)) break;
            cr_out_att_t *a = &o->att[o->natt];
            cr_ncopy(a->path, sizeof a->path, p + 1, (size_t)(e - p - 1));
            const char *sl = strrchr(a->path, '/');
            cr_copy(a->name, sizeof a->name, sl ? sl + 1 : a->path);
            cr_copy(a->type, sizeof a->type, cr_mime_type_for(a->name));
            if (cr_file_size(a->path)) o->natt++;
            p = e + 1;
        }
    }
    cr_free(j);
    cr_job_t *job = cr_job_new(CR_J_SEND);
    if (!job) {
        cr_out_free(o);
        return;
    }
    job->acct = o->acct;
    job->out_msg = o;
    PT.send_token = job->token = ui_token(OWN_PORTAL);
    cr_job_put(job);
    unlink(path);
}

static void save_att(int i)
{
    cr_msg_t *m = s_msg;
    cr_buf_t b = {0};
    if (!m || i < 0 || i >= m->natt) {
        cr_buf_str(&b, "{\"ok\":false}");
        put("att", &b);
        cr_buf_free(&b);
        return;
    }
    char dir[200], path[340];
    snprintf(dir, sizeof dir, "%s/web", cr_dir());
    cr_mkdirs(dir);
    /* one at a time: the folder holds only the last one asked for */
    cr_rmtree(dir);
    cr_mkdirs(dir);
    char safe[128];
    size_t k = 0;
    for (const char *p = m->att[i].name; *p && k + 1 < sizeof safe; p++) {
        char c = *p;
        if (strchr("/\\:*?\"<>|", c) || (unsigned char)c < 32) c = '_';
        safe[k++] = c;
    }
    safe[k] = '\0';
    snprintf(path, sizeof path, "%s/%s", dir, safe[0] ? safe : "adjunto");
    if (m->att[i].truncated || m->partial) {
        cr_job_t *j = cr_job_new(CR_J_PART);
        if (!j) return;
        j->acct = m->acct;
        j->folder = m->folder;
        j->uid = m->uid;
        cr_copy(j->text, sizeof j->text, m->att[i].part);
        cr_copy(j->path, sizeof j->path, path);
        j->arg = m->att[i].enc;
        PT.att_token = j->token = ui_token(OWN_PORTAL);
        cr_job_put(j);
        return;
    }
    size_t n = 0;
    uint8_t *d = cr_att_bytes(m, i, &n);
    bool ok = d && cr_file_write(path, d, n);
    cr_free(d);
    cr_buf_printf(&b, "{\"ok\":%s,\"path\":", ok ? "true" : "false");
    /* the path as the portal names it: from the card's root */
    const char *sd = aos_hal_path_sd_root();
    const char *rel = sd && !strncmp(path, sd, strlen(sd)) ? path + strlen(sd) : path;
    cr_json_str(&b, rel);
    cr_buf_printf(&b, ",\"t\":%lu}", (unsigned long)aos_hal_uptime_ms());
    put("att", &b);
    cr_buf_free(&b);
}

/* ---- the page's lines ------------------------------------------------------------------------- */

static void job_for(int kind, int a, int f, uint32_t uid, int arg, int arg2)
{
    if (a < 0 || a >= CR.na) return;
    cr_job_t *j = cr_job_new(kind);
    if (!j) return;
    j->acct = a;
    j->folder = f;
    j->uid = uid;
    j->arg = arg;
    j->arg2 = arg2;
    j->token = ui_token(OWN_PORTAL);
    if (kind == CR_J_OPEN) PT.open_token = j->token;
    cr_job_put(j);
}

static void line(char *l)
{
    char cmd[16] = "";
    int a = -1, f = -1, x = 0, y = 0;
    unsigned long u = 0;
    int used = 0;
    sscanf(l, "%15s %n", cmd, &used);
    char *rest = l + used;
    if (!strcmp(cmd, "state")) {
        put_state();
    } else if (!strcmp(cmd, "list")) {
        int k = 0;
        sscanf(rest, "%d %d %n", &a, &f, &k);
        put_list(a, f, k ? rest + k : "");
    } else if (!strcmp(cmd, "open") && sscanf(rest, "%d %d %lu", &a, &f, &u) == 3) {
        job_for(CR_J_OPEN, a, f, (uint32_t)u, 0, 0);
    } else if (!strcmp(cmd, "att") && sscanf(rest, "%d %d %lu %d", &a, &f, &u, &x) == 4) {
        save_att(x);
    } else if (!strcmp(cmd, "flag") && sscanf(rest, "%d %d %lu %d %d", &a, &f, &u, &x, &y) == 5) {
        cr_act_flags(a, f, (uint32_t)u, (uint16_t)x, (uint16_t)y);
    } else if (!strcmp(cmd, "move") && sscanf(rest, "%d %d %lu %d", &a, &f, &u, &x) == 4) {
        cr_act_move(a, f, (uint32_t)u, x);
    } else if (!strcmp(cmd, "archive") && sscanf(rest, "%d %d %lu", &a, &f, &u) == 3) {
        cr_act_archive(a, f, (uint32_t)u);
    } else if (!strcmp(cmd, "delete") && sscanf(rest, "%d %d %lu", &a, &f, &u) == 3) {
        cr_act_delete(a, f, (uint32_t)u);
    } else if (!strcmp(cmd, "sync") && sscanf(rest, "%d %d", &a, &f) == 2) {
        job_for(CR_J_SYNC, a, f, 0, 1, 0);
    } else if (!strcmp(cmd, "send")) {
        cr_trim(rest);
        /* the page names it from the card's root; the app reads it under the mount point */
        char path[300];
        const char *sd = aos_hal_path_sd_root();
        if (sd && rest[0] == '/' && strncmp(rest, sd, strlen(sd))) snprintf(path, sizeof path, "%s%s", sd, rest);
        else cr_copy(path, sizeof path, rest);
        send_file(path);
    }
}

void cr_portal_tick(void)
{
    char buf[AOS_LIVE_MSG_MAX + 1];
    int n;
    int guard = 0;
    while (guard++ < 16 && (n = aos_hal_live_take(CR_APP_ID, buf, (int)sizeof buf - 1)) > 0) {
        buf[n] = '\0';
        line(buf);
        PT.looked = true;
    }
    if (!PT.looked) return;
    uint32_t idle = aos_hal_live_idle_ms(CR_APP_ID);
    if (idle > 15000) {
        PT.looked = false;
        return;
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (CR.ver != PT.ver_sent && now - PT.last_state_ms > 1500) {
        PT.ver_sent = CR.ver;
        PT.last_state_ms = now;
        put_state();
    }
}

void cr_portal_job(cr_job_t *j)
{
    if (j->kind == CR_J_OPEN && j->token == PT.open_token) {
        if (j->ok && j->out) {
            cr_msg_free(s_msg);
            s_msg = j->out;
            j->out = NULL;
            put_msg(s_msg);
        } else {
            cr_buf_t b = {0};
            cr_buf_str(&b, "{\"error\":");
            cr_json_str(&b, j->why);
            cr_buf_str(&b, "}");
            put("msg", &b);
            cr_buf_free(&b);
        }
    } else if (j->kind == CR_J_SEND && j->token == PT.send_token) {
        sent_answer(j->ok, j->ok ? "" : j->why);
    } else if (j->kind == CR_J_PART && j->token == PT.att_token) {
        cr_buf_t b = {0};
        const char *sd = aos_hal_path_sd_root();
        const char *rel = sd && !strncmp(j->path, sd, strlen(sd)) ? j->path + strlen(sd) : j->path;
        cr_buf_printf(&b, "{\"ok\":%s,\"path\":", j->ok ? "true" : "false");
        cr_json_str(&b, rel);
        cr_buf_printf(&b, ",\"t\":%lu}", (unsigned long)aos_hal_uptime_ms());
        put("att", &b);
        cr_buf_free(&b);
    }
}
