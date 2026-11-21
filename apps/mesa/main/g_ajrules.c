/*
 * Juegos de mesa - chess: the rules (see g_ajedrez.h). Moves, making and
 * unmaking them, check, repetitions, SAN and FEN. Checked against the
 * usual perft counts by tools/harness.c.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "g_ajedrez.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const int8_t KN[8] = { 33, 31, 18, 14, -33, -31, -18, -14 };
static const int8_t KG[8] = { 16, -16, 1, -1, 17, 15, -15, -17 };
static const int8_t DIAG[4] = { 17, 15, -15, -17 };
static const int8_t ORTH[4] = { 16, -16, 1, -1 };

/* the keys: a piece on a square, the side, the castling rights, the file
 * of the en passant square */
static uint64_t Z[16][64], ZSIDE, ZCASTLE[16], ZEP[8];
/* the rights that stay after something leaves or reaches a square */
static uint8_t CMASK[128];
static bool s_tables;

static uint64_t xs(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

void mch_tables(void)
{
    if (s_tables) return;
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (int p = 0; p < 16; p++) {
        for (int i = 0; i < 64; i++) Z[p][i] = xs(&s);
    }
    ZSIDE = xs(&s);
    for (int i = 0; i < 16; i++) ZCASTLE[i] = xs(&s);
    for (int i = 0; i < 8; i++) ZEP[i] = xs(&s);
    memset(CMASK, 15, sizeof CMASK);
    CMASK[0x00] = (uint8_t)~CH_WQ & 15;
    CMASK[0x07] = (uint8_t)~CH_WK & 15;
    CMASK[0x04] = (uint8_t)~(CH_WK | CH_WQ) & 15;
    CMASK[0x70] = (uint8_t)~CH_BQ & 15;
    CMASK[0x77] = (uint8_t)~CH_BK & 15;
    CMASK[0x74] = (uint8_t)~(CH_BK | CH_BQ) & 15;
    s_tables = true;
}

static int mat_of(int type)
{
    return type == CH_N || type == CH_B ? 1 : type == CH_R ? 2 : type == CH_Q ? 4 : 0;
}

void mch_rekey(ch_pos_t *p)
{
    uint64_t k = 0;
    p->mat[0] = p->mat[1] = 0;
    for (int s = 0; s < 128; s++) {
        if (s & 0x88) continue;
        int pc = p->sq[s];
        if (!pc) continue;
        k ^= Z[pc][CH_SQ64(s)];
        p->mat[CH_COLOR(pc)] += (int16_t)mat_of(CH_TYPE(pc));
        if (CH_TYPE(pc) == CH_K) p->king[CH_COLOR(pc)] = (uint8_t)s;
    }
    if (p->side) k ^= ZSIDE;
    k ^= ZCASTLE[p->castle & 15];
    if (p->ep >= 0) k ^= ZEP[p->ep & 7];
    p->key = k;
    p->nh = 1;
    p->hist[0] = k;
}

void mch_start(ch_pos_t *p)
{
    mch_fen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

bool mch_fen(ch_pos_t *p, const char *fen)
{
    mch_tables();
    ch_pos_t q;
    memset(&q, 0, sizeof q);
    q.ep = -1;
    int r = 7, f = 0;
    const char *s = fen;
    while (*s == ' ') s++;
    for (; *s && *s != ' '; s++) {
        if (*s == '/') {
            if (f != 8) return false;
            r--;
            f = 0;
            continue;
        }
        if (*s >= '1' && *s <= '8') {
            f += *s - '0';
            if (f > 8) return false;
            continue;
        }
        const char *P = strchr("PNBRQKpnbrqk", *s);
        if (!P || f > 7 || r < 0) return false;
        int i = (int)(P - "PNBRQKpnbrqk");
        q.sq[r * 16 + f] = (uint8_t)((i % 6 + 1) | (i >= 6 ? CH_BLACK : 0));
        f++;
    }
    if (r != 0 || f != 8) return false;
    while (*s == ' ') s++;
    if (*s == 'b') q.side = 1;
    else if (*s != 'w') return false;
    s++;
    while (*s == ' ') s++;
    for (; *s && *s != ' '; s++) {
        if (*s == 'K') q.castle |= CH_WK;
        else if (*s == 'Q') q.castle |= CH_WQ;
        else if (*s == 'k') q.castle |= CH_BK;
        else if (*s == 'q') q.castle |= CH_BQ;
    }
    while (*s == ' ') s++;
    if (*s >= 'a' && *s <= 'h' && s[1] >= '1' && s[1] <= '8') {
        q.ep = (int8_t)((s[1] - '1') * 16 + (s[0] - 'a'));
        s += 2;
    } else if (*s == '-') {
        s++;
    }
    int half = 0, full = 1;
    if (sscanf(s, "%d %d", &half, &full) < 1) half = 0;
    q.half = (uint8_t)(half < 0 ? 0 : half > 200 ? 200 : half);
    q.full = (uint16_t)(full < 1 ? 1 : full);
    /* rights only where the king and the rook still are */
    if (q.sq[0x04] != CH_K) q.castle &= (uint8_t)~(CH_WK | CH_WQ);
    if (q.sq[0x07] != CH_R) q.castle &= (uint8_t)~CH_WK;
    if (q.sq[0x00] != CH_R) q.castle &= (uint8_t)~CH_WQ;
    if (q.sq[0x74] != (CH_K | CH_BLACK)) q.castle &= (uint8_t)~(CH_BK | CH_BQ);
    if (q.sq[0x77] != (CH_R | CH_BLACK)) q.castle &= (uint8_t)~CH_BK;
    if (q.sq[0x70] != (CH_R | CH_BLACK)) q.castle &= (uint8_t)~CH_BQ;
    int kings[2] = { 0, 0 };
    for (int i = 0; i < 128; i++) {
        if (!(i & 0x88) && CH_TYPE(q.sq[i]) == CH_K) kings[CH_COLOR(q.sq[i])]++;
        /* no pawns on the first or last rank */
        if (!(i & 0x88) && CH_TYPE(q.sq[i]) == CH_P && ((i >> 4) == 0 || (i >> 4) == 7)) return false;
    }
    if (kings[0] != 1 || kings[1] != 1) return false;
    mch_rekey(&q);
    /* the side that does not move may not be in check */
    if (mch_in_check(&q, q.side ^ 1)) return false;
    *p = q;
    return true;
}

bool mch_attacked(const ch_pos_t *p, int sq, int by)
{
    const uint8_t *b = p->sq;
    /* pawns: a white pawn attacks up, so it sits below */
    if (by == 0) {
        int a = sq - 15, c = sq - 17;
        if (!(a & 0x88) && b[a] == CH_P) return true;
        if (!(c & 0x88) && b[c] == CH_P) return true;
    } else {
        int a = sq + 15, c = sq + 17;
        if (!(a & 0x88) && b[a] == (CH_P | CH_BLACK)) return true;
        if (!(c & 0x88) && b[c] == (CH_P | CH_BLACK)) return true;
    }
    int col = by ? CH_BLACK : 0;
    for (int i = 0; i < 8; i++) {
        int t = sq + KN[i];
        if (!(t & 0x88) && b[t] == (CH_N | col)) return true;
        t = sq + KG[i];
        if (!(t & 0x88) && b[t] == (CH_K | col)) return true;
    }
    for (int i = 0; i < 4; i++) {
        for (int t = sq + DIAG[i]; !(t & 0x88); t += DIAG[i]) {
            int pc = b[t];
            if (!pc) continue;
            if ((pc & CH_BLACK) == col && (CH_TYPE(pc) == CH_B || CH_TYPE(pc) == CH_Q)) return true;
            break;
        }
        for (int t = sq + ORTH[i]; !(t & 0x88); t += ORTH[i]) {
            int pc = b[t];
            if (!pc) continue;
            if ((pc & CH_BLACK) == col && (CH_TYPE(pc) == CH_R || CH_TYPE(pc) == CH_Q)) return true;
            break;
        }
    }
    return false;
}

bool mch_in_check(const ch_pos_t *p, int side)
{
    return mch_attacked(p, p->king[side], side ^ 1);
}

static inline void add_promos(uint32_t *out, int *n, int f, int t, int fl)
{
    out[(*n)++] = CHMV(f, t, CH_Q, fl | CHM_PROMO);
    out[(*n)++] = CHMV(f, t, CH_N, fl | CHM_PROMO);
    out[(*n)++] = CHMV(f, t, CH_R, fl | CHM_PROMO);
    out[(*n)++] = CHMV(f, t, CH_B, fl | CHM_PROMO);
}

int mch_gen(const ch_pos_t *p, uint32_t *out, bool caps)
{
    const uint8_t *b = p->sq;
    int side = p->side, col = side ? CH_BLACK : 0, n = 0;
    int up = side ? -16 : 16, start = side ? 6 : 1, last = side ? 0 : 7;
    for (int s = 0; s < 128; s++) {
        if (s & 0x88) {
            s += 7;
            continue;
        }
        int pc = b[s];
        if (!pc || (pc & CH_BLACK) != col) continue;
        int ty = CH_TYPE(pc);
        if (ty == CH_P) {
            int t = s + up;
            if (!(t & 0x88) && !b[t]) {
                if ((t >> 4) == last) add_promos(out, &n, s, t, 0);
                else if (!caps) {
                    out[n++] = CHMV(s, t, 0, 0);
                    int t2 = t + up;
                    if ((s >> 4) == start && !b[t2]) out[n++] = CHMV(s, t2, 0, CHM_DOUBLE);
                }
            }
            for (int d = -1; d <= 1; d += 2) {
                int c = s + up + d;
                if (c & 0x88) continue;
                if (b[c] && (b[c] & CH_BLACK) != col) {
                    if ((c >> 4) == last) add_promos(out, &n, s, c, CHM_CAP);
                    else out[n++] = CHMV(s, c, 0, CHM_CAP);
                } else if (c == p->ep) {
                    out[n++] = CHMV(s, c, 0, CHM_CAP | CHM_EP);
                }
            }
            continue;
        }
        if (ty == CH_N || ty == CH_K) {
            const int8_t *o = ty == CH_N ? KN : KG;
            for (int i = 0; i < 8; i++) {
                int t = s + o[i];
                if (t & 0x88) continue;
                int q = b[t];
                if (!q) {
                    if (!caps) out[n++] = CHMV(s, t, 0, 0);
                } else if ((q & CH_BLACK) != col) {
                    out[n++] = CHMV(s, t, 0, CHM_CAP);
                }
            }
            continue;
        }
        for (int k = 0; k < 2; k++) {
            const int8_t *dirs = k == 0 ? DIAG : ORTH;
            if (k == 0 && ty == CH_R) continue;
            if (k == 1 && ty == CH_B) continue;
            for (int i = 0; i < 4; i++) {
                for (int t = s + dirs[i]; !(t & 0x88); t += dirs[i]) {
                    int q = b[t];
                    if (!q) {
                        if (!caps) out[n++] = CHMV(s, t, 0, 0);
                        continue;
                    }
                    if ((q & CH_BLACK) != col) out[n++] = CHMV(s, t, 0, CHM_CAP);
                    break;
                }
            }
        }
    }
    if (!caps) {
        int e = side ? 0x74 : 0x04, o = side ^ 1;
        int ks = side ? CH_BK : CH_WK, qs = side ? CH_BQ : CH_WQ;
        if ((p->castle & ks) && !b[e + 1] && !b[e + 2] && !mch_attacked(p, e, o) && !mch_attacked(p, e + 1, o) &&
            !mch_attacked(p, e + 2, o)) {
            out[n++] = CHMV(e, e + 2, 0, CHM_CASTLE);
        }
        if ((p->castle & qs) && !b[e - 1] && !b[e - 2] && !b[e - 3] && !mch_attacked(p, e, o) &&
            !mch_attacked(p, e - 1, o) && !mch_attacked(p, e - 2, o)) {
            out[n++] = CHMV(e, e - 2, 0, CHM_CASTLE);
        }
    }
    return n;
}

static void undo_board(ch_pos_t *p, uint32_t m, const ch_undo_t *u);

static inline void put(ch_pos_t *p, int s, int pc)
{
    p->sq[s] = (uint8_t)pc;
    p->key ^= Z[pc][CH_SQ64(s)];
}

static inline void take(ch_pos_t *p, int s)
{
    int pc = p->sq[s];
    p->key ^= Z[pc][CH_SQ64(s)];
    p->sq[s] = 0;
}

bool mch_make(ch_pos_t *p, uint32_t m, ch_undo_t *u)
{
    int f = CHMV_FROM(m), t = CHMV_TO(m), fl = CHMV_FL(m), side = p->side;
    int pc = p->sq[f];
    u->castle = p->castle;
    u->ep = p->ep;
    u->half = p->half;
    u->key = p->key;
    u->captured = 0;
    if (p->ep >= 0) p->key ^= ZEP[p->ep & 7];
    p->key ^= ZCASTLE[p->castle];
    p->ep = -1;
    if (fl & CHM_EP) {
        int c = t + (side ? 16 : -16);
        u->captured = p->sq[c];
        take(p, c);
    } else if (p->sq[t]) {
        u->captured = p->sq[t];
        p->mat[side ^ 1] -= (int16_t)mat_of(CH_TYPE(p->sq[t]));
        take(p, t);
    }
    take(p, f);
    if (fl & CHM_PROMO) {
        int np = CHMV_PROMO(m) | (side ? CH_BLACK : 0);
        put(p, t, np);
        p->mat[side] += (int16_t)mat_of(CHMV_PROMO(m));
    } else {
        put(p, t, pc);
    }
    if (CH_TYPE(pc) == CH_K) {
        p->king[side] = (uint8_t)t;
        if (fl & CHM_CASTLE) {
            int rf = t > f ? f + 3 : f - 4, rt = t > f ? f + 1 : f - 1;
            int r = p->sq[rf];
            take(p, rf);
            put(p, rt, r);
        }
    }
    if (fl & CHM_DOUBLE) {
        p->ep = (int8_t)((f + t) / 2);
        p->key ^= ZEP[p->ep & 7];
    }
    p->castle &= CMASK[f] & CMASK[t];
    p->key ^= ZCASTLE[p->castle];
    p->half = (uint8_t)(CH_TYPE(pc) == CH_P || u->captured ? 0 : (p->half < 250 ? p->half + 1 : 250));
    if (side) p->full++;
    p->side ^= 1;
    p->key ^= ZSIDE;
    if (mch_attacked(p, p->king[side], side ^ 1)) {
        undo_board(p, m, u);
        return false;
    }
    if (p->nh >= CH_HIST) {
        memmove(p->hist, p->hist + CH_HIST / 2, sizeof p->hist[0] * (CH_HIST - CH_HIST / 2));
        p->nh -= CH_HIST / 2;
    }
    p->hist[p->nh++] = p->key;
    return true;
}

static void undo_board(ch_pos_t *p, uint32_t m, const ch_undo_t *u)
{
    int f = CHMV_FROM(m), t = CHMV_TO(m), fl = CHMV_FL(m);
    p->side ^= 1;
    int side = p->side;
    int pc = p->sq[t];
    p->sq[t] = 0;
    if (fl & CHM_PROMO) {
        p->mat[side] -= (int16_t)mat_of(CH_TYPE(pc));
        pc = CH_P | (side ? CH_BLACK : 0);
    }
    p->sq[f] = (uint8_t)pc;
    if (CH_TYPE(pc) == CH_K) {
        p->king[side] = (uint8_t)f;
        if (fl & CHM_CASTLE) {
            int rf = t > f ? f + 3 : f - 4, rt = t > f ? f + 1 : f - 1;
            p->sq[rf] = p->sq[rt];
            p->sq[rt] = 0;
        }
    }
    if (u->captured) {
        if (fl & CHM_EP) {
            p->sq[t + (side ? 16 : -16)] = u->captured;
        } else {
            p->sq[t] = u->captured;
            p->mat[side ^ 1] += (int16_t)mat_of(CH_TYPE(u->captured));
        }
    }
    if (side) p->full--;
    p->castle = u->castle;
    p->ep = u->ep;
    p->half = u->half;
    p->key = u->key;
}

void mch_unmake(ch_pos_t *p, uint32_t m, const ch_undo_t *u)
{
    if (p->nh > 1) p->nh--;
    undo_board(p, m, u);
}

void mch_null(ch_pos_t *p, ch_undo_t *u)
{
    u->ep = p->ep;
    u->key = p->key;
    u->half = p->half;
    if (p->ep >= 0) p->key ^= ZEP[p->ep & 7];
    p->ep = -1;
    p->side ^= 1;
    p->key ^= ZSIDE;
    p->half = 0;    /* no repetition through a null move */
}

void mch_unnull(ch_pos_t *p, const ch_undo_t *u)
{
    p->side ^= 1;
    p->ep = u->ep;
    p->key = u->key;
    p->half = u->half;
}

int mch_repeats(const ch_pos_t *p)
{
    int n = 0;
    int lo = p->nh - 1 - p->half;
    if (lo < 0) lo = 0;
    for (int i = p->nh - 3; i >= lo; i -= 2) {
        if (p->hist[i] == p->key) n++;
    }
    return n;
}

bool mch_no_mating_material(const ch_pos_t *p)
{
    int minors[2] = { 0, 0 }, bish_col[2] = { -1, -1 };
    bool mixed = false;
    for (int s = 0; s < 128; s++) {
        if (s & 0x88) continue;
        int pc = p->sq[s], ty = CH_TYPE(pc);
        if (!pc || ty == CH_K) continue;
        if (ty == CH_P || ty == CH_R || ty == CH_Q) return false;
        int c = CH_COLOR(pc);
        minors[c]++;
        if (ty == CH_B) {
            int sc = ((s >> 4) + (s & 7)) & 1;
            if (bish_col[0] >= 0 && bish_col[0] != sc) mixed = true;
            if (bish_col[0] < 0) bish_col[0] = sc;
        } else {
            mixed = true;       /* a knight with anything else might mate */
        }
    }
    int total = minors[0] + minors[1];
    if (total <= 1) return true;
    /* only bishops, all on one colour of square */
    return !mixed;
}

int mch_legal(ch_pos_t *p, uint32_t *out)
{
    uint32_t tmp[256];
    int n = mch_gen(p, tmp, false), k = 0;
    for (int i = 0; i < n; i++) {
        ch_undo_t u;
        if (mch_make(p, tmp[i], &u)) {
            mch_unmake(p, tmp[i], &u);
            out[k++] = tmp[i];
        }
    }
    return k;
}

static void sqname(int s, char *o)
{
    o[0] = (char)('a' + (s & 7));
    o[1] = (char)('1' + (s >> 4));
}

void mch_san(ch_pos_t *p, uint32_t m, char *out)
{
    int f = CHMV_FROM(m), t = CHMV_TO(m), fl = CHMV_FL(m);
    int ty = CH_TYPE(p->sq[f]);
    char *o = out;
    if (fl & CHM_CASTLE) {
        const char *c = t > f ? "O-O" : "O-O-O";
        while (*c) *o++ = *c++;
    } else {
        if (ty != CH_P) {
            *o++ = " PNBRQK"[ty];
            uint32_t l[256];
            int n = mch_legal(p, l);
            bool other = false, same_file = false, same_rank = false;
            for (int i = 0; i < n; i++) {
                int f2 = CHMV_FROM(l[i]);
                if (f2 == f || CHMV_TO(l[i]) != t || CH_TYPE(p->sq[f2]) != ty) continue;
                other = true;
                if ((f2 & 7) == (f & 7)) same_file = true;
                if ((f2 >> 4) == (f >> 4)) same_rank = true;
            }
            if (other) {
                if (!same_file) *o++ = (char)('a' + (f & 7));
                else if (!same_rank) *o++ = (char)('1' + (f >> 4));
                else {
                    sqname(f, o);
                    o += 2;
                }
            }
        } else if (fl & CHM_CAP) {
            *o++ = (char)('a' + (f & 7));
        }
        if (fl & CHM_CAP) *o++ = 'x';
        sqname(t, o);
        o += 2;
        if (fl & CHM_PROMO) {
            *o++ = '=';
            *o++ = " PNBRQK"[CHMV_PROMO(m)];
        }
    }
    ch_undo_t u;
    if (mch_make(p, m, &u)) {
        if (mch_in_check(p, p->side)) {
            uint32_t l[256];
            *o++ = mch_legal(p, l) ? '+' : '#';
        }
        mch_unmake(p, m, &u);
    }
    *o = 0;
}

static void clean(const char *s, char *o, int n)
{
    int k = 0;
    for (; *s && k < n - 1; s++) {
        char c = *s;
        if (c == '+' || c == '#' || c == '!' || c == '?' || c == ' ') continue;
        if (c == '0') c = 'O';
        o[k++] = c;
    }
    o[k] = 0;
}

uint32_t mch_parse_san(ch_pos_t *p, const char *s)
{
    char want[16], have[16], san[16];
    clean(s, want, sizeof want);
    if (!want[0]) return 0;
    uint32_t l[256];
    int n = mch_legal(p, l);
    for (int i = 0; i < n; i++) {
        mch_san(p, l[i], san);
        clean(san, have, sizeof have);
        if (!strcmp(want, have)) return l[i];
        /* "exd6e.p." and promotions written without '=' */
        char nopromo[16];
        int k = 0;
        for (const char *q = have; *q && k < 15; q++) {
            if (*q != '=') nopromo[k++] = *q;
        }
        nopromo[k] = 0;
        if (!strcmp(want, nopromo)) return l[i];
    }
    /* long algebraic: e2e4, e7e8q */
    if (strlen(want) >= 4 && want[0] >= 'a' && want[0] <= 'h') {
        int f = (want[1] - '1') * 16 + (want[0] - 'a');
        int t = (want[3] - '1') * 16 + (want[2] - 'a');
        int pr = want[4] ? (int)(strchr(" pnbrqk", tolower((unsigned char)want[4])) ? strchr(" pnbrqk", tolower((unsigned char)want[4])) - " pnbrqk" : 0) : 0;
        for (int i = 0; i < n; i++) {
            if (CHMV_FROM(l[i]) == f && CHMV_TO(l[i]) == t && (!(CHMV_FL(l[i]) & CHM_PROMO) || CHMV_PROMO(l[i]) == (pr ? pr : CH_Q))) return l[i];
        }
    }
    return 0;
}

uint64_t mch_perft(ch_pos_t *p, int depth)
{
    uint32_t l[256];
    int n = mch_gen(p, l, false);
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        ch_undo_t u;
        if (!mch_make(p, l[i], &u)) continue;
        total += depth <= 1 ? 1 : mch_perft(p, depth - 1);
        mch_unmake(p, l[i], &u);
    }
    return total;
}
