/*
 * P4OS - Correo: a message taken apart, and one put together (cr.h).
 *
 * Taking apart walks the MIME tree once, over the raw bytes, which stay in
 * memory while the message is open: text parts are decoded (transfer
 * encoding, then charset) and written as formatted blocks; everything else
 * becomes an attachment that remembers where its encoded body is in the raw
 * message and its IMAP section number ("2.1"), so a part that a partial
 * fetch cut short can be asked for on its own.
 *
 * Which part is shown follows what mail readers do: of a
 * multipart/alternative the richest one understood (HTML over plain text),
 * of a multipart/related its first part with the others as the pictures it
 * names by cid:, of a multipart/mixed every part in order, text inline and
 * the rest as attachments; a forwarded message/rfc822 is shown inside, with
 * its own headers.
 */
#include "cr.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_text_safe.h"

#include <stdio.h>
#include <stdlib.h>

#define TEXT_MAX    (400 * 1024)    /* text shown, in all */
#define BLOCKS_MAX  2000

/* ---- headers ----------------------------------------------------------------------------------- */

/* The value of a header, unfolded (not decoded). False if absent. */
static bool hdr_get(const char *h, size_t n, const char *name, char *out, size_t on)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i < n;) {
        /* a line that starts a header */
        if ((i == 0 || h[i - 1] == '\n') && i + nl < n && h[i + nl] == ':' && cr_istarts(h + i, name)) {
            size_t j = i + nl + 1, k = 0;
            while (j < n && (h[j] == ' ' || h[j] == '\t')) j++;
            while (j < n) {
                if (h[j] == '\r') {
                    j++;
                    continue;
                }
                if (h[j] == '\n') {
                    if (j + 1 < n && (h[j + 1] == ' ' || h[j + 1] == '\t')) {
                        j++;
                        continue;           /* folded: the line goes on */
                    }
                    break;
                }
                if (k + 1 < on) out[k++] = h[j];
                j++;
            }
            if (on) out[k] = '\0';
            return true;
        }
        const char *nlp = memchr(h + i, '\n', n - i);
        if (!nlp) break;
        i = (size_t)(nlp - h) + 1;
    }
    if (on) out[0] = '\0';
    return false;
}

/* pct-decoding for RFC 2231 values */
static size_t pct_dec(char *out, const char *in, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '%' && i + 2 < n) {
            char hx[3] = {in[i + 1], in[i + 2], 0};
            char *e;
            long v = strtol(hx, &e, 16);
            if (!*e) {
                out[k++] = (char)v;
                i += 2;
                continue;
            }
        }
        out[k++] = in[i];
    }
    return k;
}

/* A parameter of a structured header (Content-Type, Content-Disposition):
 * plain, quoted, or RFC 2231's name*=charset''pct and name*0*= pieces. The
 * result is UTF-8. */
static bool hdr_param(const char *v, const char *name, char *out, size_t on)
{
    size_t nl = strlen(name);
    cr_buf_t acc = {0};
    char cs[32] = "";
    bool found = false, ext = false;
    const char *p = strchr(v, ';');
    while (p) {
        p++;
        while (cr_space((unsigned char)*p)) p++;
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t kn = (size_t)(eq - p);
        while (kn && cr_space((unsigned char)p[kn - 1])) kn--;
        bool match = kn >= nl && cr_istarts(p, name) && (kn == nl || p[nl] == '*');
        bool star = kn && p[kn - 1] == '*';
        const char *val = eq + 1;
        while (cr_space((unsigned char)*val)) val++;
        const char *end;
        char tmp[512];
        size_t tn = 0;
        if (*val == '"') {
            val++;
            const char *q = val;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1]) q++;
                if (tn + 1 < sizeof tmp) tmp[tn++] = *q;
                q++;
            }
            end = *q ? q + 1 : q;
        } else {
            end = val + strcspn(val, ";");
            size_t l = (size_t)(end - val);
            while (l && cr_space((unsigned char)val[l - 1])) l--;
            tn = l < sizeof tmp ? l : sizeof tmp - 1;
            memcpy(tmp, val, tn);
        }
        tmp[tn] = '\0';
        if (match) {
            found = true;
            if (star) {
                ext = true;
                char *s = tmp;
                /* the first piece may carry charset'lang' */
                char *a1 = strchr(s, '\'');
                char *a2 = a1 ? strchr(a1 + 1, '\'') : NULL;
                if (a1 && a2 && !acc.n) {
                    cr_ncopy(cs, sizeof cs, s, (size_t)(a1 - s));
                    s = a2 + 1;
                }
                char dec[512];
                size_t dn = pct_dec(dec, s, strlen(s));
                cr_buf_add(&acc, dec, dn);
            } else {
                cr_buf_add(&acc, tmp, tn);
            }
        }
        p = strchr(end, ';');
    }
    if (!found) {
        if (on) out[0] = '\0';
        cr_buf_free(&acc);
        return false;
    }
    if (ext) {
        size_t un;
        char *u = cr_to_utf8(acc.p ? acc.p : "", acc.n, cs[0] ? cs : "utf-8", &un);
        cr_ncopy(out, on, u ? u : "", u ? un : 0);
        cr_free(u);
    } else {
        /* a quoted name may still hold RFC 2047 words (Outlook does it) */
        cr_hdr_decode(out, on, acc.p ? acc.p : "", acc.n);
    }
    cr_buf_free(&acc);
    return true;
}

/* "text/html; charset=..." -> "text/html", lower case */
static void ctype_of(const char *v, char *out, size_t on)
{
    size_t k = 0;
    for (; *v && *v != ';' && k + 1 < on; v++) {
        if (!cr_space((unsigned char)*v)) out[k++] = (char)cr_lower((unsigned char)*v);
    }
    out[k] = '\0';
}

