/*
 * Estudio - the audio thread and everything with a clock in it.
 *
 * The thread keeps the speaker's ring topped up to a target (the latency
 * setting) one block at a time: render ES_BLK samples, write them, look
 * again; with the ring full it sleeps a millisecond. On the board the
 * mixer takes 480 samples (10 ms) at a time from that ring, so the target is
 * 480 plus a margin: less and the mixer finds it short and plays a hole.
 *
 * Every block starts by draining the two command queues (the UI's and the
 * MIDI thread's), then moves the transport: the sequencer fires the steps
 * whose time has come (the odd sixteenths late by the swing), the metronome
 * clicks, the arpeggiator plays, the MIDI clock ticks. The events land at
 * the start of a block, 0.67 ms of resolution, well under what a finger or
 * an ear tells apart.
 *
 * Recording: a key played while recording goes to the pattern at the
 * nearest sixteenth. When that step is still ahead, the sequencer would play
 * it a second time a moment later; a skip list keyed by the step's absolute
 * number stops that, once.
 *
 * The same engine, without the thread and the speaker, is the WAV export:
 * a copy of the project rendered block by block as fast as it goes.
 */
#include "es_eng.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif

/* ---- memory and small helpers ---- */

void *es_alloc(size_t bytes)
{
#if defined(AOS_SIM)
    void *p = malloc(bytes);
#else
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p) memset(p, 0, bytes);
    return p;
}

void es_free(void *p)
{
    if (!p) return;
#if defined(AOS_SIM)
    free(p);
#else
    heap_caps_free(p);
#endif
}

uint32_t es_rand(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x6D2B79F5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

float es_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- the queues ---- */

/* a full fence between the slot and the index; spelled out on the board,
 * where the loader's table has no __sync_synchronize to resolve */
#if defined(__riscv)
#define ES_FENCE() __asm__ volatile("fence rw,rw" ::: "memory")
#else
#define ES_FENCE() __sync_synchronize()
#endif

static bool q_push(es_cmdq_t *q, const es_cmd_t *c)
{
    uint32_t h = q->head;
    if (h - q->tail >= ES_QN) return false;
    q->q[h & (ES_QN - 1)] = *c;
    ES_FENCE();
    q->head = h + 1;
    return true;
}

static bool q_pop(es_cmdq_t *q, es_cmd_t *c)
{
    uint32_t t = q->tail;
    if (t == q->head) return false;
    ES_FENCE();
    *c = q->q[t & (ES_QN - 1)];
    ES_FENCE();
    q->tail = t + 1;
    return true;
}

bool es_cmd(es_engine_t *e, uint8_t type, int a, int b, int c, float f)
{
    if (!e) return false;
    es_cmd_t m = { type, (uint8_t)a, (uint8_t)b, (uint8_t)c, f };
    return q_push(&e->qui, &m);
}

bool es_cmd_midi(es_engine_t *e, uint8_t type, int a, int b, int c, float f)
{
    if (!e) return false;
    es_cmd_t m = { type, (uint8_t)a, (uint8_t)b, (uint8_t)c, f };
    return q_push(&e->qmidi, &m);
}

static void ev_push(es_engine_t *e, uint8_t type, int pat, int a, int b, int c, int step)
{
    if (!e->live) return;
    uint32_t h = e->ev_h;
    if (h - e->ev_t >= ES_EVN) return;
    es_ev_t *v = &e->evq[h & (ES_EVN - 1)];
    v->type = type; v->pat = (uint8_t)pat; v->a = (uint8_t)a; v->b = (uint8_t)b;
    v->c = (uint8_t)c; v->step = (uint8_t)step;
    ES_FENCE();
    e->ev_h = h + 1;
}

bool es_ev_take(es_engine_t *e, es_ev_t *out)
{
    uint32_t t = e->ev_t;
    if (t == e->ev_h) return false;
    ES_FENCE();
    *out = e->evq[t & (ES_EVN - 1)];
    ES_FENCE();
    e->ev_t = t + 1;
    return true;
}

static void mout(es_engine_t *e, uint8_t st, uint8_t d1, uint8_t d2, uint8_t dest)
{
    if (!e->live || !dest) return;
    uint32_t h = e->mo_h;
    if (h - e->mo_t >= ES_MON) return;
    es_mout_t *m = &e->moq[h & (ES_MON - 1)];
    m->st = st; m->d1 = d1; m->d2 = d2; m->dest = dest;
    ES_FENCE();
    e->mo_h = h + 1;
}

bool es_mout_take(es_engine_t *e, es_mout_t *out)
{
    uint32_t t = e->mo_t;
    if (t == e->mo_h) return false;
    ES_FENCE();
    *out = e->moq[t & (ES_MON - 1)];
    ES_FENCE();
    e->mo_t = t + 1;
    return true;
}

/* where the clock, Start and Stop go: the device on the host, and the
 * computer when it gets everything */
static uint8_t clock_dest(const es_engine_t *e)
{
    return (uint8_t)((e->mo_dest & 2) | ((e->mo_dest & 1) && e->mo_pc_track == ES_PC_ALL ? 4 : 0));
}

/* a note out: to the device on the host on the track's channel, and to the
 * computer the same way when it gets everything, or on channel 1 if it is
 * the one track it gets. 'len' in samples (< 0: the note-off comes on its
 * own) */
static void mout_note(es_engine_t *e, int track, int note, int vel, int32_t len, int src)
{
    if (!e->mo_dest) return;
    uint8_t ch = track < ES_PARTS ? (uint8_t)track : 9;
    uint8_t dest = 0;
    if (e->mo_dest & 2) dest |= 2;
    if ((e->mo_dest & 1) && src != SRC_MIDI) {
        if (e->mo_pc_track == ES_PC_ALL) dest |= 4;
        else if (track == e->mo_pc_track) dest |= 1;
    }
    if (!dest) return;
    if (vel > 0) {
        mout(e, (uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)(vel > 127 ? 127 : vel), dest);
        if (len >= 0) {
            for (int i = 0; i < ES_MOFF; i++)
                if (!e->moff[i].dest) {
                    e->moff[i] = (es_moff_t){ (uint8_t)note, ch, dest, len };
                    break;
                }
        }
    } else {
        mout(e, (uint8_t)(0x80 | ch), (uint8_t)note, 0, dest);
    }
}

static void moff_block(es_engine_t *e, bool all)
{
    for (int i = 0; i < ES_MOFF; i++) {
        es_moff_t *m = &e->moff[i];
        if (!m->dest) continue;
        m->left -= ES_BLK;
        if (m->left < 0 || all) {
            mout(e, (uint8_t)(0x80 | m->ch), m->note, 0, m->dest);
            m->dest = 0;
        }
    }
}

/* ---- voices ---- */

static es_voice_t *voice_alloc(es_engine_t *e)
{
    int n = e->nvoices, best = -1;
    for (int i = 0; i < n; i++) if (!e->v[i].on) return &e->v[i];
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < n; i++)
        if (!e->v[i].gate && e->v[i].age < oldest) { oldest = e->v[i].age; best = i; }
    if (best < 0)
        for (int i = 0; i < n; i++)
            if (e->v[i].age < oldest) { oldest = e->v[i].age; best = i; }
    return &e->v[best < 0 ? 0 : best];
}

