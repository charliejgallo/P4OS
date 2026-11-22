/*
 * Estudio - USB MIDI, in and out, in a thread of its own.
 *
 * In: a controller keyboard on the board's USB host (aos_hal_midi_read).
 * Channel 1 plays the part chosen on the screen, channels 2 to 4 the parts
 * 1 to 3, channel 10 the drums (General MIDI notes to the track with that
 * sound). The pitch bend is two semitones; CC 1 the modulation (it deepens
 * the LFO), 64 the sustain pedal, 74 and 71 the cutoff and the resonance of
 * the part; Start and Stop play and stop.
 *
 * Out: what the audio thread queues - the notes as they are played or
 * sequenced, and the clock (24 a quarter) with Start and Stop - to a device
 * on the host (aos_hal_midi_send, every channel) and to the computer on the
 * OTG port: the same, with aos_hal_usb_midi_send (P4OS 0.32), or one track's
 * notes on channel 1 for a program that listens to one channel.
 *
 * The thread reads and sends every millisecond. The audio thread never
 * calls the HAL's MIDI itself: those calls may wait on USB.
 */
#include "es.h"

#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

static struct {
    volatile bool quit, done, on;
    volatile uint32_t in_n;
    es_engine_t *e;
    es_proj_t   *p;
} M;

uint32_t es_midi_in_count(void) { return M.in_n; }

static void handle(const aos_midi_msg_t *m)
{
    es_engine_t *e = M.e;
    uint8_t st = m->status & 0xF0, ch = m->status & 0x0F;
    if (m->status == 0xFA) { es_cmd_midi(e, EC_PLAY, 0, 0, 0, SRC_MIDI); return; }
    if (m->status == 0xFC) { es_cmd_midi(e, EC_STOP, 0, 0, 0, SRC_MIDI); return; }
    if (m->status >= 0xF0) return;
    M.in_n++;
    int part = ch == 0 ? es_live_part(e) : ch <= 3 ? ch - 1 : es_live_part(e);
    bool on = st == 0x90 && m->data2 > 0;
    bool off = st == 0x80 || (st == 0x90 && m->data2 == 0);
    if (ch == 9 && (on || off)) {
        if (on) {
            int tr = es_drum_from_gm(M.p, m->data1);
            if (tr >= 0) es_cmd_midi(e, EC_DRUM, tr, m->data2, 0, SRC_MIDI);
        }
        return;
    }
    if (on) es_cmd_midi(e, EC_NOTE_ON, part, m->data1, m->data2, SRC_MIDI);
    else if (off) es_cmd_midi(e, EC_NOTE_OFF, part, m->data1, 0, SRC_MIDI);
    else if (st == 0xE0) {
        int b = ((int)m->data2 << 7 | m->data1) - 8192;
        es_cmd_midi(e, EC_BEND, part, 0, 0, (float)b * (2.0f / 8192.0f));
    } else if (st == 0xB0) {
        float v = (float)m->data2 / 127.0f;
        switch (m->data1) {
        case 1:  es_cmd_midi(e, EC_MOD, part, 0, 0, v); break;
        case 64: es_cmd_midi(e, EC_SUSTAIN, part, m->data2 >= 64, 0, 0); break;
        case 74: M.p->part[part].p[P_CUT] = es_param_from01(P_CUT, v); break;
        case 71: M.p->part[part].p[P_RES] = v; break;
        default: break;
        }
    }
}

static void send(const es_mout_t *o)
{
    if (o->dest & 2) {
        aos_midi_msg_t m = { o->st, o->d1, o->d2, 0 };
        aos_hal_midi_send(&m);
    }
    if (o->dest & 4) aos_hal_usb_midi_send(o->st, o->d1, o->d2);
    if (o->dest & 1) {
        uint8_t st = o->st & 0xF0;
        if (st == 0x90) aos_hal_usb_midi_note(o->d1, o->d2, true);
        else if (st == 0x80) aos_hal_usb_midi_note(o->d1, 0, false);
    }
}

#if defined(AOS_SIM)
/* ES_SIM_MIDI="ms:status:d1:d2,..." (hex bytes): a keyboard that plays by
 * itself, since the simulator has no USB host */
static struct { uint32_t at; aos_midi_msg_t m; } s_fake[64];
static int s_fake_n, s_fake_i;
static uint32_t s_fake_t0;

static void fake_load(void)
{
    const char *s = getenv("ES_SIM_MIDI");
    s_fake_n = s_fake_i = 0;
    s_fake_t0 = (uint32_t)aos_hal_uptime_ms();
    while (s && *s && s_fake_n < 64) {
        char *e;
        unsigned long t = strtoul(s, &e, 10);
        if (*e != ':') break;
        unsigned long a = strtoul(e + 1, &e, 16);
        if (*e != ':') break;
        unsigned long b = strtoul(e + 1, &e, 16);
        if (*e != ':') break;
        unsigned long c = strtoul(e + 1, &e, 16);
        s_fake[s_fake_n].at = (uint32_t)t;
        s_fake[s_fake_n].m = (aos_midi_msg_t){ (uint8_t)a, (uint8_t)b, (uint8_t)c, 0 };
        s_fake_n++;
        s = *e == ',' ? e + 1 : e;
    }
}

static bool fake_read(aos_midi_msg_t *m)
{
    if (s_fake_i >= s_fake_n || (uint32_t)aos_hal_uptime_ms() - s_fake_t0 < s_fake[s_fake_i].at) return false;
    *m = s_fake[s_fake_i++].m;
    return true;
}
#endif

static void midi_main(void *arg)
{
    (void)arg;
#if defined(AOS_SIM)
    fake_load();
#endif
    while (!M.quit) {
        aos_midi_msg_t m;
        for (int n = 0; n < 64 && aos_hal_midi_read(&m); n++) handle(&m);
#if defined(AOS_SIM)
        while (fake_read(&m)) handle(&m);
#endif
        es_mout_t o;
        for (int n = 0; n < 128 && es_mout_take(M.e, &o); n++) send(&o);
        aos_hal_sleep_ms(1);
    }
    M.done = true;
}

bool es_midi_start(es_engine_t *e, es_proj_t *p)
{
    if (M.on) return true;
    M.e = e;
    M.p = p;
    M.quit = false;
    M.done = false;
    if (!aos_hal_thread_start("es_midi", midi_main, NULL, 4096, 4)) return false;
    M.on = true;
    return true;
}

void es_midi_stop(void)
{
    if (!M.on) return;
    M.quit = true;
    for (int i = 0; i < 300 && !M.done; i++) aos_hal_sleep_ms(5);
    M.on = false;
}