/* ---- the rich-text builder --------------------------------------------------------------------- */

void cr_rt_init(cr_rt_t *r, cr_msg_t *m)
{
    memset(r, 0, sizeof *r);
    r->m = m;
    r->link = -1;
    r->space = true;
}

void cr_rt_free(cr_rt_t *r)
{
    cr_buf_free(&r->t);
    cr_free(r->sp);
    r->sp = NULL;
}

static void push_block(cr_msg_t *m, const cr_block_t *b)
{
    if (m->nb >= BLOCKS_MAX) {
        m->cut = true;
        return;
    }
    if (m->nb == m->capb) {
        int c = m->capb ? m->capb * 2 : 64;
        cr_block_t *nb = cr_realloc(m->b, sizeof(cr_block_t) * (size_t)c);
        if (!nb) return;
        m->b = nb;
        m->capb = c;
    }
    m->b[m->nb++] = *b;
}

void cr_rt_end(cr_rt_t *r)
{
    /* trailing white space and newlines go */
    while (r->t.n && cr_space((unsigned char)r->t.p[r->t.n - 1])) r->t.n--;
    if (r->t.p) r->t.p[r->t.n] = '\0';
    bool keep = r->t.n > 0 || r->type == CR_B_HR || r->type == CR_B_IMG;
    if (keep && r->m) {
        cr_block_t b = {0};
        b.type = (uint8_t)r->type;
        b.lvl = (uint8_t)r->lvl;
        cr_copy(b.bullet, sizeof b.bullet, r->bullet);
        b.img = -1;
        b.len = (uint32_t)r->t.n;
        /* already filtered piece by piece in cr_rt_text */
        b.text = cr_arena_strn(&r->m->ar, r->t.p ? r->t.p : "", r->t.n);
        if (!b.text) b.len = 0;
        int nsp = 0;
        for (int i = 0; i < r->nsp; i++) {
            if (r->sp[i].len && r->sp[i].start < b.len) nsp++;
        }
        if (nsp) {
            b.sp = cr_arena_alloc(&r->m->ar, sizeof(cr_span_t) * (size_t)nsp);
            int k = 0;
            for (int i = 0; i < r->nsp && b.sp; i++) {
                cr_span_t s = r->sp[i];
                if (!s.len || s.start >= b.len) continue;
                if (s.start + s.len > b.len) s.len = b.len - s.start;
                b.sp[k++] = s;
            }
            b.nsp = b.sp ? k : 0;
        }
        if (r->type == CR_B_IMG) b.img = (int16_t)r->lvl, b.lvl = 0;
        push_block(r->m, &b);
    }
    r->t.n = 0;
    if (r->t.p) r->t.p[0] = '\0';
    r->nsp = 0;
    r->space = true;
}

void cr_rt_block(cr_rt_t *r, int type, int lvl, const char *bullet)
{
    cr_rt_end(r);
    r->type = type;
    r->lvl = lvl;
    cr_copy(r->bullet, sizeof r->bullet, bullet ? bullet : "");
}

/* Every piece goes through aos_text_safe as it comes, so the spans count
 * the bytes that are drawn: emoji become the code points that draw them in
 * colour, and what the font lacks is dropped or replaced. */
void cr_rt_text(cr_rt_t *r, const char *s0, size_t n0)
{
    if (!n0 || r->total > TEXT_MAX) {
        if (r->total > TEXT_MAX && r->m) r->m->cut = true;
        return;
    }
    char stack_in[600], stack_out[1300];
    char *in = n0 < sizeof stack_in ? stack_in : cr_alloc(n0 + 1);
    char *out = n0 < sizeof stack_in ? stack_out : cr_alloc(n0 * 2 + 8);
    if (!in || !out) {
        if (in != stack_in) cr_free(in);
        if (out != stack_out) cr_free(out);
        return;
    }
    memcpy(in, s0, n0);
    in[n0] = '\0';
    size_t n = aos_text_safe(out, n0 < sizeof stack_in ? sizeof stack_out : n0 * 2 + 8, in);
    n = strlen(out);
    if (in != stack_in) cr_free(in);
    if (!n) {
        if (out != stack_out) cr_free(out);
        return;
    }
    r->total += (uint32_t)n;
    uint32_t start = (uint32_t)r->t.n;
    cr_buf_add(&r->t, out, n);
    r->space = cr_space((unsigned char)out[n - 1]);
    if (out != stack_out) cr_free(out);
    if (r->style || r->link >= 0) {
        cr_span_t *last = r->nsp ? &r->sp[r->nsp - 1] : NULL;
        if (last && last->style == r->style && last->link == r->link && last->start + last->len == start) {
            last->len += (uint32_t)n;
        } else {
            if (r->nsp == r->capsp) {
                int c = r->capsp ? r->capsp * 2 : 16;
                cr_span_t *ns = cr_realloc(r->sp, sizeof(cr_span_t) * (size_t)c);
                if (!ns) return;
                r->sp = ns;
                r->capsp = c;
            }
            r->sp[r->nsp++] = (cr_span_t){start, (uint32_t)n, r->style, r->link};
        }
    }
}

int cr_rt_link(cr_rt_t *r, const char *url, size_t n)
{
    cr_msg_t *m = r->m;
    if (!m || m->nlinks >= 400) return -1;
    /* the same address twice is one link */
    for (int i = 0; i < m->nlinks; i++) {
        if (strlen(m->links[i]) == n && !memcmp(m->links[i], url, n)) return i;
    }
    if (!(m->nlinks & 31)) {
        char **nl = cr_realloc(m->links, sizeof(char *) * (size_t)(m->nlinks + 32));
        if (!nl) return -1;
        m->links = nl;
    }
    m->links[m->nlinks] = cr_arena_strn(&m->ar, url, n);
    return m->links[m->nlinks] ? m->nlinks++ : -1;
}