static void stack_push(es_partst_t *ps, int note)
{
    int j = 0;
    for (int i = 0; i < ps->stack_n; i++) if (ps->stack[i] != note) ps->stack[j++] = ps->stack[i];
    ps->stack_n = (uint8_t)j;
    if (ps->stack_n == 8) {
        memmove(ps->stack, ps->stack + 1, 7);
        ps->stack_n = 7;
    }
    ps->stack[ps->stack_n++] = (uint8_t)note;
}

static void stack_pop(es_partst_t *ps, int note)
{
    int j = 0;
    for (int i = 0; i < ps->stack_n; i++) if (ps->stack[i] != note) ps->stack[j++] = ps->stack[i];
    ps->stack_n = (uint8_t)j;
}

static float vgain(const float *P, float vel)
{
    return P[P_VOL] * ((1.0f - P[P_VELAMP]) + P[P_VELAMP] * vel);
}

static void voice_on(es_engine_t *e, int part, int note, float vel, int32_t off_in)
{
    const float *P = e->p->part[part].p;
    es_partst_t *ps = &e->ps[part];
    bool glide = P[P_GLIDE] > 0.002f;
    if (P[P_MONO] >= 0.5f) {
        stack_push(ps, note);
        es_voice_t *v = NULL;
        for (int i = 0; i < ES_VOICES; i++)
            if (e->v[i].on && e->v[i].part == part) { v = &e->v[i]; break; }
        if (v) {
            bool legato = v->gate;
            v->note = (uint8_t)note;
            v->pitch_to = (float)note;
            if (!glide) v->pitch = (float)note;
            v->off_in = off_in;
            v->sus = 0;
            ps->last_pitch = (float)note;
            if (!legato) {
                v->gate = 1;
                v->vel = vel;
                v->gain = vgain(P, vel);
                v->age = ++e->age;
                es_voice_start(e, v, false);
            }
            return;
        }
    }
    es_voice_t *v = voice_alloc(e);
    bool fresh = !v->on;
    v->on = 1;
    v->gate = 1;
    v->sus = 0;
    v->part = (uint8_t)part;
    v->note = (uint8_t)note;
    v->age = ++e->age;
    v->vel = vel;
    v->gain = vgain(P, vel);
    v->pitch = glide && ps->last_pitch > 0.0f ? ps->last_pitch : (float)note;
    v->pitch_to = (float)note;
    v->off_in = off_in;
    v->rng = es_rand(&e->rng) | 1u;
    ps->last_pitch = (float)note;
    es_voice_start(e, v, fresh);
}

static void voice_off(es_engine_t *e, int part, int note)
{
    const float *P = e->p->part[part].p;
    es_partst_t *ps = &e->ps[part];
    if (P[P_MONO] >= 0.5f) {
        stack_pop(ps, note);
        for (int i = 0; i < ES_VOICES; i++) {
            es_voice_t *v = &e->v[i];
            if (!v->on || v->part != part || !v->gate || v->note != note) continue;
            if (ps->stack_n) {
                int top = ps->stack[ps->stack_n - 1];
                v->note = (uint8_t)top;
                v->pitch_to = (float)top;
                if (P[P_GLIDE] <= 0.002f) v->pitch = (float)top;
                ps->last_pitch = (float)top;
            } else if (ps->sustain) v->sus = 1;
            else v->gate = 0;
        }
        return;
    }
    for (int i = 0; i < ES_VOICES; i++) {
        es_voice_t *v = &e->v[i];
        if (v->on && v->gate && v->part == part && v->note == note && v->off_in < 0) {
            if (ps->sustain) v->sus = 1;
            else v->gate = 0;
        }
    }
}

