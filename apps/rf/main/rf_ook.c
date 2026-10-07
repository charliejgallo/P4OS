/*
 * RF - on-off keying (rf_ook.h): pulses out of I/Q, and what they say.
 *
 * The detector looks at the power of every sample (the whole band the
 * source gives: a cheap 433 MHz remote can be 100 kHz off its frequency),
 * averaged over ~33 us. It follows the noise floor while nothing is on; a
 * transmission starts when the power stays 6 dB over it for 40 us. From
 * then the line between mark and space is the middle, in dB, between the
 * noise and the marks' own level, with hysteresis, so a strong remote and a
 * weak sensor slice alike; and a change only counts once it lasted 40 us
 * (real pulses are 150 us and more): without that, a signal 18 dB weaker
 * than the test's came out as trains of hundreds of noise pulses. It ends after a silence of five times its
 * longest mark (3 ms at least, 20 ms at most): a remote's repeats come
 * apart at their sync gaps, each repeat a train.
 *
 * The analysis then sorts the marks and the spaces into widths (within
 * 30 %), and reads the modulation from them, as rtl_433's analyser does:
 *   PWM          two mark widths (the long one a 1)      remotes: EV1527, PT2262
 *   PPM          one mark width, two space widths (the long one a 1)
 *                                                        sensors: Nexus, Prologue
 *   Manchester   marks and spaces of T and 2T
 * and tries the decoders on the bits. What none of them knows still shows:
 * its modulation, its widths and its bits in hex.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")      /* the .so is built -Os */
#endif

#include "rf_ook.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- the detector ------------------------------------------------------- */

struct rf_ook {
    uint32_t rate;
    float us;                       /* microseconds a sample */
    int32_t dc_i, dc_q;             /* the DC, x 1 (8-bit units x 16) */
    int64_t dc_si, dc_sq;
    int32_t dc_n;
    int32_t win[64];                /* the last powers, for the average */
    int32_t wsum;
    int wpos, wlen;
    uint32_t deb, cand;             /* samples a change must last; how long the present one has */
    float noise, level;             /* power while idle (its mean), power of the marks */
    uint32_t settle;                /* samples since the start: the floor's first fast learning */
    bool in_pkt, high;
    uint32_t run;                   /* samples in the present state */
    uint32_t max_mark;
    float zr, zi;                   /* sum of x[n] conj(x[n-1]) over the marks: the frequency */
    int32_t pi, pq;
    rf_pulses_t pk;
};

rf_ook_t *rf_ook_new(uint32_t rate)
{
    rf_ook_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rate = rate;
    o->us = 1e6f / rate;
    o->noise = 1e9f;
    o->wlen = (int)(rate / 30000);
    o->wlen = o->wlen < 4 ? 4 : o->wlen > 64 ? 64 : o->wlen;
    o->deb = rate * 40 / 1000000;
    if (o->deb < 2) o->deb = 2;
    return o;
}

void rf_ook_free(rf_ook_t *o)
{
    free(o);
}

static void finish(rf_ook_t *o, void (*done)(const rf_pulses_t *, void *), void *ctx)
{
    rf_pulses_t *p = &o->pk;
    if (p->n >= 8 && done) {
        p->snr_db = 10.0f * log10f((o->level + 1) / (o->noise + 1));
        p->offset_hz = (int32_t)(atan2f(o->zi, o->zr) * o->rate / (2 * (float)M_PI));
        done(p, ctx);
    }
    p->n = 0;
    o->in_pkt = o->high = false;
    o->max_mark = 0;
    o->zr = o->zi = 0;
}

#define NOISE_CHECK 8            /* marks after which a train must stand 4 dB over the floor */

static inline uint16_t to_us(const rf_ook_t *o, uint32_t samples)
{
    float v = samples * o->us;
    return (uint16_t)(v > 65535 ? 65535 : v);
}