void cr_rt_img(cr_rt_t *r, int att)
{
    cr_rt_block(r, CR_B_IMG, att, NULL);
    cr_rt_end(r);
    r->type = CR_B_PARA;
    r->lvl = 0;
}

/* Text with its addresses made into links: http(s)://, www., mailto:. */
static void text_links(cr_rt_t *r, const char *s, size_t n)
{
    size_t i = 0, from = 0;
    while (i < n) {
        size_t skip = 0;
        if ((i == 0 || !((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z'))) &&
            (cr_istarts(s + i, "http://") || cr_istarts(s + i, "https://") || cr_istarts(s + i, "www.") ||
             cr_istarts(s + i, "mailto:"))) {
            size_t j = i;
            while (j < n && !cr_space((unsigned char)s[j]) && s[j] != '<' && s[j] != '>' && s[j] != '"') j++;
            /* a sentence's last dot or closing bracket is not the link's */
            while (j > i && strchr(".,;:!?)]'", s[j - 1])) j--;
            if (j - i > 7) skip = j - i;
        }
        if (!skip) {
            i++;
            continue;
        }
        cr_rt_text(r, s + from, i - from);
        int16_t old = r->link;
        uint8_t st = r->style;
        r->link = (int16_t)cr_rt_link(r, s + i, skip);
        r->style |= CR_S_LINK;
        cr_rt_text(r, s + i, skip);
        r->link = old;
        r->style = st;
        i += skip;
        from = i;
    }
    cr_rt_text(r, s + from, n - from);
}

/* text/plain: paragraphs, quotes by their '>' depth, format=flowed's soft
 * breaks joined, the signature after "-- " dimmed, links found. */
void cr_rt_plain(cr_rt_t *r, const char *s, size_t n, bool flowed)
{
    int cur_depth = -1;
    bool in_sig = false, soft = false;
    size_t i = 0;
    while (i <= n) {
        size_t e = i;
        while (e < n && s[e] != '\n') e++;
        size_t ln = e - i;
        if (ln && s[i + ln - 1] == '\r') ln--;
        const char *L = s + i;
        /* quote depth */
        int depth = 0;
        size_t k = 0;
        while (k < ln && (L[k] == '>' || (L[k] == ' ' && depth && k + 1 < ln && L[k + 1] == '>'))) {
            if (L[k] == '>') depth++;
            k++;
        }
        if (depth && k < ln && L[k] == ' ') k++;
        const char *T = L + k;
        size_t tn = ln - k;
        if (flowed && tn && T[0] == ' ' && !depth) {
            T++;                                  /* space-stuffed */
            tn--;
        }
        bool sig = !depth && ((tn == 3 && !memcmp(T, "-- ", 3)) || (tn == 2 && !memcmp(T, "--", 2) && !flowed));
        bool blank = true;
        for (size_t q = 0; q < tn; q++) {
            if (!cr_space((unsigned char)T[q])) {
                blank = false;
                break;
            }
        }
        if (e >= n && blank && i >= n) break;
        if (sig) {
            in_sig = true;
            cr_rt_block(r, CR_B_NOTE, 0, NULL);
            cr_rt_text(r, "--", 2);
            cur_depth = -2;
        } else if (blank) {
            if (cur_depth != -1 && !in_sig) cr_rt_end(r), cur_depth = -1;
            else if (in_sig) cr_rt_text(r, "\n", 1);
            soft = false;
        } else {
            int type = in_sig ? CR_B_NOTE : depth ? CR_B_QUOTE : CR_B_PARA;
            if (cur_depth != depth && !in_sig) {
                cr_rt_block(r, type, depth, NULL);
                cur_depth = depth;
            } else if (r->t.n) {
                if (!soft) cr_rt_text(r, "\n", 1);
            }
            bool this_soft = flowed && tn && T[tn - 1] == ' ';
            text_links(r, T, this_soft ? tn : tn);
            soft = this_soft;
        }
        if (e >= n) break;
        i = e + 1;
    }
    cr_rt_end(r);
}

/* ---- the MIME walk ----------------------------------------------------------------------------- */

typedef struct {
    cr_msg_t *m;
    cr_rt_t   rt;
    int       texts;           /* text parts shown so far */
} walk_t;

static void walk(walk_t *w, const char *s, size_t n, const char *path, int depth, bool in_related);

static void add_att(walk_t *w, const char *h, size_t hn, const char *body, size_t bn, const char *path,
                    const char *ctype, bool truncated)
{
    cr_msg_t *m = w->m;
    if (m->natt >= 64) return;
    if (!(m->natt & 7)) {
        cr_att_t *na = cr_realloc(m->att, sizeof(cr_att_t) * (size_t)(m->natt + 8));
        if (!na) return;
        m->att = na;
    }
    cr_att_t *a = &m->att[m->natt];
    memset(a, 0, sizeof *a);
    char v[600], cte[40];
    hdr_get(h, hn, "Content-Type", v, sizeof v);
    cr_copy(a->type, sizeof a->type, ctype);
    hdr_param(v, "charset", a->charset, sizeof a->charset);
    if (!hdr_param(v, "name", a->name, sizeof a->name)) a->name[0] = '\0';
    char d[600];
    if (hdr_get(h, hn, "Content-Disposition", d, sizeof d)) {
        char fn[128];
        if (hdr_param(d, "filename", fn, sizeof fn) && fn[0]) cr_copy(a->name, sizeof a->name, fn);
        a->inl = cr_istarts(d, "inline");
    }
    char cid[120];
    if (hdr_get(h, hn, "Content-ID", cid, sizeof cid)) {
        char *p = cid;
        while (*p == ' ' || *p == '<') p++;
        size_t l = strcspn(p, ">");
        cr_ncopy(a->cid, sizeof a->cid, p, l);
    }
    hdr_get(h, hn, "Content-Transfer-Encoding", cte, sizeof cte);
    a->enc = cr_istarts(cte, "base64") ? CR_ENC_B64 : cr_istarts(cte, "quoted") ? CR_ENC_QP : CR_ENC_7BIT;
    a->off = (uint32_t)(body - m->raw);
    a->len = (uint32_t)bn;
    a->size = a->enc == CR_ENC_B64 ? (uint32_t)(bn * 3 / 4) : (uint32_t)bn;
    a->truncated = truncated;
    cr_copy(a->part, sizeof a->part, path[0] ? path : "1");
    if (!a->name[0]) {
        /* a name from the type, so it can be saved */
        const char *sl = strchr(ctype, '/');
        if (!strcmp(ctype, "message/rfc822")) cr_copy(a->name, sizeof a->name, "mensaje.eml");
        else if (!strcmp(ctype, "text/calendar")) cr_copy(a->name, sizeof a->name, "invitacion.ics");
        else snprintf(a->name, sizeof a->name, "adjunto-%d.%.8s", m->natt + 1, sl ? sl + 1 : "bin");
    }
    m->natt++;
}

static char *decode_body(const char *h, size_t hn, const char *body, size_t bn, size_t *out_n)
{
    char cte[40];
    hdr_get(h, hn, "Content-Transfer-Encoding", cte, sizeof cte);
    char *d = cr_alloc(bn + 1);
    if (!d) return NULL;
    size_t dn;
    if (cr_istarts(cte, "base64")) dn = cr_b64_dec(d, body, bn);
    else if (cr_istarts(cte, "quoted")) dn = cr_qp_dec(d, body, bn, false);
    else {
        memcpy(d, body, bn);
        dn = bn;
    }
    d[dn] = '\0';
    *out_n = dn;
    return d;
}

static void show_text(walk_t *w, const char *h, size_t hn, const char *body, size_t bn, const char *ctype)
{
    char v[600], cs[40], fmt[20], delsp[8];
    hdr_get(h, hn, "Content-Type", v, sizeof v);
    hdr_param(v, "charset", cs, sizeof cs);
    hdr_param(v, "format", fmt, sizeof fmt);
    hdr_param(v, "delsp", delsp, sizeof delsp);
    size_t dn;
    char *d = decode_body(h, hn, body, bn, &dn);
    if (!d) return;
    size_t un;
    char *u = cr_to_utf8(d, dn, cs, &un);
    cr_free(d);
    if (!u) return;
    if (w->texts++) {
        cr_rt_block(&w->rt, CR_B_HR, 0, NULL);
        cr_rt_end(&w->rt);
        w->rt.type = CR_B_PARA;
    }
    if (!strcmp(ctype, "text/html")) {
        w->m->html = true;
        cr_html_render(&w->rt, u, un, w->m);
    } else {
        cr_rt_plain(&w->rt, u, un, cr_istarts(fmt, "flowed"));
    }
    cr_free(u);
}

static bool renderable(const char *ct)
{
    return !strcmp(ct, "text/plain") || !strcmp(ct, "text/html") || cr_istarts(ct, "multipart/");
}

static void walk_multipart(walk_t *w, const char *h, size_t hn, const char *body, size_t bn, const char *path,
                           int depth, const char *ctype)
{
    char v[600], bnd[200];
    hdr_get(h, hn, "Content-Type", v, sizeof v);
    if (!hdr_param(v, "boundary", bnd, sizeof bnd) || !bnd[0]) {
        show_text(w, h, hn, body, bn, "text/plain");
        return;
    }
    char delim[210];
    int dl = snprintf(delim, sizeof delim, "--%s", bnd);
    /* the parts: from each delimiter line to the next */
    const char *parts[64];
    size_t lens[64];
    bool closed = false;
    int np = 0;
    const char *cur = NULL;
    for (size_t i = 0; i + (size_t)dl <= bn;) {
        if ((i == 0 || body[i - 1] == '\n') && !memcmp(body + i, delim, (size_t)dl)) {
            const char *line_start = body + i;
            bool end = i + (size_t)dl + 1 < bn && body[i + dl] == '-' && body[i + dl + 1] == '-';
            if (cur && np < 64) {
                const char *e = line_start;
                /* the CRLF before the delimiter belongs to it */
                if (e > cur && e[-1] == '\n') e--;
                if (e > cur && e[-1] == '\r') e--;
                parts[np] = cur;
                lens[np] = (size_t)(e - cur);
                np++;
            }
            const char *nl = memchr(body + i, '\n', bn - i);
            if (end) {
                closed = true;
                cur = NULL;
                break;
            }
            cur = nl ? nl + 1 : body + bn;
            i = (size_t)(cur - body);
            continue;
        }
        const char *nl = memchr(body + i, '\n', bn - i);
        if (!nl) break;
        i = (size_t)(nl - body) + 1;
    }
    if (cur && np < 64) {          /* no closing delimiter: cut short */
        parts[np] = cur;
        lens[np] = (size_t)(body + bn - cur);
        np++;
    }
    bool truncated = !closed;
    char sub[24];
    bool alt = !strcmp(ctype, "multipart/alternative");
    bool rel = !strcmp(ctype, "multipart/related");
    if (alt) {
        /* the last one this reader can show; HTML wins over plain text */
        int best = -1, best_rank = -1;
        for (int k = 0; k < np; k++) {
            size_t ph = 0;
            const char *ps = parts[k];
            const char *sep = cr_imem(ps, lens[k], "\n\n");
            const char *sep2 = cr_imem(ps, lens[k], "\r\n\r\n");
            if (sep2 && (!sep || sep2 < sep)) sep = sep2 + 2;
            ph = sep ? (size_t)(sep - ps) : lens[k];
            char pv[300], pct[64];
            if (!hdr_get(ps, ph, "Content-Type", pv, sizeof pv)) cr_copy(pv, sizeof pv, "text/plain");
            ctype_of(pv, pct, sizeof pct);
            int rank = !strcmp(pct, "text/plain") ? 1 : !strcmp(pct, "text/html") ? 3 :
                       cr_istarts(pct, "multipart/") ? 2 : -1;
            if (cr_istarts(pct, "multipart/") && cr_imem(ps, lens[k], "text/html")) rank = 4;
            if (rank >= best_rank && rank > 0) {
                best_rank = rank;
                best = k;
            }
        }
        if (best >= 0) {
            snprintf(sub, sizeof sub, "%s%s%d", path, path[0] ? "." : "", best + 1);
            walk(w, parts[best], lens[best], sub, depth + 1, false);
        }
        return;
    }
    /* of a multipart/related, the pictures first, so the HTML finds them by cid: */
    for (int o = 0; o < np; o++) {
        int k = rel ? (o + 1) % np : o;
        snprintf(sub, sizeof sub, "%s%s%d", path, path[0] ? "." : "", k + 1);
        bool last = k == np - 1;
        if (truncated && last) {
            /* a part whose end is missing: an attachment fetched on its own */
            size_t ph = 0;
            const char *sep = cr_imem(parts[k], lens[k], "\r\n\r\n");
            ph = sep ? (size_t)(sep - parts[k]) + 4 : lens[k];
            char pv[300], pct[64];
            if (!hdr_get(parts[k], ph, "Content-Type", pv, sizeof pv)) cr_copy(pv, sizeof pv, "text/plain");
            ctype_of(pv, pct, sizeof pct);
            if (!renderable(pct) || hdr_get(parts[k], ph, "Content-Disposition", pv, sizeof pv)) {
                add_att(w, parts[k], ph, parts[k] + (ph < lens[k] ? ph : lens[k]), ph < lens[k] ? lens[k] - ph : 0, sub,
                        pct, true);
                continue;
            }
        }
        walk(w, parts[k], lens[k], sub, depth + 1, rel && k > 0);
    }
}

static void walk(walk_t *w, const char *s, size_t n, const char *path, int depth, bool in_related)
{
    if (depth > 12) return;
    /* headers and body */
    size_t hn = n, boff = n;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' && i + 1 < n && (s[i + 1] == '\n' || (s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n'))) {
            hn = i + 1;
            boff = s[i + 1] == '\n' ? i + 2 : i + 3;
            break;
        }
        if (i == 0 && (s[0] == '\n' || s[0] == '\r')) {      /* no headers at all */
            hn = 0;
            boff = s[0] == '\r' ? 2 : 1;
            break;
        }
    }
    const char *h = s, *body = s + boff;
    size_t bn = boff <= n ? n - boff : 0;
    char v[600], ctype[64], disp[300];
    if (!hdr_get(h, hn, "Content-Type", v, sizeof v)) cr_copy(v, sizeof v, "text/plain");
    ctype_of(v, ctype, sizeof ctype);
    if (!ctype[0] || !strchr(ctype, '/')) cr_copy(ctype, sizeof ctype, "text/plain");
    bool attach = hdr_get(h, hn, "Content-Disposition", disp, sizeof disp) && cr_istarts(disp, "attachment");
    char nm[8];
    bool named = hdr_param(v, "name", nm, sizeof nm);

    if (cr_istarts(ctype, "multipart/")) {
        walk_multipart(w, h, hn, body, bn, path, depth, ctype);
    } else if (!strcmp(ctype, "message/rfc822") && !attach) {
        /* a forwarded message: its headers, then it */
        const char *ih = body;
        size_t ihn = bn;
        const char *sep = cr_imem(body, bn, "\r\n\r\n");
        if (sep) ihn = (size_t)(sep - body) + 2;
        char from[200], subj[300], date[80], tmp[300];
        hdr_get(ih, ihn, "From", tmp, sizeof tmp);
        cr_hdr_decode(from, sizeof from, tmp, strlen(tmp));
        hdr_get(ih, ihn, "Subject", tmp, sizeof tmp);
        cr_hdr_decode(subj, sizeof subj, tmp, strlen(tmp));
        hdr_get(ih, ihn, "Date", tmp, sizeof tmp);
        cr_date_long(date, sizeof date, cr_date_parse(tmp));
        cr_rt_block(&w->rt, CR_B_FWD, 0, NULL);
        char line[700];
        snprintf(line, sizeof line, "%s\n%s: %s\n%s: %s\n%s: %s", _("Mensaje reenviado"), _("De"), from, _("Asunto"),
                 subj, _("Fecha"), date);
        cr_rt_text(&w->rt, line, strlen(line));
        cr_rt_end(&w->rt);
        w->rt.type = CR_B_PARA;
        w->texts = 0;
        char sub[24];
        snprintf(sub, sizeof sub, "%s", path[0] ? path : "1");
        /* the inner message's parts are numbered under this one */
        walk(w, body, bn, sub, depth + 1, false);
    } else if ((!strcmp(ctype, "text/plain") || !strcmp(ctype, "text/html")) && !attach && !(named && depth)) {
        show_text(w, h, hn, body, bn, ctype);
    } else {
        add_att(w, h, hn, body, bn, path, ctype, false);
        cr_att_t *a = &w->m->att[w->m->natt - 1];
        /* a picture that is not an attachment in the strict sense is shown
         * where it comes, unless an HTML part names it by cid: */
        if (w->m->natt && cr_istarts(ctype, "image/") && !attach && !in_related) {
            a->inl = true;
            cr_rt_img(&w->rt, w->m->natt - 1);
        } else if (in_related && a->cid[0]) {
            a->inl = true;
        }
    }
}

