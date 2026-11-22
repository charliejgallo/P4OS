/*
 * Estudio - files: the user's samples in, the song out as WAV and as MIDI.
 *
 * Samples: any PCM WAV (8, 16, 24 or 32 bits, or 32-bit float; mono or
 * stereo; any rate) from <card>/estudio/muestras, mixed down to mono 16-bit
 * in PSRAM at their own rate; the drum voice resamples as it plays. At most
 * ES_SAMPLE_SEC seconds each and ES_BANK_MAX bytes in all. They are loaded
 * from the UI's thread (when a track picks one, when a project opens) and
 * the audio thread only looks them up: a file read in the audio thread is a
 * hole in the sound. The bank only grows while the app lives; it is freed
 * when the engine has stopped.
 *
 * The export: a WAV writer that fixes its sizes on close, and a standard
 * MIDI file (type 1, 96 ticks a quarter) with a track per part on channels
 * 1-3 and the drums on channel 10 with General MIDI notes.
 */
#include "es.h"

#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ES_SAMPLE_SEC 12
#define ES_BANK_MAX   (12u * 1024u * 1024u)

static es_sample_t       s_bank[ES_BANK];
static volatile int      s_bank_n;
static uint32_t          s_bank_bytes;

static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

const es_sample_t *es_bank_find(const char *name)
{
    if (!name || !*name) return NULL;
    int n = s_bank_n;
    for (int i = 0; i < n; i++)
        if (!strcmp(s_bank[i].name, name)) return &s_bank[i];
    return NULL;
}

uint32_t es_bank_bytes(void) { return s_bank_bytes; }

void es_bank_free_all(void)
{
    for (int i = 0; i < s_bank_n; i++) es_free(s_bank[i].data);
    memset(s_bank, 0, sizeof s_bank);
    s_bank_n = 0;
    s_bank_bytes = 0;
}

static void set_err(char *err, size_t n, const char *m)
{
    if (err && n) snprintf(err, n, "%s", m);
}

const es_sample_t *es_bank_load(const char *name, char *err, size_t errn)
{
    const es_sample_t *have = es_bank_find(name);
    if (have) return have;
    if (s_bank_n >= ES_BANK) { set_err(err, errn, N_("demasiadas muestras")); return NULL; }
    char path[320];
    es_path(path, sizeof path, "muestras", name);
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errn, N_("no se pudo abrir")); return NULL; }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        set_err(err, errn, N_("no es un WAV"));
        return NULL;
    }
    int fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0, data_len = 0;
    long data_at = 0;
    uint8_t c[8];
    while (fread(c, 1, 8, f) == 8) {
        uint32_t len = rd32(c + 4);
        long at = ftell(f);
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            uint8_t b[40];
            size_t n = fread(b, 1, len < 40 ? len : 40, f);
            if (n >= 16) {
                fmt = rd16(b);
                ch = rd16(b + 2);
                rate = rd32(b + 4);
                bits = rd16(b + 14);
                if (fmt == 0xFFFE && n >= 26) fmt = rd16(b + 24);     /* the extensible one's subformat */
            }
        } else if (!memcmp(c, "data", 4)) {
            data_at = at;
            data_len = len;
            break;
        }
        fseek(f, at + (long)len + (long)(len & 1), SEEK_SET);
    }
    bool okfmt = (fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32);
    if (!data_at || !okfmt || ch < 1 || ch > 8 || rate < 4000 || rate > 192000) {
        fclose(f);
        set_err(err, errn, N_("formato de WAV que no se lee (PCM o float)"));
        return NULL;
    }
    int bps = bits / 8, frame = bps * ch;
    uint32_t frames = data_len / (uint32_t)frame;
    uint32_t cap = rate * ES_SAMPLE_SEC;
    if (frames > cap) frames = cap;
    if (frames < 2) { fclose(f); set_err(err, errn, N_("está vacío")); return NULL; }
    uint32_t bytes = (frames + 1) * 2;
    if (s_bank_bytes + bytes > ES_BANK_MAX) { fclose(f); set_err(err, errn, N_("no hay más lugar para muestras")); return NULL; }
    int16_t *out = es_alloc(bytes);
    if (!out) { fclose(f); set_err(err, errn, N_("sin memoria")); return NULL; }
    fseek(f, data_at, SEEK_SET);
    uint8_t *buf = es_alloc(4096);
    if (!buf) { es_free(out); fclose(f); set_err(err, errn, N_("sin memoria")); return NULL; }
    uint32_t done = 0;
    int per = 4096 / frame;
    while (done < frames) {
        uint32_t want = frames - done;
        if (want > (uint32_t)per) want = (uint32_t)per;
        size_t got = fread(buf, (size_t)frame, want, f);
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            const uint8_t *fr = buf + i * (size_t)frame;
            float acc = 0.0f;
            for (int k = 0; k < ch; k++) {
                const uint8_t *s = fr + k * bps;
                float v;
                if (fmt == 3) { uint32_t u = rd32(s); float fv; memcpy(&fv, &u, 4); v = fv; }
                else if (bits == 8) v = ((float)s[0] - 128.0f) / 128.0f;
                else if (bits == 16) v = (float)(int16_t)rd16(s) / 32768.0f;
                else if (bits == 24) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
                else v = (float)(int32_t)rd32(s) / 2147483648.0f;
                acc += v;
            }
            acc /= (float)ch;
            out[done + i] = (int16_t)es_clampf(acc * 32767.0f, -32768.0f, 32767.0f);
        }
        done += (uint32_t)got;
    }
    es_free(buf);
    fclose(f);
    if (done < 2) { es_free(out); set_err(err, errn, N_("está vacío")); return NULL; }
    out[done] = out[done - 1];
    es_sample_t *s = &s_bank[s_bank_n];
    snprintf(s->name, sizeof s->name, "%s", name);
    s->data = out;
    s->len = done;
    s->rate = rate;
    s_bank_bytes += bytes;
    __asm__ volatile("" ::: "memory");
    s_bank_n = s_bank_n + 1;
    return s;
}

