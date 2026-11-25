/*
 * P4OS - Infrarrojo: where a code goes out, and where one is learned.
 *
 * Two outputs: the IR LED on a pin of the header (ir_hw.c's thread), or a
 * Tuya "Zosung" IR blaster (TS1201) in the board's Zigbee network
 * (aos_zigbee_ir_*, the firmware's). The app has one output, chosen in the
 * Cableado tab (the preference "ir_out": "" the LED, or the blaster's IEEE
 * in hex), and a device can have its own ("via" in its file: "led" or an
 * IEEE), so the bedroom's air conditioner goes through the blaster in the
 * bedroom while the television uses the one in the living room. Every send
 * of the app comes through here.
 *
 * A blaster takes one code at a time, for the whole network, and a code
 * takes from half a second to several seconds over the air (the blaster
 * asks for it in chunks of 50 bytes; the first after learning took 7 s on
 * the board). So a held key is not repeated through it, one more send
 * waits its turn (an air conditioner's state replaces the state waiting
 * before it, and its "on" may wait with it), and a press beyond that is
 * told the blaster is busy. Whether a send went is seen in
 * aos_zigbee_ir_state(): SENDING, then SENT or FAILED.
 *
 * The firmware keeps one request and posts it to its own task: a second
 * call made before that task ran would overwrite the first. So a call
 * here waits until the state shows the one before it was taken (the
 * transfer's T_WAIT and T_POSTED, the learning's L_POSTED and L_PAUSE).
 *
 * Learning through a blaster: while someone listens (the Aprender tab, the
 * portal) and the source is a blaster, its learning mode is on. The code it
 * learns, Tuya's base64, is decoded to durations and handed to ir_hw.c as a
 * capture, as if the receiver had heard it; then learning starts again for
 * the next press. A send through a blaster turns its learning off first.
 *
 * Everything here runs in LVGL's task, from the app's tick, which goes on
 * in the background.
 */
#include "ir.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_zigbee.h"

#include <stdio.h>
#include <string.h>

#define PEND      2
#define LIST_MS   2000
#define POST_MS   3000          /* the firmware's task takes a request well within this */
#define FREE_MS   6000          /* waiting for someone else's transfer to end */
#define SEND_MS   35000         /* the firmware gives up at 30 s */
#define LINGER_MS 10000         /* learning kept on this long after nobody listens */
#define REARM_MS  30000         /* learning asked for again, in case the blaster let it go */
#define PAUSE_MS  700
#define CODE_MAX  3072          /* a learned code in base64 (the firmware keeps 2 KB of it) */

enum { T_IDLE = 0, T_WAIT, T_POSTED, T_SENDING };
enum { L_OFF = 0, L_POSTED, L_ON, L_PAUSE };

typedef struct {
    uint8_t   ieee[8];
    uint32_t *d;
    int       n;
    uint32_t  freq;
    bool      state, quiet;
} pend_t;

static struct {
    bool         loaded;
    char         def[IR_VIA_LEN];
    ir_blaster_t bl[IR_OUT_MAX];
    int          nbl;
    uint32_t     bl_ms;
    bool         formed;
    bool         quiet;
    char         why[96];       /* the last thing that did not go, and why */
    /* the transfer */
    int          t;             /* T_* */
    pend_t       cur;
    pend_t       pend[PEND];
    int          npend;
    int          prev_st;
    uint32_t     t_ms;
    uint32_t     sent, fails;
    /* learning */
    char         want[3][IR_VIA_LEN];   /* by IR_WHO_*: the blaster each one listens through */
    uint32_t     unwant_ms;             /* when nobody listened any more, learning still on (0: someone does) */
    int          l;             /* L_* */
    uint8_t      l_ieee[8];
    uint32_t     l_ms;
    bool         l_rx;
    bool         stopping;      /* learning off asked for, not seen yet */
    uint32_t     stop_ms;
    char        *code;
    uint32_t    *dur;
} O;

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static void hex_of(const uint8_t ie[8], char *out)
{
    snprintf(out, IR_VIA_LEN, "%02x%02x%02x%02x%02x%02x%02x%02x", ie[7], ie[6], ie[5], ie[4], ie[3], ie[2], ie[1],
             ie[0]);
}