cr_msg_t *cr_mime_parse(char *raw, size_t n, bool partial)
{
    cr_msg_t *m = cr_alloc(sizeof *m);
    if (!m) {
        cr_free(raw);
        return NULL;
    }
    m->raw = raw;
    m->raw_len = n;
    m->partial = partial;
    m->acct = m->folder = -1;
    /* the top level's headers */
    size_t hn = n;
    const char *sep = cr_imem(raw, n, "\r\n\r\n");
    const char *sep2 = cr_imem(raw, n, "\n\n");
    if (sep2 && (!sep || sep2 < sep)) hn = (size_t)(sep2 - raw) + 1;
    else if (sep) hn = (size_t)(sep - raw) + 2;
    char tmp[1000];
    hdr_get(raw, hn, "From", tmp, sizeof tmp);
    cr_hdr_decode(m->from, sizeof m->from, tmp, strlen(tmp));
    /* the lists stay raw (encoded words inside) until cr_addr_parse */
    hdr_get(raw, hn, "To", m->to, sizeof m->to);
    hdr_get(raw, hn, "Cc", m->cc, sizeof m->cc);
    hdr_get(raw, hn, "Reply-To", m->reply_to, sizeof m->reply_to);
    hdr_get(raw, hn, "From", tmp, sizeof tmp);
    cr_copy(m->from, sizeof m->from, tmp);
    hdr_get(raw, hn, "Subject", tmp, sizeof tmp);
    cr_hdr_decode(m->subject, sizeof m->subject, tmp, strlen(tmp));
    hdr_get(raw, hn, "Message-ID", m->msgid, sizeof m->msgid);
    hdr_get(raw, hn, "In-Reply-To", m->irt, sizeof m->irt);
    hdr_get(raw, hn, "References", m->refs, sizeof m->refs);
    hdr_get(raw, hn, "Date", tmp, sizeof tmp);
    m->date = cr_date_parse(tmp);
    if (hdr_get(raw, hn, "List-Unsubscribe", tmp, sizeof tmp)) cr_copy(m->list_unsub, sizeof m->list_unsub, "1");
    cr_trim(m->msgid);

    walk_t w = {0};
    w.m = m;
    cr_rt_init(&w.rt, m);
    walk(&w, raw, n, "", 0, false);
    cr_rt_end(&w.rt);
    cr_rt_free(&w.rt);
    if (partial && !m->nb) {
        cr_rt_init(&w.rt, m);
        cr_rt_block(&w.rt, CR_B_NOTE, 0, NULL);
        const char *t = _("El mensaje es muy grande: se bajó sólo el principio.");
        cr_rt_text(&w.rt, t, strlen(t));
        cr_rt_end(&w.rt);
        cr_rt_free(&w.rt);
    }
    return m;
}