void rf_ook_feed(rf_ook_t *o, const uint8_t *iq, int n, void (*done)(const rf_pulses_t *, void *), void *ctx)
{
    /* the 3 ms .. 20 ms silence that ends a train, in samples */
    const uint32_t gap_min = o->rate * 3 / 1000, gap_max = o->rate / 50, mark_max = o->rate / 50;
    int64_t si = 0, sq = 0;
    for (int k = 0; k < n; k++) {
        int32_t i = ((int32_t)iq[2 * k] << 4) - 2048 - o->dc_i, q = ((int32_t)iq[2 * k + 1] << 4) - 2048 - o->dc_q;
        si += ((int32_t)iq[2 * k] << 4) - 2048;
        sq += ((int32_t)iq[2 * k + 1] << 4) - 2048;
        int32_t pw = (i * i + q * q) >> 4;
        o->wsum += pw - o->win[o->wpos];
        o->win[o->wpos] = pw;
        if (++o->wpos == o->wlen) o->wpos = 0;
        float p = (float)o->wsum / o->wlen;
        o->run++;
        if (!o->in_pkt) {
            /* idle: follow the floor, wait for something 6 dB over it for 40 us.
             * The floor is the noise's MEAN power, followed alike up and
             * down (what stands 6 dB over it stays out, so a transmission's
             * edge does not lift it). It used to come down 20 times faster
             * than it went up: it sat at the noise's low quantiles, and the
             * board's real noise - far larger than the tests' - crossed
             * "6 dB over" by itself, opened a train that noise kept alive
             * up to its 600 marks, and a remote that came in the middle had
             * its repeats cut anywhere (a copier remote, 2026-10-07: 21, 23,
             * 28 bits of a 24-bit code). */
            float k = o->settle < o->rate / 20 ? 0.01f : 0.0005f;
            if (o->settle < o->rate / 20) o->settle++;
            if (p < o->noise * 4 || o->noise > 1e8f) o->noise += (p - o->noise) * k;
            if (p > o->noise * 4 && p > 4) {
                if (++o->cand >= o->deb) {
                    o->in_pkt = o->high = true;
                    o->level = p;
                    o->run = o->cand;
                    o->cand = 0;
                    o->pk.n = 0;
                }
            } else o->cand = 0;
            o->pi = i;
            o->pq = q;
            continue;
        }
        /* the line between mark and space: the middle in dB, with hysteresis;
         * a change counts once it lasted deb samples */
        float mid = sqrtf((o->noise + 1) * (o->level + 1));
        if (o->high) {
            o->level += (p - o->level) * 0.02f;
            o->zr += (float)i * o->pi + (float)q * o->pq;
            o->zi += (float)q * o->pi - (float)i * o->pq;
            if (p < mid * 0.8f) {
                if (++o->cand >= o->deb) {
                    uint32_t len = o->run - o->cand;
                    if (o->pk.n < RF_PULSES_MAX) o->pk.mark[o->pk.n] = to_us(o, len);
                    if (len > o->max_mark) o->max_mark = len;
                    o->high = false;
                    o->run = o->cand;
                    o->cand = 0;
                }
            } else o->cand = 0;
            if (o->high && o->run > mark_max) {
                /* a carrier that stays on: not a remote */
                o->pk.n = 0;
                finish(o, NULL, NULL);
            }
        } else {
            uint32_t limit = o->max_mark * 5;
            limit = limit < gap_min ? gap_min : limit > gap_max ? gap_max : limit;
            if (p > mid * 1.25f) {
                if (++o->cand >= o->deb) {
                    uint32_t len = o->run - o->cand;
                    if (o->pk.n < RF_PULSES_MAX) o->pk.space[o->pk.n++] = to_us(o, len);
                    if (o->pk.n >= RF_PULSES_MAX) {
                        finish(o, done, ctx);
                        o->cand = 0;
                        continue;
                    }
                    /* Noise that opened a train keeps it alive: its "marks"
                     * are barely over the line, which sits only 3 dB over
                     * the floor. After a few marks, under 4 dB is let go (noise
                     * that did came out at 0.2 to 2.7 dB on the board). */
                    if (o->pk.n == NOISE_CHECK && o->level < o->noise * 2.5f) {
                        o->pk.n = 0;
                        finish(o, NULL, NULL);
                        o->cand = 0;
                        continue;
                    }
                    o->high = true;
                    o->run = o->cand;
                    o->cand = 0;
                }
            } else {
                o->cand = 0;
                if (o->run > limit) {
                    if (o->pk.n < RF_PULSES_MAX) o->pk.space[o->pk.n++] = to_us(o, o->run);
                    finish(o, done, ctx);
                }
            }
        }
        o->pi = i;
        o->pq = q;
    }
    /* the DC: the mean over 0.1 s, as the demodulator takes it */
    o->dc_si += si;
    o->dc_sq += sq;
    o->dc_n += n;
    if (o->dc_n >= (int32_t)(o->rate / 10)) {
        o->dc_i = (int32_t)(o->dc_si / o->dc_n);
        o->dc_q = (int32_t)(o->dc_sq / o->dc_n);
        o->dc_si = o->dc_sq = 0;
        o->dc_n = 0;
    }
}

