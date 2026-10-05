/*
 * P4OS - Infrarrojo: the protocols.
 *
 * Every frame is a list of durations in microseconds, mark first (a mark is
 * the carrier on; the receiver has already taken the carrier away). Each
 * decoder looks at the first frame of a capture - up to the first space of
 * IR_GAP_US - and either recognises it completely, checksums included, or
 * says no; what nobody recognises stays raw, which is how an air
 * conditioner's frame of 100-odd bits is kept.
 *
 * The timings are the usual ones (the same IRremote and Flipper use):
 *
 *   NEC        9000/4500 header, 32 bits by pulse distance, 560 mark, 560 or
 *              1690 space, LSB first: address, ~address, command, ~command.
 *              Its extended form has a 16-bit address; a held key sends
 *              9000/2250/560 every 108 ms.
 *   Samsung    as NEC with a 4500/4500 header and the address twice.
 *   Sony       2400 header mark, 600 spaces, 1200 or 600 marks for one and
 *              zero; 12, 15 or 20 bits LSB first, the 7-bit command first.
 *              40 kHz; three frames, one every 45 ms.
 *   JVC        8400/4200, 16 bits (address, command); repeats without header.
 *   Kaseikyo   3456/1728, 48 bits: a vendor (Panasonic is 0x2002) and its
 *              parity nibble, a 12-bit address, the command, an XOR.
 *              Panasonic's carrier is 36.7 kHz.
 *   RC5        Manchester, 889 us halves, 14 bits: two start bits (the
 *              second the command's seventh bit, inverted), the toggle, 5 of
 *              address, 6 of command. 36 kHz.
 *   RC6        mode 0: 2666/889 leader, a start bit, three mode bits, the
 *              toggle at twice the length, 8 of address, 8 of command;
 *              444 us halves, a one is mark then space. 36 kHz.
 */
#include "ir.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

static const ir_proto_t PROTOS[] = {
    { "NEC",       "NEC",               38000, 108, 1,   0xFF,     0xFF },
    { "NECext",    "NEC extendido",     38000, 108, 1,   0xFFFF,   0xFF },
    { "NEC32",     "NEC 32 bits",       38000, 108, 1,   0xFFFF,   0xFFFF },
    { "Samsung",   "Samsung",           38000, 108, 1,   0xFFFF,   0xFF },
    { "Sony12",    "Sony SIRC 12",      40000,  45, 3,   0x1F,     0x7F },
    { "Sony15",    "Sony SIRC 15",      40000,  45, 3,   0xFF,     0x7F },
    { "Sony20",    "Sony SIRC 20",      40000,  45, 3,   0x1FFF,   0x7F },
    { "JVC",       "JVC",               38000,  55, 1,   0xFF,     0xFF },
    { "Panasonic", "Panasonic",         36700, 130, 1,   0xFFF,    0xFF },
    { "Kaseikyo",  "Kaseikyo",          37000, 130, 1,   0xFFF,    0xFF },
    { "RC5",       "Philips RC5",       36000, 114, 1,   0x1F,     0x7F },
    { "RC6",       "Philips RC6",       36000, 107, 1,   0xFF,     0xFF },
};
#define NPROTO ((int)(sizeof PROTOS / sizeof PROTOS[0]))

int ir_proto_count(void) { return NPROTO; }
const ir_proto_t *ir_proto_at(int i) { return (i >= 0 && i < NPROTO) ? &PROTOS[i] : NULL; }

const ir_proto_t *ir_proto_find(const char *name)
{
    if (!name || !*name) return NULL;
    for (int i = 0; i < NPROTO; i++) if (!strcasecmp(PROTOS[i].name, name)) return &PROTOS[i];
    return NULL;
}

/* A receiver stretches marks and shortens spaces by ~100 us; a quarter of
 * the length plus that is what the decoders forgive. */
static bool near(uint32_t v, uint32_t ref)
{
    uint32_t tol = ref / 4 + 120;
    return v + tol >= ref && v <= ref + tol;
}

int ir_frame_len(const uint32_t *d, int n)
{
    for (int i = 1; i < n; i += 2) if (d[i] >= IR_GAP_US) return i;
    return n;
}

uint32_t ir_total_us(const uint32_t *d, int n)
{
    uint32_t t = 0;
    for (int i = 0; i < n; i++) t += d[i];
    return t;
}

/* Pulse distance: bits from d[at], as many as the pattern holds, LSB first
 * into bytes. Returns the bit count. */