void cr_msg_free(cr_msg_t *m)
{
    if (!m) return;
    cr_free(m->raw);
    cr_free(m->b);
    cr_free(m->links);
    cr_free(m->att);
    cr_arena_free(&m->ar);
    cr_free(m);
}

uint8_t *cr_att_bytes(const cr_msg_t *m, int i, size_t *n)
{
    if (i < 0 || i >= m->natt) return NULL;
    const cr_att_t *a = &m->att[i];
    if (a->truncated || a->off + a->len > m->raw_len) return NULL;
    uint8_t *d = cr_alloc(a->len + 1);
    if (!d) return NULL;
    const char *s = m->raw + a->off;
    if (a->enc == CR_ENC_B64) *n = cr_b64_dec((char *)d, s, a->len);
    else if (a->enc == CR_ENC_QP) *n = cr_qp_dec((char *)d, s, a->len, false);
    else {
        memcpy(d, s, a->len);
        *n = a->len;
    }
    return d;
}

char *cr_msg_plain(const cr_msg_t *m, const char *prefix)
{
    cr_buf_t b = {0};
    const char *pf = prefix ? prefix : "";
    for (int i = 0; i < m->nb; i++) {
        const cr_block_t *bl = &m->b[i];
        char lead[64] = "";
        if (bl->type == CR_B_QUOTE) {
            int k = 0;
            for (int q = 0; q < bl->lvl && k < 40; q++) lead[k++] = '>';
            lead[k++] = ' ';
            lead[k] = '\0';
        } else if (bl->type == CR_B_LI) {
            snprintf(lead, sizeof lead, "%*s%s ", bl->lvl * 2, "", bl->bullet[0] ? bl->bullet : "-");
        }
        if (bl->type == CR_B_HR) {
            cr_buf_str(&b, pf);
            cr_buf_str(&b, "----\n\n");
            continue;
        }
        if (bl->type == CR_B_IMG) {
            if (bl->img >= 0 && bl->img < m->natt) {
                cr_buf_printf(&b, "%s[%s]\n\n", pf, m->att[bl->img].name);
            }
            continue;
        }
        const char *t = bl->text ? bl->text : "";
        const char *p = t;
        bool first = true;
        while (true) {
            const char *e = strchr(p, '\n');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            cr_buf_str(&b, pf);
            cr_buf_str(&b, first || bl->type != CR_B_LI ? lead : "   ");
            cr_buf_add(&b, p, l);
            cr_buf_add(&b, "\n", 1);
            first = false;
            if (!e) break;
            p = e + 1;
        }
        /* list items stay together; every other block ends in a blank line */
        bool next_li = i + 1 < m->nb && m->b[i + 1].type == CR_B_LI;
        if (!(bl->type == CR_B_LI && next_li)) {
            cr_buf_str(&b, pf);
            cr_buf_add(&b, "\n", 1);
        }
    }
    /* the links, numbered, after an HTML message */
    if (m->html && m->nlinks && !prefix) {
        for (int i = 0; i < m->nlinks && i < 40; i++) cr_buf_printf(&b, "[%d] %s\n", i + 1, m->links[i]);
    }
    return cr_buf_take(&b);
}