static void all_off(es_engine_t *e, bool hard)
{
    for (int i = 0; i < ES_VOICES; i++) {
        e->v[i].gate = 0;
        e->v[i].sus = 0;
        if (hard) e->v[i].on = 0;
    }
    for (int p = 0; p < ES_PARTS; p++) {
        e->ps[p].stack_n = 0;
        e->ps[p].sustain = 0;
    }
    if (hard)
        for (int t = 0; t < ES_DRUMS; t++) { e->dv[t][0].on = 0; e->dv[t][1].on = 0; }
    moff_block(e, true);
}

/* ---- the transport ---- */

static float sps(const es_engine_t *e)          /* samples per sixteenth */
{
    return (float)ES_SR * 15.0f / e->p->bpm;
}

static float step_t(const es_engine_t *e, int s)
{
    float sw = (e->p->swing - 0.5f) * 2.0f;
    return (float)s + ((s & 1) ? sw : 0.0f);
}

static bool roll(es_engine_t *e, int prob)
{
    if (prob >= 100) return true;
    return (int)(es_rand(&e->rng) % 100u) < prob;
}

static bool skipped(es_engine_t *e, int kind, int id)
{
    for (int i = 0; i < ES_SKIPN; i++) {
        es_skip_t *s = &e->skip[i];
        if (s->kind < 0) continue;
        if (s->abs < e->abs_step) { s->kind = -1; continue; }
        if (s->abs == e->abs_step && s->kind == kind && s->id == id) { s->kind = -1; return true; }
    }
    return false;
}

static void skip_add(es_engine_t *e, int kind, int id)
{
    for (int i = 0; i < ES_SKIPN; i++)
        if (e->skip[i].kind < 0 || e->skip[i].abs < e->abs_step) {
            e->skip[i] = (es_skip_t){ (int8_t)kind, (uint8_t)id, e->abs_step };
            return;
        }
}

static int pat_len(const es_engine_t *e)
{
    int l = e->p->pat[e->cur_pat].len;
    return l < 1 ? 1 : l > ES_STEPS ? ES_STEPS : l;
}

static void fire(es_engine_t *e, int step)
{
    es_proj_t *p = e->p;
    es_pat_t *t = &p->pat[e->cur_pat];
    e->step_now = (int16_t)step;
    if (p->met_on && (step & 3) == 0) es_click(e, ((step >> 2) % (p->beats ? p->beats : 4)) == 0);
    for (int tr = 0; tr < ES_DRUMS; tr++) {
        es_dstep_t s = t->d[tr][step];
        if (!s.vel || p->dt[tr].mute) continue;
        if (!roll(e, s.prob)) continue;
        if (skipped(e, 3, tr)) continue;
        es_drum_hit(e, tr, (float)s.vel * (1.0f / 127.0f));
        mout_note(e, 3, es_drum_gm(p->dt[tr].src), s.vel, ES_SR / 50, SRC_LIVE);
    }
    float sp = sps(e);
    for (int part = 0; part < ES_PARTS; part++) {
        if (p->ch[part].mute) continue;
        int nn = t->nn[part];
        for (int i = 0; i < nn && i < ES_NOTES; i++) {
            es_note_t n = t->n[part][i];
            if (n.step != step || !n.vel) continue;
            if (!roll(e, n.prob)) continue;
            if (skipped(e, part, n.note)) continue;
            int32_t off = (int32_t)((float)(n.len ? n.len : 1) * sp * 0.92f);
            voice_on(e, part, n.note, (float)n.vel * (1.0f / 127.0f), off);
            mout_note(e, part, n.note, n.vel, off, SRC_LIVE);
        }
    }
    e->abs_step++;
}

static void pattern_end(es_engine_t *e)
{
    es_proj_t *p = e->p;
    e->pos -= (float)pat_len(e);
    e->next_step = 0;
    if (p->song_mode && p->chain_n) {
        int c = e->chain_pos + 1;
        if (c >= p->chain_n) { c = 0; e->ended = true; }
        e->chain_pos = (int8_t)c;
        e->cur_pat = (int8_t)(p->chain[c] % ES_PATS);
    } else {
        if (e->cue_pat >= 0) { e->cur_pat = e->cue_pat; e->cue_pat = -1; }
        if (e->loops_left > 0 && --e->loops_left == 0) e->ended = true;
    }
}

