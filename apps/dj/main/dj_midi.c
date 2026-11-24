/*
 * DJ - a USB MIDI controller, learned control by control.
 *
 * There is no standard layout for DJ controllers, so the app learns: in
 * Settings, "Learn", touch a control on the screen, then move or press the
 * one on the device. A control change or a pitch bend drives a knob or a
 * fader (0..127 over its range), a note presses a button (and lets it go
 * with the note off: CUE previews while held). A control change bound to a
 * platter is taken as a jog wheel: values under 64 turn it forward, over 64
 * back (the "relative" encoding most jog wheels send), and it scratches.
 *
 * The bindings are kept by control and deck, not by position on the
 * screen, in <data>/dj/midi.txt: "kind channel number control deck" a line.
 */
#include "dj_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U dj_ui

#define MAXMAP 96

typedef struct {
    uint8_t kind;       /* 0 control change, 1 note, 2 pitch bend */
    uint8_t ch, num;
    uint8_t ctl;        /* C_* */
    int8_t  deck;       /* -1 the mixer's */
} map_t;

static map_t s_map[MAXMAP];
static int   s_n;
static bool  s_loaded;
static int   s_pending = -1;       /* the screen control waiting for its message */
static int8_t s_jog_touch[DJ_NDECK];
static uint32_t s_jog_ms[DJ_NDECK];

static void path(char *out, size_t n) { snprintf(out, n, "%s/midi.txt", dj_data_dir()); }

void dj_midi_load(void)
{
    s_n = 0;
    s_loaded = true;
    char p[200];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[64];
    while (s_n < MAXMAP && fgets(line, sizeof line, f)) {
        char *e = line;
        long v[5];
        int k = 0;
        for (; k < 5; k++) {
            char *end;
            v[k] = strtol(e, &end, 10);
            if (end == e) break;
            e = end;
        }
        if (k < 5 || v[3] < 0 || v[3] >= C_COUNT) continue;
        s_map[s_n++] = (map_t){ (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3], (int8_t)v[4] };
    }
    fclose(f);
}

static void save(void)
{
    char p[200];
    path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < s_n; i++) {
        char line[48];
        snprintf(line, sizeof line, "%d %d %d %d %d\n", s_map[i].kind, s_map[i].ch, s_map[i].num, s_map[i].ctl, s_map[i].deck);
        fputs(line, f);
    }
    fclose(f);
}

void dj_midi_clear(void)
{
    s_n = 0;
    save();
}

int dj_midi_count(void)
{
    if (!s_loaded) dj_midi_load();
    return s_n;
}

void dj_midi_learn_ctl(int ctl) { s_pending = ctl; }

static void bind(int kind, int ch, int num)
{
    if (s_pending < 0 || s_pending >= U->nctl) return;
    const ctl_t *c = &U->ctl[s_pending];
    /* the message drives one control only */
    for (int i = 0; i < s_n; i++) {
        if (s_map[i].kind == kind && s_map[i].ch == ch && s_map[i].num == num) {
            s_map[i] = s_map[--s_n];
            break;
        }
    }
    if (s_n < MAXMAP) s_map[s_n++] = (map_t){ (uint8_t)kind, (uint8_t)ch, (uint8_t)num, c->id, c->deck };
    save();
    int was = s_pending;
    s_pending = -1;
    U->learn = 0;
    U->learn_ctl = -1;
    dj_ui_dirty(was);
    aos_ui_toast(_("Control aprendido"), 1200);
}

static void act(const map_t *m, int value, bool on)
{
    int i = dj_ui_find(m->ctl, m->deck);
    if (i < 0) return;
    ctl_t *c = &U->ctl[i];
    switch (c->kind) {
    case K_BTN:
        if (m->kind == 0) on = value >= 64;
        if (on) dj_act_press(i);
        else dj_act_release(i);
        break;
    case K_KNOB: case K_VFADER: case K_HFADER:
        if (m->kind == 1) break;
        dj_act_value(i, c->lo + (c->hi - c->lo) * (float)value / (m->kind == 2 ? 16383.0f : 127.0f));
        break;
    case K_PLATTER: {
        int d = c->deck;
        if (m->kind != 0 || d < 0) break;
        int delta = value < 64 ? value : value - 128;
        dj_track_t *t = dj_deck_track(U->e, d);
        if (!t->sr) break;
        if (!s_jog_touch[d]) {
            dj_cmd(U->e, DC_TOUCH, d, 1, 0.0f);
            s_jog_touch[d] = 1;
        }
        /* a tick of a jog wheel: 1/128 of a turn */
        dj_cmd(U->e, DC_SCRATCH, d, 0, (float)delta / 128.0f * 1.8f * (float)t->sr);
        s_jog_ms[d] = (uint32_t)aos_hal_uptime_ms();
        break;
    }
    default:
        break;
    }
}

void dj_midi_poll(void)
{
    if (!U) return;
    if (!s_loaded) dj_midi_load();
    aos_midi_msg_t m;
    int guard = 0;
    while (guard++ < 64 && aos_hal_midi_read(&m)) {
        int type = m.status & 0xF0, ch = m.status & 0x0F;
        int kind, num, value;
        bool on = true;
        if (type == 0xB0) { kind = 0; num = m.data1; value = m.data2; }
        else if (type == 0x90 || type == 0x80) { kind = 1; num = m.data1; value = m.data2; on = type == 0x90 && m.data2 > 0; }
        else if (type == 0xE0) { kind = 2; num = 0; value = m.data1 | (m.data2 << 7); }
        else continue;
        if (U->learn == 2 && s_pending >= 0) {
            if (kind == 1 && !on) continue;         /* learn on the press */
            bind(kind, ch, num);
            continue;
        }
        for (int i = 0; i < s_n; i++)
            if (s_map[i].kind == kind && s_map[i].ch == ch && s_map[i].num == num) act(&s_map[i], value, on);
    }
    /* a jog wheel that stopped: let go of the record */
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int d = 0; d < DJ_NDECK; d++) {
        if (s_jog_touch[d] && now - s_jog_ms[d] > 60) {
            dj_cmd(U->e, DC_TOUCH, d, 0, 0.0f);
            s_jog_touch[d] = 0;
        }
    }
}
