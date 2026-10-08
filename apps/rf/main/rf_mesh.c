/*
 * RF - LoRa packet decode and the Meshtastic frame (rf_mesh.h). A C port of
 * apps/rf/test/lora_demod.py + lora_dec.py; the algorithm and its traps are
 * explained there. Verified on the Mac (apps/rf/test/mesh_test.c) against a
 * recording made on the board: the same three text messages come out.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")      /* the .so is built -Os */
#endif

#include "rf_mesh.h"
#include "aes.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FFT_MAX   8192
#define NSYM_MAX  160           /* symbols demodulated after the SFD */

typedef struct { float re, im; } cpx;

static void *big(size_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

static long rnd(float x) { return (long)(x < 0 ? x - 0.5f : x + 0.5f); }

/* ---- FFT (radix-2) and the dechirp peaks -------------------------------- */

static void fft(cpx *a, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { cpx t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        cpx wl = { cosf(ang), sinf(ang) };
        for (int i = 0; i < n; i += len) {
            cpx w = { 1, 0 };
            for (int k = 0; k < len / 2; k++) {
                cpx u = a[i + k], v = a[i + k + len / 2];
                cpx t = { v.re * w.re - v.im * w.im, v.re * w.im + v.im * w.re };
                a[i + k] = (cpx){ u.re + t.re, u.im + t.im };
                a[i + k + len / 2] = (cpx){ u.re - t.re, u.im - t.im };
                float wr = w.re * wl.re - w.im * wl.im;
                w.im = w.re * wl.im + w.im * wl.re;
                w.re = wr;
            }
        }
    }
}

/* the reference chirp of a symbol: up = exp(j(pi k^2/N - pi k)), down = conj */
static cpx chirp_at(int k, int N, bool down)
{
    float ph = (float)(M_PI * (double)k * k / N - M_PI * k);
    return (cpx){ cosf(ph), down ? -sinf(ph) : sinf(ph) };
}

/* dechirp seg (N samples) with the opposite chirp, FFT at N*pad, into fb */
static void dechirp(const cpx *seg, int N, bool down, int pad, cpx *fb)
{
    int M = N * pad;
    for (int k = 0; k < N; k++) {
        /* down=true -> multiply by the down-chirp, as the Python peak(seg, dn) */
        cpx c = chirp_at(k, N, down);
        fb[k] = (cpx){ seg[k].re * c.re - seg[k].im * c.im, seg[k].re * c.im + seg[k].im * c.re };
    }
    for (int k = N; k < M; k++) fb[k] = (cpx){ 0, 0 };
    fft(fb, M);
}

/* integer peak bin (in 0..N) and its share of the energy */
static int ipeak(const cpx *seg, int N, bool down, cpx *fb, float *q)
{
    dechirp(seg, N, down, 1, fb);
    float best = -1, total = 0;
    int k = 0;
    for (int i = 0; i < N; i++) {
        float p = fb[i].re * fb[i].re + fb[i].im * fb[i].im;
        total += p;
        if (p > best) best = p, k = i;
    }
    if (q) *q = total > 0 ? best / total : 0;
    return k;
}

/* fractional peak, in bins (can be negative) */
static float fpeak(const cpx *seg, int N, bool down, cpx *fb)
{
    int pad = FFT_MAX / N;
    if (pad > 16) pad = 16;
    if (pad < 1) pad = 1;
    int M = N * pad;
    dechirp(seg, N, down, pad, fb);
    float best = -1;
    int k = 0;
    for (int i = 0; i < M; i++) {
        float p = fb[i].re * fb[i].re + fb[i].im * fb[i].im;
        if (p > best) best = p, k = i;
    }
    float f = (float)k / pad;
    return f < N / 2 ? f : f - N;
}

static int isz(int k, int N) { int m = ((k + N / 2) % N) - N / 2; return (m < 0 ? -m : m) <= 2; }

/* ---- baseband and resampling -------------------------------------------- */

/* mix x (len, at fs) down by fc and low-pass (97-tap windowed sinc), in place */
static void baseband(cpx *x, int len, float fc, float cutoff, float fs, cpx *tmp)
{
    for (int i = 0; i < len; i++) {
        float ph = (float)(-2 * M_PI * fc / fs * i);
        float c = cosf(ph), s = sinf(ph);
        tmp[i] = (cpx){ x[i].re * c - x[i].im * s, x[i].re * s + x[i].im * c };
    }
    float h[97], sum = 0;
    for (int t = 0; t < 97; t++) {
        float a = (t - 48.0f);
        float si = a == 0 ? 1.0f : sinf(2 * (float)M_PI * cutoff / fs * a) / (2 * (float)M_PI * cutoff / fs * a);
        h[t] = si * (0.54f - 0.46f * cosf(2 * (float)M_PI * t / 96));
        sum += h[t];
    }
    for (int t = 0; t < 97; t++) h[t] /= sum;
    for (int i = 0; i < len; i++) {
        float re = 0, im = 0;
        for (int t = 0; t < 97; t++) {
            int j = i - 48 + t;
            if (j >= 0 && j < len) { re += tmp[j].re * h[t]; im += tmp[j].im * h[t]; }
        }
        x[i] = (cpx){ re, im };
    }
}

/* linear interpolation of x (at fs, len samples) at sample position p */
static cpx interp(const cpx *x, int len, float p)
{
    int i = (int)p;
    if (i < 0) return x[0];
    if (i >= len - 1) return x[len - 1];
    float f = p - i;
    return (cpx){ x[i].re + (x[i + 1].re - x[i].re) * f, x[i].im + (x[i + 1].im - x[i].im) * f };
}

/* ---- sync -------------------------------------------------------------- */

/* coarse preamble: returns start s0, preamble length j, up-peak u, down-peak d */
static bool demod(const cpx *x, int len, int sf, cpx *fb, int *s0, int *jn, int *u, int *d)
{
    int N = 1 << sf, step = N / 8, best = -1;
    for (int s = 0; s + N <= len - N; s += step) {
        float q;
        ipeak(x + s, N, true, fb, &q);
        if (q <= 0.3f) continue;
        int lo = 1 << 30, hi = -(1 << 30), ok = 1;
        for (int jj = 0; jj < 8; jj++) {
            if (s + (jj + 1) * N > len) { ok = 0; break; }
            int k = ipeak(x + s + jj * N, N, true, fb, NULL);
            if (k < lo) lo = k;
            if (k > hi) hi = k;
        }
        if (ok && hi - lo <= 1) { best = s; break; }
    }
    if (best < 0) return false;
    int kpre = ipeak(x + best, N, true, fb, NULL);
    int j = 0;
    while (best + (j + 1) * N <= len) {
        int k = ipeak(x + best + j * N, N, true, fb, NULL);
        int m = ((k - kpre + N / 2) % N) - N / 2;
        if ((m < 0 ? -m : m) > 1) break;
        j++;
    }
    if (best + (j + 3) * N > len) return false;
    *s0 = best; *jn = j; *u = kpre;
    *d = ipeak(x + best + (j + 2) * N, N, false, fb, NULL);
    return true;
}

/* the CFO-free signal y, the start s of the first preamble symbol, and npre */
static bool sync_sig(cpx *x, int len, int sf, cpx *fb, cpx **yout, int *sout, int *npreout, float *cfo_out)
{
    int N = 1 << sf, s0, j, u, d;
    if (!demod(x, len, sf, fb, &s0, &j, &u, &d)) return false;
    int s = s0 - u;
    while (s < 0) s += N;
    while (s + N <= len && !isz(ipeak(x + s, N, true, fb, NULL), N)) s += N;
    while (s - N >= 0 && isz(ipeak(x + s - N, N, true, fb, NULL), N)) s -= N;
    int npre = 0;
    while (s + (npre + 1) * N <= len && isz(ipeak(x + s + npre * N, N, true, fb, NULL), N)) npre++;
    if (s + (npre + 3) * N > len) return false;
    int kd = ipeak(x + s + (npre + 2) * N, N, false, fb, NULL);
    float cfo = kd / 2.0f;
    if (cfo >= N / 4.0f) cfo -= N / 2.0f;
    if (cfo >= N / 4.0f) cfo -= N / 2.0f;
    cpx *y = big((size_t)len * sizeof(cpx));
    if (!y) return false;
    for (int i = 0; i < len; i++) {
        float ph = (float)(-2 * M_PI * cfo / N * i);
        float c = cosf(ph), si = sinf(ph);
        y[i] = (cpx){ x[i].re * c - x[i].im * si, x[i].re * si + x[i].im * c };
    }
    s = (int)rnd(s + cfo);
    *yout = y; *sout = s; *npreout = npre; *cfo_out = cfo;
    return true;
}

/* ---- the decoder bits (Gray, deinterleave, Hamming, whitening, CRC) ----- */

static int demap(int sym, int sf, bool reduced, int off, int gray)
{
    int N = 1 << sf, s = ((sym + off) % N + N) % N;
    if (reduced) s /= 4;
    if (gray == 1) s = s ^ (s >> 1);
    else if (gray == 2) { int g = s, b = 0; while (g) { b ^= g; g >>= 1; } s = b; }
    return s;
}

static int getbit(int v, int n, int i) { return (v >> (n - 1 - i)) & 1; }

static void deinterleave(const int *vals, int sf_app, int cw_len, int diag, int *out)
{
    int deint[8][8];
    memset(deint, 0, sizeof deint);
    for (int i = 0; i < cw_len; i++)
        for (int jx = 0; jx < sf_app; jx++) {
            int r = diag == 0 ? ((i - jx - 1) % sf_app + sf_app) % sf_app
                  : diag == 1 ? (i + jx + 1) % sf_app
                              : ((jx - i - 1) % sf_app + sf_app) % sf_app;
            deint[r][i] = getbit(vals[i], sf_app, jx);
        }
    for (int r = 0; r < sf_app; r++) {
        int v = 0;
        for (int i = 0; i < cw_len; i++) v = (v << 1) | deint[r][i];
        out[r] = v;
    }
}

static int nibble(int cw, int cw_len, int order)
{
    int b[8];
    for (int i = 0; i < cw_len; i++) b[i] = getbit(cw, cw_len, i);
    int v = 0;
    if (order == 0) for (int i = 3; i >= 0; i--) v = (v << 1) | b[i];
    else for (int i = 0; i < 4; i++) v = (v << 1) | b[i];
    return v;
}

static bool hdr_ok(const int *n)
{
    int c4 = (n[0]>>3&1)^(n[0]>>2&1)^(n[0]>>1&1)^(n[0]&1);
    int c3 = (n[0]>>3&1)^(n[1]>>3&1)^(n[1]>>2&1)^(n[1]>>1&1)^(n[2]&1);
    int c2 = (n[0]>>2&1)^(n[1]>>3&1)^(n[1]&1)^(n[2]>>3&1)^(n[2]>>1&1);
    int c1 = (n[0]>>1&1)^(n[1]>>2&1)^(n[1]&1)^(n[2]>>2&1)^(n[2]>>1&1)^(n[2]&1);
    int c0 = (n[0]&1)^(n[1]>>1&1)^(n[2]>>3&1)^(n[2]>>2&1)^(n[2]>>1&1)^(n[2]&1);
    int chk = ((n[3] & 1) << 4) | n[4];
    return chk == (c4<<4 | c3<<3 | c2<<2 | c1<<1 | c0);
}

static bool decode_header(const int *syms, int sf, int off, int *nib /*>=8*/)
{
    int vals[8], cws[8];
    for (int i = 0; i < 8; i++) vals[i] = demap(syms[i], sf, true, off, 1);
    deinterleave(vals, sf - 2, 8, 0, cws);
    for (int i = 0; i < sf - 2; i++) nib[i] = nibble(cws[i], 8, 0);
    return hdr_ok(nib);
}

static void whitening(int n, uint8_t *out)
{
    uint8_t l = 0xFF;
    for (int i = 0; i < n; i++) {
        out[i] = l;
        uint8_t b = ((l >> 7) ^ (l >> 5) ^ (l >> 4) ^ (l >> 3)) & 1;
        l = (uint8_t)((l << 1) | b);
    }
}

static uint16_t crc16(const uint8_t *data, int n)
{
    uint16_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= data[i] << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static bool parity_bad(int cw) { return (getbit(cw,5,0)^getbit(cw,5,1)^getbit(cw,5,2)^getbit(cw,5,3)^getbit(cw,5,4)) != 0; }

/* decode the data blocks, repairing CR 4/5 codewords whose parity fails.
 * Fills data[length]; returns true if the CRC holds. */
static bool decode_packet(const int *syms, int nsym, int sf, int off, uint8_t *data, int *length_out)
{
    int nib[8];
    if (!decode_header(syms, sf, off, nib)) return false;
    int length = nib[0] * 16 + nib[1];
    int has_crc = nib[2] & 1, cr = nib[2] >> 1;
    if (length < 4 || length > 240 || cr != 1) return false;    /* CR 4/5 only */
    int need = 2 * (length + 2 * has_crc);
    int cws[256], ncw = 0;
    int k = 8;
    while (ncw < need - 2 && k + 5 <= nsym) {
        int vals[5], out[8];
        for (int i = 0; i < 5; i++) vals[i] = demap(syms[k + i], sf, false, off, 1);
        deinterleave(vals, sf, 5, 0, out);
        for (int i = 0; i < sf; i++) cws[ncw++] = out[i];
        k += 5;
    }
    if (ncw < need - 2) return false;
    uint8_t w[256];
    whitening(length, w);
    /* the codewords that fail parity; a single bit of each is flipped */
    int bad[8], nbad = 0;
    for (int i = 0; i < need - 2 && nbad < 8; i++) if (parity_bad(cws[i])) bad[nbad++] = i;
    if (!has_crc) { nbad = 0; }         /* nothing to check against; take as is */
    int combos = 1;
    for (int i = 0; i < nbad && i < 4; i++) combos *= 5;
    if (nbad > 4) return false;
    for (int c = 0; c < combos; c++) {
        int tmp[256];
        memcpy(tmp, cws, (need - 2) * sizeof(int));
        int cc = c;
        for (int i = 0; i < nbad; i++) { tmp[bad[i]] ^= 1 << (4 - cc % 5); cc /= 5; }
        uint8_t by[256];
        int nib5[2] = { nib[5], nib[6] };   /* spare nibbles of the header block */
        int nn = 0, src[256];
        src[nn++] = nib5[0]; src[nn++] = nib5[1];
        for (int i = 0; i < need - 2; i++) src[nn++] = nibble(tmp[i], 5, 0);
        for (int i = 0; i < need / 2; i++) by[i] = (uint8_t)(src[2 * i] | src[2 * i + 1] << 4);
        for (int i = 0; i < length; i++) data[i] = by[i] ^ w[i];
        if (!has_crc) { *length_out = length; return true; }
        uint16_t rx = by[length] | by[length + 1] << 8;
        uint16_t cc16 = crc16(data, length - 2) ^ data[length - 1] ^ (data[length - 2] << 8);
        if (cc16 == rx) { *length_out = length; return true; }
    }
    return false;
}

/* ---- Meshtastic frame --------------------------------------------------- */

static int parse_mesh(const uint8_t *data, int length, const uint8_t *key, int keylen, rf_mesh_t *o)
{
    if (length < 16) return 0;
    o->to = data[0] | data[1]<<8 | data[2]<<16 | (uint32_t)data[3]<<24;
    o->from = data[4] | data[5]<<8 | data[6]<<16 | (uint32_t)data[7]<<24;
    o->id = data[8] | data[9]<<8 | data[10]<<16 | (uint32_t)data[11]<<24;
    o->hop_limit = data[12] & 7;
    o->want_ack = (data[12] >> 3) & 1;
    o->chan_hash = data[13];
    o->next_hop = data[14];
    o->relay = data[15];
    o->decrypted = false;
    o->portnum = -1;
    o->text[0] = 0;
    o->payload_len = 0;
    if (!key || keylen == 0) return 1;
    int enc = length - 16;
    if (enc <= 0 || enc > (int)sizeof o->payload) return 1;
    uint8_t buf[256], iv[16] = { 0 };
    memcpy(buf, data + 16, enc);
    /* nonce = packetId (8 LE) + from (4 LE) + 0, counter big-endian */
    iv[0] = data[8]; iv[1] = data[9]; iv[2] = data[10]; iv[3] = data[11];
    iv[8] = data[4]; iv[9] = data[5]; iv[10] = data[6]; iv[11] = data[7];
    /* aes_ctr increments big-endian; Meshtastic's nonce has the counter in
     * bytes 12-15 (zero), so the whole 16-byte block as the IV matches */
    aes_ctr_xcrypt(key, keylen, iv, buf, enc);
    o->decrypted = true;
    /* protobuf Data: field 1 portnum (varint), field 2 payload (bytes) */
    int i = 0, port = -1, plen = 0;
    const uint8_t *pay = NULL;
    while (i < enc) {
        int tag = buf[i++], field = tag >> 3, wt = tag & 7;
        if (wt == 0) {
            int v = 0, s = 0;
            while (i < enc && (buf[i] & 0x80)) { v |= (buf[i] & 0x7f) << s; s += 7; i++; }
            if (i >= enc) break;
            v |= buf[i++] << s;
            if (field == 1) port = v;
        } else if (wt == 2) {
            if (i >= enc) break;
            int ln = buf[i++];
            if (field == 2) { pay = buf + i; plen = ln; }
            i += ln;
        } else break;
    }
    o->portnum = port;
    if (pay && plen > 0 && plen < (int)sizeof o->payload) {
        memcpy(o->payload, pay, plen);
        o->payload_len = plen;
        if (port == 1) { int t = plen < (int)sizeof o->text - 1 ? plen : (int)sizeof o->text - 1; memcpy(o->text, pay, t); o->text[t] = 0; }
    }
    return 1;
}

/* ---- the whole thing ---------------------------------------------------- */

static bool decode_one(const cpx *raw, int n, uint32_t rate, int32_t fc, uint32_t freq_hz,
                       uint32_t bw, int sf, int sfo_sign, cpx *fb, uint8_t *data, int *length)
{
    int N = 1 << sf;
    /* to_baseband: mix + low-pass at rate, resample to bw */
    cpx *y = big((size_t)n * sizeof(cpx)), *tmp = big((size_t)n * sizeof(cpx));
    if (!y || !tmp) { free(y); free(tmp); return false; }
    memcpy(y, raw, (size_t)n * sizeof(cpx));
    baseband(y, n, fc, bw * 0.55f, rate, tmp);
    int xlen = (int)((long)(n - 1) * bw / rate);
    cpx *x = big((size_t)xlen * sizeof(cpx));
    if (!x) { free(y); free(tmp); return false; }
    for (int k = 0; k < xlen; k++) x[k] = interp(y, n, (float)((double)k * rate / bw));
    free(y);
    /* sync */
    cpx *yc = NULL;
    int s, npre;
    float cfo;
    if (!sync_sig(x, xlen, sf, fb, &yc, &s, &npre, &cfo)) { free(x); free(tmp); return false; }
    /* fine CFO and timing from the preamble and SFD */
    float fu = 0, fd = 0;
    int nu = 0;
    for (int i = 2; i < npre - 1; i++) { fu += fpeak(yc + s + i * N, N, true, fb); nu++; }
    fu = nu ? fu / nu : 0;
    for (int i = 0; i < 2; i++) fd += fpeak(yc + s + (npre + 2 + i) * N, N, false, fb);
    fd /= 2;
    free(yc);
    float cf = (fu + fd) / 2, tf = (fu - fd) / 2;
    float cfo_bins = cfo + cf;
    float fc2 = fc + cfo_bins * bw / N;
    /* z = raw mixed by fc2, low-pass */
    cpx *z = big((size_t)n * sizeof(cpx));
    if (!z) { free(x); free(tmp); return false; }
    memcpy(z, raw, (size_t)n * sizeof(cpx));
    baseband(z, n, fc2, bw * 0.55f, rate, tmp);
    free(tmp);
    free(x);
    /* resample z at the symbol grid, with the sender's drift */
    float t0 = (s - tf) / (float)bw;
    float ppm = sfo_sign * fc2 / (float)freq_hz;
    int M = (int)((n / (float)rate - t0) * bw * 0.999f);
    int want = (int)((npre + 4.25f) * N) + NSYM_MAX * N;
    if (M > want) M = want;
    cpx *zr = big((size_t)M * sizeof(cpx));
    if (!zr) { free(z); return false; }
    for (int k = 0; k < M; k++) {
        float t = t0 + (float)k / bw * (1 + ppm);
        zr[k] = interp(z, n, t * rate);
    }
    free(z);
    /* symbols after the 2.25 down-chirps */
    int syms[NSYM_MAX], nsym = 0;
    int start = (int)((npre + 4.25f) * N);
    for (int i = 0; i < NSYM_MAX; i++) {
        int a = start + i * N;
        if (a + N > M) break;
        int v = (int)rnd(fpeak(zr + a, N, true, fb));
        syms[nsym++] = ((v % N) + N) % N;
    }
    free(zr);
    return decode_packet(syms, nsym, sf, -1, data, length);
}

bool rf_mesh_decode(const uint8_t *iq, int n, uint32_t rate, int32_t fc, uint32_t freq_hz,
                    uint32_t bw, int sf, const uint8_t *key, int keylen, rf_mesh_t *out)
{
    if (sf < 7 || sf > 12 || (1 << sf) > FFT_MAX) return false;
    cpx *raw = big((size_t)n * sizeof(cpx));
    cpx *fb = big((size_t)FFT_MAX * sizeof(cpx));
    if (!raw || !fb) { free(raw); free(fb); return false; }
    double mi = 0, mq = 0;
    for (int k = 0; k < n; k++) { mi += iq[2*k]; mq += iq[2*k+1]; }
    mi /= n; mq /= n;
    for (int k = 0; k < n; k++) raw[k] = (cpx){ (float)(iq[2*k] - mi), (float)(iq[2*k+1] - mq) };
    memset(out, 0, sizeof *out);
    uint8_t data[256];
    int length = 0;
    bool ok = false;
    for (int sgn = -1; sgn <= 1 && !ok; sgn += 2)
        ok = decode_one(raw, n, rate, fc, freq_hz, bw, sf, sgn, fb, data, &length);
    free(raw);
    free(fb);
    if (!ok) return false;
    out->crc_ok = true;
    out->length = length;
    parse_mesh(data, length, key, keylen, out);
    return true;
}

/* ---- the channel key ---------------------------------------------------- */

static const uint8_t DEFAULT_PSK[16] = {
    0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01 };

static int b64(const char *s, uint8_t *out, int max)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int acc = 0, bits = 0, n = 0;
    for (; *s && *s != '='; s++) {
        const char *p = strchr(T, *s);
        if (!p) { if (*s == '\n' || *s == '\r' || *s == ' ') continue; return -1; }
        acc = (acc << 6) | (int)(p - T);
        bits += 6;
        if (bits >= 8) { bits -= 8; if (n < max) out[n++] = (uint8_t)(acc >> bits); }
    }
    return n;
}

int rf_mesh_key(const char *s, uint8_t key[32])
{
    uint8_t psk[32];
    int n = b64(s, psk, 32);
    if (n < 0) {        /* try hex */
        n = 0;
        for (const char *p = s; p[0] && p[1] && n < 32; p += 2) {
            int hi = -1, lo = -1;
            for (int i = 0; i < 2; i++) {
                char c = p[i] | 0x20, v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
                if (v < 0) return 0;
                if (i == 0) hi = v; else lo = v;
            }
            psk[n++] = (uint8_t)(hi << 4 | lo);
        }
    }
    if (n == 1) {
        if (psk[0] == 0) return 0;      /* no encryption */
        memcpy(key, DEFAULT_PSK, 16);
        key[15] = (uint8_t)(DEFAULT_PSK[15] + psk[0] - 1);
        return 16;
    }
    if (n == 16 || n == 32) { memcpy(key, psk, n); return n; }
    return 0;
}