/* ---- a header block into a cr_hdr_t ----------------------------------------------------------- */

static const char *subject_core(const char *s)
{
    /* Re:, RE:, Fwd:, Fw:, RV:, AW:, WG:, Rif:, and [list] tags, any number */
    for (;;) {
        while (*s == ' ') s++;
        if (*s == '[') {
            const char *e = strchr(s, ']');
            if (e && e - s < 40) {
                s = e + 1;
                continue;
            }
        }
        static const char *const P[] = {"re:", "fwd:", "fw:", "rv:", "aw:", "wg:", "rif:", "r:", "tr:", "sv:"};
        bool hit = false;
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
            if (cr_istarts(s, P[i])) {
                s += strlen(P[i]);
                hit = true;
                break;
            }
        }
        if (!hit) return s;
    }
}

static uint32_t first_id(const char *s)
{
    const char *a = strchr(s, '<');
    if (!a) return 0;
    const char *b = strchr(a, '>');
    if (!b) return 0;
    return cr_hash(a, (size_t)(b - a + 1));
}

static uint32_t last_id(const char *s)
{
    const char *a = strrchr(s, '<');
    if (!a) return 0;
    const char *b = strchr(a, '>');
    if (!b) return 0;
    return cr_hash(a, (size_t)(b - a + 1));
}