static int nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ieee_of(const char *via, uint8_t ie[8])
{
    if (!via || strlen(via) != 16) return false;
    for (int i = 0; i < 8; i++) {
        int a = nib(via[2 * i]), b = nib(via[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        ie[7 - i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

static void load(void)
{
    if (O.loaded) return;
    O.loaded = true;
    uint8_t ie[8];
    if (!aos_hal_pref_get_str("ir_out", O.def, sizeof O.def) || !ieee_of(O.def, ie)) O.def[0] = 0;
}

static void refresh_list(bool force)
{
    uint32_t t = now_ms();
    if (!force && O.bl_ms && t - O.bl_ms < LIST_MS) return;
    O.bl_ms = t ? t : 1;
    aos_zigbee_info_t z;
    aos_zigbee_info(&z);
    O.formed = z.state == AOS_ZB_FORMED;
    uint8_t ie[IR_OUT_MAX][8];
    char nm[IR_OUT_MAX][33];
    int n = O.formed ? aos_zigbee_ir_blasters(ie, nm, IR_OUT_MAX) : 0;
    if (n < 0) n = 0;
    O.nbl = n;
    for (int i = 0; i < n; i++) {
        ir_blaster_t *b = &O.bl[i];
        memcpy(b->ieee, ie[i], 8);
        hex_of(ie[i], b->via);
        if (!nm[i][0]) snprintf(nm[i], sizeof nm[i], "IR");
    }
    for (int i = 0; i < n; i++) {
        /* two of the same name (the same model, unnamed) told apart by the
         * end of their IEEE */
        bool twin = false;
        for (int k = 0; k < n; k++) twin |= k != i && !strcmp(nm[k], nm[i]);
        if (twin) snprintf(O.bl[i].name, sizeof O.bl[i].name, "%s · %02x%02x", nm[i], ie[i][1], ie[i][0]);
        else snprintf(O.bl[i].name, sizeof O.bl[i].name, "%s", nm[i]);
    }
}

static const ir_blaster_t *find(const char *via)
{
    for (int i = 0; i < O.nbl; i++) if (!strcmp(O.bl[i].via, via)) return &O.bl[i];
    return NULL;
}

static void tell(bool quiet, const char *msg)
{
    ir_copy(O.why, sizeof O.why, msg);
    if (!quiet && !O.quiet) ir_ui_toast(msg);
}

const char *ir_out_why(void) { return O.why; }

/* ---------------------------------------------------------------- outputs */

int ir_out_blasters(const ir_blaster_t **out)
{
    load();
    refresh_list(false);
    *out = O.bl;
    return O.nbl;
}

const char *ir_out_default(void)
{
    load();
    return O.def;
}

void ir_out_set_default(const char *via)
{
    load();
    uint8_t ie[8];
    if (!via || !ieee_of(via, ie)) via = "";
    ir_copy(O.def, sizeof O.def, via);
    if (via[0]) aos_hal_pref_set_str("ir_out", via);
    else aos_hal_pref_erase("ir_out");
}

const char *ir_out_resolve(const char *via)
{
    load();
    if (!via || !via[0]) return O.def;
    uint8_t ie[8];
    if (!strcmp(via, "led") || !ieee_of(via, ie)) return "";
    return via;
}

bool ir_out_is_zigbee(const char *via) { return ir_out_resolve(via)[0] != 0; }

void ir_out_label(const char *via, char *out, size_t n)
{
    const char *r = ir_out_resolve(via);
    if (!r[0]) { snprintf(out, n, "%s", _("el LED de la placa")); return; }
    refresh_list(false);
    const ir_blaster_t *b = find(r);
    if (b) snprintf(out, n, _("el emisor Zigbee %s"), b->name);
    else snprintf(out, n, _("el emisor Zigbee %s (no está en la red)"), r + 12);
}

/* ------------------------------------------------------------- sending */

static void pend_free(pend_t *p)
{
    ir_free(p->d);
    memset(p, 0, sizeof *p);
}

static void finish(bool ok, const char *why)
{
    if (ok) {
        O.sent++;
    } else {
        O.fails++;
        aos_hal_log("ir", "the Zigbee blaster did not send: %s", why ? why : "failed");
        tell(O.cur.quiet, why ? why : _("El emisor Zigbee no tomó el código"));
    }
    pend_free(&O.cur);
    O.t = T_IDLE;
}

static void post_send(int st, uint32_t t)
{
    O.prev_st = st;
    if (!aos_zigbee_ir_send_raw(O.cur.ieee, O.cur.d, O.cur.n, O.cur.freq)) {
        aos_zigbee_info_t z;
        aos_zigbee_info(&z);
        uint8_t ie[8];
        int now = aos_zigbee_ir_state(ie, NULL, 0);
        if (z.state != AOS_ZB_FORMED) finish(false, _("La red Zigbee no está andando"));
        else if (now == AOS_ZB_IR_SENDING || now == AOS_ZB_IR_RECEIVING) O.t = T_WAIT;     /* someone else's, just begun */
        /* the network up and the blaster free: the code did not fit its buffer */
        else finish(false, _("Ese código es muy largo para el emisor Zigbee"));
        return;
    }
    O.t = T_POSTED;
    O.t_ms = t;
}

/* no request of ours waiting to be taken */
static bool gate(void) { return O.t != T_WAIT && O.t != T_POSTED && O.l != L_POSTED && !O.stopping; }

static void learn_off(uint32_t t)
{
    aos_zigbee_ir_learn(O.l_ieee, false);
    O.stopping = true;
    O.stop_ms = t;
    O.l = L_PAUSE;
    O.l_ms = t;
}

static void start(int st, uint32_t t)
{
    O.cur = O.pend[0];
    memmove(&O.pend[0], &O.pend[1], (size_t)(PEND - 1) * sizeof O.pend[0]);
    memset(&O.pend[PEND - 1], 0, sizeof O.pend[0]);
    O.npend--;
    O.t = T_WAIT;
    O.t_ms = t;
    if (O.l == L_ON && st == AOS_ZB_IR_LEARNING) {
        /* the blaster learning: off first, back on once this went */
        learn_off(t);
        return;
    }
    if (O.l == L_ON) { O.l = L_PAUSE; O.l_ms = t; }
    if (st != AOS_ZB_IR_SENDING && st != AOS_ZB_IR_RECEIVING) post_send(st, t);
}

int ir_out_send(const char *via, const uint32_t *d, int n, uint32_t carrier, int flags)
{
    const char *r = ir_out_resolve(via);
    if (!r[0]) return n > 0 && ir_hw_send(d, n, carrier) ? 1 : 0;
    /* a held key through a blaster: once is enough, the next would wait
     * seconds behind it */
    if (flags & IR_OUT_REPEAT) return -1;
    if (n <= 0) return 0;
    refresh_list(false);
    if (!O.formed) { tell(false, _("La red Zigbee no está andando")); return -1; }
    const ir_blaster_t *b = find(r);
    if (!b) { tell(false, _("Ese emisor Zigbee no está en la red")); return -1; }
    if (n > IR_TX_MAX) n = IR_TX_MAX;
    uint32_t *copy = ir_alloc((size_t)n * sizeof *d);
    if (!copy) return 0;
    memcpy(copy, d, (size_t)n * sizeof *d);
    pend_t *p = NULL;
    if (flags & IR_OUT_STATE)
        for (int i = O.npend - 1; i >= 0 && !p; i--)
            if (O.pend[i].state && !memcmp(O.pend[i].ieee, b->ieee, 8)) p = &O.pend[i];
    bool idle = O.t == T_IDLE && !O.npend;
    if (!p) {
        /* one more waits; an air conditioner's on and its state, two */
        int room = (flags & IR_OUT_STATE) ? PEND : 1;
        if (!idle && O.npend >= room) {
            ir_free(copy);
            tell(false, _("El emisor Zigbee está ocupado: esperá que termine"));
            return -1;
        }
        p = &O.pend[O.npend++];
    }
    ir_free(p->d);
    memset(p, 0, sizeof *p);
    p->d = copy;
    p->n = n;
    memcpy(p->ieee, b->ieee, 8);
    p->freq = carrier;
    p->state = (flags & IR_OUT_STATE) != 0;
    p->quiet = O.quiet;
    if (idle && gate()) {
        uint8_t ie[8];
        start(aos_zigbee_ir_state(ie, NULL, 0), now_ms());
    }
    return idle ? 1 : 2;
}

int ir_out_send_button(const char *via, const ir_button_t *b, bool repeat)
{
    static uint32_t *buf;
    if (!buf) buf = ir_alloc(IR_TX_MAX * sizeof *buf);
    if (!buf) return 0;
    if (repeat && ir_out_is_zigbee(via)) return -1;
    uint32_t carrier;
    int n = ir_button_pulses(b, repeat, buf, IR_TX_MAX, &carrier);
    if (n <= 0) return 0;
    return ir_out_send(via, buf, n, carrier, repeat ? IR_OUT_REPEAT : 0);
}

void ir_out_quiet(bool quiet) { O.quiet = quiet; }

uint32_t ir_out_sent(void) { return ir_hw_sent() + O.sent; }
uint32_t ir_out_failed(void) { return O.fails; }

bool ir_out_busy(char *text, size_t n)
{
    if (O.t == T_IDLE && !O.npend) return false;
    if (O.npend) snprintf(text, n, _("Mandando por el emisor Zigbee… (%d en espera)"), O.npend);
    else snprintf(text, n, "%s", _("Mandando por el emisor Zigbee…"));
    return true;
}

/* ------------------------------------------------------------- learning */

void ir_out_listen(int who, bool on, const char *via)
{
    if (who < 0 || who > 2) return;
    const char *r = on ? ir_out_resolve(via) : "";
    ir_hw_listen(who, on && !r[0]);
    ir_copy(O.want[who], sizeof O.want[who], r);
}

static const char *wanted(void)
{
    return O.want[IR_WHO_UI][0] ? O.want[IR_WHO_UI] : O.want[IR_WHO_PORTAL];
}

int ir_out_learn_state(void)
{
    const char *w = wanted();
    if (!w[0]) return IR_LS_NONE;
    refresh_list(false);
    if (!O.formed || !find(w)) return IR_LS_NONET;
    if (O.l == L_ON) return O.l_rx ? IR_LS_RX : IR_LS_ON;
    return IR_LS_WAIT;
}

static void take_code(void)
{
    if (!O.code) O.code = ir_alloc(CODE_MAX);
    if (!O.dur) O.dur = ir_alloc(IR_CAP_MAX * sizeof *O.dur);
    if (!O.code || !O.dur) return;
    uint8_t ie[8];
    aos_zigbee_ir_state(ie, O.code, CODE_MAX);
    int n = aos_zigbee_ir_decode(O.code, O.dur, IR_CAP_MAX);
    aos_hal_log("ir", "learned through the Zigbee blaster: %d durations", n);
    if (n >= 4) ir_hw_inject(O.dur, n);
}

static void learn_step(int st, const uint8_t ie[8], uint32_t t)
{
    const char *w = wanted();
    uint8_t wie[8];
    bool want = w[0] && ieee_of(w, wie) && O.formed && find(w);
    switch (O.l) {
    case L_OFF:
        if (want && gate() && st != AOS_ZB_IR_SENDING && st != AOS_ZB_IR_RECEIVING) {
            if (aos_zigbee_ir_learn(wie, true)) {
                memcpy(O.l_ieee, wie, 8);
                O.l = L_POSTED;
            } else {
                O.l = L_PAUSE;
            }
            O.l_ms = t;
            O.l_rx = false;
        }
        break;
    case L_POSTED:
        if (st == AOS_ZB_IR_LEARNING || st == AOS_ZB_IR_RECEIVING) {
            O.l = L_ON;
            O.l_ms = t;
        } else if (t - O.l_ms > POST_MS) {
            O.l = L_PAUSE;
            O.l_ms = t;
        }
        break;
    case L_ON:
        /* nobody listening: learning stays on a while, so going from tab to
         * tab does not turn it off and on; a TS1201 told so a few times in a
         * row answered the next sending 17 s late (on the board, 2026-11-25) */
        if (!want && w[0] == '\0' && t - (O.unwant_ms ? O.unwant_ms : (O.unwant_ms = t)) < LINGER_MS) break;
        if (want) O.unwant_ms = 0;
        if (!want || memcmp(wie, O.l_ieee, 8)) {
            O.unwant_ms = 0;
            if (st == AOS_ZB_IR_LEARNING) learn_off(t);
            O.l = L_PAUSE;
            O.l_ms = t;
        } else if (st == AOS_ZB_IR_LEARNED && !memcmp(ie, O.l_ieee, 8)) {
            take_code();
            O.l = L_PAUSE;
            O.l_ms = t;
        } else if (st == AOS_ZB_IR_RECEIVING) {
            O.l_rx = true;
        } else if (st == AOS_ZB_IR_LEARNING) {
            O.l_rx = false;
            if (t - O.l_ms > REARM_MS) {
                aos_zigbee_ir_learn(O.l_ieee, true);
                O.l_ms = t;
            }
        } else {
            /* someone else's turn (the Zigbee app, a send): again after it */
            O.l = L_PAUSE;
            O.l_ms = t;
        }
        break;
    case L_PAUSE:
        O.l_rx = false;
        if (t - O.l_ms > PAUSE_MS) O.l = L_OFF;
        break;
    }
}

/* ------------------------------------------------------------------ tick */

void ir_out_tick(void)
{
    load();
    refresh_list(false);
    if (O.t == T_IDLE && !O.npend && O.l == L_OFF && !wanted()[0]) { O.stopping = false; return; }
    uint8_t ie[8];
    int st = aos_zigbee_ir_state(ie, NULL, 0);
    uint32_t t = now_ms();
    if (O.stopping && (st != AOS_ZB_IR_LEARNING || t - O.stop_ms > POST_MS)) O.stopping = false;
    switch (O.t) {
    case T_WAIT:
        /* someone else's transfer, or ours learning going off */
        if (O.stopping || st == AOS_ZB_IR_SENDING || st == AOS_ZB_IR_RECEIVING) {
            if (t - O.t_ms > FREE_MS) finish(false, _("El emisor Zigbee está ocupado"));
        } else {
            post_send(st, t);
        }
        return;
    case T_POSTED:
        if (st == AOS_ZB_IR_SENDING) {
            O.t = T_SENDING;
            O.t_ms = t;
        } else if (st != O.prev_st && (st == AOS_ZB_IR_SENT || st == AOS_ZB_IR_FAILED)) {
            finish(st == AOS_ZB_IR_SENT, NULL);
        } else if (t - O.t_ms > POST_MS) {
            finish(false, NULL);
        }
        return;
    case T_SENDING:
        if (st == AOS_ZB_IR_SENT) finish(true, NULL);
        else if (st != AOS_ZB_IR_SENDING || t - O.t_ms > SEND_MS) finish(false, NULL);
        return;
    default:
        break;
    }
    if (O.npend) {
        /* learning asked for just before: seen taken (or given up) first,
         * then turned off by start() */
        if (O.l == L_POSTED) learn_step(st, ie, t);
        if (gate()) start(st, t);
        return;
    }
    learn_step(st, ie, t);
}

void ir_out_stop(void)
{
    if (O.l == L_ON || O.l == L_POSTED) {
        uint8_t ie[8];
        if (aos_zigbee_ir_state(ie, NULL, 0) == AOS_ZB_IR_LEARNING) aos_zigbee_ir_learn(O.l_ieee, false);
    }
    O.l = L_OFF;
    O.stopping = false;
    for (int i = 0; i < PEND; i++) pend_free(&O.pend[i]);
    O.npend = 0;
    pend_free(&O.cur);
    O.t = T_IDLE;
    memset(O.want, 0, sizeof O.want);
    ir_free(O.code);
    O.code = NULL;
    ir_free(O.dur);
    O.dur = NULL;
}