/* ---- the analysis ------------------------------------------------------- */

/* The widths in v[0..n), grouped within 30 %: their centres, shortest
 * first. Returns how many groups (up to max). */
static int widths(const uint16_t *v, int n, int *centre, int *count, int max)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < k && (v[i] < centre[j] * 0.7f || v[i] > centre[j] * 1.3f)) j++;
        if (j < k) {
            centre[j] = (centre[j] * count[j] + v[i]) / (count[j] + 1);
            count[j]++;
        } else if (k < max) {
            centre[k] = v[i];
            count[k++] = 1;
        } else return max + 1;      /* too many widths: not a code */
    }
    /* shortest first */
    for (int a = 0; a < k; a++)
        for (int b = a + 1; b < k; b++)
            if (centre[b] < centre[a]) {
                int t = centre[a]; centre[a] = centre[b]; centre[b] = t;
                t = count[a]; count[a] = count[b]; count[b] = t;
            }
    return k;
}

static void put_bit(rf_decoded_t *d, int b)
{
    if (d->nbits >= (int)sizeof d->bits * 8) return;
    if (b) d->bits[d->nbits / 8] |= (uint8_t)(0x80 >> (d->nbits % 8));
    d->nbits++;
}

static uint32_t bits_at(const rf_decoded_t *d, int from, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        int b = from + i;
        v = v << 1 | ((d->bits[b / 8] >> (7 - b % 8)) & 1);
    }
    return v;
}

static void to_hex(rf_decoded_t *d)
{
    int nibbles = (d->nbits + 3) / 4, k = 0;
    for (int i = 0; i < nibbles; i++) {
        int from = i * 4, len = d->nbits - from < 4 ? d->nbits - from : 4;
        uint32_t v = bits_at(d, from, len) << (4 - len);
        k += snprintf(d->hex + k, sizeof d->hex - k, "%X", (unsigned)v);
    }
}

/* ---- the decoders ------------------------------------------------------- */

/* EV1527 and PT2262: the remotes of gates, alarms and doorbells. 24 bits
 * of PWM, a 1 a long mark (3 T) and a short space, T between 150 and
 * 600 us, a sync of 31 T. EV1527: a 20-bit ID and 4 data bits (the
 * buttons); PT2262: 12 three-state symbols (00 = 0, 11 = 1, 01 = F). */