void cr_hdr_from_fields(cr_hdr_t *h, const char *f, size_t n)
{
    char tmp[1000];
    cr_addr_t ad[4];
    hdr_get(f, n, "From", tmp, sizeof tmp);
    if (cr_addr_parse(tmp, ad, 1) == 1) {
        char who[80];
        cr_addr_show(who, sizeof who, &ad[0]);
        aos_text_safe(h->from, sizeof h->from, who);
        cr_copy(h->from_addr, sizeof h->from_addr, ad[0].addr);
    }
    hdr_get(f, n, "To", tmp, sizeof tmp);
    int nt = cr_addr_parse(tmp, ad, 4);
    if (nt) {
        char who[80];
        cr_addr_show(who, sizeof who, &ad[0]);
        if (nt > 1) {
            char more[96];
            snprintf(more, sizeof more, "%.70s +%d", who, nt - 1);
            aos_text_safe(h->to, sizeof h->to, more);
        } else {
            aos_text_safe(h->to, sizeof h->to, who);
        }
    }
    hdr_get(f, n, "Subject", tmp, sizeof tmp);
    char subj[300];
    cr_hdr_decode(subj, sizeof subj, tmp, strlen(tmp));
    aos_text_safe(h->subject, sizeof h->subject, subj);
    const char *core = subject_core(subj);
    if (core != subj) h->flags |= CR_F_RE;
    char low[300];
    size_t k = 0;
    for (const char *p = core; *p && k + 1 < sizeof low; p++) low[k++] = (char)cr_lower((unsigned char)*p);
    low[k] = '\0';
    cr_trim(low);
    h->subj_h = cr_hash(low, strlen(low));
    hdr_get(f, n, "Date", tmp, sizeof tmp);
    uint32_t d = cr_date_parse(tmp);
    if (d) h->date = d;
    hdr_get(f, n, "Message-ID", tmp, sizeof tmp);
    h->mid_h = first_id(tmp);
    char refs[1000];
    uint32_t root = 0;
    if (hdr_get(f, n, "References", refs, sizeof refs)) root = first_id(refs);
    if (!root && hdr_get(f, n, "In-Reply-To", tmp, sizeof tmp)) root = last_id(tmp);
    h->root_h = root ? root : h->mid_h;
    hdr_get(f, n, "Content-Type", tmp, sizeof tmp);
    if (cr_istarts(tmp, "multipart/mixed") || cr_istarts(tmp, "application/")) h->flags |= CR_F_ATTACH;
}

/* ---- putting one together --------------------------------------------------------------------- */

static const struct {
    const char *ext, *type;
} TYPES[] = {
    {"pdf", "application/pdf"},       {"jpg", "image/jpeg"},        {"jpeg", "image/jpeg"},
    {"png", "image/png"},             {"gif", "image/gif"},         {"bmp", "image/bmp"},
    {"txt", "text/plain"},            {"md", "text/markdown"},      {"csv", "text/csv"},
    {"html", "text/html"},            {"htm", "text/html"},         {"zip", "application/zip"},
    {"wav", "audio/wav"},             {"mp3", "audio/mpeg"},        {"json", "application/json"},
    {"ics", "text/calendar"},         {"vcf", "text/vcard"},        {"eml", "message/rfc822"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"odt", "application/vnd.oasis.opendocument.text"},
    {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
    {"doc", "application/msword"},    {"xls", "application/vnd.ms-excel"},
};

const char *cr_mime_type_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot) {
        for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) {
            if (!strcasecmp(dot + 1, TYPES[i].ext)) return TYPES[i].type;
        }
    }
    return "application/octet-stream";
}

/* An address list as the user typed it, written back clean and encoded. */
static void put_list(cr_buf_t *b, const char *hname, const char *list)
{
    cr_addr_t ad[40];
    int n = cr_addr_parse(list, ad, 40);
    if (!n) return;
    cr_buf_str(b, hname);
    cr_buf_str(b, ": ");
    for (int i = 0; i < n; i++) {
        if (i) cr_buf_str(b, ",\r\n ");
        if (ad[i].name[0]) {
            cr_hdr_encode(b, ad[i].name, true);
            cr_buf_str(b, " ");
        }
        cr_buf_printf(b, "<%s>", ad[i].addr);
    }
    cr_buf_str(b, "\r\n");
}

static bool ascii_short_lines(const char *s)
{
    int col = 0;
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) return false;
        if (*s == '\n') col = 0;
        else if (++col > 76) return false;
    }
    return true;
}

