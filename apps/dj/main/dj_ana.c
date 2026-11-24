/*
 * DJ - the analysis thread: the whole track, decoded once.
 *
 * Every DJ_WHOP (256) samples of the track it writes one waveform entry:
 * the peak and the level of three bands (under 200 Hz, 200 Hz to 2.5 kHz,
 * above), which the waveform paints as colour. From the same bands, the
 * onset envelope: how much the low and mid energy jumped since the last
 * entry, which is where the kicks and the snares are.
 *
 * The tempo out of that envelope. For a tempo candidate, fold the envelope
 * over one beat (a histogram of where in the beat each onset falls): at the
 * right tempo the kicks pile into one bin; at a wrong one they smear over
 * all of them. The score is the tallest bin over the mean. Candidates from
 * 86 to 176 BPM every half BPM, the three best refined in steps of 0.02,
 * a whole number taken when it scores as well, and the tallest bin is the
 * beat grid's phase. Half a tempo above 140 wins when it scores nearly as
 * well (an 80 BPM track looks like 160 as much as like 80; drum and bass
 * at 174 often looks like 87 nearly as much, and comes out at 87: the
 * grid sheet doubles it).
 *
 * It runs at the lowest priority there is and naps every few tens of
 * milliseconds, so it only takes what nobody else wants. A track loaded
 * again while it works makes it drop the old one.
 */
#include "dj.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BPM_LO   86.0f
#define BPM_HI   176.0f
#define NB       64
#define NC       180           /* candidates: (BPM_HI - BPM_LO) * 2 */
#define CHUNK    4096          /* samples decoded at a time */

static struct {
    dj_engine_t *e;
    volatile bool quit, done, running;
    uint32_t seen[DJ_NDECK];   /* the generation each deck was analysed at */
} A;

/* ---- the tempo ---- */

static float fold_score(const float *on, int n, float P, float *hist, float *best_bin)
{
    for (int b = 0; b < NB; b++) hist[b] = 0.0f;
    float inc = (float)NB / P, ph = 0.0f, sum = 0.0f;
    for (int i = 0; i < n; i++) {
        int b = (int)ph;
        hist[b] += on[i];
        sum += on[i];
        ph += inc;
        if (ph >= (float)NB) ph -= (float)NB;
    }
    if (sum <= 0.0f) return 0.0f;
    float best = -1.0f;
    int bi = 0;
    for (int b = 0; b < NB; b++) {
        float v = hist[b] + 0.5f * (hist[(b + 1) % NB] + hist[(b + NB - 1) % NB]);
        if (v > best) { best = v; bi = b; }
    }
    if (best_bin) {
        /* the peak between bins */
        float l = hist[(bi + NB - 1) % NB], c = hist[bi], r = hist[(bi + 1) % NB];
        float den = l - 2.0f * c + r;
        float off = den < 0.0f ? 0.5f * (l - r) / den : 0.0f;
        *best_bin = (float)bi + dj_clampf(off, -0.5f, 0.5f);
    }
    return best / (2.0f * sum / (float)NB);
}