static bool dec_ev1527(rf_decoded_t *d)
{
    if (d->mod != RF_MOD_PWM || (d->nbits != 24 && d->nbits != 25)) return false;
    float r = (float)d->long_us / d->short_us;
    if (d->short_us < 150 || d->short_us > 650 || r < 2.0f || r > 4.5f) return false;
    if (d->nbits == 25 && bits_at(d, 24, 1)) return false;   /* the sync's own short mark is a 0 */
    uint32_t v = bits_at(d, 0, 24);
    bool has10 = false, has01 = false;
    for (int i = 0; i < 12; i++) {
        int pair = (v >> (22 - 2 * i)) & 3;
        has10 |= pair == 2;
        has01 |= pair == 1;
    }
    char tri[13];
    for (int i = 0; i < 12; i++) {
        int pair = (v >> (22 - 2 * i)) & 3;
        tri[i] = pair == 0 ? '0' : pair == 3 ? '1' : 'F';
    }
    tri[12] = 0;
    /* One in 30 EV1527 codes has no "10" pair and reads as PT2262 too, and
     * the bits cannot tell which. A remote's PT2262 has its buttons on the
     * last four symbols, the data pins, driven to 0 or 1: an F there is
     * more likely an EV1527 (a copier remote, 2026-10-07: 4CC454 read as
     * F01010F0 FFF0), though a sensor's PT2262 with twelve address symbols
     * can float them too. So the other reading stays in d->code. */
    bool pt_ok = !has10 && has01;
    bool data_driven = tri[8] != 'F' && tri[9] != 'F' && tri[10] != 'F' && tri[11] != 'F';
    if (pt_ok) snprintf(d->code, sizeof d->code, "%s", tri);
    if (pt_ok && data_driven) {
        snprintf(d->proto, sizeof d->proto, "PT2262");
        snprintf(d->key, sizeof d->key, "pt2262-%.8s", tri);
        snprintf(d->text, sizeof d->text, "code %.8s data %s", tri, tri + 8);
        d->id = v >> 8;
        d->button = v & 0xFF;
        d->has_button = true;
        return true;
    }
    d->id = v >> 4;
    d->button = v & 0xF;
    d->has_button = true;
    snprintf(d->proto, sizeof d->proto, "EV1527");
    snprintf(d->key, sizeof d->key, "ev1527-%05X", (unsigned)d->id);
    snprintf(d->text, sizeof d->text, "ID %05X button %X", (unsigned)d->id, d->button);
    return true;
}

/* The cheap weather sensors of 36 bits of PPM (marks ~500 us, spaces of
 * ~1000 and ~2000 us, a long one a 1): Nexus (and its many brands) and
 * Prologue. Nexus: ID 8, battery 1, a 0, channel 2, temperature 12
 * (tenths of a degree, signed), 1111, humidity 8. Prologue: type 4 (5 or
 * 9), ID 8, battery 1, button 1, channel 2, temperature 12, humidity 8. */
static bool dec_temp36(rf_decoded_t *d)
{
    if (d->mod != RF_MOD_PPM || d->nbits != 36) return false;
    if (d->short_us < 700 || d->short_us > 1400 || d->long_us < 1500 || d->long_us > 2800) return false;
    if (bits_at(d, 24, 4) == 0xF) {
        int hum = (int)bits_at(d, 28, 8);
        int temp = (int)bits_at(d, 12, 12);
        if (temp & 0x800) temp -= 0x1000;
        if (hum > 100 || temp < -400 || temp > 700) return false;
        d->id = bits_at(d, 0, 8);
        d->batt_low = !bits_at(d, 8, 1);
        d->channel = (int)bits_at(d, 10, 2) + 1;
        d->temp_c = temp / 10.0f;
        d->hum = hum;
        d->has_temp = d->has_hum = d->has_batt = true;
        snprintf(d->proto, sizeof d->proto, "Nexus-TH");
        snprintf(d->key, sizeof d->key, "nexus-%u-%d", (unsigned)d->id, d->channel);
        snprintf(d->text, sizeof d->text, "ID %u ch %d %.1f C %d %%%s", (unsigned)d->id, d->channel, (double)d->temp_c,
                 hum, d->batt_low ? " battery low" : "");
        return true;
    }
    int type = (int)bits_at(d, 0, 4);
    if (type == 5 || type == 9) {
        int temp = (int)bits_at(d, 16, 12);
        if (temp & 0x800) temp -= 0x1000;
        int hum = (int)bits_at(d, 28, 8);
        if (temp < -400 || temp > 700) return false;
        d->id = bits_at(d, 4, 8);
        d->batt_low = !bits_at(d, 12, 1);
        d->button = (int)bits_at(d, 13, 1);
        d->channel = (int)bits_at(d, 14, 2) + 1;
        d->temp_c = temp / 10.0f;
        d->has_temp = d->has_batt = true;
        d->has_hum = hum <= 100;
        d->hum = hum;
        snprintf(d->proto, sizeof d->proto, "Prologue");
        snprintf(d->key, sizeof d->key, "prologue-%u-%d", (unsigned)d->id, d->channel);
        snprintf(d->text, sizeof d->text, "ID %u ch %d %.1f C%s", (unsigned)d->id, d->channel, (double)d->temp_c,
                 d->batt_low ? " battery low" : "");
        return true;
    }
    return false;
}