static int pd_bits(const uint32_t *d, int n, int at, uint32_t mark, uint32_t s0, uint32_t s1,
                   uint8_t *bytes, int max_bits)
{
    int b = 0;
    if (bytes) memset(bytes, 0, (size_t)(max_bits + 7) / 8);
    while (at + 1 < n && b < max_bits && near(d[at], mark)) {
        uint32_t s = d[at + 1];
        int bit;
        if (near(s, s0)) bit = 0;
        else if (near(s, s1)) bit = 1;
        else break;
        if (bytes && bit) bytes[b / 8] |= (uint8_t)(1u << (b % 8));
        b++;
        at += 2;
    }
    return b;
}

static void set_code(ir_code_t *c, const char *p, uint32_t addr, uint32_t cmd, int bits)
{
    memset(c, 0, sizeof *c);
    ir_copy(c->proto, sizeof c->proto, p);
    c->addr = addr;
    c->cmd = cmd;
    c->bits = (uint8_t)bits;
}

static bool dec_nec(const uint32_t *d, int n, ir_code_t *c)
{
    if (n >= 3 && n <= 4 && near(d[0], 9000) && near(d[1], 2250) && near(d[2], 560)) {
        set_code(c, "NEC", 0, 0, 0);
        c->repeat = true;
        return true;
    }
    bool samsung = near(d[0], 4500) && near(d[1], 4500);
    if (!samsung && !(near(d[0], 9000) && near(d[1], 4500))) return false;
    uint8_t b[4];
    if (n < 67 || pd_bits(d, n, 2, 560, 560, 1690, b, 32) != 32 || !near(d[66], 560)) return false;
    if (samsung) {
        if ((uint8_t)(b[2] ^ b[3]) != 0xFF) return false;
        set_code(c, "Samsung", b[0] | b[1] << 8, b[2], 32);
        return true;
    }
    if ((uint8_t)(b[2] ^ b[3]) != 0xFF) {
        set_code(c, "NEC32", b[0] | b[1] << 8, b[2] | b[3] << 8, 32);
    } else if ((uint8_t)(b[0] ^ b[1]) != 0xFF) {
        set_code(c, "NECext", b[0] | b[1] << 8, b[2], 32);
    } else {
        set_code(c, "NEC", b[0], b[2], 32);
    }
    return true;
}

static bool dec_sony(const uint32_t *d, int n, ir_code_t *c)
{
    if (n < 25 || !near(d[0], 2400) || !near(d[1], 600)) return false;
    uint32_t v = 0;
    int bits = 0;
    for (int i = 2; i < n && bits < 20; i += 2) {
        if (near(d[i], 1200)) v |= 1u << bits;
        else if (!near(d[i], 600)) return false;
        bits++;
        if (i + 1 >= n) break;
        if (!near(d[i + 1], 600)) { if (d[i + 1] < 900) return false; break; }
    }
    if (bits != 12 && bits != 15 && bits != 20) return false;
    char p[8];
    snprintf(p, sizeof p, "Sony%d", bits);
    set_code(c, p, v >> 7, v & 0x7F, bits);
    return true;
}

static bool dec_jvc(const uint32_t *d, int n, ir_code_t *c)
{
    if (n < 35 || !near(d[0], 8400) || !near(d[1], 4200)) return false;
    uint8_t b[2];
    if (pd_bits(d, n, 2, 526, 526, 1574, b, 16) != 16) return false;
    set_code(c, "JVC", b[0], b[1], 16);
    return true;
}

static uint8_t vendor_parity(uint16_t v)
{
    uint8_t x = (uint8_t)(v ^ v >> 8);
    return (uint8_t)((x ^ x >> 4) & 0xF);
}

static bool dec_kaseikyo(const uint32_t *d, int n, ir_code_t *c)
{
    if (n < 99 || !near(d[0], 3456) || !near(d[1], 1728)) return false;
    uint8_t b[6];
    if (pd_bits(d, n, 2, 432, 432, 1296, b, 48) != 48) return false;
    uint16_t vendor = (uint16_t)(b[0] | b[1] << 8);
    if ((b[2] & 0xF) != vendor_parity(vendor)) return false;
    if ((uint8_t)(b[2] ^ b[3] ^ b[4]) != b[5]) return false;
    uint32_t addr = (uint32_t)(b[2] >> 4) | (uint32_t)b[3] << 4;
    set_code(c, vendor == 0x2002 ? "Panasonic" : "Kaseikyo", addr, b[4], 48);
    c->extra = vendor;
    return true;
}