float dj_ana_tempo(const float *onset, const float *kick, int n, float fr, float *phase_frames)
{
    if (n < (int)(fr * 8.0f)) return 0.0f;
    /* minus the local mean: what stands out of its surroundings */
    float *on = dj_alloc((size_t)n * sizeof(float));
    if (!on) return 0.0f;
    int w = (int)(fr * 0.25f);
    float acc = 0.0f;
    for (int i = 0; i < n && i < 2 * w + 1; i++) acc += onset[i];
    for (int i = 0; i < n; i++) {
        int a = i - w - 1, b = i + w;
        if (a >= 0 && b < n) acc += onset[b] - onset[a];
        float mean = acc / (float)(2 * w + 1);
        float v = onset[i] - mean;
        on[i] = v > 0.0f ? v : 0.0f;
    }
    float hist[NB];
    /* every candidate's score, then the three tallest peaks of that curve */
    float sc[NC];
    for (int i = 0; i < NC; i++) {
        sc[i] = fold_score(on, n, 60.0f * fr / (BPM_LO + 0.5f * (float)i), hist, NULL);
#ifdef DJ_ANA_DEBUG
        printf("  %.1f %.3f\n", (double)(BPM_LO + 0.5f * (float)i), (double)sc[i]);
#endif
    }
    float cand[3] = { 0 }, cs[3] = { 0 };
    for (int i = 0; i < NC; i++) {
        if ((i > 0 && sc[i - 1] > sc[i]) || (i + 1 < NC && sc[i + 1] >= sc[i])) continue;    /* not a peak */
        for (int k = 0; k < 3; k++) {
            if (sc[i] <= cs[k]) continue;
            for (int j = 2; j > k; j--) { cs[j] = cs[j - 1]; cand[j] = cand[j - 1]; }
            cs[k] = sc[i];
            cand[k] = BPM_LO + 0.5f * (float)i;
            break;
        }
    }
    float best = 0.0f, bs = 0.0f;
    for (int k = 0; k < 3; k++) {
        if (cs[k] <= 0.0f) continue;
        for (float bpm = cand[k] - 0.6f; bpm <= cand[k] + 0.6f; bpm += 0.02f) {
            float s = fold_score(on, n, 60.0f * fr / bpm, hist, NULL);
            if (s > bs) { bs = s; best = bpm; }
        }
    }
    if (best <= 0.0f) { dj_free(on); return 0.0f; }
    if (best >= 140.0f) {
        float h = best * 0.5f;
        float s = fold_score(on, n, 60.0f * fr / h, hist, NULL);
        if (s * 1.05f >= bs) { best = h; bs = s; }
    }
    float rb = floorf(best + 0.5f);
    if (fabsf(rb - best) < 0.08f) {
        float s = fold_score(on, n, 60.0f * fr / rb, hist, NULL);
        if (s >= bs * 0.98f) best = rb;
    }
    float bin = 0.0f;
    float P = 60.0f * fr / best;
    fold_score(on, n, P, hist, &bin);
    /* on the beat or half a beat off? A bass on the off-beats rises as
     * much as a kick on the beat in log terms; in plain energy the kick
     * wins. */
    if (kick) {
        float kh[NB];
        float b2;
        fold_score(kick, n, P, kh, &b2);
        int a = ((int)floorf(bin + 0.5f) % NB + NB) % NB, o = (a + NB / 2) % NB;
        float va = kh[a] + 0.5f * (kh[(a + 1) % NB] + kh[(a + NB - 1) % NB]);
        float vo = kh[o] + 0.5f * (kh[(o + 1) % NB] + kh[(o + NB - 1) % NB]);
#ifdef DJ_ANA_DEBUG
        printf("  phase: bin %.1f on %.4f off %.4f\n", (double)bin, (double)va, (double)vo);
#endif
        if (vo > va * 1.3f) bin += (float)(NB / 2);
    }
    float ph = (bin + 0.5f) * P / (float)NB;
    while (ph >= P) ph -= P;
    if (phase_frames) *phase_frames = ph;
    dj_free(on);
    return best;
}

/* ---- the pass over the track ---- */

typedef struct {
    float lp1, lp2, hp1, hp2;   /* the band filters' state */
    float a_lo, a_hi;           /* their coefficients */
    float peak, s_lo, s_mid, s_hi, s_all;
    int   cnt;
    float prev_lo, prev_mid, prev_k;
} bands_t;

static void bands_init(bands_t *b, uint32_t sr)
{
    memset(b, 0, sizeof *b);
    b->a_lo = 1.0f - expf(-2.0f * 3.14159265f * 200.0f / (float)sr);
    b->a_hi = 1.0f - expf(-2.0f * 3.14159265f * 2500.0f / (float)sr);
}

typedef struct {
    dj_track_t *t;
    uint32_t    gen;
    bands_t     b;
    float      *onset;
    float      *kick;           /* the low and mid bands' rise in plain energy */
    uint32_t    n;              /* entries done */
    float       sq_sum;         /* of the entries above the floor, for the loudness */
    uint32_t    sq_n;
    float       peak;
} pass_t;