static void put_text(cr_buf_t *b, const char *body)
{
    if (ascii_short_lines(body)) {
        cr_buf_str(b, "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 7bit\r\n\r\n");
        /* CRLF out, a lone '.' line doubled is SMTP's job */
        for (const char *p = body; *p; p++) {
            if (*p == '\r') continue;
            if (*p == '\n') cr_buf_add(b, "\r\n", 2);
            else cr_buf_add(b, p, 1);
        }
        cr_buf_str(b, "\r\n");
    } else {
        cr_buf_str(b, "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n");
        cr_qp_enc(b, body, strlen(body));
        cr_buf_str(b, "\r\n");
    }
}

static bool all_ascii_name(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80 || *s == '"' || *s == '\\') return false;
    }
    return true;
}

static void put_filename(cr_buf_t *b, const char *name)
{
    /* both forms: RFC 2231 for those that read it, an RFC 2047 word in name=
     * for the ones that only read that */
    cr_buf_str(b, "Content-Disposition: attachment;");
    if (all_ascii_name(name)) {
        cr_buf_printf(b, " filename=\"%s\"\r\n", name);
        return;
    }
    cr_buf_str(b, "\r\n filename*=UTF-8''");
    static const char H[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._", *p)) {
            cr_buf_add(b, (const char *)p, 1);
        } else {
            char e[3] = {'%', H[*p >> 4], H[*p & 15]};
            cr_buf_add(b, e, 3);
        }
    }
    cr_buf_str(b, "\r\n");
}

char *cr_mime_build(const cr_out_t *o, const cr_acct_cfg_t *from, size_t *n, char *msgid, size_t mn)
{
    cr_buf_t b = {0};
    uint32_t now = cr_now();
    if (now) {
        char d[64];
        cr_date_rfc(d, sizeof d, now);
        cr_buf_printf(&b, "Date: %s\r\n", d);
    }
    cr_buf_str(&b, "From: ");
    if (from->name[0]) {
        cr_hdr_encode(&b, from->name, true);
        cr_buf_str(&b, " ");
    }
    cr_buf_printf(&b, "<%s>\r\n", from->email);
    put_list(&b, "To", o->to);
    put_list(&b, "Cc", o->cc);
    cr_buf_str(&b, "Subject: ");
    cr_hdr_encode(&b, o->subject, false);
    cr_buf_str(&b, "\r\n");
    /* a Message-ID of our own, on the sender's domain */
    uint8_t rnd[12];
    aos_hal_random(rnd, sizeof rnd);
    char id[64];
    cr_buf_t ib = {0};
    cr_b64_enc(&ib, rnd, sizeof rnd, 0);
    for (size_t i = 0; i < ib.n; i++) {
        if (ib.p[i] == '+' || ib.p[i] == '/') ib.p[i] = 'x';
    }
    const char *dom = strchr(from->email, '@');
    snprintf(id, sizeof id, "<%s.%lu@%.30s>", ib.p ? ib.p : "x", (unsigned long)now, dom ? dom + 1 : "p4os.local");
    cr_buf_free(&ib);
    cr_buf_printf(&b, "Message-ID: %s\r\n", id);
    if (msgid) cr_copy(msgid, mn, id);
    if (o->irt[0]) cr_buf_printf(&b, "In-Reply-To: %s\r\n", o->irt);
    if (o->refs && o->refs[0]) cr_buf_printf(&b, "References: %s\r\n", o->refs);
    cr_buf_str(&b, "MIME-Version: 1.0\r\nUser-Agent: P4OS Correo\r\n");
    const char *body = o->body ? o->body : "";
    if (!o->natt) {
        put_text(&b, body);
    } else {
        char bnd[48];
        snprintf(bnd, sizeof bnd, "=_p4os_%08lx%08lx", (unsigned long)cr_hash(id, strlen(id)),
                 (unsigned long)now);
        cr_buf_printf(&b, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n", bnd);
        cr_buf_printf(&b, "--%s\r\n", bnd);
        put_text(&b, body);
        for (int i = 0; i < o->natt; i++) {
            const cr_out_att_t *a = &o->att[i];
            size_t dn = a->data_n;
            uint8_t *d = a->data;
            bool own = false;
            if (!d) {
                d = (uint8_t *)cr_file_read(a->path, &dn, CR_PART_MAX);
                own = true;
            }
            if (!d) continue;
            cr_buf_printf(&b, "--%s\r\nContent-Type: %s; name=\"", bnd, a->type[0] ? a->type : cr_mime_type_for(a->name));
            cr_buf_t nb = {0};
            cr_hdr_encode(&nb, a->name, false);
            /* folded words inside a quoted name would break it: one line */
            for (size_t k = 0; k < nb.n; k++) {
                if (nb.p[k] == '\r' || nb.p[k] == '\n') nb.p[k] = ' ';
            }
            cr_buf_add(&b, nb.p ? nb.p : "", nb.n);
            cr_buf_free(&nb);
            cr_buf_str(&b, "\"\r\n");
            put_filename(&b, a->name);
            cr_buf_str(&b, "Content-Transfer-Encoding: base64\r\n\r\n");
            cr_b64_enc(&b, d, dn, 76);
            cr_buf_str(&b, "\r\n");
            if (own) cr_free(d);
        }
        cr_buf_printf(&b, "--%s--\r\n", bnd);
    }
    if (b.oom) {
        cr_buf_free(&b);
        return NULL;
    }
    *n = b.n;
    return cr_buf_take(&b);
}

void cr_out_free(cr_out_t *o)
{
    if (!o) return;
    cr_free(o->body);
    cr_free(o->refs);
    for (int i = 0; i < o->natt; i++) cr_free(o->att[i].data);
    cr_free(o->att);
    cr_free(o);
}