/* Manchester: the durations as a row of half-bit levels (1 mark, 0 space) */
static int halves(const uint32_t *d, int n, int from, uint32_t t, uint8_t *lv, int max, int start_len)
{
    int k = start_len;
    for (int i = from; i < n; i++) {
        uint32_t units = (d[i] + t / 2) / t;
        if (units < 1 || units > 6) return -1;
        if (d[i] + t / 2 < units * t - t / 3 || d[i] > units * t + t / 2) return -1;
        for (uint32_t u = 0; u < units; u++) {
            if (k >= max) return -1;
            lv[k++] = (i & 1) ? 0 : 1;
        }
    }
    return k;
}

static bool dec_rc5(const uint32_t *d, int n, ir_code_t *c)
{
    if (n < 10 || n > 28 || !(near(d[0], 889) || near(d[0], 1778))) return false;
    uint8_t lv[32];
    lv[0] = 0;  /* the first half of the first start bit is a space */
    int k = halves(d, n, 0, 889, lv, 32, 1);
    if (k < 0) return false;
    if (k & 1) lv[k++] = 0;
    if (k != 28) return false;
    uint32_t v = 0;
    for (int i = 0; i < 14; i++) {
        uint8_t a = lv[2 * i], b = lv[2 * i + 1];
        if (a == b) return false;
        v = v << 1 | (b ? 1u : 0u);     /* space then mark is a one */
    }
    if (!(v & 0x2000)) return false;
    uint32_t cmd = (v & 0x3F) | ((v & 0x1000) ? 0 : 0x40);
    set_code(c, "RC5", v >> 6 & 0x1F, cmd, 14);
    c->extra = v >> 11 & 1;     /* the toggle, for showing */
    return true;
}

static bool dec_rc6(const uint32_t *d, int n, ir_code_t *c)
{
    if (n < 12 || !near(d[0], 2666) || !near(d[1], 889)) return false;
    uint8_t lv[80];
    int k = halves(d, n, 2, 444, lv, 80, 0);
    if (k < 0) return false;
    if (k & 1) lv[k++] = 0;
    /* start(2) mode(6) trailer(4) data(32) */
    if (k < 44) return false;
    if (lv[0] != 1 || lv[1] != 0) return false;
    int mode = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t a = lv[2 + 2 * i], b = lv[3 + 2 * i];
        if (a == b) return false;
        mode = mode << 1 | a;
    }
    if (mode != 0) return false;
    if (lv[8] != lv[9] || lv[10] != lv[11] || lv[8] == lv[10]) return false;
    uint32_t v = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t a = lv[12 + 2 * i], b = lv[13 + 2 * i];
        if (a == b) return false;
        v = v << 1 | a;                 /* mark then space is a one */
    }
    set_code(c, "RC6", v >> 8, v & 0xFF, 20);
    c->extra = lv[8];
    return true;
}

bool ir_decode(const uint32_t *d, int n, ir_code_t *out)
{
    n = ir_frame_len(d, n);
    memset(out, 0, sizeof *out);
    if (n < 3) return false;
    /* RC6 before Sony: its 2666/889 leader is within Sony's tolerance, and
     * its Manchester check is the stricter of the two */
    return dec_nec(d, n, out) || dec_rc6(d, n, out) || dec_sony(d, n, out) || dec_jvc(d, n, out) ||
           dec_kaseikyo(d, n, out) || dec_rc5(d, n, out);
}

/* ------------------------------------------------------------------ encode */

typedef struct {
    uint32_t *o;
    int n, max;
} em_t;

static void em(em_t *e, uint32_t v)
{
    if (e->n < e->max) e->o[e->n++] = v;
    else e->n = e->max + 1;
}

static void em_pd(em_t *e, const uint8_t *bytes, int bits, uint32_t mark, uint32_t s0, uint32_t s1)
{
    for (int b = 0; b < bits; b++) {
        em(e, mark);
        em(e, (bytes[b / 8] >> (b % 8) & 1) ? s1 : s0);
    }
}

/* Manchester out of half levels, merging equal neighbours */
static void em_halves(em_t *e, const uint8_t *lv, int k, uint32_t t, bool skip_leading_space)
{
    int i = 0;
    if (skip_leading_space) while (i < k && !lv[i]) i++;
    while (i < k) {
        int j = i;
        while (j < k && lv[j] == lv[i]) j++;
        if (lv[i] || j < k) em(e, (uint32_t)(j - i) * t);   /* no trailing space */
        i = j;
    }
}