static bool push(pass_t *p, const int16_t *s, int count)
{
    bands_t *b = &p->b;
    dj_track_t *t = p->t;
    for (int i = 0; i < count; i++) {
        float x = ((float)s[i * 2] + (float)s[i * 2 + 1]) * (0.5f / 32768.0f);
        float ax = fabsf(x);
        if (ax > b->peak) b->peak = ax;
        /* two poles each: a hi-hat must not leak into the mids, where the
         * beat is looked for */
        b->lp1 += (x - b->lp1) * b->a_lo;
        b->lp2 += (b->lp1 - b->lp2) * b->a_lo;
        b->hp1 += (x - b->hp1) * b->a_hi;
        b->hp2 += (b->hp1 - b->hp2) * b->a_hi;
        float lo = b->lp2, hi = x - b->hp2, mid = b->hp2 - lo;
        b->s_all += x * x;
        b->s_lo += lo * lo;
        b->s_mid += mid * mid;
        b->s_hi += hi * hi;
        if (++b->cnt == DJ_WHOP) {
            if (p->n >= t->nwave) return false;
            float k = 1.0f / DJ_WHOP;
            float rl = sqrtf(b->s_lo * k), rm = sqrtf(b->s_mid * k), rh = sqrtf(b->s_hi * k);
            dj_wave_t *w = &t->wave[p->n];
            w->peak = (uint8_t)(dj_clampf(b->peak, 0.0f, 1.0f) * 255.0f);
            /* with headroom: a loud master must not fill them all (the
             * whole track is scaled to the screen at the end) */
            w->lo = (uint8_t)(dj_clampf(rl * 1.2f, 0.0f, 1.0f) * 255.0f);
            w->mid = (uint8_t)(dj_clampf(rm * 1.8f, 0.0f, 1.0f) * 255.0f);
            w->hi = (uint8_t)(dj_clampf(rh * 3.0f, 0.0f, 1.0f) * 255.0f);
            /* the rise of each band, in log: the kick's counts most */
            float el = logf(1e-4f + rl * rl), em = logf(1e-4f + rm * rm);
            float fl = el - b->prev_lo, fm = em - b->prev_mid;
            b->prev_lo = el;
            b->prev_mid = em;
            if (p->onset) p->onset[p->n] = (fl > 0.0f ? fl : 0.0f) + 0.4f * (fm > 0.0f ? fm : 0.0f);
            /* the kick's and the snare's energy: they are on the beat */
            float ek = rl * rl + 3.0f * rm * rm;
            float kl = ek - b->prev_k;
            b->prev_k = ek;
            if (p->kick) p->kick[p->n] = kl > 0.0f ? kl : 0.0f;
            float ms = b->s_all * k;       /* the signal's, not the bands' (they overlap) */
            if (ms > 1e-4f) { p->sq_sum += ms; p->sq_n++; }
            if (b->peak > p->peak) p->peak = b->peak;
            b->peak = b->s_lo = b->s_mid = b->s_hi = b->s_all = 0.0f;
            b->cnt = 0;
            p->n++;
            if ((p->n & 63) == 0) t->wave_n = p->n;
        }
    }
    return true;
}

static bool stale(const pass_t *p) { return A.quit || p->t->gen != p->gen || p->t->state != DJ_TS_READY; }

/* a nap now and then: the idle task must run, and the UI comes first */
static void breathe(uint32_t *t0)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - *t0 >= 25) {
        aos_hal_sleep_ms(3);
        *t0 = (uint32_t)aos_hal_uptime_ms();
    }
}

static bool pass_mp3(pass_t *p, int16_t *pcm)
{
    dj_track_t *t = p->t;
    FILE *f = fopen(t->path, "rb");
    dj_mp3_t *m = dj_mp3_new();
    uint8_t *in = dj_alloc(64 * 1024 + 8);
    bool ok = f && m && in;
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    uint32_t fr = 0;
    while (ok && fr < t->nframes) {
        if (stale(p)) { ok = false; break; }
        uint32_t last = fr;
        while (last + 1 < t->nframes && t->fidx[last + 2] - t->fidx[fr] <= 64 * 1024) last++;
        uint32_t off = t->fidx[fr], len = t->fidx[last + 1] - off;
        if (len > 64 * 1024) len = 64 * 1024;
        if (fseek(f, (long)off, SEEK_SET) || fread(in, 1, len, f) != len) break;
        for (uint32_t g = fr; g <= last; g++) {
            int used, ch, hz;
            uint32_t o = t->fidx[g] - off;
            int n = dj_mp3_frame(m, in + o, (int)(len - o), pcm, &used, &ch, &hz);
            if (n <= 0) {
                memset(pcm, 0, (size_t)t->spf * 4);
                n = t->spf;
                ch = 2;
            } else if (ch == 1) {
                for (int i = n - 1; i >= 0; i--) pcm[i * 2] = pcm[i * 2 + 1] = pcm[i];
            }
            if (!push(p, pcm, n)) break;
        }
        fr = last + 1;
        breathe(&t0);
    }
    if (f) fclose(f);
    dj_mp3_free(m);
    dj_free(in);
    return ok;
}