/* ---- WAV out ---- */

typedef struct { FILE *f; uint32_t n, rate; } wavw_t;

static void wr32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }

static void wav_header(uint8_t *h, uint32_t rate, uint32_t samples)
{
    memcpy(h, "RIFF", 4);
    wr32(h + 4, 36 + samples * 2);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    wr16(h + 20, 1);
    wr16(h + 22, 1);
    wr32(h + 24, rate);
    wr32(h + 28, rate * 2);
    wr16(h + 32, 2);
    wr16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    wr32(h + 40, samples * 2);
}

bool es_wav_write_open(const char *path, uint32_t rate, void **out)
{
    *out = NULL;
    wavw_t *w = es_alloc(sizeof *w);
    if (!w) return false;
    w->f = fopen(path, "wb");
    if (!w->f) { es_free(w); return false; }
    w->rate = rate;
    uint8_t h[44];
    wav_header(h, rate, 0);
    if (fwrite(h, 1, 44, w->f) != 44) { fclose(w->f); es_free(w); return false; }
    *out = w;
    return true;
}

bool es_wav_write(void *hh, const int16_t *pcm, uint32_t n)
{
    wavw_t *w = hh;
    if (!w || !w->f) return false;
    /* little-endian on both the board and the Mac: the samples go as they are */
    if (fwrite(pcm, 2, n, w->f) != n) return false;
    w->n += n;
    return true;
}

bool es_wav_write_close(void *hh)
{
    wavw_t *w = hh;
    if (!w) return false;
    bool ok = w->f && !ferror(w->f);
    if (w->f) {
        uint8_t h[44];
        wav_header(h, w->rate, w->n);
        if (fseek(w->f, 0, SEEK_SET) != 0 || fwrite(h, 1, 44, w->f) != 44) ok = false;
        if (fclose(w->f) != 0) ok = false;
    }
    es_free(w);
    return ok;
}

/* ---- MIDI out ---- */

#define PPQ 96

typedef struct { uint32_t tick; uint16_t ord; uint8_t trk, st, d1, d2; } mev_t;

static int mev_cmp(const void *a, const void *b)
{
    const mev_t *x = a, *y = b;
    if (x->trk != y->trk) return x->trk - y->trk;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->ord - y->ord;
}

static void vlq(FILE *f, uint32_t v)
{
    uint8_t b[5];
    int n = 0;
    b[n++] = (uint8_t)(v & 0x7F);
    while (v >>= 7) b[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n) fputc(b[--n], f);
}

static void be32(FILE *f, uint32_t v) { fputc((int)(v >> 24), f); fputc((int)(v >> 16 & 255), f); fputc((int)(v >> 8 & 255), f); fputc((int)(v & 255), f); }

/* one track chunk with its length fixed afterwards */
static long trk_begin(FILE *f)
{
    fputs("MTrk", f);
    long at = ftell(f);
    be32(f, 0);
    return at;
}

static void trk_end(FILE *f, long at)
{
    fputc(0, f); fputc(0xFF, f); fputc(0x2F, f); fputc(0, f);
    long end = ftell(f);
    fseek(f, at, SEEK_SET);
    be32(f, (uint32_t)(end - at - 4));
    fseek(f, end, SEEK_SET);
}

static void meta_text(FILE *f, int type, const char *s)
{
    size_t n = strlen(s);
    vlq(f, 0);
    fputc(0xFF, f);
    fputc(type, f);
    vlq(f, (uint32_t)n);
    fwrite(s, 1, n, f);
}