int ir_encode(const ir_code_t *c, bool repeat, uint8_t toggle, uint32_t *out, int max)
{
    em_t e = { out, 0, max };
    const char *p = c->proto;
    if (!strcmp(p, "NEC") || !strcmp(p, "NECext") || !strcmp(p, "NEC32") || !strcmp(p, "Samsung")) {
        bool samsung = !strcmp(p, "Samsung");
        if (repeat && !samsung) {
            em(&e, 9000); em(&e, 2250); em(&e, 560);
            return e.n;
        }
        uint8_t b[4];
        if (!strcmp(p, "NEC")) { b[0] = (uint8_t)c->addr; b[1] = (uint8_t)~c->addr; }
        else { b[0] = (uint8_t)c->addr; b[1] = (uint8_t)(c->addr >> 8); }
        if (!strcmp(p, "NEC32")) { b[2] = (uint8_t)c->cmd; b[3] = (uint8_t)(c->cmd >> 8); }
        else { b[2] = (uint8_t)c->cmd; b[3] = (uint8_t)~c->cmd; }
        em(&e, samsung ? 4500 : 9000); em(&e, 4500);
        em_pd(&e, b, 32, 560, 560, 1690);
        em(&e, 560);
    } else if (!strncmp(p, "Sony", 4)) {
        int bits = (int)c->bits;
        if (!bits) bits = !strcmp(p, "Sony15") ? 15 : !strcmp(p, "Sony20") ? 20 : 12;
        uint32_t v = (c->cmd & 0x7F) | c->addr << 7;
        /* the bits are in the marks; the frame ends on the last one */
        em(&e, 2400);
        em(&e, 600);
        for (int i = 0; i < bits; i++) {
            em(&e, (v >> i & 1) ? 1200 : 600);
            if (i + 1 < bits) em(&e, 600);
        }
    } else if (!strcmp(p, "JVC")) {
        uint8_t b[2] = { (uint8_t)c->addr, (uint8_t)c->cmd };
        if (!repeat) { em(&e, 8400); em(&e, 4200); }
        em_pd(&e, b, 16, 526, 526, 1574);
        em(&e, 526);
    } else if (!strcmp(p, "Panasonic") || !strcmp(p, "Kaseikyo")) {
        uint16_t vendor = !strcmp(p, "Panasonic") ? 0x2002 : (uint16_t)c->extra;
        uint8_t b[6];
        b[0] = (uint8_t)vendor;
        b[1] = (uint8_t)(vendor >> 8);
        b[2] = (uint8_t)(vendor_parity(vendor) | (c->addr & 0xF) << 4);
        b[3] = (uint8_t)(c->addr >> 4);
        b[4] = (uint8_t)c->cmd;
        b[5] = (uint8_t)(b[2] ^ b[3] ^ b[4]);
        em(&e, 3456); em(&e, 1728);
        em_pd(&e, b, 48, 432, 432, 1296);
        em(&e, 432);
    } else if (!strcmp(p, "RC5")) {
        uint32_t v = 1u << 13 | ((c->cmd & 0x40) ? 0 : 1u << 12) | (uint32_t)(toggle & 1) << 11 |
                     (c->addr & 0x1F) << 6 | (c->cmd & 0x3F);
        uint8_t lv[28];
        for (int i = 0; i < 14; i++) {
            int bit = v >> (13 - i) & 1;
            lv[2 * i] = (uint8_t)!bit;
            lv[2 * i + 1] = (uint8_t)bit;
        }
        em_halves(&e, lv, 28, 889, true);
    } else if (!strcmp(p, "RC6")) {
        uint8_t lv[44];
        int k = 0;
        lv[k++] = 1; lv[k++] = 0;                       /* start */
        for (int i = 0; i < 3; i++) { lv[k++] = 0; lv[k++] = 1; }    /* mode 0 */
        uint8_t t = toggle & 1;
        lv[k++] = t; lv[k++] = t; lv[k++] = !t; lv[k++] = !t;       /* trailer */
        uint32_t v = (c->addr & 0xFF) << 8 | (c->cmd & 0xFF);
        for (int i = 15; i >= 0; i--) {
            int bit = v >> i & 1;
            lv[k++] = (uint8_t)bit;
            lv[k++] = (uint8_t)!bit;
        }
        em(&e, 2666); em(&e, 889);
        em_halves(&e, lv, k, 444, false);
    } else {
        return 0;
    }
    return e.n <= max ? e.n : 0;
}

