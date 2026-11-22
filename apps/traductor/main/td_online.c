/*
 * P4OS - Traductor: the online engines.
 *
 * MyMemory (no account): GET <base>/get?q=...&langpair=es|en[&de=email].
 * The HTTP status is always 200; the real one is responseStatus (a number,
 * or a number in a string), an error comes as the translatedText itself,
 * and the daily limit as quotaFinished true and status 429 with a "MYMEMORY
 * WARNING" text. q takes 500 bytes: td_job.c sends longer texts in pieces.
 *
 * LibreTranslate (the user's own server): POST <server>/translate with
 * {"q","source","target","format":"text","api_key"} -> {"translatedText"}
 * and, with source "auto", "detectedLanguage":{"language","confidence"}.
 * Errors are {"error":"..."} with 400 (bad request, a key needed), 403
 * (wrong key), 429 (too fast) or 500.
 */
#include "td.h"

size_t td_url_encode(char *out, size_t n, const char *in)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 15];
        }
    }
    if (n) out[o] = 0;
    return o;
}

size_t td_json_escape(char *out, size_t n, const char *in)
{
    static const char HEX[] = "0123456789abcdef";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < n; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if (c < 0x20) {
            out[o++] = '\\';
            out[o++] = 'u';
            out[o++] = '0';
            out[o++] = '0';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 15];
        } else {
            out[o++] = (char)c;
        }
    }
    if (n) out[o] = 0;
    return o;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* the text right after "key": (whitespace skipped), or NULL */
static const char *value_of(const char *json, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%.40s\"", key);
    size_t pl = strlen(pat);
    for (const char *p = strstr(json, pat); p; p = strstr(p + pl, pat)) {
        const char *q = p + pl;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        return q;
    }
    return NULL;
}

bool td_json_str(const char *json, const char *key, char *out, size_t n)
{
    const char *q = value_of(json, key);
    if (!q || *q != '"' || !n) return false;
    q++;
    size_t o = 0;
    while (*q && *q != '"' && o + 5 < n) {
        if (*q != '\\') {
            out[o++] = *q++;
            continue;
        }
        q++;
        char c = *q++;
        switch (c) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
            uint32_t cp = 0;
            for (int i = 0; i < 4; i++) {
                int h = hexv(q[i]);
                if (h < 0) return false;
                cp = cp * 16 + (uint32_t)h;
            }
            q += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && q[0] == '\\' && q[1] == 'u') {
                uint32_t lo = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hexv(q[2 + i]);
                    if (h < 0) return false;
                    lo = lo * 16 + (uint32_t)h;
                }
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    q += 6;
                }
            }
            o += put_utf8(out + o, cp);
            break;
        }
        case 0: q--; break;
        default: out[o++] = c; break;
        }
    }
    out[o] = 0;
    return true;
}

bool td_json_num(const char *json, const char *key, double *out)
{
    const char *q = value_of(json, key);
    if (!q) return false;
    if (*q == '"') q++;             /* MyMemory sends some numbers as strings */
    if (!((*q >= '0' && *q <= '9') || *q == '-')) return false;
    *out = (double)strtol(q, NULL, 10);
    return true;
}

static bool json_true(const char *json, const char *key)
{
    const char *q = value_of(json, key);
    return q && !strncmp(q, "true", 4);
}

/* &#39; &quot; &amp; &lt; &gt; as MyMemory leaves them, in place */
static void html_entities(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        if (*p == '&') {
            if (!strncmp(p, "&amp;", 5)) { *o++ = '&'; p += 5; continue; }
            if (!strncmp(p, "&quot;", 6)) { *o++ = '"'; p += 6; continue; }
            if (!strncmp(p, "&lt;", 4)) { *o++ = '<'; p += 4; continue; }
            if (!strncmp(p, "&gt;", 4)) { *o++ = '>'; p += 4; continue; }
            if (!strncmp(p, "&#39;", 5)) { *o++ = '\''; p += 5; continue; }
        }
        *o++ = *p++;
    }
    *o = 0;
}