static void play(es_engine_t *e)
{
    if (e->playing) return;
    es_proj_t *p = e->p;
    e->pos = 0.0f;
    e->next_step = 0;
    e->abs_step = 0;
    e->abs_pos = 0.0f;
    e->clk_next = 0.0f;
    e->ended = false;
    e->cue_pat = -1;
    if (p->song_mode && p->chain_n) {
        e->chain_pos = 0;
        e->cur_pat = (int8_t)(p->chain[0] % ES_PATS);
    }
    for (int i = 0; i < ES_SKIPN; i++) e->skip[i].kind = -1;
    if (e->rec && e->live) {
        int beats = p->beats ? p->beats : 4;
        e->pos = -4.0f * (float)beats;
        e->cin_next = e->pos;
        e->countin = 1;
    }
    e->arp_next = 0.0f;
    e->arp_clock = e->pos;
    e->playing = 1;
    if (e->mo_clock && clock_dest(e)) mout(e, 0xFA, 0, 0, clock_dest(e));
}

static void stop(es_engine_t *e)
{
    if (!e->playing) return;
    e->playing = 0;
    e->countin = 0;
    e->step_now = -1;
    for (int i = 0; i < ES_VOICES; i++)
        if (e->v[i].on && e->v[i].off_in >= 0) { e->v[i].gate = 0; e->v[i].off_in = -1; }
    moff_block(e, true);
    if (e->mo_clock && clock_dest(e)) mout(e, 0xFC, 0, 0, clock_dest(e));
    for (int p = 0; p < ES_PARTS; p++)
        for (int n = 0; n < 128; n++) e->rec_start[p][n] = -1;
}

/* where a key played now goes in the pattern; *ahead: that step has not
 * played yet */
static int rec_step_now(es_engine_t *e, bool *ahead)
{
    int len = pat_len(e);
    int q = (int)floorf(e->pos + 0.5f);
    *ahead = q >= e->next_step;
    if (q >= len) q -= len;
    if (q < 0) q = 0;
    return q;
}

static void rec_on(es_engine_t *e, int part, int note, int vel)
{
    if (!e->rec || !e->playing || e->countin) return;
    bool ahead;
    int st = rec_step_now(e, &ahead);
    if (ahead) skip_add(e, part, note);
    ev_push(e, EV_REC_ON, e->cur_pat, part, note, vel, st);
    e->rec_start[part][note & 127] = (int32_t)floorf(e->abs_pos + 0.5f);
    e->rec_step[part][note & 127] = (uint8_t)st;
}

static void rec_off(es_engine_t *e, int part, int note)
{
    int32_t s0 = e->rec_start[part][note & 127];
    if (s0 < 0) return;
    e->rec_start[part][note & 127] = -1;
    int len = (int)floorf(e->abs_pos + 0.5f) - s0;
    if (len < 1) len = 1;
    if (len > pat_len(e)) len = pat_len(e);
    ev_push(e, EV_REC_OFF, e->cur_pat, part, note, len, e->rec_step[part][note & 127]);
}

/* ---- the arpeggiator ---- */

static float arp_rate(int r)
{
    static const float R[AR_COUNT] = { 2.0f, 4.0f / 3.0f, 1.0f, 2.0f / 3.0f, 0.5f };
    return R[r < 0 || r >= AR_COUNT ? AR_16 : r];
}

static void arp_add(es_engine_t *e, int note, int vel)
{
    if (e->p->arp_hold && e->arp_keys_down == 0) e->arp_n = 0;
    e->arp_keys_down++;
    for (int i = 0; i < e->arp_n; i++) if (e->arp_note[i] == note) return;
    if (e->arp_n >= ES_ARPN) return;
    bool first = e->arp_n == 0;
    e->arp_note[e->arp_n] = (uint8_t)note;
    e->arp_vel[e->arp_n] = (uint8_t)vel;
    e->arp_n++;
    if (first) {
        e->arp_i = 0;
        e->arp_part = (uint8_t)e->live_part;
        if (!e->playing) e->arp_next = e->arp_clock;     /* the first note at once */
    }
}

static void arp_remove(es_engine_t *e, int note)
{
    if (e->arp_keys_down) e->arp_keys_down--;
    if (e->p->arp_hold) return;
    int j = 0;
    for (int i = 0; i < e->arp_n; i++)
        if (e->arp_note[i] != note) { e->arp_note[j] = e->arp_note[i]; e->arp_vel[j] = e->arp_vel[i]; j++; }
    e->arp_n = (uint8_t)j;
}

static void arp_clear(es_engine_t *e)
{
    e->arp_n = 0;
    e->arp_keys_down = 0;
}