/* ---------------------------------------------------------------- describe */

static void ms(char *o, size_t n, uint32_t us)
{
    snprintf(o, n, "%u,%u", (unsigned)(us / 1000), (unsigned)(us % 1000 / 100));
}

void ir_describe(const uint32_t *d, int n, char *out, size_t len)
{
    int f = ir_frame_len(d, n);
    int frames = 1;
    for (int i = 1; i < n; i += 2) if (d[i] >= IR_GAP_US && i + 1 < n) frames++;
    char total[16];
    ms(total, sizeof total, ir_total_us(d, n));
    if (f < 4) {
        snprintf(out, len, _("%d duraciones, %s ms"), n, total);
        return;
    }
    /* a header is a first pair well above the rest */
    uint32_t min_mark = 0xFFFFFFFF;
    for (int i = 2; i < f; i += 2) if (d[i] < min_mark) min_mark = d[i];
    bool header = d[0] > min_mark * 3;
    int at = header ? 2 : 0;
    /* the first two kinds of space after the header */
    uint32_t sa = 0, sb = 0;
    for (int i = at + 1; i < f; i += 2) {
        if ((sa && near(d[i], sa)) || (sb && near(d[i], sb))) continue;
        if (!sa) sa = d[i];
        else if (!sb) sb = d[i];
    }
    uint32_t s0 = sa < sb || !sb ? sa : sb, s1 = sa < sb || !sb ? sb : sa;
    uint8_t bytes[32];
    int bits = s1 ? pd_bits(d, f, at, min_mark, s0, s1, bytes, 256) : 0;
    char hdr[40] = "";
    if (header) {
        char a[12], b[12];
        ms(a, sizeof a, d[0]);
        ms(b, sizeof b, d[1]);
        snprintf(hdr, sizeof hdr, _("cabecera %s + %s ms, "), a, b);
    }
    if (bits >= 8) {
        char hex[3 * 16 + 4] = "";
        int nb = (bits + 7) / 8, shown = nb > 16 ? 16 : nb;
        for (int i = 0; i < shown; i++) {
            size_t l = strlen(hex);
            snprintf(hex + l, sizeof hex - l, "%02X ", bytes[i]);
        }
        if (nb > shown) { size_t l = strlen(hex); snprintf(hex + l, sizeof hex - l, "…"); }
        snprintf(out, len, _("%s%d bits por distancia de pulsos: %s"), hdr, bits, hex);
    } else {
        snprintf(out, len, _("%s%d duraciones, sin un patrón conocido"), hdr, f);
    }
    if (frames > 1) {
        size_t l = strlen(out);
        snprintf(out + l, len - l, _(" · %d tramas, %s ms"), frames, total);
    } else {
        size_t l = strlen(out);
        snprintf(out + l, len - l, " · %s ms", total);
    }
}

void ir_code_text(const ir_code_t *c, char *out, size_t len)
{
    const ir_proto_t *p = ir_proto_find(c->proto);
    if (!p) { snprintf(out, len, "%s", _("crudo")); return; }
    if (c->repeat) { snprintf(out, len, _("%s, repetición"), p->label); return; }
    int aw = p->addr_max > 0xFFF ? 4 : p->addr_max > 0xFF ? 3 : 2;
    int cw = p->cmd_max > 0xFF ? 4 : 2;
    if (!strcmp(c->proto, "Kaseikyo"))
        snprintf(out, len, _("%s  fab. 0x%04X  dir 0x%0*X  cmd 0x%0*X"), p->label, (unsigned)c->extra,
                 aw, (unsigned)c->addr, cw, (unsigned)c->cmd);
    else
        snprintf(out, len, _("%s  dir 0x%0*X  cmd 0x%0*X"), p->label, aw, (unsigned)c->addr, cw, (unsigned)c->cmd);
}

float ir_similarity(const uint32_t *a, int na, const uint32_t *b, int nb)
{
    if (na <= 0 || nb <= 0) return 0;
    int n = na < nb ? na : nb, longer = na > nb ? na : nb, hits = 0;
    for (int i = 0; i < n; i++) {
        uint32_t x = a[i] > 65535 ? 65535 : a[i], y = b[i] > 65535 ? 65535 : b[i];
        uint32_t big = x > y ? x : y, diff = x > y ? x - y : y - x;
        uint32_t tol = big / 5 > 60 ? big / 5 : 60;
        if (diff <= tol) hits++;
    }
    return (float)hits / (float)longer;
}
