/*
 * P4OS - Traductor: the pack's dictionary (WikDict, CC BY-SA 4.0).
 *
 * The entry "esen.dic" (tools/mkpack.py): u32 n, u32 0, u32 key_off[n]
 * sorted by key, then the records key\0 u32 body_off, then the bodies
 * head\0 "pos\tgloss\ttranslations" lines \0. Only the n offsets are read
 * into memory (240 KB for 60 000 words); a lookup reads its ~17 keys and
 * the body from the card.
 *
 * The key is the word in lowercase. A Spanish plural not found is tried
 * without its s or es; the English dictionary has the other forms of each
 * word as keys of their own ("houses", "ran").
 */
#include "td.h"
#include "td_pak.h"

static td_pak_t s_pak;
static char s_path[200], s_dir[5];
static const td_entry_t *s_e;
static uint32_t *s_off;
static uint32_t s_n;

void td_dict_close(void)
{
    if (s_path[0]) td_pak_close(&s_pak);
    td_free(s_off);
    s_off = NULL;
    s_n = 0;
    s_e = NULL;
    s_path[0] = s_dir[0] = 0;
}

static bool dict_open(const char *src, const char *dst)
{
    char dir[5];
    snprintf(dir, sizeof dir, "%.2s%.2s", src, dst);
    const td_pack_t *k = td_pack_dict(src, dst);
    if (!k) return false;
    if (s_off && !strcmp(s_path, k->path) && !strcmp(s_dir, dir)) return true;
    td_dict_close();
    if (!td_pak_open(&s_pak, k->path)) return false;
    td_scpy(s_path, sizeof s_path, k->path);
    char name[TD_NAME_LEN];
    snprintf(name, sizeof name, "%s.dic", dir);
    s_e = td_pak_find(&s_pak, name);
    uint32_t h[2];
    if (!s_e || s_e->type != TD_T_DICT || s_e->len < 8 || !td_pak_read(&s_pak, s_e->off, h, 8) ||
        h[0] == 0 || 8 + h[0] * 4 > s_e->len) {
        td_dict_close();
        return false;
    }
    s_off = (uint32_t *)td_malloc(h[0] * 4);
    if (!s_off || !td_pak_read(&s_pak, s_e->off + 8, s_off, h[0] * 4)) {
        td_dict_close();
        return false;
    }
    s_n = h[0];
    td_scpy(s_dir, sizeof s_dir, dir);
    return true;
}

/* lowercase ASCII and Latin-1 capitals, spaces collapsed, marks trimmed */
static void fold(const char *in, char *out, size_t n)
{
    size_t o = 0;
    bool sp = false;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 3 < n; p++) {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\n') {
            sp = o > 0;
            continue;
        }
        if (c == '?' || c == '!' || c == '.' || c == ',' || c == ';' || c == ':' || c == '"') continue;
        if (c == 0xC2 && (p[1] == 0xBF || p[1] == 0xA1)) {     /* ¿ ¡ */
            p++;
            continue;
        }
        if (sp) {
            out[o++] = ' ';
            sp = false;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {
            out[o++] = (char)c;
            out[o++] = (char)(p[1] + 0x20);
            p++;
            continue;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

/* the body's offset for the key, or 0 */
static uint32_t find(const char *key)
{
    uint32_t lo = 0, hi = s_n;
    char rec[72];
    size_t kl = strlen(key);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t at = s_e->off + s_off[mid];
        uint32_t want = (uint32_t)sizeof rec;
        if (s_off[mid] + want > s_e->len) want = s_e->len - s_off[mid];
        memset(rec, 0, sizeof rec);
        if (!td_pak_read(&s_pak, at, rec, want)) return 0;
        rec[sizeof rec - 1] = 0;
        int c = strncmp(rec, key, kl + 1);
        if (c == 0) {
            size_t l = strlen(rec);
            if (l + 5 > want) return 0;
            uint32_t b;
            memcpy(&b, rec + l + 1, 4);
            return b;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

int td_dict_lookup(const char *src, const char *dst, const char *word, char *out, size_t n)
{
    out[0] = 0;
    if (!dict_open(src, dst)) return 0;
    char key[64];
    fold(word, key, sizeof key);
    if (!key[0] || strlen(key) >= sizeof key - 4) return 0;
    uint32_t b = find(key);
    if (!b && !strcmp(src, "es")) {
        size_t l = strlen(key);
        if (l > 4 && !strcmp(key + l - 2, "es")) {
            key[l - 2] = 0;
            b = find(key);
            if (!b) key[l - 2] = 'e';
        }
        l = strlen(key);
        if (!b && l > 3 && key[l - 1] == 's') {
            key[l - 1] = 0;
            b = find(key);
        }
    }
    if (!b || b >= s_e->len) return 0;
    uint32_t want = (uint32_t)n - 1;
    if (b + want > s_e->len) want = s_e->len - b;
    if (!td_pak_read(&s_pak, s_e->off + b, out, want)) return 0;
    out[want] = 0;
    /* head\0lines\0 -> "head\nlines" */
    size_t hl = strlen(out);
    if (hl + 1 >= want) return 0;
    out[hl] = '\n';
    int lines = 1;
    for (char *p = out + hl + 1; *p; p++)
        if (*p == '\n') lines++;
    return lines;
}