static void arp_play(es_engine_t *e, float rate)
{
    es_proj_t *p = e->p;
    uint8_t seq[ES_ARPN * 3];
    uint8_t vel[ES_ARPN * 3];
    int n = e->arp_n, oct = p->arp_oct < 1 ? 1 : p->arp_oct > 3 ? 3 : p->arp_oct;
    uint8_t base[ES_ARPN], bvel[ES_ARPN];
    memcpy(base, e->arp_note, (size_t)n);
    memcpy(bvel, e->arp_vel, (size_t)n);
    if (p->arp_mode != ARP_PLAYED)
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && base[j - 1] > base[j]; j--) {
                uint8_t t = base[j]; base[j] = base[j - 1]; base[j - 1] = t;
                t = bvel[j]; bvel[j] = bvel[j - 1]; bvel[j - 1] = t;
            }
    int total = 0;
    for (int o = 0; o < oct; o++)
        for (int i = 0; i < n; i++) {
            int nt = base[i] + 12 * o;
            if (nt > 127) continue;
            seq[total] = (uint8_t)nt;
            vel[total] = bvel[i];
            total++;
        }
    if (!total) return;
    int k;
    switch (p->arp_mode) {
    case ARP_DOWN: k = total - 1 - (e->arp_i % total); break;
    case ARP_UPDOWN: {
        int per = total > 1 ? 2 * total - 2 : 1;
        int i = e->arp_i % per;
        k = i < total ? i : per - i;
        break;
    }
    case ARP_RANDOM: k = (int)(es_rand(&e->rng) % (uint32_t)total); break;
    default: k = e->arp_i % total; break;
    }
    e->arp_i++;
    int part = e->arp_part;
    int32_t off = (int32_t)(rate * sps(e) * 0.5f);
    voice_on(e, part, seq[k], (float)vel[k] * (1.0f / 127.0f), off);
    mout_note(e, part, seq[k], vel[k], off, SRC_LIVE);
    if (e->rec && e->playing && !e->countin) {
        bool ahead;
        int st = rec_step_now(e, &ahead);
        if (ahead) skip_add(e, part, seq[k]);
        int len = (int)floorf(rate + 0.5f);
        ev_push(e, EV_REC_ON, e->cur_pat, part, seq[k], vel[k], st);
        ev_push(e, EV_REC_OFF, e->cur_pat, part, seq[k], len < 1 ? 1 : len, st);
    }
}

static void arp_block(es_engine_t *e, float adv)
{
    if (e->playing) e->arp_clock = e->countin ? e->pos : e->abs_pos;
    else e->arp_clock += adv;
    if (!e->p->arp_on || !e->arp_n) return;
    if (e->playing && e->countin) return;        /* not in the count-in */
    float rate = arp_rate(e->p->arp_rate);
    if (e->arp_clock + 1e-4f >= e->arp_next) {
        float k = floorf((e->arp_clock + 1e-4f) / rate) + 1.0f;
        e->arp_next = k * rate;
        arp_play(e, rate);
    }
}

/* ---- the commands ---- */

static void note_in(es_engine_t *e, int part, int note, int vel, int src)
{
    if (part < 0 || part >= ES_PARTS || note > 127) return;
    if (e->p->arp_on && part == e->live_part) {
        arp_add(e, note, vel);
        return;
    }
    voice_on(e, part, note, (float)vel * (1.0f / 127.0f), -1);
    mout_note(e, part, note, vel, -1, src);
    rec_on(e, part, note, vel);
}

static void note_out(es_engine_t *e, int part, int note, int src)
{
    if (part < 0 || part >= ES_PARTS || note > 127) return;
    if (e->p->arp_on && part == e->live_part) {
        arp_remove(e, note);
        return;
    }
    voice_off(e, part, note);
    mout_note(e, part, note, 0, -1, src);
    rec_off(e, part, note);
}

static void drum_in(es_engine_t *e, int track, int vel, int src)
{
    if (track < 0 || track >= ES_DRUMS) return;
    es_drum_hit(e, track, (float)vel * (1.0f / 127.0f));
    mout_note(e, 3, es_drum_gm(e->p->dt[track].src), vel, ES_SR / 50, src);
    if (e->rec && e->playing && !e->countin) {
        bool ahead;
        int st = rec_step_now(e, &ahead);
        if (ahead) skip_add(e, 3, track);
        ev_push(e, EV_REC_DRUM, e->cur_pat, track, 0, vel, st);
    }
}

static void do_cmd(es_engine_t *e, const es_cmd_t *c)
{
    int src = (int)c->f;
    switch (c->type) {
    case EC_NOTE_ON:  note_in(e, c->a, c->b, c->c ? c->c : 100, src); break;
    case EC_NOTE_OFF: note_out(e, c->a, c->b, src); break;
    case EC_DRUM:     drum_in(e, c->a, c->b ? c->b : 100, src); break;
    case EC_PLAY:     play(e); break;
    case EC_STOP:     stop(e); break;
    case EC_PANIC:    stop(e); arp_clear(e); all_off(e, true); break;
    case EC_BEND:     if (c->a < ES_PARTS) e->ps[c->a].bend = c->f; break;
    case EC_MOD:      if (c->a < ES_PARTS) e->ps[c->a].mod = c->f; break;
    case EC_SUSTAIN:
        if (c->a < ES_PARTS) {
            e->ps[c->a].sustain = c->b;
            if (!c->b)
                for (int i = 0; i < ES_VOICES; i++)
                    if (e->v[i].on && e->v[i].part == c->a && e->v[i].sus) { e->v[i].sus = 0; e->v[i].gate = 0; }
        }
        break;
    case EC_CUE_PAT:
        if (c->a >= ES_PATS) break;
        if (e->playing && !e->p->song_mode) e->cue_pat = (int8_t)(c->a == e->cur_pat ? -1 : c->a);
        else if (!e->playing) e->cur_pat = (int8_t)c->a;
        break;
    case EC_REC:
        e->rec = c->a ? 1 : 0;
        if (!e->rec)
            for (int p = 0; p < ES_PARTS; p++)
                for (int n = 0; n < 128; n++) e->rec_start[p][n] = -1;
        break;
    case EC_ARP_RESET:
        arp_clear(e);
        break;
    case EC_AUDITION:
        if (c->a < ES_PARTS) voice_on(e, c->a, c->b, (float)(c->c ? c->c : 100) / 127.0f,
                                      (int32_t)(c->f * (float)ES_SR / 1000.0f));
        break;
    }
}