bool es_midi_export(const es_proj_t *p, const char *path, int pat, int loops)
{
    int order[ES_CHAIN * 4];
    int no = 0;
    if (p->song_mode && p->chain_n)
        for (int i = 0; i < p->chain_n; i++) order[no++] = p->chain[i] % ES_PATS;
    else
        for (int i = 0; i < (loops < 1 ? 1 : loops > 16 ? 16 : loops); i++) order[no++] = pat % ES_PATS;
    size_t cap = 4096, n = 0;
    mev_t *ev = es_alloc(cap * sizeof *ev);
    if (!ev) return false;
    uint32_t t0 = 0;
    uint32_t sw = (uint32_t)((p->swing - 0.5f) * 2.0f * (PPQ / 4) + 0.5f);
    bool ok = true;
#define ADD(tk, tr, s, a, b, o) do { \
        if (n == cap) { mev_t *nv = es_alloc(cap * 2 * sizeof *ev); if (!nv) { ok = false; break; } \
                        memcpy(nv, ev, cap * sizeof *ev); es_free(ev); ev = nv; cap *= 2; } \
        ev[n++] = (mev_t){ (tk), (uint16_t)(o), (uint8_t)(tr), (uint8_t)(s), (uint8_t)(a), (uint8_t)(b) }; } while (0)
    for (int k = 0; k < no && ok; k++) {
        const es_pat_t *t = &p->pat[order[k]];
        int len = t->len < 1 ? 16 : t->len;
        for (int s = 0; s < len && ok; s++) {
            uint32_t tk = t0 + (uint32_t)s * (PPQ / 4) + ((s & 1) ? sw : 0);
            for (int tr = 0; tr < ES_DRUMS && ok; tr++) {
                es_dstep_t d = t->d[tr][s];
                if (!d.vel || p->dt[tr].mute) continue;
                int gm = es_drum_gm(p->dt[tr].src);
                if (p->dt[tr].src >= DS_COUNT) gm = 60 + tr;
                ADD(tk, 4, 0x99, gm, d.vel, 1);
                ADD(tk + PPQ / 8, 4, 0x89, gm, 0, 0);
            }
            for (int part = 0; part < ES_PARTS && ok; part++) {
                if (p->ch[part].mute) continue;
                for (int i = 0; i < t->nn[part] && ok; i++) {
                    const es_note_t *nt = &t->n[part][i];
                    if (nt->step != s) continue;
                    uint32_t dur = (uint32_t)nt->len * (PPQ / 4);
                    dur = dur > 4 ? dur - 2 : dur;
                    ADD(tk, part + 1, 0x90 | part, nt->note, nt->vel, 1);
                    ADD(tk + dur, part + 1, 0x80 | part, nt->note, 0, 0);
                }
            }
        }
        t0 += (uint32_t)len * (PPQ / 4);
    }
#undef ADD
    if (!ok) { es_free(ev); return false; }
    qsort(ev, n, sizeof *ev, mev_cmp);

    char tmp[340];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { es_free(ev); return false; }
    fputs("MThd", f);
    be32(f, 6);
    fputc(0, f); fputc(1, f);           /* type 1 */
    fputc(0, f); fputc(5, f);           /* tempo track, three parts, drums */
    fputc(0, f); fputc(PPQ, f);
    long at = trk_begin(f);
    meta_text(f, 0x03, p->name);
    uint32_t us = (uint32_t)(60000000.0f / p->bpm);
    vlq(f, 0); fputc(0xFF, f); fputc(0x51, f); fputc(3, f);
    fputc((int)(us >> 16 & 255), f); fputc((int)(us >> 8 & 255), f); fputc((int)(us & 255), f);
    vlq(f, 0); fputc(0xFF, f); fputc(0x58, f); fputc(4, f);
    fputc(p->beats ? p->beats : 4, f); fputc(2, f); fputc(24, f); fputc(8, f);
    trk_end(f, at);
    size_t i = 0;
    for (int tr = 1; tr <= 4; tr++) {
        at = trk_begin(f);
        char nm[ES_NAME + 8];
        if (tr < 4) snprintf(nm, sizeof nm, "%d %s", tr, p->part[tr - 1].name);
        else snprintf(nm, sizeof nm, "%s", "Drums");
        meta_text(f, 0x03, nm);
        uint32_t last = 0;
        for (; i < n && ev[i].trk == tr; i++) {
            vlq(f, ev[i].tick - last);
            last = ev[i].tick;
            fputc(ev[i].st, f);
            fputc(ev[i].d1 & 127, f);
            fputc(ev[i].d2 & 127, f);
        }
        trk_end(f, at);
    }
    es_free(ev);
    ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }
    remove(path);
    return rename(tmp, path) == 0;
}
