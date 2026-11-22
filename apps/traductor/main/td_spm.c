/*
 * TRADUCTOR - SentencePiece unigram (see td_spm.h)
 */
#include "td_spm.h"
#include "td_pak.h"

#include <stdlib.h>
#include <string.h>

static uint32_t fnv(const char *p, int len)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h;
}

void td_spm_free(td_spm_t *s)
{
    td_free(s->off);
    td_free(s->len);
    td_free(s->type);
    td_free(s->score);
    td_free(s->pool);
    td_free(s->hash);
    memset(s, 0, sizeof *s);
}

int td_spm_init(td_spm_t *s, const uint8_t *data, uint32_t len)
{
    memset(s, 0, sizeof *s);
    if (len < 4) return -1;
    uint32_t n;
    memcpy(&n, data, 4);
    if (n == 0 || n > 100000) return -1;
    s->n = (int)n;
    s->off = (uint32_t *)td_malloc(n * 4);
    s->len = (uint8_t *)td_malloc(n);
    s->type = (uint8_t *)td_malloc(n);
    s->score = (float *)td_malloc(n * 4);
    s->pool = (char *)td_malloc(len);
    uint32_t hs = 1;
    while (hs < n * 2) hs <<= 1;
    s->hash = (int32_t *)td_malloc(hs * 4);
    if (!s->off || !s->len || !s->type || !s->score || !s->pool || !s->hash) {
        td_spm_free(s);
        return -1;
    }
    s->hmask = hs - 1;
    memset(s->hash, 0xff, hs * 4);
    uint32_t o = 4, pool = 0;
    float minsc = 0;
    s->unk = 0;
    s->eos = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (o + 6 > len) goto bad;
        memcpy(&s->score[i], data + o, 4);
        s->type[i] = data[o + 4];
        uint8_t l = data[o + 5];
        o += 6;
        if (o + l > len) goto bad;
        memcpy(s->pool + pool, data + o, l);
        s->off[i] = pool;
        s->len[i] = l;
        pool += l;
        o += l;
        if (s->type[i] == 2) s->unk = (int)i;
        if (s->type[i] == 3 && l == 4 && !memcmp(s->pool + s->off[i], "</s>", 4)) s->eos = (int)i;
        if (s->type[i] == 1 || s->type[i] == 4) {
            if (s->score[i] < minsc) minsc = s->score[i];
            if (l > s->maxlen) s->maxlen = l;
            uint32_t h = fnv(s->pool + s->off[i], l) & s->hmask;
            while (s->hash[h] >= 0) h = (h + 1) & s->hmask;
            s->hash[h] = (int32_t)i;
        }
    }
    s->unk_score = minsc - 10.0f;
    return 0;
bad:
    td_spm_free(s);
    return -1;
}

int td_spm_find(const td_spm_t *s, const char *p, int len)
{
    if (len <= 0 || len > s->maxlen) return -1;
    uint32_t h = fnv(p, len) & s->hmask;
    while (s->hash[h] >= 0) {
        int i = s->hash[h];
        if (s->len[i] == len && !memcmp(s->pool + s->off[i], p, (size_t)len)) return i;
        h = (h + 1) & s->hmask;
    }
    return -1;
}

static int u8len(uint8_t c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

#define SP "\xE2\x96\x81"   /* U+2581, the pieces' space */

int td_spm_encode(const td_spm_t *s, const char *text, int32_t *ids, int max)
{
    /* whitespace collapsed, a space in front, every space as U+2581 */
    size_t tl = strlen(text);
    char *t = (char *)malloc(tl * 3 + 4);
    if (!t) return 0;
    int n = 0;
    bool space = true;
    for (size_t i = 0; i < tl; i++) {
        uint8_t c = (uint8_t)text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            continue;
        }
        if (space) {
            memcpy(t + n, SP, 3);
            n += 3;
            space = false;
        }
        t[n++] = (char)c;
    }
    if (!n) {
        free(t);
        return 0;
    }
    float *best = (float *)malloc((size_t)(n + 1) * sizeof(float));
    int32_t *from = (int32_t *)malloc((size_t)(n + 1) * sizeof(int32_t));
    int32_t *piece = (int32_t *)malloc((size_t)(n + 1) * sizeof(int32_t));
    if (!best || !from || !piece) {
        free(t);
        free(best);
        free(from);
        free(piece);
        return 0;
    }
    for (int i = 0; i <= n; i++) best[i] = -1e30f;
    best[0] = 0;
    for (int i = 0; i < n; i = i + u8len((uint8_t)t[i])) {
        if (best[i] <= -1e29f) continue;
        int first = u8len((uint8_t)t[i]);
        if (i + first > n) first = n - i;
        bool single = false;
        for (int j = i + first; j <= n && j - i <= s->maxlen;) {
            int id = td_spm_find(s, t + i, j - i);
            if (id >= 0) {
                if (j == i + first) single = true;
                float v = best[i] + s->score[id];
                if (v > best[j]) {
                    best[j] = v;
                    from[j] = i;
                    piece[j] = id;
                }
            }
            if (j >= n) break;
            j += u8len((uint8_t)t[j]);
        }
        if (!single) {
            int j = i + first;
            float v = best[i] + s->unk_score;
            if (v > best[j]) {
                best[j] = v;
                from[j] = i;
                piece[j] = s->unk;
            }
        }
    }
    /* back from the end; unknowns in a row are one */
    int cnt = 0;
    for (int j = n; j > 0; j = from[j]) cnt++;
    int32_t *tmp = (int32_t *)malloc((size_t)cnt * sizeof(int32_t));
    int k = cnt, out = 0;
    if (tmp) {
        for (int j = n; j > 0; j = from[j]) tmp[--k] = piece[j];
        for (int i = 0; i < cnt && out < max; i++) {
            if (tmp[i] == s->unk && out && ids[out - 1] == s->unk) continue;
            ids[out++] = tmp[i];
        }
        free(tmp);
    }
    free(t);
    free(best);
    free(from);
    free(piece);
    return out;
}

int td_spm_decode(const td_spm_t *s, const int32_t *ids, int n, char *out, int size)
{
    int o = 0;
    for (int i = 0; i < n; i++) {
        int id = ids[i];
        if (id < 0 || id >= s->n) continue;
        if (s->type[id] == 3 || s->type[id] == 2) continue;
        const char *p = s->pool + s->off[id];
        int l = s->len[id];
        for (int k = 0; k < l;) {
            if (k + 3 <= l && !memcmp(p + k, SP, 3)) {
                if (o > 0 && o < size - 1) out[o++] = ' ';
                k += 3;
            } else {
                if (o < size - 1) out[o++] = p[k];
                k++;
            }
        }
    }
    if (size > 0) out[o < size ? o : size - 1] = 0;
    return o;
}