/* one block: commands, the clock, the sound */
static void engine_block(es_engine_t *e, float *out)
{
    es_cmd_t c;
    while (q_pop(&e->qui, &c)) do_cmd(e, &c);
    while (q_pop(&e->qmidi, &c)) do_cmd(e, &c);

    es_proj_t *p = e->p;
    float adv = p->bpm * (4.0f / 60.0f) / (float)ES_SR * ES_BLK;
    arp_block(e, adv);
    if (e->playing) {
        if (e->countin) {
            /* the count-in: one bar of the metronome before it records */
            int beats = p->beats ? p->beats : 4;
            float start = -4.0f * (float)beats;
            while (e->cin_next < 0.0f && e->cin_next <= e->pos + 1e-4f) {
                es_click(e, e->cin_next - start < 0.5f);
                e->cin_next += 4.0f;
            }
            e->pos += adv;
            if (e->pos >= 0.0f) e->countin = 0;
        } else {
            /* the pattern comes round when its time is up, not as soon as
             * its last step played: until then a key still lands in it */
            for (int guard = 0; guard < 8 * ES_STEPS; guard++) {
                if (e->next_step >= pat_len(e)) {
                    if (e->pos < (float)pat_len(e)) break;
                    pattern_end(e);
                    continue;
                }
                if (step_t(e, e->next_step) > e->pos) break;
                fire(e, e->next_step);
                e->next_step++;
            }
            if (e->mo_clock && clock_dest(e))
                while (e->clk_next <= e->abs_pos) {
                    mout(e, 0xF8, 0, 0, clock_dest(e));
                    e->clk_next += 1.0f / 6.0f;
                }
            e->pos += adv;
            e->abs_pos += adv;
        }
    }
    moff_block(e, false);
    es_dsp_block(e, out);
}

/* ---- the thread ---- */

/* Whether the board gave the output for playing live (aos_hal_spk_open_low,
 * since P4OS 0.32): the mixer then takes 96 samples at a time and not 480,
 * and there are 10 ms after this queue where there were 40. Asked each
 * time: the board changes its output a moment after the stream opens, and
 * takes it back while something records. */
static bool out_low(void)
{
#if defined(AOS_SIM)
    return false;
#else
    return aos_hal_spk_latency_ms() <= 12;
#endif
}

static int lat_samples(int level)
{
    static const int L[3]   = { 480 + 96, 480 + 480, 480 + 960 };     /* 12, 20 and 30 ms: a 10 ms mixer block and some */
    static const int LOW[3] = { 96 + 96, 96 + 288, 96 + 864 };        /* 4, 8 and 20 ms: a 2 ms block and some */
    level = level < 0 ? 0 : level > 2 ? 2 : level;
    return out_low() ? LOW[level] : L[level];
}

int es_latency_ms(int level)
{
    return lat_samples(level) * 1000 / ES_SR;
}