bool td_online_request(int engine, const char *text, const char *src, const char *dst, char *url,
                       size_t url_n, const char **method, char *body, size_t body_n, char *headers,
                       size_t headers_n)
{
    td_cfg_t *c = td_cfg();
    headers[0] = 0;
    body[0] = 0;
    if (engine == TD_E_ONLINE) {
        char *q = (char *)malloc(strlen(text) * 3 + 4);
        if (!q) return false;
        td_url_encode(q, strlen(text) * 3 + 4, text);
        char em[160] = "";
        if (c->email[0]) {
            char e[120];
            td_url_encode(e, sizeof e, c->email);
            snprintf(em, sizeof em, "&de=%s", e);
        }
        int n = snprintf(url, url_n, "%s/get?q=%s&langpair=%s%%7C%s%s", c->mm_url, q,
                         strcmp(src, "auto") ? src : "autodetect", dst, em);
        free(q);
        *method = "GET";
        return n > 0 && (size_t)n < url_n;
    }
    if (engine == TD_E_SERVER) {
        if (!c->server[0]) return false;
        snprintf(url, url_n, "%s/translate", c->server);
        size_t el = strlen(text) * 6 + 8;
        char *e = (char *)malloc(el);
        if (!e) return false;
        td_json_escape(e, el, text);
        char key[200] = "";
        if (c->server_key[0]) {
            char k[160];
            td_json_escape(k, sizeof k, c->server_key);
            snprintf(key, sizeof key, ",\"api_key\":\"%s\"", k);
        }
        int n = snprintf(body, body_n, "{\"q\":\"%s\",\"source\":\"%s\",\"target\":\"%s\",\"format\":\"text\"%s}",
                         e, src, dst, key);
        free(e);
        *method = "POST";
        return n > 0 && (size_t)n < body_n;
    }
    return false;
}

td_err_t td_online_parse(int engine, int status, const char *body, char *out, size_t out_n, char *detected,
                         char *msg, size_t msg_n)
{
    msg[0] = 0;
    if (detected) detected[0] = 0;
    if (!body) return TD_ERR_NET;
    if (engine == TD_E_ONLINE) {
        double rs = 0;
        bool have = td_json_num(body, "responseStatus", &rs);
        bool quota = json_true(body, "quotaFinished");
        if (!td_json_str(body, "translatedText", out, out_n)) {
            snprintf(msg, msg_n, "%s", status != 200 ? "HTTP" : "?");
            return status == 429 ? TD_ERR_QUOTA : TD_ERR_SERVER;
        }
        if (quota || (have && (int)rs == 429) || !strncmp(out, "MYMEMORY WARNING", 16)) {
            td_scpy(msg, msg_n, out);
            out[0] = 0;
            return TD_ERR_QUOTA;
        }
        if ((have && (int)rs != 200) || status != 200) {
            td_scpy(msg, msg_n, out);
            out[0] = 0;
            return TD_ERR_SERVER;
        }
        html_entities(out);
        /* with autodetect, the language of the best match: "es-ES" */
        if (detected) {
            char s[16];
            if (td_json_str(body, "source", s, sizeof s) && s[0]) {
                detected[0] = (char)(s[0] | 0x20);
                detected[1] = (char)(s[1] | 0x20);
                detected[2] = 0;
            }
        }
        return TD_OK;
    }
    /* LibreTranslate */
    if (status != 200) {
        if (!td_json_str(body, "error", msg, msg_n)) snprintf(msg, msg_n, "HTTP %d", status);
        if (status == 403 || strstr(msg, "API key") || strstr(msg, "api key")) return TD_ERR_KEY;
        if (status == 429) return TD_ERR_QUOTA;
        return TD_ERR_SERVER;
    }
    if (!td_json_str(body, "translatedText", out, out_n)) {
        td_scpy(msg, msg_n, "?");
        return TD_ERR_SERVER;
    }
    if (detected) {
        char s[16];
        if (td_json_str(body, "language", s, sizeof s)) td_scpy(detected, 8, s);
    }
    return TD_OK;
}