static bool pass_wav(pass_t *p, int16_t *pcm)
{
    dj_track_t *t = p->t;
    FILE *f = fopen(t->path, "rb");
    uint8_t *in = dj_alloc((size_t)CHUNK * t->wav_align);
    bool ok = f && in;
    if (ok) fseek(f, (long)t->data_off, SEEK_SET);
    uint32_t done = 0, t0 = (uint32_t)aos_hal_uptime_ms();
    int bps = t->wav_bits / 8;
    while (ok && done < t->nsamp) {
        if (stale(p)) { ok = false; break; }
        uint32_t want = t->nsamp - done < CHUNK ? t->nsamp - done : CHUNK;
        uint32_t got = (uint32_t)fread(in, t->wav_align, want, f);
        for (uint32_t i = 0; i < got; i++) {
            const uint8_t *q = in + i * t->wav_align;
            for (int c = 0; c < 2; c++) {
                const uint8_t *s = q + (c && t->wav_ch > 1 ? bps : 0);
                int16_t v;
                if (t->wav_float) {
                    union { uint32_t u; float f; } x = { .u = (uint32_t)(s[0] | (s[1] << 8) | (s[2] << 16) | ((uint32_t)s[3] << 24)) };
                    float y = dj_clampf(x.f, -1.0f, 1.0f) * 32767.0f;
                    v = (int16_t)y;
                } else if (bps == 1) v = (int16_t)((s[0] - 128) << 8);
                else if (bps == 2) v = (int16_t)(s[0] | (s[1] << 8));
                else v = (int16_t)(s[bps - 2] | (s[bps - 1] << 8));
                pcm[i * 2 + c] = v;
            }
        }
        if (!push(p, pcm, (int)got) || got < want) break;
        done += got;
        breathe(&t0);
    }
    if (f) fclose(f);
    dj_free(in);
    return ok;
}

static bool pass_flac(pass_t *p, int16_t *pcm)
{
    dj_track_t *t = p->t;
    uint32_t sr, ns;
    int ch;
    dj_flac_t *f = dj_flac_open(t->path, &sr, &ns, &ch);
    if (!f) return false;
    uint32_t done = 0, t0 = (uint32_t)aos_hal_uptime_ms();
    bool ok = true;
    while (done < t->nsamp) {
        if (stale(p)) { ok = false; break; }
        int want = t->nsamp - done < CHUNK ? (int)(t->nsamp - done) : CHUNK;
        int got = dj_flac_read(f, done, pcm, want);
        if (got <= 0) break;
        if (!push(p, pcm, got)) break;
        done += (uint32_t)got;
        breathe(&t0);
    }
    dj_flac_close(f);
    return ok;
}

/* The whole track to the screen's height, band by band, stretched between
 * the level the band rarely goes under (its 20th percentile, kept at a
 * fifth of the height) and the one its loud parts reach (the 98th, at 240).
 * A dense, loud master, whose bands hardly move, shows its kicks and
 * breaks again; a quiet track grows. */
static uint8_t band(const dj_wave_t *w, int k) { return k == 0 ? w->lo : k == 1 ? w->mid : w->hi; }

static void wave_fit(dj_track_t *t, uint32_t n)
{
    if (!n || !t->wave) return;
    float lo[3], sc[3];
    for (int k = 0; k < 3; k++) {
        uint32_t hist[256] = { 0 };
        for (uint32_t i = 0; i < n; i++) hist[band(&t->wave[i], k)]++;
        uint32_t want20 = n / 100 * 20, want98 = n / 100 * 98, acc = 0;
        int v20 = 0, v98 = 255;
        bool got20 = false;
        for (int i = 0; i < 256; i++) {
            acc += hist[i];
            if (!got20 && acc >= want20) { v20 = i; got20 = true; }
            if (acc >= want98) { v98 = i; break; }
        }
        /* the floor at 70 % of the 20th percentile, mapped to 48 */
        float f0 = 0.7f * (float)v20, span = (float)v98 - f0;
        if (span < 4.0f) span = 4.0f;
        lo[k] = f0;
        sc[k] = (240.0f - 48.0f) / span;
    }
    for (uint32_t i = 0; i < n; i++) {
        dj_wave_t *w = &t->wave[i];
        for (int k = 0; k < 3; k++) {
            float v = (float)band(w, k);
            float o = v <= lo[k] ? v * 48.0f / (lo[k] > 1.0f ? lo[k] : 1.0f) : 48.0f + (v - lo[k]) * sc[k];
            uint8_t u = (uint8_t)(o > 255.0f ? 255.0f : o < 0.0f ? 0.0f : o);
            if (k == 0) w->lo = u;
            else if (k == 1) w->mid = u;
            else w->hi = u;
        }
    }
}