static void audio_main(void *arg)
{
    es_engine_t *e = arg;
    float   buf[ES_BLK];
    int16_t pcm[ES_BLK];
    uint32_t win_t0 = (uint32_t)aos_hal_uptime_ms();
    uint32_t blocks = 0, us_sum = 0, us_max = 0;
    float peak = 0.0f;
    while (!e->quit) {
        int q = es_out_queued();
        e->queued = q;
        if (q + ES_BLK > lat_samples(e->lat_level)) {
            aos_hal_sleep_ms(1);
            continue;
        }
        if (q == 0 && e->started) e->underruns++;
        uint64_t t0 = aos_hal_uptime_us();
        engine_block(e, buf);
        uint32_t us = (uint32_t)(aos_hal_uptime_us() - t0);
        for (int i = 0; i < ES_BLK; i++) {
            float v = buf[i] * 32767.0f;
            pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        es_out_write(pcm, ES_BLK);
        e->started = true;
        blocks++;
        us_sum += us;
        if (us > us_max) us_max = us;
        if (e->st_peak > peak) peak = e->st_peak;
        e->st_peak = 0.0f;
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        if (now - win_t0 >= 1000 && blocks) {
            /* the block's audio time: ES_BLK / ES_SR = 667 us */
            float blk_us = (float)ES_BLK * 1e6f / (float)ES_SR;
            e->stats.blk_us_avg = (float)us_sum / (float)blocks;
            e->stats.blk_us_max = (float)us_max;
            e->stats.load_pct = 100.0f * e->stats.blk_us_avg / blk_us;
            e->stats.peak = peak;
#if defined(AOS_SIM)
            e->stats.underruns = e->underruns;
#else
            /* the board's own count: this ring found short by the mixer,
             * and the output itself late */
            e->stats.underruns = (uint32_t)aos_hal_spk_dry();
#endif
            e->stats.queued = q;
            blocks = us_sum = us_max = 0;
            peak = 0.0f;
            win_t0 = now;
        }
    }
    e->done = true;
}

es_engine_t *es_engine_new(es_proj_t *proj, bool live)
{
    es_engine_t *e = es_alloc(sizeof *e);
    if (!e) return NULL;
    e->p = proj;
    e->live = live;
    e->nvoices = ES_VOICES;
    e->lat_level = 1;
    e->rng = 0x2545F491u ^ (uint32_t)aos_hal_uptime_ms();
    e->cur_pat = 0;
    e->cue_pat = -1;
    e->step_now = -1;
    e->arp_last = -1;
    e->mo_pc_track = 0;
    for (int p = 0; p < ES_PARTS; p++)
        for (int n = 0; n < 128; n++) e->rec_start[p][n] = -1;
    for (int i = 0; i < ES_SKIPN; i++) e->skip[i].kind = -1;
    es_dsp_alloc(e);
    if (!e->dly || !e->rv) {
        es_dsp_release(e);
        es_free(e);
        return NULL;
    }
    return e;
}

void es_engine_free(es_engine_t *e)
{
    if (!e) return;
    es_engine_stop(e);
    es_dsp_release(e);
    es_free(e);
}

bool es_engine_running(const es_engine_t *e)
{
    return e && e->live && !e->done && !e->quit && e->started;
}

static bool s_thread_on;

bool es_engine_start(es_engine_t *e)
{
    if (!e || !e->live || s_thread_on) return false;
    if (!es_out_open()) {
        aos_hal_log(ES_TAG, "the speaker did not open");
        return false;
    }
    e->quit = false;
    e->done = false;
    e->started = false;
    /* above the player (5) and LVGL (4): a late block is a hole in the sound.
     * It sleeps whenever the ring is full, so it never holds the core. */
    if (!aos_hal_thread_start("es_audio", audio_main, e, 6144, 6)) {
        es_out_close();
        aos_hal_log(ES_TAG, "no audio thread");
        return false;
    }
    s_thread_on = true;
    return true;
}

void es_engine_stop(es_engine_t *e)
{
    if (!e || !e->live || !s_thread_on) return;
    e->quit = true;
    for (int i = 0; i < 400 && !e->done; i++) aos_hal_sleep_ms(5);
    if (!e->done) aos_hal_log(ES_TAG, "the audio thread did not stop");
    s_thread_on = false;
    es_out_close();
    /* what was sounding is gone with the ring; start clean next time */
    e->playing = 0;
    e->rec = 0;
    e->countin = 0;
    e->step_now = -1;
    arp_clear(e);
    all_off(e, true);
    e->mo_h = e->mo_t;
}

/* ---- what the UI reads ---- */

bool es_playing(const es_engine_t *e)   { return e && e->playing; }
bool es_recording(const es_engine_t *e) { return e && e->rec; }
bool es_countin(const es_engine_t *e)   { return e && e->countin; }
int  es_play_pat(const es_engine_t *e)  { return e ? e->cur_pat : 0; }
int  es_play_step(const es_engine_t *e) { return e && e->playing && !e->countin ? e->step_now : -1; }
int  es_chain_pos(const es_engine_t *e) { return e ? e->chain_pos : 0; }
int  es_cued_pat(const es_engine_t *e)  { return e ? e->cue_pat : -1; }
uint32_t es_drum_hits(const es_engine_t *e, int track) { return e ? e->dhits[track] : 0; }

int es_part_notes(const es_engine_t *e, int part, uint8_t *notes, int max)
{
    int n = 0;
    for (int i = 0; i < ES_VOICES && n < max; i++)
        if (e->v[i].on && e->v[i].gate && e->v[i].part == part) notes[n++] = e->v[i].note;
    return n;
}

void es_stats(es_engine_t *e, es_stats_t *out)
{
    *out = e->stats;
    out->voices = e->stats.voices;
    out->queued = e->queued;
    out->underruns = e->underruns;
}

int es_live_part(const es_engine_t *e) { return e ? e->live_part : 0; }

void es_set_live_part(es_engine_t *e, int part)
{
    if (e && part >= 0 && part < ES_PARTS) e->live_part = part;
}

void es_set_latency(es_engine_t *e, int level) { if (e) e->lat_level = level; }

void es_set_voices(es_engine_t *e, int n)
{
    if (e) e->nvoices = n < 4 ? 4 : n > ES_VOICES ? ES_VOICES : n;
}

void es_set_mout(es_engine_t *e, int dest_mask, bool clock, int pc_track)
{
    if (!e) return;
    e->mo_dest = dest_mask;
    e->mo_clock = clock;
    e->mo_pc_track = pc_track;
}

/* the drum voices keep a pointer into the bank: before the bank lets go of
 * anything the engine must be stopped (es_bank_free_all says so) */
void es_drop_sample_refs(es_engine_t *e)
{
    for (int t = 0; t < ES_DRUMS; t++)
        for (int k = 0; k < 2; k++)
            if (e->dv[t][k].smp) { e->dv[t][k].on = 0; e->dv[t][k].smp = NULL; }
}

/* ---- the export ---- */

static struct {
    volatile bool  busy, cancel;
    volatile float prog;
    es_proj_t     *p;
    es_engine_t   *e;
    char           path[320];
    int            pat, loops;
} X;

static void export_main(void *arg)
{
    (void)arg;
    es_engine_t *e = X.e;
    es_proj_t *p = X.p;
    void *wh = NULL;
    bool ok = es_wav_write_open(X.path, ES_SR, &wh);
    float total = 0.0f;
    if (p->song_mode && p->chain_n) {
        for (int i = 0; i < p->chain_n; i++) total += (float)p->pat[p->chain[i] % ES_PATS].len;
    } else {
        total = (float)p->pat[X.pat].len * (float)X.loops;
        e->cur_pat = (int8_t)X.pat;
        e->loops_left = X.loops;
    }
    if (total < 1.0f) total = 1.0f;
    p->met_on = 0;
    play(e);
    static int16_t out[2048];
    float blk[ES_BLK];
    int no = 0;
    uint32_t tail = 0, blocks = 0;
    const uint32_t max_blocks = (uint32_t)(30u * 60u * ES_SR / ES_BLK);
    while (ok && !X.cancel && blocks < max_blocks) {
        engine_block(e, blk);
        blocks++;
        for (int i = 0; i < ES_BLK; i++) {
            float v = blk[i] * 32767.0f;
            out[no++] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        if (no >= 2048 - ES_BLK) {
            ok = es_wav_write(wh, out, (uint32_t)no);
            no = 0;
        }
        if (e->ended) {
            if (!tail) stop(e);
            tail += ES_BLK;
            if (tail >= ES_SR * 5 / 2) break;
            X.prog = 0.98f;
        } else {
            float pr = e->abs_pos / total;
            X.prog = pr > 0.97f ? 0.97f : pr;
        }
        /* below the UI: give the core back now and then */
        if ((blocks & 255) == 0) aos_hal_sleep_ms(1);
    }
    if (ok && no) ok = es_wav_write(wh, out, (uint32_t)no);
    if (wh && !es_wav_write_close(wh)) ok = false;
    if (!ok || X.cancel) unlink(X.path);
    es_engine_free(e);
    es_free(p);
    X.e = NULL;
    X.p = NULL;
    X.prog = (!ok || X.cancel) ? -1.0f : 2.0f;
    X.busy = false;
}

bool es_export_wav_start(const es_proj_t *src, const char *path, int pat, int loops)
{
    if (X.busy) return false;
    X.p = es_alloc(sizeof *X.p);
    if (!X.p) return false;
    memcpy(X.p, src, sizeof *X.p);
    X.e = es_engine_new(X.p, false);
    if (!X.e) { es_free(X.p); X.p = NULL; return false; }
    snprintf(X.path, sizeof X.path, "%s", path);
    X.pat = pat < 0 || pat >= ES_PATS ? 0 : pat;
    X.loops = loops < 1 ? 1 : loops;
    X.cancel = false;
    X.prog = 0.0f;
    X.busy = true;
    if (!aos_hal_thread_start("es_export", export_main, NULL, 8192, 2)) {
        es_engine_free(X.e);
        es_free(X.p);
        X.e = NULL;
        X.p = NULL;
        X.busy = false;
        return false;
    }
    return true;
}

float es_export_progress(void) { return X.prog; }
bool  es_export_busy(void)     { return X.busy; }

void es_export_cancel(void)
{
    if (!X.busy) return;
    X.cancel = true;
    for (int i = 0; i < 600 && X.busy; i++) aos_hal_sleep_ms(5);
}

/* ---- the speaker ---- */

#if defined(AOS_SIM)
/* The simulator's aos_hal_spk_* swallows the samples (a pedido for the
 * firmware session), so here the app opens SDL itself, in queue mode, which
 * behaves like the board's ring: write() appends, queued() is what waits.
 * ES_SIM_TAP=<file.wav> also writes everything that goes out to a WAV, to
 * look at the sound without listening. */
#include <SDL2/SDL.h>

static SDL_AudioDeviceID s_dev;
static void *s_tap;

bool es_out_open(void)
{
    if (s_dev) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want = { 0 }, have;
    want.freq = ES_SR;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 256;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_dev) {
        aos_hal_log(ES_TAG, "SDL audio: %s", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    const char *tap = getenv("ES_SIM_TAP");
    if (tap && *tap) es_wav_write_open(tap, ES_SR, &s_tap);
    return true;
}

int es_out_write(const int16_t *pcm, int n)
{
    if (!s_dev) return 0;
    if (s_tap) es_wav_write(s_tap, pcm, (uint32_t)n);
    return SDL_QueueAudio(s_dev, pcm, (Uint32)n * 2) == 0 ? n : 0;
}

int es_out_queued(void)
{
    return s_dev ? (int)(SDL_GetQueuedAudioSize(s_dev) / 2) : 0;
}

void es_out_close(void)
{
    if (s_tap) { es_wav_write_close(s_tap); s_tap = NULL; }
    if (!s_dev) return;
    SDL_CloseAudioDevice(s_dev);
    s_dev = 0;
}
#else
bool es_out_open(void)                          { return aos_hal_spk_open_low(ES_SR); }
int  es_out_write(const int16_t *pcm, int n)    { return aos_hal_spk_write(pcm, n); }
int  es_out_queued(void)                        { return aos_hal_spk_queued(); }
void es_out_close(void)                         { aos_hal_spk_close(); }
#endif

/* the engine's block from outside, for the tests on the Mac (scratch harness) */
#if defined(ES_TEST)
void es_engine_test_block(es_engine_t *e, float *out) { engine_block(e, out); }
#endif