/* ---- putting it together ------------------------------------------------ */

bool rf_ook_decode(const rf_pulses_t *p, rf_decoded_t *d)
{
    memset(d, 0, sizeof *d);
    if (p->n < 8) return false;
    int mc[4], mn[4], sc[4], sn[4];
    int nm = widths(p->mark, p->n, mc, mn, 3);
    /* the spaces but the gap that ended it, and the sync gaps: much longer
     * than the rest (over 2.5 times the second longest kind) */
    uint16_t sp[RF_PULSES_MAX];
    int nsp = 0;
    for (int i = 0; i < p->n - 1; i++) sp[nsp++] = p->space[i];
    int ns = widths(sp, nsp, sc, sn, 3);
    int sync = 0;
    if (ns >= 2 && sc[ns - 1] > 2.5f * sc[ns - 2] && sn[ns - 1] <= 3) {
        sync = sc[ns - 1];
        ns--;
    }
    d->gap_us = sync ? sync : p->space[p->n - 1];
    if (nm > 3 || ns > 3) return false;     /* irregular: noise, or something too garbled to show */
    float rm = nm == 2 ? (float)mc[1] / mc[0] : 0, rs = ns == 2 ? (float)sc[1] / sc[0] : 0;
    if (nm == 2 && ns == 2 && rm > 1.6f && rm < 2.4f && rs > 1.6f && rs < 2.4f &&
        abs(mc[0] - sc[0]) < 0.35f * mc[0]) {
        /* Manchester: levels of half a bit each, paired */
        d->mod = RF_MOD_MANCHESTER;
        d->short_us = (mc[0] + sc[0]) / 2;
        d->long_us = 2 * d->short_us;
        int half = d->short_us, cnt = 0;
        uint8_t lv[2 * RF_PULSES_MAX * 2];
        for (int i = 0; i < p->n && cnt < (int)sizeof lv - 4; i++) {
            int a = p->mark[i] > 1.5f * half ? 2 : 1;
            for (int k = 0; k < a; k++) lv[cnt++] = 1;
            if (i == p->n - 1) break;
            int b = p->space[i] > 1.5f * half ? 2 : 1;
            if (p->space[i] > 3 * half) break;
            for (int k = 0; k < b; k++) lv[cnt++] = 0;
        }
        for (int i = 0; i + 1 < cnt; i += 2) {
            if (lv[i] == lv[i + 1]) i--;    /* out of step by half a bit: resync */
            else put_bit(d, lv[i]);
        }
    } else if (nm == 2) {
        d->mod = RF_MOD_PWM;
        d->short_us = mc[0];
        d->long_us = mc[1];
        int mid = (mc[0] + mc[1]) / 2;
        for (int i = 0; i < p->n; i++) {
            put_bit(d, p->mark[i] > mid);
            if (sync && i < p->n - 1 && p->space[i] > sync * 0.7f) break;     /* one frame */
        }
    } else if (nm == 1 && ns == 2) {
        d->mod = RF_MOD_PPM;
        d->short_us = sc[0];
        d->long_us = sc[1];
        int mid = (sc[0] + sc[1]) / 2;
        for (int i = 0; i < p->n - 1; i++) {
            if (sync && p->space[i] > (sync + sc[1]) / 2) {
                if (d->nbits) break;
                continue;
            }
            put_bit(d, p->space[i] > mid);
        }
    } else {
        d->mod = RF_MOD_UNKNOWN;
        d->short_us = mc[0];
        d->long_us = ns ? sc[ns - 1] : 0;
    }
    to_hex(d);
    if (dec_ev1527(d) || dec_temp36(d)) return true;
    /* nobody knows it: what it looks like */
    static const char *const MOD[] = { "?", "PWM", "PPM", "Manchester" };
    snprintf(d->key, sizeof d->key, "%s-%d-%.12s", MOD[d->mod], d->nbits, d->hex);
    snprintf(d->text, sizeof d->text, "%s %d/%d us, %d bits", MOD[d->mod], d->short_us, d->long_us, d->nbits);
    return true;
}