/* one track, start to end, in the calling thread (the thread below; a
 * test on the desktop) */
void dj_ana_track(dj_track_t *t)
{
    pass_t p;
    memset(&p, 0, sizeof p);
    p.t = t;
    p.gen = t->gen;
    bands_init(&p.b, t->sr);
    bool want_tempo = !t->meta_bpm;
    if (want_tempo) {
        p.onset = dj_alloc((size_t)t->nwave * sizeof(float));
        p.kick = dj_alloc((size_t)t->nwave * sizeof(float));
    }
    int16_t *pcm = dj_alloc((size_t)CHUNK * 4 + 1152 * 4);
    t->ana = DJ_ANA_RUN;
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    bool ok = pcm != NULL;
    if (ok) {
        if (t->fmt == DJ_FMT_MP3) ok = pass_mp3(&p, pcm);
        else if (t->fmt == DJ_FMT_WAV) ok = pass_wav(&p, pcm);
        else ok = pass_flac(&p, pcm);
    }
    dj_free(pcm);
    if (stale(&p)) { dj_free(p.onset); dj_free(p.kick); return; }
    t->wave_n = p.n;
    uint32_t t1 = (uint32_t)aos_hal_uptime_ms();
    float rms = p.sq_n ? sqrtf(p.sq_sum / (float)p.sq_n) : 0.0f;
    float gain = rms > 0.001f ? dj_clampf(0.20f / rms, 0.35f, 3.0f) : 1.0f;
    float bpm = 0.0f, ph = 0.0f;
    if (want_tempo && p.onset && p.n > 0) {
        float fr = (float)t->sr / (float)DJ_WHOP;
        bpm = dj_ana_tempo(p.onset, p.kick, (int)p.n, fr, &ph);
    }
    dj_free(p.onset);
    dj_free(p.kick);
    if (stale(&p)) return;
    wave_fit(t, p.n);
    t->gain = gain;
    t->peak = p.peak;
    if (bpm > 0.0f) {
        /* an onset at entry i is a rise between i-1 and i: the hit is about
         * half an entry in, and the filters lag a little more */
        float b0 = ph * (float)DJ_WHOP - 0.25f * DJ_WHOP;
        float P = 60.0f / bpm * (float)t->sr;
        while (b0 < 0.0f) b0 += P;
        t->beat0 = b0;
        t->bpm = bpm;
    }
    t->ana = ok ? DJ_ANA_DONE : DJ_ANA_FAIL;
    aos_hal_log(DJ_TAG, "analysis: %s: %.2f BPM (first beat %.0f), gain %.2f, %u entries in %u ms + %u ms",
                t->title, (double)t->bpm, (double)t->beat0, (double)gain, (unsigned)p.n,
                (unsigned)(t1 - t0), (unsigned)((uint32_t)aos_hal_uptime_ms() - t1));
}

static void ana_main(void *arg)
{
    (void)arg;
    while (!A.quit) {
        bool did = false;
        for (int d = 0; d < DJ_NDECK && !A.quit; d++) {
            dj_track_t *t = dj_deck_track(A.e, d);
            if (t->state == DJ_TS_READY && t->gen != A.seen[d]) {
                A.seen[d] = t->gen;
                dj_ana_track(t);
                did = true;
            }
        }
        if (!did) aos_hal_sleep_ms(20);
    }
    A.done = true;
}

bool dj_ana_start(dj_engine_t *e)
{
    if (A.running) return true;
    A.e = e;
    A.quit = false;
    A.done = false;
    for (int d = 0; d < DJ_NDECK; d++) A.seen[d] = 0;
    if (!aos_hal_thread_start("dj_ana", ana_main, NULL, 8192, 2)) {
        aos_hal_log(DJ_TAG, "no analysis thread");
        return false;
    }
    A.running = true;
    return true;
}

void dj_ana_stop(void)
{
    if (!A.running) return;
    A.quit = true;
    for (int i = 0; i < 600 && !A.done; i++) aos_hal_sleep_ms(5);
    if (!A.done) aos_hal_log(DJ_TAG, "the analysis thread did not stop");
    A.running = false;
}
